// SPDX-License-Identifier: MIT
#include "fs_core.h"
#include "os/perf.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "hle_config.h"
#include "hle_host.h"
#include "save_gen.h"

// runtime/os releases the caller's virtual core around blocking reads. These no-op weak defaults keep builds without
// runtime/os (tests/hle) linking; snap_boot links the real ones from os_kernel.cpp.
extern "C" __attribute__((weak)) void os_io_begin(void) {}
extern "C" __attribute__((weak)) void os_io_end(void) {}

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// Set by runtime/mods/mods.cpp at start-up when an enabled pack has a romfs/ folder: those folders, highest priority first.
std::vector<std::string> (*g_snap_fs_extra_rom_layers)() = nullptr;

namespace hle::fs {

namespace stdfs = std::filesystem;

static stdfs::path to_path(const std::string& s) { return stdfs::u8path(s); }
static std::string from_path(const stdfs::path& p) {
  auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

// ---------------------------------------------------------------------------------------------------------------
// Test fault injection (tests/hle): the `count` flushes after the first `skip` ones fail with host error `os_error`.
static std::atomic<int> g_flush_skip{0}, g_flush_fail{0}, g_flush_err{0};
void test_fail_flushes(int skip, int count, int os_error) { g_flush_err = os_error; g_flush_skip = skip; g_flush_fail = count; }

// Failed save I/O of this process (a flush the disk refused): counted, the last text kept for the crash report and the problem report's
// log (snap_fs_log_io_failures, called from host/mods/crashlog.cpp). Static buffers: the crash path must not allocate.
static std::atomic<uint64_t> g_io_failures{0};
static char g_io_last[256];
static void note_io_failure(const char* what, const std::error_code& ec) {
  g_io_failures.fetch_add(1, std::memory_order_relaxed);
  std::snprintf(g_io_last, sizeof g_io_last, "%s: %s (host error %d)", what, ec.message().c_str(), ec.value());
  log("fs: save: %s", g_io_last);
}
uint64_t io_failure_count() { return g_io_failures.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------------------------------------------
// host file with positioned I/O
class HostFile {
 public:
  ~HostFile() { close(); }
  bool open(const stdfs::path& p, bool write) {
    h_ = hle::host::file_open(from_path(p), write);
    return h_ != nullptr;
  }
  void close() {
    if (h_) { hle::host::file_close(h_); h_ = nullptr; }
  }
  int64_t size() { return hle::host::file_size(h_); }
  // returns bytes read, -1 on error
  int64_t read_at(int64_t off, void* buf, size_t n) { return hle::host::file_read_at(h_, off, buf, n); }
  bool write_at(int64_t off, const void* buf, size_t n) { return hle::host::file_write_at(h_, off, buf, n); }
  bool set_size(int64_t size) { return hle::host::file_set_size(h_, size); }
  // false when the disk did not take the data; the host error is still in GetLastError()/errno for the caller to read at once
  bool flush() {
    if (g_flush_fail.load(std::memory_order_relaxed) > 0 && g_flush_skip.fetch_sub(1) <= 0) {
      g_flush_fail.fetch_sub(1);
      hle::host::set_last_os_error(g_flush_err.load());
      return false;
    }
    return hle::host::file_flush(h_);
  }

 private:
  void* h_ = nullptr;
};

// Atomically replaces `to` with `from` (same directory/volume), writing through.
static bool replace_file(const stdfs::path& from, const stdfs::path& to) {
  return hle::host::atomic_replace_file(from_path(from), from_path(to));
}

// True when nothing exists at `p` (not even a dangling link). An unreadable status counts as present.
static bool gone(const stdfs::path& p) {
  std::error_code ec;
  return stdfs::symlink_status(p, ec).type() == stdfs::file_type::not_found;
}
// Removes a file or tree and proves it is gone. remove_all can stop part-way (a file held open without FILE_SHARE_DELETE).
static bool remove_checked(const stdfs::path& p) {
  std::error_code ec;
  stdfs::remove_all(p, ec);
  return !ec && gone(p);
}

// The result a failed commit returns, by cause. Only a full disk is the game's "not enough space" error; every other cause (a
// file held by another program, a permission or I/O error) returns TargetLocked, which the game's result mapper also handles
// (main 0x60362c, game error 0x11), instead of telling the player the disk is full.
static uint32_t commit_result(const std::error_code& ec) {
  return hle::host::is_disk_full_error(ec) ? kResultUsableSpaceNotEnough : kResultTargetLocked;
}
static std::error_code last_os_error() {
  return hle::host::last_os_error();
}

// Flushes a file to disk by path (commit durability). False, with the host error in *ec, when the disk refused the flush: the caller
// must not publish what it just wrote. A file that cannot be reopened for the flush is logged and passes (it was written whole; the
// OS still writes it back, and a held file is the usual cause).
static bool flush_path(const stdfs::path& p, std::error_code* ec = nullptr) {
  HostFile f;
  if (!f.open(p, true)) { log("fs: save: cannot reopen %s to flush it; continuing without the explicit flush", from_path(p).c_str()); return true; }
  if (f.flush()) return true;
  const std::error_code e = last_os_error();   // before close() can touch it
  if (ec) *ec = e;
  note_io_failure(("flush of " + from_path(p)).c_str(), e);
  return false;
}

bool split_path(const std::string& path, std::string* mount, std::string* rel) {
  size_t c = path.find(':');
  if (c == std::string::npos || c == 0 || c > 32) return false;
  *mount = path.substr(0, c);
  std::string r = path.substr(c + 1);
  if (r.empty() || (r[0] != '/' && r[0] != '\\')) return false;
  std::string out, comp;
  auto flush_comp = [&]() -> bool {
    if (comp.empty() || comp == ".") { comp.clear(); return true; }
    if (comp == "..") return false;
    if (!out.empty()) out += '/';
    out += comp;
    comp.clear();
    return true;
  };
  for (char ch : r) {
    if (ch == '/' || ch == '\\') { if (!flush_comp()) return false; }
    else comp += ch;
  }
  if (!flush_comp()) return false;
  *rel = out;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
namespace {

struct OpenFile {
  HostFile hf;
  bool writable = false;
  std::string mount;
  std::string rel;          // save mounts: path inside the mount, so a write after a commit re-marks the file dirty
  std::string path;         // the guest path it was opened with (trace messages)
  struct Save* save = nullptr;  // owned by the mount; kept alive by keep below
  std::shared_ptr<void> keep;   // the owning Mount: a write that outlives unmount/remount still has a valid save for mark_dirty
  std::mutex mu;  // serializes write / resize on this handle
};
struct OpenDir {
  std::vector<DirEntry> entries;
  size_t cursor = 0;
};

constexpr const char* kPointerName = "CURRENT";

// The changes a commit writes: files changed, paths deleted and directories created since the last commit.
struct Pending {
  std::set<std::string> dirty, deleted, dirs;
};

// The save mount: committed generations on disk plus a working directory for the uncommitted changes.
//   <root>/gens/<n>/      complete state of generation n (immutable once committed)
//   <root>/gens/<n>.ok    written after generation n is complete
//   <root>/CURRENT        "SNAPSAVE1\n<n>\n<check>\n", replaced atomically at commit
//   <root>/working/       copy-on-write scratch of the running session; wiped at every mount
struct Save {
  stdfs::path root;
  uint64_t cur = 0;
  int keep = 5;
  std::string lock_key;  // key of the folder lock in Fs::Impl::locks (empty: none)
  std::set<std::string> dirty, deleted, dirs;  // dirs: directories created since the last commit
  std::string work_name = "working";           // "working", or "working.<k>" when an old one could not be removed
  std::set<uint64_t> orphans;                  // complete but never published (marker stuck after a failed commit)
  // Fs::commit builds the generation without the fs lock (from a copy of the pending sets). Changes made meanwhile go to the live
  // sets and to the journal; a published commit makes the journal the new pending sets, a failed one drops it (the live sets
  // still hold everything). The journal marks every removed path deleted: the generation being built may hold it.
  bool committing = false;
  Pending journal;
  void add_dirty(const std::string& r) { dirty.insert(r); if (committing) journal.dirty.insert(r); }
  void drop_dirty(const std::string& r) { dirty.erase(r); if (committing) journal.dirty.erase(r); }
  void add_dir(const std::string& r) { dirs.insert(r); if (committing) journal.dirs.insert(r); }
  void drop_dir(const std::string& r) { dirs.erase(r); if (committing) journal.dirs.erase(r); }
  void undelete(const std::string& r) { deleted.erase(r); if (committing) journal.deleted.erase(r); }
  void mark_deleted(const std::string& r) {   // a removed path counts as deleted when the committed generation has it
    std::error_code ec;
    if (stdfs::exists(committed() / to_path(r), ec)) deleted.insert(r);
    if (committing) journal.deleted.insert(r);
  }

  stdfs::path gens() const { return root / "gens"; }
  stdfs::path gen_dir(uint64_t n) const { return gens() / std::to_string(n); }
  stdfs::path gen_ok(uint64_t n) const { return gens() / (std::to_string(n) + ".ok"); }
  // <n>.orphan: written when a failed commit cannot remove <n>.ok; such a generation is not valid (removed with it)
  stdfs::path gen_orphan(uint64_t n) const { return gens() / (std::to_string(n) + ".orphan"); }
  stdfs::path working() const { return root / work_name; }

  // Generation numbers present under gens/ (directories and markers alike).
  std::set<uint64_t> gen_numbers() const {
    std::set<uint64_t> v;
    std::error_code ec;
    for (stdfs::directory_iterator it(gens(), ec), end; !ec && it != end; it.increment(ec)) {
      const std::string stem = from_path(it->path().stem());
      if (stem.empty() || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
      v.insert(std::strtoull(stem.c_str(), nullptr, 10));
    }
    return v;
  }
  // Removes a generation marker first, then its directory, so a partly removed directory never sits under a valid marker.
  // A marker that cannot be removed gets an orphan mark beside it, so the generation stops counting either way.
  // The orphan mark is written before the marker is touched, so neither a stuck marker nor a crash part-way through leaves a
  // valid-looking generation behind. If the mark itself cannot be written, the in-memory set still covers this session.
  bool mark_orphan(uint64_t g) {
    orphans.insert(g);
    {
      std::ofstream f(gen_orphan(g), std::ios::binary | std::ios::trunc);
      f << "orphan\n";
      f.flush();
      if (!f) { log("fs: save: cannot write the orphan mark %s", from_path(gen_orphan(g)).c_str()); return false; }
    }
    flush_path(gen_orphan(g));
    return true;
  }
  bool drop_generation(uint64_t g) {
    if (!gone(gen_ok(g))) mark_orphan(g);
    if (!remove_checked(gen_ok(g))) {
      log("fs: save: cannot remove %s; generation %llu is kept whole but orphaned", from_path(gen_ok(g)).c_str(), (unsigned long long)g);
      return false;
    }
    if (!remove_checked(gen_dir(g))) { log("fs: save: cannot fully remove %s (in use?); it has no marker and is never loaded", from_path(gen_dir(g)).c_str()); return false; }
    std::error_code ec;
    stdfs::remove(gen_orphan(g), ec);   // last: harmless once the marker is gone (fresh_generation clears a leftover)
    return true;
  }
  // A new, empty scratch directory "working" or "working.<k>" (k from 1) under root; false if none can be created.
  bool fresh_working(std::string* name) const {
    for (int k = 0; k < 64; ++k) {
      std::string nm = k ? "working." + std::to_string(k) : std::string("working");
      std::error_code ec;
      if (stdfs::create_directory(root / nm, ec) && !ec) { *name = nm; return true; }
    }
    return false;
  }
  stdfs::path committed() const { return gen_dir(cur); }

  static uint32_t check_of(uint64_t n) { return (uint32_t)(n * 2654435761u) ^ 0x534e4150u; }

  bool read_pointer(uint64_t* n) const { return savegen::read_pointer_file(root / kPointerName, n); }
  bool write_pointer(uint64_t n, const std::function<void(const char*)>& hook, std::error_code* why = nullptr) const {
    stdfs::path tmp = root / "CURRENT.tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) { if (why) *why = last_os_error(); return false; }
      f << "SNAPSAVE1\n" << n << "\n" << check_of(n) << "\n";
    }
    if (!flush_path(tmp, why)) return false;   // the pointer is never switched to a file the disk did not take
    if (hook) hook("commit:pointer-tmp");
    if (!replace_file(tmp, root / kPointerName)) { if (why) *why = last_os_error(); return false; }
    if (hook) hook("commit:pointer-done");
    return true;
  }
  bool gen_valid(uint64_t n) const { return !orphans.count(n) && savegen::gen_valid(root, n); }   // shared rule (save_gen.h)

  // Finds the newest complete generation at or below `limit` (no limit: any).
  bool scan(uint64_t* out) const { return savegen::newest_valid(root, out, [&](uint64_t n) { return orphans.count(n) != 0; }); }

  // Opens the save: finds the committed generation, drops every uncommitted or half-built leftover.
  bool open(int keep_gens) {
    keep = keep_gens;
    std::error_code ec;
    stdfs::create_directories(gens(), ec);
    uint64_t n = 0;
    // CURRENT, else a valid CURRENT.tmp (the interrupted replace), else the newest valid generation: the rule profile copies use
    bool have = savegen::committed_gen(root, &n, nullptr, [&](uint64_t g) { return orphans.count(g) != 0; });
    if (!have) {
      // no save: an empty generation, built on a number whose leftovers (if any) are gone, never on top of them
      n = 0;
      for (uint64_t g = 0; g < 64; ++g) {
        if (!drop_generation(g)) continue;
        std::error_code e1;
        if (!stdfs::create_directory(gen_dir(g), e1) || e1) continue;
        n = g;
        break;
      }
      orphans.erase(n);
      std::ofstream(gen_ok(n)) << "ok\n";
    }
    cur = n;
    // leftovers newer than the committed generation never became current: an interrupted commit. One that cannot be removed
    // (a file held open) stays without a marker; the next commit skips its number.
    for (uint64_t g : gen_numbers()) if (g > cur) drop_generation(g);
    stdfs::remove(root / "CURRENT.tmp", ec);
    if (!read_pointer(&n) || n != cur) {
      std::function<void(const char*)> none;
      if (!write_pointer(cur, none)) log("fs: save: cannot rewrite %s (generation %llu stays the newest complete one)", from_path(root / kPointerName).c_str(), (unsigned long long)cur);
    }
    // working/ (and working.<k> from earlier runs) hold the last session's uncommitted changes: all are dropped. If one cannot be
    // removed it must not be reused (its files would show up as this session's), so a fresh name is taken.
    std::vector<stdfs::path> old_work;
    for (stdfs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string nm = from_path(it->path().filename());
      if (nm == "working" || nm.rfind("working.", 0) == 0) old_work.push_back(it->path());
    }
    for (auto& p : old_work)
      if (!remove_checked(p)) log("fs: save: cannot remove the old scratch folder %s; using a new one", from_path(p).c_str());
    dirty.clear(); deleted.clear(); dirs.clear();
    if (!fresh_working(&work_name)) { log("fs: save: cannot create a scratch folder in %s", from_path(root).c_str()); return false; }
    return true;
  }

  // 0 none, 1 directory, 2 file
  int kind(const std::string& rel) const {
    if (rel.empty()) return 1;
    if (deleted.count(rel)) return 0;
    std::error_code ec;
    stdfs::path w = working() / to_path(rel);
    if (stdfs::exists(w, ec)) return stdfs::is_directory(w, ec) ? 1 : 2;
    stdfs::path c = committed() / to_path(rel);
    if (stdfs::exists(c, ec)) return stdfs::is_directory(c, ec) ? 1 : 2;
    return 0;
  }
  // The host path to read `rel` from (working copy if there is one).
  stdfs::path read_path(const std::string& rel) const {
    std::error_code ec;
    stdfs::path w = working() / to_path(rel);
    if (stdfs::exists(w, ec)) return w;
    return committed() / to_path(rel);
  }
  // Makes sure the file has a working copy and returns it.
  bool ensure_working(const std::string& rel, stdfs::path* out) {
    std::error_code ec;
    stdfs::path w = working() / to_path(rel);
    stdfs::create_directories(w.parent_path(), ec);
    if (!stdfs::exists(w, ec)) {
      stdfs::path c = committed() / to_path(rel);
      if (!deleted.count(rel) && stdfs::exists(c, ec) && stdfs::is_regular_file(c, ec)) {
        stdfs::copy_file(c, w, stdfs::copy_options::overwrite_existing, ec);
        if (ec) return false;
      }
    }
    add_dirty(rel);
    *out = w;
    return true;
  }

  std::vector<DirEntry> list(const std::string& rel) const {
    std::map<std::string, DirEntry> m;
    std::error_code ec;
    auto add = [&](const stdfs::path& dir) {
      for (auto& e : stdfs::directory_iterator(dir, ec)) {
        std::string name = from_path(e.path().filename());
        std::string child = rel.empty() ? name : rel + "/" + name;
        if (deleted.count(child)) continue;
        DirEntry d;
        d.name = name;
        std::error_code e2;
        d.type = e.is_directory(e2) ? kDirectory : kFile;
        d.size = d.type == kFile ? (int64_t)e.file_size(e2) : 0;
        m[name] = d;  // working (added second) overrides committed
      }
    };
    stdfs::path c = committed() / to_path(rel), w = working() / to_path(rel);
    if (stdfs::is_directory(c, ec)) add(c);
    if (stdfs::is_directory(w, ec)) add(w);
    std::vector<DirEntry> out;
    for (auto& kv : m) out.push_back(kv.second);
    return out;
  }

  // Copies (hard-links where possible) every unchanged file of the committed generation into the new one. Any failure is returned:
  // a generation missing a carried file must never be published.
  bool carry(const stdfs::path& from, const stdfs::path& to, const std::string& rel, std::error_code& why, const Pending& pd) {
    const auto& dirty = pd.dirty;
    const auto& deleted = pd.deleted;
    const auto& dirs = pd.dirs;
    std::error_code ec;
    stdfs::directory_iterator it(from, ec);
    if (ec) { why = ec; return false; }
    for (const stdfs::directory_iterator end; it != end; it.increment(ec)) {
      if (ec) { why = ec; return false; }
      const auto& e = *it;
      std::string name = from_path(e.path().filename());
      std::string child = rel.empty() ? name : rel + "/" + name;
      if (deleted.count(child)) continue;
      std::error_code e2;
      const bool isdir = e.is_directory(e2);
      if (e2) { why = e2; return false; }
      // replaced this session by an entry of the other type (file -> directory or directory -> file, also below a replaced
      // parent): the session's entry is in working/ and is written by the dirty / dirs passes; the old one is not carried
      const stdfs::file_type wt = stdfs::symlink_status(working() / to_path(child), e2).type();
      if (wt == stdfs::file_type::none || wt == stdfs::file_type::unknown) { why = e2 ? e2 : std::make_error_code(std::errc::io_error); return false; }
      if (wt != stdfs::file_type::not_found && (wt == stdfs::file_type::directory) != isdir) continue;
      if (isdir ? dirty.count(child) != 0 : dirs.count(child) != 0) continue;   // same, from the session's own records
      e2.clear();
      if (isdir) {
        stdfs::create_directories(to / name, e2);
        if (e2) { why = e2; return false; }
        if (!carry(e.path(), to / name, child, why, pd)) return false;
      } else if (!dirty.count(child)) {
        // `to` is a directory this commit created itself, so the name is new; copy_options::none refuses an existing entry
        // instead of writing through it (an existing entry could be a hard link into a committed generation).
        stdfs::create_hard_link(e.path(), to / name, e2);
        if (e2) {
          e2.clear();
          stdfs::copy_file(e.path(), to / name, stdfs::copy_options::none, e2);
          if (e2) { why = e2; return false; }
          if (!flush_path(to / name, &e2)) { why = e2; return false; }   // a real copy must be on disk before the .ok marker, like every written file
        }
      }
    }
    if (ec) why = ec;
    return !ec;
  }

  // Picks the number for the next generation and creates its directory fresh. A leftover gens/<n> (an interrupted or failed
  // commit whose cleanup was blocked, e.g. a file held open by a scanner) is never built on: its entries may be hard links into
  // committed generations (writing to one writes through into CURRENT and the backups) and may hold files deleted since. When
  // it cannot be fully removed, the next unused number is taken; the leftover never has a .ok marker, so it is never loaded.
  bool fresh_generation(uint64_t* out) {
    std::error_code ec;
    stdfs::create_directories(gens(), ec);
    for (uint64_t n = cur + 1, tries = 0; tries < 64; ++n, ++tries) {
      if (!remove_checked(gen_ok(n))) { log("fs: save: cannot remove the stale marker %s; skipping generation %llu", from_path(gen_ok(n)).c_str(), (unsigned long long)n); continue; }
      if (!remove_checked(gen_dir(n))) { log("fs: save: cannot remove the stale %s; skipping generation %llu", from_path(gen_dir(n)).c_str(), (unsigned long long)n); continue; }
      if (!remove_checked(gen_orphan(n))) continue;   // a leftover orphan mark would make the new generation invalid
      orphans.erase(n);
      std::error_code e1;
      if (!stdfs::create_directory(gen_dir(n), e1) || e1) continue;   // false: something re-created it; never reuse an existing directory
      *out = n;
      return true;
    }
    return false;
  }

  template <class PublishFn>
  uint32_t do_commit(const std::function<void(const char*)>& hook, const Pending& pd, PublishFn&& publish) {
    auto stage = [&](const char* s) { if (hook) hook(s); };
    std::error_code ec;
    stage("commit:begin");
    uint64_t n = 0;
    if (!fresh_generation(&n)) { log("fs: save: no free generation number in %s (stale generations held open)", from_path(gens()).c_str()); return kResultTargetLocked; }
    stdfs::path ng = gen_dir(n);
    // From here a failure removes the half-built generation and returns an error; CURRENT keeps pointing at the old one.
    // If the marker cannot be removed the generation is complete (the marker is written last), so it stays whole rather than
    // torn under a valid marker, but it is marked orphaned: never listed, restored, loaded or counted as a backup.
    // A directory that cannot be removed is skipped by the next commit (fresh_generation).
    auto fail = [&](const std::error_code& why, const char* what) {
      const uint32_t rc = commit_result(why);
      log("fs: save: commit failed at %s: %s (result %#x)", what, why.message().c_str(), rc);
      // drop_generation marks it orphaned first when the .ok was already written; a folder it cannot remove is skipped later
      if (!drop_generation(n)) log("fs: save: the failed generation %llu could not be fully removed; the next commit uses a new number", (unsigned long long)n);
      return rc;
    };
    if (!carry(committed(), ng, "", ec, pd)) return fail(ec, "carry");
    stage("commit:carried");
    for (const std::string& rel : pd.dirty) {
      stdfs::path src = working() / to_path(rel), dst = ng / to_path(rel);
      const bool have = stdfs::exists(src, ec);
      if (ec) return fail(ec, "file status");   // unknown is not "deleted": never drop a file on an I/O error
      if (!have) continue;  // created then deleted in the same session
      stdfs::create_directories(dst.parent_path(), ec);
      if (ec) return fail(ec, "directory");
      // never write through an existing entry (it could be a hard link into a committed generation): remove it, prove it is
      // gone, then create the file new
      if (!gone(dst) && !remove_checked(dst)) return fail(std::make_error_code(std::errc::device_or_resource_busy), "replace");
      stdfs::copy_file(src, dst, stdfs::copy_options::none, ec);
      if (ec) return fail(ec, "file copy");
      if (!flush_path(dst, &ec)) return fail(ec, "file flush");   // the previous generation stays current
      stage("commit:file");
    }
    for (const std::string& rel : pd.dirs) {
      stdfs::create_directories(ng / to_path(rel), ec);
      if (ec) return fail(ec, "directory");
    }
    stage("commit:files-done");
    {
      std::ofstream f(gen_ok(n), std::ios::binary);
      f << "ok\n";
      f.flush();
      if (!f) return fail(std::error_code(errno ? errno : EIO, std::generic_category()), "marker");
    }
    if (!flush_path(gen_ok(n), &ec)) return fail(ec, "marker flush");
    stage("commit:marker");
    std::error_code pe;
    if (!write_pointer(n, hook, &pe)) return fail(pe ? pe : last_os_error(), "pointer");
    orphans.erase(n);
    publish(n);
    // automatic backups: keep the current generation and the `keep` newest complete ones before it (numbers can have gaps
    // where a stale leftover was skipped); everything else below the current one goes, leftovers included
    if (keep >= 0) {
      std::set<uint64_t> nums, valid;
      for (uint64_t g : gen_numbers()) if (g < cur) { nums.insert(g); if (gen_valid(g)) valid.insert(g); }
      int kept = 0;
      for (auto it = valid.rbegin(); it != valid.rend() && kept < keep; ++it, ++kept) nums.erase(*it);
      for (uint64_t g : nums) drop_generation(g);
    }
    return kSuccess;
  }
};

struct Mount {
  enum Kind { Rom, SaveData, Host } kind = Host;
  bool writable = false;
  std::vector<stdfs::path> layers;  // rom/host
  std::unique_ptr<Save> save;
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
struct Fs::Impl {
  // Lock order, and the virtual-core rule that keeps it deadlock free:
  //  1. commit_mu, then mu. Never take commit_mu while holding mu.
  //  2. of->mu (an open file's write lock), then mu. A thread holding mu never takes of->mu.
  //  3. Never take a virtual core back (os_io_end) while holding commit_mu, mu or of->mu. Most callers wait for mu while holding
  //     their core, so a mu holder that waits for a core can wait forever once every core is held by a thread queued on mu.
  //     Waiting for a lock with the core released is fine: os_io_begin, lock, work, unlock, then os_io_end (get_file,
  //     PathOpLock, commit).
  std::mutex mu;  // mounts and tables
  // Serializes commits, restores, save mounts and unmounts. Taken before mu, never while holding it. A commit holds it for the
  // whole disk work and mu only to copy and publish the pending sets. Path-changing operations on a save mount wait for it
  // while that mount commits (PathOpLock): the commit reads working/ by name, so only data reads and writes (and opens,
  // which add to the commit's journal) run alongside it.
  std::mutex commit_mu;
  std::map<std::string, std::shared_ptr<Mount>> mounts;
  std::unordered_map<uint64_t, std::shared_ptr<OpenFile>> files;
  std::unordered_map<uint64_t, std::shared_ptr<OpenDir>> dirs;
  std::atomic<uint64_t> next{0x1000};
  // Save states (state_save / state_load): the guest keeps the ids of the files and directories it had open at the save. While a state exists,
  // a close of one of those ids parks the handle here instead of dropping it, so a load can open it again; handles opened since are closed.
  bool st_valid = false;
  std::set<uint64_t> st_files, st_dirs;
  std::unordered_map<uint64_t, size_t> st_cursors;
  std::unordered_map<uint64_t, std::shared_ptr<OpenFile>> st_zfiles;
  std::unordered_map<uint64_t, std::shared_ptr<OpenDir>> st_zdirs;
  std::vector<std::string> rom_layers;
  std::string save_root;
  int save_backups = 5;
  std::function<void(const char*)> hook;
  std::map<std::string, void*> locks;  // save root -> open ".snap.lock" handle holding an exclusive byte lock (one running game per save folder)

  ~Impl() {
    for (auto& kv : locks) hle::host::release_save_lock(kv.second);
  }
  // Drops the folder lock once no save mount uses it any more (unmount, remount).
  void release_lock(const std::string& key) {
    if (key.empty()) return;
    for (auto& kv : mounts) if (kv.second->save && kv.second->save->lock_key == key) return;
    if (auto it = locks.find(key); it != locks.end()) {
      hle::host::release_save_lock(it->second);
      locks.erase(it);
    }
  }
  std::shared_ptr<Mount> find_mount(const std::string& name) {
    auto it = mounts.find(name);
    return it == mounts.end() ? nullptr : it->second;
  }
  uint64_t token() { return next.fetch_add(0x10); }
};

Fs::Fs() : p_(new Impl) {
  Config& c = config();
  p_->save_root = c.save_root;
  p_->save_backups = c.save_backups;
  std::vector<std::string> l = c.rom_layers;
  l.push_back(c.romfs_root);
  p_->rom_layers = std::move(l);
}
Fs::~Fs() = default;

void Fs::set_rom_layers(std::vector<std::string> dirs) {
  std::lock_guard<std::mutex> g(p_->mu);
  p_->rom_layers = std::move(dirs);
}
std::vector<std::string> Fs::rom_layers() const {
  std::lock_guard<std::mutex> g(p_->mu);
  return p_->rom_layers;
}
void Fs::set_save_root(const std::string& dir) { std::lock_guard<std::mutex> g(p_->mu); p_->save_root = dir; }
void Fs::set_save_backups(int n) { std::lock_guard<std::mutex> g(p_->mu); p_->save_backups = n; }
void Fs::set_fault_hook(std::function<void(const char*)> hook) { std::lock_guard<std::mutex> g(p_->mu); p_->hook = std::move(hook); }

uint32_t Fs::mount_rom(const std::string& name) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = std::make_shared<Mount>();
  m->kind = Mount::Rom;
  std::error_code ec;
  // Enabled mod packs with a romfs/ folder (whole-file overrides, e.g. an archive with added entries) go first. Null when no such
  // pack is enabled, so the default layer list is unchanged.
  std::vector<std::string> layers;
  if (g_snap_fs_extra_rom_layers) layers = g_snap_fs_extra_rom_layers();
  layers.insert(layers.end(), p_->rom_layers.begin(), p_->rom_layers.end());
  for (auto& d : layers) {
    stdfs::path p = to_path(d);
    if (stdfs::is_directory(p, ec)) m->layers.push_back(p);
    else log("fs: rom layer %s does not exist; skipped", d.c_str());
  }
  if (m->layers.empty()) { log("fs: no rom layer exists; MountRom(%s) fails", name.c_str()); return kResultPathNotFound; }
  p_->mounts[name] = m;
  if (trace_flags & kTraceFs) log("fs: mounted %s: with %zu layer(s), first %s", name.c_str(), m->layers.size(), from_path(m->layers[0]).c_str());
  return kSuccess;
}

uint32_t Fs::mount_host(const std::string& name, const std::string& dir, bool writable) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = std::make_shared<Mount>();
  m->kind = Mount::Host;
  m->writable = writable;
  std::error_code ec;
  stdfs::path p = to_path(dir);
  if (writable) stdfs::create_directories(p, ec);
  if (!stdfs::is_directory(p, ec)) return kResultPathNotFound;
  m->layers.push_back(p);
  p_->mounts[name] = m;
  return kSuccess;
}

uint32_t Fs::mount_save(const std::string& name, const uint64_t uid[2]) {
  std::lock_guard<std::mutex> cg(p_->commit_mu);
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = std::make_shared<Mount>();
  m->kind = Mount::SaveData;
  m->writable = true;
  m->save = std::make_unique<Save>();
  m->save->root = to_path(p_->save_root) / to_path(uid_dir_name(uid));
  std::error_code ec;
  // Same name mounted again: the old mount stays untouched until the new one can no longer fail (lock, directory); only then is it
  // dropped (its open handles die with it; Save::open wipes working/ anyway). The new mount takes over the same folder lock.
  // One running game per save folder. Save::open deletes working/ and every newer generation, which would wreck a second live
  // process on the same folder, so refuse before touching anything. Per save root (parallel test runs use their own roots);
  // the lock dies with the process, so a crash never leaves a stale one.
  {
    stdfs::path root = to_path(p_->save_root);
    stdfs::create_directories(root, ec);
    const std::string key = from_path(root);
    m->save->lock_key = key;
    if (!p_->locks.count(key)) {
      void* lock_h = nullptr;
      const int lock_rc = hle::host::try_acquire_save_lock(from_path(root / ".snap.lock"), &lock_h);
      if (lock_rc != 0) {
        log("fs: save folder %s is locked or unavailable (rc=%d); not mounting it", key.c_str(), lock_rc);
        return lock_rc == 1 ? kResultTargetLocked : kResultUsableSpaceNotEnough;
      }
      p_->locks[key] = lock_h;
    }
  }
  stdfs::create_directories(m->save->root, ec);
  if (ec) { log("fs: cannot create save directory %s: %s", from_path(m->save->root).c_str(), ec.message().c_str()); return kResultUsableSpaceNotEnough; }
  if (auto old = p_->mounts.find(name); old != p_->mounts.end()) {
    for (auto f = p_->files.begin(); f != p_->files.end();) { if (f->second->mount == name) f = p_->files.erase(f); else ++f; }
    p_->mounts.erase(old);   // OpenFile::keep holds the Save of a handle a writer still has in flight
  }
  if (!m->save->open(p_->save_backups)) {
    p_->release_lock(m->save->lock_key);
    return kResultUsableSpaceNotEnough;
  }
  p_->mounts[name] = m;
  if (trace_flags & kTraceFs) log("fs: mounted %s: save %s generation %llu", name.c_str(), from_path(m->save->root).c_str(), (unsigned long long)m->save->cur);
  return kSuccess;
}

uint32_t Fs::unmount(const std::string& name) {
  std::lock_guard<std::mutex> cg(p_->commit_mu);
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->mounts.find(name);
  if (it == p_->mounts.end()) return kResultNotMounted;
  for (auto f = p_->files.begin(); f != p_->files.end();) {
    if (f->second->mount == name) f = p_->files.erase(f); else ++f;
  }
  const std::string lock_key = it->second->save ? it->second->save->lock_key : std::string();
  p_->mounts.erase(it);  // uncommitted save changes are dropped, as on hardware
  p_->release_lock(lock_key);
  return kSuccess;
}

bool Fs::is_mounted(const std::string& name) {
  std::lock_guard<std::mutex> g(p_->mu);
  return p_->mounts.count(name) != 0;
}

// ---- path resolution helpers (caller holds the lock)
namespace {
struct Resolved {
  std::shared_ptr<Mount> m;
  std::string rel;
  stdfs::path host;  // for layered mounts: first layer holding rel (or the first layer if none)
  int kind = 0;      // 0 none, 1 dir, 2 file
};
}  // namespace

// Lock for a path-changing operation (create, delete, rename, directory create and delete). Holds mu. The commit builds the new
// generation outside mu and copies its dirty files from working/ by name, so a delete or rename on the mount it is committing
// could publish a generation without the file, or fail the commit. retry(m) is called right before the change: when m is a save
// mount with a commit running it drops mu, gives up the virtual core, waits for commit_mu (the commit has ended), takes mu
// again and returns true; the caller then resolves its paths again. The core comes back only after both locks are released.
namespace {
struct PathOpLock {
  bool io = false;
  std::unique_lock<std::mutex> cg, g;
  explicit PathOpLock(Fs::Impl* p) : cg(p->commit_mu, std::defer_lock), g(p->mu) {}
  PathOpLock(const PathOpLock&) = delete;
  PathOpLock& operator=(const PathOpLock&) = delete;
  bool retry(const std::shared_ptr<Mount>& m) {
    if (!m || !m->save || !m->save->committing || cg.owns_lock()) return false;
    g.unlock();
    if (!io) { os_io_begin(); io = true; }
    cg.lock();   // commit_mu before mu (rule 1); no commit can start while it is held, so an operation retries once at most
    g.lock();
    return true;
  }
  ~PathOpLock() {
    if (g.owns_lock()) g.unlock();
    if (cg.owns_lock()) cg.unlock();
    if (io) os_io_end();   // rule 3: after both locks are released
  }
};
}  // namespace

static uint32_t resolve(Fs::Impl* p, const std::string& path, Resolved* r) {
  std::string mount;
  if (!split_path(path, &mount, &r->rel)) return kResultInvalidArgument;
  r->m = p->find_mount(mount);
  if (!r->m) return kResultNotMounted;
  std::error_code ec;
  if (r->m->kind == Mount::SaveData) {
    r->kind = r->m->save->kind(r->rel);
    r->host = r->m->save->read_path(r->rel);
    return kSuccess;
  }
  r->host = r->m->layers[0] / to_path(r->rel);
  r->kind = 0;
  for (auto& layer : r->m->layers) {
    stdfs::path h = layer / to_path(r->rel);
    if (stdfs::exists(h, ec)) {
      r->host = h;
      r->kind = stdfs::is_directory(h, ec) ? 1 : 2;
      break;
    }
  }
  if (r->rel.empty()) r->kind = 1;
  return kSuccess;
}

uint32_t Fs::open_file(FileId* out, const std::string& path, int mode) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  bool want_write = (mode & (kModeWrite | kModeAppend)) != 0;
  if (want_write && !r.m->writable) return kResultWriteNotPermitted;
  if (r.kind != 2) return kResultPathNotFound;
  auto of = std::make_shared<OpenFile>();
  of->writable = want_write;
  std::string mount, rel;
  split_path(path, &mount, &rel);
  of->mount = mount;
  of->path = path;
  if (want_write && r.m->kind == Mount::SaveData) { of->rel = r.rel; of->save = r.m->save.get(); of->keep = r.m; }
  stdfs::path host = r.host;
  if (want_write) {
    if (r.m->kind == Mount::SaveData) {
      if (!r.m->save->ensure_working(r.rel, &host)) return kResultUsableSpaceNotEnough;
    }
  }
  if (!of->hf.open(host, want_write)) return kResultTargetLocked;
  uint64_t t = p_->token();
  p_->files[t] = of;
  *out = t;
  if (trace_flags & (kTraceFs | kTraceOpen)) log("fs: open %s mode %d -> %s (file %llu)", path.c_str(), mode, from_path(host).c_str(), (unsigned long long)t);
  return kSuccess;
}

uint32_t Fs::close_file(FileId f) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->files.find(f);
  if (it == p_->files.end()) return kResultInvalidArgument;
  if (p_->st_valid && p_->st_files.count(f)) p_->st_zfiles[f] = it->second;   // open at the save: a load brings it back
  p_->files.erase(it);
  if (trace_flags & kTraceFs) log("fs: close file %llu", (unsigned long long)f);
  return kSuccess;
}

// A save state is being taken (every guest thread is parked): remember which ids are open and where the directory cursors are.
void Fs::state_save() {
  std::lock_guard<std::mutex> g(p_->mu);
  p_->st_files.clear(); p_->st_dirs.clear(); p_->st_cursors.clear(); p_->st_zfiles.clear(); p_->st_zdirs.clear();
  for (auto& kv : p_->files) p_->st_files.insert(kv.first);
  for (auto& kv : p_->dirs) { p_->st_dirs.insert(kv.first); p_->st_cursors[kv.first] = kv.second->cursor; }
  p_->st_valid = true;
}

// The guest memory of the save is back: close what was opened since, reopen what was closed since, rewind the directory cursors.
// Returns how many handles were closed / reopened (for the log).
void Fs::state_load(int* closed, int* reopened) {
  std::lock_guard<std::mutex> g(p_->mu);
  int c = 0, r = 0;
  if (!p_->st_valid) { if (closed) *closed = 0; if (reopened) *reopened = 0; return; }
  for (auto it = p_->files.begin(); it != p_->files.end();) {
    if (!p_->st_files.count(it->first)) { it = p_->files.erase(it); ++c; } else ++it;
  }
  for (auto& kv : p_->st_zfiles) if (!p_->files.count(kv.first)) { p_->files[kv.first] = kv.second; ++r; }
  p_->st_zfiles.clear();
  for (auto it = p_->dirs.begin(); it != p_->dirs.end();) {
    if (!p_->st_dirs.count(it->first)) { it = p_->dirs.erase(it); ++c; } else ++it;
  }
  for (auto& kv : p_->st_zdirs) if (!p_->dirs.count(kv.first)) { p_->dirs[kv.first] = kv.second; ++r; }
  p_->st_zdirs.clear();
  for (auto& kv : p_->dirs) { auto cu = p_->st_cursors.find(kv.first); if (cu != p_->st_cursors.end()) kv.second->cursor = cu->second; }
  if (closed) *closed = c;
  if (reopened) *reopened = r;
}

static std::shared_ptr<OpenFile> get_file(Fs::Impl* p, FileId f) {
  // Contended (another thread is in a path lookup or a directory operation): wait without the virtual core, like a Horizon IPC wait.
  // The core comes back only after mu is released (lock-order rule 3 at Fs::Impl).
  std::unique_lock<std::mutex> g(p->mu, std::try_to_lock);
  if (g.owns_lock()) {
    auto it = p->files.find(f);
    return it == p->files.end() ? nullptr : it->second;
  }
  os_io_begin();
  g.lock();
  auto it = p->files.find(f);
  std::shared_ptr<OpenFile> of = it == p->files.end() ? nullptr : it->second;
  g.unlock();
  os_io_end();
  return of;
}

uint32_t Fs::read_file(FileId f, int64_t off, void* buf, size_t n, size_t* out) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  if (off < 0) return kResultOutOfRange;
  if (n == 0) { if (out) *out = 0; return kSuccess; }
  int64_t got;
  os_io_begin();   // the read releases the caller's virtual core (os review finding 9; SNAP_SCHED_LEGACY=hostblock keeps it)
  if (snap_perf::on) {   // perf pass 3: time file reads
    const uint64_t t0 = snap_perf::now_ns();
    got = of->hf.read_at(off, buf, n);
    snap_perf::wait_add(snap_perf::WT_FS, snap_perf::now_ns() - t0);
    if (got > 0) snap_perf::g_fs_bytes.fetch_add((uint64_t)got, std::memory_order_relaxed);
  } else {
    got = of->hf.read_at(off, buf, n);
  }
  os_io_end();
  if (trace_flags & kTraceFs) log("fs: read file %llu off %lld n %zu -> %lld", (unsigned long long)f, (long long)off, n, (long long)got);
  if ((got < 0 || (size_t)got != n) && (trace_flags & (kTraceFs | kTraceOpen)))
    log("fs: read file %llu (%s) off %lld n %zu -> %lld SHORT/FAILED", (unsigned long long)f, of->path.c_str(), (long long)off, n, (long long)got);
  if (got < 0) return kResultTargetLocked;
  if (out) { *out = (size_t)got; return kSuccess; }
  return (size_t)got == n ? kSuccess : kResultOutOfRange;
}

// A handle that stays open across Commit keeps writing to the working copy; do_commit cleared the dirty set, so every write
// re-marks its file (after the write, so a commit that runs in between still sees the file dirty next time). Lock order: of->mu, then p->mu
// (commit holds p->mu and never takes of->mu).
static void mark_dirty(Fs::Impl* p, OpenFile& of) {
  if (!of.save) return;
  std::lock_guard<std::mutex> g(p->mu);
  of.save->add_dirty(of.rel);
}

uint32_t Fs::write_file(FileId f, int64_t off, const void* buf, size_t n, bool flush) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  if (!of->writable) return kResultWriteNotPermitted;
  if (off < 0) return kResultOutOfRange;
  std::lock_guard<std::mutex> g(of->mu);
  // Horizon needs the file to be created with its final size (WriteFile does not grow it unless OpenMode_AllowAppend).
  // The host grows it either way: the game only ever writes inside the size it created.
  if (n && !of->hf.write_at(off, buf, n)) return kResultUsableSpaceNotEnough;
  mark_dirty(p_.get(), *of);   // before the flush result: the bytes are in the working copy whether or not the disk took the flush
  if (flush && !of->hf.flush()) {
    const std::error_code e = last_os_error();
    note_io_failure("flush after write", e);
    return commit_result(e);
  }
  if (trace_flags & kTraceFs) log("fs: write file %llu off %lld n %zu", (unsigned long long)f, (long long)off, n);
  return kSuccess;
}

uint32_t Fs::flush_file(FileId f) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  if (of->writable) {
    std::lock_guard<std::mutex> g(of->mu);
    if (!of->hf.flush()) {
      const std::error_code e = last_os_error();
      note_io_failure("flush of an open file", e);
      return commit_result(e);   // every write marked the file dirty, so the next commit still carries it
    }
  }
  return kSuccess;
}

uint32_t Fs::get_file_size(FileId f, int64_t* out) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  int64_t s = of->hf.size();
  if (s < 0) return kResultTargetLocked;
  *out = s;
  return kSuccess;
}

uint32_t Fs::set_file_size(FileId f, int64_t size) {
  auto of = get_file(p_.get(), f);
  if (!of) return kResultInvalidArgument;
  if (!of->writable) return kResultWriteNotPermitted;
  if (size < 0) return kResultOutOfRange;
  std::lock_guard<std::mutex> g(of->mu);
  if (!of->hf.set_size(size)) return kResultUsableSpaceNotEnough;
  mark_dirty(p_.get(), *of);
  return kSuccess;
}

uint32_t Fs::get_entry_type(int* out, const std::string& path) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  if (r.kind == 0) return kResultPathNotFound;
  *out = r.kind == 1 ? kDirectory : kFile;
  return kSuccess;
}

uint32_t Fs::create_file(const std::string& path, int64_t size) {
  PathOpLock L(p_.get());
  Resolved r;
  for (;;) {
    r = Resolved{};
    uint32_t rc = resolve(p_.get(), path, &r);
    if (rc) return rc;
    if (!r.m->writable) return kResultWriteNotPermitted;
    if (r.rel.empty()) return kResultInvalidArgument;
    if (r.kind != 0) return kResultPathAlreadyExists;
    if (!L.retry(r.m)) break;
  }
  std::error_code ec;
  stdfs::path host;
  if (r.m->kind == Mount::SaveData) {
    std::string parent = r.rel.find('/') == std::string::npos ? "" : r.rel.substr(0, r.rel.rfind('/'));
    if (r.m->save->kind(parent) != 1) return kResultPathNotFound;
    r.m->save->undelete(r.rel);
    r.m->save->add_dirty(r.rel);
    host = r.m->save->working() / to_path(r.rel);
    stdfs::create_directories(host.parent_path(), ec);
  } else {
    host = r.m->layers[0] / to_path(r.rel);
    if (!stdfs::is_directory(host.parent_path(), ec)) return kResultPathNotFound;
  }
  HostFile tmp;
  {
    std::ofstream f(host, std::ios::binary | std::ios::trunc);
    if (!f) return kResultPathNotFound;
  }
  if (size > 0) {
    if (!tmp.open(host, true) || !tmp.set_size(size)) return kResultUsableSpaceNotEnough;
  }
  if (trace_flags & kTraceFs) log("fs: create %s size %lld", path.c_str(), (long long)size);
  return kSuccess;
}

uint32_t Fs::delete_file(const std::string& path) {
  PathOpLock L(p_.get());
  Resolved r;
  for (;;) {
    r = Resolved{};
    uint32_t rc = resolve(p_.get(), path, &r);
    if (rc) return rc;
    if (!r.m->writable) return kResultWriteNotPermitted;
    if (r.kind != 2) return kResultPathNotFound;
    if (!L.retry(r.m)) break;
  }
  std::error_code ec;
  if (r.m->kind == Mount::SaveData) {
    Save& s = *r.m->save;
    stdfs::remove(s.working() / to_path(r.rel), ec);
    s.drop_dirty(r.rel);
    s.mark_deleted(r.rel);
  } else {
    stdfs::remove(r.m->layers[0] / to_path(r.rel), ec);
    if (ec) return kResultTargetLocked;
  }
  return kSuccess;
}

uint32_t Fs::create_directory(const std::string& path) {
  PathOpLock L(p_.get());
  Resolved r;
  for (;;) {
    r = Resolved{};
    uint32_t rc = resolve(p_.get(), path, &r);
    if (rc) return rc;
    if (!r.m->writable) return kResultWriteNotPermitted;
    if (r.kind != 0) return kResultPathAlreadyExists;
    if (!L.retry(r.m)) break;
  }
  std::error_code ec;
  if (r.m->kind == Mount::SaveData) {
    Save& s = *r.m->save;
    std::string parent = r.rel.find('/') == std::string::npos ? "" : r.rel.substr(0, r.rel.rfind('/'));
    if (s.kind(parent) != 1) return kResultPathNotFound;
    stdfs::create_directories(s.working() / to_path(r.rel), ec);
    if (ec) return kResultTargetLocked;
    // un-deleting the path is fine even if a committed *file* had this name: carry() sees the type change (working/ holds a
    // directory, dirs lists it) and does not carry the old file
    s.undelete(r.rel);
    s.add_dir(r.rel);
  } else {
    stdfs::path h = r.m->layers[0] / to_path(r.rel);
    if (!stdfs::is_directory(h.parent_path(), ec)) return kResultPathNotFound;
    stdfs::create_directory(h, ec);
    if (ec) return kResultPathNotFound;
  }
  return kSuccess;
}

uint32_t Fs::delete_directory(const std::string& path, bool recursive) {
  PathOpLock L(p_.get());
  Resolved r;
  for (;;) {
    r = Resolved{};
    uint32_t rc = resolve(p_.get(), path, &r);
    if (rc) return rc;
    if (!r.m->writable) return kResultWriteNotPermitted;
    if (r.kind != 1 || r.rel.empty()) return kResultPathNotFound;
    if (!L.retry(r.m)) break;
  }
  std::error_code ec;
  if (r.m->kind == Mount::SaveData) {
    Save& s = *r.m->save;
    std::function<uint32_t(const std::string&)> rm = [&](const std::string& rel) -> uint32_t {
      for (auto& e : s.list(rel)) {
        std::string child = rel + "/" + e.name;
        if (!recursive) return kResultDirectoryNotEmpty;
        if (e.type == kDirectory) { uint32_t x = rm(child); if (x) return x; }
        else {
          stdfs::remove(s.working() / to_path(child), ec);
          s.drop_dirty(child);
          s.mark_deleted(child);
        }
      }
      stdfs::remove(s.working() / to_path(rel), ec);
      s.drop_dir(rel);
      s.mark_deleted(rel);
      return kSuccess;
    };
    return rm(r.rel);
  }
  stdfs::path h = r.m->layers[0] / to_path(r.rel);
  if (recursive) stdfs::remove_all(h, ec);
  else {
    if (!stdfs::is_empty(h, ec)) return kResultDirectoryNotEmpty;
    stdfs::remove(h, ec);
  }
  return ec ? kResultTargetLocked : kSuccess;
}

uint32_t Fs::rename_file(const std::string& from, const std::string& to) {
  PathOpLock L(p_.get());
  Resolved a, b;
  for (;;) {
    a = Resolved{};
    b = Resolved{};
    uint32_t rc = resolve(p_.get(), from, &a);
    if (rc) return rc;
    rc = resolve(p_.get(), to, &b);
    if (rc) return rc;
    if (a.m != b.m) return kResultInvalidArgument;
    if (!a.m->writable) return kResultWriteNotPermitted;
    if (a.kind != 2) return kResultPathNotFound;
    if (b.kind != 0) return kResultPathAlreadyExists;
    if (!L.retry(a.m)) break;
  }
  std::error_code ec;
  const std::string to_parent = b.rel.find('/') == std::string::npos ? "" : b.rel.substr(0, b.rel.rfind('/'));
  if (a.m->kind == Mount::SaveData) {
    Save& s = *a.m->save;
    if (s.kind(to_parent) != 1) return kResultPathNotFound;   // as on hardware: the destination folder must exist (and be a folder)
    stdfs::path src;
    if (!s.ensure_working(a.rel, &src)) return kResultUsableSpaceNotEnough;
    stdfs::path dst = s.working() / to_path(b.rel);
    stdfs::create_directories(dst.parent_path(), ec);
    stdfs::rename(src, dst, ec);
    if (ec) return kResultTargetLocked;
    s.drop_dirty(a.rel);
    s.mark_deleted(a.rel);
    s.undelete(b.rel);
    s.add_dirty(b.rel);
  } else {
    if (!stdfs::is_directory(a.m->layers[0] / to_path(to_parent), ec)) return kResultPathNotFound;
    stdfs::rename(a.m->layers[0] / to_path(a.rel), a.m->layers[0] / to_path(b.rel), ec);
    if (ec) return kResultTargetLocked;
  }
  return kSuccess;
}

uint32_t Fs::open_directory(DirId* out, const std::string& path, int mode) {
  std::lock_guard<std::mutex> g(p_->mu);
  Resolved r;
  uint32_t rc = resolve(p_.get(), path, &r);
  if (rc) return rc;
  if (r.kind != 1) return kResultPathNotFound;
  auto od = std::make_shared<OpenDir>();
  std::map<std::string, DirEntry> merged;
  std::error_code ec;
  if (r.m->kind == Mount::SaveData) {
    for (auto& e : r.m->save->list(r.rel)) merged[e.name] = e;
  } else {
    // layered union: the first layer that has the name decides its type and size
    for (auto it = r.m->layers.rbegin(); it != r.m->layers.rend(); ++it) {
      stdfs::path d = *it / to_path(r.rel);
      if (!stdfs::is_directory(d, ec)) continue;
      for (auto& e : stdfs::directory_iterator(d, ec)) {
        DirEntry de;
        de.name = from_path(e.path().filename());
        std::error_code e2;
        de.type = e.is_directory(e2) ? kDirectory : kFile;
        de.size = de.type == kFile ? (int64_t)e.file_size(e2) : 0;
        merged[de.name] = de;
      }
    }
  }
  for (auto& kv : merged) {
    bool is_dir = kv.second.type == kDirectory;
    if ((is_dir && (mode & kOpenDirs)) || (!is_dir && (mode & kOpenFiles))) od->entries.push_back(kv.second);
  }
  uint64_t t = p_->token();
  p_->dirs[t] = od;
  *out = t;
  return kSuccess;
}

uint32_t Fs::read_directory(DirId d, DirEntry* out, int64_t max, int64_t* count) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->dirs.find(d);
  if (it == p_->dirs.end()) return kResultInvalidArgument;
  OpenDir& od = *it->second;
  int64_t n = 0;
  while (n < max && od.cursor < od.entries.size()) out[n++] = od.entries[od.cursor++];
  *count = n;
  return kSuccess;
}

uint32_t Fs::get_directory_entry_count(DirId d, int64_t* out) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->dirs.find(d);
  if (it == p_->dirs.end()) return kResultInvalidArgument;
  *out = (int64_t)it->second->entries.size();
  return kSuccess;
}

uint32_t Fs::close_directory(DirId d) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto it = p_->dirs.find(d);
  if (it == p_->dirs.end()) return kResultInvalidArgument;
  if (p_->st_valid && p_->st_dirs.count(d)) p_->st_zdirs[d] = it->second;   // open at the save: a load brings it back
  p_->dirs.erase(it);
  return kSuccess;
}

uint32_t Fs::commit(const std::string& mount) {
  static const bool locked = [] { const char* e = std::getenv("SNAP_FS_COMMIT_LOCKED"); return e && *e == '1'; }();
  const auto t0 = std::chrono::steady_clock::now();
  struct IoCoreGuard {
    bool active = false;
    std::unique_lock<std::mutex> cg;
    explicit IoCoreGuard(std::mutex& cm, bool unlock_mu) : active(unlock_mu), cg(cm, std::defer_lock) {
      if (active) os_io_begin();   // rule 3: give up virtual core before blocking on commit_mu
      cg.lock();
    }
    ~IoCoreGuard() {
      if (cg.owns_lock()) cg.unlock();
      if (active) os_io_end();     // rule 3: take virtual core back only after commit_mu and mu are released
    }
  } io_guard(p_->commit_mu, !locked);

  std::vector<std::shared_ptr<OpenFile>> to_flush;
  std::unique_lock<std::mutex> g(p_->mu);
  auto m = p_->find_mount(mount);
  if (!m) return kResultNotMounted;
  if (m->kind != Mount::SaveData) return kSuccess;  // nothing to commit on other mounts
  Save& s = *m->save;
  for (auto& kv : p_->files) if (kv.second->mount == mount && kv.second->writable) to_flush.push_back(kv.second);
  const Pending pd{s.dirty, s.deleted, s.dirs};
  auto hook = p_->hook;
  s.journal = Pending{};
  s.committing = !locked;
  struct EndJournal {   // a failed (or thrown-through) commit drops the journal; the live sets still hold every change
    Fs::Impl* p; Save& s; bool unlocked;
    ~EndJournal() {
      if (!s.committing) return;
      std::unique_lock<std::mutex> l(p->mu, std::defer_lock);
      if (unlocked) l.lock();
      s.committing = false;
      s.journal = Pending{};
    }
  } end_journal{p_.get(), s, !locked};
  if (!locked) g.unlock();
  for (auto& f : to_flush) {   // no of->mu here (lock order: of->mu, then p->mu); FlushFileBuffers on a shared handle is safe
    if (!f->hf.flush()) {
      const std::error_code e = last_os_error();
      note_io_failure("flush before commit", e);
      return commit_result(e);   // nothing was committed: CURRENT and the dirty set are as they were, so a retry carries everything
    }
  }
  uint32_t rc = s.do_commit(hook, pd, [&](uint64_t n) {
    std::unique_lock<std::mutex> l(p_->mu, std::defer_lock);
    if (!locked) l.lock();
    s.cur = n;
    if (s.committing) {   // changes made during the commit are the next commit's work
      s.dirty = std::move(s.journal.dirty); s.deleted = std::move(s.journal.deleted); s.dirs = std::move(s.journal.dirs);
      s.committing = false;
      s.journal = Pending{};
    } else {
      s.dirty.clear(); s.deleted.clear(); s.dirs.clear();
    }
  });
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  // always logged (a handful per session): how long the disk kept the game waiting is part of a "the game froze when it saved" report
  log("fs: commit %s -> generation %llu in %.1f ms (result %#x)", mount.c_str(), (unsigned long long)s.cur, ms, rc);
  if (snap_perf::on) {
    char det[96];
    snprintf(det, sizeof det, "%s gen=%llu files=%zu rc=%#x", mount.c_str(), (unsigned long long)s.cur, pd.dirty.size(), rc);
    snap_perf::event("fs_commit", ms, det);
  }
  return rc;
}

bool Fs::drain_for_exit(int ms) {
  const uint64_t end = hle::host::monotonic_ms() + static_cast<uint64_t>(ms > 0 ? ms : 0);
  while (!p_->commit_mu.try_lock()) {   // a commit runs mostly outside mu now: wait for it first (both stay held until exit)
    if (hle::host::monotonic_ms() >= end) return false;
    hle::host::sleep_ms(5);
  }
  while (!p_->mu.try_lock()) {
    if (hle::host::monotonic_ms() >= end) { p_->commit_mu.unlock(); return false; }
    hle::host::sleep_ms(5);
  }
  return true;
}

uint32_t Fs::get_free_space(int64_t* out, const std::string& mount) {
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = p_->find_mount(mount);
  if (!m) return kResultNotMounted;
  std::error_code ec;
  stdfs::path where = m->kind == Mount::SaveData ? m->save->root : m->layers[0];
  auto sp = stdfs::space(where, ec);
  int64_t avail = ec ? (int64_t)1 << 40 : (int64_t)sp.available;
  // the game extends the save when it sees under 100 MiB free (main 0x609a3c); report at least 4 GiB
  *out = std::clamp<int64_t>(avail, (int64_t)4 << 30, (int64_t)1 << 40);
  return kSuccess;
}

std::vector<SaveGeneration> Fs::list_generations(const std::string& mount) {
  std::lock_guard<std::mutex> cg(p_->commit_mu);   // a running commit changes the generation folders and orphans
  std::lock_guard<std::mutex> g(p_->mu);
  std::vector<SaveGeneration> v;
  auto m = p_->find_mount(mount);
  if (!m || m->kind != Mount::SaveData) return v;
  for (uint64_t n : m->save->gen_numbers())   // digits-only stems, each number once (a stray "1a.ok" is not generation 1)
    if (n <= m->save->cur && m->save->gen_valid(n)) v.push_back({n, n == m->save->cur});   // above CURRENT: never published
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.gen < b.gen; });
  return v;
}

uint32_t Fs::restore_generation(const std::string& mount, uint64_t gen) {
  std::lock_guard<std::mutex> cg(p_->commit_mu);
  std::lock_guard<std::mutex> g(p_->mu);
  auto m = p_->find_mount(mount);
  if (!m || m->kind != Mount::SaveData) return kResultNotMounted;
  Save& s = *m->save;
  if (gen > s.cur || !s.gen_valid(gen)) return kResultPathNotFound;
  // The restore is staged in a new scratch folder: a copy of the chosen generation where every file counts as dirty, so the
  // commit writes it all. Any copy or listing error drops the stage and leaves the session (working folder, pending changes)
  // as it was; a partial restore is never committed. The session's old scratch folder goes only after the commit succeeded.
  std::string stage_name;
  if (!s.fresh_working(&stage_name)) return kResultTargetLocked;
  const stdfs::path stage = s.root / stage_name, src = s.gen_dir(gen);
  std::set<std::string> dirty, deleted, dirs;
  std::error_code why;   // first failure; decides the result code
  {
    std::error_code ec;
    stdfs::recursive_directory_iterator it(src, ec), end;
    for (; !why && !ec && it != end; it.increment(ec)) {
      std::error_code e1;
      const auto u = it->path().lexically_relative(src).generic_u8string();
      const std::string rel(u.begin(), u.end());
      const bool isdir = it->is_directory(e1);
      if (!e1 && (rel.empty() || rel == ".")) e1 = std::make_error_code(std::errc::io_error);
      if (!e1 && isdir) {
        stdfs::create_directories(stage / to_path(rel), e1);
        dirs.insert(rel);
      } else if (!e1) {
        stdfs::create_directories((stage / to_path(rel)).parent_path(), e1);
        if (!e1) stdfs::copy_file(it->path(), stage / to_path(rel), stdfs::copy_options::none, e1);
        dirty.insert(rel);
      }
      if (e1) why = e1;
    }
    if (ec && !why) why = ec;
  }
  // entries of the current generation that the restored one lacks must disappear. One the restored generation has with the
  // other type (file vs directory) is left to carry(), which skips type-changed entries; nothing below it is walked, so no path
  // runs through a file.
  std::function<bool(const stdfs::path&, const std::string&)> walk = [&](const stdfs::path& d, const std::string& rel) -> bool {
    std::error_code e1;
    for (stdfs::directory_iterator it(d, e1), end; it != end; it.increment(e1)) {
      if (e1) { why = e1; return false; }
      std::string name = from_path(it->path().filename());
      std::string child = rel.empty() ? name : rel + "/" + name;
      std::error_code e2;
      const bool isdir = it->is_directory(e2);
      if (e2) { why = e2; return false; }
      const stdfs::file_type t = stdfs::symlink_status(src / to_path(child), e2).type();
      if (t == stdfs::file_type::not_found) { deleted.insert(child); continue; }
      if (t == stdfs::file_type::none || t == stdfs::file_type::unknown) { why = e2 ? e2 : std::make_error_code(std::errc::io_error); return false; }
      if (isdir && t == stdfs::file_type::directory && !walk(it->path(), child)) return false;
    }
    if (e1) why = e1;
    return !e1;
  };
  if (!why) walk(s.committed(), "");
  if (why) {
    if (!remove_checked(stage)) log("fs: save: cannot remove the restore stage %s", from_path(stage).c_str());
    log("fs: save: restore of generation %llu failed while copying (%s); nothing changed", (unsigned long long)gen, why.message().c_str());
    return commit_result(why);
  }
  // swap the stage in; a failed commit swaps the session back
  std::string old_name = s.work_name;
  std::set<std::string> old_dirty = std::move(s.dirty), old_deleted = std::move(s.deleted), old_dirs = std::move(s.dirs);
  s.work_name = stage_name;
  s.dirty = std::move(dirty); s.deleted = std::move(deleted); s.dirs = std::move(dirs);
  const Pending pd{s.dirty, s.deleted, s.dirs};
  uint32_t rc = s.do_commit(p_->hook, pd, [&](uint64_t n) {   // mu is held for the whole restore
    s.cur = n;
    s.dirty.clear(); s.deleted.clear(); s.dirs.clear();
  });
  if (rc != kSuccess) {
    s.work_name = old_name;
    s.dirty = std::move(old_dirty); s.deleted = std::move(old_deleted); s.dirs = std::move(old_dirs);
    if (!remove_checked(stage)) log("fs: save: cannot remove the restore stage %s", from_path(stage).c_str());
    return rc;
  }
  if (!remove_checked(s.root / old_name)) log("fs: save: cannot remove the old scratch folder %s (dropped at the next mount)", from_path(s.root / old_name).c_str());
  return rc;
}

Fs& instance() {
  static Fs fs;
  return fs;
}

}  // namespace hle::fs

// Called from the crash report (host/mods/crashlog.cpp) and so into the log tail and the problem report: one line, only when a save flush failed.
extern "C" void snap_fs_log_io_failures(void) {
  const uint64_t n = hle::fs::io_failure_count();
  if (n) std::fprintf(stderr, "[fs] save I/O failures: %llu (the last one is logged above as 'fs: save: ...')\n", (unsigned long long)n);
}
