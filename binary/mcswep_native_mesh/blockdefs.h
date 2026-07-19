#pragma once
#include "pch.h"
#include "global.h"
#include <array>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
namespace mcmesh::blockdefs {
	enum class EmitterKind :uint8_t { FullCube = 1, Cross = 2, Model = 3, Shape = 4, Connection = 5, Liquid = 6 };
	enum class LiquidKind :uint8_t { None = 0, Water = 1, Lava = 2 };
	enum class ConnectionKind :uint8_t { None = 0, Fence = 1, Pane = 2, Bars = 3, Wall = 4, Pipe6 = 5 };
	constexpr uint16_t FLAG_TRANSPARENT = 1u, FLAG_SOLID = 2u, FLAG_KNOWN_MASK = 3u;
	constexpr uint8_t CAP_FENCE = 1u, CAP_PANE = 2u, CAP_WALL = 4u, CAP_PIPE6 = 8u, CAP_END_STONE_DOWN = 16u, CAP_KNOWN_MASK = 31u;
	struct AtlasParams { uint16_t cols = 0, rows = 0, tile = 0, pad = 0, stride = 0, w = 0, h = 0; float inset = 0; bool valid = false; };
	struct FaceTexture { uint16_t tile = 0; std::array<float, 8>uv{}; };
	struct Box { float v[6]{}; };
	struct Quad { uint8_t face = 0, flags = 0; uint16_t boxIndex = 0xffff, tile = 0; std::array<float, 12>pos{}; std::array<float, 8>uv{}; };
	struct TemplateDef { uint8_t mask = 0; std::vector<Box>boxes; std::vector<Quad>quads; };
	struct OrientDef { uint8_t orient = 0, pass = 0, flags = 0, connectCaps = 0; EmitterKind emitter = EmitterKind::FullCube; std::vector<FaceTexture>faces; std::vector<Box>boxes; std::vector<Quad>quads; std::vector<TemplateDef>templates; };
	struct BlockDef { uint16_t id = 0, flags = 0; uint8_t defaultOrient = 0; LiquidKind liquid = LiquidKind::None; ConnectionKind connection = ConnectionKind::None; std::vector<OrientDef>orients; };
	struct StateDef { uint16_t blockId = 0; uint8_t orient = 0, flags = 0; std::array<uint16_t, 5>stairPlans{}; };
	struct VisualGeometry { uint32_t firstQuad = 0; uint16_t quadCount = 0; };
	struct VisualSurfaceSet { uint32_t firstEntry = 0; uint16_t entryCount = 0; };
	struct VisualModel { uint16_t geometryId = 0, surfaceId = 0; uint8_t uvRotationAdd = 0; };
	struct VisualGroup { uint16_t firstAlternative = 0; uint8_t alternativeCount = 0; };
	struct VisualAlternative { uint16_t modelId = 0; uint8_t weight = 0; };
	struct VisualPlan { uint16_t firstGroup = 0; uint8_t groupCount = 0; };
	struct PackedVisualQuad { std::array<uint8_t, 32>bytes{}; };
	struct VisualCatalog { uint32_t coordinateScale = 0, catalogStateCount = 0; std::vector<VisualGeometry>geometries; std::vector<PackedVisualQuad>quads; std::vector<VisualSurfaceSet>surfaceSets; std::vector<uint16_t>surfaces; std::vector<VisualModel>models; std::vector<VisualGroup>groups; std::vector<VisualAlternative>alternatives; std::vector<VisualPlan>plans; std::vector<uint16_t>planGroups, statePlans; std::array<uint8_t, 31>tintSlot{}; uint8_t tintSlotCount = 0; std::vector<uint8_t>stateTintCodes; bool loaded = false; };
	struct AggregateCounts { uint32_t blocks = 0, orients = 0, boxes = 0, quads = 0, templates = 0, states = 0; };
	inline std::unordered_map<uint16_t, BlockDef>g_defs; inline std::vector<StateDef>g_states; inline VisualCatalog g_visual{}; inline AtlasParams g_atlas{}; inline AggregateCounts g_counts{}; inline std::array<uint8_t, 32>g_schemaHash{}, g_visualHash{}; inline bool g_loaded = false, g_sealed = false; inline std::shared_mutex g_mutex;
	using ReadLock = std::shared_lock<std::shared_mutex>; inline ReadLock LockForRead() { return ReadLock(g_mutex); }inline const BlockDef* FindDef(uint16_t id) { auto i = g_defs.find(id); return i == g_defs.end() ? nullptr : &i->second; }inline const StateDef* FindState(uint32_t id) { return id < g_states.size() ? &g_states[id] : nullptr; }inline const BlockDef* FindStateBlock(uint32_t id) { auto* s = FindState(id); return s ? FindDef(s->blockId) : nullptr; }inline const OrientDef* OrientEntry(const BlockDef* d, uint8_t o) { if (!d || d->orients.empty())return nullptr; for (auto& v : d->orients)if (v.orient == o)return&v; return&d->orients.front(); }inline const OrientDef* StateOrient(uint32_t id) { auto* s = FindState(id); return s ? OrientEntry(FindDef(s->blockId), s->orient) : nullptr; }inline const TemplateDef* TemplateForMask(const OrientDef* o, uint8_t m) { if (!o)return nullptr; for (auto& t : o->templates)if (t.mask == m)return&t; return nullptr; }inline bool IsTransparent(const BlockDef* d) { return d && (d->flags & FLAG_TRANSPARENT); }inline bool IsSolid(const BlockDef* d) { return d && (d->flags & FLAG_SOLID); }inline bool OrientFullCube(const BlockDef* d, uint8_t o) { auto* v = OrientEntry(d, o); return v && (v->flags & 1); }inline bool StateFullCube(uint32_t id) { auto* s = FindState(id); return s && OrientFullCube(FindDef(s->blockId), s->orient); }
	bool Parse(const void*, size_t); bool Seal(); void Unseal(); void Clear(); int SetBlockDefs(ILuaBase*); int DebugBlockDef(ILuaBase*); int DebugStateDef(ILuaBase*);
}
