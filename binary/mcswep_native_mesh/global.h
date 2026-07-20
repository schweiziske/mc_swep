#pragma once

#include "pch.h"

using namespace GarrysMod::Lua;

namespace mcmesh {
	// 全局常量
	constexpr int    kCS = 16;
	constexpr int    kCH = 32;
	constexpr double kBS = 36.5;
	constexpr int    kCellsPerChunk = kCS * kCS * kCH;
	constexpr int    kStateBlobBytes = kCellsPerChunk * 4;

	// 约定信息结构体
	struct Promise
	{
		// chunk 水平尺寸 (x/y)
		int CS;
		// CH, chunk 垂直尺寸 (z)
		int CH;
		// Section size
		int SS;
		// Block size
		double BS;
		// CS / SS
		int NSX;
		// CH / SS
		int NSZ;
		// 每个Chunk的Cell数量
		int CPC;
		// 每个Chunk的Section数量
		int SPC;
		// 不透明材质
		IMaterial* MatOpaque;
		// 半透明材质
		IMaterial* MatTranslucent;
	};

	inline IMaterialSystem* g_matsys = nullptr;
	inline IMesh* g_mesh = nullptr;
	inline Promise g_promise;

	// 现行 GPU 材质(mc_gpu_chunk_lighttex_vs30)顶点输入只有 pos + uv0;
	// normal/color/uv1 按 14-float 契约照写,格式缺失分量时 CMeshBuilder 写入为
	// 无害空操作,不得作为材质格式的硬性要求(见 INTERFACE.md spike 记录)。
	inline bool SupportsNativeMeshVertexFormat(IMaterial* material)
	{
		if (!material || material->IsErrorMaterial()) return false;
		const VertexFormat_t format = material->GetVertexFormat() & ~VERTEX_FORMAT_COMPRESSED;
		return (VertexFlags(format) & VERTEX_POSITION) != 0
			&& TexCoordSize(0, format) >= 2;
	}
}
