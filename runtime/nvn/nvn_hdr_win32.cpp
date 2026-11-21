// SPDX-License-Identifier: MIT
// Windows display peak query used by the shared HDR Vulkan presentation path.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>

#include "nvn_hdr.h"

namespace snap_hdr {

float QueryDisplayPeak(void* hwnd) {
  typedef HRESULT(WINAPI * PFN_CF1)(REFIID, void**);
  HMODULE m = LoadLibraryA("dxgi.dll");
  if (!m) return 0;
  auto cf = (PFN_CF1)GetProcAddress(m, "CreateDXGIFactory1");
  IDXGIFactory1* fac = nullptr;
  float peak = 0;
  if (cf && SUCCEEDED(cf(__uuidof(IDXGIFactory1), (void**)&fac)) && fac) {
    HMONITOR mon = hwnd ? MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTOPRIMARY) : nullptr;
    float any = 0;
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) {
      IDXGIOutput* out = nullptr;
      for (UINT j = 0; ad->EnumOutputs(j, &out) == S_OK; j++) {
        IDXGIOutput6* o6 = nullptr;
        if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6)) && o6) {
          DXGI_OUTPUT_DESC1 d{};
          if (SUCCEEDED(o6->GetDesc1(&d))) {
            if (!any) any = d.MaxLuminance;
            if (mon && d.Monitor == mon) peak = d.MaxLuminance;
          }
          o6->Release();
        }
        out->Release();
      }
      ad->Release();
    }
    if (peak <= 0) peak = any;
    fac->Release();
  }
  FreeLibrary(m);
  return peak;
}

}  // namespace snap_hdr
