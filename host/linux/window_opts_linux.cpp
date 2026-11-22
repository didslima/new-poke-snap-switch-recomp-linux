// SPDX-License-Identifier: MIT
// Linux window options: borderless fullscreen (F11 / Alt+Enter), monitor choice,
// window.json persistence, pause on focus loss, mute in background, and frame cap.
#if !defined(__linux__)
#error "host/linux/window_opts_linux.cpp is Linux-only"
#endif

#include "window_opts_linux.h"
#include "window_sdl3.h"

#include "audio/audio_out.h"
#include "hle/json_lite.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" void os_time_pause(int on);   // runtime/os/os_kernel.cpp

namespace snap_linux {
namespace {

struct Settings {
  int mode = SNAP_WINDOW_WINDOWED;
  int monitor = -1;
  int width = 1280;
  int height = 720;
  bool pause_on_focus_loss = true;
  bool mute_in_background = true;
  int frame_cap = 0;
};

Settings g_s;
WindowOptsCallbacks g_cb{};
std::mutex g_mu;
std::once_flag g_init_once;
std::atomic<int> g_cap{0};
std::atomic<bool> g_focused{true};
std::atomic<bool> g_paused{false};
bool g_muted = false;
bool g_save_enabled = true;
std::vector<std::string> g_mon_names;

std::string cfg_dir() {
  if (const char* d = std::getenv("SNAP_CONFIG_DIR"); d && *d) return d;
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/SnapRecomp";
  if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.config/SnapRecomp";
  return ".";
}

std::string cfg_path() { return cfg_dir() + "/window.json"; }

bool env_int(const char* name, int* out) {
  const char* e = std::getenv(name);
  if (!e || !*e) return false;
  *out = std::atoi(e);
  return true;
}

void load_settings_locked() {
  g_s = Settings{};
  FILE* f = std::fopen(cfg_path().c_str(), "rb");
  if (f) {
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    hle::Json j;
    std::string err;
    if (hle::json_parse(text, j, err) && j.type == hle::Json::Obj) {
      if (auto* v = j.get("mode")) g_s.mode = v->str() == "borderless" ? SNAP_WINDOW_BORDERLESS : SNAP_WINDOW_WINDOWED;
      if (auto* v = j.get("monitor")) g_s.monitor = static_cast<int>(v->num(-1));
      if (auto* v = j.get("width")) g_s.width = static_cast<int>(v->num(g_s.width));
      if (auto* v = j.get("height")) g_s.height = static_cast<int>(v->num(g_s.height));
      if (auto* v = j.get("pause_on_focus_loss")) g_s.pause_on_focus_loss = v->boolean(true);
      if (auto* v = j.get("mute_in_background")) g_s.mute_in_background = v->boolean(true);
      if (auto* v = j.get("frame_cap")) g_s.frame_cap = static_cast<int>(v->num(0));
    } else {
      std::fprintf(stderr, "[window] %s is invalid (%s): defaults used\n", cfg_path().c_str(), err.c_str());
    }
  }
  int v = 0;
  if (const char* e = std::getenv("SNAP_WINDOW_MODE"); e && *e) {
    g_s.mode = (std::strcmp(e, "borderless") == 0 || std::strcmp(e, "1") == 0) ? SNAP_WINDOW_BORDERLESS : SNAP_WINDOW_WINDOWED;
  }
  if (const char* e = std::getenv("SNAP_FULLSCREEN"); e && *e && std::strcmp(e, "0") != 0) {
    g_s.mode = SNAP_WINDOW_BORDERLESS;
  }
  if (env_int("SNAP_WINDOW_MONITOR", &v)) g_s.monitor = v;
  if (env_int("SNAP_DISPLAY", &v) && v > 0) g_s.monitor = v - 1;
  if (env_int("SNAP_WINDOW_W", &v)) g_s.width = v;
  if (env_int("SNAP_WINDOW_H", &v)) g_s.height = v;
  if (env_int("SNAP_WINDOW_PAUSE", &v)) g_s.pause_on_focus_loss = (v != 0);
  if (env_int("SNAP_WINDOW_MUTE_BG", &v)) g_s.mute_in_background = (v != 0);
  if (env_int("SNAP_WINDOW_FRAME_CAP", &v)) g_s.frame_cap = v;
  if (g_s.width < 320) g_s.width = 320;
  if (g_s.height < 180) g_s.height = 180;
  if (g_s.frame_cap < 0) g_s.frame_cap = 0;
  g_cap.store(g_s.frame_cap, std::memory_order_relaxed);
  const char* sd = std::getenv("SNAP_SAVE_DIR");
  const char* ws = std::getenv("SNAP_WINDOW_SAVE");
  g_save_enabled = !(sd && *sd) || (ws && *ws && *ws != '0');
}

void save_settings_locked() {
  if (!g_save_enabled) return;
  hle::Json j = hle::Json::object();
  j.set("mode", hle::Json::string(g_s.mode == SNAP_WINDOW_BORDERLESS ? "borderless" : "windowed"));
  j.set("monitor", hle::Json::number(g_s.monitor));
  j.set("width", hle::Json::number(g_s.width));
  j.set("height", hle::Json::number(g_s.height));
  j.set("pause_on_focus_loss", hle::Json::boolean_(g_s.pause_on_focus_loss));
  j.set("mute_in_background", hle::Json::boolean_(g_s.mute_in_background));
  j.set("frame_cap", hle::Json::number(g_s.frame_cap));

  std::error_code ec;
  std::filesystem::create_directories(cfg_dir(), ec);
  const std::string p = cfg_path();
  const std::string tmp = p + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return;
  std::string out;
  hle::json_dump(j, out);
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  if (ok) std::filesystem::rename(tmp, p, ec);
  if (!ok || ec) std::filesystem::remove(tmp, ec);
}

void ensure_loaded() {
  std::call_once(g_init_once, [] {
    std::lock_guard<std::mutex> lk(g_mu);
    load_settings_locked();
  });
}

void refresh_monitors_locked() {
  g_mon_names.clear();
  if (!g_cb.monitor_count || !g_cb.monitor_name) return;
  const int n = std::max(0, g_cb.monitor_count(g_cb.ctx));
  g_mon_names.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) g_mon_names.push_back(g_cb.monitor_name(g_cb.ctx, i));
}

void apply_focus_side_effects_locked(bool active) {
  const bool want_pause = !active && g_s.pause_on_focus_loss;
  const bool want_mute = !active && g_s.mute_in_background;
  if (want_pause != g_paused.load(std::memory_order_relaxed)) {
    g_paused.store(want_pause, std::memory_order_release);
    os_time_pause(want_pause ? 1 : 0);
  }
  if (want_mute != g_muted) {
    snap_audio_set_muted(want_mute ? 1 : 0);
    g_muted = want_mute;
  }
}

}  // namespace

void window_opts_init(const WindowOptsCallbacks* cb) {
  ensure_loaded();
  std::lock_guard<std::mutex> lk(g_mu);
  g_cb = cb ? *cb : WindowOptsCallbacks{};
  refresh_monitors_locked();
  g_focused.store(true, std::memory_order_release);
  apply_focus_side_effects_locked(true);
}

void window_opts_reload_for_tests(const WindowOptsCallbacks* cb) {
  std::lock_guard<std::mutex> lk(g_mu);
  load_settings_locked();
  g_cb = cb ? *cb : WindowOptsCallbacks{};
  refresh_monitors_locked();
  g_focused.store(true, std::memory_order_release);
  g_paused.store(false, std::memory_order_release);
  if (g_muted) {
    snap_audio_set_muted(0);
    g_muted = false;
  }
}

void window_opts_shutdown() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_paused.exchange(false, std::memory_order_acq_rel)) os_time_pause(0);
  if (g_muted) {
    snap_audio_set_muted(0);
    g_muted = false;
  }
  g_cb = WindowOptsCallbacks{};
}

void window_opts_initial_config(WindowConfig* cfg) {
  if (!cfg) return;
  ensure_loaded();
  std::lock_guard<std::mutex> lk(g_mu);
  cfg->fullscreen = (g_s.mode == SNAP_WINDOW_BORDERLESS);
  cfg->width = g_s.width;
  cfg->height = g_s.height;
  cfg->display_index = (g_s.monitor >= 0) ? (g_s.monitor + 1) : 0;
}

void window_opts_on_focus(bool active) {
  ensure_loaded();
  g_focused.store(active, std::memory_order_release);
  std::lock_guard<std::mutex> lk(g_mu);
  apply_focus_side_effects_locked(active);
}

void window_opts_wait_if_paused(bool (*pump_fn)(void*), void* ctx) {
  while (g_paused.load(std::memory_order_acquire)) {
    if (pump_fn && !pump_fn(ctx)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

}  // namespace snap_linux

extern "C" {

int snap_window_get_mode(void) {
  snap_linux::ensure_loaded();
  std::lock_guard<std::mutex> lk(snap_linux::g_mu);
  return snap_linux::g_s.mode;
}

void snap_window_set_mode(int mode) {
  snap_linux::ensure_loaded();
  const int nm = (mode == SNAP_WINDOW_BORDERLESS) ? SNAP_WINDOW_BORDERLESS : SNAP_WINDOW_WINDOWED;
  snap_linux::WindowOptsCallbacks cb{};
  int mon = -1, w = 1280, h = 720;
  {
    std::lock_guard<std::mutex> lk(snap_linux::g_mu);
    if (snap_linux::g_s.mode == nm) return;
    snap_linux::g_s.mode = nm;
    mon = snap_linux::g_s.monitor;
    w = snap_linux::g_s.width;
    h = snap_linux::g_s.height;
    cb = snap_linux::g_cb;
    snap_linux::save_settings_locked();
  }
  if (cb.set_fullscreen) cb.set_fullscreen(cb.ctx, nm == SNAP_WINDOW_BORDERLESS);
  if (nm == SNAP_WINDOW_BORDERLESS && mon >= 0 && cb.set_monitor) cb.set_monitor(cb.ctx, mon);
  if (nm == SNAP_WINDOW_WINDOWED && cb.set_size) cb.set_size(cb.ctx, w, h);
}

void snap_window_toggle_fullscreen(void) {
  snap_window_set_mode(snap_window_get_mode() == SNAP_WINDOW_BORDERLESS ? SNAP_WINDOW_WINDOWED : SNAP_WINDOW_BORDERLESS);
}

int snap_window_monitor_count(void) {
  snap_linux::ensure_loaded();
  std::lock_guard<std::mutex> lk(snap_linux::g_mu);
  snap_linux::refresh_monitors_locked();
  return static_cast<int>(snap_linux::g_mon_names.size());
}

const char* snap_window_monitor_name(int index) {
  snap_linux::ensure_loaded();
  std::lock_guard<std::mutex> lk(snap_linux::g_mu);
  if (snap_linux::g_mon_names.empty()) snap_linux::refresh_monitors_locked();
  if (index < 0 || index >= static_cast<int>(snap_linux::g_mon_names.size())) return nullptr;
  return snap_linux::g_mon_names[static_cast<size_t>(index)].c_str();
}

int snap_window_get_monitor(void) {
  snap_linux::ensure_loaded();
  std::lock_guard<std::mutex> lk(snap_linux::g_mu);
  return snap_linux::g_s.monitor;
}

void snap_window_set_monitor(int index) {
  snap_linux::ensure_loaded();
  snap_linux::WindowOptsCallbacks cb{};
  int mode = SNAP_WINDOW_WINDOWED;
  int clamped = -1;
  {
    std::lock_guard<std::mutex> lk(snap_linux::g_mu);
    snap_linux::refresh_monitors_locked();
    const int n = static_cast<int>(snap_linux::g_mon_names.size());
    clamped = (index >= 0 && (n == 0 || index < n)) ? index : -1;
    snap_linux::g_s.monitor = clamped;
    mode = snap_linux::g_s.mode;
    cb = snap_linux::g_cb;
    snap_linux::save_settings_locked();
  }
  if (mode == SNAP_WINDOW_BORDERLESS && clamped >= 0 && cb.set_monitor) {
    cb.set_monitor(cb.ctx, clamped);
  }
}

int snap_window_get_pause_on_focus_loss(void) {
  snap_linux::ensure_loaded();
  std::lock_guard<std::mutex> lk(snap_linux::g_mu);
  return snap_linux::g_s.pause_on_focus_loss ? 1 : 0;
}

void snap_window_set_pause_on_focus_loss(int on) {
  snap_linux::ensure_loaded();
  std::lock_guard<std::mutex> lk(snap_linux::g_mu);
  snap_linux::g_s.pause_on_focus_loss = (on != 0);
  snap_linux::apply_focus_side_effects_locked(snap_linux::g_focused.load(std::memory_order_acquire));
  snap_linux::save_settings_locked();
}

int snap_window_get_mute_in_background(void) {
  snap_linux::ensure_loaded();
  std::lock_guard<std::mutex> lk(snap_linux::g_mu);
  return snap_linux::g_s.mute_in_background ? 1 : 0;
}

void snap_window_set_mute_in_background(int on) {
  snap_linux::ensure_loaded();
  std::lock_guard<std::mutex> lk(snap_linux::g_mu);
  snap_linux::g_s.mute_in_background = (on != 0);
  snap_linux::apply_focus_side_effects_locked(snap_linux::g_focused.load(std::memory_order_acquire));
  snap_linux::save_settings_locked();
}

int snap_window_frame_cap(void) {
  snap_linux::ensure_loaded();
  return snap_linux::g_cap.load(std::memory_order_relaxed);
}

void snap_window_set_frame_cap(int fps) {
  snap_linux::ensure_loaded();
  const int v = fps > 0 ? fps : 0;
  std::lock_guard<std::mutex> lk(snap_linux::g_mu);
  snap_linux::g_s.frame_cap = v;
  snap_linux::g_cap.store(v, std::memory_order_relaxed);
  snap_linux::save_settings_locked();
}

int snap_window_focused(void) { return snap_linux::g_focused.load(std::memory_order_acquire) ? 1 : 0; }
int snap_window_paused(void) { return snap_linux::g_paused.load(std::memory_order_acquire) ? 1 : 0; }

}  // extern "C"

extern "C" {
__attribute__((weak)) void nvnHostSetFramePacing(double) {}
__attribute__((weak)) const char* snap_vsync_get() { return "on"; }
__attribute__((weak)) void snap_vsync_set(const char*) {}
}

namespace snap_linux {

bool fast_boot_on_present(uint64_t present_num, double restore_hz, const FastBootHooks* hooks, bool reset) {
  static int s_frames = -1;
  static bool s_drop_vsync = true;
  static std::string s_saved_vsync;
  static bool s_active = false;
  if (reset) {
    s_frames = -1;
    s_drop_vsync = true;
    s_saved_vsync.clear();
    s_active = false;
  }
  if (s_frames < 0) {
    const char* e = std::getenv("SNAP_FAST_BOOT");
    if (e && *e && std::strcmp(e, "0") != 0) {
      const char* f = std::getenv("SNAP_FAST_BOOT_FRAMES");
      const int v = f ? std::atoi(f) : 3200;
      s_frames = v > 0 ? v : 3200;
    } else {
      s_frames = 0;
    }
    const char* dv = std::getenv("SNAP_FAST_BOOT_VSYNC");
    s_drop_vsync = !(dv && *dv == '0');
  }
  if (s_frames <= 0) return false;

  auto set_pacing = (hooks && hooks->set_pacing) ? hooks->set_pacing : nvnHostSetFramePacing;
  auto get_vsync = (hooks && hooks->get_vsync) ? hooks->get_vsync : snap_vsync_get;
  auto set_vsync = (hooks && hooks->set_vsync) ? hooks->set_vsync : snap_vsync_set;

  if (present_num == 1) {
    set_pacing(0.0);
    s_active = true;
    s_saved_vsync.clear();
    if (s_drop_vsync && get_vsync && set_vsync) {
      const char* cur = get_vsync();
      if (cur && (std::strcmp(cur, "on") == 0 || std::strcmp(cur, "adaptive") == 0)) {
        s_saved_vsync = cur;
        set_vsync("off");
      }
    }
    std::fprintf(stderr, "[window] fast boot: frame limiter off for %d presents%s\n", s_frames,
                 s_saved_vsync.empty() ? "" : ", vsync off");
    return true;
  }
  if (present_num == static_cast<uint64_t>(s_frames)) {
    set_pacing(restore_hz > 0.0 ? restore_hz : 60.0);
    if (!s_saved_vsync.empty() && set_vsync) {
      set_vsync(s_saved_vsync.c_str());
    }
    std::fprintf(stderr, "[window] fast boot: frame limiter back to %.0f Hz%s at present %llu\n",
                 restore_hz > 0.0 ? restore_hz : 60.0, s_saved_vsync.empty() ? "" : " and vsync restored",
                 static_cast<unsigned long long>(present_num));
    s_saved_vsync.clear();
    s_active = false;
    return false;
  }
  return s_active && present_num < static_cast<uint64_t>(s_frames);
}

}  // namespace snap_linux
