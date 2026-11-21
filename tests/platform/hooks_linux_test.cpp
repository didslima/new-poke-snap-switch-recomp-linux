// SPDX-License-Identifier: MIT
// Stage 5A: tests host/hooks.cpp on Linux with host/linux/hooks_mods_stub.cpp.
// Exercises install_libc_overrides(), setjmp/longjmp hooks, SNAP_MEMLOG atomic/thread_id tracking,
// and verifies that unported mod stubs do not register any overrides.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "loader.h"
#include "mods/photoexport_host.h"

extern "C" void snap_photoexport_drain_for_exit(int ms);

static float g_test_wide_k = 2.0f;
extern "C" float nvnHostGetUltrawide(void) { return g_test_wide_k; }

extern "C" {
void rt_hook_setjmp(Cpu *c);
void rt_hook_longjmp(Cpu *c);
}
void snap_memev(char k, uint64_t a, uint64_t sz, uint64_t lr);
void snap_memlog_find(uint64_t addr);

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

int main() {
    // 1. Install libc overrides (also calls the Linux no-op mod stubs).
    loader::install_libc_overrides();

    Thunk *t_memcpy = loader::find_override("memcpy");
    Thunk *t_memmove = loader::find_override("memmove");
    Thunk *t_memset = loader::find_override("memset");
    Thunk *t_strlen = loader::find_override("strlen");
    CHECK(t_memcpy != nullptr && t_memcpy->fn1 != nullptr);
    CHECK(t_memmove != nullptr && t_memmove->fn1 != nullptr);
    CHECK(t_memset != nullptr && t_memset->fn1 != nullptr);
    CHECK(t_strlen != nullptr && t_strlen->fn1 != nullptr);

    // 2. Linux mod stubs must not register or advertise mod overrides.
    CHECK(loader::find_override("_ZN2nn4util8snprintfEPcmPKcz") == nullptr);
    CHECK(loader::find_override("host:lodanim") == nullptr);
    CHECK(loader::thunk_addr("ovr:memcpy") == t_memcpy->addr);

    // 3. Exercise libc leaf overrides via Cpu.
    Cpu c{};
    char dst[32] = {};
    const char src[] = "snap-linux-5a";
    c.x[0] = (uint64_t)(uintptr_t)dst;
    c.x[1] = (uint64_t)(uintptr_t)src;
    c.x[2] = sizeof(src);
    t_memcpy->fn1(&c);
    CHECK(std::strcmp(dst, src) == 0);

    c.x[0] = (uint64_t)(uintptr_t)dst;
    t_strlen->fn1(&c);
    CHECK(c.x[0] == std::strlen(src));

    c.x[0] = (uint64_t)(uintptr_t)dst;
    c.x[1] = 'X';
    c.x[2] = 4;
    t_memset->fn1(&c);
    CHECK(std::memcmp(dst, "XXXX", 4) == 0);

    // 4. Exercise setjmp / longjmp hooks.
    uint64_t jb[22] = {};
    Cpu sc{};
    sc.x[0] = (uint64_t)(uintptr_t)jb;
    sc.x[19] = 0x1122334455667788ull;
    sc.x[29] = 0x7000100000ull;
    sc.x[30] = 0x7100001234ull;
    sc.sp = 0x70000ff000ull;
    rt_hook_setjmp(&sc);
    CHECK(sc.x[0] == 0);

    Cpu lc{};
    lc.x[0] = (uint64_t)(uintptr_t)jb;
    lc.x[1] = 42;
    rt_hook_longjmp(&lc);
    CHECK(lc.resuming == 1);
    CHECK(lc.x[0] == 42);
    CHECK(lc.x[19] == 0x1122334455667788ull);
    CHECK(lc.sp == 0x70000ff000ull);
    CHECK(lc.pc == 0x7100001234ull);

    // 5. Concurrent snap_memev across threads (atomic index + osk::host::thread_id()).
    std::vector<std::thread> ths;
    for (int t = 0; t < 4; t++) {
        ths.emplace_back([t]() {
            for (int i = 0; i < 200; i++)
                snap_memev('m', 0x7000000000ull + (uint64_t)t * 0x1000, 64, 0);
        });
    }
    for (auto &th : ths) th.join();
    snap_memlog_find(0x7000000000ull);

    // 6. Experimental Cutscene Ultrawide (SNAP_CUTSCENE_ULTRAWIDE) regression tests.
    {
        Thunk *t_tanf = loader::find_override("tanf");
        CHECK(t_tanf != nullptr && t_tanf->fn1 != nullptr);

        auto run_tanf = [&](uint64_t off, int reg, float aspect_in, float half_fov_rad) {
            Cpu tc{};
            uint32_t xb = 0, ab = 0;
            std::memcpy(&xb, &half_fov_rad, 4);
            std::memcpy(&ab, &aspect_in, 4);
            tc.v[0] = (__uint128_t)xb;
            tc.v[reg] = (__uint128_t)ab;
            tc.x[30] = GUEST_MAIN_BASE + off;
            t_tanf->fn1(&tc);
            float out_tan = 0.0f, out_aspect = 0.0f;
            const uint32_t out_tb = (uint32_t)tc.v[0];
            const uint32_t out_ab = (uint32_t)tc.v[reg];
            std::memcpy(&out_tan, &out_tb, 4);
            std::memcpy(&out_aspect, &out_ab, 4);
            return std::pair<float, float>{out_tan, out_aspect};
        };

        const float k169 = 16.0f / 9.0f;
        const float k329 = 32.0f / 9.0f;
        const float half = 0.349066f;  // 20 deg half-angle
        const float expected_tan = std::tan(half);
        g_test_wide_k = 2.0f;

        // Toggle absent: original 0x296400 widens, cutscene sites remain 16:9.
        unsetenv("SNAP_CUTSCENE_ULTRAWIDE");
        CHECK(std::fabs(run_tanf(0x296400, 9, k169, half).second - k329) < 0.001f);
        CHECK(std::fabs(run_tanf(0x38cdbc, 9, k169, half).second - k169) < 0.001f);
        CHECK(std::fabs(run_tanf(0x29249c, 8, k169, half).second - k169) < 0.001f);
        CHECK(std::fabs(run_tanf(0x2973b0, 12, k169, half).second - k169) < 0.001f);
        CHECK(std::fabs(run_tanf(0x297cd4, 10, k169, half).second - k169) < 0.001f);

        // Toggle = 0: cutscene sites still remain 16:9.
        setenv("SNAP_CUTSCENE_ULTRAWIDE", "0", 1);
        CHECK(std::fabs(run_tanf(0x38cdbc, 9, k169, half).second - k169) < 0.001f);
        CHECK(std::fabs(run_tanf(0x29249c, 8, k169, half).second - k169) < 0.001f);
        CHECK(std::fabs(run_tanf(0x2973b0, 12, k169, half).second - k169) < 0.001f);
        CHECK(std::fabs(run_tanf(0x297cd4, 10, k169, half).second - k169) < 0.001f);

        // Toggle = 1: all 4 cutscene/transition sites widen 16:9 -> 32:9 while preserving vertical FOV (tanf).
        setenv("SNAP_CUTSCENE_ULTRAWIDE", "1", 1);
        const auto r1 = run_tanf(0x38cdbc, 9, k169, half);
        const auto r2 = run_tanf(0x29249c, 8, k169, half);
        const auto r3 = run_tanf(0x2973b0, 12, k169, half);
        const auto r4 = run_tanf(0x297cd4, 10, k169, half);
        CHECK(std::fabs(r1.first - expected_tan) < 1e-5f && std::fabs(r1.second - k329) < 0.001f);
        CHECK(std::fabs(r2.first - expected_tan) < 1e-5f && std::fabs(r2.second - k329) < 0.001f);
        CHECK(std::fabs(r3.first - expected_tan) < 1e-5f && std::fabs(r3.second - k329) < 0.001f);
        CHECK(std::fabs(r4.first - expected_tan) < 1e-5f && std::fabs(r4.second - k329) < 0.001f);

        // Blend double-expansion guard: already-widened 32:9 input must NOT become 64:9.
        const auto r_blend_already_wide = run_tanf(0x2973b0, 12, k329, half);
        const auto r_blend2_already_wide = run_tanf(0x297cd4, 10, k329, half);
        CHECK(std::fabs(r_blend_already_wide.second - k329) < 0.001f);
        CHECK(std::fabs(r_blend2_already_wide.second - k329) < 0.001f);

        // Original site 0x296400 still works with toggle = 1.
        const auto r_orig = run_tanf(0x296400, 9, k169, half);
        CHECK(std::fabs(r_orig.first - expected_tan) < 1e-5f && std::fabs(r_orig.second - k329) < 0.001f);

        unsetenv("SNAP_CUTSCENE_ULTRAWIDE");
    }

    // 7. Linux photoexport (nn::image::JpegEncoder hooks + XDG default + async PNG + JSON sidecar).
    {
        Thunk *t_setpix = loader::find_override("_ZN2nn5image11JpegEncoder12SetPixelDataEPKvNS0_11PixelFormatERKNS0_9DimensionEi");
        Thunk *t_encode = loader::find_override("_ZN2nn5image11JpegEncoder6EncodeEPmPvmS3_mPNS0_11ExifBuilderE");
        Thunk *t_setqual = loader::find_override("_ZN2nn5image11JpegEncoder10SetQualityEi");
        CHECK(t_setpix != nullptr && t_setpix->fn1 != nullptr);
        CHECK(t_encode != nullptr && t_encode->fn1 != nullptr);
        CHECK(t_setqual != nullptr && t_setqual->fn1 != nullptr);

        setenv("XDG_DATA_HOME", "/tmp/snap_xdg_data_test", 1);
        CHECK(snap_photoexport::host::default_photos_dir() == "/tmp/snap_xdg_data_test/SnapRecomp/photos");
        unsetenv("XDG_DATA_HOME");

        namespace fs = std::filesystem;
        const fs::path photo_dir = fs::temp_directory_path() / "snap_photoexport_test_dir";
        std::error_code ec;
        fs::remove_all(photo_dir, ec);

        setenv("SNAP_PHOTO_EXPORT", "1", 1);
        setenv("SNAP_PHOTO_DIR", photo_dir.c_str(), 1);
        setenv("SNAP_PHOTO_MIN_W", "64", 1);
        setenv("SNAP_PHOTO_LOG", "0", 1);

        // 7a. Thumbnail below SNAP_PHOTO_MIN_W (32x32) must be skipped.
        std::vector<uint8_t> thumb_rgba(32 * 32 * 4, 0xAA);
        int dim_thumb[2] = {32, 32};
        uint64_t jpg_sz = 16;
        uint8_t fake_jpg[16] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xFF, 0xD9};

        Cpu pc{};
        pc.x[0] = 0x1000;
        pc.x[1] = (uint64_t)(uintptr_t)thumb_rgba.data();
        pc.x[2] = 14;  // RGBA8
        pc.x[3] = (uint64_t)(uintptr_t)dim_thumb;
        pc.x[4] = 1;
        t_setpix->fn1(&pc);

        pc.x[0] = 0x1000;
        pc.x[1] = (uint64_t)(uintptr_t)&jpg_sz;
        pc.x[2] = (uint64_t)(uintptr_t)fake_jpg;
        t_encode->fn1(&pc);
        snap_photoexport_drain_for_exit(1000);
        CHECK(!fs::exists(photo_dir));

        // 7b. Full-size photo (>= 64px wide) writes lossless PNG + JSON sidecar, and deduplicates identical JPEG CRC32.
        std::vector<uint8_t> full_rgba(64 * 32 * 4, 0);
        for (size_t i = 0; i < 64 * 32; i++) {
            full_rgba[4 * i + 0] = 0x12;
            full_rgba[4 * i + 1] = 0x34;
            full_rgba[4 * i + 2] = 0x56;
            full_rgba[4 * i + 3] = 0x80;
        }
        int dim_full[2] = {64, 32};
        pc.x[0] = 0x2000;
        pc.x[1] = (uint64_t)(uintptr_t)full_rgba.data();
        pc.x[2] = 14;
        pc.x[3] = (uint64_t)(uintptr_t)dim_full;
        pc.x[4] = 1;
        t_setpix->fn1(&pc);

        pc.x[0] = 0x2000;
        pc.x[1] = (uint64_t)(uintptr_t)&jpg_sz;
        pc.x[2] = (uint64_t)(uintptr_t)fake_jpg;
        t_encode->fn1(&pc);
        // Second encode with identical JPEG bytes must be deduplicated.
        pc.x[0] = 0x2000;
        pc.x[1] = (uint64_t)(uintptr_t)&jpg_sz;
        pc.x[2] = (uint64_t)(uintptr_t)fake_jpg;
        t_encode->fn1(&pc);

        snap_photoexport_drain_for_exit(2000);

        int png_count = 0, json_count = 0;
        fs::path png_path, json_path;
        if (fs::exists(photo_dir)) {
            for (const auto &entry : fs::directory_iterator(photo_dir)) {
                if (entry.path().extension() == ".png") { png_count++; png_path = entry.path(); }
                if (entry.path().extension() == ".json") { json_count++; json_path = entry.path(); }
            }
        }
        CHECK(png_count == 1);
        CHECK(json_count == 1);

        if (!png_path.empty()) {
            FILE *fp = std::fopen(png_path.c_str(), "rb");
            CHECK(fp != nullptr);
            if (fp) {
                uint8_t hdr[24] = {};
                const size_t n = std::fread(hdr, 1, sizeof(hdr), fp);
                std::fclose(fp);
                CHECK(n == 24);
                static const uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
                CHECK(std::memcmp(hdr, kSig, 8) == 0);
                CHECK(std::memcmp(hdr + 12, "IHDR", 4) == 0);
                const uint32_t pw = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
                const uint32_t ph = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
                CHECK(pw == 64 && ph == 32);
            }
        }
        if (!json_path.empty()) {
            FILE *fp = std::fopen(json_path.c_str(), "rb");
            CHECK(fp != nullptr);
            if (fp) {
                char buf[512] = {};
                const size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
                std::fclose(fp);
                buf[n] = '\0';
                CHECK(std::strstr(buf, "\"width\": 64") != nullptr);
                CHECK(std::strstr(buf, "\"height\": 32") != nullptr);
                CHECK(std::strstr(buf, "\"jpeg_bytes\": 16") != nullptr);
                CHECK(std::strstr(buf, "\"jpeg_crc32\": \"") != nullptr);
            }
        }

        fs::remove_all(photo_dir, ec);
        unsetenv("SNAP_PHOTO_EXPORT");
        unsetenv("SNAP_PHOTO_DIR");
        unsetenv("SNAP_PHOTO_MIN_W");
        unsetenv("SNAP_PHOTO_LOG");
    }

    std::printf("hooks_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
