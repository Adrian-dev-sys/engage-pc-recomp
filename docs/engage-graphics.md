# Anti-aliasing and upscaling (Engage)

F1 > Screen > Anti-aliasing, saved in `engage.ini` `[display]`:

- `dlaa` (DLSS / FSR anti-aliasing): temporal anti-aliasing of the 3D scene. Needs a restart to come up.
- `upscale_mode`: `native` (the scene at the render size, DLAA), `quality` (67%), `balanced` (58%), `performance`
  (50%), `ultra` (33%). Restart.
- `aa_upscaler`: `auto` (DLSS where available, else FSR 3.1), `dlss`, `fsr3`. Restart.
- `dlss_preset`: one DLSS model for every mode; by default K for DLAA / Quality / Balanced, M for Performance, L for
  Ultra Performance (DLSS 4.5).

Files next to `engage.exe`: `nvngx_dlss.dll` (DLSS) and `amd_fidelityfx_vk.dll` (FSR 3.1, FidelityFX SDK 1.1.4 signed
DLL). Neither is in the repository or the release script yet.

## Where it runs in the frame

Unity draws the scene into an R11G11B10F light target with the scene depth, at Unity's render scale times the render
size; bloom starts at half width; late transparents and particles are composited into another light target of the
scene size; the tonemap reads that target and writes the RGBA8 sRGB output at the render size; the UI follows.

- Native: DLSS runs in place on the light target at the first half-width pass (`vk_dlaa.c` `vk_dlaa_pass`).
- Upscaling: patch set `fullres` hands `RenderManager.CommitScale`'s `SetRenderScale` the mode's scale (patch page
  +0x64, `resolution.c` `engage_scene_scale`). At the first pass wider than the scene (the tonemap) DLSS or FSR
  scales the last scene-size light target to that pass's size, and that pass and the larger ones after it sample
  the result in its place (`vk_dlaa_swap_img`, `vk_exec_draw.c` `tex_info`). The UI is never jittered or upscaled.
- Jitter: the scene's draws (those with the scene depth, until the bloom) get a Halton offset; camera motion vectors
  come from the depth and Unity's view-projection (`shaders/dlaa_mv.comp`).

FSR 3.1 (`vk_ffx.cpp`) needs `VK_KHR_get_memory_requirements2` enabled (`vk_device.c`): its back end looks
`vkGetBufferMemoryRequirements2KHR` up by name. Depth is taken as reversed Z (`TOTK_FSR_DEPTH_INVERTED=0` flips it;
no visible difference). FSR shows more grain on camera movement than DLSS; sharpness does not change that.

Test knobs: `TOTK_DLAA_TRACE=n` logs n frames' passes after the scene and the images they sample;
`TOTK_DLAA_MVDEBUG=1`, `TOTK_DLAA_MV=0`, `TOTK_DLAA_JITTER=0`, `TOTK_DLAA_FLIPY=1`.
