// SPDX-License-Identifier: MIT
// Internal OS seam for full-resolution photo export (host/mods/photoexport.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace snap_photoexport::host {

struct LocalTime {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

std::string default_photos_dir();
bool ensure_dir(const std::string &dir);
std::string join_path(const std::string &dir, const std::string &name);
LocalTime local_time();
bool write_png(const std::string &path, int w, int h, const uint8_t *bgra, size_t bgra_bytes);
void worker_thread_init();

}  // namespace snap_photoexport::host
