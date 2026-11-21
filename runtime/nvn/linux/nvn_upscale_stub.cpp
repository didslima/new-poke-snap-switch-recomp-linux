// SPDX-License-Identifier: MIT
// Explicit Linux stubs for temporal upscaler backends not yet ported to Linux (XeSS / Frame Generation).
// The front-end (upscale.cpp), NVIDIA DLSS (upscale_dlss.cpp + upscale_dlss_posix.cpp), and AMD FSR 3.1 (upscale_fsr_linux.cpp) are active on Linux.
#include "../upscale/upscaler.h"

namespace snap_upscale {

namespace xess {
bool Load() { return false; }
void InstanceExtensions(std::vector<const char*>& /*ext*/) {}
bool DeviceExtensions(VkInstance /*inst*/, VkPhysicalDevice /*pd*/, std::vector<const char*>& /*ext*/, bool* /*needMut*/) {
  return false;
}
bool Init(const DeviceCtx& /*c*/) { return false; }
bool Evaluate(const Eval& /*e*/) { return false; }
void Release() {}
}  // namespace xess

}  // namespace snap_upscale

namespace snap_framegen {
bool Wanted() { return false; }
}  // namespace snap_framegen
