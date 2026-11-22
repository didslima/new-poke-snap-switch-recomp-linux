// SPDX-License-Identifier: MIT
// 1:1 mouse-look injection into the Ride camera controller (f_71004c0090 @ main+0x4c1688).
//
// Hook site: BASE + 0x4c1688 (L_71004c1688 in main_0059.c), immediately after analog stick
// (L_71004c13f4) and gyro integration, and immediately before the game's pitch clamp
// (L_71004c1688..L_71004c171c) and quaternion/camera view build.
// Off by default unless SNAP_MOUSE_AOT_CAMERA=1.
// Sensitivity scales via F1 menu (mouse_sensitivity, mouse_sens_x/y, invert_x/y) in Hub::poll.

#include <cmath>
#include <cstdint>
#include <cstring>

#include "cpu.h"
#include "hle/input_core.h"

namespace {

inline float load_f32(uint64_t addr) {
  const uint32_t bits = LD32(addr);
  float val = 0.0f;
  std::memcpy(&val, &bits, sizeof(val));
  return val;
}

inline void store_f32(uint64_t addr, float val) {
  uint32_t bits = 0;
  std::memcpy(&bits, &val, sizeof(bits));
  ST32(addr, bits);
}

}  // namespace

extern "C" void rt_gm_ride_mouse_cam(Cpu* c) {
  if (!c || !hle::input::aot_camera_enabled()) return;

  float dx = 0.0f;
  float dy = 0.0f;
  bool zoom_active = false;
  if (!hle::input::take_aot_mouse_delta(&dx, &dy, &zoom_active)) return;

  const uint64_t cam_angles = c->x[24];
  if (cam_angles < 0x10000ull) return;

  // Fixed internal base gains (1.00 normal, 2.20 zoom).
  // F1 menu Sensitivity, Horizontal/Vertical sensitivity, and Invert are applied in Hub::poll().
  constexpr float kNormalRadPerPx = 0.0018f * 1.00f;
  constexpr float kZoomRadPerPx = 0.0005f * 2.20f;
  const float rad_per_px = zoom_active ? kZoomRadPerPx : kNormalRadPerPx;

  const float pitch = load_f32(cam_angles + 0x0ull);
  const float yaw = load_f32(cam_angles + 0x4ull);
  if (!std::isfinite(pitch) || !std::isfinite(yaw)) return;

  // Matches f_71004c0090 (main_0059.c:8492-8497):
  // - Right stick UP (ry > 0) adds to [x24+0x0] (pitch); SDL3 mouse UP has dy < 0, so -dy adds to pitch.
  // - Right stick RIGHT (rx > 0) subtracts from [x24+0x4] (yaw); SDL3 mouse RIGHT has dx > 0, so -dx subtracts from yaw.
  const float new_pitch = pitch + (-dy * rad_per_px);
  const float new_yaw = yaw + (-dx * rad_per_px);
  if (!std::isfinite(new_pitch) || !std::isfinite(new_yaw)) return;

  store_f32(cam_angles + 0x0ull, new_pitch);
  store_f32(cam_angles + 0x4ull, new_yaw);
}
