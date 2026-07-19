# M5 计划：补齐全部剩余方块 emitter（2026-07-18）

## Context

M4 已通过游戏内验收，但 native 仍只实现 full-cube。Lua 后端的剩余有效路径是 cross、model、liquid 与 generic shape；fence、pane/bars、wall、pipe6 是 shape 内依赖邻居连接掩码的动态分支。native 接管期间不混合 Lua，因此这些类型目前会缺失。

M5 完成全部剩余类型，但内部按“typed definition 契约与批处理 → 静态 emitter → 动态 emitter → 全量对账”落地。Lua 保持几何语义权威；adapter 在主线程把 Lua 表解析成受限、确定、worker-ready 的数据，worker 只读取不可变 definitions 与 section snapshot。M4 的 1 worker / 16 in-flight、generation/stale 校验、双 pass section 原子提交和全局 fallback 全部保留。

## 不变量

- C++/Lua 仍是分立后端，不做 block/chunk/pass 级 fallback；完成后当前注册表中所有有效渲染定义均须有 native emitter。
- worker 不调用 Lua/worldstate/引擎 API；IMesh 只在主线程创建、销毁和绘制。
- 逐项复刻 Lua 的 emitter 选择、orient fallback、顶点顺序、坐标、UV、法线、pass、剔面和液体高度，不擅自改成其他 Minecraft 规则。
- 所有 `mcmesh.*` 输入视为敌意数据：长度感知读取；上界检查先于乘法、reserve/allocation；float 必须 finite 且在范围内；失败原子拒绝，不截断、不崩溃。
- definitions 在首个 `ApplyChunk` 后继续 seal；每次重新激活从当前 `MC.Blocks` 重新打包。

## 1. MCBD v3：typed render definitions

继续使用一个 definition blob，一次完整解析、一次原子发布和 seal。adapter 按 Lua dispatcher 的实际 fallback 链预解析每个 block 的 effective emitter，不让 C++ 根据名字猜测。

### 通用 block 元数据

- `id`、opaque/translucent pass、transparent、solid、逐 orient fullCube。
- `EmitterKind = FullCube | Cross | Model | Shape | Connection | Liquid`。
- `LiquidKind = None | Water | Lava`，water 与 bubble-column 可共享 kind。
- `ConnectionKind = None | Fence | Pane | Bars | Wall | Pipe6`，以及邻块对各连接族的 capability bits。
- 默认 orient 固定排第一；未知 orient 回退第一条。

### worker-ready orient/template 数据

- full cube/liquid：六面 tile + 完整 UV 描述。
- cross：side tile，C++ 生成固定几何。
- model：adapter 预先完成 variant、orient rotation 与 door U mirror，序列化最终 quad 的四坐标、四个 tile-local UV、tile、face。
- static shape：最终 boxes、每 box 的 face-present 和 tile/UV/rot；保留 box 索引用于 self/neighbor coverage。
- connection：adapter 为 fence/pane/bars/wall 的 16 masks 与 pipe6 的 64 masks 预生成最终 box/face templates，包括 pane edge/end UV 和 connected-boundary omission；worker 仅计算 mask 并选模板。

### MCBD v3 精确顺序布局（2026-07-19 双端实现已对齐）

所有整数 little-endian，`f32` 为 IEEE-754 binary32 little-endian；无隐式 padding。流按 block id 升序；每 block 的默认 orient 第一，其余 orient 数值升序；connection template 按 mask 升序。`Box24` 坐标为 block-local `[0,1]`；`Quad88` position 为有界 block-local extension `[-16,16]`（model 几何可合法伸出单位方块，禁止 clamp）；UV 为 tile-local `[0,16]`。面序/枚举均为 `TOP=1, BOTTOM=2, XP=3, XN=4, YP=5, YN=6`。

```
Header64:
  magic[4]="MCBD"; version:u32=3; totalBytes:u32
  atlasCols:u16; atlasRows:u16; tilePixels:u16; padPixels:u16
  stridePixels:u16; atlasWidth:u16; atlasHeight:u16; reserved0:u16=0
  insetPixels:f32
  blockCount:u32; orientCount:u32; boxCount:u32; quadCount:u32
  templateCount:u32; reserved1:u32=0; reserved2:u32=0; reserved3:u32=0

repeat blockCount Block16 + its orient records:
  id:u16
  flags:u16                 bit0=transparent, bit1=solid; all others zero
  defaultOrient:u8
  liquidKind:u8             0=None, 1=Water, 2=Lava
  connectionKind:u8         0=None, 1=Fence, 2=Pane, 3=Bars, 4=Wall, 5=Pipe6
  reserved0:u8=0
  orientCount:u16; reserved1:u16=0; reserved2:u32=0

  repeat orientCount (default first) Orient17 + variable payload:
    orient:u8
    emitterKind:u8          1=FullCube,2=Cross,3=Model,4=Shape,5=Connection,6=Liquid
    pass:u8                 0=opaque/cutout, 1=translucent
    flags:u8                bit0=fullCube; all others zero
    connectionCaps:u8       bit0=fence, bit1=pane/bars, bit2=wall,
                            bit3=pipe6/chorus, bit4=end-stone-down; others zero
    faceTexCount:u8         FullCube/Liquid=6, Cross=1, otherwise 0
    reserved0:u8=0
    boxCount:u16; quadCount:u32; templateCount:u16; reserved1:u16=0
    FaceTex36[faceTexCount]
    Box24[boxCount]
    Quad88[quadCount]
    Template...[templateCount]

FaceTex36:
  tile:u16; flags:u8=0; reserved:u8=0
  uv[4] = repeat { u:f32, v:f32 }       -- corners in face canonical order

Box24:
  x0:f32,y0:f32,z0:f32,x1:f32,y1:f32,z1:f32

Quad88:
  face:u8
  flags:u8                 bit0=run neighbor coverage; all others zero
  boxIndex:u16             0-based; 0xffff means no box (model)
  tile:u16; reserved:u16=0
  vertex[4] = repeat { x:f32,y:f32,z:f32,u:f32,v:f32 }

Template (sequential, no offset table):
  mask:u8; reserved:u8=0; boxCount:u16; quadCount:u32
  Box24[boxCount]
  Quad88[quadCount]
```

`boxCount`/`quadCount` in Header64 include both direct orient payloads and all boxes/quads nested in templates. `templateCount` counts template records. Model quads are fully variant-resolved, rotated and door-U-mirrored by Lua. Model orientations additionally carry resolved `MC.BlockBoxes(id, orient)` in their direct `Box24` payload so neighboring shape coverage can query model geometry; model quads keep `boxIndex=0xffff` and the model emitter itself performs neither self nor neighbor culling. Static shape quads are final per-face records with omitted `boxFaces` absent faces and a 0-based box index for coverage. Static shape quads on a unit-cell boundary carry `Quad.flags bit0`, directing the worker to perform the Lua-equivalent neighbor coverage test; interior faces omit that check. Connection records contain all 16 horizontal masks (fence/pane/bars/wall) or all 64 masks (pipe6), with internal/connected-boundary faces removed and pane edge/end UV already resolved. FullCube/Liquid retain six complete per-corner face UV definitions; Cross retains one side texture definition.

Hostile limits at pack time: blocks `<=65535`, orients/block `<=64`, aggregate boxes `<=1,000,000`, quads `<=4,000,000`, templates `<=1,000,000`, blob `<=512 MiB`. Any non-finite/non-integral/out-of-range value, unsupported effective definition, malformed box/quad, or limit violation aborts the entire `pcall` pack; no partial/truncated blob is sent.

### 格式与 hostile validation

header 记录 blob 总长及 aggregate block/orient/box/quad/template counts；先校验全局上界，再逐条读取。解析器检查重复 id/orient/mask、所有 enums/flags/reserved、atlas 几何与 tile 范围、所有索引/offset、Box24 坐标 `[0,1]`、Quad88 position `[-16,16]`、UV `[0,16]`、finite floats 和尾部精确耗尽。完整解析到临时 immutable DefinitionSet 后才发布；失败保留旧状态。

adapter 按 block id/orient/mask 排序，生成可复现 blob；整个 pack 用 `pcall`，非法或超限 definition 使 native 启动失败而非静默截断。`DebugBlockDef` 增加 emitter/pass/kind 与 template counts。

## 2. 多 batch 与双 pass 原子提交

Lua 可在 64,998 vertices 附近 flush，复杂 section 可能超过单 IMesh：

- PassBuild 改为 faces/vertices 聚合 + `vector<VertexBatch>`；只在完整 quad 边界 rollover。
- SectionMeshes 每 pass 改为 mesh list。
- staging 必须先成功创建全部 opaque 和 translucent candidates；任一失败销毁所有 candidates、保留旧 pair；全部成功才 swap 并销毁旧 lists。`committed` 仍按 section 计。
- Draw/Unload/Clear/Shutdown/DLL close/stats 覆盖所有 batch。
- 加 section/pass/result/in-flight 字节上界与统一 reservation 释放；超限显式 fault，不截断。保持 worker=1、in-flight=16。

共享 quad writer 统一 TRI `{1,3,2,1,4,3}`、BS、atlas point/subrect UV、normal 和现有 color payload。

## 3. CPU emitter 层

新增 CPU-only dispatcher/emitters 源文件并纳入 `.vcxproj/.filters`。`BuildSectionVerts` 只扫描 section、查定义、选 pass 并 dispatch。

- **FullCube**：迁移现有 M4 路径，行为不变。
- **Cross**：复刻 `RM.EmitCrossBlock` 的 0.08..0.92 两张对角面、双面 4 quads/24 vertices；无 orient/neighbor cull。
- **Model**：无条件发射预解析 quad templates；保留 Lua 当前没有 neighbor/cullface 剔除的行为。
- **Static shape**：复刻 same-cell self-cover、仅边界 neighbor check、transparent non-pane 例外、pane coverage、opaque full-cube coverage、neighbor box rectangle-union coverage；`EPS=1e-4`、planar projection 和 explicit subrect+rot 与 Lua 一致。
- **Connection**：worker 从 capability bits 计算 4/6-bit mask，选择预生成模板；emit 时保留 Lua template path 的 vertical/neighbor coverage。邻居若也是 connection，需要计算其 mask/boxes。
- **Liquid**：复刻 sameLiquidKind、solidForFluidHeight、8/9 surface、0.8 weighted threshold、0.001 face offset、corner averaging、full top/bottom UV 与 half-tile side UV；不新增 Lua 当前没有的 flow/animation/level-state。

## 4. Snapshot 与 dirty influence

- SectionSnapshot 显式记录 `coreSize`、`halo=2`、`side=core+2*halo`；8³ section 为 12³ cells。两格 halo 覆盖邻 connection 的 mask 和液体对角/上方采样。
- Capture 仍在主线程从 mirror 拷贝 `{id,orient}`；缺失 chunk 视为空气。
- 单 cell diff 使用保守的 Chebyshev radius-2 section marking，跨 section/chunk 去重；new chunk/unload 同样标记与两格依赖相交的已加载 sections。这样避免对角液面和 connection 二阶 coverage 延迟。

## 5. 公共 API 与生命周期收口

- Handshake 在 cast/modulo 前验证 finite integral positive cs/ch/ss、finite positive bs、ss 非零与整除，派生计算 checked；敌意输入不用会抛 Lua error 的 `Check*`。
- ApplyChunk 必须确认 length-aware pointer 非空且 exact length，再解码。
- Think/Draw/Debug 数值入口统一 finite/integer/range 检查并返回 false/nil。
- adapter 增加 ShutDown hook，只调用一次 `mcmesh.Shutdown` 并移除 hooks/timers/proxy bookkeeping，不在进程退出时调度 fallback rebuild。
- 保持 worker stop-before-definition-clear 和 stop-before-IMesh-destruction。

## 6. 测试与诊断

### 自动/对账

- M2a remains the typed-definition reconciliation and requires `unsupported=0`.
- M2b is intentionally a **fullCube-only** section face-count regression. On an M5 module it must print its exact scope plus `UNVERIFIED` for cross/model/shape/connection/liquid, aggregate pass parity, and normalized vertex-packet parity; it must never print all-emitter geometry `ALL OK`.
- Run `_native/tests/mc_native_test_m5_runtime.lua` for non-visual M5 runtime acceptance: typed definitions, nonzero live stats for all six emitters, per-section emitter census sums, pass/vertex totals, and pending/queue convergence. This script explicitly reports geometry parity as unverified.
- Geometry/UV/culling/edit parity is accepted only by completing the fixed visual/edit/boundary matrix below (or a future real normalized Lua/native vertex comparator).
- malformed blob tests：截断、count bomb/overflow、NaN/Inf、越界坐标/UV/tile/index、重复 key、未知 enum/flags、尾随字节；全部返回 false 且旧 definitions 不变。
- 复用 `_tools/test_shape_*`、generated connection、fence/wall policy tests。

### 固定矩阵

- shape：上下/双 slab、8 stairs、多 box union、透明邻居、pane 例外、explicit UV、missing model face。
- cross：24 vertices、双面 winding/normal/UV、opaque pass。
- model：horizontal/vertical/axis、door/trapdoor/bed variants、door mirror、任意 quad UV、当前 no-cull。
- connection：fence/pane/bars/wall 16 masks、pipe6 64 masks、pane edge/end UV、vertical coverage、跨边界编辑。
- liquid：孤立/上方液体、same-kind、水/bubble、水/lava、各种邻居、对角高度、offset 与 half-UV。
- batching：同 section 多 opaque/translucent batches 与 staging failure 原子性。

### 编译与游戏验收

助手编译 Release x64（并尽可能 Win32），0 errors；不覆盖独立 live addon，只列同步路径。用户最终更新 DLL/bridge 并重启，要求 definitions 与全图 sections mismatch=0、unsupported=0；所有 emitter stats 非零；pending/queues 收敛；逐类视觉/UV/pass/编辑/边界/shadow/heightfield 正常；toggle/Clear/换图/断线/退出无 crash、残留资源或 stale geometry。

## 关键文件

- Native：`blockdefs.h/.cpp`、`meshbuild.h/.cpp`、`meshworker.h/.cpp`、`worldstate.h/.cpp`、`handshake.cpp`、`dllmain.cpp`，新增 emitter 文件，更新 `.vcxproj/.filters`。
- Adapter：`_native/adapter/lua/autorun/client/mcswep_native_bridge.lua`。
- Tests/docs：`_native/tests/`、`_native/INTERFACE.md`、`_native/TECHNICAL_SPEC.md`。

## 落地记录

- 2026-07-18 Lua/project side: adapter upgraded to deterministic MCBD v3 above; effective emitter is resolved independently per orient with model variants/rotation/mirror and connection masks/templates prebuilt. Serialization is checked and atomic under `pcall`.
- 2026-07-18 reconciliation update: model orient records also serialize resolved model boxes for neighbor coverage parity; model quads remain detached (`boxIndex=0xffff`) and model emission remains unconditional/no-cull.
- Added one-shot `ShutDown` hook: removes bootstrap/fallback/Think/change-callback activity, restores patched functions, clears proxy bookkeeping, and calls `mcmesh.Shutdown` without scheduling a Lua rebuild during process teardown.
- M2a/M2b game scripts upgraded for typed-definition census/parity fields exposed by the M5 debug API; source-policy/offline tests remain applicable.
- 2026-07-19 native side complete: MCBD v3 hostile-safe parser, per-orient six-emitter dispatcher, multi-batch dual-pass atomic commit, halo/radius-2 dependencies, 32 MiB section and 128 MiB retained-result limits, live emitter stats, hardened lifecycle/API, and x64 Release build all landed.
- Offline policy suite 10/10 passed. GMod runtime acceptance is still pending. Run, in order: `_native/tests/mc_native_test_m2a.lua` (typed definitions), `mc_native_test_m2b.lua` (fullCube-only regression, not all-emitter parity), `mc_native_test_m5_runtime.lua` (typed/live-census/pass/convergence invariants), then the complete fixed matrix above. Only the matrix establishes geometry/UV/edit parity.
