// SPDX-License-Identifier: MIT
// See nvn_foliage.h.
#include "nvn_foliage.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nvn_dev.h"
#include "hle/live.h"   // rows that apply with no restart
#include "nvn_fx.h"
#include "nvn_internal.h"
#include "nvn_motionaa.h"
#include "nvn_os_host.h"

namespace nvn {

namespace {
bool EnvOn(const char* n) { const char* e = getenv(n); return e && *e && strcmp(e, "0") != 0 && !nvn::os_host::ascii_ieq(e, "off"); }
// Live values (dev loop: the A/B capture tool and F1 "Tuning" set them by env name); without SNAP_DEV they are the env values read once.
float LeafEnv() { const char* e = getenv("SNAP_LEAF_TRANSLUCENCY"); if (!e || !*e || nvn::os_host::ascii_ieq(e, "off")) return 0.0f; const float f = (float)atof(e); return f < 0 ? 0.0f : f > 1 ? 1.0f : f; }
struct Live {
  int aa = EnvOn("SNAP_FOLIAGE_AA") ? 1 : 0;
  float leaf = LeafEnv();
  int soft = EnvOn("SNAP_SOFT_PARTICLES") ? 1 : 0;
  float sway = [] { const char* e = getenv("SNAP_WIND_SWAY"); if (!e || !*e || nvn::os_host::ascii_ieq(e, "off")) return 0.0f; const float f = (float)atof(e); return f < 0 ? 0.0f : f > 1 ? 1.0f : f; }();
  int pick = 0;   // dev: one value runs one effect (cost runs): 1 edge smoothing, 2 translucency 1.0, 3 wind sway 1.0, 4 all three; 0 = the rows as set
  int swayPhase = [] { const char* e = getenv("SNAP_WIND_PHASE"); const int v = e && *e ? atoi(e) : 0; return v < 0 ? 0 : v > 7 ? 7 : v; }();   // debug: time offset step 0..7 (0.35 s each) for frozen-frame strips
  float swayAmp = [] { const char* e = getenv("SNAP_WIND_AMP"); const float f = e && *e ? (float)atof(e) : 0.0f; return f > 0 ? f : 0.0f; }();   // world units at full strength (0 = built in default)
  int leafFlip = EnvOn("SNAP_LEAF_FLIP") ? 1 : 0;      // debug: negate the camera-toward-light dot product
  int leafDebug = EnvOn("SNAP_LEAF_DEBUG") ? 1 : 0;    // debug: leaf term painted pure red (shows where it applies)
  Live() {
    dev_param_i("SNAP_LEAF_FLIP", &leafFlip, 0, 1, 0);
    dev_param_i("SNAP_LEAF_DEBUG", &leafDebug, 0, 1, 0);
    dev_param_i("SNAP_FOLIAGE_AA", &aa, 0, 1, 0);
    dev_param_f("SNAP_LEAF_TRANSLUCENCY", &leaf, 0.0f, 1.0f, 0.0f);
    dev_param_i("SNAP_SOFT_PARTICLES", &soft, 0, 1, 0);
    dev_param_f("SNAP_WIND_SWAY", &sway, 0.0f, 1.0f, 0.0f);
    dev_param_i("SNAP_WIND_PHASE", &swayPhase, 0, 7, 0);
    dev_param_i("SNAP_FOLIAGE_PICK", &pick, 0, 5, 0);
    if (aa || leaf > 0 || soft || sway > 0) fprintf(stderr, "[foliage] on: leaf edge smoothing %d, leaf translucency %.2f, soft particles %d, wind sway %.2f (photo frames keep the game's draw)\n", aa, leaf, soft, sway);
  }
};
// The Leaf edge smoothing and Leaf translucency rows apply with no restart (hle/live.h): both pick a shader variant per draw, built on demand.
// Soft particles stays as read at the start: it also doubles the particle targets when the game creates them.
Live& L() {
  static Live l;
  static std::atomic<unsigned> seen{hle::live::gen().load()};   // the constructor just read the start values
  if (hle::live::changed(seen)) { hle::live::EnvRead env_read; l.aa = EnvOn("SNAP_FOLIAGE_AA") ? 1 : 0; l.leaf = LeafEnv(); }
  return l;
}
}  // namespace
bool FoliageAaEnabled() { const int ps = MotionAaProbeState(); if (ps >= 0) return (ps & 1) != 0; const int p = L().pick; return p ? (p == 1 || p == 4) : L().aa != 0; }
bool FoliageAaPrepass() {
  const int ps = MotionAaProbeState(); if (ps >= 0) return (ps & 2) != 0;
  static const bool on = [] { const char* e = getenv("SNAP_FOLIAGE_AA_PREPASS"); return e && e[0] == '1'; }();
  return on;
}
float FoliageLeafStrength() { const int p = L().pick; if (p) return (p == 2 || p == 4) ? 1.0f : 0.0f; const float f = L().leaf; return f < 0 ? 0.0f : f > 1 ? 1.0f : f; }
bool SoftParticlesEnabled() { return L().soft != 0; }
float FoliageSwayStrength() { const int p = L().pick; if (p) return (p == 3 || p == 4 || p == 5) ? 1.0f : 0.0f; const float f = L().sway; return f < 0 ? 0.0f : f > 1 ? 1.0f : f; }
bool FoliageSwayZero() { return L().pick == 5; }   // debug pick 5: wrapper on, amplitude 0 (pad2 bit 24)
float FoliageSwayAmp() { return L().swayAmp; }
int FoliageSwayPhase() { const int v = L().swayPhase; return v < 0 ? 0 : v > 7 ? 7 : v; }
int FoliageLeafDebugFlags() { const int ps = MotionAaProbeState(); return (L().leafFlip ? 16 : 0) | ((L().leafDebug || (ps >= 0 && (ps & 16))) ? 32 : 0); }

// ---- hashed alpha / alpha to coverage (SPIR-V wrapper, like PatchAlphaTest in nvn_alpha.cpp) --------------------------------
namespace {
enum : uint32_t {
  kOpEntryPoint = 15, kOpExecutionMode = 16, kOpTypeBool = 20, kOpTypeFloat = 22, kOpTypeVector = 23, kOpTypePointer = 32, kOpVariable = 59,
  kOpFunction = 54, kOpFunctionCall = 57, kOpLoad = 61, kOpStore = 62, kOpLabel = 248, kOpBranchConditional = 250, kOpReturn = 253, kOpKill = 252,
  kOpFunctionEnd = 56, kOpSelectionMerge = 247, kOpDecorate = 71, kOpMemberDecorate = 72, kOpDecorationGroup = 73, kOpGroupDecorate = 74,
  kOpGroupMemberDecorate = 75, kOpSpecConstant = 50, kOpConstant = 43, kOpCompositeExtract = 81, kOpCompositeInsert = 82, kOpFAdd = 129,
  kOpFSub = 131, kOpFMul = 133, kOpFDiv = 136, kOpFRem = 140, kOpFOrdGreaterThan = 186, kOpFwidth = 209,
};
void Emit(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
  v.push_back(((uint32_t)(w.size() + 1) << 16) | op);
  v.insert(v.end(), w.begin(), w.end());
}
uint32_t FBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
}  // namespace

// func: NVN compare enum (2 LESS, 4 LEQUAL, 5 GREATER, 7 GEQUAL). hash: the wrapper keeps the contour of the game's test (alpha == ref) but turns the hard
// edge into a one pixel wide ramp: s = (a - ref) / (0.5 * fwidth(a) + eps) + 0.5, then a fragment survives when s beats a per-pixel interleaved-gradient
// noise value (dither; SSAA, SMAA and the temporal upscalers average it into an anti-aliased edge). Flat alpha (fwidth ~ 0) keeps the game's
// exact decision. a2c: no discard, s is written to the alpha output and the pipeline enables alphaToCoverage (multisampled targets only).
// !hash: the game's own hard test (same compare as PatchAlphaTest), used when only the leaf term is on.
// leaf 1..8: back-lit gain. The camera direction (PerViewCBuffer, set 0 binding 20, vec4 37) and the light (set 0 binding 22, vec4 1 = direction the light travels, negated to get toward-light)
// give d = dot(forward, toLight); looking toward the sun (d > -0.35) the lit colour is multiplied by 1 + gain * ((d+0.35)/1.35)^2 * tint (warm: red kept,
// green 0.9, blue 0.35), as sunlight showing through the leaf. Modules without those two constant buffers (or the GLSL.std450 import) get no leaf term.
// Returns false when the module is not of the expected shape (the caller then keeps the game's own hard test).
bool PatchAlphaHash(const std::vector<uint32_t>& in, int func, bool a2c, bool hash, int leaf, std::vector<uint32_t>& out, uint32_t covVar) {
  if (func != 2 && func != 4 && func != 5 && func != 7) return false;
  if (in.size() < 6 || in[0] != 0x07230203) return false;
  const uint32_t bound = in[3];
  uint32_t entry = 0, var0 = 0, voidT = 0, fnT = 0, boolT = 0, annotEnd = 0, firstFn = 0, fcVar = 0, inPtr = 0, varType = 0, glslSet = 0, intT = 0, uPtrF = 0;
  struct Ptr { uint32_t id, sc, pointee; };
  std::vector<Ptr> ptrs;
  struct Vec { uint32_t id, comp, n; };
  std::vector<Vec> vecs;
  std::vector<uint32_t> floats;
  std::vector<std::pair<uint32_t, uint32_t>> varSc;           // variable id -> pointer type
  std::vector<std::pair<uint32_t, uint32_t>> bindOf, setOf;   // variable id -> Binding / DescriptorSet
  size_t i = 5;
  while (i < in.size()) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return false;
    switch (op) {
      case kOpEntryPoint: if (in[i + 1] == 4 && !entry) entry = in[i + 2]; break;
      case 11: if (wc >= 4 && !strncmp((const char*)&in[i + 2], "GLSL.std.450", 12)) glslSet = in[i + 1]; break;   // OpExtInstImport
      case kOpDecorate:
        if (wc >= 4 && in[i + 2] == 30 && in[i + 3] == 0) var0 = in[i + 1];
        if (wc >= 4 && in[i + 2] == 11 && in[i + 3] == 15) fcVar = in[i + 1];   // BuiltIn FragCoord
        if (wc >= 4 && in[i + 2] == 33) bindOf.push_back({in[i + 1], in[i + 3]});
        if (wc >= 4 && in[i + 2] == 34) setOf.push_back({in[i + 1], in[i + 3]});
        annotEnd = (uint32_t)(i + wc); break;
      case kOpMemberDecorate: case kOpDecorationGroup: case kOpGroupDecorate: case kOpGroupMemberDecorate: annotEnd = (uint32_t)(i + wc); break;
      case kOpTypeBool: boolT = in[i + 1]; break;
      case 21: if (wc >= 4 && in[i + 2] == 32 && !intT) intT = in[i + 1]; break;   // OpTypeInt 32
      case kOpTypeFloat: if (in[i + 2] == 32) floats.push_back(in[i + 1]); break;
      case kOpTypeVector: vecs.push_back({in[i + 1], in[i + 2], in[i + 3]}); break;
      case kOpTypePointer: ptrs.push_back({in[i + 1], in[i + 2], in[i + 3]}); break;
      case kOpVariable: varSc.push_back({in[i + 2], in[i + 1]}); break;
      case kOpFunction: if (!firstFn) firstFn = (uint32_t)i; if (in[i + 2] == entry) { fnT = in[i + 4]; voidT = in[i + 1]; } break;
      default: break;
    }
    i += wc;
  }
  if (!entry || !var0 || !fnT || !firstFn || !annotEnd) return false;
  for (auto& v : varSc) if (v.first == var0) varType = v.second;
  uint32_t vecT = 0, floatT = 0;
  for (auto& p : ptrs) if (p.id == varType) vecT = p.pointee;
  for (auto& v : vecs) if (v.id == vecT && v.n == 4) for (uint32_t f : floats) if (f == v.comp) floatT = f;
  if (!floatT) return false;
  const bool dither = hash && !a2c;
  const bool addFc = dither && !fcVar;
  if (dither && fcVar) {   // an existing FragCoord must be an Input vec4 float; reuse it
    bool ok = false;
    for (auto& v : varSc) if (v.first == fcVar) for (auto& p : ptrs) if (p.id == v.second && p.sc == 1 && p.pointee == vecT) ok = true;
    if (!ok) return false;
  }
  for (auto& p : ptrs) if (p.sc == 1 && p.pointee == vecT) inPtr = p.id;
  for (auto& p : ptrs) if (p.sc == 2 && p.pointee == floatT) uPtrF = p.id;
  auto lookup = [](const std::vector<std::pair<uint32_t, uint32_t>>& m, uint32_t v, uint32_t& val) { for (auto& e : m) if (e.first == v) { val = e.second; return true; } return false; };
  uint32_t cam = 0, light = 0;   // set 0 binding 20 / 22 uniform variables
  for (auto& b : bindOf) {
    uint32_t st = 0;
    if (!lookup(setOf, b.first, st) || st != 0) continue;
    if (b.second == 20) cam = b.first; else if (b.second == 22) light = b.first;
  }
  const bool leafFlip = (leaf & 16) != 0;
  const int leafLvl = leaf & 15;   // 1..8 gain, 9 = debug (red)
  const bool doLeaf = leafLvl > 0 && cam && light && glslSet && intT;
  const bool addUPtr = doLeaf && !uPtrF;
  if (!hash && !doLeaf && !covVar) return false;

  uint32_t next = bound;
  auto nid = [&]() { return next++; };
  const uint32_t refId = nid(), newMain = nid(), lbl = nid(), callId = nid(), loadId = nid(), aId = nid(), dId = nid(), wId = nid(), weId = nid(), qId = nid(), sId = nid(),
                 fcId = nid(), xId = nid(), yId = nid(), txId = nid(), tyId = nid(), tId = nid(), f1Id = nid(), mId = nid(), hId = nid(), condId = nid(), killLbl = nid(),
                 mergeLbl = nid(), cHalf = nid(), cEps = nid(), cOne = nid(), cK1 = nid(), cK2 = nid(), cK3 = nid(), nvId = nid();
  const bool addBool = !boolT && (!a2c || covVar);
  if (addBool) boolT = nid();
  const uint32_t cCovQ = covVar ? nid() : 0, cCovZ = covVar ? nid() : 0;
  const bool addPtr = addFc && !inPtr;
  if (addPtr) inPtr = nid();
  if (addFc) fcVar = nid();
  uint32_t cI0 = 0, cI1 = 0, cI2 = 0, cI37 = 0, cF025 = 0, cF133 = 0, cF0 = 0, cGain = 0, cTg = 0, cTb = 0;
  if (doLeaf) {
    cI0 = nid(); cI1 = nid(); cI2 = nid(); cI37 = nid(); cF025 = nid(); cF133 = nid(); cF0 = nid(); cGain = nid(); cTg = nid(); cTb = nid();
    if (addUPtr) uPtrF = nid();
  }

  out.assign(in.begin(), in.begin() + 5);
  out[3] = 0;
  size_t k = 5;
  while (k < in.size() && k != annotEnd) {
    const uint32_t wc = in[k] >> 16, op = in[k] & 0xFFFF;
    const size_t base = out.size();
    out.insert(out.end(), in.begin() + k, in.begin() + k + wc);
    if (op == kOpEntryPoint && in[k + 2] == entry) {
      out[base + 2] = newMain;
      if (addFc) {   // the new Input variable joins the interface (after the existing ids)
        out.push_back(fcVar);
        out[base] = ((uint32_t)(wc + 1) << 16) | kOpEntryPoint;
      }
    }
    if (op == kOpExecutionMode && in[k + 1] == entry) out[base + 1] = newMain;
    k += wc;
  }
  Emit(out, kOpDecorate, {refId, 1, 0});   // SpecId 0 (the game's alpha reference, as in PatchAlphaTest)
  if (addFc) Emit(out, kOpDecorate, {fcVar, 11, 15});
  while (k < firstFn) { const uint32_t wc = in[k] >> 16; out.insert(out.end(), in.begin() + k, in.begin() + k + wc); k += wc; }
  if (addBool) Emit(out, kOpTypeBool, {boolT});
  if (addPtr) Emit(out, kOpTypePointer, {inPtr, 1, vecT});
  if (addFc) Emit(out, kOpVariable, {inPtr, fcVar, 1});
  if (addUPtr) Emit(out, kOpTypePointer, {uPtrF, 2, floatT});
  Emit(out, kOpSpecConstant, {floatT, refId, 0});
  Emit(out, kOpConstant, {floatT, cHalf, FBits(0.5f)});
  Emit(out, kOpConstant, {floatT, cEps, FBits(1.0f / 512.0f)});
  Emit(out, kOpConstant, {floatT, cOne, FBits(1.0f)});
  Emit(out, kOpConstant, {floatT, cK1, FBits(0.06711056f)});
  Emit(out, kOpConstant, {floatT, cK2, FBits(0.00583715f)});
  Emit(out, kOpConstant, {floatT, cK3, FBits(52.9829189f)});
  if (covVar) {
    static const float q = [] { const char* e = getenv("SNAP_FOLIAGE_STABLE_K"); const float v = e && *e ? (float)atof(e) : 0.25f; return v < 0 ? 0.0f : v > 2 ? 2.0f : v; }();
    Emit(out, kOpConstant, {floatT, cCovQ, FBits(q)});
    Emit(out, kOpConstant, {floatT, cCovZ, FBits(0.0f)});
  }
  if (doLeaf) {
    Emit(out, kOpConstant, {intT, cI0, 0});
    Emit(out, kOpConstant, {intT, cI1, 1});
    Emit(out, kOpConstant, {intT, cI2, 2});
    Emit(out, kOpConstant, {intT, cI37, 37});
    Emit(out, kOpConstant, {floatT, cF025, FBits(leafLvl > 8 ? -1.0f : -0.35f)});
    Emit(out, kOpConstant, {floatT, cF133, FBits(leafLvl > 8 ? 0.5f : 1.0f / 1.35f)});
    Emit(out, kOpConstant, {floatT, cF0, FBits(0.0f)});
    Emit(out, kOpConstant, {floatT, cGain, FBits(leafLvl > 8 ? 20.0f : 2.5f * (float)leafLvl / 8.0f)});
    Emit(out, kOpConstant, {floatT, cTg, FBits(leafLvl > 8 ? 0.0f : 0.9f)});
    Emit(out, kOpConstant, {floatT, cTb, FBits(leafLvl > 8 ? 0.0f : 0.35f)});
  }
  out.insert(out.end(), in.begin() + k, in.end());
  // wrapper
  Emit(out, kOpFunction, {voidT, newMain, 0, fnT});
  Emit(out, kOpLabel, {lbl});
  Emit(out, kOpFunctionCall, {voidT, callId, entry});
  Emit(out, kOpLoad, {vecT, loadId, var0});
  Emit(out, kOpCompositeExtract, {floatT, aId, loadId, 3});
  uint32_t aUse = aId;
  if (covVar) {
    const uint32_t lod = nid(), gt = nid(), lm = nid(), t = nid(), sc = nid(), a2 = nid();
    Emit(out, kOpLoad, {floatT, lod, covVar});
    Emit(out, kOpFOrdGreaterThan, {boolT, gt, lod, cCovZ});
    Emit(out, 169 /*OpSelect*/, {floatT, lm, gt, lod, cCovZ});
    Emit(out, kOpFMul, {floatT, t, lm, cCovQ});
    Emit(out, kOpFAdd, {floatT, sc, t, cOne});
    Emit(out, kOpFMul, {floatT, a2, aId, sc});
    aUse = a2;
  }
  uint32_t colId = loadId;   // the colour vector the final store writes (leaf term applied below)
  auto emitLeaf = [&]() {
    uint32_t pc[6], ld[6];
    for (int n = 0; n < 6; n++) { pc[n] = nid(); ld[n] = nid(); }
    const uint32_t idxs[6][3] = {{cI0, cI37, cI0}, {cI0, cI37, cI1}, {cI0, cI37, cI2}, {cI0, cI1, cI0}, {cI0, cI1, cI1}, {cI0, cI1, cI2}};
    for (int n = 0; n < 6; n++) {
      Emit(out, 65 /*OpAccessChain*/, {uPtrF, pc[n], n < 3 ? cam : light, idxs[n][0], idxs[n][1], idxs[n][2]});
      Emit(out, kOpLoad, {floatT, ld[n], pc[n]});
    }
    const uint32_t m1 = nid(), m2 = nid(), m3 = nid(), s1 = nid(), dd = nid(), t0 = nid(), t1 = nid(), tc = nid(), t2 = nid(), mm = nid();
    Emit(out, kOpFMul, {floatT, m1, ld[0], ld[3]});
    Emit(out, kOpFMul, {floatT, m2, ld[1], ld[4]});
    Emit(out, kOpFMul, {floatT, m3, ld[2], ld[5]});
    Emit(out, kOpFAdd, {floatT, s1, m1, m2});
    Emit(out, kOpFAdd, {floatT, dd, s1, m3});
    uint32_t dUse = dd;
    if (!leafFlip) { dUse = nid(); Emit(out, 127 /*OpFNegate*/, {floatT, dUse, dd}); }   // c5[1] is the direction the light travels; toLight = -c5[1]
    Emit(out, kOpFSub, {floatT, t0, dUse, cF025});
    Emit(out, kOpFMul, {floatT, t1, t0, cF133});
    Emit(out, 12 /*OpExtInst*/, {floatT, tc, glslSet, 43 /*FClamp*/, t1, cF0, cOne});
    Emit(out, kOpFMul, {floatT, t2, tc, tc});
    Emit(out, kOpFMul, {floatT, mm, t2, cGain});
    uint32_t cur = colId;
    const uint32_t tint[3] = {cOne, cTg, cTb};
    for (uint32_t c = 0; c < 3; c++) {
      const uint32_t ch = nid(), g = nid(), add = nid(), sum = nid(), nv = nid();
      Emit(out, kOpCompositeExtract, {floatT, ch, cur, c});
      Emit(out, kOpFMul, {floatT, g, mm, tint[c]});
      Emit(out, kOpFMul, {floatT, add, ch, g});
      Emit(out, kOpFAdd, {floatT, sum, ch, add});
      Emit(out, kOpCompositeInsert, {vecT, nv, sum, cur, c});
      cur = nv;
    }
    colId = cur;
  };
  if (hash) {
    if (func == 5 || func == 7) Emit(out, kOpFSub, {floatT, dId, aUse, refId}); else Emit(out, kOpFSub, {floatT, dId, refId, aUse});
    Emit(out, kOpFwidth, {floatT, wId, aUse});
    const uint32_t wsId = nid();   // half width ramp: fewer dithered pixels, the edge still gets a graded step
    Emit(out, kOpFMul, {floatT, wsId, wId, cHalf});
    Emit(out, kOpFAdd, {floatT, weId, wsId, cEps});
    Emit(out, kOpFDiv, {floatT, qId, dId, weId});
    Emit(out, kOpFAdd, {floatT, sId, qId, cHalf});
    if (a2c) {
      if (doLeaf) emitLeaf();
      Emit(out, kOpCompositeInsert, {vecT, nvId, sId, colId, 3});
      Emit(out, kOpStore, {var0, nvId});
      Emit(out, kOpReturn, {});
    } else {
      Emit(out, kOpLoad, {vecT, fcId, fcVar});
      Emit(out, kOpCompositeExtract, {floatT, xId, fcId, 0});
      Emit(out, kOpCompositeExtract, {floatT, yId, fcId, 1});
      Emit(out, kOpFMul, {floatT, txId, xId, cK1});
      Emit(out, kOpFMul, {floatT, tyId, yId, cK2});
      Emit(out, kOpFAdd, {floatT, tId, txId, tyId});
      Emit(out, kOpFRem, {floatT, f1Id, tId, cOne});
      Emit(out, kOpFMul, {floatT, mId, f1Id, cK3});
      Emit(out, kOpFRem, {floatT, hId, mId, cOne});
      Emit(out, kOpFOrdGreaterThan, {boolT, condId, sId, hId});
      Emit(out, kOpSelectionMerge, {mergeLbl, 0});
      Emit(out, kOpBranchConditional, {condId, mergeLbl, killLbl});
      Emit(out, kOpLabel, {killLbl});
      Emit(out, kOpKill, {});
      Emit(out, kOpLabel, {mergeLbl});
      if (doLeaf) { emitLeaf(); Emit(out, kOpStore, {var0, colId}); }
      Emit(out, kOpReturn, {});
    }
  } else {   // leaf term only: the game's own hard test, then the term
    const uint32_t cmp = func == 5 ? (uint32_t)kOpFOrdGreaterThan : func == 7 ? 190u : func == 2 ? 184u : 188u;
    Emit(out, cmp, {boolT, condId, aUse, refId});
    Emit(out, kOpSelectionMerge, {mergeLbl, 0});
    Emit(out, kOpBranchConditional, {condId, mergeLbl, killLbl});
    Emit(out, kOpLabel, {killLbl});
    Emit(out, kOpKill, {});
    Emit(out, kOpLabel, {mergeLbl});
    if (doLeaf) { emitLeaf(); Emit(out, kOpStore, {var0, colId}); }
    Emit(out, kOpReturn, {});
  }
  Emit(out, kOpFunctionEnd, {});
  out[3] = next;
  return true;
}

// Foliage stability: OpImageQueryLod after the first 2D implicit-lod sample, its .x (the mip level read, after clamping to the texture's mips and the
// sampler's bias) stored into a new Private float (initialised to 0, so a module whose sample sits in a branch that did not run reads level 0).
bool PatchCaptureAlphaLod(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, uint32_t* lodVar) {
  if (in.size() < 6 || in[0] != 0x07230203) return false;
  const uint32_t version = in[1];
  uint32_t floatT = 0, vec2T = 0, privPtr = 0, firstFn = 0, entryAt = 0;
  bool hasCap = false;
  std::map<uint32_t, uint32_t> typeOf;
  std::map<uint32_t, uint32_t> sampledImgT, img2d;
  std::vector<std::pair<size_t, uint32_t>> cands;
  std::set<uint32_t> alphaRead;
  size_t sampleAt = 0;
  size_t i = 5;
  while (i < in.size()) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return false;
    if (op == kOpCompositeExtract && wc == 5 && in[i + 4] == 3) alphaRead.insert(in[i + 3]);
    if (op == 79 && wc >= 6) for (size_t c = 5; c < wc; c++) { if (in[i + c] == 3) alphaRead.insert(in[i + 3]); if (in[i + c] == 7) alphaRead.insert(in[i + 4]); }
    switch (op) {
      case 17: if (in[i + 1] == 50) hasCap = true; break;   // OpCapability ImageQuery
      case kOpEntryPoint: if (in[i + 1] == 4 && !entryAt) entryAt = (uint32_t)i; break;
      case kOpTypeFloat: if (in[i + 2] == 32 && !floatT) floatT = in[i + 1]; break;
      case kOpTypeVector: if (floatT && in[i + 2] == floatT && in[i + 3] == 2 && !vec2T) vec2T = in[i + 1]; break;
      case kOpTypePointer: if (floatT && in[i + 2] == 6 && in[i + 3] == floatT && !privPtr) privPtr = in[i + 1]; break;
      case 25: img2d[in[i + 1]] = (in[i + 3] == 1 && in[i + 5] == 0 && in[i + 6] == 0) ? 1u : 0u; break;   // OpTypeImage: Dim 2D, Arrayed 0, MS 0
      case 27: sampledImgT[in[i + 1]] = in[i + 2]; break;   // OpTypeSampledImage
      case kOpFunction: if (!firstFn) firstFn = (uint32_t)i; break;
      case kOpLoad: case 79: case 80: case kOpCompositeExtract: case 86: case kOpFAdd: case kOpFSub: case kOpFMul: case 142: case 12: case 169:
        if (firstFn && wc >= 3) typeOf[in[i + 2]] = in[i + 1];
        break;
      case 87:   // OpImageSampleImplicitLod
        if (firstFn && wc >= 5) {
          auto si = typeOf.find(in[i + 3]); auto co = typeOf.find(in[i + 4]);
          if (si != typeOf.end() && co != typeOf.end() && vec2T && co->second == vec2T) {
            auto st = sampledImgT.find(si->second);
            if (st != sampledImgT.end()) { auto im = img2d.find(st->second); if (im != img2d.end() && im->second) cands.push_back({i, in[i + 2]}); }
          }
        }
        break;
      default: break;
    }
    i += wc;
  }
  for (auto& c : cands) if (alphaRead.count(c.second)) { sampleAt = c.first; break; }
  if (!sampleAt && !cands.empty()) sampleAt = cands[0].first;
  if (!floatT || !firstFn || !sampleAt || !entryAt) return false;
  uint32_t next = in[3];
  const bool addVec2 = !vec2T; (void)addVec2;
  const bool addPtr = !privPtr;
  if (addPtr) privPtr = next++;
  const uint32_t c0 = next++, var = next++, q = next++, l = next++;
  out.assign(in.begin(), in.begin() + 5);
  if (!hasCap) Emit(out, 17, {50});
  size_t k = 5;
  while (k < firstFn) {
    const uint32_t wc = in[k] >> 16;
    const size_t base = out.size();
    out.insert(out.end(), in.begin() + k, in.begin() + k + wc);
    if (k == entryAt && version >= 0x10400) { out.push_back(var); out[base] = ((uint32_t)(wc + 1) << 16) | kOpEntryPoint; }
    k += wc;
  }
  if (addPtr) Emit(out, kOpTypePointer, {privPtr, 6, floatT});
  Emit(out, kOpConstant, {floatT, c0, FBits(0.0f)});
  Emit(out, kOpVariable, {privPtr, var, 6, c0});
  while (k < in.size()) {
    const uint32_t wc = in[k] >> 16;
    out.insert(out.end(), in.begin() + k, in.begin() + k + wc);
    if (k == sampleAt) {
      Emit(out, 105 /*OpImageQueryLod*/, {vec2T, q, in[k + 3], in[k + 4]});
      Emit(out, kOpCompositeExtract, {floatT, l, q, 0});
      Emit(out, kOpStore, {var, l});
    }
    k += wc;
  }
  out[3] = next;
  *lodVar = var;
  return true;
}

// ---- wind sway (vertex wrapper) --------------------------------------------------------------------------------------------
// The translated vertex module is wrapped like the fragment ones: the wrapper runs the original main, reads gl_Position back, rebuilds the world position from the
// clip position with the per view buffer (set 0 binding 4 = PerViewCBuffer c3: viewProjection at vec4 16..19, its inverse at vec4 20..23, game timer at vec4 42 .z),
// and adds a small wind displacement in world space (x, z only) as viewProjection * d. Only positions change, so every output the shader writes besides gl_Position is
// untouched. Needs no knowledge of the shader's own inputs. The amplitude (world units) is specialization constant 0.
// Returns false when the module has no gl_Position block member, no set 0 binding 4 uniform array or no GLSL.std450 import (the caller keeps the game vertex shader).
bool PatchVertexSway(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
  if (in.size() < 6 || in[0] != 0x07230203) return false;
  const uint32_t bound = in[3];
  uint32_t entry = 0, posStruct = 0, posVar = 0, cb3 = 0, voidT = 0, fnT = 0, firstFn = 0, annotEnd = 0, glslSet = 0, intT = 0, vecT = 0, floatT = 0, uPtrF = 0, oPtrV = 0;
  struct Ptr { uint32_t id, sc, pointee; };
  std::vector<Ptr> ptrs;
  struct Vec { uint32_t id, comp, n; };
  std::vector<Vec> vecs;
  std::vector<uint32_t> floats;
  std::vector<std::pair<uint32_t, uint32_t>> varSc, bindOf, setOf;
  size_t i = 5;
  while (i < in.size()) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return false;
    switch (op) {
      case kOpEntryPoint: if (in[i + 1] == 0 && !entry) entry = in[i + 2]; break;
      case 11: if (wc >= 4 && !strncmp((const char*)&in[i + 2], "GLSL.std.450", 12)) glslSet = in[i + 1]; break;
      case kOpDecorate:
        if (wc >= 4 && in[i + 2] == 33) bindOf.push_back({in[i + 1], in[i + 3]});
        if (wc >= 4 && in[i + 2] == 34) setOf.push_back({in[i + 1], in[i + 3]});
        annotEnd = (uint32_t)(i + wc); break;
      case kOpMemberDecorate:
        if (wc == 5 && in[i + 2] == 0 && in[i + 3] == 11 && in[i + 4] == 0) posStruct = in[i + 1];   // BuiltIn Position on member 0
        annotEnd = (uint32_t)(i + wc); break;
      case kOpDecorationGroup: case kOpGroupDecorate: case kOpGroupMemberDecorate: annotEnd = (uint32_t)(i + wc); break;
      case 21: if (wc >= 4 && in[i + 2] == 32 && !intT) intT = in[i + 1]; break;   // OpTypeInt 32
      case kOpTypeFloat: if (in[i + 2] == 32) floats.push_back(in[i + 1]); break;
      case kOpTypeVector: vecs.push_back({in[i + 1], in[i + 2], in[i + 3]}); break;
      case kOpTypePointer: ptrs.push_back({in[i + 1], in[i + 2], in[i + 3]}); break;
      case kOpVariable: varSc.push_back({in[i + 2], in[i + 1]}); break;
      case kOpFunction: if (!firstFn) firstFn = (uint32_t)i; if (in[i + 2] == entry) { fnT = in[i + 4]; voidT = in[i + 1]; } break;
      default: break;
    }
    i += wc;
  }
  if (!entry || !posStruct || !glslSet || !intT || !fnT || !firstFn || !annotEnd) return false;
  auto lookup = [](const std::vector<std::pair<uint32_t, uint32_t>>& m, uint32_t v, uint32_t& val) { for (auto& e : m) if (e.first == v) { val = e.second; return true; } return false; };
  for (auto& v : vecs) if (v.n == 4 && !vecT) for (uint32_t f : floats) if (f == v.comp) { vecT = v.id; floatT = f; }
  if (!vecT) return false;
  for (auto& v : varSc) for (auto& p : ptrs) if (p.id == v.second && p.sc == 3 && p.pointee == posStruct) posVar = v.first;
  for (auto& b : bindOf) {
    uint32_t st = 0, pt = 0;
    if (b.second != 4 || !lookup(setOf, b.first, st) || st != 0 || !lookup(varSc, b.first, pt)) continue;
    for (auto& p : ptrs) if (p.id == pt && p.sc == 2) cb3 = b.first;
  }
  if (!posVar || !cb3) return false;
  for (auto& p : ptrs) { if (p.sc == 3 && p.pointee == vecT) oPtrV = p.id; if (p.sc == 2 && p.pointee == floatT) uPtrF = p.id; }
  uint32_t next = bound;
  auto nid = [&]() { return next++; };
  const bool addO = !oPtrV, addU = !uPtrF;
  if (addO) oPtrV = nid();
  if (addU) uPtrF = nid();
  const uint32_t newMain = nid(), lbl = nid(), callId = nid(), ampId = nid(), offId = nid(), tsId = nid(), zsId = nid();
  std::vector<uint32_t> consts, body;
  std::map<uint32_t, uint32_t> fcs, ics;
  auto cf = [&](float v) { const uint32_t b = FBits(v); auto it = fcs.find(b); if (it != fcs.end()) return it->second; const uint32_t id = nid(); Emit(consts, kOpConstant, {floatT, id, b}); fcs[b] = id; return id; };
  auto ci = [&](uint32_t v) { auto it = ics.find(v); if (it != ics.end()) return it->second; const uint32_t id = nid(); Emit(consts, kOpConstant, {intT, id, v}); ics[v] = id; return id; };
  auto ld = [&](uint32_t idx, uint32_t comp) { const uint32_t pc = nid(), l = nid(); Emit(body, 65 /*OpAccessChain*/, {uPtrF, pc, cb3, ci(0), ci(idx), ci(comp)}); Emit(body, kOpLoad, {floatT, l, pc}); return l; };
  auto bin = [&](uint32_t op, uint32_t a, uint32_t b) { const uint32_t r = nid(); Emit(body, op, {floatT, r, a, b}); return r; };
  auto mul = [&](uint32_t a, uint32_t b) { return bin(kOpFMul, a, b); };
  auto add = [&](uint32_t a, uint32_t b) { return bin(kOpFAdd, a, b); };
  auto sub = [&](uint32_t a, uint32_t b) { return bin(kOpFSub, a, b); };
  auto sine = [&](uint32_t a) { const uint32_t r = nid(); Emit(body, 12 /*OpExtInst*/, {floatT, r, glslSet, 13 /*Sin*/, a}); return r; };
  auto mad = [&](float k, uint32_t a, uint32_t b) { return add(mul(cf(k), a), b); };   // k * a + b

  Emit(body, kOpFunction, {voidT, newMain, 0, fnT});
  Emit(body, kOpLabel, {lbl});
  Emit(body, kOpFunctionCall, {voidT, callId, entry});
  const uint32_t pp = nid(), clip = nid();
  Emit(body, 65, {oPtrV, pp, posVar, ci(0)});
  Emit(body, kOpLoad, {vecT, clip, pp});
  uint32_t cl[4];
  for (uint32_t k = 0; k < 4; k++) { cl[k] = nid(); Emit(body, kOpCompositeExtract, {floatT, cl[k], clip, k}); }
  uint32_t w[4] = {0, 0, 0, 0};   // world = viewProjectionInverse * clip, components x, z, w
  for (uint32_t k : {0u, 2u, 3u}) {
    uint32_t s = mul(ld(20, k), cl[0]);
    for (uint32_t j = 1; j < 4; j++) s = add(s, mul(ld(20 + j, k), cl[j]));
    w[k] = s;
  }
  const uint32_t px = bin(kOpFDiv, w[0], w[3]), pz = bin(kOpFDiv, w[2], w[3]);
  const uint32_t t = add(mul(ld(42, 3), tsId), offId);   // absolute timer (0x2ac) plus the debug phase offset (spec constant 1)
  const uint32_t p = add(mul(cf(0.8f), px), mul(cf(0.6f), pz));     // distance along the wind
  const uint32_t q = add(mul(cf(-0.6f), px), mul(cf(0.8f), pz));    // across the wind
  const uint32_t gust = mad(0.5f, sine(sub(mul(cf(0.23f), t), mul(cf(0.045f), p))), cf(0.5f));            // 0..1, slow gusts travelling downwind
  const uint32_t f1 = sine(add(add(mul(cf(2.3f), t), mul(cf(0.9f), p)), mul(cf(0.4f), q)));
  const uint32_t f2 = sine(add(sub(mul(cf(3.7f), t), mul(cf(0.5f), p)), mul(cf(1.3f), q)));
  const uint32_t fl = add(mul(cf(0.6f), f1), mul(cf(0.4f), f2));                                           // flutter, -1..1
  const uint32_t f3 = sine(add(mul(cf(1.9f), t), mul(cf(0.7f), q)));
  const uint32_t along = mul(ampId, add(mul(cf(0.5f), gust), mul(cf(0.5f), fl)));                          // lean with the gust plus flutter
  const uint32_t across = mul(ampId, mul(cf(0.3f), f3));
  const uint32_t dx = add(mul(cf(0.8f), along), mul(cf(-0.6f), across));
  const uint32_t dz = add(mul(cf(0.6f), along), mul(cf(0.8f), across));
  uint32_t nc[4];
  for (uint32_t k = 0; k < 4; k++) nc[k] = add(cl[k], add(mul(dx, ld(16, k)), mul(dz, ld(18, k))));    // clip + viewProjection * (dx, 0, dz, 0)
  nc[2] = sub(nc[2], mul(zsId, nc[3]));   // debug depth slack (spec constant 3, pre-pass only): reverse Z, so a smaller z/w is farther and the colour pass GEQUAL keeps passing
  const uint32_t nv = nid();
  Emit(body, 80 /*OpCompositeConstruct*/, {vecT, nv, nc[0], nc[1], nc[2], nc[3]});
  Emit(body, kOpStore, {pp, nv});
  Emit(body, kOpReturn, {});
  Emit(body, kOpFunctionEnd, {});

  out.assign(in.begin(), in.begin() + 5);
  size_t k = 5;
  while (k < in.size() && k != annotEnd) {
    const uint32_t wc = in[k] >> 16, op = in[k] & 0xFFFF;
    const size_t base = out.size();
    out.insert(out.end(), in.begin() + k, in.begin() + k + wc);
    if (op == kOpEntryPoint && in[k + 2] == entry) out[base + 2] = newMain;
    if (op == kOpExecutionMode && in[k + 1] == entry) out[base + 1] = newMain;
    k += wc;
  }
  Emit(out, kOpDecorate, {ampId, 1, 0});   // SpecId 0: amplitude in world units
  Emit(out, kOpDecorate, {tsId, 1, 2});   // SpecId 2: timer scale (1 normally, 0 = static field, debug)
  Emit(out, kOpDecorate, {zsId, 1, 3});   // SpecId 3: depth slack as a fraction of w (0 normally)
  Emit(out, kOpDecorate, {offId, 1, 1});   // SpecId 1: time offset in seconds (debug phase, 0 normally)
  while (k < firstFn) { const uint32_t wc = in[k] >> 16; out.insert(out.end(), in.begin() + k, in.begin() + k + wc); k += wc; }
  if (addO) Emit(out, kOpTypePointer, {oPtrV, 3, vecT});
  if (addU) Emit(out, kOpTypePointer, {uPtrF, 2, floatT});
  Emit(out, kOpSpecConstant, {floatT, ampId, FBits(0.05f)});
  Emit(out, kOpSpecConstant, {floatT, offId, FBits(0.0f)});
  Emit(out, kOpSpecConstant, {floatT, tsId, FBits(1.0f)});
  Emit(out, kOpSpecConstant, {floatT, zsId, FBits(0.0f)});
  out.insert(out.end(), consts.begin(), consts.end());
  out.insert(out.end(), in.begin() + k, in.end());
  out.insert(out.end(), body.begin(), body.end());
  out[3] = next;
  return true;
}

// One patched module per translated vertex shader (the amplitude is a specialization constant, so every level shares it). Null when the shader does not qualify.
VkShaderModule FoliageSwayModule(Host* h, const StageProg& vs) {
  static std::mutex mu;
  static std::map<uint64_t, VkShaderModule> mods;
  if (vs.spv.empty()) return VK_NULL_HANDLE;
  uint64_t key = 1469598103934665603ull;
  for (uint32_t w : vs.spv) { key ^= w; key *= 1099511628211ull; }
  std::lock_guard<std::mutex> lk(mu);
  auto it = mods.find(key);
  if (it != mods.end()) return it->second;
  VkShaderModule m = VK_NULL_HANDLE;
  std::vector<uint32_t> out;
  if (PatchVertexSway(vs.spv, out)) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = out.size() * 4; mi.pCode = out.data();
    if (vkCreateShaderModule(h->device, &mi, nullptr, &m) == VK_SUCCESS) { std::lock_guard<std::mutex> pl(h->pipeMutex); h->variantMods.push_back(m); }
    else { m = VK_NULL_HANDLE; fprintf(stderr, "[foliage] wind sway: vkCreateShaderModule failed for vertex program %.12s\n", vs.key); }
  } else {
    static int n; if (n++ < 6) fprintf(stderr, "[foliage] wind sway: vertex program %.12s has no gl_Position block / per-view buffer: not swayed\n", vs.key);
  }
  mods[key] = m;
  return m;
}

bool FoliageSwayKeepSpv() { return FoliageSwayStrength() > 0.0f || dev_on(); }

// Pipeline bits for one draw (PipeDesc.pad2: bit 0 = hashed alpha edge, bit 1 = leaf debug flip, bits 4..7 = leaf gain level 0..8, 9 = debug paint, bits 17..20 = wind sway level 0..8; 0 = the game's own hard alpha test). Only
// opaque alpha-tested draws into the RGBA16F scene colour with a depth test qualify (blended particles, shadow casters, UI, the photo/Evaluation
// targets and 8 bit targets differ), and no photo frame does. The bits are part of the pipeline-key bytes, so the persisted cache stays valid for 0.
uint32_t FoliagePipeBits(int alphaFunc, float alphaRef, int32_t colorVkFmt, int32_t depthVkFmt, bool depthTest, int nColor, bool blend0, int depthFunc, bool depthWrite) {
  static const bool swayAll = getenv("SNAP_WIND_ALL") != nullptr;   // debug: sway every opaque scene draw (alpha func ALWAYS included), to test the mixed swayed / unswayed depth theory
  if (depthVkFmt == 0 || !depthTest || blend0) return 0;
  if (alphaFunc == 0 && !swayAll) return 0;
  if (alphaFunc != 0 && alphaFunc != 2 && alphaFunc != 4 && alphaFunc != 5 && alphaFunc != 7) return 0;
  if (nColor < 1) {   // the scene's colour-less depth pre-pass (reverse Z, GEQUAL, writes depth): the colour pass tests against its depth with GEQUAL, so it must move with the sway or the leaves z-fight themselves
    if (!depthWrite || depthFunc != 7 || FxIsPhotoFrame()) return 0;
    const int sw = (int)(FoliageSwayStrength() * 8.0f + 0.5f);
    const bool pre = alphaFunc != 0 && alphaRef >= 0.15f;
    const uint32_t edge = (pre && FoliageAaEnabled() && FoliageAaPrepass() ? 1u : 0u) | (pre && FoliageStableEnabled() ? 4u : 0u);
    if (edge) FxNoteRan();
    return (sw > 0 ? (((uint32_t)sw << 17) | ((uint32_t)FoliageSwayPhase() << 21) | (FoliageSwayZero() ? 1u << 24 : 0u) | (1u << 25)) : 0u) | edge;
  }
  if (colorVkFmt != 97 /* VK_FORMAT_R16G16B16A16_SFLOAT */) return 0;
  if (alphaFunc == 0) {   // SNAP_WIND_ALL only: sway bits, no edge or leaf effects
    if (FxIsPhotoFrame()) return 0;
    const int sw = (int)(FoliageSwayStrength() * 8.0f + 0.5f);
    return sw > 0 ? (((uint32_t)sw << 17) | ((uint32_t)FoliageSwayPhase() << 21) | (FoliageSwayZero() ? 1u << 24 : 0u)) : 0u;
  }
  if (alphaRef < 0.15f) return 0;
  const bool aa = FoliageAaEnabled();
  const int dbg = FoliageLeafDebugFlags();
  const int leaf = (dbg & 32) ? 9 : (int)(FoliageLeafStrength() * 8.0f + 0.5f);
  const int sway = (int)(FoliageSwayStrength() * 8.0f + 0.5f);   // 0..8: wind sway level, pad2 bits 17..20
  const bool cov = FoliageStableEnabled();
  if (!aa && leaf <= 0 && sway <= 0 && !cov) return 0;
  static std::atomic<int> s_photoKept{0}, s_ran{0};
  if (FxIsPhotoFrame()) {
    const int n = ++s_photoKept;
    if (n == 1 || n % 500 == 0) fprintf(stderr, "[foliage] photo frame: %d qualifying alpha-tested draws kept the game's own pipeline so far, %d variant draws on normal frames\n", n, s_ran.load());
    return 0;
  }
  ++s_ran;
  FxNoteRan();   // an effect variant is drawing this frame (A/B capture gating counters)
  return (aa ? 1u : 0u) | (cov ? 4u : 0u) | ((uint32_t)std::min(leaf, 9) << 4) | ((dbg & 16) ? 2u : 0u) | ((uint32_t)sway << 17) | (sway > 0 ? ((uint32_t)FoliageSwayPhase() << 21) | (FoliageSwayZero() ? 1u << 24 : 0u) : 0u);   // bit 1 = debug flip (bits 8..16 belong to the parallax rows, 17..20 the wind sway level)
}

namespace {
std::mutex g_lblMu;
std::map<const void*, char> g_lbl;
}
float FoliageParticleBoost(const void* tex, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t* rel, int n) {
  if (w != 960 || h != 540 || (fmt != 0x29 && fmt != 0x34)) return 1.0f;
  char lab = 0;
  for (int i = 0; i < n; i++) {
    if (rel[i] == 0x5be358 || rel[i] == 0x5be378) lab = 'A';
    else if (rel[i] == 0x5bfde8 || rel[i] == 0x5bfe08) lab = 'B';
    else if (rel[i] == 0x5c09ac) lab = 'C';
  }
  if (!lab) return 1.0f;
  { std::lock_guard<std::mutex> g(g_lblMu); g_lbl[tex] = lab; }
  static const int sel = [] { const char* e = getenv("SNAP_SOFT_PARTICLES_SEL"); return e && *e ? atoi(e) : 6; }();   // bit 0 = A, 1 = B, 2 = C: the particle pass draws into C + the depth of B (B's colour is the downsample's own output)
  if (!SoftParticlesEnabled() || !((lab == 'A' ? 1 : lab == 'B' ? 2 : 4) & sel)) return 1.0f;
  return 2.0f;
}
float FoliageParticleBoostLive(const void* tex, float created) {
  char lab;
  { std::lock_guard<std::mutex> g(g_lblMu); auto it = g_lbl.find(tex); if (it == g_lbl.end()) return created; lab = it->second; }
  static const int sel = [] { const char* e = getenv("SNAP_SOFT_PARTICLES_SEL"); return e && *e ? atoi(e) : 6; }();
  return SoftParticlesEnabled() && ((lab == 'A' ? 1 : lab == 'B' ? 2 : 4) & sel) ? 2.0f : 1.0f;
}
void FoliageTextureGone(const void* tex) { std::lock_guard<std::mutex> g(g_lblMu); g_lbl.erase(tex); }
char FoliageTexLabel(const void* tex) {
  if (!tex) return 0;
  std::lock_guard<std::mutex> g(g_lblMu);
  auto it = g_lbl.find(tex);
  return it == g_lbl.end() ? 0 : it->second;
}

extern "C" float snap_route_ride_progress();   // runtime/hle/input_core.cpp

// SNAP_PARTICLE_CENSUS=<file>: one line per present that drew particles, tagged with the rail progress (needs SNAP_ROUTE_LOG=1 for the progress): draws into the
// half-resolution particle layer (soft particles' target, more than a quad) and blended, depth-tested, no-depth-write draws into the scene colour (sprites).
// "vtx" = vertices x instances, a proxy for how much is on screen.
static void ParticleCensus(const FoliageDrawInfo& d) {
  static const char* path = getenv("SNAP_PARTICLE_CENSUS");
  if (!path || !*path) return;
  static std::mutex mu;
  static FILE* f = fopen(path, "w");
  static int cur = -1, layerN = 0, spriteN = 0;
  static long long layerV = 0, spriteV = 0;
  std::lock_guard<std::mutex> g(mu);
  if (d.present != cur) {
    if (f && cur >= 0 && (layerN || spriteN)) { fprintf(f, "p%d ride=%.2f layer=%d/%lld sprite=%d/%lld\n", cur, snap_route_ride_progress(), layerN, layerV, spriteN, spriteV); fflush(f); }
    cur = d.present; layerN = spriteN = 0; layerV = spriteV = 0;
  }
  const long long v = (long long)d.nVerts * (d.nInst > 0 ? d.nInst : 1);
  if ((d.rtLabel == 'A' || d.rtLabel == 'B') && v > 6) { layerN++; layerV += v; }
  else if (d.blend0 && d.depthTest && !d.depthWrite && d.nColor >= 1 && d.rtW >= 800) { spriteN++; spriteV += v; }
}

void FoliageLogDraw(const FoliageDrawInfo& d) {
  ParticleCensus(d);
  static const char* path = getenv("SNAP_FOLIAGE_LOG");
  if (!path || !*path) return;
  static std::mutex mu;
  static FILE* f = fopen(path, "w");
  static std::set<std::string> seen;
  char buf[900]; int n = snprintf(buf, sizeof buf, "rtl=%c dsl=%c vs=%s fs=%s af=%d ref=%.3f nc=%d rt0=f%x %ux%u s%u ds=f%x blend=%d(%d,%d,%d,%d) dw=%d dt=%d tex:", d.rtLabel ? d.rtLabel : '-', d.dsLabel ? d.dsLabel : '-', d.vs, d.fs, d.alphaFunc, d.alphaRef, d.nColor, d.colorFmt0,
                                  d.rtW, d.rtH, d.rtSamples, d.depthFmt, (int)d.blend0, d.srcRgb, d.dstRgb, d.srcA, d.dstA, (int)d.depthWrite, (int)d.depthTest);
  for (int i = 0; i < d.nTex && n < 800; i++) n += snprintf(buf + n, sizeof buf - n, " b%d:f%x:%ux%u", d.tex[i].binding, d.tex[i].fmt, d.tex[i].w, d.tex[i].h);
  std::lock_guard<std::mutex> g(mu);
  if (f && seen.insert(buf).second) { fprintf(f, "[p%d n=%d i=%d] %s\n", d.present, d.nVerts, d.nInst, buf); fflush(f); }
}

}  // namespace nvn
