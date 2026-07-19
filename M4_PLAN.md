# M4 计划：真半透明 full-cube 双 pass（2026-07-18）

## Context

M3 已完成游戏内验收；当前正式参数为 `kWorkerCount=1`、`kMaxInFlight=16`，707 个 chunk 的接管风暴可以稳定收敛到 `pendingSections=0`，稳态队列全部归零。native 目前只为每个 section 生成并持有 opaque IMesh；适配器虽已安装 `MC.Meshes` / `MC.MeshesTrans` 两个代理并在握手时上传两种材质，但 C++ 的 `DrawChunk(pass=1)` 仍为空，所以冰、染色玻璃等真半透明 full-cube 仍不显示。slime/honey 虽属于真半透明分类，但当前生成数据下不是 full cube，因此不属于本阶段支持集。

M4 只接通**非液体真半透明 full-cube**：运行时满足 `MC.IsTranslucentBlock(id)`、全部允许 orient 均为 full cube、六面 UV 均可由现有 `(tile, rot, flipU, flipV)` 无损表达的方块。水/岩浆、pane、shape、cross、model、connection emitter 全部不在本里程碑内；也不新增透明距离排序，继续复用 Lua 当前的 translucent pass、材质和 depth-test/no-write 状态。

## 不变量

- C++/Lua 仍是分立后端，不做按 chunk、按 block 或按 pass 混合回退。
- worker 仍为 1，最大在途仍为 16；worker 只读 snapshot/blockdefs 并生成 CPU 顶点。
- Lua/引擎 API、材质系统和所有 IMesh 操作仍只在主线程。
- 一个 `(chunk, section, generation)` job 同时生成 opaque/translucent；两 pass 来自同一 snapshot，并作为一个 section 事务提交。`pending` / `committed` 始终按 section 计。
- 现有 `FaceVisible` 保持不变：同 id 透明块隐藏内部面，不同透明 id 保留交界面，不透明 full cube 遮挡邻面，非 full cube 不遮挡。
- 所有 `mcmesh.*` 输入继续按敌意数据校验；失败返回 false/nil 或显式 fault，不崩溃。

## 1. MCBD v2：显式记录 render pass

修改：

- `_native/adapter/lua/autorun/client/mcswep_native_bridge.lua`
- `mcswep_native_mesh/blockdefs.h`
- `mcswep_native_mesh/blockdefs.cpp`

MCBD header version 从 1 升到 2，条目长度不变，flags 定义为：

```text
bit0 = fullCubeAll
bit1 = transparent（用于 culling，包含 alphatest 与真半透明）
bit2 = liquid
bit3 = buildable
bit4 = translucentPass
```

Lua 仍是分类权威：

- `buildable` 基础门槛沿用 `computeBuildable` 与 `resolveFace`：排除 cross/model/liquid，要求全部 orient full cube，且所有面可 canonicalize。
- 删除旧的“`IsTranslucentBlock` 即不 buildable”门槛。
- `translucentPass = buildable and MC.IsTranslucentBlock(id)`；普通玻璃/树叶仍为 `transparent + buildable + opaque pass`。
- 水/岩浆仍因 liquid 被排除，不能误走 full-cube builder。

C++ 严格只接受 MCBD v2，不从名称或 `transparent` 推导 pass。解析提交前复核：

- 仅允许 bit0..bit4；未知位拒绝 blob。
- buildable 必须 fullCubeAll 且非 liquid，否则清 buildable 与 translucentPass。
- translucentPass 必须同时为 transparent、buildable、fullCubeAll、非 liquid，否则清除。
- 保留现有带长度读取、count 上界、尾部精确耗尽和临时状态原子提交。
- 补齐与本次新路径直接相关的 atlas/face 校验：finite inset、有效 atlas 维度、tile 范围、`uvBits` 高四位必须为 0，避免敌意数据生成无效 UV。

`DebugBlockDef` 新增 `translucentPass`。ABI 函数签名未变，`mcmesh.AbiVersion()` 保持 1；新 DLL 与旧适配器会因 MCBD version 不匹配而安全留在 Lua 后端。

为防运行中敌意 `SetBlockDefs` 让 worker/result 使用不同定义版本，落实接口既有时序：首次成功 `ApplyChunk` 时 seal blockdefs；sealed 后 `SetBlockDefs` 返回 false。`ClearWorld` 在清空 worker 工作、结果和世界后解除 seal，使下一次 toggle 能重新上传 definitions。

## 2. 一次扫描生成双 pass CPU 结果

修改：

- `mcswep_native_mesh/meshbuild.h/.cpp`
- `mcswep_native_mesh/meshworker.h/.cpp`

新增结构化结果：

```cpp
struct PassVerts {
    int faces = 0;
    std::vector<Vert> verts;
};

struct SectionBuild {
    PassVerts opaque;
    PassVerts translucent;
};
```

`BuildSectionVerts(snapshot, SectionBuild&)` 一次持有 blockdefs shared lock、一次遍历 section。每个 buildable cell 只根据 `FLAG_TRANSLUCENT_PASS` 选择目标 bucket；FACES、TRI、UV、payload 与 `FaceVisible` 共用原逻辑，不复制第二套 builder。

worker `Result` 携带一个 `SectionBuild`，仍只有一个 chunkKey/section/generation/buildUs。worker 入口增加异常屏障：vector 分配或 builder 异常转换成失败 Result，正确递减 active，不得让异常逃出线程导致 `std::terminate`。两个 pass 分别检查 `MAX_VERTEX_COUNT`；失败不截断几何。

保持 `kWorkerCount=1`、`kMaxInFlight=16`，outstanding 仍是 section result 数，不因双 vector 翻倍。

## 3. 双 IMesh 存储与 section 原子提交

修改 `meshbuild.h/.cpp`，将存储改为：

```cpp
struct SectionMeshes {
    IMesh* opaque = nullptr;
    IMesh* translucent = nullptr;
    int opaqueVerts = 0;
    int translucentVerts = 0;
};

struct ChunkMeshes {
    SectionMeshes section[64];
};
```

`CreateMeshFromVerts` 显式接收目标材质，并区分三种结果：

- `Empty`：vector 为空，是合法结果；
- `Created`：新 IMesh 创建成功；
- `Failed`：非空几何因材质/context/CreateStaticMesh 等原因未创建。

opaque 使用 `MatOpaque` 的 vertex format，translucent 使用 `MatTranslucent` 的 vertex format；两者继续剥除 `VERTEX_FORMAT_COMPRESSED`。

每个 section 的提交顺序固定为：

1. generation、chunkAlive、dirty bit 校验通过后，先 staging 创建 opaque 候选；
2. 再 staging 创建 translucent 候选；
3. 任一非空 pass 创建失败：销毁本次已创建的候选，旧的两份 IMesh 都不动，`committed` 不增加；Think 返回显式 fault，适配器触发既有全局 Lua fallback；
4. 两边均为 Created/Empty 后，一次性换入两个指针与计数，再销毁两个旧 mesh；
5. 一个 section 即使替换两份 mesh，`committed` 也只加 1。

这样可正确处理 opaque-only、translucent-only、双 pass、全空，以及方块在两种 pass 间切换，避免一帧内出现不同 generation 的半提交。

## 4. Draw、生命周期与诊断

- `DrawChunk` 对 pass 做严格整数校验，只接受 0/1；pass 0 只画 opaque，pass 1 只画 translucent，其他敌意输入返回 false。继续“只画不绑”，不在 C++ 改材质、矩阵、depth 或 cull 状态。
- `DestroyChunkMeshes`、`DestroyAllMeshes`、Unload、Clear、Shutdown、DLL close 和 staging 失败路径统一覆盖两份 mesh，抽取共用销毁 helper，防漏销毁/双重销毁。
- `Think` 返回保持 `{pending, committed}`，GPU staging/build 失败时增加 `{failed=true, fault=string}`；adapter 在 `result.failed` 时立即 `deactivate`。
- `GetStats` 保留兼容总数 `meshes` / `vertices`，新增 `opaqueMeshes`、`translucentMeshes`、`opaqueVertices`、`translucentVertices`、`meshCreateFailures`。pending、queuedResults 仍按 section result 计。
- `DebugBuildSection` 保留聚合 `faces` / `verts`，新增 `opaqueFaces`、`opaqueVerts`、`translucentFaces`、`translucentVerts`。

## 5. 适配器与测试

适配器已有两套 proxy 与双材质握手，不改架构，只做：

- 打包 MCBD v2/bit4，并记录 opaque/translucent buildable 数；
- 启动日志分别打印两类支持数；
- Think 识别 `failed` 并沿现有全局 fallback；
- `committed>0` 仍每帧最多一次 `MC.MarkChunkDrawListDirty()`。

更新 `_native/tests/mc_native_test_m2a.lua`：

- 对账 bit4 / `translucentPass`；
- 真半透明 full cube 不再自动排除；液体仍排除；
- 固定抽查 stone、树叶/普通玻璃、ice/tinted glass/stained glass、slime/honey、water/lava、stained glass pane；
- 分别统计 opaque/translucent buildable。

更新 `_native/tests/mc_native_test_m2b.lua`：

- support set 改为 opaque/translucent 分类；
- 对每 section 分别计算并比较两类 faces/verts，不能只比较总数；
- 继续使用同一 `faceVisible` 权威规则；
- 额外确认 `verts == faces * 6` 和聚合字段等于两 pass 之和。

## 6. 文档

实现落地时同步更新：

- `_native/INTERFACE.md`：MCBD v2、bit4、双 pass 原子提交、stats/debug/fault 语义、M4 支持矩阵；
- `_native/M3_PLAN.md`：把实测正式参数改为 1 worker/16 in-flight，并归档用户给出的 707 chunk 加载/稳态数据；
- 清理代码和文档中“真半透明留 M4”“pass 1 空返回”“2 worker/64”的过时描述。

## 7. 编译与验收

### 助手侧

1. 编译 `Release|x64`，要求 0 error；保留用户当前 worker=1/in-flight=16。
2. 检查所有 mesh 创建失败路径：候选恰好销毁一次，旧 pair 只在双 staging 成功后销毁。
3. 确认旧聚合 stats/debug 字段仍存在。
4. 不主动覆盖 live addon 的 adapter；提供需同步的 DLL 与 bridge 路径。

### 用户游戏侧

1. 完全重启 GMod，更新 DLL 和独立 live addon 中的 bridge。
2. 先跑 `mc_native_test_m2a`、`mc_native_test_m2b`，要求两 pass 均 `mismatch=0`。
3. 基线回归：普通 full cube、baked 方块、树叶、普通玻璃、光照、shadow map、heightfield 无 M3 回归。
4. 半透明场景：单块 ice/stained glass；同 id 相邻；不同透明 id 相邻；贴 opaque；跨 section/chunk 边界；反复 opaque↔translucent 替换。
5. 预期：同 id 内面隐藏、不同 id 交界面保留、pass 0 不重复画真半透明、pass 1 stats 非零、边界编辑即时更新、pending 最终归零。
6. 生命周期：反复 toggle、换图、断线、退出；最终 active/queued/results/pending 全归零。
7. 水/岩浆、pane、cross/model/connection 仍为空缺但不得被错误建成 full cube。
8. 多层透明混合顺序瑕疵属于当前 Lua 管线继承的已知限制，不作为 M4 阻断项。

## 落地记录（2026-07-18）

M4 已按本计划完成代码落地并通过 `Release|x64` 编译，DLL 已输出到 GMod
`lua/bin`。MCBD v2/bit4、双 pass CPU worker result、section 双 IMesh 原子提交、
pass 0/1 绘制、双资源生命周期、fault 回退、按 pass stats/debug、M2a/M2b 扩展均已完成。

游戏内完整验收已通过：1921 个 blockdefs 全量对账 `mismatch=0`；707 chunk / 11312
section 的 opaque/translucent 分 pass 面数对账 `mismatch=0`；稳定后 pending、job、result
全部归零，`faulted=false`、`meshCreateFailures=0`，43 个 translucent IMesh / 1980 顶点
实际落地。DebugBuildSection 全图平均约 130.3us，单次 max 800.7us；平均值和异步主链
正常，原 `<300us max` 口径改为观察平均/稳态，避免 Windows 调度单次离群值误报。

用户已确认普通/半透明视觉、编辑更新、边界及生命周期行为均正常，M4 正式完成。

## 关键文件

- `G:\编程项目\gmod module\mcswep_native_mesh\blockdefs.h/.cpp`
- `G:\编程项目\gmod module\mcswep_native_mesh\meshbuild.h/.cpp`
- `G:\编程项目\gmod module\mcswep_native_mesh\meshworker.h/.cpp`
- `G:\编程项目\gmod module\MCSWEP\lua\mcswep-codex-mc-uv2-lightmap\_native\adapter\lua\autorun\client\mcswep_native_bridge.lua`
- `G:\编程项目\gmod module\MCSWEP\lua\mcswep-codex-mc-uv2-lightmap\_native\tests\mc_native_test_m2a.lua`
- `G:\编程项目\gmod module\MCSWEP\lua\mcswep-codex-mc-uv2-lightmap\_native\tests\mc_native_test_m2b.lua`
