// SPDX-License-Identifier: MIT
#include "input_core.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "btn_icons.h"
#include "freecam.h"
#include "hle.h"

extern "C" void snap_audio_log_checks(void);   // runtime/audio/audio_out.cpp: the "[audio] checks:" counter line, printed before a route ends the run
extern "C" void snap_audio_log_checks_none(void) {}   // test builds without the audio layer link this instead
#pragma comment(linker, "/alternatename:snap_audio_log_checks=snap_audio_log_checks_none")
namespace nvn { extern int g_presentCount; extern int g_fdBurst; }   // runtime/nvn/nvn_tex_vk.cpp: frame clock for '@f' script times; nvn_vk.cpp: NVN_FRAME_DUMP burst (script token shot=<n>)

namespace hle::input {
std::atomic<uint64_t> g_inject_buttons{0};   // NpadButton bits a host mod holds down (see Hub::poll); 0 = none
std::atomic<int64_t> g_inject_until_ns{0};   // steady_clock ns until which g_inject_buttons is held
std::atomic<bool> g_shot_req{false};          // screenshot key (consume_screenshot)
bool consume_screenshot() { return g_shot_req.exchange(false); }
extern "C" void snap_state_request(int op, const char* name);   // runtime/os/os_state.cpp: queue a save (1) or load (2); a worker thread does it
extern "C" int snap_state_idle();
extern "C" int snap_state_count(const char* kind);   // "saves..." / "loads..." (ok) or "done..." (ok or refused) finished since the process started
// test builds without the OS layer (tests/hle) link these instead
extern "C" void snap_state_request_none(int, const char*) {}
extern "C" int snap_state_idle_none() { return 1; }
extern "C" int snap_state_count_none(const char*) { return 0; }
#pragma comment(linker, "/alternatename:snap_state_request=snap_state_request_none")
#pragma comment(linker, "/alternatename:snap_state_idle=snap_state_idle_none")
#pragma comment(linker, "/alternatename:snap_state_count=snap_state_count_none")
// Route aim tool (host/mods/aim.cpp, developer tool, needs SNAP_AIM=1): the `aim`, `shoot` and `wait_pose` route lines. Test builds
// and runs without the tool get the stand-ins: no target, always "ready", no stick.
extern "C" void snap_aim_set(const char* spec);               // "<target> [tol=<f>] [range=<m>]" or "off"
extern "C" int snap_aim_ready(char* why, int n);              // the target sits on the reticle (1 when there is no target)
extern "C" int snap_aim_stick(float* rx, float* ry);          // 1 = hold the right stick there this poll, 2 = and hold ZL (zoom)
extern "C" void snap_aim_shot(void);                          // a `shoot` line fired: log target, offset, distance, pose
extern "C" int snap_aim_pose(const char* text, char* why, int n);   // the target's current motion event name contains text
extern "C" int snap_aim_act(const char* text, char* why, int n);    // the viewfinder is locked on an actor playing that event
extern "C" int snap_aim_act_none(const char*, char* why, int n) { if (why && n > 0) std::snprintf(why, (size_t)n, "aim tool off"); return 0; }
#pragma comment(linker, "/alternatename:snap_aim_act=snap_aim_act_none")
extern "C" void snap_aim_set_none(const char*) {}
extern "C" int snap_aim_ready_none(char* why, int n) { if (why && n > 0) std::snprintf(why, (size_t)n, "aim tool off"); return 1; }
extern "C" int snap_aim_stick_none(float*, float*) { return 0; }
extern "C" void snap_aim_shot_none(void) {}
extern "C" int snap_aim_pose_none(const char*, char* why, int n) { if (why && n > 0) std::snprintf(why, (size_t)n, "aim tool off"); return 1; }
#pragma comment(linker, "/alternatename:snap_aim_set=snap_aim_set_none")
#pragma comment(linker, "/alternatename:snap_aim_ready=snap_aim_ready_none")
#pragma comment(linker, "/alternatename:snap_aim_stick=snap_aim_stick_none")
#pragma comment(linker, "/alternatename:snap_aim_shot=snap_aim_shot_none")
#pragma comment(linker, "/alternatename:snap_aim_pose=snap_aim_pose_none")
extern "C" void snap_import_counts_dump(const char* tag);   // host/loader.cpp (debug, needs SNAP_IMPORT_COUNT=1): imports called since the last dump
extern "C" void snap_import_counts_dump_none(const char*) {}
#pragma comment(linker, "/alternatename:snap_import_counts_dump=snap_import_counts_dump_none")

// ---------------------------------------------------------------------------------------------------------------
// names
static const char* kButtonNames[kButtonCount] = {
    "A", "B", "X", "Y", "StickL", "StickR", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down",
    "StickLLeft", "StickLUp", "StickLRight", "StickLDown", "StickRLeft", "StickRUp", "StickRRight", "StickRDown",
    "LeftSL", "LeftSR", "RightSL", "RightSR"};
const char* button_name(int b) { return b >= 0 && b < kButtonCount ? kButtonNames[b] : "?"; }
int button_from_name(const std::string& n) {
  for (int i = 0; i < kButtonCount; ++i) if (n == kButtonNames[i]) return i;
  return -1;
}

uint32_t parse_toggle_list(const std::string& list) {
  uint32_t m = 0;
  std::string tok;
  for (size_t i = 0; i <= list.size(); ++i) {
    const char c = i < list.size() ? list[i] : ',';
    if (c == ',' || c == ' ' || c == ';' || c == '+') {
      std::string u;
      for (char ch : tok) u += (char)std::toupper((unsigned char)ch);
      tok.clear();
      for (const char* n : {"A", "B", "X", "Y", "L", "R", "ZL", "ZR"})
        if (u == n) m |= 1u << button_from_name(n);
    } else tok += c;
  }
  return m;
}
uint32_t toggle_mask_effective(const Mapping& m) {
  static const uint32_t env = [] { const char* e = std::getenv("SNAP_ACCESS_TOGGLE"); return e && *e ? parse_toggle_list(e) : 0u; }();
  uint32_t r = m.toggle_buttons | env;
  if (m.toggle_zl) r |= 1u << kZL;
  if (m.toggle_zr) r |= 1u << kZR;
  return r;
}

static const char* kPadNames[kPadCount] = {"south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick",
                                           "lshoulder", "rshoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
                                           "ltrigger", "rtrigger", "lstick_left", "lstick_right", "lstick_up", "lstick_down",
                                           "rstick_left", "rstick_right", "rstick_up", "rstick_down"};
static_assert(sizeof(kPadNames) / sizeof(kPadNames[0]) == kPadCount, "pad names");
static const char* kMouseNames[kMouseCount] = {"left", "right", "middle", "x1", "x2", "wheel_up", "wheel_down"};
const char* mouse_name(int b) { return b >= 0 && b < kMouseCount ? kMouseNames[b] : "?"; }
const char* pad_button_name(int b) { return b >= 0 && b < kPadCount ? kPadNames[b] : "?"; }
int pad_button_from_name(const std::string& n) {
  for (int i = 0; i < kPadCount; ++i) if (n == kPadNames[i]) return i;
  return -1;
}

static std::vector<std::string> make_key_names() {
  std::vector<std::string> k(1, "");
  for (char c = 'A'; c <= 'Z'; ++c) k.push_back(std::string(1, c));
  for (char c = '0'; c <= '9'; ++c) k.push_back(std::string(1, c));
  for (int i = 1; i <= 12; ++i) k.push_back("F" + std::to_string(i));
  for (const char* s : {"Up", "Down", "Left", "Right", "Space", "Enter", "Escape", "Tab", "Backspace", "LShift", "RShift",
                        "LCtrl", "RCtrl", "LAlt", "RAlt", "Comma", "Period", "Slash", "Semicolon", "Quote", "LBracket",
                        "RBracket", "Minus", "Equals", "Backslash", "Grave", "Insert", "Delete", "Home", "End", "PageUp",
                        "PageDown"})
    k.push_back(s);
  for (int i = 0; i <= 9; ++i) k.push_back("Numpad" + std::to_string(i));
  return k;
}
static const std::vector<std::string>& key_names() {
  static const std::vector<std::string> k = make_key_names();
  return k;
}
const char* key_name(int k) { return k > 0 && k < (int)key_names().size() ? key_names()[(size_t)k].c_str() : ""; }
static std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
int key_from_name(const std::string& n) {
  std::string l = lower(n);
  for (size_t i = 1; i < key_names().size(); ++i) if (lower(key_names()[i]) == l) return (int)i;
  return 0;
}
int key_from_ascii(char ch) {
  if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
  if (ch >= 'A' && ch <= 'Z') return 1 + (ch - 'A');
  if (ch >= '0' && ch <= '9') return 27 + (ch - '0');
  return 0;
}

bool parse_source(const std::string& s, Source* out) {
  size_t c = s.find(':');
  if (c == std::string::npos) return false;
  std::string kind = lower(s.substr(0, c)), name = s.substr(c + 1);
  if (kind == "pad") {
    int id = pad_button_from_name(lower(name));
    if (id < 0) return false;
    out->kind = Source::Pad; out->id = id;
    return true;
  }
  if (kind == "key") {
    int id = key_from_name(name);
    if (!id) return false;
    out->kind = Source::KeyboardKey; out->id = id;
    return true;
  }
  if (kind == "mouse") {
    std::string l = lower(name);
    int id = -1;
    for (int i = 0; i < kMouseCount; ++i) if (l == kMouseNames[i]) id = i;
    if (id < 0) return false;
    out->kind = Source::Mouse; out->id = id;
    return true;
  }
  return false;
}
std::string source_to_string(const Source& s) {
  switch (s.kind) {
    case Source::Pad: return std::string("pad:") + pad_button_name(s.id);
    case Source::KeyboardKey: return std::string("key:") + key_name(s.id);
    case Source::Mouse: return std::string("mouse:") + mouse_name(s.id);
    default: return "";
  }
}

const char* device_name(Device d) { return d == kDevKeyboard ? "Keyboard" : d == kDevMouse ? "Mouse" : "Controller"; }
Device device_of(const Source& s) { return s.kind == Source::Pad ? kDevController : s.kind == Source::Mouse ? kDevMouse : kDevKeyboard; }

std::string source_label(const Source& s) {
  switch (s.kind) {
    case Source::KeyboardKey: {
      std::string n = key_name(s.id);
      static const char* const from[] = {"LShift", "RShift", "LCtrl", "RCtrl", "LAlt", "RAlt", "PageUp", "PageDown", "LBracket", "RBracket",
                                         "Backslash", "Semicolon", "Comma", "Period", "Slash", "Quote", "Minus", "Equals", "Grave"};
      static const char* const to[] = {"Left Shift", "Right Shift", "Left Ctrl", "Right Ctrl", "Left Alt", "Right Alt", "Page Up", "Page Down", "[", "]",
                                       "\\", ";", ",", ".", "/", "'", "-", "=", "`"};
      for (size_t i = 0; i < sizeof from / sizeof from[0]; ++i) if (n == from[i]) return to[i];
      if (n.rfind("Numpad", 0) == 0) return "Num " + n.substr(6);
      return n;
    }
    case Source::Mouse: {
      static const char* const t[kMouseCount] = {"Mouse Left", "Mouse Right", "Mouse Middle", "Mouse Back", "Mouse Forward", "Wheel Up", "Wheel Down"};
      return s.id >= 0 && s.id < kMouseCount ? t[s.id] : "Mouse ?";
    }
    case Source::Pad: {
      static const char* const t[kPadCount] = {"South (Xbox A)", "East (Xbox B)", "West (Xbox X)", "North (Xbox Y)", "Back / View", "Guide", "Start / Menu",
                                               "L-Stick click", "R-Stick click", "L Shoulder", "R Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
                                               "D-pad Right", "L Trigger", "R Trigger", "L-Stick Left", "L-Stick Right", "L-Stick Up", "L-Stick Down",
                                               "R-Stick Left", "R-Stick Right", "R-Stick Up", "R-Stick Down"};
      return s.id >= 0 && s.id < kPadCount ? t[s.id] : "?";
    }
    default: return "";
  }
}

const char* action_label(int a) {
  static const char* const t[kActionCount] = {"A", "B", "X", "Y", "Left stick click", "Right stick click", "L", "R", "ZL", "ZR", "Plus (+)", "Minus (-)",
                                              "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down",
                                              "Left stick up", "Left stick down", "Left stick left", "Left stick right",
                                              "Right stick up", "Right stick down", "Right stick left", "Right stick right"};
  return a >= 0 && a < kActionCount ? t[a] : "?";
}

// ---------------------------------------------------------------------------------------------------------------
// mapping
static Source S(const char* t) { Source s; parse_source(t, &s); return s; }

Mapping Mapping::defaults() {
  Mapping m;
  auto set = [&](int b, std::initializer_list<const char*> l) { for (auto* t : l) m.buttons[(size_t)b].push_back(S(t)); };
  // Nintendo layout by position: A is the east face button, B south, X north, Y west.
  set(kA, {"pad:east", "key:L", "key:Enter"});   // Enter confirms in menus; Space (ZR) and the left mouse button are the ride shutter
  set(kB, {"pad:south", "key:K", "key:1"});   // Fluffruit; back in menus (Esc too, see map_pad)
  set(kX, {"pad:north", "key:I", "key:3"});   // scan
  set(kY, {"pad:west", "key:J", "key:2"});   // Illumina Orb
  set(kStickL, {"pad:lstick"});
  set(kStickR, {"pad:rstick"});
  set(kL, {"pad:lshoulder", "key:Q"});
  set(kR, {"pad:rshoulder", "key:E"});
  set(kZL, {"pad:ltrigger", "key:LShift"});   // zoom; the right mouse button holds it on a ride (look_zl)
  set(kZR, {"pad:rtrigger", "key:Space"});
  set(kPlus, {"pad:start", "key:P", "key:Tab"});
  set(kMinus, {"pad:back", "key:Backspace"});
  set(kLeft, {"pad:dpad_left", "key:F"});
  set(kUp, {"pad:dpad_up", "key:T"});
  set(kRight, {"pad:dpad_right", "key:H"});
  set(kDown, {"pad:dpad_down", "key:G"});
  m.look_a = {S("mouse:left")};
  m.look_zl = {S("mouse:right")};
  m.left.analog = "left";
  m.left.up = {S("key:W")}; m.left.down = {S("key:S")}; m.left.left = {S("key:A")}; m.left.right = {S("key:D")};
  m.right.analog = "right";
  m.right.up = {S("key:Up")}; m.right.down = {S("key:Down")}; m.right.left = {S("key:Left")}; m.right.right = {S("key:Right")};
  return m;
}

static Json sources_json(const std::vector<Source>& v) {
  Json a = Json::array();
  for (auto& s : v) a.a.push_back(Json::string(source_to_string(s)));
  return a;
}
static Json stick_json(const StickMap& s) {
  Json o = Json::object();
  o.set("analog", Json::string(s.analog));
  o.set("deadzone", Json::number(s.deadzone));
  o.set("outer_deadzone", Json::number(s.outer_deadzone));
  o.set("invert_y", Json::boolean_(s.invert_y));
  o.set("up", sources_json(s.up));
  o.set("down", sources_json(s.down));
  o.set("left", sources_json(s.left));
  o.set("right", sources_json(s.right));
  return o;
}

Json Mapping::to_json() const {
  Json j = Json::object();
  j.set("version", Json::number(kVersion));
  j.set("defaults_rev", Json::number(kDefaultsRev));   // 2: keyboard/mouse defaults of 2026-11-16 (see from_json)
  j.set("_comment", Json::string("Sources: pad:<south|east|west|north|back|guide|start|lstick|rstick|lshoulder|rshoulder|dpad_up|dpad_down|dpad_left|dpad_right|ltrigger|rtrigger|lstick_left..rstick_down>, key:<name>, mouse:<left|right|middle|x1|x2|wheel_up|wheel_down>. Switch button names: A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down. Edited by the in-game controls menu (F1)."));
  Json c = Json::object();
  c.set("face_layout", Json::string(face_layout == 1 ? "nintendo" : "position"));
  c.set("swap_sticks", Json::boolean_(swap_sticks));
  c.set("swap_ab", Json::boolean_(swap_ab));
  c.set("zl_threshold", Json::number(zl_threshold));
  c.set("zr_threshold", Json::number(zr_threshold));
  c.set("stick_button_threshold", Json::number(stick_button_threshold));
  j.set("controller", c);
  Json mouse = Json::object();
  mouse.set("touch", Json::boolean_(mouse_touch));
  mouse.set("look", Json::boolean_(mouse_look));
  mouse.set("sensitivity", Json::number(mouse_sensitivity));
  mouse.set("sensitivity_x", Json::number(mouse_sens_x));
  mouse.set("sensitivity_y", Json::number(mouse_sens_y));
  mouse.set("invert_x", Json::boolean_(mouse_invert_x));
  mouse.set("invert_y", Json::boolean_(mouse_invert_y));
  mouse.set("smoothing", Json::number(mouse_smoothing));
  mouse.set("lock_cursor", Json::boolean_(lock_cursor));
  mouse.set("look_click_a", sources_json(look_a));
  mouse.set("look_click_zl", sources_json(look_zl));
  j.set("mouse", mouse);
  Json acc = Json::object();
  acc.set("toggle_zl", Json::boolean_(toggle_zl));
  acc.set("toggle_zr", Json::boolean_(toggle_zr));
  if (toggle_buttons) {
    Json tb = Json::array();
    for (int i = 0; i < kButtonCount; ++i) if ((toggle_buttons >> i) & 1u) tb.a.push_back(Json::string(button_name(i)));
    acc.set("toggle_buttons", tb);
  }
  acc.set("ui_scale", Json::number(ui_scale));
  j.set("accessibility", acc);
  Json b = Json::object();
  for (int i = 0; i <= kDown; ++i) b.set(button_name(i), sources_json(buttons[(size_t)i]));
  j.set("buttons", b);
  j.set("left_stick", stick_json(left));
  j.set("right_stick", stick_json(right));
  return j;
}

static bool read_sources(const Json& a, std::vector<Source>* out, std::string* err, const std::string& where) {
  if (a.type != Json::Arr) { *err = where + ": expected an array of source names"; return false; }
  out->clear();
  for (auto& e : a.a) {
    Source s;
    if (e.type != Json::Str || !parse_source(e.s, &s)) { *err = where + ": bad source \"" + e.str() + "\""; return false; }
    out->push_back(s);
  }
  return true;
}
static bool read_stick(const Json& j, StickMap* s, std::string* err, const std::string& where) {
  if (auto* v = j.get("analog")) {
    s->analog = v->str("left");
    if (s->analog != "left" && s->analog != "right" && s->analog != "none") { *err = where + ".analog must be left, right or none"; return false; }
  }
  if (auto* v = j.get("deadzone")) s->deadzone = std::clamp((float)v->num(0.15), 0.0f, 0.9f);
  if (auto* v = j.get("outer_deadzone")) s->outer_deadzone = std::clamp((float)v->num(1.0), 0.5f, 1.0f);
  if (auto* v = j.get("invert_y")) s->invert_y = v->boolean(false);
  s->outer_deadzone = std::max(s->outer_deadzone, s->deadzone + 0.05f);
  if (auto* v = j.get("up")) if (!read_sources(*v, &s->up, err, where + ".up")) return false;
  if (auto* v = j.get("down")) if (!read_sources(*v, &s->down, err, where + ".down")) return false;
  if (auto* v = j.get("left")) if (!read_sources(*v, &s->left, err, where + ".left")) return false;
  if (auto* v = j.get("right")) if (!read_sources(*v, &s->right, err, where + ".right")) return false;
  return true;
}

bool Mapping::from_json(const Json& j, std::string* err) {
  Mapping m = defaults();
  std::string e;
  if (j.type != Json::Obj) { *err = "input map must be a JSON object"; return false; }
  int version = 1;
  if (auto* v = j.get("version")) version = (int)v->num(1);
  if (version > kVersion) { *err = "format version " + std::to_string(version) + " is newer than this build understands (" + std::to_string(kVersion) + ")"; return false; }
  auto clampf = [](const Json* v, double def, float lo, float hi) { return std::clamp((float)v->num(def), lo, hi); };
  if (auto* v = j.get("trigger_threshold")) m.zl_threshold = m.zr_threshold = clampf(v, 0.5, 0.05f, 1.0f);   // version 1
  if (auto* c = j.get("controller")) {
    if (c->type != Json::Obj) { *err = "\"controller\" must be an object"; return false; }
    if (auto* v = c->get("face_layout")) {
      std::string f = v->str("position");
      if (f != "position" && f != "nintendo") { *err = "controller.face_layout must be position or nintendo"; return false; }
      m.face_layout = f == "nintendo" ? 1 : 0;
    }
    if (auto* v = c->get("swap_sticks")) m.swap_sticks = v->boolean(false);
    if (auto* v = c->get("swap_ab")) m.swap_ab = v->boolean(false);
    if (auto* v = c->get("zl_threshold")) m.zl_threshold = clampf(v, 0.5, 0.05f, 1.0f);
    if (auto* v = c->get("zr_threshold")) m.zr_threshold = clampf(v, 0.5, 0.05f, 1.0f);
    if (auto* v = c->get("stick_button_threshold")) m.stick_button_threshold = clampf(v, 0.5, 0.1f, 0.95f);
  }
  if (auto* v = j.get("mouse")) {
    if (v->type != Json::Obj) { *err = "\"mouse\" must be an object"; return false; }
    if (auto* t = v->get("touch")) m.mouse_touch = t->boolean(true);
    if (auto* t = v->get("look_click_a")) if (!read_sources(*t, &m.look_a, err, "mouse.look_click_a")) return false;
    if (auto* t = v->get("look_click_zl")) if (!read_sources(*t, &m.look_zl, err, "mouse.look_click_zl")) return false;
    if (auto* t = v->get("look")) m.mouse_look = t->boolean(true);
    if (auto* t = v->get("sensitivity")) m.mouse_sensitivity = clampf(t, 1.0, 0.05f, 10.0f);
    if (auto* t = v->get("sensitivity_x")) m.mouse_sens_x = clampf(t, 1.0, 0.05f, 10.0f);
    if (auto* t = v->get("sensitivity_y")) m.mouse_sens_y = clampf(t, 1.0, 0.05f, 10.0f);
    if (auto* t = v->get("invert_x")) m.mouse_invert_x = t->boolean(false);
    if (auto* t = v->get("invert_y")) m.mouse_invert_y = t->boolean(false);
    if (auto* t = v->get("smoothing")) m.mouse_smoothing = clampf(t, 0.0, 0.0f, 1.0f);
    if (auto* t = v->get("lock_cursor")) m.lock_cursor = t->boolean(true);
    if (auto* s = v->get("look_sensitivity")) {   // version 1: > 0 turned mouse-look on
      float old = std::max(0.0f, (float)s->num(0));
      if (old > 0) { m.mouse_look = true; m.mouse_sensitivity = 1.0f; }
    }
  }
  if (auto* v = j.get("accessibility")) {
    if (v->type != Json::Obj) { *err = "\"accessibility\" must be an object"; return false; }
    if (auto* t = v->get("toggle_zl")) m.toggle_zl = t->boolean(false);
    if (auto* t = v->get("toggle_zr")) m.toggle_zr = t->boolean(false);
    if (auto* t = v->get("toggle_buttons")) {   // ["A","Y","L"...]: other held buttons that toggle (ZL and ZR have their own switches above)
      m.toggle_buttons = 0;
      if (t->type == Json::Arr) for (auto& e : t->a) { const int b = button_from_name(e.str()); if (b >= kA && b <= kZR && b != kStickL && b != kStickR) m.toggle_buttons |= 1u << b; }
    }
    if (auto* t = v->get("ui_scale")) m.ui_scale = clampf(t, 1.0, 0.5f, 3.0f);
  }
  if (auto* b = j.get("buttons")) {
    if (b->type != Json::Obj) { *err = "\"buttons\" must be an object"; return false; }
    for (auto& kv : b->o) {
      int id = button_from_name(kv.first);
      if (id < 0 || id > kDown) { *err = "buttons: unknown Switch button \"" + kv.first + "\""; return false; }
      if (!read_sources(kv.second, &m.buttons[(size_t)id], &e, "buttons." + kv.first)) { *err = e; return false; }
    }
  }
  if (auto* v = j.get("left_stick")) if (!read_stick(*v, &m.left, &e, "left_stick")) { *err = e; return false; }
  if (auto* v = j.get("right_stick")) if (!read_stick(*v, &m.right, &e, "right_stick")) { *err = e; return false; }
  // Files saved before the keyboard/mouse defaults of 2026-11-16 (defaults_rev < 2) hold the old table in full: an action still on exactly its old
  // default takes the new one (A +Enter, B +1, X +3, Y +2, Plus Enter -> P/Tab, ZL +wheel), and mouse look (old default off) turns on.
  int rev = 1;
  if (auto* v = j.get("defaults_rev")) rev = (int)v->num(1);
  if (rev < kDefaultsRev) {
    const Mapping nd = defaults();
    struct Old { int b; std::initializer_list<const char*> l; };
    const Old olds[] = {{kA, {"pad:east", "key:L"}}, {kB, {"pad:south", "key:K"}}, {kX, {"pad:north", "key:I"}}, {kY, {"pad:west", "key:J"}},
                        {kPlus, {"pad:start", "key:Enter"}}, {kZL, {"pad:ltrigger", "key:LShift"}}};
    for (const Old& o : olds) {
      std::vector<Source> ov;
      for (auto* t : o.l) ov.push_back(S(t));
      if (m.buttons[(size_t)o.b] == ov) m.buttons[(size_t)o.b] = nd.buttons[(size_t)o.b];
    }
    m.mouse_look = true;
  }
  *this = m;
  return true;
}

// ---- action table and slot editing
std::vector<Source>& action_sources(Mapping& m, int a) {
  static std::vector<Source> dummy;
  if (a >= 0 && a <= kDown) return m.buttons[(size_t)a];
  if (a >= 16 && a < 20) { StickMap& s = m.left; return a == 16 ? s.up : a == 17 ? s.down : a == 18 ? s.left : s.right; }
  if (a >= 20 && a < 24) { StickMap& s = m.right; return a == 20 ? s.up : a == 21 ? s.down : a == 22 ? s.left : s.right; }
  dummy.clear();
  return dummy;
}
const std::vector<Source>& action_sources(const Mapping& m, int a) { return action_sources(const_cast<Mapping&>(m), a); }

int slot_count(const std::vector<Source>& v, Device d) {
  int n = 0;
  for (auto& s : v) if (device_of(s) == d) ++n;
  return n;
}
Source slot_get(const std::vector<Source>& v, Device d, int slot) {
  int n = 0;
  for (auto& s : v) if (device_of(s) == d && n++ == slot) return s;
  return Source{};
}
bool slot_set(std::vector<Source>& v, Device d, int slot, const Source& src) {
  if (src.kind == Source::None || device_of(src) != d || slot < 0 || slot >= kMaxSlotsPerDevice) return false;
  int have = slot_count(v, d);
  if (slot > have) slot = have;   // fill the first free slot
  // a duplicate in the other slot of the same device would be a silent no-op binding: drop it
  for (int o = 0; o < have; ++o) if (o != slot && slot_get(v, d, o) == src) { slot_clear(v, d, o); if (o < slot) --slot; have = slot_count(v, d); break; }
  if (slot >= have) { v.push_back(src); return true; }
  int n = 0;
  for (auto& s : v) if (device_of(s) == d && n++ == slot) { s = src; break; }
  return true;
}
void slot_clear(std::vector<Source>& v, Device d, int slot) {
  int n = 0;
  for (size_t i = 0; i < v.size(); ++i)
    if (device_of(v[i]) == d && n++ == slot) { v.erase(v.begin() + (long)i); return; }
}
std::vector<int> actions_using(const Mapping& m, const Source& src) {
  std::vector<int> out;
  for (int a = 0; a < kActionCount; ++a)
    for (auto& s : action_sources(m, a)) if (s == src) { out.push_back(a); break; }
  return out;
}
void reset_device(Mapping& m, Device d) {
  Mapping def = Mapping::defaults();
  for (int a = 0; a < kActionCount; ++a) {
    auto& v = action_sources(m, a);
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Source& s) { return device_of(s) == d; }), v.end());
    for (auto& s : action_sources(def, a)) if (device_of(s) == d) v.push_back(s);
  }
  if (d == kDevController) {
    m.zl_threshold = def.zl_threshold; m.zr_threshold = def.zr_threshold; m.stick_button_threshold = def.stick_button_threshold;
    m.swap_sticks = false; m.face_layout = 0; m.swap_ab = false;
    for (StickMap* s : {&m.left, &m.right}) { StickMap* ds = s == &m.left ? &def.left : &def.right; s->analog = ds->analog; s->deadzone = ds->deadzone; s->outer_deadzone = ds->outer_deadzone; s->invert_y = false; }
  } else if (d == kDevMouse) {
    m.mouse_touch = def.mouse_touch; m.mouse_look = def.mouse_look; m.mouse_sensitivity = def.mouse_sensitivity;
    m.mouse_sens_x = def.mouse_sens_x; m.mouse_sens_y = def.mouse_sens_y;
    m.mouse_invert_x = m.mouse_invert_y = false; m.mouse_smoothing = 0; m.lock_cursor = def.lock_cursor;
    m.look_a = def.look_a; m.look_zl = def.look_zl;
  }
}
void set_face_layout(Mapping& m, int layout) {
  layout = layout == 1 ? 1 : 0;
  if (layout == m.face_layout) return;
  m.face_layout = layout;
  auto flip = [](int id) { return id == kPadSouth ? kPadEast : id == kPadEast ? kPadSouth : id == kPadWest ? kPadNorth : id == kPadNorth ? kPadWest : id; };
  for (int a = 0; a < kActionCount; ++a)
    for (auto& s : action_sources(m, a)) if (s.kind == Source::Pad) s.id = flip(s.id);
}

bool source_down(const Source& s, const RawInput& in, float trigger_thr) {
  switch (s.kind) {
    case Source::Pad:
      switch (s.id) {
        case kPadLTrigger: return in.pad.test((size_t)s.id) || in.lt >= trigger_thr;
        case kPadRTrigger: return in.pad.test((size_t)s.id) || in.rt >= trigger_thr;
        case kPadLStickLeft: return in.lx <= -0.5f;
        case kPadLStickRight: return in.lx >= 0.5f;
        case kPadLStickUp: return in.ly >= 0.5f;
        case kPadLStickDown: return in.ly <= -0.5f;
        case kPadRStickLeft: return in.rx <= -0.5f;
        case kPadRStickRight: return in.rx >= 0.5f;
        case kPadRStickUp: return in.ry >= 0.5f;
        case kPadRStickDown: return in.ry <= -0.5f;
        default: return in.pad.test((size_t)s.id);
      }
    case Source::KeyboardKey: return in.keys.test((size_t)s.id);
    case Source::Mouse: return (in.mouse_buttons >> s.id) & 1;
    default: return false;
  }
}
static bool any_down(const std::vector<Source>& v, const RawInput& in, float thr) {
  for (auto& s : v) if (source_down(s, in, thr)) return true;
  return false;
}

PadFx& pad_fx() { static PadFx f; return f; }
bool swap_ab_effective(const Mapping& m) {
  const int o = pad_fx().swap_ab.load(std::memory_order_relaxed);
  return o >= 0 ? o != 0 : m.swap_ab;
}

// Environment overrides (test and power-user switches): SNAP_RUMBLE=0..1.5 (0 = off), SNAP_RUMBLE_LOG=1, SNAP_GYRO=0/1,
// SNAP_GYRO_SENS=<float>, SNAP_GYRO_FAKE=<yaw_dps>,<pitch_dps> (a pretend gyro on the pad, left/up positive like SDL).
struct EnvFx {
  bool rumble_set = false; float rumble = 1.0f; bool rumble_log = false;
  bool gyro_set = false, gyro = false; bool sens_set = false; float sens = 1.0f;
  bool fake = false; float fake_yaw = 0, fake_pitch = 0;
};
static const EnvFx& env_fx() {
  static const EnvFx e = [] {
    EnvFx r;
    if (const char* v = std::getenv("SNAP_RUMBLE"); v && *v) { r.rumble_set = true; r.rumble = std::clamp((float)std::atof(v), 0.0f, 1.5f); }
    if (const char* v = std::getenv("SNAP_RUMBLE_LOG"); v && *v && *v != '0') r.rumble_log = true;
    if (const char* v = std::getenv("SNAP_GYRO"); v && *v) { r.gyro_set = true; r.gyro = *v != '0'; }
    if (const char* v = std::getenv("SNAP_GYRO_SENS"); v && *v) { r.sens_set = true; r.sens = std::max(0.0f, (float)std::atof(v)); }
    if (const char* v = std::getenv("SNAP_GYRO_FAKE"); v && *v) {
      r.fake = true; r.fake_yaw = (float)std::atof(v);
      if (const char* c = std::strchr(v, ',')) r.fake_pitch = (float)std::atof(c + 1);
      if (!r.gyro_set) { r.gyro_set = true; r.gyro = true; }
    }
    return r;
  }();
  return e;
}
bool rumble_log_enabled() { return env_fx().rumble_log; }
bool gyro_aim_enabled() { return env_fx().gyro_set ? env_fx().gyro : pad_fx().gyro_on.load(); }

int stick_curve_from_name(const std::string& n) {
  if (n == "classic" || n == "1") return kCurveClassic;
  if (n == "precise" || n == "2") return kCurvePrecise;
  return kCurveLinear;
}
float apply_stick_curve(int curve, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  switch (curve) {
    case kCurveClassic: return std::pow(t, 1.5f);
    case kCurvePrecise: return std::pow(t, 2.2f);
    default: return t;
  }
}
void VelWindow::velocity(uint64_t t, uint64_t win_ns, float* vx, float* vy) {
  size_t drop = 0;
  while (drop < v.size() && t - v[drop].t > 400000000ull) ++drop;
  if (drop) v.erase(v.begin(), v.begin() + (long)drop);
  double sx = 0, sy = 0;
  for (auto& m : v) if (t - m.t <= win_ns) { sx += m.dx; sy += m.dy; }
  const double secs = (double)win_ns * 1e-9;
  *vx = secs > 0 ? (float)(sx / secs) : 0.0f;
  *vy = secs > 0 ? (float)(sy / secs) : 0.0f;
}
float ema_step(float prev, float in, float dt_s, float tau_s) {
  if (tau_s <= 1e-6f) return in;
  if (dt_s <= 0) return prev;
  const float a = 1.0f - std::exp(-dt_s / tau_s);
  return prev + (in - prev) * a;
}
static DeltaAccum g_raw_mouse;
void post_raw_mouse(int dx, int dy) { g_raw_mouse.add(dx, dy); }
DeltaAccum& raw_mouse() { return g_raw_mouse; }

static std::atomic<bool> g_has_mouse_pos{false};
static std::atomic<float> g_mouse_pos_x{0.0f};
static std::atomic<float> g_mouse_pos_y{0.0f};
static std::atomic<bool> g_mouse_in_win{false};
static std::atomic<bool> g_mouse_ui_active{false};
static std::atomic<bool> g_ui_in_ride{false};
static std::atomic<bool> g_ui_menu_open{false};
static std::atomic<bool> g_ui_focused{true};
void post_mouse_pos(float nx, float ny, bool in_window) {
  const float prev_x = g_mouse_pos_x.load(std::memory_order_relaxed);
  const float prev_y = g_mouse_pos_y.load(std::memory_order_relaxed);
  const bool had = g_has_mouse_pos.load(std::memory_order_relaxed);
  g_mouse_pos_x.store(nx, std::memory_order_relaxed);
  g_mouse_pos_y.store(ny, std::memory_order_relaxed);
  g_mouse_in_win.store(in_window, std::memory_order_relaxed);
  g_has_mouse_pos.store(true, std::memory_order_release);
  if (in_window && (!had || std::fabs(nx - prev_x) > 1e-4f || std::fabs(ny - prev_y) > 1e-4f)) {
    g_mouse_ui_active.store(true, std::memory_order_release);
  }
}
bool read_posted_mouse_pos(float* nx, float* ny, bool* in_window) {
  if (!g_has_mouse_pos.load(std::memory_order_acquire)) return false;
  if (nx) *nx = g_mouse_pos_x.load(std::memory_order_relaxed);
  if (ny) *ny = g_mouse_pos_y.load(std::memory_order_relaxed);
  if (in_window) *in_window = g_mouse_in_win.load(std::memory_order_relaxed);
  return true;
}
bool ui_pointer_1080(float* gx, float* gy) {
  if (g_ui_in_ride.load(std::memory_order_relaxed) || g_ui_menu_open.load(std::memory_order_relaxed) ||
      !g_ui_focused.load(std::memory_order_relaxed) || !g_mouse_ui_active.load(std::memory_order_acquire)) {
    return false;
  }
  float nx = 0.0f, ny = 0.0f;
  bool in_win = false;
  if (!read_posted_mouse_pos(&nx, &ny, &in_win) || !in_win) return false;
  float x = nx * 1920.0f, y = ny * 1080.0f;
  if (g_window_to_guest) g_window_to_guest(nx, ny, &x, &y);
  if (gx) *gx = std::clamp(x, 0.0f, 1920.0f);
  if (gy) *gy = std::clamp(y, 0.0f, 1080.0f);
  return true;
}

static std::atomic<bool> g_sdl3_cursor_lock{false};
static std::atomic<bool> g_sdl3_cursor_recenter{false};
// AOT Ride camera direct mouse-look state (enabled by default; SNAP_MOUSE_AOT_CAMERA=0 disables).
namespace {
struct AotMouseCamState {
  std::mutex mu;
  float dx = 0.0f;
  float dy = 0.0f;
  bool zoom = false;
};
AotMouseCamState& aot_cam_state() {
  static AotMouseCamState s;
  return s;
}
}  // namespace
bool aot_camera_enabled() {
  const char* v = std::getenv("SNAP_MOUSE_AOT_CAMERA");
  if (!v || !*v) return true;
  return std::atoi(v) != 0;
}
void post_aot_mouse_delta(float dx, float dy, bool zoom_active) {
  AotMouseCamState& s = aot_cam_state();
  std::lock_guard<std::mutex> g(s.mu);
  s.dx += dx;
  s.dy += dy;
  s.zoom = zoom_active;
}
bool take_aot_mouse_delta(float* dx, float* dy, bool* zoom_active) {
  AotMouseCamState& s = aot_cam_state();
  std::lock_guard<std::mutex> g(s.mu);
  const float x = s.dx;
  const float y = s.dy;
  const bool z = s.zoom;
  s.dx = 0.0f;
  s.dy = 0.0f;
  if (dx) *dx = x;
  if (dy) *dy = y;
  if (zoom_active) *zoom_active = z;
  return x != 0.0f || y != 0.0f;
}
void reset_aot_mouse_delta() {
  AotMouseCamState& s = aot_cam_state();
  std::lock_guard<std::mutex> g(s.mu);
  s.dx = 0.0f;
  s.dy = 0.0f;
  s.zoom = false;
}

void set_sdl3_cursor_mode(bool lock, bool recenter) {
  g_sdl3_cursor_lock.store(lock, std::memory_order_relaxed);
  g_sdl3_cursor_recenter.store(recenter, std::memory_order_relaxed);
}
void sdl3_cursor_mode(bool* lock, bool* recenter) {
  if (lock) *lock = g_sdl3_cursor_lock.load(std::memory_order_relaxed);
  if (recenter) *recenter = g_sdl3_cursor_recenter.load(std::memory_order_relaxed);
}

void gyro_to_stick(float yaw, float pitch, float sens, bool inv_x, bool inv_y, float* sx, float* sy, float dead) {
  dead = std::clamp(dead, 0.0f, kGyroFullDegPerSec - 5.0f);
  auto dz = [dead](float v) {
    float a = std::fabs(v);
    if (a <= dead) return 0.0f;
    return std::copysign(a - dead, v);   // subtract the dead zone so there is no jump at its edge
  };
  float s = std::max(0.0f, sens) / (kGyroFullDegPerSec - dead);
  float x = -dz(yaw) * s;      // turning the pad to the right (negative yaw rate) pushes the stick right
  float y = dz(pitch) * s;     // tipping the front up pushes the stick up
  *sx = std::clamp(inv_x ? -x : x, -1.0f, 1.0f);
  *sy = std::clamp(inv_y ? -y : y, -1.0f, 1.0f);
}

static void stick_value(const StickMap& sm, bool is_left, const Mapping& m, const RawInput& in, float mouse_x, float mouse_y, float* ox, float* oy) {
  float x = 0, y = 0;
  std::string analog = sm.analog;
  if (m.swap_sticks && analog != "none") analog = analog == "left" ? "right" : "left";
  (void)is_left;
  if (analog != "none") {
    float ax = analog == "left" ? in.lx : in.rx, ay = analog == "left" ? in.ly : in.ry;
    if (sm.invert_y) ay = -ay;
    float mag = std::sqrt(ax * ax + ay * ay);
    const float ovr = pad_fx().stick_deadzone.load(std::memory_order_relaxed);
    const float dzone = ovr >= 0.0f ? std::min(ovr, 0.9f) : sm.deadzone;
    const float outer = ovr >= 0.0f ? std::max(sm.outer_deadzone, dzone + 0.05f) : sm.outer_deadzone;
    if (mag > dzone) {
      float span = std::max(0.01f, outer - dzone);
      float scale = apply_stick_curve(pad_fx().stick_curve.load(std::memory_order_relaxed), std::min(1.0f, (mag - dzone) / span)) / mag;  // radial: inner dead zone, saturation at the outer one
      x = ax * scale; y = ay * scale;
    }
  }
  float thr = m.zl_threshold;
  x += (any_down(sm.right, in, thr) ? 1.f : 0.f) - (any_down(sm.left, in, thr) ? 1.f : 0.f);
  y += (any_down(sm.up, in, thr) ? 1.f : 0.f) - (any_down(sm.down, in, thr) ? 1.f : 0.f);
  x += mouse_x; y += mouse_y;
  float mag = std::sqrt(x * x + y * y);
  if (mag > 1.0f) { x /= mag; y /= mag; }  // keep the diagonal on the unit circle
  *ox = x; *oy = y;
}

PadSample map_pad(const Mapping& m, const RawInput& in) {
  PadSample out;
  RawInput eff = in;
  if (!in.focused) { eff.keys.reset(); eff.mouse_buttons = 0; }
  out.connected = in.pad_connected || in.focused;  // the keyboard is always a controller while the window has focus
  const bool swap_ab = swap_ab_effective(m);
  for (int b = 0; b <= kDown; ++b) {
    float thr = b == kZR ? m.zr_threshold : m.zl_threshold;
    const bool ab = swap_ab && (b == kA || b == kB);   // controller sources of A press B and the reverse; keyboard and mouse keep their button
    for (auto& s : m.buttons[(size_t)b]) {
      // the trigger threshold of the physical trigger, not of the Switch button it is bound to
      float t = s.kind == Source::Pad && s.id == kPadRTrigger ? m.zr_threshold : s.kind == Source::Pad && s.id == kPadLTrigger ? m.zl_threshold : thr;
      if (source_down(s, eff, t)) {
        const int to = ab && s.kind == Source::Pad ? (b == kA ? kB : kA) : b;
        out.buttons |= 1ull << to;
        if (!ab) break;
      }
    }
  }
  {   // Esc: Back (B) in menus, Plus (pause) on a ride
    static const int kEsc = key_from_name("Escape");
    if (eff.focused && kEsc > 0 && eff.keys.test((size_t)kEsc)) out.buttons |= 1ull << (eff.in_ride ? kPlus : kB);
  }
  if (eff.focused && !eff.in_ride) {   // menus: the keys that aim the camera (arrows by default) also press the D-pad, so a keyboard user can move through any list
    const std::vector<Source>* dirs[4] = {&m.right.up, &m.right.down, &m.right.left, &m.right.right};
    const int pads[4] = {kUp, kDown, kLeft, kRight};
    for (int d = 0; d < 4; ++d)
      for (const Source& s : *dirs[d]) if (s.kind == Source::KeyboardKey && source_down(s, eff, 0.5f)) { out.buttons |= 1ull << pads[d]; break; }
  }
  if (eff.focused && !eff.in_ride && m.mouse_touch) {   // menus: left click = A (+ TouchSample), right click = B (upstream v1.4.0 / QA8 behavior)
    if (eff.mouse_buttons & (1u << kMouseLeft)) out.buttons |= 1ull << kA;
    if (eff.mouse_buttons & (1u << kMouseRight)) out.buttons |= 1ull << kB;
  }
  if (m.mouse_look && eff.focused && eff.in_ride) {   // mouse-look click bindings, ride only: menus keep the touch screen / pad flow
    if (any_down(m.look_a, eff, 0.5f)) out.buttons |= 1ull << kA;
    if (any_down(m.look_zl, eff, 0.5f)) out.buttons |= 1ull << kZL;
  }
  float lx, ly, rx, ry;
  stick_value(m.left, true, m, eff, 0, 0, &lx, &ly);
  float mlx = 0, mly = 0;
  if (m.mouse_look && eff.focused && eff.in_ride && !aot_camera_enabled()) {   // mouse aim only on a ride: menus keep a free cursor (suppressed when AOT camera handles deltas directly)
    const float sx = m.mouse_sensitivity * m.mouse_sens_x;
    const float sy = m.mouse_sensitivity * m.mouse_sens_y;
    const bool has_vel = (eff.mouse_vx != 0.0f || eff.mouse_vy != 0.0f);
    const float vx = has_vel ? eff.mouse_vx : (eff.mouse_frame_dx * 60.0f);
    const float vy = has_vel ? eff.mouse_vy : (eff.mouse_frame_dy * 60.0f);
    const char* ds_env = std::getenv("SNAP_MOUSE_DUAL_STICK");
    const bool dual_stick = (ds_env && std::atoi(ds_env) != 0);
    float dual_gain = 1.0f;
    if (dual_stick) {
      const bool zl_toggle = (toggle_mask_effective(m) & (1u << kZL)) != 0;
      const bool zoom_active = zl_toggle ? eff.zoom_latched : ((out.buttons & (1ull << kZL)) != 0);
      if (zoom_active) {
        dual_gain = 2.20f;  // QA23: validated independent zoom sensitivity (configurable via SNAP_MOUSE_DUAL_ZOOM_GAIN)
        if (const char* zg = std::getenv("SNAP_MOUSE_DUAL_ZOOM_GAIN")) {
          const float parsed = (float)std::atof(zg);
          if (std::isfinite(parsed) && parsed > 0.0f) dual_gain = std::clamp(parsed, 0.25f, 4.0f);
        }
      } else {
        dual_gain = 1.70f;  // QA23: experimental normal Dual-Stick gain (configurable via SNAP_MOUSE_DUAL_GAIN)
        if (const char* dg = std::getenv("SNAP_MOUSE_DUAL_GAIN")) {
          const float parsed = (float)std::atof(dg);
          if (std::isfinite(parsed) && parsed > 0.0f) dual_gain = std::clamp(parsed, 0.25f, 3.0f);
        }
      }
    }
    const float pre_x = (vx * sx * dual_gain / (float)kMouseLookFullPxPerSec) * (m.mouse_invert_x ? -1.f : 1.f);
    const float pre_y = (-vy * sy * dual_gain / (float)kMouseLookFullPxPerSec) * (m.mouse_invert_y ? -1.f : 1.f);
    // Radial response curve: preserves slow precision (0..0.5) while extending fast proportionality up to 2400 px/s,
    // and clamps radially so movement angle is never distorted.
    const float mmag = std::sqrt(pre_x * pre_x + pre_y * pre_y);
    if (mmag > 1e-6f) {
      const float target_mag = (mmag < 0.5f)
                                   ? (0.5f * std::pow(mmag * 2.0f, 0.72f))
                                   : std::min(mmag, 1.0f);
      const float dir_x = pre_x / mmag;
      const float dir_y = pre_y / mmag;
      mlx = dir_x * target_mag;
      mly = dir_y * target_mag;
      if (dual_stick && mmag > 1.0f) {
        // Two-Stage Split Radial: overflow above Right Stick capacity goes to Left Stick along the exact same vector,
        // unless physical Left Stick or WASD is active (in which case split yields immediately).
        if (std::fabs(lx) <= 1e-4f && std::fabs(ly) <= 1e-4f) {
          const float excess = std::min(mmag - 1.0f, 1.0f);
          lx = dir_x * excess;
          ly = dir_y * excess;
        }
      }
    }
  }
  const EnvFx& ev = env_fx();
  if (eff.gyro_present && (ev.gyro_set ? ev.gyro : pad_fx().gyro_on.load())) {
    float gx, gy;
    gyro_to_stick(eff.gyro_y, eff.gyro_x, ev.sens_set ? ev.sens : pad_fx().gyro_sens.load(), pad_fx().gyro_invert_x.load(), pad_fx().gyro_invert_y.load(), &gx, &gy, pad_fx().gyro_dead.load());
    mlx += gx; mly += gy;
  }
  stick_value(m.right, false, m, eff, mlx, mly, &rx, &ry);
  auto q = [](float v) { return (int32_t)std::lround(std::clamp(v, -1.0f, 1.0f) * kStickMax); };
  out.lx = q(lx); out.ly = q(ly); out.rx = q(rx); out.ry = q(ry);
  float t = m.stick_button_threshold;
  if (lx <= -t) out.buttons |= 1ull << kStickLLeft;
  if (ly >= t) out.buttons |= 1ull << kStickLUp;
  if (lx >= t) out.buttons |= 1ull << kStickLRight;
  if (ly <= -t) out.buttons |= 1ull << kStickLDown;
  if (rx <= -t) out.buttons |= 1ull << kStickRLeft;
  if (ry >= t) out.buttons |= 1ull << kStickRUp;
  if (rx >= t) out.buttons |= 1ull << kStickRRight;
  if (ry <= -t) out.buttons |= 1ull << kStickRDown;
  return out;
}

bool (*g_window_to_guest)(float nx, float ny, float* gx, float* gy) = nullptr;

TouchSample map_touch(const Mapping& m, const RawInput& in) {
  TouchSample t;
  if (m.mouse_touch && (!m.mouse_look || !in.in_ride) && in.focused && in.mouse_in_window && (in.mouse_buttons & 1)) {
    t.count = 1;
    float gx = in.mouse_x * 1920.0f, gy = in.mouse_y * 1080.0f;   // interface pixels; the panel is 1280x720 (x 2/3)
    if (g_window_to_guest) g_window_to_guest(in.mouse_x, in.mouse_y, &gx, &gy);
    t.pt[0].x = (int32_t)std::clamp(gx * (1280.0f / 1920.0f), 0.0f, 1279.0f);
    t.pt[0].y = (int32_t)std::clamp(gy * (720.0f / 1080.0f), 0.0f, 719.0f);
  }
  return t;
}

// ---------------------------------------------------------------------------------------------------------------
// hub
static uint64_t now_ns() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Hub::Hub() : map_(Mapping::defaults()) {}

void Hub::set_backend(std::unique_ptr<Backend> b) {
  std::lock_guard<std::mutex> g(mu_);
  backend_ = std::move(b);
  last_poll_ns_ = 0;
}
void Hub::set_mapping(const Mapping& m) {
  { std::lock_guard<std::mutex> g(mu_); map_ = m; }
  icons::set_mapping(m);   // the button icons follow the current binding
}
Mapping Hub::mapping() const { std::lock_guard<std::mutex> g(mu_); return map_; }

bool write_mapping_file(const Mapping& m, const std::string& path, std::string* err) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path target = fs::u8path(path);
  if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
  std::string text;
  json_dump(m.to_json(), text);
  fs::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write " + tmp.string(); return false; }
    f << text << "\n";
    f.flush();
    if (!f) { if (err) *err = "write failed: " + tmp.string(); return false; }
  }
  fs::rename(tmp, target, ec);   // replaces an existing file in one step
  if (ec) { fs::remove(tmp, ec); if (err) *err = "cannot replace " + path; return false; }
  return true;
}

bool write_default_mapping(const std::string& path) { return write_mapping_file(Mapping::defaults(), path, nullptr); }

static bool read_mapping_file(const std::string& path, Mapping* m, std::string* err) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) { *err = path + ": cannot open"; return false; }
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  std::string e;
  if (!json_parse(ss.str(), j, e)) { *err = path + ": " + e; return false; }
  if (!m->from_json(j, &e)) { *err = path + ": " + e; return false; }
  return true;
}

bool Hub::load_mapping(const std::string& path, std::string* err) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    write_default_mapping(path);  // first run: leave an editable template
    set_mapping(Mapping::defaults());
    std::lock_guard<std::mutex> g(mu_);
    path_ = path;
    return true;
  }
  Mapping m;
  std::string e;
  if (!read_mapping_file(path, &m, &e)) { if (err) *err = e; return false; }
  set_mapping(m);
  std::lock_guard<std::mutex> g(mu_);
  path_ = path;
  return true;
}

void Hub::set_message_locked(const std::string& m) { message_ = m; ++message_serial_; }
std::string Hub::message() const { std::lock_guard<std::mutex> g(mu_); return message_; }
uint64_t Hub::message_serial() const { std::lock_guard<std::mutex> g(mu_); return message_serial_; }
std::string Hub::config_path() const { std::lock_guard<std::mutex> g(mu_); return path_; }

bool Hub::load_config(const std::string& path, const std::string& legacy) {
  namespace fs = std::filesystem;
  std::error_code ec;
  Mapping m;
  std::string e;
  bool ok = true;
  std::string note, detail;   // note: the on-screen notice (file names only, never a user path); detail: the log line (full paths)
  auto name = [](const std::string& p) { return fs::path(p).filename().string(); };
  if (fs::exists(path, ec)) {
    if (!read_mapping_file(path, &m, &e)) {
      ok = false;
      m = Mapping::defaults();
      fs::path bad = path;
      bad += ".invalid";
      fs::copy_file(path, bad, fs::copy_options::overwrite_existing, ec);
      note = "Controls file is invalid, using default controls. " + e + " (kept as " + name(bad.string()) + "; saving a change overwrites " + name(path) + ")";
      detail = "invalid " + path + ", kept as " + bad.string();
    }
  } else if (!legacy.empty() && fs::exists(legacy, ec)) {
    if (read_mapping_file(legacy, &m, &e)) {
      std::string werr;
      write_mapping_file(m, path, &werr);
      // once: input.json now exists, so the next start loads it and says nothing
      note = "Moved your controls from " + name(legacy) + " to " + name(path) + ".";
      detail = "migrated " + legacy + " to " + path + (werr.empty() ? "" : " (write failed: " + werr + ")");
    } else {
      ok = false;
      m = Mapping::defaults();
      note = "The old controls file is invalid, using default controls. " + e;
    }
  } else {
    m = Mapping::defaults();
    std::string werr;
    write_mapping_file(m, path, &werr);
  }
  std::lock_guard<std::mutex> g(mu_);
  map_ = m;
  icons::set_mapping(m);
  path_ = path;
  if (!note.empty()) { set_message_locked(note); log("input: %s%s%s", note.c_str(), detail.empty() ? "" : " | ", detail.c_str()); }
  return ok;
}

bool Hub::save(std::string* err) {
  Mapping m;
  std::string path;
  {
    std::lock_guard<std::mutex> g(mu_);
    m = map_;
    path = path_;
  }
  if (path.empty()) { if (err) *err = "no controls file path"; return false; }
  std::string e;
  bool ok = write_mapping_file(m, path, &e);
  std::lock_guard<std::mutex> g(mu_);
  if (!ok) set_message_locked("Could not save the controls: " + e);
  if (err && !ok) *err = e;
  return ok;
}

// ---- scripted input (SNAP_INPUT_SCRIPT=<file>): timed presses OR-ed onto whatever the host devices report.
// One entry per line, '#' starts a comment, times are seconds since the first poll:
//   <start> <hold> <token>...                      press the tokens from start for hold seconds
//   repeat <start> <end> <period> <hold> <token>...  the same press every period seconds in [start, end)
//   anchor <event>                                 starts a new segment: its entries count seconds from the moment the game
//                                                  raises <event> (script_event; "swkbd" = the name prompt opened) and the
//                                                  entries of earlier segments stop. Until the event fires the old segment runs on.
//   wait <cond> [timeout=<s>] [for=<n>]            the same, but the segment starts when a game-state condition holds (for n
//                                                  presents in a row, default 1). timeout (default 300 s, 0 = none; anchors take
//                                                  it too) counts from the start of the segment before: when it runs out the
//                                                  run logs "input script: TIMEOUT" with the state it saw and exits (code 3).
//     conditions (host/mods/routes.cpp, neofilm.cpp, skips.cpp report them; '!' in front negates):
//       ui=<scene>             a Lumen UI scene (.drp base names: title, main_menu, map_select, photo_submit, evaluation,
//                              pause_menu, option, result_reward, ...) is open (between its "opened" and "closed" events)
//       ui=<scene>:<state>     that scene is open and in that layout state (a trailing '*' matches a prefix); states seen:
//                              title PressStart, main_menu MainMenu, map_select Start/StageSelect/CourseSelect/Exit,
//                              photo_submit FolderSelect (subject folders) / PhotoSubmit (photos of one folder),
//                              evaluation Start, per photo EvaluationNew/Comment/Score/Announce/PhotoRegister, then End
//       dialog                 the dialogue / skit text window waits for A (main+0x676630 ran in the last 8 presents)
//       ride                   the course rail is moving (NEO-ONE progress advanced in the last 20 presents)
//       ride>=<x>              moving and the progress along the rail (rail object +0x70, about 0.9 per second) is at least x
//       ride_end               a ride was seen and the rail has not moved for 90 presents
//       film<=<n>              the course HUD's film counter (72 at the start) is at most n
//       state=idle             no save-state request is queued or running (see the save=/load= tokens below)
//       state=saves>=<n>       n saves finished ok since the start (also state=loads>=<n>; state=done>=<n> counts refused ones too). Use these after a
//                              save=/load= line: they cannot miss a request that finished between two polls, `state=idle` can.
//       aim                    the aim tool's target is centered and in range (below)
//       pose=<text>            some actor's current pose event contains <text> (aim tool on)
//       act=<text>             the game's viewfinder is locked on an actor playing that event: a photo taken now carries it
// Aim tool (dev only, SNAP_AIM=1, host/mods/aim.cpp; without it these lines do nothing and time out):
//   aim <species|nearest|pose:<text>|off> [timeout=<s>] [tol=] [range=<m>] [up=<m>] [zoom=1]
//                                                  starts a segment that drives the right stick until the target is centered
//                                                  (species = part of the model name, e.g. CH_017); soft timeout (default 8 s):
//                                                  the route goes on either way.
//   wait_pose <text> [timeout=<s>]                 starts a segment when an actor enters that pose event (soft, default 30 s)
//   shoot [<start>]                                takes a photo (A) and logs "aim: shot target=.. off=.. dist=.. pose=.."
// Save states: the tokens save=<name> and load=<name> (once, when their entry starts) save the whole game to RAM / bring it back (one slot; docs/save-states.md).
// They run on their own thread and drain the game first, so follow them with `wait state=idle`. The script's own clock and HLE route state keep running forward.
// Frame timing (load-independent): write the start as @f<frame> (e.g. "@f600 30 A", "repeat @f0 @f3000 90 15 A") and the whole line counts
// guest presents (nvn present count) since the segment began, so a slow PC does not shift it; end/period/hold may carry an optional @f prefix.
//   src=<kind:name>        hold a HOST source through the binding table while the entry is active: key:P, mouse:left, pad:east (the other tokens press
//                          Switch buttons after the mapping). Takes effect one poll later; use it to test a remapped key end to end.
//   mouse=<x>,<y>         put the cursor at this window position (0..1, y down) while the entry is active; with src=mouse:left it is a touch-screen tap there
//                          (menus; on a ride with mouse look on the left button is the shutter instead).
// A token is a Switch button name (A B X Y L R ZL ZR Plus Minus Left Up Right Down StickL StickR ...) or an axis
// lx=<f> ly=<f> rx=<f> ry=<f> with f in [-1, 1] (y up positive).
namespace {
struct ScriptEntry { bool is_shoot = false; bool has_mpos = false; float mx = 0, my = 0; double start, end, period, hold; uint64_t buttons; int32_t ax[4]; bool has_ax[4]; int seg; bool frames; float rumble_lo, rumble_hi; bool has_rumble; float gyro_yaw, gyro_pitch; bool has_gyro; bool is_end; int shot_n; bool screenshot; std::string cond; std::vector<std::string> fc; int state_op = 0; std::string state_name; mutable bool state_prev = false; std::vector<Source> src; std::string icount_tag; };
struct Script {
  bool loaded = false;
  uint64_t t0 = 0;
  std::vector<ScriptEntry> e;
  std::vector<std::string> seg_event;   // seg_event[k] raises segment k+1 ("?<cond>" = a "wait" line)
  std::vector<double> seg_timeout;       // seconds segment k may wait for seg_event[k] (0 = forever)
  std::vector<int> seg_hold;             // presents the wait condition must hold
  std::map<int, std::string> seg_aim;    // `aim` lines: the target to lock when segment k starts waiting (host/mods/aim.cpp)
  std::set<int> seg_soft;                // segments whose timeout starts the next segment instead of ending the run (aim, wait_pose)
  int load_seg = 0;                      // segment being parsed
  std::atomic<int> cur{0};               // segment running
  std::atomic<uint64_t> cur_t0{0};       // start time (ns) of the running segment
  std::atomic<int> cur_f0{0};            // present count at the start of the running segment
};
Script g_script;
// `src=<kind:name>` tokens: host sources (key:P, mouse:left, pad:east) held down through the binding table while their entry is active. Filled by script_apply,
// read by Hub::poll before the mapping, so a route can test a rebound key end to end (the other tokens press Switch buttons after the mapping).
std::bitset<kKeyCount> g_script_keys;
std::bitset<32> g_script_pad;
uint8_t g_script_mouse = 0;
bool g_script_mpos_on = false; float g_script_mx = 0, g_script_my = 0;   // `mouse=<x>,<y>` token: a pretend cursor position (0..1 over the window) for the touch screen


// ---- route signals: game state that inline hooks report (host/mods/routes.cpp, neofilm.cpp, skips.cpp); "wait" lines read it.
// Off (every report returns at once) unless the script has a "wait" line or SNAP_ROUTE_LOG=1 (which also logs scenes and states).
struct UiScene { uint64_t player; char name[40]; char state[64]; char focus[40]; int f_event, f_state; bool open; };
struct RailSig { uint64_t obj; float prog; int f_adv, f_start; };
std::mutex g_rs_mu;
UiScene g_ui[64];
int g_nui;
RailSig g_rail[8];
int g_nrail, g_rail_cur = -1;
std::atomic<bool> g_route_on{false};
bool g_route_log;
std::atomic<int> g_dialog_f{-1000000}, g_film{-1}, g_film_f{-1000000};
std::atomic<int64_t> g_load_release_until_ns{0};   // physical pad reads as idle until then (set by route_state_loaded)
void route_enable() { g_route_on = true; }
struct RouteInit { RouteInit() { const char* e = std::getenv("SNAP_ROUTE_LOG"); g_route_log = e && *e && *e != '0'; if (g_route_log) g_route_on = true; } } g_route_init;

// evaluates one "wait" condition (text after '?'); `why` gets a short state note for the log
bool route_cond(const std::string& c0, std::string* why) {
  const bool neg = !c0.empty() && c0[0] == '!';
  std::string c = neg ? c0.substr(1) : c0;
  if (c.rfind("rail>=", 0) == 0) c = "ride>=" + c.substr(6);   // alias: `wait rail>=<x>` = `wait ride>=<x>`
  const int f = nvn::g_presentCount;
  bool r = false;
  char b[160] = "";
  std::lock_guard<std::mutex> lk(g_rs_mu);
  if (c.rfind("ui=", 0) == 0) {
    std::string scene = c.substr(3), st;
    size_t col = scene.find(':');
    if (col != std::string::npos) { st = scene.substr(col + 1); scene.resize(col); }
    for (int i = 0; i < g_nui && !r; ++i) {
      const UiScene& s = g_ui[i];
      if (scene != s.name) continue;
      r = s.open;
      if (r && !st.empty()) r = st.back() == '*' ? std::strncmp(s.state, st.c_str(), st.size() - 1) == 0 : st == s.state;
      std::snprintf(b, sizeof b, "%s %s, state '%s' for %d presents", s.name, s.open ? "open" : "closed", s.state, f - s.f_state);
    }
    if (!b[0]) std::snprintf(b, sizeof b, "scene %s not seen", scene.c_str());
  } else if (c.rfind("focus=", 0) == 0) {   // focus=<element>: the last UI focus event (any scene, newest) named that Lumen element (QA routes: press Down until focus=AllCheck)
    const UiScene* nw = nullptr;
    for (int i = 0; i < g_nui; ++i) if (g_ui[i].open && g_ui[i].focus[0] && (!nw || g_ui[i].f_event >= nw->f_event)) nw = &g_ui[i];
    r = nw && c.substr(6) == nw->focus;
    std::snprintf(b, sizeof b, "focus %s", nw ? nw->focus : "none");
  } else if (c == "dialog") {
    r = f - g_dialog_f.load() <= 8;
    std::snprintf(b, sizeof b, "dialog window last waited %d presents ago", f - g_dialog_f.load());
  } else if (c == "ride" || c.rfind("ride>=", 0) == 0 || c == "ride_end") {
    const RailSig* rs = g_rail_cur >= 0 ? &g_rail[g_rail_cur] : nullptr;
    if (c == "ride_end") r = rs && f - rs->f_adv > 90;
    else r = rs && f - rs->f_adv <= 20 && (c == "ride" || rs->prog >= (float)std::atof(c.c_str() + 6));
    if (rs) std::snprintf(b, sizeof b, "rail progress %.2f, last advance %d presents ago", rs->prog, f - rs->f_adv);
    else std::snprintf(b, sizeof b, "no rail seen");
  } else if (c.rfind("film<=", 0) == 0) {
    r = g_film.load() >= 0 && g_film.load() <= std::atoi(c.c_str() + 6);
    std::snprintf(b, sizeof b, "film counter %d", g_film.load());
  } else if (c.rfind("present>=", 0) == 0) {   // a fixed present (a cutscene shot of a frame-timed route; host/ab_capture.ps1 -Moment 'present>=4400')
    r = (int)nvn::g_presentCount >= std::atoi(c.c_str() + 9);
    std::snprintf(b, sizeof b, "present %d", (int)nvn::g_presentCount);
  } else if (c == "aim") {   // the `aim` line's target sits on the reticle (host/mods/aim.cpp)
    r = snap_aim_ready(b, (int)sizeof b) != 0;
  } else if (c.rfind("act=", 0) == 0) {   // the game's viewfinder is locked on an actor playing that event (a photo now carries it)
    r = snap_aim_act(c.c_str() + 4, b, (int)sizeof b) != 0;
  } else if (c.rfind("pose=", 0) == 0) {   // `wait_pose <text>`: the aim target's current motion event name contains text
    r = snap_aim_pose(c.c_str() + 5, b, (int)sizeof b) != 0;
  } else if (c == "state=idle") {   // no save/load request queued or running (runtime/os/os_state.cpp)
    r = snap_state_idle() != 0;
    std::snprintf(b, sizeof b, "state worker %s", r ? "idle" : "busy");
  } else if (c.rfind("state=saves>=", 0) == 0 || c.rfind("state=loads>=", 0) == 0 || c.rfind("state=done>=", 0) == 0) {   // counters never race: state=loads>=2 = two loads finished ok
    const size_t ge = c.find(">=");
    const int n = std::atoi(c.c_str() + ge + 2), have = snap_state_count(c.c_str() + 6);
    r = have >= n;
    std::snprintf(b, sizeof b, "%s: %d", c.substr(6, ge - 6).c_str(), have);
  } else {
    std::snprintf(b, sizeof b, "unknown condition");
  }
  if (why) *why = b;
  return neg ? !r : r;
}

// one line per known scene plus rail, film and dialog (timeouts)
void route_dump() {
  std::lock_guard<std::mutex> lk(g_rs_mu);
  const int f = nvn::g_presentCount;
  for (int i = 0; i < g_nui; ++i)
    log("input script:   ui %-16s %s, state '%s' for %d presents, last event %d presents ago", g_ui[i].name, g_ui[i].open ? "open  " : "closed",
        g_ui[i].state, f - g_ui[i].f_state, f - g_ui[i].f_event);
  if (g_rail_cur >= 0) log("input script:   rail progress %.2f, last advance %d presents ago", g_rail[g_rail_cur].prog, f - g_rail[g_rail_cur].f_adv);
  log("input script:   film %d, dialog last waited %d presents ago, present %d", g_film.load(), f - g_dialog_f.load(), f);
}
}  // namespace

bool route_on() { return g_route_on.load(std::memory_order_relaxed); }

// One-run A/B capture tool (host/mods/abcap.cpp, docs/ab-capture.md): read a route "wait" condition from host code, and hold buttons on top of the script.
static std::atomic<uint64_t> g_ab_buttons{0};
extern "C" int snap_route_cond(const char* c, char* why, int n) {
  route_enable();
  std::string w;
  const bool r = route_cond(c ? c : "", &w);
  if (why && n > 0) std::snprintf(why, (size_t)n, "%s", w.c_str());
  return r ? 1 : 0;
}
extern "C" float snap_route_ride_progress() {   // rail progress of the current ride, -1 when none (the particle census in runtime/nvn/nvn_foliage.cpp tags presents with it)
  std::lock_guard<std::mutex> lk(g_rs_mu);
  return g_rail_cur >= 0 && (int)nvn::g_presentCount - g_rail[g_rail_cur].f_adv <= 20 ? g_rail[g_rail_cur].prog : -1.0f;
}
extern "C" void snap_input_force_buttons(const char* names) {   // space separated Switch button names ("A", "A B"); "" releases
  uint64_t m = 0;
  std::istringstream is(names ? names : "");
  for (std::string t; is >> t;) { const int b = button_from_name(t); if (b >= 0) m |= 1ull << b; }
  g_ab_buttons.store(m);
}

void route_ui(uint64_t player, const char* scene, const char* state, int event, const char* elem, uint64_t lr) {
  const int f = nvn::g_presentCount;
  std::lock_guard<std::mutex> lk(g_rs_mu);
  UiScene* s = nullptr;
  for (int i = 0; i < g_nui; ++i) if (g_ui[i].player == player) { s = &g_ui[i]; break; }
  if (s && std::strcmp(s->name, scene) != 0) {   // the scene was freed and a new one got its address
    if (g_route_log) log("route: ui player %#llx was '%s', now '%s'", (unsigned long long)player, s->name, scene);
    s->player = 0;
    s = nullptr;
  }
  if (!s) {
    for (int i = 0; i < g_nui; ++i) if (!g_ui[i].player) { s = &g_ui[i]; break; }
    if (!s && g_nui >= 64) return;
    if (!s) s = &g_ui[g_nui++];
    *s = UiScene{};
    s->player = player;
    std::snprintf(s->name, sizeof s->name, "%s", scene);
    s->f_state = f;
    if (g_route_log) log("route: ui scene '%s' player %#llx first %s from main+%#llx, present %d", scene, (unsigned long long)player,
                         event < 0 ? "SetState" : "event", (unsigned long long)(lr - 0x7100000000ull), f);
  }
  if (event >= 0) s->f_event = f;
  if (event == 1 || event == 3) s->open = false;   // 1 = built (hidden), 3 = closed
  if (event == 2) s->open = true;                  // 2 = opened
  if (std::strcmp(s->state, state) != 0 && state[0]) {   // SetState also passes '' between two states of one page: ignored
    if (g_route_log) log("route: ui %s state '%s' -> '%s' (%s %d) present %d", s->name, s->state, state, event < 0 ? "SetState" : "event", event, f);
    std::snprintf(s->state, sizeof s->state, "%s", state);
    s->f_state = f;
  }
  if (event == 16 && elem && elem[0]) std::snprintf(s->focus, sizeof s->focus, "%s", elem);   // 16 = focus moved
  if (g_route_log && event >= 0 && event != 22) log("route: ui %s event %d state '%s' element '%s' present %d", s->name, event, state, elem, f);
}

void route_rail(uint64_t obj, float prog) {
  if (!route_on()) return;
  const int f = nvn::g_presentCount;
  std::lock_guard<std::mutex> lk(g_rs_mu);
  int k = -1;
  for (int i = 0; i < g_nrail; ++i) if (g_rail[i].obj == obj) { k = i; break; }
  if (k < 0) {
    k = g_nrail < 8 ? g_nrail++ : 7;
    g_rail[k] = RailSig{obj, prog, -1000000, f};
    if (g_route_log) log("route: rail object %#llx progress %.2f present %d", (unsigned long long)obj, prog, f);
  }
  RailSig& r = g_rail[k];
  if (prog < r.prog - 0.5f) { r.f_start = f; if (g_route_log) log("route: rail %#llx restarts (%.2f -> %.2f) present %d", (unsigned long long)obj, r.prog, prog, f); }
  if (prog > r.prog) {
    if (g_route_log && f - r.f_adv > 20) log("route: rail %#llx moving at %.2f present %d", (unsigned long long)obj, prog, f);
    r.f_adv = f;
    g_rail_cur = k;
  }
  r.prog = prog;
}

void route_dialog() {
  if (!route_on()) return;
  const int f = nvn::g_presentCount;
  if (g_route_log && f - g_dialog_f.load() > 8) log("route: dialog window waiting, present %d", f);
  g_dialog_f = f;
}

// A save-state load rewound the game: what the hooks reported since the save is stale. The film counter is only reported when the HUD changes it,
// so forget it until the next report (runtime/nvn/nvn_state.cpp PostLoad).
// It also releases the physical pad for a moment: a button held across the load was pressed (or not) in a different game moment.
void route_state_loaded() {
  g_film = -1;
  g_load_release_until_ns = std::chrono::steady_clock::now().time_since_epoch().count() + 150000000;
}

void route_film(int count) {
  if (!route_on()) return;
  if (g_route_log && count != g_film.load()) log("route: film counter %d present %d", count, nvn::g_presentCount);
  g_film = count;
  g_film_f = nvn::g_presentCount;
}

namespace {
// "@f120" or "120" -> number; sets fr when the @f prefix is present
double script_num(const std::string& w, bool* fr = nullptr) {
  bool f = w.size() > 2 && w[0] == '@' && w[1] == 'f';
  if (fr && f) *fr = true;
  return std::atof(w.c_str() + (f ? 2 : 0));
}

bool script_token(const std::string& t, ScriptEntry& en) {
  if (freecam::script_valid_token(t)) { en.fc.push_back(t); return true; }   // fc=on|off|toggle, fcmove=, fclook=, fcpose=, fcfov=, fcshot, fcfreeze=, fcspeed= (runtime/hle/freecam.h)
  size_t eq = t.find('=');
  if (eq != std::string::npos) {
    std::string k = t.substr(0, eq);
    if (k == "gyro") {   // gyro=<yaw_dps>,<pitch_dps>: a pretend gyro rate while the entry is active (needs SNAP_GYRO=1)
      en.has_gyro = true; en.gyro_yaw = (float)std::atof(t.c_str() + eq + 1);
      size_t c = t.find(',', eq); en.gyro_pitch = c == std::string::npos ? 0.0f : (float)std::atof(t.c_str() + c + 1);
      return true;
    }
    if (k == "if") { en.cond = t.substr(eq + 1); return true; }   // if=<cond>: the entry presses only while the wait condition holds (`!` negates), e.g. `repeat @f60 @f600 90 8 Down if=!focus=AllCheck`
    if (k == "save" || k == "load") {   // save=<name> / load=<name>: an in-RAM save state of the whole game, once when the entry starts (docs/save-states.md); wait state=idle for it to finish
      en.state_op = k == "save" ? 1 : 2; en.state_name = t.substr(eq + 1);
      return true;
    }
    if (k == "src") {   // src=<key:P|mouse:left|pad:east>: hold that host source while the entry is active (goes through the binding table, host/README.md "Controls menu (F1)")
      Source sr;
      if (!parse_source(t.substr(eq + 1), &sr)) return false;
      en.src.push_back(sr);
      return true;
    }
    if (k == "mouse") {   // mouse=<x>,<y>: the cursor sits at this window position (0..1, y down) while the entry is active; with src=mouse:left it is a touch / click there
      en.has_mpos = true; en.mx = (float)std::atof(t.c_str() + eq + 1);
      size_t c = t.find(',', eq); en.my = c == std::string::npos ? 0.5f : (float)std::atof(t.c_str() + c + 1);
      return true;
    }
    if (k == "icount") { en.icount_tag = t.substr(eq + 1); return true; }   // icount=<tag>: once when the entry starts, log the imports called since the last dump (debug, needs SNAP_IMPORT_COUNT=1)
    if (k == "shot") {   // shot=<n>: while the entry is active, ask NVN_FRAME_DUMP for a frame every n presents (needs NVN_FRAME_DUMP; test evidence aid)
      en.shot_n = std::max(1, std::atoi(t.c_str() + eq + 1));
      return true;
    }
    if (k == "rumble") {   // rumble=<low>,<high>: a host-side rumble request while the entry is active (test of the host chain)
      en.has_rumble = true; en.rumble_lo = (float)std::atof(t.c_str() + eq + 1);
      size_t c = t.find(',', eq); en.rumble_hi = c == std::string::npos ? en.rumble_lo : (float)std::atof(t.c_str() + c + 1);
      return true;
    }
    int i = k == "lx" ? 0 : k == "ly" ? 1 : k == "rx" ? 2 : k == "ry" ? 3 : -1;
    if (i < 0) return false;
    double f = std::atof(t.c_str() + eq + 1);
    en.ax[i] = (int32_t)std::lround(std::clamp(f, -1.0, 1.0) * 32767.0);
    en.has_ax[i] = true;
    return true;
  }
  if (t == "screenshot") { en.screenshot = true; return true; }   // screenshot: the F12 key once, when the entry starts (runtime/nvn/nvn_shot.cpp)
  int b = button_from_name(t);
  if (b < 0) return false;
  en.buttons |= 1ull << b;
  return true;
}

}  // namespace

// Route line "course <name>": pick a course on the stage-select map without depending on timing. The map's focus element (UI event 16,
// route condition focus=<n>) is the stage number. From Florio (1) the D-pad moves: Right 12, 3 (beach), 14 (reef), 4, 11 (ruins);
// from 11 Up 10 (cave); from 10 Left 9 (snow); from 1 Up 5 (jungle), 6 (forest); from 6 Right 7 (sands); from 12 Up 13 (river);
// from 4 Up 8 (volcano). Each press repeats only while the stage before it has focus, so a press the map ignored (a popup, a save
// in progress) is sent again and a press cannot land twice; the wait fails loudly if the map opened somewhere off the path.
std::vector<std::string> course_steps(const std::string& name) {
  struct Course { const char* name; const char* path; int stage; };
  static const Course kCourse[] = {{"florio", "", 1},      {"jungle", "U", 5},     {"forest", "UU", 6},     {"sands", "UUR", 7},
                                   {"river", "RU", 13},    {"beach", "RR", 3},     {"reef", "RRR", 14},     {"volcano", "RRRRU", 8},
                                   {"ruins", "RRRRR", 11}, {"cave", "RRRRRU", 10}, {"snow", "RRRRRUL", 9}};
  struct Edge { int from; char dir; int to; };
  static const Edge kEdge[] = {{1, 'R', 12}, {12, 'R', 3}, {3, 'R', 14}, {14, 'R', 4}, {4, 'R', 11}, {11, 'U', 10},
                               {10, 'L', 9}, {1, 'U', 5},  {5, 'U', 6},  {6, 'R', 7},  {12, 'U', 13}, {4, 'U', 8}};
  std::vector<std::string> out;
  for (const Course& c : kCourse) {
    if (name != c.name) continue;
    int at = 1;
    for (const char* p = c.path; *p; ++p) {
      int to = 0;
      for (const Edge& e : kEdge) if (e.from == at && e.dir == *p) to = e.to;
      if (!to) return {};
      out.push_back(std::string("repeat @f60 @f60000 240 6 ") + (*p == 'R' ? "Right" : *p == 'U' ? "Up" : "Left") + " if=focus=" + std::to_string(at));
      at = to;
    }
    if (at != c.stage) return {};
    out.push_back("wait focus=" + std::to_string(at) + " timeout=120");
    out.push_back("repeat @f120 @f60000 240 6 A");
    out.push_back("wait ui=map_select:CourseSelect timeout=240");
    return out;
  }
  return {};
}

namespace {

void script_load() {
  g_script.loaded = true;
  const char* path = std::getenv("SNAP_INPUT_SCRIPT");
  if (!path || !*path) return;
  std::ifstream f(path);
  if (!f) { log("input script: cannot open %s", path); return; }
  std::string line;
  int n = 0;
  std::vector<std::string> macro;   // lines a "course <name>" line stands for, read before the file goes on
  size_t macro_at = 0;
  auto next = [&]() {
    if (macro_at < macro.size()) { line = macro[macro_at++]; return true; }
    return (bool)std::getline(f, line);
  };
  while (next()) {
    size_t h = line.find('#');
    if (h != std::string::npos) line.resize(h);
    std::istringstream is(line);
    std::string w;
    if (!(is >> w)) continue;
    if (w == "course") {   // course <name>: see course_steps
      std::string nm;
      is >> nm;
      macro = course_steps(nm); macro_at = 0;
      if (macro.empty()) { log("input script: unknown course in '%s'; the route cannot go on", line.c_str()); std::fflush(stdout); std::_Exit(3); }
      continue;
    }
    if (w == "anchor" || w == "wait") {
      std::string ev, o;
      double to = w == "wait" ? 300.0 : 0.0;
      int hold = 1;
      if (!(is >> ev)) { log("input script: bad line '%s'", line.c_str()); continue; }
      while (is >> o) {
        if (o.rfind("timeout=", 0) == 0) to = std::atof(o.c_str() + 8);
        else if (o.rfind("for=", 0) == 0) hold = std::max(1, std::atoi(o.c_str() + 4));
        else log("input script: unknown option '%s' in '%s'", o.c_str(), line.c_str());
      }
      if (w == "wait") { ev = "?" + ev; route_enable(); }
      g_script.seg_event.push_back(ev); g_script.seg_timeout.push_back(to); g_script.seg_hold.push_back(hold);
      ++g_script.load_seg;
      continue;
    }
    if (w == "aim" || w == "wait_pose") {   // aim <CH_xxx|nearest|off> [timeout=<s>] [tol=<f>] [range=<m>] / wait_pose <text> [timeout=<s>] (host/mods/aim.cpp, SNAP_AIM=1)
      std::string tgt, o;
      double to = w == "aim" ? 8.0 : 30.0;
      if (!(is >> tgt)) { log("input script: bad line '%s'", line.c_str()); continue; }
      std::string spec = tgt;
      while (is >> o) {
        if (o.rfind("timeout=", 0) == 0) to = std::atof(o.c_str() + 8);
        else if (w == "aim" && (o.rfind("tol=", 0) == 0 || o.rfind("range=", 0) == 0 || o.rfind("up=", 0) == 0 || o.rfind("zoom=", 0) == 0 || o.rfind("cap=", 0) == 0)) spec += " " + o;
        else log("input script: unknown option '%s' in '%s'", o.c_str(), line.c_str());
      }
      if (w == "aim") g_script.seg_aim[g_script.load_seg] = spec;
      g_script.seg_soft.insert(g_script.load_seg);
      route_enable();
      g_script.seg_event.push_back(w == "aim" ? std::string("?aim") : "?pose=" + tgt); g_script.seg_timeout.push_back(to); g_script.seg_hold.push_back(w == "aim" ? 3 : 1);
      ++g_script.load_seg;
      continue;
    }
    ScriptEntry en{};
    en.seg = g_script.load_seg;
    if (w == "shoot") {   // shoot [<start>]: A for 4 presents, and the aim tool logs the shot
      std::string a;
      if (!(is >> a)) a = "@f0";
      en.start = en.end = script_num(a, &en.frames);
      en.period = 1; en.hold = en.frames ? 4 : 0.07; en.buttons = 1ull << button_from_name("A"); en.is_shoot = true; en.shot_n = 1;
      g_script.e.push_back(en);
      ++n;
      continue;
    }
    if (w == "end") {   // end <start>: the route is done; the run exits with code 0 at that time of the segment
      std::string a;
      if (!(is >> a)) a = "0";
      en.start = en.end = script_num(a, &en.frames);
      en.period = 1; en.hold = 1e9; en.is_end = true;
      g_script.e.push_back(en);
      ++n;
      continue;
    }
    bool rep = w == "repeat";
    if (rep) {
      std::string a, b, c, d;
      if (is >> a >> b >> c >> d) { en.start = script_num(a, &en.frames); en.end = script_num(b); en.period = script_num(c); en.hold = script_num(d); }
      if (a.empty() || d.empty() || en.period <= 0) { log("input script: bad line '%s'", line.c_str()); continue; }
    } else {
      en.start = script_num(w, &en.frames);
      en.end = en.start;
      en.period = 1;
      std::string hs;
      if (is >> hs) en.hold = script_num(hs);
      else { log("input script: bad line '%s'", line.c_str()); continue; }
    }
    bool ok = true;
    while (is >> w) ok &= script_token(w, en);
    if (!ok) { log("input script: unknown token in '%s'", line.c_str()); continue; }
    g_script.e.push_back(en);
    ++n;
  }
  log("input script: %d entries from %s", n, path);
}

// Applies the script at time `now` (seconds since the first poll) to `p`.
float g_script_rumble_lo = 0, g_script_rumble_hi = 0;
bool g_script_gyro_on = false; float g_script_gyro_yaw = 0, g_script_gyro_pitch = 0;   // read by Hub::poll after script_apply
// Warns when a segment ends (a wait/anchor raised the next one) while some of its lines never started: they would silently be dropped.
void script_warn_unfired(int k, double el_t, double el_f) {
  int n = 0; double first = 0; bool fr = false;
  for (const ScriptEntry& en : g_script.e) {
    if (en.seg != k || en.is_end) continue;
    const double now = en.frames ? el_f : el_t;
    if (en.start > now && (n == 0 || en.start < first)) { first = en.start; fr = en.frames; }
    if (en.start > now) ++n;
  }
  if (n > 0) log("input script: WARNING segment %d ended before %d of its lines started (first at %s%.0f); they never fire", k, n, fr ? "@f" : "", first);
}
// "wait" lines and timeouts: starts the next segment when the pending condition holds, or ends the run when time runs out.
void script_check_wait(uint64_t now_ns_v) {
  static int held_k = -1, held_n = 0, held_f = -1;
  const int k = g_script.cur.load();
  if (k >= (int)g_script.seg_event.size()) return;
  const std::string& ev = g_script.seg_event[(size_t)k];
  const double el = (double)(now_ns_v - g_script.cur_t0.load()) * 1e-9;
  { static int aim_k = -1;   // an `aim` line locks its target when its segment starts waiting
    if (aim_k != k) { aim_k = k; const auto it = g_script.seg_aim.find(k); if (it != g_script.seg_aim.end()) snap_aim_set(it->second.c_str()); } }
  if (ev[0] == '?') {
    std::string why;
    const int f = nvn::g_presentCount;
    if (route_cond(ev.substr(1), &why)) {
      if (held_k != k) { held_k = k; held_n = 0; held_f = -1; }
      if (f != held_f) { held_f = f; ++held_n; }
      if (held_n >= g_script.seg_hold[(size_t)k]) {
        script_warn_unfired(k, el, (double)(f - g_script.cur_f0.load()));
        g_script.cur_t0 = now_ns_v;
        g_script.cur_f0 = f;
        g_script.cur = k + 1;
        log("input script: wait '%s' met after %.1f s at present %d (%s): segment %d", ev.c_str() + 1, el, f, why.c_str(), k + 1);
        return;
      }
    } else if (held_k == k) {
      held_n = 0;
    }
  }
  const double to = g_script.seg_timeout[(size_t)k];
  if (to > 0 && el > to) {
    std::string why;
    if (ev[0] == '?') route_cond(ev.substr(1), &why);
    log("input script: TIMEOUT after %.0f s waiting for %s '%s' (segment %d, %s); game state:", el, ev[0] == '?' ? "condition" : "event",
        ev.c_str() + (ev[0] == '?'), k, why.empty() ? "-" : why.c_str());
    if (g_script.seg_soft.count(k)) {   // aim / wait_pose: the route goes on without it
      const int f = nvn::g_presentCount;
      g_script.cur_t0 = now_ns_v;
      g_script.cur_f0 = f;
      g_script.cur = k + 1;
      log("input script: wait '%s' gave up at present %d, the route goes on: segment %d", ev.c_str() + 1, f, k + 1);
      return;
    }
    route_dump();
    const char* e = std::getenv("SNAP_ROUTE_TIMEOUT");
    if (e && !std::strcmp(e, "continue")) { g_script.seg_timeout[(size_t)k] = 0; return; }   // keep running (debug)
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
  }
}

void script_apply(uint64_t now_ns_v, PadSample& p) {
  script_check_wait(now_ns_v);
  g_script_rumble_lo = g_script_rumble_hi = 0; g_script_gyro_on = false; g_script_gyro_yaw = g_script_gyro_pitch = 0;
  static uint64_t last = ~0ull;
  uint64_t cur = 0;
  const int seg = g_script.cur.load();
  const double now_t = (double)(now_ns_v - g_script.cur_t0.load()) * 1e-9;
  const double now_f = (double)(nvn::g_presentCount - g_script.cur_f0.load());
  double now = now_t;
  freecam::script_begin();
  g_script_keys.reset(); g_script_pad.reset(); g_script_mouse = 0; g_script_mpos_on = false;
  bool shot_on = false;
  for (const ScriptEntry& en : g_script.e) {
    if (en.seg != seg) continue;
    now = en.frames ? now_f : now_t;
    bool on;
    if (en.end > en.start) {   // repeat
      on = now >= en.start && now < en.end && std::fmod(now - en.start, en.period) < en.hold;
    } else {
      on = now >= en.start && now < en.start + en.hold;
    }
    if (!on) { en.state_prev = false; continue; }
    if (!en.cond.empty() && !route_cond(en.cond, nullptr)) continue;
    if (!en.icount_tag.empty() && !en.state_prev) { en.state_prev = true; snap_import_counts_dump(en.icount_tag.c_str()); }
    if (en.state_op && !en.state_prev) { en.state_prev = true; snap_state_request(en.state_op, en.state_name.c_str()); }   // edge: once per entry start
    if (en.is_shoot && !en.state_prev) { en.state_prev = true; snap_aim_shot(); }
    if (en.is_end) {
      log("input script: end of route reached (segment %d, present %d); exiting", seg, nvn::g_presentCount);
      std::fflush(stdout);
      std::fflush(stderr);
      std::_Exit(0);
    }
    for (const std::string& ft : en.fc) freecam::script_token(ft);
      snap_audio_log_checks();
    cur |= en.buttons;
    p.buttons |= en.buttons;
    for (const Source& sr : en.src) {
      if (sr.kind == Source::KeyboardKey) g_script_keys.set((size_t)sr.id);
      else if (sr.kind == Source::Pad) g_script_pad.set((size_t)sr.id);
      else if (sr.kind == Source::Mouse) g_script_mouse |= (uint8_t)(1u << sr.id);
    }
    if (en.has_mpos) { g_script_mpos_on = true; g_script_mx = en.mx; g_script_my = en.my; }
    if (en.has_gyro) { g_script_gyro_on = true; g_script_gyro_yaw += en.gyro_yaw; g_script_gyro_pitch += en.gyro_pitch; }
    if (en.screenshot) shot_on = true;
    if (en.shot_n > 0 && nvn::g_presentCount % en.shot_n == 0 && nvn::g_fdBurst <= 0) nvn::g_fdBurst = 4;   // `shot=<n>` token
    if (en.has_rumble) { g_script_rumble_lo = std::max(g_script_rumble_lo, en.rumble_lo); g_script_rumble_hi = std::max(g_script_rumble_hi, en.rumble_hi); }
    for (int i = 0; i < 4; ++i) if (en.has_ax[i]) (&p.lx)[i] = en.ax[i];
    p.connected = true;
  }
  { float ax = 0, ay = 0;   // a locked `aim` target steers the right stick (over any rx= / ry= token)
    if (const int am = snap_aim_stick(&ax, &ay)) {
      if (am == 2) { const uint64_t zl = 1ull << button_from_name("ZL"); cur |= zl; p.buttons |= zl; }   // zoom=1
      p.rx = (int32_t)std::lround(std::clamp((double)ax, -1.0, 1.0) * 32767.0);
      p.ry = (int32_t)std::lround(std::clamp((double)ay, -1.0, 1.0) * 32767.0);
      p.connected = true;
    } }
  freecam::script_end();
  static bool shot_prev = false;
  if (shot_on && !shot_prev) g_shot_req = true;
  shot_prev = shot_on;
  if (const uint64_t ab = g_ab_buttons.load()) { cur |= ab; p.buttons |= ab; p.connected = true; }   // snap_input_force_buttons (A/B capture tool)
  if (cur != last) { last = cur; log("input script: seg %d t=%.2f buttons=%#llx", seg, now, (unsigned long long)cur); }
}
}  // namespace

void script_event(const char* name) {
  if (!g_script.loaded) return;
  int k = g_script.cur.load();
  if (k < (int)g_script.seg_event.size() && g_script.seg_event[(size_t)k] == name) {
    script_warn_unfired(k, (double)(now_ns() - g_script.cur_t0.load()) * 1e-9, (double)(nvn::g_presentCount - g_script.cur_f0.load()));
    g_script.cur_t0 = now_ns();
    g_script.cur_f0 = nvn::g_presentCount;
    g_script.cur = k + 1;
    log("input script: event '%s' starts segment %d", name, k + 1);
  }
}

// ---- window-thread wheel feed
static std::atomic<int> g_wheel_delta{0};
void post_wheel(int delta) { g_wheel_delta.fetch_add(delta); }

static uint32_t src_key(const Source& s) { return ((uint32_t)s.kind << 8) | (uint32_t)s.id; }
static bool capture_allowed(const Source& s, unsigned mask) { return (mask >> (int)device_of(s)) & 1; }
static void each_candidate(const std::function<void(const Source&)>& f) {
  for (int i = 0; i < kPadCount; ++i) f(Source{Source::Pad, i});
  for (int k = 1; k < kKeyCount; ++k)
    if (key_name(k)[0] && k != key_from_name("F1")) f(Source{Source::KeyboardKey, k});
  for (int i = 0; i < kMouseCount; ++i) f(Source{Source::Mouse, i});
}

void Hub::set_window_focus(bool focused) {
  std::lock_guard<std::mutex> g(mu_);
  win_focus_ = focused;
  if (!focused) guard_prev_ = false;   // the game threads can be frozen while the window is in the background: remember the blur here, not in a poll
}

void Hub::set_menu_open(bool open) {
  std::lock_guard<std::mutex> g(mu_);
  if (open == menu_open_.load()) return;
  menu_open_ = open;
  tog_state_ = 0;
  chord_fired_ = true;   // the chord that opened the menu must be released before it can fire again
  chord_since_ns_ = 0;
  if (!open) { capturing_ = false; cap_have_ = false; }
  else if (backend_) backend_->rumble(0, 0);   // Hub::rumble is ignored while the menu is open: stop what was running
}
bool Hub::consume_menu_toggle() { return toggle_req_.exchange(false); }
PadSample Hub::preview() const { std::lock_guard<std::mutex> g(mu_); return preview_; }

void Hub::capture_begin(unsigned device_mask) {
  std::lock_guard<std::mutex> g(mu_);
  cap_mask_ = device_mask;
  cap_deadline_ns_ = now_ns() + 10000000000ull;
  cap_ignore_.clear();
  cap_have_ = false;
  cap_timeout_ = false;
  each_candidate([&](const Source& s) { if (source_down(s, raw_, 0.6f)) cap_ignore_.push_back(src_key(s)); });
  capturing_ = true;
}
void Hub::capture_cancel() { std::lock_guard<std::mutex> g(mu_); capturing_ = false; cap_have_ = false; }
bool Hub::capture_result(Source* out) {
  std::lock_guard<std::mutex> g(mu_);
  if (!cap_have_) return false;
  *out = cap_src_;
  cap_have_ = false;
  return true;
}
bool Hub::capture_timed_out() { std::lock_guard<std::mutex> g(mu_); bool t = cap_timeout_; cap_timeout_ = false; return t; }

void Hub::inject_source(const Source& s, bool down) {
  std::lock_guard<std::mutex> g(mu_);
  switch (s.kind) {
    case Source::KeyboardKey: inject_.keys.set((size_t)s.id, down); break;
    case Source::Pad: inject_.pad.set((size_t)s.id, down); break;
    case Source::Mouse: if (down) inject_.mouse_buttons |= (uint8_t)(1u << s.id); else inject_.mouse_buttons &= (uint8_t) ~(1u << s.id); break;
    default: break;
  }
}

static void apply_rumble(Backend* be, float low, float high, const char* src, uint64_t buttons, bool log_it);
void Hub::poll(int min_interval_ms) {
  std::lock_guard<std::mutex> g(mu_);
  if (!backend_) backend_ = make_default_backend();
  uint64_t t = now_ns();
  // SNAP_INPUT_STATS=1: how the game reads input (log line every 5 s): reads of the pad per presented frame, how old the sample it got was
  // (0 when the read polled the devices itself; the time since the last poll when the 1 ms throttle served it) and the mouse totals.
  static const bool stats_on = [] { const char* e = std::getenv("SNAP_INPUT_STATS"); return e && *e && *e != '0'; }();
  struct Stats { uint64_t t0 = 0, reads = 0, polls = 0, age_sum = 0, age_max = 0, mouse_events = 0, last_pres_ns = 0, phase_sum = 0, phase_n = 0; int pres0 = 0, last_pres = -1; double px = 0; };
  static Stats st;
  if (stats_on) {
    if (!st.t0) { st.t0 = t; st.pres0 = (int)nvn::g_presentCount; }
    ++st.reads;
    const uint64_t age = last_poll_ns_ && t - last_poll_ns_ < (uint64_t)min_interval_ms * 1000000ull ? t - last_poll_ns_ : 0;
    st.age_sum += age; st.age_max = std::max(st.age_max, age);
    if ((int)nvn::g_presentCount != st.last_pres) {   // first read after a new present: how long after it
      if (st.last_pres >= 0 && st.last_pres_ns) { st.phase_sum += t - st.last_pres_ns; ++st.phase_n; }
      st.last_pres = (int)nvn::g_presentCount; st.last_pres_ns = t;
    }
    if (t - st.t0 >= 5000000000ull) {
      const int pres = (int)nvn::g_presentCount - st.pres0;
      log("input stats: %.1f s, %llu reads, %llu polls, %d presents (%.2f reads per present), sample age mean %.3f ms max %.3f ms, read after present mean %.3f ms, mouse %llu moves %.0f px",
          (double)(t - st.t0) * 1e-9, (unsigned long long)st.reads, (unsigned long long)st.polls, pres, pres > 0 ? (double)st.reads / pres : 0.0,
          st.reads ? (double)st.age_sum / st.reads * 1e-6 : 0.0, (double)st.age_max * 1e-6, st.phase_n ? (double)st.phase_sum / st.phase_n * 1e-6 : 0.0,
          (unsigned long long)st.mouse_events, st.px);
      st = Stats{}; st.t0 = t; st.pres0 = (int)nvn::g_presentCount; st.last_pres = (int)nvn::g_presentCount;
    }
  }
  if (last_poll_ns_ && t - last_poll_ns_ < (uint64_t)min_interval_ms * 1000000ull) return;
  last_poll_ns_ = t;
  if (stats_on) ++st.polls;
  const bool open = menu_open_.load();
  const bool fcm = freecam::wants_mouse() && !open;   // the free camera flies with the mouse: confine and recenter the cursor like mouse-look
  const bool lock = (map_.lock_cursor && !open && (!map_.mouse_look || raw_.in_ride)) || fcm;   // mouse look confines the cursor on a ride only (menus: free cursor, touch screen)
  const bool recenter = (lock && map_.mouse_look) || fcm;
  backend_->set_cursor_mode(lock, recenter);
  RawInput r;
  backend_->poll(r);
  if (g_script_gyro_on) { r.gyro_present = true; r.gyro_y = g_script_gyro_yaw; r.gyro_x = g_script_gyro_pitch; }   // scripted `gyro=` token (applied one poll late)
  if (env_fx().fake) { r.gyro_present = true; r.gyro_y = env_fx().fake_yaw; r.gyro_x = env_fx().fake_pitch; }   // SNAP_GYRO_FAKE
  cursor_hidden_ = recenter && r.focused;
  {   // ride & UI scene flags (rail reports and UI scene open/close from rt_gm_neo_speed / rt_gm_route_ui_*)
    route_enable();
    std::lock_guard<std::mutex> rl(g_rs_mu);
    r.in_ride = g_rail_cur >= 0 && (int)nvn::g_presentCount - g_rail[g_rail_cur].f_adv <= 20;
  }
  // injected sources (tests) and wheel pulses
  r.keys |= inject_.keys | g_script_keys;
  r.pad |= inject_.pad | g_script_pad;
  r.mouse_buttons |= inject_.mouse_buttons | g_script_mouse;
  if (g_script_mpos_on) { r.mouse_in_window = true; r.mouse_x = g_script_mx; r.mouse_y = g_script_my; }
  if (inject_.pad.any() || g_script_pad.any()) r.pad_connected = true;
  g_wheel_delta.exchange(0);
  wheel_up_until_ns_ = 0;
  wheel_down_until_ns_ = 0;
  {   // focus guard: switching to a snipping tool or another app must not press anything in the game when the window comes back
    static const bool guard_on = !(std::getenv("SNAP_FOCUS_GUARD") && std::atoi(std::getenv("SNAP_FOCUS_GUARD")) == 0);
    static const int kEsc = key_from_name("Escape");
    const bool foc = r.focused && win_focus_.load();
    if (guard_on) {
      if (!foc) {   // nothing typed or clicked while the window is in the background reaches the game
        r.keys.reset();
        r.mouse_buttons = 0;
        r.mouse_in_window = false;
        r.focused = false;
      } else {
        if (!guard_prev_) {   // the focus just came back: whatever is down now was pressed for another window
          guard_keys_ = r.keys;
          guard_mouse_ = r.mouse_buttons;
          guard_until_ns_ = t + 300000000ull;
          log("input: focus is back, ignoring %d key(s) and mouse buttons %#x until released; Esc and the left button for 300 ms", (int)guard_keys_.count(), (unsigned)guard_mouse_);
        }
        guard_keys_ &= r.keys;
        guard_mouse_ &= r.mouse_buttons;
        r.keys &= ~guard_keys_;
        r.mouse_buttons &= (uint8_t)~guard_mouse_;
        if (t < guard_until_ns_) {
          if (kEsc > 0) r.keys.reset((size_t)kEsc);
          r.mouse_buttons &= (uint8_t)~1u;
        }
      }
      guard_prev_ = foc;
    }
  }
  if (r.focused) {
    if (t < wheel_up_until_ns_) r.mouse_buttons |= 1u << kMouseWheelUp;
    if (t < wheel_down_until_ns_) r.mouse_buttons |= 1u << kMouseWheelDown;
  }
  {   // which device was used last (button pictures: key caps for the keyboard and mouse, pad pictures for a pad)
    static bool kbm_last = false;
    const bool pad_act = r.pad.any() || std::fabs(r.lx) > 0.5f || std::fabs(r.ly) > 0.5f || std::fabs(r.rx) > 0.5f || std::fabs(r.ry) > 0.5f;
    const bool kbm_act = r.focused && (r.keys.any() || r.mouse_buttons != 0);
    if (pad_act) kbm_last = false; else if (kbm_act) kbm_last = true;
    icons::set_last_input(kbm_last);
  }
  // mouse-look per-frame displacement: consumed once per nvn::g_presentCount frame, never reused across frames.
  if (stats_on && (r.mouse_dx != 0 || r.mouse_dy != 0)) { ++st.mouse_events; st.px += std::fabs(r.mouse_dx) + std::fabs(r.mouse_dy); }
  const int cur_present = (int)nvn::g_presentCount;
  const bool new_present = (cur_present != last_mouse_present_);
  if (!r.focused) {
    frame_dx_ = 0.0f;
    frame_dy_ = 0.0f;
    frame_dt_s_ = 1.0f / 60.0f;
    sm_dx_ = 0.0f;
    sm_dy_ = 0.0f;
    sm_mouse_ns_ = 0;
    last_mouse_present_ = cur_present;
    last_mouse_present_ns_ = 0;
  } else if (new_present) {
    const float raw_dt = last_mouse_present_ns_ ? (float)(t - last_mouse_present_ns_) * 1e-9f : (1.0f / 60.0f);
    frame_dt_s_ = (raw_dt >= 0.001f && raw_dt <= 0.1f) ? raw_dt : (1.0f / 60.0f);
    last_mouse_present_ns_ = t;
    frame_dx_ = r.mouse_dx;
    frame_dy_ = r.mouse_dy;
    last_mouse_present_ = cur_present;
  } else if (r.mouse_dx != 0.0f || r.mouse_dy != 0.0f) {
    // Accumulate deltas in the same direction; on rapid reversal after strong motion (>=16 px then >=8 px opposite), drop stale delta.
    if ((frame_dx_ >= 16.0f && r.mouse_dx <= -8.0f) || (frame_dx_ <= -16.0f && r.mouse_dx >= 8.0f)) frame_dx_ = r.mouse_dx;
    else frame_dx_ += r.mouse_dx;
    if ((frame_dy_ >= 16.0f && r.mouse_dy <= -8.0f) || (frame_dy_ <= -16.0f && r.mouse_dy >= 8.0f)) frame_dy_ = r.mouse_dy;
    else frame_dy_ += r.mouse_dy;
  }
  if (map_.mouse_smoothing <= 0.001f) {
    r.mouse_frame_dx = r.focused ? frame_dx_ : 0.0f;
    r.mouse_frame_dy = r.focused ? frame_dy_ : 0.0f;
    sm_dx_ = 0.0f;
    sm_dy_ = 0.0f;
    sm_mouse_ns_ = 0;
  } else {
    if (new_present || sm_mouse_ns_ == 0) {
      const float dt = sm_mouse_ns_ ? (float)(t - sm_mouse_ns_) * 1e-9f : (1.0f / 60.0f);
      const float tau = std::clamp(map_.mouse_smoothing, 0.0f, 1.0f) * 0.06f;
      sm_dx_ = ema_step(sm_dx_, r.focused ? frame_dx_ : 0.0f, dt > 0.25f ? 1.0f : dt, dt > 0.25f ? 0.0f : tau);
      sm_dy_ = ema_step(sm_dy_, r.focused ? frame_dy_ : 0.0f, dt > 0.25f ? 1.0f : dt, dt > 0.25f ? 0.0f : tau);
      sm_mouse_ns_ = t;
    }
    r.mouse_frame_dx = sm_dx_;
    r.mouse_frame_dy = sm_dy_;
  }
  r.mouse_vx = r.mouse_frame_dx / frame_dt_s_;
  r.mouse_vy = r.mouse_frame_dy / frame_dt_s_;
  {   // gyro smoothing (off by default): a one-pole filter on the rate, time constant up to 60 ms
    const float sm = std::clamp(pad_fx().gyro_smooth.load(), 0.0f, 1.0f);
    if (r.gyro_present && sm > 0.0f) {
      const float dt = gyro_sm_t_ ? (float)(t - gyro_sm_t_) * 1e-9f : 1.0f;
      gyro_sm_x_ = ema_step(gyro_sm_x_, r.gyro_x, dt > 0.25f ? 1.0f : dt, dt > 0.25f ? 0.0f : sm * 0.06f);
      gyro_sm_y_ = ema_step(gyro_sm_y_, r.gyro_y, dt > 0.25f ? 1.0f : dt, dt > 0.25f ? 0.0f : sm * 0.06f);
      r.gyro_x = gyro_sm_x_; r.gyro_y = gyro_sm_y_;
      gyro_sm_t_ = t;
    } else { gyro_sm_t_ = 0; }
  }
  raw_ = r;
  icons::set_pad((icons::Family)r.pad_family, r.pad_connected);
  // chord: hold Back + Start for 0.5 s toggles the menu (read from the physical pad, never remapped)
  const bool chord = r.focused && r.pad.test(kPadBack) && r.pad.test(kPadStart);
  if (!chord) { chord_since_ns_ = 0; chord_fired_ = false; }
  else if (!chord_since_ns_) chord_since_ns_ = t;
  else if (!chord_fired_ && t - chord_since_ns_ >= 500000000ull) { chord_fired_ = true; toggle_req_ = true; }
  {  // screenshot: F12, or View + R3 on the physical pad (edge-triggered; the game still sees the two buttons)
    static const int kF12 = key_from_name("F12");
    static bool prev = false;
    const bool down = r.focused && ((kF12 > 0 && r.keys.test((size_t)kF12)) || (r.pad.test(kPadBack) && r.pad.test(kPadRStick)));
    if (down && !prev) g_shot_req = true;
    prev = down;
  }
  {  // save states (docs/save-states.md): F5 saves the one slot, F8 loads it; edge-triggered, the worker thread does the work
    static const int kF5 = key_from_name("F5"), kF8 = key_from_name("F8");
    static bool prev5 = false, prev8 = false;
    const bool d5 = r.focused && kF5 > 0 && r.keys.test((size_t)kF5), d8 = r.focused && kF8 > 0 && r.keys.test((size_t)kF8);
    if (d5 && !prev5) snap_state_request(1, "player");
    if (d8 && !prev8) snap_state_request(2, "player");
    prev5 = d5; prev8 = d8;
  }
  // rebinding capture
  if (capturing_.load()) {
    for (size_t i = 0; i < cap_ignore_.size();) {   // forget held sources once released
      Source s{(Source::Kind)(cap_ignore_[i] >> 8), (int)(cap_ignore_[i] & 0xff)};
      if (!source_down(s, r, 0.6f)) cap_ignore_.erase(cap_ignore_.begin() + (long)i); else ++i;
    }
    if (!cap_have_) {
      each_candidate([&](const Source& s) {
        if (cap_have_ || !capture_allowed(s, cap_mask_) || !source_down(s, r, 0.6f)) return;
        if (std::find(cap_ignore_.begin(), cap_ignore_.end(), src_key(s)) != cap_ignore_.end()) return;
        cap_have_ = true; cap_src_ = s;
      });
      if (cap_have_) capturing_ = false;
      else if (t > cap_deadline_ns_) { capturing_ = false; cap_timeout_ = true; }
    }
  }
  r.zoom_latched = (tog_state_ & (1ull << kZL)) != 0;
  PadSample p = map_pad(map_, r);
  {   // SNAP_FOCUS_GUARD_LOG=1 (tests): every Plus and B press the game is about to see, with the focus state
    static const bool plog = std::getenv("SNAP_FOCUS_GUARD_LOG") != nullptr;
    static uint64_t prev_pb = 0;
    const uint64_t pb = p.buttons & ((1ull << kPlus) | (1ull << kB));
    if (plog && (pb & ~prev_pb)) log("input: %s pressed (focused=%d in_ride=%d present=%d)", (pb & (1ull << kPlus)) ? "Plus" : "B", (int)r.focused, (int)r.in_ride, nvn::g_presentCount);
    prev_pb = pb;
  }
  // toggle mode: a press of a toggle button (ZL / ZR switches, the accessibility.toggle_buttons list, SNAP_ACCESS_TOGGLE) flips it on or off instead of holding it
  // SNAP_ACCESS_TOGGLE_SCRIPT=1 (test runs): latch what the input script presses instead of the physical pad, so a route can exercise the latch.
  static const bool script_latch = std::getenv("SNAP_ACCESS_TOGGLE_SCRIPT") != nullptr;
  const uint64_t tmask = toggle_mask_effective(map_);
  auto latch = [&](uint64_t& buttons) {
    tog_state_ &= tmask; tog_prev_ &= tmask;
    if (!tmask) return;
    const uint64_t before = tog_state_, down = buttons & tmask;
    tog_state_ ^= down & ~tog_prev_;
    tog_prev_ = down;
    buttons = (buttons & ~tmask) | tog_state_;
    if (tog_state_ != before) log("access: toggle latch %#llx -> %#llx present=%d", (unsigned long long)before, (unsigned long long)tog_state_, nvn::g_presentCount);
  };
  if (!script_latch) latch(p.buttons);
  if (std::chrono::steady_clock::now().time_since_epoch().count() < g_load_release_until_ns.load(std::memory_order_relaxed)) { p = PadSample{}; p.connected = true; }   // just after a save-state load
  preview_ = p;
  pad_ = p;
  touch_ = map_touch(map_, r);
  if (open) {   // the menu owns the input: the game sees a connected, idle pad and no touch
    pad_ = PadSample{};
    pad_.connected = true;
    touch_ = TouchSample{};
  }
  if (!g_script.loaded) { script_load(); g_script.t0 = t; g_script.cur_t0 = t; g_script.cur_f0 = nvn::g_presentCount; }
  if (!g_script.e.empty()) {
    script_apply(t, pad_);
    static float sr_lo = 0, sr_hi = 0;
    if (g_script_rumble_lo != sr_lo || g_script_rumble_hi != sr_hi) {   // scripted host rumble (`rumble=lo,hi` token)
      sr_lo = g_script_rumble_lo; sr_hi = g_script_rumble_hi;
      apply_rumble(backend_.get(), sr_lo, sr_hi, "script", pad_.buttons, true);
    }
  }
  if (script_latch && !open) latch(pad_.buttons);
  if (!open) {   // host-side presses (host/mods/skips.cpp, SNAP_SKIP_CUTSCENES): OR-ed onto the pad like a script entry
    const uint64_t inj = g_inject_buttons.load(std::memory_order_relaxed);
    if (inj && std::chrono::steady_clock::now().time_since_epoch().count() < g_inject_until_ns.load(std::memory_order_relaxed)) { pad_.buttons |= inj; pad_.connected = true; }
  }
  freecam::poll(r, pad_, touch_, open, t);   // free camera on: hotkeys, accumulated movement, and an idle pad for the game
  if (aot_camera_enabled()) {
    if (map_.mouse_look && r.focused && r.in_ride && !open) {
      const float sx = map_.mouse_sensitivity * map_.mouse_sens_x * (map_.mouse_invert_x ? -1.0f : 1.0f);
      const float sy = map_.mouse_sensitivity * map_.mouse_sens_y * (map_.mouse_invert_y ? -1.0f : 1.0f);
      const bool zoom_on = (pad_.buttons & (1ull << kZL)) != 0;
      post_aot_mouse_delta(r.mouse_dx * sx, r.mouse_dy * sy, zoom_on);
    } else {
      reset_aot_mouse_delta();
    }
  }
  g_ui_in_ride.store(r.in_ride, std::memory_order_relaxed);
  g_ui_menu_open.store(open, std::memory_order_relaxed);
  g_ui_focused.store(r.focused, std::memory_order_relaxed);
  const bool stick_or_dpad =
      std::fabs((float)pad_.lx) > (float)kStickMax * 0.12f ||
      std::fabs((float)pad_.ly) > (float)kStickMax * 0.12f ||
      (pad_.buttons & ((1ull << kUp) | (1ull << kDown) | (1ull << kLeft) | (1ull << kRight))) != 0;
  if (r.in_ride || open || !r.focused || stick_or_dpad) {
    g_mouse_ui_active.store(false, std::memory_order_relaxed);
  }
  {
    static const bool dbg = std::getenv("SNAP_MOUSE_DEBUG") && std::atoi(std::getenv("SNAP_MOUSE_DEBUG")) != 0;
    if (dbg) {
      static int dbg_n = 0;
      const bool ride_move = r.in_ride && (r.mouse_dx != 0.0f || r.mouse_dy != 0.0f || r.mouse_frame_dx != 0.0f || r.mouse_frame_dy != 0.0f);
      if (ride_move) {
        const float sx = map_.mouse_sensitivity * map_.mouse_sens_x;
        const float sy = map_.mouse_sensitivity * map_.mouse_sens_y;
        const float pre_x = (r.mouse_vx * sx / (float)kMouseLookFullPxPerSec) * (map_.mouse_invert_x ? -1.f : 1.f);
        const float pre_y = (-r.mouse_vy * sy / (float)kMouseLookFullPxPerSec) * (map_.mouse_invert_y ? -1.f : 1.f);
        const float pmag = std::sqrt(pre_x * pre_x + pre_y * pre_y);
        const float post_x = pmag > 1.0f ? (pre_x / pmag) : pre_x;
        const float post_y = pmag > 1.0f ? (pre_y / pmag) : pre_y;
        const float ang_vel = std::atan2(-r.mouse_vy, r.mouse_vx) * 57.2957795f;
        const float ang_hid = (pad_.rx != 0 || pad_.ry != 0) ? (std::atan2((float)pad_.ry, (float)pad_.rx) * 57.2957795f) : 0.0f;
        const char* ds_env = std::getenv("SNAP_MOUSE_DUAL_STICK");
        const int dual_on = (ds_env && std::atoi(ds_env) != 0) ? 1 : 0;
        const int zoom_on = (pad_.buttons & (1ull << kZL)) ? 1 : 0;
        log("mouse_dbg[pipe]: ts_ns=%llu pres=%d ev=%u dual=%d zoom=%d accum=(%.1f,%.1f) frame=(%.1f,%.1f) dt=%.4f vel=(%.1f,%.1f) pre=(%.3f,%.3f) post=(%.3f,%.3f) hid_r=(%d,%d) hid_l=(%d,%d) ang_vel=%.1f ang_hid=%.1f",
            (unsigned long long)t, cur_present, (unsigned)r.mouse_poll_events, dual_on, zoom_on,
            r.mouse_dx, r.mouse_dy, r.mouse_frame_dx, r.mouse_frame_dy, frame_dt_s_,
            r.mouse_vx, r.mouse_vy, pre_x, pre_y, post_x, post_y,
            (int)pad_.rx, (int)pad_.ry, (int)pad_.lx, (int)pad_.ly, ang_vel, ang_hid);
      } else if ((dbg_n++ % 60) == 0 || touch_.count > 0) {
        log("mouse_dbg[hub]: in_ride=%d focused=%d norm=(%.3f,%.3f) in_win=%d btn=%#x rx=%d ry=%d touch=(cnt=%d %d,%d)",
            (int)r.in_ride, (int)r.focused, r.mouse_x, r.mouse_y, (int)r.mouse_in_window, (unsigned)r.mouse_buttons,
            (int)pad_.rx, (int)pad_.ry, (int)touch_.count, (int)touch_.pt[0].x, (int)touch_.pt[0].y);
      }
    }
  }
  ++seq_;
}
PadSample Hub::pad() const { std::lock_guard<std::mutex> g(mu_); return pad_; }
TouchSample Hub::touch() const { std::lock_guard<std::mutex> g(mu_); return touch_; }
int64_t Hub::sampling_number() const { std::lock_guard<std::mutex> g(mu_); return seq_; }
RawInput Hub::raw() const { std::lock_guard<std::mutex> g(mu_); return raw_; }
// Scaling, logging and the backend call; the caller holds mu_ (Hub::rumble from the game, Hub::poll for scripted `rumble=` tokens).
static void apply_rumble(Backend* be, float low, float high, const char* src, uint64_t buttons, bool log_it) {
  const PadFx& fx = pad_fx();
  const EnvFx& ev = env_fx();
  const float in_lo = low, in_hi = high;
  const float k = ev.rumble_set ? ev.rumble : std::clamp(fx.rumble_strength.load(), 0.0f, 1.5f);
  if (ev.rumble_set ? k <= 0.0f : !fx.rumble_on.load()) { low = high = 0; }
  else { low = std::min(1.0f, low * k); high = std::min(1.0f, high * k); }
  if (ev.rumble_log && log_it) {
    log("rumble: %s low=%.3f high=%.3f -> host low=%.3f high=%.3f (strength %.2f) present=%d buttons=%#llx", src, in_lo, in_hi, low, high, k, nvn::g_presentCount, (unsigned long long)buttons);
  }
  be->rumble(low, high);
}
void Hub::rumble(float low, float high) {
  std::lock_guard<std::mutex> g(mu_);
  if (!backend_ || menu_open_.load()) return;
  static float last_lo = -1, last_hi = -1;   // log only changes (the game repeats its values every frame)
  const bool changed = low != last_lo || high != last_hi;
  last_lo = low; last_hi = high;
  apply_rumble(backend_.get(), low, high, "game", pad_.buttons, changed);
}

Hub& hub() {
  static Hub h;
  return h;
}

}  // namespace hle::input

extern "C" bool snap_ui_pointer_1080(float* gx, float* gy) {
  return hle::input::ui_pointer_1080(gx, gy);
}
