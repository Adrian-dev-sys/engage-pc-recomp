// DLAA: NVIDIA DLSS at the scene's own size on Engage (TOTK_DLAA, engage.ini dlaa). Adapted from TotK Recomp 0.3.13
// runtime/vk_dlaa.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh and contributors): NGX loading, its parameter ABI, the
// feature and evaluation are TotK's; where DLAA runs in the frame, the camera and the motion vectors are Engage's.
// NGX is loaded at run time from the driver's NGX core (_nvngx.dll through HKLM\SOFTWARE\NVIDIA Corporation\Global\
// NGXCore); NGX loads the DLSS model from the folder of the executable (nvngx_dlss.dll). No NVIDIA SDK code or header
// is compiled in: the few NGX types used are declared here from the SDK's documented ABI.
//
// The core's parameter object is a C++ class whose virtual functions are called through its vtable: the Set overloads
// in slots 0-7 and the Get overloads in 8-15, each group in reverse declaration order (MSVC). vtable_ok checks the
// layout with a set-then-get round trip before anything relies on it.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_rec.h"
#include "dlaa_mv_spv.h"

typedef int NgxResult;   // NVSDK_NGX_Result: 1 success, 0xBAD00000 | n failures
#define NGX_OK(r) (((unsigned)(r) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x15
#define NGX_FEATURE_SUPERSAMPLING 1
#define NGX_PERFQ_DLAA 5
typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
typedef struct { void *slot[17]; } NgxParamVtbl;
struct NgxParam { const NgxParamVtbl *vt; };
typedef struct {   // NVSDK_NGX_ImageViewInfo_VK
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned width, height;
} NgxImageView;
typedef struct {   // NVSDK_NGX_Resource_VK
    union { NgxImageView img; struct { VkBuffer buffer; unsigned bytes; } buf; } r;
    int type;      // 0 image view, 1 buffer
    _Bool rw;
} NgxResourceVK;

typedef NgxResult (*PFN_Init)(unsigned long long app, const wchar_t *path, VkInstance, VkPhysicalDevice, VkDevice,
                              PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const void *info);
typedef NgxResult (*PFN_ReqExt)(unsigned *ni, const char ***ie, unsigned *nd, const char ***de);
typedef NgxResult (*PFN_Params)(NgxParam **);
typedef NgxResult (*PFN_DestroyParams)(NgxParam *);
typedef NgxResult (*PFN_Create)(VkCommandBuffer, int feature, NgxParam *, NgxHandle **);
typedef NgxResult (*PFN_Evaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *callback);
typedef NgxResult (*PFN_Release)(NgxHandle *);

static struct {
    void *core;
    PFN_Init init;
    PFN_ReqExt req_ext;
    PFN_Params caps, alloc_params;
    PFN_DestroyParams destroy_params;
    PFN_Create create;
    PFN_Evaluate evaluate;
    PFN_Release release;
    int tried, loaded, available;
} N;

typedef void (*SetI)(NgxParam *, const char *, int);
typedef void (*SetU)(NgxParam *, const char *, unsigned);
typedef void (*SetF)(NgxParam *, const char *, float);
typedef void (*SetP)(NgxParam *, const char *, void *);
typedef NgxResult (*GetI)(const NgxParam *, const char *, int *);
typedef NgxResult (*GetU)(const NgxParam *, const char *, unsigned *);
typedef NgxResult (*GetF)(const NgxParam *, const char *, float *);
typedef NgxResult (*GetP)(const NgxParam *, const char *, void **);
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64 };
static void p_seti(NgxParam *p, const char *n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
static void p_setu(NgxParam *p, const char *n, unsigned v) { ((SetU)p->vt->slot[S_U32])(p, n, v); }
static void p_setf(NgxParam *p, const char *n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
static void p_setp(NgxParam *p, const char *n, void *v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
static int p_geti(const NgxParam *p, const char *n, int *v) { return NGX_OK(((GetI)p->vt->slot[G_I])(p, n, v)); }
static int p_getu(const NgxParam *p, const char *n, unsigned *v) { return NGX_OK(((GetU)p->vt->slot[G_U32])(p, n, v)); }
static int p_getf(const NgxParam *p, const char *n, float *v) { return NGX_OK(((GetF)p->vt->slot[G_F])(p, n, v)); }
static int p_getp(const NgxParam *p, const char *n, void **v) { return NGX_OK(((GetP)p->vt->slot[G_P])(p, n, v)); }

// NGX is brought up only when dlaa is on at start: its device extensions can only be asked for at device creation.
// So dlaa needs a restart to come up; while up, the menu switches it live.
int vk_dlaa_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = cfg_bool("TOTK_DLAA");
    return wanted;
}

static int core_load(void) {
    if (N.tried) return N.loaded;
    N.tried = 1;
    char path[PLAT_PATH_MAX];
    if (!plat_ngx_core_path(path, sizeof path)) {
        rt_log("dlaa: the driver has no NGX core (registry NGXCore missing): DLAA off");
        return 0;
    }
    N.core = plat_lib_open(path);
    if (!N.core) {
        rt_log("dlaa: cannot load %s (not an NVIDIA driver, or one without NGX): DLAA off", path);
        return 0;
    }
#define FN(f, name) N.f = plat_lib_sym(N.core, name)
    FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
    FN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
    FN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    FN(alloc_params, "NVSDK_NGX_VULKAN_AllocateParameters");
    FN(destroy_params, "NVSDK_NGX_VULKAN_DestroyParameters");
    FN(create, "NVSDK_NGX_VULKAN_CreateFeature");
    FN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
    FN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
#undef FN
    if (!N.init || !N.req_ext || !N.caps || !N.alloc_params || !N.create || !N.evaluate || !N.release) {
        rt_log("dlaa: the NGX core %s lacks Vulkan entry points: DLAA off", path);
        return 0;
    }
    N.loaded = 1;
    rt_log("dlaa: NGX core %s", path);
    return 1;
}

static int listed(const char *const *list, uint32_t n, const char *s) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}

void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < ni; i++)
        if (!listed(ext, *n, ie[i]) && *n < max) ext[(*n)++] = ie[i];
}

void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n,
                         uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < nd; i++) {
        int ok = 0;
        for (uint32_t k = 0; k < nhave && !ok; k++) ok = !strcmp(have[k].extensionName, de[i]);
        if (!ok) rt_log("dlaa: the device lacks %s, which NGX asks for", de[i]);
        else if (!listed(ext, *n, de[i]) && *n < max) ext[(*n)++] = de[i];
    }
}

static int vtable_ok(void) {
    NgxParam *p = NULL;
    if (!NGX_OK(N.alloc_params(&p)) || !p) return 0;
    int i = 0;
    unsigned u = 0;
    float f = 0;
    void *v = NULL;
    p_seti(p, "Engage.Test.I", -12345);
    p_setu(p, "Engage.Test.U", 4000000000u);
    p_setf(p, "Engage.Test.F", 1.25f);
    p_setp(p, "Engage.Test.P", (void *)p);
    int ok = p_geti(p, "Engage.Test.I", &i) && i == -12345 && p_getu(p, "Engage.Test.U", &u) && u == 4000000000u &&
             p_getf(p, "Engage.Test.F", &f) && f == 1.25f && p_getp(p, "Engage.Test.P", &v) && v == (void *)p;
    if (N.destroy_params) N.destroy_params(p);
    if (!ok) rt_log("dlaa: NGX parameter layout check failed (i=%d u=%u f=%g p=%p): DLAA off", i, u, f, v);
    return ok;
}

void vk_dlaa_init(void) {
    if (!vk_dlaa_wanted() || !core_load()) return;
    wchar_t wdir[PLAT_PATH_MAX];
    if (!plat_exe_dir_wide(wdir, PLAT_PATH_MAX)) {
        rt_log("dlaa: cannot name the executable's folder for NGX: DLAA off");
        return;
    }
    // 231313132: the application id NVIDIA's DLSS samples use for builds without an NVIDIA-issued id
    const VkCtx *c = vk_ctx();
    NgxResult r = N.init(231313132ull, wdir, c->instance, c->phys, c->device, vkGetInstanceProcAddr,
                         vkGetDeviceProcAddr, NGX_VERSION_API, NULL);
    if (!NGX_OK(r)) {
        rt_log("dlaa: NGX init failed (%08x): DLAA off", (unsigned)r);
        return;
    }
    if (!vtable_ok()) return;
    NgxParam *caps = NULL;
    int avail = 0, driver = 0;
    if (NGX_OK(N.caps(&caps)) && caps) {
        p_geti(caps, "SuperSampling.Available", &avail);
        p_geti(caps, "SuperSampling.NeedsUpdatedDriver", &driver);
    }
    N.available = avail != 0;
    rt_log("dlaa: NGX up; DLSS %s%s", avail ? "available" : "NOT available (is nvngx_dlss.dll next to engage.exe?)",
           driver ? "; the driver needs an update" : "");
}

int vk_dlaa_available(void) { return vk_dlaa_wanted() && N.available; }

// ---------------------------------------------------------------- where DLAA runs in Engage's frame
// Measured (TOTK_DUMP at the title menu): Unity's forward renderer draws the scene (opaque, sky, transparents) into one
// R11G11B10F light target at the render size, with the scene depth (D24S8, a D32+S8 image here) attached; then the
// bloom starts at half the width (960x540 under a 1920x1080 scene), and its chain, the tonemap into the RGBA8 sRGB
// output and the UI follow. So: a pass at the render size with one R11G11B10F/RGBA16F target and the depth arms the
// frame; the first pass at most half as wide ends the scene and runs DLAA in place on the armed target first. Bloom,
// tonemap and UI then work on the anti-aliased picture. Asset-sized targets (the character models, resolution.c
// res_rt_scale) are not the render size and never arm.
// The camera: the scene's vertex shaders read Unity's view-projection from c3 (0x40 bytes, four vec4 columns). The block
// bound to the last scene draw is copied on the GPU at the DLAA point, so it holds what the scene was drawn with.
static struct {
    VkExecTarget color, depth;   // the armed pair, valid in the frame it was armed
    int armed, done, ended;
    struct Size {
        uint32_t w, h;
        VkImage out, mv;
        VmaAllocation out_mem, mv_mem;
        VkImageView out_view, mv_view;
        NgxHandle *feat;
        uint64_t used;
    } sizes[3], *last;
    VkImage last_color;
    NgxParam *params;
    int failed, reset;
    uint64_t evals, frames_missed, mv_frames, mv_none, holds;
    // jitter
    VkImage jdepth;              // the scene depth of the last frame DLAA ran on: passes drawing with it are jittered
    float jx, jy, ux, uy;
    uint32_t jframe;
    int jit_ok, jit_used, hold;
    // camera
    uint64_t cam_addr;           // this frame's c3 block of a scene draw, 0: none seen
    VkBuffer camb;               // two slots of CAM_SLOT bytes: this frame's view-projection and last frame's
    VmaAllocation camb_mem;
    int cam_slot, cam_prev_ok;
    VkDescriptorSetLayout mv_dsl;
    VkPipelineLayout mv_layout;
    VkPipeline mv_pipe;
    VkSampler mv_samp;
    int mv_ok;
    int on, preset, preset_dirty;
    unsigned cfg_seen;
    uint64_t last_eval_ns;
} D = {.mv_ok = -1, .cfg_seen = ~0u};
#define CAM_BYTES 64u
#define CAM_SLOT 256u

static int dl_active(void) { return N.available && !D.failed && D.on; }

static void dl_fail(const char *what, NgxResult r) {
    rt_log("dlaa: %s failed (%08x): DLAA off for this run", what, (unsigned)r);
    D.failed = 1;
}

static int image2d(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkImage *img, VmaAllocation *mem,
                   VkImageView *view) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, view) == VK_SUCCESS;
}

static void free_size(struct Size *e) {
    const VkCtx *c = vk_ctx();
    vkDeviceWaitIdle(c->device);
    if (e->out_view) vkDestroyImageView(c->device, e->out_view, NULL);
    if (e->mv_view) vkDestroyImageView(c->device, e->mv_view, NULL);
    if (e->out) vmaDestroyImage(c->vma, e->out, e->out_mem);
    if (e->mv) vmaDestroyImage(c->vma, e->mv, e->mv_mem);
    if (e->feat) N.release(e->feat);
    memset(e, 0, sizeof *e);
}

static struct Size *size_for(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    struct Size *e = NULL, *lru = &D.sizes[0];
    for (int i = 0; i < 3; i++) {
        if (D.sizes[i].w == w && D.sizes[i].h == h) return &D.sizes[i];
        if (!D.sizes[i].out && !e) e = &D.sizes[i];
        if (D.sizes[i].used < lru->used) lru = &D.sizes[i];
    }
    if (!e) {
        if (D.last == lru) D.last = NULL;
        free_size(e = lru);
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image2d(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u, &e->out, &e->out_mem, &e->out_view) ||
        !image2d(w, h, VK_FORMAT_R16G16_SFLOAT, u, &e->mv, &e->mv_mem, &e->mv_view))
        return NULL;
    VkImageMemoryBarrier2 ib[2];
    VkImage im[2] = {e->out, e->mv};
    for (int i = 0; i < 2; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    e->w = w, e->h = h;
    return e;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static NgxResourceVK res_image(VkImageView v, VkImage img, VkFormat f, uint32_t w, uint32_t h, VkImageAspectFlags a,
                               int rw) {
    NgxResourceVK r;
    memset(&r, 0, sizeof r);
    r.r.img = (NgxImageView){v, img, {a, 0, 1, 0, 1}, f, w, h};
    r.type = 0;
    r.rw = rw;
    return r;
}

// ---------------------------------------------------------------- motion vectors (shaders/dlaa_mv.comp)
static int mv_init(void) {
    if (D.mv_ok >= 0) return D.mv_ok;
    D.mv_ok = 0;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_mv_spv,
                                   .pCode = dlaa_mv_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2 * CAM_SLOT,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &D.mv_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &D.mv_dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &D.mv_layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &D.mv_samp) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS ||
        vmaCreateBuffer(c->vma, &bi, &ai, &D.camb, &D.camb_mem, NULL) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = D.mv_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &D.mv_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return D.mv_ok = 1;
fail:
    rt_log("dlaa: the motion-vector pass could not be set up: DLAA runs with zero motion");
    return 0;
}

// This frame's camera block into its slot, then the vectors from it and last frame's. 0: no camera or no last frame
// (the vectors stay zero and DLSS starts a new history).
static int mv_pass(VkExec *ex, struct Size *e, VkImageView zv, float jx, float jy) {
    VkCommandBuffer cb = ex->cb;
    VkDescriptorBufferInfo cam;
    VkMemSpan sp;
    int have = 0;
    if (mv_init() && D.cam_addr) {
        if (vk_usnap_find(D.cam_addr, CAM_BYTES, 4, &cam)) have = 1;
        else if (vk_mem_resolve(D.cam_addr, CAM_BYTES, &sp)) {
            VK_MEM_PICK(sp);
            cam = (VkDescriptorBufferInfo){sp.buffer, sp.offset, CAM_BYTES};
            have = 1;
        }
    }
    if (!have) {
        D.mv_none++;
        D.cam_prev_ok = 0;
        return 0;
    }
    int s = D.cam_slot, had_prev = D.cam_prev_ok;
    VkBufferCopy bc = {cam.offset, (VkDeviceSize)s * CAM_SLOT, CAM_BYTES};
    vkCmdCopyBuffer(cb, cam.buffer, D.camb, 1, &bc);
    full_barrier(cb);
    D.cam_slot ^= 1, D.cam_prev_ok = 1;
    if (!had_prev) return 0;
    uint32_t w = e->w, h = e->h;
    VkDescriptorImageInfo zi = {D.mv_samp, zv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, e->mv_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo cur = {D.camb, (VkDeviceSize)s * CAM_SLOT, CAM_BYTES};
    VkDescriptorBufferInfo prev = {D.camb, (VkDeviceSize)(s ^ 1) * CAM_SLOT, CAM_BYTES};
    VkWriteDescriptorSet wr[4] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &zi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &cur},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &prev}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_layout, 0, 4, wr);
    static int flip = -1;
    if (flip < 0) flip = rt_knob("DLAA_FLIPY", "0");   // TOTK_DLAA_FLIPY=1 (test): the other window origin
    float pc[6] = {(float)w, (float)h, jx, jy, nvn_depth_minus_w() ? 1.0f : 0.0f, flip ? 1.0f : 0.0f};
    vkCmdPushConstants(cb, D.mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    D.mv_frames++;
    return 1;
}

// ---------------------------------------------------------------- jitter
// The scene's draws (every pass drawing with the scene depth, until the DLAA point) have their viewport moved by a
// sub-pixel Halton(2,3) offset. Jitter only follows a frame DLSS ran on: a jittered frame that DLSS does not resolve
// shakes on screen, so a missed DLAA point stops the jitter for a second.
static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

void vk_dlaa_viewport(VkExec *ex, VkViewport *v) {
    if (!D.jit_ok || D.ended || !ex->depth.image || ex->depth.image != D.jdepth) return;
    v->x += D.jx, v->y += D.jy;
    D.ux = D.jx, D.uy = D.jy, D.jit_used = 1;
}

// A draw into the armed scene pair: its vertex stage's c3 is the camera.
void vk_dlaa_draw(VkExec *ex) {
    if (!dl_active() || D.ended || !D.armed) return;
    if (ex->color[0].image != D.color.image || ex->depth.image != D.depth.image) return;
    const p_bind_buf *b = &ex->ubo[NVN_SHADER_STAGE_VERTEX][0];
    if (b->addr && b->size == CAM_BYTES) D.cam_addr = b->addr;
}

static void next_jitter(void) {
    static int nj = -1;
    if (nj < 0) nj = !rt_knob("DLAA_JITTER", "1");   // TOTK_DLAA_JITTER=0: no jitter (test)
    uint32_t i = D.jframe++ % 16 + 1;
    D.jx = halton(i, 2) - 0.5f, D.jy = halton(i, 3) - 0.5f;
    D.jit_ok = !D.hold && !nj;
}

// TOTK_DLSS_PRESET (live): default takes NVIDIA's model for DLAA (K); a letter a..m pins that model.
static int preset_value(void) {
    const char *v = cfg_get("TOTK_DLSS_PRESET");
    int c = v && v[0] && !v[1] ? v[0] | 0x20 : 0;
    return c >= 'a' && c <= 'm' ? c - 'a' + 1 : 0;
}

static void evaluate(VkExec *ex) {
    const VkExecTarget *t = &D.color, *z = &D.depth;
    uint32_t w = t->width, h = t->height;
    VkCommandBuffer cb = ex->cb;
    vk_exec_barrier(ex);
    if (D.preset_dirty) {
        for (int i = 0; i < 3; i++)
            if (D.sizes[i].out) free_size(&D.sizes[i]);
        D.last = NULL, D.preset_dirty = 0;
    }
    struct Size *e = size_for(cb, w, h);
    if (!e) return dl_fail("work image creation", 0);
    if (e != D.last) D.reset = 1;
    if (!D.params && !NGX_OK(N.alloc_params(&D.params))) return dl_fail("parameter allocation", 0);
    NgxParam *p = D.params;
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    float jx = D.jit_used ? D.ux : 0.0f, jy = D.jit_used ? D.uy : 0.0f;
    static int no_mv = -1;
    if (no_mv < 0) no_mv = !rt_knob("DLAA_MV", "1");   // TOTK_DLAA_MV=0: zero motion (test)
    if (no_mv || !mv_pass(ex, e, zv, jx, jy)) {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        full_barrier(cb);
        if (!no_mv) D.reset = 1;
    }
    VkImageView cv = vk_exec_target_view(t->img, t->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, t->level,
                                         t->base_layer, 1, 0);
    rec_drain();   // NGX records into the command buffer itself: the recorder thread must have made every call so far
    if (!e->feat) {
        int pk = D.preset ? D.preset : 11;   // K (transformer), NVIDIA's model for DLAA
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", w);
        p_setu(p, "Height", h);
        p_setu(p, "OutWidth", w);
        p_setu(p, "OutHeight", h);
        p_seti(p, "PerfQualityValue", NGX_PERFQ_DLAA);
        // IsHDR | MVLowRes | AutoExposure: linear scene light, motion vectors at the render size, no exposure texture
        p_seti(p, "DLSS.Feature.Create.Flags", (1 << 0) | (1 << 1) | (1 << 6));
        p_seti(p, "DLSS.Enable.Output.Subrects", 0);
        p_setu(p, "DLSS.Hint.Render.Preset.DLAA", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Quality", (unsigned)pk);
        NgxResult r = N.create(cb, NGX_FEATURE_SUPERSAMPLING, p, &e->feat);
        if (!NGX_OK(r) || !e->feat) return dl_fail("DLSS feature creation", r);
        D.reset = 1;
        rt_log("dlaa: DLSS feature created at %ux%u, preset %c (scene format %d, depth format %d)", w, h, 'A' + pk - 1,
               (int)t->format, (int)z->format);
    }
    if (D.last_color != t->image) D.reset = 1, D.last_color = t->image;
    NgxResourceVK rc = res_image(cv, t->image, t->format, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK ro = res_image(e->out_view, e->out, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    NgxResourceVK rz = res_image(zv, z->image, z->format, w, h, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
    NgxResourceVK rm = res_image(e->mv_view, e->mv, VK_FORMAT_R16G16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "Color", &rc);
    p_setp(p, "Output", &ro);
    p_setp(p, "Depth", &rz);
    p_setp(p, "MotionVectors", &rm);
    p_setf(p, "Jitter.Offset.X", jx);
    p_setf(p, "Jitter.Offset.Y", jy);
    p_setf(p, "Sharpness", 0.0f);
    p_seti(p, "Reset", D.reset);
    p_setp(p, "DLSS.Input.Bias.Current.Color.Mask", NULL);
    p_setf(p, "MV.Scale.X", 1.0f);
    p_setf(p, "MV.Scale.Y", 1.0f);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", w);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", h);
    p_setf(p, "DLSS.Pre.Exposure", 1.0f);
    p_setf(p, "DLSS.Exposure.Scale", 1.0f);
    uint64_t now = plat_now_ns();
    double dt = D.last_eval_ns ? (now - D.last_eval_ns) / 1e6 : 0;
    D.last_eval_ns = now;
    p_setf(p, "FrameTimeDeltaInMsec", dt > 0 && dt < 100 ? (float)dt : 16.67f);
    NgxResult r = N.evaluate(cb, e->feat, p, NULL);
    if (!NGX_OK(r)) return dl_fail("DLSS evaluation", r);
    D.reset = 0;
    D.last = e, e->used = D.evals + 1;
    full_barrier(cb);
    // TOTK_DLAA_MVDEBUG=1 (test): the motion vectors in place of the picture, x in red and y in green
    static int mvdebug = -1;
    if (mvdebug < 0) mvdebug = rt_knob("DLAA_MVDEBUG", "0");
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                      .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vkCmdBlitImage(cb, mvdebug ? e->mv : e->out, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                   VK_FILTER_NEAREST);
    full_barrier(cb);
    ex->recorded += 4;
    vk_exec_draw_new_cb();   // NGX bound pipelines and descriptor sets of its own: the next draw binds everything again
    D.jdepth = z->image;
    next_jitter();
    if (++D.evals == 1 || D.evals == 300 || D.evals % 3600 == 0)
        rt_log("dlaa: %llu frames anti-aliased (scene %ux%u; %llu frames without the DLAA point, %llu with camera motion, "
               "%llu without a camera, %llu jitter holds)",
               (unsigned long long)D.evals, w, h, (unsigned long long)D.frames_missed,
               (unsigned long long)D.mv_frames, (unsigned long long)D.mv_none, (unsigned long long)D.holds);
}

static int scene_color(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

void vk_dlaa_pass(VkExec *ex) {
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    if (!dl_active() || D.done) return;
    unsigned rw, rh;
    res_engage_render_size(&rw, &rh);
    if (ex->pass_ncolor == 1 && c->image && scene_color(c->format) && z->image && c->width == rw && c->height == rh &&
        z->width == rw && z->height == rh && c->samples <= 1 && !c->is3d) {
        if (!D.armed || c->image == D.color.image) D.color = *c, D.depth = *z, D.armed = 1;
        return;
    }
    if (!D.armed || D.ended) return;
    if (!ex->pass_w || ex->pass_w * 2 > D.color.width) return;
    D.ended = D.done = 1;
    evaluate(ex);
}

static void preset_check(void) {
    unsigned v = cfg_version();
    if (v == D.cfg_seen) return;
    D.cfg_seen = v;
    int p = preset_value();
    if (p == D.preset) return;
    if (D.evals) rt_log("dlaa: DLSS preset %s; the feature is created again", p ? (char[2]){(char)('A' + p - 1), 0} : "default");
    D.preset = p, D.preset_dirty = 1, D.reset = 1;
}

void vk_dlaa_frame(void) {
    preset_check();
    int on = N.available && cfg_bool("TOTK_DLAA");
    if (on != D.on) {
        rt_log("dlaa: %s", on ? "on" : "off");
        D.on = on;
        D.reset = 1, D.cam_prev_ok = 0, D.jit_ok = D.jit_used = D.armed = D.done = D.ended = 0;
        D.hold = 0;
        return;
    }
    if (!dl_active()) return;
    if (!D.done) D.frames_missed++;
    if (D.jit_used && !D.done) D.hold = 60, D.holds++;   // jittered but not resolved: that frame shook
    else if (D.hold) D.hold--;
    if (!D.done) D.jit_ok = 0;
    D.armed = D.done = D.jit_used = D.ended = 0;
    D.cam_addr = 0;
}
