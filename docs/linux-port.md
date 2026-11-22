# Cross-platform build and the Linux port

Status (2026-11-18): **the Linux host builds, and its unit and integration tests pass; running the game end to end is experimental and not covered by any automated test.** This page records what exists, how to build and test it, what is stubbed, and what was and was not checked. Statements are facts that were checked unless a line says otherwise.

The sections from "Goal" to "Stage 4c" are the records written when each stage landed. Their counts ("159 files", "26 shared") and their "not yet" sentences are **not** kept up to date; this section and "Known limits" are. `cmake/source_manifest.txt` is the current source classification (94 `shared`, 20 `linux`, 66 `windows`, 5 `offline` at the time of writing).

## Current state

### What was checked, and where

Checked on 2026-11-18 in the `snap-recomp-linux` distrobox (Fedora 43 container image, clang 21.1.8, CMake 3.31, Ninja 1.13, SDL 3.4.20, kernel 7.3.3), configured with `-G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`:

- `ninja` (all targets) builds with no failure, and **all 45 ctest tests pass**.
- The `snap_boot_linux` runner also builds and links in its AOT configuration (`-DSNAP_AOT_MAIN_LIB=... -DSNAP_AOT_SDK_LIB=...` pointing at locally built `libsnap_main.a` / `libsnap_sdk.a`), and `snap_boot_linux --help` runs. That binary was **not** used to execute guest code.
- **What the passing tests do not show.** `nvn_renderer_linux`, `ui_overlay_linux` and `window_sdl3_linux` have explicit skip branches for a machine with no Vulkan 1.3 GPU or no real SDL video. In the container above they took those branches (the renderer test reports "no Vulkan 1.3 ICD/GPU available; DeviceInitialize returned 0 cleanly"), so **no test run here rendered a frame**. The audio tests use SDL's dummy audio driver: no real audio device was opened.
- **Not built or run:** the Windows build (`host/build_snap.py`, `tests/os/build.bat`, `tests/hle/run_tests.ps1`). Windows-side edits made by the Linux port were reviewed by reading only. This holds for every Windows file the port touched.

### Components

| Component | State | Where | Covered by (ctest) |
|---|---|---|---|
| OS layer: scheduler, threads, fibers, virtual memory (POSIX primitives, System V context switch) | ported | `runtime/os/`, `runtime/os/linux/` | `ctx_sysv*`, `os_host*`, `os_scheduler*`, `os_state_off`, `os_posix_header` |
| Guest TLS and `.tdata` handling | ported | `runtime/os/guest_tls_block.*`, `host/guest_tls_hooks.*`, `runtime/hle/guest_tls.h` | `guest_tls*`, `tls_host_fibers*` (synthetic images only) |
| Loader integration, ELF and AOT archive preflight | ported | `host/loader.cpp`, `host/linux/preflight_linux.*`, `host/linux/init_linux.*` | `preflight_linux`, `init_linux`, `hooks_linux` (synthetic ELFs only) |
| AOT library build (`libsnap_main.a`, `libsnap_sdk.a`) | ported | `recomp/snap_build_linux.py` | `aot_build_linux` (synthetic sources) |
| Runner, signal handling, crash diagnostics | ported | `host/linux/runner_linux.*`, `host/linux/crash_linux.*` | `boot_linux*`, `crash_linux` |
| HLE: filesystem, profiles, save lock, config | ported | `runtime/hle/`, `runtime/hle/linux/` | `hle_core_linux`, `fs_linux`, `profiles_linux` |
| Input: keyboard, mouse, SDL3 gamepads | ported | `runtime/hle/linux/input_backend_sdl3.cpp` | `hid_linux`, `sdl3_backend_linux` |
| Audio (SDL3 backend) | ported | `runtime/audio/linux/` | `audio_core_linux`, `audio_sdl3_linux`, `audio_runtime_linux` (dummy driver) |
| NVN to Vulkan renderer | builds; **not exercised on a GPU in the test run** | `runtime/nvn/`, `runtime/nvn/linux/` | `nvn_os_host_linux`, `nvn_renderer_linux` (skip branch), `window_sdl3_linux` (skip branch) |
| SDL3 window, event loop, swapchain size | ported | `host/linux/window_sdl3.*` | `window_sdl3_linux` (skip branch), `init_linux` |
| ImGui overlay (ImGui 1.93.0, Vulkan + SDL3 backends) | builds; not exercised on a GPU in the test run | `runtime/ui/`, `runtime/ui/linux/` | `ui_overlay_linux` (skip branch) |
| ShaderHarness (shader translation) run as a child process | implemented; not run here (needs the separately built .NET harness) | `runtime/nvn/linux/nvn_os_host_posix.cpp` `run_shader_harness` | `nvn_os_host_linux` (child-process cases with stand-in scripts, not the real harness) |
| Ultrawide 32:9 | experimental, see below | `runtime/nvn/nvn_wide.cpp`, `host/linux/hooks_mods_stub.cpp`, `host/linux/window_sdl3.cpp` | none automated |

### Stubbed or not ported (explicit, not silent)

- **Not supported on Linux, reported as such:** XeSS (`xess` stub), frame generation (`nvn_upscale_stub.cpp`), and the Windows-imaging features (HD photo capture, free-camera clean shot, button-icon replacement) (`runtime/nvn/linux/nvn_optional_stubs.cpp`).
- **HDR10 / scRGB (`runtime/nvn/nvn_hdr.cpp`, `runtime/nvn/linux/nvn_hdr_posix.cpp`, `host/linux/window_sdl3.cpp`):**
  - **Implemented:** Shared Vulkan swapchain/presentation pass (`VK_EXT_swapchain_colorspace`, optional `VK_EXT_hdr_metadata`), SDL3 window HDR property queries (`SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`, `SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT`, `SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT`), and event-driven refresh on `SDL_EVENT_WINDOW_DISPLAY_CHANGED` / `SDL_EVENT_WINDOW_HDR_STATE_CHANGED` (`snap_hdr::NotifyDisplayChanged()` -> swapchain recreation + re-query of display peak/metadata).
  - **Wayland vs X11 & SDR fallback:** Wayland (`color-management-v1` / KDE Plasma 6) can report HDR properties when compositor HDR is enabled; X11 does not expose HDR properties via SDL3 and returns `0` (never assumes HDR). `SNAP_HDR_PEAK_NITS` / `SNAP_HDR_PEAK` overrides the peak and `SNAP_HDR_PAPER_NITS` sets paper white (defaults: `1000` / `200` nits). Automatically falls back to SDR if SDL video is uninitialized/absent or if the compositor/WSI/surface/driver does not advertise `scRGB` (`R16G16B16A16_SFLOAT`) or `HDR10_ST2084` (`A2B10G10R10_UNORM_PACK32`).
- **AMD FSR 3.1 Super Resolution (`SNAP_UPSCALER=fsr`, `runtime/nvn/linux/upscale_fsr_linux.cpp`):**
  - **Implemented:** Native Linux Vulkan backend using the vendored MIT AMD FidelityFX SDK v1.1.4 subset (`third_party/FidelityFX/`, static target `snap_ffx_fsr3_linux`) with 88 precompiled SPIR-V compute permutations (`third_party/FidelityFX/spirv/ffx_fsr3upscaler_vk.h`: HDR/SDR, inverted/standard depth, FP16/FP32). Does **not** implement Frame Generation.
  - **Selection & Quality Modes:** Set `SNAP_UPSCALER=fsr` (or `auto`, which selects FSR automatically when DLSS/NGX is unavailable) and `SNAP_UPSCALE_QUALITY=native|quality|balanced|performance` (`1.0x`, `1.5x`, `1.724x`, `2.0x` per axis).
  - **Vulkan Requirements:** Vulkan 1.3 (required by the NVN renderer). Optional extensions queried via `vkGetPhysicalDeviceFeatures2` / `ffx_vk.cpp`: `VK_KHR_shader_float16_int8` (FP16 shader math), `VK_EXT_subgroup_size_control`, `VK_KHR_dedicated_allocation`, `VK_KHR_synchronization2`.
  - **FSR vs DLSS on Linux:** DLSS (`SNAP_UPSCALER=dlss`, `runtime/nvn/linux/upscale_dlss_posix.cpp`) uses NVIDIA NGX (`libnvidia-ngx.so.1` + `libnvidia-ngx-dlss.so.*`, NVIDIA GPUs only). FSR 3.1 (`SNAP_UPSCALER=fsr`) is compiled into the binary with zero proprietary runtime dependencies and works without NGX. On NVIDIA RTX 5070 Ti (5120×1440 ultrawide, HDR10, 240 Hz), FSR Quality measured ~0.59 ms GPU (`~1821 MiB` VRAM) vs DLSS Quality ~1.87 ms GPU (`~2084 MiB` VRAM).
  - **GPU Vendor Status:** Validated on **NVIDIA RTX 5070 Ti** (Vulkan 1.4, driver 620.70.06). **AMD (Mesa RADV)** and **Intel (Mesa ANV)** have **not yet been tested on real hardware**; `kPermUseLanczosLut` (`LANCZOS_TYPE=1` SPIR-V permutations in `third_party/FidelityFX/spirv/ffx_fsr3upscaler_lanczos_vk.h` binding `r_lanczos_lut`) is implemented for GPUs reporting `waveLaneCountMin=32, waveLaneCountMax=64`.
  - **Known Motion Vector Limitations:** `upscale_mv.comp` / `upscale_mv_spv.h` generates camera-reprojection motion vectors from the depth buffer only (`prev - cur` in render pixels, unjittered). Moving Pokémon, particles, transparencies, and animated foliage/water lack per-object motion vectors and can exhibit temporal ghosting or disocclusion under both FSR 3.1 and DLSS.
  - **Diagnostic Logs:** Run with `SNAP_UPSCALE_LOG=1` (backend selection, context resolution, `p300` stats), `SNAP_UPSCALE_LOG=2` (enables `FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING` and per-frame logs), and `SNAP_PERF=1 SNAP_GPU_PASSES=/tmp/passes.csv` (per-pass Vulkan GPU timestamps for `upscale mv` and `upscale eval`).
- **DLSS (`SNAP_UPSCALER=dlss`, `runtime/nvn/linux/upscale_dlss_posix.cpp`):** Native Linux Vulkan DLSS Super Resolution via `libnvidia-ngx.so.1` (NVIDIA GPUs). DLSS Frame Generation is not implemented.
- **XeSS (`upscale_xess.cpp`):** Remains stubbed on Linux (`nvn_upscale_stub.cpp`).
- **High Frame Rate (`>60 FPS`, validated up to 240 FPS on Bazzite):**
  - `SNAP_FPS` (`default|60|72|90|120|144|165|240`, F1 **Frame rate target**) sets `nvnHostSetFramePacing()` (`g_paceHz`) live via `apply_frame_rate_target()` (`host/linux/init_linux.cpp`).
  - `host/mods/uipace.cpp` (`rt_gm_ui_enter`, `rt_gm_ui_event`, `rt_gm_ui_pad`, `rt_gm_ui_pointer`) is compiled on Linux so UI handlers, dialogue, pad edge sampling, and pause-menu pointer speed remain at 60 Hz while 3D rendering runs at 72–240 FPS (`SNAP_UI_PACE`, `SNAP_PAD_PACE`, `SNAP_PTR_PACE` kill switches preserved).
  - `SNAP_VSYNC` (`on|adaptive|mailbox|off`) selects the Vulkan swapchain present mode (`FIFO`, `FIFO_RELAXED`, `MAILBOX`, `IMMEDIATE`).
  - **Frame rate limit (`frame_cap`):** `SNAP_WINDOW_FRAME_CAP` / `window.json` `"frame_cap"` (F1 **Frame rate limit**) clamps `g_paceHz` whenever `frame_cap > 0 && frame_cap < SNAP_FPS` (e.g. `frame_cap: 120` caps `SNAP_FPS=240` to `120 FPS`). Set `frame_cap` to `0` (**Off**) for 144/240 FPS.
  - **Known unverified edge cases:** integer-ms timer truncation drift (`floor(delta_us/1000)` at 144/240 FPS), `SNAP_FPS_UNCAPPED` long-session behavior, and `lodanim` frame-count distant animation speed.
- **Overlay tabs:** the saves tab, the problem-report export, window-mode options and the scripted UI test are placeholders (`runtime/ui/linux/ui_linux_stubs.cpp`).
- **Mods:** the photo export, shiny, NEO-ONE/film and skip mods are not installed (`host/linux/hooks_mods_stub.cpp`). Of the remaining game-inline hooks (`rt_gm_*`), `rt_gm_ui_enter`, `rt_gm_ui_event`, `rt_gm_ui_pad`, and `rt_gm_ui_pointer` are implemented by `host/mods/uipace.cpp`, while the rest are no-ops. The cheat tick is a weak no-op, so cheats do not run.
- **Save states** are off (`os_state_off.cpp`): requests are refused and say so.
- **Write tracking** does not exist (`write_watch_supported()` is false): the NVN layer copies whole ranges, which is correct and slower. `NVN_WB_WATCH` is unavailable.
- **Weak link seams:** `runtime/hle/linux/hle_host_posix.cpp` and the test stubs define weak defaults (for example `snap_pace_vsync_wait`, `snap_state_*`, a null input backend, `snap_ui_pick_profile`). Where the real implementation is linked it wins; where it is not, the weak default is used **without a message**. `show_error_dialog` does nothing on Linux, so such errors reach stderr only.
- **Runner commands:** only `validate`, `init` and `run` do anything. `info`, `shared`, `eh`, `lua`, `bench` and `all` are accepted but report "not wired yet".

### Building and testing

Requirements: clang (the configure step refuses other compilers), CMake, Ninja, Python 3, the Vulkan headers, and SDL3 development files. SDL3 is optional (`SNAP_ENABLE_SDL3`, default on): without it the SDL-dependent targets are not built. Vendored: ImGui under `runtime/ui/third_party/imgui`.

```
cmake -S . -B build/linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/linux
ctest --test-dir build/linux --output-on-failure
```

Notes from the runs above:

- Several tests create their files under a **fixed `/tmp`** and ignore `TMPDIR`. If `/tmp` is full or read-only, `init_linux` cannot write its synthetic ELFs; it now says so and stops, where it used to continue into a crash. That failure was reproduced on purpose with a 4 KiB `/tmp` and is what an earlier "known failure" of this test turned out to be; it has nothing to do with SDL's dummy driver.
- `source_classification` calls `git ls-files`, so it needs a git checkout (it fails in an exported tree).

### Running the guest (experimental)

Needs files that this repository never contains and that you provide from your own copy of the game: the `main` and `sdk` modules as ELF (`--main`, `--sdk`), and the two AOT archives built from your own generated sources (`--main-lib`, `--sdk-lib`), built for runtime ABI 3 (`SNAP_RUNTIME_ABI` in `runtime/cpu.h`; archives from another ABI do not link). `recomp/snap_build_linux.py` builds the archives from `generated/v262144/{main,sdk}`; producing that directory is not covered by the Linux tests.

- `snap_boot_linux validate ...` checks the artifacts without loading them for execution; `init` maps the modules and stops before any guest instruction.
- `snap_boot_linux run ...` prepares the pipeline and stops. **Guest execution is off unless `--allow-guest-exec` is given** (add `--stop-after-init` to stop after the initializers). Nothing in the automated suite executes the game, and this page makes no statement about whether it plays.
- A window is created before the guest starts. `--no-window` or `SNAP_HEADLESS` skips it. `SNAP_WINDOW_W`, `SNAP_WINDOW_H` (at least 64), `SNAP_FULLSCREEN=1`, `SNAP_DISPLAY=<1-based index>`, `SNAP_ULTRAWIDE=off` are read by the window code. `NVN_SHADER_HARNESS` and `SNAP_HARNESS_TIMEOUT_MS` (default 600000) apply to the ShaderHarness.

### Shutdown and signals

`install_signal_handlers` (`host/linux/crash_linux.cpp`) handles `SIGSEGV`, `SIGBUS`, `SIGILL` and `SIGFPE` with an async-signal-safe report on an alternate stack, then re-raises. It refuses to install over a handler someone else already registered. `SIGINT`, `SIGTERM` and `SIGHUP` only record a request; the present loop (`hook_present` in `init_linux.cpp`) polls it every frame through `check_present_exit` and leaves with `128 + signal` (a window close leaves with `0`; a pending signal wins). A **second** such signal before the first is honored means nobody is polling (the guest stopped presenting) and the handler ends the process at once with `_exit(128 + signal)`.

Why the present loop ends with `_Exit` and not a normal return: it runs on a guest thread, and the guest's own threads cannot be stopped cooperatively, so there is nothing to join and `exit()` would run static destructors under live guest threads. Before `_Exit` it waits for a save commit in progress (`snap_fs_drain_for_exit`) and flushes stdio. It does not destroy the SDL window: that is not the thread that created it and other threads may be presenting to it. Until the first present, and whenever the guest stops presenting, only the second signal ends the process. `crash_linux` tests the first-request and second-signal behavior in child processes; **the present-loop path itself runs only with guest code, which no test executes.**

### Ultrawide 32:9 (experimental)

What it does (code read and built; behavior reported by the author, see below):

- The factor is `k = pixel_width / pixel_height / (16/9)`, used when above 1.01 and capped at 4.0 (`HostWindow::compute_ultrawide_factor`); 5120x1440 gives 2.0. `SNAP_ULTRAWIDE=off` (or `0`) disables it.
- The swapchain takes the window's real pixel size (`window_pixel_size`, from `SDL_GetWindowSizeInPixels`) when the surface does not fix an extent, clamped to the surface limits. The SDL window is synchronized (`SDL_SyncWindow`) after creation and after a fullscreen change so the size is final before it is read.
- The ride and lab camera's aspect is widened at its `tanf` call (main offset `0x296400`, build 2.0.1) only when it is exactly 16:9, so the vertical field of view is unchanged. The interface and saved photos stay 16:9. The photo Evaluation is given the 16:9 projection back (`snap_wide_eval_copy`; the same code as the Windows host). The photo-mode viewfinder mask keeps its 16:9 opening and grey side strips are drawn outside it (`WideDrawViewfinderSide`).
- The factor is applied **once**, before the guest creates its render targets, as on Windows. Resizing the window or moving it to another monitor later does not change it (the swapchain follows, the 3D view keeps its aspect until the next start); a note is logged when the window would now want another factor.

What was and was not checked: the author reports a manual check on one 5120x1440 display (swapchain size, widened camera, viewfinder mask). That is not an automated test and was **not reproduced** while writing this page. Photo scores were not compared with and without ultrawide on Linux.

Known limitations: the course-selection / SKIP transition screen is still pillarboxed (reported by the author); other screens may need their own adjustments; the camera site and the viewfinder-mask shader key are specific to build 2.0.1; the widening logic is duplicated between `host/mods/lodanim.cpp` (Windows) and `host/linux/hooks_mods_stub.cpp` (Linux) and can drift.

### Known issues and risks (not fixed)

- SDL events are polled in `hook_present`, on the thread that presents, while SDL's video subsystem was initialized on the runner thread. SDL documents that event pumping belongs to the thread that initialized video. This was not investigated on a real display.
- The signal handlers use an alternate stack only on the thread that installed them, so a stack overflow on another thread (a guest fiber, a worker) is not reported by the handler.
- `-w` (all warnings off) is used on most Linux test and runner targets, as in the Windows build.
- A weak default can hide a missing implementation (see above).
- Duplicated ultrawide logic (above).

## Goal

Keep one source tree for the Windows game (built by `host/build_snap.py`, unchanged) and add a native Linux backend in layers. This stage does not add any Linux code. It makes the split between shared, Windows and Linux code explicit and checked, so later stages can move a file from "Windows" to "shared" (or add a Linux sibling) one reviewable step at a time.

## Source classes

Every C/C++ file under `host/` and `runtime/` (159 files) is listed once in `cmake/source_manifest.txt`:

| Class | Meaning | Files now |
|---|---|---|
| `shared` | Portable as written: no Windows header, Win32 API or MSVC-only construct, directly or through the repo headers it includes, **and** it compiles with clang on Linux | 26 |
| `windows` | Windows-only today. The comment on the line says what the scanner found first. When a Linux implementation lands, the file moves to `shared` (if its branches are complete) or gets a `linux` sibling | 128 |
| `linux` | Linux-only. None yet | 0 |
| `offline` | Not compiled into the game by any target: spikes, unit-test sources, reference code, a file that another file `#include`s | 5 |

How a file is judged (`scripts/source_classes.py`): it reads the file and every repo header it includes, ignoring comments, and looks for Windows-only headers (`<windows.h>`, `<d3d11.h>`, ...), Win32 and MSVC names (`VirtualAlloc`, `HANDLE`, `__declspec`, `_MSC_VER`, `#pragma comment`, ...) and the `_WIN32` / `VK_USE_PLATFORM_WIN32_KHR` switches. A file with a `_WIN32` branch is `windows` even when the branch is guarded, because the guard means the other side does not exist yet. Vendored code under `third_party/` is not scanned (its `_WIN32` branches are the library's own portability code); its Windows backends (`imgui_impl_win32.cpp`, `imgui_impl_dx11.cpp`) are classified by name. One header is a reviewed platform switch with a working other side: `runtime/simd.h` (`SIMD_SELECTANY`); the list is `_REVIEWED_SWITCHES` in the script. Add an entry only after reading both sides.

The scanner is a filter, not a proof. A `shared` file must also compile on Linux, and the build below does that.

### Adding or moving a file

1. Add the file, then run `python3 -I scripts/source_classes.py`. It lists unclassified files with a suggested class, and complains about stale entries.
2. Put the line `<class> <path>` in `cmake/source_manifest.txt`, with `# why` for anything that is not `shared`.
3. For `shared`, build it: configure as below and `cmake --build build/cmake-stage1`. The scanner may suggest `shared` for a file that still does not compile on Linux (for example `runtime/os/os_sync.cpp`, see below). The compile decides.
4. Files that `host/build_snap.py` compiles for Windows must stay `shared` or `windows`. The test enforces it.

## CMake

`CMakeLists.txt` + `cmake/sources.cmake`. There is no glob of sources anywhere: the manifest is the only list.

- `snap_load_source_manifest()` reads the manifest, rejects unknown classes, duplicates and entries whose file is missing, and fails the configure step when a `.cpp`/`.c` under `host/` or `runtime/` is not in it. (A glob is used there only to detect unlisted files, never to select sources.)
- `snap_select_sources(windows|linux out)` returns `shared` + that platform's own list. Linux never receives a `windows` or `offline` file.
- On Linux the project builds `snap_portable_objects`: every `shared` and `linux` source, with the settings of `host/build_snap.py` (`-march=x86-64-v3 -fno-strict-aliasing -w`, C++20). Warnings are off like in the Windows build; turning them on is future work.
- clang is required (the runtime uses clang builtins). Configure inside the `snap-recomp-linux` distrobox:

```
cmake -S . -B build/cmake-stage1 -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/cmake-stage1 -j
ctest --test-dir build/cmake-stage1 --output-on-failure
```

The Windows list is exposed (`SNAP_SOURCES_WINDOWS`) for tooling; the Windows game is still built only by `host/build_snap.py`.

## Tests (stage 1)

| ctest name | What it checks | Needs |
|---|---|---|
| `source_classification` | `tests/recomp/test_source_classification.py`: every tracked source is classified, no stale entries, no `shared` file with Windows-only content, everything `host/build_snap.py` compiles is `shared` or `windows`; plus the rules and the scanner on synthetic data | Python, git |
| `source_selection` | `cmake/tests/check_selection.cmake`: lists and per-platform selection on a fixture, the Linux selection never holds a `windows`/`offline` file, the loader rejects unknown classes, duplicates, missing files and unclassified files | CMake |
| `portable_sources_compile` | Builds `snap_portable_objects` (the 26 shared sources) | clang, Vulkan headers |
| `json_lite` | `tests/platform/json_lite_test.cpp`: `hle::Json` parse/query/build/copy/move (also built by hand with `-std=c++17` and `-std=c++20`) | clang |

Results of the run on this branch are in the commit message and the stage report.

## Changes to shared code

Two small, behaviour-neutral edits, both needed to make real shared files compile with libstdc++/clang on Linux:

- `runtime/hle/json_lite.h`: `hle::Json` keeps its members in `std::vector<Member>` with a named `Member {first, second}` instead of `std::vector<std::pair<std::string, Json>>`, because libstdc++ rejects a `vector<pair<..., Json>>` member while `Json` is incomplete. The `.first`/`.second` names, the API and the insertion order are unchanged; the file is built as C++17 by `host/` sources and as C++20 by `runtime/` sources, and the test covers both standards. Impact on Windows: none expected (MSVC's library accepted the old shape and accepts this one) but **not verified on Windows**.
- `runtime/simd.h`: the two `__declspec(selectany)` globals use a `SIMD_SELECTANY` macro, which is exactly `__declspec(selectany)` when `_WIN32` is defined and `__attribute__((weak))` elsewhere. Windows still gets the same text it had.

Line endings: both files are LF, and the edits keep that. New files are LF.

## Stage 2: neutral OS interfaces

Stage 2 adds no Linux implementation. It removes one compile error from the shared OS interface, pins what the interface promises with tests, and writes down what stages 3 and 4 have to provide. Sections marked **Contract** describe code that does not exist yet.

### The OS and platform layers today (facts, read from the tree at `555a53e`)

There is no `runtime/platform/` directory and no platform abstraction in the upstream code: the runtime calls Win32 directly. Stage 2 did not introduce one. Where a later stage needs a seam it should follow the shape already used in the upstream tree (a shared header with the contract, one implementation file per platform, the shared `.cpp` calling only the contract).

| Part | Responsibility | Windows dependencies | Shared interface | Status for Linux |
|---|---|---|---|---|
| `runtime/os/os.h`, `os_layout.h`, `perf.h` | Public C interface of the nn::os layer (threads, events, SVCs, override table); guest object layouts | none | consumed by the loader, HLE, tests | portable (scanner clean) |
| `runtime/os/os_internal.h` | Scheduler types (`Thread`, `Fiber`, `HostCtx`), result codes, keyed wait queues | none (comments mention the TIB and `VirtualAlloc`) | internal to `runtime/os` | portable; **`R_OK` fixed in stage 2** |
| `os_sync.cpp`, `os_overrides.cpp`, `cheat_heap.cpp`, `abi.cpp` | Guest mutex/condvar/event/semaphore emulation on the scheduler lock, override table lookup, heap base for cheats | none | `osk::` | **shared**, compile on Linux |
| `os_kernel.cpp` | Host thread start (`_beginthreadex`, `CREATE_SUSPENDED`, reserved stack), `SetThreadPriority`, `SetThreadDescription`, `SwitchToThread`, `QueryPerformanceCounter`, `timeBeginPeriod`, `Sleep`, TLR/Cpu allocation (`_aligned_malloc`) | yes, about 20 API names | `os_init`, `os_adopt_main_thread`, `call_guest` | `windows`, stage 3 |
| `os_memory.cpp` | Guest virtual memory: `VirtualAlloc` reserve/commit/decommit/release, `VirtualQuery`, `GetWriteWatch` flag, `VirtualAlloc2` aligned reserve, `GetSystemInfo` | yes (about 19 names) | `mem_get_info`, svc memory calls | `windows`, stage 3 |
| `os_thread.cpp` | Threads, TLS slots, fibers (`do_switch`, `fiber_proc`), time; calls `ctx_*` and `snap_ctx_switch` | `SwitchToThread`, `Sleep`, `ExitProcess`, `GetCurrentProcess`, `GetLastError` | `HostCtx` contract below | `windows`, stage 3 |
| `os_ctx_win64.cpp` (was `os_ctx.cpp`) | Host context switch (Win64 assembly, TIB, `VirtualAlloc` stacks) | yes | `ctx_new_thread`, `ctx_new_fiber`, `ctx_free`, `snap_ctx_switch` | `windows`; Linux sibling `runtime/os/linux/os_ctx_sysv.cpp` (stage 3 part A, done) |
| `os_state.cpp` | Save states (drain, copy, restore) | `dbghelp`, `VirtualQuery`, `VirtualAlloc` | `state_*` hooks called by the kernel | `windows`; not part of the Linux first version (see below) |
| `perf.cpp`, `hostprof.cpp`, `sampler.cpp` | Profilers | `OpenThread`, `GetThreadContext`, toolhelp, `psapi` | `perf.h` | `windows`, later |
| `runtime/hle/*` | Services, input, settings, save files | `CreateFileW`, known folders, DLL loading; `guest_tls.h`, `access.cpp`, `btn_icons.cpp`, `freecam.cpp`, `zip_read.cpp`, `services_net.cpp` are already shared | `hle_config.h`, `settings.h` | 5 shared, 14 `windows` (stage 5 and later) |
| `runtime/audio/*` | Audio engine and the WASAPI backend | `audio_wasapi.cpp` (COM), `audio_out.h` exports with `__declspec` | `audio_out.h` | `windows` |
| `runtime/ui/*` | ImGui overlay, options, pickers | `HWND`, shell, `imgui_impl_win32`; 6 files are shared | `ui_present.h` | mostly `windows` |
| `host/*` | Loader, boot, hooks, mods | everything under `windows` | `loader.h` (`call_guest`, `init_guest_tls`) | stages 4 and 5 |

What Linux needs from this layer, in the order of the plan: an OS-layer implementation of the five Win32 services above (stage 3), the TLS contract (stage 4) and a runner (stage 5). What stays out of the first Linux version: save states (`os_state.cpp`; the kernel calls `state_gate_wait`, `state_gate_acq`, `state_kick_cores`, `state_time_get` and `state_init`, so a Linux build needs either the real port or an explicit decision about those five calls in stage 3; **a success-returning stub is not acceptable**), profilers and the HLE/UI parts that depend on the shell.

### Change: `osk::R_OK` is now `osk::R_SUCCESS`

Cause (checked by compiling): `runtime/os/os_internal.h` declared `constexpr uint32_t R_OK = 0, ...` in `namespace osk`. `<unistd.h>` defines `R_OK` as the macro `4` (the `access()` flag); here it arrives through `os_internal.h`'s own first include: `<atomic>` -> `bits/atomic_base.h` -> `bits/atomic_wait.h` -> `<unistd.h>` (libstdc++ 15, clang 21 `-H`), and `-E -dD` shows `#define R_OK 4` before the declaration (preprocessed line 50458 against 165778). The declaration became `constexpr uint32_t 4 = 0` and `clang` stopped with `expected unqualified-id` in `os_sync.cpp`, `os_overrides.cpp` and `cheat_heap.cpp`.

What `R_OK` was: the Horizon `ResultSuccess` value (0) that OS functions return in `x0` (`c->x[0] = R_OK`) and that the wait helpers return when woken (`cv_wait` returns `R_OK` or `R_TIMEOUT`). It is an internal constant, not a status of the host.

Decision: rename the identifier, keep the value. Alternatives rejected: `#undef R_OK` (silently changes `access()` users that come after the header), a `#define` of our own (would break the POSIX headers), an `osk::` qualifier (does not help: the macro is expanded before name lookup). Nothing else collides on Linux: `os_posix_header` includes the POSIX headers first and compiles with all the other names (`R_TIMEOUT`, `R_INVALID_*`, the `Wait` and `KeyKind` enumerators, `NS_INFINITE`). Not checked: the Win32 headers (no Windows toolchain here); `R_SUCCESS` is not a Win32 name that is known to this author, and `R_OK` is not defined by the MSVC runtime headers, which is why the old name compiled there.

Scope of the edit: 35 lines in `os_internal.h`, `os_kernel.cpp`, `os_memory.cpp`, `os_sync.cpp`; no other file in the repository uses the name (searched `*.cpp *.h *.c *.py *.md *.txt *.bat *.ps1`). `os_sync.cpp` keeps its CRLF line endings. Windows: the rename is a pure token change inside `osk`; on Windows the identifier did not collide before and does not now.

Covered by tests: `os_posix_header` (compiles `os_internal.h` after the POSIX headers and pins `R_SUCCESS == 0` and every other result value), `os_neutral_interfaces` (no identifier in `runtime/os` spelled like `R_OK/W_OK/X_OK/F_OK`; the interface headers scan clean), `portable_sources_compile` (the three files now build).

### Contract: `os_ctx` (host context switch), for stage 3

Where it is used (checked): `os_thread.cpp` `do_switch` calls `snap_ctx_switch(from->host, to->host)`; `ensure_own` creates the context of a thread's own stack with `ctx_new_thread()`, `do_switch` creates a fiber's with `ctx_new_fiber(g_cfg.fiber_host_stack, fiber_proc, to)`; `ctx_free` runs at `InitializeFiber` of a re-used fiber, `FinalizeFiber` and thread exit; `os_state.cpp` reads `HostCtx::serial` and `sp`.

Interface that must stay as it is (also asserted by `os_posix_header`): `struct HostCtx { void *sp; void *base; void *limit; void *dealloc; void *alloc; size_t reserve; uint64_t serial; }` with `sp` at offset 0, `HostCtx *ctx_new_thread()`, `HostCtx *ctx_new_fiber(size_t reserve, void (*entry)(void *), void *arg)`, `void ctx_free(HostCtx *)`, `extern "C" void snap_ctx_switch(HostCtx *from, HostCtx *to)`. The Win64 switch reads `base`, `limit`, `dealloc` as the TIB stack fields at +8/+16/+24; a SysV implementation has no TIB, so those three fields are free for its own use (for example the mapping base and size). Changing the layout is only justified if stage 3 shows it is needed; nothing requires it today.

Win64 implementation, for reference (`os_ctx.cpp`): pushes `rbp rbx rdi rsi r12-r15`, reserves 176 bytes for `xmm6-xmm15`, `MXCSR` (+160) and the x87 control word (+164), swaps `gs:8`, `gs:16`, `gs:0x1478`, stores `rsp` in `from->sp`, loads `to->sp` and restores in reverse order. A new fiber gets a 248-byte frame: `MXCSR = 0x1F80`, x87 CW `0x037F`, `r13 = entry`, `r12 = arg`, return address `snap_ctx_tramp`, which moves `r12` into the first argument register and calls `r13`; `int3` follows (entry must not return). The stack is `VirtualAlloc`'d: `reserve` bytes reserved, 256 KiB committed at the top, one `PAGE_GUARD` page below it so Windows grows the stack on demand.

What the SysV implementation has to preserve (derived from the System V AMD64 ABI and from the callers; the assembly itself is not written):

- Callee-saved registers: `rbx`, `rbp`, `r12-r15`, and `rsp`. `rdi` and `rsi` are caller-saved in SysV (they are callee-saved on Win64) and `xmm6-xmm15` are caller-saved (callee-saved on Win64), so they need not be saved; do not copy the Win64 frame size.
- Control state: the `MXCSR` control bits and the x87 control word are callee-saved in SysV. Each context keeps its own, because the runtime mirrors the guest FPCR into `MXCSR` (`rt_fpcr_sync`: FTZ/DAZ, round to nearest) and the comment in `cpu.h` says fibers keep their own `MXCSR`. Initial values for a new fiber: `0x1F80` and `0x037F`.
- Arguments: first argument in `rdi` (Win64: `rcx`), second in `rsi` (`rdx`); `snap_ctx_tramp` must pass `arg` in `rdi`.
- Stack alignment: `rsp` is 16-byte aligned immediately before the `call` that enters `entry`, so that `entry` sees `rsp % 16 == 8` at its first instruction; the existing frame is built so that the trampoline starts on a 16-aligned address.
- Resume on another host thread is a requirement (`do_switch` says "resumed, possibly on another host thread"). A context must not hold anything bound to the thread it first ran on. The thread pointer `fs` is per host thread and must **not** be swapped. Consequence for the C++ code around the switch: a `thread_local` address computed before `snap_ctx_switch` may be reused by the compiler after it; the code already reloads it (`self_fresh()` is `noinline` with a compiler barrier for this reason). Any new `thread_local` access that spans a switch has the same hazard.
- Stack memory: `ctx_new_fiber` must provide a usable, 16-aligned stack of `reserve` bytes (default 64 MiB per fiber, 256 MiB per guest thread in `OsConfig`), committed lazily, with an inaccessible guard page below it. Hypothesis, not checked: `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)` plus `mprotect(PROT_NONE)` on the lowest page matches `VirtualAlloc` reserve/commit/guard closely enough; overcommit settings of the host decide whether 64 MiB x many fibers is acceptable.
- Not covered by `snap_ctx_switch`: the guest registers are in `Cpu`, not on the host stack, so nothing of the guest's state is saved by this function.
- Hypothesis, not checked: Linux user-space shadow stacks (CET) are opt-in and not enabled for ordinary binaries, so a hand-written switch is not expected to hit them; this needs a check on the target kernel.
- Save states (`os_state.cpp`) depend on `serial` and on the saved `sp` being readable. They are not part of the first Linux version.

Tests required in stage 3 (not written yet): many fibers switched across several threads with checks of the callee-saved registers, `MXCSR`, the x87 control word and stack alignment; a fiber that is resumed on a different thread than the one it started on; `ctx_free` of a context that never ran.

### Contract: TLS and `.tdata`, for stage 4

Two different things are called TLS here.

1. **Host TLS**: C++ `thread_local` objects: `t_self` in `os_kernel.cpp` (the scheduler's `Thread`), `loader::g_tls_cpu` in `host/loader.cpp` (the `Cpu` of the last host call, used by crash reports). No change is needed for them on Linux except the fiber hazard above.
2. **Guest TLS**: the AArch64 thread pointer the game's code uses (`tpidr_el0`). The imported ELFs have no `PT_TLS` segment (comment in `host/loader.cpp`, `host/boot.cpp`), so the host builds the block.

Guest TLS layout per guest thread (checked in `host/loader.cpp::new_thread`, `host/boot.cpp::tp_init_hook`, `runtime/os/os_layout.h`, `runtime/os/os_kernel.cpp::host_entry`):

- A zeroed, read/write block of 0x10000 bytes; the thread pointer `tp` is the block address plus 0x8000.
- A 0x200-byte Thread Local Region (`TLR_SIZE`, aligned to its size) that `Cpu::tpidrro` points at: `TLR+0x1f0` (`TLR_TP`) holds `tp`, `TLR+0x1f8` (`TLR_THREAD`) holds the guest `ThreadType*`.
- `c->tpidr = tp`.
- `.tdata`: the game's `main` has an 8-byte initialised TLS image at `main + 0xccb900` (`hle::kMainTlsImageVaddr`, `kMainTlsImageSize`, valid for build 2.0.1, v262144); it belongs at `tp + 0x10` (`kTlsBlockOffset`: first module's block after the 16-byte TCB). `.tbss` is zero. `hle::guest_tls_init(tp, image, n)` in `runtime/hle/guest_tls.h` does the copy and only needs `<cstring>`. Why it matters: without the image the physics bounds slot is wrong and the camera scan never finds a Pokemon (comment in `guest_tls.h`).
- `loader::init_guest_tls(tp)` (`host/loader.cpp`) reads the image once, from the loaded `main` bytes, before any guest code runs (the guest later modifies its TLS), and traces a warning when the slot word is not `-1` (a build other than 2.0.1).

Where the block is created (two sites, both call `init_guest_tls`):

1. `host/loader.cpp::new_thread`: the adopted main thread. Allocates stack, TLS block, TLR, `ThreadType` with `VirtualAlloc`, then `init_guest_tls(c->tpidr)`.
2. Every thread created by the game: `host/boot.cpp::tp_init_hook` is installed as the guest function behind sdk's `nnmuslThreadPointerInitializer`. `os_kernel.cpp::host_entry` calls it through `OsConfig::tp_init_var` with `x0 = ThreadType`; the hook allocates the block, calls `init_guest_tls`, writes `TLR[0x1f0]`; `host_entry` then sets `c->tpidr = TLR[0x1f0]`. `tp_destroy_hook` frees `tp - 0x8000` when the thread ends (`tp_destroy_var`, after the TLS destructors).

What stage 4 has to provide, without changing these semantics:

- A zero-filled, read/write 0x10000-byte allocation per guest thread and its release at thread exit, from the OS layer's memory primitive (no `VirtualAlloc`/`VirtualFree`).
- `init_guest_tls` reachable from the Linux runner and from the thread-start hook, after `g_main.base` and `g_main.span` are known and before the first guest instruction of every thread.
- The order inside `host_entry` unchanged: `tp_init` hook, then `c->tpidr = TLR[0x1f0]`, then the entry.
- The build check (`-1` slot word) kept.
- Not to be changed: the offsets above, the 2.0.1 constants, `TLR` layout.

Test already existing upstream and to be reused in stage 4: the TLS tests in `tests/hle/hle_tests.cpp` (`guest_tls_init`). They are Windows-batch tests and were not run here.

### Memory and scheduler contracts noted for stage 3 (not implemented)

- `os_memory.cpp` needs a platform memory primitive with: reserve (no access), commit (read/write), decommit (back to no access), release, query of the region state, an aligned reserve (the Windows path uses `VirtualAlloc2` when present, otherwise reserve-release-reserve with up to 16 tries), and the write-watch flag that can only be set at reserve time. The write watch has a consumer outside the OS layer: `runtime/nvn/nvn_vk.cpp::SyncPoolRange` uses `GetWriteWatch` to copy only the pages the game wrote (`SNAP_SYNC_FULL=1` is the existing full-copy fallback). A Linux replacement for write watch (soft-dirty bits, `userfaultfd` write-protect or full copies) is a stage 3 decision, not made here. Guest addresses are host addresses, so reservations must land at the addresses the guest was told.
- `os_kernel.cpp` needs: a host thread with a reserved stack size (`STACK_SIZE_PARAM_IS_A_RESERVATION`, default 256 MiB), created suspended and resumed after the bookkeeping, a thread priority hint, a thread name, a yield, a monotonic counter and its frequency, a sleep, and a timer-resolution request. None of these needs a change in the scheduler design; the scheduler lock and wait queues (`os_sync.cpp`) are plain C++ and are already shared.
- The scheduler's behaviour is not changed by stage 2. Any difference introduced by Linux timers or priorities has to come with tests in stage 3.

### Tests of stage 2

| ctest name | What it checks | Needs |
|---|---|---|
| `os_posix_header` | `os_internal.h` compiled after the POSIX headers (macro collision regression); the Horizon result values; the `HostCtx` offsets | clang, no GPU |
| `os_neutral_interfaces` | `tests/platform/test_os_neutral.py`: the five interface headers scan clean of Windows content; no identifier in `runtime/os` spelled like a POSIX access macro; `R_SUCCESS` declared once with value 0 | Python |
| `source_classification`, `source_selection`, `json_lite`, `portable_sources_compile` | Stage 1 tests; the manifest now lists `cheat_heap.cpp`, `os_overrides.cpp` and `os_sync.cpp` as `shared` (29 shared files compiled) | as in stage 1 |

Not run, pending CI or a maintainer with a Windows toolchain: the Windows build (`host/build_snap.py`), `tests/os/os_test.cpp` and the other batch-built suites. The rename touches `os_kernel.cpp` and `os_memory.cpp`, which only the Windows build compiles, so their compile on Windows is **unverified** (the change is a token rename; both files fail to compile on Linux only because of `<windows.h>`, which was checked).

### Left for the next stages

- Stage 3: `os_ctx_sysv` implementation and tests, the Linux memory/thread/timer primitives for `os_memory.cpp`, `os_kernel.cpp`, `os_thread.cpp`, the decision for the `state_*` calls, and moving those files to `shared` or `linux` in the manifest.
- Stage 4: the TLS block allocation and the `.tdata` copy on Linux, as specified above.
- Stage 5: the Linux runner.
- Not designed yet: write-watch replacement, shadow-stack behaviour of the target kernel, overcommit limits for fiber stacks.

## Stage 3: System V context switch (part A) and OS layer integration (part B)

### Part A: done

**Files.** `runtime/os/os_ctx.cpp` was renamed to `runtime/os/os_ctx_win64.cpp` with `git mv` and no content change (`git diff -M` shows a 100 % rename, 0 insertions, 0 deletions); `tests/os/build.bat` names the new path (one token). The Linux implementation is `runtime/os/linux/os_ctx_sysv.cpp`. It is in a subdirectory on purpose: `host/build_snap.py` compiles `runtime/os/*.cpp` with a non-recursive glob, so a Linux-only file next to the others would have been compiled into the Windows game. `host/build_snap.py` is untouched and `scripts/source_classes.py` rebuilds the same non-recursive rule, so `source_classification` checks that no `linux` file is in the Windows build. The manifest classes: `os_ctx_win64.cpp` is `windows`, `os_ctx_sysv.cpp` is `linux` (compiled only by `snap_portable_objects`, which selects `shared` + `linux`). Neither is selected by a glob.

**Interface (unchanged).** `HostCtx` keeps its layout, `sp` at offset 0, and the four functions keep their signatures (`os_internal.h`). On Linux the three TIB fields are not TIB fields: `base` = top of the stack, `limit` = lowest usable address, `dealloc` = `alloc` = start of the mapping, `reserve` = its size (rounded up to a page). `os_posix_header` still pins the offsets.

**`snap_ctx_switch(from, to)`** (`rdi`, `rsi`): pushes `rbp rbx r12 r13 r14 r15`, reserves 16 bytes, stores `MXCSR` and the x87 control word there, stores `rsp` in `from->sp`, loads `to->sp`, reloads the control words, pops in reverse order and `ret`s. Nothing else is saved: `rdi`, `rsi` and `xmm0-15` are caller-saved in System V. The code never reads or writes `%fs` (or `%gs`).

**Frame** (lowest address first, 72 bytes; also the initial frame of a new fiber): `+0` MXCSR, `+4` x87 CW, `+8` unused (16-byte control area), `+16` r15, `+24` r14, `+32` r13, `+40` r12, `+48` rbx, `+56` rbp, `+64` return address. A new fiber has MXCSR `0x1F80`, CW `0x037F`, `r13 = entry`, `r12 = arg`, `rbp = 0`, return address `snap_ctx_tramp`.

**Stack convention.** `ctx_new_fiber` maps `reserve` bytes read/write with `MAP_NORESERVE | MAP_STACK` (lazily committed) and makes the lowest page `PROT_NONE`. The frame sits 64 bytes under the top: `sp = top - 136`, so `sp % 16 == 8` and `sp + 72` (the `rsp` after the `ret` into the trampoline) is 16-aligned. `snap_ctx_tramp` moves `r12` into `rdi` and `call`s `r13`, so the entry function sees `rsp % 16 == 8` at its first instruction, as the ABI requires; `ud2` follows the call (an entry that returns traps with SIGILL). A suspended context has no alignment requirement of its own: it resumes after its own `call snap_ctx_switch`. `.cfi_undefined rip` ends unwinding at the trampoline. Requests below four pages or impossible sizes return `nullptr`.

**Thread pointer and other threads.** `%fs` is per host thread and is not part of a context. A context resumed on another host thread therefore runs with that thread's `%fs`; the switch does not alter it. The hazard is in the C++ around a switch, not in the switch: a `thread_local` address computed before `snap_ctx_switch` may be reused after it by the compiler. The scheduler already reloads it (`self_fresh()` is `noinline` with a memory barrier, `do_switch` calls it after the switch). The tests reproduce this with a `noinline` reader of a `thread_local` and a fiber that hops between two threads. The guest TLS (`tpidr_el0`) is a different thing (stage 4) and nothing here simulates it with the host thread pointer.

**Tests** (`ctx_sysv_test`, all independent of GPU, SDL, Vulkan and the game; test-only assembly is in the test file, not in production code):

| Check | How |
|---|---|
| new contexts | `sp % 16 == 8`, `sp + 72` aligned, `base/limit/dealloc/alloc/reserve` consistent, initial MXCSR/CW in the frame, serials unique and increasing, both ends of the usable range writable, `ctx_free` of a context that never ran, `nullptr` for a too-small and for an impossible size |
| callee-saved registers | `snap_test_switch_regs` loads `rbx rbp r12-r15` with a per-context, per-round pattern, switches, and compares what it finds on resume; a ring of three contexts (main + two fibers), 2000 rounds each, with a 32-word stack canary and a local alignment check after each resume |
| MXCSR, x87 CW | three contexts with different values (round up / down / nearest, FTZ, DAZ, precision), 500 switches each; also in effect: `2.5` converted with SSE and with `fistp` rounds as the context's own mode says; a new fiber starts with the defaults although its creator had set other values |
| alignment | `rsp` recorded by assembly at the first instruction of the entry (`% 16 == 8`), an `alignas(16)` local and an aligned SSE load in the entry body |
| end of the function | ends by switching away for good (as `fiber_proc` does); an entry that returns traps with SIGILL (child process); the page below the usable range raises SIGSEGV (child process) |
| other host thread | one fiber resumed alternately by two host threads, 2000 hops: `%fs`, `gettid`, a `thread_local` value, callee-saved registers and a local alignment are checked after each hop; the switch leaves `%fs` unchanged |
| many cycles | 3 x 60000 switches with canaries and registers |

Results (Linux x86-64 container, kernel 7.3.3, clang 21): `ctx_sysv` and `ctx_sysv_repeat` (30 suites in one process) pass; 150 separate runs and 40 runs pinned to one CPU (`taskset -c 0`) had 0 failures; the suite also passes built with `clang -O0`, `clang -O3 -march=x86-64-v3`, `g++ -O0` and `g++ -O2` (10 repetitions each). Mutation check, to show that the tests can fail: removing the `ldmxcsr`, the `fldcw`, the restore of `r12`, `r13`, `r14`, `r15`, `rbx` or `rbp`, a frame misaligned by 8, a wrong default MXCSR or CW, no guard page, or no `rdi` setup in the trampoline each made the suite fail (13 variants, all detected; the register variants fail with a crash or a check, depending on which register). Not tested: AddressSanitizer (its fake stacks need annotations), other architectures, CET shadow stacks (hypothesis: user-space shadow stacks are opt-in and off for ordinary binaries), `MAP_NORESERVE` under `vm.overcommit_memory=2`.

### Part B: done (POSIX host primitives under the OS layer)

Part B moves the Win32 calls of the OS layer behind a small interface and gives Linux its own side of it, so that the real scheduler, threads, fibers and virtual memory run on Linux with the System V context switch of part A. Decisions taken (approved): a neutral header with one implementation per platform (not `#if` inside the OS files); save states off on Linux; full copies instead of write tracking.

**Files.**

| File | Class | What |
|---|---|---|
| `runtime/os/os_host.h` | shared | The interface (`osk::host`): time, threads, profiling, aligned heap, virtual memory. Only primitives with a caller |
| `runtime/os/os_host_win32.cpp` | windows | The Win32 calls `os_kernel.cpp`, `os_memory.cpp`, `os_thread.cpp` and `perf.cpp` made directly before, moved unchanged. `host/build_snap.py` already compiles `runtime/os/*.cpp`, so it is picked up there with no change to the script |
| `runtime/os/linux/os_host_posix.cpp` | linux | The POSIX side. In `runtime/os/linux/` so that the non-recursive Windows glob never compiles it |
| `runtime/os/linux/os_state_off.cpp` | linux | Save states off (below). Stands where `os_state.cpp` stands on Windows |
| `os_kernel.cpp`, `os_memory.cpp`, `os_thread.cpp`, `perf.cpp` | shared (were `windows`) | Consume `os_host.h`; no Win32 name is left in them (the scanner checks) |

`tests/os/build.bat` and `tests/hle/run_tests.ps1` name `os_host_win32.cpp` now (they link `os_kernel`/`perf` and would not link without it). `tests/os/os_test.cpp` has a four-line `_aligned_malloc` shim for non-Windows hosts.

**What each primitive does and where Windows and POSIX differ.**

| Primitive | Win32 (unchanged) | POSIX |
|---|---|---|
| `counter`, `counter_frequency` | `QueryPerformanceCounter` / `Frequency` | `CLOCK_MONOTONIC`, 1e9 |
| `sleep_precise_ns` | per-thread high-resolution waitable timer, `Sleep` fallback | `clock_nanosleep` on `CLOCK_MONOTONIC`, restarted from the remaining time after `EINTR` |
| `sleep_ms`, `timers_init` | `Sleep`; `timeBeginPeriod(1)` | `clock_nanosleep`; nothing to request |
| `thread_create_suspended` / `resume` / `join` | `_beginthreadex(..., STACK_SIZE_PARAM_IS_A_RESERVATION \| CREATE_SUSPENDED)`, `ResumeThread`, `WaitForSingleObject` + `CloseHandle` | `pthread_create` with the stack size rounded to a page; the thread waits on a mutex/condition variable until `thread_resume`; `pthread_join` and the handle freed. A thread that ends at once is still joined |
| `set_current_thread_name` | `SetThreadDescription` (about 38 characters) | `pthread_setname_np`, cut to 15 characters (kernel limit) |
| `yield` | `SwitchToThread` | `sched_yield` (best effort on both) |
| `terminate_process` | `TerminateProcess` | `_exit(0)`: no `atexit`, other threads stopped |
| `current_thread_cycles` | `QueryThreadCycleTime` (cycles) | `CLOCK_THREAD_CPUTIME_ID` (ns): the profiler column is a relative load, the unit differs |
| `for_each_thread`, `thread_description` | Toolhelp snapshot + `GetThreadTimes`; `GetThreadDescription` | `/proc/self/task/<tid>/schedstat` (ns) or `stat` (ticks); `/proc/self/task/<tid>/comm` |
| `heap_aligned_alloc` / `free` | `_aligned_malloc` / `_aligned_free` | `posix_memalign` / `free` |
| virtual memory | `VirtualAlloc` / `VirtualFree` / `VirtualQuery`, `VirtualAlloc2` for aligned reserves | `mmap(PROT_NONE, MAP_NORESERVE)` + `mprotect` + `mmap(MAP_FIXED)` over the range; the state Windows keeps is kept by the host (below) |

**Virtual memory is not `VirtualAlloc` with other names.** Windows knows the state of every page and refuses what does not fit; Linux does not. Three differences were handled on purpose, each with tests:

1. *Commit state.* A reservation is `PROT_NONE`; commit is `mprotect(RW)` on it (zero-filled, lazily backed); decommit maps a fresh anonymous `PROT_NONE` range over it (contents dropped, later access faults, committing again gives zeros). `mem_count` needs to know how many bytes are committed (the VAMM counts only fresh pages), so the host keeps the set of committed ranges.
2. *Reservations.* The host also keeps the reservations. A commit outside one fails with a non-transient error (Windows: `ERROR_INVALID_ADDRESS`; plain `mprotect` would say `ENOMEM`, which the VAMM reads as memory pressure and would retry for 5 s). A decommit or release of memory that is not a reservation is ignored, as `VirtualFree` fails there: `mmap(MAP_FIXED)` would otherwise replace unrelated memory. A release needs the base of a reservation and frees all of it.
3. *Granularity.* Reservations are at least 64 KiB aligned (the Windows allocation granularity the guest sizes were tuned for); commit and decommit round outwards to 4 KiB pages like `VirtualAlloc`.

Transient errors (retried by the VAMM with a back-off): `ENOMEM` and `EAGAIN` on POSIX, the commit-limit family on Windows. The fault injection `SNAP_FAULT_VAMM` uses `mem_injected_error()`.

**Scheduler, threads, fibers.** `os_kernel.cpp` creates host threads with `thread_create_suspended(host_stack, host_entry, t)`, stores the handle under the scheduler lock, then resumes (the same order as before: a `DestroyThread` racing the start still joins). `os_thread.cpp` switches fibers with `snap_ctx_switch` and `ctx_new_fiber` from `runtime/os/linux/os_ctx_sysv.cpp` (part A) and re-reads the thread-local `Thread*` with `self_fresh()` after every switch (a fiber may resume on another host thread). No scheduler logic was changed or simplified. `os_test.cpp` was written for exactly this and runs unchanged on the Linux sources: threads, priorities and priority inheritance, cores and migration, mutexes, condition variables, events and multi-wait, TLS slots, fibers (4008 switches), the guest clock, the VAMM and the memory heap.

**Save states are off on Linux.** `runtime/os/linux/os_state_off.cpp` defines what `os_state.cpp` defines on Windows, with a different meaning:

- `g_sphase` is defined and nothing ever sets it, so it is 0 for the whole run. `os_kernel.cpp` calls the gates only when it is not 0.
- `state_gate_wait` and `state_gate_acq` are tripwires: if one is ever reached the process stops with `state_gate_... reached on Linux` (`fatal`), it does not carry on as if a drain had happened.
- `state_init` starts no thread. `SNAP_CHECKPOINT` and `SNAP_STATE_TEST` are logged as ignored.
- `snap_state_request` refuses and logs (`save states are not supported on Linux`) and records the failed operation; `snap_state_info` says there is no state and nothing is busy; `snap_state_idle` is 1; `snap_state_count` is 0. No success is ever reported.
- Windows is not affected: `os_state.cpp` is unchanged. The rest of the OS layer keeps its `state_*` hooks (`state_time_get`, `state_kick_cores`, the VAMM limbo, the fiber table); they are plain code that nothing calls without a save request.

**Write watch and `SNAP_SYNC_FULL=1`.** `host::write_watch_supported()` is false on Linux. Guest reservations then never ask for write tracking, whatever the environment says; on Windows `SNAP_SYNC_FULL=1` or `SNAP_WRITE_WATCH=0` already dropped the flag. The decision is the pure function `host::write_watch_wanted(supported, SNAP_SYNC_FULL, SNAP_WRITE_WATCH)`, tested over its whole table, and `snap_os_write_watch_active()` (new `extern "C"`) reports the result. **The consumer is not changed here:** `nvn_vk.cpp` (`SyncPoolRange`) still decides from the two environment variables alone and is a `windows` file; the Linux NVN stage must also ask `snap_os_write_watch_active()` (or `write_watch_supported()`), otherwise it would use write tracking on memory that has none. The contract of `SyncPoolRange` (copy the whole range when nothing is tracked) is the existing one and is not touched. No soft-dirty and no `userfaultfd`: that evaluation stays for stage 7, with measurements under real NVN load.

**Reference: the five `state_*` functions (read from the tree before part B).**

| Function | Defined in | Called from, when | What it is |
|---|---|---|---|
| `state_gate_wait(t, lk)` | `os_state.cpp:932` | `os_kernel.cpp:374`, top of the keyed-wait loop, only when `g_sphase != 0`; its own first line returns when `g_sphase == 0` | Parks a thread that waits without a core while a save state drains or captures |
| `state_gate_acq(t, lk)` | `os_state.cpp:937` | `os_kernel.cpp:523`, top of the core-wait loop, only when `g_sphase != 0`; returns `false` when `g_sphase == 0` | Grants no core to a waiter during a drain (sets `Thread::sgated`, which `best_waiter_for` skips) |
| `state_kick_cores()` | `os_kernel.cpp:469` | `state_gate_acq` | One line: `kick_free_cores()`. It is scheduler code that lives in the kernel and is ported with it, not a save-state mechanism |
| `state_time_get()` / `state_time_set()` | `os_kernel.cpp:1199/1206` | only `os_state.cpp` (capture and restore, lines 603 and 766) | The guest clock (QPC minus paused time) in the kernel file; ported with the kernel's clock |
| `state_init()` | `os_state.cpp:952`, called at the end of `os_init` (`os_kernel.cpp:1264`) | once | Records the process start tick and starts optional driver threads only if `SNAP_CHECKPOINT` or `SNAP_STATE_TEST` is set |

`g_sphase` (`os_state.cpp:51`) is written only inside `os_state.cpp` (lines 150, 205, 554, 563, 606, 708, 716) and nothing can set it without a save-state request (`snap_state_request`, from `ui_savestate.cpp` or the route/hotkey code). Invariant: while it is 0 both gates are inert, so with no way to request a save state the scheduler behaves exactly as on Windows in the idle phase. That is the only behaviour a first Linux version would have to preserve; the gates cannot be separated from the capture machinery without porting it, because `gate_block` and the unclean-grant counters live there. Consequence: any Linux definition of `g_sphase`, `state_gate_wait`, `state_gate_acq` and `state_init` would be a deliberate "save states disabled" configuration whose gates are unreachable, and `snap_state_request` and friends (called by the shared `ui_savestate.cpp`) would have to say that save states are unsupported instead of returning success. That is a policy decision, recorded as decision 2.

**Reference: write watch (facts gathered before part B).** The consumer is `nvn_vk.cpp` (`SyncPoolRange`, lines 847-952): `GetWriteWatch(RESET, storage, size, ...)` per range, or one snapshot without reset per replay epoch. It already has a correct fallback: on failure it sets `ww = -1` and copies whole ranges; `SNAP_SYNC_FULL=1` or `SNAP_WRITE_WATCH=0` reserve without the flag and force that path. So full copies are semantically safe and only slower. Contract needed from a Linux mechanism: report the 4 KiB pages written in a range since the last query, with an optional reset of that range only, covering writes made by the kernel too (`fs_core.cpp:95` marks pages it filled from user mode, because on Windows kernel writes are not seen; a superset is harmless), and registration at reserve time.

Facts measured on this machine (kernel 7.3.3, inside the container, unprivileged; one probe program, not a general proof):

| Option | Works here | Per-range reset | Sees kernel writes | Privilege / availability | Cost and complexity |
|---|---|---|---|---|---|
| Full copies (existing fallback) | yes | n/a | n/a | none | no tracking; copies every range every replay; slowest, simplest, already exercised by the NVN layer |
| Soft-dirty (`/proc/self/clear_refs`, pagemap bit 55) | yes (probe: only the written page reported) | **no**: clearing is per process, not per range | yes | needs `CONFIG_MEM_SOFT_DIRTY` | a pagemap read per query; a global reset would clear the state of every range and of any other user (checkpoint tools); cannot reproduce `GetWriteWatch(RESET)` per range |
| `userfaultfd` write-protect, async (`UFFD_FEATURE_WP_ASYNC`, `UFFD_USER_MODE_ONLY`) + `PAGEMAP_SCAN` | yes (2 written pages reported at the right offsets; a kernel `read()` into a protected page succeeded, no `EFAULT`, and the page was reported) | **yes** (`PM_SCAN_WP_MATCHING` re-protects only the scanned range; the reset call itself was not exercised by the probe) | yes | kernel 6.7 or newer; unprivileged here (`vm.unprivileged_userfaultfd = 0`, `USER_MODE_ONLY` accepted); a seccomp profile may forbid it elsewhere | one ioctl per query, no fault-handler thread; the mapping must be registered at reserve time; the closest match to the Windows API. Not tested: behaviour with the NVN workload, `mprotect`/`munmap` of registered ranges, fork |

Part B chose full copies (above). Both kernel-assisted options need the NVN layer (stage 7) to exercise them, so only the contract and the facts are recorded.

### Tests of stage 3

| ctest name | What it checks | Needs |
|---|---|---|
| `ctx_sysv`, `ctx_sysv_repeat` | Part A: the System V context switch alone (registers, MXCSR, x87, alignment, other thread, stack guard) | clang |
| `os_host`, `os_host_repeat` | `tests/platform/os_host_test.cpp`: the POSIX primitives alone. Real accesses with `SIGSEGV` caught or observed in a child: a thread does not run before `thread_resume`, runs after it, is joined; a stack overflow faults at the reservation; names cut to 15 characters; the monotonic clock never goes back; sleeps last at least what was asked; `terminate_process` runs no `atexit` and ends with other threads running; reserved memory has no access, committed memory is read/write and zero-filled, decommitted memory faults and comes back zeroed; alignment of reservations; commit of overlapping and unaligned ranges; commit outside a reservation fails and is not transient; decommit and release of memory that is not a reservation leave it alone; a release frees the whole reservation; a 3000-step random commit/decommit sequence checked against a model and against real protections; 8 threads on one reservation; the policy table of `write_watch_wanted`; CPU time of another thread (a thread that burns 120 ms of CPU shows at least 80 ms, an idle one less than 40 ms) | clang |
| `os_state_off` | `tests/platform/os_state_off_test.cpp`, linked with the real OS layer: `os_init` with `SNAP_CHECKPOINT` and `SNAP_STATE_TEST` set starts no thread (thread count unchanged, also after 1.5 s) and logs the refusal; `g_sphase` stays 0; the `snap_state_*` entry points refuse and report no state, nothing busy, no counter; a save and a load request change nothing but the recorded failure; both gates abort the process with their message (children) | clang |
| `os_scheduler` | `tests/os/os_test.cpp` unchanged, on `os_kernel`, `os_thread`, `os_sync`, `os_memory`, `os_overrides`, `perf`, `os_ctx_sysv`, `os_host_posix`, `os_state_off`: 8038 checks (scheduler, threads, fibers, sync objects, VAMM, heap, clock) | clang |
| `os_scheduler_vamm_fault` | The same program with `SNAP_FAULT_VAMM=0:7 OS_TEST_ONLY=vamm`: a VAMM retry back-off (127 ms) must not hold the virtual core, so a thread pinned to the same core runs during it | clang |
| `os_scheduler_sync_full` | The same program with `SNAP_SYNC_FULL=1` | clang |
| `os_posix_header`, `os_neutral_interfaces` | The OS interface headers (now including `os_host.h`) scan clean and do not collide with POSIX macros | clang, Python |
| `source_classification`, `source_selection`, `json_lite`, `portable_sources_compile` | Stage 1 and 2 tests; the manifest has 4 more `shared` and 3 more `linux`/`windows` entries (see `cmake/source_manifest.txt`) | as before |

**Do the tests detect a wrong implementation?** Each of these was applied to a copy of the sources and the relevant test run (all were detected, each by the check named): decommit that does not drop the contents (`zero`); decommit and release without the reservation check (memory outside the reservation was overwritten or unmapped); commit without the reservation check; commit errors always transient; a thread that starts running before `thread_resume`; a sleep of half the time; alignment below 64 KiB; a `mem_count` that ignores holes; a stack not sized to the request; a `thread_join` that only detaches; `sleep_ms` that does not sleep (`os_scheduler_vamm_fault` fails: the retry ends before the second thread starts, so nothing runs during the back-off); a monotonic clock that jumps back (`os_test` "ticks went backwards"); a gate that returns instead of aborting; a `state_init` that starts a thread; a save request that reports success. Not detected, by design: `yield` that does nothing (a best-effort hint on both platforms; no test can tell it from a scheduler that had nothing else to run).

Run on this machine (Linux x86-64 container, kernel 7.3.3, clang 21): the full ctest, 14 of 14 pass; `os_test` 10 more times in a row, 10 of 10 with 8038 checks and 0 failed.

Not run: the Windows build and every Windows-side test (`tests/os/build.bat`, `tests/hle/run_tests.ps1`, the game). The Windows files were changed (below) and have only been read, not compiled.

### Windows side: what was inspected and what could not be validated

Read, against the code each function replaced (git `HEAD` of `os_kernel.cpp`, `os_memory.cpp`, `os_thread.cpp`, `perf.cpp`): the arguments, the order of calls, the error handling and the thread-start sequence of every Win32 call that moved into `os_host_win32.cpp` (`_beginthreadex` flags and stack clamp, the high-resolution timer loop, `timeBeginPeriod`, `SetThreadDescription` and its 39-character name, Toolhelp sampling, `VirtualAlloc2` with the 16-try fallback, the commit-limit error set, `MEM_DECOMMIT` of the whole region, `VirtualFree(MEM_RELEASE)` with size 0, `VirtualQuery` page counting). `os_svc` case 20 now reserves 4 KiB at 64 KiB alignment and commits it, where it used to reserve and commit in one `VirtualAlloc`; the result is the same one zeroed read/write page for the process lifetime. The shared files lost `<windows.h>`; the headers of the C and C++ functions they use are included explicitly (`<cstdio>` was added to `os_thread.cpp` for `fflush`).

**Not validated, pending CI or a maintainer with Windows:** that the five touched files and `os_host_win32.cpp` compile and link with the Windows toolchain; that `tests/os/build.bat` and `tests/hle/run_tests.ps1` (which now name `os_host_win32.cpp`) still build; `os_test.exe` on Windows; the game. `host/build_snap.py` was not edited.

## Stage 4a: shared guest TLS module (`runtime/hle/guest_tls.h`)

Part A of stage 4 only: a platform-independent module and its unit tests. The loader (`host/loader.cpp`), the hooks in `host/boot.cpp`, the OS layer and the scheduler are **not touched** and do not use it yet (that is stage 4b). No production source file was added: the module is header-only (`runtime/hle/guest_tls.h` already existed upstream and is included by the Windows loader), so the manifest did not change and `host/build_snap.py` compiles exactly what it compiled.

**Layout, checked.**

| Fact | Evidence |
|---|---|
| Block of `0x10000` bytes, `tp = block + 0x8000` | `host/loader.cpp:412` and `:424`, `host/boot.cpp:672` and `:673` |
| The imported ELFs have no `PT_TLS` | the program headers of a local `main.elf` (7 headers, none of type `PT_TLS`); the test checks this when `SNAP_MAIN_ELF` is set |
| 8-byte image at `main + 0xccb900` (build 2.0.1); its values are deliberately not recorded here | the same file: the 8 bytes follow 8 bytes of zero padding in a 16-byte `.data` section at `0xccb8f8`; read from the `PT_LOAD` that contains it |
| Image goes to `tp + 0x10` (after the 16-byte TCB) | `hle::kTlsBlockOffset`, `guest_tls_init` |
| The `TLR+0x1f0` (tp) and `TLR+0x1f8` (ThreadType) offsets are relative to the **Thread Local Region** (`c->tpidrro`), not to `tp` | `runtime/os/os_layout.h` (`TLR_TP`, `TLR_THREAD`), `os_kernel.cpp::host_entry` |

**Not known, and not assumed by the module:** how far `main`'s `.tbss` reaches (the "up to `tp + 0x670`" in the old comment is not a measurement), the largest alignment its TLS asks for, and whether the half of the block below `tp` is used. The module therefore zeroes **every** byte of the block except the image, which is what the old zero-filled allocation gave and is a superset of any `.tbss`. It requires the block address to be a multiple of 16: a conservative floor chosen here (the TCB is 16 bytes), not a proven maximum. Real blocks will be 64 KiB aligned.

**API** (namespace `hle`, no OS call, no global state, no allocation of guest memory):

| Name | Does |
|---|---|
| `GuestTlsImage` | The bytes of `main`'s `.tdata`, copied once; read-only afterwards (`data()` is `const`). Copying it copies the bytes. Changing the source later, or any block made from it, never changes it |
| `GuestTlsImage::capture(base, span, module_offset, size, &out)` | Reads the image out of a loaded module; checks it lies inside `span` without overflow. `out` is left alone on failure |
| `GuestTlsImage::capture_bytes(bytes, size, &out)` | The same from bytes in hand |
| `guest_tls_init_block(block, block_size, image, &tp, tp_offset = 0x8000)` | Zeroes the whole block, copies the image to `tp + 0x10`, returns `tp`. Needs `block_size >= tp_offset + 0x10 + image.size()`, a 16-byte aligned address and no overlap with the image. On any error nothing is written |
| `GuestTlsStatus`, `guest_tls_status_name` | `Ok`, `NullPointer`, `EmptyImage`, `OutOfRange`, `BlockTooSmall`, `BadTpOffset`, `Misaligned`, `Overlap` |
| `kGuestTlsBlockSize`, `kGuestTlsTpOffset`, `kGuestTlsMinBlockAlign` | The numbers above, pinned by `static_assert`s in the test |

The block is allocated and released by the caller. The plan for stage 4b is the OS layer's `mem_reserve` + `mem_commit` / `mem_release`; the module does not do it and the tests do not use it. The old `guest_tls_init(tp, image, n)` is unchanged (the Windows loader and `hle_tests.cpp` use it); a test checks that both functions write the same bytes. Which of the two the loaders end up calling is a stage 4b decision.

**Capture timing is not decided here.** The upstream loader captures lazily, on the first thread, before any guest code runs, in a function-local static. `GuestTlsImage` is a plain value, so *when* and *where* the one capture happens (and who guards it) is left to the caller in stage 4b.

**Tests** (ctest `guest_tls`, and `guest_tls_sanitized` when the compiler supports AddressSanitizer + UBSan; `tests/platform/guest_tls_test.cpp`, about 35,000 checks): the image is copied exactly, to the right offset, to several independent blocks, and the captured image stays intact when a block is damaged; images of 1, 2, 7, 8, 9, 16, 100, 4096 and the largest size that fits are copied byte for byte, and one byte more is refused (a copy that is one byte short has to be caught as well, so the sizes are tested on their own; the tests use a synthetic image with the shape of the real one); every byte outside the image is zero for blocks pre-filled with 0x00, 0x01, 0x5a, 0x80, 0xff and a per-byte ramp, and canary bytes around each block never change; three blocks changed independently stay independent, and a block made after the others were changed starts from the original image; the image is the bytes of capture time after the source is overwritten, and two captures share no storage; null pointers, empty images, one byte short, bad tp offsets, misalignment, overlap with the image and every overflowing capture range fail with the right status and leave the block, the guards and the output unchanged; with `SNAP_MAIN_ELF` set (a local file, read only, never committed) the constants are checked against the real module, for its shape only (no `PT_TLS`, the slot word is `-1`); the expected bytes are not kept in the repository.

**Do the tests detect a wrong module?** 16 mutations of the header were each run against the test built with the sanitizers, and each was detected: copy offset `+0x18`; copy size `n-1`; no zeroing; zeroing only from `tp`; zeroing 16 bytes past the block; all images sharing one storage; an image that re-reads its source; no block-size check; block-size check off by one; no tp-offset check; no alignment check; no overlap check; no capture range check; an overflowing capture range check; a failed capture that clobbers its output; a capture at the wrong module offset. Two of them (no overlap check, no capture range check) are detected by AddressSanitizer rather than by a check, and the first run of the copy-size mutation was *not* detected until the multi-size test above was added.

**Limits.** `<vector>` is now included by `guest_tls.h` (the Windows loader and `hle_tests.cpp` include it): the Windows build was not run, so that is read, not verified. The 16-byte alignment floor and the zero-the-whole-block rule are decisions of this stage, not facts about the game. Nothing was run with the game, and no thread, fiber or loader code was exercised.

## Stage 4b: guest TLS in the loader and in guest threads

Stage 4a made the checked initialisation. Stage 4b connects it: the one capture of main's `.tdata`, the memory of a block, who owns it, the two musl thread-pointer hooks, and `host/loader.cpp` on Linux. `host/boot.cpp` is **not modified**, and no Linux runner exists yet, so the hooks are bound by a function the runner will call (`snap_host::install_guest_tls_hooks`), not by a runner.

**Files.**

| File | Class | What |
|---|---|---|
| `runtime/os/guest_tls_block.{h,cpp}` | shared | `guest_tls_main_image` (lazy, once, thread safe), `guest_tls_block_create` / `_destroy` (reserve + commit, registry, rollback), `guest_tls_hook_init` / `_destroy` (the hook bodies), a memory seam for tests |
| `host/guest_tls_hooks.{h,cpp}` | linux | `install_guest_tls_hooks(OsConfig)`: binds the two hook variables to loader stubs, as `host/boot.cpp:971` does for Windows. Not in `host/build_snap.py`'s explicit list, so never compiled for Windows |
| `host/loader.cpp` | **shared** (was `windows`) | `new_thread` and `init_guest_tls` use the module; the Win32 calls became `osk::host` calls (below) |
| `runtime/os/os_host.h` + both implementations | | one new primitive, `mem_readable` (`IsBadReadPtr` / `process_vm_readv`), for the stall dump in the loader |
| `scripts/source_classes.py`, `cmake/source_manifest.txt`, `CMakeLists.txt` | | the loader's six remaining Win32 names are guest SVC names in a string table (a reviewed entry); three test layers |

**Capture.** The first call that can read main's image does the capture (`GuestTlsImage::capture`, stage 4a) under a mutex and publishes the object with a release store; every later call, from any thread, returns that object and does not look at its arguments. A call that cannot capture (no module yet, or a module too small) captures nothing and latches nothing, so the next call tries again. The loader makes the first call in the first `new_thread`, before any other guest thread exists, as upstream did with a function-local static. The object is immutable and lives for the process; a block copies from it, so changing a block or the module afterwards changes nothing else. The `-1` slot-word check of upstream is kept (a trace line, once).

**Memory of a block.** `host::mem_reserve(0x10000, align 0x10000)` then `host::mem_commit`, then `guest_tls_init_block` (stage 4a: zero the whole block, copy the image to `tp + 0x10`). If the reserve fails the host's error is returned and nothing was reserved. If the commit fails the reservation is released through the same path before returning (`guest_tls_block_test` counts the calls and checks the address space is free again). There is no second allocator. Without an image (no module loaded) the block is zeros, as a thread got before the image existed.

**Ownership and release.** Every live block is in a registry: `tp -> {reservation, owner}`. The owner is the guest `ThreadType` (the loader passes the `ThreadType` it makes for the adopted main thread). A release needs the tp **and** the owner: an unknown tp, an interior pointer, the base of the reservation, a block already released and another thread's block all return false and release nothing. The registry entry is removed before the memory is, so two threads releasing the same block at once cannot both succeed (200 racing rounds in the test).

**The hooks.** `guest_tls_hook_init(c)`: `x0` = `ThreadType`, `c->tpidrro` = the TLR. It makes the block and stores `tp` in `TLR+0x1f0` (an offset of the TLR; the tests also check that `tp+0x1f0` is just zero TLS). It does not touch `c->tpidr`: `host_entry` reads `TLR+0x1f0` into `tpidr_el0` after the hook, which the test pins (the hook sees `tpidr == 0`, the entry sees `tpidr == TLR[0x1f0]`). A TLR that already holds a thread pointer is left alone (logged; a second block would leak the first). If no block can be made the hook calls `fatal`: the thread cannot run without its thread pointer. `guest_tls_hook_destroy(c)` runs after the TLS destructors (`host_entry` order, pinned by a mutation test): it releases the block named by `TLR+0x1f0` if `x0` is its owner, then clears the TLR word so a second call finds nothing. The Windows hooks in `host/boot.cpp` are unchanged and keep allocating their own block with `VirtualAlloc` and calling `loader::init_guest_tls`.

**Why zeroing the whole block is safe here (decision E).** The only writers of a fresh block before `guest_tls_init_block` are none: in `new_thread` the block is created after the stack, the TLR and the ThreadType, and the loader writes only pointers *to* it (`TLR+0x1f0`, `tpidr`); in the hook the block is created and then its tp is stored in the TLR. Nothing prepared in the block can be lost, and the stage-4a function is not changed. `loader::init_guest_tls(tp)` (the entry point `host/boot.cpp` calls for a block it allocated itself) keeps its old meaning: only the image is written, nothing is zeroed.

**Windows side of `host/loader.cpp`: what was read, not compiled.** Each replaced call and what it is now (`os_host_win32.cpp` holds the Win32 calls): `VirtualAlloc(base, span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)` -> `mem_reserve_at` + `mem_commit` (`VirtualAlloc(MEM_RESERVE, PAGE_NOACCESS)` then `VirtualAlloc(MEM_COMMIT, PAGE_READWRITE)` on the same range, rolled back with `VirtualFree` if the commit fails); `VirtualAlloc(nullptr, ...)` -> `mem_reserve(size, 0, ...)` + `mem_commit`, with the size rounded up to a page as `VirtualAlloc` did (a reservation of 4 KiB to 64 KiB gets a 64 KiB-aligned base either way); `_aligned_malloc` / `QueryPerformance*` / `IsBadReadPtr` -> the matching `osk::host` calls. The TLS block of a thread made by `new_thread` is still one 64 KiB reservation committed whole, so `os_state.cpp`'s check (`AllocationBase == tls`, `RegionSize == 0x10000`) holds. `<windows.h>` is no longer included by the loader: the file compiles on Linux with no Windows identifier left, which is the check that nothing else needed it (`os_host.h` replaces it). Line endings: `host/loader.cpp` is a mixed file (the lines of upstream's TLS commit are CRLF); the lines replaced in those blocks stayed CRLF and every other edit is LF. **Not verified on Windows:** that the loader compiles and links (it now needs `os_host_win32.cpp` and `guest_tls_block.cpp`, both in the `runtime/os/*.cpp` glob of `host/build_snap.py`), and that the game runs. `host/build_boot.py` (an early standalone build that compiles `loader.cpp`, `hooks.cpp` and `boot.cpp` only) already could not link `boot.cpp`'s `os_init` before this change and was not touched.

**Tests** (ctest, all without the game, a GPU or a window):

| ctest | What it shows |
|---|---|
| `guest_tls_block`, `_sanitized`, `_tsan` | The module alone, also under ASan + UBSan and under ThreadSanitizer: capture once (child processes), 60 rounds of 16 threads racing to capture with different images (one whole image wins, all see the same object); alignment, bytes, zeros and a 5000-cycle leak check on `/proc/self/maps`; ownership (foreign owner, interior pointer, base, zero, double release); 64 blocks isolated and released in halves; 8 threads releasing each other's blocks (refused); 200 rounds of 4 threads releasing one block (exactly one wins); rollback through the memory seam (a failed reserve reserves nothing; a failed commit releases once and the address space is free again; the host's error reaches the caller); the hook bodies on hand-built `Cpu`s (the TLR word, a second init, a wrong owner, a TLR holding a foreign pointer, a second destroy, no TLR); the hook aborts when no block can be made |
| `guest_tls_os` | The hooks inside the real OS layer, through `OsConfig::tp_init_var` / `tp_destroy_var`: 24 guest threads alive at once; per thread: the hook ran once before the entry, saw `tpidr == 0` and `TLR+0x1f0 == 0`, left `tpidr` to `host_entry`, the entry sees `tpidr == TLR[0x1f0]`, the image and zeros, its own words intact while the others write theirs; a TLS destructor armed on every thread finds its block live and intact; at exit the destroy hook saw its own block live, released it and cleared the TLR word; 24 distinct blocks; nothing live afterwards. Fibers: one fiber resumed by three live threads in turn runs with each resumer's thread pointer and reads that thread's own TLS; the thread keeps its pointer and words afterwards. The main thread has no hook |
| `guest_tls_loader` | `host/loader.cpp` linked on Linux with a stand-in `main` and stubs for the AOT tables: `new_thread` (block, TLR word, owner, 64 KiB alignment, capture once with the source overwritten afterwards, 8 host threads making 160 threads, no module loaded gives zeros and still captures later), `init_guest_tls` on a caller's block, and `install_guest_tls_hooks` driven through the loader's own thunk dispatch (`call_guest` on the bound stub): create, a wrong ThreadType refused, the owner releases, a second call does nothing; a non-empty variable is left alone |

**Do the tests detect a wrong integration?** 26 mutations of `guest_tls_block.cpp`, `os_kernel.cpp`, `host/loader.cpp` and `host/guest_tls_hooks.cpp` were each run against the three tests. 21 were detected at once (capture that latches a failure; a reservation that outlives a failed commit; no commit; an error that is not reported; a release that ignores the owner; a registry entry kept after release; the hook that also sets `tpidr`, writes the pointer at `tp+0x1f0` instead of `TLR+0x1f0`, replaces an existing pointer, or uses another owner; a destroy hook that does not clear the TLR word, skips the owner check or is skipped; `tpidr` read before the init hook; the loader not using the image, setting `tpidr` to the block base, not setting the TLR word, using the wrong owner, or `init_guest_tls` writing nothing or zeroing the whole block; the destroy stub bound to the init body; an installer that overwrites a non-empty variable). Five survived the first run: two are equivalent (removing only the unlocked fast path of the capture, which the check under the mutex repeats; and asking the seam for a page-aligned reservation, which the host still gives 64 KiB aligned) and three were gaps in the tests, closed afterwards and re-run: a second capture under a race (the threads now read the image right after the call and again later: 3 of 3 runs of the mutant fail), a release of the wrong size (the seam records it), and a destroy hook run before the TLS destructors (the OS test now has a TLS destructor that must still find its block). The module test also runs under ThreadSanitizer (`guest_tls_block_tsan`) and AddressSanitizer + UBSan.

**Not shown by these tests.** The ELF mapping (`load()` needs the game's files), any guest instruction reading `tpidr_el0`, the hooks bound by a real Linux runner (there is none), and a thread exiting by a path that skips the destroy hook (not investigated; the registry would keep its block until process exit).

**Left for 4c / the runner.** `g_tls_cpu` and `g_cur` (`thread_local` in the loader) after a fiber resumes on another host thread; who calls `install_guest_tls_hooks` and when (after `os_init`, as `host/boot.cpp` does); what a thread that exits without the destroy hook should do; whether Windows `boot.cpp` should call the module's hooks instead of its own copies (a separate, Windows-validated change).

## Stage 4c: guest TLS across host fibers, thread life cycle, hook order

Evidence for the questions stage 4b left open. One behaviour change in production code (the destroy hook also clears `c->tpidr`) and one signature change (`install_guest_tls_hooks` returns how many hooks are bound); everything else is tests and this text. `host/boot.cpp` and every Windows-only file are untouched.

**`g_tls_cpu` and `g_cur` (read from `host/loader.cpp`, `host/boot.cpp`, `host/mods/lodanim.cpp`).**

| | `loader::g_tls_cpu` | `g_cur` (static in the loader) |
|---|---|---|
| What | `thread_local Cpu *`: "the Cpu of the last host call on this host thread" (the header's own comment) | `thread_local GuestThread *`: what `new_thread` made, on the host thread that called it |
| Written | `rt_dispatch` on every thunk call (`g_tls_cpu = c`), `call_guest_nested` before it dispatches | `new_thread` only |
| Read | crash and stall reports (`host/boot.cpp:134`, the guest-chain printers `:874/:896/:918`), `lodanim.cpp:104` (a trace line), a stress switch that is off by default | `fp_ok`, which decides whether a frame pointer is plausible in `guest_chain` (stall and crash dumps) |
| Host or guest state | **host-thread** diagnostic state. Nothing the game computes depends on it | the same; only meaningful on the host thread that created the GuestThread |

*Can a fiber see a wrong `Cpu`?* Only in the window between a resume on another host thread and the fiber's next host call: the call that switched to the fiber was made by the resuming thread with that thread's own Cpu, so `g_tls_cpu` still names it. Every thunk call sets it again to the fiber's Cpu. A crash report taken inside that window would name the thread's registers instead of the fiber's. The test pins this exactly (`tls_host_fibers`: after each of 600 resumes `g_tls_cpu` is the resumer's Cpu; after the fiber's next host call it is the fiber's; the very first run is a dispatch on the fiber's own Cpu). *`g_cur`*: on the thread that called `new_thread` every `guest_chain` frame must lie in that GuestThread's stack, so a fiber running there has its chain cut after the first two entries; elsewhere the check is a 64 MB window above `sp`. The test shows both sides.

*Windows or Linux?* Neither is a Linux defect: the variables, their writers and their readers are the same code on both, and no function reads one of them after a call that can change the host thread (checked in `rt_dispatch` and `call_guest_nested`: the write comes first and nothing follows it). *Smallest correction:* none to the semantics, and `thread_local` stays. What changes is the contract, now written down: they are host-thread diagnostics; a reader that needs "the Cpu running now" for a guest context asks the OS layer (`Thread::cur`, `os_current_thread_type`), which `self_fresh()` keeps right across switches. For the Linux runner this means its fatal-signal handler must not trust `g_tls_cpu` right after a resume (stage 5). Not done here, because it would be a design change with no failure behind it.

**Other `thread_local` state that spans two host calls (a risk register, not a bug list).** Where a value is written in one host call and read in a later one, a fiber that is switched out between the two and resumed on another host thread reads the other thread's value. Found: `host/mods/gamemods.cpp` (`t_font_scale`, set at the SetScale call and read at the glyph hooks; `t_font_ring*`), `host/mods/drpfast.cpp` (`t_want`, `t_wantState`, `t_begin`, `t_stateAddr`: a pending chunk between two hooks), `host/mods/crcfast.cpp` (`t_want`, `t_pending`), `runtime/nvn/nvn_entry.cpp` (`t_guestFp`, `t_guestLr`: set and read inside one NVN call, so safe), `runtime/nvn/nvn_shiny.cpp` (`tl_twinSp`, `tl_twinArea`). The three mods are optional and the pairs of hooks sit inside one guest function that does not wait; no case was found where the game switches fibers between them, and none was tested (no guest code). OS layer: `t_self` (always read through `self_fresh()`), `t_helper`, the perf ledger and the few counters inside blocking calls, none of which spans a fiber switch.

**Host TLS is untouched by the switch (`tls_host_fibers`).** The real loader dispatch, the real OS layer and the System V switch together, with three guest threads on three host threads resuming one fiber 600 times in turn. After every resume the fiber sees the resuming thread's `%fs` base (read with `arch_prctl(ARCH_GET_FS)`, never set), TCB canary (`%fs:0x28`), `pthread_self()`, `gettid()`, a `thread_local` value of its own, and `osk::self_fresh()` (its Thread, its host tid); after the fiber returns, the thread sees its own again. A `thread_local` address taken before the switch and read after it went stale in 599 of 600 hops: the hazard is real, it is exactly what `self_fresh()` exists for, and the test would fail if the fibers did not really change host threads. Run optimised, at `-O0`, and with stack protectors in every function. The test also found a hazard in itself: `pthread_self()` is declared `const`, so the compiler reused one value across a switch in the fiber's loop; it is read through a function the compiler cannot see through. Guest side, at each resume: the fiber's `tpidr` and `tpidrro` are the resuming thread's, a word of that thread's TLS read through them is that thread's own, a word written by the fiber lands in that thread's block, and the fiber's `Cpu` is never the thread's.

**Life of a guest thread, read from `os_kernel.cpp` and `os_thread.cpp`.** A thread ends only when its entry returns inside `host_entry`: there is no `nn::os::ExitThread` override and `os_svc` handles no svcExitThread (a guest that used one would stop at "unhandled svc", not leak a block). Then, in this order: the TLS destructors, the destroy hook, `fiber_thread_exit` (the thread's own context, shadow fiber and guest fiber object), the thread state, the core. A thread created and never started never reaches `host_entry`, so it never has a block (tested). A block is released once, only by its owner: the registry entry goes first, and a replay of the destroy hook with the values of a thread that is gone releases nothing (tested, also with a live block of another owner in the registry). A parked fiber keeps no pointer into a finished thread: `do_switch` copies `tpidr`/`tpidrro` from the thread that resumes it, so a fiber last run by thread A and resumed by thread B reads B's TLS (tested: A exits first, its block is gone, the fiber runs again with B's). The one correction made: the destroy hook now also clears `c->tpidr` when it still holds the released tp, so the thread's own `Cpu` does not keep a pointer into memory that is gone (a mutation test fails without it). A block is released with the thread, not with the host thread that ran a fiber last. At process exit (`QuickExit`, `terminate_process`) nothing is released; the OS reclaims the mappings. Counts: 640 guest threads in 40 waves of 16 made 640 reservations and released 640 (counted through the memory seam), `guest_tls_blocks_live()` back to its start after every wave, each thread's TLS destructor found its block live and holding its own word, and no `Cpu` kept a tp after its hook. Not shown: a guest holding a TLS pointer in a register across a fiber switch (that is a bug on Horizon too) and the entry returning on a fiber (it cannot: `host_entry` is on the thread's own stack).

**Hook installation order (the contract for the Linux runner; `host/guest_tls_hooks.h`).** `host/boot.cpp` does: `load` -> (`os_init` with the two variables from `loader::lookup`) -> bind the stubs -> `new_thread()` -> `os_adopt_main_thread` and `TLR+0x1f0 = c->tpidr`. Reasons: the variables are guest memory in the loaded modules, and `add_stub` needs the loader's thunk table (so `load` first); the OS layer reads the variable addresses at `os_init` and the function pointers when a thread starts (so before the first guest thread, not necessarily before `os_init`); the main thread's `new_thread` makes the first block and captures main's image, so a thread started earlier would get a block of zeros (the hook warns). The binding may be repeated: only an empty variable is filled, nothing is added the second time; it depends on the loader's thunk table and nothing else. `install_guest_tls_hooks` returns 2 when both are bound, fewer when a variable is missing or null, and prints which. `tls_host_fibers` runs this exact order with the real loader and OS layer.

**Tests added** (ctest): `guest_tls_loader` (extended: the binding's return values, `tpidr` cleared with the block), `guest_tls_lifecycle`, `tls_host_fibers_plain`, `_O0`, `_ssp`. No ThreadSanitizer or AddressSanitizer build of the fiber tests: both break on a hand-made stack switch (false reports); the module test keeps both.

**Do the tests detect a wrong implementation?** 11 mutations were run against `tls_host_fibers` and `guest_tls_lifecycle`, and all were detected: a switch that restores the previous host thread's `%fs` after a resume (the fibers test hangs: the host's own TLS is corrupted); a fiber `Cpu` that does not take the resuming thread's `tpidr`, or `tpidrro`; `do_switch` keeping the pre-switch `Thread` after a resume; `self_fresh()` caching the first Thread per process; `g_cur` and `g_tls_cpu` as process-wide variables (the second was first missed, because the hops are serialised, and a direct per-thread check was added); `rt_dispatch` not setting `g_tls_cpu`; a destroy hook that leaves `c->tpidr` dangling; a destroy hook that never runs. One more mutation of the first round did nothing to the code (an empty function added) and is not counted.

**What needs the runner.** Real guest code reading `tpidr_el0` in `main` and `sdk`; the hooks reached as lifted thunks from sdk's trampoline (here the OS layer calls them through the same dispatch, with stand-in guest functions); the Linux fatal-signal handler (it must find the running Cpu through the OS layer, not `g_tls_cpu`); the order above enforced by one function; whether the game ever switches fibers between the two hooks of the mods listed above.

## Known limits

Observed:

- The host builds and its tests pass on Linux (see "Current state"). The files still classified `windows` in `cmake/source_manifest.txt` are not compiled there. Whether the game runs end to end on Linux is **not established** by any test: guest execution is opt-in (`--allow-guest-exec`) and experimental. The renderer, the overlay and the window were built but not exercised on a GPU in the test run.
- Save states do not exist on Linux (`os_state_off.cpp`): a request is refused and says so. The UI and the route code that call `snap_state_*` get honest answers.
- Write tracking does not exist on Linux: guest reservations are plain and `runtime/nvn/nvn_vk.cpp` copies whole ranges. It asks `write_watch_supported()` and falls back to full copies when that is false (the existing fallback; slower, not incorrect). A kernel-assisted replacement (see the options above) is not implemented.
- POSIX differences that are documented and not hidden: `yield` is `sched_yield`; thread CPU times come from `schedstat` (or `stat`, 10 ms ticks, where the kernel has no `schedstat`); the profiler's "mcycles" column is nanoseconds of CPU time on Linux; thread names are cut to 15 characters.
- Virtual memory limits that Windows does not have: every commit/decommit that splits a mapping adds a kernel mapping, and a process may have `vm.max_map_count` of them (65530 by default). Heavy fragmentation of a reservation could reach it; the error is `ENOMEM`, which the VAMM treats as transient and retries for the configured time before failing. Under `vm.overcommit_memory=2` (strict accounting) `MAP_NORESERVE` is ignored and large reservations may be refused. Neither was exercised beyond the random-sequence test.
- `os_scheduler` pinned to one CPU (`taskset -c 0`) is not a supported condition: no check failed, but the run did not finish in 240 s (`sched yield any` took 70 s instead of 80 ms; the stress and migration tests also took several seconds longer). The test spins and yields waiting for other threads, and `sched_yield` on one CPU hands the CPU over less readily than `SwitchToThread`. This is a hypothesis; it was not investigated, and it was not run on Windows for comparison. `os_host_test` passes on one CPU.
- No Windows toolchain exists in this environment: nothing Windows was built or run. The Windows-side changes are listed under "Windows side".
- The renderer, overlay and window tests exist (`nvn_os_host_linux`, `nvn_renderer_linux`, `window_sdl3_linux`, `ui_overlay_linux`) but skip their GPU and display parts when no Vulkan 1.3 GPU or SDL video is present, as in the container used for the test run. A run on a machine with a GPU and a display has to be done to know what they cover. The earlier port's `linux_nvn_host*` tests, which lost focus under Wayland, are not part of this branch.

Hypotheses (not checked):

- The shared-code edits and the five Windows-side file edits behave the same on the Windows toolchain.
- Classes will shift as stages land; `windows` is expected to shrink.

## What the next stages depend on

| Stage | Needs from this one |
|---|---|
| 3 OS layer (`os_ctx`, `os_state`) | Done in parts A and B. Left in `runtime/os`: `hostprof.cpp` and `sampler.cpp` (profilers, `windows`), `os_state.cpp` (Windows only; Linux has `os_state_off.cpp`) |
| 4 TLS | Done (4a, 4b, 4c above) |
| 5 Linux runner | Done: `host/linux/` (runner, init, crash and signal handling, preflight, window) |
| 6 HLE, audio, input, UI | Done for the parts listed in "Current state"; the rest is stubbed explicitly |
| 7 Vulkan backend | Ported and built (`runtime/nvn/linux/`); `nvn_vk.cpp` falls back to full copies without write tracking. Open: running it on a GPU, and the uffd-wp evaluation with measurements |

(This table was written for the stages ahead and is kept with its rows updated; see "Current state" for what exists.)
