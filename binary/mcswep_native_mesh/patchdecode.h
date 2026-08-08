#pragma once

#include "pch.h"

namespace mcmesh::patchdecode {
    int StartCompressedPatchDecode(GarrysMod::Lua::ILuaBase* LUA);
    int PollCompressedPatchDecode(GarrysMod::Lua::ILuaBase* LUA);
    int CommitCompressedPatch(GarrysMod::Lua::ILuaBase* LUA);
    int StartBinarySectionPatchDecode(GarrysMod::Lua::ILuaBase* LUA);
    int PollBinarySectionPatchDecode(GarrysMod::Lua::ILuaBase* LUA);
    int CommitBinarySectionPatch(GarrysMod::Lua::ILuaBase* LUA);
    int CancelCompressedPatchDecode(GarrysMod::Lua::ILuaBase* LUA);
    int CancelAllCompressedPatchDecodes(GarrysMod::Lua::ILuaBase* LUA);

    void CancelAll();
    void Shutdown();
}
