// SPDX-License-Identifier: MIT
#include "launcher_config_linux.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace snap_linux {

namespace {

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string env_str(const char *name) {
    const char *v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

std::string sanitize_window_mode(const std::string &mode) {
    if (mode == "windowed" || mode == "borderless" || mode == "fullscreen") {
        return mode;
    }
    return "fullscreen";
}

std::string first_existing_file(const std::vector<fs::path> &candidates) {
    std::error_code ec;
    for (const fs::path &cand : candidates) {
        if (cand.empty()) continue;
        if (fs::exists(cand, ec) && !ec && fs::is_regular_file(cand, ec) && !ec &&
            ::access(cand.c_str(), R_OK) == 0) {
            const fs::path abs = fs::weakly_canonical(cand, ec);
            return (!ec && !abs.empty()) ? abs.string() : cand.string();
        }
    }
    return {};
}

std::string first_existing_dir(const std::vector<fs::path> &candidates) {
    std::error_code ec;
    for (const fs::path &cand : candidates) {
        if (cand.empty()) continue;
        if (fs::exists(cand, ec) && !ec && fs::is_directory(cand, ec) && !ec &&
            ::access(cand.c_str(), R_OK | X_OK) == 0) {
            const fs::path abs = fs::weakly_canonical(cand, ec);
            return (!ec && !abs.empty()) ? abs.string() : cand.string();
        }
    }
    return {};
}

std::vector<fs::path> collect_search_roots(const std::string &exe_dir) {
    std::vector<fs::path> roots;
    auto push_unique = [&](const fs::path &p) {
        if (p.empty()) return;
        std::error_code ec;
        fs::path norm = fs::weakly_canonical(p, ec);
        if (ec || norm.empty()) norm = p.lexically_normal();
        if (std::find(roots.begin(), roots.end(), norm) == roots.end()) {
            roots.push_back(norm);
        }
    };

    const std::string appimage = env_str("APPIMAGE");
    if (!appimage.empty()) {
        const fs::path app_dir = fs::u8path(appimage).parent_path();
        push_unique(app_dir);
        push_unique(app_dir.parent_path());
    }
    if (!exe_dir.empty()) {
        const fs::path ed = fs::u8path(exe_dir);
        push_unique(ed);
        push_unique(ed.parent_path());
        push_unique(ed.parent_path().parent_path());
    }
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (!ec && !cwd.empty()) {
        push_unique(cwd);
        push_unique(cwd.parent_path());
    }

    const std::string home = env_str("HOME");
    if (!home.empty()) {
        const fs::path hp = fs::u8path(home);
        push_unique(hp / "Games" / "Pokemon Snap");
        push_unique(hp / "Games" / "New Pokemon Snap");
        push_unique(hp / "Games" / "New Pokémon Snap");
        push_unique(hp / "Games");
        push_unique(hp / "ROMs");
        push_unique(hp / "Projects" / "snap-recomp");
        push_unique(hp / "Projects" / "new-poke-snap-switch-recomp-linux");
        push_unique(hp);
    }

    const std::string xdg_data = env_str("XDG_DATA_HOME");
    if (!xdg_data.empty()) {
        push_unique(fs::u8path(xdg_data) / "SnapRecomp");
    } else if (!home.empty()) {
        push_unique(fs::u8path(home) / ".local" / "share" / "SnapRecomp");
    }
    return roots;
}

} // namespace

std::string launcher_config_path() {
    std::string cfg_home = env_str("XDG_CONFIG_HOME");
    if (cfg_home.empty()) {
        const std::string home = env_str("HOME");
        if (home.empty()) return {};
        cfg_home = (fs::u8path(home) / ".config").string();
    }
    return (fs::u8path(cfg_home) / "new-pokemon-snap-recomp" / "config").string();
}

std::string default_data_dir() {
    std::string data_home = env_str("XDG_DATA_HOME");
    if (data_home.empty()) {
        const std::string home = env_str("HOME");
        if (home.empty()) return {};
        data_home = (fs::u8path(home) / ".local" / "share").string();
    }
    return (fs::u8path(data_home) / "SnapRecomp").string();
}

bool load_launcher_config(const std::string &path, LauncherConfig &cfg, std::string &err) {
    err.clear();
    if (path.empty()) return true;

    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (!fs::exists(p, ec)) {
        return true; // First run without config file is expected.
    }
    std::ifstream in(p);
    if (!in.is_open()) {
        err = "Could not open configuration file: " + path;
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim_copy(trimmed.substr(0, eq));
        const std::string v = trim_copy(trimmed.substr(eq + 1));

        if (k == "main_elf") cfg.main_elf = v;
        else if (k == "sdk_elf") cfg.sdk_elf = v;
        else if (k == "main_lib") cfg.main_lib = v;
        else if (k == "sdk_lib") cfg.sdk_lib = v;
        else if (k == "romfs") cfg.romfs = v;
        else if (k == "data_dir") cfg.data_dir = v;
        else if (k == "window_mode") cfg.window_mode = sanitize_window_mode(v);
        else if (k == "width") {
            const int w = std::atoi(v.c_str());
            if (w >= 320 && w <= 16384) cfg.width = w;
        } else if (k == "height") {
            const int h = std::atoi(v.c_str());
            if (h >= 240 && h <= 16384) cfg.height = h;
        }
    }
    return true;
}

bool save_launcher_config(const std::string &path, const LauncherConfig &cfg, std::string &err) {
    err.clear();
    if (path.empty()) {
        err = "Cannot save configuration: HOME and XDG_CONFIG_HOME are unset.";
        return false;
    }

    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (!p.parent_path().empty()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            err = "Failed to create config directory '" + p.parent_path().string() + "': " + ec.message();
            return false;
        }
    }

    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) {
            err = "Failed to write configuration file '" + path + "'.";
            return false;
        }
        out << "# New Pokemon Snap Recomp Linux Launcher Configuration\n";
        out << "main_elf=" << cfg.main_elf << "\n";
        out << "sdk_elf=" << cfg.sdk_elf << "\n";
        out << "main_lib=" << cfg.main_lib << "\n";
        out << "sdk_lib=" << cfg.sdk_lib << "\n";
        out << "romfs=" << cfg.romfs << "\n";
        out << "data_dir=" << cfg.data_dir << "\n";
        out << "window_mode=" << sanitize_window_mode(cfg.window_mode) << "\n";
        out << "width=" << cfg.width << "\n";
        out << "height=" << cfg.height << "\n";
        out.flush();
        if (!out.good()) {
            err = "Failed while flushing configuration file '" + path + "'.";
            return false;
        }
    }

    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        err = "Failed to finalize configuration file '" + path + "'.";
        return false;
    }
    return true;
}

bool is_valid_regular_file(const std::string &path) {
    if (path.empty()) return false;
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    return fs::exists(p, ec) && !ec && fs::is_regular_file(p, ec) && !ec &&
           ::access(p.c_str(), R_OK) == 0;
}

bool is_valid_romfs_dir(const std::string &path) {
    if (path.empty()) return false;
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    return fs::exists(p, ec) && !ec && fs::is_directory(p, ec) && !ec &&
           ::access(p.c_str(), R_OK | X_OK) == 0;
}

ComponentStatus validate_launcher_config(const LauncherConfig &cfg, std::string *summary_err) {
    ComponentStatus st;
    st.main_elf_ok = is_valid_regular_file(cfg.main_elf);
    st.sdk_elf_ok = is_valid_regular_file(cfg.sdk_elf);
    st.main_lib_ok = is_valid_regular_file(cfg.main_lib);
    st.sdk_lib_ok = is_valid_regular_file(cfg.sdk_lib);
    st.romfs_ok = is_valid_romfs_dir(cfg.romfs);

    if (summary_err) {
        summary_err->clear();
        if (!st.all_ok()) {
            std::vector<std::string> missing;
            if (!st.main_elf_ok) missing.push_back("main.elf");
            if (!st.sdk_elf_ok) missing.push_back("sdk.elf");
            if (!st.main_lib_ok) missing.push_back("libsnap_main.a");
            if (!st.sdk_lib_ok) missing.push_back("libsnap_sdk.a");
            if (!st.romfs_ok) missing.push_back("ROMFS folder");
            std::ostringstream oss;
            oss << "Missing or inaccessible required component(s): ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << missing[i];
            }
            oss << ".";
            *summary_err = oss.str();
        }
    }
    return st;
}

void auto_detect_launcher_paths(LauncherConfig &cfg, const std::string &exe_dir) {
    const std::vector<fs::path> roots = collect_search_roots(exe_dir);

    if (!is_valid_regular_file(cfg.main_elf)) {
        std::vector<fs::path> cands;
        const std::string env_v = env_str("SNAP_MAIN_ELF");
        if (!env_v.empty()) cands.push_back(fs::u8path(env_v));
        for (const fs::path &r : roots) {
            cands.push_back(r / "main.elf");
            cands.push_back(r / "elf" / "main.elf");
            cands.push_back(r / "build" / "linux-aot" / "elf" / "main.elf");
            cands.push_back(r / "linux-aot" / "elf" / "main.elf");
            cands.push_back(r / "analysis" / "import" / "v262144-main.elf");
        }
        cfg.main_elf = first_existing_file(cands);
    }

    if (!is_valid_regular_file(cfg.sdk_elf)) {
        std::vector<fs::path> cands;
        const std::string env_v = env_str("SNAP_SDK_ELF");
        if (!env_v.empty()) cands.push_back(fs::u8path(env_v));
        for (const fs::path &r : roots) {
            cands.push_back(r / "sdk.elf");
            cands.push_back(r / "elf" / "sdk.elf");
            cands.push_back(r / "build" / "linux-aot" / "elf" / "sdk.elf");
            cands.push_back(r / "linux-aot" / "elf" / "sdk.elf");
            cands.push_back(r / "analysis" / "import" / "v262144-sdk.elf");
        }
        cfg.sdk_elf = first_existing_file(cands);
    }

    if (!is_valid_regular_file(cfg.main_lib)) {
        std::vector<fs::path> cands;
        const std::string env_v = env_str("SNAP_MAIN_LIB");
        if (!env_v.empty()) cands.push_back(fs::u8path(env_v));
        for (const fs::path &r : roots) {
            cands.push_back(r / "libsnap_main.a");
            cands.push_back(r / "lib" / "libsnap_main.a");
            cands.push_back(r / "build" / "linux-aot-abi3" / "main" / "lib" / "libsnap_main.a");
            cands.push_back(r / "linux-aot-abi3" / "main" / "lib" / "libsnap_main.a");
            cands.push_back(r / "build" / "aot" / "main" / "lib" / "libsnap_main.a");
        }
        cfg.main_lib = first_existing_file(cands);
    }

    if (!is_valid_regular_file(cfg.sdk_lib)) {
        std::vector<fs::path> cands;
        const std::string env_v = env_str("SNAP_SDK_LIB");
        if (!env_v.empty()) cands.push_back(fs::u8path(env_v));
        for (const fs::path &r : roots) {
            cands.push_back(r / "libsnap_sdk.a");
            cands.push_back(r / "lib" / "libsnap_sdk.a");
            cands.push_back(r / "build" / "linux-aot-abi3" / "sdk" / "lib" / "libsnap_sdk.a");
            cands.push_back(r / "linux-aot-abi3" / "sdk" / "lib" / "libsnap_sdk.a");
            cands.push_back(r / "build" / "aot" / "sdk" / "lib" / "libsnap_sdk.a");
        }
        cfg.sdk_lib = first_existing_file(cands);
    }

    if (!is_valid_romfs_dir(cfg.romfs)) {
        std::vector<fs::path> cands;
        const std::string env_v = env_str("SNAP_ROMFS");
        if (!env_v.empty()) cands.push_back(fs::u8path(env_v));
        for (const fs::path &r : roots) {
            cands.push_back(r / "romfs");
            cands.push_back(r / "extracted" / "romfs-v262144");
            cands.push_back(r / "Pokemon Snap" / "romfs");
            cands.push_back(r / "New Pokemon Snap" / "romfs");
        }
        cfg.romfs = first_existing_dir(cands);
    }

    if (cfg.data_dir.empty()) {
        const std::string env_dd = env_str("SNAP_DATA_DIR");
        cfg.data_dir = !env_dd.empty() ? env_dd : default_data_dir();
    }
}

std::vector<std::string> build_launch_cli_args(const std::string &exe_path, const LauncherConfig &cfg) {
    const std::string mode = sanitize_window_mode(cfg.window_mode);
    const int w = (cfg.width >= 320 && cfg.width <= 16384) ? cfg.width : 1920;
    const int h = (cfg.height >= 240 && cfg.height <= 16384) ? cfg.height : 1080;

    std::vector<std::string> args;
    args.push_back(exe_path);
    args.push_back("--main");
    args.push_back(cfg.main_elf);
    args.push_back("--sdk");
    args.push_back(cfg.sdk_elf);
    args.push_back("--main-lib");
    args.push_back(cfg.main_lib);
    args.push_back("--sdk-lib");
    args.push_back(cfg.sdk_lib);
    args.push_back("--romfs");
    args.push_back(cfg.romfs);
    if (!cfg.data_dir.empty() && cfg.data_dir != default_data_dir()) {
        args.push_back("--data-dir");
        args.push_back(cfg.data_dir);
    }
    args.push_back("--" + mode);
    args.push_back("--width");
    args.push_back(std::to_string(w));
    args.push_back("--height");
    args.push_back(std::to_string(h));
    args.push_back("--play");
    args.push_back("--allow-guest-exec");
    return args;
}

bool install_desktop_integration(const std::string &exec_path, const std::string &icon_src_path, std::string &msg_out) {
    msg_out.clear();
    if (exec_path.empty()) {
        msg_out = "Could not determine executable or AppImage path for desktop integration.";
        return false;
    }

    std::string data_home = env_str("XDG_DATA_HOME");
    if (data_home.empty()) {
        const std::string home = env_str("HOME");
        if (home.empty()) {
            msg_out = "HOME and XDG_DATA_HOME are unset; cannot install desktop entry.";
            return false;
        }
        data_home = (fs::u8path(home) / ".local" / "share").string();
    }

    std::error_code ec;
    const fs::path apps_dir = fs::u8path(data_home) / "applications";
    const fs::path icons_dir = fs::u8path(data_home) / "icons" / "hicolor" / "256x256" / "apps";
    fs::create_directories(apps_dir, ec);
    if (ec) {
        msg_out = "Failed to create applications directory: " + ec.message();
        return false;
    }

    std::string icon_ref = "new-pokemon-snap-recomp";
    if (!icon_src_path.empty() && fs::is_regular_file(fs::u8path(icon_src_path), ec) && !ec) {
        fs::create_directories(icons_dir, ec);
        if (!ec) {
            const fs::path icon_dst = icons_dir / "new-pokemon-snap-recomp.png";
            fs::copy_file(fs::u8path(icon_src_path), icon_dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) icon_ref = icon_dst.string();
        }
    }

    const fs::path desktop_file = apps_dir / "new-pokemon-snap-recomp.desktop";
    std::ofstream out(desktop_file, std::ios::trunc);
    if (!out.is_open()) {
        msg_out = "Failed to write desktop entry: " + desktop_file.string();
        return false;
    }

    std::string escaped_exec;
    escaped_exec.reserve(exec_path.size() + 8);
    for (char ch : exec_path) {
        if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') {
            escaped_exec.push_back('\\');
            escaped_exec.push_back(ch);
        } else if (ch == '%') {
            escaped_exec += "%%";
        } else {
            escaped_exec.push_back(ch);
        }
    }

    out << "[Desktop Entry]\n";
    out << "Type=Application\n";
    out << "Name=New Pokémon Snap Recomp\n";
    out << "Comment=Unofficial Linux port of New Pokémon Snap Recomp\n";
    out << "Exec=\"" << escaped_exec << "\"\n";
    out << "Icon=" << icon_ref << "\n";
    out << "Categories=Game;\n";
    out << "StartupWMClass=snap_boot_linux\n";
    out << "Terminal=false\n";
    out.flush();
    if (!out.good()) {
        msg_out = "Failed while writing desktop entry: " + desktop_file.string();
        return false;
    }

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execlp("update-desktop-database", "update-desktop-database", apps_dir.c_str(), nullptr);
        _exit(0);
    } else if (pid > 0) {
        int st = 0;
        ::waitpid(pid, &st, 0);
    }

    msg_out = "Desktop entry installed at " + desktop_file.string();
    return true;
}

} // namespace snap_linux
