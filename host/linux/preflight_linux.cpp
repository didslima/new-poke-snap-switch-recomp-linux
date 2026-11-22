// SPDX-License-Identifier: MIT
// Linux preflight validator implementation (stage 5D-B1). See preflight_linux.h.
#include "preflight_linux.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "cpu.h"
#include "elf_check.h"

namespace snap_linux {
namespace {

constexpr uint64_t kGuestMainBase = 0x7100000000ull;
constexpr uint64_t kGuestSdkBase = 0x7110000000ull;
constexpr uint64_t kMaxMainSpan = kGuestSdkBase - kGuestMainBase;   // 256 MiB before sdk
constexpr uint64_t kMaxSdkSpan = 0x7300000000ull - kGuestSdkBase;   // before THUNK_BASE

#pragma pack(push, 1)
struct Elf64Ehdr {
    uint8_t id[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Elf64Phdr {
    uint32_t type, flags;
    uint64_t off, vaddr, paddr, filesz, memsz, align;
};
struct Elf64Dyn {
    int64_t tag;
    uint64_t val;
};
#pragma pack(pop)

bool read_whole_file(const std::string &path, std::vector<uint8_t> &out, std::string &err) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = std::strerror(errno);
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        err = "fseek failed";
        std::fclose(f);
        return false;
    }
    const long sz = std::ftell(f);
    if (sz < 0) {
        err = "ftell failed";
        std::fclose(f);
        return false;
    }
    std::rewind(f);
    out.resize((size_t)sz);
    const bool ok = sz == 0 || std::fread(out.data(), 1, (size_t)sz, f) == (size_t)sz;
    std::fclose(f);
    if (!ok) err = "short read";
    return ok;
}

// Runs a subprocess via execvp (no shell, safe for paths with spaces) and captures stdout+stderr.
bool run_tool(const char *const argv[], std::string &output, int &exit_code) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], const_cast<char *const *>(argv));
        _exit(127);
    }
    close(pipefd[1]);
    output.clear();
    char buf[1024];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        output.append(buf, (size_t)n);
    }
    close(pipefd[0]);
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return false;
    exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    return true;
}

bool run_tool_any(const char *prog1, const char *prog2, const std::string &arg1, const std::string &arg2,
                  std::string &output, int &exit_code) {
    for (const char *prog : {prog1, prog2}) {
        if (!prog) continue;
        std::vector<const char *> argv{prog};
        if (!arg1.empty()) argv.push_back(arg1.c_str());
        if (!arg2.empty()) argv.push_back(arg2.c_str());
        argv.push_back(nullptr);
        if (run_tool(argv.data(), output, exit_code) && exit_code != 127) {
            return true;
        }
    }
    return false;
}

const std::unordered_set<std::string> &known_runtime_providers() {
    static const std::unordered_set<std::string> s = {
        "g_fnprof",
        "fnprof_hit",
        "rt_dispatch",
        "rt_import_call",
        "rt_svc",
        "rt_brk",
        "rt_udf",
        "rt_ticks",
        "rt_spin_gate",
        "rt_inline_hook",
        "rt_hook_unw_jumpto",
        "rt_hook_setjmp",
        "rt_hook_longjmp",
        "rt_hook_get_exception_info",
        // shared host/mods/drpfast.cpp
        "rt_drp_decrypt",
        "rt_drp_decrypt_check",
        // QA26 host/mods/ride_mouse_cam.cpp
        "rt_gm_ride_mouse_cam",
        // libc / compiler builtins provided by host libc
        "memcpy",
        "memmove",
        "memset",
        "strlen",
        "strcmp",
        "getenv",
        "abort",
        "fma",
        "fmaf",
        "sqrt",
        "sqrtf",
    };
    return s;
}

bool is_disabled_mod_hook(const std::string &sym) {
    static const std::unordered_set<std::string> s = {
        "rt_mod_entry", "rt_gm_arena7_size", "rt_gm_ao_simple", "rt_gm_ao_hbao", "rt_gm_pass",
        "rt_gm_lod_ratio", "rt_gm_lod_ratio_sub", "rt_gm_anim_rate", "rt_gm_font_atlas_info",
        "rt_gm_font_atlas_dims", "rt_gm_font_glyph1", "rt_gm_font_glyph2", "rt_gm_font_fix",
        "rt_gm_neo_speed", "rt_gm_film_c", "rt_gm_skip_wait", "rt_gm_route_ui_event",
        "rt_gm_route_ui_setstate", "rt_gm_ui_enter", "rt_gm_ui_event", "rt_gm_ui_pad",
        "rt_gm_ui_pointer", "rt_ui_opt_event", "rt_ui_opt_state",
    };
    return s.count(sym) > 0;
}

bool is_windows_only_symbol(const std::string &sym) {
    return sym.rfind("__imp_", 0) == 0 ||
           sym.rfind("_CRT_", 0) == 0 ||
           sym == "VirtualAlloc" ||
           sym == "VirtualFree" ||
           sym == "GetCurrentThreadId" ||
           sym == "InterlockedIncrement64";
}

const char *status_str(PreflightStatus s) {
    switch (s) {
    case PreflightStatus::Pass: return "PASS";
    case PreflightStatus::Fail: return "FAIL";
    case PreflightStatus::Blocked: return "BLOCKED";
    }
    return "FAIL";
}

}  // namespace

ArtifactCheck validate_guest_elf(const std::string &path, const char *label, uint64_t base_addr, uint64_t max_span) {
    ArtifactCheck r;
    r.label = std::string(label) + "_elf";
    r.path = path;

    if (path.empty()) {
        r.status = PreflightStatus::Blocked;
        r.detail = "path is empty";
        return r;
    }

    std::vector<uint8_t> raw;
    std::string io_err;
    if (!read_whole_file(path, raw, io_err)) {
        r.status = PreflightStatus::Blocked;
        r.detail = "missing or unreadable (" + io_err + ")";
        return r;
    }
    r.size_bytes = raw.size();

    std::string why;
    if (!elf_check::check_bytes(raw.data(), raw.size(), why)) {
        r.status = PreflightStatus::Fail;
        r.detail = "invalid guest ELF (" + why + ")";
        return r;
    }

    Elf64Ehdr eh{};
    std::memcpy(&eh, raw.data(), sizeof(eh));
    if (eh.type != 3 /* ET_DYN */) {
        r.status = PreflightStatus::Fail;
        r.detail = "unexpected ELF type " + std::to_string(eh.type) + " (expected ET_DYN=3)";
        return r;
    }

    std::vector<Elf64Phdr> ph(eh.phnum);
    std::memcpy(ph.data(), raw.data() + eh.phoff, ph.size() * sizeof(Elf64Phdr));
    uint64_t hi = 0, dynva = 0;
    for (const auto &p : ph) {
        if (p.type == 1) hi = std::max<uint64_t>(hi, p.vaddr + p.memsz);
        if (p.type == 2) dynva = p.vaddr;
    }
    const uint64_t span = (hi + 0xFFFFull) & ~0xFFFFull;
    if (span == 0 || span > max_span) {
        r.status = PreflightStatus::Fail;
        r.detail = "mapped span exceeds loader window at base";
        return r;
    }

    // Check dynamic table bounds that loader.cpp map_module() indexes directly.
    const Elf64Phdr *dyn_seg = nullptr;
    for (const auto &p : ph) {
        if (p.type == 1 && dynva >= p.vaddr && dynva < p.vaddr + p.filesz) {
            dyn_seg = &p;
            break;
        }
    }
    if (!dyn_seg) {
        r.status = PreflightStatus::Fail;
        r.detail = "PT_DYNAMIC outside PT_LOAD file range";
        return r;
    }
    uint64_t symtab = 0, strtab = 0, hash = 0, relasz = 0, pltrelsz = 0;
    uint64_t init_fn = 0, init_arr = 0, init_arr_sz = 0;
    const uint64_t seg_end = dyn_seg->off + dyn_seg->filesz;
    for (uint64_t off = dyn_seg->off + (dynva - dyn_seg->vaddr); off + sizeof(Elf64Dyn) <= seg_end; off += sizeof(Elf64Dyn)) {
        Elf64Dyn dy{};
        std::memcpy(&dy, raw.data() + off, sizeof(dy));
        if (dy.tag == 0) break;
        switch (dy.tag) {
        case 4: hash = dy.val; break;
        case 5: strtab = dy.val; break;
        case 6: symtab = dy.val; break;
        case 8: relasz = dy.val; break;
        case 2: pltrelsz = dy.val; break;
        case 12: init_fn = dy.val; break;
        case 25: init_arr = dy.val; break;
        case 27: init_arr_sz = dy.val; break;
        default: break;
        }
    }
    if ((relasz % 24) != 0 || (pltrelsz % 24) != 0) {
        r.status = PreflightStatus::Fail;
        r.detail = "relocation table size is not a multiple of 24 bytes (Elf64_Rela)";
        return r;
    }
    if (init_fn >= span || (init_arr_sz && (init_arr >= span || init_arr_sz > span - init_arr))) {
        r.status = PreflightStatus::Fail;
        r.detail = "DT_INIT / DT_INIT_ARRAY outside mapped span";
        return r;
    }
    if (hash) {
        // Read DT_HASH nchain if inside a file-backed PT_LOAD segment.
        for (const auto &p : ph) {
            if (p.type == 1 && hash >= p.vaddr && hash + 8 <= p.vaddr + p.filesz) {
                uint32_t hdr[2]{};
                std::memcpy(hdr, raw.data() + p.off + (hash - p.vaddr), sizeof(hdr));
                const uint64_t nchain = hdr[1];
                if (nchain > (span - symtab) / 24) {
                    r.status = PreflightStatus::Fail;
                    r.detail = "DT_HASH nchain extends symbol table past mapped span";
                    return r;
                }
                break;
            }
        }
    }
    (void)strtab;

    char buf[160];
    std::snprintf(buf, sizeof(buf), "ELF64 LE ET_DYN AArch64, base=%#llx, span=%#llx",
                  (unsigned long long)base_addr, (unsigned long long)span);
    r.status = PreflightStatus::Pass;
    r.detail = buf;
    return r;
}

ArtifactCheck validate_aot_archive(const std::string &path, const char *module_name) {
    ArtifactCheck r;
    r.label = std::string(module_name) + "_lib";
    r.path = path;

    if (path.empty()) {
        r.status = PreflightStatus::Blocked;
        r.detail = "path is empty";
        return r;
    }

    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        r.status = PreflightStatus::Blocked;
        r.detail = std::string("missing or unreadable (") + std::strerror(errno) + ")";
        return r;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::rewind(f);
    char magic[8] = {};
    const size_t rd = std::fread(magic, 1, sizeof(magic), f);
    std::fclose(f);
    r.size_bytes = sz > 0 ? (uint64_t)sz : 0;

    if (rd != sizeof(magic) || std::memcmp(magic, "!<arch>\n", 8) != 0) {
        r.status = PreflightStatus::Fail;
        r.detail = "not a valid ar archive (missing !<arch> header)";
        return r;
    }

    // 1. Archive members
    std::string out;
    int rc = -1;
    if (!run_tool_any("llvm-ar", "ar", "t", path, out, rc) || rc != 0) {
        r.status = PreflightStatus::Fail;
        r.detail = "ar failed to list archive members";
        return r;
    }
    if (out.empty()) {
        r.status = PreflightStatus::Fail;
        r.detail = "archive contains no object members";
        return r;
    }

    // 2. ELF object headers (must all be ELF64 LE ET_REL x86-64)
    if (!run_tool_any("readelf", "llvm-readelf", "-h", path, out, rc) || rc != 0) {
        r.status = PreflightStatus::Fail;
        r.detail = "readelf failed to inspect archive objects";
        return r;
    }
    int files_seen = 0;
    {
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("Class:") != std::string::npos) {
                files_seen++;
                if (line.find("ELF64") == std::string::npos) {
                    r.status = PreflightStatus::Fail;
                    r.detail = "incompatible object class (" + line + ")";
                    return r;
                }
            } else if (line.find("Data:") != std::string::npos) {
                if (line.find("little endian") == std::string::npos) {
                    r.status = PreflightStatus::Fail;
                    r.detail = "incompatible endianness (" + line + ")";
                    return r;
                }
            } else if (line.find("Type:") != std::string::npos) {
                if (line.find("REL") == std::string::npos) {
                    r.status = PreflightStatus::Fail;
                    r.detail = "unexpected ELF object type (" + line + ")";
                    return r;
                }
            } else if (line.find("Machine:") != std::string::npos) {
                if (line.find("X86-64") == std::string::npos && line.find("x86-64") == std::string::npos) {
                    r.status = PreflightStatus::Fail;
                    r.detail = "incompatible object architecture (" + line + ")";
                    return r;
                }
            }
        }
    }
    if (files_seen == 0) {
        r.status = PreflightStatus::Fail;
        r.detail = "archive contains no ELF objects";
        return r;
    }

    // 3. Defined global symbols
    if (!run_tool_any("nm", "llvm-nm", "-g", path, out, rc) || rc != 0) {
        r.status = PreflightStatus::Fail;
        r.detail = "nm failed to read archive symbols";
        return r;
    }
    std::set<std::string> defined;
    std::set<std::string> undefined;
    {
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            std::istringstream ls(line);
            std::string col1, col2, col3;
            ls >> col1;
            if (col1.empty() || col1.back() == ':') continue;
            if (col1 == "U" || col1 == "w" || col1 == "v") {
                ls >> col2;
                if (!col2.empty()) undefined.insert(col2);
            } else {
                ls >> col2 >> col3;
                if (!col3.empty()) {
                    defined.insert(col3);
                } else if (!col2.empty() && col1.size() == 1) {
                    defined.insert(col2);
                }
            }
        }
    }

    std::vector<std::string> required;
    if (std::strcmp(module_name, "main") == 0) {
        required = {"main_fn_addrs", "main_fn_ptrs", "main_fn_count",
                    "main_import_names", "main_import_stub_addrs", "main_import_count"};
    } else {
        required = {"sdk_fn_addrs", "sdk_fn_ptrs", "sdk_fn_count"};
    }
    for (const auto &req : required) {
        if (!defined.count(req)) {
            r.status = PreflightStatus::Fail;
            r.detail = "missing required AOT symbol: " + req;
            return r;
        }
    }

    // 4. SNAP_RUNTIME_ABI verification
    const std::string want_abi = "snap_runtime_abi_" + std::to_string(SNAP_RUNTIME_ABI);
    std::string found_abi;
    for (const auto &u : undefined) {
        if (u.rfind("snap_runtime_abi_", 0) == 0) {
            found_abi = u;
            break;
        }
    }
    if (found_abi.empty()) {
        r.status = PreflightStatus::Fail;
        r.detail = "missing SNAP_RUNTIME_ABI marker (expected " + want_abi + ")";
        return r;
    }
    if (found_abi != want_abi) {
        r.status = PreflightStatus::Fail;
        r.detail = "ABI mismatch: archive requires " + found_abi + ", runtime provides " + want_abi;
        return r;
    }

    // 5. Undefined external symbol classification
    const auto &providers = known_runtime_providers();
    size_t disabled_mods = 0;
    for (const auto &u : undefined) {
        if (u == want_abi || u.rfind("f_", 0) == 0 || u.rfind("imp_", 0) == 0) continue;
        if (is_windows_only_symbol(u)) {
            r.status = PreflightStatus::Fail;
            r.detail = "Windows-only symbol referenced: " + u;
            return r;
        }
        if (is_disabled_mod_hook(u)) {
            disabled_mods++;
            continue;
        }
        if (!providers.count(u) && !defined.count(u)) {
            r.unprovided_symbols.push_back(u);
        }
    }
    if (disabled_mods > 0) {
        r.warnings.push_back(std::to_string(disabled_mods) + " Windows mod/UI inline hook(s) disabled via explicit no-op stubs");
    }

    if (!r.unprovided_symbols.empty()) {
        r.status = PreflightStatus::Blocked;
        r.detail = "ELF64 x86-64 archive (" + std::to_string(files_seen) + " objects, " + want_abi +
                   "), but " + std::to_string(r.unprovided_symbols.size()) + " external symbol(s) have no Linux provider";
        return r;
    }

    r.status = PreflightStatus::Pass;
    r.detail = "ELF64 x86-64 archive (" + std::to_string(files_seen) + " objects, " + want_abi + ")";
    return r;
}

PreflightReport run_preflight(const std::string &main_elf, const std::string &sdk_elf,
                              const std::string &main_lib, const std::string &sdk_lib) {
    PreflightReport rep;
    rep.main_elf = validate_guest_elf(main_elf, "main", kGuestMainBase, kMaxMainSpan);
    rep.sdk_elf = validate_guest_elf(sdk_elf, "sdk", kGuestSdkBase, kMaxSdkSpan);
    rep.main_lib = validate_aot_archive(main_lib, "main");
    rep.sdk_lib = validate_aot_archive(sdk_lib, "sdk");

    const ArtifactCheck *all[] = {&rep.main_elf, &rep.sdk_elf, &rep.main_lib, &rep.sdk_lib};
    bool any_fail = false, any_blocked = false;
    for (const auto *c : all) {
        if (c->status == PreflightStatus::Fail) any_fail = true;
        if (c->status == PreflightStatus::Blocked) any_blocked = true;
    }
    rep.overall = any_fail ? PreflightStatus::Fail : (any_blocked ? PreflightStatus::Blocked : PreflightStatus::Pass);
    return rep;
}

void print_preflight_report(const PreflightReport &r, FILE *out) {
    std::fprintf(out, "preflight: %s (runtime SNAP_RUNTIME_ABI=%d)\n", status_str(r.overall), SNAP_RUNTIME_ABI);
    for (const auto *c : {&r.main_elf, &r.sdk_elf, &r.main_lib, &r.sdk_lib}) {
        std::fprintf(out, "  [%s] %-8s path=%s size=%llu: %s\n",
                     status_str(c->status), c->label.c_str(), c->path.c_str(),
                     (unsigned long long)c->size_bytes, c->detail.c_str());
        for (const auto &w : c->warnings) {
            std::fprintf(out, "    [WARN] %s\n", w.c_str());
        }
        if (!c->unprovided_symbols.empty()) {
            std::fprintf(out, "    unprovided external symbols (%zu):", c->unprovided_symbols.size());
            const size_t limit = c->unprovided_symbols.size() < 12 ? c->unprovided_symbols.size() : 12;
            for (size_t i = 0; i < limit; ++i) {
                std::fprintf(out, " %s", c->unprovided_symbols[i].c_str());
            }
            if (c->unprovided_symbols.size() > limit) std::fprintf(out, " ...");
            std::fprintf(out, "\n");
        }
    }
}

}  // namespace snap_linux
