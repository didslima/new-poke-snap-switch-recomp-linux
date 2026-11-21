// SPDX-License-Identifier: MIT
// Internal OS seam for DLSS NGX core loading and C++ parameter vtable layout.
#pragma once

#include <string>
#include <vector>

namespace snap_upscale::dlss::host {

struct VtblLayout {
  int s_i, s_u32, s_f, s_p;
  int g_i, g_u32, g_f, g_p;
};

VtblLayout vtbl_layout();
void* open_core(std::string* out_desc);
void* get_sym(void* lib, const char* name);
std::wstring app_data_dir();
std::vector<std::wstring> feature_search_dirs();
const char* missing_feature_hint();

}  // namespace snap_upscale::dlss::host
