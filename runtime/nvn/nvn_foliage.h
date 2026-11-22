// SPDX-License-Identifier: MIT
// Foliage and particle display upgrades (project code, ours): hashed alpha / alpha-to-coverage for alpha-tested draws, leaf
// translucency, full-resolution soft particles. Every effect is display only: photo frames (FxIsPhotoFrame), the shadow-map passes and
// the Evaluation targets keep the game's own draw. Switches (all off by default; read through the settings exports, see settings.cpp):
//   SNAP_FOLIAGE_AA=1             alpha-tested draws in the scene pass dither/cover their edge instead of a hard discard
//   SNAP_LEAF_TRANSLUCENCY=0..1   strength of the back-lit leaf term (0 = off)
//   SNAP_SOFT_PARTICLES=1         particles fade out where they meet geometry and are drawn at full render resolution
//   SNAP_FOLIAGE_LOG=<file>       debug: one line per distinct alpha-tested / blended scene draw combination
#pragma once
#include <cstdint>
#include <vector>

namespace nvn {

struct Host;
struct StageProg;
}  // namespace nvn
struct VkShaderModule_T;   // same handle type as vulkan.h (64 bit), so this header needs no Vulkan include
typedef VkShaderModule_T* VkShaderModule;
namespace nvn {

struct FoliageTex { int binding; uint32_t fmt, w, h; };
struct FoliageDrawInfo {
  int present;
  const char* vs; const char* fs;
  int alphaFunc; float alphaRef;
  int nColor; uint32_t colorFmt0, rtW, rtH, rtSamples; uint32_t depthFmt;
  bool blend0, depthWrite, depthTest;
  int srcRgb, dstRgb, srcA, dstA;
  int nTex; FoliageTex tex[8];
  int nVerts, nInst;
  char rtLabel, dsLabel;   // FoliageTexLabel of colour 0 / depth ('A', 'B' = the two half-resolution particle target pairs, 0 = other)
};
void FoliageLogDraw(const FoliageDrawInfo& d);   // debug census (SNAP_FOLIAGE_LOG), cheap when unset

// SPIR-V wrapper for alpha-tested fragment modules (nvn_foliage.cpp): `in` is the translated module, `out` replaces it; spec constant 0 = alpha reference.
// hash: dithered / covered edge; leaf: 0..8 back-lit gain level (needs the scene light and camera constant buffers; ignored when the module has none).
// covVar (Foliage stability): a Private float variable holding the mip level the alpha texture was read at (PatchCaptureAlphaLod); the tested alpha is
// a * (1 + 0.25 * max(lod, 0)), so the smaller mips keep the coverage of the full-size texture. 0 = none.
bool PatchAlphaHash(const std::vector<uint32_t>& in, int func, bool a2c, bool hash, int leaf, std::vector<uint32_t>& out, uint32_t covVar = 0);
// Foliage stability: after the first 2D (not arrayed) implicit-lod sample of the module, stores OpImageQueryLod(...).x (the mip level read) into a new
// Private float variable (*lodVar). Returns false when the module has no such sample (the caller keeps the module as it was).
bool PatchCaptureAlphaLod(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, uint32_t* lodVar);
bool FoliageAaPrepass();       // the depth pre-pass gets the same dithered edge as the colour pass (SNAP_FOLIAGE_AA_PREPASS=1, test aid; default the hard test)
// Wind sway (SNAP_WIND_SWAY, 0..1): vertex wrapper that adds a small world-space wind displacement to alpha-tested scene draws (nvn_foliage.cpp). PipeDesc.pad2 bits 17..20 = level 0..8;
// the pipeline builder asks FoliageSwayModule for the patched vertex module (null = keep the game one) and passes FoliageSwayAmpFor(level) as specialization constant 0.
bool PatchVertexSway(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);
VkShaderModule FoliageSwayModule(Host* h, const StageProg& vs);
float FoliageSwayStrength();
int FoliageSwayPhase();   // SNAP_WIND_PHASE debug time offset step 0..7 (pad2 bits 21..23, 0.35 s each; frozen-frame strips)
bool FoliageSwayZero();   // debug (SNAP_FOLIAGE_PICK 5): the wrapper runs with amplitude 0 (pad2 bit 24)
float FoliageSwayAmp();   // SNAP_WIND_AMP override (world units at full strength), 0 = built in default
inline float FoliageSwayAmpFor(int level) { const float a = FoliageSwayAmp(); return (a > 0 ? a : 0.05f) * (float)level * 0.125f; }
bool FoliageSwayKeepSpv();   // true when vertex programs keep their SPIR-V for the wrapper (sway on, or the dev loop)
// PipeDesc.pad2 value for one draw (0 = the game's own hard alpha test; bit 0 = hashed edge, bits 4..7 = leaf gain level); formats are Vulkan format values.
uint32_t FoliagePipeBits(int alphaFunc, float alphaRef, int32_t colorVkFmt, int32_t depthVkFmt, bool depthTest, int nColor, bool blend0, int depthFunc = 0, bool depthWrite = false);   // depthFunc/depthWrite: the scene depth pre-pass (no colour target) joins the wind sway

// Full-resolution particles (SNAP_SOFT_PARTICLES): the game draws particles into 960x540 targets (RGBA16F colour + 32 bit float depth, a downsample
// of the scene depth) and composites them back; those targets are made `boost` times larger so the particle layer has the scene's resolution.
// Called from TextureInitialize with the guest call chain (relative to main); returns the extra scale (1 = unchanged) and labels the texture.
float FoliageParticleBoost(const void* tex, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t* chainRel, int n);
void FoliageTextureGone(const void* tex);
char FoliageTexLabel(const void* tex);
float FoliageParticleBoostLive(const void* tex, float created);

bool FoliageAaEnabled();    // SNAP_FOLIAGE_AA
float FoliageLeafStrength();   // SNAP_LEAF_TRANSLUCENCY
int FoliageLeafDebugFlags();   // SNAP_LEAF_FLIP (16) | SNAP_LEAF_DEBUG (32): debug switches for the leaf term
bool SoftParticlesEnabled();   // SNAP_SOFT_PARTICLES

}  // namespace nvn
