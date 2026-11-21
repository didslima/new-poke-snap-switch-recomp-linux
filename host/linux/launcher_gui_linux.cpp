// SPDX-License-Identifier: MIT
#include "launcher_gui_linux.h"
#include "launcher_config_linux.h"

#include <SDL3/SDL.h>
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace snap_linux {

namespace {

enum class PickerTarget {
    None = 0,
    MainElf,
    SdkElf,
    MainLib,
    SdkLib,
    Romfs,
    DataDir,
};

struct PickerState {
    std::mutex mu;
    bool pending = false;
    PickerTarget target = PickerTarget::None;
    std::string chosen_path;
    std::string error;
};

struct DialogCallbackCtx {
    std::shared_ptr<PickerState> state;
};

void SDLCALL on_dialog_done(void *userdata, const char *const *filelist, int /*filter*/) {
    std::unique_ptr<DialogCallbackCtx> ctx(static_cast<DialogCallbackCtx *>(userdata));
    if (!ctx || !ctx->state) return;
    std::lock_guard<std::mutex> lk(ctx->state->mu);
    if (!filelist) {
        const char *sdl_err = SDL_GetError();
        ctx->state->error = (sdl_err && *sdl_err) ? sdl_err : "File dialog failed.";
    } else if (filelist[0] && *filelist[0]) {
        ctx->state->chosen_path = filelist[0];
        ctx->state->error.clear();
    }
    ctx->state->pending = false;
}

std::string resolve_self_exe_path(const std::string &argv0) {
    char buf[4096] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return std::string(buf);
    }
    return argv0;
}

std::string resolve_bundled_icon_path(const std::string &self_exe) {
    if (self_exe.empty()) return {};
    std::error_code ec;
    const fs::path bin_dir = fs::u8path(self_exe).parent_path();
    const fs::path cand = bin_dir.parent_path() / "share" / "icons" / "hicolor" / "256x256" / "apps" / "new-pokemon-snap-recomp.png";
    if (fs::is_regular_file(cand, ec) && !ec) return cand.string();
    return {};
}

ImGuiKey sdl_key_to_imgui(SDL_Keycode key) {
    switch (key) {
        case SDLK_TAB: return ImGuiKey_Tab;
        case SDLK_LEFT: return ImGuiKey_LeftArrow;
        case SDLK_RIGHT: return ImGuiKey_RightArrow;
        case SDLK_UP: return ImGuiKey_UpArrow;
        case SDLK_DOWN: return ImGuiKey_DownArrow;
        case SDLK_PAGEUP: return ImGuiKey_PageUp;
        case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
        case SDLK_HOME: return ImGuiKey_Home;
        case SDLK_END: return ImGuiKey_End;
        case SDLK_INSERT: return ImGuiKey_Insert;
        case SDLK_DELETE: return ImGuiKey_Delete;
        case SDLK_BACKSPACE: return ImGuiKey_Backspace;
        case SDLK_SPACE: return ImGuiKey_Space;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return ImGuiKey_Enter;
        case SDLK_ESCAPE: return ImGuiKey_Escape;
        case SDLK_A: return ImGuiKey_A;
        case SDLK_C: return ImGuiKey_C;
        case SDLK_V: return ImGuiKey_V;
        case SDLK_X: return ImGuiKey_X;
        case SDLK_Y: return ImGuiKey_Y;
        case SDLK_Z: return ImGuiKey_Z;
        default: return ImGuiKey_None;
    }
}

void apply_breeze_dark_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.FrameRounding = 6.0f;
    st.GrabRounding = 6.0f;
    st.ChildRounding = 8.0f;
    st.PopupRounding = 6.0f;
    st.WindowPadding = ImVec2(18.0f, 16.0f);
    st.FramePadding = ImVec2(10.0f, 6.0f);
    st.ItemSpacing = ImVec2(10.0f, 8.0f);

    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_Border] = ImVec4(0.24f, 0.27f, 0.32f, 1.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.28f, 0.34f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.24f, 0.29f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.33f, 0.42f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.45f, 0.72f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.42f, 0.68f, 0.80f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.48f, 0.76f, 0.90f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
}

void render_imgui_to_sdl(SDL_Renderer *renderer, ImDrawData *draw_data) {
    if (!draw_data || draw_data->CmdListsCount == 0) return;

    const float scale_x = draw_data->FramebufferScale.x;
    const float scale_y = draw_data->FramebufferScale.y;
    const float off_x = draw_data->DisplayPos.x;
    const float off_y = draw_data->DisplayPos.y;

    SDL_SetRenderScale(renderer, scale_x, scale_y);

    std::vector<SDL_FColor> colors;
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList *cmd_list = draw_data->CmdLists[n];
        const ImDrawVert *vtx = cmd_list->VtxBuffer.Data;
        const ImDrawIdx *idx = cmd_list->IdxBuffer.Data;

        colors.resize(static_cast<size_t>(cmd_list->VtxBuffer.Size));
        for (int i = 0; i < cmd_list->VtxBuffer.Size; ++i) {
            const ImU32 col = vtx[i].col;
            colors[i].r = ((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
            colors[i].g = ((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
            colors[i].b = ((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
            colors[i].a = ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        }

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; ++cmd_i) {
            const ImDrawCmd &pcmd = cmd_list->CmdBuffer[cmd_i];
            if (pcmd.UserCallback) {
                pcmd.UserCallback(cmd_list, &pcmd);
                continue;
            }

            SDL_Rect clip{};
            clip.x = static_cast<int>(pcmd.ClipRect.x - off_x);
            clip.y = static_cast<int>(pcmd.ClipRect.y - off_y);
            clip.w = static_cast<int>(pcmd.ClipRect.z - pcmd.ClipRect.x);
            clip.h = static_cast<int>(pcmd.ClipRect.w - pcmd.ClipRect.y);
            if (clip.w <= 0 || clip.h <= 0) continue;
            SDL_SetRenderClipRect(renderer, &clip);

            auto *tex = reinterpret_cast<SDL_Texture *>(static_cast<intptr_t>(pcmd.GetTexID()));
            SDL_RenderGeometryRaw(
                renderer,
                tex,
                reinterpret_cast<const float *>(reinterpret_cast<const char *>(vtx) + offsetof(ImDrawVert, pos)),
                sizeof(ImDrawVert),
                colors.data(),
                sizeof(SDL_FColor),
                reinterpret_cast<const float *>(reinterpret_cast<const char *>(vtx) + offsetof(ImDrawVert, uv)),
                sizeof(ImDrawVert),
                cmd_list->VtxBuffer.Size,
                idx + pcmd.IdxOffset,
                static_cast<int>(pcmd.ElemCount),
                sizeof(ImDrawIdx));
        }
    }
    SDL_SetRenderClipRect(renderer, nullptr);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

void copy_to_buf(const std::string &src, std::array<char, 1024> &dst) {
    dst.fill('\0');
    const size_t n = std::min(src.size(), dst.size() - 1);
    std::memcpy(dst.data(), src.data(), n);
}

struct ResolutionPreset {
    const char *label;
    int w;
    int h;
};

constexpr ResolutionPreset kResPresets[] = {
    {"1280 x 720 (HD)", 1280, 720},
    {"1920 x 1080 (Full HD)", 1920, 1080},
    {"2560 x 1440 (QHD)", 2560, 1440},
    {"3440 x 1440 (Ultrawide 21:9)", 3440, 1440},
    {"3840 x 2160 (4K UHD)", 3840, 2160},
    {"5120 x 1440 (Super Ultrawide 32:9)", 5120, 1440},
    {"Custom", 0, 0},
};

int match_res_preset(int w, int h) {
    for (int i = 0; i < 6; ++i) {
        if (kResPresets[i].w == w && kResPresets[i].h == h) return i;
    }
    return 6;
}

int spawn_and_wait_game(const std::string &self_exe, const LauncherConfig &cfg, std::string &err_out) {
    err_out.clear();
    const std::vector<std::string> args = build_launch_cli_args(self_exe, cfg);
    std::vector<char *> c_args;
    c_args.reserve(args.size() + 1);
    for (const std::string &a : args) {
        c_args.push_back(const_cast<char *>(a.c_str()));
    }
    c_args.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        err_out = std::string("Could not start game process: ") + std::strerror(errno);
        return -1;
    }
    if (pid == 0) {
        ::execv(self_exe.c_str(), c_args.data());
        _exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            err_out = "Failed while waiting for the game process.";
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        if (code != 0) {
            err_out = "Game exited with status " + std::to_string(code) +
                      ". Please verify your ROMFS, ELF, and AOT library files.";
        }
        return code;
    }
    if (WIFSIGNALED(status)) {
        err_out = "Game stopped unexpectedly (signal " + std::to_string(WTERMSIG(status)) + ").";
        return 128 + WTERMSIG(status);
    }
    return 1;
}

} // namespace

int run_launcher_gui(const std::string &argv0, const std::string &initial_error) {
    const std::string self_exe = resolve_self_exe_path(argv0);
    const std::string exe_dir = self_exe.empty() ? std::string() : fs::u8path(self_exe).parent_path().string();
    const std::string cfg_path = launcher_config_path();

    LauncherConfig cfg;
    std::string status_msg = initial_error;
    bool status_is_error = !initial_error.empty();

    std::string load_err;
    if (!load_launcher_config(cfg_path, cfg, load_err) && !load_err.empty()) {
        status_msg = load_err;
        status_is_error = true;
    }
    auto_detect_launcher_paths(cfg, exe_dir);

    while (true) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "snap_boot_linux: failed to initialize SDL3 video for launcher GUI: %s\n", SDL_GetError());
            return 1;
        }

        SDL_Window *window = SDL_CreateWindow(
            "New Pokémon Snap Recomp — Launcher",
            820,
            636,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) {
            std::fprintf(stderr, "snap_boot_linux: failed to create launcher window: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }
        SDL_SetWindowMinimumSize(window, 680, 520);

        SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            std::fprintf(stderr, "snap_boot_linux: failed to create SDL3 renderer: %s\n", SDL_GetError());
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        SDL_SetRenderVSync(renderer, 1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        apply_breeze_dark_style();

        float dpi_scale = SDL_GetWindowDisplayScale(window);
        if (dpi_scale < 1.0f) dpi_scale = 1.0f;
        ImFontConfig font_cfg;
        font_cfg.SizePixels = static_cast<float>(static_cast<int>(15.0f * dpi_scale + 0.5f));
        io.Fonts->AddFontDefault(&font_cfg);
        io.FontGlobalScale = 1.0f / dpi_scale;

        unsigned char *pixels = nullptr;
        int tex_w = 0, tex_h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &tex_w, &tex_h);
        SDL_Texture *font_tex = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, tex_w, tex_h);
        SDL_UpdateTexture(font_tex, nullptr, pixels, tex_w * 4);
        SDL_SetTextureBlendMode(font_tex, SDL_BLENDMODE_BLEND);
        io.Fonts->SetTexID(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(font_tex)));

        std::array<char, 1024> main_elf_buf{}, sdk_elf_buf{}, main_lib_buf{}, sdk_lib_buf{}, romfs_buf{}, data_dir_buf{};
        auto sync_buffers_from_cfg = [&]() {
            copy_to_buf(cfg.main_elf, main_elf_buf);
            copy_to_buf(cfg.sdk_elf, sdk_elf_buf);
            copy_to_buf(cfg.main_lib, main_lib_buf);
            copy_to_buf(cfg.sdk_lib, sdk_lib_buf);
            copy_to_buf(cfg.romfs, romfs_buf);
            copy_to_buf(cfg.data_dir, data_dir_buf);
        };
        sync_buffers_from_cfg();

        auto picker = std::make_shared<PickerState>();
        bool running = true;
        bool launch_requested = false;
        Uint64 last_ticks = SDL_GetTicks();

        SDL_StartTextInput(window);

        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        if (ev.window.windowID == SDL_GetWindowID(window)) running = false;
                        break;
                    case SDL_EVENT_MOUSE_MOTION:
                        io.AddMousePosEvent(ev.motion.x, ev.motion.y);
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    case SDL_EVENT_MOUSE_BUTTON_UP: {
                        int btn = -1;
                        if (ev.button.button == SDL_BUTTON_LEFT) btn = 0;
                        else if (ev.button.button == SDL_BUTTON_RIGHT) btn = 1;
                        else if (ev.button.button == SDL_BUTTON_MIDDLE) btn = 2;
                        if (btn >= 0) io.AddMouseButtonEvent(btn, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                        break;
                    }
                    case SDL_EVENT_MOUSE_WHEEL:
                        io.AddMouseWheelEvent(ev.wheel.x, ev.wheel.y);
                        break;
                    case SDL_EVENT_KEY_DOWN:
                    case SDL_EVENT_KEY_UP: {
                        const ImGuiKey ik = sdl_key_to_imgui(ev.key.key);
                        if (ik != ImGuiKey_None) io.AddKeyEvent(ik, ev.type == SDL_EVENT_KEY_DOWN);
                        io.AddKeyEvent(ImGuiMod_Ctrl, (ev.key.mod & SDL_KMOD_CTRL) != 0);
                        io.AddKeyEvent(ImGuiMod_Shift, (ev.key.mod & SDL_KMOD_SHIFT) != 0);
                        io.AddKeyEvent(ImGuiMod_Alt, (ev.key.mod & SDL_KMOD_ALT) != 0);
                        break;
                    }
                    case SDL_EVENT_TEXT_INPUT:
                        io.AddInputCharactersUTF8(ev.text.text);
                        break;
                    default:
                        break;
                }
            }

            // Apply async SDL3 file/folder dialog result.
            {
                std::lock_guard<std::mutex> lk(picker->mu);
                if (!picker->pending && picker->target != PickerTarget::None) {
                    if (!picker->chosen_path.empty()) {
                        switch (picker->target) {
                            case PickerTarget::MainElf: cfg.main_elf = picker->chosen_path; break;
                            case PickerTarget::SdkElf: cfg.sdk_elf = picker->chosen_path; break;
                            case PickerTarget::MainLib: cfg.main_lib = picker->chosen_path; break;
                            case PickerTarget::SdkLib: cfg.sdk_lib = picker->chosen_path; break;
                            case PickerTarget::Romfs: cfg.romfs = picker->chosen_path; break;
                            case PickerTarget::DataDir: cfg.data_dir = picker->chosen_path; break;
                            case PickerTarget::None: break;
                        }
                        sync_buffers_from_cfg();
                        std::string save_err;
                        if (save_launcher_config(cfg_path, cfg, save_err)) {
                            status_msg = "Selection updated and saved.";
                            status_is_error = false;
                        } else {
                            status_msg = save_err;
                            status_is_error = true;
                        }
                    } else if (!picker->error.empty()) {
                        status_msg = picker->error;
                        status_is_error = true;
                    }
                    picker->target = PickerTarget::None;
                    picker->chosen_path.clear();
                    picker->error.clear();
                }
            }

            int win_w = 0, win_h = 0, fb_w = 0, fb_h = 0;
            SDL_GetWindowSize(window, &win_w, &win_h);
            SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
            io.DisplaySize = ImVec2(static_cast<float>(win_w), static_cast<float>(win_h));
            io.DisplayFramebufferScale = ImVec2(
                win_w > 0 ? static_cast<float>(fb_w) / static_cast<float>(win_w) : 1.0f,
                win_h > 0 ? static_cast<float>(fb_h) / static_cast<float>(win_h) : 1.0f);

            const Uint64 now = SDL_GetTicks();
            io.DeltaTime = std::max(0.001f, static_cast<float>(now - last_ticks) / 1000.0f);
            last_ticks = now;

            ImGui::NewFrame();

            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin(
                "##LauncherRoot",
                nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

            ImGui::TextUnformatted("New Pokémon Snap Recomp");
            ImGui::SameLine();
#if defined(SNAP_VERSION) && defined(SNAP_LINUX_PORT_VERSION)
            ImGui::TextDisabled("v%s (Linux Port %s)", SNAP_VERSION, SNAP_LINUX_PORT_VERSION);
#elif defined(SNAP_VERSION)
            ImGui::TextDisabled("v%s (Linux)", SNAP_VERSION);
#else
            ImGui::TextDisabled("(Linux)");
#endif
            ImGui::TextDisabled("Configure required game files, ROMFS, and display preferences.");
            ImGui::Separator();
            ImGui::Spacing();

            const ComponentStatus comp = validate_launcher_config(cfg);

            auto open_file_picker = [&](PickerTarget t, const char *filter_name, const char *pattern, const std::string &current) {
                std::lock_guard<std::mutex> lk(picker->mu);
                if (picker->pending) return;
                picker->pending = true;
                picker->target = t;
                picker->chosen_path.clear();
                picker->error.clear();
                SDL_ClearError();
                const SDL_DialogFileFilter filters[] = {{filter_name, pattern}};
                const char *def_loc = current.empty() ? nullptr : current.c_str();
                auto *ctx = new DialogCallbackCtx{picker};
                SDL_ShowOpenFileDialog(on_dialog_done, ctx, window, filters, 1, def_loc, false);
            };

            auto open_folder_picker = [&](PickerTarget t, const std::string &current) {
                std::lock_guard<std::mutex> lk(picker->mu);
                if (picker->pending) return;
                picker->pending = true;
                picker->target = t;
                picker->chosen_path.clear();
                picker->error.clear();
                SDL_ClearError();
                const char *def_loc = current.empty() ? nullptr : current.c_str();
                auto *ctx = new DialogCallbackCtx{picker};
                SDL_ShowOpenFolderDialog(on_dialog_done, ctx, window, def_loc, false);
            };

            ImGui::BeginChild("##ComponentsBox", ImVec2(0.0f, 286.0f), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Required Components");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 105.0f);
            if (ImGui::Button("Auto-Detect", ImVec2(110.0f, 0.0f))) {
                auto_detect_launcher_paths(cfg, exe_dir);
                sync_buffers_from_cfg();
                std::string save_err;
                save_launcher_config(cfg_path, cfg, save_err);
                const ComponentStatus after = validate_launcher_config(cfg);
                status_msg = after.all_ok() ? "All required components detected." : "Auto-detect finished; some items still need selection.";
                status_is_error = !after.all_ok();
            }
            ImGui::Separator();

            auto draw_path_row = [&](const char *id, const char *label, bool ok, std::array<char, 1024> &buf, std::string &field, auto on_browse) {
                ImGui::PushID(id);
                if (ok) {
                    ImGui::TextColored(ImVec4(0.32f, 0.85f, 0.48f, 1.0f), "[OK]");
                } else {
                    ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.36f, 1.0f), "[Missing]");
                }
                ImGui::SameLine(86.0f);
                ImGui::TextUnformatted(label);
                ImGui::SameLine(205.0f);
                ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 98.0f));
                if (ImGui::InputText("##path", buf.data(), buf.size())) {
                    field = buf.data();
                }
                ImGui::SameLine();
                if (ImGui::Button("Browse...", ImVec2(88.0f, 0.0f))) {
                    on_browse();
                }
                ImGui::PopID();
            };

            draw_path_row("main_elf", "main.elf", comp.main_elf_ok, main_elf_buf, cfg.main_elf,
                          [&]() { open_file_picker(PickerTarget::MainElf, "ELF Files", "elf", cfg.main_elf); });
            draw_path_row("sdk_elf", "sdk.elf", comp.sdk_elf_ok, sdk_elf_buf, cfg.sdk_elf,
                          [&]() { open_file_picker(PickerTarget::SdkElf, "ELF Files", "elf", cfg.sdk_elf); });
            draw_path_row("main_lib", "libsnap_main.a", comp.main_lib_ok, main_lib_buf, cfg.main_lib,
                          [&]() { open_file_picker(PickerTarget::MainLib, "Static Libraries", "a", cfg.main_lib); });
            draw_path_row("sdk_lib", "libsnap_sdk.a", comp.sdk_lib_ok, sdk_lib_buf, cfg.sdk_lib,
                          [&]() { open_file_picker(PickerTarget::SdkLib, "Static Libraries", "a", cfg.sdk_lib); });
            draw_path_row("romfs", "ROMFS Folder", comp.romfs_ok, romfs_buf, cfg.romfs,
                          [&]() { open_folder_picker(PickerTarget::Romfs, cfg.romfs); });
            draw_path_row("data_dir", "Save / Data Dir", !cfg.data_dir.empty(), data_dir_buf, cfg.data_dir,
                          [&]() { open_folder_picker(PickerTarget::DataDir, cfg.data_dir); });

            ImGui::EndChild();
            ImGui::Spacing();

            // Video Settings
            ImGui::BeginChild("##VideoBox", ImVec2(0.0f, 118.0f), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Video Settings");
            ImGui::Separator();

            const char *modes[] = {"Fullscreen", "Windowed", "Borderless"};
            const char *mode_keys[] = {"fullscreen", "windowed", "borderless"};
            int mode_idx = 0;
            for (int i = 0; i < 3; ++i) {
                if (cfg.window_mode == mode_keys[i]) mode_idx = i;
            }
            ImGui::TextUnformatted("Window Mode");
            ImGui::SameLine(145.0f);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("##WinMode", &mode_idx, modes, 3)) {
                cfg.window_mode = mode_keys[mode_idx];
            }

            int res_idx = match_res_preset(cfg.width, cfg.height);
            const char *res_labels[] = {
                kResPresets[0].label, kResPresets[1].label, kResPresets[2].label,
                kResPresets[3].label, kResPresets[4].label, kResPresets[5].label,
                kResPresets[6].label,
            };
            ImGui::TextUnformatted("Resolution");
            ImGui::SameLine(145.0f);
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("##ResPreset", &res_idx, res_labels, 7)) {
                if (res_idx < 6) {
                    cfg.width = kResPresets[res_idx].w;
                    cfg.height = kResPresets[res_idx].h;
                }
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(76.0f);
            ImGui::InputInt("##W", &cfg.width, 0, 0);
            ImGui::SameLine();
            ImGui::TextUnformatted("x");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(76.0f);
            ImGui::InputInt("##H", &cfg.height, 0, 0);
            cfg.width = std::clamp(cfg.width, 320, 16384);
            cfg.height = std::clamp(cfg.height, 240, 16384);

            ImGui::EndChild();
            ImGui::Spacing();

            // Status / Error Banner
            if (!status_msg.empty()) {
                const ImVec4 col = status_is_error ? ImVec4(0.95f, 0.45f, 0.38f, 1.0f) : ImVec4(0.45f, 0.85f, 0.58f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextWrapped("%s", status_msg.c_str());
                ImGui::PopStyleColor();
            } else if (!comp.all_ok()) {
                std::string missing_err;
                validate_launcher_config(cfg, &missing_err);
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.30f, 1.0f), "%s", missing_err.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.58f, 1.0f), "Ready to launch.");
            }

            // Bottom Action Bar
            ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 6.0f, ImGui::GetWindowHeight() - 56.0f));
            if (ImGui::Button("Save Config", ImVec2(120.0f, 34.0f))) {
                std::string save_err;
                if (save_launcher_config(cfg_path, cfg, save_err)) {
                    status_msg = "Configuration saved to " + cfg_path;
                    status_is_error = false;
                } else {
                    status_msg = save_err;
                    status_is_error = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Add to KDE Menu", ImVec2(150.0f, 34.0f))) {
                const char *appimage_env = std::getenv("APPIMAGE");
                const std::string target_exec = (appimage_env && *appimage_env) ? std::string(appimage_env) : self_exe;
                const std::string icon_src = resolve_bundled_icon_path(self_exe);
                std::string desk_msg;
                const bool ok = install_desktop_integration(target_exec, icon_src, desk_msg);
                status_msg = desk_msg;
                status_is_error = !ok;
            }

            ImGui::SameLine(ImGui::GetWindowWidth() - 165.0f);
            ImGui::BeginDisabled(!comp.all_ok());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.52f, 0.82f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.60f, 0.90f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.44f, 0.72f, 1.00f));
            if (ImGui::Button("Play", ImVec2(145.0f, 34.0f))) {
                std::string val_err;
                const ComponentStatus st = validate_launcher_config(cfg, &val_err);
                if (!st.all_ok()) {
                    status_msg = val_err;
                    status_is_error = true;
                } else {
                    std::string save_err;
                    save_launcher_config(cfg_path, cfg, save_err);
                    launch_requested = true;
                    running = false;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            if (!comp.all_ok() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Select all required ELF/library files and the ROMFS folder before launching.");
            }

            ImGui::End();
            ImGui::Render();

            SDL_SetRenderDrawColor(renderer, 28, 31, 38, 255);
            SDL_RenderClear(renderer);
            render_imgui_to_sdl(renderer, ImGui::GetDrawData());
            SDL_RenderPresent(renderer);
        }

        SDL_StopTextInput(window);
        SDL_DestroyTexture(font_tex);
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();

        if (!launch_requested) {
            return 0;
        }

        std::string launch_err;
        const int game_rc = spawn_and_wait_game(self_exe, cfg, launch_err);
        if (game_rc == 0) {
            return 0;
        }
        status_msg = launch_err.empty() ? "Game exited with an error." : launch_err;
        status_is_error = true;
    }
}

} // namespace snap_linux
