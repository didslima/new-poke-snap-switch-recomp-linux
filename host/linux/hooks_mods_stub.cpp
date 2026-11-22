// SPDX-License-Identifier: MIT
// Explicit Linux no-op stubs for Windows-only host/mods/* hooks and tracers (stages 5A & 5D-B2).
//
// Why these stubs are safe:
//   - All 24 rt_gm_* / rt_ui_opt_* / rt_mod_entry hooks below are MAIN_INLINE_HOOKS sites called via
//     rt_inline_hook(Cpu*, ra, fn) before the original guest instruction runs. When they return without
//     modifying Cpu state, the lifted guest code executes its original unmodded Switch instructions.
//   - None of them advertises mod support or registers host overrides via loader::add_override().
//   - rt_drp_decrypt / rt_drp_decrypt_check are NOT stubbed here: they are provided by shared host/mods/drpfast.cpp.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <strings.h>
#include <unordered_map>
#include <unordered_set>

#include "cpu.h"
#include "loader.h"

// The real ultrawide factor lives in runtime/nvn/nvn_wide.cpp. Targets that link this stub without the NVN renderer (hooks, audio runtime, ...)
// get 1.0 = no widening. The definition is weak on purpose: where the renderer is linked its strong definition wins, so this never overrides it.
extern "C" __attribute__((weak)) float nvnHostGetUltrawide(void) { return 1.0f; }
extern "C" __attribute__((weak)) void nvnHostSetUiPresentAspect(float) {}
extern "C" __attribute__((weak)) uint64_t snap_present_count(void) { return 0; }

namespace hle::input {
__attribute__((weak)) bool route_on() { return false; }
__attribute__((weak)) void route_rail(uint64_t, float) {}
__attribute__((weak)) void route_ui(uint64_t, const char *, const char *, int, const char *, uint64_t) {}
}  // namespace hle::input

namespace {

struct WideSite { uint64_t off; int reg; };
WideSite g_wide_sites[8] = {{0x296400, 9}};   // ride and lab camera (builder 0x295170)
int g_wide_nsites = 1;
const WideSite kCutsceneWideSites[] = {
    {0x38cdbc, 9},   // f_710038b710: cutscene camera projection (s9 = aspect)
    {0x29249c, 8},   // f_7100291d70: cutscene camera projection (s8 = aspect)
    {0x2973b0, 12},  // f_71002971c0: camera blend/transition projection (s12 = aspect)
    {0x297cd4, 10},  // f_7100297540: sibling camera blend/transition projection for ride start/end skip (s10 = aspect)
};

bool cutscene_ultrawide_enabled() {
    const char *e = std::getenv("SNAP_CUTSCENE_ULTRAWIDE");
    return e && e[0] != '\0' && std::strcmp(e, "0") != 0 && strcasecmp(e, "off") != 0 && strcasecmp(e, "false") != 0;
}

float wide_k() { return nvnHostGetUltrawide(); }   // not cached: the Ultrawide row applies with no restart (runtime/nvn/nvn_retarget.cpp)

bool fov_trace_enabled() {
    static const bool on = [] {
        const char *e = std::getenv("SNAP_TRACE_FOV");
        return e && e[0] != '\0' && std::strcmp(e, "0") != 0;
    }();
    return on;
}

std::atomic<bool> g_photodex_16_9{false};

void wide_camera(Cpu *c, uint64_t off) {
    if (g_photodex_16_9.load(std::memory_order_acquire)) return;
    int reg = -1;
    bool is_cutscene = false;
    for (int i = 0; i < g_wide_nsites; i++) if (g_wide_sites[i].off == off) { reg = g_wide_sites[i].reg; break; }
    if (reg < 0 && cutscene_ultrawide_enabled()) {
        for (const auto &site : kCutsceneWideSites) {
            if (site.off == off) { reg = site.reg; is_cutscene = true; break; }
        }
    }
    if (reg < 0) return;
    uint32_t ab = (uint32_t)c->v[reg];
    float a;
    std::memcpy(&a, &ab, 4);
    const float orig_a = a;
    if (std::fabs(a - 16.0f / 9.0f) >= 0.01f) return;
    a *= wide_k();
    std::memcpy(&ab, &a, 4);
    c->v[reg] = (__uint128_t)ab;
    if (fov_trace_enabled()) {
        static std::mutex wmu;
        static std::unordered_set<uint64_t> wseen;
        const uint64_t key = (off << 8) | (uint64_t)(reg & 0xff) | (is_cutscene ? (1ull << 63) : 0ull);
        std::lock_guard<std::mutex> lock(wmu);
        if (wseen.insert(key).second) {
            std::fprintf(stderr, "[fov-wide] p%llu site=main+%#llx cutscene_uw=%d (%s) reg=s%d aspect %.4f -> %.4f\n",
                         static_cast<unsigned long long>(snap_present_count()),
                         static_cast<unsigned long long>(off),
                         cutscene_ultrawide_enabled() ? 1 : 0,
                         is_cutscene ? "cutscene" : "base",
                         reg, orig_a, a);
            std::fflush(stderr);
        }
    }
}

void trace_tanf_caller(const Cpu *c, float x) {
    static std::mutex mu;
    static std::unordered_set<uint64_t> seen;
    const uint64_t lr = c->x[30];
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!seen.insert(lr).second) return;
    }
    const uint64_t off = lr >= GUEST_MAIN_BASE ? (lr - GUEST_MAIN_BASE) : lr;
    bool in_wide_sites = false;
    for (int i = 0; i < g_wide_nsites; i++) {
        if (g_wide_sites[i].off == off) {
            in_wide_sites = true;
            break;
        }
    }
    if (!in_wide_sites && cutscene_ultrawide_enabled()) {
        for (const auto &site : kCutsceneWideSites) {
            if (site.off == off) { in_wide_sites = true; break; }
        }
    }
    char regs[256] = "";
    int pos = 0;
    constexpr float k169 = 16.0f / 9.0f;
    for (int i = 0; i < 16; i++) {
        const uint32_t bits = static_cast<uint32_t>(c->v[i]);
        float val = 0.0f;
        std::memcpy(&val, &bits, sizeof(val));
        if (std::isfinite(val) && std::fabs(val - k169) < 0.01f) {
            pos += std::snprintf(regs + pos, sizeof(regs) - static_cast<size_t>(pos),
                                 "%ss%d=%.4f", pos ? " " : "", i, val);
            if (pos >= static_cast<int>(sizeof(regs)) - 1) break;
        }
    }
    std::fprintf(stderr,
                 "[fov] p%llu caller=%s%#llx x=%g rad (half=%.2f deg) wide_site=%s regs_16_9=[%s]\n",
                 static_cast<unsigned long long>(snap_present_count()),
                 lr >= GUEST_MAIN_BASE ? "main+" : "",
                 static_cast<unsigned long long>(off),
                 x,
                 x * (180.0 / 3.14159265358979323846),
                 in_wide_sites ? "yes" : "no",
                 regs[0] ? regs : "none");
    std::fflush(stderr);
}

void h_tanf(Cpu *c) {
    uint32_t b = (uint32_t)c->v[0];
    float x;
    std::memcpy(&x, &b, 4);
    if (fov_trace_enabled()) trace_tanf_caller(c, x);
    if (wide_k() > 1.0f) wide_camera(c, c->x[30] - GUEST_MAIN_BASE);
    float r = tanf(x);
    uint32_t rb;
    std::memcpy(&rb, &r, 4);
    c->v[0] = (__uint128_t)rb;
}

bool ui_trace_enabled() {
    static const bool on = [] {
        const char *e = std::getenv("SNAP_UI_TRACE");
        if (!e || !*e || std::strcmp(e, "0") == 0) e = std::getenv("SNAP_ROUTE_LOG");
        return e && e[0] != '\0' && std::strcmp(e, "0") != 0;
    }();
    return on;
}

bool ptr_plausible(uint64_t p) {
    return p >= 0x100000ull && p < 0x00007fffffffffffull;
}

// libc++ std::string (short: byte 0 = size * 2, text at +1; long: byte 0 bit 0 set, size at +8, data at +0x10)
bool read_guest_str(uint64_t s, char *out, size_t cap) {
    out[0] = 0;
    if (!ptr_plausible(s)) return false;
    const uint8_t b0 = *reinterpret_cast<const uint8_t *>(s);
    const char *p = nullptr;
    size_t n = 0;
    if (b0 & 1) {
        n = *reinterpret_cast<const uint64_t *>(s + 8);
        const uint64_t ptr = *reinterpret_cast<const uint64_t *>(s + 16);
        if (n > 256 || !ptr_plausible(ptr)) return false;
        p = reinterpret_cast<const char *>(ptr);
    } else {
        n = b0 >> 1;
        if (n > 22) return false;
        p = reinterpret_cast<const char *>(s + 1);
    }
    if (n >= cap) n = cap - 1;
    if (n) std::memcpy(out, p, n);
    out[n] = 0;
    return true;
}

void read_player_name(uint64_t player, char *nm, size_t cap) {
    constexpr uint64_t kPlayerVt = GUEST_MAIN_BASE + 0xc92020ull;
    nm[0] = 0;
    if (ptr_plausible(player) && *reinterpret_cast<const uint64_t *>(player) == kPlayerVt) {
        read_guest_str(player + 0x10, nm, cap);
    }
    if (!nm[0]) std::snprintf(nm, cap, "?");
}

void log_ui_transition(const char *src, uint64_t player, const char *scene, const char *state, int ev) {
    struct UiTrack {
        std::string scene;
        std::string state;
        int last_ev = -999;
    };
    static std::mutex mu;
    static std::unordered_map<uint64_t, UiTrack> per_player;
    std::lock_guard<std::mutex> lock(mu);
    UiTrack &tr = per_player[player];
    const bool same_scene = (tr.scene == scene);
    const bool same_state = (tr.state == state);
    // ev=22 fires every frame for every active player: only log when state is non-empty and changed.
    if (ev == 22) {
        if (state[0] == '\0' || (same_scene && same_state)) return;
    } else if (ev != 2 && ev != 3 && ev != -1) {
        if (same_scene && same_state && tr.last_ev == ev) return;
    } else if (ev == -1 && same_scene && same_state) {
        return;
    }
    tr.scene = scene;
    if (state[0] != '\0' || ev == 2 || ev == 3) tr.state = state;
    tr.last_ev = ev;
    if (ev == 3) per_player.erase(player);
    std::fprintf(stderr, "[ui-trace] p%llu %s player=%#llx scene=%s ev=%d state=%s\n",
                 static_cast<unsigned long long>(snap_present_count()),
                 src,
                 static_cast<unsigned long long>(player),
                 scene[0] ? scene : "?",
                 ev,
                 state[0] ? state : "-");
    std::fflush(stderr);
}

enum class PostRidePhase { Idle, InRide, PostRide16_9 };

struct PostRideState {
    std::mutex mu;
    PostRidePhase phase = PostRidePhase::Idle;
    uint64_t last_ride_present = 0;
    int ticks = 0;
    bool retry_logged = false;
};

PostRideState &post_ride_state() {
    static PostRideState st;
    return st;
}

void enter_post_ride_16_9(PostRideState &st, const char *trigger, int ev, uint64_t now) {
    st.phase = PostRidePhase::PostRide16_9;
    st.ticks = 0;
    st.retry_logged = false;
    nvnHostSetUiPresentAspect(16.0f / 9.0f);
    std::fprintf(stderr, "[post-ride-wide] p%llu enter 16:9 (trigger=%s ev=%d last_ride=p%llu)\n",
                 static_cast<unsigned long long>(now), trigger, ev,
                 static_cast<unsigned long long>(st.last_ride_present));
    std::fflush(stderr);
}

void on_ride_tick() {
    g_photodex_16_9.store(false, std::memory_order_relaxed);
    PostRideState &st = post_ride_state();
    const uint64_t now = snap_present_count();
    std::lock_guard<std::mutex> lock(st.mu);
    st.last_ride_present = now;
    if (st.phase == PostRidePhase::Idle) st.phase = PostRidePhase::InRide;
}

void on_eval_copy_after_ride() {
    PostRideState &st = post_ride_state();
    const uint64_t now = snap_present_count();
    std::lock_guard<std::mutex> lock(st.mu);
    if (st.phase == PostRidePhase::InRide) {
        enter_post_ride_16_9(st, "post-ride-eval-copy", 0, now);
    }
}

void on_dialogue_wait() {
    PostRideState &st = post_ride_state();
    const uint64_t now = snap_present_count();
    std::lock_guard<std::mutex> lock(st.mu);
    if (st.phase == PostRidePhase::InRide && now > st.last_ride_present + 10) {
        enter_post_ride_16_9(st, "post-ride-dialogue", 0, now);
    }
}

void update_post_ride_16_9_aspect(const char *scene, int ev) {
    if (std::strncmp(scene, "zukan", 5) == 0) {
        if (ev == 2 || ev == -1) {
            if (!g_photodex_16_9.exchange(true, std::memory_order_acq_rel)) {
                nvnHostSetUiPresentAspect(16.0f / 9.0f);
            }
            return;
        }
        if (ev == 3 && g_photodex_16_9.exchange(false, std::memory_order_acq_rel)) {
            PostRideState &st = post_ride_state();
            std::lock_guard<std::mutex> lock(st.mu);
            if (st.phase != PostRidePhase::PostRide16_9) nvnHostSetUiPresentAspect(0.0f);
            return;
        }
    } else if (g_photodex_16_9.load(std::memory_order_acquire) &&
               (std::strcmp(scene, "loading") == 0 ||
                (std::strcmp(scene, "main_menu") == 0 && ev == 2) ||
                (std::strcmp(scene, "map_select") == 0 && (ev == 2 || ev == -1)) ||
                (std::strcmp(scene, "title") == 0 && ev == 2))) {
        g_photodex_16_9.store(false, std::memory_order_release);
        PostRideState &st = post_ride_state();
        std::lock_guard<std::mutex> lock(st.mu);
        if (st.phase != PostRidePhase::PostRide16_9) nvnHostSetUiPresentAspect(0.0f);
    }

    PostRideState &st = post_ride_state();
    const uint64_t now = snap_present_count();
    std::lock_guard<std::mutex> lock(st.mu);

    if (st.phase != PostRidePhase::PostRide16_9) {
        if ((std::strcmp(scene, "photo_submit") == 0 || std::strcmp(scene, "evaluation") == 0) && ev == 2) {
            enter_post_ride_16_9(st, scene, ev, now);
        } else if (st.phase == PostRidePhase::InRide &&
                   (std::strcmp(scene, "loading") == 0 ||
                    (std::strcmp(scene, "main_menu") == 0 && ev == 2) ||
                    (std::strcmp(scene, "map_select") == 0 && (ev == 2 || ev == -1)) ||
                    (std::strcmp(scene, "title") == 0 && ev == 2))) {
            st.phase = PostRidePhase::Idle;
        }
        return;
    }

    if (std::strcmp(scene, "retry_menu") == 0) {
        if (!st.retry_logged) {
            st.retry_logged = true;
            std::fprintf(stderr, "[post-ride-wide] p%llu retry_menu active -> 16:9 maintained\n",
                         static_cast<unsigned long long>(now));
            std::fflush(stderr);
        }
        return;
    }

    const bool exit_post_ride =
        std::strcmp(scene, "loading") == 0 ||
        (std::strcmp(scene, "main_menu") == 0 && ev == 2) ||
        (std::strcmp(scene, "map_select") == 0 && (ev == 2 || ev == -1)) ||
        (std::strcmp(scene, "title") == 0 && ev == 2);
    if (exit_post_ride) {
        st.phase = PostRidePhase::Idle;
        st.ticks = 0;
        st.retry_logged = false;
        nvnHostSetUiPresentAspect(0.0f);
        std::fprintf(stderr, "[post-ride-wide] p%llu exit 16:9 -> 32:9 restored (scene=%s ev=%d)\n",
                     static_cast<unsigned long long>(now),
                     scene[0] ? scene : "?",
                     ev);
        std::fflush(stderr);
    } else if (++st.ticks == 30) {
        std::fprintf(stderr, "[post-ride-wide] p%llu maintain 16:9 (scene=%s)\n",
                     static_cast<unsigned long long>(now),
                     scene[0] ? scene : "?");
        std::fflush(stderr);
    }
}

}  // namespace

namespace loader {

void snap_wide_eval_copy(uint64_t src) {
    const float k = wide_k();
    if (k <= 1.0f) return;
    int fixed = 0;
    for (uint64_t o = 0; o + 64 <= 0x480; o += 4) {
        float *m = (float *)(src + o);   // column-major; a perspective projection has m[11] = -1 and zeros off its terms
        uint32_t w11;
        std::memcpy(&w11, m + 11, 4);
        if (w11 != 0xbf800000u) continue;
        if (m[1] != 0 || m[2] != 0 || m[3] != 0 || m[4] != 0 || m[6] != 0 || m[7] != 0 || m[12] != 0 || m[13] != 0 || m[15] != 0) continue;
        if (!(m[0] > 0 && m[5] > 0) || std::fabs(m[5] / m[0] / (16.0 / 9.0 * k) - 1.0) > 0.01) continue;
        m[0] *= k;
        fixed++;
    }
    static bool logged;
    if (fixed && !logged) { logged = true; std::fprintf(stderr, "ultrawide: photo Evaluation uses the 16:9 projection (x scale times %.4f)\n", k); }
    if (fixed) on_eval_copy_after_ride();
}
void snap_shiny_install() {}
void snap_neofilm_install() {}
void snap_skips_install() {}
void snap_lodanim_install() {
    const char *uw = std::getenv("SNAP_ULTRAWIDE");
    const bool wide = !(uw && (strcasecmp(uw, "off") == 0 || std::strcmp(uw, "0") == 0));
    if (const char *ws = std::getenv("SNAP_WIDE_SITES")) {
        for (const char *p = ws; *p && g_wide_nsites < 8;) {
            char *e;
            const uint64_t off = std::strtoull(p, &e, 0);
            if (*e != ':') break;
            const int reg = (int)std::strtol(e + 1, &e, 10);
            if (off && reg >= 0 && reg < 32) g_wide_sites[g_wide_nsites++] = {off, reg};
            if (*e != ',') break;
            p = e + 1;
        }
        trace("ultrawide: %d camera sites", g_wide_nsites);
    }
    if (wide) add_override("tanf", h_tanf);
}

}  // namespace loader

extern "C" {

// Called by host/loader.cpp die() on fatal load errors (Windows uses host/mods/crashlog.cpp).
void snap_crashlog_fatal(const char *kind, const char *detail) {
    std::fprintf(stderr, "[linux] fatal %s: %s\n", kind ? kind : "error", detail ? detail : "");
}

// Function-entry observers called by host/loader.cpp fnprof_hit() (disabled on Linux until mods are ported).
void snap_fn_observe(uint64_t, uint64_t, uint64_t) {}
void snap_skips_observe(uint64_t, uint64_t, uint64_t) {}
void snap_neo_observe(uint64_t, uint64_t, uint64_t) {}
void snap_highfps_observe(uint64_t, uint64_t, uint64_t) {}

// MAIN_INLINE_HOOKS no-ops: each leaves Cpu untouched so original lifted guest instructions run unchanged.
void rt_mod_entry(Cpu *) {}
void rt_gm_arena7_size(Cpu *) {}       // keeps original 56 MiB arena 7 size (host/boot.cpp overrides to 112 MiB)
void rt_gm_ao_simple(Cpu *) {}
void rt_gm_ao_hbao(Cpu *) {}
void rt_gm_pass(Cpu *) {}
void rt_gm_lod_ratio(Cpu *) {}
void rt_gm_lod_ratio_sub(Cpu *) {}
void rt_gm_anim_rate(Cpu *) {}
void rt_gm_font_atlas_info(Cpu *) {}
void rt_gm_font_atlas_dims(Cpu *) {}
void rt_gm_font_glyph1(Cpu *) {}
void rt_gm_font_glyph2(Cpu *) {}
void rt_gm_font_fix(Cpu *) {}
void rt_gm_neo_speed(Cpu *c) {
    on_ride_tick();
    if (c && hle::input::route_on() && ptr_plausible(c->x[19] + 0x70)) {
        hle::input::route_rail(c->x[19], *reinterpret_cast<const float *>(c->x[19] + 0x70));
    }
}
void rt_gm_film_c(Cpu *) {}
void rt_gm_skip_wait(Cpu *) { on_dialogue_wait(); }
void rt_gm_route_ui_event(Cpu *c) {
    if (!c) return;
    const uint64_t pe = c->x[1], pp = c->x[2];
    if (!ptr_plausible(pe) || !ptr_plausible(pp)) return;
    const int ev = *reinterpret_cast<const int *>(pe);
    const uint64_t player = *reinterpret_cast<const uint64_t *>(pp);
    char st[64]{};
    read_guest_str(c->x[3], st, sizeof(st));
    char nm[48]{};
    read_player_name(player, nm, sizeof(nm));
    update_post_ride_16_9_aspect(nm, ev);
    if (hle::input::route_on()) hle::input::route_ui(player, nm, st, ev, "", c->x[30]);
    if (ui_trace_enabled()) log_ui_transition("event", player, nm, st, ev);
}
void rt_gm_route_ui_setstate(Cpu *c) {
    if (!c) return;
    const uint64_t player = c->x[0], desc = c->x[1];
    if (!ptr_plausible(player) || !ptr_plausible(desc)) return;
    char st[64]{};
    if (!read_guest_str(desc + 0x18, st, sizeof(st))) return;
    char nm[48]{};
    read_player_name(player, nm, sizeof(nm));
    update_post_ride_16_9_aspect(nm, -1);
    if (hle::input::route_on()) hle::input::route_ui(player, nm, st, -1, "", c->x[30]);
    if (ui_trace_enabled()) log_ui_transition("setstate", player, nm, st, -1);
}
// rt_gm_ui_enter, rt_gm_ui_event, rt_gm_ui_pad and rt_gm_ui_pointer are provided by host/mods/uipace.cpp.
void rt_ui_opt_event(Cpu *) {}
void rt_ui_opt_state(Cpu *) {}

}  // extern "C"
