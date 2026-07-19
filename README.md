# mcswep_native_mesh

MCSWEP的独立native mesh后端仓库。

## 目录

- `binary/mcswep_native_mesh/`：Garry's Mod二进制模块C++源码与Visual Studio工程。
- `adapter/lua/`：Lua适配器addon。
- `tests/`：游戏内native验收脚本。
- 顶层Markdown文件：接口、技术规格与里程碑记录。

## 克隆

依赖使用Git子模块：

```bash
git clone --recurse-submodules https://github.com/ABSD546316187/mcswep_native_mesh.git
```

已有工作副本可执行：

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

依赖分支：

- `garrysmod_common`: `x86-64-support-sourcesdk`
- 32位`source-sdk`: `master`
- 64位`source-sdk`: `x86-64-branch`

主仓库固定子模块commit，普通构建不会自动升级依赖。

## 构建

打开：

```text
binary/mcswep_native_mesh/mcswep_native_mesh.sln
```

工程使用`$(ProjectDir)`相对路径查找`include/`下的依赖。构建产物、本地Visual Studio缓存和`.vcxproj.user`不会提交。

## Adapter

发布时将：

```text
adapter/lua/autorun/client/mcswep_native_bridge.lua
```

放入独立adapter addon，并将对应平台DLL放入Garry's Mod的`lua/bin/`。
