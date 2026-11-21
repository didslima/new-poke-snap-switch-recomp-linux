// Full-resolution photo export (mod: every photo the game saves to the album is also written as a PNG; project code, ours).
//
// The game encodes each album photo (and its thumbnail) through nn::image::JpegEncoder, which is lifted sdk code. Two host
// overrides wrap it: SetPixelData records the pixel buffer, format and size per encoder object; Encode runs the original, and
// when the pixels are photo-sized (width >= SNAP_PHOTO_MIN_W, default 640) the pixels are copied and a worker thread writes
// <photos>/photo_<date>_<time>_<n>.png (lossless) plus a .json sidecar with the size, the game's JPEG byte size and its CRC32
// (the gallery links an export to its album entry through that CRC).
//
//   SNAP_PHOTO_EXPORT=1|0   enables / disables the export (default per hle/visual_defaults.h: off, on with SNAP_VISUALS=enhanced; when off the overrides are not registered at all).
//   SNAP_PHOTO_DIR=<dir>    output folder (default %APPDATA%\SnapRecomp\photos on Windows, $XDG_DATA_HOME/SnapRecomp/photos on Linux; test runs should set it).
//   SNAP_PHOTO_MIN_W=<n>    smallest width that counts as a photo (default 640; thumbnails are smaller).
//   SNAP_PHOTO_LOG=1        logs every encoder call (format, size, stride) to stderr.
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "loader.h"
#include "hle/visual_defaults.h"   // default on/off (SNAP_VISUALS)
#include "photoexport_host.h"

using namespace loader;

extern "C" void snap_shiny_photo_encoded(uint64_t enc, const uint8_t *jpg, size_t n);
extern "C" void snap_shiny_pixels(uint64_t enc, uint64_t data, const int *dim);   // host/mods/shiny.cpp
extern "C" void snap_hdphoto_pixels(uint64_t enc, uint64_t data, const int *dim);                   // runtime/nvn/nvn_hdphoto.cpp (HD photos; no-op when off)
extern "C" void snap_hdphoto_encoded(uint64_t enc, const uint8_t *jpg, size_t n);

namespace {

const char *kSetPixel = "_ZN2nn5image11JpegEncoder12SetPixelDataEPKvNS0_11PixelFormatERKNS0_9DimensionEi";
const char *kEncode = "_ZN2nn5image11JpegEncoder6EncodeEPmPvmS3_mPNS0_11ExifBuilderE";

struct PixInfo { uint64_t data; int fmt, w, h, align; };
struct Job { std::vector<uint8_t> bgra; int w, h; std::string base; uint32_t jpg_size, jpg_crc; int fmt, align; };

struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<uint64_t, PixInfo> pix;
    std::unordered_set<uint32_t> seen;
    std::deque<Job> q;
    bool log = false, started = false, busy = false;   // busy: the worker holds a job (under mu)
    std::condition_variable idle_cv;
    int min_w = 640;
    long count = 0;
    std::string dir;
};

State &st() {
    static State *s = new State();
    return *s;
}

bool ascii_ieq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    static bool init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

void worker() {
    snap_photoexport::host::worker_thread_init();
    State &s = st();
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(s.mu);
            s.cv.wait(lk, [&s] { return !s.q.empty(); });
            j = std::move(s.q.front());
            s.q.pop_front();
            s.busy = true;
        }
        std::string png = snap_photoexport::host::join_path(s.dir, j.base + ".png");
        bool ok = snap_photoexport::host::write_png(png, j.w, j.h, j.bgra.data(), j.bgra.size());
        if (ok) {
            char js[512];
            snprintf(js, sizeof js, "{\"png\": \"%s.png\", \"width\": %d, \"height\": %d, \"jpeg_bytes\": %u, \"jpeg_crc32\": \"%08x\", \"pixel_format\": %d, \"source\": \"nn::image::JpegEncoder\"}\n",
                     j.base.c_str(), j.w, j.h, j.jpg_size, j.jpg_crc, j.fmt);
            FILE *fp = fopen(snap_photoexport::host::join_path(s.dir, j.base + ".json").c_str(), "wb");
            if (fp) { fputs(js, fp); fclose(fp); }
        }
        fprintf(stderr, "[photo-export] %s %s (%dx%d)\n", ok ? "wrote" : "FAILED", png.c_str(), j.w, j.h);
        { std::lock_guard<std::mutex> lk(s.mu); s.busy = false; }
        s.idle_cv.notify_all();
    }
}

void ensure_started() {
    State &s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.started) return;
    s.started = true;
    snap_photoexport::host::ensure_dir(s.dir);
    std::thread(worker).detach();
}

// Off words (case-insensitive) disable; on words enable; anything else falls back to the profile default.
bool parse_enabled(const char *e) {
    if (!e || !*e) e = snapdef::def("SNAP_PHOTO_EXPORT");
    if (!e) return false;
    if (ascii_ieq(e, "0") || ascii_ieq(e, "off") || ascii_ieq(e, "no") || ascii_ieq(e, "false") || ascii_ieq(e, "n") || ascii_ieq(e, "f")) return false;
    if (ascii_ieq(e, "1") || ascii_ieq(e, "on") || ascii_ieq(e, "yes") || ascii_ieq(e, "true") || ascii_ieq(e, "y") || ascii_ieq(e, "t")) return true;
    const char *d = snapdef::def("SNAP_PHOTO_EXPORT");
    return d && d[0] == '1';
}

// Resolved once, at the first hook call (after settings.cfg has been exported to the env).
bool enabled() {
    static std::once_flag once;
    static bool on;
    std::call_once(once, [] {
        if (!parse_enabled(snapdef::get("SNAP_PHOTO_EXPORT"))) return;
        State &s = st();
        s.log = getenv("SNAP_PHOTO_LOG") && getenv("SNAP_PHOTO_LOG")[0] != '0';
        if (const char *m = getenv("SNAP_PHOTO_MIN_W")) { int v = atoi(m); if (v > 0) s.min_w = v; }
        const char *d = getenv("SNAP_PHOTO_DIR");
        if (d && *d) s.dir = d;
        else if (getenv("SNAP_SAVE_DIR")) return;   // test runs (they set SNAP_SAVE_DIR) never write into the player's photo folder
        else s.dir = snap_photoexport::host::default_photos_dir();
        on = true;
    });
    return on;
}

// Source pixel format: the game passes 14 (R,G,B,A bytes); that is the only one converted. Photos are 1080x720, thumbnails 240x200 and 204x136.
void h_setpixel(Cpu *c) {
    snap_shiny_pixels(c->x[0], c->x[1], (const int *)c->x[3]);
    snap_hdphoto_pixels(c->x[0], c->x[1], (const int *)c->x[3]);   // HD photos: which buffer this encoder compresses
    if (!enabled()) {
        uint64_t o = lookup(kSetPixel);
        if (o) c->x[0] = call_guest_nested(c, o, {c->x[0], c->x[1], c->x[2], c->x[3], c->x[4]});
        return;
    }
    State &s = st();
    uint64_t self = c->x[0];
    PixInfo p{c->x[1], (int)c->x[2], 0, 0, (int)c->x[4]};
    const int *dim = (const int *)c->x[3];
    if (dim) { p.w = dim[0]; p.h = dim[1]; }
    { std::lock_guard<std::mutex> lk(s.mu); s.pix[self] = p; }
    if (s.log) fprintf(stderr, "[photo-export] SetPixelData enc=%#llx data=%#llx fmt=%d %dx%d align=%d\n", (unsigned long long)self, (unsigned long long)p.data, p.fmt, p.w, p.h, p.align);
    uint64_t orig = lookup(kSetPixel);
    if (orig) c->x[0] = call_guest_nested(c, orig, {c->x[0], c->x[1], c->x[2], c->x[3], c->x[4]});
}

// Log only (SNAP_PHOTO_LOG=1): the game retries a failed Encode (result 0x...ffbf, buffer too small) with a lower quality (main+0x473868),
// so the quality of each attempt shows how much a photo had to be squeezed into its save budget.
const char *kSetQuality = "_ZN2nn5image11JpegEncoder10SetQualityEi";
void h_setquality(Cpu *c) {
    (void)enabled();   // reads SNAP_PHOTO_LOG once
    if (st().log) fprintf(stderr, "[photo-export] SetQuality enc=%#llx quality=%d\n", (unsigned long long)c->x[0], (int)c->x[1]);
    uint64_t orig = lookup(kSetQuality);
    if (orig) c->x[0] = call_guest_nested(c, orig, {c->x[0], c->x[1]});
}

void h_encode(Cpu *c) {
    uint64_t self = c->x[0], psize = c->x[1], out = c->x[2];
    uint64_t args[7] = {c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6]};
    uint64_t orig = lookup(kEncode);
    uint64_t res = orig ? call_guest_nested(c, orig, {args[0], args[1], args[2], args[3], args[4], args[5], args[6]}) : 0;
    if ((uint32_t)res == 0 && psize && out) snap_shiny_photo_encoded(self, (const uint8_t *)out, (size_t)*(const uint64_t *)psize);   // shiny mod (host/mods/shiny.cpp; off unless SNAP_SHINY=1)
    if ((uint32_t)res == 0 && psize && out) snap_hdphoto_encoded(self, (const uint8_t *)out, (size_t)*(const uint64_t *)psize);   // HD photos: the album JPEG names the HD copy
    if (!enabled()) { c->x[0] = res; return; }
    State &s = st();
    PixInfo p{};
    bool have = false;
    { std::lock_guard<std::mutex> lk(s.mu); auto it = s.pix.find(self); if (it != s.pix.end()) { p = it->second; have = true; } }
    uint64_t jsz = psize ? *(const uint64_t *)psize : 0;
    if (s.log) fprintf(stderr, "[photo-export] Encode enc=%#llx result=%#llx jpeg=%llu have=%d\n", (unsigned long long)self, (unsigned long long)res, (unsigned long long)jsz, have);
    c->x[0] = res;
    if (!have || (uint32_t)res != 0) return;   // failed encode
    if (p.w < s.min_w || p.h < 1 || p.w > 16384 || p.h > 16384 || !p.data) return;
    int bpp = 4;
    if (p.fmt != 14) { fprintf(stderr, "[photo-export] pixel format %d not handled, photo skipped\n", p.fmt); return; }
    size_t stride = (size_t)p.w * bpp;
    if (p.align > 1) stride = (stride + p.align - 1) / p.align * p.align;
    Job j;
    j.w = p.w; j.h = p.h; j.fmt = p.fmt; j.align = p.align;
    j.bgra.resize((size_t)p.w * p.h * 4);
    const uint8_t *src = (const uint8_t *)p.data;
    for (int y = 0; y < p.h; y++) {
        const uint8_t *sr = src + y * stride;
        uint8_t *d = &j.bgra[(size_t)y * p.w * 4];
        for (int x = 0; x < p.w; x++) {
            d[4 * x + 0] = sr[bpp * x + 2];
            d[4 * x + 1] = sr[bpp * x + 1];
            d[4 * x + 2] = sr[bpp * x + 0];
            d[4 * x + 3] = 255;   // photos are opaque; the game's alpha is not meaningful in a JPEG
        }
    }
    j.jpg_size = (uint32_t)jsz;
    j.jpg_crc = (out && jsz && jsz < (1u << 26)) ? crc32((const uint8_t *)out, (size_t)jsz) : 0;
    if (j.jpg_crc) {   // the game encodes some photos twice; write each distinct JPEG once per session
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.seen.insert(j.jpg_crc).second) return;
    }
    const snap_photoexport::host::LocalTime t = snap_photoexport::host::local_time();
    char nm[96];
    long n;
    { std::lock_guard<std::mutex> lk(s.mu); n = ++s.count; }
    snprintf(nm, sizeof nm, "photo_%04d%02d%02d_%02d%02d%02d_%03ld", t.year, t.month, t.day, t.hour, t.minute, t.second, n);
    j.base = nm;
    ensure_started();
    { std::lock_guard<std::mutex> lk(s.mu); s.q.push_back(std::move(j)); }
    s.cv.notify_one();
}

}  // namespace

namespace loader {
// The hooks are always registered; the env (SNAP_PHOTO_EXPORT / SNAP_PHOTO_DIR, which settings.cfg exports in bind_host_layers, after
// install_libc_overrides) is read lazily at the first hook call.
void snap_photoexport_install() {
    add_override(kSetPixel, h_setpixel);
    add_override(kEncode, h_encode);
    add_override(kSetQuality, h_setquality);
}

// Waits (bounded) until every queued photo export is written. Called by snap_fs_drain_for_exit (runtime/hle/fs.cpp), which the
// WM_CLOSE path and ov_QuickExit (runtime/os/os_thread.cpp) both call.
extern "C" void snap_photoexport_drain_for_exit(int ms) {
    State &s = st();
    std::unique_lock<std::mutex> lk(s.mu);
    s.idle_cv.wait_for(lk, std::chrono::milliseconds(ms), [&s] { return s.q.empty() && !s.busy; });
}
}  // namespace loader
