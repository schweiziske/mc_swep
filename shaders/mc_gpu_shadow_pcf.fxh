#ifndef MC_GPU_SHADOW_PCF_FXH
#define MC_GPU_SHADOW_PCF_FXH

// Precomputed 32-point Vogel table. The current Photon path samples only the
// first 16 entries; entries 16..31 are DEPRECATED/UNUSED and retained as a
// reference table. Precomputation avoids per-pixel sin/cos/sqrt work on Source
// ps_3_0.
static const int MC_SHADOW_PHOTON_SAMPLES = 16;
static const int MC_SHADOW_CSM_SAMPLES = 16;
static const float MC_TAU = 6.28318531f;
static const float2 mcShadowVogelDisk[32] = {
	float2(  0.125000000f,  0.000000000f ),
	float2( -0.159645045f,  0.146247939f ),
	float2(  0.024436233f, -0.278438271f ),
	float2(  0.201222239f,  0.262458779f ),
	float2( -0.369267557f, -0.065318232f ),
	float2(  0.349802466f, -0.222515695f ),
	float2( -0.117002080f,  0.435241902f ),
	float2( -0.223135653f, -0.429634124f ),
	float2(  0.484115115f,  0.176798065f ),
	float2( -0.503641109f,  0.207895727f ),
	float2(  0.242788296f, -0.518824482f ),
	float2(  0.179414372f,  0.572001296f ),
	float2( -0.540757005f, -0.313379740f ),
	float2(  0.634369524f, -0.139464358f ),
	float2( -0.387145847f,  0.550675125f ),
	float2( -0.089439652f, -0.690199644f ),
	// DEPRECATED/UNUSED by the current 16-sample Photon path:
	float2(  0.549071755f,  0.462758261f ),
	float2( -0.738878471f,  0.030554941f ),
	float2(  0.538955128f, -0.536332332f ),
	float2( -0.036058190f,  0.779791515f ),
	float2( -0.512817529f, -0.614526796f ),
	float2(  0.812359593f,  0.109301839f ),
	float2( -0.688310643f,  0.478908612f ),
	float2(  0.188086063f, -0.836061381f ),
	float2(  0.435033261f,  0.759191058f ),
	float2( -0.850448409f, -0.271316244f ),
	float2(  0.826102404f, -0.381680257f ),
	float2( -0.357888208f,  0.855155559f ),
	float2( -0.319407328f, -0.888033760f ),
	float2(  0.849908632f,  0.446688166f ),
	float2( -0.944034648f,  0.248844495f ),
	float2(  0.536595815f, -0.834529766f )
};

float2 mcRotateShadowTap( int index, float2 rotation )
{
	float2 tap = mcShadowVogelDisk[index];
	return float2(
		tap.x * rotation.x - tap.y * rotation.y,
		tap.x * rotation.y + tap.y * rotation.x
	);
}

// Preserve the existing 16-sample CSM kernel. CSM samples in cascade-local UV
// space; the distorted single-map path uses the first 16 entries of the Vogel
// table above.
static const float2 mcShadowCsmPoissonDisk[16] = {
	float2( -0.94201624f, -0.39906216f ),
	float2(  0.94558609f, -0.76890725f ),
	float2( -0.094184101f, -0.92938870f ),
	float2(  0.34495938f,  0.29387760f ),
	float2( -0.91588581f,  0.45771432f ),
	float2( -0.81544232f, -0.87912464f ),
	float2( -0.38277543f,  0.27676845f ),
	float2(  0.97484398f,  0.75648379f ),
	float2(  0.44323325f, -0.97511554f ),
	float2(  0.53742981f, -0.47373420f ),
	float2( -0.26496911f, -0.41893023f ),
	float2(  0.79197514f,  0.19090188f ),
	float2( -0.24188840f,  0.99706507f ),
	float2( -0.81409955f,  0.91437590f ),
	float2(  0.19984126f,  0.78641367f ),
	float2(  0.14383161f, -0.14100790f )
};

float2 mcRotateCsmShadowTap( int index, float2 rotation )
{
	float2 tap = mcShadowCsmPoissonDisk[index];
	return float2(
		tap.x * rotation.x - tap.y * rotation.y,
		tap.x * rotation.y + tap.y * rotation.x
	);
}

#endif
