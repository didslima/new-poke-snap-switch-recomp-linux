// SPDX-License-Identifier: MIT
// Stage 6C-B1: isolated unit tests for portable HID/Input core (hid.cpp + input_core.cpp).
// Uses a synthetic Backend and synthetic Cpu structs; never initializes SDL3 or executes guest code.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hle/hle.h"
#include "hle/hle_config.h"
#include "hle/input_core.h"
#include "cpu.h"
extern "C" void rt_gm_ride_mouse_cam(Cpu* c);
#include "hle/svc_common.h"

extern "C" void os_get_stats(void*) {}

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

namespace {

struct FakeInputState {
    hle::input::RawInput next;
    float last_rumble_low = -1.0f;
    float last_rumble_high = -1.0f;
    int rumble_calls = 0;
    int poll_calls = 0;
    bool last_lock = false;
    bool last_recenter = false;
    int cursor_mode_calls = 0;
};

class FakeBackend final : public hle::input::Backend {
  public:
    explicit FakeBackend(FakeInputState* st) : st_(st) {}
    const char* name() const override { return "fake"; }
    void poll(hle::input::RawInput& out) override {
        st_->poll_calls++;
        out = st_->next;
    }
    void set_cursor_mode(bool lock, bool recenter) override {
        st_->last_lock = lock;
        st_->last_recenter = recenter;
        st_->cursor_mode_calls++;
    }
    void rumble(float low, float high) override {
        st_->rumble_calls++;
        st_->last_rumble_low = low;
        st_->last_rumble_high = high;
    }
  private:
    FakeInputState* st_;
};

}  // namespace

static void test_hid_registry() {
    const char* const kHidSyms[] = {
        "_ZN2nn3hid14InitializeNpadEv",
        "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",
        "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
        "_ZN2nn3hid15GetNpadStyleSetERKj",
        "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",
        "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",
        "_ZN2nn3hid19GetTouchScreenStateILm6EEEvPNS0_16TouchScreenStateIXT_EEE",
        "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
        "_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE",
        "_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
        "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
        "_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE",
        "_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
    };

    std::unordered_map<std::string, GuestFn> bound;
    std::unordered_map<std::string, bool> is_real;
    struct Ctx {
        std::unordered_map<std::string, GuestFn>* map;
        std::unordered_map<std::string, bool>* real;
    } ctx{&bound, &is_real};

    hle::bind_all([](const char* sym, GuestFn fn, void* u) {
        auto* c = static_cast<Ctx*>(u);
        (*c->map)[sym] = fn;
        (*c->real)[sym] = (hle::find_override(sym) == fn);
    }, &ctx);

    for (const char* sym : kHidSyms) {
        CHECK(hle::find_override(sym) != nullptr);
        CHECK(bound[sym] == hle::find_override(sym));
        CHECK(is_real[sym]);
    }
}

static void test_input_core_and_hid_shims() {
    FakeInputState st;
    st.next.focused = true;
    st.next.pad_connected = true;
    st.next.pad_family = hle::input::kPadFamXbox;

    hle::input::Hub& hub = hle::input::hub();
    hub.set_backend(std::make_unique<FakeBackend>(&st));
    hub.set_mapping(hle::input::Mapping::defaults());
    hub.set_menu_open(false);
    hub.set_window_focus(true);

    // Prevent touching user input.json by ensuring hid sees it already initialized or uses temp config.
    // Calling InitializeNpad triggers ensure_init once; point input_map_path to nonexistent temp path first.
    const std::string orig_input_map = hle::config().input_map_path;
    hle::config().input_map_path = "/tmp/snap_hid_test_nonexistent_input_map.json";
    GuestFn init_npad = hle::find_override("_ZN2nn3hid14InitializeNpadEv");
    CHECK(init_npad != nullptr);
    Cpu c{};
    init_npad(&c);
    hub.set_mapping(hle::input::Mapping::defaults());

    // 1. Buttons, triggers, axes, and radial deadzone (Mapping::defaults: East=A, South=B on Switch).
    st.next.pad.reset();
    st.next.pad.set(hle::input::kPadEast);    // Switch A
    st.next.pad.set(hle::input::kPadLShoulder);
    st.next.lx = 0.05f;   // inside radial 0.15 deadzone -> 0
    st.next.ly = 0.05f;
    st.next.lt = 0.90f;   // ZL
    st.next.rt = 0.10f;   // below trigger threshold
    hub.poll(0);

    auto p = hub.pad();
    CHECK(p.connected);
    CHECK((p.buttons & (1ull << hle::input::kA)) != 0);
    CHECK((p.buttons & (1ull << hle::input::kB)) == 0);
    CHECK((p.buttons & (1ull << hle::input::kL)) != 0);
    CHECK((p.buttons & (1ull << hle::input::kZL)) != 0);
    CHECK((p.buttons & (1ull << hle::input::kZR)) == 0);
    CHECK(p.lx == 0 && p.ly == 0);

    st.next.lx = 0.0f;
    st.next.ly = 0.80f;   // outside radial deadzone -> > 0
    hub.poll(0);
    CHECK(hub.pad().ly > 10000);

    // 2. Swap A/B override (East becomes Switch B).
    const int orig_swap = hle::input::pad_fx().swap_ab.exchange(1);
    hub.poll(0);
    p = hub.pad();
    CHECK((p.buttons & (1ull << hle::input::kB)) != 0);
    CHECK((p.buttons & (1ull << hle::input::kA)) == 0);
    hle::input::pad_fx().swap_ab.store(orig_swap);

    // 3. Button injection via Hub::inject_source.
    hub.inject_source({hle::input::Source::Pad, hle::input::kPadStart}, true);
    hub.poll(0);
    CHECK((hub.pad().buttons & (1ull << hle::input::kPlus)) != 0);
    hub.inject_source({hle::input::Source::Pad, hle::input::kPadStart}, false);

    // 4. Touch via synthetic mouse + GetTouchScreenState<6>.
    st.next.mouse_in_window = true;
    st.next.mouse_buttons = 1;
    st.next.mouse_x = 0.5f;
    st.next.mouse_y = 0.25f;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));   // allow GetTouchScreenState's poll(1ms) to run
    GuestFn get_touch = hle::find_override("_ZN2nn3hid19GetTouchScreenStateILm6EEEvPNS0_16TouchScreenStateIXT_EEE");
    CHECK(get_touch != nullptr);
    uint8_t touch_buf[16 + 6 * 0x28] = {};
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(touch_buf);
    get_touch(&c);
    int32_t touch_count = 0;
    std::memcpy(&touch_count, touch_buf + 8, 4);
    int32_t tx = 0, ty = 0;
    std::memcpy(&tx, touch_buf + 16 + 16, 4);
    std::memcpy(&ty, touch_buf + 16 + 20, 4);
    CHECK(touch_count == 1);
    CHECK(tx == 640 && ty == 180);
    st.next.mouse_buttons = 0;

    // 5. Gyro + SixAxis overrides.
    const bool orig_gyro = hle::input::pad_fx().gyro_on.exchange(true);
    st.next.gyro_present = true;
    st.next.gyro_y = 45.0f;
    hub.poll(0);

    GuestFn is_rest = hle::find_override("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE");
    GuestFn get_six = hle::find_override("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE");
    CHECK(is_rest != nullptr && get_six != nullptr);
    c = Cpu{};
    is_rest(&c);
    CHECK(c.x[0] == 0);

    uint8_t six_buf[0x60 * 2] = {};
    uint32_t six_handle = 1;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(six_buf);
    c.x[1] = 2;
    c.x[2] = reinterpret_cast<uint64_t>(&six_handle);
    get_six(&c);
    float ang_y = 0.0f;
    std::memcpy(&ang_y, six_buf + 0x20, 4);
    CHECK(c.x[0] == 1);
    CHECK(ang_y > 0.1f);
    hle::input::pad_fx().gyro_on.store(orig_gyro);

    // 6. Vibration handles + SendVibrationValue -> synthetic rumble.
    GuestFn get_vib = hle::find_override("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE");
    GuestFn send_vib = hle::find_override("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE");
    CHECK(get_vib != nullptr && send_vib != nullptr);
    uint32_t vib_handles[2] = {};
    uint32_t npad_id = 0;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(vib_handles);
    c.x[1] = 2;
    c.x[2] = reinterpret_cast<uint64_t>(&npad_id);
    c.x[3] = 1;  // FullKey
    get_vib(&c);
    CHECK(c.x[0] == 2);

    float vib_val[4] = {0.6f, 160.0f, 0.4f, 320.0f};
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(&vib_handles[0]);
    c.x[1] = reinterpret_cast<uint64_t>(vib_val);
    send_vib(&c);
    CHECK(st.rumble_calls > 0);
    CHECK(st.last_rumble_low >= 0.59f && st.last_rumble_high >= 0.39f);

    // 7. GetNpadState (valid FullKey ID=0 vs unsupported ID=1) + ShowControllerSupport(nullptr).
    GuestFn get_npad = hle::find_override("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj");
    GuestFn get_style = hle::find_override("_ZN2nn3hid15GetNpadStyleSetERKj");
    GuestFn show_ctrl = hle::find_override("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE");
    CHECK(get_npad != nullptr && get_style != nullptr && show_ctrl != nullptr);

    uint8_t npad_state[0x28] = {};
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(npad_state);
    c.x[1] = reinterpret_cast<uint64_t>(&npad_id);
    get_npad(&c);
    uint64_t npad_btns = 0;
    uint32_t npad_attr = 0;
    std::memcpy(&npad_btns, npad_state + 8, 8);
    std::memcpy(&npad_attr, npad_state + 0x20, 4);
    CHECK((npad_btns & (1ull << hle::input::kA)) != 0);
    CHECK(npad_attr == 1);

    uint32_t bad_id = 1;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(&bad_id);
    get_style(&c);
    CHECK(c.x[0] == 0);

    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(npad_state);
    c.x[1] = reinterpret_cast<uint64_t>(&bad_id);
    get_npad(&c);
    std::memcpy(&npad_btns, npad_state + 8, 8);
    std::memcpy(&npad_attr, npad_state + 0x20, 4);
    CHECK(npad_btns == 0 && npad_attr == 0);

    // Null output pointer to ShowControllerSupport and invalid/zero count to SixAxis must not crash.
    c = Cpu{};
    c.x[0] = 0;
    show_ctrl(&c);
    CHECK(c.x[0] == hle::kSuccess);

    uint32_t bad_six = 0x7fffffffu;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(six_buf);
    c.x[1] = 0;
    c.x[2] = reinterpret_cast<uint64_t>(&bad_six);
    get_six(&c);
    CHECK(c.x[0] == 0);

    // 8. Basic concurrency: repeated poll + read across 2 threads.
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        for (int i = 0; i < 200; ++i) {
            auto ps = hub.pad();
            auto sn = hub.sampling_number();
            (void)ps;
            (void)sn;
        }
        stop.store(true);
    });
    while (!stop.load()) hub.poll(0);
    reader.join();
    CHECK(hub.sampling_number() > 0);

    hle::config().input_map_path = orig_input_map;
}

static void test_aim_feel() {
    using namespace hle::input;

    // 1. Stick curves (linear identity, classic t^1.5, precise t^2.2).
    CHECK(stick_curve_from_name("linear") == kCurveLinear);
    CHECK(stick_curve_from_name("classic") == kCurveClassic);
    CHECK(stick_curve_from_name("precise") == kCurvePrecise);
    CHECK(stick_curve_from_name("unknown") == kCurveLinear);
    CHECK(std::fabs(apply_stick_curve(kCurveLinear, 0.5f) - 0.5f) < 1e-5f);
    CHECK(apply_stick_curve(kCurveClassic, 0.5f) < 0.5f);
    CHECK(apply_stick_curve(kCurvePrecise, 0.5f) < apply_stick_curve(kCurveClassic, 0.5f));
    CHECK(std::fabs(apply_stick_curve(kCurvePrecise, 1.0f) - 1.0f) < 1e-5f);

    // 2. Deadzone override & stick curve in map_pad; device without gyro produces zero gyro contribution.
    Mapping m{};
    m.left.analog = "left";
    m.left.deadzone = 0.15f;
    m.left.outer_deadzone = 0.9f;
    m.right.analog = "right";
    m.right.deadzone = 0.15f;
    m.right.outer_deadzone = 0.9f;
    RawInput raw{};
    raw.lx = 0.25f;
    pad_fx().stick_deadzone.store(0.30f);
    pad_fx().stick_curve.store(kCurveLinear);
    PadSample s0 = map_pad(m, raw);
    CHECK(s0.lx == 0 && s0.ly == 0);

    raw.lx = 0.65f;
    PadSample s1 = map_pad(m, raw);
    CHECK(s1.lx > 0);
    pad_fx().stick_curve.store(kCurvePrecise);
    PadSample s2 = map_pad(m, raw);
    CHECK(s2.lx > 0 && s2.lx < s1.lx);

    // Device without gyro (gyro_present == false) ignores gyro_on and avoids drift.
    pad_fx().gyro_on.store(true);
    raw = RawInput{};
    raw.gyro_present = false;
    raw.gyro_x = 45.0f;
    raw.gyro_y = 45.0f;
    PadSample s_nogyro = map_pad(m, raw);
    CHECK(s_nogyro.rx == 0 && s_nogyro.ry == 0);

    // Gyro deadzone suppresses small noise, passes larger motion.
    float gx = 0.0f, gy = 0.0f;
    gyro_to_stick(1.0f, 1.0f, 1.0f, false, false, &gx, &gy, 2.0f);
    CHECK(gx == 0.0f && gy == 0.0f);
    gyro_to_stick(20.0f, -10.0f, 1.0f, false, false, &gx, &gy, 2.0f);
    CHECK(gx < 0.0f && gy < 0.0f);

    // 3. GyroAvg & VelWindow.
    GyroAvg ga;
    ga.add(10.0f, 20.0f, 30.0f);
    ga.add(30.0f, 40.0f, 50.0f);
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    ga.take(&ax, &ay, &az);
    CHECK(std::fabs(ax - 20.0f) < 1e-4f && std::fabs(ay - 30.0f) < 1e-4f && std::fabs(az - 40.0f) < 1e-4f);

    VelWindow vw;
    vw.add(1000000000ull, 10.0f, 0.0f);
    vw.add(1020000000ull, 10.0f, 0.0f);
    float vx = 0.0f, vy = 0.0f;
    vw.velocity(1020000000ull, 40000000ull, &vx, &vy);
    CHECK(vx > 400.0f && vy == 0.0f);

    // Reset pad_fx defaults.
    pad_fx().stick_deadzone.store(-1.0f);
    pad_fx().stick_curve.store(kCurveLinear);
    pad_fx().gyro_on.store(false);
}

namespace hle::input {
void route_rail(uint64_t obj, float prog);
void route_ui(uint64_t player, const char* scene, const char* state, int event, const char* elem, uint64_t lr);
}
namespace nvn { extern int g_presentCount; }

static void test_mouse_and_keyboard_support() {
    using namespace hle::input;

    Mapping m = Mapping::defaults();
    CHECK(m.mouse_look);
    CHECK(m.mouse_touch);
    CHECK(m.lock_cursor);

    // 1 & 2. Horizontal, vertical, and diagonal mouse velocity during ride -> right stick (rx, ry).
    // At 60 Hz fallback, kMouseLookFullPxPerSec (2400) / 60 = 40.0 px/frame; 20.0 px/frame -> 0.5.
    RawInput in{};
    in.focused = true;
    in.in_ride = true;
    in.mouse_frame_dx = 20.0f;
    in.mouse_frame_dy = 0.0f;
    PadSample s = map_pad(m, in);
    CHECK(s.rx > 14000 && s.rx < 18500 && s.ry == 0);

    in.mouse_frame_dx = -20.0f;
    s = map_pad(m, in);
    CHECK(s.rx < -14000 && s.rx > -18500 && s.ry == 0);

    in.mouse_frame_dx = 0.0f;
    in.mouse_frame_dy = -20.0f;  // moving mouse up (negative y) -> positive ry
    s = map_pad(m, in);
    CHECK(s.ry > 14000 && s.ry < 18500 && s.rx == 0);

    in.mouse_frame_dy = 20.0f;   // moving mouse down (positive y) -> negative ry
    s = map_pad(m, in);
    CHECK(s.ry < -14000 && s.ry > -18500 && s.rx == 0);

    // All 4 diagonal quadrants preserve sign; asymmetric fast diagonal preserves exact direction ratio!
    in.mouse_frame_dx = 12.0f; in.mouse_frame_dy = -12.0f;
    s = map_pad(m, in); CHECK(s.rx > 10000 && s.ry > 10000);
    in.mouse_frame_dx = -12.0f; in.mouse_frame_dy = -12.0f;
    s = map_pad(m, in); CHECK(s.rx < -10000 && s.ry > 10000);
    in.mouse_frame_dx = -12.0f; in.mouse_frame_dy = 12.0f;
    s = map_pad(m, in); CHECK(s.rx < -10000 && s.ry < -10000);
    in.mouse_frame_dx = 12.0f; in.mouse_frame_dy = 12.0f;
    s = map_pad(m, in); CHECK(s.rx > 10000 && s.ry < -10000);

    // Fast asymmetric flick (vx=3000, vy=-1000 -> 3:1 ratio) must keep 3:1 direction ratio after radial clamp.
    in.mouse_frame_dx = 0.0f; in.mouse_frame_dy = 0.0f;
    in.mouse_vx = 3000.0f;
    in.mouse_vy = -1000.0f;
    s = map_pad(m, in);
    CHECK(s.rx > 0 && s.ry > 0);
    CHECK(std::abs((float)s.rx / (float)s.ry - 3.0f) < 0.05f);

    // Extreme asymmetric vectors at high speed must preserve physical angle (< 0.5 deg error) in final HID int32_t.
    const float asym_cases[4][2] = {
        {5000.0f, -50.0f},    // 100:1 X-dominant
        {50.0f, -5000.0f},    // 1:100 Y-dominant
        {-4000.0f, 250.0f},   // opposite quadrant (-X, -Y in stick)
        {3200.0f, 1600.0f},   // +X, -Y in stick
    };
    for (const auto& ac : asym_cases) {
      in.mouse_vx = ac[0];
      in.mouse_vy = ac[1];
      s = map_pad(m, in);
      const float ang_in = std::atan2(-ac[1], ac[0]) * 57.2957795f;
      const float ang_hid = std::atan2((float)s.ry, (float)s.rx) * 57.2957795f;
      float diff = std::fabs(ang_in - ang_hid);
      if (diff > 180.0f) diff = 360.0f - diff;
      CHECK(diff < 0.5f);
      CHECK(s.rx >= -32767 && s.rx <= 32767 && s.ry >= -32767 && s.ry <= 32767);
    }
    in.mouse_vx = 0.0f; in.mouse_vy = 0.0f;

    // DeltaAccum lossless atomic pair grouping + rapid reversal anti-cancellation.
    {
      DeltaAccum da;
      for (int i = 0; i < 50; ++i) da.add(3, -1);
      float tx = 0.0f, ty = 0.0f;
      uint64_t cnt = 0;
      da.take(&tx, &ty, &cnt);
      CHECK(tx == 150.0f && ty == -50.0f && cnt == 50);
      da.take(&tx, &ty, &cnt);
      CHECK(tx == 0.0f && ty == 0.0f && cnt == 0);

      // Rapid reversal (+40, -20 then -12, +10) drops stale pre-reversal deltas instead of cancelling.
      da.add(40, -20);
      da.add(-12, 10);
      da.take(&tx, &ty, &cnt);
      CHECK(tx == -12.0f && ty == 10.0f && cnt == 2);
    }

    // Proportionality from slow -> moderate -> medium -> fast (saturation at kMouseLookFullPxPerSec = 2400 px/s).
    in.mouse_frame_dy = 0.0f;
    in.mouse_frame_dx = 2.0f;   // 120 px/s -> ~0.096
    PadSample s_tiny = map_pad(m, in);
    in.mouse_frame_dx = 10.0f;  // 600 px/s -> ~0.304
    PadSample s_mod = map_pad(m, in);
    in.mouse_frame_dx = 20.0f;  // 1200 px/s -> 0.500
    PadSample s_med = map_pad(m, in);
    in.mouse_frame_dx = 30.0f;  // 1800 px/s -> 0.750
    PadSample s_fast = map_pad(m, in);
    in.mouse_frame_dx = 40.0f;  // 2400 px/s -> 1.0 saturated
    PadSample s_full = map_pad(m, in);
    CHECK(s_tiny.rx > 2500 && s_tiny.rx < s_mod.rx);
    CHECK(s_mod.rx < s_med.rx && s_med.rx < s_fast.rx && s_fast.rx < s_full.rx);
    CHECK(s_full.rx == 32767);

    // 3. Independent horizontal and vertical sensitivity scaling + JSON round-trip.
    Mapping m_low = m;
    m_low.mouse_sensitivity = 0.5f;
    Mapping m_high = m;
    m_high.mouse_sensitivity = 2.0f;
    in.mouse_frame_dx = 4.0f;
    in.mouse_frame_dy = 0.0f;
    PadSample s_low = map_pad(m_low, in);
    PadSample s_high = map_pad(m_high, in);
    CHECK(s_low.rx > 0 && s_high.rx > s_low.rx);

    Mapping m_xy = m;
    m_xy.mouse_sens_x = 2.0f;
    m_xy.mouse_sens_y = 0.5f;
    in.mouse_frame_dx = 4.0f;
    in.mouse_frame_dy = -4.0f;
    PadSample s_xy = map_pad(m_xy, in);
    CHECK(s_xy.rx > s_xy.ry * 2);
    {
        hle::Json j = m_xy.to_json();
        Mapping m_loaded;
        std::string err;
        CHECK(m_loaded.from_json(j, &err));
        CHECK(std::fabs(m_loaded.mouse_sens_x - 2.0f) < 1e-4f);
        CHECK(std::fabs(m_loaded.mouse_sens_y - 0.5f) < 1e-4f);
    }

    // 4. Axis inversion.
    Mapping m_inv = m;
    m_inv.mouse_invert_x = true;
    m_inv.mouse_invert_y = true;
    in.mouse_frame_dx = 15.0f;
    in.mouse_frame_dy = -15.0f;
    PadSample s_norm = map_pad(m, in);
    PadSample s_inv = map_pad(m_inv, in);
    CHECK(s_norm.rx > 0 && s_inv.rx == -s_norm.rx);
    CHECK(s_norm.ry > 0 && s_inv.ry == -s_norm.ry);

    // 5. Mouse buttons in ride (look_a = left -> A, look_zl = right -> ZL) and in menu (touch + A/B/wheel).
    in = RawInput{};
    in.focused = true;
    in.in_ride = true;
    in.mouse_buttons = (1u << kMouseLeft) | (1u << kMouseRight);
    s = map_pad(m, in);
    CHECK((s.buttons & (1ull << kA)) != 0);
    CHECK((s.buttons & (1ull << kZL)) != 0);

    // Middle, X1, X2 bindings via Source::Mouse.
    Mapping m_extra = m;
    m_extra.buttons[kX].push_back(Source{Source::Mouse, kMouseX1});
    m_extra.buttons[kY].push_back(Source{Source::Mouse, kMouseX2});
    m_extra.buttons[kR].push_back(Source{Source::Mouse, kMouseMiddle});
    in.mouse_buttons = (1u << kMouseMiddle) | (1u << kMouseX1) | (1u << kMouseX2);
    s = map_pad(m_extra, in);
    CHECK((s.buttons & (1ull << kR)) != 0);
    CHECK((s.buttons & (1ull << kX)) != 0);
    CHECK((s.buttons & (1ull << kY)) != 0);

    // Menu mode (!in_ride): left click -> A + touch, right click -> B, wheel never triggers Up/Down.
    in = RawInput{};
    in.focused = true;
    in.in_ride = false;
    in.mouse_in_window = true;
    in.mouse_x = 0.5f;
    in.mouse_y = 0.5f;
    in.mouse_buttons = (1u << kMouseLeft) | (1u << kMouseWheelUp);
    s = map_pad(m, in);
    CHECK((s.buttons & (1ull << kA)) != 0);
    CHECK((s.buttons & (1ull << kUp)) == 0);
    TouchSample ts = map_touch(m, in);
    CHECK(ts.count == 1);

    in.mouse_buttons = (1u << kMouseRight) | (1u << kMouseWheelDown);
    s = map_pad(m, in);
    CHECK((s.buttons & (1ull << kB)) != 0);
    CHECK((s.buttons & (1ull << kDown)) == 0);
    CHECK((s.buttons & (1ull << kZL)) == 0);

    // Middle and side buttons with default mapping produce zero unexpected actions.
    in.in_ride = true;
    in.mouse_buttons = (1u << kMouseMiddle) | (1u << kMouseX1) | (1u << kMouseX2);
    s = map_pad(m, in);
    CHECK(s.buttons == 0);

    // 6 & 7. Focus loss/regain and cursor capture policy through Hub (including rail advancing -> in_ride).
    FakeInputState st;
    st.next.focused = true;
    Hub& h = hub();
    h.set_backend(std::make_unique<FakeBackend>(&st));
    h.set_mapping(m);
    h.set_window_focus(true);
    h.set_menu_open(false);
    // Outside ride with mouse_look=true: lock=false, recenter=false (free cursor in menu).
    st.next.mouse_buttons = 1;
    h.poll(0);
    CHECK(!st.last_lock && !st.last_recenter);
    CHECK(!h.cursor_hidden());

    // Simulate rail advancing (as rt_gm_neo_speed does): in_ride becomes true, cursor captured & hidden on next poll.
    hle::input::route_rail(0xdeadbeefULL, 1.0f);
    hle::input::route_rail(0xdeadbeefULL, 2.0f);
    st.next.mouse_buttons = (1u << kMouseRight);
    h.poll(0);
    CHECK(h.raw().in_ride);
    h.poll(0);
    CHECK(st.last_lock && st.last_recenter);
    CHECK(h.cursor_hidden());
    PadSample ride_s = map_pad(m, h.raw());
    CHECK((ride_s.buttons & (1ull << kZL)) != 0);
    CHECK((ride_s.buttons & (1ull << kB)) == 0);

    // Opening F1 overlay during ride releases cursor capture cleanly.
    h.set_menu_open(true);
    h.poll(0);
    CHECK(!st.last_lock && !st.last_recenter);
    CHECK(!h.cursor_hidden());
    h.set_menu_open(false);
    h.poll(0);
    CHECK(st.last_lock && st.last_recenter);

    // Lose focus -> focus guard clears mouse buttons and cursor_hidden().
    st.next.focused = false;
    h.set_window_focus(false);
    h.poll(0);
    CHECK(!h.cursor_hidden());
    CHECK(h.raw().mouse_buttons == 0);

    // Regain focus while button still held -> focus guard ignores held button until released.
    st.next.focused = true;
    st.next.mouse_buttons = 2;
    h.set_window_focus(true);
    h.poll(0);
    CHECK(h.raw().mouse_buttons == 0);
    st.next.mouse_buttons = 0;
    h.poll(0);
    st.next.mouse_buttons = 2;
    h.poll(0);
    CHECK(h.raw().mouse_buttons == 2);

    // Zero scroll bindings in default mapping.
    for (int b = 0; b < kButtonCount; ++b) {
        for (const auto& src : m.buttons[(size_t)b]) {
            CHECK(!(src.kind == Source::Mouse && (src.id == kMouseWheelUp || src.id == kMouseWheelDown)));
        }
    }

    // Wheel up and wheel down produce zero gameplay actions and zero 100 ms ZL pulse.
    st.next.mouse_buttons = 0;
    post_wheel(120);
    h.poll(0);
    CHECK((h.raw().mouse_buttons & ((1u << kMouseWheelUp) | (1u << kMouseWheelDown))) == 0);
    PadSample w_up = map_pad(m, h.raw());
    CHECK(w_up.buttons == 0);

    post_wheel(-120);
    h.poll(0);
    CHECK((h.raw().mouse_buttons & ((1u << kMouseWheelUp) | (1u << kMouseWheelDown))) == 0);
    PadSample w_dn = map_pad(m, h.raw());
    CHECK(w_dn.buttons == 0);

    // Per-frame displacement: multiple events accumulate in the same present, stay stable across intra-frame polls,
    // and reset immediately to zero on the next present with no movement.
    nvn::g_presentCount++;
    st.next.mouse_dx = 12.0f;
    st.next.mouse_dy = -6.0f;
    h.poll(0);
    CHECK(h.raw().mouse_frame_dx == 12.0f && h.raw().mouse_frame_dy == -6.0f);
    // Second delta within the same frame accumulates without loss.
    st.next.mouse_dx = 8.0f;
    st.next.mouse_dy = -4.0f;
    h.poll(0);
    CHECK(h.raw().mouse_frame_dx == 20.0f && h.raw().mouse_frame_dy == -10.0f);
    // Extra read in the same frame with zero new delta keeps the frame displacement intact (no double-consume).
    st.next.mouse_dx = 0.0f;
    st.next.mouse_dy = 0.0f;
    h.poll(0);
    CHECK(h.raw().mouse_frame_dx == 20.0f && h.raw().mouse_frame_dy == -10.0f);
    // Next frame with no movement resets immediately to 0.
    nvn::g_presentCount++;
    h.poll(0);
    CHECK(h.raw().mouse_frame_dx == 0.0f && h.raw().mouse_frame_dy == 0.0f);
    CHECK(h.raw().mouse_vx == 0.0f && h.raw().mouse_vy == 0.0f);

    // FPS-independent ride velocity: same physical speed (~1200 px/s) across ~100 FPS (10 ms) and ~33 FPS (30 ms)
    // yields equivalent stick deflection; many small events, single large delta, multi-frame events, and immediate stop.
    st.next.in_ride = true;
    st.next.focused = true;
    nvn::g_presentCount++;
    h.poll(0);  // establish baseline present timestamp
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    nvn::g_presentCount++;
    st.next.mouse_dx = 12.0f;  // ~1200 px/s over 10 ms -> ~0.5 stick
    st.next.mouse_dy = 0.0f;
    h.poll(0);
    const int32_t rx_fast_fps = h.pad().rx;
    CHECK(rx_fast_fps > 9000 && rx_fast_fps < 23000);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    nvn::g_presentCount++;
    st.next.mouse_dx = 36.0f;  // ~1200 px/s over 30 ms (3x dx in 3x dt) -> ~0.5 stick
    h.poll(0);
    const int32_t rx_slow_fps = h.pad().rx;
    CHECK(rx_slow_fps > 9000 && rx_slow_fps < 23000);
    CHECK(std::abs(rx_fast_fps - rx_slow_fps) < 6000);

    // Many small events in one frame accumulate cleanly (10 x 1.0f = 10.0f) -> proportional direction & magnitude.
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    nvn::g_presentCount++;
    st.next.mouse_dx = 1.0f;
    st.next.mouse_dy = -0.5f;
    for (int i = 0; i < 10; ++i) h.poll(0);
    CHECK(h.raw().mouse_frame_dx == 10.0f && h.raw().mouse_frame_dy == -5.0f);
    CHECK(h.pad().rx > 8000 && h.pad().ry > 4000);

    // Direction reversal within the same frame (+24,-20 then -14,+12) drops stale pre-reversal deltas -> -14,+12 directly.
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    nvn::g_presentCount++;
    st.next.mouse_dx = 24.0f; st.next.mouse_dy = -20.0f;
    h.poll(0);
    st.next.mouse_dx = -14.0f; st.next.mouse_dy = 12.0f;
    h.poll(0);
    CHECK(h.raw().mouse_frame_dx == -14.0f && h.raw().mouse_frame_dy == 12.0f);
    CHECK(h.pad().rx < -12000 && h.pad().ry < -10000);

    // Single large delta event (> saturation) preserves direction and stops immediately on the next frame.
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    nvn::g_presentCount++;
    st.next.mouse_dx = 60.0f;
    st.next.mouse_dy = 20.0f;  // right & down (3:1 ratio)
    h.poll(0);
    CHECK(h.pad().rx > 28000 && h.pad().ry < -9000);
    CHECK(std::abs((float)h.pad().rx / (float)(-h.pad().ry) - 3.0f) < 0.05f);

    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    nvn::g_presentCount++;
    st.next.mouse_dx = 0.0f;
    st.next.mouse_dy = 0.0f;
    h.poll(0);
    CHECK(h.pad().rx == 0 && h.pad().ry == 0);
    st.next.in_ride = false;

    // QA20: Two-Stage Split Radial (SNAP_MOUSE_DUAL_STICK=1, off by default -> QA18 behavior).
    {
      unsetenv("SNAP_MOUSE_DUAL_STICK");
      RawInput d_in{};
      d_in.focused = true;
      d_in.in_ride = true;
      d_in.mouse_vx = 4800.0f;  // 2.0x single-stick cap
      PadSample s_off = map_pad(m, d_in);
      CHECK(s_off.rx == 32767 && s_off.lx == 0 && s_off.ly == 0);

      setenv("SNAP_MOUSE_DUAL_STICK", "1", 1);
      unsetenv("SNAP_MOUSE_DUAL_GAIN");
      unsetenv("SNAP_MOUSE_DUAL_ZOOM_GAIN");
      // QA23: Default normal gain is 1.70 vs QA20 baseline (SNAP_MOUSE_DUAL_GAIN=1.0).
      d_in.mouse_vx = 1200.0f;  // 0.5x base -> 0.85x with 1.70 gain
      PadSample s_qa23_mid = map_pad(m, d_in);
      setenv("SNAP_MOUSE_DUAL_GAIN", "1.0", 1);
      PadSample s_qa20_mid = map_pad(m, d_in);
      CHECK(s_qa23_mid.rx > s_qa20_mid.rx && s_qa23_mid.lx == 0 && s_qa23_mid.ly == 0);
      CHECK(std::abs((float)s_qa23_mid.rx / (float)s_qa20_mid.rx - 1.70f) < 0.02f);

      // Configurable normal gains without recompiling: 1.60, 1.70, 1.80, 2.00.
      {
        d_in.mouse_vx = 900.0f;  // 0.375x base -> stays <= 0.75 across all four gains
        int32_t prev_rx = 0;
        for (const char* dg_val : {"1.60", "1.70", "1.80", "2.00"}) {
          setenv("SNAP_MOUSE_DUAL_GAIN", dg_val, 1);
          PadSample s_cfg = map_pad(m, d_in);
          CHECK(s_cfg.rx > prev_rx && s_cfg.lx == 0);
          prev_rx = s_cfg.rx;
        }
        unsetenv("SNAP_MOUSE_DUAL_GAIN");
      }

      // Fine/slow movement (120 px/s) keeps smooth precision with lx == 0.
      d_in.mouse_vx = 120.0f;
      PadSample s_fine = map_pad(m, d_in);
      CHECK(s_fine.rx > 4000 && s_fine.rx < 6000 && s_fine.lx == 0);

      // Medium (1200 px/s -> 0.85x with 1.70 gain) stays strictly on Right Stick, Left Stick stays 0.
      d_in.mouse_vx = 1200.0f;
      PadSample s_slow = map_pad(m, d_in);
      CHECK(s_slow.rx > 26500 && s_slow.rx < 29000 && s_slow.lx == 0 && s_slow.ly == 0);

      // Fast (2400 px/s): QA23 normal gain 1.70 increases Left Stick contribution vs QA22 (1.40).
      d_in.mouse_vx = 2400.0f;
      PadSample s_17 = map_pad(m, d_in);
      setenv("SNAP_MOUSE_DUAL_GAIN", "1.40", 1);
      PadSample s_14 = map_pad(m, d_in);
      unsetenv("SNAP_MOUSE_DUAL_GAIN");
      CHECK(s_17.rx == 32767 && s_14.rx == 32767);
      CHECK(s_17.lx > 21500 && s_17.lx < 24500 && s_17.lx > s_14.lx && s_17.ly == 0);

      // QA23: Independent zoom sensitivity (SNAP_MOUSE_DUAL_ZOOM_GAIN=2.20 default) via right-click look_zl,
      // gamepad ZL, and toggle_zl; unaffected by SNAP_MOUSE_DUAL_GAIN; immediate transition back to 1.70 on release.
      {
        d_in.mouse_vx = 900.0f;  // 0.375x base: 0.6375 with 1.70 normal, 0.825 with 2.20 zoom
        d_in.mouse_vy = 0.0f;
        d_in.mouse_buttons = 0;
        PadSample s_no_zoom = map_pad(m, d_in);

        // Right mouse button (look_zl -> kZL) activates independent 2.20 zoom gain.
        d_in.mouse_buttons = (1u << kMouseRight);
        PadSample s_zoom_rmb = map_pad(m, d_in);
        CHECK((s_zoom_rmb.buttons & (1ull << kZL)) != 0);
        CHECK(s_zoom_rmb.rx > 26000 && s_zoom_rmb.rx < 28500 && s_zoom_rmb.lx == 0);
        CHECK(std::abs((float)s_zoom_rmb.rx / (float)s_no_zoom.rx - (2.20f / 1.70f)) < 0.02f);

        // Changing normal gain (1.40 vs 2.00) must NOT affect zoom sensitivity (still 2.20).
        setenv("SNAP_MOUSE_DUAL_GAIN", "2.00", 1);
        PadSample s_zoom_indep = map_pad(m, d_in);
        CHECK(s_zoom_indep.rx == s_zoom_rmb.rx);
        unsetenv("SNAP_MOUSE_DUAL_GAIN");

        // Release zoom -> immediately returns to 1.70 normal sensitivity on the very next call.
        d_in.mouse_buttons = 0;
        PadSample s_released = map_pad(m, d_in);
        CHECK((s_released.buttons & (1ull << kZL)) == 0);
        CHECK(s_released.rx == s_no_zoom.rx);

        // Gamepad ZL trigger also activates zoom gain.
        d_in.pad_connected = true;
        d_in.lt = 1.0f;
        PadSample s_zoom_pad = map_pad(m, d_in);
        CHECK((s_zoom_pad.buttons & (1ull << kZL)) != 0);
        CHECK(s_zoom_pad.rx == s_zoom_rmb.rx);
        d_in.lt = 0.0f;
        d_in.pad_connected = false;

        // Configurable SNAP_MOUSE_DUAL_ZOOM_GAIN values: 1.60, 1.80, 2.00, 2.20.
        d_in.mouse_buttons = (1u << kMouseRight);
        d_in.mouse_vx = 900.0f;  // 0.375x base -> below 1.0 across all four gains
        int32_t prev_rx = 0;
        for (const char* zg_val : {"1.60", "1.80", "2.00", "2.20"}) {
          setenv("SNAP_MOUSE_DUAL_ZOOM_GAIN", zg_val, 1);
          PadSample s_cfg = map_pad(m, d_in);
          CHECK(s_cfg.rx > prev_rx && s_cfg.lx == 0);
          prev_rx = s_cfg.rx;
        }
        unsetenv("SNAP_MOUSE_DUAL_ZOOM_GAIN");

        // Diagonal fast during zoom preserves Two-Stage Split Radial direction (3:1 ratio).
        d_in.mouse_vx = 2400.0f;
        d_in.mouse_vy = -800.0f;
        PadSample s_zdiag = map_pad(m, d_in);
        CHECK(s_zdiag.rx > 28000 && s_zdiag.ry > 9000);
        CHECK(s_zdiag.lx > 25000 && s_zdiag.ly > 8000);
        CHECK(std::abs((float)s_zdiag.rx / (float)s_zdiag.ry - 3.0f) < 0.05f);
        CHECK(std::abs((float)s_zdiag.lx / (float)s_zdiag.ly - 3.0f) < 0.05f);

        // toggle_zl latch support (zoom_latched = true).
        Mapping m_tog = m;
        m_tog.toggle_zl = true;
        d_in.mouse_buttons = 0;
        d_in.mouse_vx = 900.0f;
        d_in.mouse_vy = 0.0f;
        d_in.zoom_latched = true;
        PadSample s_zlat = map_pad(m_tog, d_in);
        CHECK(s_zlat.rx == s_zoom_rmb.rx);
        d_in.zoom_latched = false;
      }

      // Diagonal fast (>1.0x): both sticks point along exact same vector (3:1 ratio).
      d_in.mouse_vx = 3600.0f;
      d_in.mouse_vy = -1200.0f;
      PadSample s_diag = map_pad(m, d_in);
      CHECK(s_diag.rx > 28000 && s_diag.ry > 9000);
      CHECK(s_diag.lx > 14000 && s_diag.ly > 4500);
      CHECK(std::abs((float)s_diag.rx / (float)s_diag.ry - 3.0f) < 0.05f);
      CHECK(std::abs((float)s_diag.lx / (float)s_diag.ly - 3.0f) < 0.05f);

      // Both sticks saturate at >= 2.0x (6000 px/s).
      d_in.mouse_vx = 6000.0f;
      d_in.mouse_vy = 0.0f;
      PadSample s_2x = map_pad(m, d_in);
      CHECK(s_2x.rx == 32767 && s_2x.lx == 32767);

      // Immediate stop zeros both sticks.
      d_in.mouse_vx = 0.0f;
      d_in.mouse_vy = 0.0f;
      PadSample s_stop = map_pad(m, d_in);
      CHECK(s_stop.rx == 0 && s_stop.ry == 0 && s_stop.lx == 0 && s_stop.ly == 0);

      // Conflict with physical Left Stick -> split yields to gamepad.
      d_in.mouse_vx = 4800.0f;
      d_in.pad_connected = true;
      d_in.lx = -0.6f;
      PadSample s_pad = map_pad(m, d_in);
      CHECK(s_pad.rx == 32767 && s_pad.lx < -10000);

      // Conflict with WASD -> split yields to keyboard.
      d_in.lx = 0.0f;
      d_in.pad_connected = false;
      const int kA_key = key_from_name("A");
      CHECK(kA_key > 0);
      d_in.keys.set((size_t)kA_key);
      PadSample s_wasd = map_pad(m, d_in);
      CHECK(s_wasd.rx == 32767 && s_wasd.lx == -32767);

      unsetenv("SNAP_MOUSE_DUAL_STICK");
    }

    // Coexistence with gamepad: gamepad left stick and buttons stay intact alongside mouse aim.
    in = RawInput{};
    in.focused = true;
    in.in_ride = true;
    in.pad_connected = true;
    in.lx = 0.75f;
    in.pad.set(kPadNorth);
    in.mouse_frame_dx = 20.0f;
    s = map_pad(m, in);
    CHECK(s.lx > 20000);
    CHECK(s.rx > 10000);
    CHECK((s.buttons & (1ull << kX)) != 0);

    // 8. Coexistence with gamepad + no-gyro safety (9) + Aim Feel window (10).
    pad_fx().gyro_on.store(true);
    pad_fx().mouse_window_ms.store(20.0f);
    in = RawInput{};
    in.focused = true;
    in.in_ride = true;
    in.pad_connected = true;
    in.gyro_present = false;
    in.gyro_x = 90.0f;
    in.gyro_y = 90.0f;
    in.mouse_frame_dx = 20.0f;
    s = map_pad(m, in);
    CHECK(s.rx > 10000);
    CHECK(s.ry == 0);
    pad_fx().gyro_on.store(false);
    pad_fx().mouse_window_ms.store(40.0f);

    // 32:9 ultrawide window-to-guest touch mapping check (5120x1440 -> centered 16:9 1920x1080 -> 1280x720).
    auto orig_w2g = g_window_to_guest;
    g_window_to_guest = [](float nx, float ny, float* gx, float* gy) -> bool {
        // 5120x1440 has 16:9 active UI area in nx=[0.25..0.75], ny=[0..1].
        const float u = std::clamp((nx - 0.25f) / 0.5f, 0.0f, 1.0f);
        *gx = u * 1920.0f;
        *gy = ny * 1080.0f;
        return true;
    };
    in = RawInput{};
    in.focused = true;
    in.in_ride = false;
    in.mouse_in_window = true;
    in.mouse_x = 0.5f;
    in.mouse_y = 0.5f;
    in.mouse_buttons = (1u << kMouseLeft);
    TouchSample ts_32_9 = map_touch(m, in);
    CHECK(ts_32_9.count == 1 && ts_32_9.pt[0].x == 640 && ts_32_9.pt[0].y == 360);
    in.mouse_x = 0.75f;
    ts_32_9 = map_touch(m, in);
    CHECK(ts_32_9.count == 1 && ts_32_9.pt[0].x == 1279);
    g_window_to_guest = orig_w2g;

    // UI scene routing: left click always produces kA + TouchSample on menus (title, main_menu, map_select),
    // and right click produces kB, preserving QA8 / upstream v1.4.0 behavior.
    nvn::g_presentCount += 25;
    std::this_thread::sleep_for(std::chrono::milliseconds(310));
    route_ui(0x1000ULL, "title", "Main", 2, "", 0);
    st.next = RawInput{};
    st.next.focused = true;
    st.next.mouse_in_window = true;
    st.next.mouse_x = 0.5f;
    st.next.mouse_y = 0.5f;
    st.next.mouse_buttons = (1u << kMouseLeft);
    h.poll(0);
    CHECK((h.pad().buttons & (1ull << kA)) != 0);
    CHECK(h.touch().count == 1);

    // Even when map_select opens (and even if title wasn't closed with ev=3), left click still sends kA + TouchSample.
    route_ui(0x2000ULL, "map_select", "StageSelect", 2, "", 0);
    h.poll(0);
    CHECK((h.pad().buttons & (1ull << kA)) != 0);
    CHECK(h.touch().count == 1);
    // Right click maps to kB.
    st.next.mouse_buttons = (1u << kMouseRight);
    h.poll(0);
    CHECK((h.pad().buttons & (1ull << kB)) != 0);
    CHECK(h.touch().count == 0);
    route_ui(0x1000ULL, "title", "Main", 3, "", 0);
    route_ui(0x2000ULL, "map_select", "StageSelect", 3, "", 0);

    // Continuous cursor movement outside ride (no buttons held) -> click lands on new 1280x720 coordinates;
    // stationary mouse produces zero camera delta; ride relative motion remains independent.
    auto orig_w2g2 = g_window_to_guest;
    g_window_to_guest = [](float nx, float ny, float* gx, float* gy) -> bool {
        const float u = std::clamp((nx - 0.25f) / 0.5f, 0.0f, 1.0f);
        *gx = u * 1920.0f;
        *gy = ny * 1080.0f;
        return true;
    };
    nvn::g_presentCount++;
    st.next = RawInput{};
    st.next.focused = true;
    st.next.mouse_in_window = true;
    st.next.mouse_x = 0.375f;  // 0.25 -> 1280*0.25 = 320
    st.next.mouse_y = 0.25f;   // 720*0.25 = 180
    st.next.mouse_buttons = 0;
    h.poll(0);
    CHECK(h.raw().mouse_x == 0.375f && h.raw().mouse_y == 0.25f);
    CHECK(h.raw().mouse_frame_dx == 0.0f && h.pad().rx == 0);
    CHECK(h.touch().count == 0);

    // Move continuously without clicking -> position updates, still no spurious touch or camera motion.
    nvn::g_presentCount++;
    st.next.mouse_x = 0.625f;  // 0.75 -> 1280*0.75 = 960
    st.next.mouse_y = 0.75f;   // 720*0.75 = 540
    h.poll(0);
    CHECK(h.raw().mouse_x == 0.625f && h.raw().mouse_y == 0.75f);
    CHECK(h.pad().rx == 0 && h.touch().count == 0);

    // Click after movement -> TouchSample lands at the new 1280x720 position (960, 540).
    nvn::g_presentCount++;
    st.next.mouse_buttons = (1u << kMouseLeft);
    h.poll(0);
    CHECK(h.touch().count == 1 && h.touch().pt[0].x == 960 && h.touch().pt[0].y == 540);

    // ui_pointer_1080: moving mouse activates internal 1920x1080 pointer respecting 32:9 16:9 sub-rect,
    // yields immediately to L-stick or WASD, and stays off during ride or F1 overlay.
    st.next.mouse_buttons = 0;
    post_mouse_pos(0.50f, 0.50f, true);
    h.poll(0);
    float ptr_x = 0.0f, ptr_y = 0.0f;
    CHECK(ui_pointer_1080(&ptr_x, &ptr_y));
    CHECK(std::fabs(ptr_x - 960.0f) < 1e-2f && std::fabs(ptr_y - 540.0f) < 1e-2f);

    // 32:9 edge clamping (nx=0.25 -> 0, nx=0.75 -> 1920; nx=0.1 -> clamped 0).
    post_mouse_pos(0.10f, 0.50f, true);
    CHECK(ui_pointer_1080(&ptr_x, &ptr_y));
    CHECK(ptr_x == 0.0f);
    post_mouse_pos(0.90f, 0.50f, true);
    CHECK(ui_pointer_1080(&ptr_x, &ptr_y));
    CHECK(ptr_x == 1920.0f);

    // L-stick immediately reclaims pointer control.
    st.next.pad_connected = true;
    st.next.lx = 0.6f;
    h.poll(0);
    CHECK(!ui_pointer_1080(&ptr_x, &ptr_y));

    // Moving mouse again re-activates mouse pointer; WASD (key:W) immediately reclaims control.
    post_mouse_pos(0.40f, 0.30f, true);
    st.next.lx = 0.0f;
    h.poll(0);
    CHECK(ui_pointer_1080(&ptr_x, &ptr_y));
    const int kW = key_from_name("W");
    CHECK(kW > 0);
    st.next.keys.set((size_t)kW);
    h.poll(0);
    CHECK(!ui_pointer_1080(&ptr_x, &ptr_y));
    st.next.keys.reset();

    // F1 overlay open disables game pointer overwrite.
    post_mouse_pos(0.55f, 0.45f, true);
    h.set_menu_open(true);
    h.poll(0);
    CHECK(!ui_pointer_1080(&ptr_x, &ptr_y));
    h.set_menu_open(false);

    // During ride, ui_pointer_1080 stays off.
    route_rail(0xdeadbeefULL, 3.0f);
    post_mouse_pos(0.52f, 0.48f, true);
    h.poll(0);
    CHECK(!ui_pointer_1080(&ptr_x, &ptr_y));

    g_window_to_guest = orig_w2g2;

    // Clean up fake backend.
    h.set_backend(nullptr);
}

static void test_aot_camera_bridge() {
    using namespace hle::input;
    unsetenv("SNAP_MOUSE_AOT_CAMERA");
    CHECK(!aot_camera_enabled());
    setenv("SNAP_MOUSE_AOT_CAMERA", "1", 1);
    CHECK(aot_camera_enabled());
    reset_aot_mouse_delta();

    // 1. Suppresses mouse-to-stick in map_pad while preserving physical gamepad & WASD.
    Mapping m = Mapping::defaults();
    m.mouse_look = true; m.mouse_sensitivity = 1.0f;
    RawInput in{};
    in.pad_connected = true; in.focused = true; in.in_ride = true;
    in.mouse_frame_dx = 40.0f; in.mouse_vx = 2400.0f;
    in.mouse_frame_dy = -20.0f; in.mouse_vy = -1200.0f;
    PadSample p = map_pad(m, in);
    CHECK(p.rx == 0 && p.ry == 0 && p.lx == 0 && p.ly == 0);
    in.rx = 0.8f; in.ly = -0.8f;
    p = map_pad(m, in);
    CHECK(p.rx > 15000 && p.ly < -15000);

    // 2. Delta accumulation and atomic drain (no loss, no double-apply).
    post_aot_mouse_delta(10.0f, -5.0f, false);
    post_aot_mouse_delta(5.0f, -2.5f, false);
    float dx = 0.0f, dy = 0.0f; bool zoom = true;
    CHECK(take_aot_mouse_delta(&dx, &dy, &zoom));
    CHECK(std::fabs(dx - 15.0f) < 1e-5f && std::fabs(dy + 7.5f) < 1e-5f && !zoom);
    CHECK(!take_aot_mouse_delta(&dx, &dy, &zoom));

    // 3. rt_gm_ride_mouse_cam updates [x24+0x0] (pitch) and [x24+0x4] (yaw) at L_71004c1688.
    alignas(16) uint8_t cam[0x20]{};
    auto put_f = [&](size_t off, float v) { uint32_t b; std::memcpy(&b, &v, 4); std::memcpy(cam + off, &b, 4); };
    auto get_f = [&](size_t off) { uint32_t b; std::memcpy(&b, cam + off, 4); float v; std::memcpy(&v, &b, 4); return v; };
    put_f(0x0, 0.10f); put_f(0x4, -0.20f);
    Cpu cpu{};
    cpu.x[24] = (uint64_t)(uintptr_t)cam;
    post_aot_mouse_delta(100.0f, -50.0f, false);
    rt_gm_ride_mouse_cam(&cpu);
    float np = get_f(0x0), ny = get_f(0x4);
    // Mouse up (dy=-50) increases pitch; mouse right (dx=100) decreases yaw.
    CHECK(np > 0.10f && ny < -0.20f);

    // 4. Immediate stop (no pending delta -> no change).
    rt_gm_ride_mouse_cam(&cpu);
    CHECK(std::fabs(get_f(0x0) - np) < 1e-6f && std::fabs(get_f(0x4) - ny) < 1e-6f);

    // 5. Independent zoom sensitivity (2.20 default).
    put_f(0x0, 0.0f); put_f(0x4, 0.0f);
    post_aot_mouse_delta(100.0f, 0.0f, true);
    rt_gm_ride_mouse_cam(&cpu);
    float zoom_yaw = get_f(0x4);
    CHECK(std::fabs(zoom_yaw - (-100.0f * 0.0005f * 2.20f)) < 1e-5f);

    // 5b. QA26.3: F1 Sensitivity, axis sensitivity, invert, and zoom transition via Hub::poll.
    {
      struct LocalFake : Backend {
        RawInput next{};
        const char* name() const override { return "fake"; }
        void poll(RawInput& out) override { out = next; }
      };
      auto be = std::make_unique<LocalFake>();
      auto* raw_be = be.get();
      Hub hub;
      hub.set_backend(std::move(be));
      Mapping hm = Mapping::defaults();
      hm.mouse_look = true;
      hm.mouse_sensitivity = 2.0f;
      hm.mouse_sens_x = 1.5f;
      hm.mouse_sens_y = 0.5f;
      hm.mouse_invert_x = true;
      hm.mouse_invert_y = false;
      hub.set_mapping(hm);
      raw_be->next.focused = true;
      raw_be->next.in_ride = true;
      raw_be->next.mouse_dx = 10.0f;
      raw_be->next.mouse_dy = -20.0f;
      reset_aot_mouse_delta();
      hub.poll(0);
      put_f(0x0, 0.0f); put_f(0x4, 0.0f);
      rt_gm_ride_mouse_cam(&cpu);
      // dx_eff = 10 * 2.0 * 1.5 * (-1) = -30 -> yaw = -(-30)*0.0018 = +0.054
      // dy_eff = -20 * 2.0 * 0.5 * (+1) = -20 -> pitch = -(-20)*0.0018 = +0.036
      CHECK(std::fabs(get_f(0x4) - 0.054f) < 1e-5f);
      CHECK(std::fabs(get_f(0x0) - 0.036f) < 1e-5f);
      // Zoom active via right mouse button (kMouseRight -> kZL)
      raw_be->next.mouse_buttons = (1u << kMouseRight);
      raw_be->next.mouse_dx = 10.0f; raw_be->next.mouse_dy = 0.0f;
      hub.poll(0);
      put_f(0x0, 0.0f); put_f(0x4, 0.0f);
      rt_gm_ride_mouse_cam(&cpu);
      // zoom_yaw = -(-30) * (0.0005 * 2.20) = +0.033
      CHECK(std::fabs(get_f(0x4) - 0.033f) < 1e-5f);
      // Zoom released transitions immediately back to normal
      raw_be->next.mouse_buttons = 0;
      hub.poll(0);
      put_f(0x0, 0.0f); put_f(0x4, 0.0f);
      rt_gm_ride_mouse_cam(&cpu);
      CHECK(std::fabs(get_f(0x4) - 0.054f) < 1e-5f);
      hub.set_backend(nullptr);
    }

    // 6. Fallback to QA23 Dual-Stick when SNAP_MOUSE_AOT_CAMERA=0.
    setenv("SNAP_MOUSE_AOT_CAMERA", "0", 1);
    setenv("SNAP_MOUSE_DUAL_STICK", "1", 1);
    in.rx = 0.0f; in.ly = 0.0f; in.mouse_vy = 0.0f; in.mouse_frame_dy = 0.0f;
    p = map_pad(m, in);
    CHECK(p.rx > 30000 && p.lx > 20000);
    unsetenv("SNAP_MOUSE_DUAL_STICK");
    unsetenv("SNAP_MOUSE_AOT_CAMERA");
}

int main() {
    test_hid_registry();
    test_input_core_and_hid_shims();
    test_aim_feel();
    test_mouse_and_keyboard_support();
    test_aot_camera_bridge();
    std::printf("hid_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
