// SPDX-License-Identifier: MIT
// Linux window options implementation backing host/window_opts_public.h.
#pragma once

#include <string>

#include "window_opts_public.h"

namespace snap_linux {

struct WindowConfig;

struct WindowOptsCallbacks {
  void* ctx = nullptr;
  bool (*set_fullscreen)(void* ctx, bool fullscreen) = nullptr;
  bool (*set_monitor)(void* ctx, int index) = nullptr;
  bool (*set_size)(void* ctx, int w, int h) = nullptr;
  int (*monitor_count)(void* ctx) = nullptr;
  std::string (*monitor_name)(void* ctx, int index) = nullptr;
};

void window_opts_init(const WindowOptsCallbacks* cb = nullptr);
void window_opts_reload_for_tests(const WindowOptsCallbacks* cb = nullptr);
void window_opts_shutdown();
void window_opts_initial_config(WindowConfig* cfg);
void window_opts_on_focus(bool active);
void window_opts_wait_if_paused(bool (*pump_fn)(void*), void* ctx);

struct FastBootHooks {
  void (*set_pacing)(double hz) = nullptr;
  const char* (*get_vsync)() = nullptr;
  void (*set_vsync)(const char* mode) = nullptr;
};
// Returns true while the fast-boot uncapped window is active (present 1 .. frames-1).
bool fast_boot_on_present(uint64_t present_num, double restore_hz = 60.0, const FastBootHooks* hooks = nullptr, bool reset = false);

}  // namespace snap_linux
