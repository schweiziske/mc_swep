#include "pch.h"

// imesh.h 声明的索引生成函数, 实现在引擎 materialsystem 内部不可链接,
// CMeshBuilder 内联代码引用它们, 模块侧自行提供定义。
void GenerateSequentialIndexBuffer(unsigned short* pIndices, int nIndexCount, int nFirstVertex)
{
    if (!pIndices) return;
    for (int i = 0; i < nIndexCount; ++i)
        pIndices[i] = (unsigned short)(nFirstVertex + i);
}

void GenerateQuadIndexBuffer(unsigned short* pIndices, int nIndexCount, int nFirstVertex)
{
    if (!pIndices) return;
    int nQuads = nIndexCount / 6;
    unsigned short v = (unsigned short)nFirstVertex;
    for (int i = 0; i < nQuads; ++i)
    {
        pIndices[0] = v;  pIndices[1] = v + 1;  pIndices[2] = v + 2;
        pIndices[3] = v;  pIndices[4] = v + 2;  pIndices[5] = v + 3;
        pIndices += 6;  v += 4;
    }
}

void GeneratePolygonIndexBuffer(unsigned short* pIndices, int nIndexCount, int nFirstVertex)
{
    if (!pIndices) return;
    int nTris = nIndexCount / 3;
    for (int i = 0; i < nTris; ++i)
    {
        pIndices[0] = (unsigned short)nFirstVertex;
        pIndices[1] = (unsigned short)(nFirstVertex + i + 1);
        pIndices[2] = (unsigned short)(nFirstVertex + i + 2);
        pIndices += 3;
    }
}

void GenerateLineStripIndexBuffer(unsigned short* pIndices, int nIndexCount, int nFirstVertex)
{
    if (!pIndices) return;
    int nLines = nIndexCount / 2;
    for (int i = 0; i < nLines; ++i)
    {
        pIndices[0] = (unsigned short)(nFirstVertex + i);
        pIndices[1] = (unsigned short)(nFirstVertex + i + 1);
        pIndices += 2;
    }
}

void GenerateLineLoopIndexBuffer(unsigned short* pIndices, int nIndexCount, int nFirstVertex)
{
    if (!pIndices) return;
    int nLines = nIndexCount / 2;
    if (nLines <= 0) return;
    for (int i = 0; i < nLines - 1; ++i)
    {
        pIndices[0] = (unsigned short)(nFirstVertex + i);
        pIndices[1] = (unsigned short)(nFirstVertex + i + 1);
        pIndices += 2;
    }
    pIndices[0] = (unsigned short)(nFirstVertex + nLines - 1);
    pIndices[1] = (unsigned short)nFirstVertex;
}