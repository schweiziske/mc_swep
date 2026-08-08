#pragma once

#include "pch.h"
#include "global.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mcmesh::meshbuild {
    constexpr int kDirectionalGroupCount = 7;
    constexpr int kUnassignedDirectionalGroup = 6;
    constexpr uint8_t kAllSectionFaces = 0x3f;
    struct Vert { float pos[3]{}, normal[3]{}, u = 0, v = 0; uint8_t rgba[4]{}; };
    struct SnapshotCell { uint32_t stateId = 0; uint16_t id = 0; uint8_t orient = 0; };
    struct SectionSnapshot {
        int section = 0, coreSize = 0, halo = 2, side = 0, lx0 = 0, ly0 = 0, lz0 = 0, cx = 0, cy = 0, cz = 0;
        uint16_t lightSlot = 0;
        uint64_t lightGeneration = 0;
        std::vector<SnapshotCell> cells;
    };
    struct VertexBatch {
        int faces = 0;
        size_t vertices = 0;
        std::array<std::vector<Vert>, kDirectionalGroupCount> groups;
    };
    struct PassVerts { int faces = 0; size_t vertices = 0; std::vector<VertexBatch> batches; };
    struct EmitterStats { std::array<uint64_t, 6> blocks{}, faces{}; };
    struct SectionOcclusion {
        std::array<uint8_t, 6> visibleFrom{};
        bool ready = false;
    };
    struct SectionBuild {
        bool ok = true;
        bool lodReady = false;
        uint16_t lightSlot = 0;
        uint64_t lightGeneration = 0;
        PassVerts opaque, translucent, lodOpaque;
        EmitterStats emitters;
        SectionOcclusion occlusion;
    };
    struct DirectionalRange { int firstIndex = 0, indexCount = 0; };
    struct MeshDirectionalRanges {
        std::array<DirectionalRange, kDirectionalGroupCount> groups{};
        int vertices = 0;
    };
    struct SectionMeshes {
        std::vector<IMesh*> opaque, translucent, lodOpaque;
        std::vector<MeshDirectionalRanges> opaqueRanges, translucentRanges, lodOpaqueRanges;
        int opaqueVerts = 0, translucentVerts = 0, lodOpaqueVerts = 0;
        bool lodReady = false;
        uint16_t lightSlot = 0;
        uint64_t lightGeneration = 0;
        EmitterStats emitters{};
        SectionOcclusion occlusion{};
    };
    struct ChunkMeshes { SectionMeshes section[64]; };
    struct TimeProfile {
        uint64_t calls = 0;
        double lastUs = 0, totalUs = 0, maxUs = 0;
    };
    struct MeshStageStats {
        TimeProfile allocate{}, upload{};
        uint64_t meshesCreated = 0, batchesUploaded = 0, verticesUploaded = 0, bytesUploaded = 0;
    };
    struct ThinkProfileSample {
        uint64_t sequence = 0;
        double budgetMS = 0, totalUs = 0, budgetOverrunUs = 0;
        double workerStartUs = 0, collectUs = 0, finalizeUs = 0;
        double queueWaitUs = 0, stageUs = 0, allocateUs = 0, uploadUs = 0;
        double replaceUs = 0, destroyUs = 0, dirtyScanUs = 0, captureUs = 0, enqueueUs = 0;
        uint64_t resultsCollected = 0, resultsVisited = 0, committed = 0, stale = 0, failed = 0;
        uint64_t captureAttempts = 0, captureSuccess = 0, enqueueAttempts = 0, enqueueSuccess = 0, enqueueRejected = 0;
        uint64_t meshesCreated = 0, meshesDestroyed = 0, batchesUploaded = 0, verticesUploaded = 0, bytesUploaded = 0;
        size_t dirtyBefore = 0, dirtyAfter = 0;
        size_t workerJobsBefore = 0, workerJobsAfter = 0, workerActiveBefore = 0, workerActiveAfter = 0;
        size_t workerResultsBefore = 0, workerResultsAfter = 0, readyBefore = 0, readyAfter = 0;
        size_t outstandingBefore = 0, outstandingAfter = 0, resultBytesBefore = 0, resultBytesAfter = 0;
    };
    struct DetailedProfileState {
        bool enabled = false;
        uint64_t generation = 0, nextThinkSequence = 0, operationSequence = 0;
        ThinkProfileSample latest{};
        TimeProfile think{}, workerStart{}, collect{}, finalize{}, queueWait{}, stage{}, allocate{}, upload{};
        TimeProfile replace{}, destroy{}, dirtyScan{}, capture{}, enqueue{}, unload{}, clear{}, shutdown{}, workerStop{};
        uint64_t meshesCreated = 0, meshesDestroyed = 0, batchesUploaded = 0, verticesUploaded = 0, bytesUploaded = 0;
        size_t peakDirty = 0, peakWorkerJobs = 0, peakWorkerResults = 0, peakReadyResults = 0;
        size_t peakOutstanding = 0, peakResultBytes = 0;
        const char* lastOperation = "none";
        double lastOperationUs = 0;
    };
    inline std::unordered_map<uint64_t, ChunkMeshes> g_meshes;
    inline double g_lastThinkMS = 0, g_lastBuildUs = 0;
    inline uint64_t g_meshCreateFailures = 0, g_resultBytes = 0;
    inline bool g_simpleGreedyEnabled = true;
    inline bool g_lodBuildEnabled = false;
    inline std::atomic<bool> g_directionalDrawEnabled{ false };
    inline std::atomic<bool> g_occlusionCullEnabled{ false };
    inline std::atomic<bool> g_lodDrawEnabled{ false };
    inline std::atomic<double> g_lodStartChunks{ 24.0 };
    inline EmitterStats g_emitterStats{};
    inline TimeProfile g_vertexBuildProfile{}, g_meshStageProfile{}, g_meshCommitProfile{}, g_meshDestroyProfile{};
    inline DetailedProfileState g_detailedProfile{};
    inline ThinkProfileSample* g_activeThinkProfile = nullptr;
    inline bool g_faulted = false; inline const char* g_faultReason = "";

    bool CaptureSectionSnapshot(uint64_t chunkKey, int sec, SectionSnapshot& out);
    bool BuildSectionVerts(const SectionSnapshot& snapshot, SectionBuild& out);
    bool StageSectionMeshes(const SectionBuild& build, SectionMeshes& staged, MeshStageStats* stats = nullptr);
    bool CommitStagedSectionMeshes(uint64_t key, int sec, SectionMeshes& staged);
    void DestroyStagedSectionMeshes(SectionMeshes& staged, bool recordProfile = true);
    void DestroyChunkMeshes(uint64_t chunkKey); void DestroyAllMeshes(); void StopWorkers();
    int CreateChunk(GarrysMod::Lua::ILuaBase*); int ApplyChunk(GarrysMod::Lua::ILuaBase*); int ApplyChunkCells(GarrysMod::Lua::ILuaBase*);
    int SetCell(GarrysMod::Lua::ILuaBase*); int GetCell(GarrysMod::Lua::ILuaBase*); int GetChunkCells(GarrysMod::Lua::ILuaBase*); int Think(GarrysMod::Lua::ILuaBase*);
    int RebuildBlockLight(GarrysMod::Lua::ILuaBase*); int GetBlockLightChunk(GarrysMod::Lua::ILuaBase*); int GetSkyLightChunk(GarrysMod::Lua::ILuaBase*);
    int GetChunkLightTile(GarrysMod::Lua::ILuaBase*);
    int StartBlockLightRebuild(GarrysMod::Lua::ILuaBase*); int PollBlockLightRebuild(GarrysMod::Lua::ILuaBase*);
    int CancelBlockLightRebuild(GarrysMod::Lua::ILuaBase*);
    int DrawChunk(GarrysMod::Lua::ILuaBase*); int DrawVisible(GarrysMod::Lua::ILuaBase*);
    int SetChunkVisible(GarrysMod::Lua::ILuaBase*); int UnloadChunk(GarrysMod::Lua::ILuaBase*);
    int ClearWorld(GarrysMod::Lua::ILuaBase*); int GetStats(GarrysMod::Lua::ILuaBase*); int SetProfiling(GarrysMod::Lua::ILuaBase*);
    int SetSimpleGreedy(GarrysMod::Lua::ILuaBase*);
    int SetDirectionalDraw(GarrysMod::Lua::ILuaBase*);
    int SetOcclusionCull(GarrysMod::Lua::ILuaBase*);
    int SetLod(GarrysMod::Lua::ILuaBase*);
    void RefreshChunkDrawMetadata(uint64_t key);
    void InvalidateDirtyOcclusionMetadata();
    int Shutdown(GarrysMod::Lua::ILuaBase*); int DebugBuildSection(GarrysMod::Lua::ILuaBase*);
}
