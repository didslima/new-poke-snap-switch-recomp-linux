// SPDX-License-Identifier: MIT
// Native Linux AMD FSR 3.1 Super Resolution backend using the vendored MIT FidelityFX SDK v1.1.4
// (third_party/FidelityFX: ffx_fsr3upscaler + Vulkan backend + precompiled SPIR-V permutations).
#include "../upscale/upscaler.h"

#include <bit>
#include <cstdlib>
#include <cstring>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <ffx_shader_blobs.h>
#include <spirv/ffx_fsr3upscaler_vk.h>
#include <spirv/ffx_fsr3upscaler_lanczos_vk.h>

namespace {

// Matches Fs3UpscalerShaderPermutationOptions in ffx_fsr3upscaler_private.h.
constexpr uint32_t kPermUseLanczosLut = (1u << 0);
constexpr uint32_t kPermHdrInput = (1u << 1);
constexpr uint32_t kPermLowResMv = (1u << 2);
constexpr uint32_t kPermJitterMv = (1u << 3);
constexpr uint32_t kPermDepthInverted = (1u << 4);
constexpr uint32_t kPermForceWave64 = (1u << 6);
constexpr uint32_t kPermAllowFp16 = (1u << 7);
constexpr uint32_t kNumFsr3Passes = 12u;

struct State {
  bool tried = false;
  bool loaded = false;
  bool failed = false;
  snap_upscale::DeviceCtx dc{};
  FfxInterface iface{};
  void* scratch = nullptr;
  FfxFsr3UpscalerContext ctx{};
  bool haveCtx = false;
  FfxUInt32 sharedCtx = 0;
  bool haveSharedCtx = false;
  FfxResourceInternal shared[3] = {};
  bool haveShared[3] = {};
  uint32_t ow = 0, oh = 0, mw = 0, mh = 0, flags = 0;
  uint64_t evals = 0;
} F;

void OnMessage(FfxMsgType type, const wchar_t* message) {
  if (!message) return;
  snap_upscale::Logf("fsr: %s: %ls", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
}

VKAPI_ATTR void VKAPI_CALL NoLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
VKAPI_ATTR void VKAPI_CALL NoEndLabel(VkCommandBuffer) {}
VKAPI_ATTR VkResult VKAPI_CALL NoName(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) { return VK_SUCCESS; }

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ProcAddr(VkDevice dev, const char* name) {
  if (!dev || !name) return nullptr;
  PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
  if (f) return f;
  const size_t n = std::strlen(name);
  if (n > 3 && (!std::strcmp(name + n - 3, "KHR") || !std::strcmp(name + n - 3, "EXT"))) {
    char core[128] = {};
    if (n - 3 < sizeof(core)) {
      std::memcpy(core, name, n - 3);
      f = vkGetDeviceProcAddr(dev, core);
      if (f) return f;
    }
  }
  if (!std::strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return reinterpret_cast<PFN_vkVoidFunction>(NoLabel);
  if (!std::strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return reinterpret_cast<PFN_vkVoidFunction>(NoEndLabel);
  if (!std::strcmp(name, "vkSetDebugUtilsObjectNameEXT")) return reinterpret_cast<PFN_vkVoidFunction>(NoName);
  snap_upscale::Logf("fsr: device function %s not available", name);
  return nullptr;
}

FfxResource Res(const snap_upscale::Image& m, uint32_t w, uint32_t h, FfxResourceUsage usage, FfxResourceStates state,
                const wchar_t* name) {
  FfxResourceDescription d{};
  d.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  d.format = ffxGetSurfaceFormatVK(m.format);
  d.width = w;
  d.height = h;
  d.depth = 1;
  d.mipCount = 1;
  d.flags = FFX_RESOURCE_FLAGS_NONE;
  d.usage = usage;
  return ffxGetResourceVK(reinterpret_cast<void*>(m.image), d, name, state);
}

void DestroyContext() {
  if (!F.dc.device) return;
  if (F.haveCtx || F.haveSharedCtx) vkDeviceWaitIdle(F.dc.device);
  for (int i = 0; i < 3; i++) {
    if (F.haveShared[i] && F.iface.fpDestroyResource) {
      F.iface.fpDestroyResource(&F.iface, F.shared[i], F.sharedCtx);
      F.haveShared[i] = false;
      F.shared[i] = {};
    }
  }
  if (F.haveSharedCtx && F.iface.fpDestroyBackendContext) {
    F.iface.fpDestroyBackendContext(&F.iface, F.sharedCtx);
    F.haveSharedCtx = false;
    F.sharedCtx = 0;
  }
  if (F.haveCtx) {
    ffxFsr3UpscalerContextDestroy(&F.ctx);
    F.haveCtx = false;
    std::memset(&F.ctx, 0, sizeof(F.ctx));
  }
  if (F.scratch) {
    std::free(F.scratch);
    F.scratch = nullptr;
  }
  std::memset(&F.iface, 0, sizeof(F.iface));
  F.ow = F.oh = F.mw = F.mh = F.flags = 0;
}

bool CreateContext(const snap_upscale::Eval& e, uint32_t flags) {
  const size_t scratch_sz = ffxGetScratchMemorySizeVK(F.dc.phys, 2);
  if (!scratch_sz) {
    snap_upscale::Logf("fsr: ffxGetScratchMemorySizeVK returned 0");
    return false;
  }
  const size_t aligned_sz = (scratch_sz + 63u) & ~size_t(63u);
  F.scratch = std::aligned_alloc(64, aligned_sz);
  if (!F.scratch) {
    snap_upscale::Logf("fsr: out of memory allocating %zu bytes scratch", aligned_sz);
    return false;
  }
  std::memset(F.scratch, 0, aligned_sz);

  VkDeviceContext dctx{F.dc.device, F.dc.phys, ProcAddr};
  FfxErrorCode rc = ffxGetInterfaceVK(&F.iface, ffxGetDeviceVK(&dctx), F.scratch, aligned_sz, 2);
  if (rc != FFX_OK) {
    snap_upscale::Logf("fsr: ffxGetInterfaceVK failed (%d)", rc);
    DestroyContext();
    return false;
  }

  FfxFsr3UpscalerContextDescription cd{};
  cd.flags = flags;
  cd.maxRenderSize = {e.w, e.h};
  cd.maxUpscaleSize = {e.ow, e.oh};
  cd.fpMessage = OnMessage;
  cd.backendInterface = F.iface;
  rc = ffxFsr3UpscalerContextCreate(&F.ctx, &cd);
  if (rc != FFX_OK) {
    snap_upscale::Logf("fsr: ffxFsr3UpscalerContextCreate failed (%d)", rc);
    DestroyContext();
    return false;
  }
  F.haveCtx = true;

  rc = F.iface.fpCreateBackendContext(&F.iface, FFX_EFFECT_FSR3UPSCALER, nullptr, &F.sharedCtx);
  if (rc != FFX_OK) {
    snap_upscale::Logf("fsr: shared backend context creation failed (%d)", rc);
    DestroyContext();
    return false;
  }
  F.haveSharedCtx = true;

  FfxFsr3UpscalerSharedResourceDescriptions srd{};
  rc = ffxFsr3UpscalerGetSharedResourceDescriptions(&F.ctx, &srd);
  if (rc != FFX_OK) {
    snap_upscale::Logf("fsr: ffxFsr3UpscalerGetSharedResourceDescriptions failed (%d)", rc);
    DestroyContext();
    return false;
  }
  FfxCreateResourceDescription* descs[3] = {
      &srd.dilatedDepth,
      &srd.dilatedMotionVectors,
      &srd.reconstructedPrevNearestDepth,
  };
  for (int i = 0; i < 3; i++) {
    rc = F.iface.fpCreateResource(&F.iface, descs[i], F.sharedCtx, &F.shared[i]);
    if (rc != FFX_OK) {
      snap_upscale::Logf("fsr: shared resource %d creation failed (%d)", i, rc);
      DestroyContext();
      return false;
    }
    F.haveShared[i] = true;
  }

  F.ow = e.ow;
  F.oh = e.oh;
  F.mw = e.w;
  F.mh = e.h;
  F.flags = flags;
  snap_upscale::Logf("fsr: FSR 3.1.4 native Linux Vulkan context %ux%u -> %ux%u, %s colour%s",
                     e.w, e.h, e.ow, e.oh,
                     (flags & FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE) ? "HDR" : "LDR",
                     e.invDepth ? ", inverted depth" : "");
  return true;
}

}  // namespace

extern "C" FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const* /*config*/) {
  return FFX_ERROR_INVALID_ARGUMENT;
}

extern "C" FfxErrorCode ffxIsWave64(FfxEffect /*effectId*/, uint32_t /*permutationOptions*/, bool& isWave64) {
  isWave64 = false;
  return FFX_OK;
}

extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effectId,
                                                     FfxPass passId,
                                                     FfxBindStage bindStage,
                                                     uint32_t permutationOptions,
                                                     FfxShaderBlob* outBlob) {
  if (!outBlob || effectId != FFX_EFFECT_FSR3UPSCALER || bindStage != FFX_BIND_COMPUTE_SHADER_STAGE) {
    return FFX_ERROR_INVALID_ARGUMENT;
  }
  if (passId >= kNumFsr3Passes) return FFX_ERROR_INVALID_ARGUMENT;
  // Never silently substitute an unmatched permutation (e.g. SDR for HDR, non-inverted for inverted depth, or non-LUT for LUT).
  if ((permutationOptions & kPermLowResMv) == 0 ||
      (permutationOptions & kPermJitterMv) != 0 ||
      (permutationOptions & kPermForceWave64) != 0) {
    return FFX_ERROR_INVALID_ARGUMENT;
  }
  const int hdr = (permutationOptions & kPermHdrInput) ? 1 : 0;
  const int inv = (permutationOptions & kPermDepthInverted) ? 1 : 0;
  const int half = (permutationOptions & kPermAllowFp16) ? 1 : 0;
  const bool useLut = (permutationOptions & kPermUseLanczosLut) != 0;
  const FfxShaderBlob& blob =
      (useLut && (passId == FFX_FSR3UPSCALER_PASS_ACCUMULATE || passId == FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN))
          ? ffx_fu_lanczos_blobs[hdr][inv][half][passId - FFX_FSR3UPSCALER_PASS_ACCUMULATE]
          : ffx_fu_blobs[hdr][inv][half][passId];
  if (!blob.data || blob.size == 0) return FFX_ERROR_INVALID_ARGUMENT;
  std::memcpy(outBlob, &blob, sizeof(*outBlob));
  return FFX_OK;
}

namespace snap_upscale::fsr {

bool Load() {
  if (F.tried) return F.loaded;
  F.tried = true;
  F.loaded = true;
  Logf("fsr: built-in FidelityFX SDK v1.1.4 (FSR 3.1.4 Vulkan)");
  return true;
}

bool Init(const DeviceCtx& c) {
  if (!F.loaded) return false;
  if (c.instance == VK_NULL_HANDLE || c.phys == VK_NULL_HANDLE || c.device == VK_NULL_HANDLE) {
    Logf("fsr: invalid Vulkan device context: FSR unavailable");
    F.failed = true;
    return false;
  }
  F.dc = c;
  F.failed = false;
  return true;
}

bool Evaluate(const Eval& e) {
  if (!F.loaded || F.failed || F.dc.device == VK_NULL_HANDLE || F.dc.phys == VK_NULL_HANDLE) return false;
  if (e.cb == VK_NULL_HANDLE || !e.w || !e.h || !e.ow || !e.oh ||
      e.color.image == VK_NULL_HANDLE || e.depth.image == VK_NULL_HANDLE ||
      e.mv.image == VK_NULL_HANDLE || e.out.image == VK_NULL_HANDLE) {
    Logf("fsr: invalid input/output resource: FSR off for this run");
    F.failed = true;
    return false;
  }

  // Keep FSR internal processing in SDR/LDR mode on Linux to avoid white-frame flicker observed on the HDR path,
  // while preserving AUTO_EXPOSURE and preExposure = 1.0f (global NVN/Vulkan HDR presentation is unaffected).
  uint32_t flags = (e.hdr ? FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE : 0u) |
                   (e.invDepth ? FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED : 0u) |
                   (e.infFar ? FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE : 0u);
  if (const char* lg = std::getenv("SNAP_UPSCALE_LOG"); lg && std::atoi(lg) >= 2) {
    flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
  }

  bool reset = e.reset;
  if (F.haveCtx && (F.ow != e.ow || F.oh != e.oh || F.flags != flags || e.w > F.mw || e.h > F.mh)) {
    DestroyContext();
  }
  if (!F.haveCtx) {
    if (!CreateContext(e, flags)) {
      F.failed = true;
      return false;
    }
    reset = true;
  }

  FfxFsr3UpscalerDispatchDescription d{};
  d.commandList = ffxGetCommandListVK(e.cb);
  d.color = Res(e.color, e.w, e.h, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Color");
  d.depth = Res(e.depth, e.w, e.h, FFX_RESOURCE_USAGE_DEPTHTARGET, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Depth");
  d.motionVectors = Res(e.mv, e.w, e.h, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_MV");
  d.dilatedDepth = F.iface.fpGetResource(&F.iface, F.shared[0]);
  d.dilatedMotionVectors = F.iface.fpGetResource(&F.iface, F.shared[1]);
  d.reconstructedPrevNearestDepth = F.iface.fpGetResource(&F.iface, F.shared[2]);
  d.output = Res(e.out, e.ow, e.oh, FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"FSR3_Out");
  d.jitterOffset = {e.jx, e.jy};
  d.motionVectorScale = {1.0f, 1.0f};
  d.renderSize = {e.w, e.h};
  d.upscaleSize = {e.ow, e.oh};
  d.enableSharpening = false;
  d.sharpness = 0.0f;
  d.frameTimeDelta = e.dtMs;
  d.preExposure = 1.0f;
  d.reset = reset;
  d.cameraNear = e.nearZ;
  d.cameraFar = e.farZ;
  d.cameraFovAngleVertical = e.fovY;
  d.viewSpaceToMetersFactor = 1.0f;
  d.flags = 0;

  // FSR's Vulkan backend samples depth in SHADER_READ_ONLY_OPTIMAL; the caller provides DEPTH_STENCIL_READ_ONLY_OPTIMAL.
  auto depthLayout = [&](VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = e.depth.image;
    const bool st = e.depth.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                    e.depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                    e.depth.format == VK_FORMAT_D16_UNORM_S8_UINT;
    b.subresourceRange = {VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (st ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(e.cb, &di);
  };

  depthLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  const FfxErrorCode rc = ffxFsr3UpscalerContextDispatch(&F.ctx, &d);
  depthLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  if (rc != FFX_OK) {
    Logf("fsr: dispatch failed (%d): FSR off for this run", rc);
    F.failed = true;
    return false;
  }
  F.evals++;
  return true;
}

void Release() {
  const uint64_t evals = F.evals;
  const bool had = F.haveCtx || F.haveSharedCtx || F.scratch != nullptr;
  DestroyContext();
  F.dc = DeviceCtx{};
  F.evals = 0;
  F.failed = false;
  F.tried = false;
  F.loaded = false;
  if (had && std::getenv("SNAP_UPSCALE_LOG") && std::atoi(std::getenv("SNAP_UPSCALE_LOG")) > 0) {
    Logf("fsr: released (%llu evaluations)", (unsigned long long)evals);
  }
}

}  // namespace snap_upscale::fsr
