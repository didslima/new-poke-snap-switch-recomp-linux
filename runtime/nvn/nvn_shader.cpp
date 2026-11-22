// SPDX-License-Identifier: MIT
// NVN program objects: GLSLC program binaries -> SPIR-V -> VkShaderModule + descriptor layouts.
//
// Translation path (docs/nvn-vulkan-design.md, "Shader pipeline"): the game's shaders are Maxwell SASS inside the
// GLSLC binary (the NVN driver never sees GLSL at run time). The SASS is translated by the Ryujinx shader
// translator (MIT), driven by scripts/shader_harness (dotnet) in --nvn mode, and the result is cached on disk keyed
// by SHA-256 of the program's data section. Nothing of the game is stored in the repository: the cache lives under
// the layer's cache directory (NVN_CACHE_DIR, default <exe dir>\nvn_cache).
//
// Control/data section facts used here come from analysis/shaders/nushdb_layout.md (derived from the game's own
// files): control+0x6F8 program size, +0x700 constant-data offset, +0x704 data size, +0x708 header size (0x30);
// the data section starts with 0x12345678 and, for graphics stages, the Maxwell SPH at +0x30. Compute programs
// have neither.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "nvn_aa.h"
#include "nvn_foliage.h"
#include "nvn_hostbuf.h"
#include "nvn_internal.h"
#include "nvn_os_host.h"
#include "nvn_pipeline.h"
#include "nvn_pom.h"
#include "nvn_rt.h"   // [rt]
#include "os/perf.h"   // SNAP_PERF instrumentation

namespace nvn {
namespace {

// SNAP_XLATE_REV=<n> switches to translator revision n (patched Ryujinx translator, scripts/xlate_patch.sh): the revision joins the cache salt, so
// old and new outputs never share a key, and the nvn_cache and shadercache folders get a _r<n> suffix (nvn_vk.cpp, nvn_pipeline.cpp).
// Unset or 0 = the original translation, unchanged.
const std::string& TranslatorSaltStr() {
  static const std::string s = [] {
    std::string b = "nvn-xlate-v2|binding-model-1|interlock-off|clip7";
    const char* r = getenv("SNAP_XLATE_REV");
    if (r && *r && strcmp(r, "0")) b += std::string("|xlate-r") + r;
    return b;
  }();
  return s;
}

// Shader modules whose SPIR-V uses subgroup operations (GroupNonUniform*, SubgroupLocalInvocationId and the lane masks): their lane math
// assumes 32 lanes, so the pipeline builders pin them to a required subgroup size of 32 (nvn_draw.cpp, g_sgPinStages).
std::mutex g_sgMu;
std::unordered_set<uint64_t> g_sgMods;
bool SpvUsesSubgroup(const std::vector<uint32_t>& w) {
  for (size_t i = 5; i < w.size();) {
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    if (!wc || i + wc > w.size()) return false;
    if (op >= 333 && op <= 366) return true;   // OpGroupNonUniform*
    if (op == 71 && wc >= 4 && w[i + 2] == 11 && (w[i + 3] == 41 || (w[i + 3] >= 4416 && w[i + 3] <= 4420))) return true;   // BuiltIn SubgroupLocalInvocationId, SubgroupEq/Ge/Gt/Le/LtMask
    i += wc;
  }
  return false;
}
void NoteSubgroup(VkShaderModule m, const std::vector<uint32_t>& w) {
  if (!SpvUsesSubgroup(w)) return;
  std::lock_guard<std::mutex> l(g_sgMu);
  g_sgMods.insert((uint64_t)(uintptr_t)m);
}

// ---- SHA-256 (BCrypt) ---------------------------------------------------------------------------------------------
// Load pass (2026-11-13): the provider is opened once (BCrypt algorithm handles are shareable between threads); opening it per
// shader cost about as much as the hash. SNAP_SHADER_FASTIO=0 restores one open per call.
bool ShaderFastIo() {
  static const bool on = [] { const char* e = getenv("SNAP_SHADER_FASTIO"); return !(e && !strcmp(e, "0")); }();
  return on;
}
std::string Sha256Hex(const uint8_t* d, size_t n, const std::string& extra) {
  uint8_t dig[32] = {};
  nvn::os_host::sha256_2(d, n, extra.data(), extra.size(), dig);
  char hx[65];
  for (int i = 0; i < 32; i++) snprintf(hx + 2 * i, 3, "%02x", dig[i]);
  return std::string(hx, 64);
}

// ---- tiny JSON reader (the manifest is machine-written, numbers only) ---------------------------------------------
struct JKV;
struct J {
  char type = 0;  // 'n' number, 'a' array, 'o' object
  double num = 0;
  std::vector<J> arr;
  std::vector<JKV> obj;
  J();
  ~J();
  J(const J&);
  J(J&&) noexcept;
  J& operator=(const J&);
  J& operator=(J&&) noexcept;
  const J* get(const char* k) const;
  int i(const char* k, int def = 0) const { auto* v = get(k); return v ? (int)v->num : def; }
};
struct JKV {
  std::string first;
  J second;
};
inline J::J() = default;
inline J::~J() = default;
inline J::J(const J&) = default;
inline J::J(J&&) noexcept = default;
inline J& J::operator=(const J&) = default;
inline J& J::operator=(J&&) noexcept = default;
inline const J* J::get(const char* k) const {
  for (auto& kv : obj) if (kv.first == k) return &kv.second;
  return nullptr;
}
struct JP {
  const char* p; const char* e;
  void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
  bool parse(J& out) {
    ws();
    if (p >= e) return false;
    if (*p == '{') {
      p++; out.type = 'o';
      for (;;) {
        ws();
        if (p < e && *p == '}') { p++; return true; }
        if (p < e && *p == ',') { p++; continue; }
        if (p >= e || *p != '"') return false;
        const char* s = ++p;
        while (p < e && *p != '"') p++;
        std::string k(s, p); p++;
        ws();
        if (p >= e || *p != ':') return false;
        p++;
        J v;
        if (!parse(v)) return false;
        out.obj.push_back({std::move(k), std::move(v)});
      }
    }
    if (*p == '[') {
      p++; out.type = 'a';
      for (;;) {
        ws();
        if (p < e && *p == ']') { p++; return true; }
        if (p < e && *p == ',') { p++; continue; }
        J v;
        if (!parse(v)) return false;
        out.arr.push_back(std::move(v));
      }
    }
    char* end = nullptr;
    out.type = 'n';
    out.num = strtod(p, &end);
    if (end == p) return false;
    p = end;
    return true;
  }
};

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  return nvn::os_host::read_file_bytes(path, out);
}
bool WriteFile(const std::string& path, const void* d, size_t n) {
  return nvn::os_host::write_file_bytes(path, d, n);
}

// Run the dotnet shader harness (BelowNormal priority, 10 minute cap). Returns true when the manifest exists.
bool RunHarness(const std::string& bin, const std::string& spv, const std::string& json, int stage, uint32_t constOff,
                uint32_t codeOff) {
  std::string dll;
#ifdef NVN_HARNESS_DLL
  dll = NVN_HARNESS_DLL;
#endif
  if (const char* e = getenv("SNAP_XLATE_REV"); e && *e && strcmp(e, "0")) {   // revision n: build\xlate\r<n>\h (scripts/xlate_build.ps1), under the same project root as the default harness
    size_t k = dll.find("tools\\shader_harness");
    if (k == std::string::npos) k = dll.find("tools/shader_harness");
    if (k != std::string::npos) {
      const char sep = nvn::os_host::path_sep();
      dll = dll.substr(0, k) + "build" + sep + "xlate" + sep + "r" + e + sep + "h" + sep + "bin" + sep + "Release" + sep + "net10.0" + sep + "ShaderHarness.dll";
    }
  }
  if (dll.empty() || !nvn::os_host::file_exists(dll)) {   // release build or fallback: harness folder next to exe or repo tools/
    const char sep = nvn::os_host::path_sep();
    const std::string exe_base = nvn::os_host::exe_dir() + sep + "shader_harness" + sep;
    const std::string repo_rel = std::string("tools") + sep + "shader_harness" + sep + "bin" + sep + "Release" + sep + "net10.0" + sep + "ShaderHarness.dll";
    if (nvn::os_host::file_exists(exe_base + "ShaderHarness.exe")) dll = exe_base + "ShaderHarness.exe";
    else if (nvn::os_host::file_exists(exe_base + "ShaderHarness")) dll = exe_base + "ShaderHarness";
    else if (nvn::os_host::file_exists(exe_base + "ShaderHarness.dll")) dll = exe_base + "ShaderHarness.dll";
    else if (nvn::os_host::file_exists(repo_rel)) dll = repo_rel;
    else if (dll.empty()) dll = exe_base + "ShaderHarness.exe";
  }
  if (const char* e = getenv("NVN_SHADER_HARNESS"); e && *e) dll = e;
  if (dll.empty()) { LogError("shader translation needs the harness: set NVN_SHADER_HARNESS=<ShaderHarness.dll>"); return false; }
  const bool is_dll = dll.size() > 4 && nvn::os_host::ascii_ieq(dll.c_str() + dll.size() - 4, ".dll");   // .dll runs via dotnet; native apphosts (.exe or Linux binary) run directly
  const std::string launch = std::string(is_dll ? "dotnet " : "") + "\"" + dll + "\"";
  bool srv_ok = false;
  if (nvn::os_host::run_shader_harness_server(launch, bin, spv, json, stage, constOff, codeOff, &srv_ok)) {
    return srv_ok;
  }
  std::ostringstream cl;
  cl << launch << " --nvn \"" << bin << "\" \"" << spv << "\" \"" << json << "\" " << stage << " " << constOff << " " << codeOff;
  if (!nvn::os_host::run_shader_harness(cl.str(), json)) {
    LogError("shader harness failed or is unavailable");
    return false;
  }
  return true;
}

VkShaderStageFlagBits StageBit(int nvnStage) {
  static const VkShaderStageFlagBits t[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                             VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                             VK_SHADER_STAGE_COMPUTE_BIT};
  return t[nvnStage];
}

uint32_t Rd32(const void* p, size_t o) { uint32_t v; memcpy(&v, static_cast<const uint8_t*>(p) + o, 4); return v; }

void FreeStage(Host* h, StageProg& s) {
  if (s.mod) vkDestroyShaderModule(h->device, s.mod, nullptr);
  // fragMods variants are owned by the host (Host::variantMods): composite programs hold copies of this map
  if (s.constBuf) vkDestroyBuffer(h->device, s.constBuf, nullptr);
  if (s.constMem) vkFreeMemory(h->device, s.constMem, nullptr);
  s = StageProg{};
}

// In-memory memoization + background prefetch of translation-cache files (ported from upstream 911c61e).
// SNAP_XCACHE_MEMO_MB=<n> caps the copy (default 256; 0 = read every file from disk); SNAP_XCACHE_PREFETCH=0 disables prefetch.
struct XMemoEntry { std::vector<uint8_t> spv, js; };
static std::mutex& g_xmMu = *new std::mutex;
static auto& g_xm = *new std::unordered_map<std::string, std::shared_ptr<const XMemoEntry>>;
static uint64_t g_xmBytes = 0;

static uint64_t XMemoCapBytes() {
  static const uint64_t cap = [] {
    const char* e = std::getenv("SNAP_XCACHE_MEMO_MB");
    const long long mb = (e && *e) ? std::atoll(e) : 256;
    return mb > 0 ? static_cast<uint64_t>(mb) << 20 : 0ull;
  }();
  return cap;
}

static std::shared_ptr<const XMemoEntry> XMemoFind(const std::string& k) {
  if (!XMemoCapBytes()) return nullptr;
  std::lock_guard<std::mutex> l(g_xmMu);
  auto it = g_xm.find(k);
  return it == g_xm.end() ? nullptr : it->second;
}

static void XMemoPut(const std::string& k, const std::vector<uint8_t>& spvIn, const std::vector<uint8_t>& jsIn) {
  const uint64_t cap = XMemoCapBytes();
  if (!cap) return;
  std::lock_guard<std::mutex> l(g_xmMu);
  if (g_xm.count(k) || g_xmBytes + spvIn.size() + jsIn.size() > cap) return;
  auto e = std::make_shared<XMemoEntry>();
  e->spv = spvIn;
  e->js = jsIn;
  g_xmBytes += spvIn.size() + jsIn.size();
  g_xm.emplace(k, std::move(e));
}

static void XMemoPrefetch(const std::string& cacheDir) {
  static std::once_flag g_xmPrefetchOnce;
  std::call_once(g_xmPrefetchOnce, [&]() {
    const char* pf = std::getenv("SNAP_XCACHE_PREFETCH");
    if (!XMemoCapBytes() || (pf && *pf == '0') || cacheDir.empty()) return;
    std::thread([cacheDir] {
      std::error_code ec;
      std::vector<std::string> keys;
      for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        const std::string fn = entry.path().filename().string();
        if (fn.size() > 5 && fn.find('.') == fn.size() - 5 && fn.compare(fn.size() - 5, 5, ".json") == 0) {
          keys.push_back(fn.substr(0, fn.size() - 5));
        }
      }
      for (const std::string& k : keys) {
        if (XMemoFind(k)) continue;
        {
          std::lock_guard<std::mutex> l(g_xmMu);
          if (g_xmBytes >= XMemoCapBytes()) break;
        }
        const std::string b = cacheDir + nvn::os_host::path_sep() + k;
        std::vector<uint8_t> spvBuf, jsBuf;
        if (!ReadFile(b + ".spv", &spvBuf) || !ReadFile(b + ".json", &jsBuf)) continue;
        if (spvBuf.size() < 20 || (spvBuf.size() & 3) != 0) continue;
        JP jp{reinterpret_cast<const char*>(jsBuf.data()), reinterpret_cast<const char*>(jsBuf.data()) + jsBuf.size()};
        J tmp;
        if (jp.parse(tmp)) XMemoPut(k, spvBuf, jsBuf);
      }
    }).detach();
  });
}

// SNAP_XLATE_AB test aid (see XlateAbVariant in nvn_internal.h): loads the same fragment program translated by the other revision
// (SNAP_XLATE_AB_REV, cache dir SNAP_XLATE_AB_DIR) as StageProg::modAlt/spvAlt, with the same post-processing as the normal module.
void LoadAbAlt(Host* h, const uint8_t* data, uint32_t dataSize, int stage, uint32_t codeOff, uint32_t constOff, bool eft, const std::string& baseKey, StageProg* sp) {
  const char* dir = getenv("SNAP_XLATE_AB_DIR");
  if (!dir || !*dir) return;
  const char* r = getenv("SNAP_XLATE_AB_REV");
  std::string salt = "nvn-xlate-v2|binding-model-1|interlock-off|clip7";
  if (r && *r && strcmp(r, "0")) salt += std::string("|xlate-r") + r;
  struct ShaCtx { const uint8_t* data; uint32_t dataSize; };
  ShaCtx shaCtx{data, dataSize};
  const std::string key = nvn::os_host::resolve_shader_cache_key(
      dir, [](void* ctx, const std::string& extra) { auto* c = static_cast<ShaCtx*>(ctx); return Sha256Hex(c->data, c->dataSize, extra); },
      &shaCtx, salt, stage, codeOff, constOff);   // same identity rule as BuildStage
  std::vector<uint8_t> spv;
  if (key.empty() || !ReadFile(std::string(dir) + nvn::os_host::path_sep() + key + ".spv", &spv)) { Log("xlate A/B: no alternate translation for %s", baseKey.c_str()); return; }
  std::vector<uint32_t> words(spv.size() / 4);
  memcpy(words.data(), spv.data(), words.size() * 4);
  if (eft) SpvAddEarlyFragmentTests(words);
  SpvAddStorageImageCaps(words);
  AaPatchFragment(words, baseKey.c_str());
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4; ci.pCode = words.data();
  if (vkCreateShaderModule(h->device, &ci, nullptr, &sp->modAlt) != VK_SUCCESS) { sp->modAlt = VK_NULL_HANDLE; return; }
  NoteSubgroup(sp->modAlt, words);
  sp->spvAlt = std::move(words);
}

bool BuildStage(Host* h, const ShaderData& sd, StageProg* out) {
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!h->ResolveAddress(sd.data, &pool, &off) || !pool->storage) {
    LogError("ProgramSetShaders: shader address 0x%llx is not in a pool", (unsigned long long)sd.data);
    return false;
  }
  if (!sd.control) { LogError("ProgramSetShaders: null control section"); return false; }
  const uint8_t* data = static_cast<const uint8_t*>(pool->storage) + off;
  const uint32_t progSize = Rd32(sd.control, 0x6F8), constOff = Rd32(sd.control, 0x700), dataSize = Rd32(sd.control, 0x704),
                 hdrSize = Rd32(sd.control, 0x708);
  if (dataSize == 0 || dataSize > (16u << 20) || constOff > dataSize || progSize > dataSize) {
    LogError("ProgramSetShaders: bad control section (size 0x%x const 0x%x)", dataSize, constOff);
    return false;
  }
  bool compute = Rd32(data, 0) != 0x12345678u;
  int stage = 5;
  uint32_t codeOff = 0;
  if (!compute) {
    codeOff = hdrSize;
    const uint32_t w0 = Rd32(data, codeOff);
    switch ((w0 >> 10) & 15) {   // SPH stage field [analysis/shaders/nushdb_layout.md]
      case 1: stage = 0; break;
      case 2: stage = 3; break;
      case 3: stage = 4; break;
      case 4: stage = 2; break;
      case 5: stage = 1; break;
      default: LogError("ProgramSetShaders: unknown SPH stage %u", (w0 >> 10) & 15); return false;
    }
  }
  struct ShaCtx { const uint8_t* data; uint32_t dataSize; };
  ShaCtx shaCtx{data, dataSize};
  const std::string key = nvn::os_host::resolve_shader_cache_key(
      h->cacheDir, [](void* ctx, const std::string& extra) { auto* c = static_cast<ShaCtx*>(ctx); return Sha256Hex(c->data, c->dataSize, extra); },
      &shaCtx, TranslatorSaltStr(), stage, codeOff, constOff);
  if (key.empty()) return false;
  const std::string base = h->cacheDir + nvn::os_host::path_sep() + key;
  const std::string spvPath = base + ".spv", jsonPath = base + ".json";
  std::vector<uint8_t> spv, js;
  J m;
  struct CacheCtx {
    Host* h;
    const uint8_t* data;
    uint32_t dataSize;
    int stage;
    uint32_t constOff;
    uint32_t codeOff;
    const std::string& key;
    const std::string& base;
    const std::string& spvPath;
    const std::string& jsonPath;
    std::vector<uint8_t>& spv;
    std::vector<uint8_t>& js;
    J& m;
  } cacheCtx{h, data, dataSize, stage, constOff, codeOff, key, base, spvPath, jsonPath, spv, js, m};
  XMemoPrefetch(h->cacheDir);
  const bool loaded = nvn::os_host::ensure_shader_cache_entry(
      h->cacheDir, key, constOff,
      [](void* p) {
        auto* c = static_cast<CacheCtx*>(p);
        if (auto e = XMemoFind(c->key)) {
          c->spv = e->spv;
          c->js = e->js;
          JP jp{reinterpret_cast<const char*>(c->js.data()), reinterpret_cast<const char*>(c->js.data()) + c->js.size()};
          c->m = J();
          if (jp.parse(c->m)) return true;
        }
        if (!ReadFile(c->spvPath, &c->spv) || !ReadFile(c->jsonPath, &c->js)) return false;
        if (c->spv.size() < 20 || (c->spv.size() & 3) != 0) {
          LogError("bad shader cache entry %s (spv %zu bytes); retranslating", c->base.c_str(), c->spv.size());
          return false;
        }
        JP jp{reinterpret_cast<const char*>(c->js.data()), reinterpret_cast<const char*>(c->js.data()) + c->js.size()};
        c->m = J();
        if (jp.parse(c->m)) {
          XMemoPut(c->key, c->spv, c->js);
          return true;
        }
        LogError("bad shader manifest %s; retranslating", c->jsonPath.c_str());
        return false;
      },
      [](void* p, const std::string& binPath, const std::string& spvOut, const std::string& jsonOut) {
        auto* c = static_cast<CacheCtx*>(p);
        WriteFile(binPath, c->data, c->dataSize);
        Log("translating shader %s (stage %d)", c->key.c_str(), c->stage);
        const uint64_t xt0 = snap_perf::now_ns();
        const bool ok = RunHarness(binPath, spvOut, jsonOut, c->stage, c->constOff, c->codeOff);
        snap_perf::event("xlate", (double)(snap_perf::now_ns() - xt0) / 1e6, c->key.c_str());
        if (ok) {
          static std::atomic<int> translated{0};
          int n = ++translated;
          if (n == 1 || n % 100 == 0)
            fprintf(stderr, "[nvn] first-run shader translation: %d shaders translated so far (cache: %s)\n", n, c->h->cacheDir.c_str());
        }
        return ok;
      },
      &cacheCtx);
  if (!loaded) { LogError("shader cache entry %s unusable after retranslation", base.c_str()); return false; }
  StageProg sp;
  sp.valid = true; sp.stage = stage; snprintf(sp.key, sizeof sp.key, "%.12s", key.c_str()); sp.fragOutputMap = m.i("fragmentOutputMap");
  snprintf(sp.fkey, sizeof sp.fkey, "%s", key.c_str());
  PipelineRegisterStageKey(sp.key, sp.fkey, stage == 1 && (Rd32(sd.control, 0x718) & 1));
  if (auto* a = m.get("cbuffers")) for (auto& c : a->arr) sp.cb.push_back({c.i("slot"), c.i("set"), c.i("binding")});
  if (auto* a = m.get("sbuffers"))
    for (auto& c : a->arr) sp.sb.push_back({c.i("slot"), c.i("set"), c.i("binding"), c.i("sbCbSlot"), c.i("sbCbOffset"), c.i("write")});
  auto readTex = [](const J* a, std::vector<MTex>& v) {
    if (!a) return;
    for (auto& c : a->arr)
      v.push_back({c.i("set"), c.i("binding"), c.i("handleIndex"), c.i("cbufSlot", -1), c.i("type"), std::max(c.i("array", 1), 1), c.i("separate")});
  };
  readTex(m.get("textures"), sp.tex);
  readTex(m.get("images"), sp.img);
  std::vector<uint32_t> words(spv.size() / 4);
  memcpy(words.data(), spv.data(), words.size() * 4);
  if (stage == 5) {   // workgroup size: u32 x, y, z at control+0x748 (checked on the three resource.drp compute programs: 1x1x1, 16x16x1, 16x16x1)
    const uint32_t lx = Rd32(sd.control, 0x748), ly = Rd32(sd.control, 0x74C), lz = Rd32(sd.control, 0x750);
    if (lx && ly && lz && lx <= 1024 && ly <= 1024 && lz <= 64 && lx * ly * lz <= 1024) { SpvSetLocalSize(words, lx, ly, lz); Log("compute shader %s: local size %ux%ux%u", key.c_str(), lx, ly, lz); }
    else LogError("compute shader %s: implausible local size %u %u %u in the control section", key.c_str(), lx, ly, lz);
  }
  if (stage == 1 && (Rd32(sd.control, 0x718) & 1)) {   // layout(early_fragment_tests): see SpvAddEarlyFragmentTests
    if (SpvAddEarlyFragmentTests(words)) Log("fragment shader %s: early fragment tests", key.c_str());
    std::vector<uint8_t> mark;
    if (!ReadFile(base + ".efz", &mark)) WriteFile(base + ".efz", "1", 1);   // the cache key hashes only the data section, so the pipeline service needs this marker
  }
  SpvAddStorageImageCaps(words);   // the translator omits StorageImageRead/WriteWithoutFormat for storage images of unknown format
  if (stage == 1) GtaoPatchFragment(words, key.c_str());   // SNAP_SSAO_GTAO (nvn_gtao.cpp)
  if (stage == 1) AaPatchFragment(words, key.c_str());   // SNAP_AA=off/smaa: FXAA tonemap shaders lose their edge test (nvn_aa.cpp)
  if (stage == 0) snap_rt::PatchVertexXfb(words, key.c_str());   // [rt] SNAP_RT=1: gl_Position transform feedback decorations (no-op otherwise)
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4; ci.pCode = words.data();
  if (vkCreateShaderModule(h->device, &ci, nullptr, &sp.mod) != VK_SUCCESS) { LogError("vkCreateShaderModule failed for %s", key.c_str()); return false; }
  NoteSubgroup(sp.mod, words);
  if (stage == 0) sp.inputMask = SpvVertexInputMask(words);
  if (stage == 1) { sp.spv = words; sp.outLocs = SpvFragmentOutputs(words); }
  if (stage == 0 && FoliageSwayKeepSpv()) sp.spv = words;   // vertex program kept for the wind sway wrapper (nvn_foliage.cpp)
  if (stage == 1 && getenv("SNAP_XLATE_AB")) LoadAbAlt(h, data, dataSize, stage, codeOff, constOff, (Rd32(sd.control, 0x718) & 1) != 0, key, &sp);
  // c[1]: the constant-data blob that follows the code (data section tail)
  sp.constSize = dataSize - constOff;
  bool needC1 = false;
  for (auto& c : sp.cb) if (c.slot == 1) needC1 = true;
  if (needC1 && sp.constSize) {
    void* mp = nullptr;
    if (CreateHostBuffer(h, (sp.constSize + 15) & ~15ull, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &sp.constBuf, &sp.constMem, &mp))
      memcpy(mp, data + constOff, sp.constSize);
    else LogError("shader %s: its c[1] constant buffer could not be created; the stage has no c[1] data", key.c_str());
  }
  *out = std::move(sp);
  return true;
}

// Rebuilds a stage object (module, reflection, no constant buffer) from the translation cache by its full key. Used by the pipeline
// service to compile pipelines without the game having created the program. Compute stages are not supported.
bool LoadStageFromCacheImpl(Host* h, const std::string& key, StageProg* out, bool eft, bool* eftApplied) {
  std::vector<uint8_t> spv, js;
  const std::string base = h->cacheDir + nvn::os_host::path_sep() + key;
  if (!ReadFile(base + ".spv", &spv) || !ReadFile(base + ".json", &js)) return false;
  J m;
  JP jp{reinterpret_cast<const char*>(js.data()), reinterpret_cast<const char*>(js.data()) + js.size()};
  if (!jp.parse(m)) return false;
  StageProg sp;
  sp.stage = m.i("stage", -1);
  if (sp.stage < 0 || sp.stage > 4) return false;
  sp.valid = true; snprintf(sp.key, sizeof sp.key, "%.12s", key.c_str()); snprintf(sp.fkey, sizeof sp.fkey, "%s", key.c_str());
  sp.fragOutputMap = m.i("fragmentOutputMap");
  if (auto* a = m.get("cbuffers")) for (auto& c : a->arr) sp.cb.push_back({c.i("slot"), c.i("set"), c.i("binding")});
  if (auto* a = m.get("sbuffers"))
    for (auto& c : a->arr) sp.sb.push_back({c.i("slot"), c.i("set"), c.i("binding"), c.i("sbCbSlot"), c.i("sbCbOffset"), c.i("write")});
  auto readTex = [](const J* a, std::vector<MTex>& v) {
    if (!a) return;
    for (auto& c : a->arr)
      v.push_back({c.i("set"), c.i("binding"), c.i("handleIndex"), c.i("cbufSlot", -1), c.i("type"), std::max(c.i("array", 1), 1), c.i("separate")});
  };
  readTex(m.get("textures"), sp.tex);
  readTex(m.get("images"), sp.img);
  std::vector<uint32_t> words(spv.size() / 4);
  memcpy(words.data(), spv.data(), words.size() * 4);
  { std::vector<uint8_t> mark; if (sp.stage == 1 && (eft || ReadFile(base + ".efz", &mark)) && SpvAddEarlyFragmentTests(words) && eftApplied) *eftApplied = true; }
  SpvAddStorageImageCaps(words);
  if (sp.stage == 1) GtaoPatchFragment(words, key.c_str());
  if (sp.stage == 1) AaPatchFragment(words, key.c_str());   // same patch for pipelines built by the pipeline service
  if (sp.stage == 0) snap_rt::PatchVertexXfb(words, key.c_str());   // [rt]
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4; ci.pCode = words.data();
  if (vkCreateShaderModule(h->device, &ci, nullptr, &sp.mod) != VK_SUCCESS) return false;
  NoteSubgroup(sp.mod, words);
  if (sp.stage == 0) sp.inputMask = SpvVertexInputMask(words);
  if (sp.stage == 1) { sp.spv = words; sp.outLocs = SpvFragmentOutputs(words); }
  if (sp.stage == 0 && FoliageSwayKeepSpv()) sp.spv = words;
  *out = std::move(sp);
  return true;
}

}  // namespace

bool LoadStageFromCache(Host* h, const std::string& fullKey, StageProg* out, bool eft, bool* eftApplied) { return LoadStageFromCacheImpl(h, fullKey, out, eft, eftApplied); }
const char* TranslatorVersion() { return TranslatorSaltStr().c_str(); }
bool ModuleUsesSubgroup(VkShaderModule m) { std::lock_guard<std::mutex> l(g_sgMu); return g_sgMods.count((uint64_t)(uintptr_t)m) != 0; }

// A host-visible mapped buffer (the steps and their failure handling live in nvn_hostbuf.h, which tests/nvn/hostbuf_test.cpp drives with a
// failing fake). True only when created, bound and mapped; a failed step is logged to stderr (so the problem report has it).
bool CreateHostBuffer(Host* h, uint64_t size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
  struct RealOps {
    Host* h; uint64_t size;
    VkResult CreateBuffer(const VkBufferCreateInfo* i, VkBuffer* b) { return vkCreateBuffer(h->device, i, nullptr, b); }
    void MemReqs(VkBuffer b, VkMemoryRequirements* r) { vkGetBufferMemoryRequirements(h->device, b, r); }
    uint32_t MemType(uint32_t bits) { return h->FindMemoryType(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
    VkResult Allocate(const VkMemoryAllocateInfo* i, VkDeviceMemory* m) { return vkAllocateMemory(h->device, i, nullptr, m); }
    VkResult Bind(VkBuffer b, VkDeviceMemory m) { return vkBindBufferMemory(h->device, b, m, 0); }
    VkResult Map(VkDeviceMemory m, void** p) { return vkMapMemory(h->device, m, 0, VK_WHOLE_SIZE, 0, p); }
    void DestroyBuffer(VkBuffer b) { vkDestroyBuffer(h->device, b, nullptr); }
    void Free(VkDeviceMemory m) { vkFreeMemory(h->device, m, nullptr); }
    void Error(const char* step, VkResult r) { LogError("host buffer of %llu bytes: %s failed: %d", (unsigned long long)size, step, (int)r); }
  } ops{h, size};
  return snapnvn::CreateHostBufferWith(ops, size, usage, buf, mem, mapped);
}

void ProgramDestroy(ProgramImpl* p) {
  if (!p) return;
  Host* h = p->host;
  if (h && h->device) {
    // pipelines built from this program stay in the host map (keyed by program id, ids are never reused) and are
    // destroyed at shutdown
    if (!p->composite && p->id) {   // composites built from this program hold copies of its stage objects
      std::lock_guard<std::mutex> lk(h->compMutex);
      for (auto it = h->composites.begin(); it != h->composites.end();) {
        bool uses = false;
        for (uint64_t k : it->first) uses |= k == p->id;
        if (uses) { ProgramDestroy(it->second); delete it->second; it = h->composites.erase(it); } else ++it;
      }
    }
    if (!p->composite) for (auto& s : p->st) FreeStage(h, s);
    for (auto l : p->setLayout) if (l) vkDestroyDescriptorSetLayout(h->device, l, nullptr);
    if (p->layout) vkDestroyPipelineLayout(h->device, p->layout, nullptr);
  }
  p->layout = VK_NULL_HANDLE;
  for (auto& l : p->setLayout) l = VK_NULL_HANDLE;
  for (auto& u : p->setUsed) u = false;
}

// Binding model (see the harness): set 0 = support buffer (0) + c[s] of stage at 1 + stage*16 + s; set 1 = storage
// buffers stage*16 + n; set 2 = combined image samplers stage*32 + n; set 3 = storage images stage*8 + n.
bool ProgramSetShadersImpl(ProgramImpl* p, int n, const ShaderData* sd) {
  Host* h = p->host;
  snap_perf::Scope perfShader(snap_perf::C_SHADER); snap_perf::count(snap_perf::V_SHADERS);
  ProgramDestroy(p);
  if (n < 1 || n > 6 || !sd) return false;
  for (int i = 0; i < n; i++) {
    StageProg sp;
    if (!BuildStage(h, sd[i], &sp)) { ProgramDestroy(p); return false; }
    if (p->st[sp.stage].valid) {
      LogError("ProgramSetShaders: two shaders for stage %d", sp.stage);
      FreeStage(h, sp); ProgramDestroy(p);
      return false;
    }
    p->st[sp.stage] = std::move(sp);
  }
  return ProgramBuildLayout(p);
}

bool ProgramBuildLayout(ProgramImpl* p) {
  Host* h = p->host;
  struct B { VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; uint32_t count = 0; VkShaderStageFlags stages = 0; };
  std::map<std::pair<int, int>, B> binds;
  VkShaderStageFlags all = 0;
  for (auto& s : p->st) if (s.valid) all |= StageBit(s.stage);
  binds[{0, 0}] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all};
  auto add = [&](int set, int binding, VkDescriptorType t, uint32_t cnt, VkShaderStageFlags st) {
    auto& b = binds[{set, binding}];
    b.type = t; b.count = std::max(b.count, cnt); b.stages |= st;
  };
  for (auto& s : p->st) {
    if (!s.valid) continue;
    VkShaderStageFlags sb = StageBit(s.stage);
    for (auto& c : s.cb) add(c.set, c.binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sb);
    for (auto& c : s.sb) add(c.set, c.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sb);
    for (auto& t : s.tex) {
      if (t.separate) { LogError("separate texture/sampler bindings are not implemented yet"); continue; }
      add(t.set, t.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)t.array, sb);
    }
    for (auto& t : s.img) add(t.set, t.binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)t.array, sb);
  }
  for (int set = 0; set < 4; set++) {
    std::vector<VkDescriptorSetLayoutBinding> v;
    for (auto& kv : binds)
      if (kv.first.first == set) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = (uint32_t)kv.first.second; b.descriptorType = kv.second.type; b.descriptorCount = kv.second.count;
        b.stageFlags = kv.second.stages;
        v.push_back(b);
      }
    p->setUsed[set] = !v.empty();
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = (uint32_t)v.size(); lci.pBindings = v.data();
    if (vkCreateDescriptorSetLayout(h->device, &lci, nullptr, &p->setLayout[set]) != VK_SUCCESS) { ProgramDestroy(p); return false; }
  }
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 4; pl.pSetLayouts = p->setLayout;
  const uint32_t pomRange = PomPushRangeBytes(h->props.limits.maxPushConstantsSize);   // parallax (nvn_pom.cpp) uses bytes 128..255 of the same fragment range
  const VkPushConstantRange rtRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, pomRange ? pomRange : (uint32_t)sizeof(snap_rt::ShadowPush)};   // [rt] M3: the RT shadow receivers' ray-query constants (nvn_shadowrt.cpp); every layout carries it so push constants survive pipeline changes
  if (snap_rt::Capture() || pomRange) { pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &rtRange; }
  if (vkCreatePipelineLayout(h->device, &pl, nullptr, &p->layout) != VK_SUCCESS) { ProgramDestroy(p); return false; }
  {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    p->id = h->nextProgramId++;
  }
  return true;
}

ProgramImpl* ProgramResolve(Host* h, ProgramImpl* const src[6]) {
  ProgramImpl* first = nullptr;
  int bound = 0;
  std::array<uint64_t, 6> key{};
  for (int s = 0; s < 6; s++) {
    if (!src[s]) continue;
    if (!first) first = src[s];
    if (!src[s]->st[s].valid) continue;
    bound |= 1 << s;
    key[s] = src[s]->id;
  }
  if (!bound) return first;   // nothing usable: hand back any bound program so the draw reports it
  ProgramImpl* one = nullptr;
  bool single = true;
  for (int s = 0; s < 6; s++) if (bound & (1 << s)) { if (!one) one = src[s]; else if (one != src[s]) single = false; }
  if (single) for (int s = 0; s < 6; s++) if (one->st[s].valid && !(bound & (1 << s))) single = false;
  if (single) return one;
  std::lock_guard<std::mutex> lk(h->compMutex);
  auto it = h->composites.find(key);
  if (it != h->composites.end()) return it->second;
  auto* c = new ProgramImpl;
  c->host = h; c->composite = true;
  for (int s = 0; s < 6; s++) if (bound & (1 << s)) c->st[s] = src[s]->st[s];
  if (!ProgramBuildLayout(c)) { ProgramDestroy(c); delete c; return one; }
  h->composites[key] = c;
  return c;
}

}  // namespace nvn
