#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Java Minecraft .mca -> GMod MC addon import JSON.

Phase 5 strict importer:
  - reads Java Anvil region files from a selected box
  - selects an audited chunk-storage adapter from DataVersion
  - supports modern sections[].block_states.palette/data
  - rejects legacy Level.Sections/Palette/BlockStates instead of guessing its packing layout
  - validates complete block states against the generated Minecraft report
  - requires the exact generated DataVersion for every imported chunk
  - preserves unknown Block Entity NBT as an inert typed sidecar
  - writes garrysmod/data/mc_import/*.json consumed by the mc_import command
  - does not execute Block Entity payloads or attempt Minecraft behavior simulation
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import lzma
import math
import os
import pathlib
import re
import shutil
import struct
import sys
import time
import zlib
from typing import Any


TAG_END = 0
TAG_BYTE = 1
TAG_SHORT = 2
TAG_INT = 3
TAG_LONG = 4
TAG_FLOAT = 5
TAG_DOUBLE = 6
TAG_BYTE_ARRAY = 7
TAG_STRING = 8
TAG_LIST = 9
TAG_COMPOUND = 10
TAG_INT_ARRAY = 11
TAG_LONG_ARRAY = 12

AIR = {"minecraft:air"}
CS = 16
CS2 = CS * CS
CH = 32
BS = 36.5
DEFAULT_ORIGIN_Z = 0
DEFAULT_MAX_BLOCKS = 0
DEFAULT_SAVE_PART_JSON_BYTES = 1024 * 1024 * 4
LZMA_FILTERS = [{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16, "lc": 3, "lp": 0, "pb": 2}]
PHASE5_FORMAT_VERSION = 10
MAX_NBT_DEPTH = 64
MAX_NBT_COLLECTION_ITEMS = 1_048_576
MAX_DECOMPRESSED_CHUNK_BYTES = 64 * 1024 * 1024


class ImportValidationError(ValueError):
    def __init__(self, code: str, message: str, **context: Any) -> None:
        super().__init__(message)
        self.code = code
        self.context = context


class NBTCompound(dict[str, Any]):
    def __init__(self) -> None:
        super().__init__()
        self.tag_types: dict[str, int] = {}


class NBTList(list[Any]):
    def __init__(self, values: list[Any], element_tag: int) -> None:
        super().__init__(values)
        self.element_tag = element_tag


class NBTArray(list[Any]):
    def __init__(self, values: list[Any], element_tag: int) -> None:
        super().__init__(values)
        self.element_tag = element_tag


class NBTReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def read(self, n: int) -> bytes:
        if n < 0:
            raise ImportValidationError("negative_nbt_length", f"NBT length is negative: {n}")
        if self.pos + n > len(self.data):
            raise EOFError("NBT 数据不完整")
        out = self.data[self.pos : self.pos + n]
        self.pos += n
        return out

    def u8(self) -> int:
        return self.read(1)[0]

    def i8(self) -> int:
        return struct.unpack(">b", self.read(1))[0]

    def i16(self) -> int:
        return struct.unpack(">h", self.read(2))[0]

    def i32(self) -> int:
        return struct.unpack(">i", self.read(4))[0]

    def i64(self) -> int:
        return struct.unpack(">q", self.read(8))[0]

    def f32(self) -> float:
        return struct.unpack(">f", self.read(4))[0]

    def f64(self) -> float:
        return struct.unpack(">d", self.read(8))[0]

    def string(self) -> str:
        n = struct.unpack(">H", self.read(2))[0]
        try:
            return self.read(n).decode("utf-8", "strict")
        except UnicodeDecodeError as exc:
            raise ImportValidationError("invalid_nbt_utf8", "NBT string is not valid UTF-8") from exc

    def payload(self, tag: int, depth: int = 0) -> Any:
        if depth > MAX_NBT_DEPTH:
            raise ImportValidationError("nbt_depth_limit", f"NBT depth exceeds {MAX_NBT_DEPTH}")
        if tag == TAG_BYTE:
            return self.i8()
        if tag == TAG_SHORT:
            return self.i16()
        if tag == TAG_INT:
            return self.i32()
        if tag == TAG_LONG:
            return self.i64()
        if tag == TAG_FLOAT:
            return self.f32()
        if tag == TAG_DOUBLE:
            return self.f64()
        if tag == TAG_BYTE_ARRAY:
            n = self.i32()
            if n > MAX_NBT_COLLECTION_ITEMS:
                raise ImportValidationError("nbt_collection_limit", f"NBT byte array has {n} elements")
            return self.read(n)
        if tag == TAG_STRING:
            return self.string()
        if tag == TAG_LIST:
            elem_tag = self.u8()
            n = self.i32()
            if n < 0:
                raise ImportValidationError("negative_nbt_list_length", f"NBT list length is negative: {n}")
            if n > MAX_NBT_COLLECTION_ITEMS:
                raise ImportValidationError("nbt_collection_limit", f"NBT list has {n} elements")
            return NBTList([self.payload(elem_tag, depth + 1) for _ in range(n)], elem_tag)
        if tag == TAG_COMPOUND:
            out = NBTCompound()
            while True:
                child_tag = self.u8()
                if child_tag == TAG_END:
                    break
                name = self.string()
                if name in out:
                    raise ImportValidationError("duplicate_nbt_key", f"duplicate NBT compound key: {name}")
                out[name] = self.payload(child_tag, depth + 1)
                out.tag_types[name] = child_tag
            return out
        if tag == TAG_INT_ARRAY:
            n = self.i32()
            if n < 0:
                raise ImportValidationError("negative_nbt_array_length", f"NBT int array length is negative: {n}")
            if n > MAX_NBT_COLLECTION_ITEMS:
                raise ImportValidationError("nbt_collection_limit", f"NBT int array has {n} elements")
            return NBTArray([self.i32() for _ in range(n)], TAG_INT)
        if tag == TAG_LONG_ARRAY:
            n = self.i32()
            if n < 0:
                raise ImportValidationError("negative_nbt_array_length", f"NBT long array length is negative: {n}")
            if n > MAX_NBT_COLLECTION_ITEMS:
                raise ImportValidationError("nbt_collection_limit", f"NBT long array has {n} elements")
            return NBTArray([self.i64() for _ in range(n)], TAG_LONG)
        raise ValueError(f"未知 NBT tag: {tag}")

    def root(self) -> dict[str, Any]:
        tag = self.u8()
        if tag != TAG_COMPOUND:
            raise ValueError("NBT root 不是 compound")
        _name = self.string()
        return self.payload(tag, 0)


def decompress_zlib_limited(data: bytes, wbits: int) -> bytes:
    decoder = zlib.decompressobj(wbits)
    out = decoder.decompress(data, MAX_DECOMPRESSED_CHUNK_BYTES + 1)
    if len(out) > MAX_DECOMPRESSED_CHUNK_BYTES or decoder.unconsumed_tail:
        raise ImportValidationError("chunk_decompressed_too_large", "decompressed chunk exceeds 64 MiB")
    out += decoder.flush(MAX_DECOMPRESSED_CHUNK_BYTES + 1 - len(out))
    if len(out) > MAX_DECOMPRESSED_CHUNK_BYTES:
        raise ImportValidationError("chunk_decompressed_too_large", "decompressed chunk exceeds 64 MiB")
    if not decoder.eof:
        raise ImportValidationError("invalid_compressed_chunk", "compressed chunk stream is incomplete")
    return out


def read_nbt(data: bytes) -> dict[str, Any]:
    if data[:2] == b"\x1f\x8b":
        data = decompress_zlib_limited(data, 16 + zlib.MAX_WBITS)
    if len(data) > MAX_DECOMPRESSED_CHUNK_BYTES:
        raise ImportValidationError("chunk_decompressed_too_large", "NBT payload exceeds 64 MiB")
    reader = NBTReader(data)
    root = reader.root()
    if reader.pos != len(data):
        raise ImportValidationError("trailing_nbt_data", f"NBT payload has {len(data) - reader.pos} trailing bytes")
    return root


def typed_nbt_value(value: Any, tag: int) -> dict[str, Any]:
    if tag == TAG_COMPOUND:
        if not isinstance(value, NBTCompound):
            raise ImportValidationError("missing_nbt_type_metadata", "compound lost its tag metadata")
        entries = [
            {"name": name, "value": typed_nbt_value(child, value.tag_types[name])}
            for name, child in value.items()
        ]
        return {"tag": "compound", "value": entries}
    if tag == TAG_LIST:
        if not isinstance(value, NBTList):
            raise ImportValidationError("missing_nbt_type_metadata", "list lost its element tag")
        return {
            "tag": "list",
            "elementTag": value.element_tag,
            "value": [typed_nbt_value(item, value.element_tag) for item in value],
        }
    if tag == TAG_BYTE_ARRAY:
        if not isinstance(value, (bytes, bytearray)):
            raise ImportValidationError("invalid_nbt_byte_array", "byte array payload is invalid")
        return {"tag": "byte_array", "base64": base64.b64encode(bytes(value)).decode("ascii")}
    if tag in (TAG_INT_ARRAY, TAG_LONG_ARRAY):
        if not isinstance(value, (list, tuple)):
            raise ImportValidationError("invalid_nbt_array", "numeric array payload is invalid")
        return {
            "tag": "int_array" if tag == TAG_INT_ARRAY else "long_array",
            "value": [str(item) if tag == TAG_LONG_ARRAY else int(item) for item in value],
        }
    names = {
        TAG_BYTE: "byte", TAG_SHORT: "short", TAG_INT: "int", TAG_LONG: "long",
        TAG_FLOAT: "float", TAG_DOUBLE: "double", TAG_STRING: "string",
    }
    if tag not in names:
        raise ImportValidationError("unknown_nbt_tag", f"unsupported typed NBT tag: {tag}")
    return {"tag": names[tag], "value": str(value) if tag == TAG_LONG else value}


class BlockStateRegistry:
    def __init__(self, report: dict[str, Any], metadata: dict[str, Any]) -> None:
        self.report = report
        self.metadata = metadata
        self.world_version = int(metadata["worldVersion"])
        self.schemas: dict[str, tuple[tuple[str, ...], dict[str, frozenset[str]], frozenset[tuple[str, ...]]]] = {}
        for name, definition in report.items():
            if not isinstance(definition, dict):
                continue
            properties = definition.get("properties") or {}
            if not isinstance(properties, dict):
                continue
            order = tuple(properties)
            allowed = {key: frozenset(str(value) for value in values) for key, values in properties.items()}
            legal = frozenset(
                tuple(str((state.get("properties") or {}).get(key, "")) for key in order)
                for state in (definition.get("states") or [])
                if isinstance(state, dict)
            )
            self.schemas[name] = (order, allowed, legal)

    def canonical_spec(self, entry: Any) -> str:
        if not isinstance(entry, dict):
            raise ImportValidationError("invalid_palette_entry", "block state palette entry is not a compound")
        unknown_fields = sorted(set(entry) - {"Name", "Properties"})
        if unknown_fields:
            raise ImportValidationError(
                "unknown_block_state_field",
                f"unknown block state fields: {', '.join(unknown_fields)}",
                fields=unknown_fields,
            )
        name = entry.get("Name")
        if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9_.-]+:[a-z0-9_./-]+", name):
            raise ImportValidationError("invalid_block_name", f"invalid block name: {name!r}")
        schema = self.schemas.get(name)
        if schema is None:
            raise ImportValidationError("unknown_block", f"unknown block: {name}", block=name)
        property_order, expected_properties, legal_states = schema
        raw_properties = entry.get("Properties", {})
        if not isinstance(raw_properties, dict):
            raise ImportValidationError("invalid_properties", f"Properties for {name} is not a compound")
        actual = set(raw_properties)
        expected = set(property_order)
        missing = sorted(expected - actual)
        unknown = sorted(actual - expected)
        if missing:
            raise ImportValidationError("missing_property", f"{name} is missing properties: {', '.join(missing)}", block=name, properties=missing)
        if unknown:
            raise ImportValidationError("unknown_property", f"{name} has unknown properties: {', '.join(unknown)}", block=name, properties=unknown)

        values: list[str] = []
        for property_name in property_order:
            value = raw_properties[property_name]
            if not isinstance(value, str):
                raise ImportValidationError(
                    "invalid_property_type",
                    f"{name}[{property_name}] is not a string",
                    block=name,
                    property=property_name,
                )
            allowed = expected_properties[property_name]
            if value not in allowed:
                raise ImportValidationError(
                    "invalid_property_value",
                    f"{name}[{property_name}={value}] is not valid",
                    block=name,
                    property=property_name,
                    value=value,
                )
            values.append(value)

        state_key = tuple(values)
        if state_key not in legal_states:
            raise ImportValidationError("illegal_state_combination", f"illegal property combination for {name}", block=name)
        if not property_order:
            return name
        return name + "[" + ",".join(
            f"{property_name}={raw_properties[property_name]}" for property_name in property_order
        ) + "]"


def _generated_metadata(repo_root: pathlib.Path) -> dict[str, Any]:
    metadata_path = repo_root / "lua" / "mc" / "generated" / "sh_generated_block_state_schemas.lua"
    if not metadata_path.is_file():
        metadata_path = pathlib.Path(__file__).resolve().parent / "data" / "sh_generated_block_state_schemas.lua"
    text = metadata_path.read_text(encoding="utf-8")
    fields: dict[str, Any] = {}
    for key in ("minecraftId", "seriesId", "schemaHash", "reportSha256"):
        match = re.search(rf'{key}="([^"]+)"', text)
        if not match:
            raise ImportValidationError("missing_generated_metadata", f"generated metadata is missing {key}")
        fields[key] = match.group(1)
    match = re.search(r"worldVersion=(\d+)", text)
    if not match:
        raise ImportValidationError("missing_generated_metadata", "generated metadata is missing worldVersion")
    fields["worldVersion"] = int(match.group(1))
    return fields


def load_block_state_registry(repo_root: pathlib.Path | None = None) -> BlockStateRegistry:
    root = repo_root or pathlib.Path(__file__).resolve().parents[1]
    report_path = root / "generated" / "reports" / "blocks.json"
    if not report_path.is_file():
        report_path = pathlib.Path(__file__).resolve().parent / "data" / "blocks.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    return BlockStateRegistry(report, _generated_metadata(root))


def floor_div(a: int, b: int) -> int:
    return math.floor(a / b)


def chunk_range(a: int, b: int) -> range:
    lo, hi = sorted((a, b))
    return range(floor_div(lo, 16), floor_div(hi, 16) + 1)


def region_range(chunk_a: int, chunk_b: int) -> range:
    lo, hi = sorted((chunk_a, chunk_b))
    return range(floor_div(lo, 32), floor_div(hi, 32) + 1)


def chunk_box(
    from_chunk: tuple[int, int] | list[int],
    to_chunk: tuple[int, int] | list[int],
    y_range: tuple[int, int] | list[int],
) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
    min_cx, max_cx = sorted((int(from_chunk[0]), int(to_chunk[0])))
    min_cz, max_cz = sorted((int(from_chunk[1]), int(to_chunk[1])))
    min_y, max_y = sorted((int(y_range[0]), int(y_range[1])))
    lo = (min_cx * 16, min_y, min_cz * 16)
    hi = (max_cx * 16 + 15, max_y, max_cz * 16 + 15)
    return lo, hi


def gmod_save_slot_name(name: str | None) -> str | None:
    name = str(name or "").strip()
    if name == "":
        return None
    name = re.sub(r'[\x00-\x1f/\\:\*\?"<>\|]', "_", name)
    name = re.sub(r"\.{2,}", "_", name)
    name = re.sub(r"^\.+", "", name)
    name = name[:64].strip()
    return name or None


def section_y(sec: dict[str, Any]) -> int | None:
    y = sec.get("Y")
    return int(y) if isinstance(y, int) else None


def unsigned_long(v: int) -> int:
    return v & ((1 << 64) - 1)


def unpack_palette_indices(data: list[int] | None, palette_len: int) -> list[int]:
    """Decode the post-1.16 non-crossing packed-long layout used by modern sections."""
    if palette_len < 1 or palette_len > 4096:
        raise ImportValidationError("invalid_palette_size", f"invalid block-state palette size: {palette_len}")
    if palette_len == 1:
        if data not in (None, []):
            raise ImportValidationError("unexpected_singleton_palette_data", "singleton palette must not contain packed data")
        return [0] * 4096
    if not isinstance(data, list):
        raise ImportValidationError("missing_packed_block_states", "multi-entry palette is missing packed data")
    bits = max(4, (palette_len - 1).bit_length())
    mask = (1 << bits) - 1
    values_per_long = 64 // bits
    expected_longs = math.ceil(4096 / values_per_long)
    if len(data) != expected_longs:
        raise ImportValidationError(
            "invalid_packed_block_state_length",
            f"packed block states need {expected_longs} longs, got {len(data)}",
            expected=expected_longs,
            actual=len(data),
        )
    values: list[int] = []
    for i in range(4096):
        li = i // values_per_long
        shift = (i - li * values_per_long) * bits
        raw = data[li]
        if not isinstance(raw, int):
            raise ImportValidationError("invalid_packed_block_state_word", f"packed word {li} is not an integer")
        value = (unsigned_long(raw) >> shift) & mask
        if value >= palette_len:
            raise ImportValidationError(
                "palette_index_out_of_range",
                f"cell {i} references palette index {value}, palette has {palette_len} entries",
                cell=i,
                paletteIndex=value,
            )
        values.append(value)
    return values


class ChunkStorageAdapter:
    __slots__ = ("adapter_id", "min_data_version", "max_data_version")

    def __init__(self, adapter_id: str, min_data_version: int, max_data_version: int) -> None:
        self.adapter_id = adapter_id
        self.min_data_version = min_data_version
        self.max_data_version = max_data_version

    def supports(self, data_version: int) -> bool:
        return self.min_data_version <= data_version <= self.max_data_version

    def sections(self, root: dict[str, Any], cx: int, cz: int) -> list[dict[str, Any]]:
        _reject_legacy_chunk_layout(root, cx, cz)
        sections = root.get("sections")
        if sections is None:
            return []
        if not isinstance(sections, list):
            raise ImportValidationError(
                "invalid_modern_sections",
                f"chunk {cx},{cz} sections is not a list",
                chunk=[cx, cz],
                adapter=self.adapter_id,
            )
        for index, section in enumerate(sections):
            if not isinstance(section, dict):
                raise ImportValidationError(
                    "invalid_modern_section",
                    f"chunk {cx},{cz} section {index} is not a compound",
                    chunk=[cx, cz],
                    section=index,
                    adapter=self.adapter_id,
                )
        return list(sections)

    def section_palette(
        self,
        section: dict[str, Any],
        cx: int,
        cz: int,
        section_index: int,
    ) -> tuple[list[dict[str, Any]], list[int] | None] | None:
        if "Palette" in section or "BlockStates" in section:
            raise ImportValidationError(
                "unsupported_legacy_chunk_storage",
                f"chunk {cx},{cz} section {section_index} uses legacy Palette/BlockStates",
                chunk=[cx, cz],
                section=section_index,
                adapter=self.adapter_id,
            )
        block_states = section.get("block_states")
        if block_states is None:
            return None
        if not isinstance(block_states, dict):
            raise ImportValidationError(
                "invalid_modern_block_states",
                f"chunk {cx},{cz} section {section_index} block_states is not a compound",
                chunk=[cx, cz],
                section=section_index,
                adapter=self.adapter_id,
            )
        unknown_fields = sorted(set(block_states) - {"palette", "data"})
        if unknown_fields:
            raise ImportValidationError(
                "unknown_modern_block_states_field",
                f"chunk {cx},{cz} section {section_index} has unknown block_states fields: {', '.join(unknown_fields)}",
                chunk=[cx, cz],
                section=section_index,
                fields=unknown_fields,
                adapter=self.adapter_id,
            )
        palette = block_states.get("palette")
        if not isinstance(palette, list):
            raise ImportValidationError(
                "invalid_modern_block_state_palette",
                f"chunk {cx},{cz} section {section_index} block_states.palette is not a list",
                chunk=[cx, cz],
                section=section_index,
                adapter=self.adapter_id,
            )
        data = block_states.get("data")
        if data is not None and not isinstance(data, list):
            raise ImportValidationError(
                "invalid_modern_block_state_data",
                f"chunk {cx},{cz} section {section_index} block_states.data is not a long array",
                chunk=[cx, cz],
                section=section_index,
                adapter=self.adapter_id,
            )
        return palette, data

    def block_entities(self, root: dict[str, Any], cx: int, cz: int) -> list[dict[str, Any]]:
        _reject_legacy_chunk_layout(root, cx, cz)
        entries = root.get("block_entities")
        if entries is None:
            return []
        if not isinstance(entries, list):
            raise ImportValidationError(
                "invalid_block_entities",
                f"chunk {cx},{cz} block_entities is not a list",
                chunk=[cx, cz],
                adapter=self.adapter_id,
            )
        return list(entries)


# 1.18 release (DataVersion 2860) introduced the root-level sections/block_states
# shape. Keep the upper bound audited: a future format change must add a new adapter
# instead of silently reusing this decoder. This range covers source 4671 and target 4897.
MODERN_ROOT_BLOCK_STATES_ADAPTER = ChunkStorageAdapter(
    adapter_id="java_anvil_root_sections_block_states_v1",
    min_data_version=2860,
    max_data_version=4897,
)
CHUNK_STORAGE_ADAPTERS: tuple[ChunkStorageAdapter, ...] = (
    MODERN_ROOT_BLOCK_STATES_ADAPTER,
)


def _reject_legacy_chunk_layout(root: dict[str, Any], cx: int, cz: int) -> None:
    level = root.get("Level")
    if isinstance(level, dict) and ("Sections" in level or "TileEntities" in level):
        raise ImportValidationError(
            "unsupported_legacy_chunk_storage",
            f"chunk {cx},{cz} uses legacy Level.Sections/TileEntities storage",
            chunk=[cx, cz],
        )


def chunk_storage_adapter_for_data_version(
    data_version: int,
    cx: int = 0,
    cz: int = 0,
) -> ChunkStorageAdapter:
    matches = [adapter for adapter in CHUNK_STORAGE_ADAPTERS if adapter.supports(data_version)]
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise ImportValidationError(
            "ambiguous_chunk_storage_adapter",
            f"DataVersion {data_version} matches multiple chunk-storage adapters",
            chunk=[cx, cz],
            dataVersion=data_version,
            adapters=[adapter.adapter_id for adapter in matches],
        )
    raise ImportValidationError(
        "unsupported_chunk_storage_data_version",
        f"chunk {cx},{cz} DataVersion {data_version} has no audited chunk-storage adapter",
        chunk=[cx, cz],
        dataVersion=data_version,
        supported=[
            {
                "adapter": adapter.adapter_id,
                "minDataVersion": adapter.min_data_version,
                "maxDataVersion": adapter.max_data_version,
            }
            for adapter in CHUNK_STORAGE_ADAPTERS
        ],
    )


def resolve_chunk_storage_adapter(
    root: dict[str, Any],
    data_version: int,
    cx: int,
    cz: int,
) -> ChunkStorageAdapter:
    adapter = chunk_storage_adapter_for_data_version(data_version, cx, cz)
    _reject_legacy_chunk_layout(root, cx, cz)
    return adapter


def section_palette(sec: dict[str, Any]) -> tuple[list[dict[str, Any]], list[int] | None] | None:
    """Modern-format compatibility wrapper used by the preset importer."""
    return MODERN_ROOT_BLOCK_STATES_ADAPTER.section_palette(sec, 0, 0, 0)


def block_state_text(entry: dict[str, Any], registry: BlockStateRegistry | None = None) -> str:
    registry = registry or load_block_state_registry()
    spec = registry.canonical_spec(entry)
    start = spec.find("[")
    return spec[start + 1 : -1] if start >= 0 else ""


def chunk_sections(root: dict[str, Any]) -> list[dict[str, Any]]:
    """Modern-format compatibility wrapper used by the preset importer."""
    return MODERN_ROOT_BLOCK_STATES_ADAPTER.sections(root, 0, 0)


def chunk_data_version(root: dict[str, Any]) -> int | None:
    value = root.get("DataVersion")
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    level = root.get("Level")
    value = level.get("DataVersion") if isinstance(level, dict) else None
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def validate_chunk_data_version(
    root: dict[str, Any],
    expected: int,
    cx: int,
    cz: int,
    allowed_compatible: set[int] | None = None,
) -> int:
    actual = chunk_data_version(root)
    if actual is None:
        raise ImportValidationError("missing_data_version", f"chunk {cx},{cz} is missing DataVersion", chunk=[cx, cz])
    if actual != expected and actual not in (allowed_compatible or set()):
        raise ImportValidationError(
            "data_version_mismatch",
            f"chunk {cx},{cz} has DataVersion {actual}, expected {expected}",
            chunk=[cx, cz],
            actual=actual,
            expected=expected,
        )
    chunk_storage_adapter_for_data_version(actual, cx, cz)
    return actual


def chunk_block_entities(root: dict[str, Any]) -> list[dict[str, Any]]:
    """Modern-format compatibility wrapper used by external Python tools."""
    return MODERN_ROOT_BLOCK_STATES_ADAPTER.block_entities(root, 0, 0)


def validate_block_entity_source_position(source_pos: tuple[Any, Any, Any], cx: int, cz: int) -> tuple[int, int, int]:
    if not all(isinstance(value, int) and not isinstance(value, bool) for value in source_pos):
        raise ImportValidationError("invalid_block_entity_position", f"chunk {cx},{cz} has invalid block entity coordinates")
    x, y, z = source_pos
    if floor_div(x, 16) != cx or floor_div(z, 16) != cz:
        raise ImportValidationError(
            "block_entity_chunk_mismatch",
            f"block entity at {source_pos} is stored in chunk {cx},{cz}",
            sourcePos=list(source_pos),
            chunk=[cx, cz],
        )
    return x, y, z


def opaque_block_entity_record(
    entry: Any,
    data_version: int,
    lo: tuple[int, int, int],
    origin: tuple[int, int, int],
    keep_coords: bool,
) -> tuple[tuple[int, int, int], dict[str, Any], dict[str, Any]]:
    if not isinstance(entry, NBTCompound):
        raise ImportValidationError("invalid_block_entity", "block entity is not a typed NBT compound")
    type_name = entry.get("id")
    if not isinstance(type_name, str) or not re.fullmatch(r"[a-z0-9_.-]+:[a-z0-9_./-]+", type_name):
        raise ImportValidationError("invalid_block_entity_id", f"invalid block entity id: {type_name!r}")
    coords = []
    for key in ("x", "y", "z"):
        value = entry.get(key)
        if not isinstance(value, int) or isinstance(value, bool):
            raise ImportValidationError("invalid_block_entity_position", f"block entity {type_name} has invalid {key}")
        coords.append(value)
    x, y, z = coords
    bx, by, bz = dest_pos(x, y, z, lo, origin, keep_coords)
    cx, cy, cz = floor_div(bx, CS), floor_div(by, CS), floor_div(bz, CH)
    lx, ly, lz = bx - cx * CS, by - cy * CS, bz - cz * CH
    local_index = lx + ly * CS + lz * CS2
    record = {
        "localIndex": local_index,
        "sourceType": type_name,
        "sourceDataVersion": data_version,
        "sourcePos": [x, y, z],
        "encoding": "typed-nbt-json-v1",
        "data": typed_nbt_value(entry, TAG_COMPOUND),
    }
    event = {
        "code": "block_entity_opaque_preserved",
        "severity": "warning",
        "action": "preserve_opaque",
        "sourceType": type_name,
        "sourcePos": [x, y, z],
        "targetPos": [bx, by, bz],
        "sourceDataVersion": data_version,
    }
    return (cx, cy, cz), record, event


def make_loss_report(events: list[dict[str, Any]]) -> dict[str, Any]:
    ordered = sorted(
        events,
        key=lambda event: (
            tuple(event.get("sourcePos") or [0, 0, 0]),
            str(event.get("code") or ""),
            str(event.get("sourceType") or ""),
        ),
    )
    counts: dict[str, int] = {}
    for event in ordered:
        code = str(event.get("code") or "unknown")
        counts[code] = counts.get(code, 0) + 1
    loss_events = [event for event in ordered if event.get("loss") is True]
    opaque_events = [event for event in ordered if event.get("action") == "preserve_opaque"]
    return {
        "formatVersion": 1,
        "archiveRoundTripLossless": len(loss_events) == 0,
        "runtimeComplete": len(ordered) == 0,
        "lossCount": len(loss_events),
        "eventCount": len(ordered),
        "opaqueCount": len(opaque_events),
        "opaqueBlockEntities": sum(1 for event in opaque_events if event.get("code") == "block_entity_opaque_preserved"),
        "substitutedOrDropped": len(loss_events) > 0,
        "counts": {key: counts[key] for key in sorted(counts)},
        "events": ordered,
    }


def read_region_chunk(region_path: pathlib.Path, local_cx: int, local_cz: int) -> dict[str, Any] | None:
    with region_path.open("rb") as f:
        file_size = region_path.stat().st_size
        header = f.read(8192)
        if len(header) < 8192:
            return None
        idx = local_cx + local_cz * 32
        loc = header[idx * 4 : idx * 4 + 4]
        sector_offset = int.from_bytes(loc[:3], "big")
        sector_count = loc[3]
        if sector_offset == 0 or sector_count == 0:
            return None
        if sector_offset < 2:
            raise ImportValidationError("invalid_region_sector", f"{region_path.name} chunk points into the region header")
        sector_start = sector_offset * 4096
        sector_bytes = sector_count * 4096
        if sector_start + sector_bytes > file_size:
            raise ImportValidationError("region_sector_out_of_bounds", f"{region_path.name} chunk allocation exceeds file size")
        f.seek(sector_start)
        length_raw = f.read(4)
        if len(length_raw) != 4:
            raise ImportValidationError("truncated_region_chunk", f"{region_path.name} chunk length is truncated")
        length = struct.unpack(">I", length_raw)[0]
        if length < 1 or length + 4 > sector_bytes:
            raise ImportValidationError(
                "invalid_region_chunk_length",
                f"{region_path.name} chunk length {length} exceeds {sector_count} allocated sectors",
            )
        compression = f.read(1)
        payload = f.read(max(0, length - 1))
        if len(compression) != 1 or len(payload) != length - 1:
            raise ImportValidationError("truncated_region_chunk", f"{region_path.name} chunk payload is truncated")

    if not payload:
        return None
    ctype = compression[0]
    if ctype == 1:
        payload = decompress_zlib_limited(payload, 16 + zlib.MAX_WBITS)
    elif ctype == 2:
        payload = decompress_zlib_limited(payload, zlib.MAX_WBITS)
    elif ctype == 3:
        if len(payload) > MAX_DECOMPRESSED_CHUNK_BYTES:
            raise ImportValidationError("chunk_decompressed_too_large", "uncompressed chunk exceeds 64 MiB")
    else:
        raise ValueError(f"{region_path.name} chunk {local_cx},{local_cz} 使用暂不支持的压缩类型 {ctype}")
    return read_nbt(payload)


def in_box(x: int, y: int, z: int, lo: tuple[int, int, int], hi: tuple[int, int, int]) -> bool:
    return lo[0] <= x <= hi[0] and lo[1] <= y <= hi[1] and lo[2] <= z <= hi[2]


def dest_pos(
    x: int,
    y: int,
    z: int,
    lo: tuple[int, int, int],
    origin: tuple[int, int, int],
    keep_coords: bool,
) -> tuple[int, int, int]:
    if keep_coords:
        return x + origin[0], z + origin[1], y + origin[2]
    return x - lo[0] + origin[0], z - lo[2] + origin[1], y - lo[1] + origin[2]


def add_output_block(
    chunks: dict[tuple[int, int, int], list[Any]],
    palette: list[str],
    palette_index: dict[str, int],
    bx: int,
    by: int,
    bz: int,
    name: str,
    state: str,
) -> bool:
    spec = f"{name}[{state}]" if state else name
    return add_output_state(chunks, palette, palette_index, bx, by, bz, spec)


def add_output_state(
    chunks: dict[tuple[int, int, int], list[Any]],
    palette: list[str],
    palette_index: dict[str, int],
    bx: int,
    by: int,
    bz: int,
    spec: str,
) -> bool:
    cx, cy, cz = floor_div(bx, CS), floor_div(by, CS), floor_div(bz, CH)
    lx, ly, lz = bx - cx * CS, by - cy * CS, bz - cz * CH
    li = lx + ly * CS + lz * CS2
    pi = palette_index.get(spec)
    if pi is None:
        pi = len(palette)
        palette.append(spec)
        palette_index[spec] = pi
    chunks.setdefault((cx, cy, cz), []).extend([li, pi])
    return True


def find_region_dir(world: pathlib.Path, dimension: str) -> pathlib.Path:
    if dimension in ("overworld", "minecraft:overworld"):
        candidates = [
            world / "region",
            world / "dimensions" / "minecraft" / "overworld" / "region",
        ]
    elif dimension in ("the_nether", "nether", "minecraft:the_nether"):
        candidates = [
            world / "DIM-1" / "region",
            world / "dimensions" / "minecraft" / "the_nether" / "region",
        ]
    elif dimension in ("the_end", "end", "minecraft:the_end"):
        candidates = [
            world / "DIM1" / "region",
            world / "dimensions" / "minecraft" / "the_end" / "region",
        ]
    else:
        dim = dimension.replace("minecraft:", "")
        candidates = [world / "dimensions" / "minecraft" / dim / "region"]
    for path in candidates:
        if path.is_dir():
            return path
    raise FileNotFoundError("找不到 region 目录，试过: " + ", ".join(str(p) for p in candidates))


def import_world(args: argparse.Namespace, registry: BlockStateRegistry | None = None) -> dict[str, Any]:
    registry = registry or load_block_state_registry()
    world = pathlib.Path(args.world)
    region_dir = find_region_dir(world, args.dimension)

    p1 = tuple(args.from_pos)
    p2 = tuple(args.to_pos)
    lo = (min(p1[0], p2[0]), min(p1[1], p2[1]), min(p1[2], p2[2]))
    hi = (max(p1[0], p2[0]), max(p1[1], p2[1]), max(p1[2], p2[2]))
    origin = tuple(args.origin)
    chunks: dict[tuple[int, int, int], list[Any]] = {}
    out_palette: list[str] = []
    out_palette_index: dict[str, int] = {}
    unique: dict[str, int] = {}
    scanned_chunks = 0
    missing_chunks = 0
    emitted = 0
    data_versions: set[int] = set()
    opaque_entities: dict[tuple[int, int, int], list[dict[str, Any]]] = {}
    loss_events: list[dict[str, Any]] = []
    allowed_data_versions = set(getattr(args, "allow_data_version", None) or [])

    cxs = chunk_range(lo[0], hi[0])
    czs = chunk_range(lo[2], hi[2])
    for rx in region_range(cxs.start, cxs.stop - 1):
        for rz in region_range(czs.start, czs.stop - 1):
            region_path = region_dir / f"r.{rx}.{rz}.mca"
            if not region_path.exists():
                continue
            for cx in cxs:
                if floor_div(cx, 32) != rx:
                    continue
                for cz in czs:
                    if floor_div(cz, 32) != rz:
                        continue
                    root = read_region_chunk(region_path, cx % 32, cz % 32)
                    if not root:
                        missing_chunks += 1
                        continue
                    scanned_chunks += 1
                    data_version = validate_chunk_data_version(
                        root, registry.world_version, cx, cz, allowed_data_versions,
                    )
                    storage_adapter = resolve_chunk_storage_adapter(root, data_version, cx, cz)
                    data_versions.add(data_version)
                    for section_index, sec in enumerate(storage_adapter.sections(root, cx, cz)):
                        sy = section_y(sec)
                        if sy is None:
                            continue
                        y_base = sy * 16
                        if y_base > hi[1] or y_base + 15 < lo[1]:
                            continue
                        pal_data = storage_adapter.section_palette(sec, cx, cz, section_index)
                        if not pal_data:
                            continue
                        palette, packed = pal_data
                        canonical_palette = [registry.canonical_spec(entry) for entry in palette]
                        indices = unpack_palette_indices(packed, len(palette))
                        for i, pi in enumerate(indices):
                            spec = canonical_palette[pi]
                            name = spec.split("[", 1)[0]
                            if name in AIR:
                                continue
                            lx = i & 15
                            lz = (i >> 4) & 15
                            ly = (i >> 8) & 15
                            x = cx * 16 + lx
                            y = y_base + ly
                            z = cz * 16 + lz
                            if not in_box(x, y, z, lo, hi):
                                continue
                            bx, by, bz = dest_pos(x, y, z, lo, origin, args.keep_coords)
                            if not add_output_state(chunks, out_palette, out_palette_index, bx, by, bz, spec):
                                continue
                            unique[name] = unique.get(name, 0) + 1
                            emitted += 1
                            if args.max_blocks and emitted >= args.max_blocks:
                                return make_output(
                                    args, chunks, out_palette, unique, scanned_chunks, missing_chunks, emitted, True,
                                    registry, data_versions, opaque_entities, loss_events,
                                )

                    seen_block_entities: set[tuple[int, int, int]] = set()
                    for block_entity in storage_adapter.block_entities(root, cx, cz):
                        if not isinstance(block_entity, dict):
                            raise ImportValidationError("invalid_block_entity", f"chunk {cx},{cz} has a non-compound block entity")
                        source_pos = tuple(block_entity.get(key) for key in ("x", "y", "z"))
                        x, y, z = validate_block_entity_source_position(source_pos, cx, cz)
                        if source_pos in seen_block_entities:
                            raise ImportValidationError("duplicate_block_entity", f"duplicate block entity at {source_pos}")
                        seen_block_entities.add(source_pos)
                        if not in_box(x, y, z, lo, hi):
                            continue
                        out_key, record, event = opaque_block_entity_record(
                            block_entity, data_version, lo, origin, args.keep_coords,
                        )
                        opaque_entities.setdefault(out_key, []).append(record)
                        loss_events.append(event)

    return make_output(
        args, chunks, out_palette, unique, scanned_chunks, missing_chunks, emitted, False,
        registry, data_versions, opaque_entities, loss_events,
    )


def make_output(
    args: argparse.Namespace,
    chunks: dict[tuple[int, int, int], list[Any]],
    palette: list[str],
    unique: dict[str, int],
    scanned_chunks: int,
    missing_chunks: int,
    emitted: int,
    clipped: bool,
    registry: BlockStateRegistry | None = None,
    data_versions: set[int] | None = None,
    opaque_entities: dict[tuple[int, int, int], list[dict[str, Any]]] | None = None,
    loss_events: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    registry = registry or load_block_state_registry()
    opaque_entities = opaque_entities or {}
    all_chunk_keys = sorted(set(chunks) | set(opaque_entities))
    chunk_list = []
    for cx, cy, cz in all_chunk_keys:
        record: list[Any] = [cx, cy, cz, pack_cells(chunks.get((cx, cy, cz), []))]
        sidecar = sorted(opaque_entities.get((cx, cy, cz), []), key=lambda item: (item["localIndex"], item["sourceType"]))
        if sidecar:
            record.append(sidecar)
        chunk_list.append(record)
    metadata = registry.metadata
    observed_versions = sorted(data_versions or [])
    if len(observed_versions) > 1:
        raise ImportValidationError("mixed_data_versions", f"import contains mixed DataVersions: {observed_versions}")
    source_data_version = observed_versions[0] if observed_versions else registry.world_version
    source_storage_adapter = chunk_storage_adapter_for_data_version(source_data_version)
    source = {
        "format": "minecraft_java_anvil",
        "storageAdapter": source_storage_adapter.adapter_id,
        "minecraftVersion": metadata["minecraftId"] if source_data_version == registry.world_version else f"DataVersion-{source_data_version}",
        "targetMinecraftVersion": metadata["minecraftId"],
        "dataVersion": source_data_version,
        "targetDataVersion": registry.world_version,
        "compatibility": "exact" if source_data_version == registry.world_version else "schema-validated-state-only",
        "series": metadata["seriesId"],
        "dimension": args.dimension,
        "world": str(pathlib.Path(args.world)),
        "from": list(args.from_pos),
        "to": list(args.to_pos),
        "origin": list(args.origin),
        "keepCoords": bool(args.keep_coords),
        "maxBlocks": int(args.max_blocks or 0),
        "observedDataVersions": observed_versions,
    }
    target = {
        "schemaHash": metadata["schemaHash"],
        "blockReportSha256": metadata["reportSha256"],
        "blockSize": BS,
        "chunkSize": CS,
        "chunkHeight": CH,
        "sectionHeight": 16,
    }
    report_events = list(loss_events or [])
    if clipped:
        report_events.append({
            "code": "max_blocks_clipped",
            "severity": "warning",
            "action": "truncate",
            "loss": True,
        })
    return {
        "format": "mc_world_import",
        "formatVersion": 1,
        "v": PHASE5_FORMAT_VERSION,
        "h": CH,
        "source": source,
        "target": target,
        "schemaHash": metadata["schemaHash"],
        "p": palette,
        "stats": {
            "blocks": emitted,
            "chunks": len(chunk_list),
            "scannedMcChunks": scanned_chunks,
            "missingMcChunks": missing_chunks,
            "uniqueBlocks": len(unique),
            "clippedByMaxBlocks": clipped,
            "opaqueBlockEntities": sum(len(items) for items in opaque_entities.values()),
        },
        "lossReport": make_loss_report(report_events),
        "chunks": chunk_list,
    }


def pack_cells(cells: list[Any]) -> str:
    pairs: list[str] = []
    for i in range(0, len(cells), 2):
        pairs.append(f"{cells[i]}:{cells[i + 1]}")
    return ";".join(pairs)


def cell_count(cells: Any) -> int:
    if isinstance(cells, list):
        return len(cells) // 2
    if isinstance(cells, str):
        return 0 if cells == "" else cells.count(";") + 1
    return 0


def saved_block_count(data: dict[str, Any]) -> tuple[int, int]:
    chunks = data.get("chunks")
    if not isinstance(chunks, list):
        return 0, 0
    blocks = 0
    for chunk in chunks:
        if isinstance(chunk, list) and len(chunk) >= 4:
            blocks += cell_count(chunk[3])
    return len(chunks), blocks


def make_gmod_save_data(
    import_data: dict[str, Any],
    slot_name: str,
    map_name: str,
    saved_at: int | None = None,
    block_size: float = BS,
) -> dict[str, Any]:
    chunks = import_data.get("chunks") or []
    if import_data.get("format") == "mc_world_import" and int(import_data.get("v") or 0) >= PHASE5_FORMAT_VERSION:
        chunks = [import_chunk_record_to_save_record(chunk) for chunk in chunks]
    out = {
        "v": int(import_data.get("v") or 5),
        "map": map_name,
        "slotName": slot_name,
        "savedAt": int(saved_at if saved_at is not None else time.time()),
        "bs": block_size,
        "cs": CS,
        "h": int(import_data.get("h") or CH),
        "p": import_data.get("p") or [],
        "chunks": chunks,
    }
    if "stats" in import_data:
        out["stats"] = import_data["stats"]
    for key in ("format", "formatVersion", "source", "target", "schemaHash", "lossReport"):
        if key in import_data:
            out[key] = import_data[key]
    if isinstance(out.get("target"), dict):
        out["target"] = dict(out["target"])
        out["target"]["blockSize"] = block_size
    return out


def import_chunk_record_to_save_record(chunk: Any) -> Any:
    """Move import-bundle opaque Block Entities from slot 5 to save-v10 slot 6."""
    if not isinstance(chunk, list) or len(chunk) < 5:
        return chunk
    if len(chunk) > 5 and chunk[5] not in (None, [], {}):
        raise ImportValidationError(
            "unsupported_import_opaque_states_in_save",
            "cannot convert import opaqueStates to the v10 save format",
        )
    return [chunk[0], chunk[1], chunk[2], chunk[3], [], chunk[4]]


def copy_bundle_metadata(target: dict[str, Any], source: dict[str, Any]) -> None:
    for key in ("format", "formatVersion", "source", "target", "schemaHash", "lossReport"):
        if key in source:
            target[key] = source[key]


def finalize_stream_bundle_metadata(save_fields: dict[str, Any], stats: dict[str, Any]) -> None:
    events = stats.pop("_lossEvents", [])
    observed = sorted(stats.pop("_observedDataVersions", set()))
    if len(observed) > 1:
        raise ImportValidationError("mixed_data_versions", f"import contains mixed DataVersions: {observed}")
    if observed and isinstance(save_fields.get("source"), dict):
        actual = observed[0]
        target = int(save_fields["source"].get("targetDataVersion") or save_fields["source"].get("dataVersion") or actual)
        save_fields["source"]["dataVersion"] = actual
        save_fields["source"]["observedDataVersions"] = observed
        save_fields["source"]["storageAdapter"] = chunk_storage_adapter_for_data_version(actual).adapter_id
        save_fields["source"]["compatibility"] = "exact" if actual == target else "schema-validated-state-only"
        if actual != target:
            save_fields["source"]["minecraftVersion"] = f"DataVersion-{actual}"
    if int(save_fields.get("v") or 0) >= PHASE5_FORMAT_VERSION:
        save_fields["lossReport"] = make_loss_report(events)


def gmod_data_relative_path(path: pathlib.Path) -> str:
    parts = path.parts
    lowered = [p.lower() for p in parts]
    for i in range(len(parts) - 1):
        if lowered[i] == "mc" and lowered[i + 1] == "saves":
            return pathlib.PurePosixPath(*parts[i:]).as_posix()
    if "data" in lowered:
        i = len(lowered) - 1 - lowered[::-1].index("data")
        if i + 1 < len(parts):
            return pathlib.PurePosixPath(*parts[i + 1 :]).as_posix()
    return path.name


def gmod_lzma_compress(data: bytes) -> bytes:
    return lzma.compress(data, format=lzma.FORMAT_ALONE, filters=LZMA_FILTERS)


def write_gmod_json(path: pathlib.Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")


def write_gmod_save_meta(
    save_path: pathlib.Path,
    slot_name: str,
    map_name: str,
    saved_at: int,
    block_size: float,
    chunk_height: int,
    chunks: int,
    blocks: int,
) -> pathlib.Path:
    meta = {
        "v": 1,
        "name": slot_name,
        "path": gmod_data_relative_path(save_path),
        "map": map_name or "unknown",
        "savedAt": saved_at,
        "savedAtText": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(saved_at)),
        "bs": block_size,
        "h": chunk_height,
        "chunks": chunks,
        "blocks": blocks,
    }
    meta_path = save_path.with_suffix(".json")
    meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    return meta_path


def write_gmod_save_files(save_path: pathlib.Path, save_data: dict[str, Any]) -> list[pathlib.Path]:
    save_path.parent.mkdir(parents=True, exist_ok=True)
    write_gmod_json(save_path, save_data)

    slot_name = gmod_save_slot_name(str(save_data.get("slotName") or save_path.stem)) or save_path.stem
    chunks, blocks = saved_block_count(save_data)
    saved_at = int(save_data.get("savedAt") or time.time())
    meta_path = write_gmod_save_meta(
        save_path,
        slot_name,
        str(save_data.get("map") or "unknown"),
        saved_at,
        float(save_data.get("bs") or BS),
        int(save_data.get("h") or CH),
        chunks,
        blocks,
    )
    return [save_path, meta_path]


def write_gmod_save_chunked(
    save_path: pathlib.Path,
    save_fields: dict[str, Any],
    chunks: Any,
    palette: list[str],
    stats: dict[str, Any],
    max_part_json_bytes: int = DEFAULT_SAVE_PART_JSON_BYTES,
) -> list[pathlib.Path]:
    save_path.parent.mkdir(parents=True, exist_ok=True)
    slot_name = gmod_save_slot_name(str(save_fields.get("slotName") or save_path.stem)) or save_path.stem
    map_name = str(save_fields.get("map") or "unknown")
    saved_at = int(save_fields.get("savedAt") or time.time())
    block_size = float(save_fields.get("bs") or BS)
    chunk_height = int(save_fields.get("h") or CH)
    max_part_json_bytes = max(1024, int(max_part_json_bytes or DEFAULT_SAVE_PART_JSON_BYTES))

    part_dir = save_path.with_name(save_path.stem + "_parts")
    if part_dir.exists():
        shutil.rmtree(part_dir)
    part_dir.mkdir(parents=True, exist_ok=True)

    written_parts: list[pathlib.Path] = []
    part_names: list[Any] = []
    part_chunks: list[Any] = []
    part_bytes = 96
    artifact_version = int(save_fields.get("v") or 7)

    def flush_part() -> None:
        nonlocal part_chunks, part_bytes
        if not part_chunks:
            return
        index = len(written_parts) + 1
        part_path = part_dir / f"part_{index:05d}.dat"
        part_data = {
            "v": artifact_version if artifact_version >= PHASE5_FORMAT_VERSION else 5,
            "h": chunk_height,
            "chunks": part_chunks,
        }
        write_gmod_json(part_path, part_data)
        written_parts.append(part_path)
        part_names.append(gmod_data_relative_path(part_path))
        part_chunks = []
        part_bytes = 96

    for chunk in chunks:
        chunk_json_bytes = len(json.dumps(chunk, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
        if part_chunks and part_bytes + chunk_json_bytes + 1 > max_part_json_bytes:
            flush_part()
        part_chunks.append(chunk)
        part_bytes += chunk_json_bytes + 1
    flush_part()
    finalize_stream_bundle_metadata(save_fields, stats)

    manifest_stats = dict(stats)
    manifest_stats["parts"] = len(part_names)
    manifest = {
        "v": artifact_version,
        "map": map_name,
        "slotName": slot_name,
        "savedAt": saved_at,
        "bs": block_size,
        "cs": CS,
        "h": chunk_height,
        "chunks": [],
        "p": palette,
        "parts": part_names,
        "stats": manifest_stats,
    }
    source = save_fields.get("source")
    if source is not None:
        manifest["source"] = source
    copy_bundle_metadata(manifest, save_fields)
    write_gmod_json(save_path, manifest)

    meta_path = write_gmod_save_meta(
        save_path,
        slot_name,
        map_name,
        saved_at,
        block_size,
        chunk_height,
        int(manifest_stats.get("chunks") or 0),
        int(manifest_stats.get("blocks") or 0),
    )
    return [save_path, meta_path] + written_parts


def chunk_key_text(cx: int, cy: int, cz: int) -> str:
    return f"{int(cx)}:{int(cy)}:{int(cz)}"


def write_gmod_save_streaming(
    save_path: pathlib.Path,
    save_fields: dict[str, Any],
    chunks: Any,
    palette: list[str],
    stats: dict[str, Any],
    max_part_json_bytes: int = DEFAULT_SAVE_PART_JSON_BYTES,
) -> list[pathlib.Path]:
    save_path.parent.mkdir(parents=True, exist_ok=True)
    slot_name = gmod_save_slot_name(str(save_fields.get("slotName") or save_path.stem)) or save_path.stem
    map_name = str(save_fields.get("map") or "unknown")
    saved_at = int(save_fields.get("savedAt") or time.time())
    block_size = float(save_fields.get("bs") or BS)
    chunk_height = int(save_fields.get("h") or CH)
    max_part_json_bytes = max(1024, int(max_part_json_bytes or DEFAULT_SAVE_PART_JSON_BYTES))

    part_dir = save_path.with_name(save_path.stem + "_parts")
    if part_dir.exists():
        shutil.rmtree(part_dir)
    part_dir.mkdir(parents=True, exist_ok=True)

    written_parts: list[pathlib.Path] = []
    part_entries: list[dict[str, Any]] = []
    chunk_index: dict[str, dict[str, int]] = {}
    part_chunks: list[Any] = []
    part_bytes = 96
    artifact_version = int(save_fields.get("v") or 8)

    def flush_part() -> None:
        nonlocal part_chunks, part_bytes
        if not part_chunks:
            return
        part_no = len(written_parts) + 1
        part_name = f"part_{part_no:05d}.json"
        part_path = part_dir / part_name
        part_data = {
            "v": artifact_version,
            "h": chunk_height,
            "chunks": part_chunks,
        }
        write_gmod_json(part_path, part_data)
        part_entries.append({
            "path": f"{save_path.stem}_parts/{part_name}",
            "chunks": len(part_chunks),
        })
        written_parts.append(part_path)
        part_chunks = []
        part_bytes = 96

    for chunk in chunks:
        if not isinstance(chunk, list) or len(chunk) < 4:
            continue
        chunk_json = json.dumps(chunk, ensure_ascii=False, separators=(",", ":"))
        chunk_json_bytes = len(chunk_json.encode("utf-8"))
        if part_chunks and part_bytes + chunk_json_bytes + 1 > max_part_json_bytes:
            flush_part()
        part_no = len(written_parts) + 1
        index_in_part = len(part_chunks) + 1
        cx, cy, cz = int(chunk[0]), int(chunk[1]), int(chunk[2])
        chunk_index[chunk_key_text(cx, cy, cz)] = {
            "part": part_no,
            "index": index_in_part,
        }
        part_chunks.append(chunk)
        part_bytes += chunk_json_bytes + 1
    flush_part()
    finalize_stream_bundle_metadata(save_fields, stats)

    manifest_stats = dict(stats)
    manifest_stats["parts"] = len(part_entries)
    manifest_stats["chunks"] = int(manifest_stats.get("chunks") or len(chunk_index))
    manifest = {
        "v": artifact_version,
        "map": map_name,
        "slotName": slot_name,
        "savedAt": saved_at,
        "bs": block_size,
        "cs": CS,
        "h": chunk_height,
        "chunks": [],
        "p": palette,
        "parts": part_entries,
        "chunkIndex": chunk_index,
        "editOverlay": f"{save_path.stem}_edits.dat",
        "stats": manifest_stats,
    }
    source = save_fields.get("source")
    if source is not None:
        manifest["source"] = source
    copy_bundle_metadata(manifest, save_fields)
    write_gmod_json(save_path, manifest)

    meta_path = write_gmod_save_meta(
        save_path,
        slot_name,
        map_name,
        saved_at,
        block_size,
        chunk_height,
        int(manifest_stats.get("chunks") or 0),
        int(manifest_stats.get("blocks") or 0),
    )
    return [save_path, meta_path] + written_parts


def write_gmod_save_stream(
    save_path: pathlib.Path,
    save_fields: dict[str, Any],
    chunks: Any,
    palette: list[str],
    stats: dict[str, Any],
) -> list[pathlib.Path]:
    save_path.parent.mkdir(parents=True, exist_ok=True)
    slot_name = gmod_save_slot_name(str(save_fields.get("slotName") or save_path.stem)) or save_path.stem
    map_name = str(save_fields.get("map") or "unknown")
    saved_at = int(save_fields.get("savedAt") or time.time())
    block_size = float(save_fields.get("bs") or BS)
    chunk_height = int(save_fields.get("h") or CH)
    artifact_version = int(save_fields.get("v") or 5)

    with save_path.open("wb") as f:
        def write_json(text: str) -> None:
            f.write(text.encode("utf-8"))

        write_json("{")
        write_json('"v":' + str(artifact_version))
        write_json(',"map":' + json.dumps(map_name, ensure_ascii=False, separators=(",", ":")))
        write_json(',"slotName":' + json.dumps(slot_name, ensure_ascii=False, separators=(",", ":")))
        write_json(',"savedAt":' + str(saved_at))
        write_json(',"bs":' + json.dumps(block_size, separators=(",", ":")))
        write_json(',"cs":' + str(CS))
        write_json(',"h":' + str(chunk_height))
        write_json(',"chunks":[')
        first = True
        for chunk in chunks:
            if not first:
                write_json(",")
            first = False
            write_json(json.dumps(chunk, ensure_ascii=False, separators=(",", ":")))
        write_json('],"p":')
        write_json(json.dumps(palette, ensure_ascii=False, separators=(",", ":")))
        source = save_fields.get("source")
        if source is not None:
            write_json(',"source":' + json.dumps(source, ensure_ascii=False, separators=(",", ":")))
        for key in ("format", "formatVersion", "target", "schemaHash", "lossReport"):
            if key in save_fields:
                write_json(',"' + key + '":' + json.dumps(save_fields[key], ensure_ascii=False, separators=(",", ":")))
        write_json("}")

    meta_path = write_gmod_save_meta(
        save_path,
        slot_name,
        map_name,
        saved_at,
        block_size,
        chunk_height,
        int(stats.get("chunks") or 0),
        int(stats.get("blocks") or 0),
    )
    return [save_path, meta_path]


def can_stream_gmod_save(args: argparse.Namespace) -> bool:
    p1 = tuple(args.from_pos)
    p2 = tuple(args.to_pos)
    lo_x, hi_x = sorted((p1[0], p2[0]))
    lo_z, hi_z = sorted((p1[2], p2[2]))
    origin = tuple(args.origin)
    return (
        lo_x % CS == 0
        and (hi_x + 1) % CS == 0
        and lo_z % CS == 0
        and (hi_z + 1) % CS == 0
        and origin[0] % CS == 0
        and origin[1] % CS == 0
        and origin[2] % CH == 0
    )


def estimate_gmod_chunk_count(args: argparse.Namespace) -> int:
    p1 = tuple(args.from_pos)
    p2 = tuple(args.to_pos)
    lo = (min(p1[0], p2[0]), min(p1[1], p2[1]), min(p1[2], p2[2]))
    hi = (max(p1[0], p2[0]), max(p1[1], p2[1]), max(p1[2], p2[2]))
    width_x = max(1, hi[0] - lo[0] + 1)
    width_y = max(1, hi[1] - lo[1] + 1)
    width_z = max(1, hi[2] - lo[2] + 1)
    return math.ceil(width_x / CS) * math.ceil(width_z / CS) * math.ceil(width_y / CH)


def choose_save_format(args: argparse.Namespace, write_json: bool) -> str:
    requested = str(getattr(args, "save_format", "auto") or "auto")
    if requested != "auto":
        return requested
    if write_json:
        return "classic"
    if can_stream_gmod_save(args) and estimate_gmod_chunk_count(args) >= 2048:
        return "streaming"
    if can_stream_gmod_save(args):
        return "chunked"
    return "classic"


def iter_gmod_save_chunks_from_world(
    args: argparse.Namespace,
    stats: dict[str, Any],
    palette: list[str],
    palette_index: dict[str, int],
    registry: BlockStateRegistry | None = None,
) -> Any:
    registry = registry or load_block_state_registry()
    world = pathlib.Path(args.world)
    region_dir = find_region_dir(world, args.dimension)
    p1 = tuple(args.from_pos)
    p2 = tuple(args.to_pos)
    lo = (min(p1[0], p2[0]), min(p1[1], p2[1]), min(p1[2], p2[2]))
    hi = (max(p1[0], p2[0]), max(p1[1], p2[1]), max(p1[2], p2[2]))
    origin = tuple(args.origin)
    cxs = chunk_range(lo[0], hi[0])
    czs = chunk_range(lo[2], hi[2])
    unique: set[str] = set()
    limit_reached = False
    allowed_data_versions = set(getattr(args, "allow_data_version", None) or [])
    observed_data_versions: set[int] = set()

    for rx in region_range(cxs.start, cxs.stop - 1):
        for rz in region_range(czs.start, czs.stop - 1):
            region_path = region_dir / f"r.{rx}.{rz}.mca"
            if not region_path.exists():
                continue
            for cx in cxs:
                if floor_div(cx, 32) != rx:
                    continue
                for cz in czs:
                    if floor_div(cz, 32) != rz:
                        continue
                    root = read_region_chunk(region_path, cx % 32, cz % 32)
                    if not root:
                        stats["missingMcChunks"] += 1
                        continue
                    stats["scannedMcChunks"] += 1
                    data_version = validate_chunk_data_version(
                        root, registry.world_version, cx, cz, allowed_data_versions,
                    )
                    storage_adapter = resolve_chunk_storage_adapter(root, data_version, cx, cz)
                    observed_data_versions.add(data_version)
                    stats["_observedDataVersions"] = observed_data_versions
                    chunks: dict[tuple[int, int, int], list[Any]] = {}
                    opaque_entities: dict[tuple[int, int, int], list[dict[str, Any]]] = {}
                    for section_index, sec in enumerate(storage_adapter.sections(root, cx, cz)):
                        sy = section_y(sec)
                        if sy is None:
                            continue
                        y_base = sy * 16
                        if y_base > hi[1] or y_base + 15 < lo[1]:
                            continue
                        pal_data = storage_adapter.section_palette(sec, cx, cz, section_index)
                        if not pal_data:
                            continue
                        source_palette, packed = pal_data
                        canonical_palette = [registry.canonical_spec(entry) for entry in source_palette]
                        indices = unpack_palette_indices(packed, len(source_palette))
                        for i, pi in enumerate(indices):
                            spec = canonical_palette[pi]
                            name = spec.split("[", 1)[0]
                            if name in AIR:
                                continue
                            lx = i & 15
                            lz = (i >> 4) & 15
                            ly = (i >> 8) & 15
                            x = cx * 16 + lx
                            y = y_base + ly
                            z = cz * 16 + lz
                            if not in_box(x, y, z, lo, hi):
                                continue
                            bx, by, bz = dest_pos(x, y, z, lo, origin, args.keep_coords)
                            add_output_state(chunks, palette, palette_index, bx, by, bz, spec)
                            unique.add(name)
                            stats["blocks"] += 1
                            if args.max_blocks and stats["blocks"] >= args.max_blocks:
                                limit_reached = True
                                break
                        if limit_reached:
                            break

                    seen_block_entities: set[tuple[int, int, int]] = set()
                    for block_entity in storage_adapter.block_entities(root, cx, cz):
                        if not isinstance(block_entity, dict):
                            raise ImportValidationError("invalid_block_entity", f"chunk {cx},{cz} has a non-compound block entity")
                        source_pos = tuple(block_entity.get(key) for key in ("x", "y", "z"))
                        x, y, z = validate_block_entity_source_position(source_pos, cx, cz)
                        if source_pos in seen_block_entities:
                            raise ImportValidationError("duplicate_block_entity", f"duplicate block entity at {source_pos}")
                        seen_block_entities.add(source_pos)
                        if not in_box(x, y, z, lo, hi):
                            continue
                        out_key, record, event = opaque_block_entity_record(
                            block_entity, data_version, lo, origin, args.keep_coords,
                        )
                        opaque_entities.setdefault(out_key, []).append(record)
                        stats.setdefault("_lossEvents", []).append(event)
                        stats["opaqueBlockEntities"] = int(stats.get("opaqueBlockEntities") or 0) + 1

                    for out_key in sorted(set(chunks) | set(opaque_entities)):
                        cells = chunks.get(out_key, [])
                        stats["chunks"] += 1
                        record: list[Any] = [out_key[0], out_key[1], out_key[2], pack_cells(cells)]
                        sidecar = sorted(opaque_entities.get(out_key, []), key=lambda item: (item["localIndex"], item["sourceType"]))
                        if sidecar:
                            record.extend([[], sidecar])
                        yield record
                    if limit_reached:
                        stats["clippedByMaxBlocks"] = True
                        stats.setdefault("_lossEvents", []).append({
                            "code": "max_blocks_clipped",
                            "severity": "warning",
                            "action": "truncate",
                            "loss": True,
                        })
                        stats["uniqueBlocks"] = len(unique)
                        return
                if limit_reached:
                    break
            if limit_reached:
                break
        if limit_reached:
            break

    stats["uniqueBlocks"] = len(unique)


def stream_world_to_gmod_save(
    args: argparse.Namespace,
    save_path: pathlib.Path,
    slot_name: str,
    map_name: str,
    saved_at: int | None,
    block_size: float,
    max_part_json_bytes: int = DEFAULT_SAVE_PART_JSON_BYTES,
    save_format: str = "chunked",
) -> tuple[dict[str, Any], list[pathlib.Path]]:
    registry = load_block_state_registry()
    stats: dict[str, Any] = {
        "blocks": 0,
        "chunks": 0,
        "scannedMcChunks": 0,
        "missingMcChunks": 0,
        "uniqueBlocks": 0,
        "clippedByMaxBlocks": False,
        "opaqueBlockEntities": 0,
    }
    palette: list[str] = []
    palette_index: dict[str, int] = {}
    save_fields = {
        "format": "mc_world_import",
        "formatVersion": 1,
        "v": PHASE5_FORMAT_VERSION,
        "schemaHash": registry.metadata["schemaHash"],
        "slotName": slot_name,
        "map": map_name,
        "savedAt": int(saved_at if saved_at is not None else time.time()),
        "bs": block_size,
        "h": CH,
        "source": {
            "format": "minecraft_java_anvil",
            "storageAdapter": chunk_storage_adapter_for_data_version(registry.world_version).adapter_id,
            "minecraftVersion": registry.metadata["minecraftId"],
            "targetMinecraftVersion": registry.metadata["minecraftId"],
            "dataVersion": registry.world_version,
            "targetDataVersion": registry.world_version,
            "compatibility": "exact",
            "series": registry.metadata["seriesId"],
            "observedDataVersions": [registry.world_version],
            "world": str(pathlib.Path(args.world)),
            "dimension": args.dimension,
            "from": list(args.from_pos),
            "to": list(args.to_pos),
            "origin": list(args.origin),
            "keepCoords": bool(args.keep_coords),
            "maxBlocks": int(args.max_blocks or 0),
            "streamed": True,
        },
        "target": {
            "schemaHash": registry.metadata["schemaHash"],
            "blockReportSha256": registry.metadata["reportSha256"],
            "blockSize": block_size,
            "chunkSize": CS,
            "chunkHeight": CH,
            "sectionHeight": 16,
        },
    }
    chunks = iter_gmod_save_chunks_from_world(args, stats, palette, palette_index, registry)
    if save_format == "streaming":
        written = write_gmod_save_streaming(save_path, save_fields, chunks, palette, stats, max_part_json_bytes)
    else:
        written = write_gmod_save_chunked(save_path, save_fields, chunks, palette, stats, max_part_json_bytes)
    return stats, written


def write_output(out_path: pathlib.Path, data: dict[str, Any], split_chunks: bool) -> list[pathlib.Path]:
    if not split_chunks:
        out_path.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        return [out_path]

    chunks = data.get("chunks")
    if not isinstance(chunks, list):
        out_path.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        return [out_path]

    stem = out_path.stem
    part_dir = out_path.with_name(stem + "_parts")
    part_dir.mkdir(parents=True, exist_ok=True)

    part_names: list[str] = []
    written: list[pathlib.Path] = []
    for i, chunk in enumerate(chunks, 1):
        if not isinstance(chunk, list) or len(chunk) < 4:
            continue
        cx, cy, cz, cells = chunk[0], chunk[1], chunk[2], chunk[3]
        count = cell_count(cells)
        part = {
            "v": data.get("v", 5),
            "h": data.get("h", CH),
            "source": data.get("source"),
            "p": data.get("p"),
            "stats": {"blocks": count, "chunks": 1, "part": i, "clippedByMaxBlocks": False},
            "chunks": [chunk],
        }
        copy_bundle_metadata(part, data)
        part_name = f"{stem}_parts/{stem}_{i:03d}_{cx}_{cy}_{cz}.json"
        part_path = out_path.parent / part_name
        part_bytes = json.dumps(part, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        part_path.write_bytes(part_bytes)
        if int(data.get("v") or 0) >= PHASE5_FORMAT_VERSION:
            part_names.append({
                "path": part_name,
                "sha256": hashlib.sha256(part_bytes).hexdigest(),
                "chunks": 1,
                "blocks": count,
            })
        else:
            part_names.append(part_name)
        written.append(part_path)

    manifest = dict(data)
    manifest.pop("chunks", None)
    manifest["v"] = data.get("v", 6) if int(data.get("v") or 0) >= PHASE5_FORMAT_VERSION else 6
    manifest["parts"] = part_names
    manifest["stats"] = dict(data.get("stats") or {})
    manifest["stats"]["parts"] = len(part_names)
    out_path.write_text(json.dumps(manifest, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    return [out_path] + written


def default_out_path(world: str) -> pathlib.Path:
    script = pathlib.Path(__file__).resolve()
    garrysmod = script.parents[3]
    name = pathlib.Path(world).name or "world"
    return garrysmod / "data" / "mc_import" / f"{name}.json"


def default_save_path(slot_name: str) -> pathlib.Path:
    script = pathlib.Path(__file__).resolve()
    garrysmod = script.parents[3]
    return garrysmod / "data" / "mc" / "saves" / f"{slot_name}.dat"


def default_gmod_map() -> str:
    script = pathlib.Path(__file__).resolve()
    save_dir = script.parents[3] / "data" / "mc"
    maps = sorted(p.stem for p in save_dir.glob("*.dat") if p.is_file())
    return maps[0] if maps else "gm_flatgrass"


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Convert a Minecraft Java Anvil world area into GMod MC import data")
    p.add_argument("--world", required=True, help="Minecraft Java world directory, for example C:\\Users\\Name\\.minecraft\\saves\\World")
    p.add_argument("--dimension", default="overworld", help="Dimension: overworld, the_nether, or the_end (default: overworld)")
    p.add_argument("--out", help="Output import JSON; defaults to garrysmod/data/mc_import/<world>.json")
    p.add_argument("--from", dest="from_pos", nargs=3, type=int, metavar=("X", "Y", "Z"), help="Inclusive first Minecraft block coordinate")
    p.add_argument("--to", dest="to_pos", nargs=3, type=int, metavar=("X", "Y", "Z"), help="Inclusive last Minecraft block coordinate")
    p.add_argument("--from-chunk", nargs=2, type=int, metavar=("CX", "CZ"), help="Inclusive first Minecraft chunk coordinate")
    p.add_argument("--to-chunk", nargs=2, type=int, metavar=("CX", "CZ"), help="Inclusive last Minecraft chunk coordinate")
    p.add_argument("--y-range", nargs=2, type=int, default=(-64, 319), metavar=("MINY", "MAXY"), help="Minecraft Y range for chunk selection (default: -64 319)")
    p.add_argument("--origin", nargs=3, type=int, default=(0, 0, DEFAULT_ORIGIN_Z), metavar=("BX", "BY", "BZ"), help="Destination GMod block origin (default: 0 0 0)")
    p.add_argument("--keep-coords", action="store_true", help="Retain Minecraft coordinates and apply origin as an offset")
    p.add_argument("--max-blocks", type=int, default=DEFAULT_MAX_BLOCKS, help="Maximum exported non-air blocks; 0 means unlimited")
    p.add_argument("--allow-data-version", action="append", type=int, default=[], help="Explicitly allow a schema-validated source DataVersion; may be repeated")
    p.add_argument("--split-chunks", action=argparse.BooleanOptionalAction, default=True, help="write one small JSON per GMod chunk plus a manifest")
    p.add_argument("--save-name", help="Also create data/mc/saves/<name>.dat for the save manager")
    p.add_argument("--save-out", help="GMod save .dat output path; matching JSON metadata is also written")
    p.add_argument("--save-map", help="GMod map stored in save metadata; defaults to an existing map or gm_flatgrass")
    p.add_argument("--block-size", type=float, default=BS, help="Saved MC.BS block size (default: 36.5)")
    p.add_argument("--saved-at", type=int, help="Saved Unix timestamp (default: current time)")
    p.add_argument("--save-format", choices=("auto", "classic", "chunked", "streaming"), default="auto", help="GMod save format: auto/classic/chunked/streaming")
    p.add_argument("--save-part-json-bytes", type=int, default=DEFAULT_SAVE_PART_JSON_BYTES, help="Target uncompressed JSON bytes per save part (default: 4 MiB)")
    args = p.parse_args(argv)

    if args.from_chunk or args.to_chunk:
        if not args.from_chunk or not args.to_chunk:
            p.error("--from-chunk and --to-chunk must be used together")
        lo, hi = chunk_box(args.from_chunk, args.to_chunk, args.y_range)
        args.from_pos = lo
        args.to_pos = hi
    elif not args.from_pos or not args.to_pos:
        p.error("use either --from/--to or --from-chunk/--to-chunk")

    if args.save_name:
        args.save_name = gmod_save_slot_name(args.save_name)
        if not args.save_name:
            p.error("--save-name is empty after sanitization")
    if args.save_out and not args.save_name:
        args.save_name = gmod_save_slot_name(pathlib.Path(args.save_out).stem)
        if not args.save_name:
            p.error("the --save-out filename cannot be converted into a valid save name")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    written: list[pathlib.Path] = []
    save_written: list[pathlib.Path] = []

    write_json = bool(args.out) or not (args.save_name or args.save_out)
    out_path = pathlib.Path(args.out) if args.out else default_out_path(args.world)
    data: dict[str, Any] | None = None
    stats: dict[str, Any]

    if args.save_name or args.save_out:
        slot_name = args.save_name or gmod_save_slot_name(pathlib.Path(args.save_out).stem)
        if not slot_name:
            raise ValueError("missing valid save slot name")
        save_path = pathlib.Path(args.save_out) if args.save_out else default_save_path(slot_name)
        map_name = args.save_map or default_gmod_map()
        save_format = choose_save_format(args, write_json)
        if not write_json and can_stream_gmod_save(args) and save_format in ("chunked", "streaming"):
            stats, save_written = stream_world_to_gmod_save(
                args,
                save_path,
                slot_name,
                map_name,
                args.saved_at,
                args.block_size,
                args.save_part_json_bytes,
                save_format,
            )
        else:
            data = import_world(args)
            save_data = make_gmod_save_data(
                data,
                slot_name=slot_name,
                map_name=map_name,
                saved_at=args.saved_at,
                block_size=args.block_size,
            )
            if save_format == "streaming":
                save_written = write_gmod_save_streaming(
                    save_path,
                    save_data,
                    save_data.get("chunks") or [],
                    save_data.get("p") or [],
                    data["stats"],
                    args.save_part_json_bytes,
                )
            elif save_format == "chunked":
                save_written = write_gmod_save_chunked(
                    save_path,
                    save_data,
                    save_data.get("chunks") or [],
                    save_data.get("p") or [],
                    data["stats"],
                    args.save_part_json_bytes,
                )
            else:
                save_written = write_gmod_save_files(save_path, save_data)
            stats = data["stats"]
    else:
        data = import_world(args)
        stats = data["stats"]

    if write_json:
        if data is None:
            data = import_world(args)
            stats = data["stats"]
        out_path.parent.mkdir(parents=True, exist_ok=True)
        written = write_output(out_path, data, args.split_chunks)

    if written:
        print(f"[MC import] JSON 输出: {out_path}")
    print(f"[MC import] 方块: {stats['blocks']}  GMod chunks: {stats['chunks']}  MC chunks: {stats['scannedMcChunks']}")
    print(f"[MC import] 唯一方块: {stats['uniqueBlocks']}  clipped={stats['clippedByMaxBlocks']}")
    if len(written) > 1:
        print(f"[MC import] parts: {len(written) - 1}")
    if save_written:
        print(f"[MC import] GMod save: {save_written[0]}")
        print(f"[MC import] GMod save meta: {save_written[1]}")
        print("[MC import] 进服务器后执行: mc_save_manager 或 mc_load_save " + str(args.save_name))
    elif written:
        print("[MC import] 进服务器后执行: mc_import " + out_path.stem)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
