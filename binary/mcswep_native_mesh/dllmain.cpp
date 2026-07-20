#include "pch.h"
#include "test.h"
#include "handshake.h"
#include "worldstate.h"
#include "blockdefs.h"
#include "meshbuild.h"

using namespace GarrysMod::Lua;
using namespace std;

// 简化匿名函数
#define GTLUAF(func) \
    [](lua_State* L) -> int { \
        ILuaBase* LUA = L->luabase; \
        return func(LUA); \
    }

// 用于辅助添加函数。只能在初始化时使用。
// 注：该类必须先手动push全局表，最后手动弹出全局表。
class FunctionAdder {
	ILuaBase* L;
	const char* name;
public:
	FunctionAdder(ILuaBase* LUA) {
		L = LUA;
	}

	// 从某个表开始
	void Start(const char* table) {
		name = table;
		L->CreateTable();
		// 此时栈顶为特定名称的表
	}

	// 添加函数
	void AddFunction(const char* name, CFunc f) {
		L->PushString(name);
		L->PushCFunction(f);
		L->SetTable(-3);
		// 这里会弹出键和值
	}

	// 添加常量（或枚举）
	void AddConstant(const char* name, double number) {
		L->PushNumber(number);
		L->SetField(-2, name);
	}

	void End() {
		// 弹出这个表
		L->SetField(-2, name);
	}
};

GMOD_MODULE_OPEN()
{
	if (!mcmesh::handshake::AcquireMaterialSystem()) {
		LUA->ThrowError("mcmesh: materialsystem acquire failed");
		return 0;
	}
	
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);

	FunctionAdder fa(LUA);
	fa.Start("mcmesh");

	// ---- 握手 / 生命周期 ----
	fa.AddFunction("AbiVersion", GTLUAF(mcmesh::handshake::AbiVersion));
	fa.AddFunction("Handshake", GTLUAF(mcmesh::handshake::Handshake));

	// ---- 世界数据流入(worldstate 持有镜像 + diff; meshbuild 加握手守卫)----
	fa.AddFunction("CreateChunk", GTLUAF(mcmesh::meshbuild::CreateChunk));
	fa.AddFunction("ApplyChunk", GTLUAF(mcmesh::meshbuild::ApplyChunk));
	fa.AddFunction("ApplyChunkCells", GTLUAF(mcmesh::meshbuild::ApplyChunkCells));
	fa.AddFunction("SetCellsBatch", GTLUAF(mcmesh::meshbuild::ApplyChunkCells));
	fa.AddFunction("SetCell", GTLUAF(mcmesh::meshbuild::SetCell));
	fa.AddFunction("GetCell", GTLUAF(mcmesh::meshbuild::GetCell));
	fa.AddFunction("GetChunkCells", GTLUAF(mcmesh::meshbuild::GetChunkCells));
	fa.AddFunction("RebuildBlockLight", GTLUAF(mcmesh::meshbuild::RebuildBlockLight));
	fa.AddFunction("StartBlockLightRebuild", GTLUAF(mcmesh::meshbuild::StartBlockLightRebuild));
	fa.AddFunction("PollBlockLightRebuild", GTLUAF(mcmesh::meshbuild::PollBlockLightRebuild));
	fa.AddFunction("GetBlockLightChunk", GTLUAF(mcmesh::meshbuild::GetBlockLightChunk));
	fa.AddFunction("GetSkyLightChunk", GTLUAF(mcmesh::meshbuild::GetSkyLightChunk));

	// ---- 方块静态数据 ----
	fa.AddFunction("SetBlockDefs", GTLUAF(mcmesh::blockdefs::SetBlockDefs));
	fa.AddFunction("SetLightDefs", GTLUAF(mcmesh::blockdefs::SetLightDefs));

	// ---- 建面 / 帧循环 / 绘制(meshbuild; Unload/Clear 先毁 mesh 再转调 worldstate)----
	fa.AddFunction("Think", GTLUAF(mcmesh::meshbuild::Think));
	fa.AddFunction("DrawChunk", GTLUAF(mcmesh::meshbuild::DrawChunk));
	fa.AddFunction("UnloadChunk", GTLUAF(mcmesh::meshbuild::UnloadChunk));
	fa.AddFunction("ClearWorld", GTLUAF(mcmesh::meshbuild::ClearWorld));
	fa.AddFunction("Shutdown", GTLUAF(mcmesh::meshbuild::Shutdown));
	fa.AddFunction("GetStats", GTLUAF(mcmesh::meshbuild::GetStats));

	// ---- 调试口(正式功能, 不删)----
	fa.AddFunction("DebugGetCell", GTLUAF(mcmesh::Test::DebugGetCell));
	fa.AddFunction("DebugClearDirty", GTLUAF(mcmesh::Test::mcmesh_DebugClearDirty));
	fa.AddFunction("DebugBlockDef", GTLUAF(mcmesh::blockdefs::DebugBlockDef));
	fa.AddFunction("DebugStateDef", GTLUAF(mcmesh::blockdefs::DebugStateDef));
	fa.AddFunction("DebugBuildSection", GTLUAF(mcmesh::meshbuild::DebugBuildSection));
	fa.AddFunction("mcmesh_SpikeQuad", GTLUAF(mcmesh::Test::mcmesh_SpikeQuad));

	fa.End();

	// 弹出全局表
	LUA->Pop(1);

	return 0;
}

GMOD_MODULE_CLOSE()
{
	// Lua Shutdown hook 不保证在模块卸载前必达；DLL close 是最终线程回收屏障。
	// 引擎资源必须在线程完全停止后释放，避免 worker 仍持有建面数据。
	mcmesh::meshbuild::StopWorkers();
	mcmesh::worldstate::StopBlockLightWorker();
	mcmesh::meshbuild::DestroyAllMeshes();
	return 0;
}
