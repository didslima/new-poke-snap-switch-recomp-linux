// SPDX-License-Identifier: MIT
// POSIX (Linux) host helpers for the shared NVN->Vulkan layer (stage 6E-A).
#if !defined(__linux__)
#error "runtime/nvn/linux/nvn_os_host_posix.cpp is the Linux implementation of nvn_os_host.h"
#endif

#include "../nvn_os_host.h"
#include "../nvn_os_host_vk.h"
#include "../../ui/ui_present.h"

extern "C" {
__attribute__((weak)) void os_host_block_begin(void) {}
__attribute__((weak)) void os_host_block_end(void) {}
__attribute__((weak)) void snap_ui_present(const SnapUiFrame*) {}
__attribute__((weak)) bool snap_ui_start_pending() { return false; }
__attribute__((weak)) void snap_ui_first_picture() {}
__attribute__((weak)) void snap_look_present(const SnapUiFrame*) {}
__attribute__((weak)) int snap_window_frame_cap() { return 0; }
__attribute__((weak)) void snap_fs_drain_for_exit(int) {}
__attribute__((weak)) void snap_crashlog_fatal(const char*, const char*) {}
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Sha256Ctx {
  uint32_t s[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  uint64_t bits = 0;
  uint8_t buf[64] = {};
  size_t len = 0;

  static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

  void block(const uint8_t b[64]) {
    static constexpr uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) |
             (uint32_t(b[i * 4 + 2]) << 8) | uint32_t(b[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], c = s[1], d = s[2], e = s[3], f = s[4], g = s[5], h = s[6], j = s[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = rotr(f, 6) ^ rotr(f, 11) ^ rotr(f, 25);
      const uint32_t ch = (f & g) ^ ((~f) & h);
      const uint32_t t1 = j + S1 + ch + k[i] + w[i];
      const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & c) ^ (a & d) ^ (c & d);
      const uint32_t t2 = S0 + maj;
      j = h; h = g; g = f; f = e + t1; e = d; d = c; c = a; a = t1 + t2;
    }
    s[0] += a; s[1] += c; s[2] += d; s[3] += e; s[4] += f; s[5] += g; s[6] += h; s[7] += j;
  }

  void update(const void* data, size_t n) {
    if (!data || n == 0) return;
    const auto* p = static_cast<const uint8_t*>(data);
    bits += uint64_t(n) * 8ull;
    while (n > 0) {
      const size_t take = std::min(n, 64 - len);
      std::memcpy(buf + len, p, take);
      len += take;
      p += take;
      n -= take;
      if (len == 64) {
        block(buf);
        len = 0;
      }
    }
  }

  void finish(uint8_t out[32]) {
    buf[len++] = 0x80u;
    if (len > 56) {
      while (len < 64) buf[len++] = 0;
      block(buf);
      len = 0;
    }
    while (len < 56) buf[len++] = 0;
    for (int i = 0; i < 8; ++i) buf[56 + i] = static_cast<uint8_t>((bits >> (56 - i * 8)) & 0xffu);
    block(buf);
    for (int i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>((s[i] >> 24) & 0xffu);
      out[i * 4 + 1] = static_cast<uint8_t>((s[i] >> 16) & 0xffu);
      out[i * 4 + 2] = static_cast<uint8_t>((s[i] >> 8) & 0xffu);
      out[i * 4 + 3] = static_cast<uint8_t>(s[i] & 0xffu);
    }
  }
};

}  // namespace

namespace nvn::os_host {

int primary_screen_height() {
  return 0;
}

uint64_t monotonic_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t monotonic_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void sleep_ms(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint64_t current_thread_id() {
#if defined(SYS_gettid)
  return static_cast<uint64_t>(::syscall(SYS_gettid));
#else
  return static_cast<uint64_t>(::pthread_self());
#endif
}

uint64_t current_process_id() {
  return static_cast<uint64_t>(::getpid());
}

bool set_current_thread_above_normal() {
  // May fail with EACCES/EPERM when unprivileged without RLIMIT_NICE/CAP_SYS_NICE; never fatal.
  return ::setpriority(PRIO_PROCESS, static_cast<id_t>(current_thread_id()), -2) == 0;
}

bool set_current_thread_normal() {
  return ::setpriority(PRIO_PROCESS, static_cast<id_t>(current_thread_id()), 0) == 0;
}

bool set_current_thread_below_normal() {
  return ::setpriority(PRIO_PROCESS, static_cast<id_t>(current_thread_id()), 5) == 0;
}

void* aligned_alloc(size_t alignment, size_t size) {
  if (size == 0) size = 1;
  if (alignment < sizeof(void*)) alignment = sizeof(void*);
  void* ptr = nullptr;
  if (::posix_memalign(&ptr, alignment, size) != 0) return nullptr;
  return ptr;
}

void aligned_free(void* ptr) {
  std::free(ptr);
}

uint64_t rdtsc() {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return monotonic_ns();
#endif
}

void cpu_yield() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

bool readable_addr(const void* ptr, size_t size) {
  if (!size) return true;
  if (!ptr) return false;
  char buf[4096];
  const char* p = static_cast<const char*>(ptr);
  const pid_t self = ::getpid();
  while (size) {
    const size_t n = size < sizeof(buf) ? size : sizeof(buf);
    iovec l{buf, n};
    iovec r{const_cast<char*>(p), n};
    if (::process_vm_readv(self, &l, 1, &r, 1, 0) != static_cast<ssize_t>(n)) return false;
    p += n;
    size -= n;
  }
  return true;
}

bool thread_cycle_time(uint64_t* out_cycles) {
  if (out_cycles) *out_cycles = 0;
  // Linux does not expose an unprivileged per-thread hardware cycle counter equivalent to Win32 QueryThreadCycleTime.
  return false;
}

char path_sep() { return '/'; }

std::string cache_dir() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    return std::string(xdg) + "/SnapRecomp";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::string(home) + "/.cache/SnapRecomp";
  }
  return "/tmp/SnapRecomp";
}

std::string temp_dir() {
  if (const char* t = std::getenv("TMPDIR"); t && *t) return t;
  return "/tmp";
}

std::string exe_dir() {
  char buf[4096] = {};
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = '\0';
  const std::string p(buf, static_cast<size_t>(n));
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

bool ensure_dir(const std::string& path) {
  if (path.empty()) return false;
  std::error_code ec;
  fs::create_directories(fs::path(path), ec);
  return !ec;
}

bool file_exists(const std::string& path) {
  std::error_code ec;
  return fs::is_regular_file(fs::path(path), ec);
}

bool file_mtime_ns(const std::string& path, int64_t* out_ns) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  if (out_ns) {
    *out_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
              static_cast<int64_t>(st.st_mtim.tv_nsec);
  }
  return true;
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>* out) {
  if (!out) return false;
  out->clear();
  std::error_code ec;
  const uintmax_t sz = fs::file_size(fs::path(path), ec);
  if (ec || sz >= (64ull << 20)) return false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out->resize(static_cast<size_t>(sz));
  const bool ok = sz == 0 || std::fread(out->data(), 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz);
  std::fclose(f);
  if (!ok) out->clear();
  return ok;
}

bool write_file_bytes(const std::string& path, const void* data, size_t size) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
  const bool flush_ok = std::fflush(f) == 0;
  const bool close_ok = std::fclose(f) == 0;
  return ok && flush_ok && close_ok;
}

bool remove_file(const std::string& path) {
  std::error_code ec;
  return fs::remove(fs::path(path), ec);
}

bool atomic_replace(const std::string& tmp_path, const std::string& dst_path) {
  std::error_code ec;
  fs::rename(fs::path(tmp_path), fs::path(dst_path), ec);
  return !ec;
}

std::vector<std::string> list_files(const std::string& dir, const char* prefix, const char* suffix) {
  std::vector<std::string> out;
  std::error_code ec;
  const std::string pfx = prefix ? prefix : "";
  const std::string sfx = suffix ? suffix : "";
  for (const auto& entry : fs::directory_iterator(fs::path(dir), ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name.size() >= pfx.size() + sfx.size() &&
        name.compare(0, pfx.size(), pfx) == 0 &&
        name.compare(name.size() - sfx.size(), sfx.size(), sfx) == 0) {
      out.push_back(name);
    }
  }
  return out;
}

void sha256_2(const void* d1, size_t n1, const void* d2, size_t n2, uint8_t out[32]) {
  Sha256Ctx ctx;
  ctx.update(d1, n1);
  ctx.update(d2, n2);
  ctx.finish(out);
}

void sha256(const void* data, size_t len, uint8_t out[32]) {
  sha256_2(data, len, nullptr, 0, out);
}

namespace {

bool parse_command_argv(const std::string& cmd, std::vector<std::string>& out) {
  out.clear();
  std::string cur;
  bool in_tok = false;
  char quote = '\0';
  for (size_t i = 0; i < cmd.size(); ++i) {
    const char ch = cmd[i];
    if (quote) {
      if (ch == quote) {
        quote = '\0';
      } else {
        cur.push_back(ch);
      }
      in_tok = true;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      in_tok = true;
    } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      if (in_tok) {
        out.push_back(std::move(cur));
        cur.clear();
        in_tok = false;
      }
    } else {
      cur.push_back(ch);
      in_tok = true;
    }
  }
  if (quote != '\0') return false;
  if (in_tok) out.push_back(std::move(cur));
  return !out.empty();
}

int harness_timeout_ms() {
  if (const char* e = std::getenv("SNAP_HARNESS_TIMEOUT_MS"); e && *e) {
    const int v = std::atoi(e);
    if (v > 0) return v;
  }
  return 600000;
}

}  // namespace

bool run_shader_harness(const std::string& cmd, const std::string& json_path) {
  std::vector<std::string> argv_storage;
  if (!parse_command_argv(cmd, argv_storage) || json_path.empty()) {
    std::fprintf(stderr, "[nvn][harness] invalid command or empty output path\n");
    return false;
  }

  const char* runtime_exe = argv_storage[0].c_str();
  const char* harness_target = argv_storage.size() > 1 ? argv_storage[1].c_str() : runtime_exe;
  if (argv_storage[0] == "dotnet" && argv_storage.size() > 1) {
    struct stat st_dll {};
    if (::stat(argv_storage[1].c_str(), &st_dll) != 0 || !S_ISREG(st_dll.st_mode)) {
      std::fprintf(stderr, "[nvn][harness] missing harness dll: %s\n", argv_storage[1].c_str());
      return false;
    }
  }

  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    std::fprintf(stderr, "[nvn][harness] pipe() failed (errno=%d: %s)\n", errno, std::strerror(errno));
    return false;
  }
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);

  std::vector<char*> c_argv;
  c_argv.reserve(argv_storage.size() + 1);
  for (std::string& s : argv_storage) c_argv.push_back(s.data());
  c_argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::fprintf(stderr, "[nvn][harness] fork() failed (errno=%d: %s)\n", errno, std::strerror(errno));
    ::close(fds[0]);
    ::close(fds[1]);
    return false;
  }

  if (pid == 0) {
    ::close(fds[0]);
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[1]);
    ::execvp(c_argv[0], c_argv.data());
    std::fprintf(stderr, "execvp(%s) failed: %s\n", c_argv[0], std::strerror(errno));
    _exit(127);
  }

  ::close(fds[1]);
  const int fl = ::fcntl(fds[0], F_GETFL, 0);
  if (fl >= 0) ::fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);

  const int timeout_ms = harness_timeout_ms();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::string output;
  bool timed_out = false;
  bool pipe_open = true;

  while (pipe_open) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      timed_out = true;
      break;
    }
    const int rem_ms = static_cast<int>(
        std::min<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count(), 1000));
    struct pollfd pfd {};
    pfd.fd = fds[0];
    pfd.events = POLLIN | POLLHUP;
    const int pr = ::poll(&pfd, 1, std::max(1, rem_ms));
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    char buf[512];
    for (;;) {
      const ssize_t n = ::read(fds[0], buf, sizeof(buf));
      if (n > 0) {
        if (output.size() < 65536) {
          output.append(buf, static_cast<size_t>(std::min<ssize_t>(n, static_cast<ssize_t>(65536 - output.size()))));
        }
      } else if (n == 0) {
        pipe_open = false;
        break;
      } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        pipe_open = false;
        break;
      }
    }
  }

  if (timed_out) {
    ::kill(pid, SIGKILL);
  }
  ::close(fds[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }

  if (timed_out) {
    std::fprintf(stderr, "[nvn][harness] timeout after %d ms (runtime=%s, target=%s)\n",
                 timeout_ms, runtime_exe, harness_target);
    return false;
  }

  if (WIFSIGNALED(status)) {
    const int sig = WTERMSIG(status);
    const char* desc = ::strsignal(sig);
    const std::string snippet = output.size() > 512 ? output.substr(0, 512) + "..." : output;
    std::fprintf(stderr,
                 "[nvn][harness] child terminated by signal: sig=%d (%s) (runtime=%s, target=%s)%s%s\n",
                 sig, (desc && *desc) ? desc : "unknown signal", runtime_exe, harness_target,
                 snippet.empty() ? "" : ": ", snippet.c_str());
    return false;
  }

  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (!WIFEXITED(status) || exit_code != 0) {
    const std::string snippet = output.size() > 512 ? output.substr(0, 512) + "..." : output;
    std::fprintf(stderr, "[nvn][harness] failed (runtime=%s, target=%s, exit=%d)%s%s\n",
                 runtime_exe, harness_target, exit_code, snippet.empty() ? "" : ": ", snippet.c_str());
    return false;
  }

  struct stat st_out {};
  if (::stat(json_path.c_str(), &st_out) != 0 || !S_ISREG(st_out.st_mode) || st_out.st_size <= 0) {
    std::fprintf(stderr, "[nvn][harness] missing or empty output artifact: %s (runtime=%s, target=%s, exit=0)\n",
                 json_path.c_str(), runtime_exe, harness_target);
    return false;
  }

  return true;
}

namespace {

class PosixXlateServer {
 public:
  enum class Result { Ok, Failed, Unavailable };

  explicit PosixXlateServer(std::string cmdline, int request_ms = 600000, int idle_ms = 45000)
      : cmd_(std::move(cmdline)), request_ms_(request_ms), idle_ms_(idle_ms) {}

  ~PosixXlateServer() { Stop(); }
  PosixXlateServer(const PosixXlateServer&) = delete;
  PosixXlateServer& operator=(const PosixXlateServer&) = delete;

  Result Translate(const std::string& bin, const std::string& spv, const std::string& json, int stage, uint32_t const_off,
                   uint32_t code_off, std::string* msg = nullptr) {
    std::lock_guard<std::mutex> lk(mu_);
    if (disabled_) return Result::Unavailable;
    if (pid_ <= 0 && !StartLocked()) return Fail();
    char tail[96];
    std::snprintf(tail, sizeof(tail), "\t%d\t%u\t%u\n", stage, const_off, code_off);
    const std::string req = bin + "\t" + spv + "\t" + json + tail;
    if (!WriteAll(in_w_, req.data(), req.size())) {
      StopLocked();
      return Fail();
    }
    std::string line;
    if (!ReadLine(&line)) {
      StopLocked();
      return Fail();
    }
    served_++;
    last_use_ = std::chrono::steady_clock::now();
    wake_.notify_all();
    if (line == "ok") return Result::Ok;
    if (msg) *msg = line.size() > 4 ? line.substr(4) : line;
    return Result::Failed;
  }

  void Stop() {
    std::thread t;
    {
      std::lock_guard<std::mutex> lk(mu_);
      StopLocked();
      stop_idle_ = true;
      t = std::move(idle_thread_);
    }
    wake_.notify_all();
    if (t.joinable()) t.join();
  }

 private:
  Result Fail() {
    failures_++;
    if (served_ == 0 || failures_ >= 5) disabled_ = true;
    return Result::Unavailable;
  }

  static bool WriteAll(int fd, const char* data, size_t len) {
    sigset_t block_pipe, old_mask;
    sigemptyset(&block_pipe);
    sigaddset(&block_pipe, SIGPIPE);
    const bool masked = (::pthread_sigmask(SIG_BLOCK, &block_pipe, &old_mask) == 0);
    const bool had_pipe = masked && sigismember(&old_mask, SIGPIPE);
    size_t off = 0;
    bool ok = true;
    while (off < len) {
      const ssize_t n = ::write(fd, data + off, len - off);
      if (n > 0) {
        off += static_cast<size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        ok = false;
        break;
      }
    }
    if (masked) {
      if (!ok && errno == EPIPE && !had_pipe) {
        struct timespec zero{0, 0};
        ::sigtimedwait(&block_pipe, nullptr, &zero);
      }
      ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    }
    return ok;
  }

  bool ReadLine(std::string* out) {
    out->clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request_ms_);
    for (;;) {
      const size_t nl = pending_.find('\n');
      if (nl != std::string::npos) {
        *out = pending_.substr(0, nl);
        if (!out->empty() && out->back() == '\r') out->pop_back();
        pending_.erase(0, nl + 1);
        return true;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return false;
      const int rem_ms = static_cast<int>(
          std::min<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count(), 60000));
      struct pollfd pfd{out_r_, POLLIN, 0};
      const int pr = ::poll(&pfd, 1, std::max(rem_ms, 1));
      if (pr < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (pr == 0) continue;
      char buf[4096];
      const ssize_t r = ::read(out_r_, buf, sizeof(buf));
      if (r > 0) {
        pending_.append(buf, static_cast<size_t>(r));
      } else if (r == 0) {
        return false;
      } else if (errno != EINTR) {
        return false;
      }
    }
  }

  bool StartLocked() {
    std::vector<std::string> argv_storage;
    if (!parse_command_argv(cmd_, argv_storage)) return false;
    if (argv_storage[0] == "dotnet" && argv_storage.size() > 1) {
      struct stat st_dll {};
      if (::stat(argv_storage[1].c_str(), &st_dll) != 0 || !S_ISREG(st_dll.st_mode)) return false;
    }

    int in_fds[2] = {-1, -1};
    int out_fds[2] = {-1, -1};
    if (::pipe(in_fds) != 0) return false;
    if (::pipe(out_fds) != 0) {
      ::close(in_fds[0]);
      ::close(in_fds[1]);
      return false;
    }
    ::fcntl(in_fds[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(out_fds[0], F_SETFD, FD_CLOEXEC);

    std::vector<char*> c_argv;
    c_argv.reserve(argv_storage.size() + 1);
    for (std::string& s : argv_storage) c_argv.push_back(s.data());
    c_argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
      ::close(in_fds[0]);
      ::close(in_fds[1]);
      ::close(out_fds[0]);
      ::close(out_fds[1]);
      return false;
    }
    if (pid == 0) {
      ::prctl(PR_SET_PDEATHSIG, SIGTERM);
      ::dup2(in_fds[0], STDIN_FILENO);
      ::dup2(out_fds[1], STDOUT_FILENO);
      const int devnull = ::open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) ::close(devnull);
      }
      ::close(in_fds[0]);
      ::close(in_fds[1]);
      ::close(out_fds[0]);
      ::close(out_fds[1]);
      ::execvp(c_argv[0], c_argv.data());
      _exit(127);
    }

    ::close(in_fds[0]);
    ::close(out_fds[1]);
    pid_ = pid;
    in_w_ = in_fds[1];
    out_r_ = out_fds[0];
    pending_.clear();
    last_use_ = std::chrono::steady_clock::now();
    if (!idle_thread_.joinable() && !stop_idle_ && idle_ms_ > 0) {
      idle_thread_ = std::thread([this] { IdleLoop(); });
    }
    return true;
  }

  void StopLocked() {
    if (in_w_ >= 0) {
      ::close(in_w_);
      in_w_ = -1;
    }
    if (out_r_ >= 0) {
      ::close(out_r_);
      out_r_ = -1;
    }
    if (pid_ > 0) {
      auto wait_exit = [&](int max_ms) -> bool {
        for (int elapsed = 0; elapsed < max_ms; elapsed += 10) {
          int st = 0;
          const pid_t r = ::waitpid(pid_, &st, WNOHANG);
          if (r == pid_ || (r < 0 && errno == ECHILD)) return true;
          struct timespec ts{0, 10 * 1000 * 1000};
          ::nanosleep(&ts, nullptr);
        }
        return false;
      };
      if (!wait_exit(500)) {
        ::kill(pid_, SIGTERM);
        if (!wait_exit(500)) {
          ::kill(pid_, SIGKILL);
          int st = 0;
          while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
        }
      }
      pid_ = -1;
    }
    pending_.clear();
  }

  void IdleLoop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_idle_) {
      if (pid_ <= 0) {
        wake_.wait(lk);
        continue;
      }
      const auto idle_for = std::chrono::steady_clock::now() - last_use_;
      const auto limit = std::chrono::milliseconds(idle_ms_);
      if (idle_for >= limit) {
        StopLocked();
        continue;
      }
      wake_.wait_for(lk, limit - idle_for);
    }
  }

  std::string cmd_;
  int request_ms_;
  int idle_ms_;
  std::mutex mu_;
  std::condition_variable wake_;
  pid_t pid_ = -1;
  int in_w_ = -1;
  int out_r_ = -1;
  std::string pending_;
  std::chrono::steady_clock::time_point last_use_{};
  std::thread idle_thread_;
  bool stop_idle_ = false;
  bool disabled_ = false;
  int failures_ = 0;
  uint64_t served_ = 0;
};

struct ServerSlot {
  std::mutex mu;
  std::string launch;
  std::unique_ptr<PosixXlateServer> srv;
};

ServerSlot& server_slot() {
  static ServerSlot slot;
  return slot;
}

void shutdown_xlate_server_atexit() {
  ServerSlot& slot = server_slot();
  std::lock_guard<std::mutex> lk(slot.mu);
  if (slot.srv) slot.srv->Stop();
}

}  // namespace

bool run_shader_harness_server(const std::string& launch, const std::string& bin, const std::string& spv, const std::string& json,
                               int stage, uint32_t const_off, uint32_t code_off, bool* out_ok) {
  if (out_ok) *out_ok = false;
  if (launch.empty() || bin.empty() || spv.empty() || json.empty()) return false;
  if (const char* e = std::getenv("SNAP_XLATE_SERVER"); e && e[0] == '0' && e[1] == '\0') return false;

  PosixXlateServer* srv = nullptr;
  {
    ServerSlot& slot = server_slot();
    std::lock_guard<std::mutex> lk(slot.mu);
    static std::once_flag reg_atexit;
    std::call_once(reg_atexit, []() { std::atexit(shutdown_xlate_server_atexit); });
    if (!slot.srv || slot.launch != launch) {
      slot.launch = launch;
      slot.srv = std::make_unique<PosixXlateServer>(launch + " --nvn-serve", harness_timeout_ms());
    }
    srv = slot.srv.get();
  }

  std::string msg;
  const auto r = srv->Translate(bin, spv, json, stage, const_off, code_off, &msg);
  if (r == PosixXlateServer::Result::Unavailable) return false;
  if (r == PosixXlateServer::Result::Failed) {
    std::fprintf(stderr, "[nvn][harness-server] shader rejected: %s\n", msg.c_str());
    return true;
  }
  struct stat st_out {};
  if (::stat(json.c_str(), &st_out) != 0 || !S_ISREG(st_out.st_mode) || st_out.st_size <= 0) {
    std::fprintf(stderr, "[nvn][harness-server] missing or empty output artifact: %s\n", json.c_str());
    return true;
  }
  if (out_ok) *out_ok = true;
  return true;
}

std::string resolve_shader_cache_key(const std::string&, std::string (*sha_fn)(void*, const std::string&), void* sha_ctx,
                                     const std::string& salt, int stage, uint32_t code_off, uint32_t) {
  return sha_fn(sha_ctx, salt + "|" + std::to_string(stage) + "|" + std::to_string(code_off));
}

bool ensure_shader_cache_entry(const std::string& dir, const std::string& key, uint32_t,
                               bool (*try_load)(void*), bool (*translate_fn)(void*, const std::string&, const std::string&, const std::string&),
                               void* ctx) {
  const std::string base = dir + path_sep() + key;
  const std::string spvPath = base + ".spv", jsonPath = base + ".json";
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (try_load(ctx)) return true;
    remove_file(spvPath);
    remove_file(jsonPath);
    if (attempt) break;
    const std::string binPath = base + ".bin", spvTmp = spvPath + ".tmp", jsonTmp = jsonPath + ".tmp";
    remove_file(spvTmp);
    remove_file(jsonTmp);
    bool ok = translate_fn(ctx, binPath, spvTmp, jsonTmp);
    remove_file(binPath);
    if (ok) ok = atomic_replace(spvTmp, spvPath) && atomic_replace(jsonTmp, jsonPath);
    if (!ok) {
      remove_file(spvTmp);
      remove_file(jsonTmp);
      return false;
    }
  }
  return false;
}

bool run_process_capture(const std::string& cmd, std::string* out_output, int* out_exit_code) {
  std::string full = cmd + " 2>&1";
  FILE* p = ::popen(full.c_str(), "r");
  if (!p) {
    if (out_output) *out_output = "cannot start process";
    if (out_exit_code) *out_exit_code = -1;
    return false;
  }
  std::string msg;
  char buf[512];
  while (const size_t n = std::fread(buf, 1, sizeof(buf), p)) {
    msg.append(buf, n);
  }
  const int st = ::pclose(p);
  const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  if (out_output) *out_output = std::move(msg);
  if (out_exit_code) *out_exit_code = code;
  return code == 0;
}

const char* glslang_default_exe() { return "glslangValidator"; }

std::string glslang_sdk_candidate(const std::string& root) {
  return root + "/tools/vulkan_sdk/bin/glslangValidator";
}

bool watch_writeback_page(void* /*addr*/, size_t /*size*/, const int* /*present_count_ptr*/) {
  // Windows PAGE_GUARD + VEH cannot be emulated safely on Linux without global SIGSEGV handlers
  // that would conflict with runtime memory management; report unavailable explicitly.
  static std::once_flag warned;
  std::call_once(warned, [] {
    std::fprintf(stderr, "[wbwatch] NVN_WB_WATCH is unavailable on Linux (no safe PAGE_GUARD equivalent)\n");
  });
  return false;
}

void sleep_until_steady_ns(int64_t target_ns, int64_t spin_ns) {
  const int64_t now = static_cast<int64_t>(monotonic_ns());
  if (target_ns <= now) return;

  const int64_t sleep_target = (spin_ns > 0 && target_ns - now > spin_ns) ? (target_ns - spin_ns) : target_ns;
  if (sleep_target > now) {
    struct timespec ts {};
    ts.tv_sec = static_cast<time_t>(sleep_target / 1000000000LL);
    ts.tv_nsec = static_cast<long>(sleep_target % 1000000000LL);
    while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
  }
  if (spin_ns > 0) {
    while (static_cast<int64_t>(monotonic_ns()) < target_ns) {
      cpu_yield();
    }
  }
}

bool pace_query_display(void* native_window, int monitor_override, PaceDisplayInfo* out) {
  if (!out) return false;
  *out = PaceDisplayInfo{};
#ifdef HLE_WITH_SDL3
  if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    SDL_DisplayID id = 0;
    if (monitor_override > 0) {
      int count = 0;
      SDL_DisplayID* displays = SDL_GetDisplays(&count);
      if (displays) {
        if (monitor_override <= count) {
          id = displays[monitor_override - 1];
        }
        SDL_free(displays);
      }
    } else if (native_window) {
      id = SDL_GetDisplayForWindow(static_cast<SDL_Window*>(native_window));
    } else {
      id = SDL_GetPrimaryDisplay();
    }
    if (id != 0) {
      out->monitor_id = static_cast<uint64_t>(id);
      if (const char* dname = SDL_GetDisplayName(id)) {
        std::snprintf(out->name, sizeof(out->name), "%s", dname);
      } else {
        std::snprintf(out->name, sizeof(out->name), "SDL_Display_%llu", static_cast<unsigned long long>(id));
      }
      if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(id)) {
        if (dm->refresh_rate > 0.0f) {
          out->refresh_hz = static_cast<double>(dm->refresh_rate);
          return true;
        }
      }
    }
  }
#else
  (void)native_window;
  (void)monitor_override;
#endif
  return false;
}

bool pace_vblank_supported() {
  // No D3DKMT vblank-wait equivalent on Linux yet; use steady-clock grid fallback without fake VSync precision.
  return false;
}

void* pace_vblank_open(const char* /*display_name*/) {
  return nullptr;
}

bool pace_vblank_wait(void* /*handle*/, int* out_status) {
  if (out_status) *out_status = -1;
  return false;
}

void pace_vblank_close(void* /*handle*/) {}

[[noreturn]] void fatal_exit(int code) {
  std::_Exit(code);
}

#ifdef HLE_WITH_SDL3
static bool ensure_sdl_vulkan_loaded() {
  static std::once_flag loaded;
  static bool ok = false;
  std::call_once(loaded, [] {
    ok = SDL_Vulkan_LoadLibrary(nullptr);
  });
  return ok;
}
#endif

void append_vulkan_surface_instance_extensions(std::vector<const char*>& out) {
#ifdef HLE_WITH_SDL3
  if (SDL_WasInit(SDL_INIT_VIDEO) != 0 && ensure_sdl_vulkan_loaded()) {
    Uint32 count = 0;
    if (const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&count); exts && count > 0) {
      for (Uint32 i = 0; i < count; ++i) {
        out.push_back(exts[i]);
      }
      return;
    }
  }
#endif
  out.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
}

bool physical_device_presentation_support(VkInstance instance, VkPhysicalDevice phys, uint32_t queue_family) {
  if (phys == VK_NULL_HANDLE) return false;
#ifdef HLE_WITH_SDL3
  if (instance != VK_NULL_HANDLE && SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    if (!ensure_sdl_vulkan_loaded()) return false;
    return SDL_Vulkan_GetPresentationSupport(instance, phys, queue_family);
  }
#else
  (void)instance;
  (void)queue_family;
#endif
  return true;
}

bool window_pixel_size(void* native_window, uint32_t* out_w, uint32_t* out_h) {
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
#ifdef HLE_WITH_SDL3
  if (native_window && SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(native_window), &pw, &ph) && pw > 0 && ph > 0) {
      if (out_w) *out_w = static_cast<uint32_t>(pw);
      if (out_h) *out_h = static_cast<uint32_t>(ph);
      return true;
    }
  }
#else
  (void)native_window;
#endif
  return false;
}

bool write_watch_supported() { return false; }

bool query_write_watch(void* /*base*/, size_t /*size*/, bool /*reset*/, std::vector<void*>* out_addrs, uint32_t* out_error) {
  if (out_addrs) out_addrs->clear();
  if (out_error) *out_error = 0;
  return false;
}

bool create_vulkan_surface(VkInstance instance,
                           void* native_window,
                           const VkAllocationCallbacks* allocator,
                           VkSurfaceKHR* out_surface) {
  if (!out_surface) return false;
  *out_surface = VK_NULL_HANDLE;
  if (instance == VK_NULL_HANDLE || !native_window) return false;
#ifdef HLE_WITH_SDL3
  if (SDL_WasInit(SDL_INIT_VIDEO) == 0 || !ensure_sdl_vulkan_loaded()) return false;
  return SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(native_window), instance, allocator, out_surface);
#else
  (void)allocator;
  return false;
#endif
}

}  // namespace nvn::os_host
