// SPDX-License-Identifier: MIT
// Controlled Linux guest runtime initialization (stage 5D-B2). See init_linux.h.
#include "init_linux.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio/audio_out.h"
#include "crash_linux.h"
#include "guest_tls_hooks.h"
#include "hle/hle.h"
#include "hle/hle_config.h"
#include "hle/live.h"
#include "loader.h"
#include "nvn/nvn_host.h"
#include "os/os.h"
#include "preflight_linux.h"
#include "window_opts_linux.h"

int snap_bind_audio();
void snap_bind_nvn();
int snap_nvn_bridge_count();
extern "C" void nvnHostNotifyWindowResize(void *nativeWindow);
extern "C" void snap_fs_drain_for_exit(int ms);
extern "C" void snap_ui_shutdown();

namespace snap_linux {
namespace {

HostWindow g_host_win;
std::atomic<uint64_t> g_presents{0};
std::atomic<uint64_t> g_submits{0};
std::atomic<bool> g_hooks_active{false};

extern "C" uint64_t snap_present_count() {
    return g_presents.load(std::memory_order_relaxed);
}

void apply_frame_rate_target(bool allow_pacing_set = true) {
    static std::atomic<unsigned> seen{0};
    static int s_target = 60;
    if (hle::live::changed(seen)) {
        int target = 60;
        {
            hle::live::EnvRead env_read;
            const char *value = std::getenv("SNAP_FPS");
            if (value && *value && std::strcmp(value, "default") != 0) {
                char *end = nullptr;
                const long parsed = std::strtol(value, &end, 10);
                if (end && *end == '\0' && (parsed == 72 || parsed == 90 || parsed == 120 || parsed == 144 || parsed == 165 || parsed == 240)) {
                    target = static_cast<int>(parsed);
                } else if (parsed != 60) {
                    std::fprintf(stderr, "[highfps] SNAP_FPS=%s ignored (use default, 60, 72, 90, 120, 144, 165 or 240)\n", value);
                }
            }
        }
        s_target = target;
        if (allow_pacing_set) {
            nvnHostSetFramePacing(static_cast<double>(s_target));
            const int cap = snap_window_frame_cap();
            std::fprintf(stderr, "[highfps] pacing target %d Hz%s\n", s_target,
                         cap > 0 && cap < s_target ? " (lowered by the frame-rate limit option)" : "");
        }
    }
}

double current_fps_target_hz() {
    const char *value = std::getenv("SNAP_FPS");
    if (value && *value && std::strcmp(value, "default") != 0) {
        char *end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end && *end == '\0' && (parsed == 72 || parsed == 90 || parsed == 120 || parsed == 144 || parsed == 165 || parsed == 240)) {
            return static_cast<double>(parsed);
        }
    }
    return 60.0;
}

bool ultrawide_disabled_by_env() {
    const char *uw = std::getenv("SNAP_ULTRAWIDE");
    return uw && (std::strcmp(uw, "off") == 0 || std::strcmp(uw, "OFF") == 0 || std::strcmp(uw, "0") == 0);
}

void apply_ultrawide_from_window(const HostWindow &win) {
    if (ultrawide_disabled_by_env() || !win.is_open()) {
        nvnHostSetUltrawide(1.0f);
        return;
    }
    nvnHostSetUltrawide(HostWindow::compute_ultrawide_factor(win.pixel_width(), win.pixel_height()));
}

// The factor is applied once, before the guest creates its render targets: nvn_wide.cpp sizes every 16:9 target from it at creation, and the
// Windows host sets it once in boot.cpp. The pump runs from the present hook, after those targets exist, so a later resize or monitor change
// must not rewrite it: existing targets keep the factor they were made with, and the replay threads read it without a lock. The swapchain still
// follows the window (nvnHostNotifyWindowResize); the 3D view keeps its aspect until the next start. Logged once per distinct value.
void note_ultrawide_aspect_change(const HostWindow &win) {
    if (ultrawide_disabled_by_env() || !win.is_open()) return;
    const float want = HostWindow::compute_ultrawide_factor(win.pixel_width(), win.pixel_height());
    const float have = nvnHostGetUltrawide();
    static float last_noted = 0.0f;
    if (std::fabs(want - have) > 0.01f && std::fabs(want - last_noted) > 0.01f) {
        last_noted = want;
        std::fprintf(stderr,
                     "init_linux: window aspect now needs ultrawide factor %.3f, render targets were created with %.3f; keeping %.3f until the next start\n",
                     want, have, have);
    }
}

void vi_get_native_window_host(Cpu *c) {
    if (c && c->x[0]) {
        *reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(c->x[0])) =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_host_win.native_handle()));
    }
    if (c) c->x[0] = 0;
}

void hook_present(void *, int slot, int w, int h) {
    const WindowEvents ev = pump_host_window_events();
    const uint64_t n = ++g_presents;
    const bool fast_boot_active = fast_boot_on_present(n, current_fps_target_hz());
    apply_frame_rate_target(!fast_boot_active);
    if (n <= 3 || n % 30 == 0) {
        loader::trace("nvn: present #%llu slot %d %dx%d (submits %llu)",
                      static_cast<unsigned long long>(n), slot, w, h,
                      static_cast<unsigned long long>(g_submits.load(std::memory_order_relaxed)));
    }
    int exit_code = 0;
    if (check_present_exit(ev.quit_requested, &exit_code)) {
        if (shutdown_requested()) {
            std::fprintf(stderr, "init_linux: exiting present loop on shutdown signal %d\n", shutdown_signal());
        }
        // Why _Exit and not a normal return: this runs on a guest thread, and the guest's own threads (main, workers, audio) cannot be stopped
        // cooperatively, so there is nothing to join and a normal exit() would run static destructors under live guest threads (undefined
        // behavior). What can be done safely first: let a save commit in progress finish (the same drain the Windows host does), and flush stdio,
        // because _Exit does not. The SDL window is not destroyed here on purpose: this is not the thread that created it, other threads may be
        // presenting to its surface right now, and the compositor drops it when the process ends.
        snap_audio_log_checks();
        snap_fs_drain_for_exit(5000);
        std::fflush(nullptr);
        std::_Exit(exit_code);
    }
    window_opts_wait_if_paused(
        [](void *) -> bool {
            const WindowEvents pev = pump_host_window_events();
            int code = 0;
            return !check_present_exit(pev.quit_requested, &code);
        },
        nullptr);
}

void hook_submit(void *, int, int) {
    ++g_submits;
}

int svc_dispatch(Cpu *c, uint32_t imm) {
    return os_svc(c, imm) ? 1 : 0;
}

void hle_bind_one(const char *sym, GuestFn fn, void *) {
    if (hle::find_override(sym) == fn) loader::add_override(sym, fn);
    else loader::add_weak_override(sym, fn);
}

uint64_t g_opus_vtbl[4];
void opus_ctor(Cpu *c) {
    if (c && c->x[0]) *reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(c->x[0])) = reinterpret_cast<uint64_t>(g_opus_vtbl);
}
void opus_nop(Cpu *) {}
void opus_work_size(Cpu *c) {
    if (c) c->x[0] = 0x20000ull * (c->x[2] ? c->x[2] : 1);
}
uint64_t g_ngc_vtbl[4];
void ngc_ctor(Cpu *c) {
    if (c && c->x[0]) *reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(c->x[0])) = reinterpret_cast<uint64_t>(g_ngc_vtbl);
}

int bind_codec_stubs() {
    g_opus_vtbl[0] = loader::add_override("host:HardwareOpusDecoder::dtor", opus_nop)->addr;
    g_opus_vtbl[1] = loader::add_override("host:HardwareOpusDecoder::deleting_dtor", opus_nop)->addr;
    g_opus_vtbl[2] = loader::add_override("host:HardwareOpusDecoder::GetWorkBufferSize", opus_work_size)->addr;
    loader::add_override("_ZN2nn5codec19HardwareOpusDecoderC1Ei", opus_ctor);

    g_ngc_vtbl[0] = loader::add_override("host:ProfanityFilter::dtor", opus_nop)->addr;
    g_ngc_vtbl[1] = loader::add_override("host:ProfanityFilter::deleting_dtor", opus_nop)->addr;
    loader::add_override("_ZN2nn3ngc15ProfanityFilterC1Ev", ngc_ctor);
    return 2;
}

}  // namespace

const char *init_stage_name(InitStage s) {
    switch (s) {
    case InitStage::None: return "none";
    case InitStage::Preflight: return "preflight";
    case InitStage::Window: return "window";
    case InitStage::Overrides: return "overrides";
    case InitStage::LoaderLoad: return "loader::load";
    case InitStage::OsInit: return "os_init";
    case InitStage::TlsHooks: return "install_guest_tls_hooks";
    case InitStage::NewThread: return "loader::new_thread";
    case InitStage::AdoptMainThread: return "os_adopt_main_thread";
    case InitStage::ReadyBeforeInitializers: return "ready_before_initializers";
    }
    return "unknown";
}

bool init_guest_runtime(const BootOpts &opts, bool validate_archives, InitResult &out) {
    out = InitResult{};

    // 1. Static preflight before mapping any guest address space.
    out.stage = InitStage::Preflight;
    const ArtifactCheck main_elf = validate_guest_elf(opts.main_elf, "main", 0x7100000000ull, 0x10000000ull);
    const ArtifactCheck sdk_elf = validate_guest_elf(opts.sdk_elf, "sdk", 0x7110000000ull, 0x1f0000000ull);
    if (main_elf.status != PreflightStatus::Pass) {
        out.error = "main ELF preflight failed: " + main_elf.detail;
        return false;
    }
    if (sdk_elf.status != PreflightStatus::Pass) {
        out.error = "sdk ELF preflight failed: " + sdk_elf.detail;
        return false;
    }
    if (validate_archives) {
        const ArtifactCheck main_lib = validate_aot_archive(opts.main_lib, "main");
        const ArtifactCheck sdk_lib = validate_aot_archive(opts.sdk_lib, "sdk");
        if (main_lib.status != PreflightStatus::Pass) {
            out.error = "main AOT archive preflight failed: " + main_lib.detail;
            return false;
        }
        if (sdk_lib.status != PreflightStatus::Pass) {
            out.error = "sdk AOT archive preflight failed: " + sdk_lib.detail;
            return false;
        }
    }

    if (shutdown_requested()) {
        out.error = "shutdown requested before host window creation";
        return false;
    }

    // 2. Initialize SDL3 host window BEFORE loader::load / NVN so nn::vi::GetNativeWindow
    // has a valid SDL_Window* ready before guest code ever touches NVN.
    out.stage = InitStage::Window;
    if (!opts.no_window && !std::getenv("SNAP_HEADLESS")) {
        WindowConfig wcfg;
        window_opts_initial_config(&wcfg);
        wcfg.hidden = (opts.cmd == "init" || opts.stop_after_init);
        if (g_host_win.create(wcfg)) {
            out.window_created = true;
            out.native_window = g_host_win.native_handle();
            WindowOptsCallbacks wcb;
            wcb.ctx = &g_host_win;
            wcb.set_fullscreen = [](void *c, bool fs) { return static_cast<HostWindow *>(c)->set_fullscreen(fs); };
            wcb.set_monitor = [](void *c, int idx) { return static_cast<HostWindow *>(c)->set_display(idx); };
            wcb.set_size = [](void *c, int w, int h) { return static_cast<HostWindow *>(c)->set_window_size(w, h); };
            wcb.monitor_count = [](void *) { return HostWindow::display_count(); };
            wcb.monitor_name = [](void *, int idx) { return HostWindow::display_name(idx); };
            window_opts_init(&wcb);
            apply_ultrawide_from_window(g_host_win);
        } else {
            std::fprintf(stderr, "init_linux: warning: SDL3 window creation failed; continuing headless\n");
        }
    }

    // 3. Register host overrides before loader::load() so relocations and import tables bind to them.
    out.stage = InitStage::Overrides;
    loader::install_libc_overrides();
    hle::init_trace_from_env();
    hle::load_config("");
    hle::activate_profiles();
    // Apply the profile's frame-rate target before the first NVN present. The present hook also
    // calls this helper so changes made in the live options menu take effect without restarting.
    apply_frame_rate_target();
    hle::bind_all(hle_bind_one, nullptr);
    out.hle_overrides_bound = hle::overrides().size();
    hle::set_log_sink([](const char *l, size_t n) { loader::trace("%.*s", (int)n, l); });
    hle::os_hooks().init_system_event = [](void *ev, bool manual) { os_system_event_create((uint64_t)(uintptr_t)ev, manual ? 0 : 1); };
    hle::os_hooks().signal_system_event = [](void *ev) { os_system_event_signal((uint64_t)(uintptr_t)ev); };
    out.audio_overrides_bound = snap_bind_audio();
    snap_audio_set_os_hooks([](void *ev) { os_system_event_signal(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ev))); });
    snap_audio_set_event_init_hook([](void *ev) { os_system_event_create(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ev)), 1); });
    snap_bind_nvn();
    out.nvn_bound = true;
    out.nvn_procs_bound = snap_nvn_bridge_count();
    out.codec_stubs_bound = bind_codec_stubs();
    loader::add_override("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window_host);
    {
        static const NvnHostHooks hk{nullptr, hook_present, hook_submit};
        nvnHostSetHooks(&hk);
        g_hooks_active.store(true, std::memory_order_release);
        out.hooks_installed = true;
    }
    size_t os_count = 0;
    const OsOverride *os_tab = os_override_table(&os_count);
    for (size_t i = 0; i < os_count; ++i) {
        loader::add_override(os_tab[i].sym, os_tab[i].fn);
    }
    for (const auto &st : opts.stubs) {
        loader::add_probe_stub(st);
    }

    // 3. Map and relocate guest modules (executes zero guest instructions).
    out.stage = InitStage::LoaderLoad;
    if (!loader::load(opts.main_elf.c_str(), opts.sdk_elf.c_str())) {
        out.error = "loader::load() returned false";
        return false;
    }

    if (shutdown_requested()) {
        out.error = "shutdown requested after loader::load()";
        return false;
    }

    // 4. Initialize Horizon OS scheduler with guest TLS hook variable addresses.
    out.stage = InitStage::OsInit;
    OsConfig oc = {};
    oc.trace = std::getenv("SNAP_OS_TRACE") != nullptr ? 1 : 0;
    oc.tp_init_var = loader::lookup("nnmuslThreadPointerInitializer");
    oc.tp_destroy_var = loader::lookup("nnmuslThreadPointerDestroyer");
    os_init(&oc);

    // 5. Bind musl thread-pointer hooks before creating the first guest thread.
    out.stage = InitStage::TlsHooks;
    out.tls_hooks_bound = snap_host::install_guest_tls_hooks(oc);

    loader::g_svc_handler = svc_dispatch;
    loader::g_spin_gate_fn = os_spin_gate;
    loader::g_ticks_fn = os_ticks;

    // 6. Allocate main guest thread (captures main's .tdata image into GuestTlsImage).
    out.stage = InitStage::NewThread;
    loader::GuestThread *th = loader::new_thread();
    if (!th || !th->c) {
        out.error = "loader::new_thread() returned null";
        return false;
    }
    Cpu *c = th->c;

    // 7. Adopt main thread into scheduler and publish ELF thread pointer into TLR+0x1f0.
    out.stage = InitStage::AdoptMainThread;
    os_adopt_main_thread(c, 0, -1);
    *reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(c->tpidrro + 0x1f0)) = c->tpidr;

    out.main_cpu = c;
    out.main_thread_type = os_current_thread_type();
    out.tpidr = c->tpidr;
    out.tlr_tp_word = *reinterpret_cast<const uint64_t *>(static_cast<uintptr_t>(c->tpidrro + 0x1f0));
    out.initializers_run = static_cast<uint64_t>(loader::g_init_log.size());

    if (!out.tpidr || out.tlr_tp_word != out.tpidr || os_current_cpu() != c || !out.main_thread_type) {
        out.error = "post-adopt invariant check failed (tpidr/TLR/os_current_cpu)";
        return false;
    }

    // Stop explicitly before run_initializers(), nninitStartup(), and nnMain().
    out.stage = InitStage::ReadyBeforeInitializers;
    out.ok = true;
    std::fprintf(stderr,
                 "init_linux: main=%llu funcs, sdk=%llu funcs, tls_hooks=%d, tpidr=%#llx, tpidrro=%#llx; "
                 "stopping before run_initializers() and nnMain() (stage 5D-B2)\n",
                 static_cast<unsigned long long>(loader::g_main.fn_count),
                 static_cast<unsigned long long>(loader::g_sdk.fn_count),
                 out.tls_hooks_bound,
                 static_cast<unsigned long long>(out.tpidr),
                 static_cast<unsigned long long>(c->tpidrro));
    return true;
}

WindowEvents pump_host_window_events() {
    if (!g_host_win.is_open()) return {};
    const WindowEvents ev = g_host_win.poll_events();
    if (ev.toggle_fullscreen) snap_window_toggle_fullscreen();
    if (ev.focus_changed) window_opts_on_focus(ev.focused && !ev.minimized);
    if (ev.resized || ev.display_changed) {
        note_ultrawide_aspect_change(g_host_win);
        nvnHostNotifyWindowResize(g_host_win.native_handle());
    }
    return ev;
}

bool host_window_quit_requested() {
    return g_host_win.quit_requested();
}

HostWindow *host_window() {
    return g_host_win.is_open() ? &g_host_win : nullptr;
}

void shutdown_guest_runtime() {
    std::fprintf(stderr, "init_linux: shutting down guest runtime (vsync -> hooks -> ui -> audio -> window)\n");
    hle::vi_stop_vsync();
    if (g_hooks_active.exchange(false, std::memory_order_acq_rel)) {
        nvnHostSetHooks(nullptr);
    }
    snap_ui_shutdown();
    window_opts_shutdown();
    snap_audio_log_checks();
    snap_audio_shutdown();
    g_host_win.destroy();
}

bool prepare_run_pipeline(const BootOpts &opts, const InitResult &init, RunPlan &out, std::string &err) {
    out = RunPlan{};
    err.clear();
    if (!init.ok || init.stage != InitStage::ReadyBeforeInitializers || !init.main_cpu || !init.tpidr ||
        init.tlr_tp_word != init.tpidr) {
        err = "init_guest_runtime has not reached ReadyBeforeInitializers";
        return false;
    }
    out.main_cpu = init.main_cpu;
    out.startup_addr = loader::lookup("nninitStartup");
    out.nnmain_addr = loader::lookup("nnMain");
    out.stop_after_init = opts.stop_after_init;
    out.trace_enabled = !opts.trace_file.empty();
    out.valid = true;
    return true;
}

bool execute_run_pipeline(const RunPlan &plan, const GuestExecutionOps &ops, RunResult &out, std::string &err) {
    out = RunResult{};
    err.clear();
    if (!plan.valid || !plan.main_cpu) {
        err = "invalid RunPlan";
        return false;
    }
    if (!ops.run_initializers || !ops.call_guest) {
        err = "GuestExecutionOps missing required entry points";
        return false;
    }
    std::fprintf(stderr, "run_linux: entering run_initializers()\n");
    out.initializers_run = ops.run_initializers(plan.main_cpu, true);
    std::fprintf(stderr, "run_linux: run_initializers() completed (%d initializers)\n", out.initializers_run);
    if (plan.stop_after_init) {
        std::fprintf(stderr, "run_linux: --stop-after-init set, stopping before nninitStartup/nnMain\n");
        out.stopped_after_init = true;
        if (ops.stop_vi_vsync) {
            ops.stop_vi_vsync();
            out.stopped_vsync = true;
        }
        out.ok = true;
        return true;
    }
    if (plan.startup_addr) {
        std::fprintf(stderr, "run_linux: entering nninitStartup (%#llx)\n",
                     static_cast<unsigned long long>(plan.startup_addr));
        ops.call_guest(plan.main_cpu, plan.startup_addr);
        std::fprintf(stderr, "run_linux: nninitStartup returned\n");
        out.called_startup = true;
    }
    if (plan.nnmain_addr) {
        std::fprintf(stderr, "run_linux: entering nnMain (%#llx)\n",
                     static_cast<unsigned long long>(plan.nnmain_addr));
        ops.call_guest(plan.main_cpu, plan.nnmain_addr);
        std::fprintf(stderr, "run_linux: nnMain returned\n");
        out.called_nnmain = true;
    }
    if (ops.stop_vi_vsync) {
        ops.stop_vi_vsync();
        out.stopped_vsync = true;
    }
    out.ok = true;
    return true;
}

}  // namespace snap_linux
