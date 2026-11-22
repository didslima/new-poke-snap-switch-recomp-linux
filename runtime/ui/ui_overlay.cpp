// SPDX-License-Identifier: MIT
// In-game overlay core: Dear ImGui (Vulkan + Win32 backends) drawn on top of the finished scene blit at native output resolution.
//
//  - snap_ui_present() is the only entry from the NVN layer (runtime/nvn/nvn_vk.cpp, PresentWindow). It runs on the presenting
//    thread, records into the present command buffer and leaves the swapchain image in TRANSFER_DST for the caller's final barrier.
//  - The game window (a plain HWND owned by host/boot.cpp) is subclassed once: F1 toggles the menu, the wheel feeds the input hub,
//    the cursor is hidden/released per the hub's policy and the rest goes to ImGui while the menu is open.
//  - While the menu is open the input hub reports an idle pad to the game; the menu reads keyboard/mouse through ImGui's Win32
//    backend and the pad through the hub (SDL3 + XInput merged), so every controller the game accepts also drives the menu.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hle/access.h"
#include "hle/hle.h"
#include "hle/input_core.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "ui_font.h"
#include "ui_host.h"
#include "ui_i18n.h"
#include "ui_internal.h"
#include "ui_present.h"
#include "ui_theme.h"

namespace snap::ui {

Theme& theme() {
  static Theme t;
  return t;
}

namespace {

using hle::input::hub;

double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Core {
  std::recursive_mutex mu;             // ImGui context (recursive: ImGui_ImplWin32_NewFrame can re-enter the window procedure on the presenting thread): the window thread (messages) and the present thread (frames)
  std::atomic<bool> open{false};
  std::atomic<bool> modal{false};      // shader prompt / progress screen
  bool was_modal = false;
  std::atomic<bool> imgui_ready{false};
  void* hwnd = nullptr;
  bool was_open = false;
  // Vulkan
  VkDevice device = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  std::unordered_map<VkImage, VkImageView> views;
  bool vk_ready = false, srgb = false, failed = false;
  // style
  float applied_scale = 0;
  // notice
  uint64_t seen_serial = 0;
  double toast_until = 0;
};
Core g;


// ---- colors: the swapchain is often an sRGB format, which would brighten straight ImGui colors; pre-convert them
float lin(float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); }
ImVec4 fb(const ImVec4& c) { return g.srgb ? ImVec4(lin(c.x), lin(c.y), lin(c.z), c.w) : c; }

// Accessibility: the high-contrast theme (white on black, blue selection, yellow headings) replaces the light palette in place while the row is on.
// The original values are kept when it is first switched on and put back when it is switched off.
void set_high_contrast(bool on) {
  static bool applied = false;
  static Theme saved;
  static bool have_saved = false;
  if (on == applied) return;
  Theme& t = theme();
  if (on) {
    if (!have_saved) { saved = t; have_saved = true; }
    t.backdrop = ImVec4(0, 0, 0, 0.62f);
    t.panel = ImVec4(0.02f, 0.02f, 0.02f, 1.0f);
    t.panel_alt = ImVec4(0.09f, 0.09f, 0.09f, 1.0f);
    t.border = ImVec4(1, 1, 1, 1);
    t.text = ImVec4(1, 1, 1, 1);
    t.text_dim = ImVec4(0.84f, 0.84f, 0.84f, 1);
    t.title = ImVec4(1.0f, 0.90f, 0.20f, 1);
    t.accent = ImVec4(0.0f, 0.30f, 0.85f, 1);
    t.accent_hover = ImVec4(1.0f, 0.85f, 0.0f, 1);
    t.accent_active = ImVec4(0.10f, 0.45f, 1.0f, 1);
    t.on_accent = ImVec4(1, 1, 1, 1);
    t.slot = ImVec4(0.17f, 0.17f, 0.17f, 1);
    t.slot_hover = ImVec4(0.30f, 0.30f, 0.30f, 1);
    t.slot_listening = ImVec4(0.0f, 0.55f, 0.65f, 1);
    t.conflict = ImVec4(1.0f, 0.66f, 0.0f, 1);
    t.good = ImVec4(0.45f, 1.0f, 0.55f, 1);
    t.error = ImVec4(1.0f, 0.45f, 0.40f, 1);
    t.toast_bg = ImVec4(0, 0, 0, 1);
  } else if (have_saved) {
    t = saved;
  }
  applied = on;
  g.applied_scale = 0;   // apply_style runs again with the new colors
}

void apply_style(float scale) {
  const Theme& t = theme();
  ImGuiStyle s;
  ImGui::StyleColorsLight(&s);
  s.WindowRounding = t.window_round;
  s.ChildRounding = s.FrameRounding = s.PopupRounding = s.GrabRounding = s.TabRounding = t.rounding;
  s.ScrollbarRounding = t.rounding;
  s.WindowPadding = ImVec2(t.window_pad, t.window_pad);
  s.FramePadding = ImVec2(t.frame_pad_x, t.frame_pad_y);
  s.ItemSpacing = ImVec2(t.item_spacing, t.item_spacing);
  s.ItemInnerSpacing = ImVec2(t.item_spacing * 0.75f, t.item_spacing * 0.5f);
  s.WindowBorderSize = 2.0f;
  s.TabBarBorderSize = 0.0f;
  s.FrameBorderSize = 0.0f;
  s.FrameBorderSize = 0.0f;
  s.ScrollbarSize = 14.0f;
  s.GrabMinSize = 16.0f;
  s.CellPadding = ImVec2(6.0f, 4.0f);
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = fb(t.text);
  c[ImGuiCol_TextDisabled] = fb(t.text_dim);
  c[ImGuiCol_WindowBg] = fb(t.panel);
  c[ImGuiCol_ChildBg] = fb(t.panel_alt);
  c[ImGuiCol_PopupBg] = fb(t.panel);
  c[ImGuiCol_Border] = fb(t.border);
  c[ImGuiCol_FrameBg] = fb(t.slot);
  c[ImGuiCol_FrameBgHovered] = fb(t.slot_hover);
  c[ImGuiCol_FrameBgActive] = fb(t.accent);
  c[ImGuiCol_Button] = fb(t.slot);
  c[ImGuiCol_ButtonHovered] = fb(t.slot_hover);
  c[ImGuiCol_ButtonActive] = fb(t.accent);
  c[ImGuiCol_Header] = fb(t.slot);
  c[ImGuiCol_HeaderHovered] = fb(t.slot_hover);
  c[ImGuiCol_HeaderActive] = fb(t.accent);
  c[ImGuiCol_CheckMark] = fb(t.title);
  c[ImGuiCol_SliderGrab] = fb(t.accent);
  c[ImGuiCol_SliderGrabActive] = fb(t.accent_hover);
  c[ImGuiCol_Separator] = fb(t.border);
  c[ImGuiCol_Tab] = fb(t.slot);
  c[ImGuiCol_TabHovered] = fb(t.slot_hover);
  c[ImGuiCol_TabSelected] = fb(t.accent);
  c[ImGuiCol_TabSelectedOverline] = fb(t.accent_hover);   // orange line under the yellow tab
  c[ImGuiCol_TabDimmed] = fb(t.slot);
  c[ImGuiCol_TabDimmedSelected] = fb(t.accent);
  c[ImGuiCol_TableHeaderBg] = fb(t.panel_alt);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(0.4f, 0.3f, 0.2f, g.srgb ? 0.025f : 0.05f);
  c[ImGuiCol_TableBorderLight] = fb(ImVec4(t.border.x, t.border.y, t.border.z, 0.5f));
  c[ImGuiCol_TableBorderStrong] = fb(t.border);
  c[ImGuiCol_NavCursor] = fb(t.accent_hover);
  c[ImGuiCol_TextSelectedBg] = fb(ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.5f));
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.5f);
  c[ImGuiCol_ScrollbarBg] = fb(t.panel);
  c[ImGuiCol_ScrollbarGrab] = fb(t.slot_hover);
  s.ScaleAllSizes(scale);
  s.FontSizeBase = t.font_px;
  s.FontScaleMain = scale;
  ImGui::GetStyle() = s;
  g.applied_scale = scale;
}

// Fallback fonts for the glyphs the base font lacks. The game's own UI font has full Hangul and most kana, but only part of the accented Latin letters
// and of the kanji / hanzi, so the menu in French, German, Spanish, Italian, Japanese, Korean or Chinese needs a second source. System files from the
// Windows fonts folder, never shipped or copied. Added when the menu language first needs them (ImGui merges fonts at run time and bakes glyphs on demand).
bool g_font_explicit = true;   // the base font was added with an explicit size (a merged font then needs one too)
unsigned g_font_done = 0;      // bit 0: Latin letters (Segoe UI), bit 1 + language: the file that covers that language's script

void merge_system_font(const char* file, int face, float em_scale) {   // em_scale: the game font draws a bigger letter for the same size than these
  const std::string path = host::system_font_path(file);
  if (path.empty()) { hle::log("ui: fallback font %s not found (some characters will show as boxes)", file); return; }
  ImFontConfig fc;
  fc.MergeMode = true;
  fc.FontNo = face;
  fc.ExtraSizeScale = em_scale;
  ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), g_font_explicit ? theme().font_px : 0.0f, &fc);
  hle::log("ui: merged fallback font %s", file);
}

// Called with the ImGui lock held before every frame: cheap once everything the language needs is in.
void ensure_fallback_fonts(bool base_is_game_font) {
  if (!(g_font_done & 1u)) {
    g_font_done |= 1u;
    if (base_is_game_font) merge_system_font("segoeui.ttf", 0, 1.33f);   // accented Latin letters the game font does not have
  }
  const int lang = i18n_lang();
  if (!i18n_is_cjk(lang) || (g_font_done & (2u << lang))) return;
  g_font_done |= 2u << lang;
  switch (lang) {
    case kLangJa: merge_system_font("YuGothM.ttc", 0, 1.10f); break;
    case kLangKo: merge_system_font("malgun.ttf", 0, 1.33f); break;
    case kLangZhHans: merge_system_font("msyh.ttc", 0, 1.32f); break;
    case kLangZhHant: merge_system_font("msjh.ttc", 0, 1.33f); break;
    default: break;
  }
}
bool g_base_game_font = false;

bool init_imgui(const SnapUiFrame* f) {
  if (g.failed) return false;
  if (g.imgui_ready.load()) return g.format == f->format;   // a format change after init is not supported: no overlay then
  std::lock_guard<std::recursive_mutex> lk(g.mu);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  g.format = f->format;
  g.srgb = f->format == VK_FORMAT_B8G8R8A8_SRGB || f->format == VK_FORMAT_R8G8B8A8_SRGB;
  g.device = f->device;
  g.hwnd = f->hwnd;
  if (!host::init_platform(f->hwnd)) { hle::log("ui: platform init failed"); g.failed = true; ImGui::DestroyContext(); return false; }
  ImGui_ImplVulkan_InitInfo ii{};
  ii.ApiVersion = VK_API_VERSION_1_3;
  ii.Instance = f->instance;
  ii.PhysicalDevice = f->phys;
  ii.Device = f->device;
  ii.QueueFamily = f->family;
  ii.Queue = f->queue;
  ii.DescriptorPoolSize = 16;
  ii.MinImageCount = 2;
  ii.ImageCount = std::max(4u, f->image_count + 2);   // frames the present path can keep in flight
  ii.UseDynamicRendering = true;
  ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  static VkFormat color_format;
  color_format = f->format;
  ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
  if (!ImGui_ImplVulkan_Init(&ii)) { hle::log("ui: ImGui_ImplVulkan_Init failed"); g.failed = true; host::shutdown_platform(); ImGui::DestroyContext(); return false; }
  g.vk_ready = true;
  // font: the game's own UI font decoded from the user's extracted files at run time (ui_font.h; never copied into the repo), else Segoe UI
  // from the Windows fonts folder when present (system file, not shipped), else the embedded vector font
  bool font_ok = false;
  {
    static std::vector<unsigned char> game_font;   // must outlive the atlas
    if (game_font_ttf(game_font)) {
      ImFontConfig fc;
      fc.FontDataOwnedByAtlas = false;
      static const ImWchar g_font_blank_ranges[] = {0xA0, 0xFF, 0x30FC, 0x30FC, 0};   // the accented Latin letters are tiny here and the long-vowel mark is empty: the merged system fonts draw them
      fc.GlyphExcludeRanges = g_font_blank_ranges;
      font_ok = io.Fonts->AddFontFromMemoryTTF(game_font.data(), (int)game_font.size(), theme().font_px, &fc) != nullptr;
      if (font_ok) { g_base_game_font = true; hle::log("ui: using the game's UI font (%s) decoded from the extracted files", game_font_family()); }
    }
  }
  if (!font_ok) {
    const std::string path = host::system_font_path(theme().font_file);
    if (!path.empty()) font_ok = io.Fonts->AddFontFromFileTTF(path.c_str(), theme().font_px) != nullptr;
  }
  if (!font_ok) { io.Fonts->AddFontDefaultVector(); g_font_explicit = false; }
  ensure_fallback_fonts(g_base_game_font);
  apply_style(1.0f);
  g.imgui_ready = true;
  hle::log("ui: ImGui %s ready, swapchain format %d%s", IMGUI_VERSION, (int)f->format, g.srgb ? " (sRGB, colors pre-converted)" : "");
  return true;
}

VkImageView view_for(const SnapUiFrame* f) {
  if (g.swapchain != f->swapchain) {   // swapchain recreated (the present path waited for the device to go idle): drop old views
    for (auto& kv : g.views) vkDestroyImageView(f->device, kv.second, nullptr);
    g.views.clear();
    g.swapchain = f->swapchain;
  }
  auto it = g.views.find(f->image);
  if (it != g.views.end()) return it->second;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = f->image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = f->format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView v = VK_NULL_HANDLE;
  if (vkCreateImageView(f->device, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
  g.views[f->image] = v;
  return v;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
}

// Gamepad navigation from the hub's merged pad state (SDL3 + XInput), so any controller the game accepts drives the menu.
void feed_gamepad(ImGuiIO& io, bool enabled) {
  using namespace hle::input;
  RawInput r = hub().raw();
  const bool on = enabled && r.focused;
  auto btn = [&](ImGuiKey k, bool v) { io.AddKeyEvent(k, on && v); };
  btn(ImGuiKey_GamepadFaceDown, r.pad.test(kPadSouth));
  btn(ImGuiKey_GamepadFaceRight, r.pad.test(kPadEast));
  btn(ImGuiKey_GamepadFaceLeft, r.pad.test(kPadWest));
  btn(ImGuiKey_GamepadFaceUp, r.pad.test(kPadNorth));
  btn(ImGuiKey_GamepadDpadUp, r.pad.test(kPadDUp));
  btn(ImGuiKey_GamepadDpadDown, r.pad.test(kPadDDown));
  btn(ImGuiKey_GamepadDpadLeft, r.pad.test(kPadDLeft));
  btn(ImGuiKey_GamepadDpadRight, r.pad.test(kPadDRight));
  btn(ImGuiKey_GamepadL1, r.pad.test(kPadLShoulder));
  btn(ImGuiKey_GamepadR1, r.pad.test(kPadRShoulder));
  btn(ImGuiKey_GamepadL3, r.pad.test(kPadLStick));
  btn(ImGuiKey_GamepadR3, r.pad.test(kPadRStick));
  auto analog = [&](ImGuiKey k, float v) {
    const float dz = 0.25f;
    float a = on && v > dz ? std::min(1.0f, (v - dz) / (1.0f - dz)) : 0.0f;
    io.AddKeyAnalogEvent(k, a > 0.0f, a);
  };
  analog(ImGuiKey_GamepadLStickLeft, -r.lx);
  analog(ImGuiKey_GamepadLStickRight, r.lx);
  analog(ImGuiKey_GamepadLStickUp, r.ly);
  analog(ImGuiKey_GamepadLStickDown, -r.ly);
  analog(ImGuiKey_GamepadRStickLeft, -r.rx);
  analog(ImGuiKey_GamepadRStickRight, r.rx);
  analog(ImGuiKey_GamepadRStickUp, r.ry);
  analog(ImGuiKey_GamepadRStickDown, -r.ry);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, on && r.lt > 0.3f, on ? r.lt : 0.0f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, on && r.rt > 0.3f, on ? r.rt : 0.0f);
}

}  // namespace

ImVec4 overlay_fb(const ImVec4& c) { return fb(c); }   // for ui_skin.cpp: the same sRGB pre-conversion the ImGui style colors get

bool overlay_open() { return g.open.load(); }
void* overlay_hwnd() { return g.hwnd; }
std::recursive_mutex& overlay_mutex() { return g.mu; }
bool overlay_imgui_ready() { return g.imgui_ready.load(); }
bool overlay_modal_active() { return g.modal.load(); }

// Window thread (F1) and present thread (chord, scripted test) both call this.
void overlay_set_open(bool open) {
  if (g.open.exchange(open) == open) return;
  hle::input::hub().set_menu_open(open || g.modal.load());
  if (open) host::release_cursor_confine();   // the cursor must reach the menu, not stay confined by the game
  hle::log("ui: controls menu %s", open ? "opened" : "closed");
}

void overlay_set_modal(bool on) {
  if (g.modal.exchange(on) == on) return;
  hle::input::hub().set_menu_open(on || g.open.load());
  if (on) host::release_cursor_confine();
  hle::log("ui: shader screen %s", on ? "shown" : "closed");
}

}  // namespace snap::ui

static void ui_present_impl(const SnapUiFrame* f);
extern "C" bool snap_ui_start_pending() { return snap::ui::start_note_on(); }
extern "C" void snap_ui_first_picture() { snap::ui::start_note_done(); }
// Exceptions never reach the NVN present path: log, drop the frame's overlay and keep the game running.
extern "C" void snap_ui_present(const SnapUiFrame* f) {
  static int errors = 0;
  try {
    // performance overlay cost (ui_fps.cpp): CPU time of the whole host-overlay frame while it is on, logged every 1200 presents
    if (snap::ui::fps_overlay_mode() != 0) {
      const auto a = std::chrono::steady_clock::now();
      ui_present_impl(f);
      const auto b = std::chrono::steady_clock::now();
      static float samp[1200]; static int n = 0;
      samp[n] = std::chrono::duration<float, std::micro>(b - a).count();
      if (++n == 1200) {   // median and p99 (the mean is dominated by the present thread being preempted on a busy PC)
        double sum = 0; for (float s : samp) sum += s;
        std::sort(samp, samp + n);
        hle::log("ui: performance overlay %d: host overlay CPU per present: median %.1f us, p99 %.1f us, mean %.1f us", snap::ui::fps_overlay_mode(), samp[n / 2], samp[n * 99 / 100], sum / n);
        n = 0;
      }
      return;
    }
    ui_present_impl(f);
  } catch (const std::exception& e) {
    if (errors++ < 20) hle::log("ui: exception in the overlay frame: %s", e.what());
  } catch (...) {
    if (errors++ < 20) hle::log("ui: unknown exception in the overlay frame");
  }
}

static void ui_present_impl(const SnapUiFrame* f) {
  using namespace snap::ui;
  auto& hubr = hle::input::hub();
  g.hwnd = f->hwnd;
  const double now = now_s();
  test_tick(now);
  const bool shader = shader_screen_active(now);
  overlay_set_modal(shader);
  if (!shader && g.was_modal) shader_screen_closed();
  g.was_modal = shader;
  if (hubr.consume_menu_toggle() && !shader) overlay_set_open(!g.open.load());
  const bool open = g.open.load() && !shader;
  if (open && !g.was_open) menu_opened();
  if (!open && g.was_open) menu_closed();
  g.was_open = open;
  menu_tick_save();

  // a load or save problem (invalid input.json, migration) shows as a notice at the bottom for a few seconds
  const uint64_t serial = hubr.message_serial();
  if (serial != g.seen_serial) {
    g.seen_serial = serial;
    if (!hubr.message().empty()) g.toast_until = now + 8.0;   // once per message: shown 8 s, the last 1.5 s fading
  }
  const bool toast = !open && now < g.toast_until;
  const bool perf = fps_overlay_tick();   // performance overlay (ui_fps.cpp); one atomic load when Off
  const bool tune = tune_tick();          // SNAP_DEV=1 dev layer (ui_tune.cpp): GPU cost overlay, shader errors; one cached bool when off
  const bool safe = safe_banner_on();     // safe-mode banner (ui_fps.cpp)
  const bool stnote = state_note_on();    // save state note (ui_savestate.cpp)
  const bool starting = start_note_on();  // "Starting the game" until the first picture (ui_fps.cpp)
  if (!open && !shader && !toast && !perf && !tune && !safe && !stnote && !starting && !shiny_fx_active() && !shiny_badges_active()) return;
  if (!init_imgui(f)) return;
  if (open || shader) hubr.poll(8);   // keeps rebinding and the live readout fresh even when the game is not polling

  const ImVec2 size(static_cast<float>(f->extent.width), static_cast<float>(f->extent.height));
  ImDrawData* dd = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    const hle::access::State& acc = hle::access::state();
    set_high_contrast(acc.contrast != 0);
    float scale = std::clamp(hubr.mapping().ui_scale, 0.5f, 3.0f) * std::clamp(acc.ui_size, 100, 200) / 100.0f * std::max(0.75f, size.y / 1080.0f);
    scale = std::clamp(scale, 0.5f, std::max(0.5f, std::min(size.x / 960.0f, size.y / 540.0f)));
    if (std::fabs(scale - g.applied_scale) > 0.001f) apply_style(scale);
    ensure_fallback_fonts(g_base_game_font);   // the menu language can change while the game runs (Game language row)
    ImGui_ImplVulkan_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    host::new_frame(io, size.x, size.y);
    feed_gamepad(io, (open || shader) && !hubr.capturing());
    ImGui::NewFrame();
    if (shader) {
      draw_shader_screen(size);
    } else if (open) {
      bool close = false;
      draw_menu(size, &close);
      if (close) overlay_set_open(false);
    } else if (toast) {   // only inside its window: other host layers (performance overlay, shiny badges) also draw frames
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, (float)std::clamp((g.toast_until - now) / 1.5, 0.0, 1.0));   // fades out over the last 1.5 s
      if (!draw_toast(size, now)) g.toast_until = 0;
      ImGui::PopStyleVar();
    }
    if (!open && !shader) { draw_shiny_fx(size); draw_shiny_badges(size); }   // shiny capture effect (ui_shiny.cpp)
    if (tune && !shader) draw_tune_overlay(size);   // never in the game's own photos (host overlay)
    if (safe && !shader) draw_safe_banner(size);
    if (starting && !open && !shader) draw_start_note(size);   // host overlay: never in the game's own photos
    if (stnote && !shader) draw_state_note(size);   // host overlay: never in the game's own photos
    if (perf && !shader) draw_fps_overlay(size);   // over the game and the F1 menu; never in the game's own photos (host overlay)
    ImGui::Render();
    dd = ImGui::GetDrawData();
  }
  if (!dd || dd->CmdListsCount == 0) return;
  VkImageView view = view_for(f);
  if (!view) return;
  barrier(f->cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = view;
  ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // keep the scene
  ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, f->extent};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &ca;
  vkCmdBeginRendering(f->cb, &ri);
  ImGui_ImplVulkan_RenderDrawData(dd, f->cb);
  vkCmdEndRendering(f->cb);
  barrier(f->cb, f->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

extern "C" void snap_ui_shutdown() {
  std::lock_guard<std::recursive_mutex> lk(snap::ui::g.mu);
  if (snap::ui::g.device != VK_NULL_HANDLE) {
    for (auto& kv : snap::ui::g.views) vkDestroyImageView(snap::ui::g.device, kv.second, nullptr);
  }
  snap::ui::g.views.clear();
  snap::ui::g.swapchain = VK_NULL_HANDLE;
  snap::ui::g.device = VK_NULL_HANDLE;
  if (snap::ui::g.vk_ready) { ImGui_ImplVulkan_Shutdown(); snap::ui::g.vk_ready = false; }
  if (snap::ui::g.imgui_ready.exchange(false)) {
    snap::ui::host::shutdown_platform();
    ImGui::DestroyContext();
  }
  snap::ui::g.hwnd = nullptr;
  snap::ui::g.format = VK_FORMAT_UNDEFINED;
  snap::ui::g.failed = false;
}
