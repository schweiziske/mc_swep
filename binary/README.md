# MCSWEP Native Mesh

A native client-side meshing backend for MCSWEP. The module maintains a mirror of the block world in C++, generates chunk vertices on worker threads, and creates and owns Source `IMesh` objects on Garry's Mod's main thread. The Lua integration passes MCSWEP's state-native world snapshots and static visual definitions to the module, then redirects the existing chunk rendering entry points to the native backend.

Current interface version: **ABI 6**.

> This repository contains only the native mesh backend, interface documentation, and acceptance tests.

## Features

- Uses `stateId` as the authoritative block identifier instead of lossy `blockId + orient` world snapshots.
- Fixed-size chunk snapshots: `8192 × stateId:u32 LE`, totaling 32,768 bytes.
- MCBD v4 static definitions and a compact generated visual catalog.
- Opaque/translucent dual-pass rendering, multiple `IMesh` batches, and atomic section replacement.
- FullCube, Cross, Model, Shape, Connection, and Liquid emitters.
- Coordinate-dependent weighted model selection.
- Dynamic stair corners, fence/glass-pane/wall connections, liquid heights, and state tints.
- Typed model UV transforms:
  - uniform rotation corrections for piston models;
  - per-face rotation corrections for the four standard rail corners.
- Persistent workers process immutable snapshots only and never access Lua, Source rendering interfaces, or `IMesh`.
- Generation and stale-result validation, result memory limits, and global Lua fallback.
- Runtime Lua/native A/B switching and statistics.

## Repository Layout

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

Key components:

- `binary/mcswep_native_mesh/worldstate.*`: the `stateId` world mirror, diffs, and dirty-section propagation.
- `binary/mcswep_native_mesh/blockdefs.*`: hostile-input validation and MCBD definition parsing.
- `binary/mcswep_native_mesh/emitters.cpp`: pure CPU meshing implementations.
- `binary/mcswep_native_mesh/meshworker.*`: worker queues, result queues, and memory limits.
- `binary/mcswep_native_mesh/meshbuild.*`: main-thread snapshot capture, `IMesh` staging, commits, drawing, and statistics.
- `INTERFACE.md`: the current calling contract between Lua and the native module.
- `GWATER2_REFERENCE.md`: GWater2 research findings, adopted and rejected designs, and concurrency improvement TODOs.

## Dependencies

Building requires:

- Windows 10 or 11;
- Visual Studio 2022;
- MSVC v143;
- Windows 10 or 11 SDK;
- C++17;
- the Garry's Mod client;
- [danielga/sourcesdk-minimal](https://github.com/danielga/sourcesdk-minimal).

The project uses separate Source SDK checkouts for 32-bit and 64-bit targets:

| Target | Source SDK directory | Upstream branch |
|---|---|---|
| Win32 | `binary/mcswep_native_mesh/include/sourcesdk-minimal` | `master` |
| x64 | `binary/mcswep_native_mesh/include/sourcesdk-minimal-x86-64-branch` | `x86-64-branch` |

Initialize the submodules when cloning the repository:

```bash
git clone --recurse-submodules <repository-url>
cd mcswep_native_mesh_standalone
git submodule update --init --recursive
```

If the local dependencies have not been initialized, ensure that the two directories above check out their corresponding upstream branches. Do not substitute the `master` Source SDK checkout for the x64 directory, as doing so may cause outdated SDK header, MMX intrinsic, or library architecture mismatch errors.

## Building

Open the following solution in Visual Studio:

```text
binary/mcswep_native_mesh/mcswep_native_mesh.sln
```

Recommended configuration:

```text
Release | x64
```

Example command-line build:

```bat
msbuild binary\mcswep_native_mesh\mcswep_native_mesh.vcxproj ^
  -target:Build ^
  -property:Configuration=Release ^
  -property:Platform=x64 ^
  -maxCpuCount
```

Available targets:

| Configuration | Output filename |
|---|---|
| Release x64 | `gmcl_mcswep_native_mesh_win64.dll` |
| Release Win32 | `gmcl_mcswep_native_mesh_win32.dll` |
| Release_Final x64 | `gmcl_mcswep_native_mesh_win64.dll` |
| Release_Final Win32 | `gmcl_mcswep_native_mesh_win32.dll` |

Note: non-Final configurations disable optimization to make breakpoint debugging practical.

**Do not use the Debug configuration.**

The current Visual Studio project writes the DLL directly to the local Garry's Mod installation:

```text
<garrysmod>/garrysmod/lua/bin/
```

If Garry's Mod is installed elsewhere, adjust the project's `OutDir` locally. Do not make a personal absolute path a shared build requirement.

Source `CMeshBuilder` writes an uncompressed vertex layout, so static meshes must continue to be created with:

```cpp
vertexFormat & ~VERTEX_FORMAT_COMPRESSED
```

Removing this handling misaligns the vertex stream and makes meshes invisible with normal materials.

## Installation

### DLL

Place the DLL for the appropriate architecture at:

```text
garrysmod/lua/bin/gmcl_mcswep_native_mesh_win64.dll
```

or:

```text
garrysmod/lua/bin/gmcl_mcswep_native_mesh_win32.dll
```

Load it from Lua without the platform suffix:

```lua
require("mcswep_native_mesh")
```

### Lua Adapter

~~Copy `adapter/lua/autorun/client/mcswep_native_bridge.lua`~~
~~and install it as a standalone client addon, for example:~~

~~`garrysmod/addons/mcswep-native/lua/autorun/client/mcswep_native_bridge.lua`~~

The standalone adapter is deprecated. The main repository now integrates support for the binary module directly.

## Usage

The integration provides the following client ConVars and commands:

```text
mc_native_mesh       1 enables native; 0 uses the Lua backend
mc_native_think_ms   per-frame IMesh commit budget; defaults to 3 ms
mc_native_toggle     switches between the Lua and native backends at runtime
mc_native_status     prints backend status and native statistics
mc_native_pack_profile [reset]  prints or resets Lua data-packing profiling
```

When enabled, native rendering is a complete parallel backend. It does not use block-level, pass-level, or chunk-level mixed fallback. Any definition parsing, worker meshing, or `IMesh` staging failure disables the entire native backend, after which the integration returns the world to Lua for rebuilding.

## Threading and Security Boundaries

- All exported `mcmesh.*` functions may only be called from Garry's Mod's client main thread.
- Lua, the material system, and `IMesh` may only be accessed on the main thread.
- Workers consume immutable `SectionSnapshot` objects and produce CPU vertices only.
- Every blob and numeric value supplied by Lua is treated as hostile input.
- Binary strings must be read with explicit lengths and must never rely on `strlen`.
- Count upper bounds must be validated before multiplication or allocation.
- MCBD parsing must validate enums, flags, reserved fields, index ranges, floating-point ranges, and the exact end of the payload.
- Definitions are published atomically only after the entire payload has parsed successfully.
- Lua functions, source code, and other executable payloads are never uploaded or executed.

## Testing

Offline policy tests are located in `tests/`. The main MCSWEP repository also contains tests for the state registry, visual catalog, UVs and tints, and rail behavior.

Before a release, at minimum:

1. Build `Release | x64` and confirm that it completes with zero errors.
2. Run the native acceptance tests and state-visual offline tests.
3. Check `mc_native_status` in Garry's Mod.
4. Switch between Lua and native multiple times.
5. Check opaque/translucent rendering, light textures, shadow maps, and heightfields.
6. Check placement and removal across section and chunk boundaries.
7. Check stairs, connected blocks, liquids, pistons, and all four standard rail corners.
8. Check bulk save loading, map changes, disconnection, and shutdown.
9. Wait for `pendingSections`, `queuedJobs`, `activeJobs`, and `queuedResults` to converge to zero.

The current rail-corner correction matches Lua: the standard rail models for `south_east`, `south_west`, `north_west`, and `north_east` apply a UV rotation of `+1` to the bottom face and `+3` to every other face. This correction does not apply to straight rails, ascending rails, or other rail types.

## Protocol and Compatibility

Current primary boundaries:

- module ABI: 6;
- MCBD: v4;
- visual extension: v2;
- chunk snapshot: a 32,768-byte `stateId` array;
- state 0: air.

Visual extension v2 supports:

- uniform model UV rotation;
- per-face model UV rotation;
- state-major tint codes;
- generated plan, group, model, geometry, and surface catalogs.

See [INTERFACE.md](INTERFACE.md) for protocol details, function signatures, lifecycle requirements, and diagnostic fields. Historical stage records remain in the various `M*_PLAN.md` files and do not supersede the current interface contract.

## Development Guidelines

- Make all C++ updates in this standalone repository.
- The main MCSWEP repository is responsible only for game logic, the state registry, the generated catalog, and other Lua-side functionality.
- When the bridge protocol changes, update the DLL parser, ABI/cache identity, and interface documentation together.
- Do not commit local build artifacts such as `.vs/`, `Release/`, `x64/`, `.user`, `.obj`, `.pch`, or `.pdb` files.
- Do not add Lua calls or Source rendering API calls to worker threads.
- Prefer encoding new dynamic visual corrections as typed data instead of hard-coding registry ordinals, block IDs, or Lua block names in C++.

## License

This repository does not currently declare a unified project license. Third-party dependencies remain subject to their upstream licenses and legal notices. Preserve and review the applicable notices before distributing the DLL or source code.
