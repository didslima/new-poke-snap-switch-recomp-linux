// SPDX-License-Identifier: MIT
// Linux NVIDIA NGX Vulkan loader (libnvidia-ngx.so.1) and Itanium C++ ABI parameter vtable layout.
#include "../upscale/upscale_dlss_host.h"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace snap_upscale::dlss::host {

namespace {
std::wstring to_wide_ascii(const std::string& s) {
  return std::wstring(s.begin(), s.end());
}

void add_dir_if_nonempty(std::vector<std::wstring>& out, const fs::path& p) {
  if (p.empty()) return;
  const std::wstring w = to_wide_ascii(p.string());
  for (const auto& existing : out) {
    if (existing == w) return;
  }
  out.push_back(w);
}
}  // namespace

VtblLayout vtbl_layout() {
  // Itanium C++ ABI keeps exact declaration order in NVSDK_NGX_Parameter (include/nvsdk_ngx_params.h):
  // Set(u64=0, f=1, d=2, u32=3, i=4, d3d11=5, d3d12=6, void*=7),
  // Get(u64=8, f=9, d=10, u32=11, i=12, d3d11=13, d3d12=14, void*=15), Reset(16).
  return {4, 3, 1, 7, 12, 11, 9, 15};
}

void* open_core(std::string* out_desc) {
  if (const char* env_core = std::getenv("SNAP_NGX_CORE"); env_core && *env_core) {
    dlerror();
    if (void* h = dlopen(env_core, RTLD_NOW | RTLD_LOCAL)) {
      if (out_desc) *out_desc = env_core;
      return h;
    }
    if (out_desc) {
      const char* err = dlerror();
      *out_desc = std::string("SNAP_NGX_CORE='") + env_core + "' failed (" + (err ? err : "unknown error") + ")";
    }
    return nullptr;
  }

  const char* candidates[] = {
      "libnvidia-ngx.so.1",
      "/usr/lib64/libnvidia-ngx.so.1",
      "/usr/lib/x86_64-linux-gnu/libnvidia-ngx.so.1",
  };
  for (const char* cand : candidates) {
    dlerror();
    if (void* h = dlopen(cand, RTLD_NOW | RTLD_LOCAL)) {
      if (out_desc) *out_desc = cand;
      return h;
    }
  }
  if (out_desc) {
    const char* err = dlerror();
    *out_desc = err ? std::string("libnvidia-ngx.so.1 not available (") + err + ")" : "libnvidia-ngx.so.1 not available";
  }
  return nullptr;
}

void* get_sym(void* lib, const char* name) {
  return lib ? dlsym(lib, name) : nullptr;
}

std::wstring app_data_dir() {
  const char* cache_home = std::getenv("XDG_CACHE_HOME");
  const char* home = std::getenv("HOME");
  fs::path dir = (cache_home && *cache_home) ? fs::path(cache_home) / "SnapRecomp" / "ngx"
                 : (home && *home)           ? fs::path(home) / ".cache" / "SnapRecomp" / "ngx"
                                             : fs::temp_directory_path() / "snap_ngx";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return to_wide_ascii(dir.string());
}

std::vector<std::wstring> feature_search_dirs() {
  std::vector<std::wstring> dirs;
  if (const char* p = std::getenv("SNAP_DLSS_DIR"); p && *p) add_dir_if_nonempty(dirs, fs::path(p));
  if (const char* p = std::getenv("SNAP_NGX_PATH"); p && *p) add_dir_if_nonempty(dirs, fs::path(p));

  const char* data_home = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  if (data_home && *data_home) {
    add_dir_if_nonempty(dirs, fs::path(data_home) / "SnapRecomp" / "upscalers");
  } else if (home && *home) {
    add_dir_if_nonempty(dirs, fs::path(home) / ".local" / "share" / "SnapRecomp" / "upscalers");
  }

  const char* cache_home = std::getenv("XDG_CACHE_HOME");
  if (cache_home && *cache_home) {
    add_dir_if_nonempty(dirs, fs::path(cache_home) / "SnapRecomp" / "ngx");
  } else if (home && *home) {
    add_dir_if_nonempty(dirs, fs::path(home) / ".cache" / "SnapRecomp" / "ngx");
  }

  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
    add_dir_if_nonempty(dirs, fs::path(appimage).parent_path());
    add_dir_if_nonempty(dirs, fs::path(appimage).parent_path() / "upscalers");
  }
  if (const char* appdir = std::getenv("APPDIR"); appdir && *appdir) {
    add_dir_if_nonempty(dirs, fs::path(appdir) / "usr" / "lib");
  }
  char exe_buf[4096] = {};
  const ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
  if (n > 0) {
    exe_buf[n] = '\0';
    add_dir_if_nonempty(dirs, fs::path(exe_buf).parent_path());
  }
  return dirs;
}

const char* missing_feature_hint() {
  return "libnvidia-ngx-dlss.so.* in SNAP_DLSS_DIR or ~/.local/share/SnapRecomp/upscalers?";
}

}  // namespace snap_upscale::dlss::host
