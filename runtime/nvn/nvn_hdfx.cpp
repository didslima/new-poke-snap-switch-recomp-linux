// SPDX-License-Identifier: GPL-3.0-only
// HD effects (docs/hd-effects.md): the game's own reduced-size effect targets get the same extra factor the render scale gives
// the scene, so no part of a 1440p / 4K frame is drawn at a fraction of the scene resolution the Switch chose.
//
// SNAP_RT_AUDIT=1 (test aid): one "[rtaudit]" line the first time each texture is bound as a render target (guest size, image
// size, format, flags, creating call chain), and one per storage image made. The list in docs/hd-effects.md came from this.
#include "nvn_internal.h"
#include "nvn_dev.h"
#include "nvn_foliage.h"
#include "hle/visual_defaults.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace nvn {
namespace {
struct AuditRec { uint64_t chain[4]; int n; bool bound; };
std::mutex g_auMu;
std::unordered_map<const TextureImpl*, AuditRec> g_au;
bool AuditOn() { static const bool on = [] { const char* e = getenv("SNAP_RT_AUDIT"); return e && e[0] == '1'; }(); return on; }
// SNAP_HD_EFFECTS (settings key gfx_hd_effects, default from the Look in hle/visual_defaults.h): 0/off = the game's sizes,
// 1/on = 2x, or a multiplier 1..4. Each eligible target grows by the factor but never past the scene size (1920 guest wide).
bool CiEq(const char* a, const char* b) { while (*a && *b) { if (std::tolower((unsigned char)*a++) != std::tolower((unsigned char)*b++)) return false; } return *a == *b; }
float ParseFactor(const char* e) {
  if (!e || !*e || CiEq(e, "off") || CiEq(e, "default")) return 1.0f;
  if (CiEq(e, "on")) return 2.0f;
  const float v = (float)atof(e);
  if (v <= 0.0f) return 1.0f;
  if (v == 1.0f) return 2.0f;   // "1" = On
  return std::min(4.0f, std::max(1.0f, v));
}
int AbBlock() { static const int p = [] { const char* e = getenv("SNAP_HD_EFFECTS_AB"); return e && *e ? std::max(0, atoi(e)) : 0; }(); return p; }
std::atomic<int> g_abPhase{0};   // 1 = this block is forced off (odd blocks)
float g_devOverride = -1.0f;     // dev control channel: SNAP_HD_EFFECTS_DEV (-1 = follow the setting); same-photo score checks flip it live
float g_devSeen = -1.0f;
}  // namespace

float HdfxFactorNow() {
  if (g_abPhase.load(std::memory_order_relaxed)) return 1.0f;
  if (g_devOverride >= 0.0f) return g_devOverride < 1.0f ? 1.0f : std::min(4.0f, g_devOverride);
  return ParseFactor(std::getenv("SNAP_HD_EFFECTS"));
}
void HdfxInit() {
  dev_param_f("SNAP_HD_EFFECTS_DEV", &g_devOverride, -1.0f, 4.0f, -1.0f);
  g_devSeen = g_devOverride;
  g_scale.hd = HdfxFactorNow();
  if (g_scale.hd > 1.0f) fprintf(stderr, "[hdfx] HD effects %.2fx: the half-size sea and particle layer at the scene size, bloom target larger\n", g_scale.hd);
}
bool HdfxAbFlip(int present) {
  bool flip = false;
  if (g_devOverride != g_devSeen) { g_devSeen = g_devOverride; flip = true; }
  const int p = AbBlock();
  if (p > 0) {
    const int ph = (present / p) & 1;
    if (ph != g_abPhase.load(std::memory_order_relaxed)) { g_abPhase.store(ph); flip = true; fprintf(stderr, "[hdfx] AB p%d: HD effects %s\n", present, ph ? "off" : "on"); }
  }
  return flip;
}
// Extra factor for a new or resized texture. Two targets qualify, both plain render targets that follow the render scale:
//   * the half-size layer the game draws the sea, splashes, dust and pollen into (960x540 colour C and the downsample B with its
//     depth; recognised by creation site in nvn_foliage.cpp, the same targets Soft particles grows): up to the scene size;
//   * the 192x108 bloom target: k times.
// Left alone on purpose: the 6-level 960x540 blur chain and the other 960x540 pair (A). The depth-of-field blur there samples a
// fixed number of texels, so a larger chain blurs less and shows blocky steps (analysis/hd_effects/crop_lab_dof_on_off.png).
// Photo targets, the Evaluation depth, shadow maps and window textures never qualify (Follows).
float HdfxBoost(const void* tex, const TextureBuilder& b, uint32_t tag) {
  const float k = g_scale.hd;
  if (k <= 1.0f || !g_scale.Follows(b, tag)) return 1.0f;
  if (b.width == 192 && b.height == 108) return k;
  if (b.width == 960 && b.height == 540 && b.levels <= 1) {
    const char lab = FoliageTexLabel(tex);
    if (lab == 'B' || lab == 'C') return std::min(k, 2.0f);
  }
  return 1.0f;
}

void HdfxAuditCreate(const TextureImpl* t, const uint64_t* rel, int n) {
  if (!AuditOn() || !t) return;
  AuditRec r{}; r.n = n < 4 ? n : 4;
  for (int i = 0; i < r.n; i++) r.chain[i] = rel[i];
  { std::lock_guard<std::mutex> g(g_auMu); g_au[t] = r; }
  if ((t->b.flags & TF_IMAGE) || t->b.target == TT_CUBEMAP || t->b.target == TT_CUBEMAP_ARRAY || t->b.target == TT_3D)
    fprintf(stderr, "[rtaudit] p%d made %dx%dx%d fmt=0x%x flags=0x%x tgt=%d lv=%d smp=%d -> %ux%u scale %.2f; main+%llx %llx %llx\n", g_presentCount,
            t->b.width, t->b.height, t->b.depth, t->b.format, t->b.flags, t->b.target, t->b.levels, t->b.samples, t->HW(), t->HH(), t->scale,
            (unsigned long long)r.chain[0], (unsigned long long)r.chain[1], (unsigned long long)r.chain[2]);
}
void HdfxAuditGone(const TextureImpl* t) {
  if (!AuditOn()) return;
  std::lock_guard<std::mutex> g(g_auMu);
  g_au.erase(t);
}
void HdfxAuditTargets(TextureImpl* const* rt) {
  if (!AuditOn()) return;
  for (int k = 0; k < 9; k++) {
    const TextureImpl* t = rt[k];
    if (!t) continue;
    AuditRec r{};
    {
      std::lock_guard<std::mutex> g(g_auMu);
      auto it = g_au.find(t);
      if (it == g_au.end()) { g_au[t] = AuditRec{}; g_au[t].bound = true; }
      else { if (it->second.bound) continue; it->second.bound = true; r = it->second; }
    }
    fprintf(stderr, "[rtaudit] p%d rt%d %dx%d fmt=0x%x flags=0x%x tgt=%d lv=%d smp=%d -> %ux%u scale %.2f wide %.2f; main+%llx %llx %llx %llx\n", g_presentCount, k,
            t->b.width, t->b.height, t->b.format, t->b.flags, t->b.target, t->b.levels, t->b.samples, t->HW(), t->HH(), t->scale, t->wide,
            (unsigned long long)r.chain[0], (unsigned long long)r.chain[1], (unsigned long long)r.chain[2], (unsigned long long)r.chain[3]);
  }
}
}  // namespace nvn
