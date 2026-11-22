// SPDX-License-Identifier: MIT
// Unit tests for Linux window/display options (host/linux/window_opts_linux.cpp).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "host/linux/window_opts_linux.h"
#include "host/linux/window_sdl3.h"

struct Cpu;
extern "C" void rt_dispatch(Cpu*) {}

namespace {

int g_fails = 0;
#define CHECK(cond)                                                                              \
  do {                                                                                           \
    if (!(cond)) {                                                                               \
      std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++g_fails;                                                                                 \
    }                                                                                            \
  } while (0)

struct MockWin {
  bool fullscreen = false;
  int monitor = -1;
  int w = 1280, h = 720;
  int fs_calls = 0;
  int mon_calls = 0;
  int size_calls = 0;
  std::vector<std::string> monitors = {"DP-1 5120x1440", "HDMI-A-1 1920x1080"};
};

void set_env(const char* k, const char* v) {
  if (v) setenv(k, v, 1);
  else unsetenv(k);
}

snap_linux::WindowOptsCallbacks make_cb(MockWin* m) {
  snap_linux::WindowOptsCallbacks cb{};
  cb.ctx = m;
  cb.set_fullscreen = [](void* c, bool fs) {
    auto* mw = static_cast<MockWin*>(c);
    mw->fullscreen = fs;
    ++mw->fs_calls;
    return true;
  };
  cb.set_monitor = [](void* c, int idx) {
    auto* mw = static_cast<MockWin*>(c);
    mw->monitor = idx;
    ++mw->mon_calls;
    return true;
  };
  cb.set_size = [](void* c, int w, int h) {
    auto* mw = static_cast<MockWin*>(c);
    mw->w = w;
    mw->h = h;
    ++mw->size_calls;
    return true;
  };
  cb.monitor_count = [](void* c) {
    return static_cast<int>(static_cast<MockWin*>(c)->monitors.size());
  };
  cb.monitor_name = [](void* c, int idx) -> std::string {
    auto* mw = static_cast<MockWin*>(c);
    if (idx < 0 || idx >= static_cast<int>(mw->monitors.size())) return {};
    return mw->monitors[static_cast<size_t>(idx)];
  };
  return cb;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::temp_directory_path() / "snap_window_opts_test";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);

  set_env("SNAP_CONFIG_DIR", tmp.c_str());
  set_env("SNAP_SAVE_DIR", nullptr);
  set_env("SNAP_WINDOW_SAVE", nullptr);
  set_env("SNAP_WINDOW_MODE", nullptr);
  set_env("SNAP_FULLSCREEN", nullptr);
  set_env("SNAP_WINDOW_MONITOR", nullptr);
  set_env("SNAP_DISPLAY", nullptr);
  set_env("SNAP_WINDOW_W", nullptr);
  set_env("SNAP_WINDOW_H", nullptr);
  set_env("SNAP_WINDOW_PAUSE", nullptr);
  set_env("SNAP_WINDOW_MUTE_BG", nullptr);
  set_env("SNAP_WINDOW_FRAME_CAP", nullptr);

  MockWin mw;
  const auto cb = make_cb(&mw);
  snap_linux::window_opts_reload_for_tests(&cb);

  // Defaults + monitor enumeration
  CHECK(snap_window_get_mode() == SNAP_WINDOW_WINDOWED);
  CHECK(snap_window_get_monitor() == -1);
  CHECK(snap_window_monitor_count() == 2);
  CHECK(std::string(snap_window_monitor_name(0)) == "DP-1 5120x1440");
  CHECK(std::string(snap_window_monitor_name(1)) == "HDMI-A-1 1920x1080");
  CHECK(snap_window_monitor_name(2) == nullptr);
  CHECK(snap_window_get_pause_on_focus_loss() == 1);
  CHECK(snap_window_get_mute_in_background() == 1);
  CHECK(snap_window_frame_cap() == 0);

  // Mode toggle & monitor move
  snap_window_set_monitor(1);
  CHECK(snap_window_get_monitor() == 1);
  CHECK(mw.mon_calls == 0);   // windowed: does not move borderless yet
  snap_window_toggle_fullscreen();
  CHECK(snap_window_get_mode() == SNAP_WINDOW_BORDERLESS);
  CHECK(mw.fullscreen == true);
  CHECK(mw.mon_calls == 1);
  CHECK(mw.monitor == 1);

  snap_window_set_monitor(0);
  CHECK(mw.monitor == 0);
  CHECK(mw.mon_calls == 2);

  snap_window_set_frame_cap(120);
  CHECK(snap_window_frame_cap() == 120);
  snap_window_set_frame_cap(-5);
  CHECK(snap_window_frame_cap() == 0);

  // Persistence check across reload
  snap_window_set_frame_cap(60);
  snap_window_set_pause_on_focus_loss(0);
  snap_window_set_mute_in_background(1);
  snap_linux::window_opts_reload_for_tests(&cb);
  CHECK(snap_window_get_mode() == SNAP_WINDOW_BORDERLESS);
  CHECK(snap_window_get_monitor() == 0);
  CHECK(snap_window_frame_cap() == 60);
  CHECK(snap_window_get_pause_on_focus_loss() == 0);
  CHECK(snap_window_get_mute_in_background() == 1);

  // Focus loss/regain state
  snap_linux::window_opts_on_focus(false);
  CHECK(snap_window_focused() == 0);
  CHECK(snap_window_paused() == 0);   // pause_on_focus_loss is 0
  snap_window_set_pause_on_focus_loss(1);
  CHECK(snap_window_paused() == 1);
  snap_linux::window_opts_on_focus(true);
  CHECK(snap_window_focused() == 1);
  CHECK(snap_window_paused() == 0);

  // Env overrides & initial config
  set_env("SNAP_WINDOW_MODE", "windowed");
  set_env("SNAP_DISPLAY", "2");
  set_env("SNAP_WINDOW_W", "1600");
  set_env("SNAP_WINDOW_H", "900");
  set_env("SNAP_WINDOW_FRAME_CAP", "144");
  snap_linux::window_opts_reload_for_tests(&cb);
  snap_linux::WindowConfig wcfg{};
  snap_linux::window_opts_initial_config(&wcfg);
  CHECK(!wcfg.fullscreen);
  CHECK(wcfg.display_index == 2);
  CHECK(wcfg.width == 1600);
  CHECK(wcfg.height == 900);
  CHECK(snap_window_frame_cap() == 144);

  snap_linux::window_opts_shutdown();

  // Fast boot tests: OFF (default), ON with vsync drop/restore, and SNAP_FAST_BOOT_VSYNC=0
  {
    static double g_pace = -1.0;
    static std::string g_vsync = "on";
    static int g_pace_calls = 0;
    static int g_vsync_calls = 0;
    snap_linux::FastBootHooks fbh{};
    fbh.set_pacing = [](double hz) { g_pace = hz; ++g_pace_calls; };
    fbh.get_vsync = []() -> const char* { return g_vsync.c_str(); };
    fbh.set_vsync = [](const char* m) { g_vsync = m; ++g_vsync_calls; };

    unsetenv("SNAP_FAST_BOOT");
    unsetenv("SNAP_FAST_BOOT_FRAMES");
    unsetenv("SNAP_FAST_BOOT_VSYNC");
    CHECK(!snap_linux::fast_boot_on_present(1, 60.0, &fbh, true));
    CHECK(g_pace_calls == 0 && g_vsync_calls == 0);

    setenv("SNAP_FAST_BOOT", "1", 1);
    setenv("SNAP_FAST_BOOT_FRAMES", "5", 1);
    g_vsync = "on";
    CHECK(snap_linux::fast_boot_on_present(1, 120.0, &fbh, true));
    CHECK(g_pace == 0.0 && g_vsync == "off");
    CHECK(snap_linux::fast_boot_on_present(2, 120.0, &fbh, false));
    CHECK(!snap_linux::fast_boot_on_present(5, 120.0, &fbh, false));
    CHECK(g_pace == 120.0 && g_vsync == "on");

    // SNAP_FAST_BOOT_VSYNC=0 keeps vsync untouched
    g_vsync = "adaptive";
    g_vsync_calls = 0;
    setenv("SNAP_FAST_BOOT_VSYNC", "0", 1);
    CHECK(snap_linux::fast_boot_on_present(1, 60.0, &fbh, true));
    CHECK(g_pace == 0.0 && g_vsync == "adaptive" && g_vsync_calls == 0);
    CHECK(!snap_linux::fast_boot_on_present(5, 60.0, &fbh, false));
    CHECK(g_pace == 60.0 && g_vsync == "adaptive" && g_vsync_calls == 0);
    unsetenv("SNAP_FAST_BOOT");
    unsetenv("SNAP_FAST_BOOT_FRAMES");
    unsetenv("SNAP_FAST_BOOT_VSYNC");
  }

  fs::remove_all(tmp, ec);
  if (g_fails) return 1;
  std::printf("window_opts_linux_test: all checks passed\n");
  return 0;
}
