// SPDX-License-Identifier: GPL-3.0-or-later
// Player command-line flags of the game exe (docs/command-line.md). Header only and free of Windows and runtime includes, so
// tests/hle/cli_flags_test.cpp checks it on its own. host/boot.cpp main() calls parse() first, applies the result, and hands `rest`
// to the older handlers (save import/export, benchmark, shader preparation, the developer boot commands).
//
//   --play                        skip the launcher window and start the game; the process ends when the game closes
//   --safe-mode                   docs/safe-mode.md
//   --fullscreen | --borderless   borderless full-screen window on the chosen monitor (the game has no exclusive mode)
//   --windowed                    a normal window
//   --monitor <n>  --width <px>  --height <px>
//   --steam-deck                  the Steam Deck preset (host/steamdeck.h, docs/steam-deck.md): borderless 1280 x 800, handheld picture settings,
//                                 bigger HUD, Xbox button icons. Flags after it win (--set hud_scale=200, --width ...).
//   --profile <name or id>        the profile to play (also the profile of --import-save / --export-save)
//   --set <key>=<value>           one settings key for this run only (repeatable); a name that starts with SNAP_ sets that variable
//   --romfs <folder>              extracted RomFS folder (sets SNAP_ROMFS; overrides env and extracted/romfs-v262144 fallback)
//   --data-dir <folder>           one folder for saves, settings, mods, caches and logs
//   --update | --check-install | --report | --restore-backup | --upscalers | --hdtex
//                                 one launcher action without the launcher window (host/launcher.cpp run_action); the game does not start.
//                                 No action opens a dialog. --restore-backup, --upscalers and --hdtex take everything after them as their
//                                 own arguments (action_takes_args), so the other player flags go before them.
//   --help | -h | /?   --version
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "steamdeck.h"

namespace snap_cli {

struct Parsed {
  bool help = false, version = false, play = false, safe = false;
  std::string data_dir, profile, romfs;
  std::string action;                                      // one launcher action (action_names) without its "--", or ""
  std::vector<std::string> action_args;                    // what followed an action that takes arguments (action_takes_args)
  std::vector<std::pair<std::string, std::string>> env;    // variables to set for this run (window flags, --set SNAP_X=v)
  std::vector<std::pair<std::string, std::string>> sets;   // --set key=value, in order
  std::vector<std::string> rest;                           // everything else, in order, for the older handlers
  std::string error;                                       // not empty: print it and exit 2
};

inline std::string on_off(const std::string& s) {
  if (s == "on" || s == "1" || s == "true" || s == "yes") return "1";
  if (s == "off" || s == "0" || s == "false" || s == "no") return "0";
  return "";
}

inline bool is_number(const std::string& s) {
  if (s.empty() || s.size() > 6) return false;
  for (char c : s) if (c < '0' || c > '9') return false;
  return true;
}

// Launcher actions that have a flag of their own: "--" + name.
inline const std::vector<std::string>& action_names() {
  static const std::vector<std::string> k = {"update", "check-install", "report", "restore-backup", "upscalers", "hdtex"};
  return k;
}

// These actions own every argument after them: --restore-backup [latest | <n>], --upscalers [<name> --accept-license <name>],
// --hdtex [the builder's hdtex options]. Without arguments each one prints its state and changes nothing.
inline bool action_takes_args(const std::string& n) { return n == "restore-backup" || n == "upscalers" || n == "hdtex"; }

inline Parsed parse(const std::vector<std::string>& args) {
  Parsed p;
  auto value = [&](size_t& i, std::string* out) {
    if (i + 1 >= args.size()) { p.error = args[i] + " needs a value"; return false; }
    *out = args[++i];
    return true;
  };
  for (size_t i = 0; i < args.size() && p.error.empty(); i++) {
    const std::string& a = args[i];
    std::string v;
    if (a == "--help" || a == "-h" || a == "/?") p.help = true;
    else if (a == "--version") p.version = true;
    else if (a == "--play") p.play = true;
    else if (a == "--safe-mode") p.safe = true;
    else if (a == "--fullscreen" || a == "--borderless") p.env.push_back({"SNAP_WINDOW_MODE", "borderless"});
    else if (a == "--windowed") p.env.push_back({"SNAP_WINDOW_MODE", "windowed"});
    else if (a == "--steam-deck") {
      for (auto& kv : snapdeck::env()) p.env.push_back(kv);
      for (const snapdeck::Kv& kv : snapdeck::cfg()) p.sets.push_back({kv.key, kv.val});
    }
    else if (a == "--pause-in-background" || a == "--mute-in-background" || a == "--update-check") {
      if (!value(i, &v)) break;
      const std::string b = on_off(v);
      if (b.empty()) { p.error = a + " needs on or off, got \"" + v + "\""; break; }
      p.env.push_back({a == "--pause-in-background" ? "SNAP_WINDOW_PAUSE" : a == "--mute-in-background" ? "SNAP_WINDOW_MUTE_BG" : "SNAP_UPDATE_CHECK", b});
    } else if (a == "--frame-cap") {
      if (!value(i, &v)) break;
      if (!is_number(v)) { p.error = a + " needs a whole number of frames per second (0 = no extra limit), got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_WINDOW_FRAME_CAP", v});
    } else if (a == "--update-channel") {
      if (!value(i, &v)) break;
      if (v != "stable" && v != "testing") { p.error = a + " needs stable or testing, got \"" + v + "\""; break; }
      p.env.push_back({"SNAP_UPDATE_CHANNEL", v});
    }
    else if (a == "--monitor" || a == "--width" || a == "--height") {
      if (!value(i, &v)) break;
      if (!is_number(v)) { p.error = a + " needs a whole number, got \"" + v + "\""; break; }
      p.env.push_back({a == "--monitor" ? "SNAP_WINDOW_MONITOR" : a == "--width" ? "SNAP_WINDOW_W" : "SNAP_WINDOW_H", v});
    } else if (a == "--profile") {
      if (!value(i, &p.profile)) break;
    } else if (a == "--romfs") {
      if (!value(i, &p.romfs)) break;
      if (p.romfs.empty()) p.error = "--romfs needs a folder";
    } else if (a == "--data-dir") {
      if (!value(i, &p.data_dir)) break;
      if (p.data_dir.empty()) p.error = "--data-dir needs a folder";
    } else if (a == "--set") {
      if (!value(i, &v)) break;
      const size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) { p.error = "--set needs key=value, got \"" + v + "\""; break; }
      const std::string k = v.substr(0, eq), val = v.substr(eq + 1);
      if (k.compare(0, 5, "SNAP_") == 0) p.env.push_back({k, val});
      else p.sets.push_back({k, val});
    } else {
      bool act = false;
      if (a.compare(0, 2, "--") == 0) for (const std::string& n : action_names()) if (a.compare(2, std::string::npos, n) == 0) act = true;
      if (!act) { p.rest.push_back(a); continue; }
      if (!p.action.empty()) { p.error = "only one of --" + p.action + " and " + a + " at a time"; break; }
      p.action = a.substr(2);
      if (action_takes_args(p.action)) {
        p.action_args.assign(args.begin() + (std::ptrdiff_t)i + 1, args.end());
        break;
      }
    }
  }
  if (p.error.empty() && !p.action.empty() && p.play) p.error = "--" + p.action + " does not start the game; leave out --play";
  if (p.error.empty() && !p.action.empty() && !p.rest.empty()) p.error = "--" + p.action + " cannot be used with " + p.rest[0];
  return p;
}

// The first "--" option in `rest` that no handler knows, or "". The lists are the options host/boot.cpp, host/benchmark.cpp and
// host/shaderprep.cpp read; an option added there must be added here.
inline std::string first_unknown(const std::vector<std::string>& rest) {
  static const char* const kTakesValue[] = {"--import-save", "--export-save", "--benchmark-view", "--courses", "--main", "--sdk", "--probe",
                                            "--iters", "--trace", "--timeout", "--stub"};
  static const char* const kPlain[] = {"--benchmark", "--benchmark-capped", "--benchmark-uncapped", "--prepare-shaders", "--force", "--visible",
                                       "--list", "--stop", "--libc", "--stop-after-init"};
  for (size_t i = 0; i < rest.size(); i++) {
    const std::string& a = rest[i];
    if (a.compare(0, 2, "--") != 0) continue;
    bool known = false;
    for (const char* k : kTakesValue) if (a == k) { known = true; i++; break; }
    if (!known) for (const char* k : kPlain) if (a == k) { known = true; break; }
    if (!known) return a;
  }
  return "";
}

// "builder_version": "<x>" out of config.json text, or "" (a development build has no config.json).
inline std::string version_from_config(const std::string& text) {
  const char* key = "\"builder_version\"";
  size_t k = text.find(key);
  if (k == std::string::npos) return "";
  size_t a = text.find('"', text.find(':', k + 17));
  if (a == std::string::npos) return "";
  size_t b = text.find('"', a + 1);
  if (b == std::string::npos) return "";
  return text.substr(a + 1, b - a - 1);
}

inline const char* help_text() {
  return "Snap Recomp\n"
         "\n"
         "  SnapRecomp.exe                    open the launcher\n"
         "  SnapRecomp.exe --play [options]   start the game without the launcher window\n"
         "\n"
         "Options:\n"
         "  --play                       skip the launcher window; the process ends when the game closes\n"
         "  --safe-mode                  mods, HD packs, enhanced effects and the shader cache off for this start\n"
         "  --fullscreen, --borderless   borderless full-screen window\n"
         "  --windowed                   normal window\n"
         "  --monitor <n>                monitor number, 0 is the first\n"
         "  --width <px> --height <px>   window size\n"
         "  --profile <name>             profile to play or to import to / export from\n"
         "  --romfs <folder>             extracted RomFS folder (overrides SNAP_ROMFS)\n"
         "  --set <key>=<value>          one setting for this start only, repeatable (keys: docs/command-line.md);\n"
         "                               a name that starts with SNAP_ sets that environment variable\n"
         "  --data-dir <folder>          keep saves, settings, mods, caches and logs in this folder\n"
         "\n"
         "Launcher actions without the launcher window (the game does not start):\n"
         "  --update                     install the newest version; it runs in its own window (exit 0 = started)\n"
         "  --check-install              check the game files and print the result (exit 0 = all good, 1 = something is wrong)\n"
         "  --report                     make the problem report zip and print where it is; nothing is uploaded\n"
         "  --restore-backup [latest|<n>]\n"
         "                               list the save backups, or put one back (no window)\n"
         "  --upscalers [<name> --accept-license <name>]\n"
         "                               list DLSS / FSR / XeSS, or download one whose license you accept (no window)\n"
         "  --hdtex [options]            HD texture state, or the HD texture tool with its own options (no window)\n"
         "\n"
         "Other commands (no game window unless noted):\n"
         "  --import-save <folder or zip>, --export-save <folder>\n"
         "  --benchmark [--benchmark-capped | --benchmark-uncapped] [--benchmark-view <file>]   plays the benchmark ride\n"
         "  --prepare-shaders [--force] [--visible] [--list] [--stop] [--courses <list>]\n"
         "  --help, --version\n";
}

}  // namespace snap_cli
