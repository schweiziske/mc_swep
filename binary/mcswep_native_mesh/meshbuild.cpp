#include "pch.h"
#include "meshbuild.h"
#include "meshworker.h"
#include "blockdefs.h"
#include "worldstate.h"
#include "blob_format.h"
#include "emitters.h"

#include "tier0/platform.h"

#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <utility>

using namespace GarrysMod::Lua;
using namespace mcmesh;
namespace ws = mcmesh::worldstate;

namespace {
    struct FaceCorner { float ox, oy, oz; int uf, vf; };
    struct FaceDef {
        float nx, ny, nz;
        int ox, oy, oz;
        FaceCorner c[4];
    };

    // cl_mesh_constants.lua:39-46，顺序 TOP,BOTTOM,XP,XN,YP,YN。
    static const FaceDef FACES[6] = {
        { 0,0,1,   0,0,1,   {{0,0,1,0,1},{1,0,1,1,1},{1,1,1,1,0},{0,1,1,0,0}} },
        { 0,0,-1,  0,0,-1,  {{0,1,0,0,1},{1,1,0,1,1},{1,0,0,1,0},{0,0,0,0,0}} },
        { 1,0,0,   1,0,0,   {{1,0,0,0,1},{1,1,0,1,1},{1,1,1,1,0},{1,0,1,0,0}} },
        { -1,0,0, -1,0,0,   {{0,1,0,0,1},{0,0,0,1,1},{0,0,1,1,0},{0,1,1,0,0}} },
        { 0,1,0,   0,1,0,   {{1,1,0,0,1},{0,1,0,1,1},{0,1,1,1,0},{1,1,1,0,0}} },
        { 0,-1,0,  0,-1,0,  {{0,0,0,0,1},{1,0,0,1,1},{1,0,1,1,0},{0,0,1,0,0}} },
    };

    static const int TRI[6] = { 0, 2, 1, 0, 3, 2 };

    inline void RotatedUVFlags(int uf, int vf, int rot, bool flipU, bool flipV, int& ou, int& ov) {
        rot &= 3;
        if (rot == 1) { int nu = vf, nv = 1 - uf; uf = nu; vf = nv; }
        else if (rot == 2) { uf = 1 - uf; vf = 1 - vf; }
        else if (rot == 3) { int nu = 1 - vf, nv = uf; uf = nu; vf = nv; }
        if (flipU) uf = 1 - uf;
        if (flipV) vf = 1 - vf;
        ou = uf; ov = vf;
    }

    inline void TileUV(int tile, double& u0, double& v0, double& u1, double& v1) {
        const auto& A = blockdefs::g_atlas;
        const int col = tile % A.cols;
        const int row = tile / A.cols;
        const int stride = A.stride ? A.stride : A.tile;
        const double px0 = double(col) * stride + A.pad;
        const double py0 = double(row) * stride + A.pad;
        const double inset = A.inset;
        u0 = (px0 + inset) / A.w;
        v0 = (py0 + inset) / A.h;
        u1 = (px0 + A.tile - inset) / A.w;
        v1 = (py0 + A.tile - inset) / A.h;
    }

    inline bool FaceVisible(uint16_t curId, uint16_t nbId, uint8_t nbOrient) {
        if (nbId == 0) return true;
        const blockdefs::BlockDef* nd = blockdefs::FindDef(nbId);
        if (!nd) return true;
        if (nd->flags & blockdefs::FLAG_TRANSPARENT) return nbId != curId;
        if (!blockdefs::OrientFullCube(nd, nbOrient)) return true;
        return false;
    }

    inline int PopCount64(uint64_t v) {
        int n = 0;
        while (v) { v &= v - 1; ++n; }
        return n;
    }

    inline int LowestSetBit(uint64_t mask) {
        int bit = 0;
        while ((mask & 1) == 0) { mask >>= 1; ++bit; }
        return bit;
    }

    inline bool Ready() {
        auto defsLock = blockdefs::LockForRead();
        return g_promise.SS > 0 && blockdefs::g_loaded;
    }

    inline IMatRenderContext* RC() {
        return g_matsys ? g_matsys->GetRenderContext() : nullptr;
    }

    using GenerationArray = std::array<uint64_t, 64>;
    std::unordered_map<uint64_t, GenerationArray> g_generations;
    uint64_t g_nextGeneration = 0;
    std::deque<mcmesh::meshworker::Result> g_readyResults;

    bool DirtyBitSet(uint64_t key, int sec) {
        auto it = ws::g_dirtyMask.find(key);
        return it != ws::g_dirtyMask.end() && (it->second & (1ull << sec)) != 0;
    }

    bool GenerationMatches(uint64_t key, int sec, uint64_t generation) {
        auto it = g_generations.find(key);
        return it != g_generations.end() && it->second[sec] == generation;
    }

    int PushThinkResult(ILuaBase* LUA, size_t pending, int committed) {
        LUA->CreateTable();
        LUA->PushNumber((double)pending); LUA->SetField(-2, "pending");
        LUA->PushNumber((double)committed); LUA->SetField(-2, "committed");
        if (mcmesh::meshbuild::g_faulted) {
            LUA->PushBool(true); LUA->SetField(-2, "failed");
            LUA->PushString(mcmesh::meshbuild::g_faultReason); LUA->SetField(-2, "fault");
        }
        return 1;
    }
}

namespace mcmesh::meshbuild {
    static void RecordTime(TimeProfile& profile, double us) {
        ++profile.calls;
        profile.lastUs = us;
        profile.totalUs += us;
        if (us > profile.maxUs) profile.maxUs = us;
    }


    bool CaptureSectionSnapshot(uint64_t chunkKey, int sec, SectionSnapshot& out) {
        if (g_promise.SS <= 0 || sec < 0 || sec >= g_promise.SPC) return false;
        if (ws::g_world.find(chunkKey) == ws::g_world.end()) return false;

        const int cs = g_promise.CS;
        const int ch = g_promise.CH;
        const int ss = g_promise.SS;
        const int nsx = g_promise.NSX;
        const int sx = sec % nsx;
        const int sy = (sec / nsx) % nsx;
        const int sz = sec / (nsx * nsx);

        out.section = sec;
        out.coreSize = ss;
        out.halo = 2;
        out.side = ss + 4;
        out.lx0 = sx * ss;
        out.ly0 = sy * ss;
        out.lz0 = sz * ss;
        const size_t cellCount = size_t(out.side) * size_t(out.side) * size_t(out.side);
        out.cells.assign(cellCount, SnapshotCell{});

        int cx, cy, cz;
        ws::UnpackChunkKey(chunkKey, cx, cy, cz);
        out.cx=cx;out.cy=cy;out.cz=cz;

        for (int z = 0; z < out.side; ++z)
        for (int y = 0; y < out.side; ++y)
        for (int x = 0; x < out.side; ++x) {
            int lx = out.lx0 + x - out.halo;
            int ly = out.ly0 + y - out.halo;
            int lz = out.lz0 + z - out.halo;
            int ncx = cx, ncy = cy, ncz = cz;
            if (lx < 0) { --ncx; lx += cs; } else if (lx >= cs) { ++ncx; lx -= cs; }
            if (ly < 0) { --ncy; ly += cs; } else if (ly >= cs) { ++ncy; ly -= cs; }
            if (lz < 0) { --ncz; lz += ch; } else if (lz >= ch) { ++ncz; lz -= ch; }

            auto wit = ws::g_world.find(ws::PackChunkKey(ncx, ncy, ncz));
            if (wit == ws::g_world.end()) continue;
            const int li = lx + ly * cs + lz * cs * cs;
            SnapshotCell& cell = out.cells[size_t(x) + size_t(y) * out.side
                + size_t(z) * out.side * out.side];
            cell.stateId = wit->second->stateId[li];
            const blockdefs::StateDef* stateDef = blockdefs::FindState(cell.stateId);
            if (cell.stateId != 0 && !stateDef) return false;
            if (stateDef) { cell.id = stateDef->blockId; cell.orient = stateDef->orient; }
        }
        return true;
    }

    bool BuildSectionVerts(const SectionSnapshot& snapshot, SectionBuild& out) {
        out = SectionBuild{};
        if (snapshot.coreSize <= 0 || snapshot.halo != 2 || snapshot.side != snapshot.coreSize + 4) { out.ok=false; return false; }
        const size_t side=size_t(snapshot.side);
        if (side > 64 || snapshot.cells.size()!=side*side*side) { out.ok=false; return false; }
        auto lock=blockdefs::LockForRead();
        if (!blockdefs::g_loaded || !blockdefs::g_atlas.valid) { out.ok=false; return false; }
        try { out.ok=emitters::Build(snapshot,out); } catch (...) { out.ok=false; }
        return out.ok;
    }

    enum class MeshCreateStatus { Empty, Created, Failed };
    struct MeshCreateResult {
        MeshCreateStatus status = MeshCreateStatus::Failed;
        IMesh* mesh = nullptr;
    };

    static MeshCreateResult CreateMeshFromVerts(const std::vector<Vert>& verts, IMaterial* mat) {
        if (verts.empty()) return { MeshCreateStatus::Empty, nullptr };
        if (verts.size() > mcmesh::MAX_VERTEX_COUNT || (verts.size() % 3) != 0)
            return { MeshCreateStatus::Failed, nullptr };
        if (!mat || mat->IsErrorMaterial()) return { MeshCreateStatus::Failed, nullptr };
        IMatRenderContext* ctx = RC();
        if (!ctx) return { MeshCreateStatus::Failed, nullptr };

        const VertexFormat_t fmt = mat->GetVertexFormat();
        IMesh* mesh = ctx->CreateStaticMesh(fmt & ~VERTEX_FORMAT_COMPRESSED,
            TEXTURE_GROUP_STATIC_VERTEX_BUFFER_OTHER, mat);
        if (!mesh) return { MeshCreateStatus::Failed, nullptr };

        CMeshBuilder mb;
        mb.Begin(mesh, MATERIAL_TRIANGLES, (int)(verts.size() / 3));
        for (const Vert& v : verts) {
            mb.Position3f(v.pos[0], v.pos[1], v.pos[2]);
            mb.Normal3f(v.normal[0], v.normal[1], v.normal[2]);
            mb.TexCoord2f(0, v.u, v.v);
            mb.TexCoord2f(1, 0.0f, 0.0f);
            mb.Color4ub(v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3]);
            mb.AdvanceVertex();
        }
        mb.End();
        return { MeshCreateStatus::Created, mesh };
    }

    static void DestroyMesh(IMatRenderContext* ctx, IMesh*& mesh) {
        if (!mesh) return;
        if (ctx) ctx->DestroyStaticMesh(mesh);
        mesh = nullptr;
    }

    static bool CreateMeshList(const PassVerts& pass, IMaterial* mat, std::vector<IMesh*>& out) {
        IMatRenderContext* ctx=RC();
        for (const auto& batch:pass.batches) {
            MeshCreateResult made=CreateMeshFromVerts(batch.verts,mat);
            if (made.status==MeshCreateStatus::Failed) { for(IMesh*&m:out) DestroyMesh(ctx,m); out.clear(); return false; }
            if (made.mesh) out.push_back(made.mesh);
        }
        return true;
    }
    static void DestroyMeshList(IMatRenderContext* ctx,std::vector<IMesh*>& list){for(IMesh*&m:list)DestroyMesh(ctx,m);list.clear();}
    static void SubtractEmitterStats(EmitterStats& total,const EmitterStats& part){
        for(size_t i=0;i<6;++i){
            total.blocks[i]=part.blocks[i]>total.blocks[i]?0:total.blocks[i]-part.blocks[i];
            total.faces[i]=part.faces[i]>total.faces[i]?0:total.faces[i]-part.faces[i];
        }
    }
    bool StageSectionMeshes(const SectionBuild& build, SectionMeshes& staged) {
        staged = SectionMeshes{};
        try {
            if (!CreateMeshList(build.opaque, g_promise.MatOpaque, staged.opaque)
                || !CreateMeshList(build.translucent, g_promise.MatTranslucent, staged.translucent)) {
                DestroyStagedSectionMeshes(staged);
                return false;
            }
            staged.opaqueVerts = (int)build.opaque.vertices;
            staged.translucentVerts = (int)build.translucent.vertices;
            staged.emitters = build.emitters;
            return true;
        }
        catch (...) {
            DestroyStagedSectionMeshes(staged);
            return false;
        }
    }

    void DestroyStagedSectionMeshes(SectionMeshes& staged) {
        if (staged.opaque.empty() && staged.translucent.empty()) {
            staged = SectionMeshes{};
            return;
        }
        IMatRenderContext* ctx = RC();
        DestroyMeshList(ctx, staged.opaque);
        DestroyMeshList(ctx, staged.translucent);
        staged = SectionMeshes{};
    }

    bool CommitStagedSectionMeshes(uint64_t key, int sec, SectionMeshes& staged) {
        const double commitT0 = Plat_FloatTime();
        try {
            auto inserted = g_meshes.try_emplace(key);
            SectionMeshes& live = inserted.first->second.section[sec];
            SectionMeshes old = std::move(live);
            live = std::move(staged);
            SubtractEmitterStats(g_emitterStats, old.emitters);
            for (size_t i = 0; i < 6; ++i) {
                g_emitterStats.blocks[i] += live.emitters.blocks[i];
                g_emitterStats.faces[i] += live.emitters.faces[i];
            }
            RecordTime(g_meshCommitProfile, (Plat_FloatTime() - commitT0) * 1e6);
            const double destroyT0 = Plat_FloatTime();
            DestroyStagedSectionMeshes(old);
            RecordTime(g_meshDestroyProfile, (Plat_FloatTime() - destroyT0) * 1e6);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void DestroyChunkMeshes(uint64_t chunkKey) {
        auto it = g_meshes.find(chunkKey);
        if (it == g_meshes.end()) return;
        IMatRenderContext* ctx = RC();
        for (int i = 0; i < 64; ++i) {
            SubtractEmitterStats(g_emitterStats,it->second.section[i].emitters);
            DestroyMeshList(ctx, it->second.section[i].opaque);
            DestroyMeshList(ctx, it->second.section[i].translucent);
        }
        g_meshes.erase(it);
    }

    void DestroyAllMeshes() {
        IMatRenderContext* ctx = RC();
        for (auto& kv : g_meshes)
            for (int i = 0; i < 64; ++i) {
                SubtractEmitterStats(g_emitterStats,kv.second.section[i].emitters);
                DestroyMeshList(ctx, kv.second.section[i].opaque);
                DestroyMeshList(ctx, kv.second.section[i].translucent);
            }
        g_meshes.clear();
        g_emitterStats={};
    }

    void StopWorkers() {
        meshworker::Stop();
        for (auto& result : g_readyResults)
            DestroyStagedSectionMeshes(result.stagedMeshes);
        g_readyResults.clear();
        g_generations.clear();
    }

    int Think(ILuaBase* LUA) {
        if (!LUA->IsType(1, Type::Number)) { LUA->PushBool(false); return 1; }
        double budgetMS = LUA->GetNumber(1);
        if (!std::isfinite(budgetMS) || budgetMS < 0 || budgetMS > 100) { LUA->PushBool(false); return 1; }
        if (!Ready()) return PushThinkResult(LUA, 0, 0);
        if (g_faulted) return PushThinkResult(LUA, 0, 0);
        const double budgetSec = budgetMS / 1000.0;
        const double t0 = Plat_FloatTime();

        if (!meshworker::Start()) {
            meshworker::Stop();
            g_faulted = true;
            g_faultReason = "worker_start_failed";
            return PushThinkResult(LUA, 0, 0);
        }

        // Transfer ownership with a deque swap. No allocation can occur here, so
        // every outstanding/resultBytes reservation remains represented exactly once.
        meshworker::CollectResults(g_readyResults);

        int finalized = 0;
        while (!g_readyResults.empty()) {
            if (finalized > 0 && (Plat_FloatTime() - t0) >= budgetSec) break;
            meshworker::Result& result = g_readyResults.front();
            RecordTime(g_vertexBuildProfile, result.vertexBuildUs);
            if constexpr (meshworker::kWorkerCreatesMeshes) {
                RecordTime(g_meshStageProfile, result.meshStageUs);
            }
            else if (result.ok) {
                const double stageT0 = Plat_FloatTime();
                result.meshesReady = StageSectionMeshes(result.build, result.stagedMeshes);
                result.meshStageUs = (Plat_FloatTime() - stageT0) * 1e6;
                RecordTime(g_meshStageProfile, result.meshStageUs);
                result.build = SectionBuild{};
                if (!result.meshesReady) {
                    result.ok = false;
                    result.meshCreateFailed = true;
                }
            }

            const bool chunkAlive = ws::g_world.find(result.chunkKey) != ws::g_world.end();
            // dirty bit 重新置位说明快照之后世界又变了；即使新 job 尚未投递也不能上屏。
            const bool current = chunkAlive
                && GenerationMatches(result.chunkKey, result.section, result.generation)
                && !DirtyBitSet(result.chunkKey, result.section);
            if (!current) {
                // jobsDropped 由 worker 统计队列丢弃；此处 stale 结果另行记账。
                DestroyStagedSectionMeshes(result.stagedMeshes);
                meshworker::RecordStaleDrop();
                meshworker::ReleaseResult(result.resultBytes);
                g_readyResults.pop_front();
                continue;
            }

            if (!result.ok || !result.meshesReady) {
                const bool meshCreateFailed = result.meshCreateFailed;
                DestroyStagedSectionMeshes(result.stagedMeshes);
                meshworker::ReleaseResult(result.resultBytes);
                g_readyResults.pop_front();
                g_faulted = true;
                if (meshCreateFailed) {
                    ++g_meshCreateFailures;
                    g_faultReason = "worker_mesh_create_failed";
                }
                else g_faultReason = "worker_build_failed";
                break;
            }
            if (!CommitStagedSectionMeshes(result.chunkKey, result.section, result.stagedMeshes)) {
                DestroyStagedSectionMeshes(result.stagedMeshes);
                meshworker::ReleaseResult(result.resultBytes);
                g_readyResults.pop_front();
                ++g_meshCreateFailures;
                g_faulted = true;
                g_faultReason = "mesh_create_failed";
                break;
            }
            g_lastBuildUs = result.buildUs;
            meshworker::ReleaseResult(result.resultBytes);
            g_readyResults.pop_front();
            ++finalized;
        }

        // 脏位是有界溢出缓冲。只有 job 成功入队后才清位；上限满则原位保留。
        while (!g_faulted && !ws::g_dirtyQueue.empty()) {
            const uint64_t key = ws::g_dirtyQueue.front();
            auto mit = ws::g_dirtyMask.find(key);
            if (mit == ws::g_dirtyMask.end() || mit->second == 0) {
                ws::g_dirtyQueue.erase(ws::g_dirtyQueue.begin());
                if (mit != ws::g_dirtyMask.end()) ws::g_dirtyMask.erase(mit);
                continue;
            }
            if (ws::g_world.find(key) == ws::g_world.end()) {
                ws::g_dirtyQueue.erase(ws::g_dirtyQueue.begin());
                ws::g_dirtyMask.erase(mit);
                continue;
            }

            const int sec = LowestSetBit(mit->second);
            SectionSnapshot snapshot;
            if (!CaptureSectionSnapshot(key, sec, snapshot)) break;

            GenerationArray& generations = g_generations[key];
            uint64_t generation = ++g_nextGeneration;
            if (generation == 0) generation = ++g_nextGeneration;
            meshworker::Job job;
            job.chunkKey = key;
            job.section = sec;
            job.generation = generation;
            job.snapshot = std::move(snapshot);
            if (!meshworker::TryEnqueue(std::move(job))) break;

            generations[sec] = generation;
            mit->second &= ~(1ull << sec);
            if (mit->second == 0) {
                ws::g_dirtyQueue.erase(ws::g_dirtyQueue.begin());
                ws::g_dirtyMask.erase(mit);
            }
        }

        g_lastThinkMS = (Plat_FloatTime() - t0) * 1000.0;
        int dirty = 0;
        for (const auto& kv : ws::g_dirtyMask) dirty += PopCount64(kv.second);
        const meshworker::Stats stats = meshworker::GetStats();
        const size_t pending = size_t(dirty) + stats.outstanding;
        return PushThinkResult(LUA, pending, finalized);
    }

    int DrawChunk(ILuaBase* LUA) {
        int cx, cy, cz;
        if (!ws::ReadChunkCoords(LUA, cx, cy, cz) || g_faulted) {
            LUA->PushBool(false);
            return 1;
        }
        if (!LUA->IsType(4, Type::Number)) { LUA->PushBool(false); return 1; }
        const double passD = LUA->GetNumber(4);
        if (!std::isfinite(passD) || passD != std::floor(passD) || (passD != 0 && passD != 1)) {
            LUA->PushBool(false);
            return 1;
        }
        const int pass = (int)passD;

        auto it = g_meshes.find(ws::PackChunkKey(cx, cy, cz));
        if (it != g_meshes.end())
            for (int i = 0; i < 64; ++i) {
                const auto& meshes = pass == 0 ? it->second.section[i].opaque : it->second.section[i].translucent;
                for (IMesh* mesh : meshes) if (mesh) mesh->Draw();
            }
        LUA->PushBool(true);
        return 1;
    }

    int UnloadChunk(ILuaBase* LUA) {
        int cx, cy, cz;
        if (!ws::ReadChunkCoords(LUA, cx, cy, cz)) {
            LUA->PushBool(false);
            return 1;
        }
        const uint64_t key = ws::PackChunkKey(cx, cy, cz);
        const size_t workerRemoved = meshworker::DiscardChunk(key);
        for (size_t i = 0; i < workerRemoved; ++i) meshworker::ReleaseResult();
        g_generations.erase(key);
        for (auto it = g_readyResults.begin(); it != g_readyResults.end();) {
            if (it->chunkKey == key) {
                DestroyStagedSectionMeshes(it->stagedMeshes);
                meshworker::ReleaseResult(it->resultBytes);
                meshworker::RecordStaleDrop();
                it = g_readyResults.erase(it);
            }
            else ++it;
        }
        DestroyChunkMeshes(key);
        return ws::UnloadChunk(LUA);
    }

    int ApplyChunk(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0; unsigned int len=0;
        if (g_promise.SS<=0 || g_faulted || !ws::ReadChunkCoords(LUA,cx,cy,cz)) { LUA->PushBool(false); return 1; }
        const char* data=LUA->GetString(4,&len);
        if (!data || len!=kStateBlobBytes || !blockdefs::Seal()) { LUA->PushBool(false); return 1; }
        return ws::ApplyChunk(LUA);
    }

    int CreateChunk(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushBool(false); return 1; }
        return ws::CreateChunk(LUA);
    }

    int ApplyChunkCells(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushBool(false); return 1; }
        return ws::ApplyChunkCells(LUA);
    }

    int SetCell(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushBool(false); return 1; }
        return ws::SetCell(LUA);
    }

    int GetCell(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushNil(); return 1; }
        return ws::GetCell(LUA);
    }

    int GetChunkCells(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushNil(); return 1; }
        return ws::GetChunkCells(LUA);
    }

    int RebuildBlockLight(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushBool(false); return 1; }
        return ws::RebuildBlockLight(LUA);
    }

    int GetBlockLightChunk(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushNil(); return 1; }
        return ws::GetBlockLightChunk(LUA);
    }

    int GetSkyLightChunk(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushNil(); return 1; }
        return ws::GetSkyLightChunk(LUA);
    }

    int StartBlockLightRebuild(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushBool(false); return 1; }
        return ws::StartBlockLightRebuild(LUA);
    }

    int PollBlockLightRebuild(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted) { LUA->PushBool(false); return 1; }
        return ws::PollBlockLightRebuild(LUA);
    }

    int ClearWorld(ILuaBase* LUA) {
        const size_t workerRemoved = meshworker::DiscardAll();
        for (size_t i = 0; i < workerRemoved; ++i) meshworker::ReleaseResult();
        for (auto& result : g_readyResults) {
            DestroyStagedSectionMeshes(result.stagedMeshes);
            meshworker::ReleaseResult(result.resultBytes);
            meshworker::RecordStaleDrop();
        }
        g_readyResults.clear();
        g_generations.clear();
        DestroyAllMeshes();
        blockdefs::Unseal();
        g_faulted = false; g_faultReason = ""; g_lastThinkMS=0; g_lastBuildUs=0; g_meshCreateFailures=0; g_emitterStats={}; g_resultBytes=0;
        g_vertexBuildProfile={}; g_meshStageProfile={}; g_meshCommitProfile={}; g_meshDestroyProfile={};
        return ws::ClearWorld(LUA);
    }

    int Shutdown(ILuaBase* LUA) {
        StopWorkers();
        ws::StopBlockLightWorker();
        DestroyAllMeshes();
        blockdefs::Clear();
        g_faulted = false; g_faultReason = ""; g_lastThinkMS=0; g_lastBuildUs=0; g_meshCreateFailures=0; g_emitterStats={}; g_resultBytes=0;
        g_vertexBuildProfile={}; g_meshStageProfile={}; g_meshCommitProfile={}; g_meshDestroyProfile={};
        ws::g_world.clear();
        ws::g_dirtyMask.clear();
        ws::g_dirtyQueue.clear(); ws::g_status={}; g_promise={};
        return 0;
    }

    int GetStats(ILuaBase* LUA) {
        int opaqueMeshCount = 0, translucentMeshCount = 0;
        long long opaqueVertCount = 0, translucentVertCount = 0;
        for (const auto& kv : g_meshes)
            for (int i = 0; i < 64; ++i) {
                const SectionMeshes& sm = kv.second.section[i];
                if (!sm.opaque.empty()) { opaqueMeshCount += (int)sm.opaque.size(); opaqueVertCount += sm.opaqueVerts; }
                if (!sm.translucent.empty()) { translucentMeshCount += (int)sm.translucent.size(); translucentVertCount += sm.translucentVerts; }
            }
        const int meshCount = opaqueMeshCount + translucentMeshCount;
        const long long vertCount = opaqueVertCount + translucentVertCount;

        int dirty = 0;
        for (const auto& kv : ws::g_dirtyMask) dirty += PopCount64(kv.second);
        const meshworker::Stats stats = meshworker::GetStats();
        const size_t pending = size_t(dirty) + stats.outstanding;

        LUA->CreateTable();
        LUA->PushNumber((double)ws::g_world.size()); LUA->SetField(-2, "mirrorChunks");
        LUA->PushNumber((double)meshCount); LUA->SetField(-2, "meshes");
        LUA->PushNumber((double)vertCount); LUA->SetField(-2, "vertices");
        LUA->PushNumber(opaqueMeshCount); LUA->SetField(-2, "opaqueMeshes");
        LUA->PushNumber(translucentMeshCount); LUA->SetField(-2, "translucentMeshes");
        LUA->PushNumber((double)opaqueVertCount); LUA->SetField(-2, "opaqueVertices");
        LUA->PushNumber((double)translucentVertCount); LUA->SetField(-2, "translucentVertices");
        LUA->PushNumber((double)g_meshCreateFailures); LUA->SetField(-2, "meshCreateFailures");
        LUA->PushBool(g_faulted); LUA->SetField(-2, "faulted");
        LUA->PushString(g_faultReason); LUA->SetField(-2, "faultReason");
        LUA->PushNumber((double)pending); LUA->SetField(-2, "pendingSections");
        LUA->PushNumber(g_lastBuildUs); LUA->SetField(-2, "lastBuildUs");
        LUA->PushNumber(g_lastThinkMS); LUA->SetField(-2, "lastThinkMS");
        LUA->PushNumber(stats.workerCount); LUA->SetField(-2, "workerCount");
        LUA->PushNumber((double)stats.maxInFlight); LUA->SetField(-2, "maxInFlight");
        LUA->PushBool(stats.workerCreatesMeshes); LUA->SetField(-2, "workerCreatesMeshes");
        LUA->PushNumber((double)stats.queuedJobs); LUA->SetField(-2, "queuedJobs");
        LUA->PushNumber((double)stats.activeJobs); LUA->SetField(-2, "activeJobs");
        LUA->PushNumber((double)(stats.queuedResults + g_readyResults.size())); LUA->SetField(-2, "queuedResults");
        LUA->PushNumber((double)stats.jobsEnqueued); LUA->SetField(-2, "jobsEnqueued");
        LUA->PushNumber((double)stats.jobsDropped); LUA->SetField(-2, "jobsDropped");
        LUA->PushNumber((double)stats.resultBytes); LUA->SetField(-2, "resultBytes");
        auto pushProfile = [LUA](const char* prefix, const TimeProfile& p) {
            const std::string base(prefix);
            LUA->PushNumber((double)p.calls); LUA->SetField(-2, (base + "Calls").c_str());
            LUA->PushNumber(p.lastUs); LUA->SetField(-2, (base + "LastUs").c_str());
            LUA->PushNumber(p.calls ? p.totalUs / (double)p.calls : 0.0); LUA->SetField(-2, (base + "AvgUs").c_str());
            LUA->PushNumber(p.maxUs); LUA->SetField(-2, (base + "MaxUs").c_str());
            LUA->PushNumber(p.totalUs / 1000.0); LUA->SetField(-2, (base + "TotalMS").c_str());
        };
        pushProfile("vertexBuild", g_vertexBuildProfile);
        pushProfile("meshStage", g_meshStageProfile);
        pushProfile("meshCommit", g_meshCommitProfile);
        pushProfile("meshDestroy", g_meshDestroyProfile);
        LUA->PushNumber((double)ws::g_status.LastApplyDirty); LUA->SetField(-2, "lastApplyDirty");
        LUA->PushNumber((double)ws::g_status.BatchCalls); LUA->SetField(-2, "nativeBatchCalls");
        LUA->PushNumber((double)ws::g_status.BatchCells); LUA->SetField(-2, "nativeBatchCells");
        LUA->PushNumber(ws::g_status.BatchLastMS); LUA->SetField(-2, "nativeBatchLastMS");
        LUA->PushNumber(ws::g_status.BatchTotalMS); LUA->SetField(-2, "nativeBatchTotalMS");
        LUA->PushNumber((double)ws::g_status.SetCellCalls); LUA->SetField(-2, "nativeSetCellCalls");
        LUA->PushNumber(ws::g_status.SetCellLastUs); LUA->SetField(-2, "nativeSetCellLastUs");
        LUA->PushNumber(ws::g_status.SetCellTotalMS); LUA->SetField(-2, "nativeSetCellTotalMS");
        LUA->PushNumber((double)ws::g_status.LightRebuilds); LUA->SetField(-2, "nativeLightRebuilds");
        LUA->PushNumber(ws::g_status.LightLastMS); LUA->SetField(-2, "nativeLightLastMS");
        LUA->PushNumber((double)ws::g_status.LightSources); LUA->SetField(-2, "nativeLightSources");
        LUA->PushNumber((double)ws::g_status.LightProcessed); LUA->SetField(-2, "nativeLightProcessed");
        LUA->PushNumber((double)ws::g_status.LightWritten); LUA->SetField(-2, "nativeLightWritten");
        LUA->PushNumber(ws::g_status.SkyLastMS); LUA->SetField(-2, "nativeSkyLastMS");
        LUA->PushNumber((double)ws::g_status.SkySources); LUA->SetField(-2, "nativeSkySources");
        LUA->PushNumber((double)ws::g_status.SkyProcessed); LUA->SetField(-2, "nativeSkyProcessed");
        LUA->PushNumber((double)ws::g_status.SkyWritten); LUA->SetField(-2, "nativeSkyWritten");
        LUA->PushNumber((double)ws::g_worldVersion); LUA->SetField(-2, "nativeWorldVersion");
        LUA->PushNumber((double)blockdefs::g_counts.states); LUA->SetField(-2, "stateDefinitions");
        LUA->PushBool(blockdefs::g_lightEmission.size()==blockdefs::g_states.size()
            && blockdefs::g_lightOpacity.size()==blockdefs::g_states.size()); LUA->SetField(-2, "lightDefinitionsLoaded");
        LUA->PushBool(blockdefs::g_visual.loaded); LUA->SetField(-2, "generatedCatalogLoaded");
        LUA->PushNumber((double)blockdefs::g_visual.catalogStateCount); LUA->SetField(-2, "generatedCatalogStates");
        LUA->PushNumber((double)blockdefs::g_visual.plans.size()); LUA->SetField(-2, "generatedPlans");
        LUA->PushNumber((double)blockdefs::g_visual.models.size()); LUA->SetField(-2, "generatedModels");
        LUA->PushNumber((double)blockdefs::g_visual.geometries.size()); LUA->SetField(-2, "generatedGeometries");
        LUA->PushNumber((double)blockdefs::g_visual.surfaceSets.size()); LUA->SetField(-2, "generatedSurfaceSets");
        LUA->PushNumber((double)blockdefs::g_visual.tintSlotCount); LUA->SetField(-2, "generatedTintSlots");
        static const char* emitterNames[6] = { "fullCube", "cross", "model", "shape", "connection", "liquid" };
        LUA->CreateTable();
        for (int i=0;i<6;++i) { LUA->CreateTable(); LUA->PushNumber((double)g_emitterStats.blocks[i]); LUA->SetField(-2,"blocks"); LUA->PushNumber((double)g_emitterStats.faces[i]); LUA->SetField(-2,"faces"); LUA->SetField(-2,emitterNames[i]); }
        LUA->SetField(-2,"emitters");
        return 1;
    }

    int DebugBuildSection(ILuaBase* LUA) {
        int cx, cy, cz;
        if (!ws::ReadChunkCoords(LUA, cx, cy, cz)) return 0;
        if (!LUA->IsType(4, Type::Number)) return 0;
        const double secD = LUA->GetNumber(4);
        if (!std::isfinite(secD) || secD != std::floor(secD) || secD < 0 || secD >= g_promise.SPC) return 0;
        if (!Ready()) return 0;

        SectionSnapshot snapshot;
        if (!CaptureSectionSnapshot(ws::PackChunkKey(cx, cy, cz), (int)secD, snapshot)) return 0;
        SectionBuild scratch;
        const double t0 = Plat_FloatTime();
        if (!BuildSectionVerts(snapshot, scratch)) return 0;
        const double us = (Plat_FloatTime() - t0) * 1e6;
        const int faces = scratch.opaque.faces + scratch.translucent.faces;
        const size_t verts = scratch.opaque.vertices + scratch.translucent.vertices;

        LUA->CreateTable();
        LUA->PushNumber(faces); LUA->SetField(-2, "faces");
        LUA->PushNumber((double)verts); LUA->SetField(-2, "verts");
        LUA->PushNumber(scratch.opaque.faces); LUA->SetField(-2, "opaqueFaces");
        LUA->PushNumber((double)scratch.opaque.vertices); LUA->SetField(-2, "opaqueVerts");
        LUA->PushNumber(scratch.translucent.faces); LUA->SetField(-2, "translucentFaces");
        LUA->PushNumber((double)scratch.translucent.vertices); LUA->SetField(-2, "translucentVerts");
        LUA->PushNumber(us); LUA->SetField(-2, "buildUs");
        static const char* emitterNames[6] = { "fullCube", "cross", "model", "shape", "connection", "liquid" };
        LUA->CreateTable();
        for (int i=0; i<6; ++i) {
            LUA->PushNumber((double)scratch.emitters.faces[i]);
            LUA->SetField(-2, emitterNames[i]);
        }
        LUA->SetField(-2, "emitterFaces");
        return 1;
    }
}
