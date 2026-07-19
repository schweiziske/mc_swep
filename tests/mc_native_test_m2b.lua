--[[----------------------------------------------------------------------------
	mc_native_test_m2b.lua —— M2b 验收: fullCube-only 逐 section 面数回归 + 建面耗时

	用法: 激活 native、世界安静后(mc_native_status 里 pendingSections=0):
	    lua_openscript_cl mc_native_test_m2b.lua
	    mc_native_test_m2b            -- 全图
	    mc_native_test_m2b 30         -- 只抽前 30 个 chunk(大地图省时间)

	做什么: 对每个已加载 chunk 的每个 section, 仅用 fullCube supportSet +
	visibleAt 剔面语义在 Lua 侧数一遍 fullCube 应有面数，并与 M5 debug 的
	`emitterFaces.fullCube` 比对。此脚本不重建 cross/model/shape/connection/liquid，
	也不对 aggregate pass totals 或 normalized vertex packet 作权威比较。

	因此 M5 输出只能是 fullCube scoped OK；其余五类 emitter、pass totals 和
	vertex packet 必须明确 UNVERIFIED。全 emitter 几何 parity 仍需固定视觉/编辑矩阵。
	同时汇报 native 建面耗时 max/avg(验收线: < 300us/section)。

	注意: 必须在世界静止时跑(推送未完成或有编辑在途会产生瞬时假差异)。
------------------------------------------------------------------------------]]

if SERVER then return end

local FACE_OFF = {
	{ 0, 0, 1 },   -- TOP
	{ 0, 0, -1 },  -- BOTTOM
	{ 1, 0, 0 },   -- XP
	{ -1, 0, 0 },  -- XN
	{ 0, 1, 0 },   -- YP
	{ 0, -1, 0 },  -- YN
}

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

-- baked 归一化判定(与适配器 packBlockDefs / m2a 脚本同款, 2026-07-18)
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

local function faceCanonical( b, id, o, f )
	local bf = bakedFaceFor( b, o, f )
	if not bf then return true end
	if bf.uvs then return canonicalizeUvs( bf.uvs ) ~= nil end
	return isFullRect( bf.uv )
end

local function buildSupportSet()
	local set = {}
	for id, b in pairs( MC.Blocks ) do
		if type( id ) == "number" and id >= 1 and id <= 65535 and b
			and b.shape ~= "cross" and b.shape ~= "model"
			and not ( MC.IsLiquidBlock and MC.IsLiquidBlock( id ) ) then
			local ok = true
			for _, o in ipairs( orientsForBlock( b ) ) do
				if not MC.BlockIsFullCube( id, o ) then ok = false break end
				for f = 1, 6 do
					if not faceCanonical( b, id, o, f ) then ok = false break end
				end
				if not ok then break end
			end
			if ok then
				set[id] = ( MC.IsTranslucentBlock and MC.IsTranslucentBlock( id ) ) and 1 or 0
			end
		end
	end
	return set
end

-- visibleAt 复刻(cl_mesh_build_context.lua:191-199; cur 恒为不透明非液体
-- full-cube, sameLiquidKind 分支恒 false 故省略)
local function faceVisible( cur, nb, nbOrientNorm )
	if nb == 0 then return true end
	local d = MC.Blocks[nb]
	if not d then return true end
	if d.transparent then return nb ~= cur end
	if not MC.BlockIsFullCube( nb, nbOrientNorm ) then return true end
	return false
end

concommand.Add( "mc_native_test_m2b", function( _, _, args )
	if not ( mcmesh and mcmesh.DebugBuildSection ) then
		print( "[m2b] mcmesh.DebugBuildSection 不存在(模块未加载或未更新)" )
		return
	end
	if not ( MC and MC.World and MC.Blocks and MC.BlockIsFullCube and MC.GetBlock ) then
		print( "[m2b] MC 未就绪" )
		return
	end

	local CS = MC.CS or 16
	local CH = MC.CH or 32
	local SS = 8                          -- 与握手一致
	local NSX = math.ceil( CS / SS )
	local NSZ = math.ceil( CH / SS )
	local SPC = NSX * NSX * NSZ
	local maxChunks = tonumber( args and args[1] or "" ) or math.huge

	local support = buildSupportSet()
	local normalizeOrient = MC.NormalizeOrient

	local chunksDone, secCompared, secMismatch = 0, 0, 0
	local typedSections, packetSections = 0, 0
	local emitterFaces = { fullCube=0, cross=0, model=0, shape=0, connection=0, liquid=0 }
	local firstReports = {}
	local usMax, usSum, usN = 0, 0, 0

	for key, chunk in pairs( MC.World ) do
		if chunksDone >= maxChunks then break end
		if chunk and ( chunk.count or 0 ) > 0 then
			local cx, cy, cz = MC.ChunkKeyToCoords( key )
			if cx then
				local blocks = chunk.blocks or {}
				local orients = chunk.orients or {}
				local bxo, byo, bzo = cx * CS, cy * CS, cz * CH

				for sec = 0, SPC - 1 do
					local sx = sec % NSX
					local sy = math.floor( sec / NSX ) % NSX
					local sz = math.floor( sec / ( NSX * NSX ) )
					local luaOpaqueFaces, luaTranslucentFaces = 0, 0

					for lz = sz * SS, sz * SS + SS - 1 do
					for ly = sy * SS, sy * SS + SS - 1 do
					for lx = sx * SS, sx * SS + SS - 1 do
						local li = lx + ly * CS + lz * CS * CS
						local id = blocks[li]
						if id and support[id] then
							for f = 1, 6 do
								local off = FACE_OFF[f]
								local nlx, nly, nlz = lx + off[1], ly + off[2], lz + off[3]
								local nb, nbo
								if nlx >= 0 and nlx < CS and nly >= 0 and nly < CS
									and nlz >= 0 and nlz < CH then
									local nli = nlx + nly * CS + nlz * CS * CS
									nb = blocks[nli] or 0
									if nb ~= 0 then
										nbo = normalizeOrient and normalizeOrient( nb, orients[nli] ) or ( orients[nli] or 0 )
									end
								else
									local bx, by, bz = bxo + nlx, byo + nly, bzo + nlz
									nb = MC.GetBlock( bx, by, bz ) or 0
									if nb ~= 0 then
										nbo = ( MC.GetBlockOrient and MC.GetBlockOrient( bx, by, bz ) ) or 0
									end
								end
								if faceVisible( id, nb, nbo or 0 ) then
									if support[id] == 1 then
										luaTranslucentFaces = luaTranslucentFaces + 1
									else
										luaOpaqueFaces = luaOpaqueFaces + 1
									end
								end
							end
						end
					end
					end
					end

					local r = mcmesh.DebugBuildSection( cx, cy, cz, sec )
					local typedEmitterSum
					if istable(r) then
						local ec = r.emitterFaces or r.emitters
						if istable(ec) then
							typedSections = typedSections + 1
							typedEmitterSum = 0
							for kind in pairs(emitterFaces) do
								local n = tonumber(ec[kind]) or 0
								emitterFaces[kind] = emitterFaces[kind] + n
								typedEmitterSum = typedEmitterSum + n
							end
						end
						if r.vertexPacket ~= nil or r.vertexHash ~= nil or r.opaqueVertexPacket ~= nil then packetSections = packetSections + 1 end
					end
					local nativeOpaque = istable( r ) and r.opaqueFaces or -1
					local nativeTranslucent = istable( r ) and r.translucentFaces or -1
					local totalFaces = luaOpaqueFaces + luaTranslucentFaces
					local fieldsOk
					if typedEmitterSum ~= nil then
						-- M5 all-emitter builds cannot be compared to the old full-cube-only
						-- replica. Keep this as a focused full-cube regression and use the
						-- native per-emitter census; aggregate/pass parity belongs to cached
						-- MC.MeshSectionGeometry when that debug packet API is available.
						local ec = r.emitterFaces or r.emitters
						fieldsOk = (tonumber(ec.fullCube) or 0) == totalFaces
					else
						fieldsOk = istable( r ) and r.faces == totalFaces
							and r.verts == totalFaces * 6
							and r.opaqueVerts == luaOpaqueFaces * 6
							and r.translucentVerts == luaTranslucentFaces * 6
					end
					secCompared = secCompared + 1
					local countsMismatch = typedEmitterSum == nil
						and (nativeOpaque ~= luaOpaqueFaces or nativeTranslucent ~= luaTranslucentFaces)
					if countsMismatch or not fieldsOk then
						secMismatch = secMismatch + 1
						if #firstReports < 20 then
							firstReports[#firstReports + 1] = string.format(
								"chunk(%d,%d,%d) sec %d: opaque lua=%d native=%s, trans lua=%d native=%s",
								cx, cy, cz, sec, luaOpaqueFaces, tostring( nativeOpaque ),
								luaTranslucentFaces, tostring( nativeTranslucent ) )
						end
					end
					if istable( r ) and r.buildUs then
						usN = usN + 1
						usSum = usSum + r.buildUs
						if r.buildUs > usMax then usMax = r.buildUs end
					end
				end
				chunksDone = chunksDone + 1
			end
		end
	end

	print( string.format( "[m2b] chunks=%d sections=%d mismatch=%d typedSections=%d vertexPacketSections=%d", chunksDone, secCompared, secMismatch, typedSections, packetSections ) )
	if typedSections > 0 then
		print(string.format("[m2b] native emitter census (diagnostic only) fullCube=%d cross=%d model=%d shape=%d connection=%d liquid=%d",
			emitterFaces.fullCube,emitterFaces.cross,emitterFaces.model,emitterFaces.shape,emitterFaces.connection,emitterFaces.liquid))
		print("[m2b] scope: fullCube face-count parity only (Lua fullCube replica vs native emitterFaces.fullCube)")
		print("[m2b] UNVERIFIED geometry parity: cross, model, shape, connection, liquid")
		print("[m2b] UNVERIFIED pass parity: opaque/translucent totals are not compared for all emitters")
		if packetSections > 0 then
			print("[m2b] UNVERIFIED vertex packet parity: packet/hash present, but no Lua normalized packet comparator is implemented")
		else
			print("[m2b] UNVERIFIED vertex packet parity: no packet/hash exposed on sampled sections")
		end
	else
		print("[m2b] legacy debug API: counts cover only the historical fullCube supportSet")
	end
	for _, l in ipairs( firstReports ) do print( "[m2b]   " .. l ) end
	if usN > 0 then
		print( string.format( "[m2b] buildUs avg=%.1f max=%.1f (%d sections, 验收线 <300us)",
			usSum / usN, usMax, usN ) )
	end
	if secMismatch == 0 and secCompared > 0 then
		if typedSections > 0 then
			print( "[m2b] fullCube scope OK —— sampled sections fullCube face counts match" )
			print( "[m2b] M5 GEOMETRY PARITY UNVERIFIED —— run the visual/edit/boundary fixed matrix for all emitters" )
		else
			print( "[m2b] legacy fullCube scope OK —— sampled supportSet aggregate counts match" )
		end
	elseif secCompared == 0 then
		print( "[m2b] UNVERIFIED —— no sections were compared" )
	else
		print( "[m2b] fullCube scope FAIL —— see mismatches above" )
	end
end, nil, "M2b: fullCube-only per-section face-count regression (not M5 all-emitter parity)" )

print( "[m2b] loaded — run: mc_native_test_m2b [maxChunks]" )
