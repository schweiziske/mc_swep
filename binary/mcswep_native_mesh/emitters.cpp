#include "pch.h"
#include "emitters.h"
#include "blockdefs.h"
#include "blob_format.h"
#include "meshworker.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bd = mcmesh::blockdefs; namespace mb = mcmesh::meshbuild;
namespace {
    constexpr float EPS = 1e-4f, FLUID_H = 0.8888889f, FLUID_OFF = .001f;
    constexpr int TRI[6] = { 0,2,1,0,3,2 };
    const float N[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
    const int OFF[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
    const float CUBE[6][4][3] = {
     {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},{{0,1,0},{1,1,0},{1,0,0},{0,0,0}},
     {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},{{0,1,0},{0,0,0},{0,0,1},{0,1,1}},
     {{1,1,0},{0,1,0},{0,1,1},{1,1,1}},{{0,0,0},{1,0,0},{1,0,1},{0,0,1}} };
    const float CUBE_UV[6][4][2] = {
     {{0,16},{16,16},{16,0},{0,0}},{{0,16},{16,16},{16,0},{0,0}},
     {{0,16},{16,16},{16,0},{0,0}},{{0,16},{16,16},{16,0},{0,0}},
     {{0,16},{16,16},{16,0},{0,0}},{{0,16},{16,16},{16,0},{0,0}} };

    struct Context {
        const mb::SectionSnapshot& s; mb::SectionBuild& o; int side, halo, ss; double BS;
        const mb::SnapshotCell& at(int x, int y, int z)const { return s.cells[size_t(x) + size_t(y) * side + size_t(z) * side * side]; }
        bool ensure(mb::PassVerts& p) {
            const size_t totalVertices = o.opaque.vertices + o.translucent.vertices;
            if (totalVertices > mcmesh::meshworker::kMaxSectionResultBytes / sizeof(mb::Vert) - 6)return false;
            if (p.batches.empty() || p.batches.back().verts.size() > mcmesh::MAX_VERTEX_COUNT - 6) {
                if (p.batches.size() >= 64)return false; p.batches.emplace_back(); p.batches.back().verts.reserve(std::min<uint32_t>(mcmesh::MAX_VERTEX_COUNT, 12288));
            }
            return true;
        }
        void atlas(uint16_t tile, float u16, float v16, float& u, float& v)const {
            const auto& A = bd::g_atlas; float px = float(tile % A.cols) * A.stride + A.pad + u16 / 16 * A.tile;
            float py = float(tile / A.cols) * A.stride + A.pad + v16 / 16 * A.tile; if (u16 <= 0)px += A.inset; else if (u16 >= 16)px -= A.inset; if (v16 <= 0)py += A.inset; else if (v16 >= 16)py -= A.inset; u = px / A.w; v = py / A.h;
        }
        bool quad(mb::PassVerts& p, int face, const float pos[4][3], const float uv[4][2], uint16_t tile, int lx, int ly, int lz, uint8_t tintCode = 0) {
            if (!ensure(p))return false; auto& b = p.batches.back(); for (int i : TRI) { mb::Vert v{}; for (int a = 0; a < 3; ++a) { v.pos[a] = float((a == 0 ? lx : a == 1 ? ly : lz) + pos[i][a]) * float(BS); v.normal[a] = N[face][a]; }atlas(tile, uv[i][0], uv[i][1], v.u, v.v); v.rgba[0] = 255; v.rgba[1] = uint8_t(face + 1); v.rgba[2] = tintCode; v.rgba[3] = 255; b.verts.push_back(v); }++b.faces; ++p.faces; p.vertices += 6; return true;
        }
    };
    bool visible(const bd::BlockDef* cur, const mb::SnapshotCell& nb) { if (!nb.stateId)return true; auto* s = bd::FindState(nb.stateId); auto* d = s ? bd::FindDef(s->blockId) : nullptr; if (!d)return true; if (cur->liquid != bd::LiquidKind::None && d->liquid == cur->liquid)return false; if (bd::IsTransparent(d))return nb.stateId != 0 && nb.id != cur->id; return !s || !(s->flags & 4); }
    void rotateUV(float& u, float& v, int r, bool fu, bool fv) { for (int i = 0; i < (r & 3); ++i) { float nu = v, nv = 16 - u; u = nu; v = nv; }if (fu)u = 16 - u; if (fv)v = 16 - v; }
    void rotateFlags(bool& u, bool& v, int r) { for (int i = 0; i < (r & 3); ++i) { bool nu = v, nv = !u; u = nu; v = nv; } }
    void boxCorners(const bd::Box& b, int f, float p[4][3]) { float x0 = b.v[0], y0 = b.v[1], z0 = b.v[2], x1 = b.v[3], y1 = b.v[4], z1 = b.v[5]; const float t[6][4][3] = { {{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}},{{x0,y1,z0},{x1,y1,z0},{x1,y0,z0},{x0,y0,z0}},{{x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1}},{{x0,y1,z0},{x0,y0,z0},{x0,y0,z1},{x0,y1,z1}},{{x1,y1,z0},{x0,y1,z0},{x0,y1,z1},{x1,y1,z1}},{{x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1}} }; std::copy(&t[f][0][0], &t[f][0][0] + 12, &p[0][0]); }
    void rect(const bd::Box& b, int f, float& r0, float& r1, float& s0, float& s1) { if (f < 2) { r0 = b.v[0]; r1 = b.v[3]; s0 = b.v[1]; s1 = b.v[4]; } else if (f < 4) { r0 = b.v[1]; r1 = b.v[4]; s0 = b.v[2]; s1 = b.v[5]; } else { r0 = b.v[0]; r1 = b.v[3]; s0 = b.v[2]; s1 = b.v[5]; } }
    float plane(const bd::Box& b, int f) { return f == 0 ? b.v[5] : f == 1 ? b.v[2] : f == 2 ? b.v[3] : f == 3 ? b.v[0] : f == 4 ? b.v[4] : b.v[1]; }
    bool coverRect(const bd::Box& t, int f, const std::vector<bd::Box>& boxes, float ox, float oy, float oz) { float a0, a1, b0, b1; rect(t, f, a0, a1, b0, b1); std::vector<float>as{ a0,a1 }, bs{ b0,b1 }; struct R { float a0, a1, b0, b1; }; std::vector<R>rs; float pl = plane(t, f); for (const auto& q : boxes) { if (&q == &t)continue; float qpl = f == 0 ? q.v[2] + oz : f == 1 ? q.v[5] + oz : f == 2 ? q.v[0] + ox : f == 3 ? q.v[3] + ox : f == 4 ? q.v[1] + oy : q.v[4] + oy; if (std::abs(qpl - pl) > EPS)continue; float c0, c1, d0, d1; rect(q, f, c0, c1, d0, d1); if (f < 2) { c0 += ox; c1 += ox; d0 += oy; d1 += oy; } else if (f < 4) { c0 += oy; c1 += oy; d0 += oz; d1 += oz; } else { c0 += ox; c1 += ox; d0 += oz; d1 += oz; }c0 = std::max(c0, a0); c1 = std::min(c1, a1); d0 = std::max(d0, b0); d1 = std::min(d1, b1); if (c1 <= c0 + EPS || d1 <= d0 + EPS)continue; rs.push_back({ c0,c1,d0,d1 }); as.push_back(c0); as.push_back(c1); bs.push_back(d0); bs.push_back(d1); }std::sort(as.begin(), as.end()); std::sort(bs.begin(), bs.end()); for (size_t i = 1; i < as.size(); ++i)for (size_t j = 1; j < bs.size(); ++j) { float a = (as[i - 1] + as[i]) / 2, b = (bs[j - 1] + bs[j]) / 2; bool ok = false; for (auto& r : rs)if (a >= r.a0 - EPS && a <= r.a1 + EPS && b >= r.b0 - EPS && b <= r.b1 + EPS) { ok = true; break; }if (!ok)return false; }return !rs.empty(); }
    const std::vector<bd::Box>* neighborBoxes(Context& c, int x, int y, int z, const bd::BlockDef* d, const bd::OrientDef* o, uint8_t mask) { const auto* t = bd::TemplateForMask(o, mask); return t ? &t->boxes : nullptr; }
    uint8_t connMask(Context& c, int x, int y, int z, const bd::BlockDef* d) {
        static const int delta[6][3] = { {0,-1,0},{0,1,0},{-1,0,0},{1,0,0},{0,0,-1},{0,0,1} };
        uint8_t m = 0; int n = d->connection == bd::ConnectionKind::Pipe6 ? 6 : 4;
        uint16_t cap = d->connection == bd::ConnectionKind::Fence ? bd::CAP_FENCE : (d->connection == bd::ConnectionKind::Wall ? bd::CAP_WALL : (d->connection == bd::ConnectionKind::Pipe6 ? bd::CAP_PIPE6 : bd::CAP_PANE));
        for (int i = 0; i < n; ++i) { auto& nb = c.at(x + delta[i][0], y + delta[i][1], z + delta[i][2]); auto* nd = bd::FindDef(nb.id); auto* no = bd::OrientEntry(nd, nb.orient); uint16_t accepted = cap; if (d->connection == bd::ConnectionKind::Pipe6 && i == 4)accepted |= bd::CAP_END_STONE_DOWN; if (no && (no->connectCaps & accepted))m |= uint8_t(1u << i); }return m;
    }
    float fluidAt(Context& c, int x, int y, int z, bd::LiquidKind k) { auto& a = c.at(x, y, z); auto* d = bd::FindDef(a.id); if (d && d->liquid == k) { auto& up = c.at(x, y, z + 1); auto* ud = bd::FindDef(up.id); return ud && ud->liquid == k ? 1.f : FLUID_H; }if (!a.id)return 0; return(!d || ((d->flags & bd::FLAG_SOLID) && !(d->flags & bd::FLAG_TRANSPARENT) && bd::OrientFullCube(d, a.orient))) ? -1.f : 0.f; }
    float avg(Context& c, bd::LiquidKind k, float hs, float a, float b, int x, int y, int z) { if (a >= 1 || b >= 1)return 1; float total = 0, w = 0; auto add = [&](float h) {if (h >= .8f) { total += h * 10; w += 10; } else if (h >= 0) { total += h; ++w; }}; if (a > 0 || b > 0) { float q = fluidAt(c, x, y, z, k); if (q >= 1)return 1; add(q); }add(hs); add(a); add(b); return w ? total / w : 0; }
    uint16_t u16le(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }int16_t i16le(const uint8_t* p) { return int16_t(u16le(p)); }
    uint16_t selectModel(uint16_t groupId, uint16_t planId, int bx, int by, int bz) { const auto& V = bd::g_visual; const auto& g = V.groups[groupId - 1]; if (g.alternativeCount == 1)return V.alternatives[g.firstAlternative].modelId; uint32_t total = 0; for (uint32_t i = 0; i < g.alternativeCount; ++i)total += V.alternatives[g.firstAlternative + i].weight; uint32_t h = uint32_t(bx) * 73428767u ^ uint32_t(by) * 912931u ^ uint32_t(bz) * 19349663u ^ uint32_t(groupId + uint32_t(planId) * 131u) * 83492791u; uint32_t choice = (h & 0x7fffffffu) % total; for (uint32_t i = 0; i < g.alternativeCount; ++i) { const auto& a = V.alternatives[g.firstAlternative + i]; if (choice < a.weight)return a.modelId; choice -= a.weight; }return V.alternatives[g.firstAlternative].modelId; }
    uint8_t tintCode(uint32_t stateId, uint8_t tintIndex) { const auto& v = bd::g_visual; if (tintIndex == 0xff || tintIndex >= v.tintSlot.size() || stateId >= v.catalogStateCount)return 0; uint8_t slot = v.tintSlot[tintIndex]; if (slot == 0xff || slot >= v.tintSlotCount)return 0; size_t offset = size_t(stateId) * v.tintSlotCount + slot; return offset < v.stateTintCodes.size() ? v.stateTintCodes[offset] : 0; }
    int stairFacing(uint8_t orient) { return orient & 3; }bool stairHalf(const mb::SnapshotCell& c) { return (c.orient & 4) != 0; }bool stairAt(const mb::SnapshotCell& c, int& f, bool& h) { auto* s = bd::FindState(c.stateId); if (!s || !(s->flags & 2))return false; f = stairFacing(c.orient); h = stairHalf(c); return true; }uint16_t stairPlan(Context& c, int x, int y, int z, const mb::SnapshotCell& cell) { auto* s = bd::FindState(cell.stateId); if (!s || !(s->flags & 2))return 0; static const int dx[4] = { 0,0,-1,1 }, dy[4] = { -1,1,0,0 }, opp[4] = { 1,0,3,2 }, ccw[4] = { 2,3,1,0 }; int facing = stairFacing(cell.orient); bool half = stairHalf(cell); auto canTake = [&](int direction) {int nf; bool nh; return !stairAt(c.at(x + dx[direction], y + dy[direction], z), nf, nh) || nf != facing || nh != half; }; int af; bool ah; if (stairAt(c.at(x + dx[facing], y + dy[facing], z), af, ah) && ah == half && (af / 2) != (facing / 2) && canTake(opp[af]))return s->stairPlans[af == ccw[facing] ? 3 : 4]; int behind = opp[facing], bf; bool bh; if (stairAt(c.at(x + dx[behind], y + dy[behind], z), bf, bh) && bh == half && (bf / 2) != (facing / 2) && canTake(bf))return s->stairPlans[bf == ccw[facing] ? 1 : 2]; return s->stairPlans[0]; }
    bool emitGenerated(Context& c, const mb::SnapshotCell& cell, mb::PassVerts& p, int lx, int ly, int lz, uint16_t overridePlan = 0) { const auto& V = bd::g_visual; if (!V.loaded || cell.stateId >= V.statePlans.size())return false; uint16_t planId = overridePlan ? overridePlan : V.statePlans[cell.stateId]; if (!planId)return false; const auto& plan = V.plans[planId - 1]; int bx = c.s.cx * mcmesh::kCS + lx, by = c.s.cy * mcmesh::kCS + ly, bz = c.s.cz * mcmesh::kCH + lz; for (uint32_t gi = 0; gi < plan.groupCount; ++gi) { uint16_t groupId = V.planGroups[plan.firstGroup + gi]; uint16_t modelId = selectModel(groupId, planId, bx, by, bz); const auto& m = V.models[modelId - 1]; if (!m.geometryId)continue; const auto& geo = V.geometries[m.geometryId - 1]; const auto& surfs = V.surfaceSets[m.surfaceId - 1]; for (uint32_t qi = 0; qi < geo.quadCount; ++qi) { const auto& raw = V.quads[geo.firstQuad + qi].bytes; int face = (raw[0] & 7) - 1; uint8_t tint = (raw[0] >> 3); tint = tint ? tint - 1 : 0xff; uint8_t localSurface = raw[1], rot = (raw[2] + m.uvRotationAdd) & 3, flags = raw[3]; if (localSurface > surfs.entryCount)return false; uint16_t surface = V.surfaces[surfs.firstEntry + localSurface - 1], tile = surface & 0x0fff; float inset16 = bd::g_atlas.tile ? 16.f * bd::g_atlas.inset / bd::g_atlas.tile : 0.f; float u0 = raw[4] * .5f + inset16, v0 = raw[5] * .5f + inset16, u1 = raw[6] * .5f - inset16, v1 = raw[7] * .5f - inset16, pos[4][3], uv[4][2]; for (int vi = 0; vi < 4; ++vi) { for (int a = 0; a < 3; ++a)pos[vi][a] = float(i16le(raw.data() + 8 + vi * 6 + a * 2)) / float(V.coordinateScale); bool useU = (flags & (1u << (vi * 2))) != 0, useV = (flags & (1u << (vi * 2 + 1))) != 0; rotateFlags(useU, useV, rot); float u = useU ? u1 : u0, v = useV ? v1 : v0; uv[vi][0] = u; uv[vi][1] = v; }bool boundary = true; for (int vi = 0; vi < 4; ++vi) { float q = face == 0 ? pos[vi][2] - 1 : face == 1 ? pos[vi][2] : face == 2 ? pos[vi][0] - 1 : face == 3 ? pos[vi][0] : face == 4 ? pos[vi][1] - 1 : pos[vi][1]; if (std::abs(q) > EPS) { boundary = false; break; } }bool cullGenerated = !bd::FindState(cell.stateId) || ((bd::FindState(cell.stateId)->flags & 8) != 0); if (boundary && cullGenerated && !visible(bd::FindDef(cell.id), c.at(c.halo + (lx - c.s.lx0) + OFF[face][0], c.halo + (ly - c.s.ly0) + OFF[face][1], c.halo + (lz - c.s.lz0) + OFF[face][2])))continue; if (!c.quad(p, face, pos, uv, tile, lx, ly, lz, tintCode(cell.stateId, tint)))return false; } }return true; }
}
namespace mcmesh::emitters {
    bool Build(const mb::SectionSnapshot& s, mb::SectionBuild& o) {
        Context c{ s,o,s.side,s.halo,s.coreSize,mcmesh::kBS }; for (int z = c.halo; z < c.halo + c.ss; ++z)for (int y = c.halo; y < c.halo + c.ss; ++y)for (int x = c.halo; x < c.halo + c.ss; ++x) {
            auto cell = c.at(x, y, z); auto* d = bd::FindDef(cell.id); if (!d)continue; auto* od = bd::OrientEntry(d, cell.orient); if (!od)return false; int lx = s.lx0 + x - c.halo, ly = s.ly0 + y - c.halo, lz = s.lz0 + z - c.halo; if (d->liquid == bd::LiquidKind::None && d->connection == bd::ConnectionKind::None && bd::g_visual.loaded && cell.stateId < bd::g_visual.statePlans.size() && bd::g_visual.statePlans[cell.stateId]) { auto& p = od->pass ? o.translucent : o.opaque; int before = p.faces; uint16_t derivedPlan = stairPlan(c, x, y, z, cell); if (!emitGenerated(c, cell, p, lx, ly, lz, derivedPlan))return false; ++o.emitters.blocks[2]; o.emitters.faces[2] += p.faces - before; continue; }auto& p = od->pass ? o.translucent : o.opaque; int ef = int(od->emitter) - 1; ++o.emitters.blocks[ef]; int before = p.faces;
            if (od->emitter == bd::EmitterKind::FullCube) { for (int f = 0; f < 6; ++f)if (visible(d, c.at(x + OFF[f][0], y + OFF[f][1], z + OFF[f][2]))) { float uv[4][2]; for (int k = 0; k < 4; ++k) { uv[k][0] = od->faces[f].uv[k * 2]; uv[k][1] = od->faces[f].uv[k * 2 + 1]; }if (!c.quad(p, f, CUBE[f], uv, od->faces[f].tile, lx, ly, lz))return false; } }
            else if (od->emitter == bd::EmitterKind::Cross) { float a = .08f, q = .92f; float p1[4][3] = { {a,a,0},{q,q,0},{q,q,1},{a,a,1} }, p2[4][3] = { {q,a,0},{a,q,0},{a,q,1},{q,a,1} }, rev[4][3], uv[4][2] = { {0,16},{16,16},{16,0},{0,0} }, revUv[4][2]; for (int k = 0; k < 4; ++k) { for (int j = 0; j < 3; ++j)rev[k][j] = p1[3 - k][j]; for (int j = 0; j < 2; ++j)revUv[k][j] = uv[3 - k][j]; }if (!c.quad(p, 4, p1, uv, od->faces[0].tile, lx, ly, lz) || !c.quad(p, 5, rev, revUv, od->faces[0].tile, lx, ly, lz))return false; for (int k = 0; k < 4; ++k)for (int j = 0; j < 3; ++j)rev[k][j] = p2[3 - k][j]; if (!c.quad(p, 2, p2, uv, od->faces[0].tile, lx, ly, lz) || !c.quad(p, 3, rev, revUv, od->faces[0].tile, lx, ly, lz))return false; }
            else if (od->emitter == bd::EmitterKind::Liquid) { auto k = d->liquid; float hs = fluidAt(c, x, y, z, k), xn = fluidAt(c, x - 1, y, z, k), xp = fluidAt(c, x + 1, y, z, k), yn = fluidAt(c, x, y - 1, z, k), yp = fluidAt(c, x, y + 1, z, k); float h[4]; if (hs >= 1)std::fill(h, h + 4, 1); else { h[0] = avg(c, k, hs, yn, xn, x - 1, y - 1, z); h[1] = avg(c, k, hs, yn, xp, x + 1, y - 1, z); h[2] = avg(c, k, hs, yp, xp, x + 1, y + 1, z); h[3] = avg(c, k, hs, yp, xn, x - 1, y + 1, z); }bool up = visible(d, c.at(x, y, z + 1)), down = visible(d, c.at(x, y, z - 1)); for (float& v : h)v = std::max(0.f, v - (up ? FLUID_OFF : 0)); float uvfull[4][2] = { {0,16},{16,16},{16,0},{0,0} }, pos[4][3]; if (up) { float t[4][3] = { {0,0,h[0]},{1,0,h[1]},{1,1,h[2]},{0,1,h[3]} }; if (!c.quad(p, 0, t, uvfull, od->faces[0].tile, lx, ly, lz))return false; }float bot = down ? FLUID_OFF : 0; if (down) { float t[4][3] = { {0,1,bot},{1,1,bot},{1,0,bot},{0,0,bot} }; if (!c.quad(p, 1, t, uvfull, od->faces[1].tile, lx, ly, lz))return false; }for (int f = 2; f < 6; ++f)if (visible(d, c.at(x + OFF[f][0], y + OFF[f][1], z))) { float inset = FLUID_OFF; float hh0 = f == 2 ? h[1] : f == 3 ? h[3] : f == 4 ? h[2] : h[0], hh1 = f == 2 ? h[2] : f == 3 ? h[0] : f == 4 ? h[3] : h[1]; boxCorners(bd::Box{ {f == 3 ? inset : 0,f == 5 ? inset : 0,bot,f == 2 ? 1 - inset : 1,f == 4 ? 1 - inset : 1,1} }, f, pos); pos[2][2] = hh1; pos[3][2] = hh0; float uv[4][2] = { {0,8},{8,8},{8,std::clamp((1 - hh1) * 8.f,0.f,8.f)},{0,std::clamp((1 - hh0) * 8.f,0.f,8.f)} }; if (!c.quad(p, f, pos, uv, od->faces[f].tile, lx, ly, lz))return false; } }
            else {
                uint8_t mask = od->emitter == bd::EmitterKind::Connection ? connMask(c, x, y, z, d) : 0;
                bd::TemplateDef direct; const bd::TemplateDef* t = nullptr;
                if (od->emitter == bd::EmitterKind::Connection)t = bd::TemplateForMask(od, mask);
                else { direct.boxes = od->boxes; direct.quads = od->quads; t = &direct; }
                if (!t)return false;
                for (const auto& q : t->quads) {
                    bool covered = false;
                    if (q.boxIndex != 0xffff) {
                        const auto& b = t->boxes[q.boxIndex]; covered = coverRect(b, q.face, t->boxes, 0, 0, 0);
                        if (!covered && (q.flags & 1)) {
                            int nx = x + OFF[q.face][0], ny = y + OFF[q.face][1], nz = z + OFF[q.face][2]; auto& nb = c.at(nx, ny, nz); auto* nd = bd::FindDef(nb.id);
                            if (nd) { if ((nd->flags & bd::FLAG_TRANSPARENT) && nd->connection != bd::ConnectionKind::Pane && nd->connection != bd::ConnectionKind::Bars)covered = false; else if (bd::OrientFullCube(nd, nb.orient))covered = true; else { auto* no = bd::OrientEntry(nd, nb.orient); uint8_t nm = no && no->emitter == bd::EmitterKind::Connection ? connMask(c, nx, ny, nz, nd) : 0; const bd::TemplateDef* nt = no && no->emitter == bd::EmitterKind::Connection ? bd::TemplateForMask(no, nm) : nullptr; bd::TemplateDef ndirect; if (no && (no->emitter == bd::EmitterKind::Shape || no->emitter == bd::EmitterKind::Model)) { ndirect.boxes = no->boxes; nt = &ndirect; }if (nt)covered = coverRect(b, q.face, nt->boxes, float(OFF[q.face][0]), float(OFF[q.face][1]), float(OFF[q.face][2])); } }
                        }
                    }
                    if (covered)continue; float pos[4][3], uv[4][2]; for (int i = 0; i < 4; ++i) { for (int a = 0; a < 3; ++a)pos[i][a] = q.pos[i * 3 + a]; uv[i][0] = q.uv[i * 2]; uv[i][1] = q.uv[i * 2 + 1]; }if (!c.quad(p, q.face, pos, uv, q.tile, lx, ly, lz))return false;
                }
            }
            o.emitters.faces[ef] += p.faces - before;
        }
        return true;
    }
}
