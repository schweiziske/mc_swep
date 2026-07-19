# M2 计划:full-cube 建面 + IMesh 落地(2026-07-16 修订)

前置:M0 骨架、M1 世界镜像 + diff 已完成归档(见 INTERFACE.md「正式模块进度」)。
M2 是第一个用户能用眼睛验收的里程碑,预计 3~5 天,分 M2a/M2b/M2c 三阶段,
每阶段有独立验收,不憋大招。

## 0. 架构决定:C++ 与 Lua 分立(2026-07-16)

按 chunk 回退、块级混合两案均否——太麻烦,且不支持的方块迟早都要在 C++ 里
实现,过渡机器全是废件。定案:

- **native 激活 = 全权接管建面**。适配器行为保持现状:替换代理 mesh、
  完全旁路 Lua 建面,不做任何混合/回退分工。
- v1 建面器只会建不透明 full-cube;**其余方块(shape/model/cross/liquid/
  透明)暂时不渲染,画面上空缺**。P8 起在 C++ 里逐类补齐 emitter,空缺
  收敛到零。
- `mc_native_mesh` 本就是自选实验开关:M2~M4 测试期用纯 full-cube 世界;
  真实地图开 native 缺树/缺水/缺异形是**已知且接受**的中间态,不是 bug。
- blob 仍打包**全部方块**:culling 需要邻居的 transparent / 逐 orient
  fullCube 位(邻居可以是任何方块,即使 native 不建它)。buildable 位标记
  "native 会建的 id",P8 每补一类 emitter 该位覆盖扩大,契约零改动。

## 1. 契约级发现(先固定,双端据此写码)

读原项目真实代码后确认两点,SetBlockDefs blob 布局据此比 INTERFACE.md
原草案多带信息:

1. `MC.BlockFace(id, face, orient)`(`sh_blocks.lua:1295`)返回的是
   **(tile, rot, flipU, flipV)** 四元组——柱状块(原木侧躺)、furnace /
   dispenser / observer 的朝向面全靠 rot/flip 转 UV。只存 faceTile u16×6
   会把侧躺原木的纹理转向丢掉。
2. `visibleAt`(`cl_mesh_build_context.lua:191`)对邻居调
   `MC.BlockIsFullCube(nb, orient)` 是**逐 orient** 的(双层台阶 = full
   cube,单台阶不是)。culling 要精确对齐,每个 orient 需要一个 fullCube 位。

适配器打包时直接**逐 (id, orient, face) 调用这两个 Lua 函数,把结果快照进
blob**。C++ 零理解成本,对齐自动成立——运行时打包方案的红利。

## 2. SetBlockDefs blob 布局定稿(LE)

```
header:
  magic       u32  "MCBD"
  version     u32  = 1
  atlasCols   u16, atlasRows u16, atlasTile u16, atlasPad u16
  atlasStride u16, atlasW u16, atlasH u16, reserved u16 = 0
  atlasInset  f32                     -- 0.5, 可能是小数所以 f32
  blockCount  u32
per block:
  id          u16                     -- >65535 的 id 打包时跳过
  flags       u8   bit0=全 orient fullCube, bit1=transparent,
                   bit2=liquid, bit3=buildable(native 会建)
  orientCount u8   ≥1; 第一条必须是 DefaultOrient(C++ 未知 orient 回退
                   entry[0],即复刻 NormalizeOrient 的 allowed-else-default
                   语义, sh_blocks.lua:737-744)
  per orient:
    orient  u8
    oflags  u8   bit0 = 该 orient 下 fullCube
    6 × { tile u16, uvBits u8 }      -- face 顺序 TOP,BOTTOM,XP,XN,YP,YN (f=1..6)
                                     -- uvBits: bit0-1=rot, bit2=flipU, bit3=flipV
```

每 orient 条目 20 字节,全量 ~1300 方块 ≈ 35KB,一次上传。

**buildable 判定**(适配器打包时算,复刻 `selectBlockEmitter` @
`cl_mesh_emitters.lua:829-835` 的分支序):非 cross、非 model、非 liquid、
非**真半透明**(`IsTranslucentBlock`=true 的水/冰/染色玻璃走 pass=1 + 排序,
留 M4;树叶/普通玻璃是 alphatest 不透明 pass,2026-07-18 起纳入——Lua 本来
就把它们和石头塞同一张 opaque mesh,culling 公式对透明 cur 天然成立)、
且**所有** allowed orient 下 `MC.BlockIsFullCube` 为真。
**baked 处理(2026-07-18 修订)**:Lua 的 full-cube 发射器里 baked 数据优先于
BlockFace(`cl_mesh_emitters.lua:976-987`),生成库 ~1/3 方块带 baked,一刀切
排除会让砂岩/碎石等大量普通方块空洞。实测 baked.uvs 全库只有 5 种模式且四角
坐标全为 0/16(整格的旋转/翻转排列)——打包时无损归一成 (tile, rot, flipU,
flipV) 塞进现有 uvBits,blob 与 C++ 零改动;子矩形 uv / 非 0-16 uvs 的面无法
归一,该块整体回退 buildable=false。C++ 解析时防御性复核(buildable ⇒ bit0
且非 bit1/bit2,不符就清位)。

透明 full-cube(玻璃/树叶)v1 不建,是 M4 的既定决策点,不提前。

## 3. C++ 侧(核心工作量)

### M2a — 数据面(0.5~1 天)

1. `SetBlockDefs` 解析:敌意输入规则照旧(magic/version 校验、count 上界
   先于乘法、滚动 offset 越界检查、重复 id 后者覆盖、id=0 忽略)。解析进
   `unordered_map<uint16_t, BlockDef>`,BlockDef = { flags, orient 条目数组
   (orient, fullCube, 6×{tile,rot,flipU,flipV}) };atlas 参数存全局。
2. 调试口 `DebugBlockDef(id)`:吐解析结果表(flags + 每 orient 每面
   tile/rot/flip),供双端对账。

### M2b — 建面器,先不上屏(1~2 天)

3. FACES/TRI 常量表移植(出处见 §5 对齐点表,逐字抄)。
4. culling 精确移植。cur 保证是不透明非液体 full-cube,`visibleAt` 化简为:

   ```cpp
   // 对齐 cl_mesh_build_context.lua:191-199
   bool faceVisible( uint16_t cur, uint16_t nb, uint8_t nbOrient ) {
       if ( nb == 0 ) return true;               // 空气/未镜像邻 chunk → 可见
       const BlockDef* d = FindDef( nb );
       if ( !d ) return true;                    // 未知 id: Lua 同样判可见
       if ( d->transparent ) return nb != cur;   // cur 不透明, 恒真; 保留字面一致
       if ( !OrientFullCube( d, nbOrient ) ) return true;
       return false;                             // 不透明 full-cube 邻居 → 剔除
   }
   ```

   sameLiquidKind 分支不用移植:cur 非液体时它恒 false
   (`cl_mesh_geometry.lua:39-44`)。邻格取值:本 chunk / 邻 chunk 镜像;
   邻 chunk 未镜像 → 0,与 Lua `MC.GetBlock` 未加载返 0
   (`sh_voxel.lua:575-581`)一致。**邻居可以是任意方块**(包括 native
   不建的),culling 只看 blob 里的 transparent / 逐 orient fullCube 位。
5. 逐 section 建面(纯 CPU buffer):只处理 `buildable(id)` 的格子,其余
   跳过(v1 空缺);顶点 = chunk 局部坐标 `(l + corner) * kBS`;payload 色
   R=255 / G=faceIndex / B=0 / A=255。
6. 调试口 `DebugBuildSection(cx,cy,cz,sec)` → { faces, verts, buildUs },
   先量面数和耗时,不碰 IMesh。

### M2c — IMesh + 帧循环(1~2 天)

7. `Think(budgetMS)`:FIFO 取 chunk → 取最低脏位 → 建该 section →
   `CreateStaticMesh`(剥压缩位,spike 已验)+ CMeshBuilder 写入 →
   **新 mesh 建好后再 DestroyStaticMesh 旧的**(swap 语义)→ 清位;
   每 section 之间查 `Plat_FloatTime` 预算;mask 清零才出队;已卸载 chunk
   懒删除跳过。返回剩余 pending section 总数。空结果 section → 销毁旧
   mesh 存 null。
8. `DrawChunk(cx,cy,cz,pass)`:遍历该 chunk 的 section mesh 逐个
   `->Draw()`;v1 无半透明 mesh,pass=1 直接返 true。
9. `UnloadChunk` / `ClearWorld` 补上 M2 hook 注释处的 IMesh 销毁;
   `GetStats` 填真值(workerCount=0,M3 再改)。

## 4. 适配器侧(改动很小)

1. `packBlockDefs()`:遍历 `MC.Blocks`,orient 集合取
   `MC.OrientRules[b.orient].allowed`(无规则 → 单条 orient 0),默认
   orient 排第一,逐 (orient, face) 调 `MC.BlockFace` /
   `MC.BlockIsFullCube` 快照。Lua 5.1 没有 string.pack,沿用 packChunkBlob
   的 string.char 手工 LE。
2. 调用时序:Handshake 成功 → `SetBlockDefs(blob)` 必须返 true,否则留在
   Lua 路径。
3. **GPU 光照档案门槛**:`tryStart` 检查
   `MC.CurrentLightProfile().gpuPayloadMaterial` 存在(函数形式要解一层,
   `cl_mesh_config.lua:148-158`);Think 里监视 `MC.CurrentMeshLightStyle()`
   变化 → deactivate。native 的固定 payload 只对 GPU light texture 档案安全。
4. `mc_native_status` 接入 `GetStats` 真值输出(现有代码已会打印,确认
   字段对齐即可)。
5. 双端对账测试脚本(见 §6 验收)。

其余(代理 mesh 替换、补丁/解补丁、回退语义)保持现状,零改动。

## 5. 精确对齐点(C++ 移植出处,一处不许自由发挥)

| 项 | 出处 | 内容 |
|---|---|---|
| FACES 表 | `cl_mesh_constants.lua:39-46` | 6 面顺序 TOP,BOTTOM,XP,XN,YP,YN;每面 4 角 `{offX,offY,offZ,uFlag,vFlag}` + off + 法线,逐字抄 |
| 三角形顶点序 | `cl_mesh_emitters.lua:272-277` | push 顺序 = 角 **1,3,2,1,4,3**(Source 正面绕向与角序相反,spike 已实证) |
| TileUV | `sh_blocks.lua:2304-2317` | `px0=col*STRIDE+PAD; u0=(px0+INSET)/W; u1=(px0+TILE-INSET)/W`(v 同理);参数全部来自 blob header,**不许硬编码 56/72/4096** |
| UV 旋转/翻转 | `cl_mesh_geometry.lua:216-224` | `rotatedUVFlags`:rot%4 四种 swizzle 后再套 flip |
| 逐角 UV 组装 | `cl_mesh_emitters.lua:451-458` | 角的 uFlag/vFlag 过 rotatedUVFlags → `uf==1 ? u1 : u0` |
| 顶点字段 | `cl_mesh_upload.lua:81-95` | pos + normal + uv0 + color4ub,与 spike 相同 |
| culling | `cl_mesh_build_context.lua:191-199` | §3 的 faceVisible |
| orient 回退 | `sh_blocks.lua:737-744` | allowed 查不到 → DefaultOrient = blob entry[0] |

UV 全程 double 运算,最后喂 CMeshBuilder 才降 float(Lua 侧也是 double)。

## 6. 验收

- **M2a**:双端对账脚本——Lua 侧对一组代表性 id(石头、原木×3 orient、
  furnace 各朝向、玻璃、水、台阶)打印 `MC.BlockFace` /
  `BlockIsFullCube` / transparent,与 `DebugBlockDef` 输出逐项比对;
  台阶/玻璃/水的 buildable 必须为 0。
- **M2b**:全图逐 section 面数对照——Lua 脚本用 `ctx.visibleAt` 同款逻辑数
  每个 section 中 **buildable 方块**的应有面数,与 `DebugBuildSection` 比,
  **要求 100% 一致**;单 section buildUs < 300(Lua 暖态整 chunk
  2.5~5.8ms 量级)。
- **M2c**:纯 full-cube 测试世界 `mc_native_toggle` A/B 截图一致(贴图、
  原木转向、光影);chunk 边界放/挖方块两侧面即时正确(吃 M1 跨 chunk
  传播);故障注入(强制 DrawChunk 返 false)→ 适配器自动回退全图重建;
  `mc_native_status` 看 Think 耗时与 mesh/顶点数。

## 7. 已知偏差(v1 接受)

- **非 full-cube / 透明方块不渲染**:native 开启时树、水、台阶、玻璃等
  画面空缺,P8 逐类补齐。实验开关,测试期用纯 full-cube 世界。
- 逐 orient fullCube 位使双层台阶等邻居的 culling **无偏差**;真正的偏差
  只剩"Lua 侧有定义、blob 打包后才注册的 custom block"——native 判可见
  多画一张被遮面,纯过绘无伪影,重新握手才刷新(v2)。

## 8. 开工顺序

1. C++ 侧先动 M2a 的 SetBlockDefs 解析骨架;
2. 适配器侧同步落 `packBlockDefs()` + M2a 对账脚本,双方联调;
3. blob 布局(§2)与分立形态(§0)写进 INTERFACE.md,取代原
   "按 chunk 混合后端"条目;
4. M2a 验收过 → M2b → M2c,逐段销账。

## 9. 落地记录(2026-07-18)

代码全部写完,待人工测试(详见 INTERFACE.md「正式模块进度」M2 条目):

- C++(原生仓库):新增 `blockdefs.h/.cpp`(§2 blob 解析 + 查询 +
  `DebugBlockDef`)与 `meshbuild.h/.cpp`(§4 对齐表移植 + 逐 section 建面 +
  IMesh swap + `Think`/`DrawChunk`/`DebugBuildSection`);dllmain 注册表重排,
  `ApplyChunk` 经 meshbuild 加握手守卫,`UnloadChunk`/`ClearWorld`/`Shutdown`
  先毁 IMesh 再转调 worldstate;worldstate/handshake 源文件零改动。
- 适配器:`packBlockDefs()`(含手写 f32,经 Python struct 逐字节验证)、
  SetBlockDefs 调用时序、GPU 光照档案门槛(tryStart + Think 双处)。
- 测试脚本:`_native/tests/mc_native_test_m2a.lua` / `mc_native_test_m2b.lua`,
  跑法见文件头。M2c 的 A/B 截图与边界编辑回归按 §6 手工执行。
