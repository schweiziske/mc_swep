#pragma once

#include "pch.h"
#include <windows.h>
#include "global.h"

using namespace GarrysMod::Lua;

namespace mcmesh::Test {
    inline IMesh* g_mesh = nullptr;

    static bool AcquireMaterialSystem() {
        HMODULE dll = GetModuleHandleA("materialsystem.dll");
        if (!dll) return false;
        auto factory = (CreateInterfaceFn)GetProcAddress(dll, "CreateInterface");
        if (!factory) return false;

        g_matsys = (IMaterialSystem*)factory(MATERIAL_SYSTEM_INTERFACE_VERSION, nullptr);
        return g_matsys != nullptr;
    }

    static void BuildSpikeMesh(IMaterial* mat) {
        IMatRenderContext* ctx = g_matsys->GetRenderContext();
        // 2013 系 SDK 里 CreateStaticMesh 在渲染上下文上; 若你的头文件里它在
        // IMaterialSystem 上, 以头文件为准
        VertexFormat_t fmt = mat->GetVertexFormat();

        g_mesh = ctx->CreateStaticMesh(fmt & ~VERTEX_FORMAT_COMPRESSED, TEXTURE_GROUP_STATIC_VERTEX_BUFFER_OTHER, mat);
        if (!g_mesh) return;

        CMeshBuilder mb;
        mb.Begin(g_mesh, MATERIAL_TRIANGLES, 2);   // 2 个三角形 = 正反各一, 让剔除藏不住它
        const float P[3][3] = { {0,0,0}, {0,2000,0}, {0,0,2000} };  // 大到不可能看漏
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < 3; i++) {
                int v = pass == 0 ? i : 2 - i;     // 第二遍反绕序
                mb.Position3f(P[v][0], P[v][1], P[v][2]);
                mb.Normal3f(1, 0, 0);
                mb.TexCoord2f(0, 0.5f, 0.5f);
                mb.TexCoord2f(1, 0.5f, 0.5f);
                mb.Color4ub(255, 255, 255, 255);
                mb.AdvanceVertex();
            }
        }
        mb.End();
    }

    static void RebuildSpikeMeshFromVerts(IMaterial* mat, float* vs, int numVerts) {
        // 1. 安全检查：防止空指针或无效顶点数
        if (!mat || !vs || numVerts <= 0) return;

        // 2. 获取渲染上下文
        IMatRenderContext* ctx = g_matsys->GetRenderContext();
        if (!ctx) return;

        // 3. 获取材质期望的顶点格式，并剥离压缩位（防止格式不匹配导致的渲染异常）
        VertexFormat_t fmt = mat->GetVertexFormat();

        // 4. 创建保留模式静态网格
        g_mesh = ctx->CreateStaticMesh(fmt & ~VERTEX_FORMAT_COMPRESSED, TEXTURE_GROUP_STATIC_VERTEX_BUFFER_OTHER, mat);
        if (!g_mesh) return;

        // 5. 开始构建网格，声明图元类型为独立三角形 (MATERIAL_TRIANGLES)
        CMeshBuilder mb;
        mb.Begin(g_mesh, MATERIAL_TRIANGLES, numVerts / 3);

        // 6. 循环写入顶点数据
        // 每个顶点包含 14 个浮点数 (px,py,pz, nx,ny,nz, u0,v0, u1,v1, r,g,b,a)
        for (int i = 0; i < numVerts; ++i) {
            // 计算当前顶点在扁平数组中的起始偏移量
            int offset = i * 14;

            // 空间位置 (Position)
            mb.Position3f(
                vs[offset + 0],
                vs[offset + 1],
                vs[offset + 2]
            );

            // 法线方向 (Normal)
            mb.Normal3f(
                vs[offset + 3],
                vs[offset + 4],
                vs[offset + 5]
            );

            // 主纹理坐标 (UV0)
            mb.TexCoord2f(0,
                vs[offset + 6],
                vs[offset + 7]
            );

            // 光照贴图坐标 (UV1)
            mb.TexCoord2f(1,
                vs[offset + 8],
                vs[offset + 9]
            );

            // 顶点颜色 (Color)
            // 注意：Lua 侧传入的是 0-255 的整数（以 float 形式存储），这里直接强转为 unsigned char
            mb.Color4ub(
                (unsigned char)vs[offset + 10],
                (unsigned char)vs[offset + 11],
                (unsigned char)vs[offset + 12],
                (unsigned char)vs[offset + 13]
            );

            // 推进到下一个顶点
            mb.AdvanceVertex();
        }

        // 7. 结束构建，将数据提交给 GPU
        mb.End();
    }

	int DrawChunk(ILuaBase* LUA);
    int mcmesh_SpikeQuad(ILuaBase* LUA);
    int DebugGetCell(ILuaBase* LUA);
    int mcmesh_DebugClearDirty(ILuaBase* LUA);
}