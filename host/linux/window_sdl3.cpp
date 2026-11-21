// SPDX-License-Identifier: MIT
// Stage 6E-B: Linux SDL3 host window and non-blocking event loop for the NVN->Vulkan renderer.
#if !defined(__linux__)
#error "host/linux/window_sdl3.cpp is Linux-only"
#endif

#include "window_sdl3.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

extern "C" __attribute__((weak)) void snap_ui_on_sdl_event(const void*) {}
namespace snap_hdr {
void NotifyDisplayChanged();
void UpdateDisplayPeak(float peak_nits);
}

#ifdef HLE_WITH_SDL3
namespace {
float query_sdl_hdr_peak(SDL_Window* win) {
  if (!win || SDL_WasInit(SDL_INIT_VIDEO) == 0) return 0.0f;
  const SDL_PropertiesID props = SDL_GetWindowProperties(win);
  if (props == 0) return 0.0f;
  // X11 does not expose HDR properties via SDL3; HDR_enabled stays false there.
  if (!SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false)) return 0.0f;
  const float sdr_white_level = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
  const float hdr_headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
  if (sdr_white_level <= 0.0f || hdr_headroom <= 1.0f) return 0.0f;
  // In SDL_COLORSPACE_SRGB_LINEAR, 1.0 corresponds to 80 nits.
  return sdr_white_level * 80.0f * hdr_headroom;
}
}  // namespace
#endif

namespace snap_linux {

HostWindow::~HostWindow() {
  destroy();
}

bool HostWindow::create(const WindowConfig& cfg) {
  destroy();
#ifdef HLE_WITH_SDL3
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "[window_sdl3] SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
    return false;
  }
  sdl_video_owned_ = true;

  if (!SDL_Vulkan_LoadLibrary(nullptr)) {
    std::fprintf(stderr, "[window_sdl3] SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
    destroy();
    return false;
  }

  int w = cfg.width > 0 ? cfg.width : 1280;
  int h = cfg.height > 0 ? cfg.height : 720;
  if (const char* ew = std::getenv("SNAP_WINDOW_W"); ew && *ew) {
    const int v = std::atoi(ew);
    if (v >= 64) w = v;
  }
  if (const char* eh = std::getenv("SNAP_WINDOW_H"); eh && *eh) {
    const int v = std::atoi(eh);
    if (v >= 64) h = v;
  }
  bool fs = cfg.fullscreen;
  if (const char* ef = std::getenv("SNAP_FULLSCREEN"); ef && *ef) {
    fs = (*ef == '1');
  }
  int disp_idx = cfg.display_index;
  if (const char* ed = std::getenv("SNAP_DISPLAY"); ed && *ed) {
    const int v = std::atoi(ed);
    if (v > 0) disp_idx = v;
  }

  SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
  if (cfg.resizable) flags |= SDL_WINDOW_RESIZABLE;
  if (fs) flags |= SDL_WINDOW_FULLSCREEN;
  if (cfg.hidden) flags |= SDL_WINDOW_HIDDEN;
  if (cfg.high_pixel_density) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

  if (disp_idx > 0) {
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    SDL_Rect bounds{};
    bool have_bounds = false;
    if (displays && disp_idx <= count) {
      have_bounds = SDL_GetDisplayBounds(displays[disp_idx - 1], &bounds);
    }
    if (displays) SDL_free(displays);

    if (have_bounds) {
      SDL_PropertiesID props = SDL_CreateProperties();
      SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, cfg.title.c_str());
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, bounds.x + std::max(0, (bounds.w - w) / 2));
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, bounds.y + std::max(0, (bounds.h - h) / 2));
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
      window_ = SDL_CreateWindowWithProperties(props);
      SDL_DestroyProperties(props);
    }
  }

  if (!window_) {
    window_ = SDL_CreateWindow(cfg.title.c_str(), w, h, flags);
  }
  if (!window_) {
    std::fprintf(stderr, "[window_sdl3] SDL_CreateWindow failed: %s\n", SDL_GetError());
    destroy();
    return false;
  }

  if (!cfg.hidden) {
    SDL_SyncWindow(window_);
  }
  window_id_ = SDL_GetWindowID(window_);
  fullscreen_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
  focused_ = (SDL_GetKeyboardFocus() == window_);
  minimized_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
  quit_requested_ = false;
  sync_sizes();
  snap_hdr::UpdateDisplayPeak(query_sdl_hdr_peak(window_));
  return true;
#else
  (void)cfg;
  std::fprintf(stderr, "[window_sdl3] built without SDL3 (HLE_WITH_SDL3 not defined)\n");
  return false;
#endif
}

void HostWindow::destroy() {
#ifdef HLE_WITH_SDL3
  snap_hdr::UpdateDisplayPeak(0.0f);
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  if (sdl_video_owned_) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    sdl_video_owned_ = false;
  }
#endif
  window_id_ = 0;
  fullscreen_ = false;
  quit_requested_ = false;
  focused_ = false;
  minimized_ = false;
  width_ = 0;
  height_ = 0;
  pixel_width_ = 0;
  pixel_height_ = 0;
}

bool HostWindow::set_fullscreen(bool fullscreen) {
#ifdef HLE_WITH_SDL3
  if (!window_) return false;
  if (!SDL_SetWindowFullscreen(window_, fullscreen)) return false;
  SDL_SyncWindow(window_);
  fullscreen_ = fullscreen;
  sync_sizes();
  return true;
#else
  (void)fullscreen;
  return false;
#endif
}

int HostWindow::display_count() {
#ifdef HLE_WITH_SDL3
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  if (displays) SDL_free(displays);
  return count > 0 ? count : 0;
#else
  return 0;
#endif
}

std::string HostWindow::display_name(int index) {
#ifdef HLE_WITH_SDL3
  if (index < 0) return {};
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  if (!displays || index >= count) {
    if (displays) SDL_free(displays);
    return {};
  }
  const SDL_DisplayID id = displays[index];
  SDL_free(displays);
  const char* nm = SDL_GetDisplayName(id);
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
  char buf[128];
  if (mode && mode->w > 0 && mode->h > 0) {
    std::snprintf(buf, sizeof(buf), "%s %dx%d", (nm && *nm) ? nm : "Display", mode->w, mode->h);
  } else {
    std::snprintf(buf, sizeof(buf), "%s %d", (nm && *nm) ? nm : "Display", index + 1);
  }
  return buf;
#else
  (void)index;
  return {};
#endif
}

bool HostWindow::set_display(int index) {
#ifdef HLE_WITH_SDL3
  if (!window_ || index < 0) return false;
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  if (!displays || index >= count) {
    if (displays) SDL_free(displays);
    return false;
  }
  const SDL_DisplayID id = displays[index];
  SDL_free(displays);
  SDL_Rect bounds{};
  if (!SDL_GetDisplayBounds(id, &bounds)) return false;
  const bool was_fs = fullscreen_;
  if (was_fs) SDL_SetWindowFullscreen(window_, false);
  SDL_SetWindowPosition(window_, bounds.x, bounds.y);
  if (was_fs) SDL_SetWindowFullscreen(window_, true);
  SDL_SyncWindow(window_);
  sync_sizes();
  return true;
#else
  (void)index;
  return false;
#endif
}

bool HostWindow::set_window_size(int w, int h) {
#ifdef HLE_WITH_SDL3
  if (!window_ || w < 64 || h < 64) return false;
  if (!SDL_SetWindowSize(window_, w, h)) return false;
  SDL_SyncWindow(window_);
  sync_sizes();
  return true;
#else
  (void)w;
  (void)h;
  return false;
#endif
}

void HostWindow::sync_sizes() {
#ifdef HLE_WITH_SDL3
  if (!window_) return;
  int w = 0, h = 0, pw = 0, ph = 0;
  if (SDL_GetWindowSize(window_, &w, &h)) {
    width_ = w;
    height_ = h;
  }
  if (SDL_GetWindowSizeInPixels(window_, &pw, &ph)) {
    pixel_width_ = pw;
    pixel_height_ = ph;
  } else {
    pixel_width_ = width_;
    pixel_height_ = height_;
  }
#endif
}

WindowEvents HostWindow::poll_events() {
  WindowEvents ev_out{};
#ifdef HLE_WITH_SDL3
  if (!window_) return ev_out;
  SDL_Event ev{};
  while (SDL_PollEvent(&ev)) {
    snap_ui_on_sdl_event(&ev);
    switch (ev.type) {
      case SDL_EVENT_QUIT:
        quit_requested_ = true;
        ev_out.quit_requested = true;
        break;
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (ev.window.windowID == window_id_) {
          quit_requested_ = true;
          ev_out.quit_requested = true;
        }
        break;
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (ev.window.windowID == window_id_) {
          sync_sizes();
          ev_out.resized = true;
        }
        break;
      case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        if (ev.window.windowID == window_id_) {
          sync_sizes();
          ev_out.display_changed = true;
          snap_hdr::UpdateDisplayPeak(query_sdl_hdr_peak(window_));
          snap_hdr::NotifyDisplayChanged();
        }
        break;
      case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
        if (ev.window.windowID == window_id_) {
          ev_out.display_changed = true;
          snap_hdr::UpdateDisplayPeak(query_sdl_hdr_peak(window_));
          snap_hdr::NotifyDisplayChanged();
        }
        break;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (ev.window.windowID == window_id_) {
          focused_ = true;
          ev_out.focus_changed = true;
        }
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (ev.window.windowID == window_id_) {
          focused_ = false;
          ev_out.focus_changed = true;
        }
        break;
      case SDL_EVENT_WINDOW_MINIMIZED:
        if (ev.window.windowID == window_id_) {
          minimized_ = true;
          ev_out.focus_changed = true;
        }
        break;
      case SDL_EVENT_WINDOW_RESTORED:
        if (ev.window.windowID == window_id_) {
          minimized_ = false;
          sync_sizes();
          ev_out.resized = true;
          ev_out.focus_changed = true;
        }
        break;
      case SDL_EVENT_KEY_DOWN:
        if (ev.key.windowID == window_id_ && !ev.key.repeat) {
          const bool alt = (ev.key.mod & SDL_KMOD_ALT) != 0;
          if (ev.key.key == SDLK_F11 || (alt && (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER))) {
            ev_out.toggle_fullscreen = true;
          }
        }
        break;
      default:
        break;
    }
  }
#endif
  ev_out.width = width_;
  ev_out.height = height_;
  ev_out.pixel_width = pixel_width_;
  ev_out.pixel_height = pixel_height_;
  ev_out.focused = focused_;
  ev_out.minimized = minimized_;
  return ev_out;
}

float HostWindow::compute_ultrawide_factor(int pixel_width, int pixel_height) {
  if (pixel_width <= 0 || pixel_height <= 0) return 1.0f;
  const double k = static_cast<double>(pixel_width) / static_cast<double>(pixel_height) / (16.0 / 9.0);
  if (k <= 1.01) return 1.0f;
  return static_cast<float>(std::min(k, 4.0));
}

}  // namespace snap_linux
