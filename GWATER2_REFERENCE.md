# GWater2 参考记录与并发 TODO

记录日期：2026-07-19
参考项目：[meetric1/gwater2](https://github.com/meetric1/gwater2)
调研版本：`d21167c`（`main`）

## 结论

GWater2 与本项目都采用“Lua 负责游戏集成，原生模块负责高成本计算和渲染资源”的结构。其多核任务分片、可见性剔除、模块加载诊断和自动化构建值得参考；但其 worker 直接创建 Source `IMesh`、绘制时 `future.get()` 等做法不直接移植。

本项目确定保留并强化以下目标结构：

```text
Lua / GMod 主线程
  ├─ 捕获不可变 SectionSnapshot
  ├─ 投递有界、可排序的 CPU 任务
  └─ 在 Think 预算内创建并原子替换 IMesh

CPU worker 1..N
  └─ 只读取快照和已封存定义，输出 CPU 顶点

DrawChunk
  └─ 永远绘制当前 live mesh，不等待 worker
```

## 借鉴了什么

| GWater2 做法 | 本项目处理 | 状态 |
|---|---|---|
| 根据 `hardware_concurrency()` 创建多个 worker | 使用硬件线程数选择 worker，但限制在 1～4；第一阶段默认最多 2 | 待实现 |
| 按最大 primitive 数拆分多个 mesh 任务 | 继续保留现有顶点上限和多 batch；不允许单个 `IMesh` 越界 | 已有等价实现 |
| 尽早投递建面，稍后绘制时消费 | 继续使用跨帧 job/result 队列；与 GWater2 不同，Draw 不执行阻塞等待 | 已有且保留 |
| 建面前进行视锥/PVS剔除 | 转化为 section 调度优先级，而不是永久跳过不可见 section | 待实现 |
| Lua 检查模块安装、平台、分支和加载错误 | 扩充 `mc_native_status` 和启动日志，报告期望 DLL、架构、ABI、材质与 `require` 错误 | 待实现 |
| Premake + GitHub Actions 自动编译并上传 DLL | 清除本机绝对路径，建立可重复的 Win32/Win64 构建和 artifact | 待实现 |
| Lua userdata 同时提供 `Destroy` 和 `__gc` | 本项目继续使用显式 `Shutdown` + `GMOD_MODULE_CLOSE` 最终屏障，并加强重复调用安全 | 部分已有 |

本次调研没有复制 GWater2 源码。GWater2 主项目使用 GPLv3；设计思想可以参考，但复制实现前必须单独确认许可证兼容性。其 `binary/ThreadPool` 使用独立宽松许可证，但本项目已有更适合自身需求的有界队列，不计划替换。

## 明确不照搬

- 不在 worker 中调用 Lua API、`IMaterialSystem`、`IMatRenderContext`、`CreateStaticMesh`、`CMeshBuilder` 或 `IMesh`。
- 不在 `DrawChunk` 中调用 `future.get()`、等待条件变量或等待 worker。
- 不在新网格完成前销毁旧网格；继续使用候选网格全部成功后原子替换。
- 不直接使用全部逻辑核心；必须给 GMod 主线程、驱动和其他系统保留 CPU。
- 不使用无界任务队列；任务数、活跃计算内存和已完成结果内存都必须有硬上限。
- 不为了模仿 GWater2 而引入 FleX、CUDA 或计算着色器；其 GPU 流体架构与方块建面不是同一问题。

## 推荐线程模型

自动 worker 数建议：

| 逻辑线程数 | worker 数 |
|---|---:|
| 1～4 | 1 |
| 5～8 | 2 |
| 9～12 | 3 |
| 13 以上 | 4 |

初次落地时建议先将最大值限制为 2，完成内存统计和游戏内压力测试后再开放 3～4。

每个 section 的状态应可诊断：

```text
Dirty -> Queued -> Active -> ReadyCPU -> Committing -> Live
```

任何世界变化都递增 generation。旧 generation 可以继续计算，但必须在提交前丢弃；后续可增加取消 epoch，使长任务提前退出以减少浪费。

## TODO

### P0：先补测量

- [ ] 为每个阶段增加耗时：`captureUs`、`queueWaitUs`、`buildUs`、`commitUs`。
- [ ] 记录每次提交的 `vertices`、`batches`、opaque/translucent 数量。
- [ ] `GetStats` 增加最近值、峰值和适合长期观察的滑动平均。
- [ ] 区分“worker backlog”和“主线程 commit backlog”，避免错误地用增加线程解决提交瓶颈。

验收：能明确回答一帧卡顿主要来自快照、排队、CPU 建面还是 `IMesh` 创建。

### P0：让 Think 预算覆盖完整工作

- [ ] 当前结果提交循环之外，快照捕获/任务投递循环也检查时间预算。
- [ ] 保留“至少取得进展”的规则，但单帧最多允许一个不可中断的大 section 超预算。
- [ ] 增加 `budgetOverrunUs` 统计。

验收：大量 dirty section 时，除单 section 原子提交外，`Think` 不持续突破配置预算。

### P1：动态、有界多 worker

- [ ] 将 `kWorkerCount` 改为启动时确定的运行时数量。
- [ ] `Start(workerCount)` 创建 1～4 个线程，失败时完整 join 已创建线程并回退。
- [ ] 第一阶段自动上限设为 2，不改变 Lua ABI。
- [ ] `GetStats` 同时报告目标 worker 数和实际存活数。
- [ ] 分别使用 1/2/4 worker 测试队列收敛、编辑、卸载、ClearWorld 和 Shutdown。

验收：2 worker 相比 1 worker 有实际吞吐提升，且无结果错乱、死锁或关闭挂起。

### P1：补齐多 worker 内存预算

- [ ] 当前 128 MiB 只统计保留结果；增加 active build 内存预留或令牌。
- [ ] 总预算同时覆盖 active build、queued result 和主线程 ready result。
- [ ] 内存不足时停止取新任务，保留 dirty bit，而不是截断几何。
- [ ] 捕获 `SectionSnapshot` 分配和任务入队异常，转换成明确 fault，禁止异常跨 Lua/C 边界。

验收：最坏 section、2～4 worker 和 16 个在途任务下，峰值内存有可证明的上限。

### P1：可见性和距离优先级

- [ ] Lua 提供当前相机附近/当前 draw list 的 chunk 优先级提示，或由 native 保存最近 DrawChunk 时间。
- [ ] 优先处理可见、近距离和刚编辑的 section。
- [ ] 不可见 section 仍最终完成，避免阴影、heightfield 或快速转身时缺网格。
- [ ] 加入 aging，防止低优先级任务永久饥饿。

验收：传送、快速转身和大范围编辑时，近处网格更早完成，同时全队列最终收敛。

### P1：生命周期压力测试

- [ ] native/Lua 连续热切换至少 100 次。
- [ ] worker 活跃时执行 chunk unload、ClearWorld、地图切换和游戏关闭。
- [ ] 验证 `stop -> notify -> join -> DestroyAllMeshes -> Clear definitions` 顺序。
- [ ] 验证 `Shutdown`、Lua `ShutDown` hook 和 `GMOD_MODULE_CLOSE` 重复进入安全。
- [ ] 在不同显卡厂商环境测试地图切换，避免重复 GWater2 的卸载/驱动崩溃问题。

验收：所有线程归零、结果内存归零、无悬挂 `IMesh`，无退出崩溃。

### P2：构建与发布自动化

- [ ] 删除 `.vcxproj` 中个人 `G:\...` 输出和库目录依赖。
- [ ] 统一 Release/Debug 的 Garry's Mod Lua 头文件包含方式。
- [ ] 添加可参数化的构建入口（Premake、CMake 或稳定的 MSBuild props 三选一）。
- [ ] GitHub Actions 递归初始化 Source SDK 子模块并编译 Win32/Win64。
- [ ] 上传 DLL、Lua adapter 和版本信息 artifact。

验收：全新 runner 无个人路径即可生成可安装产物。

### P2：模块加载和诊断

- [ ] Lua 启动时记录操作系统、GMod branch、`jit.arch`、期望 DLL 名称和模块是否安装。
- [ ] 保存并展示 `require` 的原始错误。
- [ ] `mc_native_status` 展示 ABI、MCBD 版本、材质状态、worker 配置和各阶段耗时。
- [ ] 失败时区分“尚未就绪、永久不兼容、运行时故障”三类状态。

验收：用户只需执行一次 `mc_native_status` 就能定位大多数安装和启动问题。

### P3：worker 创建 IMesh 的隔离实验（默认不采用）

- [ ] 只有 `commitUs` 被证明是主要瓶颈时才进行实验。
- [ ] 独立实验分支和默认关闭的调试开关，禁止进入正式默认路径。
- [ ] 对比主线程/worker 创建相同静态 mesh 的耗时、稳定性和地图切换行为。
- [ ] 覆盖 NVIDIA、AMD、Intel 和 Win32/Win64。
- [ ] 任一驱动、关闭或地图切换异常即否决该方案。

正式结论仍为：worker 只生成 CPU 顶点，`IMesh` 创建和销毁留在主线程。
