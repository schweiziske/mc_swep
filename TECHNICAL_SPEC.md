# mcmesh 原生建面模块 —— 实现规格书 v1

**对应契约**: `_native/INTERFACE.md` v1（唯一权威）
**最后更新**: 2026-07-14
**面向读者**: C++ 模块实现者（合作者）

本文档是 INTERFACE.md 的展开实现指南，补充具体的数据布局、算法约束、构建配置、spike 踩坑记录和集成时序。所有内容以 INTERFACE.md 的 v1 契约为准，此处为衍生解释。

---

## M5 typed definitions supplement (2026-07-18)

The adapter now sends deterministic MCBD v3 rather than the provisional v1/v2 record described below. It resolves the effective emitter per orient and serializes FullCube, Cross, Model, static Shape, Connection and Liquid definitions. Model variants/rotation/door-U mirror and all sorted connection mask templates are resolved on the Lua main thread; model orientations also serialize resolved BlockBoxes for use only when an adjacent shape performs coverage (model quads remain `boxIndex=0xffff` and model emission remains unconditional/no-cull). Workers receive only immutable numeric records. The normative byte-for-byte v3 contract is `_native/M5_PLAN.md` under “MCBD v3 精确顺序布局”; older layout examples in this document are historical implementation notes only.

Packing is one `pcall` transaction. Numeric conversion rejects non-finite, fractional integer, coordinate/UV/tile, aggregate-count and total-size violations rather than wrapping or truncating. Block IDs, non-default orientations and masks are sorted for reproducible output. During process `ShutDown`, the adapter removes its scheduling hooks/timers/callback, restores patched Lua functions, clears proxy bookkeeping and invokes `mcmesh.Shutdown` once without starting a fallback rebuild.

---

## 1. 架构全景

```
Lua 层 (MCSWEP 主项目, 零改动)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  MC.World (方块数据)              MC.Meshes / MC.MeshesTrans
       │                                    ▲
       │ chunk blob (快照)                   │ 代理 mesh { mesh = proxy }
       ▼                                    │
  ┌─────────────────┐              ┌─────────────────┐
  │ mcswep_native_  │  Handshake   │ 代理 mesh.Draw() │
  │  bridge.lua     │◄────────────►│ → mcmesh.Draw   │
  │ (独立 addon)    │  SetBlockDefs│   Chunk(cx,cy,  │
  └───────┬─────────┘  ApplyChunk  │   cz, pass)     │
          │              Think()    └────────┬────────┘
          │                                  │
  ━━━━━━━━┿━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┿━━━━━━━━━━━━━━━━
  C++ 层  │  gmcl_mcswep_native_mesh_win64   │
  (二进制  │  .dll                            │
   模块)  ▼                                  │
  ┌──────────────────────────────────────────┴──────┐
  │  mcmesh 全局表 (GMOD_MODULE_OPEN 注册)          │
  │                                                  │
  │  ┌──────────┐  ┌──────────┐  ┌───────────────┐  │
  │  │ 世界镜像  │  │ 方块定义  │  │ 工作线程池     │  │
  │  │ blocks +  │  │ MCBD blob │  │ section 建面  │  │
  │  │ orients   │  │ → 支持集  │  │ (diff 定脏)   │  │
  │  └─────┬────┘  └──────────┘  └───────┬───────┘  │
  │        │                              │          │
  │        │         Think()              │          │
  │        │    主线程窗口期落 IMesh ◄────┘          │
  │        │    (IMaterialSystem::                   │
  │        │     CreateStaticMesh)                   │
  │        │                              │          │
  │        │      DrawChunk()             │          │
  │        │    只画不绑 ◄────────────────┘          │
  └────────┼────────────────────────────────────────┘
           │
  ━━━━━━━━━┿━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Source   │
  引擎     ▼
  ┌────────────────────────────────────────────────┐
  │ IMaterialSystem → IMesh (GPU 静态顶点缓冲)      │
  │ shader: mc_gpu_chunk_lighttex_vs30 + fancy ps   │
  │ 材质绑定 / PushModelMatrix 由调用方 (Lua) 负责   │
  └────────────────────────────────────────────────┘
```

核心原则:

- **顶点永不进 Lua**：C++ 建面 → `CMeshBuilder` 写 `IMesh` → GPU
- **Lua 保留**: 写入漏斗、可见性剔除、材质管理、光照纹理烘焙、ConVar/UI
- **分立后端**: native 激活后全权负责当前注册表几何，不与 Lua 按 block/chunk/pass
  混合；M5 的 MCBD v3 覆盖 FullCube/Cross/Model/Shape/Connection/Liquid，
  任一不可恢复故障统一回退整个 Lua 后端。

---

## 2. 模块骨架

### 2.1 DLL 和命名

| 项目 | 值 |
|------|-----|
| DLL 文件名 | `gmcl_mcswep_native_mesh_win32.dll` / `gmcl_mcswep_native_mesh_win64.dll` |
| 模块加载名 | `mcswep_native_mesh`（适配器 `require("mcswep_native_mesh")`） |
| Lua 全局表名 | `mcmesh`（GMOD_MODULE_OPEN 内手动注册，与 DLL 文件名无关） |
| ABI 版本 | `mcmesh.AbiVersion()` 返回 `1` |

### 2.2 入口结构

```cpp
GMOD_MODULE_OPEN() {
    // 1. 获取 IMaterialSystem（在 GMod 启动早期，引擎已初始化）
    //    从 materialsystem.dll 获取 CreateInterface 入口
    //    见 test.h: AcquireMaterialSystem() 参考实现
    // 2. 创建全局 Lua 表 "mcmesh"
    // 3. 注册所有导出函数 (12 个)
    // 4. 兜底: AcquireMaterialSystem 失败 → ThrowError
}

GMOD_MODULE_CLOSE() {
    // 释放所有 IMesh、世界镜像、工作线程
}
```

### 2.3 编译配置要点

- **配置**: ReleaseWithSymbols（非 Debug, 非纯 Release）
  - Debug 缺少 tier0 导入库，无法链接
  - 用户要求关闭优化以便断点调试
- **SDK 分支** (双 SDK 是必要的):
  - win32 (`Release`): `include/sourcesdk-minimal`
  - win64 (`x64/Release`): `include/sourcesdk-minimal-x86-64-branch`
  - 双分支独立配置，不可混用。x64 必须用 x86-64 分支，否则 MMX intrinsic 与 x64 冲突
- **额外源文件**: `imesh_index.cpp`（自补 5 个 SDK 只有声明的 `Generate*IndexBuffer` 函数）
- **模块状态**: 所有全局状态必须在单一 `.cpp` 内持有（或使用 `inline` 变量, C++17）
  - 禁止在头文件使用非 inline 的 `static` 变量/函数（每 TU 独立副本, 状态分裂）
  - 当前 `test.h` 中大量 `inline` 变量是 OK 的写法
- **输出目录**: DLL 输出到 `garrysmod/lua/bin/`（由 vcxproj 配置）

---

## 3. API 函数规格

### 3.1 生命周期函数

#### `mcmesh.AbiVersion() -> number`

```cpp
LUA->PushNumber(1);
return 1;
```

#### `mcmesh.Handshake(cs, ch, sectionSize, bs, matOpaqueName, matTranslucentName) -> bool`

**参数（全部位置参数, 非 table）**:

| 参数 | 类型 | 示例值 | 说明 |
|------|------|--------|------|
| 1 | number | 16 | CS, chunk 水平尺寸 (x/y) |
| 2 | number | 32 | CH, chunk 垂直尺寸 (z) |
| 3 | number | 8 | sectionSize, 不得硬编码（粒度实验预留） |
| 4 | number | 36.5 | BS, blockHalfExtent |
| 5 | string | `"mc_atlas_gpu_lighttex_fancy13_localdeps_vface_opaque"` | 不透明材质名 |
| 6 | string | `"mc_atlas_gpu_lighttex_fancy_trans5_localdeps_vface"` | 半透明材质名 |

**实现步骤**:

1. `LUA->CheckNumber(1..4)` + `LUA->CheckString(5..6)` 取参
2. 验证 `cs == 16 && ch == 32 && bs ≈ 36.5`（浮点容差 ±0.1）
3. 验证 `sectionSize` 合法：2 的幂, ≥1, ≤64, 且 `cs` 和 `ch` 能被 `sectionSize` 整除
4. `g_matsys->FindMaterial(matOpaqueName, TEXTURE_GROUP_OTHER)` 取不透明材质
5. `g_matsys->FindMaterial(matTranslucentName, TEXTURE_GROUP_OTHER)` 取半透明材质
6. 检查两个材质均非空且 `!IsErrorMaterial()`
7. 从材质取 `GetVertexFormat()` 保存（后续建 mesh 时剥 `VERTEX_FORMAT_COMPRESSED`）
8. 任一失败 → 返回 false（适配器静默走 Lua）

**注意**: 不要在此处 `CreateStaticMesh`——只是验证材质可用。

#### `mcmesh.SetBlockDefs(blob) -> bool`

**时机**: 握手成功后、首次 `ApplyChunk` 前（适配器由 `MC.Blocks` + `MC.Atlas` 运行时打包）

**MCBD 格式**: M5 使用严格冻结的 MCBD v3 typed definition stream。精确
Header64/Block16/Orient17/FaceTex36/Box24/Quad88/Template 顺序、枚举、aggregate
计数和 hostile 上限以 `_native/M5_PLAN.md` 的“MCBD v3 精确顺序布局”为唯一依据。

实现要求:

1. 用带长度 `GetString` + `BlobReader` 读取，验证 `totalBytes` 和尾部精确耗尽；
2. count/乘法/剩余字节检查必须先于 reserve/resize；
3. 校验 aggregate counts、重复 id/orient/mask、enum/flags/reserved、tile、atlas
   几何、finite `[0,1]` 坐标与 `[0,16]` UV；
4. 先解析到临时 DefinitionSet，完整成功且未 sealed 才原子发布；
5. 首个通过入参验证的 `ApplyChunk` seal definitions，ClearWorld/Shutdown 解封清理；
6. 所有 effective emitter 都由 per-orient typed record 明确指定，C++ 不按名称猜测。

#### `mcmesh.Shutdown()`

**调用时机**: Lua `ShutDown` hook（适配器注册）

**实现**:
- 释放所有 IMesh（`IMesh::Delete()` 或等价）
- 清空世界镜像
- 等待工作线程完成并销毁线程池
- 释放所有动态分配资源

### 3.2 世界数据流入

#### `mcmesh.ApplyChunk(cx, cy, cz, blob) -> bool`

**blob 格式**: 8192 × `{ id u16 LE, orient u8 }` = 24576 字节
**格序**: `li = lx + ly*16 + lz*256`

沿用 `blob_format.h` 的 `ChunkCell`（3 字节 packed, v0 布局不变）:

```cpp
#pragma pack(push, 1)
struct ChunkCell { uint16_t id; uint8_t orient; };
#pragma pack(pop)
static_assert(sizeof(ChunkCell) == 3);
static_assert(8192 * sizeof(ChunkCell) == 24576);
```

**语义: 权威快照**——不是增量, 不是脏标记。native 自行 diff:

1. `ParseChunkBlob(data, len, localCells)` → 严格 24576 字节
2. 与镜像 diff:
   - 首次: 全量存入, 所有 section 标脏
   - 已有: `memcmp(mirror, localCells, 24576)`, 收集差异 cell 列表
3. 推导脏 section: 每个变化 cell → section index
   - `sx = lx / sectionSize`, `sy = ly / sectionSize`, `sz = lz / sectionSize`
   - `sectionIndex = sx + sy*(CS/sectionSize) + sz*(CS*CS)/(sectionSize*sectionSize)`
   - **跨 chunk 邻居面**: 变化 cell 在 chunk 边界时, 邻居 chunk 会由它自己的 `ApplyChunk` 触发重建（两端都推快照）, 所以不需要手动遍历邻居 chunk
4. 脏 section 入队到工作线程
5. 更新镜像 ← 本地 cells

**性能**: `memcmp` 24576 字节约 1µs, 加 section 推导 < 0.1ms——主线程安全。

#### `mcmesh.SetBlock(bx, by, bz, id, orient) -> bool`

可选细粒度增量接口。适配器 v1 **不使用**，但须实现:

1. 校验 `0 <= bx < CS`, `0 <= by < CS`, `0 <= bz < CH`
2. 更新镜像对应 cell
3. 标脏影响 section
4. 入队工作线程

#### `mcmesh.UnloadChunk(cx, cy, cz) -> bool`

1. 从镜像移除该 chunk
2. 释放该 chunk 下所有 IMesh（按 section 粒度管理）
3. 取消该 chunk 的待处理任务

#### `mcmesh.ClearWorld()`

1. 释放全部 IMesh
2. 清空镜像
3. 清空工作队列
4. 适配器在回退时调用, 确保下次激活干净

### 3.3 帧循环

#### `mcmesh.Think(budgetMS) -> result`

每帧一次（适配器 Think hook）。返回
`{pending=number, committed=number, failed=true?, fault=string?}`；非法、非 finite 或
超范围 budget 返回 false。`pending` 是 dirty sections + 全部尚未释放的 job/result，
`committed` 是本帧原子替换成功的 section 数。

主线程在预算内收割 worker CPU 结果，先校验 chunk/generation/dirty currency，再为
opaque/translucent 的所有 batches staging `CreateStaticMesh`。任一 batch 失败或抛异常时
销毁全部 candidates 并保留旧 section pair；两 pass 全部成功后才一次 swap。单 IMesh
不超过 64,998 vertices；单 section CPU result 上限 32 MiB，worker retained results
合计上限 128 MiB，任何超限显式 fault 而不截断。

**顶点布局**（CMeshBuilder 写入顺序）:

```
Position3f(px, py, pz)     → float × 3   (chunk 局部坐标)
Normal3f(nx, ny, nz)       → float × 3   (面法向)
TexCoord2f(0, u, v)        → float × 2   (atlas UV)
TexCoord2f(1, 0, 0)        → float × 2   (uv1, shader 不读)
Color4ub(255, faceIdx, 0, 255) → u8 × 4  (G=faceIndex 0-5)
AdvanceVertex()
```

**当前 shader 有效字段**: 仅 `pos + uv0`。Color4ub 的 G 通道存 faceIndex 供像素 shader 导数重建面朝向。

### 3.4 绘制

#### `mcmesh.DrawChunk(cx, cy, cz, pass) -> bool`

pass: 0=opaque, 1=translucent
**只画不绑**——不设材质、不改矩阵、不改渲染状态

**实现**:

1. 查找 `(cx,cy,cz,pass)` 下所有 section IMesh
2. 逐个 `mesh->Draw()`
3. 返回 false = 不可恢复故障 → 触发后端回退

**spike 实锤**: mesh 必须构建在 chunk 局部坐标（原点 = chunk 角点），因为适配器在调用前已 `cam.PushModelMatrix(chunk 平移)`, shader 用 `worldPos - localPos` 反推光照。

### 3.5 诊断

#### `mcmesh.GetStats() -> table`

返回: `{ mirrorChunks, meshes, vertices, pendingSections, lastBuildUs, lastThinkMS, workerCount }`

---

## 4. 数据结构

### 4.1 ChunkCell（v0 遗留, v1 沿用）

```cpp
#pragma pack(push, 1)
struct ChunkCell { uint16_t id; uint8_t orient; };
#pragma pack(pop)
// 格序: li = lx + ly*16 + lz*256
// 总长: 24576 字节
```

### 4.2 世界镜像

```cpp
// chunk → 方块数据
std::unordered_map<ChunkKey, ChunkData> mirrorChunks;
// section → IMesh*
std::unordered_map<SectionKey, IMesh*> sectionMeshes;

struct ChunkData {
    ChunkCell cells[8192];
    uint64_t generation;  // 递增版本号, 废弃过时工作结果
};

// ChunkKey: 可用 int64_t (cx<<40)|(cy<<20)|cz, 或 tuple<int,int,int>
// SectionKey: chunk key + pass + sectionIndex
```

### 4.3 工作线程

```cpp
struct SectionTask {
    int cx, cy, cz;
    uint8_t sectionIndex;
    uint64_t generation;
};

struct SectionResult {
    SectionTask task;
    std::vector<float> vertices; // 交错: pos(3)+normal(3)+uv(2)+uv1(2)+color(4)=14 floats/顶点
    int numTriangles;
};

// 队列: 可用 moodycamel::ConcurrentQueue 或 std::mutex + std::deque
```

---

## 5. 建面算法

### 5.1 M5 支持范围

- FullCube：opaque/cutout 与 true-translucent 双 pass；完整 per-corner UV。
- Cross：固定双面对角 quad emitter。
- Model：Lua 预解析 variant/orient/mirror 后的任意 quads；自身保持 no-cull。
- Shape：静态 boxes/faces、self-cover 与邻居 rectangle-union coverage。
- Connection：fence/pane/bars/wall 16 masks、pipe6 64 masks。
- Liquid：water/lava kind、same-kind seam、加权角高和 half-tile side UV。

当前注册表若存在无法合法序列化的 effective definition，整个 MCBD pack 失败并留在
Lua 后端；激活后不按单 block/chunk 混合。

### 5.2 面剔除

等价于 Lua `visibleAt`: 邻居为空/半透明 → 面可见; 邻居为不透明 full-cube → 面被遮挡。

### 5.3 UV 计算

用 SetBlockDefs 的 atlas 参数 + per-orient faceTile 索引 → tile 像素坐标 → (u0,v0,u1,v1) 纹理坐标。

### 5.4 顶点坐标

所有顶点在 chunk 局部坐标空间: 方块位置 × (BS×2) = bx×73 + bs 等, blockHalfExtent=36.5。

---

## 6. 线程模型

```
主线程 (GMod)                    工作线程
─────────────                    ────────
ApplyChunk → 标脏 → 入队 ──────→ 取任务
                                读方块定义(只读共享)
                                读镜像 cells(只读共享)
                                构建顶点
                                结果排队 ──────┐
Think() ← 取结果 ◄────────────────────────────┘
→ CreateStaticMesh (主线程!)
→ 存入 section→mesh 映射

DrawChunk() (渲染 hook 内)
→ 查 IMesh → mesh->Draw()
```

- `CreateStaticMesh` / `CMeshBuilder` / `IMesh::Draw` 限主线程
- 建面计算在工作线程
- 方块定义和镜像 cells 在工作线程只读共享

---

## 7. 安全与回退

### 7.1 输入校验（敌意数据原则）

- blob 非空、长度精确匹配
- 坐标在合理范围
- sectionIndex 合法
- pass ∈ {0,1}
- magic/version 匹配, count 上限先于乘法
- 所有 `BlobReader` 用 `CanRead` 先查后读
- 校验失败 → false/nil, 不崩溃

### 7.2 回退语义

任何 native 调用失败 → 适配器 `state.failed=true` → 下帧 `deactivate()` → 解补丁、清代理、逐帧交还 Lua 重建（每次 4 chunk）。

### 7.3 不做的事

- 不写文件、不弹对话框
- 不在运行时 `ThrowError`
- 工作线程代码须 try/catch，失败丢弃

---

## 8. 与适配器的交互时序

```
启动:
  GMOD_MODULE_OPEN → AcquireMaterialSystem
  Lua 加载 bridge → timer 1s 重试
    → require("mcswep_native_mesh")
    → AbiVersion() == 1
    → Handshake(cs,ch,8,bs, matOpaqueName, matTransName)
    → SetBlockDefs(MCBD blob)
    → 遍历 MC.World → FreeChunkMesh → ApplyChunk → installProxies

每帧:
  Think → mcmesh.Think(budgetMS)

方块编辑:
  BuildChunkMesh(key) [包裹] → packChunkBlob → ApplyChunk → installProxies

渲染:
  cam.PushModelMatrix → render.SetMaterial → 代理:Draw() → DrawChunk → cam.PopModelMatrix

关闭:
  ShutDown hook → mcmesh.Shutdown()
```

---

## 9. Spike 验证记录（踩坑速查）

### 已验证通过

| 项目 | 状态 | 关键发现 |
|------|------|----------|
| win64 IMaterialSystem + IMesh + Draw | ✅ | 全链路通过, 真材质下贴图/亮度/shadow map 正常 |
| win32 编译 | ✅ | Release 通过 (2026-07-13) |
| worldPos-localPos 光照反推 | ✅ | DrawChunk 必须在 PushModelMatrix 内, 否则命中 tile 0 兜底 |
| 顶点有效字段=pos+uv0 | ✅ | shader 不消费 NORMAL/COLOR |
| VERTEX_FORMAT_COMPRESSED 必须剥 | ✅ | 不剥 → 顶点错位 → mesh 隐形 |
| 材质名通过 CurrentChunkMaterial | ✅ | 调用方负责 |

### 关键注意

1. **x64 SDK**: 必须用 `sourcesdk-minimal-x86-64-branch`, MMX intrinsic 冲突
2. **Debug 不可用**: 缺 tier0 导入库 → ReleaseWithSymbols
3. **Generate*IndexBuffer**: SDK 只有声明, 模块侧 `imesh_index.cpp` 自补
4. **模块状态**: 单 TU 持有, 头文件 `static` 每 TU 副本坑
5. **Color4ub**: 用语义 API, 不裸拷 BGRA 字节
6. **cmake 重试**: 材质加载早期可能未就绪, 适配器 1s 间隔重试

### v0 blob_format.h 处理

- `ChunkCell` (3B): ApplyChunk 复用 → 保留
- `BlobReader` / `BlobWriter`: 解析 MCBD 也适用 → 保留
- `Vertex44` / `VertexBlob*`: **已废弃** (v1 顶点不进 Lua)

---

## 10. M0 开发序列

1. **补全 12 个 API stub**: dllmain.cpp 注册全部函数, 编译通过
2. **Handshake 实现**: FindMaterial + 参数校验
3. **SetBlockDefs 实现**: MCBD 解析 → 支持集
4. **世界镜像**: unordered_map + ApplyChunk diff
5. **工作线程框架**: section task → worker → Think 落地 IMesh
6. **full-cube emitter**: 最简建面
7. **适配器对齐**: SetBlockDefs 调用、混合后端过滤、ShutDown hook（当前有缺口）

---

## 附录 A: 相关文件

| 文件 | 路径 |
|------|------|
| 契约 (唯一权威) | `_native/INTERFACE.md` |
| 本文档 | `_native/TECHNICAL_SPEC.md` |
| v0 数据契约 | `_native/blob_format.h` |
| 适配器 | `_native/adapter/lua/autorun/client/mcswep_native_bridge.lua` |
| C++ 主入口 | `mcswep_native_mesh/dllmain.cpp` |
| Spike 测试 | `mcswep_native_mesh/test.cpp` / `test.h` |
| 握手框架 | `mcswep_native_mesh/handshake.cpp` / `handshake.h` |
| Index buffer 自补 | `mcswep_native_mesh/imesh_index.cpp` |
| vcxproj | `mcswep_native_mesh/mcswep_native_mesh.vcxproj` |

## 附录 B: Lua 侧依赖接口（需保持稳定）

适配器依赖（改名/改签名前同步）:

- `MC.BuildChunkMesh(key, sectionIndex)`
- `MC.RebuildChunkMeshLighting(key, sectionIndex)`
- `MC.FreeChunkMesh(key)`
- `MC.World`, `MC.Meshes`, `MC.MeshesTrans`
- `MC.GetChunkByKey(key)`, `MC.ChunkKeyToCoords(key)`, `MC.CanonicalChunkKey(key)`
- `MC.MarkChunkDrawListDirty()`
- `MC.CurrentChunkMaterial(translucent)` → IMaterial
- `MC.CS`, `MC.CH`, `MC.BS`
- `drawMeshListInner` 条目约定: `{ mesh = { Draw = function } }`