// SPDX-License-Identifier: MIT
// Input pipeline for nn::hid: backend (gamepad / keyboard / mouse) -> RawInput -> remapping table -> Npad sample.
// Pure host code with no guest types so the tests can drive it with a fake backend.
//
// Remapping table: JSON (format version 2), default %APPDATA%\SnapRecomp\input.json, folder overridable with SNAP_CONFIG_DIR.
// The in-game controls menu (runtime/ui) edits it live through Hub::set_mapping and saves it atomically (Hub::save).
// The old config\input_map.json (version 1) is migrated on first load. The schema is documented in docs/hle-services.md and
// kept in Mapping::to_json.
#pragma once
#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json_lite.h"

namespace hle::input {

// ---- Switch side: nn::hid::NpadButton bit numbers (SDK order)
enum Button : int {
  kA = 0, kB, kX, kY, kStickL, kStickR, kL, kR, kZL, kZR, kPlus, kMinus, kLeft, kUp, kRight, kDown,
  kStickLLeft, kStickLUp, kStickLRight, kStickLDown, kStickRLeft, kStickRUp, kStickRRight, kStickRDown,
  kLeftSL, kLeftSR, kRightSL, kRightSR, kButtonCount
};
const char* button_name(int b);  // "A", "B", ... "ZL"
int button_from_name(const std::string& n);  // -1 if unknown
constexpr int kStickMax = 32767;

// ---- host side
// Gamepad buttons are named by position (SDL3 convention), so one table serves XInput and SDL: south = Xbox A.
enum PadButton : int {
  kPadSouth = 0, kPadEast, kPadWest, kPadNorth, kPadBack, kPadGuide, kPadStart, kPadLStick, kPadRStick,
  kPadLShoulder, kPadRShoulder, kPadDUp, kPadDDown, kPadDLeft, kPadDRight, kPadLTrigger, kPadRTrigger,
  // an analog stick pushed past half-way, as a button (so a stick direction can be bound to any Switch button)
  kPadLStickLeft, kPadLStickRight, kPadLStickUp, kPadLStickDown, kPadRStickLeft, kPadRStickRight, kPadRStickUp, kPadRStickDown,
  kPadCount
};
enum MouseBtn : int { kMouseLeft = 0, kMouseRight, kMouseMiddle, kMouseX1, kMouseX2, kMouseWheelUp, kMouseWheelDown, kMouseCount };
const char* mouse_name(int b);   // "left", "right", "middle", "x1", "x2", "wheel_up", "wheel_down"
const char* pad_button_name(int b);
int pad_button_from_name(const std::string& n);

// Keyboard keys by name (letters, digits, F1-F12, arrows, ...). Backends translate host key codes to these ids.
enum Key : int { kKeyNone = 0, kKeyCount = 128 };
const char* key_name(int k);
int key_from_name(const std::string& n);  // case-insensitive; 0 if unknown
int key_from_ascii(char ch);              // 'A'..'Z', '0'..'9'

// What kind of pad is connected (button icon mod: the face button labels follow it). Same numbering as hle::icons::Family.
enum PadFamily : int { kPadFamNone = 0, kPadFamSwitch, kPadFamXbox, kPadFamPlayStation };

struct RawInput {
  bool focused = true;                 // the game window has the keyboard focus
  bool pad_connected = false;
  int pad_family = kPadFamNone;        // PadFamily of the first connected pad (Xbox for XInput; SDL3 reports the type)
  std::bitset<32> pad;                 // PadButton bits (triggers are also given as axes)
  float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, y up positive
  float lt = 0, rt = 0;                // 0..1
  std::bitset<kKeyCount> keys;
  bool mouse_in_window = false;
  float mouse_x = 0, mouse_y = 0;      // 0..1 over the window client area
  uint8_t mouse_buttons = 0;           // MouseBtn bits: left, right, middle, x1, x2, wheel up and down (a short pulse per notch)
  float mouse_dx = 0, mouse_dy = 0;    // pixels moved since the previous backend poll
  float mouse_vx = 0, mouse_vy = 0;    // pixels per second (legacy telemetry)
  float mouse_frame_dx = 0, mouse_frame_dy = 0;  // per-frame accumulated displacement (used by mouse-look); y down positive
  uint32_t mouse_poll_events = 0;      // raw mouse events drained in this poll
  bool in_ride = false;                // set by the Hub: a rail is advancing (ride); gates the mouse-look click bindings
  bool zoom_latched = false;           // set by the Hub when ZL toggle-mode latch is active
  bool gyro_present = false;           // a connected pad reports a gyroscope (SDL3 sensor API)
  float gyro_x = 0, gyro_y = 0, gyro_z = 0;   // angular rate in deg/s, SDL axes: x = pitch (positive tips the front up), y = yaw (positive turns left), z = roll
};

constexpr float kGyroDeadDegPerSecDefault = 1.5f;
// Controller extras, set from the profile settings (runtime/hle/settings.cpp) and read on every poll.
struct PadFx {
  std::atomic<bool> gyro_on{false};              // gyro drives the right (camera) stick, added to the stick and mouse-look
  std::atomic<float> gyro_sens{1.0f};            // 1 = full stick deflection at kGyroFullDegPerSec
  std::atomic<bool> gyro_invert_x{false}, gyro_invert_y{false};
  std::atomic<bool> rumble_on{true};
  std::atomic<float> rumble_strength{1.0f};      // 0..1.5 scale on both motors
  // Aim feel (docs/aim-feel.md); every default reproduces the behavior before these options existed.
  std::atomic<int> stick_curve{0};               // analog stick response after the dead zone: 0 linear, 1 classic (t^1.5), 2 precise (t^2.2)
  std::atomic<float> stick_deadzone{-1.0f};      // < 0 follows the input.json dead zone of each stick; 0..0.6 overrides both
  std::atomic<float> gyro_smooth{0.0f};          // 0 off .. 1 (time constant up to 60 ms)
  std::atomic<float> gyro_dead{kGyroDeadDegPerSecDefault};   // deg/s ignored as sensor noise
  std::atomic<bool> gyro_avg{false};             // sample the gyro on its own thread and give the game the average since its last read
  std::atomic<bool> raw_mouse{false};            // mouse-look deltas from WM_INPUT (no pointer acceleration, nothing lost between reads)
  std::atomic<float> mouse_window_ms{40.0f};     // mouse-look velocity window with smoothing off (8..80 ms)
  std::atomic<int> swap_ab{-1};                  // A/B swap override from SNAP_SWAP_AB / the swap_ab setting: -1 follow input.json, 0 off, 1 on
};
PadFx& pad_fx();
bool rumble_log_enabled();
bool gyro_aim_enabled();     // profile gyro_on or SNAP_GYRO   // SNAP_RUMBLE_LOG=1
constexpr float kGyroFullDegPerSec = 90.0f;     // turn rate that gives full stick deflection at sensitivity 1
constexpr float kGyroDeadDegPerSec = kGyroDeadDegPerSecDefault;      // below this the sensor noise is ignored
// Gyro rate to camera stick (-1..1, y up positive). Pure; used by map_pad and the tests.
void gyro_to_stick(float yaw_rate_left_pos_dps, float pitch_rate_up_pos_dps, float sens, bool inv_x, bool inv_y, float* sx, float* sy,
                   float dead_dps = kGyroDeadDegPerSecDefault);

// ---- aim feel helpers (pure; docs/aim-feel.md)
enum StickCurve : int { kCurveLinear = 0, kCurveClassic, kCurvePrecise };
int stick_curve_from_name(const std::string& n);   // "linear" | "classic" | "precise" (or 0/1/2); unknown -> linear
// Response of a stick whose dead zone is already removed: t in 0..1 -> 0..1. Linear is the identity (what the game always had).
float apply_stick_curve(int curve, float t);
// Mouse-look velocity: movement samples kept for a window; the velocity is the sum over the window divided by its length.
struct VelWindow {
  struct S { uint64_t t; float dx, dy; };
  std::vector<S> v;
  void add(uint64_t t_ns, float dx, float dy) { v.push_back({t_ns, dx, dy}); }
  void velocity(uint64_t t_ns, uint64_t win_ns, float* vx, float* vy);   // pixels per second; drops samples older than 400 ms
};
// Raw mouse deltas posted by the window thread (WM_INPUT) and drained by the poll: nothing is lost between reads.
struct DeltaAccum {
  std::mutex mu;
  int64_t dx_sum = 0, dy_sum = 0;
  uint64_t batch_events = 0;
  std::atomic<uint64_t> events{0};
  void add(int x, int y) {
    std::lock_guard<std::mutex> lk(mu);
    // On rapid direction reversal after strong motion (>=16 px followed by >=8 px opposite), discard stale pre-reversal delta.
    if ((dx_sum >= 16 && x <= -8) || (dx_sum <= -16 && x >= 8)) dx_sum = x; else dx_sum += x;
    if ((dy_sum >= 16 && y <= -8) || (dy_sum <= -16 && y >= 8)) dy_sum = y; else dy_sum += y;
    ++batch_events;
    events.fetch_add(1, std::memory_order_relaxed);
  }
  void take(float* x, float* y, uint64_t* n_events = nullptr) {
    std::lock_guard<std::mutex> lk(mu);
    *x = (float)dx_sum; *y = (float)dy_sum;
    if (n_events) *n_events = batch_events;
    dx_sum = 0; dy_sum = 0; batch_events = 0;
  }
};
// Gyro samples gathered on a fast thread; the game's read gets their mean since its previous read (or the last sample if none arrived).
struct GyroAvg {
  double sx = 0, sy = 0, sz = 0; int n = 0; float lx = 0, ly = 0, lz = 0;
  void add(float x, float y, float z) { sx += x; sy += y; sz += z; ++n; lx = x; ly = y; lz = z; }
  void take(float* x, float* y, float* z) {
    if (n > 0) { *x = (float)(sx / n); *y = (float)(sy / n); *z = (float)(sz / n); } else { *x = lx; *y = ly; *z = lz; }
    sx = sy = sz = 0; n = 0;
  }
};
// One-pole low pass on the gyro rate: tau 0 passes the input through; dt is the time since the last step.
float ema_step(float prev, float in, float dt_s, float tau_s);
// Window thread entry for raw mouse input (WM_INPUT, relative motion) and its state for the backend.
void post_raw_mouse(int dx, int dy);
DeltaAccum& raw_mouse();
// Window-thread absolute cursor position (0..1) updated continuously from SDL_EVENT_MOUSE_MOTION.
void post_mouse_pos(float nx, float ny, bool in_window);
bool read_posted_mouse_pos(float* nx, float* ny, bool* in_window);
// Returns true when the mouse recently moved outside a ride/overlay and converts it to 1920x1080 UI coordinates.
bool ui_pointer_1080(float* gx, float* gy);
// Desired cursor mode reported by Sdl3Backend::set_cursor_mode() for the main/window thread to apply.
void set_sdl3_cursor_mode(bool lock, bool recenter);
void sdl3_cursor_mode(bool* lock, bool* recenter);
// QA26 AOT Ride camera direct mouse-look bridge (active only when SNAP_MOUSE_AOT_CAMERA=1).
bool aot_camera_enabled();
void post_aot_mouse_delta(float dx, float dy, bool zoom_active);
bool take_aot_mouse_delta(float* dx, float* dy, bool* zoom_active);
void reset_aot_mouse_delta();

class Backend {
 public:
  virtual ~Backend() = default;
  virtual const char* name() const = 0;
  virtual void poll(RawInput& out) = 0;
  virtual void rumble(float low, float high) { (void)low; (void)high; }  // 0..1 each
  // Cursor policy, set by the Hub before every poll: `lock` confines the cursor to the game window (only while it has the focus),
  // `recenter` also re-centers it after every poll and reports mouse_dx/dy relative to the center (mouse-look).
  virtual void set_cursor_mode(bool lock, bool recenter) { (void)lock; (void)recenter; }
};
std::unique_ptr<Backend> make_default_backend();  // SDL3 if built with HLE_WITH_SDL3, else Win32 XInput/keyboard

// ---- mapping
struct Source {
  enum Kind { None, Pad, KeyboardKey, Mouse } kind = None;
  int id = 0;  // PadButton / Key / MouseBtn
  bool operator==(const Source& o) const { return kind == o.kind && id == o.id; }
};
bool parse_source(const std::string& s, Source* out);
std::string source_to_string(const Source& s);

enum Device : int { kDevKeyboard = 0, kDevMouse, kDevController, kDevCount };
const char* device_name(Device d);          // "Keyboard", "Mouse", "Controller"
Device device_of(const Source& s);          // None counts as Keyboard (never stored)
std::string source_label(const Source& s);  // for the UI: "W", "Left Shift", "Mouse Left", "Wheel Up", "South (Xbox A)"

struct StickMap {
  std::string analog = "left";                      // which physical stick drives it: "left", "right" or "none"
  std::vector<Source> up, down, left, right;        // digital sources (keyboard, d-pad, mouse button ...)
  float deadzone = 0.15f;                           // radial inner dead zone, applied to the analog source
  float outer_deadzone = 1.0f;                      // magnitude at which the analog source reaches full deflection (0.5..1)
  bool invert_y = false;                            // analog source only
};

// The bindable actions: 0..15 are the Switch buttons A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down (Button enum),
// then the four directions of the left stick (16..19: up, down, left, right) and of the right stick (20..23).
constexpr int kActionCount = 24;
constexpr int kMaxSlotsPerDevice = 2;
const char* action_label(int action);        // "A", "ZL", "D-pad Up", "Left stick up" ...
constexpr int kMouseLookFullPxPerSec = 2400;
constexpr float kMouseLookFullPxPerFrame = 40.0f;  // per-frame mouse displacement (px) that gives full stick deflection at sensitivity 1

struct Mapping {
  static constexpr int kVersion = 2;
  static constexpr int kDefaultsRev = 2;   // default table revision stored in the file; from_json upgrades an untouched older table
  std::array<std::vector<Source>, kButtonCount> buttons;
  StickMap left, right;
  // controller
  float zl_threshold = 0.5f, zr_threshold = 0.5f;   // LT/RT analog value that counts as pressed
  float stick_button_threshold = 0.5f;              // virtual StickLLeft..StickRDown bits
  bool swap_sticks = false;                         // the left stick's analog source is the physical right stick and vice versa
  int face_layout = 0;                              // 0 by position (Xbox B = Switch A), 1 Nintendo by label (Xbox A = Switch A)
  bool swap_ab = false;                             // confirm and back swapped on the controller only (the A and B bindings trade their pad sources at run time; keyboard and mouse stay); see swap_ab_effective
  // mouse
  bool mouse_touch = true;                          // left mouse button drives the touch screen
  bool mouse_look = true;                           // mouse movement drives the right stick during a ride (menus get a free cursor and the touch screen)
  float mouse_sensitivity = 1.0f;                   // 1 = full deflection at kMouseLookFullPxPerSec
  float mouse_sens_x = 1.0f, mouse_sens_y = 1.0f;   // independent horizontal/vertical multipliers
  bool mouse_invert_x = false, mouse_invert_y = false;
  float mouse_smoothing = 0.0f;                     // 0 off .. 1 (averaging window 40 ms .. 340 ms)
  std::vector<Source> look_a, look_zl;              // mouse-look click bindings: pressed during a ride while mouse_look is on (defaults: left = A, right = ZL; []: none)
  bool lock_cursor = true;                          // confine the cursor to the window while playing (not while the menu is open)
  // accessibility
  bool toggle_zl = false, toggle_zr = false;        // a press flips the trigger on or off instead of holding it
  uint32_t toggle_buttons = 0;                      // the same for any other button: bit (1 << Button) for A B X Y L R (input.json accessibility.toggle_buttons, SNAP_ACCESS_TOGGLE adds more)
  float ui_scale = 1.0f;
  static Mapping defaults();
  Json to_json() const;
  bool from_json(const Json& j, std::string* err);  // starts from defaults(): keys not present keep their default; version 1 files load too
};

std::vector<Source>& action_sources(Mapping& m, int action);
const std::vector<Source>& action_sources(const Mapping& m, int action);
// Slot editing (a slot is the 0th or 1st binding of one device on one action).
Source slot_get(const std::vector<Source>& v, Device d, int slot);              // kind None when empty
int slot_count(const std::vector<Source>& v, Device d);
bool slot_set(std::vector<Source>& v, Device d, int slot, const Source& s);     // false if s is of another device; drops a duplicate in the other slot
void slot_clear(std::vector<Source>& v, Device d, int slot);
std::vector<int> actions_using(const Mapping& m, const Source& s);              // action ids that use s (more than one is a conflict)
void reset_device(Mapping& m, Device d);                                        // defaults for that device's bindings and settings only
void set_face_layout(Mapping& m, int layout);                                   // swaps south/east and west/north in every controller binding
bool swap_ab_effective(const Mapping& m);                                       // the override (pad_fx().swap_ab) when set, else m.swap_ab: controller sources of A press B and the reverse

struct PadSample {
  uint64_t buttons = 0;
  int32_t lx = 0, ly = 0, rx = 0, ry = 0;           // +-32767, y up positive
  bool connected = false;
};
struct TouchPoint { int32_t x, y; };                // 1280x720 panel coordinates
struct TouchSample { int count = 0; TouchPoint pt[1]; };

uint32_t parse_toggle_list(const std::string& list);       // "ZL,Y a" -> bit mask of Buttons that can toggle (A B X Y L R ZL ZR); unknown names ignored
uint32_t toggle_mask_effective(const Mapping& m);          // m.toggle_zl / toggle_zr / toggle_buttons plus the SNAP_ACCESS_TOGGLE list
PadSample map_pad(const Mapping& m, const RawInput& in);   // stateless (no toggle mode): what the Hub computes before suppression
bool source_down(const Source& s, const RawInput& in, float trigger_threshold = 0.5f);
TouchSample map_touch(const Mapping& m, const RawInput& in);
// Window position (0..1 over the client area) -> 1920x1080 interface pixels: the renderer sets this (nvn_hud.cpp) so mouse touch follows the blit
// rectangle (letterbox, ultrawide) and the HUD scale. Null (tests, no renderer): the window is the whole 16:9 interface.
extern bool (*g_window_to_guest)(float nx, float ny, float* gx, float* gy);

// ---- the hub the nn::hid shims read
class Hub {
 public:
  Hub();
  void set_backend(std::unique_ptr<Backend> b);
  void set_mapping(const Mapping& m);   // live: the next poll uses it
  Mapping mapping() const;
  // Reads the JSON table; on a missing file writes the defaults there. Returns false (and keeps the old table) if invalid.
  bool load_mapping(const std::string& path, std::string* err);
  // Startup load with the user-facing rules: `path` (input.json) if present, else the version 1 file `legacy` (migrated and saved
  // to `path`), else defaults. An invalid file is kept as <path>.invalid, the defaults are used and message() says so.
  // Returns false when the defaults were used because of an error.
  bool load_config(const std::string& path, const std::string& legacy);
  bool save(std::string* err = nullptr);                 // atomic write of the current mapping to the loaded path
  std::string config_path() const;
  std::string message() const;                           // last load / save problem or note ("" none)
  uint64_t message_serial() const;                       // increments whenever message() changes
  // Game input is suppressed while the menu is open (the npad reads neutral); the mapped state stays visible as preview().
  void set_menu_open(bool open);
  // Window focus as the window thread sees it (host/window_opts.cpp: WM_ACTIVATE / WM_SETFOCUS / WM_KILLFOCUS). While it is false, or the backend says the window has
  // no keyboard focus, no key or mouse button reaches the game; after it comes back, keys and buttons that are already down stay ignored until released, and Esc and
  // the left button stay ignored for 300 ms (the Esc that closes a snipping tool, the click that brings the window back). SNAP_FOCUS_GUARD=0 turns this off.
  void set_window_focus(bool focused);
  bool menu_open() const { return menu_open_.load(); }
  bool consume_menu_toggle();                            // true once per Back+Start chord held 0.5 s
  bool cursor_hidden() const { return cursor_hidden_.load(); }
  PadSample preview() const;                             // mapped pad state ignoring the suppression
  // Rebinding: after capture_begin the next newly pressed source of an allowed device (bit per Device) is reported once.
  // Sources already held at the start are ignored until released. F1 is never reported (it opens the menu). 10 s timeout.
  void capture_begin(unsigned device_mask);
  void capture_cancel();
  bool capturing() const { return capturing_.load(); }
  bool capture_result(Source* out);                      // true once a source was caught
  bool capture_timed_out();                              // true once after the timeout
  // Test hook (SNAP_UI_TEST): hold a source down in addition to the real devices.
  void inject_source(const Source& s, bool down);
  // Polls the backend if the last poll is older than `min_interval_ms` and returns the newest samples.
  void poll(int min_interval_ms = 1);
  PadSample pad() const;
  TouchSample touch() const;
  int64_t sampling_number() const;
  void rumble(float low, float high);
  RawInput raw() const;
 private:
  mutable std::mutex mu_;
  std::unique_ptr<Backend> backend_;
  Mapping map_;
  RawInput raw_;
  PadSample pad_, preview_;
  TouchSample touch_;
  int64_t seq_ = 0;
  uint64_t last_poll_ns_ = 0;
  std::string path_, message_;
  uint64_t message_serial_ = 0;
  std::atomic<bool> menu_open_{false}, cursor_hidden_{false}, toggle_req_{false}, capturing_{false};
  uint64_t tog_state_ = 0, tog_prev_ = 0;   // toggle-mode buttons (one bit per Button: ZL, ZR and the accessibility.toggle_buttons list)
  VelWindow msamples_;                                                  // kept for Aim Feel tests
  int last_mouse_present_ = -1;                                         // last nvn::g_presentCount that updated frame_dx_/frame_dy_
  uint64_t last_mouse_present_ns_ = 0;                                  // timestamp of the last present transition for frame dt
  float frame_dt_s_ = 1.0f / 60.0f;                                     // measured frame duration in seconds (clamped)
  float frame_dx_ = 0.0f, frame_dy_ = 0.0f;                             // per-frame accumulated mouse displacement
  float sm_dx_ = 0.0f, sm_dy_ = 0.0f;                                   // optional EMA smoothing state
  uint64_t sm_mouse_ns_ = 0;
  float gyro_sm_x_ = 0, gyro_sm_y_ = 0; uint64_t gyro_sm_t_ = 0;        // gyro smoothing state
  uint64_t chord_since_ns_ = 0;                                         // Back+Start
  bool chord_fired_ = false;
  unsigned cap_mask_ = 0;                                               // capture
  uint64_t cap_deadline_ns_ = 0;
  std::vector<uint32_t> cap_ignore_;
  bool cap_have_ = false, cap_timeout_ = false;
  Source cap_src_;
  RawInput inject_;                                                     // injected sources (test hook)
  uint64_t wheel_up_until_ns_ = 0, wheel_down_until_ns_ = 0;
  std::atomic<bool> win_focus_{true};                                   // set_window_focus
  bool guard_prev_ = true;                                              // the last poll saw the window focused (false after a blur, even one no poll saw)
  std::bitset<kKeyCount> guard_keys_;                                   // keys that were down when the focus came back: ignored until released
  uint8_t guard_mouse_ = 0;                                             // the same for the mouse buttons
  uint64_t guard_until_ns_ = 0;                                         // Esc and the left button are ignored until then
  void set_message_locked(const std::string& m);
};
Hub& hub();
// Screenshot key (runtime/nvn/nvn_shot.cpp polls it once per present): true once per F12 press, View+R3 press on a controller
// (read from the physical pad, never remapped) or route token "screenshot".
bool consume_screenshot();
// Window-thread entry for the mouse wheel (WM_MOUSEWHEEL delta in 1/120 notches); each notch becomes a 100 ms press of Wheel Up/Down.
void post_wheel(int delta);
// Raises a game event for SNAP_INPUT_SCRIPT "anchor" lines (no effect without a script).
void script_event(const char* name);
// Prints every pad device and then the live pad state for `seconds` (snap_boot pads).
void list_pads(int seconds);

// Writes the default mapping JSON (used on first run and by the tests); returns false on I/O error.
// True when the real pads must be ignored: a scripted run (SNAP_INPUT_SCRIPT) that takes input in the background (SNAP_INPUT_BACKGROUND,
// the hidden test desktop), unless SNAP_INPUT_REAL_PADS is set. Pads are global devices: one in use elsewhere on the PC pressed buttons in route runs.
inline bool script_run_drops_pads(bool background, const char* script, const char* real_pads) {
  return background && script && *script && !(real_pads && *real_pads && *real_pads != '0');
}
// Route line "course <name>" (test routes): the route lines it stands for, or none for an unknown name. On the stage-select map each
// D-pad press is tied to the stage that has focus, so the choice does not depend on timing (see input_core.cpp).
std::vector<std::string> course_steps(const std::string& name);
bool write_default_mapping(const std::string& path);
// Atomic JSON write: <path>.tmp then rename over <path>.
bool write_mapping_file(const Mapping& m, const std::string& path, std::string* err);

}  // namespace hle::input
