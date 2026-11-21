// SPDX-License-Identifier: MIT
// Stage 6E-A Groups 2A, 2B & 2C: Linux unit tests for nvn::os_host.
#include "nvn_os_host.h"
#include "nvn_os_host_vk.h"
#include "nvn_btnicon.h"
#include "nvn_framegen.h"
#include "nvn_freecam.h"
#include "nvn_hdphoto.h"
#include "nvn_hdr.h"
#include "nvn_shot.h"
#include "ui/ui_present.h"
#include "upscale/upscaler.h"
#include "upscale/framegen.h"

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* msg, int line) {
  ++g_checks;
  if (!ok) {
    ++g_fails;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, msg);
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void test_aligned_alloc_free() {
  void* p1 = nvn::os_host::aligned_alloc(0x1000, 100);  // size not a multiple of alignment
  CHECK(p1 != nullptr);
  CHECK((reinterpret_cast<uintptr_t>(p1) & 0xFFFull) == 0);
  std::memset(p1, 0xAB, 100);
  nvn::os_host::aligned_free(p1);

  void* p2 = nvn::os_host::aligned_alloc(64, 4096);
  CHECK(p2 != nullptr);
  CHECK((reinterpret_cast<uintptr_t>(p2) & 63ull) == 0);
  nvn::os_host::aligned_free(p2);

  nvn::os_host::aligned_free(nullptr);
}

void test_ids_and_priorities() {
  const uint64_t pid = nvn::os_host::current_process_id();
  const uint64_t main_tid = nvn::os_host::current_thread_id();
  CHECK(pid != 0);
  CHECK(main_tid != 0);

  const int main_prio_before = ::getpriority(PRIO_PROCESS, 0);
  uint64_t worker_tid = 0;
  std::thread th([&] {
    worker_tid = nvn::os_host::current_thread_id();
    // Must never crash even when unprivileged (set_current_thread_above_normal typically returns false).
    (void)nvn::os_host::set_current_thread_below_normal();
    (void)nvn::os_host::set_current_thread_above_normal();
  });
  th.join();
  CHECK(::getpriority(PRIO_PROCESS, 0) == main_prio_before);
  CHECK(worker_tid != 0);
  CHECK(worker_tid != main_tid);
}

void test_monotonic_and_rdtsc() {
  const uint64_t ns0 = nvn::os_host::monotonic_ns();
  const uint64_t ms0 = nvn::os_host::monotonic_ms();
  const uint64_t tsc0 = nvn::os_host::rdtsc();

  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  const uint64_t ns1 = nvn::os_host::monotonic_ns();
  const uint64_t ms1 = nvn::os_host::monotonic_ms();
  const uint64_t tsc1 = nvn::os_host::rdtsc();

  CHECK(ns1 > ns0);
  CHECK(ms1 >= ms0);
  CHECK(tsc1 != tsc0);

  uint64_t cyc = 12345;
  CHECK(!nvn::os_host::thread_cycle_time(&cyc));
  CHECK(cyc == 0);
}

void test_ascii_ieq() {
  CHECK(nvn::os_host::ascii_ieq("enhanced", "ENHANCED"));
  CHECK(nvn::os_host::ascii_ieq("On", "oN"));
  CHECK(!nvn::os_host::ascii_ieq("high", "low"));
  CHECK(!nvn::os_host::ascii_ieq("high", "high2"));
  CHECK(nvn::os_host::ascii_ieq(nullptr, nullptr));
  CHECK(!nvn::os_host::ascii_ieq(nullptr, "a"));
}

void test_yield_and_readable() {
  nvn::os_host::cpu_yield();
  const uint64_t val = 0x123456789abcdef0ull;
  CHECK(nvn::os_host::readable_addr(&val, sizeof(val)));
  CHECK(nvn::os_host::readable_addr(nullptr, 0));
  CHECK(!nvn::os_host::readable_addr(nullptr, 8));
  CHECK(!nvn::os_host::readable_addr(reinterpret_cast<const void*>(0x1000), 8));
}

std::string to_hex(const uint8_t d[32]) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = kHex[d[i] >> 4];
    out[i * 2 + 1] = kHex[d[i] & 0xF];
  }
  return out;
}

void test_sha256_vectors() {
  uint8_t digest[32]{};

  // FIPS 180-4 empty message
  nvn::os_host::sha256("", 0, digest);
  CHECK(to_hex(digest) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // FIPS 180-4 "abc"
  nvn::os_host::sha256("abc", 3, digest);
  CHECK(to_hex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // FIPS 180-4 448-bit (multi-block after padding)
  const char* mb = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  nvn::os_host::sha256(mb, std::strlen(mb), digest);
  CHECK(to_hex(digest) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // Binary buffer containing NUL bytes + sha256_2 equivalence
  const uint8_t bin1[] = {0x00, 0x01, 0x00, 0xff, 0x10};
  const uint8_t bin2[] = {0x00, 0x20, 0x30, 0x00};
  std::vector<uint8_t> combined;
  combined.insert(combined.end(), std::begin(bin1), std::end(bin1));
  combined.insert(combined.end(), std::begin(bin2), std::end(bin2));

  uint8_t d_single[32]{}, d_two[32]{};
  nvn::os_host::sha256(combined.data(), combined.size(), d_single);
  nvn::os_host::sha256_2(bin1, sizeof(bin1), bin2, sizeof(bin2), d_two);
  CHECK(std::memcmp(d_single, d_two, 32) == 0);
}

void test_cache_dir_xdg() {
  const char* old_xdg = std::getenv("XDG_CACHE_HOME");
  const char* old_home = std::getenv("HOME");
  const std::string saved_xdg = old_xdg ? old_xdg : "";
  const std::string saved_home = old_home ? old_home : "";

  ::setenv("HOME", "/tmp/snap_test_home", 1);
  ::setenv("XDG_CACHE_HOME", "/tmp/snap_test_xdg", 1);
  CHECK(nvn::os_host::cache_dir() == "/tmp/snap_test_xdg/SnapRecomp");

  ::unsetenv("XDG_CACHE_HOME");
  CHECK(nvn::os_host::cache_dir() == "/tmp/snap_test_home/.cache/SnapRecomp");

  ::setenv("XDG_CACHE_HOME", "", 1);
  CHECK(nvn::os_host::cache_dir() == "/tmp/snap_test_home/.cache/SnapRecomp");

  if (old_xdg) ::setenv("XDG_CACHE_HOME", saved_xdg.c_str(), 1);
  else ::unsetenv("XDG_CACHE_HOME");
  if (old_home) ::setenv("HOME", saved_home.c_str(), 1);
  else ::unsetenv("HOME");
}

void test_filesystem_and_harness() {
  CHECK(nvn::os_host::path_sep() == '/');
  CHECK(!nvn::os_host::temp_dir().empty());
  CHECK(!nvn::os_host::exe_dir().empty());

  const std::string base = nvn::os_host::temp_dir() + "/snap_nvn_test_" + std::to_string(nvn::os_host::current_process_id());
  std::error_code ec;
  std::filesystem::remove_all(base, ec);

  const std::string nested = base + "/a/b/c";
  CHECK(nvn::os_host::ensure_dir(nested));
  CHECK(std::filesystem::is_directory(nested, ec));

  const std::string f1 = nested + "/deadbeef1234.spv";
  const std::string f2 = nested + "/deadbeef9999.spv";
  const std::string f3 = nested + "/other.json";
  const uint8_t payload1[] = {0x03, 0x02, 0x23, 0x07, 0x00, 0x01};
  const uint8_t payload2[] = {0xaa, 0xbb, 0xcc};

  CHECK(nvn::os_host::write_file_bytes(f1, payload1, sizeof(payload1)));
  CHECK(nvn::os_host::write_file_bytes(f2, payload2, sizeof(payload2)));
  CHECK(nvn::os_host::write_file_bytes(f3, "{}", 2));

  CHECK(nvn::os_host::file_exists(f1));
  CHECK(!nvn::os_host::file_exists(nested));  // directory is not a regular file

  int64_t mtime = 0;
  CHECK(nvn::os_host::file_mtime_ns(f1, &mtime));
  CHECK(mtime > 0);

  std::vector<uint8_t> read_back;
  CHECK(nvn::os_host::read_file_bytes(f1, &read_back));
  CHECK(read_back.size() == sizeof(payload1));
  CHECK(std::memcmp(read_back.data(), payload1, sizeof(payload1)) == 0);

  const auto spv_matches = nvn::os_host::list_files(nested, "deadbeef", ".spv");
  CHECK(spv_matches.size() == 2);
  const auto json_matches = nvn::os_host::list_files(nested, nullptr, ".json");
  CHECK(json_matches.size() == 1 && json_matches[0] == "other.json");

  // Atomic replace overwrites destination
  const std::string tmp_ok = nested + "/f1.tmp";
  CHECK(nvn::os_host::write_file_bytes(tmp_ok, payload2, sizeof(payload2)));
  CHECK(nvn::os_host::atomic_replace(tmp_ok, f1));
  CHECK(!nvn::os_host::file_exists(tmp_ok));
  CHECK(nvn::os_host::read_file_bytes(f1, &read_back));
  CHECK(read_back.size() == sizeof(payload2));
  CHECK(std::memcmp(read_back.data(), payload2, sizeof(payload2)) == 0);

  // Failed atomic replace preserves prior destination file
  CHECK(!nvn::os_host::atomic_replace(nested + "/missing.tmp", f1));
  CHECK(nvn::os_host::read_file_bytes(f1, &read_back));
  CHECK(read_back.size() == sizeof(payload2));

  CHECK(nvn::os_host::remove_file(f1));
  CHECK(!nvn::os_host::file_exists(f1));

  // Explicit false on Linux for ShaderHarness
  CHECK(!nvn::os_host::run_shader_harness("ShaderHarness.exe", f3));

  std::filesystem::remove_all(base, ec);
}

void test_writeback_watch() {
  uint8_t buf[4096]{};
  int present_count = 7;
  CHECK(!nvn::os_host::watch_writeback_page(nullptr, 0, nullptr));
  CHECK(!nvn::os_host::watch_writeback_page(buf, sizeof(buf), &present_count));
  // Buffer must remain safely readable and writable without any SIGSEGV/PAGE_GUARD side effects.
  buf[0] = 0x5A;
  buf[sizeof(buf) - 1] = 0xA5;
  CHECK(buf[0] == 0x5A && buf[sizeof(buf) - 1] == 0xA5);
}

void test_pacing_helpers() {
  // Past target returns immediately without stalling.
  const int64_t t_now = static_cast<int64_t>(nvn::os_host::monotonic_ns());
  nvn::os_host::sleep_until_steady_ns(t_now - 1000000, 0);

  // Future target sleeps at least until target_ns on the same CLOCK_MONOTONIC epoch.
  const int64_t t_start = static_cast<int64_t>(nvn::os_host::monotonic_ns());
  const int64_t t_target = t_start + 3000000;  // +3 ms
  nvn::os_host::sleep_until_steady_ns(t_target, 300000);
  const int64_t t_end = static_cast<int64_t>(nvn::os_host::monotonic_ns());
  CHECK(t_end >= t_target);

  // VBlank wait is explicitly unsupported on Linux (steady-clock fallback).
  CHECK(!nvn::os_host::pace_vblank_supported());
  CHECK(nvn::os_host::pace_vblank_open("HDMI-A-1") == nullptr);
  int st = 0;
  CHECK(!nvn::os_host::pace_vblank_wait(nullptr, &st));
  CHECK(st == -1);
  nvn::os_host::pace_vblank_close(nullptr);

  // Headless/uninitialized display query falls back cleanly without crashing.
  nvn::os_host::PaceDisplayInfo info{};
  info.refresh_hz = 999.0;
  CHECK(!nvn::os_host::pace_query_display(nullptr, 0, &info));
  CHECK(info.refresh_hz == 0.0);
  CHECK(!nvn::os_host::pace_query_display(nullptr, 0, nullptr));
}

extern "C" {
void os_host_block_begin(void);
void os_host_block_end(void);
bool snap_ui_start_pending();
void snap_ui_first_picture();
void snap_look_present(const SnapUiFrame*);
int snap_window_frame_cap();
void snap_fs_drain_for_exit(int ms);
void snap_crashlog_fatal(const char*, const char*);
}

void test_vulkan_surface_and_upscale_stubs() {
  // 1. Write-watch is explicitly unsupported on Linux (falls back to full copy).
  CHECK(!nvn::os_host::write_watch_supported());
  std::vector<void*> dirty;
  uint32_t ww_err = 123;
  uint8_t dummy_page[4096]{};
  CHECK(!nvn::os_host::query_write_watch(dummy_page, sizeof(dummy_page), true, &dirty, &ww_err));
  CHECK(dirty.empty());
  CHECK(ww_err == 0);

  // 2. Weak optional host/UI/OS symbols link and execute safely.
  os_host_block_begin();
  os_host_block_end();
  snap_ui_present(nullptr);
  CHECK(!snap_ui_start_pending());
  snap_ui_first_picture();
  snap_look_present(nullptr);
  CHECK(snap_window_frame_cap() == 0);
  snap_fs_drain_for_exit(0);
  snap_crashlog_fatal("test", "noop");

  // 3. Upscaler & optional Windows-only feature stubs report unsupported explicitly.
  CHECK(!snap_upscale::xess::Load());
  CHECK(!snap_framegen::Wanted());
  CHECK(!nvn::FgWanted());
  CHECK(!nvn::FgActive());
  CHECK(nvn::FgRecordGenerate(nullptr, VK_NULL_HANDLE, nullptr) == VK_NULL_HANDLE);
  CHECK(!nvn::BtnIconActive());
  CHECK(!nvn::HdPhotoOn());
  CHECK(!nvn::ShotWritePng("/tmp/unused.png", nullptr, 0, 0, false));
  nvnHostFreecamShot();

  // 4. Neutral Vulkan surface guards.
  std::vector<const char*> exts;
  nvn::os_host::append_vulkan_surface_instance_extensions(exts);
  bool has_khr_surface = false;
  for (const char* e : exts) {
    if (e && std::strcmp(e, VK_KHR_SURFACE_EXTENSION_NAME) == 0) has_khr_surface = true;
  }
  CHECK(has_khr_surface);

  VkSurfaceKHR surf = reinterpret_cast<VkSurfaceKHR>(0x1);
  CHECK(!nvn::os_host::create_vulkan_surface(VK_NULL_HANDLE, nullptr, nullptr, &surf));
  CHECK(surf == VK_NULL_HANDLE);
  CHECK(!nvn::os_host::create_vulkan_surface(VK_NULL_HANDLE, reinterpret_cast<void*>(0x1234), nullptr, nullptr));
  CHECK(!nvn::os_host::physical_device_presentation_support(VK_NULL_HANDLE, VK_NULL_HANDLE, 0));

#if defined(SNAP_HAVE_VULKAN) && defined(HLE_WITH_SDL3)
  if (SDL_Init(SDL_INIT_VIDEO)) {
    std::vector<const char*> sdl_exts;
    nvn::os_host::append_vulkan_surface_instance_extensions(sdl_exts);
    CHECK(!sdl_exts.empty());

    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "nvn_os_host_linux_test";
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = static_cast<uint32_t>(sdl_exts.size());
    ici.ppEnabledExtensionNames = sdl_exts.data();
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) == VK_SUCCESS && inst != VK_NULL_HANDLE) {
      SDL_Window* win = SDL_CreateWindow("nvn_vk_surface_test", 64, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
      if (win) {
        VkSurfaceKHR win_surf = VK_NULL_HANDLE;
        CHECK(nvn::os_host::create_vulkan_surface(inst, win, nullptr, &win_surf));
        CHECK(win_surf != VK_NULL_HANDLE);
        if (win_surf != VK_NULL_HANDLE) {
          vkDestroySurfaceKHR(inst, win_surf, nullptr);
        }
        SDL_DestroyWindow(win);
      }
      vkDestroyInstance(inst, nullptr);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
#endif
}

void test_shader_harness_posix() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path base = fs::path(nvn::os_host::temp_dir()) / "snap_harness_test";
  fs::remove_all(base, ec);
  const fs::path spaced_dir = base / "dir with spaces";
  fs::create_directories(spaced_dir, ec);
  CHECK(fs::exists(spaced_dir));

  const fs::path ok_script = spaced_dir / "mock harness.py";
  const fs::path out_json = spaced_dir / "out shader.json";
  {
    const std::string body =
        "#!/usr/bin/env python3\n"
        "import os, signal, sys, time\n"
        "mode = sys.argv[1]\n"
        "out = sys.argv[2]\n"
        "if mode == 'ok':\n"
        "    open(out, 'w').write('{\"ok\":true}\\n')\n"
        "    sys.exit(0)\n"
        "elif mode == 'empty':\n"
        "    open(out, 'w').write('')\n"
        "    sys.exit(0)\n"
        "elif mode == 'noout':\n"
        "    sys.exit(0)\n"
        "elif mode == 'fail':\n"
        "    sys.stderr.write('synthetic failure\\n')\n"
        "    sys.exit(3)\n"
        "elif mode == 'sleep':\n"
        "    time.sleep(2.0)\n"
        "    sys.exit(0)\n"
        "elif mode == 'sig':\n"
        "    signal.signal(signal.SIGTERM, signal.SIG_DFL)\n"
        "    os.kill(os.getpid(), signal.SIGTERM)\n";
    CHECK(nvn::os_host::write_file_bytes(ok_script.string(), body.data(), body.size()));
    fs::permissions(ok_script,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace, ec);
    CHECK(!ec);
  }

  // 1. Success with spaces in both executable path and output json path.
  CHECK(nvn::os_host::run_shader_harness("\"" + ok_script.string() + "\" ok \"" + out_json.string() + "\"",
                                        out_json.string()));
  CHECK(nvn::os_host::file_exists(out_json.string()));

  // 2. Missing executable / missing dll.
  CHECK(!nvn::os_host::run_shader_harness("\"" + (spaced_dir / "missing_harness").string() + "\"", out_json.string()));
  CHECK(!nvn::os_host::run_shader_harness("dotnet \"" + (spaced_dir / "missing.dll").string() + "\"", out_json.string()));

  // 3. Nonzero exit code.
  fs::remove(out_json, ec);
  CHECK(!nvn::os_host::run_shader_harness("\"" + ok_script.string() + "\" fail \"" + out_json.string() + "\"",
                                         out_json.string()));

  // 4. Exit 0 but missing output artifact.
  fs::remove(out_json, ec);
  CHECK(!nvn::os_host::run_shader_harness("\"" + ok_script.string() + "\" noout \"" + out_json.string() + "\"",
                                         out_json.string()));

  // 5. Exit 0 but empty output artifact.
  CHECK(!nvn::os_host::run_shader_harness("\"" + ok_script.string() + "\" empty \"" + out_json.string() + "\"",
                                         out_json.string()));

  // 6. Timeout handling.
  ::setenv("SNAP_HARNESS_TIMEOUT_MS", "100", 1);
  CHECK(!nvn::os_host::run_shader_harness("\"" + ok_script.string() + "\" sleep \"" + out_json.string() + "\"",
                                         out_json.string()));
  ::unsetenv("SNAP_HARNESS_TIMEOUT_MS");

  // 7. Abnormal termination by POSIX signal (SIGTERM) logs signal number, description, and runtime/target.
  {
    std::fflush(stderr);
    int cap_fds[2] = {-1, -1};
    CHECK(::pipe(cap_fds) == 0);
    const int saved_stderr = ::dup(STDERR_FILENO);
    CHECK(saved_stderr >= 0);
    CHECK(::dup2(cap_fds[1], STDERR_FILENO) >= 0);
    ::close(cap_fds[1]);

    CHECK(!nvn::os_host::run_shader_harness("\"" + ok_script.string() + "\" sig \"" + out_json.string() + "\"",
                                           out_json.string()));
    std::fflush(stderr);
    ::dup2(saved_stderr, STDERR_FILENO);
    ::close(saved_stderr);

    std::string diag;
    char buf[512];
    for (;;) {
      const ssize_t n = ::read(cap_fds[0], buf, sizeof(buf));
      if (n > 0) diag.append(buf, static_cast<size_t>(n));
      else break;
    }
    ::close(cap_fds[0]);
    std::fwrite(diag.data(), 1, diag.size(), stderr);

    const std::string expected_sig = "sig=" + std::to_string(SIGTERM) + " (";
    CHECK(diag.find("[nvn][harness] child terminated by signal:") != std::string::npos);
    const size_t sig_pos = diag.find(expected_sig);
    CHECK(sig_pos != std::string::npos);
    if (sig_pos != std::string::npos) {
      const size_t close_paren = diag.find(')', sig_pos + expected_sig.size());
      CHECK(close_paren != std::string::npos && close_paren > sig_pos + expected_sig.size());
    }
    CHECK(diag.find("runtime=") != std::string::npos);
    CHECK(diag.find("target=sig") != std::string::npos);
  }

  fs::remove_all(base, ec);
}

}  // namespace

int main() {
  test_aligned_alloc_free();
  test_ids_and_priorities();
  test_monotonic_and_rdtsc();
  test_ascii_ieq();
  test_yield_and_readable();
  test_sha256_vectors();
  test_cache_dir_xdg();
  test_filesystem_and_harness();
  test_writeback_watch();
  test_pacing_helpers();
  test_vulkan_surface_and_upscale_stubs();
  test_shader_harness_posix();
  std::printf("nvn_os_host_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
