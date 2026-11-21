// SPDX-License-Identifier: MIT
// Persistent XDG configuration, auto-detection, validation, and desktop integration
// for the Linux GUI launcher.
#pragma once

#include <string>
#include <vector>

namespace snap_linux {

struct LauncherConfig {
    std::string main_elf;
    std::string sdk_elf;
    std::string main_lib;
    std::string sdk_lib;
    std::string romfs;
    std::string data_dir;
    std::string window_mode = "fullscreen"; // fullscreen | windowed | borderless
    int width = 1920;
    int height = 1080;
};

struct ComponentStatus {
    bool main_elf_ok = false;
    bool sdk_elf_ok = false;
    bool main_lib_ok = false;
    bool sdk_lib_ok = false;
    bool romfs_ok = false;

    bool all_ok() const {
        return main_elf_ok && sdk_elf_ok && main_lib_ok && sdk_lib_ok && romfs_ok;
    }
};

// Returns $XDG_CONFIG_HOME/new-pokemon-snap-recomp/config (fallback $HOME/.config/...).
std::string launcher_config_path();

// Returns default shared data root: $XDG_DATA_HOME/SnapRecomp
// (fallback $HOME/.local/share/SnapRecomp).
std::string default_data_dir();

// Loads key=value config if it exists; missing file is not an error (returns true with defaults).
bool load_launcher_config(const std::string &path, LauncherConfig &cfg, std::string &err);

// Saves key=value config atomically, creating parent directories as needed.
bool save_launcher_config(const std::string &path, const LauncherConfig &cfg, std::string &err);

// Component checks (non-mutating).
bool is_valid_regular_file(const std::string &path);
bool is_valid_romfs_dir(const std::string &path);
ComponentStatus validate_launcher_config(const LauncherConfig &cfg, std::string *summary_err = nullptr);

// Populates empty or invalid fields by scanning env vars, exe/AppImage dir, CWD, and common $HOME dirs.
void auto_detect_launcher_paths(LauncherConfig &cfg, const std::string &exe_dir = "");

// Builds argv vector for invoking snap_boot_linux in CLI `run` mode.
std::vector<std::string> build_launch_cli_args(const std::string &exe_path, const LauncherConfig &cfg);

// Installs a user-level .desktop entry (and optional icon) under $XDG_DATA_HOME.
bool install_desktop_integration(const std::string &exec_path, const std::string &icon_src_path, std::string &msg_out);

} // namespace snap_linux
