// SPDX-License-Identifier: MIT
// Stage 6C-B2: isolated tests for Linux SDL3 input backend (and NullBackend when built headless without SDL3).
// Never depends on a physical controller or an SDL window; never executes guest code.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "hle/hle.h"
#include "hle/hle_config.h"
#include "hle/input_core.h"
#include "hle/svc_common.h"

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

extern "C" void os_get_stats(void*) {}

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void test_backend_lifecycle_and_headless() {
    auto b = hle::input::make_default_backend();
    CHECK(b != nullptr);
#ifdef HLE_WITH_SDL3
    CHECK(std::string(b->name()) == "sdl3");
#else
    CHECK(std::string(b->name()) == "none");
#endif

    // Safe poll & rumble with no window and no physical device.
    hle::input::RawInput raw;
    b->poll(raw);
#ifdef HLE_WITH_SDL3
    CHECK(!raw.focused);
#else
    CHECK(!raw.pad_connected);
#endif
    CHECK(raw.keys.none());
    CHECK(raw.mouse_buttons == 0);
    CHECK(!raw.mouse_in_window);
    b->rumble(0.5f, 0.5f);
    b->rumble(0.0f, 0.0f);

    // Clean destruction and re-creation.
    b.reset();
    b = hle::input::make_default_backend();
    CHECK(b != nullptr);
    raw = hle::input::RawInput{};
    b->poll(raw);
    b.reset();
}

#ifdef HLE_WITH_SDL3
static void test_sdl3_virtual_hotplug_and_disconnect() {
    auto b = hle::input::make_default_backend();
    CHECK(b != nullptr && std::string(b->name()) == "sdl3");

    // Ensure SDL initialized before attaching virtual joystick.
    hle::input::RawInput raw;
    b->poll(raw);

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST);
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    desc.name = "SnapTestVirtualPad";

    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    if (vid == 0) {
        std::fprintf(stderr, "sdl3_backend_linux_test: virtual joystick unsupported in this environment (%s); skipping virtual hotplug checks\n",
                     SDL_GetError());
        return;
    }

    // 1. Hotplug discovery (pads_ was empty, so poll() scans immediately).
    raw = hle::input::RawInput{};
    b->poll(raw);
    CHECK(raw.pad_connected);

    // 2. Drive virtual button + axis and verify Hub receives mapped input.
    SDL_Joystick* vjoy = SDL_OpenJoystick(vid);
    CHECK(vjoy != nullptr);
    if (vjoy) {
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_EAST, true);
        SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, -32767);  // up (negative in SDL -> positive in RawInput.ly)
    }

    hle::input::Hub hub;
    hub.set_backend(std::move(b));
    hub.set_mapping(hle::input::Mapping::defaults());
    hub.poll(0);

    const auto r = hub.raw();
    CHECK(r.pad_connected);
    CHECK(r.pad.test(hle::input::kPadEast));
    CHECK(r.ly > 0.9f);
    CHECK((hub.pad().buttons & (1ull << hle::input::kA)) != 0);
    CHECK(hub.pad().ly > 20000);

    // 3. Rumble fallback on virtual device without rumble capability must not crash.
    hub.rumble(0.75f, 0.25f);
    hub.rumble(0.0f, 0.0f);

    // 4. Disconnect handling.
    if (vjoy) SDL_CloseJoystick(vjoy);
    CHECK(SDL_DetachVirtualJoystick(vid));
    hub.poll(0);
    CHECK(!hub.raw().pad_connected);
}
#endif

static void test_end_to_end_sdl3_hub_hid_integration() {
    // End-to-end: bind_all() -> real nn::hid overrides -> Hub -> real make_default_backend() (SDL3 or NullBackend).
    std::unordered_map<std::string, GuestFn> bound;
    hle::bind_all([](const char* sym, GuestFn fn, void* u) {
        (*static_cast<std::unordered_map<std::string, GuestFn>*>(u))[sym] = fn;
    }, &bound);

    GuestFn fn_init = bound["_ZN2nn3hid14InitializeNpadEv"];
    GuestFn fn_npad = bound["_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj"];
    GuestFn fn_style = bound["_ZN2nn3hid15GetNpadStyleSetERKj"];
    CHECK(fn_init != nullptr && fn_npad != nullptr && fn_style != nullptr);
    CHECK(hle::find_override("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj") == fn_npad);

    char tmpl[] = "/tmp/snap_hid_integ_XXXXXX";
    char* tmp = mkdtemp(tmpl);
    CHECK(tmp != nullptr);
    const std::string orig_map = hle::config().input_map_path;
    if (tmp) hle::config().input_map_path = std::string(tmp) + "/input.json";

    // Reset Hub backend to force real make_default_backend() instantiation inside Hub::poll().
    hle::input::hub().set_backend(nullptr);
    hle::input::hub().set_mapping(hle::input::Mapping::defaults());
    hle::input::hub().set_menu_open(false);
    hle::input::hub().set_window_focus(true);

    Cpu c{};
    fn_init(&c);

    uint32_t npad_id = 0;
    uint8_t state[0x28] = {};
    auto read_npad = [&](uint64_t* btns, int32_t* ly, uint32_t* attr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::memset(state, 0, sizeof(state));
        c = Cpu{};
        c.x[0] = reinterpret_cast<uint64_t>(state);
        c.x[1] = reinterpret_cast<uint64_t>(&npad_id);
        fn_npad(&c);
        std::memcpy(btns, state + 8, 8);
        std::memcpy(ly, state + 0x14, 4);
        std::memcpy(attr, state + 0x20, 4);
    };

    // Baseline with no physical/virtual controller: neutral state.
    uint64_t btns = 0;
    int32_t ly = 0;
    uint32_t attr = 0;
    read_npad(&btns, &ly, &attr);
    CHECK(btns == 0 && ly == 0 && attr == 0);

#ifdef HLE_WITH_SDL3
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                       (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH);
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    desc.name = "SnapIntegPad1";

    const SDL_JoystickID vid1 = SDL_AttachVirtualJoystick(&desc);
    if (vid1 != 0) {
        SDL_Joystick* j1 = SDL_OpenJoystick(vid1);
        CHECK(j1 != nullptr);
        if (j1) {
            SDL_SetJoystickVirtualButton(j1, SDL_GAMEPAD_BUTTON_EAST, true);
            SDL_SetJoystickVirtualAxis(j1, SDL_GAMEPAD_AXIS_LEFTY, -32767);
        }
        read_npad(&btns, &ly, &attr);
        CHECK(attr == 1);
        CHECK((btns & (1ull << hle::input::kA)) != 0);
        CHECK(ly > 20000);

        // Disconnect -> state returns to neutral.
        if (j1) SDL_CloseJoystick(j1);
        CHECK(SDL_DetachVirtualJoystick(vid1));
        read_npad(&btns, &ly, &attr);
        CHECK(attr == 0 && btns == 0 && ly == 0);

        // Reconnect -> state recovers.
        desc.name = "SnapIntegPadReconnect";
        const SDL_JoystickID vid2 = SDL_AttachVirtualJoystick(&desc);
        CHECK(vid2 != 0);
        if (vid2 != 0) {
            SDL_Joystick* j2 = SDL_OpenJoystick(vid2);
            CHECK(j2 != nullptr);
            if (j2) SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_NORTH, true);
            read_npad(&btns, &ly, &attr);
            CHECK(attr == 1);
            CHECK((btns & (1ull << hle::input::kX)) != 0);
            if (j2) SDL_CloseJoystick(j2);
            CHECK(SDL_DetachVirtualJoystick(vid2));
            read_npad(&btns, &ly, &attr);
            CHECK(attr == 0 && btns == 0);
        }
    }
#endif

    // Release Hub backend before main exits so SDL shuts down cleanly.
    hle::input::hub().set_backend(nullptr);
    hle::config().input_map_path = orig_map;
    if (tmp) {
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }
}

static void test_sdl3_mouse_cursor_mode_and_raw_mouse() {
    auto b = hle::input::make_default_backend();
    CHECK(b != nullptr);
    b->set_cursor_mode(true, true);
    bool lock = false, recenter = false;
    hle::input::sdl3_cursor_mode(&lock, &recenter);
    CHECK(lock && recenter);

    b->set_cursor_mode(true, false);
    hle::input::sdl3_cursor_mode(&lock, &recenter);
    CHECK(lock && !recenter);

    b->set_cursor_mode(false, false);
    hle::input::sdl3_cursor_mode(&lock, &recenter);
    CHECK(!lock && !recenter);

    // Raw mouse delta accumulation and draining.
    float rx = 0.0f, ry = 0.0f;
    hle::input::raw_mouse().take(&rx, &ry);
    hle::input::post_raw_mouse(18, -12);
    hle::input::post_raw_mouse(7, 4);
    hle::input::raw_mouse().take(&rx, &ry);
    CHECK(rx == 25.0f && ry == -8.0f);

    // Continuous absolute cursor position posting without any button pressed, and boundary checks.
    hle::input::post_mouse_pos(0.25f, 0.50f, true);
    float px = 0.0f, py = 0.0f;
    bool pin = false;
    CHECK(hle::input::read_posted_mouse_pos(&px, &py, &pin));
    CHECK(pin && std::fabs(px - 0.25f) < 1e-5f && std::fabs(py - 0.50f) < 1e-5f);

    hle::input::post_mouse_pos(0.0f, 1.0f, true);
    CHECK(hle::input::read_posted_mouse_pos(&px, &py, &pin));
    CHECK(pin && px == 0.0f && py == 1.0f);

    hle::input::post_mouse_pos(0.0f, 0.0f, false);
    CHECK(hle::input::read_posted_mouse_pos(&px, &py, &pin));
    CHECK(!pin);

    // Headless poll keeps mouse fields clean when unfocused.
    hle::input::RawInput raw;
    b->poll(raw);
    CHECK(!raw.focused);
    CHECK(raw.mouse_dx == 0.0f && raw.mouse_dy == 0.0f);
    CHECK(raw.mouse_buttons == 0);
}

int main() {
    test_backend_lifecycle_and_headless();
#ifdef HLE_WITH_SDL3
    test_sdl3_virtual_hotplug_and_disconnect();
#endif
    test_end_to_end_sdl3_hub_hid_integration();
    test_sdl3_mouse_cursor_mode_and_raw_mouse();
    std::printf("sdl3_backend_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
