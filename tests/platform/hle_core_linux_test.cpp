// SPDX-License-Identifier: MIT
// Stage 6B: unit tests for Linux HLE core infrastructure:
//   1. HLE registry (hle::bind_all, hle::find_override, real vs weak precedence, unimplemented hook + safe_cstr)
//   2. HLE config & settings (XDG_CONFIG_HOME / XDG_DATA_HOME, fallbacks, isolated temp config)
//   3. System services (oe, settings, time, account, vi headless token, err headless, weak pace fallback)
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "hle/hle.h"
#include "hle/hle_config.h"
#include "hle/hle_host.h"
#include "hle/settings.h"
#include "hle/svc_common.h"
#include "hle/zip_read.h"
#include "mods/mods_core.h"

namespace fs = std::filesystem;

extern "C" int snap_pace_vsync_wait(int64_t*);
extern "C" void snap_pace_vsync_note();
extern "C" void os_get_stats(void*) {}

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static std::string g_last_unimpl;
static void test_unimpl_hook(const char* sym) { g_last_unimpl = sym ? sym : ""; }

struct EnvGuard {
    std::string name;
    bool had = false;
    std::string old;
    explicit EnvGuard(const char* n) : name(n) {
        if (const char* v = std::getenv(n)) {
            had = true;
            old = v;
        }
    }
    ~EnvGuard() {
        if (had) setenv(name.c_str(), old.c_str(), 1);
        else unsetenv(name.c_str());
    }
};

static void test_registry() {
    // Real overrides from services_sys + services_net must be present.
    CHECK(hle::overrides().size() > 100);
    GuestFn op_mode = hle::find_override("_ZN2nn2oe16GetOperationModeEv");
    GuestFn lang_code = hle::find_override("_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE");
    GuestFn sock_write = hle::find_override("_ZN2nn6socket5WriteEiPKvm");
    CHECK(op_mode != nullptr);
    CHECK(lang_code != nullptr);
    CHECK(sock_write != nullptr);

    // Real overrides from fs and hid must now also be present; unported/nonexistent symbols remain null.
    CHECK(hle::find_override("_ZN2nn2fs8MountRomEPKcPvm") != nullptr);
    CHECK(hle::find_override("_ZN2nn3hid14InitializeNpadEv") != nullptr);
    CHECK(hle::find_override("_ZN2nn2fs10MountRomFsEPKc") == nullptr);

    // bind_all() must distinguish real overrides from generated weak stubs (unimplemented.cpp).
    std::unordered_map<std::string, GuestFn> bound;
    int real_count = 0, weak_count = 0;
    struct Ctx {
        std::unordered_map<std::string, GuestFn>* map;
        int* real_c;
        int* weak_c;
    } ctx{&bound, &real_count, &weak_count};

    hle::bind_all([](const char* sym, GuestFn fn, void* u) {
        auto* c = static_cast<Ctx*>(u);
        (*c->map)[sym] = fn;
        if (hle::find_override(sym) == fn) (*c->real_c)++;
        else (*c->weak_c)++;
    }, &ctx);

    CHECK(real_count > 100);
    CHECK(weak_count > 50);
    CHECK(bound["_ZN2nn2oe16GetOperationModeEv"] == op_mode);
    // Real override must win over generated weak stub if both exist.
    CHECK(bound.count("_ZN2nn2os20GetCurrentCoreNumberEv") == 1);

    // Isolated probe test of hle::host::readable_addr(), hle::safe_cstr(), and hle::unimplemented().
    char probe_str[] = "snap_probe_arg";
    CHECK(!hle::host::readable_addr(0, 1));
    CHECK(!hle::host::readable_addr(0x100, 1));
    CHECK(hle::host::readable_addr(reinterpret_cast<uint64_t>(probe_str), sizeof(probe_str)));
    CHECK(hle::safe_cstr(0, 32) == "<null>");
    CHECK(hle::safe_cstr(0x100, 32) == "<bad pointer>");
    CHECK(hle::safe_cstr(reinterpret_cast<uint64_t>(probe_str), 32) == "snap_probe_arg");

    // PROT_NONE mapping (>= 0x10000): must report unreadable without crashing.
    void* guard_page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard_page != MAP_FAILED);
    if (guard_page != MAP_FAILED) {
        const uint64_t guard_addr = reinterpret_cast<uint64_t>(guard_page);
        CHECK(guard_addr >= 0x10000);
        CHECK(!hle::host::readable_addr(guard_addr, 1));
        CHECK(hle::safe_cstr(guard_addr, 32) == "<unreadable>");
        CHECK(munmap(guard_page, 4096) == 0);
        // Immediately after munmap (high unmapped address): must also report unreadable safely.
        CHECK(!hle::host::readable_addr(guard_addr, 1));
        CHECK(hle::safe_cstr(guard_addr, 32) == "<unreadable>");
    }

    hle::unimplemented_hook = test_unimpl_hook;
    Cpu c{};
    c.x[0] = reinterpret_cast<uint64_t>(probe_str);
    hle::unimplemented(&c, "nn::test::ProbeTarget");
    CHECK(g_last_unimpl == "nn::test::ProbeTarget");
    CHECK(c.x[0] == 0);
    hle::unimplemented_hook = nullptr;
}

static void test_config_and_xdg() {
    EnvGuard g_home("HOME");
    EnvGuard g_cfg("XDG_CONFIG_HOME");
    EnvGuard g_data("XDG_DATA_HOME");
    EnvGuard g_hcfg("SNAP_HLE_CONFIG");
    EnvGuard g_romfs("SNAP_ROMFS");
    EnvGuard g_sroot("SNAP_SAVE_ROOT");
    EnvGuard g_sdir("SNAP_SAVE_DIR");
    EnvGuard g_hless("SNAP_HLE_HEADLESS");
    EnvGuard g_pname("SNAP_PLAYER_NAME");

    unsetenv("SNAP_ROMFS");
    unsetenv("SNAP_SAVE_ROOT");
    unsetenv("SNAP_SAVE_DIR");
    unsetenv("SNAP_HLE_HEADLESS");
    unsetenv("SNAP_PLAYER_NAME");

    char tmpl[] = "/tmp/snap_hle_xdg_XXXXXX";
    char* tmp = mkdtemp(tmpl);
    CHECK(tmp != nullptr);
    const fs::path root(tmp);

    // 1. Explicit XDG_CONFIG_HOME / XDG_DATA_HOME.
    setenv("HOME", (root / "home").string().c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "cfg_home").string().c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "data_home").string().c_str(), 1);
    unsetenv("SNAP_HLE_CONFIG");

    CHECK(hle::host::default_config_dir() == (root / "cfg_home/SnapRecomp").string());
    CHECK(hle::host::default_data_root() == (root / "data_home/SnapRecomp").string());

    // 2. Fallbacks when XDG_* are unset.
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    CHECK(hle::host::default_config_dir() == (root / "home/.config/SnapRecomp").string());
    CHECK(hle::host::default_data_root() == (root / "home/.local/share/SnapRecomp").string());

    // 3. Non-existent directories handled cleanly + load_config from isolated file.
    fs::create_directories(root / "cfg_home/SnapRecomp");
    const fs::path hle_json = root / "cfg_home/SnapRecomp/hle.json";
    {
        std::ofstream f(hle_json);
        f << R"({"user_name":"Tester","language":2,"docked":false,"headless":true})";
    }
    setenv("XDG_CONFIG_HOME", (root / "cfg_home").string().c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "data_home").string().c_str(), 1);
    CHECK(hle::load_config(""));
    CHECK(hle::config().user_name == "Tester");
    CHECK(hle::config().language == 2);
    CHECK(hle::config().docked == false);
    CHECK(hle::config().headless == true);
    CHECK(hle::config().data_root == (root / "data_home/SnapRecomp").string());

    // 4. Settings read/write in isolated temp file.
    const fs::path cfg_path = root / "settings.cfg";
    hle::settings::use_path_for_test(cfg_path.string());
    hle::settings::set("game_language", "fr");
    CHECK(fs::exists(cfg_path));
    CHECK(hle::settings::get("game_language") == "fr");

    std::error_code ec;
    fs::remove_all(root, ec);
}

static void test_services_sys() {
    const bool orig_docked = hle::config().docked;
    const bool orig_headless = hle::config().headless;

    // Weak NVN pace fallback works without NVN linked.
    int64_t last = 0;
    CHECK(snap_pace_vsync_wait(&last) == 0);
    snap_pace_vsync_note();

    // nn::oe::GetOperationMode (docked=false -> 0, docked=true -> 1).
    GuestFn get_op = hle::find_override("_ZN2nn2oe16GetOperationModeEv");
    CHECK(get_op != nullptr);
    Cpu c{};
    hle::config().docked = true;
    get_op(&c);
    CHECK(c.x[0] == 1);
    hle::config().docked = false;
    get_op(&c);
    CHECK(c.x[0] == 0);

    // nn::settings::LanguageCode::Make(0 -> "ja").
    GuestFn get_lang = hle::find_override("_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE");
    CHECK(get_lang != nullptr);
    if (get_lang) {
        c = Cpu{};
        c.x[0] = 0;
        get_lang(&c);
        char code[8] = {};
        std::memcpy(code, &c.x[0], 8);
        CHECK(std::strcmp(code, "ja") == 0);
    }

    // nn::time::StandardUserSystemClock::GetCurrentTime(int64_t* out)
    GuestFn get_time = hle::find_override("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
    CHECK(get_time != nullptr);
    int64_t posix_t = 0;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(&posix_t);
    get_time(&c);
    CHECK(c.x[0] == 0);
    CHECK(posix_t > 1700000000ll);

    // nn::account::GetUserCount(int32_t* out)
    GuestFn get_uc = hle::find_override("_ZN2nn7account12GetUserCountEPi");
    CHECK(get_uc != nullptr);
    int32_t uc = 0;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(&uc);
    get_uc(&c);
    CHECK(c.x[0] == 0 && uc == 1);

    // nn::vi::GetNativeWindow(void** out, Layer*) without a real HWND: returns opaque non-zero token, not a fake HWND.
    GuestFn get_nw = hle::find_override("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE");
    CHECK(get_nw != nullptr);
    uint64_t win_tok = 0;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(&win_tok);
    get_nw(&c);
    CHECK(c.x[0] == 0 && win_tok != 0);

    // nn::err::ShowError in headless mode: does not block on GUI dialog.
    hle::config().headless = true;
    GuestFn show_err = hle::find_override("_ZN2nn3err9ShowErrorENS_6ResultE");
    CHECK(show_err != nullptr);
    c = Cpu{};
    c.x[0] = 0x1234;
    show_err(&c);

    // nn::time::ToCalendarTime across two timezones (TZ=UTC0 and TZ=EST5).
    GuestFn to_cal = hle::find_override(
        "_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE");
    CHECK(to_cal != nullptr);
    if (to_cal) {
        EnvGuard g_tz("TZ");
        EnvGuard g_stz("SNAP_TIME_ZONE");
        unsetenv("SNAP_TIME_ZONE");

        struct CalTime { int16_t year; uint8_t month, day, hour, min, sec, pad; } cal{};
        struct CalInfo { int32_t dow, doy; char name[8]; uint8_t dst; uint8_t pad[3]; int32_t offset; } info{};
        int64_t sample_t = 1700000000ll;  // 2023-11-14 22:13:20 UTC
        uint8_t dummy_rule[8] = {};

        setenv("TZ", "UTC0", 1);
        c = Cpu{};
        c.x[0] = reinterpret_cast<uint64_t>(&cal);
        c.x[1] = reinterpret_cast<uint64_t>(&info);
        c.x[2] = reinterpret_cast<uint64_t>(&sample_t);
        c.x[3] = reinterpret_cast<uint64_t>(dummy_rule);
        to_cal(&c);
        CHECK(c.x[0] == 0);
        CHECK(cal.year == 2023 && cal.month == 11 && cal.day == 14);
        CHECK(cal.hour == 22 && cal.min == 13 && cal.sec == 20);
        CHECK(info.dst == 0 && info.offset == 0);
        CHECK(std::strcmp(info.name, "UTC") == 0);

        setenv("TZ", "EST5", 1);
        cal = {};
        info = {};
        c = Cpu{};
        c.x[0] = reinterpret_cast<uint64_t>(&cal);
        c.x[1] = reinterpret_cast<uint64_t>(&info);
        c.x[2] = reinterpret_cast<uint64_t>(&sample_t);
        c.x[3] = reinterpret_cast<uint64_t>(dummy_rule);
        to_cal(&c);
        CHECK(c.x[0] == 0);
        CHECK(cal.year == 2023 && cal.month == 11 && cal.day == 14);
        CHECK(cal.hour == 17 && cal.min == 13 && cal.sec == 20);
        CHECK(info.dst == 0 && info.offset == -18000);
        CHECK(std::strcmp(info.name, "EST") == 0);
    }

    hle::config().docked = orig_docked;
    hle::config().headless = orig_headless;
}

static void test_zip_and_mods_safety() {
    // 1. Valid pack IDs pass.
    CHECK(mods::safe_pack_id("hd_textures"));
    CHECK(mods::safe_pack_id("my-mod_v1.2"));
    CHECK(mods::safe_pack_id("cool mod"));

    // 2. Path traversal, separators, trailing dot/space, control chars, and DOS devices are refused.
    CHECK(!mods::safe_pack_id(""));
    CHECK(!mods::safe_pack_id("."));
    CHECK(!mods::safe_pack_id(".."));
    CHECK(!mods::safe_pack_id("../escape"));
    CHECK(!mods::safe_pack_id("a/b"));
    CHECK(!mods::safe_pack_id("a\\b"));
    CHECK(!mods::safe_pack_id("pack."));
    CHECK(!mods::safe_pack_id("pack "));
    CHECK(!mods::safe_pack_id(".. "));
    CHECK(!mods::safe_pack_id("con"));
    CHECK(!mods::safe_pack_id("NUL.zip"));
    CHECK(!mods::safe_pack_id("com1"));
    CHECK(!mods::safe_pack_id("lpt9.txt"));
    CHECK(!mods::safe_pack_id("bad\x01id"));

    // 3. unpack_zip / pack_info refuse unsafe ids without touching disk.
    const fs::path tmp = fs::temp_directory_path() / ("snap_mods_safe_test_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    CHECK(fs::create_directories(tmp, ec));
    std::vector<std::string> warn;
    CHECK(mods::unpack_zip(tmp.string(), "../escape", warn).empty());
    CHECK(!warn.empty());
    mods::Pack pk;
    warn.clear();
    CHECK(!mods::pack_info(tmp.string(), "con", pk, warn));
    CHECK(!warn.empty());

    // 4. zip_read deflate-bomb guard: a deflate entry declaring 1 byte but inflating 10 bytes is refused.
    // Raw deflate block for 10x 'A': stored block header 0x01, len=10 (0x000a), nlen=0xfff5, followed by 10*'A'.
    // Craft a minimal valid ZIP with declared uncompressed size = 1, actual inflated = 10.
    const fs::path bomb_zip = tmp / "bomb.zip";
    const uint8_t deflated[] = {0x01, 0x0a, 0x00, 0xf5, 0xff, 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A'};
    auto w16 = [](std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff); };
    auto w32 = [](std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff); b.push_back((v >> 16) & 0xff); b.push_back((v >> 24) & 0xff);
    };
    std::vector<uint8_t> z;
    const char fname[] = "test.txt";
    // Local file header
    w32(z, 0x04034b50u); w16(z, 20); w16(z, 0); w16(z, 8); w16(z, 0); w16(z, 0);
    w32(z, 0); w32(z, sizeof(deflated)); w32(z, 1 /* declared size 1 < 10 */);
    w16(z, sizeof(fname) - 1); w16(z, 0);
    z.insert(z.end(), fname, fname + sizeof(fname) - 1);
    z.insert(z.end(), deflated, deflated + sizeof(deflated));
    const uint32_t cd_off = static_cast<uint32_t>(z.size());
    // Central directory header
    w32(z, 0x02014b50u); w16(z, 20); w16(z, 20); w16(z, 0); w16(z, 8); w16(z, 0); w16(z, 0);
    w32(z, 0); w32(z, sizeof(deflated)); w32(z, 1);
    w16(z, sizeof(fname) - 1); w16(z, 0); w16(z, 0); w16(z, 0); w16(z, 0); w32(z, 0); w32(z, 0);
    z.insert(z.end(), fname, fname + sizeof(fname) - 1);
    const uint32_t cd_size = static_cast<uint32_t>(z.size()) - cd_off;
    // EOCD
    w32(z, 0x06054b50u); w16(z, 0); w16(z, 0); w16(z, 1); w16(z, 1); w32(z, cd_size); w32(z, cd_off); w16(z, 0);
    {
        std::ofstream f(bomb_zip, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size()));
    }
    std::vector<hle::zipr::Entry> entries;
    std::string zerr;
    CHECK(hle::zipr::list(bomb_zip, entries, &zerr));
    CHECK(entries.size() == 1);
    CHECK(!hle::zipr::extract(bomb_zip, entries[0], tmp / "out.txt", &zerr));

    fs::remove_all(tmp, ec);
}

int main() {
    test_registry();
    test_config_and_xdg();
    test_services_sys();
    test_zip_and_mods_safety();
    std::printf("hle_core_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
