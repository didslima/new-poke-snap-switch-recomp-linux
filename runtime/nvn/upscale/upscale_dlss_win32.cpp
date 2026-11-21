// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "upscale_dlss_host.h"

namespace snap_upscale::dlss::host {

VtblLayout vtbl_layout() {
  // MSVC C++ ABI reverses overload groups in NVSDK_NGX_Parameter.
  return {3, 4, 6, 0, 11, 12, 14, 8};
}

void* open_core(std::string* out_desc) {
  wchar_t path[MAX_PATH * 2 + 32] = {0};
  HMODULE icd = GetModuleHandleW(L"nvoglv64.dll");
  DWORD len = icd ? GetModuleFileNameW(icd, path, MAX_PATH * 2) : 0;
  while (len && path[len - 1] != L'\\') len--;
  HMODULE core = nullptr;
  if (len) {
    wcscpy(path + len, L"_nvngx.dll");
    core = LoadLibraryW(path);
  }
  if (!core) {
    DWORD n = MAX_PATH * 2 * sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ, nullptr, path, &n) == ERROR_SUCCESS) {
      wcscat(path, L"\\_nvngx.dll");
      core = LoadLibraryW(path);
    }
  }
  if (out_desc) {
    if (core) {
      char narrow[MAX_PATH * 2 + 32] = {0};
      WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
      *out_desc = narrow;
    } else {
      *out_desc = icd ? "_nvngx.dll not found" : "_nvngx.dll not found (no NVIDIA Vulkan driver loaded)";
    }
  }
  return reinterpret_cast<void*>(core);
}

void* get_sym(void* lib, const char* name) {
  return lib ? reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name)) : nullptr;
}

std::wstring app_data_dir() {
  wchar_t wdir[MAX_PATH] = {0};
  DWORD len = GetModuleFileNameW(nullptr, wdir, MAX_PATH);
  while (len && wdir[len - 1] != L'\\') len--;
  wdir[len] = 0;
  return wdir;
}

std::vector<std::wstring> feature_search_dirs() { return {}; }

const char* missing_feature_hint() { return "nvngx_dlss.dll next to the exe?"; }

}  // namespace snap_upscale::dlss::host
