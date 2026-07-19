--[[----------------------------------------------------------------------------
	mc_native_test_m2a.lua —— M2a 验收: SetBlockDefs 双端对账(baked 归一化版)

	用法: 激活 native 后(mc_native_status 显示 NATIVE), 控制台:
	    lua_openscript_cl mc_native_test_m2a.lua
	    mc_native_test_m2a

	做什么: 全量遍历 MC.Blocks, 把 mcmesh.DebugBlockDef 的解析结果与 Lua 权威
	数据逐项比对。真值口径与适配器 packBlockDefs 一致(2026-07-18 起 baked
	优先于 BlockFace 并归一化为 rot/flip; 归一失败的块 buildable 必须为 false)。
	期望输出 "ALL OK"; 有差异会点名前 20 条。
------------------------------------------------------------------------------]]

if SERVER then return end

--------------------------------------------------------------- 与适配器同款判定
local function orientsForBlock( b )
	local rule = b.orient and MC.OrientRules and MC.OrientRules[b.orient]
	if not rule or not rule.allowed then return { 0 } end
	local def = ( MC.DefaultOrient and MC.DefaultOrient( b.id ) ) or rule.default or 0
	local list = { def }
	local seen = { [def] = true }
	for o in pairs( rule.allowed ) do
		if not seen[o] and #list < 64 then list[#list + 1] = o; seen[o] = true end
	end
	return list
end

local function computeBuildableBase( b, orients )
	if b.shape == "cross" then return false end
	if b.shape == "model" then return false end
	-- alphatest 与真半透明 full-cube 均支持；后者由 flags bit4 分流。
	if MC.IsLiquidBlock and MC.IsLiquidBlock( b.id ) then return false end
	for _, o in ipairs( orients ) do
		if not MC.BlockIsFullCube( b.id, o ) then return false end
	end
	return true
end

local function rotatedFlags( uf, vf, rot, flipU, flipV )
	rot = rot % 4
	if rot == 1 then uf, vf = vf, 1 - uf
	elseif rot == 2 then uf, vf = 1 - uf, 1 - vf
	elseif rot == 3 then uf, vf = 1 - vf, uf end
	if flipU then uf = 1 - uf end
	if flipV then vf = 1 - vf end
	return uf, vf
end

local CORNER_FLAGS = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } }

local function canonicalizeUvs( uvs )
	local flags = {}
	for k = 1, 4 do
		local p = uvs[k]
		if type( p ) ~= "table" then return nil end
		local a, b = tonumber( p[1] ), tonumber( p[2] )
		if ( a ~= 0 and a ~= 16 ) or ( b ~= 0 and b ~= 16 ) then return nil end
		flags[k] = { a / 16, b / 16 }
	end
	for rot = 0, 3 do
		for fu = 0, 1 do
			for fv = 0, 1 do
				local match = true
				for k = 1, 4 do
					local u, v = rotatedFlags( CORNER_FLAGS[k][1], CORNER_FLAGS[k][2], rot, fu == 1, fv == 1 )
					if u ~= flags[k][1] or v ~= flags[k][2] then match = false break end
				end
				if match then return rot, fu == 1, fv == 1 end
			end
		end
	end
	return nil
end

local function isFullRect( uv )
	if uv == nil then return true end
	return tonumber( uv[1] ) == 0 and tonumber( uv[2] ) == 0
		and tonumber( uv[3] ) == 16 and tonumber( uv[4] ) == 16
end

local function bakedFaceFor( b, orient, f )
	local baked = b.baked
	if not baked then return nil end
	local o = baked[orient]
		or ( MC.DefaultOrient and baked[MC.DefaultOrient( b.id )] )
		or baked[0]
	return o and o[f] or nil
end

-- 返回 tile, rot, flipU, flipV, canonical
local function resolveFace( b, id, o, f )
	local bf = bakedFaceFor( b, o, f )
	if bf then
		local tile = tonumber( bf.tile ) or 0
		if bf.uvs then
			local rot, fu, fv = canonicalizeUvs( bf.uvs )
			if rot == nil then return tile, 0, false, false, false end
			return tile, rot, fu, fv, true
		end
		if not isFullRect( bf.uv ) then return tile, 0, false, false, false end
		return tile, math.floor( tonumber( bf.rot ) or 0 ) % 4, false, false, true
	end
	local tile, rot = MC.BlockFace( id, f, o )
	return tile or 0, math.floor( ( rot or 0 ) % 4 ), false, false, true
end

local function expectedEmitter( b, id, orient )
	if b.shape == "cross" then return "cross" end
	local conn = b.connection
	local isConnection = conn == "fence" or conn == "pane" or conn == "bars" or conn == "wall" or conn == "pipe6"
		or ( MC.IsFenceBlock and MC.IsFenceBlock( id ) ) or ( MC.IsPaneBlock and MC.IsPaneBlock( id ) )
		or ( MC.IsWallBlock and MC.IsWallBlock( id ) )
	if b.shape == "model" and not isConnection then
		local RM = MC.RenderMesh
		local q = RM and RM.rotatedModelQuads and RM.rotatedModelQuads( b, orient )
		if q and #q > 0 then return "model" end
	end
	if MC.IsLiquidBlock and MC.IsLiquidBlock( id ) then return "liquid" end
	if MC.BlockIsFullCube and MC.BlockIsFullCube( id, orient ) then return "fullCube" end
	return isConnection and "connection" or "shape"
end

local function debugEmitter( d )
	local v = d and ( d.emitter or d.emitterKind or d.kind )
	if type( v ) == "number" then return ({ [1]="fullCube", [2]="cross", [3]="model", [4]="shape", [5]="connection", [6]="liquid" })[v] end
	return v
end

local function expectedConnectionKind( b, id )
	local k = b.connection
	if k == "fence" or k == "pane" or k == "bars" or k == "wall" or k == "pipe6" then return k end
	if MC.IsFenceBlock and MC.IsFenceBlock(id) then return "fence" end
	if MC.IsPaneBlock and MC.IsPaneBlock(id) then return k == "bars" and "bars" or "pane" end
	if MC.IsWallBlock and MC.IsWallBlock(id) then return "wall" end
end

local function enumName( v, names ) return type(v)=="number" and names[v] or v end

--------------------------------------------------------------------- 对账主体
concommand.Add( "mc_native_test_m2a", function()
	if not ( mcmesh and mcmesh.DebugBlockDef ) then
		print( "[m2a] mcmesh.DebugBlockDef 不存在(模块未加载或未更新)" )
		return
	end
	if not ( MC and MC.Blocks and MC.BlockFace and MC.BlockIsFullCube ) then
		print( "[m2a] MC 未就绪" )
		return
	end

	local checked, missing, mismatch = 0, 0, 0
	local unsupported, typedDebug = 0, 0
	local emitterCensus = { fullCube=0, cross=0, model=0, shape=0, connection=0, liquid=0 }
	local bakedBlocks, bakedCanonical = 0, 0
	local reports = {}
	local function report( fmt, ... )
		mismatch = mismatch + 1
		if #reports < 20 then reports[#reports + 1] = string.format( fmt, ... ) end
	end

	for id, b in pairs( MC.Blocks ) do
		if type( id ) == "number" and id >= 1 and id <= 65535 and b then
			checked = checked + 1
			local d = mcmesh.DebugBlockDef( id )
			if not istable( d ) then
				missing = missing + 1
				if missing <= 5 then print( "[m2a] id " .. id .. " 不在 native 表里" ) end
			else
				local orients = orientsForBlock( b )
				local nativeOrients = d.orients or {}
				for _, o in ipairs( orients ) do
					local od = nativeOrients[o] or ( o == orients[1] and d or nil )
					local want = expectedEmitter( b, id, o )
					emitterCensus[want] = emitterCensus[want] + 1
					local got = debugEmitter( od )
					if got ~= nil then
						typedDebug = typedDebug + 1
						if got ~= want then report( "id %d orient %d emitter: native=%s lua=%s", id, o, tostring(got), want ) end
					elseif d.buildable == false then unsupported = unsupported + 1 end
					local wantPass = MC.IsTranslucentBlock and MC.IsTranslucentBlock(id) and 1 or 0
					local gotPass = od and ( od.pass or ( od.translucentPass ~= nil and (od.translucentPass and 1 or 0) ) )
					if gotPass ~= nil and gotPass ~= wantPass then report("id %d orient %d pass: native=%s lua=%d",id,o,tostring(gotPass),wantPass) end
					local wantLiquid = MC.LiquidKind and MC.LiquidKind(id) or nil
					local gotLiquid = enumName(od and od.liquidKind or d.liquidKind, {[0]=nil,[1]="water",[2]="lava"})
					if gotLiquid ~= nil and gotLiquid ~= wantLiquid then report("id %d liquidKind: native=%s lua=%s",id,tostring(gotLiquid),tostring(wantLiquid)) end
					local wantConn = expectedConnectionKind(b,id)
					local gotConn = enumName(od and od.connectionKind or d.connectionKind,{[0]=nil,[1]="fence",[2]="pane",[3]="bars",[4]="wall",[5]="pipe6"})
					if gotConn ~= nil and gotConn ~= wantConn then report("id %d connectionKind: native=%s lua=%s",id,tostring(gotConn),tostring(wantConn)) end
					if want == "connection" and od and od.templateCount ~= nil then
						local wantTemplates = wantConn == "pipe6" and 64 or 16
						if od.templateCount ~= wantTemplates then report("id %d orient %d templates: native=%s lua=%d",id,o,tostring(od.templateCount),wantTemplates) end
					end
					if (want == "shape" or want == "model") and od and od.boxCount ~= nil then
						local boxes = MC.BlockBoxes(id,o) or {}
						if od.boxCount ~= #boxes then report("id %d orient %d %s boxes: native=%s lua=%d",id,o,want,tostring(od.boxCount),#boxes) end
					end
					if want == "model" and od and od.quads then
						for qi, q in ipairs(od.quads) do
							local boxIndex = q.boxIndex
							if boxIndex ~= nil and boxIndex ~= 65535 then report("id %d orient %d model quad %d boxIndex=%s, want 65535",id,o,qi,tostring(boxIndex)) end
						end
					end
					if (want == "fullCube" or want == "liquid") and od and od.faces then
						for f=1,6 do
							local fd=od.faces[f]
							if fd and fd.uvs and #fd.uvs ~= 4 then report("id %d orient %d face %d UV corner count=%d",id,o,f,#fd.uvs) end
						end
					end
				end
				local fullAll = true
				for _, o in ipairs( orients ) do
					if not MC.BlockIsFullCube( id, o ) then fullAll = false break end
				end
				local canonical = true
				for _, o in ipairs( orients ) do
					for f = 1, 6 do
						local _, _, _, _, ok = resolveFace( b, id, o, f )
						if not ok then canonical = false end
					end
				end
				if b.baked then
					bakedBlocks = bakedBlocks + 1
					if canonical then bakedCanonical = bakedCanonical + 1 end
				end

				local transparent = b.transparent == true
				local liquid = ( MC.IsLiquidBlock and MC.IsLiquidBlock( id ) ) or false
				local buildable = computeBuildableBase( b, orients ) and canonical
				local translucentPass = buildable and MC.IsTranslucentBlock
					and MC.IsTranslucentBlock( id ) or false
				if d.fullCubeAll ~= nil and d.fullCubeAll ~= fullAll then report( "id %d fullCubeAll: native=%s lua=%s", id, tostring( d.fullCubeAll ), tostring( fullAll ) ) end
				if d.transparent ~= nil and d.transparent ~= transparent then report( "id %d transparent: native=%s lua=%s", id, tostring( d.transparent ), tostring( transparent ) ) end
				if d.liquid ~= nil and d.liquid ~= liquid then report( "id %d liquid: native=%s lua=%s", id, tostring( d.liquid ), tostring( liquid ) ) end
				if d.buildable ~= nil and typedDebug == 0 and d.buildable ~= buildable then report( "id %d buildable: native=%s lua=%s", id, tostring( d.buildable ), tostring( buildable ) ) end
				if d.translucentPass ~= nil and typedDebug == 0 and d.translucentPass ~= translucentPass then report( "id %d translucentPass: native=%s lua=%s", id, tostring( d.translucentPass ), tostring( translucentPass ) ) end

				for _, o in ipairs( orients ) do
					local od = d.orients and d.orients[o]
					if not od then
						report( "id %d orient %d 缺条目", id, o )
					else
						local fc = MC.BlockIsFullCube( id, o )
						if od.fullCube ~= nil and od.fullCube ~= fc then
							report( "id %d orient %d fullCube: native=%s lua=%s", id, o, tostring( od.fullCube ), tostring( fc ) )
						end
						-- Pre-M5 debug exposed canonical rot/flip faces. M5 debug may expose
						-- raw per-corner UV instead; typed emitter/pass checks above remain authoritative.
						if od.faces and od.faces[1] and od.faces[1].rot ~= nil then
							for f = 1, 6 do
								local tile, rot, fu, fv, ok = resolveFace( b, id, o, f )
								local fd = od.faces[f]
								if fd and ok then
									if fd.tile ~= tile then report( "id %d o%d f%d tile: native=%d lua=%d", id, o, f, fd.tile, tile ) end
									if fd.rot ~= rot then report( "id %d o%d f%d rot: native=%d lua=%d", id, o, f, fd.rot, rot ) end
									if fd.flipU ~= fu then report( "id %d o%d f%d flipU: native=%s lua=%s", id, o, f, tostring( fd.flipU ), tostring( fu ) ) end
									if fd.flipV ~= fv then report( "id %d o%d f%d flipV: native=%s lua=%s", id, o, f, tostring( fd.flipV ), tostring( fv ) ) end
								end
							end
						end
					end
				end
			end
		end
	end

	print( string.format( "[m2a] checked=%d missingInNative=%d mismatch=%d unsupported=%d typedOrientDebug=%d", checked, missing, mismatch, unsupported, typedDebug ) )
	print( string.format( "[m2a] emitter census fullCube=%d cross=%d model=%d shape=%d connection=%d liquid=%d",
		emitterCensus.fullCube, emitterCensus.cross, emitterCensus.model, emitterCensus.shape, emitterCensus.connection, emitterCensus.liquid ) )
	print( string.format( "[m2a] baked 方块=%d 其中可归一=%d(不可归一的必须 buildable=false)", bakedBlocks, bakedCanonical ) )
	for _, l in ipairs( reports ) do print( "[m2a]   " .. l ) end
	if missing == 0 and mismatch == 0 and unsupported == 0 then
		print( "[m2a] ALL OK —— MCBD v3 typed definitions 双端一致, unsupported=0" )
	elseif typedDebug == 0 then
		print( "[m2a] module debug API is pre-M5; emitter/template parity could not be observed" )
	end

	local names = { "stone", "gravel", "sandstone", "oak_log", "glass", "oak_leaves",
		"ice", "tinted_glass", "red_stained_glass", "slime_block", "honey_block",
		"water", "lava", "red_stained_glass_pane" }
	for _, n in ipairs( names ) do
		local b = MC.BlockAliases and ( MC.BlockAliases[n] or MC.BlockAliases["minecraft:" .. n] )
		if b then
			local d = mcmesh.DebugBlockDef( b.id )
			print( string.format( "[m2a] sample %-24s id=%d buildable=%s transPass=%s baked=%s",
				n, b.id, d and tostring( d.buildable ) or "?",
				d and tostring( d.translucentPass ) or "?", tostring( b.baked ~= nil ) ) )
		end
	end
end, nil, "M2a: SetBlockDefs parse reconciliation between native module and Lua truth" )

print( "[m2a] loaded — run: mc_native_test_m2a" )
