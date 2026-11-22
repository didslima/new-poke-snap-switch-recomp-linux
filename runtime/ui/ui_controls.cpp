// SPDX-License-Identifier: MIT
// The controls menu: bindings per device (keyboard, mouse, controller), mouse and controller options, accessibility, live npad readout.
// Edits go to the input hub at once (live) and are saved to input.json a moment later (atomic write). Colors and sizes come from
// ui_theme.h only.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <string>

#include "hle/hle.h"
#include "hle/input_core.h"
#include "hle/settings.h"
#include "imgui.h"
#include "imgui_internal.h"   // GImGui: is the pad / keyboard cursor showing
#include "nvn/nvn_dev.h"
#include "ui_internal.h"
#include "ui_i18n.h"
#include "ui_skin.h"
#include "ui_theme.h"

namespace snap::ui {

using namespace hle::input;

namespace {

double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Rebind {
  bool active = false;
  int action = 0;
  Device dev = kDevKeyboard;
  int slot = 0;
};

struct MenuState {
  Mapping edit;
  bool dirty = false;
  double dirty_t = 0;
  Rebind rb;
  const char* want_tab = nullptr;
  int cur_tab = 0;               // index into kTabs of the page drawn this frame
  int reset_dev = -1;            // device whose reset is awaiting confirmation
  std::string note;              // short feedback line ("Bound A to P", "Saved")
  double note_until = 0;
  uint64_t dismissed_serial = 0;
  float ui_scale_edit = 1.0f;
  double saved_flash_until = 0;
} S;

// scale a design size (pixels at font 18) with the current font size
float px(float v) { return v * ImGui::GetFontSize() / theme().font_px; }

std::string sf(const char* f, ...) {   // printf into a std::string (the format is a tr() key)
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

void say(const std::string& s, double secs = 4.0) {
  S.note = s;
  S.note_until = now_s() + secs;
}

void apply() {
  hub().set_mapping(S.edit);
  S.dirty = true;
  S.dirty_t = now_s();
}

ImU32 col(const ImVec4& c) { return ImGui::GetColorU32(c); }

// ---- rebinding -------------------------------------------------------------------------------------------------------
void finish_rebind() {
  S.rb.active = false;
  hub().capture_cancel();
}

void begin_rebind(int action, Device dev, int slot) {
  S.rb = Rebind{true, action, dev, slot};
  hub().capture_begin(1u << (int)dev);
}

void resolve_rebind(bool* esc_used) {
  if (!S.rb.active) return;
  Source s;
  bool cancel = false, clear = false, got = false;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
  else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) clear = true;
  else if (hub().capture_result(&s)) {
    got = true;
    if (s.kind == Source::KeyboardKey && s.id == key_from_name("Escape")) { cancel = true; got = false; }
    else if (s.kind == Source::KeyboardKey && s.id == key_from_name("Delete")) { clear = true; got = false; }
  } else if (hub().capture_timed_out()) {
    finish_rebind();
    say(tr("Rebinding timed out. Nothing changed."));
    return;
  }
  if (!cancel && !clear && !got) return;
  *esc_used = true;
  auto& v = action_sources(S.edit, S.rb.action);
  if (got) {
    slot_set(v, S.rb.dev, S.rb.slot, s);
    apply();
    say(sf(tr("%s (%s) = %s"), tr(action_label(S.rb.action)), tr(device_name(S.rb.dev)), tr(source_label(s))));
  } else if (clear) {
    slot_clear(v, S.rb.dev, S.rb.slot);
    apply();
    say(sf(tr("%s (%s) cleared"), tr(action_label(S.rb.action)), tr(device_name(S.rb.dev))));
  } else {
    say(tr("Rebinding cancelled."));
  }
  finish_rebind();
}

void slot_button(int action, Device dev, int slot) {
  const Theme& t = theme();
  auto& v = action_sources(S.edit, action);
  Source s = slot_get(v, dev, slot);
  const bool listening = S.rb.active && S.rb.action == action && S.rb.dev == dev && S.rb.slot == slot;
  std::vector<int> users;
  if (s.kind != Source::None) users = actions_using(S.edit, s);
  const bool conflict = users.size() > 1;
  std::string label = listening ? std::string(tr("Press a key...")) : s.kind == Source::None ? std::string("-") : std::string(tr(source_label(s)));
  if (dev == kDevMouse && listening) label = tr("Press a button...");
  if (dev == kDevController && listening) label = tr("Press a button...");
  if (conflict && !listening) label = "! " + label;   // a cue besides the color
  int pushed = 0;
  if (listening) { ImGui::PushStyleColor(ImGuiCol_Button, t.slot_listening); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1)); pushed = 2; }
  else if (conflict) { ImGui::PushStyleColor(ImGuiCol_Button, t.conflict); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1)); pushed = 2; }
  else if (s.kind == Source::None) { ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim); pushed = 1; }
  // Button icons setting: a controller button shows as its Xbox or PlayStation picture instead of its name
  const float gh = ImGui::GetFontSize() * 1.25f;
  const bool glyphed = !listening && s.kind == Source::Pad && style_pad_glyph_width(s.id, gh) > 0.0f;
  ImGui::PushID(action * 16 + (int)dev * 4 + slot);
  if (ImGui::Button(glyphed ? "##pad" : label.c_str(), ImVec2(px(t.slot_w), 0))) {
    if (listening) { finish_rebind(); say(tr("Rebinding cancelled.")); }
    else begin_rebind(action, dev, slot);
  }
  if (glyphed) {
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* mark = conflict ? "! " : "";
    const float mw = ImGui::CalcTextSize(mark).x, gw = style_pad_glyph_width(s.id, gh);
    float x = (mn.x + mx.x - mw - gw) * 0.5f;
    if (conflict) { dl->AddText(ImVec2(x, (mn.y + mx.y - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), mark); x += mw; }
    style_pad_glyph(dl, ImVec2(x, (mn.y + mx.y - gh) * 0.5f), gh, s.id);
  }
  const bool hovered = ImGui::IsItemHovered();
  if (s.kind != Source::None && !listening &&
      ((hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) || (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))))) {
    slot_clear(v, dev, slot);
    apply();
    say(sf(tr("%s (%s) cleared"), tr(action_label(action)), tr(device_name(dev))));
  }
  if (hovered && ImGui::BeginTooltip()) {
    if (conflict) {
      std::string others;
      for (int a : users) if (a != action) others += std::string(others.empty() ? "" : ", ") + tr(action_label(a));
      ImGui::TextColored(t.conflict, tr("Also bound to: %s"), others.c_str());
      ImGui::TextDisabled("%s", tr("Allowed: both actions fire together."));
    }
    ImGui::TextDisabled("%s", tr("Click, then press the key, button or axis.\nEsc cancels. Delete or right-click clears."));
    ImGui::EndTooltip();
  }
  ImGui::PopID();
  if (pushed) ImGui::PopStyleColor(pushed);
}

// ---- small widgets ---------------------------------------------------------------------------------------------------
void desc(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, theme().text_dim);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(tr(text));
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

bool checkbox(const char* label, bool* v, const char* d) {
  bool ch = ImGui::Checkbox((std::string(tr(label)) + "###" + label).c_str(), v);
  if (d) { ImGui::Indent(px(30)); desc(d); ImGui::Unindent(px(30)); }
  ImGui::Spacing();
  return ch;
}

bool slider(const char* label, const char* id, float* v, float lo, float hi, const char* fmt, const char* d) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(tr(label));
  ImGui::SameLine(px(250));
  ImGui::SetNextItemWidth(px(340));
  bool ch = ImGui::SliderFloat(id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
  if (d) { ImGui::Indent(px(30)); desc(d); ImGui::Unindent(px(30)); }
  ImGui::Spacing();
  return ch;
}

void stick_plot(const char* id, float x, float y, float dz, float outer) {
  const Theme& t = theme();
  float sz = px(t.stick_plot);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(sz, sz));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
  float r = sz * 0.5f - 2;
  dl->AddCircleFilled(c, r, col(t.slot), 48);
  if (dz > 0) dl->AddCircle(c, r * dz, col(t.border), 32);
  if (outer < 0.995f) dl->AddCircle(c, r * outer, col(t.border), 32);
  dl->AddCircle(c, r, col(t.border), 48);
  dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col(t.border));
  dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col(t.border));
  float dx = std::clamp(x, -1.0f, 1.0f) * r, dy = -std::clamp(y, -1.0f, 1.0f) * r;
  dl->AddCircleFilled(ImVec2(c.x + dx, c.y + dy), px(7), col(std::fabs(x) + std::fabs(y) > 0.01f ? t.accent_hover : t.text_dim), 16);
  (void)id;
}

// device 0..2 resets that device's bindings and options, kDevCount resets all three (the buttons ask first; the test script command reset_binds calls it directly)
void do_reset(int dev) {
  if (dev == (int)kDevCount) {
    for (int dv = 0; dv < kDevCount; ++dv) reset_device(S.edit, (Device)dv);
    apply();
    say(tr("All controls reset to defaults"));
  } else {
    const Device d = (Device)std::clamp(dev, 0, (int)kDevCount - 1);
    reset_device(S.edit, d);
    apply();
    say(sf(tr("%s reset to defaults"), tr(device_name(d))));
  }
}

void confirm_reset() {
  if (S.reset_dev >= 0) ImGui::OpenPopup("###resetpop");
  ImVec2 c = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal((std::string(tr("Reset to defaults?")) + "###resetpop").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    const bool all = S.reset_dev == (int)kDevCount;   // "Reset all controls": keyboard, mouse and controller together
    Device d = (Device)std::clamp(S.reset_dev, 0, (int)kDevCount - 1);
    if (all) {
      ImGui::TextUnformatted(tr("Reset the bindings and options of the keyboard, the mouse and the controller to the defaults?"));
    } else {
      ImGui::Text(tr("Reset all %s bindings and options to the defaults?"), tr(device_name(d)));
      ImGui::TextDisabled("%s", tr("Other devices keep their settings."));
    }
    ImGui::Spacing();
    if (ImGui::Button(tr("Reset"), ImVec2(px(140), 0))) {
      do_reset(S.reset_dev);
      S.reset_dev = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(px(140), 0))) { S.reset_dev = -1; ImGui::CloseCurrentPopup(); }
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
  } else if (!ImGui::IsPopupOpen("###resetpop")) {
    S.reset_dev = -1;
  }
}

void reset_button(Device d) {
  std::string l = sf(tr("Reset %s to defaults"), tr(device_name(d)));
  if (ImGui::Button(l.c_str())) S.reset_dev = (int)d;
}
void reset_all_button() {
  if (ImGui::Button(tr("Reset all controls to defaults"))) S.reset_dev = (int)kDevCount;
}

// ---- tabs ------------------------------------------------------------------------------------------------------------
const char* const kTabs[] = {"Bindings", "Mouse", "Controller", "Accessibility", "Audio", "Graphics", "Display", "Photos", "Mods", "Saves", "Save state", "Cheats", "Benchmark", "Test input", "Tuning"};
constexpr int kTabCount = (int)(sizeof kTabs / sizeof kTabs[0]);
int tab_count() { return dev_on() ? kTabCount : kTabCount - 1; }   // "Tuning" (ui_tune.cpp) exists only with SNAP_DEV=1

void tab_bindings(float footer_h) {
  const Theme& t = theme();
  desc("Click a slot, then press a key, mouse button, wheel notch, controller button or stick direction. Two slots per device for each Switch input. "
       "Esc cancels, Delete or right-click clears. A '!' marks a source used by more than one input (allowed).");
  ImGui::Spacing();
  reset_button(kDevKeyboard); ImGui::SameLine();
  reset_button(kDevMouse); ImGui::SameLine();
  reset_button(kDevController); ImGui::SameLine();
  reset_all_button();
  ImGui::Spacing();
  const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
  float h = ImGui::GetContentRegionAvail().y - footer_h;
  if (ImGui::BeginTable("bindings", 7, tf, ImVec2(0, std::max(h, px(120))))) {
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn(tr("Switch input"), ImGuiTableColumnFlags_WidthFixed, px(t.label_w));
    const char* heads[6] = {"Keyboard 1", "Keyboard 2", "Mouse 1", "Mouse 2", "Controller 1", "Controller 2"};
    for (const char* hname : heads) ImGui::TableSetupColumn(tr(hname), ImGuiTableColumnFlags_WidthFixed, px(t.slot_w) + px(8));
    ImGui::TableHeadersRow();
    struct Group { const char* title; int first, count; };
    static const int buttons[] = {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kUp, kDown, kLeft, kRight, kStickL, kStickR};
    int conflicts = 0;
    for (int a = 0; a < kActionCount; ++a)
      for (int d = 0; d < kDevCount; ++d)
        for (int sl = 0; sl < kMaxSlotsPerDevice; ++sl) {
          Source s = slot_get(action_sources(S.edit, a), (Device)d, sl);
          if (s.kind != Source::None && actions_using(S.edit, s).size() > 1) ++conflicts;
        }
    auto section = [&](const char* title) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(t.title, "%s", tr(title));
    };
    auto row = [&](int action) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(tr(action_label(action)));
      int ci = 1;
      for (int d = 0; d < kDevCount; ++d)
        for (int sl = 0; sl < kMaxSlotsPerDevice; ++sl) {
          ImGui::TableSetColumnIndex(ci++);
          slot_button(action, (Device)d, sl);
        }
    };
    section("Buttons");
    for (int b : buttons) row(b);
    section("Left stick (movement)");
    for (int a = 16; a < 20; ++a) row(a);
    section("Right stick (camera aim)");
    for (int a = 20; a < 24; ++a) row(a);
    ImGui::EndTable();
    if (conflicts) {
      ImGui::TextColored(t.conflict, conflicts == 1 ? tr("! %d slot shares a source with another input.") : tr("! %d slots share a source with another input."), conflicts);
      // which sources, and which inputs they drive (first three; hover a marked slot for the rest)
      std::vector<Source> seen;
      std::string shared;
      int listed = 0;
      for (int a = 0; a < kActionCount && listed < 3; ++a)
        for (int d = 0; d < kDevCount && listed < 3; ++d)
          for (int sl = 0; sl < kMaxSlotsPerDevice && listed < 3; ++sl) {
            const Source s = slot_get(action_sources(S.edit, a), (Device)d, sl);
            if (s.kind == Source::None || std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
            const std::vector<int> users = actions_using(S.edit, s);
            if (users.size() < 2) continue;
            seen.push_back(s);
            std::string who;
            for (int u : users) who += std::string(who.empty() ? "" : ", ") + tr(action_label(u));
            shared += std::string(shared.empty() ? "" : "   |   ") + tr(source_label(s)) + ": " + who;
            ++listed;
          }
      ImGui::TextColored(t.conflict, "%s", shared.c_str());
    } else ImGui::TextDisabled("%s", tr("No conflicts."));
  }
}

void tab_mouse() {
  Mapping& m = S.edit;
  desc("Mouse buttons and the wheel are bound on the Bindings tab. The left button is also the touch screen.");
  ImGui::Spacing();
  reset_button(kDevMouse);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  bool ch = false;
  ch |= checkbox("Mouse look (mouse moves the camera)", &m.mouse_look,
                 "Mouse movement drives the right stick, the camera aim. Off by default. While on, the cursor is hidden and held at the window center, the left button no longer acts as the touch screen, and during a ride the left button is A (Take Photo) and the right button is ZL (Zoom). Change those two in input.json (mouse.look_click_a, mouse.look_click_zl).");
  ImGui::BeginDisabled(!m.mouse_look);
  ch |= slider("Sensitivity", "##sens", &m.mouse_sensitivity, 0.1f, 5.0f, "%.2f",
               "Master multiplier for mouse look speed. 1.00 reaches full deflection at 40 pixels per frame.");
  ch |= slider("Horizontal sensitivity", "##sens_x", &m.mouse_sens_x, 0.1f, 5.0f, "%.2f",
               "Independent horizontal sensitivity multiplier.");
  ch |= slider("Vertical sensitivity", "##sens_y", &m.mouse_sens_y, 0.1f, 5.0f, "%.2f",
               "Independent vertical sensitivity multiplier.");
  ch |= checkbox("Invert horizontal", &m.mouse_invert_x, "Moving the mouse right turns the camera left.");
  ch |= checkbox("Invert vertical", &m.mouse_invert_y, "Moving the mouse up aims down, like a flight stick.");
  ch |= slider("Smoothing", "##smooth", &m.mouse_smoothing, 0.0f, 1.0f, m.mouse_smoothing <= 0.001f ? tr("Off") : "%.2f",
               "Averages the movement over a longer window. Off is the most direct; higher values trade a little lag for steadier aim.");
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::Spacing();
  ch |= checkbox("Left mouse button is the touch screen", &m.mouse_touch,
                 "Clicking in the window taps the Switch touch screen (used by some menus). Ignored while mouse look is on.");
  ch |= checkbox("Lock the cursor to the window while playing", &m.lock_cursor,
                 "Keeps the cursor inside the game window during play. It is released while this menu is open and whenever the window loses focus.");
  if (ch) apply();
}

void tab_controller() {
  Mapping& m = S.edit;
  desc("Controller buttons are bound on the Bindings tab. Any controller SDL3 or XInput knows works; all of them act as one Switch Pro Controller.");
  ImGui::Spacing();
  reset_button(kDevController);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  bool ch = false;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(tr("Face button layout"));
  int layout = m.face_layout;
  ImGui::SameLine(px(250));
  if (ImGui::RadioButton(tr("By position (Xbox B = Switch A)"), layout == 0)) layout = 0;
  ImGui::SameLine();
  if (ImGui::RadioButton(tr("Nintendo labels (Xbox A = Switch A)"), layout == 1)) layout = 1;
  if (layout != m.face_layout) { set_face_layout(m, layout); ch = true; }
  ImGui::Indent(px(30));
  desc("By position puts Switch A on the right-hand face button like a Switch. Nintendo labels match letters instead. Switching swaps the south/east and west/north bindings in place.");
  ImGui::Unindent(px(30));
  ImGui::Spacing();
  ch |= checkbox("Swap sticks", &m.swap_sticks, "The physical left stick drives the Switch right stick (camera) and the physical right stick drives the left stick (movement).");
  ImGui::Separator();
  ImGui::Spacing();
  struct S2 { const char* name; StickMap* sm; const char* id; };
  S2 sticks[2] = {{"Left stick (movement)", &m.left, "L"}, {"Right stick (camera)", &m.right, "R"}};
  RawInput raw = hub().raw();
  for (int i = 0; i < 2; ++i) {
    StickMap& sm = *sticks[i].sm;
    ImGui::PushID(sticks[i].id);
    ImGui::TextColored(theme().title, "%s", tr(sticks[i].name));
    ImGui::BeginGroup();
    ch |= slider("Dead zone (inner)", "##dz", &sm.deadzone, 0.0f, 0.6f, "%.2f", "Ignores small movements near the center, which stops drift.");
    ch |= slider("Outer dead zone", "##oz", &sm.outer_deadzone, 0.5f, 1.0f, "%.2f", "Reaches full deflection before the stick hits its edge. Lower it if your stick cannot reach 100%.");
    sm.outer_deadzone = std::max(sm.outer_deadzone, sm.deadzone + 0.05f);
    ch |= checkbox("Invert Y", &sm.invert_y, "Pushing the stick up acts as down.");
    ImGui::EndGroup();
    ImGui::SameLine();
    float rx = (sm.analog == "right") ? raw.rx : raw.lx, ry = (sm.analog == "right") ? raw.ry : raw.ly;
    if (m.swap_sticks && sm.analog != "none") { rx = sm.analog == "right" ? raw.lx : raw.rx; ry = sm.analog == "right" ? raw.ly : raw.ry; }
    if (sm.analog == "none") rx = ry = 0;
    ImGui::BeginGroup();
    stick_plot("plot", rx, ry, sm.deadzone, sm.outer_deadzone);
    ImGui::TextDisabled("%s", tr("live input"));
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::PopID();
    ImGui::Separator();
    ImGui::Spacing();
  }
  ch |= slider("ZL trigger threshold", "##zl", &m.zl_threshold, 0.05f, 1.0f, "%.2f", "How far the left analog trigger must be pulled to count as pressed.");
  ch |= slider("ZR trigger threshold", "##zr", &m.zr_threshold, 0.05f, 1.0f, "%.2f", "The same for the right trigger.");
  ImGui::TextDisabled(tr("Live triggers: ZL %.2f   ZR %.2f   (%s)"), raw.lt, raw.rt, raw.pad_connected ? tr("controller connected") : tr("no controller detected"));
  if (ch) apply();
  extras_controller();
}

void tab_access() {
  Mapping& m = S.edit;
  desc("Options that make the game and these menus easier to see and use. Every option starts off and applies at once.");
  ImGui::Spacing();
  ImGui::TextColored(theme().title, "%s", tr("Controls"));
  ImGui::Separator();
  ImGui::Spacing();
  bool ch = false;
  ch |= checkbox("ZL: toggle instead of hold", &m.toggle_zl, "One press holds ZL down until the next press, for players who cannot keep a trigger held. Applies to every ZL binding.");
  ch |= checkbox("ZR: toggle instead of hold", &m.toggle_zr, "The same for ZR, the camera shutter in the game.");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(tr("Other buttons that toggle"));
  ImGui::SameLine(px(250));
  for (int b : {kA, kB, kX, kY, kL, kR}) {
    bool on = (m.toggle_buttons >> b) & 1u;
    ImGui::PushID(b);
    if (ImGui::Checkbox(button_name(b), &on)) { if (on) m.toggle_buttons |= 1u << b; else m.toggle_buttons &= ~(1u << b); ch = true; }
    ImGui::PopID();
    ImGui::SameLine();
  }
  ImGui::NewLine();
  ImGui::Indent(px(30));
  desc("A press holds the button down until the next press, for any button the game wants held (zoom, scan, and so on). SNAP_ACCESS_TOGGLE=ZL,Y does the same from a script.");
  ImGui::Unindent(px(30));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(tr("Menu size (UI scale)"));
  ImGui::SameLine(px(250));
  ImGui::SetNextItemWidth(px(340));
  ImGui::SliderFloat("##uiscale", &S.ui_scale_edit, 0.75f, 2.5f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemDeactivatedAfterEdit()) { m.ui_scale = S.ui_scale_edit; ch = true; }
  ImGui::Indent(px(30));
  desc("Scales text and buttons in this menu. The change applies when you release the slider. The menu also grows with the window on larger screens.");
  ImGui::Unindent(px(30));
  if (ch) apply();
  access_rows();
}

void tab_test() {
  const Theme& t = theme();
  desc("What the game would read from npad 0 right now, after your bindings. The game itself sees no input while this menu is open.");
  ImGui::Spacing();
  PadSample p = hub().preview();
  RawInput raw = hub().raw();
  ImGui::Text(tr("Controller: %s    Keyboard focus: %s"), raw.pad_connected ? tr("connected") : tr("none detected"), raw.focused ? tr("yes") : tr("no"));
  ImGui::Spacing();
  ImGui::TextColored(t.title, "%s", tr("Buttons"));
  static const int order[] = {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kUp, kDown, kLeft, kRight, kStickL, kStickR};
  int n = 0;
  for (int b : order) {
    bool on = (p.buttons >> b) & 1;
    std::string label = std::string(on ? "(*) " : "( ) ") + button_name(b);
    ImVec2 sz(px(110), 0);
    ImGui::PushStyleColor(ImGuiCol_Button, on ? t.accent : t.slot);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? t.accent : t.slot);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? t.accent : t.slot);
    ImGui::PushID(b);
    ImGui::Button(label.c_str(), sz);
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    if (++n % 8) ImGui::SameLine();
  }
  ImGui::Spacing();
  ImGui::TextColored(t.title, "%s", tr("Sticks"));
  ImGui::BeginGroup();
  stick_plot("l", p.lx / 32767.0f, p.ly / 32767.0f, 0, 1);
  ImGui::Text("L  %6d %6d", p.lx, p.ly);
  ImGui::EndGroup();
  ImGui::SameLine(0, px(40));
  ImGui::BeginGroup();
  stick_plot("r", p.rx / 32767.0f, p.ry / 32767.0f, 0, 1);
  ImGui::Text("R  %6d %6d", p.rx, p.ry);
  ImGui::EndGroup();
  ImGui::SameLine(0, px(40));
  ImGui::BeginGroup();
  ImGui::TextUnformatted(tr("Raw triggers"));
  ImGui::ProgressBar(raw.lt, ImVec2(px(180), 0), "LT");
  ImGui::ProgressBar(raw.rt, ImVec2(px(180), 0), "RT");
  ImGui::Text(tr("Raw mouse speed: %.0f, %.0f px/s"), raw.mouse_vx, raw.mouse_vy);
  ImGui::EndGroup();
  ImGui::TextDisabled(tr("Raw buttons held: %d    keys held: %d    mouse buttons: %d"), (int)raw.pad.count(), (int)raw.keys.count(), (int)__builtin_popcount(raw.mouse_buttons));
}

}  // namespace

// ---- public ----------------------------------------------------------------------------------------------------------
void menu_opened() {
  options_menu_opened();
  S.edit = hub().mapping();
  S.ui_scale_edit = S.edit.ui_scale;
  S.rb.active = false;
  S.reset_dev = -1;
  S.note.clear();
}

void menu_closed() {
  finish_rebind();
  if (S.dirty) { hub().save(); S.dirty = false; }
}

void menu_tick_save() {
  if (S.dirty && now_s() - S.dirty_t > 0.5) {
    S.dirty = false;
    std::string err;
    if (hub().save(&err)) S.saved_flash_until = now_s() + 2.0;
  }
}

void menu_select_tab(const char* name) { S.want_tab = name; }
const char* menu_current_tab() { return kTabs[std::clamp(S.cur_tab, 0, kTabCount - 1)]; }
static std::atomic<bool> g_page_cursor{false};
static char g_nav_dbg[200];
bool menu_page_cursor() { return g_page_cursor.load(); }
const char* menu_nav_debug() { return g_nav_dbg; }
bool menu_rebinding() { return S.rb.active; }
bool menu_reset_binds(int dev) {   // test hook: dev 0..2 = one device, 3 = all
  if (dev < 0 || dev > (int)kDevCount) return false;
  do_reset(dev);
  return true;
}
bool menu_begin_rebind(int action, Device dev, int slot) {
  if (action < 0 || action >= kActionCount) return false;
  begin_rebind(action, dev, slot);
  return true;
}

void draw_menu(ImVec2 size, bool* want_close) {
  // opened just now? (draw_menu only runs while the menu is open, so a gap means it was closed)
  static double last_draw = -10.0;
  static bool tab_restored = false;
  const bool just_opened = ImGui::GetTime() - last_draw > 0.3;
  last_draw = ImGui::GetTime();
  if (just_opened) skin_open_started();
  if (!tab_restored) {   // the page the player used last (settings key menu_tab); a scripted test may override it below
    tab_restored = true;
    const std::string last = hle::settings::get("menu_tab", "");
    for (int i = 0; i < kTabCount; ++i) if (last == kTabs[i] && i < tab_count()) S.cur_tab = i;
  }
  SkinScope skin_scope;   // the game-style dark palette for everything the menu draws; the picker and shader screens keep the light theme
  const SkinPal& pal = skin_pal();
  const Theme& t = theme();
  bool esc_used = false;
  const bool was_rebinding = S.rb.active;
  resolve_rebind(&esc_used);
  if (!S.rb.active && !was_rebinding && !esc_used && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) *want_close = true;

  // controller: LB/RB switch pages, B closes (the same as Escape)
  if (!S.rb.active && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive()) {
    const ImGuiIO& kio = ImGui::GetIO();   // keyboard: Q / E (the game's L / R keys), PageUp / PageDown or Ctrl+Shift+Tab / Ctrl+Tab switch pages too (2026-11-16: no controller needed)
    const bool lb = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) || (kio.KeyCtrl && kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
    const bool rb = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) || (kio.KeyCtrl && !kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
    if (lb) S.want_tab = kTabs[(S.cur_tab + tab_count() - 1) % tab_count()];
    if (rb) S.want_tab = kTabs[(S.cur_tab + 1) % tab_count()];
    if (lb || rb) skin_cursor_first_row();   // the cursor lands on the new page's first row
    if (!esc_used && !was_rebinding && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) *want_close = true;
  }
  if (S.want_tab) {   // L / R, a click on a tab, or the scripted test
    for (int i = 0; i < kTabCount; ++i) if (std::string(kTabs[i]) == S.want_tab) S.cur_tab = i;
    S.want_tab = nullptr;
  }
  if (S.cur_tab < 0 || S.cur_tab >= tab_count()) S.cur_tab = 0;

  const float open_f = skin_open_factor();
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, open_f);
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), size, skin_col(pal.backdrop));
  const float scale = ImGui::GetFontSize() / t.font_px;
  const float w = std::min(size.x * t.panel_width, t.panel_max_w * scale);
  const float h = std::min(size.y * t.panel_height, 1100.0f * scale);
  const ImVec2 wpos((size.x - w) * 0.5f, (size.y - h) * 0.5f + (1.0f - open_f) * px(18));   // slides up a little as it fades in
  style_shadow(ImGui::GetBackgroundDrawList(), wpos, ImVec2(w, h), t.window_round * scale);
  ImGui::SetNextWindowPos(wpos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
  const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
  if (!ImGui::Begin("Controls##snapmenu", nullptr, wf)) { ImGui::End(); ImGui::PopStyleVar(); return; }
  skin_cursor_apply();
  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  const float round = ImGui::GetStyle().WindowRounding;
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), skin_col(pal.panel), round);
    dl->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), skin_col(pal.panel_edge), round, 0, std::max(1.0f, px(1.5f)));
  }

  // page tabs: the game's Settings strip (L and R tags at the ends)
  {
    const char* names[kTabCount];
    const int n = tab_count();
    for (int i = 0; i < n; ++i) names[i] = tr(kTabs[i]);   // drawn text only; kTabs stay the English identities
    const int clicked = skin_tabs(names, n, S.cur_tab, ImGui::GetContentRegionAvail().x);
    if (clicked >= 0) S.cur_tab = clicked;
    ImGui::Dummy(ImVec2(0, px(10)));
  }
  static int drawn_tab = -1;
  if (just_opened) drawn_tab = -1;
  const bool page_changed = S.cur_tab != drawn_tab;
  if (page_changed && drawn_tab >= 0) hle::settings::set("menu_tab", kTabs[S.cur_tab]);
  drawn_tab = S.cur_tab;

  // notice from loading or saving the controls file
  const uint64_t serial = hub().message_serial();
  const std::string msg = hub().message();
  if (!msg.empty() && serial != S.dismissed_serial) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.toast_bg);
    float mh = ImGui::GetTextLineHeightWithSpacing() * 2.6f;
    if (ImGui::BeginChild("notice", ImVec2(0, mh), ImGuiChildFlags_Borders)) {
      ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - px(110));
      ImGui::TextColored(msg.find("invalid") != std::string::npos || msg.find("Could not") != std::string::npos ? t.error : t.good, "%s", tr(msg));
      ImGui::PopTextWrapPos();
      ImGui::SameLine(ImGui::GetWindowWidth() - px(110));
      if (ImGui::Button(tr("Dismiss"))) S.dismissed_serial = serial;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }

  options_restart_bar();   // "N changes need a restart" with the Apply and restart button

  const float bar_h = px(138);   // the description bar at the bottom edge of the panel (room for four readable lines)
  const float footer_h = bar_h - ImGui::GetStyle().WindowPadding.y + px(6);
  const std::string page = kTabs[S.cur_tab];
  if (page == "Bindings") tab_bindings(footer_h + ImGui::GetTextLineHeightWithSpacing() * 2.4f);
  else if (ImGui::BeginChild("p", ImVec2(0, -footer_h), ImGuiChildFlags_NavFlattened)) {   // flattened: the rows are the d-pad's targets, not a scroll box to enter with A
    if (page == "Mouse") tab_mouse();
    else if (page == "Controller") tab_controller();
    else if (page == "Accessibility") tab_access();
    else if (page == "Audio") tab_audio();
    else if (page == "Graphics") tab_graphics();
    else if (page == "Display") tab_display();
    else if (page == "Photos") tab_photos();
    else if (page == "Mods") tab_mods();
    else if (page == "Saves") tab_saves();
    else if (page == "Save state") tab_savestate();
    else if (page == "Cheats") tab_cheats();
    else if (page == "Benchmark") tab_benchmark();
    else if (page == "Test input") tab_test();
    else if (page == "Tuning" && dev_on()) tab_tune();   // SNAP_DEV=1 only (ui_tune.cpp)
  }
  if (page != "Bindings") {
    ImGuiContext& gc = *GImGui;
    g_page_cursor = gc.NavId != 0 && gc.NavId != ImGui::GetCurrentWindow()->ChildId && gc.NavWindow && gc.NavWindow->RootWindow == ImGui::GetCurrentWindow()->RootWindow;   // the pad / keyboard cursor is on an item of the page
    std::snprintf(g_nav_dbg, sizeof g_nav_dbg, "navid=%08x navwin=%s vis=%d childid=%08x childwin=%08x childflags=%x", (unsigned)gc.NavId, gc.NavWindow ? gc.NavWindow->Name : "-", (int)gc.NavCursorVisible,
                  (unsigned)ImGui::GetCurrentWindow()->ChildId, (unsigned)ImGui::GetCurrentWindow()->ID, (unsigned)ImGui::GetCurrentWindow()->ChildFlags);
    ImGui::EndChild();
  } else {
    g_page_cursor = false;
  }

  confirm_reset();

  // bottom bar: the description of the focused row (or a hint), the Back tag and the save status
  {
    const double now = now_s();
    std::string status;
    bool status_good = false;
    if (now < S.note_until) { status = S.note; status_good = true; }
    else if (S.dirty) status = tr("Saving...");
    else if (now < S.saved_flash_until) { status = tr("Saved"); status_good = true; }
    else status = tr("Saved automatically");
    ImGui::PushClipRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), false);
    if (!skin_help_pending()) skin_help_set(tr("Game input is paused while this menu is open. F1 or hold Back+Start closes it."), hub().config_path(), false);
    const ImVec2 bmin(wp.x, wp.y + ws.y - bar_h), bmax(wp.x + ws.x, wp.y + ws.y);
    skin_help_draw(bmin, ws.x, bar_h);
    if (skin_bar_prompts(bmin, bmax, true, status.c_str(), status_good)) *want_close = true;
    ImGui::PopClipRect();
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

bool draw_toast(ImVec2 size, double now) {
  (void)now;
  const std::string msg = hub().message();
  if (msg.empty()) return false;
  const Theme& t = theme();
  const float scale = ImGui::GetFontSize() / t.font_px;
  const float w = std::min(size.x * 0.9f, t.toast_w * scale * 1.6f);
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y - 24.0f * scale), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, t.toast_bg);
  const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##snaptoast", nullptr, wf)) {
    bool bad = msg.find("invalid") != std::string::npos || msg.find("Could not") != std::string::npos;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(bad ? t.error : t.good, tr("Controls: %s"), tr(msg));
    ImGui::TextDisabled("%s", tr("Press F1 to open the controls menu."));
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  return true;
}

}  // namespace snap::ui
