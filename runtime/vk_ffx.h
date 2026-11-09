// FSR 3.1 temporal upscaling through AMD's FidelityFX API library (vk_ffx.cpp), for vk_dlaa.c.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkImage image;            // mip 0, layer 0, VK_IMAGE_LAYOUT_GENERAL
    VkFormat format;
    uint32_t width;
    uint32_t height;
    int storage;              // 1: FSR writes it (made with VK_IMAGE_USAGE_STORAGE_BIT)
} VkFfxImage;

int vk_ffx_load(void);        // opens amd_fidelityfx_vk beside the exe once; 1 when usable
// Records an upscale of scene (render size) with its depth and motion vectors (render pixels, this frame to the last)
// into dst (output size). jitter_x, jitter_y: the scene's jitter in render pixels; fov_y in radians, 0 if unknown. 1 on success.
int vk_ffx_upscale(VkCommandBuffer cmd, const VkFfxImage *scene, const VkFfxImage *zbuf, const VkFfxImage *vecs,
                   const VkFfxImage *dst, float jitter_x, float jitter_y, int reset, float frame_ms, float fov_y);
void vk_ffx_release(void);    // drops the FSR context; before the device goes away
