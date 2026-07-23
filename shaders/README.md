# Shader Sources

This directory contains the HLSL source used by MC SWEP's Fancy lighting path with the default shadow-map mode:

```text
mc_light_style fancy
mc_shadow_mode map
```

Source Engine convention uses the `.fxc` extension for shader entry points and `.fxh` for shared HLSL includes. These are HLSL source files; compiled Source Engine shaders use the `.vcs` extension.

## Included Files

| Source file | Role | Matching runtime shader |
| --- | --- | --- |
| `mc_gpu_chunk_lighttex_fancy_ps30.fxc` | Opaque chunk pixel shader | `mc_gpu_chunk_lighttex_fancy31_clean_pcf_vface_ps30.vcs` |
| `mc_gpu_chunk_lighttex_fancy_trans_ps30.fxc` | Translucent chunk pixel shader | `mc_gpu_chunk_lighttex_fancy_trans22_clean_pcf_vface_ps30.vcs` |
| `mc_gpu_chunk_lighttex_vs30.fxc` | Chunk vertex shader | `mc_gpu_chunk_lighttex_vs30.vcs` |
| `mc_gpu_shadow_depth_ps30.fxc` | Shadow depth pixel shader | `mc_gpu_shadow_depth5_ps30.vcs` |
| `mc_gpu_shadow_depth_distort_vs30.fxc` | Distorted shadow-map vertex shader | `mc_gpu_shadow_depth_distort1_vs30.vcs` |
| `mc_gpu_shadow_pcf.fxh` | Shared shadow PCF sampling code | Included by the Fancy pixel shaders |
| `mc_gpu_block_tint.fxh` | Shared block tint code | Included by the Fancy pixel shaders |

## Building

The shaders target Shader Model 3.0 and are intended for Valve's Source shader build environment. Compilation requires Valve's shader headers, including `common_ps_fxc.h` and `common_vs_fxc.h`, which are not duplicated here.

The source files in this directory correspond to the compiled `.vcs` shaders used by the current in-game configuration. Generated `.vcs` binaries are intentionally not included in this source directory.
