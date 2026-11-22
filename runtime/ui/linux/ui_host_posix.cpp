// SPDX-License-Identifier: MIT
// Linux SDL3 platform host implementation for runtime/ui (Stage 6F).
#if !defined(__linux__)
#error "runtime/ui/linux/ui_host_posix.cpp is Linux-only"
#endif

#include "../ui_host.h"
#include "../ui_internal.h"
#include "hle/hle.h"
#include "hle/input_core.h"
#include "imgui.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

namespace snap::ui {
std::recursive_mutex& overlay_mutex();
bool overlay_imgui_ready();
bool overlay_modal_active();
}  // namespace snap::ui

namespace snap::ui::host {
namespace {

#ifdef HLE_WITH_SDL3
SDL_Window* g_window = nullptr;
bool g_text_input_active = false;
bool g_rel_mouse_active = false;
bool g_mouse_grab_active = false;
bool g_cursor_hidden_state = false;
bool g_cursor_in_window = false;
bool g_atexit_registered = false;
double g_raw_mouse_carry_x = 0.0;
double g_raw_mouse_carry_y = 0.0;

void restore_os_cursor_at_exit() {
  if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    if (g_window) {
      SDL_SetWindowRelativeMouseMode(g_window, false);
      SDL_SetWindowMouseGrab(g_window, false);
    }
    SDL_ShowCursor();
  }
}

void update_posted_abs_pos(float px, float py) {
  if (!g_window) return;
  int ww = 0, wh = 0;
  if (SDL_GetWindowSize(g_window, &ww, &wh) && ww > 0 && wh > 0) {
    const float nx = px / static_cast<float>(ww);
    const float ny = py / static_cast<float>(wh);
    const bool in_win = (px >= 0.0f && py >= 0.0f && px < static_cast<float>(ww) && py < static_cast<float>(wh));
    hle::input::post_mouse_pos(nx, ny, in_win);
    static const bool dbg = std::getenv("SNAP_MOUSE_DEBUG") && std::atoi(std::getenv("SNAP_MOUSE_DEBUG")) != 0;
    if (dbg) {
      static int cnt = 0;
      if ((cnt++ % 30) == 0) {
        std::fprintf(stderr, "[mouse_dbg][sdl3] abs=(%.1f,%.1f) win=(%dx%d) norm=(%.3f,%.3f) in_win=%d\n",
                     px, py, ww, wh, nx, ny, (int)in_win);
      }
    }
  }
}

void flush_raw_mouse_deltas() {
  g_raw_mouse_carry_x = 0.0;
  g_raw_mouse_carry_y = 0.0;
  float tx = 0.0f, ty = 0.0f;
  hle::input::raw_mouse().take(&tx, &ty);
  SDL_GetRelativeMouseState(nullptr, nullptr);
}

ImGuiKey sdl_key_to_imgui(SDL_Keycode key) {
  switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return ImGuiKey_Enter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: break;
  }
  if (key >= SDLK_A && key <= SDLK_Z) {
    return static_cast<ImGuiKey>(ImGuiKey_A + (key - SDLK_A));
  }
  if (key >= SDLK_0 && key <= SDLK_9) {
    return static_cast<ImGuiKey>(ImGuiKey_0 + (key - SDLK_0));
  }
  if (key >= SDLK_F1 && key <= SDLK_F12) {
    return static_cast<ImGuiKey>(ImGuiKey_F1 + (key - SDLK_F1));
  }
  return ImGuiKey_None;
}

int sdl_button_to_imgui(Uint8 btn) {
  switch (btn) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
  }
}
#endif

std::chrono::steady_clock::time_point g_last_frame{};
bool g_have_last_frame = false;
bool g_initialized = false;

}  // namespace

bool init_platform(void* native_window) {
  g_have_last_frame = false;
  g_initialized = true;
#ifdef HLE_WITH_SDL3
  g_window = static_cast<SDL_Window*>(native_window);
  ImGuiIO& io = ImGui::GetIO();
  io.BackendPlatformName = "snap_sdl3_linux";
  if (g_window) {
    hle::log("ui: overlay hooked into SDL3 window (F1 or hold Back+Start for 0.5 s opens the controls menu)");
  }
#else
  (void)native_window;
#endif
  return true;
}

void new_frame(ImGuiIO& io, float width, float height) {
  const auto now = std::chrono::steady_clock::now();
  float dt = 1.0f / 60.0f;
  if (g_have_last_frame) {
    dt = std::chrono::duration<float>(now - g_last_frame).count();
    if (dt <= 0.0f) dt = 1e-6f;
    if (dt > 0.5f) dt = 0.5f;
  }
  g_last_frame = now;
  g_have_last_frame = true;
  io.DeltaTime = dt;
  io.DisplaySize = ImVec2(width, height);

#ifdef HLE_WITH_SDL3
  if (g_window && SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    const bool ui_active = snap::ui::overlay_open() || snap::ui::overlay_modal_active();
    if (ui_active && !g_text_input_active) {
      SDL_StartTextInput(g_window);
      g_text_input_active = true;
    } else if (!ui_active && g_text_input_active) {
      SDL_StopTextInput(g_window);
      g_text_input_active = false;
    }
    bool want_lock = false, want_recenter = false;
    hle::input::sdl3_cursor_mode(&want_lock, &want_recenter);
    const bool focused = (SDL_GetKeyboardFocus() == g_window) &&
                         ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) == 0);
    const bool target_rel = !ui_active && focused && want_recenter;
    const bool target_grab = !ui_active && focused && want_lock && !target_rel;
    if (target_rel != g_rel_mouse_active) {
      SDL_SetWindowRelativeMouseMode(g_window, target_rel);
      g_rel_mouse_active = target_rel;
      flush_raw_mouse_deltas();
    }
    if (target_grab != g_mouse_grab_active) {
      SDL_SetWindowMouseGrab(g_window, target_grab);
      g_mouse_grab_active = target_grab;
    }
    if (!g_atexit_registered) {
      std::atexit(restore_os_cursor_at_exit);
      g_atexit_registered = true;
    }
    static const bool hide_os_cursor_opt =
        !(std::getenv("SNAP_HIDE_OS_CURSOR") && std::atoi(std::getenv("SNAP_HIDE_OS_CURSOR")) == 0);
    const bool want_hide =
        !ui_active && focused &&
        (hle::input::hub().cursor_hidden() || target_rel || (hide_os_cursor_opt && g_cursor_in_window));
    if (want_hide != g_cursor_hidden_state) {
      if (want_hide) SDL_HideCursor(); else SDL_ShowCursor();
      g_cursor_hidden_state = want_hide;
      static const bool dbg = std::getenv("SNAP_MOUSE_DEBUG") && std::atoi(std::getenv("SNAP_MOUSE_DEBUG")) != 0;
      if (dbg) {
        std::fprintf(stderr, "[mouse_dbg][cursor] hide=%d rel=%d grab=%d ui_active=%d focused=%d\n",
                     (int)want_hide, (int)target_rel, (int)target_grab, (int)ui_active, (int)focused);
      }
    }
  }
#endif
}

void shutdown_platform() {
#ifdef HLE_WITH_SDL3
  if (g_window && SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    if (g_text_input_active) SDL_StopTextInput(g_window);
    SDL_SetWindowRelativeMouseMode(g_window, false);
    SDL_SetWindowMouseGrab(g_window, false);
    SDL_ShowCursor();
  }
  g_text_input_active = false;
  g_rel_mouse_active = false;
  g_mouse_grab_active = false;
  g_cursor_hidden_state = false;
  g_cursor_in_window = false;
  flush_raw_mouse_deltas();
  g_window = nullptr;
#endif
  g_initialized = false;
  g_have_last_frame = false;
}

void release_cursor_confine() {
#ifdef HLE_WITH_SDL3
  if (g_window && SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    SDL_SetWindowRelativeMouseMode(g_window, false);
    SDL_SetWindowMouseGrab(g_window, false);
    SDL_ShowCursor();
  }
  g_rel_mouse_active = false;
  g_mouse_grab_active = false;
  g_cursor_hidden_state = false;
  g_cursor_in_window = false;
  flush_raw_mouse_deltas();
#endif
}

std::string system_font_path(const char* filename) {
  if (!filename || !*filename) return {};
  // Map Windows fallback font names to standard Linux system fonts when present.
  static constexpr const char* kLatinCandidates[] = {
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
  };
  if (std::strcmp(filename, "segoeui.ttf") == 0) {
    std::error_code ec;
    for (const char* p : kLatinCandidates) {
      if (std::filesystem::is_regular_file(p, ec)) return p;
    }
  }
  return {};
}

uint64_t current_process_id() {
  return static_cast<uint64_t>(::getpid());
}

char path_sep() {
  return '/';
}

namespace {

void spawn_detached_argv(const char* dir, char* const argv[]) {
  const pid_t pid = ::fork();
  if (pid != 0) return;
  ::setsid();
  if (dir && *dir) {
    if (::chdir(dir) != 0) _exit(127);
  }
  ::execvp(argv[0], argv);
  _exit(127);
}

bool has_gui_display() {
  const char* disp = std::getenv("DISPLAY");
  const char* way = std::getenv("WAYLAND_DISPLAY");
  return (disp && *disp) || (way && *way);
}

}  // namespace

__attribute__((weak)) extern "C" bool snap_benchmark_launch_from_game() {
  return false;
}

void play_wav_async(const void* /*wav_bytes*/, size_t /*size*/) {
  // Optional audio chime is not yet wired on Linux; visual badge still renders.
}

std::string documents_dir() {
  if (const char* xdg = std::getenv("XDG_DOCUMENTS_DIR"); xdg && *xdg) {
    return xdg;
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return (std::filesystem::path(home) / "Documents").string();
  }
  return {};
}

std::string exe_dir() {
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec || p.empty()) return {};
  return p.parent_path().string();
}

bool beside_exe(const char* rel_path) {
  if (!rel_path || !*rel_path) return false;
  const std::string dir = exe_dir();
  if (dir.empty()) return false;
  std::string rel(rel_path);
  std::replace(rel.begin(), rel.end(), '\\', '/');
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(dir) / rel, ec);
}

bool benchmark_launch_from_game() {
  return snap_benchmark_launch_from_game();
}

bool tcp_port_open(const char* ipv4_addr, unsigned short port) {
  const int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (::inet_pton(AF_INET, ipv4_addr ? ipv4_addr : "127.0.0.1", &a.sin_addr) != 1) {
    ::close(s);
    return false;
  }
  const bool ok = (::connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0);
  ::close(s);
  return ok;
}

void sleep_ms(unsigned ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void open_folder(const std::string& dir) {
  if (dir.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(dir), ec);
  if (!has_gui_display()) return;
  char* const argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(dir.c_str()), nullptr};
  spawn_detached_argv(nullptr, argv);
}

void open_url(const std::string& url) {
  if (url.empty() || !has_gui_display()) return;
  char* const argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(url.c_str()), nullptr};
  spawn_detached_argv(nullptr, argv);
}

bool launch_gallery_server(const std::string& root, const std::string& profile, const std::string& port_file) {
  if (root.empty()) return false;
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root_p(root);
  const fs::path py_venv = root_p / "tools" / "venv-recomp" / "bin" / "python3";
  const std::string py_exe = fs::exists(py_venv, ec) ? py_venv.string() : std::string("python3");
  const std::string script = (root_p / "tools-src" / "gallery" / "serve.py").string();
  if (!fs::exists(script, ec)) return false;
  std::string prof = profile.empty() ? std::string("default") : profile;
  char* const argv[] = {
      const_cast<char*>(py_exe.c_str()),
      const_cast<char*>("-E"),
      const_cast<char*>("-P"),
      const_cast<char*>(script.c_str()),
      const_cast<char*>("--no-open"),
      const_cast<char*>("--profile"),
      const_cast<char*>(prof.c_str()),
      const_cast<char*>("--port"),
      const_cast<char*>("8777"),
      const_cast<char*>("--port-file"),
      const_cast<char*>(port_file.c_str()),
      nullptr,
  };
  spawn_detached_argv(root.c_str(), argv);
  return true;
}

}  // namespace snap::ui::host

extern "C" void snap_ui_on_sdl_event(const void* sdl_event) {
#ifdef HLE_WITH_SDL3
  if (!sdl_event) return;
  const auto* ev = static_cast<const SDL_Event*>(sdl_event);
  const bool ui_active = snap::ui::overlay_open() || snap::ui::overlay_modal_active();
  switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
      if (!ev->key.repeat) {
        if (ev->key.key == SDLK_F1 && !snap::ui::overlay_modal_active()) {
          snap::ui::overlay_set_open(!snap::ui::overlay_open());
          return;
        }
        if (ev->key.key == SDLK_F3 && snap::ui::fps_overlay_hotkey_free()) {
          snap::ui::fps_overlay_cycle();
          return;
        }
        if (ev->key.key == SDLK_F4 && snap::ui::tune_hotkey_free()) {
          snap::ui::tune_toggle_overlay();
          return;
        }
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (!ui_active && snap::ui::host::g_window && SDL_GetKeyboardFocus() == snap::ui::host::g_window) {
        snap::ui::host::g_cursor_in_window = true;
        snap::ui::host::update_posted_abs_pos(ev->motion.x, ev->motion.y);
        snap::ui::host::g_raw_mouse_carry_x += static_cast<double>(ev->motion.xrel);
        snap::ui::host::g_raw_mouse_carry_y += static_cast<double>(ev->motion.yrel);
        const int dx = static_cast<int>(std::trunc(snap::ui::host::g_raw_mouse_carry_x));
        const int dy = static_cast<int>(std::trunc(snap::ui::host::g_raw_mouse_carry_y));
        snap::ui::host::g_raw_mouse_carry_x -= static_cast<double>(dx);
        snap::ui::host::g_raw_mouse_carry_y -= static_cast<double>(dy);
        if (dx != 0 || dy != 0) hle::input::post_raw_mouse(dx, dy);
        static const bool dbg = std::getenv("SNAP_MOUSE_DEBUG") && std::atoi(std::getenv("SNAP_MOUSE_DEBUG")) != 0;
        if (dbg && snap::ui::host::g_rel_mouse_active && (ev->motion.xrel != 0.0f || ev->motion.yrel != 0.0f)) {
          const double ang_deg = std::atan2(-static_cast<double>(ev->motion.yrel), static_cast<double>(ev->motion.xrel)) * 57.2957795;
          std::fprintf(stderr, "[mouse_dbg][sdl] ts_ns=%llu xrel=%.3f yrel=%.3f posted=(%d,%d) ang=%.1fdeg\n",
                       (unsigned long long)ev->motion.timestamp, ev->motion.xrel, ev->motion.yrel, dx, dy, ang_deg);
        }
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (!ui_active && snap::ui::host::g_window && SDL_GetKeyboardFocus() == snap::ui::host::g_window) {
        snap::ui::host::update_posted_abs_pos(ev->button.x, ev->button.y);
      }
      break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
      snap::ui::host::g_cursor_in_window = true;
      break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      snap::ui::host::g_cursor_in_window = false;
      if (snap::ui::host::g_cursor_hidden_state && !snap::ui::host::g_rel_mouse_active) {
        SDL_ShowCursor();
        snap::ui::host::g_cursor_hidden_state = false;
      }
      hle::input::post_mouse_pos(0.0f, 0.0f, false);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MINIMIZED:
      snap::ui::host::release_cursor_confine();
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      snap::ui::host::flush_raw_mouse_deltas();
      break;
    default:
      break;
  }

  if (!ui_active || !snap::ui::overlay_imgui_ready()) {
    return;
  }

  std::lock_guard<std::recursive_mutex> lk(snap::ui::overlay_mutex());
  ImGuiIO& io = ImGui::GetIO();
  switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION: {
      int ww = 0, wh = 0;
      if (snap::ui::host::g_window) SDL_GetWindowSize(snap::ui::host::g_window, &ww, &wh);
      float ix = ev->motion.x, iy = ev->motion.y;
      snap::ui::host::scale_window_pos_to_display(ev->motion.x, ev->motion.y, ww, wh, io.DisplaySize.x, io.DisplaySize.y, &ix, &iy);
      io.AddMousePosEvent(ix, iy);
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      int ww = 0, wh = 0;
      if (snap::ui::host::g_window) SDL_GetWindowSize(snap::ui::host::g_window, &ww, &wh);
      float ix = ev->button.x, iy = ev->button.y;
      snap::ui::host::scale_window_pos_to_display(ev->button.x, ev->button.y, ww, wh, io.DisplaySize.x, io.DisplaySize.y, &ix, &iy);
      io.AddMousePosEvent(ix, iy);
      const int b = snap::ui::host::sdl_button_to_imgui(ev->button.button);
      if (b >= 0) io.AddMouseButtonEvent(b, ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
      io.AddMouseWheelEvent(ev->wheel.x, ev->wheel.y);
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      const bool down = (ev->type == SDL_EVENT_KEY_DOWN);
      const SDL_Keymod mod = SDL_GetModState();
      io.AddKeyEvent(ImGuiMod_Ctrl, (mod & SDL_KMOD_CTRL) != 0);
      io.AddKeyEvent(ImGuiMod_Shift, (mod & SDL_KMOD_SHIFT) != 0);
      io.AddKeyEvent(ImGuiMod_Alt, (mod & SDL_KMOD_ALT) != 0);
      const ImGuiKey ik = snap::ui::host::sdl_key_to_imgui(ev->key.key);
      if (ik != ImGuiKey_None) io.AddKeyEvent(ik, down);
      break;
    }
    case SDL_EVENT_TEXT_INPUT:
      if (ev->text.text && *ev->text.text) io.AddInputCharactersUTF8(ev->text.text);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      io.AddFocusEvent(true);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      io.AddFocusEvent(false);
      break;
    default:
      break;
  }
#else
  (void)sdl_event;
#endif
}
