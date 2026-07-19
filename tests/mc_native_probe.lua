--[[----------------------------------------------------------------------------
	mc_native_probe.lua —— 对着一个"该显示却没显示"的方块查全链路

	用法: 准星对准问题方块(或它该在的位置), 控制台:
	    lua_openscript_cl mc_native_probe.lua
	    mc_native_probe

	一次打印: 世界数据(id/orient/名字/flags) -> 适配器判定 -> native defs
	(DebugBlockDef) -> native 镜像(DebugGetCell) -> native 建面
	(DebugBuildSection 该 section 面数) -> GetStats。哪一环断了当场可见。
------------------------------------------------------------------------------]]

if SERVER then return end

concommand.Add( "mc_native_probe", function()
	local ply = LocalPlayer()
	if not IsValid( ply ) then return end
	local tr = ply:GetEyeTrace()
	if not tr or not tr.HitPos then print( "[probe] no trace" ) return end

	local BS = MC.BS or 36.5
	-- 命中点沿法线往里探半格拿到方块内部; 没碰撞的空洞就探准星停住的位置
	local inside = tr.Hit and ( tr.HitPos - tr.HitNormal * ( BS * 0.5 ) ) or tr.HitPos
	local bx, by, bz = MC.WorldToCell( inside )
	if not bx then print( "[probe] WorldToCell failed" ) return end

	local cx, cy, cz, li = MC.CellToChunk( bx, by, bz )
	local id = MC.GetBlock( bx, by, bz )
	local orient = MC.GetBlockOrient and MC.GetBlockOrient( bx, by, bz ) or 0
	local b = MC.Blocks[id]

	print( string.format( "[probe] cell(%d,%d,%d) chunk(%d,%d,%d) li=%d", bx, by, bz, cx, cy, cz, li ) )
	print( string.format( "[probe] lua world: id=%d orient=%d name=%s", id, orient, b and b.name or "?" ) )
	if b then
		print( string.format( "[probe]   shape=%s transparent=%s baked=%s solid=%s orientRule=%s",
			tostring( b.shape ), tostring( b.transparent ), tostring( b.baked ~= nil ),
			tostring( b.solid ), tostring( b.orient ) ) )
		print( string.format( "[probe]   BlockIsFullCube(该orient)=%s IsTranslucent=%s IsLiquid=%s",
			tostring( MC.BlockIsFullCube and MC.BlockIsFullCube( id, orient ) ),
			tostring( MC.IsTranslucentBlock and MC.IsTranslucentBlock( id ) ),
			tostring( MC.IsLiquidBlock and MC.IsLiquidBlock( id ) ) ) )
		local t1, r1 = MC.BlockFace( id, 1, orient )
		local t3, r3 = MC.BlockFace( id, 3, orient )
		print( string.format( "[probe]   BlockFace top=(%s,%s) side=(%s,%s)",
			tostring( t1 ), tostring( r1 ), tostring( t3 ), tostring( r3 ) ) )
	end

	if not mcmesh then print( "[probe] mcmesh 未加载" ) return end

	local d = mcmesh.DebugBlockDef and mcmesh.DebugBlockDef( id )
	if istable( d ) then
		print( string.format( "[probe] native defs: buildable=%s transparent=%s fullCubeAll=%s flags=%d",
			tostring( d.buildable ), tostring( d.transparent ), tostring( d.fullCubeAll ), d.flags or -1 ) )
		local od = d.orients and d.orients[orient]
		if od then
			local f1 = od.faces and od.faces[1]
			local f3 = od.faces and od.faces[3]
			print( string.format( "[probe]   orient %d: fullCube=%s top tile=%s rot=%s / side tile=%s rot=%s",
				orient, tostring( od.fullCube ),
				f1 and f1.tile or "?", f1 and f1.rot or "?",
				f3 and f3.tile or "?", f3 and f3.rot or "?" ) )
		else
			print( string.format( "[probe]   orient %d 无条目(会回退默认条目)", orient ) )
		end
	else
		print( "[probe] native defs: id " .. id .. " 不在表里!" )
	end

	local mid, morient = mcmesh.DebugGetCell and mcmesh.DebugGetCell( cx, cy, cz, li )
	print( string.format( "[probe] native mirror: id=%s orient=%s %s",
		tostring( mid ), tostring( morient ),
		( mid == id ) and "(与世界一致)" or "(!! 与世界不一致)" ) )

	local lx = li % 16
	local ly = math.floor( li / 16 ) % 16
	local lz = math.floor( li / 256 )
	local sec = math.floor( lx / 8 ) + math.floor( ly / 8 ) * 2 + math.floor( lz / 8 ) * 4
	local r = mcmesh.DebugBuildSection and mcmesh.DebugBuildSection( cx, cy, cz, sec )
	print( string.format( "[probe] native section %d: faces=%s verts=%s buildUs=%s",
		sec, istable( r ) and r.faces or "?", istable( r ) and r.verts or "?",
		istable( r ) and string.format( "%.1f", r.buildUs or 0 ) or "?" ) )

	local ok, s = pcall( mcmesh.GetStats )
	if ok and istable( s ) then
		print( string.format( "[probe] stats: meshes=%s vertices=%s pending=%s mirrorChunks=%s",
			tostring( s.meshes ), tostring( s.vertices ), tostring( s.pendingSections ),
			tostring( s.mirrorChunks ) ) )
	end
end, nil, "Probe one missing block through the whole native pipeline" )

print( "[probe] loaded — 准星对着问题方块, 运行: mc_native_probe" )
