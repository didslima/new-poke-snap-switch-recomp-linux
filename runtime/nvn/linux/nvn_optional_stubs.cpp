// SPDX-License-Identifier: MIT
// Explicit Linux stubs for optional Windows-only NVN features (stage 6E-A Group 2D-2):
//   - nvn_framegen.h (AMD FSR Frame Generation)
//   - nvn_hdr.h (DXGI HDR10 / scRGB output)
//   - nvn_btnicon.h (WIC button-icon replacement mod)
//   - nvn_freecam.h (WIC freecam clean-shot PNG writer)
//   - nvn_hdphoto.h (WIC HD photo capture)
//   - nvn_shot.h (WIC screenshot PNG writer)
// Each stub reports unsupported/inactive explicitly without altering Windows behavior.
#include "../nvn_btnicon.h"
#include "../nvn_framegen.h"
#include "../nvn_freecam.h"
#include "../nvn_hdphoto.h"
#include "../nvn_shot.h"

#include <cstdint>

namespace nvn {

// ---- nvn_framegen.h ----
bool FgWanted() { return false; }
void FgDeviceCreated(Host* /*h*/) {}
void FgDestroy(Host* /*h*/) {}
void FgForgetTexture(const TextureImpl* /*t*/) {}
const char* FgNotice() { return nullptr; }
void FgFail(const char* /*why*/) {}
bool FgForcedFail(const char* /*stage*/) { return false; }
bool FgActive() { return false; }
bool FgPrepare(ReplayCtx& /*cx*/, const FgScene& /*s*/) { return false; }
void FgAfterDraw(ReplayCtx& /*cx*/, const char* /*fragKey12*/) {}
bool FgBeginPresent(Host* /*h*/, TextureImpl* /*src*/, bool /*hdr*/) { return false; }
VkImage FgRecordGenerate(Host* /*h*/, VkCommandBuffer /*cb*/, TextureImpl* /*src*/) { return VK_NULL_HANDLE; }
void FgSleepHalf() {}
void FgNotePresent(bool /*generated*/) {}
bool FgThreaded() { return false; }
void FgPresentAt(std::function<void()> present, int64_t /*dueNs*/) {
  if (present) present();
}
void FgDrainPresents() {}
int64_t FgReleaseNs() { return 0; }
double FgBaseDt() { return 0.0; }
int64_t FgNowNs() { return 0; }

// ---- nvn_btnicon.h ----
bool BtnIconActive() { return false; }
TextureImpl* BtnIconSubstitute(TextureImpl* t, const BtnDraw& /*d*/) { return t; }
void BtnIconForget(TextureImpl* /*t*/) {}

// ---- nvn_freecam.h ----
void FcShotAfterDraw(ReplayCtx& /*cx*/, const char* /*fragKey12*/) {}
void FcShotAfterSubmit(Host* /*h*/, VkSemaphore /*timeline*/, uint64_t /*value*/) {}

// ---- nvn_hdphoto.h ----
bool HdPhotoOn() { return false; }
void HdPhotoCapture(ReplayCtx& /*cx*/, ProgramImpl* /*p*/, TextureImpl* /*src*/, TextureImpl* /*rt0*/,
                    const float* /*c4*/, const float* /*uv*/) {}
void HdPhotoCompress(const TextureImpl* /*bc*/, const TextureImpl* /*src*/) {}
void HdPhotoCopyTex(const TextureImpl* /*s*/, const TextureImpl* /*d*/) {}
void HdPhotoCopyRegion(ReplayCtx& /*cx*/, TextureImpl* /*s*/, TextureImpl* /*d*/,
                       int /*sx*/, int /*sy*/, int /*sw*/, int /*sh*/) {}
void HdPhotoCopyToBuffer(const TextureImpl* /*t*/, const uint8_t* /*dst*/) {}
void HdPhotoUploadFrom(const TextureImpl* /*t*/, const uint8_t* /*src*/) {}
void HdPhotoTexGone(const TextureImpl* /*t*/) {}
void HdPhotoReadback(uint64_t /*id*/, const uint8_t* /*map*/, uint64_t /*size*/) {}
TextureImpl* HdPhotoSubstitute(TextureImpl* /*t*/, float /*drawnW*/, float /*hostPerUnit*/) { return nullptr; }

// ---- nvn_shot.h ----
void ShotOnPresent(Host* /*h*/, VkCommandBuffer /*cb*/, TextureImpl* /*src*/) {}
void ShotAfterDraw(ReplayCtx& /*cx*/, const char* /*fragKey12*/) {}
void ShotAfterSubmit(Host* /*h*/, VkSemaphore /*timeline*/, uint64_t /*value*/) {}
bool ShotWritePng(const std::string& /*path*/, const uint8_t* /*px*/, uint32_t /*w*/, uint32_t /*h*/, bool /*bgra*/) {
  return false;
}
void ShotPost(std::function<void()> /*job*/) {}
void ShotDrain() {}
void ShotShutdown() {}

}  // namespace nvn

extern "C" void nvnHostFreecamShot(void) {}

extern "C" {
void snap_framegen_state_loaded() {}
__attribute__((weak)) unsigned snap_mods_ab_period() { return 0; }
__attribute__((weak)) bool snap_mods_ab_live(unsigned /*present*/) { return false; }
__attribute__((weak)) const uint8_t* snap_mods_ab_original(const uint8_t* /*src*/, uint32_t /*w*/, uint32_t /*h*/,
                                                           uint32_t /*mips*/, uint32_t* /*ow*/, uint32_t* /*oh*/,
                                                           uint32_t* /*om*/) {
  return nullptr;
}
__attribute__((weak)) void snap_shiny_fx_trigger(const char* /*species*/) {}
}
