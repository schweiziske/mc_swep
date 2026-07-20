#pragma once

#include "pch.h"
#include "global.h"
#include <windows.h>

using namespace GarrysMod::Lua;

namespace mcmesh::handshake {
	// 返回版本信息
	inline int AbiVersion(ILuaBase* LUA) {
		LUA->PushNumber(3);
		return 1;
	}

	// 获取API
	static bool AcquireMaterialSystem() {
		HMODULE dll = GetModuleHandleA("materialsystem.dll");
		if (!dll) return false;
		auto factory = (CreateInterfaceFn)GetProcAddress(dll, "CreateInterface");
		if (!factory) return false;

		g_matsys = (IMaterialSystem*)factory(MATERIAL_SYSTEM_INTERFACE_VERSION, nullptr);
		return g_matsys != nullptr;
	}

	int Handshake(ILuaBase* LUA);
	
	int SetBlockDefs(ILuaBase* LUA);
	int Think(ILuaBase* LUA);
	int DrawChunk(ILuaBase* LUA);
	int UnloadChunk(ILuaBase* LUA);
	int ClearWorld(ILuaBase* LUA);
	int Shutdown(ILuaBase* LUA);
}
