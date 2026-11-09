// DLAA (vk_dlaa.c): NVIDIA DLSS at the scene's own size on Engage, behind TOTK_DLAA (engage.ini dlaa = 1).
// Adapted from TotK Recomp 0.3.13 runtime/vk_dlaa.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh and contributors).
#pragma once
#include "vk.h"

int vk_dlaa_wanted(void);   // the setting is on at start (the device may still lack DLSS)
// vk_device.c: NGX's instance and device extensions join the lists (room for max names); vk_dlaa_init after the
// device exists brings NGX up and checks that DLSS is available.
void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_init(void);
int vk_dlaa_available(void);
// vk_exec.c vk_exec_pass_begin, once the pass's targets are resolved and before its barrier: arms the frame on the
// scene's light target or, at the first half-width pass after it (the bloom), runs DLAA on that target.
typedef struct VkExec VkExec;
void vk_dlaa_pass(VkExec *ex);
void vk_dlaa_frame(void);   // each present (also feeds the watchdog)
const char *vk_dlaa_status(void);   // the F10 overlay's anti-aliasing line
// vk_exec_draw.c, each draw before its fixed state is recorded: notes the scene camera block (vk_dlaa_draw), and moves
// the viewport of the scene's draws by the frame's jitter (vk_dlaa_viewport).
void vk_dlaa_draw(VkExec *ex);
void vk_dlaa_viewport(VkExec *ex, VkViewport *v);
// tex_info: in a pass larger than it, swap_img (the scene's light target, once DLSS scaled it this frame) is sampled
// through swap_view (DLSS's output) instead
extern const VkTexImage *vk_dlaa_swap_img;
extern VkImageView vk_dlaa_swap_view;
extern int vk_dlaa_trace_now;   // this frame is traced (TOTK_DLAA_TRACE)
void vk_dlaa_tex_seen(VkExec *ex, const VkTexImage *img);   // tex_info: a sampled image (TOTK_DLAA_TRACE)
