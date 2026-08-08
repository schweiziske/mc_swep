#pragma once

#include "pch.h"

#include <cstdint>
#include <vector>

namespace mcmesh::lightatlas {
    constexpr int kAtlasSize = 4096;
    constexpr int kTileWidth = 324;
    constexpr int kTileHeight = 9;
    constexpr int kTilesPerRow = 12;
    constexpr int kTileRows = 455;
    constexpr int kSlotCount = kTilesPerRow * kTileRows;
    constexpr int kRealCapacity = kSlotCount - 1;

    struct SlotRef {
        uint16_t slot = 0;
        uint64_t generation = 0;
        bool fallback = true;
    };

    bool Initialize();
    bool BindMaterial(const char* materialName);

    bool EnsureChunkSlot(uint64_t chunkKey, SlotRef& out);
    bool GetChunkSlot(uint64_t chunkKey, SlotRef& out);
    bool SlotMatches(uint64_t chunkKey, uint16_t slot, uint64_t generation);
    bool EnsureChunkTileUploaded(uint64_t chunkKey, uint16_t slot, uint64_t generation);

    bool RefreshChangedChunks(const std::vector<uint64_t>& changedChunkKeys);
    bool RefreshAllAssigned();
    void MarkChunkNeighborhoodDirty(uint64_t chunkKey);
    void ReleaseChunk(uint64_t chunkKey);
    void ClearWorld();
    void Shutdown();

    int BindLightAtlasMaterial(GarrysMod::Lua::ILuaBase* LUA);
    int GetLightAtlasStats(GarrysMod::Lua::ILuaBase* LUA);
    int GetLightAtlasSlot(GarrysMod::Lua::ILuaBase* LUA);
}
