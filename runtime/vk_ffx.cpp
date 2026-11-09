// FSR 3.1 temporal upscaling for vk_dlaa.c (its fallback when DLSS is missing, or TOTK_AA_UPSCALER = fsr3). The work is
// done by AMD's FidelityFX API library, amd_fidelityfx_vk.dll beside engage.exe, opened at run time; this file only
// fills the API's descriptors (third_party/ffx_api, MIT, Advanced Micro Devices). C++ for those headers' initialisers.
#include <stdio.h>
#include <string.h>

#include "third_party/ffx_api/ffx_api.h"
#include "third_party/ffx_api/ffx_upscale.h"
#include "third_party/ffx_api/vk/ffx_api_vk.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_ffx.h"
}

static struct {
    int state;   // 0 not tried, 1 usable, -1 missing
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxDispatch dispatch;
    PfnFfxQuery query;
} lib;

static struct {
    ffxContext ctx;   // NULL until the first upscale at a size
    uint32_t w, h, ow, oh, flags;
} up;

// The library's messages, a few of them, to the log.
static void on_message(uint32_t type, const wchar_t *text) {
    static int count;
    if (count >= 32) return;
    count++;
    char s[256];
    snprintf(s, sizeof s, "%ls", text ? text : L"");
    rt_log("ffx: %s %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", s);
}

extern "C" int vk_ffx_load(void) {
    if (lib.state) return lib.state > 0;
    lib.state = -1;
    char path[1024];
    if (!plat_exe_path(path, sizeof path)) return 0;
    char *cut = plat_last_sep(path);
    if (!cut || (size_t)(cut - path) + 32 > sizeof path) return 0;
#ifdef _WIN32
    strcpy(cut + 1, "amd_fidelityfx_vk.dll");
#else
    strcpy(cut + 1, "libamd_fidelityfx_vk.so");
#endif
    void *h = plat_lib_open(path);
    if (!h) {
        rt_log("ffx: no %s: FSR 3.1 unavailable", cut + 1);
        return 0;
    }
    lib.create = (PfnFfxCreateContext)plat_lib_sym(h, "ffxCreateContext");
    lib.destroy = (PfnFfxDestroyContext)plat_lib_sym(h, "ffxDestroyContext");
    lib.dispatch = (PfnFfxDispatch)plat_lib_sym(h, "ffxDispatch");
    lib.query = (PfnFfxQuery)plat_lib_sym(h, "ffxQuery");
    if (lib.create && lib.destroy && lib.dispatch) lib.state = 1;
    rt_log("ffx: %s %s", cut + 1, lib.state > 0 ? "opened" : "has no FidelityFX API: FSR 3.1 unavailable");
    return lib.state > 0;
}

extern "C" void vk_ffx_release(void) {
    if (up.ctx && lib.destroy) lib.destroy(&up.ctx, NULL);
    up.ctx = NULL;
}

// A context per render and output size and flag set: made on first use, again when one of them changes.
static int context(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t flags) {
    if (up.ctx && up.w == w && up.h == h && up.ow == ow && up.oh == oh && up.flags == flags) return 1;
    VkCtx *vk = vk_ctx();
    vkDeviceWaitIdle(vk->device);   // the old context's resources may still be in flight
    vk_ffx_release();
    ffxCreateBackendVKDesc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = vk->device;
    backend.vkPhysicalDevice = vk->phys;
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescUpscale desc = {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.flags = flags;
    desc.maxRenderSize.width = w, desc.maxRenderSize.height = h;
    desc.maxUpscaleSize.width = ow, desc.maxUpscaleSize.height = oh;
    desc.fpMessage = on_message;
    ffxReturnCode_t rc = lib.create(&up.ctx, &desc.header, NULL);
    if (rc != FFX_API_RETURN_OK) {
        up.ctx = NULL;
        rt_log("ffx: no FSR 3.1 context for %ux%u -> %ux%u (code %u)", w, h, ow, oh, (unsigned)rc);
        return 0;
    }
    up.w = w, up.h = h, up.ow = ow, up.oh = oh, up.flags = flags;
    const char *name = "";
    if (lib.query) {
        ffxQueryGetProviderVersion q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (lib.query(&up.ctx, &q.header) == FFX_API_RETURN_OK && q.versionName) name = q.versionName;
    }
    rt_log("ffx: FSR 3.1 context %ux%u -> %ux%u %s", w, h, ow, oh, name);
    return 1;
}

// One of our images as an API resource: a 2D texture, read or written by compute, in VK_IMAGE_LAYOUT_GENERAL.
static FfxApiResource resource(const VkFfxImage *im) {
    FfxApiResourceDescription d = {};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    d.format = ffxApiGetSurfaceFormatVK(im->format);
    d.width = im->width, d.height = im->height;
    d.depth = 1, d.mipCount = 1;
    d.usage = im->storage ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    if (ffxApiIsDepthFormat(im->format)) d.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    return ffxApiGetResourceVK((void *)im->image, d, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

extern "C" int vk_ffx_upscale(VkCommandBuffer cmd, const VkFfxImage *scene, const VkFfxImage *zbuf, const VkFfxImage *vecs,
                              const VkFfxImage *dst, float jitter_x, float jitter_y, int reset, float frame_ms, float fov_y) {
    if (lib.state <= 0) return 0;
    // The scene is linear HDR with no exposure value of its own. Unity on Switch writes reversed Z (near 1, far 0);
    // TOTK_FSR_DEPTH_INVERTED=0 is a test switch for the other direction.
    uint32_t flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    const char *inv = cfg_get("TOTK_FSR_DEPTH_INVERTED");
    if (!inv || strcmp(inv, "0")) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    int renew = !up.ctx || up.w != scene->width || up.h != scene->height;
    if (!context(scene->width, scene->height, dst->width, dst->height, flags)) return 0;
    ffxDispatchDescUpscale job = {};
    job.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    job.commandList = (void *)cmd;
    job.color = resource(scene);
    job.depth = resource(zbuf);
    job.motionVectors = resource(vecs);   // render pixels, from this frame's position to the previous one
    job.output = resource(dst);
    job.jitterOffset.x = jitter_x, job.jitterOffset.y = jitter_y;
    job.motionVectorScale.x = 1, job.motionVectorScale.y = 1;
    job.renderSize.width = scene->width, job.renderSize.height = scene->height;
    job.upscaleSize.width = dst->width, job.upscaleSize.height = dst->height;
    double sharp = cfg_double("TOTK_SHARPNESS") / 100;   // the Screen tab's sharpness, 0..100
    job.enableSharpening = sharp > 0.001;
    job.sharpness = (float)(sharp < 0 ? 0 : sharp > 1 ? 1 : sharp);
    job.frameTimeDelta = frame_ms;
    job.preExposure = 1;
    job.reset = reset || renew;
    job.cameraNear = 0.3f;   // Unity's default near and far planes, in metres
    job.cameraFar = 1000;
    job.cameraFovAngleVertical = fov_y > 0 ? fov_y : 0.6f;   // about 35 degrees when the caller does not know
    job.viewSpaceToMetersFactor = 1;
    ffxReturnCode_t rc = lib.dispatch(&up.ctx, &job.header);
    static int failures;
    if (rc != FFX_API_RETURN_OK && failures++ < 4) rt_log("ffx: FSR 3.1 dispatch failed (code %u)", (unsigned)rc);
    return rc == FFX_API_RETURN_OK;
}
