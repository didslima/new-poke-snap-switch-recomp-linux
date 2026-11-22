// SPDX-License-Identifier: MIT
// Image stability in motion (project code, ours): display-only switches that cut shimmer while the camera moves (docs/advanced-graphics-options.md section 11).
//   SNAP_FOLIAGE_STABLE=0|1   Foliage stability (env only, no row: no measured change yet): alpha-tested scene draws keep their coverage in the smaller mip levels (alpha scaled up with the
//                             mip level the texture is read at), so distant leaves, grass and fences stay solid instead of breaking into
//                             flickering holes. The scene's depth pre-pass gets the same test, so the colour pass and its depth agree.
//   SNAP_SPECULAR_AA=0|1      Specular sparkle filter: in the pre-tonemap slot, a pixel far brighter than all but one of its eight neighbours
//                             (a one-pixel specular spark) is pulled down to its neighbourhood. Edges, lines and highlights two pixels or wider keep their value.
// Both apply live (hle/live.h). Photo frames keep the game's own draw: FoliagePipeBits and the Fx slot return early on FxIsPhotoFrame().
//
// Test aid, dev loop only (SNAP_DEV=1): SNAP_AA_PROBE = state * 16 + step. step 1..15 shifts every scene-depth draw by a sub-pixel offset
// (step-1)/8 px right and (step-1)/16 px down (render pixels), so a frozen frame (host\ab_capture.ps1) can be "panned" in steps of 1/8 px;
// the frame-to-frame change is the shimmer a slow pan shows. state bits override the rows while the probe is not 0:
//   1 Leaf edge smoothing, 2 its depth pre-pass, 4 Foliage stability, 8 Specular sparkle filter, 16 paint the alpha-tested draws (foliage mask).
#pragma once

namespace nvn {
struct Host;
struct ReplayCtx;

bool FoliageStableEnabled();     // SNAP_FOLIAGE_STABLE (or the probe state)
bool SpecularAaEnabled();        // SNAP_SPECULAR_AA (or the probe state)
void MotionAaShutdown(Host* h);  // destroys all Vulkan resources created by MotionAA before vkDestroyDevice
// The probe state (bits above) while SNAP_AA_PROBE is not 0, else -1 (the rows as set).
int MotionAaProbeState();
// Sub-pixel offset (render pixels) to add to the viewports emitted for the bound targets: non-zero only for draws into the scene depth target
// while the probe step is not 0 and the present is not a photo frame.
void MotionAaProbeOffset(const ReplayCtx& cx, float* dx, float* dy);
bool MotionAaProbeMoving();      // a probe offset is in use (viewports are emitted again at every SetRenderTargets)

}  // namespace nvn
