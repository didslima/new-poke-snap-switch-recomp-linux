// SPDX-License-Identifier: MIT
// See nvn_motionaa.h.
#include "nvn_motionaa.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>

#include "hle/live.h"
#include "nvn_dev.h"
#include "nvn_fx.h"
#include "nvn_internal.h"
#include "nvn_sparkle_spv.h"

namespace nvn {

namespace {
bool CiEq(const char* a, const char* b) { while (*a && *b) { if (std::tolower((unsigned char)*a++) != std::tolower((unsigned char)*b++)) return false; } return *a == *b; }
bool EnvOn(const char* n) { const char* e = getenv(n); return e && *e && strcmp(e, "0") != 0 && !CiEq(e, "off"); }
struct Live {
  int stable = EnvOn("SNAP_FOLIAGE_STABLE") ? 1 : 0;
  int spec = EnvOn("SNAP_SPECULAR_AA") ? 1 : 0;
  int probe = 0;
  Live() {
    dev_param_i("SNAP_FOLIAGE_STABLE", &stable, 0, 1, 0);
    dev_param_i("SNAP_SPECULAR_AA", &spec, 0, 1, 0);
    dev_param_i("SNAP_AA_PROBE", &probe, 0, 1023, 0);
    if (stable || spec) fprintf(stderr, "[motionaa] on: foliage stability %d, specular sparkle filter %d (photo frames keep the game's draw)\n", stable, spec);
  }
};
Live& L() {
  static Live l;
  static std::atomic<unsigned> seen{hle::live::gen().load()};
  if (hle::live::changed(seen)) { hle::live::EnvRead r; l.stable = EnvOn("SNAP_FOLIAGE_STABLE") ? 1 : 0; l.spec = EnvOn("SNAP_SPECULAR_AA") ? 1 : 0; }
  return l;
}
}  // namespace

int MotionAaProbeState() { const int p = L().probe; return p > 0 ? p / 16 : -1; }
bool FoliageStableEnabled() { const int s = MotionAaProbeState(); return s >= 0 ? (s & 4) != 0 : L().stable != 0; }
bool SpecularAaEnabled() { const int s = MotionAaProbeState(); return s >= 0 ? (s & 8) != 0 : L().spec != 0; }
bool MotionAaProbeMoving() { const int p = L().probe; return p > 0 && (p % 16) != 0; }

// ---- Specular sparkle filter (Fx pre-tonemap effect, opaque scene only) ----------------------------------------------------------------------------
namespace {
struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; uint32_t w = 0, h = 0; };
struct G {
  bool failed = false, ready = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkShaderModule fs = VK_NULL_HANDLE;
  VkSampler samp = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE; VkFormat fmt = VK_FORMAT_UNDEFINED;
  Img src;
} g;
struct Tune {
  float k = 2.0f, floor = 0.05f; int debug = 0;
  Tune() {
    if (const char* e = getenv("SNAP_SPECULAR_AA_K")) k = (float)atof(e);
    if (const char* e = getenv("SNAP_SPECULAR_AA_FLOOR")) floor = (float)atof(e);
    debug = EnvOn("SNAP_SPECULAR_AA_DEBUG") ? 1 : 0;
    dev_param_f("SNAP_SPECULAR_AA_K", &k, 1.0f, 8.0f, 2.0f);
    dev_param_f("SNAP_SPECULAR_AA_FLOOR", &floor, 0.0f, 1.0f, 0.05f);
    dev_param_i("SNAP_SPECULAR_AA_DEBUG", &debug, 0, 1, 0);
  }
};
Tune& T() { static Tune t; return t; }

void ImgBar(VkCommandBuffer cb, Img& im, VkImageLayout to) {
  if (im.layout == to) return;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = im.layout; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
  im.layout = to;
}
bool MakeCopyImage(Host* h, Img& o, uint32_t w, uint32_t hh, VkFormat fmt) {
  if (o.img || o.view || o.mem) { RetireVkImage(h, o.img, o.view, o.mem); o = Img{}; }   // render size changed: the old copy goes once the GPU is past it
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, hh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  o = Img{}; o.w = w; o.h = hh;
  if (vkCreateImage(h->device, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, o.img, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(h->device, &ai, nullptr, &o.mem) != VK_SUCCESS || vkBindImageMemory(h->device, o.img, o.mem, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = o.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return vkCreateImageView(h->device, &vc, nullptr, &o.view) == VK_SUCCESS;
}
bool Setup(Host* h, VkFormat fmt) {
  if (!g.ready) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(h->device, &si, nullptr, &g.samp) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 1; dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(h->device, &dl, nullptr, &g.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &g.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(h->device, &pli, nullptr, &g.pl) != VK_SUCCESS) return false;
    g.fs = FxMakeModule(h, kSparkleFrag, sizeof kSparkleFrag);
    if (!g.fs) return false;
    g.ready = true;
  }
  if (g.fmt != fmt || !g.pipe) { g.pipe = FxMakeFullscreenPipeline(h, g.pl, g.fs, fmt, false); g.fmt = fmt; }
  return g.pipe != VK_NULL_HANDLE;
}
bool SparkleEnabled() { return SpecularAaEnabled() && !g.failed; }
void SparklePre(ReplayCtx& cx) {
  if (g.failed || FxIsPhotoFrame()) return;   // the Fx slot never calls on a photo frame; checked again here
  TextureImpl* c = FxSceneColor();
  if (!c || !FxWritable(c) || !c->image) return;
  if (!Setup(cx.h, c->fi->vk)) { g.failed = true; fprintf(stderr, "[motionaa] sparkle filter setup failed, disabled\n"); return; }
  if (g.src.w != c->HW() || g.src.h != c->HH() || !g.src.img) {
    if (!MakeCopyImage(cx.h, g.src, c->HW(), c->HH(), c->fi->vk)) { g.failed = true; fprintf(stderr, "[motionaa] sparkle filter image failed, disabled\n"); return; }
  }
  VkCommandBuffer cb = cx.cb;
  ImgBar(cb, g.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  Transition(cb, c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkImageCopy r{}; r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.extent = {g.src.w, g.src.h, 1};
  vkCmdCopyImage(cb, c->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
  ImgBar(cb, g.src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  VkDescriptorSet set = FxAllocSet(cx, g.dsl);
  if (!set) return;
  VkDescriptorImageInfo ii{g.samp, g.src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
  vkUpdateDescriptorSets(cx.h->device, 1, &w, 0, nullptr);
  const Tune& t = T();
  const float pc[4] = {t.k < 1.0f ? 1.0f : t.k, t.floor < 0 ? 0.0f : t.floor, (float)t.debug, 0.0f};
  dev_pass_begin(cb, "sparkle");
  FxDrawFullscreen(cx, c, g.pipe, g.pl, set, pc, sizeof pc);
  dev_pass_end(cb, "sparkle");
  FxNoteRan();
}
const FxEffect kSparkle = {"specular_sparkle", SparkleEnabled, SparklePre, nullptr, true};
struct Reg { Reg() { FxRegister(&kSparkle); } } reg;
}  // namespace

void MotionAaShutdown(Host* h) {
  if (h && h->device != VK_NULL_HANDLE) {
    if (g.src.view != VK_NULL_HANDLE) vkDestroyImageView(h->device, g.src.view, nullptr);
    if (g.src.img != VK_NULL_HANDLE) vkDestroyImage(h->device, g.src.img, nullptr);
    if (g.src.mem != VK_NULL_HANDLE) vkFreeMemory(h->device, g.src.mem, nullptr);
    if (g.pipe != VK_NULL_HANDLE) vkDestroyPipeline(h->device, g.pipe, nullptr);
    if (g.fs != VK_NULL_HANDLE) vkDestroyShaderModule(h->device, g.fs, nullptr);
    if (g.pl != VK_NULL_HANDLE) vkDestroyPipelineLayout(h->device, g.pl, nullptr);
    if (g.dsl != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(h->device, g.dsl, nullptr);
    if (g.samp != VK_NULL_HANDLE) vkDestroySampler(h->device, g.samp, nullptr);
  }
  g = G{};
}

void MotionAaProbeOffset(const ReplayCtx& cx, float* dx, float* dy) {
  *dx = *dy = 0;
  const int p = L().probe;
  const int step = p > 0 ? p % 16 : 0;
  if (!step) return;
  const TextureImpl* d = cx.rt[8];
  if (!d || d->b.format != FMT_DEPTH32F || d->b.width != 1920 || d->b.height != 1080 || d->b.target != TT_2D || FxIsPhotoFrame()) return;
  *dx = (float)(step - 1) * 0.125f;
  *dy = (float)(step - 1) * 0.0625f;
}

}  // namespace nvn
