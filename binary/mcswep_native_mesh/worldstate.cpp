#include "pch.h"

#include "handshake.h"
#include "global.h"
#include "blob_format.h"
#include "worldstate.h"
#include "blockdefs.h"

#include <deque>
#include <atomic>
#include <array>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace GarrysMod::Lua;
using namespace mcmesh;

namespace {
    static thread_local uint32_t s_stateId[kCellsPerChunk];
    struct LightAsyncChunk { uint64_t key=0;int cx=0,cy=0,cz=0;std::vector<uint32_t> states;std::vector<uint8_t> light,sky; };
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
        const auto current=g_world.find(key);
        if(current==g_world.end()){
            auto mirror=std::make_unique<ChunkMirror>();
            memset(mirror->stateId,0,sizeof(mirror->stateId));
            mirror->stateId[li]=state;
            mirror->version=1;
            g_world.emplace(key,std::move(mirror));
            ++g_worldVersion;
            g_status.LastApplyDirty=MarkWholeChunkDirty(key)+MarkNeighborsOfChunkDirty(cx,cy,cz);
            const double us=(Plat_FloatTime()-t0)*1e6;++g_status.SetCellCalls;g_status.SetCellLastUs=us;g_status.SetCellTotalMS+=us/1000.0;
            LUA->PushBool(true);return 1;
        }
        ChunkMirror* mirror=current->second.get();
        if(mirror->stateId[li]==state){g_status.LastApplyDirty=0;const double us=(Plat_FloatTime()-t0)*1e6;++g_status.SetCellCalls;g_status.SetCellLastUs=us;g_status.SetCellTotalMS+=us/1000.0;LUA->PushBool(true);return 1;}
        const int lx=li%g_promise.CS,ly=(li/g_promise.CS)%g_promise.CS,lz=li/(g_promise.CS*g_promise.CS);
        mirror->stateId[li]=state;
        const bool firstWrite=mirror->version==0;
        ++mirror->version;
        ++g_worldVersion;
        g_status.LastApplyDirty=firstWrite?(MarkWholeChunkDirty(key)+MarkNeighborsOfChunkDirty(cx,cy,cz))
            :MarkCellAndNeighborsDirty(cx,cy,cz,lx,ly,lz);
        const double us=(Plat_FloatTime()-t0)*1e6;++g_status.SetCellCalls;g_status.SetCellLastUs=us;g_status.SetCellTotalMS+=us/1000.0;
        LUA->PushBool(true);return 1;
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
            LightAsyncChunk chunk;chunk.key=kv.first;UnpackChunkKey(kv.first,chunk.cx,chunk.cy,chunk.cz);
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
        const bool stale=result->version!=g_worldVersion;
        struct ChangedLightChunk { int cx=0,cy=0,cz=0,count=0;std::array<uint8_t,kCellsPerChunk/8> bits{}; };
        std::vector<ChangedLightChunk> changedChunks;uint64_t changedCellCount=0;
        if(!stale)for(auto& source:result->chunks){const auto it=g_world.find(source.key);if(it!=g_world.end()){
            ChangedLightChunk changed;changed.cx=source.cx;changed.cy=source.cy;changed.cz=source.cz;
            for(int li=0;li<kCellsPerChunk;++li)if(it->second->blockLight[li]!=source.light[li]||it->second->skyLight[li]!=source.sky[li]){
                changed.bits[(size_t)li>>3]|=(uint8_t)(1u<<(li&7));++changed.count;}
            if(changed.count){changedCellCount+=(uint64_t)changed.count;changedChunks.push_back(std::move(changed));}
            memcpy(it->second->blockLight,source.light.data(),kCellsPerChunk);memcpy(it->second->skyLight,source.sky.data(),kCellsPerChunk);}}
        if(!stale){++g_status.LightRebuilds;g_status.LightSources=result->sources;g_status.LightProcessed=result->processed;g_status.LightWritten=result->written;g_status.LightLastMS=result->ms;
            g_status.SkySources=result->skySources;g_status.SkyProcessed=result->skyProcessed;g_status.SkyWritten=result->skyWritten;g_status.SkyLastMS=result->skyMS;}
        LUA->CreateTable();LUA->PushString(stale?"stale":"committed");LUA->SetField(-2,"status");LUA->PushNumber(result->ms);LUA->SetField(-2,"ms");
        LUA->PushNumber((double)result->sources);LUA->SetField(-2,"sources");LUA->PushNumber((double)result->processed);LUA->SetField(-2,"processed");LUA->PushNumber((double)result->written);LUA->SetField(-2,"written");
        LUA->PushNumber(result->skyMS);LUA->SetField(-2,"skyMS");LUA->PushNumber((double)result->skySources);LUA->SetField(-2,"skySources");LUA->PushNumber((double)result->skyProcessed);LUA->SetField(-2,"skyProcessed");LUA->PushNumber((double)result->skyWritten);LUA->SetField(-2,"skyWritten");
        LUA->PushNumber((double)changedChunks.size());LUA->SetField(-2,"changedChunkCount");LUA->PushNumber((double)changedCellCount);LUA->SetField(-2,"changedCellCount");
        LUA->CreateTable();int out=1;for(const auto& c:changedChunks){
            LUA->PushNumber(out++);LUA->PushNumber(c.cx);LUA->SetTable(-3);LUA->PushNumber(out++);LUA->PushNumber(c.cy);LUA->SetTable(-3);LUA->PushNumber(out++);LUA->PushNumber(c.cz);LUA->SetTable(-3);
            LUA->PushNumber(out++);LUA->PushString(reinterpret_cast<const char*>(c.bits.data()),(unsigned int)c.bits.size());LUA->SetTable(-3);}
        LUA->SetField(-2,"changedCellChunks");return 1;
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
