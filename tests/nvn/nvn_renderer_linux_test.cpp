// SPDX-License-Identifier: MIT
// Stage 6E-A Group 2E: Linux NVN->Vulkan renderer link-closure & initialization validation.
// Links against the complete production portable runtime ($<TARGET_OBJECTS:snap_portable_objects>)
// without running any guest/game code.
#include "nvn_os_host.h"
#include "nvn_os_host_vk.h"
#include "nvn_host.h"
#include "nvn_hdr.h"
#include "nvn_internal.h"
#include "nvn_foliage.h"
#include "nvn_motionaa.h"
#include "upscale/upscale_dlss_host.h"
#include "upscale/upscale_nvn.h"
#include "upscale/upscaler.h"
#include "hle/live.h"
#include "hle/settings.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <ffx_shader_blobs.h>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

// os_kernel.cpp calls rt_dispatch for guest execution; this renderer link test never runs guest code.
extern "C" void rt_dispatch(void*) {
  std::fprintf(stderr, "unexpected rt_dispatch call in nvn_renderer_linux_test\n");
  std::abort();
}

extern "C" {
void (*nvnBootstrapLoader(const char* name))(void);
void nvnHostSetFramePacing(double hz);
}

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* msg, int line) {
  ++g_checks;
  if (!ok) {
    ++g_fails;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, msg);
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void test_nvn_bootstrap_table() {
  CHECK(nvnBootstrapLoader("nvnDeviceInitialize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnDeviceFinalize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnDeviceGetProcAddress") != nullptr);
  CHECK(nvnBootstrapLoader("nvnQueueInitialize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnWindowInitialize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnCommandBufferDrawArrays") != nullptr);
  CHECK(nvnBootstrapLoader("nvnTextureInitialize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnSyncWait") != nullptr);
  CHECK(nvnBootstrapLoader("nonexistent_entry_point") == nullptr);
  nvnHostSetFramePacing(60.0);
}

void test_vulkan_device_and_sdl_surface() {
#ifdef HLE_WITH_SDL3
  const bool sdl_ok = SDL_Init(SDL_INIT_VIDEO);
#else
  const bool sdl_ok = false;
#endif

  using FnDevBuilderSetDefaults = void (*)(void*);
  using FnDevInit = uint8_t (*)(void*, const void*);
  using FnDevFinalize = void (*)(void*);

  auto dev_builder_defaults = reinterpret_cast<FnDevBuilderSetDefaults>(nvnBootstrapLoader("nvnDeviceBuilderSetDefaults"));
  auto dev_init = reinterpret_cast<FnDevInit>(nvnBootstrapLoader("nvnDeviceInitialize"));
  auto dev_finalize = reinterpret_cast<FnDevFinalize>(nvnBootstrapLoader("nvnDeviceFinalize"));
  CHECK(dev_builder_defaults != nullptr && dev_init != nullptr && dev_finalize != nullptr);

  // Storage for opaque NVNDevice / NVNDeviceBuilder (host layer only stores pointers in HandleMap).
  alignas(16) uint8_t dev_builder[256]{};
  alignas(16) uint8_t dev[256]{};
  dev_builder_defaults(dev_builder);

  // Only attempt real VkDevice creation when a Vulkan 1.3 physical device exists;
  // in headless/no-ICD environments, DeviceInitialize must return 0 cleanly without crashing.
  const uint8_t init_ok = dev_init(dev, dev_builder);
  if (init_ok) {
    std::printf("nvn_renderer_linux_test: real Vulkan 1.3 Host::Init succeeded\n");
#ifdef HLE_WITH_SDL3
    if (sdl_ok) {
      SDL_Window* win = SDL_CreateWindow("nvn_renderer_linux_test", 128, 128, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
      CHECK(win != nullptr);
      if (win) {
        nvn::os_host::PaceDisplayInfo dinfo{};
        (void)nvn::os_host::pace_query_display(win, 0, &dinfo);
        SDL_DestroyWindow(win);
      }
    }
#endif
    dev_finalize(dev);
  } else {
    std::printf("nvn_renderer_linux_test: no Vulkan 1.3 ICD/GPU available; DeviceInitialize returned 0 cleanly\n");
  }

#ifdef HLE_WITH_SDL3
  if (sdl_ok) SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
}

extern "C" {
unsigned snap_mods_ab_period();
bool snap_mods_ab_live(unsigned present);
const uint8_t* snap_mods_ab_original(const uint8_t* src, uint32_t w, uint32_t h, uint32_t mips,
                                     uint32_t* ow, uint32_t* oh, uint32_t* om);
void snap_shiny_fx_trigger(const char* species);
int snap_live_size_blocked(void);
}

void test_live_graphics_and_ultrawide() {
  // Isolate settings writes into a temporary SNAP_CONFIG_DIR so user settings are untouched.
  const std::filesystem::path tmp_cfg = std::filesystem::temp_directory_path() / "snap_nvn_live_test_cfg";
  std::error_code ec;
  std::filesystem::remove_all(tmp_cfg, ec);
  std::filesystem::create_directories(tmp_cfg, ec);
  const char* prev_cfg = std::getenv("SNAP_CONFIG_DIR");
  const std::string saved_cfg = prev_cfg ? prev_cfg : "";
  setenv("SNAP_CONFIG_DIR", tmp_cfg.c_str(), 1);
  hle::settings::load();

  std::atomic<unsigned> seen{0};
  (void)hle::live::changed(seen);

  // Verify each v1.3.0 live-apply graphics setting bumps the live generation via hle::settings::set.
  const struct { const char* k; const char* v; } live_cases[] = {
      {"gfx_render_scale", "1.5"},
      {"gfx_aa", "smaa"},
      {"gfx_shadow_res", "2048"},
      {"gfx_ultrawide", "off"},
      {"gfx_ultrawide", "auto"},
      {"ssao_quality", "high"},
  };
  for (const auto& tc : live_cases) {
    hle::settings::set(tc.k, tc.v);
    CHECK(hle::live::changed(seen));
    CHECK(!hle::live::changed(seen));
  }

  // Verify SpecularAaEnabled() defaults OFF and toggles live via specular_aa.
  CHECK(!nvn::SpecularAaEnabled());
  hle::settings::set("specular_aa", "1");
  CHECK(hle::live::changed(seen));
  CHECK(nvn::SpecularAaEnabled());
  hle::settings::set("specular_aa", "default");
  CHECK(hle::live::changed(seen));
  CHECK(!nvn::SpecularAaEnabled());

  // Verify HdfxFactorNow() defaults OFF (1.0f) and responds to SNAP_HD_EFFECTS.
  unsetenv("SNAP_HD_EFFECTS");
  CHECK(std::fabs(nvn::HdfxFactorNow() - 1.0f) < 0.001f);
  setenv("SNAP_HD_EFFECTS", "1", 1);
  CHECK(std::fabs(nvn::HdfxFactorNow() - 2.0f) < 0.001f);
  unsetenv("SNAP_HD_EFFECTS");

  if (saved_cfg.empty()) unsetenv("SNAP_CONFIG_DIR");
  else setenv("SNAP_CONFIG_DIR", saved_cfg.c_str(), 1);
  std::filesystem::remove_all(tmp_cfg, ec);

  // Verify snap_live_size_blocked() reports 0 when no upscaler/framegen is active.
  CHECK(snap_live_size_blocked() == 0);

  // Verify live ultrawide factor updates (21:9, 32:9, and Off = 1.0) without caching.
  const float orig_uw = nvnHostGetUltrawide();
  nvnHostSetUltrawide(1.34f);
  CHECK(std::fabs(nvnHostGetUltrawide() - 1.34f) < 0.001f);
  nvnHostSetUltrawide(2.0f);
  CHECK(std::fabs(nvnHostGetUltrawide() - 2.0f) < 0.001f);
  nvnHostSetUltrawide(1.0f);
  CHECK(std::fabs(nvnHostGetUltrawide() - 1.0f) < 0.001f);
  nvnHostSetUltrawide(orig_uw);
}

void test_optional_stub_functions() {
  CHECK(snap_mods_ab_period() == 0u);
  CHECK(!snap_mods_ab_live(0));
  CHECK(!snap_mods_ab_live(120));
  uint32_t ow = 1, oh = 1, om = 1;
  const uint8_t dummy[4] = {};
  CHECK(snap_mods_ab_original(dummy, 64, 64, 1, &ow, &oh, &om) == nullptr);
  snap_shiny_fx_trigger("Pikachu");
}

void test_upscale_and_hdr_defaults() {
  float sc = 1.0f, oc = 1.0f;
  CHECK(!nvn::UpscaleApplyPolicy(&sc, &oc));
  float jx = 1.0f, jy = 1.0f;
  nvn::UpscaleSceneJitter(&jx, &jy);
  CHECK(jx == 0.0f && jy == 0.0f);
  nvn::UpscaleBeforeSubmit(0, nullptr);
  nvn::UpscaleOnPresent();
  nvn::UpscaleDestroy(nullptr);
  CHECK(!snap_hdr::Requested());
  CHECK(!snap_hdr::IsScrgb());

  const auto vt = snap_upscale::dlss::host::vtbl_layout();
  CHECK(vt.s_i == 4 && vt.s_u32 == 3 && vt.s_f == 1 && vt.s_p == 7);
  CHECK(vt.g_i == 12 && vt.g_u32 == 11 && vt.g_f == 9 && vt.g_p == 15);
  CHECK(!snap_upscale::dlss::host::app_data_dir().empty());

  setenv("SNAP_DLSS_DIR", "/tmp/snap_test_dlss_dir", 1);
  const auto dirs = snap_upscale::dlss::host::feature_search_dirs();
  unsetenv("SNAP_DLSS_DIR");
  CHECK(!dirs.empty() && dirs[0] == L"/tmp/snap_test_dlss_dir");

  setenv("SNAP_NGX_CORE", "/tmp/definitely_missing_ngx_core.so", 1);
  std::string err_desc;
  CHECK(snap_upscale::dlss::host::open_core(&err_desc) == nullptr);
  CHECK(err_desc.find("SNAP_NGX_CORE=") != std::string::npos);
  unsetenv("SNAP_NGX_CORE");
}

void test_ultrawide_cutscene_upscale_aspect() {
  const float orig_uw = nvnHostGetUltrawide();
  nvnHostSetUltrawide(1.340625f);

  nvn::TextureImpl disp{};
  disp.b.width = 1920;
  disp.b.height = 1080;
  disp.b.flags = nvn::TF_DISPLAY;
  disp.scale = 1440.0f / 1080.0f;
  disp.wide = 1.340625f;
  disp.hostW = 3432;
  disp.hostH = 1440;

  // Fixed EnsureOut preserves wide > 1.0f -> tonemap pass into DISPLAY keeps wideMode == 0 (full)
  // even when a 16:9 Skip cutscene produced zero widened 3D draws.
  nvn::TextureImpl up_fixed{};
  up_fixed.b.width = 1920;
  up_fixed.b.height = 1080;
  up_fixed.scale = disp.scale;
  up_fixed.wide = disp.wide;
  up_fixed.hostW = 3432;
  up_fixed.hostH = 1440;

  nvn::ReplayCtx cx{};
  cx.rt[0] = &disp;
  nvn::TextureImpl* samp_fixed[1] = {&up_fixed};
  const float box[4] = {0.0f, 0.0f, 1920.0f, 1080.0f};
  nvn::WideOnDraw(cx, samp_fixed, 1, nullptr, 0, box, false);
  CHECK(cx.wideMode == 0);

  // Without wide copied onto U.out (wide == 1.0f), WideOnDraw falls back to narrow (mode == 1),
  // squashing the 3432x1440 upscaled image horizontally into the 16:9 center.
  nvn::TextureImpl up_buggy = up_fixed;
  up_buggy.wide = 1.0f;
  nvn::TextureImpl* samp_buggy[1] = {&up_buggy};
  cx.wideMode = 0;
  nvn::WideOnDraw(cx, samp_buggy, 1, nullptr, 0, box, false);
  CHECK(cx.wideMode == 1);

  int sx = 0, sw = 0;
  CHECK(nvn::WidePresentCrop(&disp, &sx, &sw));
  CHECK(sx > 0 && sw == 2560);

  nvn::TextureImpl t1080{};
  t1080.b.width = 1080;
  t1080.b.height = 720;
  CHECK(nvn::UpscaleSubstitute(cx, &t1080) == nullptr);

  nvnHostSetUltrawide(orig_uw);
}

void test_fsr3_linux_backend() {
  // 1. Load() succeeds and is idempotent.
  CHECK(snap_upscale::fsr::Load());
  CHECK(snap_upscale::fsr::Load());

  // 2. Init() rejects null Vulkan handles cleanly; Evaluate() without Init() fails cleanly.
  snap_upscale::DeviceCtx null_ctx{};
  CHECK(!snap_upscale::fsr::Init(null_ctx));

  snap_upscale::Eval empty_eval{};
  CHECK(!snap_upscale::fsr::Evaluate(empty_eval));

  // 3. Release() is safe even when uninitialized.
  snap_upscale::fsr::Release();

  // 4. ffxIsWave64 and ffxGetPermutationBlobByIndex cover all 88 FSR 3.1.4 permutations strictly.
  bool wave64 = true;
  CHECK(ffxIsWave64(FFX_EFFECT_FSR3UPSCALER, 0u, wave64) == FFX_OK);
  CHECK(!wave64);

  constexpr uint32_t kPermUseLanczosLut = (1u << 0);
  constexpr uint32_t kPermHdrInput = (1u << 1);
  constexpr uint32_t kPermLowResMv = (1u << 2);
  constexpr uint32_t kPermJitterMv = (1u << 3);
  constexpr uint32_t kPermDepthInverted = (1u << 4);
  constexpr uint32_t kPermAllowFp16 = (1u << 7);
  constexpr uint32_t kPermForceWave64 = (1u << 6);

  for (int hdr = 0; hdr <= 1; ++hdr) {
    for (int inv = 0; inv <= 1; ++inv) {
      for (int fp16 = 0; fp16 <= 1; ++fp16) {
        const uint32_t perm = kPermLowResMv |
                              (hdr ? kPermHdrInput : 0u) |
                              (inv ? kPermDepthInverted : 0u) |
                              (fp16 ? kPermAllowFp16 : 0u);
        for (uint32_t pass = 0; pass <= 10; ++pass) {
          FfxShaderBlob blob{};
          CHECK(ffxGetPermutationBlobByIndex(
                    FFX_EFFECT_FSR3UPSCALER, static_cast<FfxPass>(pass),
                    FFX_BIND_COMPUTE_SHADER_STAGE, perm, &blob) == FFX_OK);
          CHECK(blob.data != nullptr && blob.size > 0);

          // Lanczos LUT permutation (AMD RDNA waveLaneCountMin=32, waveLaneCountMax=64):
          // passes 6 (ACCUMULATE) and 7 (ACCUMULATE_SHARPEN) must serve distinct LANCZOS_TYPE=1 blobs
          // binding r_lanczos_lut (8 SRVs instead of 7).
          FfxShaderBlob lut_blob{};
          CHECK(ffxGetPermutationBlobByIndex(
                    FFX_EFFECT_FSR3UPSCALER, static_cast<FfxPass>(pass),
                    FFX_BIND_COMPUTE_SHADER_STAGE, perm | kPermUseLanczosLut, &lut_blob) == FFX_OK);
          CHECK(lut_blob.data != nullptr && lut_blob.size > 0);
          if (pass == FFX_FSR3UPSCALER_PASS_ACCUMULATE || pass == FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN) {
            CHECK(lut_blob.data != blob.data);
            CHECK(blob.srvTextureCount == 7 && lut_blob.srvTextureCount == 8);
            bool has_lut_binding = false;
            for (uint32_t s = 0; s < lut_blob.srvTextureCount; ++s) {
              if (lut_blob.boundSRVTextures[s] == 4u) has_lut_binding = true;
            }
            CHECK(has_lut_binding);
          } else {
            CHECK(lut_blob.data == blob.data);
          }
        }
      }
    }
  }

  // Unsupported/unmatched permutations must be rejected explicitly (never silently substituted).
  FfxShaderBlob bad_blob{};
  CHECK(ffxGetPermutationBlobByIndex(FFX_EFFECT_FSR3UPSCALER, 0, FFX_BIND_COMPUTE_SHADER_STAGE,
                                     0u /* missing LowResMv */, &bad_blob) == FFX_ERROR_INVALID_ARGUMENT);
  CHECK(ffxGetPermutationBlobByIndex(FFX_EFFECT_FSR3UPSCALER, 0, FFX_BIND_COMPUTE_SHADER_STAGE,
                                     kPermLowResMv | kPermJitterMv, &bad_blob) == FFX_ERROR_INVALID_ARGUMENT);
  CHECK(ffxGetPermutationBlobByIndex(FFX_EFFECT_FSR3UPSCALER, 0, FFX_BIND_COMPUTE_SHADER_STAGE,
                                     kPermLowResMv | kPermForceWave64, &bad_blob) == FFX_ERROR_INVALID_ARGUMENT);
  CHECK(ffxGetPermutationBlobByIndex(FFX_EFFECT_FSR3UPSCALER, 12, FFX_BIND_COMPUTE_SHADER_STAGE,
                                     kPermLowResMv, &bad_blob) == FFX_ERROR_INVALID_ARGUMENT);
}

void test_motionaa_shutdown_and_foliage_stable() {
  // 1. MotionAaShutdown is safe when uninitialized, with null device, and when called repeatedly (no double-free).
  nvn::MotionAaShutdown(nullptr);
  nvn::Host dummy_host{};
  dummy_host.device = VK_NULL_HANDLE;
  nvn::MotionAaShutdown(&dummy_host);
  nvn::MotionAaShutdown(&dummy_host);

  // 2. FoliageStableEnabled() defaults OFF and toggles live via SNAP_FOLIAGE_STABLE without affecting SpecularAA/HD Effects.
  unsetenv("SNAP_FOLIAGE_STABLE");
  unsetenv("SNAP_FOLIAGE_AA");
  unsetenv("SNAP_LEAF_TRANSLUCENCY");
  unsetenv("SNAP_SPECULAR_AA");
  unsetenv("SNAP_HD_EFFECTS");
  hle::live::bump();
  CHECK(!nvn::FoliageStableEnabled());
  CHECK(!nvn::SpecularAaEnabled());
  CHECK(nvn::HdfxFactorNow() == 1.0f);
  CHECK(nvn::FoliagePipeBits(5, 0.5f, 97 /*R16G16B16A16_SFLOAT*/, 1001 /*DEPTH32F*/, true, 1, false, 7, true) == 0u);

  setenv("SNAP_FOLIAGE_STABLE", "1", 1);
  hle::live::bump();
  CHECK(nvn::FoliageStableEnabled());
  const uint32_t color_bits = nvn::FoliagePipeBits(5, 0.5f, 97, 1001, true, 1, false, 7, true);
  CHECK((color_bits & 4u) != 0u);
  const uint32_t prepass_bits = nvn::FoliagePipeBits(5, 0.5f, 0, 1001, true, 0, false, 7, true);
  CHECK((prepass_bits & 4u) != 0u);

  // Compatible with SpecularAA and HD Effects both ON.
  setenv("SNAP_SPECULAR_AA", "1", 1);
  setenv("SNAP_HD_EFFECTS", "1", 1);
  hle::live::bump();
  CHECK(nvn::FoliageStableEnabled());
  CHECK(nvn::SpecularAaEnabled());
  CHECK(nvn::HdfxFactorNow() == 2.0f);

  // 3. PatchCaptureAlphaLod + PatchAlphaHash SPIR-V patching on a minimal valid fragment shader.
  // Minimal SPIR-V 1.0 fragment module with OpImageSampleImplicitLod(vec2) -> vec4, OpCompositeExtract alpha -> OpStore outCol.
  auto op = [](uint16_t opcode, std::initializer_list<uint32_t> ops) {
    std::vector<uint32_t> v;
    v.push_back((static_cast<uint32_t>(ops.size() + 1) << 16) | opcode);
    v.insert(v.end(), ops.begin(), ops.end());
    return v;
  };
  std::vector<uint32_t> spv = {0x07230203u, 0x00010000u, 0u, 22u, 0u};
  auto emit = [&](std::initializer_list<uint32_t> words) { spv.insert(spv.end(), words.begin(), words.end()); };
  auto emit_op = [&](uint16_t opcode, std::initializer_list<uint32_t> ops) {
    auto v = op(opcode, ops);
    spv.insert(spv.end(), v.begin(), v.end());
  };
  emit_op(17, {1});                 // OpCapability Shader
  emit_op(14, {0, 1});              // OpMemoryModel Logical GLSL450
  emit_op(15, {4, 1, 0x6e69616du, 18}); // OpEntryPoint Fragment %1 "main" %18
  emit_op(16, {1, 7});              // OpExecutionMode %1 OriginUpperLeft
  emit_op(71, {18, 30, 0});         // OpDecorate %18 Location 0
  emit_op(20, {2});                 // %2 = OpTypeVoid
  emit_op(33, {3, 2});              // %3 = OpTypeFunction %2
  emit_op(22, {4, 32});             // %4 = OpTypeFloat 32
  emit_op(23, {5, 4, 2});           // %5 = OpTypeVector %4 2
  emit_op(23, {6, 4, 4});           // %6 = OpTypeVector %4 4
  emit_op(25, {7, 4, 1, 0, 0, 0, 1, 0}); // %7 = OpTypeImage %4 2D 0 0 0 1 Unknown
  emit_op(27, {8, 7});              // %8 = OpTypeSampledImage %7
  emit_op(32, {9, 0, 8});           // %9 = OpTypePointer UniformConstant %8
  emit_op(32, {10, 3, 6});          // %10 = OpTypePointer Output %6
  emit_op(32, {11, 6, 5});          // %11 = OpTypePointer Private %5
  emit_op(59, {9, 12, 0});          // %12 = OpVariable %9 UniformConstant
  emit_op(59, {10, 18, 3});         // %18 = OpVariable %10 Output
  emit_op(59, {11, 13, 1});         // %13 = OpVariable %11 Input (used as vec2 coord source)
  emit_op(54, {2, 1, 0, 3});        // %1 = OpFunction %2 None %3
  emit_op(248, {14});               // %14 = OpLabel
  emit_op(61, {8, 15, 12});         // %15 = OpLoad %8 %12
  emit_op(61, {5, 16, 13});         // %16 = OpLoad %5 %13
  emit_op(87, {6, 17, 15, 16});     // %17 = OpImageSampleImplicitLod %6 %15 %16
  emit_op(81, {4, 19, 17, 3});      // %19 = OpCompositeExtract %4 %17 3
  emit_op(62, {18, 17});            // OpStore %18 %17
  emit_op(253, {});                 // OpReturn
  emit_op(56, {});                  // OpFunctionEnd
  (void)emit;

  std::vector<uint32_t> spv_lod, spv_patched;
  uint32_t lod_var = 0;
  CHECK(nvn::PatchCaptureAlphaLod(spv, spv_lod, &lod_var));
  CHECK(lod_var != 0);
  CHECK(spv_lod.size() > spv.size());
  // Verify OpCapability ImageQuery (50) and OpImageQueryLod (105) were emitted.
  bool has_img_query_cap = false, has_query_lod = false;
  for (size_t i = 5; i < spv_lod.size();) {
    const uint32_t wc = spv_lod[i] >> 16, opcode = spv_lod[i] & 0xFFFFu;
    if (opcode == 17 && wc >= 2 && spv_lod[i + 1] == 50) has_img_query_cap = true;
    if (opcode == 105) has_query_lod = true;
    i += wc ? wc : 1;
  }
  CHECK(has_img_query_cap && has_query_lod);

  CHECK(nvn::PatchAlphaHash(spv_lod, 5 /*GREATER*/, false, false, 0, spv_patched, lod_var));
  CHECK(spv_patched.size() > spv_lod.size());

  // Restore clean default env state.
  unsetenv("SNAP_FOLIAGE_STABLE");
  unsetenv("SNAP_SPECULAR_AA");
  unsetenv("SNAP_HD_EFFECTS");
  hle::live::bump();
}

}  // namespace

int main() {
  test_nvn_bootstrap_table();
  test_vulkan_device_and_sdl_surface();
  test_optional_stub_functions();
  test_live_graphics_and_ultrawide();
  test_upscale_and_hdr_defaults();
  test_ultrawide_cutscene_upscale_aspect();
  test_fsr3_linux_backend();
  test_motionaa_shutdown_and_foliage_stable();
  std::printf("nvn_renderer_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
