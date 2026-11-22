// SPDX-License-Identifier: MIT
// Stage 6C-A: unit tests for POSIX Filesystem HLE (fs_core.cpp + fs.cpp + hle_host_posix.cpp).
// Uses exclusively isolated temporary directories under /tmp; never touches real ROMFS or saves.
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hle/fs_core.h"
#include "hle/hle.h"
#include "hle/hle_config.h"
#include "hle/hle_host.h"
#include "hle/svc_common.h"

namespace fs = std::filesystem;

extern "C" void os_get_stats(void*) {}

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/snap_fs_test_XXXXXX";
        char* p = mkdtemp(tmpl);
        if (p) path = p;
    }
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
            for (auto it = fs::recursive_directory_iterator(path, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::error_code e2;
                fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, e2);
            }
            fs::remove_all(path, ec);
        }
    }
};

static void test_host_fs_operations() {
    TempDir tmp;
    CHECK(!tmp.path.empty());
    const fs::path host_dir = tmp.path / "host";
    const fs::path rom_dir = tmp.path / "rom";
    fs::create_directories(host_dir);
    fs::create_directories(rom_dir);
    {
        std::ofstream rf(rom_dir / "hello.txt", std::ios::binary);
        rf << "rom_payload";
    }

    hle::fs::Fs hfs;
    CHECK(hfs.mount_host("host", host_dir.string(), true) == hle::kSuccess);
    hfs.set_rom_layers({rom_dir.string()});
    CHECK(hfs.mount_rom("rom") == hle::kSuccess);

    // 1 & 2. Create, open, write, flush, get_file_size, set_file_size, read, close.
    CHECK(hfs.create_file("host:/alpha.bin", 4) == hle::kSuccess);
    CHECK(hfs.create_file("host:/alpha.bin", 4) == hle::fs::kResultPathAlreadyExists);

    hle::fs::FileId fh = 0;
    CHECK(hfs.open_file(&fh, "host:/alpha.bin", hle::fs::kModeRead | hle::fs::kModeWrite) == hle::kSuccess);
    CHECK(fh != 0);

    int64_t fsz = -1;
    CHECK(hfs.get_file_size(fh, &fsz) == hle::kSuccess && fsz == 4);

    const char msg[] = "0123456789ABCDEF";
    CHECK(hfs.set_file_size(fh, 16) == hle::kSuccess);
    CHECK(hfs.write_file(fh, 0, msg, 16, true) == hle::kSuccess);
    CHECK(hfs.flush_file(fh) == hle::kSuccess);
    CHECK(hfs.get_file_size(fh, &fsz) == hle::kSuccess && fsz == 16);

    char rbuf[32] = {};
    CHECK(hfs.read_file(fh, 0, rbuf, 16, nullptr) == hle::kSuccess);
    CHECK(std::memcmp(rbuf, msg, 16) == 0);

    // 3. Partial read and EOF (non-strict vs strict).
    size_t got = 0;
    std::memset(rbuf, 0, sizeof(rbuf));
    CHECK(hfs.read_file(fh, 10, rbuf, 16, &got) == hle::kSuccess);
    CHECK(got == 6);
    CHECK(std::memcmp(rbuf, "ABCDEF", 6) == 0);
    CHECK(hfs.read_file(fh, 16, rbuf, 4, &got) == hle::kSuccess && got == 0);
    CHECK(hfs.read_file(fh, 10, rbuf, 16, nullptr) == hle::fs::kResultOutOfRange);

    // 4. Seek / offset read and write.
    const char patch[] = "XYZ";
    CHECK(hfs.write_file(fh, 4, patch, 3, false) == hle::kSuccess);
    std::memset(rbuf, 0, sizeof(rbuf));
    CHECK(hfs.read_file(fh, 3, rbuf, 5, nullptr) == hle::kSuccess);
    CHECK(std::memcmp(rbuf, "3XYZ7", 5) == 0);

    CHECK(hfs.close_file(fh) == hle::kSuccess);

    // 5 & 6. Non-existent files and directories.
    hle::fs::FileId missing_f = 0;
    hle::fs::DirId missing_d = 0;
    int etype = -1;
    CHECK(hfs.open_file(&missing_f, "host:/no_such_file.bin", hle::fs::kModeRead) == hle::fs::kResultPathNotFound);
    CHECK(hfs.open_directory(&missing_d, "host:/no_such_dir", hle::fs::kOpenAll) == hle::fs::kResultPathNotFound);
    CHECK(hfs.get_entry_type(&etype, "host:/no_such_file.bin") == hle::fs::kResultPathNotFound);
    CHECK(hfs.open_file(&missing_f, "unmounted:/x", hle::fs::kModeRead) == hle::fs::kResultNotMounted);

    // 7. Create and enumerate directories.
    CHECK(hfs.create_directory("host:/sub") == hle::kSuccess);
    CHECK(hfs.create_directory("host:/sub") == hle::fs::kResultPathAlreadyExists);
    CHECK(hfs.create_file("host:/sub/a.txt", 2) == hle::kSuccess);
    CHECK(hfs.create_directory("host:/sub/nested") == hle::kSuccess);

    CHECK(hfs.get_entry_type(&etype, "host:/sub") == hle::kSuccess && etype == hle::fs::kDirectory);
    CHECK(hfs.get_entry_type(&etype, "host:/sub/a.txt") == hle::kSuccess && etype == hle::fs::kFile);

    hle::fs::DirId dh = 0;
    CHECK(hfs.open_directory(&dh, "host:/sub", hle::fs::kOpenAll) == hle::kSuccess);
    int64_t dcount = -1;
    CHECK(hfs.get_directory_entry_count(dh, &dcount) == hle::kSuccess && dcount == 2);
    hle::fs::DirEntry entries[4];
    int64_t read_cnt = 0;
    CHECK(hfs.read_directory(dh, entries, 4, &read_cnt) == hle::kSuccess && read_cnt == 2);
    CHECK(hfs.close_directory(dh) == hle::kSuccess);

    // Non-empty directory delete fails; recursive delete succeeds.
    CHECK(hfs.delete_directory("host:/sub", false) == hle::fs::kResultDirectoryNotEmpty);
    CHECK(hfs.delete_directory("host:/sub", true) == hle::kSuccess);

    // 8. Rename and delete files.
    CHECK(hfs.rename_file("host:/alpha.bin", "host:/beta.bin") == hle::kSuccess);
    CHECK(hfs.get_entry_type(&etype, "host:/alpha.bin") == hle::fs::kResultPathNotFound);
    CHECK(hfs.get_entry_type(&etype, "host:/beta.bin") == hle::kSuccess);
    CHECK(hfs.delete_file("host:/beta.bin") == hle::kSuccess);
    CHECK(hfs.get_entry_type(&etype, "host:/beta.bin") == hle::fs::kResultPathNotFound);

    // 9. Permission denied & read-only rom mount.
    CHECK(hfs.create_file("rom:/illegal.bin", 1) == hle::fs::kResultWriteNotPermitted);
    CHECK(hfs.open_file(&missing_f, "rom:/hello.txt", hle::fs::kModeRead | hle::fs::kModeWrite) == hle::fs::kResultWriteNotPermitted);
    {
        hle::fs::FileId rf = 0;
        CHECK(hfs.open_file(&rf, "rom:/hello.txt", hle::fs::kModeRead) == hle::kSuccess);
        char rb[16] = {};
        CHECK(hfs.read_file(rf, 0, rb, 11, nullptr) == hle::kSuccess);
        CHECK(std::memcmp(rb, "rom_payload", 11) == 0);
        CHECK(hfs.close_file(rf) == hle::kSuccess);
    }
    if (::geteuid() != 0) {
        CHECK(hfs.create_file("host:/locked.bin", 4) == hle::kSuccess);
        const fs::path locked_p = host_dir / "locked.bin";
        CHECK(::chmod(locked_p.c_str(), 0444) == 0);
        hle::fs::FileId lf = 0;
        CHECK(hfs.open_file(&lf, "host:/locked.bin", hle::fs::kModeRead | hle::fs::kModeWrite) != hle::kSuccess);
        CHECK(::chmod(locked_p.c_str(), 0644) == 0);
    }

    // 10. Invalid handles.
    CHECK(hfs.read_file(0xdeadbeefull, 0, rbuf, 4, nullptr) == hle::fs::kResultInvalidArgument);
    CHECK(hfs.write_file(0xdeadbeefull, 0, msg, 4, false) == hle::fs::kResultInvalidArgument);
    CHECK(hfs.get_file_size(0xdeadbeefull, &fsz) == hle::fs::kResultInvalidArgument);
    CHECK(hfs.set_file_size(0xdeadbeefull, 4) == hle::fs::kResultInvalidArgument);
    CHECK(hfs.flush_file(0xdeadbeefull) == hle::fs::kResultInvalidArgument);
    CHECK(hfs.read_directory(0xdeadbeefull, entries, 4, &read_cnt) == hle::fs::kResultInvalidArgument);
    CHECK(hfs.get_directory_entry_count(0xdeadbeefull, &dcount) == hle::fs::kResultInvalidArgument);

    // 11. UTF-8 paths.
    CHECK(hfs.create_directory("host:/caf\xc3\xa9") == hle::kSuccess);
    const std::string utf8_file = "host:/caf\xc3\xa9/pok\xc3\xa9mon_\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.bin";
    CHECK(hfs.create_file(utf8_file, 5) == hle::kSuccess);
    hle::fs::FileId uf = 0;
    CHECK(hfs.open_file(&uf, utf8_file, hle::fs::kModeRead | hle::fs::kModeWrite) == hle::kSuccess);
    CHECK(hfs.write_file(uf, 0, "utf8!", 5, false) == hle::kSuccess);
    std::memset(rbuf, 0, sizeof(rbuf));
    CHECK(hfs.read_file(uf, 0, rbuf, 5, nullptr) == hle::kSuccess);
    CHECK(std::memcmp(rbuf, "utf8!", 5) == 0);
    CHECK(hfs.close_file(uf) == hle::kSuccess);

    // 12. POSIX error conversion helpers.
    CHECK(hle::host::is_disk_full_error(std::make_error_code(std::errc::no_space_on_device)));
    CHECK(hle::host::is_disk_full_error(std::make_error_code(std::errc::file_too_large)));
    CHECK(!hle::host::is_disk_full_error(std::make_error_code(std::errc::permission_denied)));
    errno = ENOENT;
    CHECK(hle::host::last_os_error() == std::make_error_code(std::errc::no_such_file_or_directory));

    // Save lock exclusivity on same lock file.
    void* lk1 = nullptr;
    void* lk2 = nullptr;
    const std::string lock_p = (tmp.path / ".snap.lock").string();
    CHECK(hle::host::try_acquire_save_lock(lock_p, &lk1) == 0 && lk1 != nullptr);
    CHECK(hle::host::try_acquire_save_lock(lock_p, &lk2) == 1 && lk2 == nullptr);
    hle::host::release_save_lock(lk1);
    CHECK(hle::host::try_acquire_save_lock(lock_p, &lk2) == 0 && lk2 != nullptr);
    hle::host::release_save_lock(lk2);
}

static void test_fs_registry_and_shims() {
    // 13 & 14. Real nn::fs overrides registered and replace weak stubs in bind_all().
    const char* const kFsSyms[] = {
        "_ZN2nn2fs8MountRomEPKcPvm",
        "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE",
        "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",
        "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm",
        "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE",
        "_ZN2nn2fs9CloseFileENS0_10FileHandleE",
        "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",
        "_ZN2nn2fs15CreateDirectoryEPKc",
        "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl",
    };

    std::unordered_map<std::string, GuestFn> bound;
    std::unordered_map<std::string, bool> is_real;
    struct Ctx {
        std::unordered_map<std::string, GuestFn>* map;
        std::unordered_map<std::string, bool>* real;
    } ctx{&bound, &is_real};

    hle::bind_all([](const char* sym, GuestFn fn, void* u) {
        auto* c = static_cast<Ctx*>(u);
        (*c->map)[sym] = fn;
        (*c->real)[sym] = (hle::find_override(sym) == fn);
    }, &ctx);

    for (const char* sym : kFsSyms) {
        CHECK(hle::find_override(sym) != nullptr);
        CHECK(bound[sym] == hle::find_override(sym));
        CHECK(is_real[sym]);
    }

    // Drive nn::fs::MountRom + OpenFile + ReadFile + GetFileSize + CloseFile via synthetic Cpu on temp dir.
    TempDir tmp;
    const fs::path rom_dir = tmp.path / "synth_rom";
    fs::create_directories(rom_dir);
    {
        std::ofstream rf(rom_dir / "data.bin", std::ios::binary);
        rf << "SYNTH_ROM_DATA";
    }
    hle::fs::instance().set_rom_layers({rom_dir.string()});

    Cpu c{};
    const char mname[] = "rom";
    c.x[0] = reinterpret_cast<uint64_t>(mname);
    bound["_ZN2nn2fs8MountRomEPKcPvm"](&c);
    CHECK(c.x[0] == hle::kSuccess);

    uint64_t handle_out = 0;
    const char fpath[] = "rom:/data.bin";
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(&handle_out);
    c.x[1] = reinterpret_cast<uint64_t>(fpath);
    c.x[2] = hle::fs::kModeRead;
    bound["_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"](&c);
    CHECK(c.x[0] == hle::kSuccess && handle_out != 0);

    int64_t sz_out = -1;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(&sz_out);
    c.x[1] = handle_out;
    bound["_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE"](&c);
    CHECK(c.x[0] == hle::kSuccess && sz_out == 14);

    char buf[32] = {};
    c = Cpu{};
    c.x[0] = handle_out;
    c.x[1] = 6;
    c.x[2] = reinterpret_cast<uint64_t>(buf);
    c.x[3] = 8;
    bound["_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm"](&c);
    CHECK(c.x[0] == hle::kSuccess);
    CHECK(std::memcmp(buf, "ROM_DATA", 8) == 0);

    c = Cpu{};
    c.x[0] = handle_out;
    bound["_ZN2nn2fs9CloseFileENS0_10FileHandleE"](&c);
    CHECK(c.x[0] == 0);

    hle::fs::instance().unmount("rom");
}

static void put_save(hle::fs::Fs& f, const std::string& p, const std::string& s) {
    uint32_t cr = f.create_file(p, static_cast<int64_t>(s.size()));
    CHECK(cr == 0u || cr == hle::fs::kResultPathAlreadyExists);
    hle::fs::FileId id = 0;
    CHECK(f.open_file(&id, p, hle::fs::kModeRead | hle::fs::kModeWrite) == 0u);
    CHECK(f.set_file_size(id, static_cast<int64_t>(s.size())) == 0u);
    CHECK(f.write_file(id, 0, s.data(), s.size(), true) == 0u);
    CHECK(f.close_file(id) == 0u);
}

static bool get_save(hle::fs::Fs& f, const std::string& p, std::string* out) {
    hle::fs::FileId id = 0;
    if (f.open_file(&id, p, hle::fs::kModeRead) != 0u) return false;
    int64_t sz = 0;
    CHECK(f.get_file_size(id, &sz) == 0u);
    out->assign(static_cast<size_t>(sz), '\0');
    size_t got = 0;
    if (sz > 0) CHECK(f.read_file(id, 0, out->data(), static_cast<size_t>(sz), &got) == 0u);
    CHECK(f.close_file(id) == 0u);
    return true;
}

static std::string slurp_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static uint64_t read_current_gen(const fs::path& ud) {
    std::ifstream f(ud / "CURRENT");
    std::string magic;
    uint64_t g = 0;
    f >> magic >> g;
    return g;
}

static std::set<std::string> gen_files(const fs::path& dir) {
    std::set<std::string> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        out.insert(e.path().filename().string());
    }
    return out;
}

template <class Fn>
static bool run_side_held(std::thread& t, std::atomic<bool>& done, Fn fn, int wait_ms = 60) {
    done.store(false);
    t = std::thread([&done, fn = std::move(fn)]() mutable {
        fn();
        done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    return done.load();
}

static void test_save_commit_concurrent_ops() {
    TempDir tmp;
    const uint64_t uid[2] = {0x1122334455667788ull, 0x99aabbccddeeff00ull};
    const fs::path root = tmp.path / "save_conc";
    const fs::path ud = root / hle::uid_dir_name(uid);
    const std::string a = "savedata:/A", b = "savedata:/B", c = "savedata:/C",
                      d = "savedata:/D", e = "savedata:/E", g = "savedata:/G",
                      sv = "savedata:/save.dat", bak = "savedata:/save.bak";
    std::string body;

    {
        hle::fs::Fs f;
        f.set_save_root(root.string());
        CHECK(f.mount_save("savedata", uid) == 0u);
        put_save(f, a, "AAAA");
        put_save(f, d, "DDDD");
        CHECK(f.commit("savedata") == 0u);

        put_save(f, b, "BBBB");
        put_save(f, e, "EEEE");

        bool ran_carried = false, ran_done = false, side1_early = true, side2_early = true;
        std::thread t1, t2;
        std::atomic<bool> d1{false}, d2{false};
        uint32_t rc1 = 1, rc1b = 1, rc2 = 1;
        f.set_fault_hook([&](const char* s) {
            if (!std::strcmp(s, "commit:carried") && !ran_carried) {
                ran_carried = true;
                put_save(f, b, "B2B2");  // data write runs concurrently with commit
                side1_early = run_side_held(t1, d1, [&] {
                    rc1 = f.delete_file(a);
                    rc1b = f.create_file(c, 4);
                });
            }
            if (!std::strcmp(s, "commit:files-done") && !ran_done) {
                ran_done = true;
                side2_early = run_side_held(t2, d2, [&] {
                    rc2 = f.delete_file(e);
                });
            }
        });
        CHECK(f.commit("savedata") == 0u);
        f.set_fault_hook(nullptr);
        t1.join();
        t2.join();
        CHECK(rc1 == 0u && rc1b == 0u && rc2 == 0u);
        put_save(f, c, "CCCC");
        CHECK(ran_carried && ran_done);
        CHECK(!side1_early && !side2_early);  // PathOpLock held both path ops until commit completed
        CHECK((gen_files(ud / "gens" / std::to_string(read_current_gen(ud))) == std::set<std::string>{"A", "B", "D", "E"}));
        CHECK(slurp_file(ud / "gens" / std::to_string(read_current_gen(ud)) / "B") == "B2B2");
        CHECK(!get_save(f, a, &body));
        CHECK(!get_save(f, e, &body));
        CHECK(get_save(f, c, &body) && body == "CCCC");
        CHECK(get_save(f, b, &body) && body == "B2B2");
        CHECK(f.commit("savedata") == 0u);

        // Rename + delete of dirty save.dat during commit must wait until commit finishes.
        put_save(f, sv, "SAVE1");
        CHECK(f.commit("savedata") == 0u);
        put_save(f, sv, "SAVE2");
        bool ran_sv = false, side3_early = true;
        std::thread t3;
        std::atomic<bool> d3{false};
        uint32_t rc3 = 1, rc3b = 1;
        f.set_fault_hook([&](const char* s) {
            if (!std::strcmp(s, "commit:carried") && !ran_sv) {
                ran_sv = true;
                side3_early = run_side_held(t3, d3, [&] {
                    rc3 = f.rename_file(sv, bak);
                    rc3b = f.delete_file(bak);
                });
            }
        });
        CHECK(f.commit("savedata") == 0u);
        f.set_fault_hook(nullptr);
        t3.join();
        CHECK(rc3 == 0u && rc3b == 0u && ran_sv && !side3_early);
        const fs::path svgen = ud / "gens" / std::to_string(read_current_gen(ud));
        CHECK(gen_files(svgen).count("save.dat") == 1);
        CHECK(slurp_file(svgen / "save.dat") == "SAVE2");
        CHECK(!get_save(f, sv, &body) && !get_save(f, bak, &body));
        put_save(f, sv, "SAVE3");
        CHECK(f.commit("savedata") == 0u);

        // Failed commit (inject non-empty CURRENT.tmp directory) preserves concurrent writes/creations.
        bool ran_fail = false, injected = false;
        std::thread t4;
        std::atomic<bool> d4{false};
        uint32_t rc4 = 1;
        f.set_fault_hook([&](const char* s) {
            if (!std::strcmp(s, "commit:carried") && !ran_fail) {
                ran_fail = true;
                put_save(f, d, "D3D3");
                run_side_held(t4, d4, [&] { rc4 = f.create_file(g, 4); }, 20);
            }
            if (!std::strcmp(s, "commit:marker") && !injected) {
                injected = true;
                fs::create_directories(ud / "CURRENT.tmp" / "block");
            }
        });
        put_save(f, d, "D2D2");
        CHECK(f.commit("savedata") != 0u);
        t4.join();
        CHECK(rc4 == 0u);
        put_save(f, g, "GGGG");
        fs::remove_all(ud / "CURRENT.tmp");
        f.set_fault_hook(nullptr);
        CHECK(f.commit("savedata") == 0u);
        CHECK(f.unmount("savedata") == 0u);
        CHECK(f.drain_for_exit(100));
    }

    hle::fs::Fs f;
    f.set_save_root(root.string());
    CHECK(f.mount_save("savedata", uid) == 0u);
    CHECK(!get_save(f, a, &body));
    CHECK(!get_save(f, e, &body));
    CHECK(!get_save(f, bak, &body));
    CHECK(get_save(f, b, &body) && body == "B2B2");
    CHECK(get_save(f, c, &body) && body == "CCCC");
    CHECK(get_save(f, d, &body) && body == "D3D3");
    CHECK(get_save(f, g, &body) && body == "GGGG");
    CHECK(get_save(f, sv, &body) && body == "SAVE3");
    CHECK((gen_files(ud / "gens" / std::to_string(read_current_gen(ud))) == std::set<std::string>{"B", "C", "D", "G", "save.dat"}));
    CHECK(f.unmount("savedata") == 0u);
}

int main() {
    test_host_fs_operations();
    test_fs_registry_and_shims();
    test_save_commit_concurrent_ops();
    std::printf("fs_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
