# New Pokémon Snap Recomp — Linux Port Roadmap

- **Repository:** `https://github.com/didslima/new-poke-snap-switch-recomp-linux`
- **Active branch:** `integration/upstream-v1.3.0`
- **Current release:** `v1.3.0-linux.1-rc1` (pre-release)

---

## Completed Milestones

- Linux ROMFS path configuration and preflight validation before entering `nnMain()`.
- CLI support for `--romfs <folder>` with precedence `--romfs` > `SNAP_ROMFS` > `extracted/romfs-v262144`.
- Built-in graphical launcher using SDL3 (`SDL_Renderer`) and Dear ImGui (`v1.93.0`).
- Automatic discovery of required ELF/archive artifacts and ROMFS with persistent XDG configuration (`$XDG_CONFIG_HOME/new-pokemon-snap-recomp/config`).
- KDE Plasma desktop integration (`.desktop` entry and icon installation under `$XDG_DATA_HOME`, native `xdg-desktop-portal` file/folder pickers).
- Manual Bazzite / KDE Plasma validation.
- Published pre-release `v1.3.0-linux.1-rc1` (preserving upstream `Snap Recomp 1.3.0` and stable release `v1.3.0`).
- Linux profile activation (`hle::activate_profiles()` in `host/linux/init_linux.cpp`) preserving existing Profile 1 saves alongside explicit `--data-dir` overrides.
- High frame-rate support (>60 FPS, validated up to **240 FPS** during real ride gameplay on Bazzite / NVIDIA RTX 5070 Ti): ported `host/mods/uipace.cpp` (`rt_gm_ui_enter`, `rt_gm_ui_event`, `rt_gm_ui_pad`, `rt_gm_ui_pointer`) and `snap_present_count()` to Linux so menus, dialogue, button edges, and pointer keep 60 Hz pacing.

---

## Prioritized Pipeline

### P1 — Launcher Update Notifications *(Planned — Not Yet Implemented)*

Allow the graphical launcher to detect newer Linux-port releases without interfering with offline use or normal game startup.

#### Requirements
- Query GitHub Releases for `didslima/new-poke-snap-switch-recomp-linux`.
- Identify the latest Linux-port version (distinguishing Linux-port tags/versions from upstream `1.3.0`).
- Distinguish stable releases from pre-releases.
- Allow the user to choose whether to include pre-releases.
- Display a non-intrusive notification in the launcher UI when a newer version is available.
- Provide a direct link to the GitHub release page.
- Never download or install updates automatically.
- Do not interrupt or block normal game startup.
- Cache the last successful check result locally.
- Avoid checking on every launch by using a configurable check interval.
- Handle offline conditions, network/API errors, HTTP rate limits, and malformed JSON responses gracefully.
- Never transmit ROMFS paths, save paths, game data, or personal information.
- Do not require authentication for public release checks.

#### Acceptance Criteria
- No update available: no intrusive notification is shown.
- Stable update available: notification displays the new version and a link to the release page.
- Pre-release available: notification is only shown when the user has enabled pre-release checks.
- Offline or API failure: the launcher continues working normally without blocking **Play**.
- Repeated launches within the configured interval reuse the cached result without issuing new network requests.
- The user can manually trigger an update check from the launcher UI.

#### Testing Plan
- Mock GitHub Releases API responses in unit tests.
- Test version comparison across stable and pre-release Linux-port version strings.
- Test network failures, timeouts, HTTP rate limiting (`403`/`429`), and malformed responses.
- Test cache persistence and configurable check intervals.
- Verify that requests contain no ROMFS paths, save paths, game data, or personal information.

---

### P1 (High) — Fix Pokémon 3D Model Positioning in Photodex on Ultrawide *(Validated in QA13)*

**Priority:** High  
**Evidence:** QA12 screenshot of the Photodex screen showing Sharpedo extending outside the centered 16:9 area on an ultrawide monitor (`5120×1440`).

#### Investigation Items
- Investigate how the Photodex calculates 3D model position and scale.
- Determine whether positioning uses full window resolution or the usable 16:9 area.
- Check for ultrawide-specific camera, viewport, or transform adjustments (`nvn_wide.cpp`, `hooks_mods_stub.cpp`, `nvn_hud.cpp`).
- Identify how to fix ultrawide bounds without affecting 16:9 displays.
- Verify other Pokémon and other screens that render 3D models.

#### Goal
Keep 3D models properly positioned within the usable 16:9 area on ultrawide resolutions without stretching or clipping models.

---

### P2 (Medium) — Hide Native Linux OS Cursor While Game Window Has Focus *(Validated in QA13)*

**Priority:** Medium

#### Requirements
- Hide only the native OS cursor while the game window is focused.
- Preserve the game's internal cursor (`rt_gm_ui_pointer` / UI pointer).
- Restore the OS cursor cleanly when losing focus, minimizing, or closing the game.
- Prevent the OS cursor from staying permanently hidden after an unexpected crash/exit.
- Verify compatibility with SDL3, KDE Plasma, and Wayland.

---

### P0/P1 — Fix Mouse Speed / Direction / FPS Consistency During Ride *(Implemented — Awaiting Manual QA in QA20)*

- Preserved QA18 extended radial curve by default (`SNAP_MOUSE_DUAL_STICK` off).
- Added experimental **Two-Stage Split Radial** behind `SNAP_MOUSE_DUAL_STICK=1`: slow/medium (`mmag <= 1.0`) stays strictly on Right Stick; overflow (`mmag > 1.0`) saturates Right Stick in the exact movement direction and sends proportional excess `min(mmag - 1.0, 1.0)` along the same vector to Left Stick, yielding immediately if physical Left Stick or `WASD` is active.
- Added rapid-reversal anti-cancellation guard (`>=16 px` followed by `>=8 px` opposite sign) in `DeltaAccum::add` and `Hub::poll`.

---

## Future Pipeline *(Lower-Priority Ideas — Not Yet Scheduled / Not Committed Scope)*

- **P3 — Melhoria visual futura — Tremedeira nas transições de menus:**
  - Alguns objetos apresentam tremedeira (*jitter*) durante as animações de transição entre menus.
  - Investigar a causa e corrigir em uma atualização futura.
  - Não é bloqueador da integração atual.
  - Não implementado nesta etapa.

- **FPS target on Linux:** the Linux runtime now applies the saved/live `SNAP_FPS` target to NVN frame pacing after profile activation and again when settings change. Still needs a Linux build and in-game validation; actual FPS remains limited by display refresh, GPU performance, and the separate frame-cap option.
- **Linux temporal upscaling and frame generation:** Linux currently uses explicit unsupported stubs. First implementation milestone should be temporal super resolution, keeping frame generation separate. NVIDIA publishes Linux NGX libraries for DLSS SR; FSR needs a Linux Vulkan backend integration; Intel's current XeSS SDK offers Vulkan APIs but the public SDK/release assets do not establish a native Linux runtime library. Audit API/licensing and package optional vendor libraries before choosing a backend. HDR10/scRGB implementation is tracked separately and still needs real HDR hardware validation.
- **P2:** Improve launcher diagnostics and log access.
- **P3:** Add configurable graphics presets.
- **P4:** Improve installation and KDE integration documentation.
- **[LOW] DLSS residual flickering on Professor Mirror during Research Level / Expedition Points screen:**
  - Occurs during the post-evaluation results presentation (after individual photo scoring), while Research Level / Expedition Points animate with Professor Mirror on screen.
  - Reproduces with `SNAP_UPSCALER=dlss`; does not occur with `SNAP_UPSCALER=off`.
  - Still present in `v1.3.0-linux.12-expedition-fix` (`lateClean` streak + `lateActive`).
  - Next steps when resuming: investigate DLSS temporal history and character/object motion vectors during that screen, and test `SNAP_AA=fxaa` as a diagnostic (not yet tested).
  - Not a release blocker.
