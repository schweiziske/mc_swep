#ifndef MC_GPU_BLOCK_TINT_FXH
#define MC_GPU_BLOCK_TINT_FXH

float3 mcBlockTint( float encodedCode )
{
	float code = floor( encodedCode * 255.0f + 0.5f );
	if ( code < 0.5f ) return float3( 1.0f, 1.0f, 1.0f );
	if ( code < 16.5f )
	{
		float power = code - 1.0f;
		float f = power / 15.0f;
		float r = power < 0.5f ? 0.3f : f * 0.6f + 0.4f;
		float g = max( 0.0f, f * f * 0.7f - 0.5f );
		float b = max( 0.0f, f * f * 0.6f - 0.7f );
		return float3( r, g, b );
	}
	if ( code < 24.5f )
	{
		float age = code - 17.0f;
		return float3( age * 32.0f, 255.0f - age * 8.0f, age * 4.0f ) / 255.0f;
	}
	if ( code < 25.5f ) return float3( 224.0f, 199.0f, 28.0f ) / 255.0f;
	return float3( 1.0f, 1.0f, 1.0f );
}

#endif
