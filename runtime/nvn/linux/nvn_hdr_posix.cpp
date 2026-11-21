// SPDX-License-Identifier: MIT
// Linux HDR display-peak query: reads the atomic peak cached on the SDL3 main/event thread
// (zero SDL calls or SDL_Window* dereferences from the NVN present thread).
#include "../nvn_hdr.h"

namespace snap_hdr {

float QueryDisplayPeak(void* /*nativeWindow*/) {
  return CachedDisplayPeak();
}

}  // namespace snap_hdr
