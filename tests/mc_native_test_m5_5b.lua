-- M5.5 checkpoint B: compact generated visual catalog acceptance.
if SERVER then return end

concommand.Add( "mc_native_test_m5_5b", function()
	if not ( MC and mcmesh and MC.StateVisualCatalogStats and mcmesh.GetStats ) then
		print( "[m5.5b] FAIL: catalog or native API unavailable" ) return
	end
	local failures = 0
	local function fail( text ) failures = failures + 1; print( "[m5.5b]   " .. text ) end
	local c = MC.StateVisualCatalogStats
	if tonumber( c.states ) ~= 32366 then fail( "unexpected generated catalog state count: " .. tostring( c.states ) ) end
	if tonumber( c.plans ) ~= 12127 then fail( "unexpected plan count: " .. tostring( c.plans ) ) end
	if tonumber( c.models ) ~= 6812 then fail( "unexpected model count: " .. tostring( c.models ) ) end
	if tonumber( c.geometryTemplates ) ~= 934 then fail( "unexpected geometry count: " .. tostring( c.geometryTemplates ) ) end
	if tonumber( c.surfaceSets ) ~= 1843 then fail( "unexpected surface count: " .. tostring( c.surfaceSets ) ) end
	local stats = mcmesh.GetStats()
	if not istable( stats ) then fail( "GetStats unavailable" )
	else
		if stats.faulted == true then fail( "native faulted: " .. tostring( stats.faultReason ) ) end
		for _, field in ipairs( { "pendingSections", "queuedJobs", "activeJobs", "queuedResults" } ) do
			if tonumber( stats[field] ) ~= 0 then fail( field .. " has not converged: " .. tostring( stats[field] ) ) end
		end
	end
	if failures == 0 then print( "[m5.5b] GENERATED CATALOG RUNTIME OK" ) else print( "[m5.5b] FAIL: failures=" .. failures ) end
	print( "[m5.5b] VISUAL ACCEPTANCE REQUIRED: inspect stairs/slabs/rails/redstone/crops/fences and random model variants" )
	print( "[m5.5b] TINT PARITY UNVERIFIED" )
end, nil, "M5.5 generated visual catalog checkpoint" )

print( "[m5.5b] loaded -- run after queues converge: mc_native_test_m5_5b" )
