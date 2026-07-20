#pragma once

#include "pch.h"
#include "global.h"

#include <unordered_map>
#include <memory>

namespace mcmesh::worldstate {
    struct ChunkMirror {
        uint32_t stateId[mcmesh::kCellsPerChunk];
        uint8_t blockLight[mcmesh::kCellsPerChunk]{};
        uint8_t skyLight[mcmesh::kCellsPerChunk]{};
        uint64_t version = 0;
        bool lightValid = false;
    };

    struct Status {
        // 上一次标脏数量
        int LastApplyDirty;
        uint64_t BatchCalls = 0, BatchCells = 0, SetCellCalls = 0;
        double BatchLastMS = 0, BatchTotalMS = 0, SetCellLastUs = 0, SetCellTotalMS = 0;
        uint64_t LightRebuilds = 0, LightSources = 0, LightProcessed = 0, LightWritten = 0;
        uint64_t SkySources = 0, SkyProcessed = 0, SkyWritten = 0;
        double LightLastMS = 0, SkyLastMS = 0;
        uint64_t IncrementalLightUpdates = 0, IncrementalLightCells = 0;
        uint64_t AsyncPartialCommits = 0, AsyncDeferredChunks = 0;
        double IncrementalLightLastMS = 0;
    };

    inline std::unordered_map<uint64_t, std::unique_ptr<ChunkMirror>> g_world;
    inline uint64_t g_worldVersion = 0;
    inline bool g_lightFieldReady = false;
    inline std::unordered_map<uint64_t, uint64_t> g_dirtyMask;  // chunkKey -> section 位掩码
    inline std::vector<uint64_t> g_dirtyQueue;                  // 入过队的 chunkKey, FIFO
    inline Status g_status;

    // ---- chunk key: 每轴 ±1M 范围, 21bit 偏移编码打包 u64 ----
    inline uint64_t PackChunkKey(int cx, int cy, int cz) {
        // 极致的内存优化 将3个坐标全部压缩到一个unsigned long long
        // 先加一个超大整数，防止负数问题，然后进行截断，将其余位置让给其他的坐标
        const uint64_t bx = uint64_t(uint32_t(cx + (1 << 20))) & 0x1FFFFF;
        const uint64_t by = uint64_t(uint32_t(cy + (1 << 20))) & 0x1FFFFF;
        const uint64_t bz = uint64_t(uint32_t(cz + (1 << 20))) & 0x1FFFFF;
        return bx | (by << 21) | (bz << 42);
    }

    // 解包Key
    inline void UnpackChunkKey(uint64_t key, int& cx, int& cy, int& cz)
    {
        cx = int(key & 0x1FFFFF) - (1 << 20);
        cy = int((key >> 21) & 0x1FFFFF) - (1 << 20);
        cz = int((key >> 42) & 0x1FFFFF) - (1 << 20);
    }

    // 检查
    inline bool ChunkCoordValid(double c) {   // 敌意输入: 先验范围再打包
        return c == floor(c) && c >= -1000000 && c <= 1000000;
    }

    // 前 3 个栈位读 chunk 坐标, 敌意输入校验; 失败返回 false 不动任何状态
    static bool ReadChunkCoords(GarrysMod::Lua::ILuaBase* LUA, int& cx, int& cy, int& cz)
    {
        for (int i = 1; i <= 3; ++i)
            if (!LUA->IsType(i, GarrysMod::Lua::Type::Number)) return false;
        const double x = LUA->GetNumber(1), y = LUA->GetNumber(2), z = LUA->GetNumber(3);
        if (!ChunkCoordValid(x) || !ChunkCoordValid(y) || !ChunkCoordValid(z)) return false;
        cx = int(x); cy = int(y); cz = int(z);
        return true;
    }

    // 将方块在chunk的局部坐标转换为所在的Section的Index
    inline int SectionIndex(int lx, int ly, int lz) {
        return (lx / g_promise.SS) + (ly / g_promise.SS) * g_promise.NSX + (lz / g_promise.SS) * g_promise.NSX * g_promise.NSX;
    }

    // 返回"是否新置位", ApplyChunk 用它累计 lastApplyDirty
    static bool MarkSectionDirty(uint64_t chunkKey, int sx, int sy, int sz)
    {
        auto g_nsx = g_promise.NSX;
        const int idx = sx + sy * g_nsx + sz * g_nsx * g_nsx;
        uint64_t& mask = g_dirtyMask[chunkKey];
        const uint64_t bit = 1ull << idx;
        if (mask & bit) return false;
        if (mask == 0) g_dirtyQueue.push_back(chunkKey);   // 掩码 0->非0 才入队
        mask |= bit;
        return true;
    }

    static int MarkCellAndNeighborsDirty(int cx, int cy, int cz, int lx, int ly, int lz)
    {
        const int ss = g_promise.SS, cs = g_promise.CS, ch = g_promise.CH;
        const int baseWX = cx * cs + lx, baseWY = cy * cs + ly, baseWZ = cz * ch + lz;
        int added = 0;
        // Conservative Chebyshev radius two. Mapping each influenced world cell
        // to a section naturally deduplicates within and across chunk borders.
        for (int dz=-2; dz<=2; ++dz) for (int dy=-2; dy<=2; ++dy) for (int dx=-2; dx<=2; ++dx) {
            int wx=baseWX+dx, wy=baseWY+dy, wz=baseWZ+dz;
            int ncx=int(std::floor(double(wx)/cs)), ncy=int(std::floor(double(wy)/cs)), ncz=int(std::floor(double(wz)/ch));
            int nlx=wx-ncx*cs, nly=wy-ncy*cs, nlz=wz-ncz*ch;
            const uint64_t key=PackChunkKey(ncx,ncy,ncz);
            if (g_world.find(key)==g_world.end()) continue;
            if (MarkSectionDirty(key,nlx/ss,nly/ss,nlz/ss)) ++added;
        }
        return added;
    }

    static inline int PopCount64(uint64_t v)   // 手写, 避开 win32 没有 __popcnt64 的坑
    {
        int n = 0;
        while (v) { v &= v - 1; ++n; }
        return n;
    }

    static int MarkWholeChunkDirty(uint64_t chunkKey)
    {
        const uint64_t all = (g_promise.SPC >= 64)
            ? ~0ull : ((1ull << g_promise.SPC) - 1);
        uint64_t& mask = g_dirtyMask[chunkKey];
        if (mask == 0 && all != 0) g_dirtyQueue.push_back(chunkKey);
        const int added = PopCount64(all & ~mask);
        mask |= all;
        return added;
    }

    // 邻 chunk 面向 (指定轴 sideSection 那一层) 的整排 section 标脏。
    // 只对已镜像的邻居生效; 返回新增脏位数。
    static int MarkNeighborFacePlaneDirty(int ncx, int ncy, int ncz, int axis, int sideSection)
    {
        const uint64_t nkey = PackChunkKey(ncx, ncy, ncz);
        if (g_world.find(nkey) == g_world.end()) return 0;
        int added = 0;
        if (axis == 0) {
            for (int sy = 0; sy < g_promise.NSX; ++sy)
                for (int sz = 0; sz < g_promise.NSZ; ++sz)
                    if (MarkSectionDirty(nkey, sideSection, sy, sz)) added++;
        }
        else if (axis == 1) {
            for (int sx = 0; sx < g_promise.NSX; ++sx)
                for (int sz = 0; sz < g_promise.NSZ; ++sz)
                    if (MarkSectionDirty(nkey, sx, sideSection, sz)) added++;
        }
        else {
            for (int sx = 0; sx < g_promise.NSX; ++sx)
                for (int sy = 0; sy < g_promise.NSX; ++sy)
                    if (MarkSectionDirty(nkey, sx, sy, sideSection)) added++;
        }
        return added;
    }

    // Chunk arrival/removal affects every loaded neighbour whose section bounds
    // intersect the two-cell dependency halo, including edges and corners.
    static int MarkNeighborsOfChunkDirty(int cx, int cy, int cz)
    {
        int added = 0;
        for (int dz=-1; dz<=1; ++dz) for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
            if (!dx && !dy && !dz) continue;
            const uint64_t key=PackChunkKey(cx+dx,cy+dy,cz+dz);
            if (g_world.find(key)==g_world.end()) continue;
            const int sx0=dx>0?0:(dx<0?g_promise.NSX-1:0), sx1=dx? sx0:g_promise.NSX-1;
            const int sy0=dy>0?0:(dy<0?g_promise.NSX-1:0), sy1=dy? sy0:g_promise.NSX-1;
            const int sz0=dz>0?0:(dz<0?g_promise.NSZ-1:0), sz1=dz? sz0:g_promise.NSZ-1;
            for(int sz=sz0;sz<=sz1;++sz)for(int sy=sy0;sy<=sy1;++sy)for(int sx=sx0;sx<=sx1;++sx)
                if(MarkSectionDirty(key,sx,sy,sz))++added;
        }
        return added;
    }

    int ApplyChunk(ILuaBase* LUA);
    int CreateChunk(ILuaBase* LUA);
    int ApplyChunkCells(ILuaBase* LUA);
    int SetCell(ILuaBase* LUA);
    int GetCell(ILuaBase* LUA);
    int GetChunkCells(ILuaBase* LUA);
    int RebuildBlockLight(ILuaBase* LUA);
    int GetBlockLightChunk(ILuaBase* LUA);
    int GetSkyLightChunk(ILuaBase* LUA);
    int StartBlockLightRebuild(ILuaBase* LUA);
    int PollBlockLightRebuild(ILuaBase* LUA);
    void StopBlockLightWorker();
    int UnloadChunk(ILuaBase* LUA);
    int ClearWorld(ILuaBase* LUA);

    int GetStats(ILuaBase* LUA);
}
