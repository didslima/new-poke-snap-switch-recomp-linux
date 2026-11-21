// SPDX-License-Identifier: MIT
// Linux XDG photo directory and self-contained lossless PNG writer for host/mods/photoexport.cpp.
#include "../mods/photoexport_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace snap_photoexport::host {

namespace {

uint32_t png_crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}

void push_be32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void push_chunk(std::vector<uint8_t> &out, const char type[4], const uint8_t *data, size_t len) {
    push_be32(out, static_cast<uint32_t>(len));
    const size_t type_off = out.size();
    out.insert(out.end(), type, type + 4);
    if (len && data) out.insert(out.end(), data, data + len);
    const uint32_t crc = png_crc32(out.data() + type_off, 4 + len);
    push_be32(out, crc);
}

}  // namespace

std::string default_photos_dir() {
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return (fs::path(xdg) / "SnapRecomp" / "photos").string();
    const char *home = std::getenv("HOME");
    if (home && *home) return (fs::path(home) / ".local" / "share" / "SnapRecomp" / "photos").string();
    return (fs::path(".") / "SnapRecomp" / "photos").string();
}

bool ensure_dir(const std::string &dir) {
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return !ec;
}

std::string join_path(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? (dir + name) : (dir + "/" + name);
}

LocalTime local_time() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&now, &tm_now);
    return {tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec};
}

bool write_png(const std::string &path, int w, int h, const uint8_t *bgra, size_t bgra_bytes) {
    if (!bgra || w <= 0 || h <= 0) return false;
    const size_t row_bytes = static_cast<size_t>(w) * 4;
    if (bgra_bytes < row_bytes * static_cast<size_t>(h)) return false;

    // Build uncompressed PNG scanlines (filter byte 0 + RGBA pixels).
    const size_t scanline_bytes = 1 + row_bytes;
    const size_t raw_bytes = scanline_bytes * static_cast<size_t>(h);
    std::vector<uint8_t> raw(raw_bytes);
    uint32_t s1 = 1, s2 = 0;
    constexpr uint32_t kMod = 65521u;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = bgra + static_cast<size_t>(y) * row_bytes;
        uint8_t *dst = raw.data() + static_cast<size_t>(y) * scanline_bytes;
        dst[0] = 0;  // filter type None
        s1 = (s1 + dst[0]) % kMod;
        s2 = (s2 + s1) % kMod;
        for (int x = 0; x < w; x++) {
            const uint8_t r = src[4 * x + 2];
            const uint8_t g = src[4 * x + 1];
            const uint8_t b = src[4 * x + 0];
            const uint8_t a = src[4 * x + 3];
            dst[1 + 4 * x + 0] = r;
            dst[1 + 4 * x + 1] = g;
            dst[1 + 4 * x + 2] = b;
            dst[1 + 4 * x + 3] = a;
            s1 = (s1 + r) % kMod; s2 = (s2 + s1) % kMod;
            s1 = (s1 + g) % kMod; s2 = (s2 + s1) % kMod;
            s1 = (s1 + b) % kMod; s2 = (s2 + s1) % kMod;
            s1 = (s1 + a) % kMod; s2 = (s2 + s1) % kMod;
        }
    }
    const uint32_t adler = (s2 << 16) | s1;

    // Wrap raw scanlines in zlib stored-DEFLATE stream.
    std::vector<uint8_t> idat;
    idat.reserve(raw_bytes + (raw_bytes / 65535 + 1) * 5 + 6);
    idat.push_back(0x78);  // CMF: deflate, 32K window
    idat.push_back(0x01);  // FLG: no dict, fastest (FCHECK valid: 0x7801 % 31 == 0)
    size_t offset = 0;
    while (offset < raw_bytes) {
        const size_t chunk = std::min<size_t>(65535u, raw_bytes - offset);
        const bool last = (offset + chunk) == raw_bytes;
        idat.push_back(last ? 0x01u : 0x00u);
        const uint16_t len = static_cast<uint16_t>(chunk);
        const uint16_t nlen = static_cast<uint16_t>(~len);
        idat.push_back(static_cast<uint8_t>(len & 0xFFu));
        idat.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
        idat.push_back(static_cast<uint8_t>(nlen & 0xFFu));
        idat.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFFu));
        idat.insert(idat.end(), raw.data() + offset, raw.data() + offset + chunk);
        offset += chunk;
    }
    push_be32(idat, adler);

    std::vector<uint8_t> png;
    png.reserve(64 + idat.size());
    static const uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), kSig, kSig + 8);

    uint8_t ihdr[13] = {};
    ihdr[0] = static_cast<uint8_t>((w >> 24) & 0xFF);
    ihdr[1] = static_cast<uint8_t>((w >> 16) & 0xFF);
    ihdr[2] = static_cast<uint8_t>((w >> 8) & 0xFF);
    ihdr[3] = static_cast<uint8_t>(w & 0xFF);
    ihdr[4] = static_cast<uint8_t>((h >> 24) & 0xFF);
    ihdr[5] = static_cast<uint8_t>((h >> 16) & 0xFF);
    ihdr[6] = static_cast<uint8_t>((h >> 8) & 0xFF);
    ihdr[7] = static_cast<uint8_t>(h & 0xFF);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // color type: RGBA
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = 0;  // interlace
    push_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    push_chunk(png, "IDAT", idat.data(), idat.size());
    push_chunk(png, "IEND", nullptr, 0);

    const std::string tmp = path + ".tmp";
    std::FILE *fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) return false;
    const size_t written = std::fwrite(png.data(), 1, png.size(), fp);
    const int flush_rc = std::fflush(fp);
    const int close_rc = std::fclose(fp);
    if (written != png.size() || flush_rc != 0 || close_rc != 0) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void worker_thread_init() {}

}  // namespace snap_photoexport::host

extern "C" __attribute__((weak)) void snap_shiny_pixels(uint64_t, uint64_t, const int *) {}
extern "C" __attribute__((weak)) void snap_shiny_photo_encoded(uint64_t, const uint8_t *, size_t) {}
extern "C" __attribute__((weak)) void snap_hdphoto_pixels(uint64_t, uint64_t, const int *) {}
extern "C" __attribute__((weak)) void snap_hdphoto_encoded(uint64_t, const uint8_t *, size_t) {}
