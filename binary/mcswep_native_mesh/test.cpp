#include "pch.h"
#include "test.h"
#include <array>
#include <vector>
#include "global.h"
#include "worldstate.h"
using namespace mcmesh::worldstate;

using namespace mcmesh;

namespace mcmesh::Test {
    int DrawChunk(ILuaBase* LUA) {
        if (!g_mesh) {
            IMaterial* mat = mcmesh::g_matsys->FindMaterial("!mc_atlas_gpu_lighttex_fancy13_localdeps_vface_opaque", TEXTURE_GROUP_OTHER);
            BuildSpikeMesh(mat);
        }
        if (g_mesh) g_mesh->Draw();
        LUA->PushBool(g_mesh != nullptr);
        return 1;
    }

    int mcmesh_SpikeQuad(ILuaBase* LUA) {
        LUA->CheckType(1, GarrysMod::Lua::Type::Table);
        int n = (int)LUA->ObjLen(1);
        if (n % 14 != 0 || n == 0 || n > 14 * 600) { LUA->PushBool(false); return 1; }
        std::vector<float> v(n);
        for (int i = 1; i <= n; i++) {
            LUA->PushNumber(i); LUA->GetTable(1);
            v[i - 1] = (float)LUA->GetNumber(-1); LUA->Pop();
        }
        IMaterial* mat = g_matsys->FindMaterial("!mc_atlas_gpu_lighttex_fancy13_localdeps_vface_opaque", TEXTURE_GROUP_OTHER);
        RebuildSpikeMeshFromVerts(mat, v.data(), n / 14);   // CMeshBuilder 逐顶点:
        // Position3f / Normal3f / TexCoord2f(0,..) / TexCoord2f(1,..) / Color4ub / AdvanceVertex
        if (g_mesh) g_mesh->Draw();
        LUA->PushBool(g_mesh != nullptr);
        return 1;
    }

    int DebugGetCell(ILuaBase* LUA) {
        if (!LUA->IsType(1, GarrysMod::Lua::Type::Number)
            || !LUA->IsType(2, GarrysMod::Lua::Type::Number)
            || !LUA->IsType(3, GarrysMod::Lua::Type::Number)
            || !LUA->IsType(4, GarrysMod::Lua::Type::Number))
            return 0;

        const double cx = LUA->GetNumber(1);
        const double cy = LUA->GetNumber(2);
        const double cz = LUA->GetNumber(3);
        const double liD = LUA->GetNumber(4);

        if (!ChunkCoordValid(cx) || !ChunkCoordValid(cy) || !ChunkCoordValid(cz))
            return 0;
        // cells = 握手时算好的 cs*cs*ch(默认 8192), 别写死
        if (liD != floor(liD) || liD < 0 || liD >= double(g_promise.CPC))
            return 0;

        auto it = g_world.find(PackChunkKey(int(cx), int(cy), int(cz)));
        if (it == g_world.end())
            return 0;

        const int li = int(liD);
        LUA->PushNumber(it->second->stateId[li]);
        return 1;
    }

    int mcmesh_DebugClearDirty(ILuaBase* LUA)
    {
        g_dirtyMask.clear();
        g_dirtyQueue.clear();
        return 0;
    }
}