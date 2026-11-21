// SPDX-License-Identifier: MIT
#include "linux/launcher_config_linux.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_checks{0};
std::atomic<int> g_fail{0};

#define CHECK(expr)                                                                 \
    do {                                                                            \
        g_checks.fetch_add(1);                                                      \
        if (!(expr)) {                                                              \
            g_fail.fetch_add(1);                                                    \
            std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        }                                                                           \
    } while (0)

void write_dummy_file(const fs::path &p) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << "dummy\n";
}

std::string save_env(const char *k) {
    const char *v = std::getenv(k);
    return v ? std::string(v) : std::string();
}

void restore_env(const char *k, const std::string &v) {
    if (v.empty()) unsetenv(k);
    else setenv(k, v.c_str(), 1);
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "snap_launcher_cfg_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    const std::string prev_xdg_cfg = save_env("XDG_CONFIG_HOME");
    const std::string prev_xdg_data = save_env("XDG_DATA_HOME");
    const std::string prev_home = save_env("HOME");
    const std::string prev_romfs = save_env("SNAP_ROMFS");

    // 1. XDG config path resolution
    {
        setenv("XDG_CONFIG_HOME", (base / "xdg_cfg").string().c_str(), 1);
        CHECK(snap_linux::launcher_config_path() == (base / "xdg_cfg" / "new-pokemon-snap-recomp" / "config").string());

        unsetenv("XDG_CONFIG_HOME");
        setenv("HOME", (base / "home_dir").string().c_str(), 1);
        CHECK(snap_linux::launcher_config_path() == (base / "home_dir" / ".config" / "new-pokemon-snap-recomp" / "config").string());
    }

    // 2. Save + Load persistence (with spaces)
    {
        const fs::path cfg_file = base / "xdg_cfg" / "new-pokemon-snap-recomp" / "config";
        snap_linux::LauncherConfig wcfg;
        wcfg.main_elf = (base / "files with spaces" / "main.elf").string();
        wcfg.sdk_elf = (base / "files with spaces" / "sdk.elf").string();
        wcfg.main_lib = (base / "files with spaces" / "libsnap_main.a").string();
        wcfg.sdk_lib = (base / "files with spaces" / "libsnap_sdk.a").string();
        wcfg.romfs = (base / "files with spaces" / "romfs").string();
        wcfg.data_dir = (base / "files with spaces" / "profiles" / "p1").string();
        wcfg.window_mode = "windowed";
        wcfg.width = 2560;
        wcfg.height = 1440;

        std::string err;
        CHECK(snap_linux::save_launcher_config(cfg_file.string(), wcfg, err));
        CHECK(err.empty());

        snap_linux::LauncherConfig rcfg;
        CHECK(snap_linux::load_launcher_config(cfg_file.string(), rcfg, err));
        CHECK(err.empty());
        CHECK(rcfg.main_elf == wcfg.main_elf);
        CHECK(rcfg.sdk_elf == wcfg.sdk_elf);
        CHECK(rcfg.main_lib == wcfg.main_lib);
        CHECK(rcfg.sdk_lib == wcfg.sdk_lib);
        CHECK(rcfg.romfs == wcfg.romfs);
        CHECK(rcfg.data_dir == wcfg.data_dir);
        CHECK(rcfg.window_mode == "windowed");
        CHECK(rcfg.width == 2560);
        CHECK(rcfg.height == 1440);
    }

    // 3. Validation (missing ELF/lib/ROMFS vs valid)
    {
        snap_linux::LauncherConfig cfg;
        std::string summary;
        auto st = snap_linux::validate_launcher_config(cfg, &summary);
        CHECK(!st.all_ok());
        CHECK(summary.find("main.elf") != std::string::npos);
        CHECK(summary.find("ROMFS") != std::string::npos);

        const fs::path root = base / "valid_tree";
        const fs::path f_main = root / "build" / "linux-aot" / "elf" / "main.elf";
        const fs::path f_sdk = root / "build" / "linux-aot" / "elf" / "sdk.elf";
        const fs::path f_mlib = root / "build" / "linux-aot-abi3" / "main" / "lib" / "libsnap_main.a";
        const fs::path f_slib = root / "build" / "linux-aot-abi3" / "sdk" / "lib" / "libsnap_sdk.a";
        const fs::path d_romfs = base / "fake_home" / "Games" / "Pokemon Snap" / "romfs";
        write_dummy_file(f_main);
        write_dummy_file(f_sdk);
        write_dummy_file(f_mlib);
        write_dummy_file(f_slib);
        fs::create_directories(d_romfs, ec);

        // Passing a file as ROMFS must fail directory validation
        cfg.main_elf = f_main.string();
        cfg.sdk_elf = f_sdk.string();
        cfg.main_lib = f_mlib.string();
        cfg.sdk_lib = f_slib.string();
        cfg.romfs = f_main.string();
        st = snap_linux::validate_launcher_config(cfg, &summary);
        CHECK(!st.romfs_ok);
        CHECK(!st.all_ok());

        cfg.romfs = d_romfs.string();
        st = snap_linux::validate_launcher_config(cfg, &summary);
        CHECK(st.all_ok());
        CHECK(summary.empty());
    }

    // 4. Auto-detection from exe_dir + $HOME
    {
        unsetenv("SNAP_ROMFS");
        setenv("HOME", (base / "fake_home").string().c_str(), 1);
        snap_linux::LauncherConfig detected;
        snap_linux::auto_detect_launcher_paths(detected, (base / "valid_tree" / "dist").string());
        const auto st = snap_linux::validate_launcher_config(detected);
        CHECK(st.main_elf_ok);
        CHECK(st.sdk_elf_ok);
        CHECK(st.main_lib_ok);
        CHECK(st.sdk_lib_ok);
        CHECK(st.romfs_ok);
        CHECK(detected.data_dir == (base / "fake_home" / ".local" / "share" / "SnapRecomp").string());

        // Default data_dir must NOT emit --data-dir (so activate_profiles() selects profiles/p1).
        detected.window_mode = "borderless";
        detected.width = 3440;
        detected.height = 1440;
        const auto args = snap_linux::build_launch_cli_args("/tmp/snap_boot_linux", detected);
        CHECK(!args.empty() && args.front() == "/tmp/snap_boot_linux");
        CHECK(args.back() == "--allow-guest-exec");
        bool saw_romfs = false, saw_mode = false, saw_data_dir = false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--romfs" && i + 1 < args.size() && args[i + 1] == detected.romfs) saw_romfs = true;
            if (args[i] == "--data-dir") saw_data_dir = true;
            if (args[i] == "--borderless") saw_mode = true;
        }
        CHECK(saw_romfs);
        CHECK(!saw_data_dir);
        CHECK(saw_mode);

        // Custom data_dir MUST emit --data-dir explicitly.
        detected.data_dir = (base / "custom_data").string();
        const auto custom_args = snap_linux::build_launch_cli_args("/tmp/snap_boot_linux", detected);
        bool saw_custom_dd = false;
        for (size_t i = 0; i + 1 < custom_args.size(); ++i) {
            if (custom_args[i] == "--data-dir" && custom_args[i + 1] == detected.data_dir) saw_custom_dd = true;
        }
        CHECK(saw_custom_dd);
    }

    // 5. Desktop integration in isolated XDG_DATA_HOME (spaces + no duplicates)
    {
        setenv("XDG_DATA_HOME", (base / "xdg_data").string().c_str(), 1);
        const fs::path icon = base / "icon.png";
        write_dummy_file(icon);
        std::string msg;
        const fs::path appimage_space = base / "My Apps" / "New Pokemon Snap 100%.AppImage";
        CHECK(snap_linux::install_desktop_integration(appimage_space.string(), icon.string(), msg));
        // Second call must overwrite cleanly without creating duplicates
        CHECK(snap_linux::install_desktop_integration(appimage_space.string(), icon.string(), msg));

        const fs::path desktop_file = base / "xdg_data" / "applications" / "new-pokemon-snap-recomp.desktop";
        CHECK(fs::exists(desktop_file));
        CHECK(fs::exists(base / "xdg_data" / "icons" / "hicolor" / "256x256" / "apps" / "new-pokemon-snap-recomp.png"));

        int desktop_count = 0;
        for (const auto &entry : fs::directory_iterator(base / "xdg_data" / "applications")) {
            if (entry.path().extension() == ".desktop") ++desktop_count;
        }
        CHECK(desktop_count == 1);

        std::ifstream df(desktop_file);
        std::string content((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
        CHECK(content.find("100%%.AppImage") != std::string::npos);
        CHECK(content.find("StartupWMClass=snap_boot_linux") != std::string::npos);
    }

    restore_env("XDG_CONFIG_HOME", prev_xdg_cfg);
    restore_env("XDG_DATA_HOME", prev_xdg_data);
    restore_env("HOME", prev_home);
    restore_env("SNAP_ROMFS", prev_romfs);
    fs::remove_all(base, ec);

    std::printf("launcher_config_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
