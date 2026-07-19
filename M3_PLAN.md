# M3 计划:工作线程建面(2026-07-18 定稿)

前置:M2 已完成并实测通过(见 INTERFACE.md「正式模块进度」)。M2 形态下
`Think` 在主线程预算内同步建面(实测 ~132us/section):挖放方块的常规节奏
已无感,但**接管风暴**(激活/换图,700 chunk × 16 section ≈ 1.5s 纯建面
CPU)要按预算摊 ~8 秒才收敛,且这 1.5s 全部挤占主线程帧预算。M3 把建面
搬进工作线程,主线程只剩 IMesh 上传(落地),收敛时间贴近纯 CPU 耗时,
帧预算几乎不再为建面买单。

## 0. 不变量(契约红线,全部沿用)

- 所有 `mcmesh.*` 导出仍只在主线程调;工作线程是模块内部实现,不暴露
  (INTERFACE.md 线程规则原文)。
- **IMesh 创建/销毁/绘制只发生在主线程**(Think / DrawChunk 内)。
- 工作线程**不触碰任何 Lua / 引擎 API**,纯 CPU 顶点生成。
- 几何输出与 M2 逐字节等价:同一份 `BuildSectionVerts` 逻辑,只是喂它的
  数据从"直接读 g_world"改为"主线程拍的快照"。

## 1. 架构

```
主线程                                     工作线程 × N
──────                                     ────────────
ApplyChunk: 镜像 diff + 标脏(不变)
Think:
  1) 收割 resultQueue -> 校验代际 ->        loop:
     CMeshBuilder 落地 IMesh(预算内)         pop jobQueue(condvar 等待)
  2) 从脏集合取 section -> 拍快照 ->           纯 CPU 建面(快照 -> 顶点)
     ++代际 -> 投 jobQueue                    push resultQueue
DrawChunk / Unload / Clear(不变)
```

- **快照**:job 携带 (ss+2)^3 = 10×10×10 cell(id u16 + orient u8 = 3KB),
  拍照时在主线程读本 chunk + 面邻镜像,未镜像邻居填 0。拍照 ~微秒级;
  builder 从此不读 `g_world`(线程安全的根)。
- **代际(generation)**:每 (chunkKey, section) 一个计数器,投 job 时 ++
  并随 job 携带;结果回来时代际不等于当前值(期间又被标脏重投)或 chunk
  已卸载 -> 整包丢弃。陈旧结果永远不上屏。
- **队列**:jobQueue = mutex + condvar;resultQueue = mutex。在途 job 上限
  (如 64)防内存与陈旧堆积,脏集合天然是溢出缓冲(位掩码不丢事件)。
- **收割预算**:Think 里 IMesh 落地按 budgetMS 限流(每个 section 落地后
  查表),没落完的结果留在主线程本地列表下帧继续,不回锁。
- **pendingCount** = 脏位数 + 在途 job + 未落地结果(语义不变:还没上屏的
  section 数)。
- **线程数**:先定 2(编译期常量);GetStats.workerCount 如实上报。
- **关停**:stop 旗 + notify + join。`Shutdown` 和 `GMOD_MODULE_CLOSE`
  都要 join(后者现在是空的,M3 补上——换图/退出路径必须干净)。

## 2. 改动面

| 文件 | 动作 |
|---|---|
| `meshworker.h/.cpp`(新) | 线程池、jobQueue/resultQueue、快照结构、代际表 |
| `meshbuild.cpp` | `BuildSectionVerts` 拆成"拍快照(主线程)"+"快照建面(纯函数,线程安全)";Think 重写为 收割→落地→补投;`DebugBuildSection` 改走快照路径(顺带验证纯函数确定性) |
| `worldstate.*` | **零改动**(标脏机器原样) |
| 适配器 | **零改动**(Think 语义不变) |
| dllmain | `GMOD_MODULE_CLOSE` 补 join;注册表不变 |

blob / 握手 / 契约函数签名全部不变,不用重新对齐任何东西。

## 3. 陷阱清单(写码前压住)

- 结果落地前必须**双重校验**:代际相等 + chunk 仍在 `g_world`。只查一样
  都会把陈旧 mesh 钉上屏。
- 快照要连 orient 一起拍(culling 的逐 orient fullCube 位需要邻格 orient)。
- 主线程收割时 CMeshBuilder 用的材质仍来自 `g_promise.MatOpaque`,材质
  error 检查保留(M2 的 "!" 教训)。
- `std::thread` 在 DLL 卸载路径上 join 的顺序:先 stop+notify 再 join,
  绝不能在持锁状态下 join。
- 在途上限打满时 Think 直接跳过补投(脏位还在,下帧再来),不忙等。
- 帧间没有任何工作时不许空转唤醒 worker(condvar 等待,不轮询)。

## 4. 验收

1. **等价性**:`mc_native_test_m2a` / `m2b` 依旧 ALL OK(m2b 走
   DebugBuildSection = 快照路径,顺带验证纯函数与 M2 几何一致)。
2. **收敛**:换图/toggle 接管风暴,从激活到 pending=0 的耗时显著缩短
   (预期从 ~8s 到 ~2s 量级);期间 `lastThinkMS` 稳定低于预算。
3. **正确性压测**:高频连挖连放(含 chunk 边界),画面无陈旧面/撕裂;
   GetStats 新增 jobsEnqueued/jobsDropped,丢弃数 > 0 且画面正确即代际
   机制在工作。
4. **生命周期**:反复 mc_native_toggle、换图、断线重连,无崩溃无泄漏
   (meshes 计数回零,线程 join 干净)。

## 5. 顺序

1. 我写 `meshworker` + meshbuild 拆分 + dllmain join(一次交付);
2. 你重编 DLL,按 §4 顺序测;
## 6. 落地记录(2026-07-18)

M3 C++ 已按本计划一次落地；`Release|x64` 已于 2026-07-18 实际编译通过，
DLL 已输出到 GMod `lua/bin`，剩余为游戏内人工验收：

- 新增 `meshworker.h/.cpp`：1 worker、condvar job queue、result queue、16 在途
  上限、stop/notify/join、排队/运行/结果/丢弃统计。正式参数由 2026-07-18 的
  707 chunk 游戏测试定稿：加载中在途精确封顶 16，稳定后 pending/queued/active/results
  全部归零；单 section build 约 154us，稳态 Think 约 0.0035ms。
- `meshbuild` 已拆成 `CaptureSectionSnapshot`(主线程读 worldstate)与
  `BuildSectionVerts(snapshot)`(纯 CPU、worker 安全)，`DebugBuildSection` 也走
  快照路径。
- `Think` 已改为收割→代际与脏位双检→预算内 IMesh 落地→补投；返回
  `{pending, committed}`，pending 精确覆盖 dirty 与所有尚未落地结果。
  `committed>0` 时适配器每帧至多 bump 一次 `MarkChunkDrawListDirty`，使稳定代理
  背后的 native IMesh 内容变化能立即失效 shadow map / heightfield 缓存。
  Unload/Clear/Shutdown 会使旧工作失效。
- `blockdefs` 增加 shared_mutex：worker 建面持共享锁，敌意 Lua 运行时
  `SetBlockDefs`/Clear 独占提交，消除全局 defs/atlas 数据竞争。
- `GMOD_MODULE_CLOSE` 已补最终 join，vcxproj/filters 已加入新文件。

下一步只做 §4 人工验收，不再改适配器或 blob 契约。
