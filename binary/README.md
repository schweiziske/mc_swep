# MCSWEP Native Mesh

MCSWEP 客户端原生建面后端。模块使用 C++ 维护方块世界镜像，在工作线程中生成区块顶点，并在 Garry's Mod 主线程中创建和持有 Source `IMesh`。Lua 适配器负责把 MCSWEP 的 state-native 世界快照和静态视觉定义传入模块，并将原有区块绘制入口转发给 native backend。

当前接口版本：**ABI 5**。

> 本仓库只包含 native mesh 后端、接口文档和验收脚本。

## 功能

- 以 `stateId` 为权威方块标识，不使用有损的 `blockId + orient` 世界快照。
- 固定长度 chunk snapshot：`8192 × stateId:u32 LE`，共 32768 字节。
- MCBD v4 静态定义和 compact generated visual catalog。
- 支持 opaque/translucent 双 pass、多 IMesh batch 和 section 原子替换。
- 支持 FullCube、Cross、Model、Shape、Connection 和 Liquid 六类 emitter。
- 支持坐标相关的 weighted model choice。
- 支持动态楼梯转角、栅栏/玻璃板/墙等连接、液体高度和 state tint。
- 支持 typed model UV transform：
  - 活塞模型统一旋转修正；
  - 普通铁轨四种转角按面旋转修正。
- 常驻 worker 只处理不可变快照，不访问 Lua、Source 渲染接口或 `IMesh`。
- generation/stale-result 校验、结果内存上限和全局 Lua fallback。
- Lua/native A/B 热切换及运行时统计。

## 目录

```text
.
├─ binary/mcswep_native_mesh/
│  ├─ *.cpp / *.h
│  ├─ mcswep_native_mesh.sln
│  ├─ mcswep_native_mesh.vcxproj
│  └─ include/
├─ tests/
├─ INTERFACE.md
├─ TECHNICAL_SPEC.md
├─ GWATER2_REFERENCE.md
└─ M2_PLAN.md ... M5.5_PLAN.md
```

关键组件：

- `binary/mcswep_native_mesh/worldstate.*`：stateId世界镜像、diff和脏section传播。
- `binary/mcswep_native_mesh/blockdefs.*`：敌意输入校验及MCBD定义解析。
- `binary/mcswep_native_mesh/emitters.cpp`：纯CPU建面实现。
- `binary/mcswep_native_mesh/meshworker.*`：worker队列、结果队列和内存限制。
- `binary/mcswep_native_mesh/meshbuild.*`：主线程快照、IMesh staging、提交、绘制和统计。
- `INTERFACE.md`：Lua与native之间的当前调用契约。
- `GWATER2_REFERENCE.md`：GWater2调研结论、已采用/不采用的设计及并发改进TODO。

## 依赖

构建需要：

- Windows 10/11；
- Visual Studio 2022；
- MSVC v143；
- Windows 10/11 SDK；
- C++17；
- Garry's Mod客户端；
- [danielga/sourcesdk-minimal](https://github.com/danielga/sourcesdk-minimal)。

本工程区分32位和64位Source SDK：

| 目标 | Source SDK目录 | 上游分支 |
|---|---|---|
| Win32 | `binary/mcswep_native_mesh/include/sourcesdk-minimal` | `master` |
| x64 | `binary/mcswep_native_mesh/include/sourcesdk-minimal-x86-64-branch` | `x86-64-branch` |

克隆仓库时优先初始化子模块：

```bash
git clone --recurse-submodules <repository-url>
cd mcswep_native_mesh_standalone
git submodule update --init --recursive
```

如果本地依赖尚未初始化，应确保上述两个目录分别检出对应上游分支。不要用`master`版Source SDK替代x64目录，否则可能遇到旧SDK头文件、MMX intrinsic或库架构不匹配问题。

## 构建

Visual Studio中打开：

```text
binary/mcswep_native_mesh/mcswep_native_mesh.sln
```

推荐配置：

```text
Release | x64
```

命令行构建示例：

```bat
msbuild binary\mcswep_native_mesh\mcswep_native_mesh.vcxproj ^
  -target:Build ^
  -property:Configuration=Release ^
  -property:Platform=x64 ^
  -maxCpuCount
```

可用目标：

| 配置 | 输出文件名 |
|---|---|
| Release x64 | `gmcl_mcswep_native_mesh_win64.dll` |
| Release Win32 | `gmcl_mcswep_native_mesh_win32.dll` |
| Release_Final x64 | `gmcl_mcswep_native_mesh_win64.dll` |
| Release_Final Win32 | `gmcl_mcswep_native_mesh_win32.dll` |

注：非Final关闭了优化以适合进行断点调试。

**请勿使用Debug配置**

当前Visual Studio工程将DLL直接输出到本机Garry's Mod：

```text
<garrysmod>/garrysmod/lua/bin/
```

如果Garry's Mod安装位置不同，请在本地调整工程的`OutDir`，不要把个人绝对路径作为公共构建要求。

Source `CMeshBuilder`按未压缩顶点布局写入数据，因此创建static mesh时必须保留现有的：

```cpp
vertexFormat & ~VERTEX_FORMAT_COMPRESSED
```

删除这一处理会导致顶点流错位，表现为mesh在正常材质下不可见。

## 安装

### DLL

将对应架构的DLL放入：

```text
garrysmod/lua/bin/gmcl_mcswep_native_mesh_win64.dll
```

或：

```text
garrysmod/lua/bin/gmcl_mcswep_native_mesh_win32.dll
```

Lua中通过以下名称加载，不包含平台后缀：

```lua
require("mcswep_native_mesh")
```

### Lua适配器

~~将：adapter/lua/autorun/client/mcswep_native_bridge.lua~~
~~安装为独立客户端addon，例如：~~

~~garrysmod/addons/mcswep-native/lua/autorun/client/mcswep_native_bridge.lua~~

当前版本已经弃用独立适配器，二进制模块已在主仓库中支持。

## 使用

适配器提供以下客户端ConVar和命令：

```text
mc_native_mesh       1启用native，0使用Lua后端
mc_native_think_ms   每帧IMesh提交预算，默认3ms
mc_native_toggle     在Lua/native后端之间热切换
mc_native_status     输出后端状态和native统计
mc_native_pack_profile [reset]  输出/重置Lua数据打包profiling
```

native启用后是完整的平行渲染后端，不进行方块级、pass级或chunk级混合回退。任一定义解析、worker建面或IMesh staging故障都会停用整个native后端，并由适配器把世界交还Lua重建。

## 线程和安全边界

- 所有导出的`mcmesh.*`函数只能从Garry's Mod客户端主线程调用。
- Lua、材质系统和`IMesh`只允许主线程访问。
- worker只消费不可变`SectionSnapshot`并输出CPU顶点。
- 所有Lua传入的blob和数值都按敌意数据处理。
- 二进制字符串必须使用显式长度，禁止依赖`strlen`。
- count在乘法和allocation前校验上限。
- MCBD解析必须校验enum、flag、reserved、索引范围、浮点范围和精确尾部。
- definitions仅在完整解析成功后原子发布。
- 不上传或执行Lua函数、源码及其他可执行payload。

## 测试

离线策略测试位于`tests/`，主MCSWEP仓库还包含state registry、visual catalog、UV/tint及rail behavior测试。

发布前至少执行：

1. 编译`Release | x64`并确认0 error；
2. 运行native acceptance及state visual相关离线测试；
3. 在Garry's Mod中检查`mc_native_status`；
4. 执行Lua/native多次toggle；
5. 检查opaque/translucent、光照纹理、shadow map和heightfield；
6. 检查跨section/chunk放置和拆除；
7. 检查楼梯、连接方块、液体、活塞和四种普通铁轨转角；
8. 检查批量存档加载、地图切换、断线和Shutdown；
9. 等待`pendingSections`、`queuedJobs`、`activeJobs`和`queuedResults`收敛到零。

当前铁轨转角修正规则与Lua一致：普通铁轨的`south_east`、`south_west`、`north_west`和`north_east`模型使用底面`+1`、其余面`+3`的UV旋转。直轨、上坡轨和其他轨道类型不应用该修正。

## 协议与兼容性

当前主要边界：

- module ABI：5；
- MCBD：v4；
- visual extension：v2；
- chunk snapshot：32768字节stateId数组；
- state 0：air。

visual extension v2支持：

- model uniform UV rotation；
- model per-face UV rotation；
- state-major tint code；
- generated plan/group/model/geometry/surface catalog。

协议细节、函数签名、生命周期和诊断字段以[INTERFACE.md](INTERFACE.md)为准。历史阶段记录保留在各`M*_PLAN.md`中，不应覆盖当前接口契约。

## 开发约定

- C++代码更新统一在本独立仓库进行。
- MCSWEP主仓库只负责游戏逻辑、state registry、generated catalog和其他Lua侧功能。
- bridge协议变化时必须同步更新DLL解析器、ABI/cache identity和接口文档。
- 不要提交`.vs/`、`Release/`、`x64/`、`.user`、`.obj`、`.pch`、`.pdb`等本机构建产物。
- 不要在worker中加入Lua调用或Source渲染API调用。
- 新的动态视觉修正应优先编码为typed data，而不是在C++中硬编码registry ordinal、block ID或Lua方块名。

## 许可证

本仓库尚未声明统一的项目许可证。第三方依赖分别遵循其上游仓库的许可证和legal notices；分发DLL或源码前请保留并核对相应声明。
