// SPDX-License-Identifier: MIT
// F1 menu extras backed by the per-profile settings file (hle/settings.h): gyro aim and rumble (Controller tab, live) and the photo gallery
// (inside the Photos page; opens the local gallery server in the browser). Graphics options moved to ui_options.cpp.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "hle/input_core.h"
#include "hle/profiles.h"
#include "hle/settings.h"
#include "ui_host.h"
#include "ui_internal.h"
#include "ui_i18n.h"

namespace snap::ui {

namespace {

namespace st = hle::settings;

std::atomic<int> g_gallery_state{0};   // 0 idle, 1 starting, 2 opened, 3 failed
std::atomic<unsigned> g_gallery_port{0};   // port the server we started reported (8777, or whatever the OS picked)

bool port_open(unsigned short port) {
  return host::tcp_port_open("127.0.0.1", port);
}

// Project root: the folder holding tools-src\gallery (the game runs from the project root; the exe sits in build\play or build\snap).
std::filesystem::path find_project_root() {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<fs::path> starts = {fs::current_path(ec)};
  if (const std::string exe_dir = host::exe_dir(); !exe_dir.empty()) starts.push_back(fs::u8path(exe_dir));
  if (const char* r = std::getenv("SNAP_PLAY_ROOT")) starts.insert(starts.begin(), fs::u8path(r));
  for (fs::path p : starts) {
    for (int i = 0; i < 5 && !p.empty(); ++i, p = p.parent_path())
      if (fs::exists(p / "tools-src" / "gallery" / "serve.py", ec)) return p;
  }
  return {};
}

std::string gallery_profile_id() {
  const hle::profiles::Active& a = hle::profiles::active();
  return a.on && !a.id.empty() ? a.id : "default";
}

// Starts tools-src\gallery\serve.py (extracts the photos, serves http://127.0.0.1:<port>/) unless the server this game started earlier still answers, then opens
// the browser at the port the server reports through --port-file (it tries 8777, then lets the OS pick; 8765 belongs to other software on some PCs).
void open_gallery_async() {
  if (g_gallery_state.exchange(1) == 1) return;
  std::string prof = gallery_profile_id();
  std::thread([prof] {
    namespace fs = std::filesystem;
    unsigned port = g_gallery_port.load();
    bool ok = port && port_open((unsigned short)port);
    if (!ok) {
      port = 0;
      fs::path root = find_project_root();
      if (root.empty()) { g_gallery_state = 3; return; }
      std::error_code ec;
      fs::path pf = fs::temp_directory_path(ec) / ("snap_gallery_port_" + std::to_string(host::current_process_id()) + ".txt");
      fs::remove(pf, ec);
      auto to_u8 = [](const fs::path& path) { const auto u = path.u8string(); return std::string(reinterpret_cast<const char*>(u.data()), u.size()); };
      if (!host::launch_gallery_server(to_u8(root), prof, to_u8(pf))) {
        g_gallery_state = 3;
        return;
      }
      for (int i = 0; i < 240 && !ok; ++i) {   // extraction runs first, then the port file appears
        host::sleep_ms(500);
        std::ifstream f(pf);
        unsigned p = 0;
        if (f >> p && p > 0 && p < 65536 && port_open((unsigned short)p)) { port = p; ok = true; }
      }
      fs::remove(pf, ec);
    }
    if (ok) {
      g_gallery_port = port;
      std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
      host::open_url(url);
      g_gallery_state = 2;
    } else {
      g_gallery_state = 3;
    }
  }).detach();
}

}  // namespace

void extras_controller() {
  const Theme& t = theme();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(t.title, "%s", tr("Gyro aim"));
  ImGui::TextWrapped("%s", tr("Tilting a controller with a gyroscope (DualSense, Switch Pro, Joy-Con) moves the camera, added to the right stick. Off by default."));
  bool on = st::get_bool("gyro_on", false);
  if (ImGui::Checkbox(tr("Gyro aim"), &on)) st::set_bool("gyro_on", on);
  ImGui::BeginDisabled(!on);
  float sens = st::get_float("gyro_sens", 1.0f);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
  if (ImGui::SliderFloat(tr("Gyro sensitivity"), &sens, 0.1f, 4.0f, "%.2f")) st::set_float("gyro_sens", sens);
  bool ix = st::get_bool("gyro_invert_x", false), iy = st::get_bool("gyro_invert_y", false);
  if (ImGui::Checkbox(tr("Invert gyro X"), &ix)) st::set_bool("gyro_invert_x", ix);
  ImGui::SameLine();
  if (ImGui::Checkbox(tr("Invert gyro Y"), &iy)) st::set_bool("gyro_invert_y", iy);
  float gsm = st::get_float("gyro_smooth", 0.0f);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
  if (ImGui::SliderFloat(tr("Gyro smoothing"), &gsm, 0.0f, 1.0f, gsm <= 0.001f ? tr("Off") : "%.2f")) st::set_float("gyro_smooth", gsm);
  float gdz = st::get_float("gyro_dead", 1.5f);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
  if (ImGui::SliderFloat(tr("Gyro dead zone (deg/s)"), &gdz, 0.0f, 6.0f, "%.1f")) st::set_float("gyro_dead", gdz);
  bool gavg = st::get_bool("gyro_avg", false);
  if (ImGui::Checkbox(tr("Average gyro samples between frames"), &gavg)) st::set_bool("gyro_avg", gavg);
  ImGui::EndDisabled();
  hle::input::RawInput r = hle::input::hub().raw();
  if (r.gyro_present) ImGui::TextColored(t.text_dim, tr("Gyro: %.0f, %.0f deg/s"), r.gyro_x, r.gyro_y);
  else ImGui::TextColored(t.text_dim, "%s", tr("Gyro: none reported (turn gyro aim on with a controller that has one)"));
  ImGui::TextColored(t.text_dim, "%s", tr("Applies at once."));

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(t.title, "%s", tr("Aim feel"));
  {
    static const char* const kCurves[] = {"linear", "classic", "precise"};
    static const char* const kCurveLabels[] = {"Linear (default)", "Classic", "Precise"};
    int cur = hle::input::stick_curve_from_name(st::get("stick_curve", "linear"));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    if (ImGui::BeginCombo(tr("Stick response"), tr(kCurveLabels[cur]))) {
      for (int i = 0; i < 3; ++i) if (ImGui::Selectable(tr(kCurveLabels[i]), i == cur)) st::set("stick_curve", kCurves[i]);
      ImGui::EndCombo();
    }
    float dz = st::get_float("stick_deadzone", -1.0f);
    bool follow = dz < 0.0f;
    if (ImGui::Checkbox(tr("Use the dead zone from the controls menu"), &follow)) st::set_float("stick_deadzone", follow ? -1.0f : 0.15f);
    if (!follow) {
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
      if (ImGui::SliderFloat(tr("Stick dead zone"), &dz, 0.0f, 0.6f, "%.2f")) st::set_float("stick_deadzone", dz);
    }
    bool raw = st::get_bool("raw_mouse", false);
    if (ImGui::Checkbox(tr("Raw mouse input"), &raw)) st::set_bool("raw_mouse", raw);
    float mw = st::get_float("mouse_window_ms", 40.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    if (ImGui::SliderFloat(tr("Mouse response window (ms)"), &mw, 8.0f, 80.0f, "%.0f")) st::set_float("mouse_window_ms", mw);
    ImGui::TextColored(t.text_dim, "%s", tr("Defaults match earlier versions. Applies at once."));
  }

  ImGui::Spacing();
  ImGui::TextColored(t.title, "%s", tr("Rumble"));
  bool ron = st::get_bool("rumble_on", true);
  if (ImGui::Checkbox(tr("Controller rumble"), &ron)) st::set_bool("rumble_on", ron);
  ImGui::BeginDisabled(!ron);
  float rs = st::get_float("rumble_strength", 1.0f);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
  if (ImGui::SliderFloat(tr("Rumble strength"), &rs, 0.0f, 1.5f, "%.2f")) st::set_float("rumble_strength", rs);
  ImGui::EndDisabled();
  ImGui::TextColored(t.text_dim, "%s", tr("Rumble reaches DualSense, Xbox and Switch Pro pads. Applies at once."));
}

void gallery_section() {
  const Theme& t = theme();
  ImGui::TextColored(t.title, "%s", tr("Photo gallery"));
  ImGui::TextWrapped(tr("Opens your saved photos for the running profile (\"%s\") in your web browser, from a small server on this PC (127.0.0.1). Nothing leaves the machine."),
                     hle::profiles::active().on ? hle::profiles::active().name.c_str() : "default");
  ImGui::Spacing();
  const int s = g_gallery_state.load();
  ImGui::BeginDisabled(s == 1);
  if (ImGui::Button(tr("Open gallery in browser"))) open_gallery_async();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (s == 1) ImGui::TextColored(t.text_dim, "%s", tr("Reading photos from the save..."));
  else if (s == 2) ImGui::TextColored(t.good, "%s", tr("Opened. Use it again after taking new photos: it re-reads the save."));
  else if (s == 3) ImGui::TextColored(t.error, "%s", tr("Could not start the gallery server (needs tools-src\\gallery and Python)."));
}

}  // namespace snap::ui
