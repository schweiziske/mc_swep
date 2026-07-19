#include "pch.h"

#include "handshake.h"
#include "global.h"
#include "blob_format.h"
#include "worldstate.h"
#include "blockdefs.h"

using namespace GarrysMod::Lua;
using namespace mcmesh;

namespace {
    static thread_local uint32_t s_stateId[kCellsPerChunk];
}

namespace mcmesh::worldstate {
    int ApplyChunk(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;
        if (!ReadChunkCoords(LUA,cx,cy,cz)) { LUA->PushBool(false); return 1; }

        unsigned int len = 0;
        const char* data = LUA->GetString(4, &len);
        if (!data || len != kStateBlobBytes) {
            LUA->PushBool(false);
            return 1;
        }

        uint64_t key = PackChunkKey(cx, cy, cz);
        
        // 解码
        const uint8_t* p = (const uint8_t*)data;
        for (int li = 0; li < kCellsPerChunk; ++li) {
            s_stateId[li] = uint32_t(p[0]) | (uint32_t(p[1]) << 8)
                | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
            p += 4;
        }
        for (int li = 0; li < kCellsPerChunk; ++li) {
            if (s_stateId[li] >= blockdefs::g_states.size()) { LUA->PushBool(false); return 1; }
        }

        auto it = g_world.find(key);
        if (it != g_world.end()) {
            // 已经存在
            ChunkMirror* mirror = it->second.get();

            const bool statesSame = memcmp(s_stateId, mirror->stateId, sizeof(s_stateId)) == 0;

            if (statesSame) {
                // 两者完全相同，不需要去改
                g_status.LastApplyDirty = 0;
                LUA->PushBool(true);
                return 1;
            }
            
            // 需要修改原数据
            g_status.LastApplyDirty = 0;
            for (int li = 0; li < kCellsPerChunk; ++li)
            {
                if (mirror->stateId[li] == s_stateId[li]) continue;
                const int lx = li % g_promise.CS, ly = (li / g_promise.CS) % g_promise.CS, lz = li / (g_promise.CS * g_promise.CS);
                g_status.LastApplyDirty += MarkCellAndNeighborsDirty(cx, cy, cz, lx, ly, lz);
            }

            // 将新数据复制回mirror
            memcpy(mirror->stateId, s_stateId, sizeof(s_stateId));
        }
        else {
            // 需要创建新的mirror
            auto mirror = std::make_unique<ChunkMirror>();

            memcpy(mirror->stateId, s_stateId, sizeof(s_stateId));
            g_world.emplace(key, std::move(mirror));
            g_status.LastApplyDirty = MarkWholeChunkDirty(key)
                + MarkNeighborsOfChunkDirty(cx, cy, cz);
        }
        
        LUA->PushBool(true);
        return 1;
    }

    int UnloadChunk(ILuaBase* LUA) {
        int cx, cy, cz;
        if (!ReadChunkCoords(LUA, cx, cy, cz))
        {
            LUA->PushBool(false);
            return 1;
        }

        const uint64_t key = PackChunkKey(cx, cy, cz);
        auto it = g_world.find(key);
        if (it != g_world.end())
        {
            // M2 接入点: 在这里遍历销毁该 chunk 全部 section 的 IMesh
            g_world.erase(it);
            g_dirtyMask.erase(key);   // g_dirtyQueue 里的残留 key 不抠, Think 弹到无掩码时跳过
            MarkNeighborsOfChunkDirty(cx, cy, cz);
        }

        LUA->PushBool(true);
        return 1;
    }

    int ClearWorld(ILuaBase* LUA) {
        // M2 接入点: 先遍历销毁全部 IMesh
        g_world.clear();
        g_dirtyMask.clear();
        g_dirtyQueue.clear();
        g_status.LastApplyDirty = 0;
        return 0;
    }

    int GetStats(ILuaBase* LUA) {
        LUA->CreateTable();
        LUA->PushNumber(g_status.LastApplyDirty);
        LUA->SetField(-2, "lastApplyDirty");
        return 1;
    }
}