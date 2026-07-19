# mcmesh 模块调用约定 v2（State-native，C++ 持有 IMesh）

2026-07-11 定稿方向。取代 v0(v0 的"顶点 blob 回 Lua"经 P2 实测被否决:
Lua 对象构造 + mesh API 调用地板 ≈ 1.4µs/顶点,满 section 回程 + 上传 ≈ 94ms,
预算 3ms 只容 ~2100 顶点/帧;实测见 `docs/native-mesh-plan.md` P2 记录)。

## 总体形态

native 是**平行渲染后端**,不是 Lua 建面函数的替换件:

- 顶点数据永远不进 Lua。C++ 维护世界镜像,内部推导脏 section,
  工作线程建面,主线程窗口期经 `IMaterialSystem::CreateStaticMesh` /
  `CMeshBuilder` 生成 IMesh 并持有。
- Lua 保留:写入转发、每帧预算调用、可见性剔除、光照纹理烘焙、
  shadow map pass 的时机与材质、所有 ConVar/UI/诊断。
- native 激活时,Lua 侧建面(`MC.BuildChunkMesh`)、mesh 存储
  (`MC.MeshSections`/`MC.Meshes`)、逐 IMesh Draw 对 native 管理的
  内容全部旁路;shader/材质/`.fxc` 零改动。

## 线程与安全规则

- 所有 `mcmesh.*` 函数只能在客户端主线程调用(渲染 hook / Think)。
  工作线程是模块内部实现,不暴露。
- 所有入参按敌意数据校验(恶意服务器可下发 clientside Lua 调用本 API);
  校验失败返回 false/nil,不得崩溃。
- 任何 C++ 侧不可恢复错误:该帧起 `Draw*` 返回 false,Lua 侧据此
  注销后端、回退 Lua 路径(全部 section 标脏重建)。

## 命名(2026-07-14 定稿)

- DLL 文件:`gmcl_mcswep_native_mesh_win32.dll` / `gmcl_mcswep_native_mesh_win64.dll`,
  适配器 `require("mcswep_native_mesh")`。
- Lua 全局命名空间仍为 `mcmesh`:GMOD_MODULE_OPEN 里注册的全局表名与 DLL
  文件名无关,C++ 侧建全局表 `mcmesh` 挂全部函数。本文档 `mcmesh.*` 均指该表。

## 函数清单

### 握手 / 生命周期

```
mcmesh.AbiVersion() -> number            -- 本契约 = 4
mcmesh.Handshake( cs, ch, sectionSize, bs, matOpaque, matTranslucent ) -> boolean
    扁平位置参数(2026-07-14 改, 原单 table 入参作废, C++ 侧直接
    CheckNumber(1..4) + CheckString(5..6) 取参; 原 dataHash 保留字段一并
    移除 —— SetBlockDefs 方案下恒为空串, 无存在意义):
      cs = 16, ch = 32, sectionSize = 8, bs = 36.5          (number × 4)
      matOpaque      = MC.CurrentChunkMaterial(false):GetName()   (string)
      matTranslucent = MC.CurrentChunkMaterial(true):GetName()    (string)
    任一参数与模块编译预期不符 -> false, Lua 不得注册后端。
    sectionSize 是参数不是常量: C++ 不得硬编码 8(粒度实验预留)。
    matOpaque/matTranslucent(2026-07-13 新增): C++ 侧 FindMaterial 这两个
    名字取 GetVertexFormat 建 mesh(剥 VERTEX_FORMAT_COMPRESSED); 找不到或
    IsErrorMaterial -> 握手失败。**适配器传名必须带 "!" 前缀**(2026-07-18
    实测定稿): chunk 材质是运行时 CreateMaterial 的, 裸名 FindMaterial 会去
    磁盘找 .vmt 拿到 error material, 症状 = 建面全部静默失败、画面空白。
mcmesh.SetBlockDefs( blob ) -> boolean
    适配器运行时从 finalized state registry、MC.Blocks、MC.Atlas 和
    MC.StateVisualCatalog 打包静态数据。M5.5 使用 **MCBD v4**：保留六类
    legacy typed emitter，并加入完整 stateId 表、schema/visual hash、compact
    plan/group/model/geometry/surface catalog、state fullCube/generated-cull flags、
    state liquid kind/amount（流动液体高度为`amount/9`）、
    动态楼梯五路 plan、state tint code 与 typed model UV transform。M5.6 的
    visual extension v2支持按模型统一旋转和六面独立旋转：活塞继续使用统一`+2`，
    普通铁轨四种转角按Lua规则使用底面`+1`、其余面`+3`。MCBD visual extension
    旧版本不会被ABI 3解析器静默接受。精确语义见
    `_native/M5.5_PLAN.md`；MCBD v3仅为历史M5契约。
    block id、非默认 orient、template mask 均确定排序；整个 pack 原子失败，不截断。
    native 解析必须校验 totalBytes/aggregate counts/enums/flags/reserved/tile/finite
    Box24 coordinates `[0,1]`、Quad88 position `[-16,16]`（model 可伸出 unit cube，
    不得 clamp）、UV `[0,16]`、duplicate keys/exact tail，并只在完整临时
    DefinitionSet 成功后发布。
mcmesh.Shutdown()                        -- Lua 在 ShutDown hook 调; 释放全部 IMesh/镜像
```

### 世界数据流入（state-native）

```
mcmesh.ApplyChunk( cx, cy, cz, blob ) -> boolean
    blob: 8192 × `{ stateId u32 LE }` = 32768 字节，li = lx + ly*16 + lz*256。
    adapter从权威`chunk.stateSections`展开；`chunk.blocks/orients`不得作为native输入。
    语义 = 该 chunk 的**权威快照**: native 与镜像 memcmp diff, 自行推导
    受影响 section(含跨 chunk 邻居面)并标脏; 无差异则为 no-op。
    适配器只在原管线请求重建时推快照, 不做任何脏管理(实测打包 0.12ms/chunk)。
    `ClearWorld`清世界/mesh但可保留活动worker；A/B停用使用`Shutdown`完整停止worker、
    清definitions和promise，保证下次Handshake可重试。bridge缓存immutable MCBD v4 blob，
    registry/catalog identity不变时toggle无需重新执行Lua definitions打包。
mcmesh.SetBlock( bx, by, bz, id, orient ) -> boolean
    历史预留接口；ABI 2 adapter不使用，权威写入只有完整state快照。
mcmesh.UnloadChunk( cx, cy, cz ) -> boolean   -- 释放镜像与该 chunk 全部 IMesh
mcmesh.ClearWorld()                           -- mc_clear / 后端回退时调
```

### 帧循环

```
mcmesh.Think( budgetMS ) -> result
    result = { pending=number, committed=number }。
    每帧一次(Lua Think hook)。模块在预算内把工作线程完成的
    section 结果落成 IMesh(IMesh 创建/加锁只发生在这里和 Draw 内)。
    pending = 剩余尚未上屏的 section 数；committed = 本次调用实际替换/清空
    IMesh 的 section 数。M3 稳定代理不会因内部 IMesh 替换而改变 Lua mesh list，
    因此适配器在 committed>0 时每帧至多调用一次 MarkChunkDrawListDirty，通知
    shadow map / heightfield 的几何内容版本失效。
```

### 绘制(只能在渲染 hook 内调)

```
mcmesh.DrawChunk( cx, cy, cz, pass ) -> boolean
    pass: 0 = opaque, 1 = translucent。
    **只画不绑**: 用当前渲染状态(材质、model matrix、depth/cull override)
    逐个 mesh->Draw()。材质绑定、chunk 平移矩阵(cam.PushModelMatrix)、
    shadow map 深度材质全部由调用方在调用前设置 —— 适配器把本函数包成
    代理 mesh 塞进 MC.Meshes, 由原管线(cl_draw / shadow map / heightfield)
    在其现有绑定逻辑内触发, 因此 mesh 必须构建在 **chunk 局部坐标**
    (原点 = chunk 角, 原管线的矩阵负责平移到世界)。
    返回 false = 后端故障, 见安全规则。
```

### 集成形态:代理 mesh(适配器 v1 实测采用)

`cl_draw.lua:102` 的 `drawMeshListInner` 对条目只要求 `entry.mesh:Draw()`;
材质/矩阵/渲染状态在调用前已由原管线设好。适配器据此:

1. 运行时包裹 `MC.BuildChunkMesh` / `MC.RebuildChunkMeshLighting` /
   `MC.FreeChunkMesh`(独立 addon, 原项目零改动);
2. 重建请求 → `ApplyChunk` 权威快照(native diff 定脏)→ 往
   `MC.Meshes[key]` / `MC.MeshesTrans[key]` 装 `{ mesh = 代理 }` 记录;
3. 主绘制、shadow map、heightfield 三条消费路径经代理自动导流, 零改动;
4. 每帧 `mcmesh.Think(mc_native_think_ms)`;任何 native 调用失败 →
   解补丁、清代理、全图逐帧交还 Lua 重建。
5. **C++/Lua 分立(2026-07-16 定案)**:native 激活即全权接管建面，不做
   chunk/block/pass 级混合回退。M5 已实现 FullCube/Cross/Model/Shape/
   Connection/Liquid 六类 effective emitter；当前源码中的所有有效定义均由
   MCBD v3 typed definitions 驱动。任何定义解析、worker build 或 IMesh staging
   故障都触发全局 Lua fallback，不静默漏画单类方块。
适配器实现: `_native/adapter/lua/autorun/client/mcswep_native_bridge.lua`。

### 诊断

```
mcmesh.GetStats() -> table
    { mirrorChunks, meshes, vertices, opaqueMeshes, translucentMeshes,
      opaqueVertices, translucentVertices, pendingSections,
      lastBuildUs, lastThinkMS, workerCount,
      queuedJobs, activeJobs, queuedResults, jobsEnqueued, jobsDropped,
      resultBytes, meshCreateFailures, faulted, faultReason,
      emitters={ fullCube=..., cross=..., model=..., shape=...,
                 connection=..., liquid=... } }
    `emitters.*` 为当前已提交 live geometry 的 `{blocks,faces}`，section 替换/
    unload/clear 会同步减去旧值；`resultBytes` 为 worker 当前保留的 CPU 结果字节。
    对接现有 probe/stats 生态(MC.RenderStats 风格)。
```

## Lua 侧接入(由适配器插件完成,主仓库零改动)

原计划的 5 处主仓库接入点已被"代理 mesh"方案取代:适配器作为独立 addon
随 DLL 发布,运行时补丁接管,主仓库不需要任何修改。合作者唯一需要知道的:

1. 适配器依赖这些 MC 接口保持存在且语义不变(改名/改签名要通知):
   `MC.BuildChunkMesh` / `MC.RebuildChunkMeshLighting` / `MC.FreeChunkMesh` /
   `MC.World` / `MC.Meshes` / `MC.MeshesTrans` / `MC.GetChunkByKey` /
   `MC.ChunkKeyToCoords` / `MC.CanonicalChunkKey` / `MC.MarkChunkDrawListDirty`,
   以及 `drawMeshListInner` 的"条目 = { mesh = 有 :Draw() 的对象 }"约定。
2. ConVar / 指令(适配器提供):`mc_native_mesh 0/1`、`mc_native_think_ms`、
   `mc_native_toggle`(A/B 热切换,即时生效)、`mc_native_status`(后端状态
   + `GetStats` 输出)。
3. 回退语义:ABI/握手失败静默走 Lua;运行时故障自动解补丁并全图逐帧重建。

## v1 遗留到 v2 的优化(明确不做,防止范围蔓延)

- 绑定挪进 C++、按材质排序消状态切换(需要 chunk→纹理名契约);
- 索引化 quad(4 顶点/面 + index buffer);
- section 粒度调大(依赖握手的 sectionSize 参数,Lua 侧配合全量重建)。

## spike 验收(写任何正式代码前)

1. `CreateInterface` 取 `IMaterialSystem`,`CreateStaticMesh` 用
   `material->GetVertexFormat()` 填一个已知三角形,
   `PostDrawOpaqueRenderables` 里 `mcmesh.DrawChunk` 画出;
2. color 通道序核对:填 `G=faceIndex` 已知值,shader 侧读出比对
   (D3D BGRA 陷阱,用 `CMeshBuilder::Color4ub` 语义 API,不裸拷);
3. ~~texcoord1 写入 uv2 已知值~~(2026-07-13 作废:GPU light texture 的
   顶点 shader `mc_gpu_chunk_lighttex_vs30.fxc` 的 VS_INPUT 只有 POSITION +
   TEXCOORD0,光照采样坐标由像素 shader 用 `vWorldPos - vLocalPos` 反推
   chunk 原点、查全局 4096² 光照图集的间接表得到,**顶点不携带任何光照
   UV**。native 顶点只需 pos + uv0 + color payload);
4. win32 / win64 双分支各过一遍;
5. GMod 更新适应性:接口版本字符串校验失败时 `Handshake` 返回 false 的
   降级路径人工验证。

**spike 进度(2026-07-11)**:第 1 条主链路已过——win64 下 CreateInterface 取
IMaterialSystem、CreateStaticMesh + CMeshBuilder 填已知三角形、渲染 hook 内
DrawChunk 画出成功。途中踩坑记录:x64 需 garrysmod_common
`x86-64-support-sourcesdk` 分支(老头文件撞 MMX intrinsic);Debug 配置缺
tier0 导入库,工作配置定为 ReleaseWithSymbols;`Generate*IndexBuffer` 5 个
函数 SDK 只有声明,模块侧自补实现;模块状态须单编译单元持有(头文件 static
每 TU 复制副本坑);**CreateStaticMesh 必须剥压缩位**
(`fmt & ~VERTEX_FORMAT_COMPRESSED`,CMeshBuilder 只写未压缩布局,不剥则
顶点流错位、正规材质下 mesh 全部隐形——实测根因);mesh 格式与绘制时绑定
的材质必须配对。

**spike 进度(2026-07-13)**:第 2/3 条收官——镜像真实顶点测试(从
MeshSectionGeometry 缓存取真实面顶点,C++ 原样建 mesh 偏移绘制)在真材质下
贴图、亮度、shadow map 光影全部与真实方块一致。关键契约实锤:**DrawChunk
必须在 `cam.PushModelMatrix(chunk 平移矩阵)` 内调用**——shader 用
`worldPos - localPos` 反推 chunk 原点查全局光照图集,单位矩阵会命中 tile 0
白色兜底,症状 = 恒定满亮度且无光影(排障速记)。顶点 shader 不消费
COLOR0(面朝向由屏幕空间导数重建,vface 变体),payload 按契约照填但对
现行 GPU 材质无画面影响;顶点语义上有效的字段 = pos + uv0。
**几何缓存结构备忘**:MeshSectionGeometry 的 verts 只有 pos/normal/uv0/color,
光照相关是伴生的 light meta(bx/by/bz、faceIndex、li、shadeIndex 等)与
lightByLi 索引,仅供 Lua 光照烘焙使用,native 建面无需产出。
待办:~~win32 分支~~(2026-07-13 主分支全链路复跑通过)、握手降级(并入
正式骨架 M0)。**spike 归档:引擎集成全部技术风险已清零。**

## 正式模块进度

**M0 骨架(2026-07-14 完成)**:状态单 TU;`AbiVersion`=1;`Handshake` 改为扁平
位置参(cs/ch/sectionSize/bs number×4 + matOpaque/matTranslucent string×2),cs/ch/bs
对编译期常量、sectionSize 存运行时;`FindMaterial` 两材质名取格式剥压缩位;
握手成功置 `g_handshaken`,每个碰世界状态的导出函数首行守该旗(未握手 g_ss=0
会除零);故意 ABI 不符 → 适配器日志 "staying on Lua path"(spike 第 5 条销账)。
适配器同步:`require("mcswep_native_mesh")`、握手传扁平参、材质未就绪则 bootstrap
下秒重试。

**M1 世界镜像 + diff(2026-07-14 完成)**:chunk key 21bit×3 打包 u64;镜像 = 定长
`{ id u16[kCellsPerChunk], orient u8[kCellsPerChunk] }`(cs/ch 编译期常量,消除
8192/24576 魔法数);blob 取值用带长度 GetString(全 \0 字节,strlen 路径会截断);
脏集合 = 每 chunk u64 section 掩码 + chunk 级 FIFO(位或天然去重、有界);`ApplyChunk`
memcmp 快路(全同 = no-op,RebuildLighting 重复快照走这条)→ 逐格 diff → 每变化格
6 面邻传播(每轴独立无对角,跨 section/跨 chunk,邻 chunk 未镜像则跳过);新 chunk
入场/`UnloadChunk` 离场惊动 6 个已镜像面邻的贴边 section 整面。验收:Test A 镜像抽查
零差异、Test B no-op=0、Test C 受控三 chunk 传播 corner=4(中心 sec0/4 + -x 邻 sec5
+ -y 邻 sec6)全部对号。调试口 `DebugGetCell`/`DebugDirty`/`DebugClearDirty` 留作正式
功能不删。

**M2 代码落地(2026-07-18,待测试)**:C++ 与适配器全部写完,测试留待人工。
- C++ 新增两模块(GBK 编码,含中文注释,与既有文件同风格):`blockdefs.h/.cpp`
  (MCBD blob 解析 + `unordered_map<u16, BlockDef>` 存储 + `DebugBlockDef`;
  敌意输入全程 BlobReader 带界读取、count 上界、尾部精确耗尽、buildable 位
  防御复核)与 `meshbuild.h/.cpp`(FACES/TRI/rotatedUVFlags/TileUV/visibleAt
  五处逐字移植;逐 section 建面纯函数 `BuildSectionVerts`;CreateStaticMesh
  剥压缩位 + CMeshBuilder 写入 + 先建后毁 swap;`Think` 预算排水(至少建一个
  section,清理不计预算);`DrawChunk` 只画不绑,pass=1 直接 true;
  `DebugBuildSection` 不碰 IMesh)。
- 生命周期改经 meshbuild 包装注册:`ApplyChunk` 加握手守卫(未握手 SS=0 会
  除零,敌意抢跑返回 false)、`UnloadChunk`/`ClearWorld` 先毁 IMesh 再转调
  worldstate、`Shutdown` 全量清理、`GetStats` 填契约全字段。dllmain 注册表
  重排;worldstate/handshake/test 源文件零改动(handshake 里旧空壳成为死代码,
  可留可删)。vcxproj/filters 已加新文件。
- 适配器同步:`packBlockDefs()`(MCBD 打包,含手写 IEEE-754 f32,已对照
  Python struct 逐字节验证;orient 集合取 OrientRules.allowed 默认排首;
  buildable 判定复刻 selectBlockEmitter)、握手后 `SetBlockDefs` 必须 true、
  GPU 光照档案门槛(tryStart 拦 + Think 监视切风格即回退)、status 显示光照
  档案。头部 32 字节与 C++ `McbdHeader` pack(1) 逐字节对齐已验证。
- 验收脚本:`_native/tests/mc_native_test_m2a.lua`(全量 typed defs 对账)、
  `mc_native_test_m2b.lua`(**仅 fullCube** 逐 section 面数回归 + buildUs；M5 下
  必须将其余五 emitter/pass/vertex packet 标为 UNVERIFIED，不得声称 all-emitter
  parity)、`mc_native_test_m5_runtime.lua`(六 emitter typed/live census、section census
  求和、pass/vertex totals、queue convergence；明确不验证 geometry parity)。几何/UV/
  culling/edit 的发布验收仍须完成 `M5_PLAN.md` 固定矩阵。buildUs 验收线 <300us。
- **实测修正(2026-07-18,M2a/M2b 首轮全绿后)**:①材质名必须带 "!" 前缀
  (见握手条目);②baked 方块空洞——生成库 ~1/3 方块(砂岩/碎石等)带
  baked 面数据,Lua 发射器里它优先于 BlockFace,原判定一刀切排除导致空洞;
  修复 = 打包时把 baked 无损归一成 (tile, rot, flip)(全库 uvs 仅 5 种整格
  排列,已逐一验证),blob 与 C++ 零改动,适配器与两个测试脚本同步。诊断
  工具 `_native/tests/mc_native_probe.lua`(准星对方块查全链路)留档。
  ③alphatest 透明 full-cube(树叶/普通玻璃)纳入支持集——它们
  `IsTranslucentBlock`=false,Lua 本来就进 opaque mesh 靠贴图 alpha 镂空,
  culling 公式对透明 cur 天然成立(同 id 相邻隐藏);真半透明
  (水/冰/染色玻璃)仍留 M4。C++ 侧 blockdefs 防御复核放宽一行(透明不再
  否决 buildable),需重编 DLL。

**M3 工作线程建面(2026-07-18 完成并通过游戏内测试)**:
- 新增 `meshworker.h/.cpp`:1 个常驻 worker、mutex+condition_variable job queue、
  result queue、16 个在途结果硬上限。worker 只消费不可变 section 快照并产生 CPU
  顶点,不接触 Lua、worldstate、材质系统或 IMesh。
- `meshbuild` 改为主线程拍 `(SS+2)^3` 邻域快照(`id+orient`),同一份纯 CPU
  `BuildSectionVerts(snapshot)` 同时供 worker 与 `DebugBuildSection` 使用；IMesh
  创建/替换/销毁仍完全留在主线程。
- 每 `(chunk,section)` 维护 generation；结果落地前同时检查 chunk 尚存在、
  generation 相等、该 section 未被重新标脏。卸载/清图会丢弃排队工作和未落地结果，
  正在运行的旧 job 返回后由上述校验丢弃。
- `Think` 顺序为收割结果→预算内落地→从 dirty mask 拍快照投递；只有成功入队
  才清 dirty bit，队列满时脏位留到下帧。`pendingSections` 精确计为 dirty bits +
  尚未落地的在途结果。
- `Shutdown` 与 `GMOD_MODULE_CLOSE` 都执行 stop+notify+join，且不持队列锁 join。
  `GetStats` 新增 queued/active/results/jobsEnqueued/jobsDropped 诊断字段。
- 项目文件已纳入新源码；707 chunk 实测可稳定收敛，pending/queued/active/results
  最终全部归零，M2a/M2b、编辑、光照与阴影测试通过。

**M5 全 emitter native backend(2026-07-19 代码/离线验收完成，待游戏验收)**:
- MCBD v3 Header64/Block16/Orient17 typed contract 已与 adapter 对齐，覆盖
  FullCube/Cross/Model/Shape/Connection/Liquid；无法合法序列化的 definition 使启动
  原子失败，不按块混合。
- worker 复刻 model variant、shape rectangle-union coverage、16/64 connection masks、
  liquid weighted heights；section snapshot 为 halo=2，cell/new/unload dirty influence 为
  radius 2。
- 每 pass 支持多 IMesh batch；opaque/translucent candidates 全部成功才原子替换旧 pair。
  单 section CPU result 32 MiB、retained results 128 MiB，worker=1/in-flight=16。
- hostile MCBD/chunk/API validation、definition seal、generation/stale rejection、shutdown
  stop/join、fault fallback 与 live emitter stats 已收口；Release x64 0 error，离线策略
  tests 10/10 通过。待用户依次运行 M2a、fullCube-only M2b、M5 runtime acceptance，
  并完成视觉/UV/pass/编辑/边界/生命周期固定矩阵；前三者不单独证明 geometry parity。

**M5 Lua/project side(2026-07-18)**:
- Adapter now emits deterministic hostile-safe MCBD v3 typed definitions for all effective emitter kinds. Models are variant/rotation/mirror resolved and connections contain complete sorted 16/64-mask templates. Exact layout is frozen in `_native/M5_PLAN.md`.
- Adapter lifecycle now has a one-shot `ShutDown` hook which stops/removes its scheduling and bookkeeping before `mcmesh.Shutdown`; it does not enqueue fallback rebuilding while the process is exiting.
- M5 census/parity scripts consume typed `DebugBlockDef`/`DebugBuildSection` fields when the updated module exposes them.

**M4 真半透明 full-cube(2026-07-18 完成并通过游戏内验收)**:
- MCBD 升至 v2，flags bit4=`translucentPass`；Lua 以 `MC.IsTranslucentBlock`
  分类非液体、全 orient full-cube、UV 可归一的冰/染色玻璃。普通玻璃/树叶仍走
  opaque alphatest；slime/honey 在当前生成数据下不是 full cube，水/岩浆及异形 emitter
  仍不建。
- 一个 worker result 同时携带 opaque/translucent 顶点，两份 IMesh 在主线程 staging 后
  按 section 原子提交。任一创建失败保留旧 pair，并通过 Think fault 让适配器全局回退。
- `DrawChunk` 严格按 pass 0/1 绘制；GetStats/DebugBuildSection 增加按 pass 字段并保留
  聚合字段。M4 不新增透明距离排序，沿用 Lua 当前 translucent draw-list 顺序。
- 实测 1921 blockdefs、707 chunk / 11312 section 双 pass 对账均 mismatch=0；43 个
  translucent IMesh / 1980 顶点实际落地，视觉、编辑、边界及生命周期测试均正常。

