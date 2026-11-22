// SPDX-License-Identifier: MIT
// Pure DSP for the AudioOut core and the device backends (no OS headers, so tests/audio/dsp_test.cpp can include it):
//   Resampler   48 kHz mix -> device rate, linear or windowed-sinc (Kaiser), streaming, any channel count
//   Limiter     1 ms look-ahead peak limiter: transparent below the ceiling, never over it (replaces the hard clip)
//   Compressor  "night mode" dynamic range reducer (reduced / night), switched in and out with a short ramp
//   GainSlew    master gain that moves at a fixed rate per frame, whatever the device block size is
//   fade_in / fade_out  edge fades for the holes an underrun leaves
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace snap_audio {
namespace dsp {

// ---- Resampler -------------------------------------------------------------------------------------------------------
enum class ResampleQuality { Linear, Sinc };

class Resampler {
 public:
  static constexpr int kSincHalf = 24;      // 48 taps
  static constexpr int kPhases = 512;       // fractional-position table, linearly interpolated between phases

  void init(uint32_t inRate, uint32_t outRate, uint32_t channels, ResampleQuality q) {
    inRate_ = inRate; outRate_ = outRate; ch_ = channels ? channels : 1; q_ = q;
    ratio_ = (double)inRate / (double)outRate;
    half_ = q == ResampleQuality::Sinc ? kSincHalf : 1;
    taps_ = half_ * 2;
    table_.clear();
    if (q == ResampleQuality::Sinc) build_table();
    reset();
  }
  bool passthrough() const { return inRate_ == outRate_; }
  void reset() {
    buf_.assign((size_t)half_ * ch_, 0.0f);   // `half_` frames of silent history before the first input frame
    have_ = (size_t)half_;
    pos_ = (double)half_;
  }
  // Writes exactly n output frames (interleaved). pull(float* dst, uint32_t frames) must fill `frames` input frames.
  template <class Pull>
  void produce(float* out, uint32_t n, Pull&& pull) {
    if (!n) return;
    if (passthrough()) { pull(out, n); return; }
    const size_t last = (size_t)(pos_ + (double)(n - 1) * ratio_);        // input index of the last output's i0
    const size_t need = last + (size_t)half_ + 1;                          // frames the last output reads (i0 + half_)
    if (need > have_) {
      const size_t add = need - have_;
      buf_.resize((have_ + add) * ch_);
      pull(buf_.data() + have_ * ch_, (uint32_t)add);
      have_ += add;
    }
    for (uint32_t o = 0; o < n; ++o) {
      const size_t i0 = (size_t)pos_;
      const double frac = pos_ - (double)i0;
      float* dst = out + (size_t)o * ch_;
      if (q_ == ResampleQuality::Linear) {
        const float f = (float)frac;
        const float* a = buf_.data() + i0 * ch_;
        const float* b = a + ch_;
        for (uint32_t c = 0; c < ch_; ++c) dst[c] = a[c] + (b[c] - a[c]) * f;
      } else {
        const double pf = frac * kPhases;
        const int p = (int)pf;
        const float w = (float)(pf - p);
        const float* t0 = table_.data() + (size_t)p * taps_;
        const float* t1 = t0 + taps_;
        const float* src = buf_.data() + (i0 + 1 - (size_t)half_) * ch_;   // first tap: i0 - half_ + 1
        for (uint32_t c = 0; c < ch_; ++c) {
          float acc = 0;
          for (int t = 0; t < taps_; ++t) acc += src[(size_t)t * ch_ + c] * (t0[t] + (t1[t] - t0[t]) * w);
          dst[c] = acc;
        }
      }
      pos_ += ratio_;
    }
    // drop what no later output can read, keeping the history the taps reach back to
    const size_t i0 = (size_t)pos_;
    const size_t keepFrom = i0 + 1 > (size_t)half_ ? i0 + 1 - (size_t)half_ : 0;
    if (keepFrom) {
      buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)(keepFrom * ch_));
      have_ -= keepFrom;
      pos_ -= (double)keepFrom;
    }
  }

 private:
  static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 60; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; if (term < 1e-12 * sum) break; }
    return sum;
  }
  void build_table() {
    const double kPi = 3.14159265358979323846;
    const double cutoff = std::min(1.0, (double)outRate_ / (double)inRate_) * 0.96;   // below the output Nyquist when downsampling
    const double beta = 8.0, i0b = bessel_i0(beta);
    table_.assign((size_t)(kPhases + 1) * taps_, 0.0f);
    for (int p = 0; p <= kPhases; ++p) {
      const double frac = (double)p / kPhases;
      double row[2 * kSincHalf], sum = 0;
      for (int t = 0; t < taps_; ++t) {
        const double d = (double)(t - half_ + 1) - frac;
        const double x = d * cutoff * kPi;
        const double s = std::fabs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
        const double r = d / (double)half_;
        const double win = std::fabs(r) >= 1.0 ? 0.0 : bessel_i0(beta * std::sqrt(1.0 - r * r)) / i0b;
        row[t] = cutoff * s * win;
        sum += row[t];
      }
      for (int t = 0; t < taps_; ++t) table_[(size_t)p * taps_ + t] = (float)(row[t] / sum);   // unity gain at DC for every phase
    }
  }

  uint32_t inRate_ = 48000, outRate_ = 48000, ch_ = 2;
  ResampleQuality q_ = ResampleQuality::Sinc;
  double ratio_ = 1.0, pos_ = 0.0;
  int half_ = kSincHalf, taps_ = 2 * kSincHalf;
  std::vector<float> table_, buf_;
  size_t have_ = 0;
};

// ---- Limiter ---------------------------------------------------------------------------------------------------------
// out[n] = in[n - L] * g[n], L = 1 ms. g[n] is the average of the last L values of a sliding minimum of the per-frame required
// gain over L + 1 frames, so g[n] <= required(in[n - L]): the ceiling holds exactly, and the gain eases in over 1 ms (no zipper
// edge). Release is 100 ms. Below the ceiling g is exactly 1.0 and the signal is only delayed. Non-finite input becomes silence.
class Limiter {
 public:
  static constexpr float kCeiling = 0.98f;
  void init(uint32_t rate, uint32_t channels) {
    ch_ = channels ? channels : 1;
    L_ = std::max<uint32_t>(2, rate / 1000);
    rel_ = 1.0f / (0.1f * (float)rate);
    delay_.assign((size_t)L_ * ch_, 0.0f);
    req_.assign((size_t)L_ + 1, 1.0f);
    mins_.assign((size_t)L_, 1.0f);
    w_ = 0; gOut_ = 1.0f;
  }
  uint32_t channels() const { return ch_; }
  uint32_t delay_frames() const { return L_; }
  void process(float* buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) {
      float* f = buf + (size_t)i * ch_;
      float peak = 0;
      for (uint32_t c = 0; c < ch_; ++c) {
        if (!std::isfinite(f[c])) f[c] = 0.0f;
        peak = std::max(peak, std::fabs(f[c]));
      }
      req_[w_ % (L_ + 1)] = peak > kCeiling ? kCeiling / peak : 1.0f;
      float m = 1.0f;
      for (uint32_t k = 0; k <= L_; ++k) m = std::min(m, req_[k]);
      mins_[w_ % L_] = m;
      float sum = 0;
      for (uint32_t k = 0; k < L_; ++k) sum += mins_[k];
      const float g = sum / (float)L_;
      gOut_ = g < gOut_ ? g : std::min(g, gOut_ + rel_);
      float* d = delay_.data() + (size_t)(w_ % L_) * ch_;
      for (uint32_t c = 0; c < ch_; ++c) { const float x = d[c]; d[c] = f[c]; f[c] = x * gOut_; }
      ++w_;
    }
  }
  float gain() const { return gOut_; }
 private:
  uint32_t ch_ = 2, L_ = 48;
  float rel_ = 1e-4f, gOut_ = 1.0f;
  uint64_t w_ = 0;
  std::vector<float> delay_, req_, mins_;
};

// ---- Compressor (night mode) -------------------------------------------------------------------------------------------
// Feed-forward peak compressor, all channels linked so the stereo image and the surround balance do not move. Over the threshold
// the level rises 1 dB per `ratio` dB; makeup lifts the whole result, so quiet passages get louder and loud ones do not.
enum class Range { Full = 0, Reduced = 1, Night = 2 };

class Compressor {
 public:
  void init(uint32_t rate) {
    rate_ = rate;
    atk_ = 1.0f - std::exp(-1.0f / (0.005f * (float)rate));
    wetStep_ = 1.0f / (0.05f * (float)rate);
    env_ = 0; wet_ = 0;
    set_range(Range::Full);
  }
  void set_range(Range r) {
    range_ = r;
    switch (r) {
      case Range::Reduced: thrDb_ = -22.0f; ratio_ = 2.0f; makeDb_ = 3.0f; rel_ = 0.30f; break;
      case Range::Night: thrDb_ = -30.0f; ratio_ = 4.0f; makeDb_ = 9.0f; rel_ = 0.25f; break;
      default: break;
    }
    relC_ = 1.0f - std::exp(-1.0f / (rel_ * (float)rate_));
  }
  Range range() const { return range_; }
  // Gain in dB for a given envelope level in dBFS (the static curve; tests check it).
  float curve_db(float levelDb) const {
    const float over = levelDb - thrDb_;
    return (over > 0 ? -over * (1.0f - 1.0f / ratio_) : 0.0f) + makeDb_;
  }
  void process(float* buf, uint32_t ch, uint32_t frames) {
    const float target = range_ == Range::Full ? 0.0f : 1.0f;
    if (target == 0.0f && wet_ == 0.0f) { env_ = 0; return; }
    for (uint32_t i = 0; i < frames; ++i) {
      float* f = buf + (size_t)i * ch;
      float peak = 0;
      for (uint32_t c = 0; c < ch; ++c) peak = std::max(peak, std::fabs(f[c]));
      env_ += (peak > env_ ? atk_ : relC_) * (peak - env_);
      const float db = 20.0f * std::log10(std::max(env_, 1e-5f));
      const float g = std::pow(10.0f, curve_db(db) / 20.0f);
      wet_ += wet_ < target ? std::min(wetStep_, target - wet_) : -std::min(wetStep_, wet_ - target);
      const float k = 1.0f + wet_ * (g - 1.0f);
      for (uint32_t c = 0; c < ch; ++c) f[c] *= k;
    }
  }
 private:
  uint32_t rate_ = 48000;
  Range range_ = Range::Full;
  float thrDb_ = 0, ratio_ = 1, makeDb_ = 0, rel_ = 0.25f;
  float atk_ = 0.004f, relC_ = 0.0001f, wetStep_ = 0.0004f, env_ = 0, wet_ = 0;
};

// ---- GainSlew ----------------------------------------------------------------------------------------------------------
// Master volume / mute: the gain moves toward the target by at most 1 / (ms * rate) per frame, so a mute or a slider drag has the
// same smooth 0-to-1 time (default 30 ms) with a 128-frame low-latency block as with a 1056-frame one.
class GainSlew {
 public:
  void init(uint32_t rate, float ms, float start) { step_ = 1.0f / (ms * 0.001f * (float)rate); g_ = start; }
  // Multiplies `frames` interleaved frames by the gain, which slews toward `target`.
  void apply(float* buf, uint32_t ch, uint32_t frames, float target) {
    for (uint32_t i = 0; i < frames; ++i) {
      g_ += g_ < target ? std::min(step_, target - g_) : -std::min(step_, g_ - target);
      float* f = buf + (size_t)i * ch;
      for (uint32_t c = 0; c < ch; ++c) f[c] *= g_;
    }
  }
  float gain() const { return g_; }
 private:
  float step_ = 0.0007f, g_ = 1.0f;
};

// ---- edge fades --------------------------------------------------------------------------------------------------------
// A guest stream that runs dry leaves a hole: the last good frames end on a step (a click) and the data after the hole starts on
// one. Fade the first / last `n` frames of the produced part linearly (2 ms makes a click inaudible).
inline void fade_in(float* buf, uint32_t ch, uint32_t frames, uint32_t n) {
  n = std::min(n, frames);
  for (uint32_t i = 0; i < n; ++i) { const float g = (float)(i + 1) / (float)(n + 1); for (uint32_t c = 0; c < ch; ++c) buf[(size_t)i * ch + c] *= g; }
}
inline void fade_out(float* buf, uint32_t ch, uint32_t frames, uint32_t n) {
  n = std::min(n, frames);
  for (uint32_t i = 0; i < n; ++i) {
    const float g = (float)(i + 1) / (float)(n + 1);
    float* f = buf + (size_t)(frames - 1 - i) * ch;
    for (uint32_t c = 0; c < ch; ++c) f[c] *= g;
  }
}

}  // namespace dsp
}  // namespace snap_audio
