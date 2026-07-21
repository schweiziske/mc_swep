# Minecraft Java World Importer

`mc_import_world.py` converts a selected area from a Minecraft Java Edition
Anvil world (`.mca` region files) into data that the MC SWEP addon can load.

The importer can either:

- create an import bundle for the `mc_import` console command; or
- create a named Garry's Mod save that can be opened with `mc_load_save` or the
  save manager.

## Requirements

- Python 3.10 or newer
- A Minecraft Java Edition world directory
- No third-party Python packages are required

The generated block-state report required by the importer is included in
`tools/data`.

## Supported Minecraft versions

The audited chunk-storage adapter supports Java Edition `DataVersion` 2860
through 4897. This corresponds to the modern root-level `sections` and
`block_states` format introduced in Minecraft 1.18, up to the addon's current
target, Minecraft 26.2 Pre-Release 4 (`DataVersion` 4897).

By default, the importer only accepts the exact target `DataVersion` 4897. To
import another structurally supported version, explicitly allow it:

```powershell
--allow-data-version 4671
```

This option may be repeated. Older block names and complete block-state
properties must still pass validation against the bundled target registry.
Legacy 1.17-and-earlier chunk storage is not supported, and a selected area
may not contain mixed `DataVersion` values.

## Quick start: create a named save

Run this command from the repository root:

```powershell
python tools/mc_import_world.py `
  --world "C:\Users\Name\AppData\Roaming\.minecraft\saves\World" `
  --dimension overworld `
  --from-chunk -4 -4 `
  --to-chunk 4 4 `
  --y-range -64 319 `
  --origin 0 0 0 `
  --save-name imported_world `
  --save-format auto
```

When the repository is installed as a Garry's Mod addon, the default output is:

```text
garrysmod/data/mc/saves/imported_world.dat
garrysmod/data/mc/saves/imported_world.json
garrysmod/data/mc/saves/imported_world_parts/  (for split save formats)
```

Load it in the server console:

```text
mc_load_save imported_world
```

For very large worlds, use `--save-format streaming`.

## Create an `mc_import` bundle

```powershell
python tools/mc_import_world.py `
  --world "C:\Users\Name\AppData\Roaming\.minecraft\saves\World" `
  --dimension overworld `
  --from -64 -64 -64 `
  --to 63 319 63 `
  --origin 0 0 0 `
  --out "C:\gmod\garrysmod\data\mc_import\world.json"
```

Import it in the server console:

```text
mc_import world replace preserve
```

The command syntax is:

```text
mc_import <file> [replace|append] [reject|preserve]
```

- `replace` replaces the current MC block world.
- `append` adds the imported blocks to the current world.
- `reject` rejects unknown imported content.
- `preserve` keeps unknown block-entity NBT as inert opaque data.

You can validate a bundle without importing it:

```text
mc_import_probe world preserve
```

## Coordinate mapping

The selection endpoints are inclusive. Coordinates are mapped as follows:

```text
Minecraft X -> GMod block X (`bx`)
Minecraft Z -> GMod block Y (`by`)
Minecraft Y -> GMod block Z (`bz`)
```

By default, the minimum corner of the selected area is moved to `--origin`.
With `--keep-coords`, the original Minecraft coordinates are retained and
`--origin` is applied as an additional offset.

## Options

```text
--world PATH                  Minecraft Java world directory (required)
--dimension NAME             overworld, the_nether, or the_end
--from X Y Z                 Inclusive first block coordinate
--to X Y Z                   Inclusive last block coordinate
--from-chunk CX CZ           Inclusive first chunk coordinate
--to-chunk CX CZ             Inclusive last chunk coordinate
--y-range MINY MAXY          Y range for chunk selection; default: -64 319
--origin BX BY BZ            GMod destination origin; default: 0 0 0
--keep-coords                Retain source coordinates and add the origin offset
--max-blocks N               Maximum non-air blocks; 0 means unlimited
--allow-data-version N       Explicitly allow a compatible source DataVersion
--out PATH                   Write an mc_import JSON bundle
--split-chunks               Write a manifest and per-GMod-chunk files (default)
--no-split-chunks            Write one JSON file
--save-name NAME             Create data/mc/saves/NAME.dat
--save-out PATH              Set the save .dat output path
--save-map NAME              Map name stored in save metadata
--block-size NUMBER          Saved MC block size; default: 36.5
--saved-at UNIX_TIME         Override the save timestamp
--save-format FORMAT         auto, classic, chunked, or streaming
--save-part-json-bytes N     Target uncompressed part size; default: 4194304
```

Use either `--from` with `--to`, or `--from-chunk` with `--to-chunk`.

## Dimensions

Built-in aliases:

```text
overworld / minecraft:overworld
the_nether / nether / minecraft:the_nether
the_end / end / minecraft:the_end
```

The importer can also look for a custom dimension under
`dimensions/minecraft/<dimension>/region`.

## Save formats

- `auto`: chooses a suitable format from the selected area.
- `classic`: one save file, intended for small imports.
- `chunked`: a manifest plus part files, intended for medium or large imports.
- `streaming`: indexed lazy-loading storage, intended for very large imports.

For efficient chunked or streaming output, select complete Minecraft chunks and
use an origin aligned to the addon's `16 x 16 x 32` GMod chunk dimensions.

## Notes and limitations

- Only Java Edition Anvil region worlds are supported.
- Air is not exported as a placed block.
- Block IDs and complete legal block states are preserved when supported.
- Minecraft entities and gameplay logic are not simulated.
- Block-entity NBT is preserved as inert data and is never executed by the
  importer.
