// SPDX-License-Identifier: MIT
// Windows WIC PNG writer and APPDATA directory support for host/mods/photoexport.cpp.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <cstdlib>
#include <string>

#include "photoexport_host.h"

#pragma comment(lib, "windowscodecs")

namespace snap_photoexport::host {

namespace {

std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

}  // namespace

std::string default_photos_dir() {
    const char *a = std::getenv("APPDATA");
    return std::string(a ? a : ".") + "\\SnapRecomp\\photos";
}

bool ensure_dir(const std::string &dir) {
    return CreateDirectoryA(dir.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string join_path(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    return (last == '\\' || last == '/') ? (dir + name) : (dir + "\\" + name);
}

LocalTime local_time() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return {t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond};
}

bool write_png(const std::string &path, int w, int h, const uint8_t *bgra, size_t bgra_bytes) {
    if (!bgra || w <= 0 || h <= 0 || bgra_bytes < static_cast<size_t>(w) * static_cast<size_t>(h) * 4) return false;
    IWICImagingFactory *f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr;
    IPropertyBag2 *pb = nullptr;
    bool ok = false;
    std::wstring tmp = widen(path + ".tmp");
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(w, h))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        fr->SetPixelFormat(&pf);
        if (SUCCEEDED(fr->WritePixels(h, w * 4, static_cast<UINT>(bgra_bytes), const_cast<BYTE *>(bgra))) &&
            SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) {
            ok = true;
        }
    }
    if (pb) pb->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    f->Release();
    if (ok) {
        std::wstring fin = widen(path);
        ok = MoveFileExW(tmp.c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

void worker_thread_init() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

}  // namespace snap_photoexport::host
