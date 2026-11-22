// SPDX-License-Identifier: MIT
// Live render-target sizes (project code, ours). The Internal resolution, Anti-aliasing (its supersampling part), Shadow resolution and Ultrawide
// rows used to need a restart because a texture's image size is fixed when the game creates it (TextureInitialize). Here the size policy is read
// again when a setting changes (hle/live.h) and every live texture whose size it changes gets a new image:
//   * at a present, under the queue mutex, after the present's own copy of the window texture was recorded;
//   * the old image, its views and its stand-ins (compressed-target, guest-size and 16:9 twins) move into a throwaway TextureImpl that goes to the
//     graveyard after the present's submit, so the GPU finishes with them first;
//   * the texture keeps its address (the game and every cache name it by pointer) and makes its new image right away;
//   * the old contents are stretched into the new image (targets drawn once, like the static shadow maps, stay valid until they are drawn again).
// Not done here: switching an upscaler or frame generation on or off, and switching to or from the game's own FXAA (its shaders are patched when
// they are built). Those report "blocked" and the options menu keeps its restart note for them. SNAP_LIVE_SIZE=0 turns the whole file off.
#include "nvn_internal.h"
#include "nvn_os_host.h"
#include "nvn_aa.h"
#include "nvn_framegen.h"
#include "nvn_foliage.h"
#include "hle/live.h"
#include "hle/lighting_preset.h"
#include "hle/visual_defaults.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nvn {
namespace {
struct Rec { uint32_t tag; float boost; };
std::mutex g_mu;                                   // g_recs, and the policy values while they move
std::unordered_map<TextureImpl*, Rec> g_recs;      // every texture the game has made and not finalized
std::vector<TextureImpl*> g_husks;                 // old images of this present, retired after its submit (queue mutex)
std::atomic<int> g_blocked{0};                     // bit 0: the anti-aliasing mode, bit 1: the sizes could not follow the settings

bool On() { static const bool on = [] { const char* e = getenv("SNAP_LIVE_SIZE"); return !(e && e[0] == '0'); }(); return on; }

void SizeFor(const TextureImpl* t, const Rec& r, float* scale, float* wide, uint32_t* hw, uint32_t* hh) {
  *scale = g_scale.For(t->b, r.tag) * std::max(FoliageParticleBoostLive(t, r.boost), HdfxBoost(t, t->b, r.tag));
  *wide = WideFor(t->b, r.tag);
  *hw = *hh = 0;
  if (*scale != 1.0f || *wide != 1.0f) {   // as TextureInitialize
    *hw = (uint32_t)std::max(1.0f, std::ceil(t->b.width * *scale * *wide));
    *hh = (uint32_t)std::max(1.0f, std::ceil(t->b.height * *scale));
  }
}

void Bar(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img; b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
}

// Can the old contents be stretched into the new image with a blit, and with which filter?
bool CanBlit(Host* h, const FormatInfo* fi, VkFilter* filter) {
  if (!fi || fi->bw > 1 || fi->bh > 1) return false;   // block-compressed: no blit (its stand-in is drawn again)
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(h->phys, fi->vk, &fp);
  const VkFormatFeatureFlags f = fp.optimalTilingFeatures;
  if (!(f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(f & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return false;
  const bool color = fi->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
  *filter = color && (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  return true;
}

// queue mutex and g_mu held. Returns 1 when the old contents were copied, 0 when the texture starts empty again.
int Swap(Host* h, VkCommandBuffer cb, TextureImpl* t, float scale, float wide, uint32_t hw, uint32_t hh) {
  TextureImpl* k = nullptr;
  if (t->image || t->bcShadow || t->xferShadow || t->wideTwin) {
    k = new TextureImpl();
    k->host = t->host; k->b = t->b; k->fi = t->fi; k->scale = t->scale; k->wide = t->wide; k->hostW = t->hostW; k->hostH = t->hostH;
    k->image = t->image; k->memory = t->memory; k->layout = t->layout; k->subLayout.swap(t->subLayout);
    k->imgLevels = t->imgLevels; k->imgLayers = t->imgLayers; k->ci_flags = t->ci_flags;
    k->views.swap(t->views); k->viewTypes.swap(t->viewTypes);
    k->bcShadow = t->bcShadow; k->xferShadow = t->xferShadow; k->wideTwin = t->wideTwin;
    g_husks.push_back(k);
  }
  TexPrefetchForget(t);
  FgForgetTexture(t);
  t->image = VK_NULL_HANDLE; t->memory = VK_NULL_HANDLE; t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  t->subLayout.clear(); t->views.clear(); t->viewTypes.clear();
  t->viewLast = VK_NULL_HANDLE; memset(t->viewLastKey, 0, sizeof t->viewLastKey);
  t->bcShadow = t->xferShadow = t->wideTwin = nullptr; t->wideDirty = false;
  t->scale = scale; t->wide = wide; t->hostW = hw; t->hostH = hh;
  VkFilter filter = VK_FILTER_NEAREST;
  if (k && k->image && t->uploaded && k->subLayout.empty() && t->b.samples <= 1 && CanBlit(h, t->fi, &filter) && t->EnsureImage()) {
    const VkImageAspectFlags aspect = t->fi->aspect;
    Bar(cb, k->image, aspect, k->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Bar(cb, t->image, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const uint32_t levels = std::min(k->imgLevels, t->imgLevels), layers = std::min(k->imgLayers, t->imgLayers);
    for (uint32_t l = 0; l < levels; l++) {
      VkImageBlit r{};
      r.srcSubresource = {aspect, l, 0, layers}; r.dstSubresource = {aspect, l, 0, layers};
      r.srcOffsets[1] = {(int32_t)k->LevelW(l), (int32_t)k->LevelH(l), 1};
      r.dstOffsets[1] = {(int32_t)t->LevelW(l), (int32_t)t->LevelH(l), 1};
      vkCmdBlitImage(cb, k->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, filter);
    }
    k->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    t->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    return 1;
  }
  t->uploaded = false;   // as a texture the game has just made
  return 0;
}

// What the size rows say now, as one string: the work below runs only when this moves.
std::string Sig(uint32_t scW, uint32_t scH) {
  std::string s;
  for (const char* k : {"SNAP_RENDER_SCALE", "SNAP_AA", "SNAP_SHADOW_RES", "SNAP_LIGHTING", "SNAP_ULTRAWIDE", "SNAP_HD_EFFECTS"}) { const char* e = getenv(k); s += e ? e : ""; s += '|'; }
  s += std::to_string(snapdef::saved_look().load(std::memory_order_relaxed));
  s += '|'; s += std::to_string(scW); s += 'x'; s += std::to_string(scH);
  return s;
}
}  // namespace

void RetargetNote(TextureImpl* t, uint32_t tag, float boost) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_recs[t] = {tag, boost};
  float s, w; uint32_t hw, hh;
  SizeFor(t, g_recs[t], &s, &w, &hw, &hh);
  if (s != t->scale || w != t->wide || hw != t->hostW || hh != t->hostH) { t->scale = s; t->wide = w; t->hostW = hw; t->hostH = hh; }
}
void RetargetGone(TextureImpl* t) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_recs.erase(t);
}

// Dedicated-memory images of the effect modules (lighting, atmosphere, lens, water): freed once the GPU is past every submit made so far.
void RetireVkImage(Host* h, VkImage img, VkImageView view, VkDeviceMemory mem) {
  if (!h || !h->device || (!img && !view && !mem)) return;
  VkDevice d = h->device;
  RetireFn([d, img, view, mem] {
    if (view) vkDestroyImageView(d, view, nullptr);
    if (img) vkDestroyImage(d, img, nullptr);
    if (mem) vkFreeMemory(d, mem, nullptr);
  });
}

void RetargetAtPresent(Host* h, VkCommandBuffer cb, uint32_t scW, uint32_t scH) {
  if (!On()) return;
  // The longest present-to-present time over the frames a resize touches (this present and the three after it), for the mid-ride test.
  static std::chrono::steady_clock::time_point prevT; static int watch = 0; static double worst = 0;
  {
    const auto now = std::chrono::steady_clock::now();
    if (watch > 0) {
      worst = std::max(worst, std::chrono::duration<double, std::milli>(now - prevT).count());
      if (--watch == 0) fprintf(stderr, "[live] p%d longest frame over the resize: %.1f ms\n", g_presentCount, worst);
    }
    prevT = now;
  }
  static std::atomic<unsigned> seen{0};
  static uint32_t appliedW = 0, appliedH = 0, candW = 0, candH = 0; static int stable = 0;
  const bool envMoved = hle::live::changed(seen);
  bool sizeMoved = false;
  if (scW && scH) {
    if (!appliedW) { appliedW = scW; appliedH = scH; }
    else if (scW != appliedW || scH != appliedH) {
      if (scW != candW || scH != candH) { candW = scW; candH = scH; stable = 0; }
      else if (++stable >= 30) { appliedW = scW; appliedH = scH; sizeMoved = true; }
    } else { candW = candH = 0; stable = 0; }
  }
  const bool abFlip = HdfxAbFlip(g_presentCount);
  if (!envMoved && !sizeMoved && !abFlip) return;
  hle::live::EnvRead er;
  static std::string last;
  static std::string lastWide;
  static bool first = true;
  const std::string sig = Sig(appliedW, appliedH);
  { const char* uw = getenv("SNAP_ULTRAWIDE"); const std::string w = uw ? uw : ""; if (first) lastWide = w; }
  if (first) { first = false; last = sig; if (!abFlip) return; }
  if (sig == last && !abFlip) return;
  last = sig;
  const auto t0 = std::chrono::steady_clock::now();

  // An upscaler sets the scene and window sizes itself, and frame generation keeps images of earlier frames: both stay restart-only.
  if (g_scale.displayScale > 0.f || g_aaUpscalerOn || FgWanted()) {
    g_blocked.store(3);
    static int n; if (n++ < 3) fprintf(stderr, "[live] p%d size rows changed: not applied while an upscaler or frame generation runs (restart)\n", g_presentCount);
    return;
  }
  float sc = 1.0f;
  if (const char* e = getenv("SNAP_RENDER_SCALE")) { const float v = (float)atof(e); if (v >= 0.25f && v <= 8.f) sc = v; }
  bool aaBlocked = false;
  { const float ss = AaReparseLive(&aaBlocked); if (ss > 0.f && !AaFailed()) sc = ss; }
  int sr = 0;
  if (snaplight::enhanced()) sr = snaplight::shadow_res_for_scale(sc);
  else if (snaplight::shadow_res()) sr = snaplight::shadow_res();
  if (const char* e = getenv("SNAP_SHADOW_RES")) { const int v = atoi(e); sr = ((v > 1024 && v <= 8192) || v == 512) ? v : 0; }
  float wk = g_wideK;
  {   // as host/boot.cpp, from the swapchain size; read only when the row itself moved (a resized window alone changes nothing mid-run)
    const char* uw = getenv("SNAP_ULTRAWIDE"); const std::string w = uw ? uw : "";
    if ((w != lastWide || sizeMoved) && scW && scH) {
      lastWide = w;
      const bool off = nvn::os_host::ascii_ieq(w.c_str(), "off") || w == "0";
      const float k = (float)((double)scW / scH / (16.0 / 9.0));
      wk = (!off && k > 1.01f) ? std::min(k, 4.0f) : 1.0f;
    }
  }
  g_blocked.store(aaBlocked ? 1 : 0);
  const float hd = HdfxFactorNow();
  if (sc == g_scale.scale && sr == g_scale.shadowRes && wk == g_wideK && hd == g_scale.hd) return;

  int changed = 0, copied = 0;
  const float sc0 = g_scale.scale, wk0 = g_wideK, hd0 = g_scale.hd; const int sr0 = g_scale.shadowRes;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_scale.scale = sc; g_scale.shadowRes = sr; g_wideK = wk; g_scale.hd = hd;
    for (auto& kv : g_recs) {
      TextureImpl* t = kv.first;
      float s, w; uint32_t hw, hh;
      SizeFor(t, kv.second, &s, &w, &hw, &hh);
      if (s == t->scale && w == t->wide && hw == t->hostW && hh == t->hostH) continue;
      changed++;
      copied += Swap(h, cb, t, s, w, hw, hh);
    }
  }
  watch = 4; worst = 0;
  if (SamplerPolicyReparse() || (sc != sc0 && g_lodBiasAuto)) g_samplerPolicyGen.fetch_add(1, std::memory_order_relaxed);   // the automatic mip bias follows the render scale
  fprintf(stderr, "[live] p%d render targets resized: scale %.3f -> %.3f, shadow map %d -> %d, ultrawide %.3f -> %.3f; %d textures (%d kept their contents), %.1f ms\n",
          g_presentCount, sc0, sc, sr0 ? sr0 : 1024, sr ? sr : 1024, wk0, wk, changed, copied,
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void RetargetSubmitFailed() {
  for (TextureImpl* k : g_husks) Retire(GK_TEXTURE, k);
  g_husks.clear();
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& kv : g_recs) if (kv.first->layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) { kv.first->layout = VK_IMAGE_LAYOUT_UNDEFINED; kv.first->uploaded = false; }
}
void RetargetAfterSubmit() {
  for (TextureImpl* k : g_husks) Retire(GK_TEXTURE, k);
  g_husks.clear();
}
}  // namespace nvn

// Options menu: which size rows the running game could not follow (bit 0 the anti-aliasing mode, bit 1 the sizes). 0 = all applied.
extern "C" int snap_live_size_blocked(void) { return nvn::g_blocked.load(); }
