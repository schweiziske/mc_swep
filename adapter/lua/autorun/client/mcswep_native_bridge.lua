--[[----------------------------------------------------------------------------
	mcswep_native_bridge.lua —— MCSWEP 原生建面后端适配器(独立插件, 不改原项目)

	安装: 本文件随 gmcl_mcswep_native_mesh DLL 发布, 作为独立 addon:
	    garrysmod/addons/mcswep-native/lua/autorun/client/mcswep_native_bridge.lua
	    garrysmod/lua/bin/gmcl_mcswep_native_mesh_win64.dll (或 _win32)
	原项目(MCSWEP)零改动。没装 DLL / 握手失败 / 原项目不存在时本文件不做任何事。

	原理(运行时补丁 + 代理 mesh, 契约见原生仓库 INTERFACE.md v1):
	  1. 包一层 MC.BuildChunkMesh / MC.RebuildChunkMeshLighting / MC.FreeChunkMesh。
	     激活后 Lua 不再构建 IMesh; 每次重建请求把该 chunk 打包推给 mcmesh
	     (ApplyChunk 是权威快照, native 自行 diff 出脏 section 并在工作线程重建)。
	  2. 往 MC.Meshes / MC.MeshesTrans 塞代理对象: 其 :Draw() 转调
	     mcmesh.DrawChunk(cx,cy,cz,pass)。cl_draw 的主循环、shadow map、
	     heightfield 都只对条目调 :Draw(), 材质/矩阵/渲染状态由原管线设置,
	     C++ 侧只画不绑 —— 三条消费路径零改动自动导流。
	  3. 每帧 mcmesh.Think(预算) 落地工作线程结果。
	  4. 任一 native 调用失败 -> 解除全部补丁、清代理、全图交还 Lua 路径重建。

	ConVar / 指令:
	  mc_native_mesh        1=启用(默认), 0=禁用并回退 Lua
	  mc_native_think_ms    每帧落地预算, 默认 3.0
	  mc_native_toggle      A/B 热切换两种后端(即时, 带耗时日志)
	  mc_native_status      打印当前后端与模块统计
------------------------------------------------------------------------------]]

if SERVER then return end

local CONVAR_ENABLED = CreateClientConVar( "mc_native_mesh", "1", true, false,
	"MCSWEP native mesh backend: 1 = use mcswep_native_mesh module when available, 0 = pure Lua" )
local CONVAR_THINK_MS = CreateClientConVar( "mc_native_think_ms", "3", true, false,
	"Per-frame budget (ms) for native mesh finalization" )

local ADAPTER_ABI = 3   -- M5.6 MCBD v4 visual transform format v2

local state = {
	active = false,
	failed = false,          -- 渲染中途置位, Think 里统一处理回退
	shuttingDown = false,
	shutdownCalled = false,
	originals = nil,         -- { BuildChunkMesh=, RebuildChunkMeshLighting=, FreeChunkMesh= }
	proxied = {},            -- key -> true (我们接管的 chunk)
	pushedFrame = {},        -- key -> FrameNumber (chunk 推送去重)
	defCache = nil,          -- immutable registry/catalog MCBD v4 blob for A/B toggles
}

local function log( msg )
	print( "[mcswep-native] " .. tostring( msg ) )
end

-- native 顶点携带固定 GPU payload(R=255/G=faceIndex/B=0/A=255), 只对 GPU light
-- texture 光照档案安全。档案的采样坐标由像素 shader 从 worldPos 反推, 顶点不带
-- 光照 UV —— 其它光照风格(vanilla/fancy 顶点烘焙)会因此错光。此门槛拦住它们。
local function gpuLightProfileActive()
	if not MC.CurrentLightProfile then return false end
	local ok, profile = pcall( MC.CurrentLightProfile )
	if not ok or not profile then return false end
	local mat = profile.gpuPayloadMaterial
	if type( mat ) == "function" then
		local ok2, m = pcall( mat, profile )
		mat = ok2 and m or nil
	end
	return mat ~= nil
end

----------------------------------------------------------------- blockdefs 打包
-- MCBD v4 keeps the M5 typed legacy emitters and adds a complete stateId map.
-- Generated state plans are migrated in the next protocol stage.
local string_char = string.char
local bit_band, bit_rshift = bit.band, bit.rshift
local HUGE = math.huge

local function finite( n ) return type( n ) == "number" and n == n and n ~= HUGE and n ~= -HUGE end
local function checkedNumber( value, lo, hi, label )
	local n = tonumber( value )
	if not finite( n ) or n < lo or n > hi then error( label .. " out of range", 0 ) end
	return n
end
local function checkedInt( value, lo, hi, label )
	local n = checkedNumber( value, lo, hi, label )
	if n ~= math.floor( n ) then error( label .. " is not integral", 0 ) end
	return n
end
local function u8( n )
	n = checkedInt( n, 0, 255, "u8" )
	return string_char( n )
end
local function u16le( n )
	n = checkedInt( n, 0, 65535, "u16" )
	return string_char( bit_band( n, 0xFF ), bit_band( bit_rshift( n, 8 ), 0xFF ) )
end
local function u32le( n )
	n = checkedInt( n, 0, 4294967295, "u32" )
	return string_char( bit_band( n, 0xFF ), bit_band( bit_rshift( n, 8 ), 0xFF ),
		bit_band( bit_rshift( n, 16 ), 0xFF ), bit_band( bit_rshift( n, 24 ), 0xFF ) )
end

-- IEEE-754 binary32 LE; reject values which would become NaN/Inf/subnormal.
local function f32le( value )
	local x = checkedNumber( value, -16777216, 16777216, "f32" )
	if x == 0 then return string_char( 0, 0, 0, x < 0 and 128 or 0 ) end
	local sign = 0
	if x < 0 then sign = 0x80; x = -x end
	local mant, expo = math.frexp( x )
	expo = expo - 1
	local e = expo + 127
	if e <= 0 or e >= 255 then error( "f32 exponent out of range", 0 ) end
	local m = math.floor( ( mant * 2 - 1 ) * 2 ^ 23 + 0.5 )
	if m >= 2 ^ 23 then m = 0; e = e + 1 end
	if e >= 255 then error( "f32 overflow", 0 ) end
	return string_char( bit_band( m, 0xFF ), bit_band( bit_rshift( m, 8 ), 0xFF ),
		bit_band( bit_rshift( m, 16 ), 0x7F ) + bit_band( e * 128, 0x80 ), sign + math.floor( e / 2 ) )
end

local EMITTER = { fullCube = 1, cross = 2, model = 3, shape = 4, connection = 5, liquid = 6 }
local LIQUID = { water = 1, lava = 2 }
local CONNECTION = { fence = 1, pane = 2, bars = 3, wall = 4, pipe6 = 5 }
local FACE_TOP, FACE_BOTTOM, FACE_XP, FACE_XN, FACE_YP, FACE_YN = 1, 2, 3, 4, 5, 6
local TRI = { 1, 3, 2, 1, 4, 3 }
local MAX_BLOCKS, MAX_ORIENTS = 65535, 64
local MAX_BOXES, MAX_QUADS, MAX_TEMPLATES = 1000000, 4000000, 1000000
local MAX_BLOB_BYTES = 512 * 1024 * 1024

local function orientsForBlock( b )
	local rule = b.orient and MC.OrientRules and MC.OrientRules[b.orient]
	if not rule or not rule.allowed then return { 0 } end
	local def = checkedInt( ( MC.DefaultOrient and MC.DefaultOrient( b.id ) ) or rule.default or 0, 0, 255, "default orient" )
	local rest, seen = {}, { [def] = true }
	for raw in pairs( rule.allowed ) do
		local o = checkedInt( raw, 0, 255, "orient" )
		if not seen[o] then rest[#rest + 1] = o; seen[o] = true end
	end
	table.sort( rest )
	if #rest + 1 > MAX_ORIENTS then error( "too many orients for block " .. b.id, 0 ) end
	local list = { def }
	for i = 1, #rest do list[#list + 1] = rest[i] end
	return list
end

local function connectionKind( b, id )
	if not b then return nil end
	if CONNECTION[b.connection] then return b.connection end
	if MC.IsFenceBlock and MC.IsFenceBlock( id ) then return "fence" end
	if MC.IsPaneBlock and MC.IsPaneBlock( id ) then return b.connection == "bars" and "bars" or "pane" end
	if MC.IsWallBlock and MC.IsWallBlock( id ) then return "wall" end
end

local function modelUsesConnection( b, id ) return connectionKind( b, id ) ~= nil end

local function capabilitiesFor( b, id, orient )
	local generic = b and ( b.connection == "fence" or b.connection == "fence_gate"
		or b.connection == "wall" or b.connection == "pane" or b.connection == "bars" )
	if not generic and b and b.solid ~= false then
		generic = MC.BlockIsFullCube and MC.BlockIsFullCube( id, orient ) or not b.shape
	end
	local caps = generic and 0x07 or 0
	if b and ( b.connection == "pipe6" or b.name == "chorus_flower" ) then caps = caps + 0x08 end
	if b and b.name == "end_stone" then caps = caps + 0x10 end
	return caps
end

local function faceCorners( box, face )
	local x0, y0, z0, x1, y1, z1 = unpack( box, 1, 6 )
	if face == FACE_TOP then return { {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1} } end
	if face == FACE_BOTTOM then return { {x0,y1,z0},{x1,y1,z0},{x1,y0,z0},{x0,y0,z0} } end
	if face == FACE_XP then return { {x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1} } end
	if face == FACE_XN then return { {x0,y1,z0},{x0,y0,z0},{x0,y0,z1},{x0,y1,z1} } end
	if face == FACE_YP then return { {x1,y1,z0},{x0,y1,z0},{x0,y1,z1},{x1,y1,z1} } end
	return { {x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1} }
end
local function projectedUV( face, x, y, z )
	if face == FACE_TOP then return x * 16, ( 1 - y ) * 16 end
	if face == FACE_BOTTOM then return x * 16, y * 16 end
	if face == FACE_XP then return y * 16, ( 1 - z ) * 16 end
	if face == FACE_XN then return ( 1 - y ) * 16, ( 1 - z ) * 16 end
	if face == FACE_YP then return ( 1 - x ) * 16, ( 1 - z ) * 16 end
	return x * 16, ( 1 - z ) * 16
end
local function rotateFlags( u, v, rot, flipU, flipV )
	rot = math.floor( tonumber( rot ) or 0 ) % 4
	if rot == 1 then u, v = v, 1 - u elseif rot == 2 then u, v = 1 - u, 1 - v elseif rot == 3 then u, v = 1 - v, u end
	if flipU then u = 1 - u end
	if flipV then v = 1 - v end
	return u, v
end

-- Quad88: face:u8 flags:u8 boxIndex:u16 tile:u16 reserved:u16,
-- then four {x,y,z,u,v}:f32. Quad positions are bounded block-local extension
-- coordinates [-16,16] (models legitimately extend outside the unit cube); UVs 0..16.
local function packQuad( face, flags, boxIndex, tile, verts, context )
	face, flags = checkedInt( face, 1, 6, "quad face" ), checkedInt( flags or 0, 0, 1, "quad flags" )
	boxIndex, tile = checkedInt( boxIndex or 65535, 0, 65535, "box index" ), checkedInt( tile or 0, 0, 65535, "tile" )
	if type( verts ) ~= "table" or #verts ~= 4 then error( ( context or "quad" ) .. " needs four vertices", 0 ) end
	local p = { u8( face ), u8( flags ), u16le( boxIndex ), u16le( tile ), u16le( 0 ) }
	for i = 1, 4 do
		local v = verts[i]
		local label = ( context or "quad" ) .. " vertex " .. i
		p[#p+1] = f32le( checkedNumber( v[1], -16, 16, label .. " x" ) )
		p[#p+1] = f32le( checkedNumber( v[2], -16, 16, label .. " y" ) )
		p[#p+1] = f32le( checkedNumber( v[3], -16, 16, label .. " z" ) )
		p[#p+1] = f32le( checkedNumber( v[4], 0, 16, label .. " u" ) )
		p[#p+1] = f32le( checkedNumber( v[5], 0, 16, label .. " v" ) )
	end
	return table.concat( p )
end
local function packBox( box )
	if type( box ) ~= "table" then error( "invalid box", 0 ) end
	local p = {}
	for i = 1, 6 do p[i] = f32le( checkedNumber( box[i], 0, 1, "box coordinate" ) ) end
	if box[1] > box[4] or box[2] > box[5] or box[3] > box[6] then error( "inverted box", 0 ) end
	return table.concat( p )
end

-- FaceTex36: tile:u16 flags:u8 reserved:u8 + four tile-local {u,v}:f32.
local function packFaceTex( tile, uvs, flags )
	local p = { u16le( checkedInt( tile or 0, 0, 65535, "tile" ) ), u8( flags or 0 ), u8( 0 ) }
	for i = 1, 4 do
		p[#p+1] = f32le( checkedNumber( uvs[i][1], 0, 16, "face u" ) )
		p[#p+1] = f32le( checkedNumber( uvs[i][2], 0, 16, "face v" ) )
	end
	return table.concat( p )
end
local FACE_UV_FLAGS = { {0,16},{16,16},{16,0},{0,0} }
local function resolveFaceTex( b, id, orient, face )
	local baked = b.baked and ( b.baked[orient] or b.baked[MC.DefaultOrient( id )] or b.baked[0] )
	baked = baked and baked[face]
	local tile, rot, fu, fv, rect, raw
	if baked then tile, rot, rect, raw = baked.tile or 0, baked.rot or 0, baked.uv, baked.uvs
	else tile, rot, fu, fv = MC.BlockFace( id, face, orient ) end
	local out = {}
	if raw then
		for i = 1, 4 do out[i] = { raw[i][1], raw[i][2] } end
	else
		local u0, v0, u1, v1 = 0, 0, 16, 16
		if rect then u0,v0,u1,v1 = rect[1] or 0,rect[2] or 0,rect[3] or 16,rect[4] or 16 end
		for i = 1, 4 do
			local uf, vf = rotateFlags( FACE_UV_FLAGS[i][1] / 16, FACE_UV_FLAGS[i][2] / 16, rot, fu, fv )
			out[i] = { uf == 1 and u1 or u0, vf == 1 and v1 or v0 }
		end
	end
	return checkedInt( tile or 0, 0, 65535, "face tile" ), out
end

local function shapeQuad( b, id, orient, box, boxIndex, face, fd, explicitUV, flags )
	local tile = fd and fd.tile or MC.BlockFace( id, face, orient )
	local corners, verts = faceCorners( box, face ), {}
	local rect = explicitUV or ( fd and fd.uv )
	for i = 1, 4 do
		local x,y,z = corners[i][1],corners[i][2],corners[i][3]
		local u,v
		if rect then
			local uf,vf = rotateFlags( FACE_UV_FLAGS[i][1]/16, FACE_UV_FLAGS[i][2]/16, explicitUV and 0 or fd.rot )
			u = uf == 1 and (rect[3] or 16) or (rect[1] or 0); v = vf == 1 and (rect[4] or 16) or (rect[2] or 0)
		else u,v = projectedUV( face, x,y,z ) end
		verts[i] = {x,y,z,u,v}
	end
	return packQuad( face, flags or 0, boxIndex, tile, verts,
		"block " .. tostring( id ) .. " " .. tostring( b and b.name or "?" )
		.. " orient " .. tostring( orient ) .. " shape box " .. tostring( boxIndex ) .. " face " .. tostring( face ) )
end

local function modelQuads( b, orient )
	local RM = MC.RenderMesh
	local quads = RM and RM.rotatedModelQuads and RM.rotatedModelQuads( b, orient )
	if not quads then return nil end
	local out = {}
	for qi, q in ipairs( quads ) do
		if q.verts and q.verts[1] and q.verts[2] and q.verts[3] and q.verts[4] then
			local rect = q.uv or {0,0,16,16}; local verts = {}
			for i = 1, 4 do
				local x, y, z, rawU, rawV = MC.ModelQuadVertex( q, i )
				if not finite( x ) or not finite( y ) or not finite( z ) or not finite( rawU ) or not finite( rawV ) then
					error( "invalid model quad vertex", 0 )
				end
				local uf,vf = rotateFlags( rawU, rawV, q.rot or 0 )
				if b.orient == "door" then uf = 1 - uf end
				verts[i] = {x,y,z,uf == 1 and rect[3] or rect[1],vf == 1 and rect[4] or rect[2]}
			end
			out[#out+1] = packQuad( q.face or FACE_TOP, 0, 65535, q.tile or 0, verts,
				"block " .. tostring( b.id ) .. " " .. tostring( b.name or "?")
				.. " orient " .. tostring( orient ) .. " model quad " .. tostring( qi ) )
		end
	end
	return out
end

local function connectionFaceValues( b, face, box, orient )
	local RM = MC.RenderMesh
	if RM and RM.connectionShapeFaceValues then
		local tile, uv, ok = RM.connectionShapeFaceValues( b, face, box )
		if ok then return tile, uv end
	end
	return MC.BlockFace( b.id, face, orient ), nil
end
local function boundaryBit( box, face )
	local e = 0.0001
	if face == FACE_TOP and math.abs(box[6]-1)<=e then return 32 end
	if face == FACE_BOTTOM and math.abs(box[3])<=e then return 16 end
	if face == FACE_XP and math.abs(box[4]-1)<=e then return 8 end
	if face == FACE_XN and math.abs(box[1])<=e then return 4 end
	if face == FACE_YP and math.abs(box[5]-1)<=e then return 2 end
	if face == FACE_YN and math.abs(box[2])<=e then return 1 end
	return 0
end

local function buildConnectionTemplates( b, id, orient, kind )
	local RM, out = MC.RenderMesh, {}
	local maxMask = kind == "pipe6" and 63 or 15
	for mask = 0, maxMask do
		local boxKind = kind == "bars" and "pane" or kind
		local boxes = MC.ConnectionBlockBoxes( boxKind, mask )
		if not boxes then error( "missing connection boxes", 0 ) end
		local bp, qp = {}, {}
		for i, box in ipairs( boxes ) do bp[i] = packBox( box ) end
		for bi, box in ipairs( boxes ) do
			for face = 1, 6 do
				local bitv = boundaryBit( box, face )
				local connected = bitv ~= 0 and bit_band( mask, bitv ) ~= 0
				local selfCovered = RM and RM.coveredByBoxes and #boxes > 1 and RM.coveredByBoxes( box, face, boxes, 0,0,0 )
				if not connected and not selfCovered then
					local tile, uv = connectionFaceValues( b, face, box, orient )
					local raw = shapeQuad( b, id, orient, box, bi - 1, face, {tile=tile}, uv )
					-- bit0 asks worker to run neighbor coverage (only vertical connection faces).
					if bitv == 16 or bitv == 32 then raw = raw:sub(1,1) .. u8(1) .. raw:sub(3) end
					qp[#qp+1] = raw
				end
			end
		end
		out[#out+1] = u8(mask)..u8(0)..u16le(#boxes)..u32le(#qp)..table.concat(bp)..table.concat(qp)
	end
	return out
end

local function staticShapePayload( b, id, orient )
	local boxes = MC.BlockBoxes( id, orient )
	if not boxes then return nil end
	local bp, qp = {}, {}
	for i, box in ipairs( boxes ) do bp[i] = packBox( box ) end
	for bi, box in ipairs( boxes ) do
		local faces = b.boxFaces and b.boxFaces[bi]
		for face = 1, 6 do
			local fd = faces and faces[face]
			if not b.boxFaces or fd then
				local flags = boundaryBit( box, face ) ~= 0 and 1 or 0
				qp[#qp+1] = shapeQuad( b,id,orient,box,bi-1,face,fd,nil,flags )
			end
		end
	end
	return bp, qp
end

local function hexHashBytes( value, label )
	value = tostring( value or "" )
	if #value ~= 64 or value:find( "[^0-9a-fA-F]" ) then error( label .. " must be 64 hex characters", 0 ) end
	local out = {}
	for i = 1, 64, 2 do out[#out + 1] = string_char( tonumber( value:sub( i, i + 1 ), 16 ) ) end
	return table.concat( out )
end

local function stateMapPayload()
	if not MC.BlockStateRegistryFrozen then error( "block state registry is not frozen", 0 ) end
	local count = checkedInt( MC.BlockStateCount or 0, 1, 1048576, "state count" )
	local parts = {}
	for stateId = 0, count - 1 do
		local blockId, orient, stairPlans = 0, 0, {0,0,0,0,0}
		if stateId ~= 0 then
			local exact, projectionError
			blockId, orient, exact, projectionError = MC.LegacyFromState( stateId )
			if not blockId then error( "state " .. stateId .. " projection failed: " .. tostring( projectionError ), 0 ) end
			local block = MC.Blocks and MC.Blocks[blockId]
			local stateFlags = exact and 1 or 0
			local visual = MC.ResolveBlockStateVisual and MC.ResolveBlockStateVisual( stateId ) or nil
			local stateFullCube = visual and MC.BlockVisualIsFullCube and MC.BlockVisualIsFullCube( visual ) == true
				or ( not visual and MC.BlockIsFullCube and MC.BlockIsFullCube( blockId, orient ) == true )
			if stateFullCube then stateFlags = stateFlags + 4 end
			if not visual or visual.cullGeneratedFaces ~= false then stateFlags = stateFlags + 8 end
			if block and block.shape == "stairs" and MC.StateWith and MC.StateVisualPlanIdFast then
				stateFlags = stateFlags + 2
				for index, shape in ipairs( { "straight", "inner_left", "inner_right", "outer_left", "outer_right" } ) do
					local shaped = MC.StateWith( stateId, "shape", shape )
					local planId = shaped and MC.StateVisualPlanIdFast( shaped ) or nil
					stairPlans[index] = checkedInt( planId or 0, 1, 65535, "stair plan" )
				end
			end
			parts[#parts + 1] = u32le( stateId ) .. u16le( checkedInt( blockId, 1, 65535, "state block id" ) )
				.. u8( checkedInt( orient or 0, 0, 255, "state orient" ) ) .. u8( stateFlags ) .. u8( 0 )
				.. u16le(stairPlans[1])..u16le(stairPlans[2])..u16le(stairPlans[3])..u16le(stairPlans[4])..u16le(stairPlans[5])
		else
			parts[#parts + 1] = u32le( 0 ) .. u16le( 0 ) .. u8( 0 ) .. u8( 0 ) .. u8( 0 ) .. string.rep( "\0", 10 )
		end
	end
	return table.concat( parts ), count
end

local function visualCatalogPayload( totalStateCount )
	local c = MC.StateVisualCatalog
	if type( c ) ~= "table" then return "", 0 end
	local function count( field, hi ) return checkedInt( c[field] or 0, 0, hi, "visual " .. field ) end
	local cs=count("stateCount",totalStateCount); local gc=count("geometryTemplateCount",65535); local gq=count("geometryQuadCount",4000000)
	local sc=count("surfaceSetCount",65535); local se=count("surfaceEntryCount",4000000); local mc=count("modelInstanceCount",65535)
	local cc=count("choiceGroupCount",65535); local ca=count("choiceAlternativeCount",65535); local pc=count("visualPlanCount",65535); local pg=count("visualPlanGroupCount",65535)
	local fields={{"geometryIndex",gc*6},{"geometryData",gq*32},{"surfaceIndex",sc*6},{"surfaceData",se*2},{"modelMap",mc*4},{"groupIndex",cc*3},{"groupData",ca*3},{"planIndex",pc*3},{"planData",pg*2},{"statePlans",cs*2}}
	local data={u32le(checkedInt(c.coordinateScale or 0,1,1048576,"coordinate scale")),u32le(checkedInt(c.packedQuadBytes or 0,1,256,"packed quad bytes")),u32le(cs),u32le(gc),u32le(gq),u32le(sc),u32le(se),u32le(mc),u32le(cc),u32le(ca),u32le(pc),u32le(pg)}
	for _,f in ipairs(fields) do local raw=c[f[1]];if type(raw)~="string" or #raw~=f[2] then error("visual "..f[1].." length mismatch",0) end;data[#data+1]=raw end
	local tintSeen,tintIndexes={},{}
	for offset=1,#c.geometryData,32 do local packed=c.geometryData:byte(offset) or 0;local tv=bit_rshift(packed,3);if tv>0 and not tintSeen[tv-1] then tintSeen[tv-1]=true;tintIndexes[#tintIndexes+1]=tv-1 end end
	table.sort(tintIndexes);if #tintIndexes>31 then error("too many visual tint indexes",0) end
	data[#data+1]=u8(#tintIndexes);for _,ti in ipairs(tintIndexes)do data[#data+1]=u8(ti) end
	for stateId=0,cs-1 do local blockId=MC.BlockTypeForState and MC.BlockTypeForState(stateId) or 0;for _,ti in ipairs(tintIndexes)do local code=MC.BlockStateTintCode and MC.BlockStateTintCode(stateId,blockId,ti) or 0;data[#data+1]=u8(checkedInt(code,0,255,"state tint code")) end end
	-- Runtime model transforms are typed into the native stream. Format v2
	-- supports both uniform rotation and face-specific rotation in catalog face
	-- order (top, bottom, east, west, north, south).
	local transforms = {}
	local function addTransform( modelId, kind, rotations, label )
		modelId = checkedInt( modelId, 1, mc, label .. " model id" )
		local encoded = u8( kind ) .. u8( 0 )
		if kind == 0 then
			encoded = encoded .. u8( checkedInt( rotations[1], 0, 3, label .. " rotation" ) ) .. u8( 0 )
		elseif kind == 1 then
			for face = 1, 6 do encoded = encoded .. u8( checkedInt( rotations[face], 0, 3, label .. " face rotation" ) ) end
		else error( label .. " transform kind invalid", 0 ) end
		local previous = transforms[modelId]
		if previous and previous ~= encoded then error( label .. " model transform conflict", 0 ) end
		transforms[modelId] = encoded
	end
	for _,name in ipairs({"minecraft:piston","minecraft:sticky_piston","minecraft:piston_head"}) do
		local schema=MC.BlockStateSchemaByName and MC.BlockStateSchemaByName[name]
		if schema and MC.StateVisualPlanRangeFast and MC.StateVisualPlanModelAtFast then for stateId=schema.firstStateId,schema.lastStateId do
			local planId,groupOffset,groupCount=MC.StateVisualPlanRangeFast(stateId)
			if groupCount==1 then local modelId=MC.StateVisualPlanModelAtFast(planId,groupOffset,0,0,0,0);if modelId then addTransform(modelId,0,{2},"piston") end end
		end end
	end
	local railSchema=MC.BlockStateSchemaByName and MC.BlockStateSchemaByName["minecraft:rail"]
	local railCorners={south_east=true,south_west=true,north_west=true,north_east=true}
	if railSchema and MC.GetStateProperty and MC.StateVisualPlanRangeFast and MC.StateVisualPlanModelAtFast then
		for stateId=railSchema.firstStateId,railSchema.lastStateId do
			if railCorners[MC.GetStateProperty(stateId,"shape")] then
				local planId,groupOffset,groupCount=MC.StateVisualPlanRangeFast(stateId)
				if groupCount~=1 then error("rail corner transform requires one model group",0) end
				local modelId=MC.StateVisualPlanModelAtFast(planId,groupOffset,0,0,0,0)
				if not modelId then error("rail corner transform model missing",0) end
				addTransform(modelId,1,{3,1,3,3,3,3},"rail corner")
			end
		end
	end
	local transformed={};for modelId in pairs(transforms)do transformed[#transformed+1]=modelId end
	table.sort(transformed);data[#data+1]=u16le(#transformed)
	for _,modelId in ipairs(transformed)do data[#data+1]=u16le(modelId)..transforms[modelId] end
	local payload=table.concat(data);return payload,#payload
end

local function packBlockDefsUnsafe()
	if not ( MC.Blocks and MC.Atlas and MC.BlockFace and MC.BlockIsFullCube and MC.BlockBoxes ) then return nil end
	local ids = {}
	for id, b in pairs( MC.Blocks ) do
		if type(id)=="number" and id==math.floor(id) and id>=1 and id<=65535 and b then ids[#ids+1]=id end
	end
	table.sort(ids)
	if #ids > MAX_BLOCKS then error("too many block definitions",0) end

	local body, counts = {}, { orient=0, box=0, quad=0, template=0 }
	local emitterCensus, passCensus = {}, { [0]=0,[1]=0 }
	for _, id in ipairs(ids) do
		local b, orients = MC.Blocks[id], orientsForBlock(MC.Blocks[id])
		local orientParts, defaultOrient = {}, orients[1]
		local liquidKind = MC.LiquidKind and MC.LiquidKind(id) or nil
		local connKind = connectionKind(b,id)
		for _, orient in ipairs(orients) do
			local emitter, faces, boxes, quads, templates = nil, {}, {}, {}, {}
			if b.shape == "cross" then emitter = "cross"
			elseif b.shape == "model" and not modelUsesConnection(b,id) then
				quads = modelQuads(b,orient) or {}
				if #quads>0 then
					emitter="model"
					-- Models remain unconditional/no-cull emitters. Their resolved boxes
					-- are nevertheless needed when an adjacent shape asks whether this
					-- model covers its boundary face.
					local modelBoxes = MC.BlockBoxes(id,orient) or {}
					for bi, box in ipairs(modelBoxes) do boxes[bi] = packBox(box) end
				end
			end
			if not emitter and liquidKind then emitter="liquid" end
			if not emitter and MC.BlockIsFullCube(id,orient) then emitter="fullCube" end
			if not emitter and connKind then emitter="connection" end
			if not emitter then
				boxes,quads=staticShapePayload(b,id,orient); if boxes then emitter="shape" else error("unsupported block "..id,0) end
			end
			if emitter=="fullCube" or emitter=="liquid" then
				for face=1,6 do local tile,uvs=resolveFaceTex(b,id,orient,face); faces[#faces+1]=packFaceTex(tile,uvs,0) end
			elseif emitter=="cross" then
				local tile=checkedInt(b.side or 0,0,65535,"cross tile"); faces[1]=packFaceTex(tile,FACE_UV_FLAGS,0)
			elseif emitter=="connection" then templates=buildConnectionTemplates(b,id,orient,connKind) end
			local pass = MC.IsTranslucentBlock and MC.IsTranslucentBlock(id) and 1 or 0
			local flags = MC.BlockIsFullCube(id,orient) and 1 or 0
			local cap = capabilitiesFor(b,id,orient)
			local oh = u8(EMITTER[emitter])..u8(pass)..u8(flags)..u8(cap)..u8(#faces)..u8(0)
				..u16le(#boxes)..u32le(#quads)..u16le(#templates)..u16le(0)
			orientParts[#orientParts+1] = u8(orient)..oh..table.concat(faces)..table.concat(boxes)..table.concat(quads)..table.concat(templates)
			counts.orient=counts.orient+1; counts.box=counts.box+#boxes; counts.quad=counts.quad+#quads; counts.template=counts.template+#templates
			-- aggregate boxes/quads inside templates are part of hostile-validation census.
			if emitter=="connection" then
				local maxMask=connKind=="pipe6" and 63 or 15
				for mask=0,maxMask do local bs=MC.ConnectionBlockBoxes(connKind=="bars" and "pane" or connKind,mask); counts.box=counts.box+#bs end
				-- derive quad census from fixed record lengths after template headers/boxes.
				for _,raw in ipairs(templates) do
					local b0,b1,b2,b3=raw:byte(5,8); counts.quad=counts.quad+(b0+b1*256+b2*65536+b3*16777216)
				end
			end
			emitterCensus[emitter]=(emitterCensus[emitter] or 0)+1; passCensus[pass]=passCensus[pass]+1
		end
		local blockFlags=(b.transparent and 1 or 0)+(b.solid~=false and 2 or 0)
		body[#body+1]=u16le(id)..u16le(blockFlags)..u8(defaultOrient)..u8(LIQUID[liquidKind] or 0)
			..u8(CONNECTION[connKind] or 0)..u8(0)..u16le(#orients)..u16le(0)..u32le(0)..table.concat(orientParts)
	end
	if counts.orient>MAX_BLOCKS*MAX_ORIENTS or counts.box>MAX_BOXES or counts.quad>MAX_QUADS or counts.template>MAX_TEMPLATES then error("definition aggregate limit exceeded",0) end
	local blockPayload=table.concat(body); local A=MC.Atlas
	local states, stateCount = stateMapPayload()
	local visualPayload, visualBytes = visualCatalogPayload( stateCount )
	local metadata = MC.BlockStateRegistryMetadata or {}
	local catalog = MC.StateVisualCatalog or {}
	local schemaHash = hexHashBytes( metadata.schemaHash, "schema hash" )
	local visualHash = hexHashBytes( catalog.visualPlanSha256 or catalog.blockstateSha256, "visual hash" )
	local header="MCBD"..u32le(4)..u32le(128+#blockPayload+#states+visualBytes)
		..u16le(checkedInt(A.COLS or 1,1,65535,"atlas cols"))..u16le(checkedInt(A.ROWS or 1,1,65535,"atlas rows"))
		..u16le(checkedInt(A.TILE or 64,1,65535,"atlas tile"))..u16le(checkedInt(A.PAD or 0,0,65535,"atlas pad"))
		..u16le(checkedInt(A.STRIDE or A.TILE or 64,1,65535,"atlas stride"))..u16le(checkedInt(A.W or 4096,1,65535,"atlas width"))
		..u16le(checkedInt(A.H or 4096,1,65535,"atlas height"))..u16le(0)..f32le(checkedNumber(A.INSET or .5,0,65535,"atlas inset"))
		..u32le(#ids)..u32le(counts.orient)..u32le(counts.box)..u32le(counts.quad)..u32le(counts.template)
		..u32le(stateCount)..u32le(visualBytes)..u32le(visualBytes>0 and 2 or 0)..schemaHash..visualHash
	local blob=header..blockPayload..states..visualPayload
	if #blob>MAX_BLOB_BYTES then error("definition blob too large",0) end
	return blob, { emitters=emitterCensus, passes=passCensus }, #ids, passCensus[0], passCensus[1]
end

local function packBlockDefs()
	local ok,a,b,c,d,e=pcall(packBlockDefsUnsafe)
	if not ok then log("MCBD v4 pack failed: "..tostring(a)); return nil end
	return a,b,c,d,e
end

----------------------------------------------------------------- chunk 打包
local table_concat = table.concat

local function packChunkBlobUnsafe( chunk )
	if type( chunk ) ~= "table" then error( "chunk is not a table", 0 ) end
	local migrated, migrationError = MC.MigrateChunkStateStorage( chunk )
	if not migrated then error( "state storage migration failed: " .. tostring( migrationError ), 0 ) end
	local cs = checkedInt( MC.CS or 16, 1, 65535, "chunk size" )
	local ch = checkedInt( MC.CH or 32, 1, 65535, "chunk height" )
	local cells = cs * cs * ch
	if cells > 16777216 then error( "chunk cell count out of range", 0 ) end
	local maxStateId = checkedInt( MC.BlockStateCount or 1, 1, 1048577, "state count" ) - 1
	local parts, np = {}, 0
	local args, na = {}, 0
	for li = 0, cells - 1 do
		local stateId, readError = MC.GetChunkBlockStateByLocalIndex( chunk, li )
		if stateId == nil then error( "state read failed at " .. li .. ": " .. tostring( readError ), 0 ) end
		stateId = checkedInt( stateId, 0, maxStateId, "chunk stateId at " .. li )
		args[na + 1] = bit_band( stateId, 0xFF )
		args[na + 2] = bit_band( bit_rshift( stateId, 8 ), 0xFF )
		args[na + 3] = bit_band( bit_rshift( stateId, 16 ), 0xFF )
		args[na + 4] = bit_band( bit_rshift( stateId, 24 ), 0xFF )
		na = na + 4
		if na >= 192 then
			np = np + 1
			parts[np] = string_char( unpack( args, 1, na ) )
			na = 0
		end
	end
	if na > 0 then np = np + 1; parts[np] = string_char( unpack( args, 1, na ) ) end
	return table_concat( parts )
end

local function packChunkBlob( chunk )
	local ok, blob = pcall( packChunkBlobUnsafe, chunk )
	if not ok then return nil, tostring( blob ) end
	return blob
end

----------------------------------------------------------------- 代理 mesh
local function makeProxy( cx, cy, cz, pass )
	local proxy = {}
	function proxy:Draw()
		if state.failed then return end
		local ok = mcmesh.DrawChunk( cx, cy, cz, pass )
		if ok == false then state.failed = true end
	end
	function proxy:Destroy() end          -- 原管线清理路径可能调用
	function proxy:IsValid() return true end
	return proxy
end

local function installProxies( key )
	if state.proxied[key] then return end
	local cx, cy, cz = MC.ChunkKeyToCoords( key )
	if not cx then return end
	MC.Meshes[key] = { { mesh = makeProxy( cx, cy, cz, 0 ) } }
	MC.MeshesTrans[key] = { { mesh = makeProxy( cx, cy, cz, 1 ) } }
	state.proxied[key] = true
	if MC.MarkChunkDrawListDirty then MC.MarkChunkDrawListDirty() end
end

local function removeProxies( key )
	if not state.proxied[key] then return end
	MC.Meshes[key] = nil
	MC.MeshesTrans[key] = nil
	state.proxied[key] = nil
	if MC.MarkChunkDrawListDirty then MC.MarkChunkDrawListDirty() end
end

----------------------------------------------------------------- native 调用
local function pushChunk( key, chunk )
	local frame = FrameNumber()
	if state.pushedFrame[key] == frame then return true end
	local cx, cy, cz = MC.ChunkKeyToCoords( key )
	if not cx then return false end
	local blob, packError = packChunkBlob( chunk )
	if not blob then
		log( "chunk pack rejected for " .. tostring( key ) .. ": " .. tostring( packError ) )
		state.failed = true
		return false
	end
	local callOk, result = pcall( mcmesh.ApplyChunk, cx, cy, cz, blob )
	if not callOk then
		log( "ApplyChunk error for " .. tostring( key ) .. ": " .. tostring( result ) )
		state.failed = true
		return false
	end
	if result == true then state.pushedFrame[key] = frame end
	return result == true
end

local function unloadChunkNative( key )
	local cx, cy, cz = MC.ChunkKeyToCoords( key )
	if cx then pcall( mcmesh.UnloadChunk, cx, cy, cz ) end
	state.pushedFrame[key] = nil
end

----------------------------------------------------------------- 激活 / 回退
local deactivate -- 前置声明

local function wrappedBuildChunkMesh( key, sectionIndex )
	if not state.active then return state.originals.BuildChunkMesh( key, sectionIndex ) end
	key = MC.CanonicalChunkKey and MC.CanonicalChunkKey( key ) or key
	local chunk = MC.GetChunkByKey( key )
	if not chunk or ( chunk.count or 0 ) == 0 then
		removeProxies( key )
		unloadChunkNative( key )
		return
	end
	if not pushChunk( key, chunk ) then
		state.failed = true
		return
	end
	installProxies( key )
end

local function wrappedRebuildLighting( key, sectionIndex )
	-- GPU light texture 模式下光照不改几何; 权威快照推一次, native diff 后
	-- 无变化即为 no-op, 有变化(缓存失效兜底路径)则正常重建。
	return wrappedBuildChunkMesh( key, sectionIndex )
end

local function wrappedFreeChunkMesh( key )
	key = MC.CanonicalChunkKey and MC.CanonicalChunkKey( key ) or key
	removeProxies( key )
	unloadChunkNative( key )
	return state.originals.FreeChunkMesh( key )
end

local function patch()
	state.originals = {
		BuildChunkMesh = MC.BuildChunkMesh,
		RebuildChunkMeshLighting = MC.RebuildChunkMeshLighting,
		FreeChunkMesh = MC.FreeChunkMesh,
	}
	MC.BuildChunkMesh = wrappedBuildChunkMesh
	MC.RebuildChunkMeshLighting = wrappedRebuildLighting
	MC.FreeChunkMesh = wrappedFreeChunkMesh
end

local function unpatch()
	if not state.originals then return end
	MC.BuildChunkMesh = state.originals.BuildChunkMesh
	MC.RebuildChunkMeshLighting = state.originals.RebuildChunkMeshLighting
	MC.FreeChunkMesh = state.originals.FreeChunkMesh
	state.originals = nil
end

local function activate()
	timer.Remove( "mcswep_native_fallback_rebuild" )   -- 快速来回切时终止未完成的回退重建
	patch()
	state.active = true
	state.failed = false
	local t0 = SysTime()
	local pushed = 0
	-- 接管已加载世界: 释放 Lua mesh -> 推快照 -> 装代理
	for key, chunk in pairs( MC.World or {} ) do
		state.originals.FreeChunkMesh( key )
		if chunk and ( chunk.count or 0 ) > 0 then
			if not pushChunk( key, chunk ) then
				state.failed = true
				break
			end
			installProxies( key )
			pushed = pushed + 1
		end
	end
	if state.failed then
		log( "activation failed, falling back to Lua" )
		return deactivate( "activation failure" )
	end
	log( ">>> backend now: NATIVE (" .. pushed .. " chunks pushed in "
		.. math.Round( ( SysTime() - t0 ) * 1000, 1 ) .. "ms)" )
end

deactivate = function( reason )
	state.active = false
	unpatch()
	for key in pairs( state.proxied ) do
		MC.Meshes[key] = nil
		MC.MeshesTrans[key] = nil
	end
	state.proxied = {}
	state.pushedFrame = {}
	-- ClearWorld keeps the permanent worker alive for an active native session.
	-- A backend deactivation must fully stop workers and clear definitions/promise,
	-- otherwise the next Handshake correctly rejects workerCount != 0.
	if mcmesh and isfunction( mcmesh.Shutdown ) then pcall( mcmesh.Shutdown )
	elseif mcmesh and isfunction( mcmesh.ClearWorld ) then pcall( mcmesh.ClearWorld ) end
	if MC.MarkChunkDrawListDirty then MC.MarkChunkDrawListDirty() end
	if state.shuttingDown then return end
	-- 全图交还 Lua 重建(每 tick 一批, 避免回退瞬间一次性尖峰)
	local keys = {}
	for key, chunk in pairs( MC.World or {} ) do
		if chunk and ( chunk.count or 0 ) > 0 then keys[#keys + 1] = key end
	end
	local PER_TICK = 4
	local i = 0
	timer.Create( "mcswep_native_fallback_rebuild", 0, math.ceil( #keys / PER_TICK ), function()
		for _ = 1, PER_TICK do
			i = i + 1
			if not keys[i] then return end
			if MC.BuildChunkMesh then MC.BuildChunkMesh( keys[i] ) end
		end
	end )
	log( ">>> backend now: LUA (" .. tostring( reason ) .. "), "
		.. #keys .. " chunks rebuilding" )
end

----------------------------------------------------------------- 帧循环 / 看护
hook.Add( "Think", "mcswep_native_think", function()
	if not state.active then return end
	if state.failed then return deactivate( "native call failed" ) end
	if not CONVAR_ENABLED:GetBool() then return deactivate( "mc_native_mesh 0" ) end
	-- 光照风格切走 GPU light texture -> native 固定 payload 会错光, 交还 Lua
	if not gpuLightProfileActive() then return deactivate( "light style changed" ) end
	-- 原项目 autorefresh 会重定义 MC.BuildChunkMesh, 检测到就重新打补丁
	if MC.BuildChunkMesh ~= wrappedBuildChunkMesh then
		log( "detected function reload, re-patching" )
		state.originals = nil
		return activate()
	end
	local ok, result = pcall( mcmesh.Think, math.Clamp( CONVAR_THINK_MS:GetFloat(), 0.5, 16 ) )
	if not ok then
		log( "Think error: " .. tostring( result ) )
		return deactivate( "Think error" )
	end
	if istable( result ) and result.failed then
		log( "native fault: " .. tostring( result.fault or "unknown" ) )
		return deactivate( "native fault" )
	end
	-- M3 的 BuildChunkMesh wrapper 只提交快照，真正的 IMesh 替换发生在 native
	-- Think。稳定代理不会自行改变 ChunkDrawListVersion，而 shadow map / heightfield
	-- 用它作为几何内容版本；每帧有任意 section 真正落地后统一 bump 一次。
	if istable( result ) and ( tonumber( result.committed ) or 0 ) > 0 then
		if MC.MarkChunkDrawListDirty then MC.MarkChunkDrawListDirty() end
	end
end )

----------------------------------------------------------------- 启动引导
local function tryStart()
	if state.shuttingDown or state.shutdownCalled then return false end
	if state.active then return true end
	if not CONVAR_ENABLED:GetBool() then return false end
	if not ( MC and MC.BuildChunkMesh and MC.World and MC.Meshes and MC.MeshesTrans
		and MC.GetChunkByKey and MC.ChunkKeyToCoords ) then return false end
	if not mcmesh then
		local ok = pcall( require, "mcswep_native_mesh" )
		if not ok or not mcmesh then return false end
	end
	if not ( mcmesh.AbiVersion and mcmesh.AbiVersion() == ADAPTER_ABI ) then
		log( "ABI mismatch (module " .. tostring( mcmesh.AbiVersion and mcmesh.AbiVersion() )
			.. ", adapter " .. ADAPTER_ABI .. "), staying on Lua path" )
		timer.Remove( "mcswep_native_bootstrap" )
		return false
	end
	-- 导出面检查: 契约函数必须全部存在(空壳也行), 缺一个就点名并留在 Lua 路径
	for _, fname in ipairs( { "Handshake", "SetBlockDefs", "ApplyChunk", "UnloadChunk",
		"ClearWorld", "Think", "DrawChunk", "GetStats", "Shutdown" } ) do
		if not isfunction( mcmesh[fname] ) then
			log( "module missing mcmesh." .. fname .. "(), staying on Lua path" )
			timer.Remove( "mcswep_native_bootstrap" )
			return false
		end
	end
	-- GPU 光照档案门槛: 非 GPU light texture 风格下 native 的固定 payload 会错光
	if not gpuLightProfileActive() then
		return false   -- 静默: 玩家可能切了光照风格, bootstrap 下秒重试
	end
	-- 材质在加载早期可能尚未创建; 未就绪则静默返回, bootstrap 定时器下秒重试
	local matOpaque = MC.CurrentChunkMaterial and MC.CurrentChunkMaterial( false )
	local matTranslucent = MC.CurrentChunkMaterial and MC.CurrentChunkMaterial( true )
	if not ( matOpaque and matOpaque.GetName and not matOpaque:IsError()
		and matTranslucent and matTranslucent.GetName and not matTranslucent:IsError() ) then
		return false
	end
	-- 材质名带 "!" 前缀: chunk 材质是运行时 CreateMaterial 的, C++ 侧
	-- FindMaterial 裸名会去磁盘找 .vmt 而拿到 error material(实测无画面),
	-- "!" 强制查运行时材质表 —— spike 里 test.cpp 硬编码的名字同款前缀。
	local shook = mcmesh.Handshake and mcmesh.Handshake(
		MC.CS or 16, MC.CH or 32, 8, MC.BS or 36.5,
		"!" .. matOpaque:GetName(), "!" .. matTranslucent:GetName() )
	if shook ~= true then
		log( "handshake rejected, staying on Lua path" )
		timer.Remove( "mcswep_native_bootstrap" )
		return false
	end
	-- 方块静态数据: 握手后、首个 ApplyChunk 前上传一次。失败必须先清理
	-- handshake 创建的 native 状态，否则 bootstrap 的下一次重试会继承半启动状态。
	local function clearFailedStartup()
		if mcmesh and isfunction( mcmesh.Shutdown ) then
			pcall( mcmesh.Shutdown )
		elseif mcmesh and isfunction( mcmesh.ClearWorld ) then
			pcall( mcmesh.ClearWorld )
		end
	end
	local packStarted = SysTime()
	local blob, census, defCount, opaqueCount, translucentCount
	local metadata = MC.BlockStateRegistryMetadata or {}
	local catalog = MC.StateVisualCatalog or {}
	local cacheKey = "mcbd4-visual2:" .. tostring( metadata.schemaHash or "" ) .. ":" .. tostring( catalog.visualPlanSha256 or "" )
		.. ":" .. tostring( MC.BlockStateCount or 0 )
	if state.defCache and state.defCache.key == cacheKey then
		blob, census, defCount, opaqueCount, translucentCount = state.defCache.blob, state.defCache.census,
			state.defCache.defCount, state.defCache.opaqueCount, state.defCache.translucentCount
	else
		blob, census, defCount, opaqueCount, translucentCount = packBlockDefs()
		if blob then state.defCache = { key=cacheKey,blob=blob,census=census,defCount=defCount,opaqueCount=opaqueCount,translucentCount=translucentCount } end
	end
	local packMS = ( SysTime() - packStarted ) * 1000
	if not blob then
		clearFailedStartup()
		return false -- MC.Blocks/Atlas 尚未就绪或定义非法, 下秒从干净状态重试
	end
	local parseStarted = SysTime()
	if mcmesh.SetBlockDefs( blob ) ~= true then
		log( "SetBlockDefs rejected (" .. #blob .. " bytes), staying on Lua path" )
		clearFailedStartup()
		timer.Remove( "mcswep_native_bootstrap" )
		return false
	end
	local parseMS = ( SysTime() - parseStarted ) * 1000
	log( "SetBlockDefs v4 ok: " .. tostring( defCount ) .. " ids, "
		.. tostring( opaqueCount ) .. " opaque + " .. tostring( translucentCount )
		.. " translucent orient definitions, " .. #blob .. " bytes, pack/cache="
		.. math.Round( packMS, 1 ) .. "ms parse=" .. math.Round( parseMS, 1 ) .. "ms" )
	if census and census.emitters then
		local c = census.emitters
		log( "emitter census: fullCube="..tostring(c.fullCube or 0).." cross="..tostring(c.cross or 0)
			.." model="..tostring(c.model or 0).." shape="..tostring(c.shape or 0)
			.." connection="..tostring(c.connection or 0).." liquid="..tostring(c.liquid or 0) )
	end
	activate()
	return true
end

timer.Create( "mcswep_native_bootstrap", 1, 0, function()
	if tryStart() then timer.Remove( "mcswep_native_bootstrap" ) end
end )

cvars.AddChangeCallback( "mc_native_mesh", function( _, _, new )
	if new == "1" and not state.active then
		if tryStart() then return end
		timer.Create( "mcswep_native_bootstrap", 1, 0, function()
			if tryStart() then timer.Remove( "mcswep_native_bootstrap" ) end
		end )
	end
	-- "0" 的关闭由 Think 里的 convar 检查处理
end, "mcswep_native_bridge" )

-- Process teardown is not a fallback: stop scheduling, restore Lua functions,
-- discard proxy bookkeeping, and call the native shutdown exactly once.
hook.Add( "ShutDown", "mcswep_native_shutdown", function()
	if state.shutdownCalled then return end
	state.shutdownCalled = true
	state.shuttingDown = true
	timer.Remove( "mcswep_native_bootstrap" )
	timer.Remove( "mcswep_native_fallback_rebuild" )
	hook.Remove( "Think", "mcswep_native_think" )
	cvars.RemoveChangeCallback( "mc_native_mesh", "mcswep_native_bridge" )
	unpatch()
	state.active = false
	state.failed = false
	state.proxied = {}
	state.pushedFrame = {}
	state.defCache = nil
	if mcmesh and isfunction( mcmesh.Shutdown ) then pcall( mcmesh.Shutdown ) end
end )

----------------------------------------------------------------- A/B 热切换指令
local function setEnabledConVar( on )
	-- 保持 convar 与实际状态一致, 否则 Think 的守护检查会立刻切回去
	local ok = pcall( function() CONVAR_ENABLED:SetBool( on ) end )
	if not ok then RunConsoleCommand( "mc_native_mesh", on and "1" or "0" ) end
end

concommand.Add( "mc_native_toggle", function()
	if state.active then
		setEnabledConVar( false )
		deactivate( "toggle" )
	else
		setEnabledConVar( true )
		if not tryStart() then
			log( "cannot activate: module or MC pipeline not ready (see mc_native_status)" )
		end
	end
end, nil, "Hot-swap MCSWEP mesh backend between native (mcswep_native_mesh) and pure Lua for A/B comparison" )

concommand.Add( "mc_native_status", function()
	local lines = {
		"backend        = " .. ( state.active and "NATIVE" or "LUA" ),
		"convar         = mc_native_mesh " .. ( CONVAR_ENABLED:GetBool() and "1" or "0" ),
		"module loaded  = " .. tostring( mcmesh ~= nil ),
		"proxied chunks = " .. table.Count( state.proxied ),
		"light profile  = " .. tostring( MC.CurrentMeshLightStyle and MC.CurrentMeshLightStyle() or "?" )
			.. ( gpuLightProfileActive() and " (gpu, native-ok)" or " (non-gpu, native blocked)" ),
	}
	if mcmesh and mcmesh.GetStats then
		local ok, s = pcall( mcmesh.GetStats )
		if ok and istable( s ) then
			for k, v in SortedPairs( s ) do
				lines[#lines + 1] = "native." .. k .. " = " .. tostring( v )
			end
		end
	end
	for _, l in ipairs( lines ) do log( l ) end
end, nil, "Print MCSWEP native mesh backend state and module stats" )
