#include "pch.h"

#include "handshake.h"
#include "global.h"
#include "blob_format.h"
#include "worldstate.h"
#include "blockdefs.h"

#include <deque>
#include <atomic>
#include <array>
#include <algorithm>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

using namespace GarrysMod::Lua;
using namespace mcmesh;

namespace {
    static thread_local uint32_t s_stateId[kCellsPerChunk];
    struct LightAsyncChunk { uint64_t key=0,version=0;int cx=0,cy=0,cz=0;std::vector<uint32_t> states;std::vector<uint8_t> light,sky; };
    struct LightAsyncResult { uint64_t version=0,sources=0,processed=0,written=0,skySources=0,skyProcessed=0,skyWritten=0;double ms=0,skyMS=0;std::vector<LightAsyncChunk> chunks; };
    std::mutex s_lightMutex;
    std::thread s_lightThread;
    std::atomic<bool> s_lightRunning{false},s_lightReady{false},s_lightCancel{false};
    std::unique_ptr<LightAsyncResult> s_lightResult;
}

namespace mcmesh::worldstate {
    void StopBlockLightWorker() {
        s_lightCancel.store(true, std::memory_order_release);
        if (s_lightThread.joinable()) s_lightThread.join();
        std::lock_guard<std::mutex> lock(s_lightMutex);
        s_lightResult.reset();
        s_lightRunning.store(false, std::memory_order_release);
        s_lightReady.store(false, std::memory_order_release);
        s_lightCancel.store(false, std::memory_order_release);
    }

    namespace {
        constexpr int kLightRadius = 15;
        constexpr int kNoColumnTop = std::numeric_limits<int>::min();
        static constexpr int kLightOffsets[6][3] = {
            {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
        };

        struct ChangedLightChunk {
            uint64_t key = 0;
            int cx = 0, cy = 0, cz = 0, count = 0;
            std::array<uint8_t, kCellsPerChunk / 8> bits{};
        };

        struct LightChangeCollector {
            std::vector<ChangedLightChunk> chunks;
            std::unordered_map<uint64_t, size_t> byKey;
            uint64_t cellCount = 0;

            void Note(uint64_t key, int cx, int cy, int cz, int li) {
                size_t index = 0;
                const auto found = byKey.find(key);
                if (found == byKey.end()) {
                    index = chunks.size();
                    ChangedLightChunk chunk;
                    chunk.key = key; chunk.cx = cx; chunk.cy = cy; chunk.cz = cz;
                    chunks.push_back(std::move(chunk));
                    byKey.emplace(key, index);
                }
                else index = found->second;
                ChangedLightChunk& chunk = chunks[index];
                const size_t byteIndex = size_t(li) >> 3;
                const uint8_t bit = uint8_t(1u << (li & 7));
                if ((chunk.bits[byteIndex] & bit) != 0) return;
                chunk.bits[byteIndex] |= bit;
                ++chunk.count;
                ++cellCount;
            }
        };

        struct RegionBounds { int x0=0,y0=0,z0=0,x1=-1,y1=-1,z1=-1; };
        struct RegionCell {
            ChunkMirror* chunk = nullptr;
            uint64_t key = 0;
            int cx=0,cy=0,cz=0,li=0,wx=0,wy=0,wz=0;
            uint8_t oldLight = 0;
        };
        struct LightNode { int wx=0,wy=0,wz=0;uint8_t level=0; };

        int FloorDiv(int value, int divisor) {
            int quotient = value / divisor;
            const int remainder = value % divisor;
            if (remainder < 0) --quotient;
            return quotient;
        }

        bool Inside(const RegionBounds& bounds, int wx, int wy, int wz) {
            return wx >= bounds.x0 && wx <= bounds.x1
                && wy >= bounds.y0 && wy <= bounds.y1
                && wz >= bounds.z0 && wz <= bounds.z1;
        }

        bool ResolveCell(int wx, int wy, int wz, ChunkMirror*& chunk, uint64_t& key,
            int& cx, int& cy, int& cz, int& li) {
            cx = FloorDiv(wx, g_promise.CS);
            cy = FloorDiv(wy, g_promise.CS);
            cz = FloorDiv(wz, g_promise.CH);
            const int lx = wx - cx * g_promise.CS;
            const int ly = wy - cy * g_promise.CS;
            const int lz = wz - cz * g_promise.CH;
            key = PackChunkKey(cx, cy, cz);
            const auto found = g_world.find(key);
            if (found == g_world.end()) return false;
            chunk = found->second.get();
            li = lx + ly * g_promise.CS + lz * g_promise.CS * g_promise.CS;
            return li >= 0 && li < kCellsPerChunk;
        }

        uint8_t LightCost(uint32_t state) {
            if (state >= blockdefs::g_lightOpacity.size()) return 16;
            const uint8_t opacity = blockdefs::g_lightOpacity[state];
            return opacity >= 15 ? 16 : (opacity > 1 ? opacity : 1);
        }

        uint8_t LightEmission(uint32_t state) {
            return state < blockdefs::g_lightEmission.size() ? blockdefs::g_lightEmission[state] : 0;
        }

        std::vector<RegionCell> CollectRegionCells(const RegionBounds& bounds, bool sky) {
            std::vector<RegionCell> cells;
            const size_t width = size_t(std::max(0, bounds.x1 - bounds.x0 + 1));
            const size_t depth = size_t(std::max(0, bounds.y1 - bounds.y0 + 1));
            const size_t height = size_t(std::max(0, bounds.z1 - bounds.z0 + 1));
            cells.reserve(std::min<size_t>(width * depth * height, g_world.size() * size_t(kCellsPerChunk)));
            for (int wz = bounds.z0; wz <= bounds.z1; ++wz)
            for (int wy = bounds.y0; wy <= bounds.y1; ++wy)
            for (int wx = bounds.x0; wx <= bounds.x1; ++wx) {
                RegionCell cell; cell.wx=wx;cell.wy=wy;cell.wz=wz;
                if (!ResolveCell(wx,wy,wz,cell.chunk,cell.key,cell.cx,cell.cy,cell.cz,cell.li)) continue;
                cell.oldLight = sky ? cell.chunk->skyLight[cell.li] : cell.chunk->blockLight[cell.li];
                cells.push_back(cell);
            }
            return cells;
        }

        void RebuildBlockRegion(const RegionBounds& bounds, LightChangeCollector& changed) {
            std::vector<RegionCell> cells = CollectRegionCells(bounds, false);
            std::deque<LightNode> queue;
            for (const RegionCell& cell : cells) cell.chunk->blockLight[cell.li] = 0;

            for (const RegionCell& cell : cells) {
                const uint8_t cost = LightCost(cell.chunk->stateId[cell.li]);
                uint8_t level = LightEmission(cell.chunk->stateId[cell.li]);
                for (const auto& offset : kLightOffsets) {
                    const int nx=cell.wx+offset[0],ny=cell.wy+offset[1],nz=cell.wz+offset[2];
                    if (Inside(bounds,nx,ny,nz)) continue;
                    ChunkMirror* neighbor=nullptr;uint64_t key=0;int cx=0,cy=0,cz=0,li=0;
                    if (!ResolveCell(nx,ny,nz,neighbor,key,cx,cy,cz,li)) continue;
                    const int candidate = int(neighbor->blockLight[li]) - int(cost);
                    if (candidate > int(level)) level = uint8_t(candidate);
                }
                if (level > 0) {
                    cell.chunk->blockLight[cell.li] = level;
                    queue.push_back({cell.wx,cell.wy,cell.wz,level});
                }
            }

            while (!queue.empty()) {
                const LightNode node=queue.front();queue.pop_front();
                ChunkMirror* source=nullptr;uint64_t sourceKey=0;int scx=0,scy=0,scz=0,sli=0;
                if (!ResolveCell(node.wx,node.wy,node.wz,source,sourceKey,scx,scy,scz,sli)
                    || source->blockLight[sli] != node.level || node.level <= 1) continue;
                for (const auto& offset : kLightOffsets) {
                    const int nx=node.wx+offset[0],ny=node.wy+offset[1],nz=node.wz+offset[2];
                    if (!Inside(bounds,nx,ny,nz)) continue;
                    ChunkMirror* target=nullptr;uint64_t key=0;int cx=0,cy=0,cz=0,li=0;
                    if (!ResolveCell(nx,ny,nz,target,key,cx,cy,cz,li)) continue;
                    const int next = int(node.level) - int(LightCost(target->stateId[li]));
                    if (next <= 0 || target->blockLight[li] >= next) continue;
                    target->blockLight[li] = uint8_t(next);
                    queue.push_back({nx,ny,nz,uint8_t(next)});
                }
            }

            for (const RegionCell& cell : cells) {
                if (cell.oldLight != cell.chunk->blockLight[cell.li])
                    changed.Note(cell.key,cell.cx,cell.cy,cell.cz,cell.li);
            }
        }

        uint64_t ColumnKey(int wx, int wy) {
            return uint64_t(uint32_t(wx)) | (uint64_t(uint32_t(wy)) << 32);
        }

        int FindColumnTop(int wx, int wy) {
            const int cx=FloorDiv(wx,g_promise.CS),cy=FloorDiv(wy,g_promise.CS);
            const int lx=wx-cx*g_promise.CS,ly=wy-cy*g_promise.CS;
            int top=kNoColumnTop;
            for (const auto& entry : g_world) {
                int ecx=0,ecy=0,ecz=0;UnpackChunkKey(entry.first,ecx,ecy,ecz);
                if (ecx!=cx || ecy!=cy) continue;
                const ChunkMirror* chunk=entry.second.get();
                for (int lz=0;lz<g_promise.CH;++lz) {
                    const int li=lx+ly*g_promise.CS+lz*g_promise.CS*g_promise.CS;
                    const uint32_t state=chunk->stateId[li];
                    if (state>=blockdefs::g_lightOpacity.size() || blockdefs::g_lightOpacity[state]<15) continue;
                    top=std::max(top,ecz*g_promise.CH+lz);
                }
            }
            return top;
        }

        bool LoadedZBounds(int x0,int y0,int x1,int y1,int& z0,int& z1) {
            bool found=false;z0=std::numeric_limits<int>::max();z1=std::numeric_limits<int>::min();
            for (const auto& entry : g_world) {
                int cx=0,cy=0,cz=0;UnpackChunkKey(entry.first,cx,cy,cz);
                const int chunkX0=cx*g_promise.CS,chunkY0=cy*g_promise.CS;
                const int chunkX1=chunkX0+g_promise.CS-1,chunkY1=chunkY0+g_promise.CS-1;
                if (chunkX1<x0 || chunkX0>x1 || chunkY1<y0 || chunkY0>y1) continue;
                found=true;z0=std::min(z0,cz*g_promise.CH);z1=std::max(z1,cz*g_promise.CH+g_promise.CH-1);
            }
            return found;
        }

        void RebuildSkyRegion(const RegionBounds& bounds, LightChangeCollector& changed) {
            std::vector<RegionCell> cells = CollectRegionCells(bounds, true);
            std::unordered_map<uint64_t,int> tops;
            tops.reserve(size_t(bounds.x1-bounds.x0+1)*size_t(bounds.y1-bounds.y0+1));
            for (const auto& entry : g_world) {
                int cx=0,cy=0,cz=0;UnpackChunkKey(entry.first,cx,cy,cz);
                const int chunkX0=cx*g_promise.CS,chunkY0=cy*g_promise.CS;
                const int chunkX1=chunkX0+g_promise.CS-1,chunkY1=chunkY0+g_promise.CS-1;
                if (chunkX1<bounds.x0 || chunkX0>bounds.x1 || chunkY1<bounds.y0 || chunkY0>bounds.y1) continue;
                const ChunkMirror* chunk=entry.second.get();
                for (int li=0;li<kCellsPerChunk;++li) {
                    const uint32_t state=chunk->stateId[li];
                    if (state>=blockdefs::g_lightOpacity.size() || blockdefs::g_lightOpacity[state]<15) continue;
                    const int lx=li%g_promise.CS,ly=(li/g_promise.CS)%g_promise.CS,lz=li/(g_promise.CS*g_promise.CS);
                    const int wx=chunkX0+lx,wy=chunkY0+ly;
                    if (wx<bounds.x0 || wx>bounds.x1 || wy<bounds.y0 || wy>bounds.y1) continue;
                    const int wz=cz*g_promise.CH+lz;const uint64_t key=ColumnKey(wx,wy);
                    const auto old=tops.find(key);
                    if (old==tops.end() || wz>old->second) tops[key]=wz;
                }
            }
            auto exposed=[&tops](int wx,int wy,int wz){const auto found=tops.find(ColumnKey(wx,wy));return found==tops.end()||wz>found->second;};

            std::deque<LightNode> queue;
            for (const RegionCell& cell : cells) cell.chunk->skyLight[cell.li]=0;
            for (const RegionCell& cell : cells) {
                const uint8_t cost=LightCost(cell.chunk->stateId[cell.li]);
                uint8_t level=exposed(cell.wx,cell.wy,cell.wz)?15:0;
                if (level<15) for (const auto& offset:kLightOffsets) {
                    const int nx=cell.wx+offset[0],ny=cell.wy+offset[1],nz=cell.wz+offset[2];
                    if (Inside(bounds,nx,ny,nz)) continue;
                    ChunkMirror* neighbor=nullptr;uint64_t key=0;int cx=0,cy=0,cz=0,li=0;
                    if (!ResolveCell(nx,ny,nz,neighbor,key,cx,cy,cz,li)) continue;
                    const int candidate=int(neighbor->skyLight[li])-int(cost);
                    if (candidate>int(level)) level=uint8_t(candidate);
                }
                if (level>0) {cell.chunk->skyLight[cell.li]=level;queue.push_back({cell.wx,cell.wy,cell.wz,level});}
            }

            while (!queue.empty()) {
                const LightNode node=queue.front();queue.pop_front();
                ChunkMirror* source=nullptr;uint64_t sourceKey=0;int scx=0,scy=0,scz=0,sli=0;
                if (!ResolveCell(node.wx,node.wy,node.wz,source,sourceKey,scx,scy,scz,sli)
                    || source->skyLight[sli]!=node.level || node.level<=1) continue;
                for (const auto& offset:kLightOffsets) {
                    const int nx=node.wx+offset[0],ny=node.wy+offset[1],nz=node.wz+offset[2];
                    if (!Inside(bounds,nx,ny,nz)) continue;
                    ChunkMirror* target=nullptr;uint64_t key=0;int cx=0,cy=0,cz=0,li=0;
                    if (!ResolveCell(nx,ny,nz,target,key,cx,cy,cz,li)) continue;
                    const int next=int(node.level)-int(LightCost(target->stateId[li]));
                    if (next<=0 || target->skyLight[li]>=next) continue;
                    target->skyLight[li]=uint8_t(next);queue.push_back({nx,ny,nz,uint8_t(next)});
                }
            }

            for (const RegionCell& cell : cells) {
                if (cell.oldLight!=cell.chunk->skyLight[cell.li])
                    changed.Note(cell.key,cell.cx,cell.cy,cell.cz,cell.li);
            }
        }

        void PushChangedLightTable(ILuaBase* LUA,const LightChangeCollector& changed,double ms,const char* status) {
            LUA->CreateTable();
            LUA->PushString(status);LUA->SetField(-2,"status");
            LUA->PushNumber(ms);LUA->SetField(-2,"ms");
            LUA->PushNumber((double)changed.chunks.size());LUA->SetField(-2,"changedChunkCount");
            LUA->PushNumber((double)changed.cellCount);LUA->SetField(-2,"changedCellCount");
            LUA->CreateTable();int out=1;
            for (const ChangedLightChunk& chunk:changed.chunks) {
                LUA->PushNumber(out++);LUA->PushNumber(chunk.cx);LUA->SetTable(-3);
                LUA->PushNumber(out++);LUA->PushNumber(chunk.cy);LUA->SetTable(-3);
                LUA->PushNumber(out++);LUA->PushNumber(chunk.cz);LUA->SetTable(-3);
                LUA->PushNumber(out++);LUA->PushString(reinterpret_cast<const char*>(chunk.bits.data()),(unsigned int)chunk.bits.size());LUA->SetTable(-3);
            }
            LUA->SetField(-2,"changedCellChunks");
        }
    }

    static bool ApplyDenseStates(int cx, int cy, int cz, const uint32_t* states) {
        if (!states) return false;
        for (int li = 0; li < kCellsPerChunk; ++li)
            if (states[li] >= blockdefs::g_states.size()) return false;

        const uint64_t key = PackChunkKey(cx, cy, cz);
        auto it = g_world.find(key);
        if (it != g_world.end()) {
            ChunkMirror* mirror = it->second.get();
            if (mirror->version == 0) {
                memcpy(mirror->stateId, states, sizeof(mirror->stateId));
                mirror->version = 1;
                mirror->lightValid = false;
                ++g_worldVersion;
                g_status.LastApplyDirty = MarkWholeChunkDirty(key) + MarkNeighborsOfChunkDirty(cx, cy, cz);
                return true;
            }
            if (memcmp(states, mirror->stateId, sizeof(mirror->stateId)) == 0) {
                g_status.LastApplyDirty = 0;
                return true;
            }
            g_status.LastApplyDirty = 0;
            for (int li = 0; li < kCellsPerChunk; ++li) {
                if (mirror->stateId[li] == states[li]) continue;
                const int lx = li % g_promise.CS;
                const int ly = (li / g_promise.CS) % g_promise.CS;
                const int lz = li / (g_promise.CS * g_promise.CS);
                g_status.LastApplyDirty += MarkCellAndNeighborsDirty(cx, cy, cz, lx, ly, lz);
            }
            memcpy(mirror->stateId, states, sizeof(mirror->stateId));
            ++mirror->version;
            mirror->lightValid = false;
            ++g_worldVersion;
            return true;
        }

        auto mirror = std::make_unique<ChunkMirror>();
        memcpy(mirror->stateId, states, sizeof(mirror->stateId));
        mirror->version = 1;
        g_world.emplace(key, std::move(mirror));
        ++g_worldVersion;
        g_status.LastApplyDirty = MarkWholeChunkDirty(key) + MarkNeighborsOfChunkDirty(cx, cy, cz);
        return true;
    }

    int CreateChunk(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;if(!ReadChunkCoords(LUA,cx,cy,cz)){LUA->PushBool(false);return 1;}
        const uint64_t key=PackChunkKey(cx,cy,cz);
        if(g_world.find(key)==g_world.end()){
            auto mirror=std::make_unique<ChunkMirror>();
            memset(mirror->stateId,0,sizeof(mirror->stateId));
            mirror->version=0;
            g_world.emplace(key,std::move(mirror));
            ++g_worldVersion;
        }
        LUA->PushBool(true);return 1;
    }

    int ApplyChunk(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;
        if (!ReadChunkCoords(LUA,cx,cy,cz)) { LUA->PushBool(false); return 1; }

        unsigned int len = 0;
        const char* data = LUA->GetString(4, &len);
        if (!data || len != kStateBlobBytes) {
            LUA->PushBool(false);
            return 1;
        }

        // 解码
        const uint8_t* p = (const uint8_t*)data;
        for (int li = 0; li < kCellsPerChunk; ++li) {
            s_stateId[li] = uint32_t(p[0]) | (uint32_t(p[1]) << 8)
                | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
            p += 4;
        }
        LUA->PushBool(ApplyDenseStates(cx, cy, cz, s_stateId));
        return 1;
    }

    int ApplyChunkCells(ILuaBase* LUA) {
        const double t0=Plat_FloatTime();
        int cx=0,cy=0,cz=0;
        if (!ReadChunkCoords(LUA,cx,cy,cz) || !LUA->IsType(4,Type::Table)) { LUA->PushBool(false); return 1; }
        const bool replace = !LUA->IsType(5,Type::Bool) || LUA->GetBool(5);
        const auto current = g_world.find(PackChunkKey(cx,cy,cz));
        if (replace || current==g_world.end()) memset(s_stateId,0,sizeof(s_stateId));
        else memcpy(s_stateId,current->second->stateId,sizeof(s_stateId));

        const int count=LUA->ObjLen(4);
        if(count<0||(count&1)!=0||count>kCellsPerChunk*2){LUA->PushBool(false);return 1;}
        for(int i=1;i<=count;i+=2){
            LUA->PushNumber(i);LUA->RawGet(4);
            if(!LUA->IsType(-1,Type::Number)){LUA->Pop(1);LUA->PushBool(false);return 1;}
            const double liD=LUA->GetNumber(-1);LUA->Pop(1);
            LUA->PushNumber(i+1);LUA->RawGet(4);
            if(!LUA->IsType(-1,Type::Number)){LUA->Pop(1);LUA->PushBool(false);return 1;}
            const double stateD=LUA->GetNumber(-1);LUA->Pop(1);
            if(liD!=floor(liD)||liD<0||liD>=kCellsPerChunk||stateD!=floor(stateD)||stateD<0
                ||stateD>=blockdefs::g_states.size()){LUA->PushBool(false);return 1;}
            s_stateId[(int)liD]=(uint32_t)stateD;
        }
        const bool ok=ApplyDenseStates(cx,cy,cz,s_stateId);
        const double elapsed=(Plat_FloatTime()-t0)*1000.0;
        ++g_status.BatchCalls;g_status.BatchCells+=(uint64_t)(count/2);
        g_status.BatchLastMS=elapsed;g_status.BatchTotalMS+=elapsed;
        LUA->PushBool(ok);return 1;
    }

    int SetCell(ILuaBase* LUA) {
        const double t0=Plat_FloatTime();
        int cx=0,cy=0,cz=0;
        if(!ReadChunkCoords(LUA,cx,cy,cz)||!LUA->IsType(4,Type::Number)||!LUA->IsType(5,Type::Number)){
            LUA->PushBool(false);return 1;
        }
        const double liD=LUA->GetNumber(4),stateD=LUA->GetNumber(5);
        if(liD!=floor(liD)||liD<0||liD>=kCellsPerChunk||stateD!=floor(stateD)||stateD<0
            ||stateD>=blockdefs::g_states.size()){LUA->PushBool(false);return 1;}
        const int li=(int)liD;
        const uint32_t state=(uint32_t)stateD;
        const uint64_t key=PackChunkKey(cx,cy,cz);
        const int lx=li%g_promise.CS,ly=(li/g_promise.CS)%g_promise.CS,lz=li/(g_promise.CS*g_promise.CS);
        const int wx=cx*g_promise.CS+lx,wy=cy*g_promise.CS+ly,wz=cz*g_promise.CH+lz;
        auto current=g_world.find(key);
        const bool newChunk=current==g_world.end();
        const uint32_t oldState=newChunk?0:current->second->stateId[li];
        if(oldState==state){
            g_status.LastApplyDirty=0;const double us=(Plat_FloatTime()-t0)*1e6;
            ++g_status.SetCellCalls;g_status.SetCellLastUs=us;g_status.SetCellTotalMS+=us/1000.0;
            LUA->PushBool(true);return 1;
        }

        const bool lightDefsReady=blockdefs::g_lightEmission.size()==blockdefs::g_states.size()
            &&blockdefs::g_lightOpacity.size()==blockdefs::g_states.size();
        const bool updateLighting=g_lightFieldReady&&lightDefsReady;
        const int oldTop=updateLighting?FindColumnTop(wx,wy):kNoColumnTop;
        bool firstWrite=newChunk;
        if(newChunk){
            auto mirror=std::make_unique<ChunkMirror>();
            memset(mirror->stateId,0,sizeof(mirror->stateId));
            mirror->stateId[li]=state;
            mirror->version=1;
            g_world.emplace(key,std::move(mirror));
            current=g_world.find(key);
            ++g_worldVersion;
            g_status.LastApplyDirty=MarkWholeChunkDirty(key)+MarkNeighborsOfChunkDirty(cx,cy,cz);
        }
        else {
            ChunkMirror* mirror=current->second.get();
            firstWrite=mirror->version==0;
            mirror->stateId[li]=state;
            ++mirror->version;
            ++g_worldVersion;
            g_status.LastApplyDirty=firstWrite?(MarkWholeChunkDirty(key)+MarkNeighborsOfChunkDirty(cx,cy,cz))
                :MarkCellAndNeighborsDirty(cx,cy,cz,lx,ly,lz);
        }

        LightChangeCollector changed;
        double incrementalMS=0;
        if(updateLighting){
            const double lightT0=Plat_FloatTime();
            const uint8_t oldEmission=LightEmission(oldState),newEmission=LightEmission(state);
            const uint8_t oldOpacity=oldState<blockdefs::g_lightOpacity.size()?blockdefs::g_lightOpacity[oldState]:15;
            const uint8_t newOpacity=state<blockdefs::g_lightOpacity.size()?blockdefs::g_lightOpacity[state]:15;
            if(firstWrite||oldEmission!=newEmission||oldOpacity!=newOpacity){
                RegionBounds blockBounds;
                if(firstWrite){
                    blockBounds={cx*g_promise.CS-kLightRadius,cy*g_promise.CS-kLightRadius,cz*g_promise.CH-kLightRadius,
                        cx*g_promise.CS+g_promise.CS-1+kLightRadius,cy*g_promise.CS+g_promise.CS-1+kLightRadius,
                        cz*g_promise.CH+g_promise.CH-1+kLightRadius};
                }
                else blockBounds={wx-kLightRadius,wy-kLightRadius,wz-kLightRadius,wx+kLightRadius,wy+kLightRadius,wz+kLightRadius};
                RebuildBlockRegion(blockBounds,changed);
            }
            if(firstWrite||oldOpacity!=newOpacity){
                RegionBounds skyBounds;
                skyBounds.x0=firstWrite?cx*g_promise.CS-kLightRadius:wx-kLightRadius;
                skyBounds.y0=firstWrite?cy*g_promise.CS-kLightRadius:wy-kLightRadius;
                skyBounds.x1=firstWrite?cx*g_promise.CS+g_promise.CS-1+kLightRadius:wx+kLightRadius;
                skyBounds.y1=firstWrite?cy*g_promise.CS+g_promise.CS-1+kLightRadius:wy+kLightRadius;
                int loadedMin=0,loadedMax=0;
                if(LoadedZBounds(skyBounds.x0,skyBounds.y0,skyBounds.x1,skyBounds.y1,loadedMin,loadedMax)){
                    const int newTop=FindColumnTop(wx,wy);
                    if(firstWrite){skyBounds.z0=loadedMin;skyBounds.z1=loadedMax;}
                    else if(oldTop!=newTop){
                        const int low=(oldTop==kNoColumnTop||newTop==kNoColumnTop)?loadedMin:std::min(oldTop,newTop)+1;
                        const int high=(oldTop==kNoColumnTop||newTop==kNoColumnTop)?wz:std::max(oldTop,newTop);
                        skyBounds.z0=std::max(loadedMin,std::min(wz,low)-kLightRadius);
                        skyBounds.z1=std::min(loadedMax,std::max(wz,high)+kLightRadius);
                    }
                    else {skyBounds.z0=std::max(loadedMin,wz-kLightRadius);skyBounds.z1=std::min(loadedMax,wz+kLightRadius);}
                    if(skyBounds.z0<=skyBounds.z1)RebuildSkyRegion(skyBounds,changed);
                }
            }
            current->second->lightValid=true;
            incrementalMS=(Plat_FloatTime()-lightT0)*1000.0;
            ++g_status.IncrementalLightUpdates;g_status.IncrementalLightCells+=changed.cellCount;
            g_status.IncrementalLightLastMS=incrementalMS;
        }

        const double us=(Plat_FloatTime()-t0)*1e6;++g_status.SetCellCalls;
        g_status.SetCellLastUs=us;g_status.SetCellTotalMS+=us/1000.0;
        LUA->PushBool(true);
        if(updateLighting){PushChangedLightTable(LUA,changed,incrementalMS,"incremental");return 2;}
        return 1;
    }

    int GetCell(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;
        if(!ReadChunkCoords(LUA,cx,cy,cz)||!LUA->IsType(4,Type::Number)){LUA->PushNil();return 1;}
        const double liD=LUA->GetNumber(4);
        if(liD!=floor(liD)||liD<0||liD>=kCellsPerChunk){LUA->PushNil();return 1;}
        const auto it=g_world.find(PackChunkKey(cx,cy,cz));
        if(it==g_world.end()){LUA->PushNil();return 1;}
        LUA->PushNumber((double)it->second->stateId[(int)liD]);return 1;
    }

    int GetChunkCells(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;if(!ReadChunkCoords(LUA,cx,cy,cz)){LUA->PushNil();return 1;}
        const auto it=g_world.find(PackChunkKey(cx,cy,cz));if(it==g_world.end()){LUA->PushNil();return 1;}
        LUA->CreateTable();int out=1;
        for(int li=0;li<kCellsPerChunk;++li){const uint32_t state=it->second->stateId[li];if(state==0)continue;
            LUA->PushNumber(out++);LUA->PushNumber(li);LUA->SetTable(-3);
            LUA->PushNumber(out++);LUA->PushNumber((double)state);LUA->SetTable(-3);}
        return 1;
    }

    int RebuildBlockLight(ILuaBase* LUA) {
        StopBlockLightWorker();
        if(blockdefs::g_lightEmission.size()!=blockdefs::g_states.size()
            ||blockdefs::g_lightOpacity.size()!=blockdefs::g_states.size()){LUA->PushBool(false);return 1;}
        const double t0=Plat_FloatTime();
        struct Node{ChunkMirror* chunk;int cx,cy,cz,li;uint8_t level;};
        std::deque<Node> queue;
        uint64_t sources=0,processed=0,written=0;
        for(auto& kv:g_world){
            int cx,cy,cz;UnpackChunkKey(kv.first,cx,cy,cz);ChunkMirror* chunk=kv.second.get();
            memset(chunk->blockLight,0,sizeof(chunk->blockLight));
            for(int li=0;li<kCellsPerChunk;++li){const uint32_t state=chunk->stateId[li];const uint8_t level=blockdefs::g_lightEmission[state];if(!level)continue;chunk->blockLight[li]=level;queue.push_back({chunk,cx,cy,cz,li,level});++sources;}
        }
        static constexpr int off[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        while(!queue.empty()){
            const Node node=queue.front();queue.pop_front();++processed;
            if(node.chunk->blockLight[node.li]!=node.level||node.level<=1)continue;
            const int lx=node.li%g_promise.CS,ly=(node.li/g_promise.CS)%g_promise.CS,lz=node.li/(g_promise.CS*g_promise.CS);
            for(const auto& d:off){
                int ncx=node.cx,ncy=node.cy,ncz=node.cz,nlx=lx+d[0],nly=ly+d[1],nlz=lz+d[2];
                if(nlx<0){--ncx;nlx+=g_promise.CS;}else if(nlx>=g_promise.CS){++ncx;nlx-=g_promise.CS;}
                if(nly<0){--ncy;nly+=g_promise.CS;}else if(nly>=g_promise.CS){++ncy;nly-=g_promise.CS;}
                if(nlz<0){--ncz;nlz+=g_promise.CH;}else if(nlz>=g_promise.CH){++ncz;nlz-=g_promise.CH;}
                const auto it=g_world.find(PackChunkKey(ncx,ncy,ncz));if(it==g_world.end())continue;
                ChunkMirror* neighbor=it->second.get();const int nli=nlx+nly*g_promise.CS+nlz*g_promise.CS*g_promise.CS;
                const uint32_t state=neighbor->stateId[nli];const uint8_t opacity=blockdefs::g_lightOpacity[state];
                const int cost=opacity>=15?16:(opacity>1?(int)opacity:1);const int next=(int)node.level-cost;
                if(next<=0||neighbor->blockLight[nli]>=next)continue;
                neighbor->blockLight[nli]=(uint8_t)next;++written;queue.push_back({neighbor,ncx,ncy,ncz,nli,(uint8_t)next});
            }
        }
        ++g_status.LightRebuilds;g_status.LightSources=sources;g_status.LightProcessed=processed;g_status.LightWritten=written;g_status.LightLastMS=(Plat_FloatTime()-t0)*1000.0;
        LUA->CreateTable();LUA->PushNumber(g_status.LightLastMS);LUA->SetField(-2,"ms");LUA->PushNumber((double)sources);LUA->SetField(-2,"sources");LUA->PushNumber((double)processed);LUA->SetField(-2,"processed");LUA->PushNumber((double)written);LUA->SetField(-2,"written");return 1;
    }

    int GetBlockLightChunk(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;if(!ReadChunkCoords(LUA,cx,cy,cz)){LUA->PushNil();return 1;}
        const auto it=g_world.find(PackChunkKey(cx,cy,cz));if(it==g_world.end()){LUA->PushNil();return 1;}
        LUA->PushString(reinterpret_cast<const char*>(it->second->blockLight),(unsigned int)sizeof(it->second->blockLight));return 1;
    }

    int GetSkyLightChunk(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;if(!ReadChunkCoords(LUA,cx,cy,cz)){LUA->PushNil();return 1;}
        const auto it=g_world.find(PackChunkKey(cx,cy,cz));if(it==g_world.end()){LUA->PushNil();return 1;}
        LUA->PushString(reinterpret_cast<const char*>(it->second->skyLight),(unsigned int)sizeof(it->second->skyLight));return 1;
    }

    int StartBlockLightRebuild(ILuaBase* LUA) {
        if(blockdefs::g_lightEmission.size()!=blockdefs::g_states.size()
            ||blockdefs::g_lightOpacity.size()!=blockdefs::g_states.size()){LUA->PushBool(false);return 1;}
        if(s_lightRunning.load(std::memory_order_acquire)||s_lightReady.load(std::memory_order_acquire)){LUA->PushBool(false);return 1;}
        if(s_lightThread.joinable()) s_lightThread.join();

        auto work=std::make_unique<LightAsyncResult>();
        work->version=g_worldVersion;
        work->chunks.reserve(g_world.size());
        for(const auto& kv:g_world){
            LightAsyncChunk chunk;chunk.key=kv.first;chunk.version=kv.second->version;UnpackChunkKey(kv.first,chunk.cx,chunk.cy,chunk.cz);
            chunk.states.assign(kv.second->stateId,kv.second->stateId+kCellsPerChunk);
            chunk.light.assign(kCellsPerChunk,0);chunk.sky.assign(kCellsPerChunk,0);work->chunks.push_back(std::move(chunk));
        }
        const std::vector<uint8_t> emission=blockdefs::g_lightEmission;
        const std::vector<uint8_t> opacity=blockdefs::g_lightOpacity;
        s_lightCancel.store(false,std::memory_order_release);
        s_lightReady.store(false,std::memory_order_release);
        s_lightRunning.store(true,std::memory_order_release);
        s_lightThread=std::thread([work=std::move(work),emission,opacity]() mutable {
#ifdef _WIN32
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
#endif
            const double t0=Plat_FloatTime();
            struct Node{size_t chunk;int li;uint8_t level;};
            std::deque<Node> queue;
            std::unordered_map<uint64_t,size_t> byKey;byKey.reserve(work->chunks.size()*2+1);
            for(size_t i=0;i<work->chunks.size();++i)byKey.emplace(work->chunks[i].key,i);
            for(size_t ci=0;ci<work->chunks.size();++ci){auto& chunk=work->chunks[ci];
                for(int li=0;li<kCellsPerChunk;++li){const uint32_t state=chunk.states[li];
                    if(state>=emission.size())continue;const uint8_t level=emission[state];if(!level)continue;
                    chunk.light[li]=level;queue.push_back({ci,li,level});++work->sources;}}
            static constexpr int off[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
            while(!queue.empty()&&!s_lightCancel.load(std::memory_order_acquire)){
                const Node node=queue.front();queue.pop_front();++work->processed;auto& chunk=work->chunks[node.chunk];
                if(chunk.light[node.li]!=node.level||node.level<=1)continue;
                const int lx=node.li%g_promise.CS,ly=(node.li/g_promise.CS)%g_promise.CS,lz=node.li/(g_promise.CS*g_promise.CS);
                for(const auto& d:off){int ncx=chunk.cx,ncy=chunk.cy,ncz=chunk.cz,nlx=lx+d[0],nly=ly+d[1],nlz=lz+d[2];
                    if(nlx<0){--ncx;nlx+=g_promise.CS;}else if(nlx>=g_promise.CS){++ncx;nlx-=g_promise.CS;}
                    if(nly<0){--ncy;nly+=g_promise.CS;}else if(nly>=g_promise.CS){++ncy;nly-=g_promise.CS;}
                    if(nlz<0){--ncz;nlz+=g_promise.CH;}else if(nlz>=g_promise.CH){++ncz;nlz-=g_promise.CH;}
                    const auto found=byKey.find(PackChunkKey(ncx,ncy,ncz));if(found==byKey.end())continue;
                    auto& neighbor=work->chunks[found->second];const int nli=nlx+nly*g_promise.CS+nlz*g_promise.CS*g_promise.CS;
                    const uint32_t state=neighbor.states[nli];if(state>=opacity.size())continue;const uint8_t op=opacity[state];
                    const int cost=op>=15?16:(op>1?(int)op:1),next=(int)node.level-cost;
                    if(next<=0||neighbor.light[nli]>=next)continue;neighbor.light[nli]=(uint8_t)next;++work->written;
                    queue.push_back({found->second,nli,(uint8_t)next});}}
            const double skyStart=Plat_FloatTime();
            struct ColumnKey { int x,y; bool operator==(const ColumnKey& o)const{return x==o.x&&y==o.y;} };
            struct ColumnHash { size_t operator()(const ColumnKey& k)const{return (size_t(uint32_t(k.x))*0x9E3779B185EBCA87ull)^size_t(uint32_t(k.y));} };
            std::unordered_map<ColumnKey,int,ColumnHash> tops;tops.reserve(work->chunks.size()*g_promise.CS*g_promise.CS);
            for(const auto& chunk:work->chunks)for(int li=0;li<kCellsPerChunk;++li){const uint32_t state=chunk.states[li];
                if(state>=opacity.size()||opacity[state]<15)continue;const int lx=li%g_promise.CS,ly=(li/g_promise.CS)%g_promise.CS,lz=li/(g_promise.CS*g_promise.CS);
                const ColumnKey ck{chunk.cx*g_promise.CS+lx,chunk.cy*g_promise.CS+ly};const int wz=chunk.cz*g_promise.CH+lz;
                const auto old=tops.find(ck);if(old==tops.end()||wz>old->second)tops[ck]=wz;}
            auto exposed=[&tops](int wx,int wy,int wz){const auto it=tops.find({wx,wy});return it==tops.end()||wz>it->second;};
            std::deque<Node> skyQueue;
            static constexpr int lateral[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
            for(size_t ci=0;ci<work->chunks.size();++ci){auto& chunk=work->chunks[ci];for(int li=0;li<kCellsPerChunk;++li){
                const int lx=li%g_promise.CS,ly=(li/g_promise.CS)%g_promise.CS,lz=li/(g_promise.CS*g_promise.CS);
                const int wx=chunk.cx*g_promise.CS+lx,wy=chunk.cy*g_promise.CS+ly,wz=chunk.cz*g_promise.CH+lz;
                if(exposed(wx,wy,wz)){chunk.sky[li]=15;continue;}
                uint8_t level=0;const uint32_t state=chunk.states[li];const uint8_t op=state<opacity.size()?opacity[state]:15;
                const int cost=op>=15?16:(op>1?(int)op:1);
                for(const auto& d:lateral)if(exposed(wx+d[0],wy+d[1],wz)){const int candidate=15-cost;if(candidate>level)level=(uint8_t)candidate;}
                if(level){chunk.sky[li]=level;skyQueue.push_back({ci,li,level});++work->skySources;}}}
            while(!skyQueue.empty()&&!s_lightCancel.load(std::memory_order_acquire)){
                const Node node=skyQueue.front();skyQueue.pop_front();++work->skyProcessed;auto& chunk=work->chunks[node.chunk];
                if(chunk.sky[node.li]!=node.level||node.level<=1)continue;
                const int lx=node.li%g_promise.CS,ly=(node.li/g_promise.CS)%g_promise.CS,lz=node.li/(g_promise.CS*g_promise.CS);
                for(const auto& d:off){int ncx=chunk.cx,ncy=chunk.cy,ncz=chunk.cz,nlx=lx+d[0],nly=ly+d[1],nlz=lz+d[2];
                    if(nlx<0){--ncx;nlx+=g_promise.CS;}else if(nlx>=g_promise.CS){++ncx;nlx-=g_promise.CS;}
                    if(nly<0){--ncy;nly+=g_promise.CS;}else if(nly>=g_promise.CS){++ncy;nly-=g_promise.CS;}
                    if(nlz<0){--ncz;nlz+=g_promise.CH;}else if(nlz>=g_promise.CH){++ncz;nlz-=g_promise.CH;}
                    const auto found=byKey.find(PackChunkKey(ncx,ncy,ncz));if(found==byKey.end())continue;auto& neighbor=work->chunks[found->second];
                    const int nli=nlx+nly*g_promise.CS+nlz*g_promise.CS*g_promise.CS;const uint32_t state=neighbor.states[nli];const uint8_t op=state<opacity.size()?opacity[state]:15;
                    const int cost=op>=15?16:(op>1?(int)op:1),next=(int)node.level-cost;if(next<=0||neighbor.sky[nli]>=next)continue;
                    neighbor.sky[nli]=(uint8_t)next;++work->skyWritten;skyQueue.push_back({found->second,nli,(uint8_t)next});}}
            work->skyMS=(Plat_FloatTime()-skyStart)*1000.0;
            work->ms=(Plat_FloatTime()-t0)*1000.0;
            if(!s_lightCancel.load(std::memory_order_acquire)){std::lock_guard<std::mutex> lock(s_lightMutex);s_lightResult=std::move(work);s_lightReady.store(true,std::memory_order_release);}
            s_lightRunning.store(false,std::memory_order_release);
        });
        LUA->PushBool(true);return 1;
    }

    int PollBlockLightRebuild(ILuaBase* LUA) {
        if(!s_lightReady.load(std::memory_order_acquire)){
            LUA->CreateTable();LUA->PushString(s_lightRunning.load(std::memory_order_acquire)?"running":"idle");LUA->SetField(-2,"status");return 1;
        }
        if(s_lightThread.joinable())s_lightThread.join();
        std::unique_ptr<LightAsyncResult> result;{std::lock_guard<std::mutex> lock(s_lightMutex);result=std::move(s_lightResult);}
        s_lightReady.store(false,std::memory_order_release);
        if(!result){LUA->PushBool(false);return 1;}
        struct ConflictCoord { int cx=0,cy=0,cz=0; };
        std::vector<ConflictCoord> conflicts;
        std::unordered_set<uint64_t> snapshotKeys;snapshotKeys.reserve(result->chunks.size()*2+1);
        for(const auto& source:result->chunks){
            snapshotKeys.insert(source.key);const auto current=g_world.find(source.key);
            if(current==g_world.end()||current->second->version!=source.version)conflicts.push_back({source.cx,source.cy,source.cz});
        }
        for(const auto& current:g_world)if(snapshotKeys.find(current.first)==snapshotKeys.end()){
            int cx=0,cy=0,cz=0;UnpackChunkKey(current.first,cx,cy,cz);conflicts.push_back({cx,cy,cz});
        }
        auto conflictsWith=[&conflicts](const LightAsyncChunk& source){
            for(const auto& conflict:conflicts)
                if(std::abs(source.cx-conflict.cx)<=1&&std::abs(source.cy-conflict.cy)<=1)return true;
            return false;
        };

        LightChangeCollector changed;uint64_t committedChunks=0,deferredChunks=0;
        for(auto& source:result->chunks){
            const auto current=g_world.find(source.key);
            if(current==g_world.end()||conflictsWith(source)){++deferredChunks;continue;}
            ChunkMirror* target=current->second.get();
            for(int li=0;li<kCellsPerChunk;++li)if(target->blockLight[li]!=source.light[li]||target->skyLight[li]!=source.sky[li])
                changed.Note(source.key,source.cx,source.cy,source.cz,li);
            memcpy(target->blockLight,source.light.data(),kCellsPerChunk);
            memcpy(target->skyLight,source.sky.data(),kCellsPerChunk);
            target->lightValid=true;++committedChunks;
        }
        const bool complete=deferredChunks==0;
        if(complete)g_lightFieldReady=true;
        else {++g_status.AsyncPartialCommits;g_status.AsyncDeferredChunks+=deferredChunks;}
        ++g_status.LightRebuilds;g_status.LightSources=result->sources;g_status.LightProcessed=result->processed;g_status.LightWritten=result->written;g_status.LightLastMS=result->ms;
        g_status.SkySources=result->skySources;g_status.SkyProcessed=result->skyProcessed;g_status.SkyWritten=result->skyWritten;g_status.SkyLastMS=result->skyMS;
        PushChangedLightTable(LUA,changed,result->ms,complete?"committed":"partial");
        LUA->PushNumber((double)result->sources);LUA->SetField(-2,"sources");LUA->PushNumber((double)result->processed);LUA->SetField(-2,"processed");LUA->PushNumber((double)result->written);LUA->SetField(-2,"written");
        LUA->PushNumber(result->skyMS);LUA->SetField(-2,"skyMS");LUA->PushNumber((double)result->skySources);LUA->SetField(-2,"skySources");LUA->PushNumber((double)result->skyProcessed);LUA->SetField(-2,"skyProcessed");LUA->PushNumber((double)result->skyWritten);LUA->SetField(-2,"skyWritten");
        LUA->PushNumber((double)committedChunks);LUA->SetField(-2,"committedChunkCount");
        LUA->PushNumber((double)deferredChunks);LUA->SetField(-2,"deferredChunkCount");return 1;
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
            ++g_worldVersion;
            g_dirtyMask.erase(key);   // g_dirtyQueue 里的残留 key 不抠, Think 弹到无掩码时跳过
            MarkNeighborsOfChunkDirty(cx, cy, cz);
        }

        LUA->PushBool(true);
        return 1;
    }

    int ClearWorld(ILuaBase* LUA) {
        StopBlockLightWorker();
        // M2 接入点: 先遍历销毁全部 IMesh
        g_world.clear();
        g_dirtyMask.clear();
        g_dirtyQueue.clear();
        g_status.LastApplyDirty = 0;
        g_lightFieldReady = false;
        ++g_worldVersion;
        return 0;
    }

    int GetStats(ILuaBase* LUA) {
        LUA->CreateTable();
        LUA->PushNumber(g_status.LastApplyDirty);
        LUA->SetField(-2, "lastApplyDirty");
        return 1;
    }
}
