#pragma once

#include "pch.h"
#include "global.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mcmesh::meshbuild {
    struct Vert { float pos[3]{}, normal[3]{}, u = 0, v = 0; uint8_t rgba[4]{}; };
    struct SnapshotCell { uint32_t stateId = 0; uint16_t id = 0; uint8_t orient = 0; };
    struct SectionSnapshot {
        int section = 0, coreSize = 0, halo = 2, side = 0, lx0 = 0, ly0 = 0, lz0 = 0, cx = 0, cy = 0, cz = 0;
        std::vector<SnapshotCell> cells;
    };
    struct VertexBatch { int faces = 0; std::vector<Vert> verts; };
    struct PassVerts { int faces = 0; size_t vertices = 0; std::vector<VertexBatch> batches; };
    struct EmitterStats { std::array<uint64_t, 6> blocks{}, faces{}; };
    struct SectionBuild { bool ok = true; PassVerts opaque, translucent; EmitterStats emitters; };
    struct SectionMeshes {
        std::vector<IMesh*> opaque, translucent;
        int opaqueVerts = 0, translucentVerts = 0;
        EmitterStats emitters{};
    };
    struct ChunkMeshes { SectionMeshes section[64]; };
    struct TimeProfile {
        uint64_t calls = 0;
        double lastUs = 0, totalUs = 0, maxUs = 0;
    };
    inline std::unordered_map<uint64_t, ChunkMeshes> g_meshes;
    inline double g_lastThinkMS = 0, g_lastBuildUs = 0;
    inline uint64_t g_meshCreateFailures = 0, g_resultBytes = 0;
    inline EmitterStats g_emitterStats{};
    inline TimeProfile g_vertexBuildProfile{}, g_meshStageProfile{}, g_meshCommitProfile{}, g_meshDestroyProfile{};
    inline bool g_faulted = false; inline const char* g_faultReason = "";

    bool CaptureSectionSnapshot(uint64_t chunkKey, int sec, SectionSnapshot& out);
    bool BuildSectionVerts(const SectionSnapshot& snapshot, SectionBuild& out);
    bool StageSectionMeshes(const SectionBuild& build, SectionMeshes& staged);
    bool CommitStagedSectionMeshes(uint64_t key, int sec, SectionMeshes& staged);
    void DestroyStagedSectionMeshes(SectionMeshes& staged);
    void DestroyChunkMeshes(uint64_t chunkKey); void DestroyAllMeshes(); void StopWorkers();
    int CreateChunk(GarrysMod::Lua::ILuaBase*); int ApplyChunk(GarrysMod::Lua::ILuaBase*); int ApplyChunkCells(GarrysMod::Lua::ILuaBase*);
    int SetCell(GarrysMod::Lua::ILuaBase*); int GetCell(GarrysMod::Lua::ILuaBase*); int GetChunkCells(GarrysMod::Lua::ILuaBase*); int Think(GarrysMod::Lua::ILuaBase*);
    int RebuildBlockLight(GarrysMod::Lua::ILuaBase*); int GetBlockLightChunk(GarrysMod::Lua::ILuaBase*); int GetSkyLightChunk(GarrysMod::Lua::ILuaBase*);
    int StartBlockLightRebuild(GarrysMod::Lua::ILuaBase*); int PollBlockLightRebuild(GarrysMod::Lua::ILuaBase*);
    int DrawChunk(GarrysMod::Lua::ILuaBase*); int UnloadChunk(GarrysMod::Lua::ILuaBase*);
    int ClearWorld(GarrysMod::Lua::ILuaBase*); int GetStats(GarrysMod::Lua::ILuaBase*);
    int Shutdown(GarrysMod::Lua::ILuaBase*); int DebugBuildSection(GarrysMod::Lua::ILuaBase*);
}
