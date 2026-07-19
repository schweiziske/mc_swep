--[[----------------------------------------------------------------------------
	mc_native_test_m5_runtime.lua -- M5 runtime acceptance (not geometry parity)

	Run after the fixed-matrix test world is loaded and native queues converge:
	  lua_openscript_cl mc_native_test_m5_runtime.lua
	  mc_native_test_m5_runtime [maxChunks]

	Checks typed definitions, all six live-emitter stats, section emitter/pass census
	consistency, face/vertex totals, and queue convergence. It deliberately does not
	claim vertex/geometry parity; visual/UV/edit/boundary parity remains the fixed
	matrix in _native/M5_PLAN.md.
------------------------------------------------------------------------------]]
if SERVER then return end

local KINDS = { "fullCube", "cross", "model", "shape", "connection", "liquid" }
local function emitterName( value )
	if type( value ) == "number" then return KINDS[value] end
	return value
end

concommand.Add( "mc_native_test_m5_runtime", function( _, _, args )
	if not ( mcmesh and mcmesh.GetStats and mcmesh.DebugBlockDef and mcmesh.DebugBuildSection ) then
		print( "[m5] FAIL: updated mcmesh diagnostic API is unavailable" )
		return
	end
	if not ( MC and MC.Blocks and MC.World and MC.ChunkKeyToCoords ) then
		print( "[m5] FAIL: MC runtime is not ready" )
		return
	end

	local failures, reports = 0, {}
	local function fail( text )
		failures = failures + 1
		if #reports < 30 then reports[#reports + 1] = text end
	end

	local definitionCensus = {}
	for _, kind in ipairs( KINDS ) do definitionCensus[kind] = 0 end
	local definitions, typedOrients = 0, 0
	for id, b in pairs( MC.Blocks ) do
		if type( id ) == "number" and id >= 1 and id <= 65535 and b then
			definitions = definitions + 1
			local d = mcmesh.DebugBlockDef( id )
			if not istable( d ) then
				fail( "definition missing for id=" .. id )
			else
				local seen = false
				for _, od in pairs( d.orients or {} ) do
					local kind = emitterName( od.emitter or od.emitterKind or od.kind )
					if definitionCensus[kind] ~= nil then
						definitionCensus[kind] = definitionCensus[kind] + 1
						typedOrients = typedOrients + 1
						seen = true
					else
						fail( "definition id=" .. id .. " has unknown emitter=" .. tostring( kind ) )
					end
				end
				if not seen then fail( "definition id=" .. id .. " exposes no typed orient" ) end
			end
		end
	end
	for _, kind in ipairs( KINDS ) do
		if definitionCensus[kind] == 0 then fail( "typed definition census is zero for " .. kind ) end
	end

	local stats = mcmesh.GetStats()
	if not istable( stats ) then
		fail( "GetStats did not return a table" )
	else
		for _, field in ipairs( { "pendingSections", "queuedJobs", "activeJobs", "queuedResults" } ) do
			if tonumber( stats[field] ) ~= 0 then fail( field .. " has not converged: " .. tostring( stats[field] ) ) end
		end
		if stats.faulted == true then fail( "native is faulted: " .. tostring( stats.faultReason ) ) end
		local live = stats.emitters
		if not istable( live ) then
			fail( "GetStats.emitters is unavailable" )
		else
			for _, kind in ipairs( KINDS ) do
				local e = live[kind]
				local faces = istable( e ) and tonumber( e.faces ) or tonumber( e )
				if not faces or faces <= 0 then fail( "live emitter faces are zero for " .. kind .. " (load the fixed-matrix world)" ) end
			end
		end
	end

	local CS, CH, SS = MC.CS or 16, MC.CH or 32, 8
	local NSX, NSZ = math.ceil( CS / SS ), math.ceil( CH / SS )
	local SPC = NSX * NSX * NSZ
	local maxChunks = tonumber( args and args[1] or "" ) or math.huge
	local chunks, sections = 0, 0
	local sectionEmitter = {}
	for _, kind in ipairs( KINDS ) do sectionEmitter[kind] = 0 end

	for key, chunk in pairs( MC.World ) do
		if chunks >= maxChunks then break end
		if chunk and ( chunk.count or 0 ) > 0 then
			local cx, cy, cz = MC.ChunkKeyToCoords( key )
			if cx then
				for sec = 0, SPC - 1 do
					local r = mcmesh.DebugBuildSection( cx, cy, cz, sec )
					sections = sections + 1
					if not istable( r ) then
						fail( string.format( "DebugBuildSection missing chunk(%d,%d,%d) sec=%d", cx,cy,cz,sec ) )
					else
						local ec = r.emitterFaces or r.emitters
						if not istable( ec ) then
							fail( "section has no emitter census" )
						else
							local sum = 0
							for _, kind in ipairs( KINDS ) do
								local n = tonumber( ec[kind] ) or 0
								sectionEmitter[kind] = sectionEmitter[kind] + n
								sum = sum + n
							end
							if tonumber( r.faces ) ~= sum then fail( "section emitter census does not sum to faces" ) end
						end
						local opaque, trans = tonumber( r.opaqueFaces ), tonumber( r.translucentFaces )
						if not opaque or not trans or opaque + trans ~= tonumber( r.faces ) then fail( "section pass totals do not sum to faces" ) end
						if tonumber( r.verts ) ~= tonumber( r.faces ) * 6 then fail( "section verts != faces*6" ) end
						if tonumber( r.opaqueVerts ) ~= opaque * 6 or tonumber( r.translucentVerts ) ~= trans * 6 then fail( "section pass verts != pass faces*6" ) end
					end
				end
				chunks = chunks + 1
			end
		end
	end
	if sections == 0 then fail( "no live sections were checked" ) end
	for _, kind in ipairs( KINDS ) do
		if sectionEmitter[kind] == 0 then fail( "sampled section emitter census is zero for " .. kind ) end
	end

	print( string.format( "[m5] definitions=%d typedOrients=%d chunks=%d sections=%d failures=%d", definitions, typedOrients, chunks, sections, failures ) )
	print( string.format( "[m5] definition emitters fullCube=%d cross=%d model=%d shape=%d connection=%d liquid=%d",
		definitionCensus.fullCube, definitionCensus.cross, definitionCensus.model, definitionCensus.shape, definitionCensus.connection, definitionCensus.liquid ) )
	print( string.format( "[m5] section emitter faces fullCube=%d cross=%d model=%d shape=%d connection=%d liquid=%d",
		sectionEmitter.fullCube, sectionEmitter.cross, sectionEmitter.model, sectionEmitter.shape, sectionEmitter.connection, sectionEmitter.liquid ) )
	for _, text in ipairs( reports ) do print( "[m5]   " .. text ) end
	if failures == 0 then print( "[m5] RUNTIME ACCEPTANCE OK: typed/census/pass/convergence invariants passed" )
	else print( "[m5] RUNTIME ACCEPTANCE FAIL" ) end
	print( "[m5] GEOMETRY PARITY UNVERIFIED: complete the visual/UV/pass/edit/boundary fixed matrix; this script never reports geometry ALL OK" )
end, nil, "M5 runtime invariants and live emitter acceptance (not geometry parity)" )

print( "[m5] loaded -- run: mc_native_test_m5_runtime [maxChunks]" )
