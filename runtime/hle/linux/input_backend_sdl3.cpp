// SPDX-License-Identifier: MIT
// Stage 6C-B2: Linux SDL3 input backend (and NullBackend when built headless without SDL3).
// Replaces the weak Stage 6C-B1 make_default_backend() in hle_host_posix.cpp.
#if !defined(__linux__)
#error "runtime/hle/linux/input_backend_sdl3.cpp is the Linux implementation of make_default_backend()"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "../hle.h"
#include "../input_core.h"

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

namespace hle::input {

#ifdef HLE_WITH_SDL3
namespace {

void merge_pad(RawInput& dst, const RawInput& src) {
  if (!src.pad_connected) return;
  dst.pad_connected = true;
  if (dst.pad_family == kPadFamNone) dst.pad_family = src.pad_family;
  dst.pad |= src.pad;
  auto big = [](float& d, float s) {
    if (std::fabs(s) > std::fabs(d)) d = s;
  };
  big(dst.lx, src.lx);
  big(dst.ly, src.ly);
  big(dst.rx, src.rx);
  big(dst.ry, src.ry);
  dst.lt = std::max(dst.lt, src.lt);
  dst.rt = std::max(dst.rt, src.rt);
  if (src.gyro_present) {
    dst.gyro_present = true;
    dst.gyro_x = src.gyro_x;
    dst.gyro_y = src.gyro_y;
    dst.gyro_z = src.gyro_z;
  }
}

int family_of(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return kPadFamPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return kPadFamSwitch;
    default:
      return kPadFamXbox;
  }
}

class Sdl3Backend final : public Backend {
 public:
  Sdl3Backend() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    inited_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
    if (!inited_) log("sdl3: gamepad init failed: %s", SDL_GetError());
  }

  ~Sdl3Backend() override {
    {
      std::lock_guard<std::mutex> lk(wake_mu_);
      stop_.store(true);
    }
    wake_cv_.notify_all();
    if (scan_thread_.joinable()) scan_thread_.join();
    if (gyro_thread_.joinable()) gyro_thread_.join();
    {
      std::lock_guard<std::mutex> lk(pads_mu_);
      for (SDL_Gamepad* p : pads_) {
        if (p) SDL_CloseGamepad(p);
      }
      pads_.clear();
    }
    if (inited_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
  }

  const char* name() const override { return "sdl3"; }

  void set_cursor_mode(bool lock, bool recenter) override {
    lock_.store(lock, std::memory_order_relaxed);
    recenter_.store(recenter, std::memory_order_relaxed);
    set_sdl3_cursor_mode(lock, recenter);
  }

  void poll(RawInput& out) override {
    if (!inited_) return;
    const uint64_t now_ns = static_cast<uint64_t>(SDL_GetTicksNS());
    if (scan_threaded_) {
      if (!scan_thread_.joinable()) {
        scan_thread_ = std::thread([this] {
          while (!stop_.load()) {
            {
              std::lock_guard<std::mutex> slk(scan_mu_);
              scan_pads();
            }
            std::unique_lock<std::mutex> wlk(wake_mu_);
            wake_cv_.wait_for(wlk, std::chrono::milliseconds(1000), [this] { return stop_.load(); });
          }
        });
      }
      bool any_connected = false;
      {
        std::lock_guard<std::mutex> plk(pads_mu_);
        for (SDL_Gamepad* p : pads_) {
          if (SDL_GamepadConnected(p)) { any_connected = true; break; }
        }
      }
      if (!any_connected && SDL_HasJoystick()) {
        std::lock_guard<std::mutex> slk(scan_mu_);
        if (failed_.empty()) scan_pads();
      }
      SDL_UpdateGamepads();
    } else if (!last_scan_ns_ || now_ns - last_scan_ns_ > 1000000000ull) {
      last_scan_ns_ = now_ns;
      std::lock_guard<std::mutex> slk(scan_mu_);
      scan_pads();
      SDL_UpdateGamepads();
    } else {
      SDL_UpdateGamepads();
    }

    std::lock_guard<std::mutex> lk(pads_mu_);
    for (SDL_Gamepad* pad : pads_) {
      if (!SDL_GamepadConnected(pad)) continue;
      RawInput r;
      r.pad_connected = true;
      r.pad_family = family_of(SDL_GetGamepadType(pad));
      auto b = [&](SDL_GamepadButton sb, int bit) {
        if (SDL_GetGamepadButton(pad, sb)) r.pad.set(static_cast<size_t>(bit));
      };
      b(SDL_GAMEPAD_BUTTON_SOUTH, kPadSouth);
      b(SDL_GAMEPAD_BUTTON_EAST, kPadEast);
      b(SDL_GAMEPAD_BUTTON_WEST, kPadWest);
      b(SDL_GAMEPAD_BUTTON_NORTH, kPadNorth);
      b(SDL_GAMEPAD_BUTTON_BACK, kPadBack);
      b(SDL_GAMEPAD_BUTTON_GUIDE, kPadGuide);
      b(SDL_GAMEPAD_BUTTON_START, kPadStart);
      b(SDL_GAMEPAD_BUTTON_LEFT_STICK, kPadLStick);
      b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, kPadRStick);
      b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLShoulder);
      b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRShoulder);
      b(SDL_GAMEPAD_BUTTON_DPAD_UP, kPadDUp);
      b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDDown);
      b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadDLeft);
      b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadDRight);
      auto ax = [&](SDL_GamepadAxis a) {
        return std::clamp(static_cast<float>(SDL_GetGamepadAxis(pad, a)) / 32767.0f, -1.0f, 1.0f);
      };
      r.lx = ax(SDL_GAMEPAD_AXIS_LEFTX);
      r.ly = -ax(SDL_GAMEPAD_AXIS_LEFTY);
      r.rx = ax(SDL_GAMEPAD_AXIS_RIGHTX);
      r.ry = -ax(SDL_GAMEPAD_AXIS_RIGHTY);
      r.lt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
      r.rt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

      const bool avg = pad_fx().gyro_on.load() && pad_fx().gyro_avg.load();
      if (avg && !gyro_thread_.joinable() && !stop_.load()) {
        gyro_thread_ = std::thread([this] { gyro_loop(); });
      }
      if (avg && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
        if (!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) {
          SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        }
        std::lock_guard<std::mutex> ga(gavg_mu_);
        gavg_.take(&r.gyro_x, &r.gyro_y, &r.gyro_z);
        r.gyro_present = true;
      } else if (pad_fx().gyro_on.load() && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
        if (!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) {
          SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        }
        float g[3] = {0.0f, 0.0f, 0.0f};
        if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3)) {
          constexpr float kDeg = 57.29578f;
          r.gyro_present = true;
          r.gyro_x = g[0] * kDeg;
          r.gyro_y = g[1] * kDeg;
          r.gyro_z = g[2] * kDeg;
        }
      }
      merge_pad(out, r);
    }

    // Keyboard and mouse only when an SDL window has keyboard focus.
    out.focused = SDL_GetKeyboardFocus() != nullptr;
    if (out.focused) {
      int n = 0;
      const bool* ks = SDL_GetKeyboardState(&n);
      auto key = [&](int id, SDL_Scancode sc) {
        if (ks && sc < n && ks[sc]) out.keys.set(static_cast<size_t>(id));
      };
      for (int k = 1; k <= 26; ++k) key(k, static_cast<SDL_Scancode>(SDL_SCANCODE_A + (k - 1)));
      key(27, SDL_SCANCODE_0);
      for (int k = 28; k <= 36; ++k) key(k, static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (k - 28)));
      for (int k = 37; k <= 48; ++k) key(k, static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (k - 37)));
      static const SDL_Scancode fixed[] = {
          SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
          SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_TAB, SDL_SCANCODE_BACKSPACE,
          SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, SDL_SCANCODE_LALT,
          SDL_SCANCODE_RALT, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_SEMICOLON,
          SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_MINUS,
          SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_GRAVE, SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE,
          SDL_SCANCODE_HOME, SDL_SCANCODE_END, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN};
      constexpr int nf = static_cast<int>(sizeof fixed / sizeof fixed[0]);
      for (int i = 0; i < nf; ++i) key(49 + i, fixed[i]);
      key(49 + nf, SDL_SCANCODE_KP_0);
      for (int i = 1; i <= 9; ++i) key(49 + nf + i, static_cast<SDL_Scancode>(SDL_SCANCODE_KP_1 + (i - 1)));

      float mx = 0.0f, my = 0.0f;
      const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
      if (mb & SDL_BUTTON_LMASK) out.mouse_buttons |= 1;
      if (mb & SDL_BUTTON_RMASK) out.mouse_buttons |= 2;
      if (mb & SDL_BUTTON_MMASK) out.mouse_buttons |= 4;
      if (mb & SDL_BUTTON_X1MASK) out.mouse_buttons |= 8;
      if (mb & SDL_BUTTON_X2MASK) out.mouse_buttons |= 16;
      float pnx = 0.0f, pny = 0.0f;
      bool pin = false;
      if (read_posted_mouse_pos(&pnx, &pny, &pin)) {
        out.mouse_in_window = pin;
        out.mouse_x = pnx;
        out.mouse_y = pny;
      } else {
        SDL_Window* w = SDL_GetMouseFocus();
        if (!w) w = SDL_GetKeyboardFocus();
        int ww = 0, wh = 0;
        if (w && SDL_GetWindowSize(w, &ww, &wh) && ww > 0 && wh > 0) {
          out.mouse_in_window = (mx >= 0.0f && my >= 0.0f && mx < static_cast<float>(ww) && my < static_cast<float>(wh));
          out.mouse_x = mx / static_cast<float>(ww);
          out.mouse_y = my / static_cast<float>(wh);
        }
      }
      const bool recenter = recenter_.load(std::memory_order_relaxed);
      float dx = 0.0f, dy = 0.0f;
      uint64_t ev_cnt = 0;
      if (raw_mouse().events.load(std::memory_order_relaxed) > 0) {
        raw_mouse().take(&dx, &dy, &ev_cnt);
        SDL_GetRelativeMouseState(nullptr, nullptr);
      } else {
        SDL_GetRelativeMouseState(&dx, &dy);
      }
      // Discard deltas on focus regain or recenter transition to avoid a camera jump.
      if (!was_focused_ || (recenter && !was_recenter_)) {
        dx = 0.0f;
        dy = 0.0f;
        ev_cnt = 0;
      }
      out.mouse_dx = dx;
      out.mouse_dy = dy;
      out.mouse_poll_events = static_cast<uint32_t>(ev_cnt);
      was_focused_ = true;
      was_recenter_ = recenter;
    } else {
      if (was_focused_) {
        float tmp_x = 0.0f, tmp_y = 0.0f;
        raw_mouse().take(&tmp_x, &tmp_y);
        SDL_GetRelativeMouseState(nullptr, nullptr);
      }
      was_focused_ = false;
      was_recenter_ = false;
    }
  }

  void rumble(float low, float high) override {
    if (!inited_) return;
    std::lock_guard<std::mutex> lk(pads_mu_);
    if (pads_.empty()) return;
    const Uint16 lo = static_cast<Uint16>(std::clamp(low, 0.0f, 1.0f) * 65535.0f);
    const Uint16 hi = static_cast<Uint16>(std::clamp(high, 0.0f, 1.0f) * 65535.0f);
    for (SDL_Gamepad* pad : pads_) {
      if (!SDL_GamepadConnected(pad)) continue;
      if (!SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) continue;
      SDL_RumbleGamepad(pad, lo, hi, (lo || hi) ? 500 : 0);
    }
  }

 private:
  void scan_pads() {
    // scan_pads() has one caller at a time (either scan_thread_ or poll() when SNAP_SDL_SCAN_THREAD=0).
    std::vector<SDL_Gamepad*> disconnected;
    {
      std::lock_guard<std::mutex> lk(pads_mu_);
      for (size_t i = 0; i < pads_.size();) {
        if (!SDL_GamepadConnected(pads_[i])) {
          disconnected.push_back(pads_[i]);
          pads_.erase(pads_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
          ++i;
        }
      }
    }
    for (SDL_Gamepad* gone : disconnected) SDL_CloseGamepad(gone);

    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    const uint64_t now_ns = static_cast<uint64_t>(SDL_GetTicksNS());
    for (size_t i = 0; i < failed_.size();) {
      bool still_present = false;
      for (int k = 0; k < n; ++k) {
        if (ids[k] == failed_[i].first) { still_present = true; break; }
      }
      if (!still_present) failed_.erase(failed_.begin() + static_cast<std::ptrdiff_t>(i));
      else ++i;
    }

    for (int k = 0; k < n; ++k) {
      const SDL_JoystickID id = ids[k];
      if (!SDL_IsGamepad(id)) continue;
      bool already_open = false;
      {
        std::lock_guard<std::mutex> lk(pads_mu_);
        for (SDL_Gamepad* g : pads_) {
          if (SDL_GetGamepadID(g) == id) {
            already_open = true;
            break;
          }
        }
      }
      if (already_open) continue;
      auto fit = std::find_if(failed_.begin(), failed_.end(), [id](const auto& p) { return p.first == id; });
      if (fit != failed_.end() && now_ns - fit->second < 3000000000ull) continue;
      if (SDL_Gamepad* g = SDL_OpenGamepad(id)) {
        if (fit != failed_.end()) failed_.erase(fit);
        std::lock_guard<std::mutex> lk(pads_mu_);
        pads_.push_back(g);
      } else {
        if (fit != failed_.end()) fit->second = now_ns;
        else failed_.emplace_back(id, now_ns);
      }
    }
    SDL_free(ids);
  }

  void gyro_loop() {
    while (!stop_.load()) {
      if (pad_fx().gyro_on.load() && pad_fx().gyro_avg.load()) {
        std::lock_guard<std::mutex> pl(pads_mu_);
        for (SDL_Gamepad* pad : pads_) {
          if (!SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) continue;
          if (!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
          float g[3] = {0.0f, 0.0f, 0.0f};
          if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3)) {
            constexpr float kDeg = 57.29578f;
            std::lock_guard<std::mutex> ga(gavg_mu_);
            gavg_.add(g[0] * kDeg, g[1] * kDeg, g[2] * kDeg);
          }
          break;
        }
      }
      SDL_Delay(1);
    }
  }

  bool inited_ = false;
  uint64_t last_scan_ns_ = 0;
  std::vector<SDL_Gamepad*> pads_;
  std::vector<std::pair<SDL_JoystickID, uint64_t>> failed_;
  std::mutex pads_mu_;
  std::mutex scan_mu_;
  std::mutex wake_mu_;
  std::condition_variable wake_cv_;
  std::thread scan_thread_;
  std::mutex gavg_mu_;
  GyroAvg gavg_;
  std::thread gyro_thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> lock_{false};
  std::atomic<bool> recenter_{false};
  bool was_focused_ = false;
  bool was_recenter_ = false;
  bool scan_threaded_ = [] {
    const char* e = std::getenv("SNAP_SDL_SCAN_THREAD");
    return !(e && *e == '0');
  }();
};

}  // namespace
#endif

std::unique_ptr<Backend> make_default_backend() {
#ifdef HLE_WITH_SDL3
  return std::make_unique<Sdl3Backend>();
#else
  struct NullBackend final : Backend {
    const char* name() const override { return "none"; }
    void poll(RawInput& out) override { out.focused = false; }
  };
  return std::make_unique<NullBackend>();
#endif
}

}  // namespace hle::input
