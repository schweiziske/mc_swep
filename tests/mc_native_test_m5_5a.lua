-- M5.5 checkpoint A: stateId definition/mirror transport.
-- This checkpoint does not claim generated visual parity.
if SERVER then return end

concommand.Add( "mc_native_test_m5_5a", function()
	if not ( MC and mcmesh and MC.BlockStateCount and MC.GetBlockState ) then
		print( "[m5.5a] FAIL: state runtime or mcmesh unavailable" )
		return
	end
	if not ( mcmesh.DebugStateDef and mcmesh.DebugGetCell ) then
		print( "[m5.5a] FAIL: state diagnostic API unavailable" )
		return
	end

	local failures = 0
	local function fail( text ) failures = failures + 1; print( "[m5.5a]   " .. text ) end
	local count = tonumber( MC.BlockStateCount ) or 0
	if count < 1 then fail( "invalid BlockStateCount" ) end

	local samples = { 0, 1, math.max( 0, math.floor( count / 2 ) ), math.max( 0, count - 1 ) }
	for _, stateId in ipairs( samples ) do
		local d = mcmesh.DebugStateDef( stateId )
		if not istable( d ) or tonumber( d.stateId ) ~= stateId then
			fail( "missing native state definition " .. stateId )
		end
	end

	local checked = 0
	for key, chunk in pairs( MC.World or {} ) do
		if chunk and ( chunk.count or 0 ) > 0 then
			local cx, cy, cz = MC.ChunkKeyToCoords( key )
			if cx then
				local migrated, err = MC.MigrateChunkStateStorage( chunk )
				if not migrated then fail( "migration failed: " .. tostring( err ) ) break end
				for li = 0, ( MC.CS or 16 ) * ( MC.CS or 16 ) * ( MC.CH or 32 ) - 1 do
					local luaState = MC.GetChunkBlockStateByLocalIndex( chunk, li )
					local nativeState = mcmesh.DebugGetCell( cx, cy, cz, li )
					if tonumber( nativeState ) ~= tonumber( luaState ) then
						fail( string.format( "mirror mismatch chunk(%d,%d,%d) li=%d lua=%s native=%s", cx,cy,cz,li,tostring(luaState),tostring(nativeState) ) )
						break
					end
				end
				checked = checked + 1
				if checked >= 4 then break end
			end
		end
	end
	if checked == 0 then fail( "no loaded non-empty chunks checked" ) end

	if failures == 0 then
		print( string.format( "[m5.5a] STATE TRANSPORT OK: states=%d chunks=%d", count, checked ) )
	else
		print( string.format( "[m5.5a] FAIL: failures=%d", failures ) )
	end
	print( "[m5.5a] GENERATED VISUAL PARITY UNVERIFIED" )
end, nil, "M5.5 stateId definition and chunk mirror checkpoint" )

print( "[m5.5a] loaded -- run: mc_native_test_m5_5a" )
