#include "pch.h"
#include "blockdefs.h"
#include "blob_format.h"
#include <cmath>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <unordered_set>
using namespace GarrysMod::Lua; using namespace mcmesh;
namespace { constexpr char MAGIC[4] = { 'M','C','B','D' }; constexpr uint32_t VERSION = 4, MAX_BYTES = 512u * 1024u * 1024u, MAX_BLOCKS = 65535, MAX_STATES = 1048576, MAX_ORIENTS = 4194240, MAX_BOXES = 1000000, MAX_QUADS = 4000000, MAX_TEMPLATES = 1000000; bool fr(float v, float a, float b) { return std::isfinite(v) && v >= a && v <= b; }struct Header { char magic[4]; uint32_t version, totalBytes; uint16_t cols, rows, tile, pad, stride, w, h, r0; float inset; uint32_t blocks, orients, boxes, quads, templates, states, visualBytes, visualVersion; uint8_t schemaHash[32], visualHash[32]; }; struct VisualHeader { uint32_t coordinateScale, packedQuadBytes, catalogStates, geometries, geometryQuads, surfaceSets, surfaceEntries, models, groups, alternatives, plans, planGroups; }; static_assert(sizeof(Header) == 128); static_assert(sizeof(VisualHeader) == 48); }
namespace mcmesh::blockdefs {
    static bool readBox(BlobReader& r, Box& b) { for (float& v : b.v)if (!r.Read(v) || !fr(v, 0, 1))return false; return b.v[0] <= b.v[3] && b.v[1] <= b.v[4] && b.v[2] <= b.v[5]; }
    static bool readQuad(BlobReader& r, Quad& q, uint32_t bc, uint64_t tiles) { uint16_t res = 0; if (!r.Read(q.face) || !r.Read(q.flags) || !r.Read(q.boxIndex) || !r.Read(q.tile) || !r.Read(res) || q.face < 1 || q.face>6 || (q.flags & ~1) || res || q.tile >= tiles || (q.boxIndex != 0xffff && q.boxIndex >= bc))return false; --q.face; for (int i = 0; i < 4; ++i) { for (int a = 0; a < 3; ++a)if (!r.Read(q.pos[i * 3 + a]) || !fr(q.pos[i * 3 + a], -16, 16))return false; for (int a = 0; a < 2; ++a)if (!r.Read(q.uv[i * 2 + a]) || !fr(q.uv[i * 2 + a], 0, 16))return false; }return true; }
    static bool parseVisual(BlobReader& r, const Header& h, uint64_t tiles, VisualCatalog& out) { if (!h.visualBytes)return true; if (h.visualBytes != r.Remaining())return false; VisualHeader v{}; if (!r.Read(v) || v.coordinateScale != 16384 || v.packedQuadBytes != 32 || v.catalogStates > h.states || v.geometries > 65535 || v.geometryQuads > MAX_QUADS || v.surfaceSets > 65535 || v.surfaceEntries > 4000000 || v.models > 65535 || v.groups > 65535 || v.alternatives > 65535 || v.plans > 65535 || v.planGroups > 65535)return false; uint64_t expected = sizeof(VisualHeader) + uint64_t(v.geometries) * 6 + uint64_t(v.geometryQuads) * 32 + uint64_t(v.surfaceSets) * 6 + uint64_t(v.surfaceEntries) * 2 + uint64_t(v.models) * 4 + uint64_t(v.groups) * 3 + uint64_t(v.alternatives) * 3 + uint64_t(v.plans) * 3 + uint64_t(v.planGroups) * 2 + uint64_t(v.catalogStates) * 2; if (expected > h.visualBytes)return false; try { out.geometries.resize(v.geometries); out.quads.resize(v.geometryQuads); out.surfaceSets.resize(v.surfaceSets); out.surfaces.resize(v.surfaceEntries); out.models.resize(v.models); out.groups.resize(v.groups); out.alternatives.resize(v.alternatives); out.plans.resize(v.plans); out.planGroups.resize(v.planGroups); out.statePlans.resize(v.catalogStates); } catch (...) { return false; }uint32_t qend = 0; for (auto& g : out.geometries) { uint32_t byteOffset = 0; if (!r.Read(byteOffset) || !r.Read(g.quadCount) || byteOffset != uint64_t(qend) * 32u || qend > v.geometryQuads || g.quadCount > v.geometryQuads - qend)return false; g.firstQuad = qend; qend += g.quadCount; }if (qend != v.geometryQuads)return false; for (auto& q : out.quads) { if (!r.ReadBytes(q.bytes.data(), q.bytes.size()))return false; uint8_t face = q.bytes[0] & 7, surface = q.bytes[1], rotation = q.bytes[2]; if (face < 1 || face>6 || !surface || rotation > 3)return false; }uint32_t send = 0; for (auto& s : out.surfaceSets) { if (!r.Read(s.firstEntry) || !r.Read(s.entryCount) || s.firstEntry != send || s.firstEntry > v.surfaceEntries || s.entryCount > v.surfaceEntries - s.firstEntry)return false; send += s.entryCount; }if (send != v.surfaceEntries)return false; for (auto& s : out.surfaces) { if (!r.Read(s) || (s & 0x0fff) >= tiles)return false; }for (auto& m : out.models) { if (!r.Read(m.geometryId) || !r.Read(m.surfaceId) || ((m.geometryId == 0) != (m.surfaceId == 0)) || m.geometryId > v.geometries || m.surfaceId > v.surfaceSets)return false; }uint32_t aend = 0; for (auto& g : out.groups) { if (!r.Read(g.firstAlternative) || !r.Read(g.alternativeCount) || !g.alternativeCount || g.firstAlternative != aend || g.firstAlternative > v.alternatives || g.alternativeCount > v.alternatives - g.firstAlternative)return false; aend += g.alternativeCount; }if (aend != v.alternatives)return false; for (auto& a : out.alternatives) { if (!r.Read(a.modelId) || !r.Read(a.weight) || !a.modelId || a.modelId > v.models || !a.weight)return false; }uint32_t pend = 0; for (auto& p : out.plans) { if (!r.Read(p.firstGroup) || !r.Read(p.groupCount) || p.firstGroup != pend || p.firstGroup > v.planGroups || p.groupCount > v.planGroups - p.firstGroup)return false; pend += p.groupCount; }if (pend != v.planGroups)return false; for (auto& g : out.planGroups)if (!r.Read(g) || !g || g > v.groups)return false; for (auto& p : out.statePlans)if (!r.Read(p) || p > v.plans)return false; uint8_t tintCount = 0; if (!r.Read(tintCount) || tintCount > 31)return false; out.tintSlot.fill(0xff); for (uint8_t slot = 0; slot < tintCount; ++slot) { uint8_t index = 0; if (!r.Read(index) || index > 30 || out.tintSlot[index] != 0xff)return false; out.tintSlot[index] = slot; }uint64_t tintBytes = uint64_t(v.catalogStates) * tintCount; if (tintBytes > r.Remaining())return false; try { out.stateTintCodes.resize(size_t(tintBytes)); } catch (...) { return false; }if (tintBytes && !r.ReadBytes(out.stateTintCodes.data(), size_t(tintBytes)))return false; uint16_t transformCount = 0; if (!r.Read(transformCount) || transformCount > out.models.size())return false; uint16_t previousModel = 0; for (uint32_t i = 0; i < transformCount; ++i) { uint16_t modelId = 0; uint8_t kind = 0, reserved = 0; std::array<uint8_t, 6> rotations{}; if (!r.Read(modelId) || !r.Read(kind) || !r.Read(reserved) || !modelId || modelId > out.models.size() || modelId <= previousModel || kind > 1 || reserved)return false; if (kind == 0) { uint8_t rotation = 0, tail = 0; if (!r.Read(rotation) || !r.Read(tail) || rotation > 3 || tail)return false; rotations.fill(rotation); } else { for (uint8_t& rotation : rotations)if (!r.Read(rotation) || rotation > 3)return false; }out.models[modelId - 1].uvRotationAdd = rotations; previousModel = modelId; }if (r.Remaining() != 0)return false; for (const auto& q : out.quads) { uint8_t tv = q.bytes[0] >> 3; if (tv && out.tintSlot[tv - 1] == 0xff)return false; }out.tintSlotCount = tintCount; out.coordinateScale = v.coordinateScale; out.catalogStateCount = v.catalogStates; out.loaded = true; return true; }
    static bool ParseImpl(const void* data, size_t len) {
        if (!data || len<sizeof(Header) || len>MAX_BYTES)return false; BlobReader r(data, len); Header h{}; if (!r.Read(h) || memcmp(h.magic, MAGIC, 4) || h.version != VERSION || h.totalBytes != len || h.r0 || h.blocks > MAX_BLOCKS || h.states > MAX_STATES || h.orients > MAX_ORIENTS || h.boxes > MAX_BOXES || h.quads > MAX_QUADS || h.templates > MAX_TEMPLATES || (h.visualBytes ? h.visualVersion != 2 || h.visualBytes < sizeof(VisualHeader) : h.visualVersion != 0))return false;
        uint64_t tiles = uint64_t(h.cols) * h.rows;
        if (!h.cols || !h.rows || !h.tile || !h.stride || !h.w || !h.h || !fr(h.inset, 0, float(h.tile) * .4999f) || h.states == 0)return false;
        const uint64_t cellExtent = uint64_t(h.pad) + h.tile;
        if (cellExtent > h.stride)return false;
        const uint64_t atlasX = uint64_t(h.cols - 1) * h.stride + cellExtent;
        const uint64_t atlasY = uint64_t(h.rows - 1) * h.stride + cellExtent;
        if (atlasX > h.w || atlasY > h.h)return false; std::unordered_map<uint16_t, BlockDef>parsed; try { parsed.reserve(h.blocks); }
        catch (...) { return false; }uint32_t go = 0, gb = 0, gq = 0, gt = 0; uint16_t prevId = 0; bool firstBlock = true;
        for (uint32_t bi = 0; bi < h.blocks; ++bi) {
            BlockDef d; uint8_t lk = 0, ck = 0, res8 = 0; uint16_t oc = 0, res16 = 0; uint32_t res32 = 0; if (!r.Read(d.id) || !r.Read(d.flags) || !r.Read(d.defaultOrient) || !r.Read(lk) || !r.Read(ck) || !r.Read(res8) || !r.Read(oc) || !r.Read(res16) || !r.Read(res32) || d.id == 0 || (d.flags & ~FLAG_KNOWN_MASK) || lk > 2 || ck > 5 || res8 || res16 || res32 || !oc || oc > 64 || (!firstBlock && d.id <= prevId))return false; firstBlock = false; prevId = d.id; d.liquid = LiquidKind(lk); d.connection = ConnectionKind(ck); if (go > h.orients - std::min<uint32_t>(oc, h.orients))return false; go += oc; std::unordered_set<uint8_t>okeys; try { d.orients.reserve(oc); }
            catch (...) { return false; }
            for (uint32_t oi = 0; oi < oc; ++oi) {
                OrientDef o; uint8_t ek = 0, ftc = 0, res0 = 0; uint16_t bc = 0, tc = 0, res1 = 0; uint32_t qc = 0; if (!r.Read(o.orient) || !r.Read(ek) || !r.Read(o.pass) || !r.Read(o.flags) || !r.Read(o.connectCaps) || !r.Read(ftc) || !r.Read(res0) || !r.Read(bc) || !r.Read(qc) || !r.Read(tc) || !r.Read(res1) || ek < 1 || ek>6 || o.pass > 1 || (o.flags & ~1) || (o.connectCaps & ~CAP_KNOWN_MASK) || res0 || res1 || !okeys.insert(o.orient).second)return false; if (oi == 0 && o.orient != d.defaultOrient)return false; o.emitter = EmitterKind(ek); uint8_t expected = (ek == 1 || ek == 6) ? 6 : (ek == 2 ? 1 : 0); if (ftc != expected)return false; if ((ek == 6) != (lk != 0) || (ek == 5) != (ck != 0))return false; if (gb > h.boxes - std::min<uint32_t>(bc, h.boxes) || gq > h.quads - std::min<uint32_t>(qc, h.quads) || gt > h.templates - std::min<uint32_t>(tc, h.templates))return false; gb += bc; gq += qc; gt += tc;
                const uint64_t directBytes = uint64_t(ftc) * 36u + uint64_t(bc) * 24u + uint64_t(qc) * 88u + uint64_t(tc) * 8u;
                if (directBytes > r.Remaining())return false;
                try { o.faces.resize(ftc); o.boxes.resize(bc); o.quads.resize(qc); o.templates.reserve(tc); }
                catch (...) { return false; }for (auto& f : o.faces) { uint8_t fl = 0, rr = 0; if (!r.Read(f.tile) || !r.Read(fl) || !r.Read(rr) || fl || rr || f.tile >= tiles)return false; for (float& v : f.uv)if (!r.Read(v) || !fr(v, 0, 16))return false; }for (auto& b : o.boxes)if (!readBox(r, b))return false; for (auto& q : o.quads)if (!readQuad(r, q, bc, tiles))return false; std::unordered_set<uint8_t>masks; for (uint32_t ti = 0; ti < tc; ++ti) { TemplateDef t; uint8_t rr = 0; uint16_t tbc = 0; uint32_t tqc = 0; if (!r.Read(t.mask) || !r.Read(rr) || !r.Read(tbc) || !r.Read(tqc) || rr || !masks.insert(t.mask).second)return false; uint8_t maxm = ck == 5 ? 63 : 15; if (ek != 5 || t.mask > maxm)return false; if (gb > h.boxes - std::min<uint32_t>(tbc, h.boxes) || gq > h.quads - std::min<uint32_t>(tqc, h.quads))return false; gb += tbc; gq += tqc; const uint64_t templateBytes = uint64_t(tbc) * 24u + uint64_t(tqc) * 88u; if (templateBytes > r.Remaining())return false; try { t.boxes.resize(tbc); t.quads.resize(tqc); } catch (...) { return false; }for (auto& b : t.boxes)if (!readBox(r, b))return false; for (auto& q : t.quads)if (!readQuad(r, q, tbc, tiles))return false; o.templates.push_back(std::move(t)); }if (ek == 5 && tc != (ck == 5 ? 64 : 16))return false; d.orients.push_back(std::move(o));
            }parsed.emplace(d.id, std::move(d));
        }
        std::vector<StateDef> states; try { states.resize(h.states); }
        catch (...) { return false; }for (uint32_t si = 0; si < h.states; ++si) { uint32_t stateId = 0; StateDef s; uint8_t reserved = 0; if (!r.Read(stateId) || !r.Read(s.blockId) || !r.Read(s.orient) || !r.Read(s.flags) || !r.Read(reserved) || stateId != si || (s.flags & ~15) || reserved)return false; for (auto& plan : s.stairPlans)if (!r.Read(plan))return false; if (si == 0) { if (s.blockId || s.orient || s.flags || std::any_of(s.stairPlans.begin(), s.stairPlans.end(), [](uint16_t p) {return p != 0; }))return false; } else { auto di = parsed.find(s.blockId); if (di == parsed.end() || !OrientEntry(&di->second, s.orient))return false; if ((s.flags & 2) == 0 && std::any_of(s.stairPlans.begin(), s.stairPlans.end(), [](uint16_t p) {return p != 0; }))return false; if ((s.flags & 2) && std::any_of(s.stairPlans.begin(), s.stairPlans.end(), [](uint16_t p) {return p == 0; }))return false; }states[si] = s; }VisualCatalog visualCatalog; if (!parseVisual(r, h, tiles, visualCatalog))return false; for (const auto& s : states)for (uint16_t plan : s.stairPlans)if (plan > visualCatalog.plans.size())return false;
        if (!r.AtEnd() || go != h.orients || gb != h.boxes || gq != h.quads || gt != h.templates)return false; AtlasParams a{ h.cols,h.rows,h.tile,h.pad,h.stride,h.w,h.h,h.inset,true }; AggregateCounts c{ h.blocks,h.orients,h.boxes,h.quads,h.templates,h.states }; std::array<uint8_t, 32> schemaValue{}, visualValue{}; std::copy(std::begin(h.schemaHash), std::end(h.schemaHash), schemaValue.begin()); std::copy(std::begin(h.visualHash), std::end(h.visualHash), visualValue.begin()); std::unique_lock<std::shared_mutex>l(g_mutex); if (g_sealed)return false; g_defs = std::move(parsed); g_states = std::move(states); g_visual = std::move(visualCatalog); g_atlas = a; g_counts = c; g_schemaHash = schemaValue; g_visualHash = visualValue; g_loaded = true; return true;
    }
    bool Parse(const void* data, size_t len) { try { return ParseImpl(data, len); } catch (...) { return false; } }
    bool Seal() { std::unique_lock<std::shared_mutex>l(g_mutex); if (!g_loaded)return false; g_sealed = true; return true; }void Unseal() { std::unique_lock<std::shared_mutex>l(g_mutex); g_sealed = false; }void Clear() { std::unique_lock<std::shared_mutex>l(g_mutex); g_defs.clear(); g_states.clear(); g_visual = {}; g_atlas = {}; g_counts = {}; g_schemaHash = {}; g_visualHash = {}; g_loaded = g_sealed = false; }int SetBlockDefs(ILuaBase* L) { unsigned int n = 0; const char* p = L->GetString(1, &n); L->PushBool(p && Parse(p, n)); return 1; }int DebugStateDef(ILuaBase* L) { if (!L->IsType(1, Type::Number))return 0; double x = L->GetNumber(1); if (!std::isfinite(x) || x != std::floor(x) || x < 0 || x>1048575)return 0; auto lock = LockForRead(); const StateDef* s = FindState(uint32_t(x)); if (!s)return 0; L->CreateTable(); L->PushNumber(x); L->SetField(-2, "stateId"); L->PushNumber(s->blockId); L->SetField(-2, "blockId"); L->PushNumber(s->orient); L->SetField(-2, "orient"); L->PushBool((s->flags & 1) != 0); L->SetField(-2, "legacyExact"); L->PushBool((s->flags & 4) != 0); L->SetField(-2, "fullCube"); L->PushBool((s->flags & 8) != 0); L->SetField(-2, "cullGeneratedFaces"); const BlockDef* d = FindDef(s->blockId); const OrientDef* o = OrientEntry(d, s->orient); if (o) { L->PushNumber(uint8_t(o->emitter)); L->SetField(-2, "emitter"); L->PushNumber(o->pass); L->SetField(-2, "pass"); L->PushBool((o->flags & 1) != 0); L->SetField(-2, "fullCube"); }return 1; }
    int DebugBlockDef(ILuaBase* L) {
        if (!L->IsType(1, Type::Number)) return 0;
        const double x = L->GetNumber(1);
        if (!std::isfinite(x) || x != std::floor(x) || x < 0 || x>65535) return 0;
        auto lock = LockForRead(); const BlockDef* d = FindDef(uint16_t(x)); if (!d)return 0;
        L->CreateTable();
        L->PushNumber(d->flags); L->SetField(-2, "flags");
        L->PushNumber(d->defaultOrient); L->SetField(-2, "defaultOrient");
        L->PushNumber(uint8_t(d->liquid)); L->SetField(-2, "liquidKind");
        L->PushNumber(uint8_t(d->connection)); L->SetField(-2, "connectionKind");
        L->PushBool(IsTransparent(d)); L->SetField(-2, "transparent");
        L->PushBool(d->liquid != LiquidKind::None); L->SetField(-2, "liquid");
        L->PushBool(true); L->SetField(-2, "buildable");
        L->PushNumber(d->orients.size()); L->SetField(-2, "orientCount");
        bool fullAll = true; for (const auto& o : d->orients)if (!(o.flags & 1)) { fullAll = false; break; }
        L->PushBool(fullAll); L->SetField(-2, "fullCubeAll");
        if (!d->orients.empty()) {
            const auto& o = d->orients.front();
            L->PushNumber(uint8_t(o.emitter)); L->SetField(-2, "emitter");
            L->PushNumber(o.pass); L->SetField(-2, "pass");
            L->PushBool(o.pass != 0); L->SetField(-2, "translucentPass");
            L->PushNumber(o.connectCaps); L->SetField(-2, "connectionCaps");
        }
        L->CreateTable(); // orients
        for (const auto& o : d->orients) {
            L->PushNumber(o.orient);
            L->CreateTable();
            L->PushNumber(uint8_t(o.emitter)); L->SetField(-2, "emitter");
            L->PushNumber(uint8_t(o.emitter)); L->SetField(-2, "emitterKind");
            L->PushNumber(o.pass); L->SetField(-2, "pass");
            L->PushBool(o.pass != 0); L->SetField(-2, "translucentPass");
            L->PushBool((o.flags & 1) != 0); L->SetField(-2, "fullCube");
            L->PushNumber(o.connectCaps); L->SetField(-2, "connectionCaps");
            L->PushNumber(uint8_t(d->liquid)); L->SetField(-2, "liquidKind");
            L->PushNumber(uint8_t(d->connection)); L->SetField(-2, "connectionKind");
            L->PushNumber(o.faces.size()); L->SetField(-2, "faceTexCount");
            L->PushNumber(o.boxes.size()); L->SetField(-2, "boxCount");
            L->PushNumber(o.quads.size()); L->SetField(-2, "quadCount");
            L->PushNumber(o.templates.size()); L->SetField(-2, "templateCount");
            L->CreateTable();
            for (size_t fi = 0; fi < o.faces.size(); ++fi) {
                L->PushNumber(double(fi + 1)); L->CreateTable();
                L->PushNumber(o.faces[fi].tile); L->SetField(-2, "tile");
                L->CreateTable();
                for (int k = 0; k < 4; ++k) { L->PushNumber(k + 1); L->CreateTable(); L->PushNumber(o.faces[fi].uv[k * 2]); L->SetField(-2, "u"); L->PushNumber(o.faces[fi].uv[k * 2 + 1]); L->SetField(-2, "v"); L->SetTable(-3); }
                L->SetField(-2, "uvs"); L->SetTable(-3);
            }
            L->SetField(-2, "faces");
            L->CreateTable();
            for (size_t ti = 0; ti < o.templates.size(); ++ti) { const auto& t = o.templates[ti]; L->PushNumber(t.mask); L->CreateTable(); L->PushNumber(t.boxes.size()); L->SetField(-2, "boxCount"); L->PushNumber(t.quads.size()); L->SetField(-2, "quadCount"); L->SetTable(-3); }
            L->SetField(-2, "templates");
            L->SetTable(-3);
        }
        L->SetField(-2, "orients");
        return 1;
    }
}
