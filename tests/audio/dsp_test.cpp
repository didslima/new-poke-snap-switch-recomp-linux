// Unit test for runtime/audio/audio_dsp.h: resampler quality, limiter ceiling, night-mode compressor, gain slew, underrun fades.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "audio_dsp.h"

using namespace snap_audio::dsp;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const double kPi = 3.14159265358979323846;

// Sine of `hz` at input rate `inRate`, `ch` channels, amplitude `amp`; sample index i is global (so any block split sees the same signal).
struct SineSrc {
  double hz, inRate, amp; uint32_t ch; uint64_t at = 0;
  void operator()(float* d, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i, ++at) for (uint32_t c = 0; c < ch; ++c) d[i * ch + c] = (float)(amp * std::sin(2 * kPi * hz * (double)at / inRate));
  }
};

// Resample `outFrames` frames and return the error energy of channel 0 against the ideal sine, in dB relative to the sine power, after the
// first 400 frames (filter start-up).
static double sine_error_db(ResampleQuality q, double hz, uint32_t inRate, uint32_t outRate, uint32_t outFrames, bool aliasOnly = false) {
  Resampler r; r.init(inRate, outRate, 1, q);
  SineSrc s{hz, (double)inRate, 0.5, 1};
  std::vector<float> out(outFrames);
  r.produce(out.data(), outFrames, [&](float* d, uint32_t n) { s(d, n); });
  double err = 0, sig = 0;
  for (uint32_t i = 400; i < outFrames; ++i) {
    const double ideal = aliasOnly ? 0.0 : 0.5 * std::sin(2 * kPi * hz * (double)i / (double)outRate);
    const double e = out[i] - ideal;
    err += e * e; sig += 0.25 * 0.5;
  }
  return 10.0 * std::log10(err / sig + 1e-30);
}

int main() {
  // ---- resampler: accuracy
  {
    const double sincDown = sine_error_db(ResampleQuality::Sinc, 1000, 48000, 44100, 20000);
    const double linDown = sine_error_db(ResampleQuality::Linear, 1000, 48000, 44100, 20000);
    const double sincUp = sine_error_db(ResampleQuality::Sinc, 1000, 48000, 96000, 40000);
    const double linUp = sine_error_db(ResampleQuality::Linear, 1000, 48000, 96000, 40000);
    std::printf("1 kHz error vs ideal: 48->44.1 sinc %.1f dB linear %.1f dB; 48->96 sinc %.1f dB linear %.1f dB\n", sincDown, linDown, sincUp, linUp);
    CHECK(sincDown < -70.0, "sinc 48->44.1 error %.1f dB", sincDown);
    CHECK(sincUp < -70.0, "sinc 48->96 error %.1f dB", sincUp);
    CHECK(linDown > sincDown + 20.0 && linUp > sincUp + 20.0, "sinc must beat linear by 20 dB");
    // high tone: 15 kHz is where linear interpolation loses (gain droop and images)
    const double sincHi = sine_error_db(ResampleQuality::Sinc, 15000, 48000, 44100, 20000);
    const double linHi = sine_error_db(ResampleQuality::Linear, 15000, 48000, 44100, 20000);
    std::printf("15 kHz error: sinc %.1f dB linear %.1f dB\n", sincHi, linHi);
    CHECK(sincHi < -55.0 && linHi > -25.0, "15 kHz: sinc %.1f linear %.1f", sincHi, linHi);
  }
  // ---- resampler: a 23.5 kHz tone (above the 44.1 kHz Nyquist) must be filtered, not folded to 20.6 kHz
  {
    const double sinc = sine_error_db(ResampleQuality::Sinc, 23500, 48000, 44100, 20000, true);
    const double lin = sine_error_db(ResampleQuality::Linear, 23500, 48000, 44100, 20000, true);
    std::printf("23.5 kHz alias level (rel. to tone): sinc %.1f dB, linear %.1f dB\n", sinc, lin);
    CHECK(sinc < -40.0, "sinc alias rejection %.1f dB", sinc);
    CHECK(lin > sinc + 20.0, "linear folds the tone");
  }
  // ---- resampler: output does not depend on the block size, and passthrough is exact
  {
    auto run = [](uint32_t block) {
      Resampler r; r.init(48000, 44100, 2, ResampleQuality::Sinc);
      SineSrc s{440, 48000, 0.4, 2};
      std::vector<float> all;
      for (uint32_t done = 0; done < 9000; done += block) {
        std::vector<float> o((size_t)block * 2);
        r.produce(o.data(), block, [&](float* d, uint32_t n) { s(d, n); });
        all.insert(all.end(), o.begin(), o.end());
      }
      all.resize(9000 * 2);
      return all;
    };
    const auto a = run(1000), b = run(120), c = run(1);
    double d1 = 0, d2 = 0;
    for (size_t i = 0; i < a.size(); ++i) { d1 = std::max(d1, (double)std::fabs(a[i] - b[i])); d2 = std::max(d2, (double)std::fabs(a[i] - c[i])); }
    CHECK(d1 < 1e-6 && d2 < 1e-6, "block size changed the output (max diff %g, %g)", d1, d2);
    Resampler p; p.init(48000, 48000, 2, ResampleQuality::Sinc);
    float in[8] = {1, 2, 3, 4, 5, 6, 7, 8}, out[8] = {};
    p.produce(out, 4, [&](float* d, uint32_t n) { for (uint32_t i = 0; i < n * 2; ++i) d[i] = in[i]; });
    CHECK(out[0] == 1 && out[7] == 8, "passthrough");
  }
  // ---- resampler: channels do not leak into each other (6 ch, only the LFE slot carries a tone); rate and length are right
  {
    Resampler r; r.init(48000, 44100, 6, ResampleQuality::Sinc);
    uint64_t at = 0; uint64_t pulled = 0;
    std::vector<float> out((size_t)44100 * 6);
    r.produce(out.data(), 44100, [&](float* d, uint32_t n) {
      for (uint32_t i = 0; i < n; ++i, ++at) for (int c = 0; c < 6; ++c) d[i * 6 + c] = c == 3 ? 0.3f * (float)std::sin(2 * kPi * 100.0 * (double)at / 48000.0) : 0.0f;
      pulled += n;
    });
    double other = 0, lfe = 0;
    for (size_t i = 1000; i < 44100; ++i) for (int c = 0; c < 6; ++c) (c == 3 ? lfe : other) += out[i * 6 + c] * out[i * 6 + c];
    CHECK(other == 0 && lfe > 100, "channel isolation: other %g, lfe %g", other, lfe);
    CHECK(pulled >= 48000 - 2 && pulled <= 48000 + 40, "44100 output frames consumed %llu input frames, want about 48000", (unsigned long long)pulled);
  }

  // ---- limiter: ceiling holds on loud, hot and random input; below the ceiling the signal is only delayed
  {
    Limiter L; L.init(48000, 2);
    std::vector<float> b(2 * 48000);
    for (int i = 0; i < 48000; ++i) { b[i * 2] = 3.0f * (float)std::sin(i * 0.05); b[i * 2 + 1] = (i % 977 == 0 ? 8.0f : 0.2f) * (float)std::sin(i * 0.11); }
    L.process(b.data(), 48000);
    float peak = 0; for (float v : b) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= Limiter::kCeiling + 1e-6f, "limiter peak %f", peak);
    // 1 kHz at 4x full scale for 0.5 s then quiet: the quiet part must come back to unity within the 100 ms release
    Limiter M; M.init(48000, 2);
    std::vector<float> c(2 * 48000, 0.0f);
    for (int i = 0; i < 24000; ++i) c[i * 2] = c[i * 2 + 1] = 4.0f * (float)std::sin(2 * kPi * 1000.0 * i / 48000.0);
    for (int i = 24000; i < 48000; ++i) c[i * 2] = c[i * 2 + 1] = 0.1f;
    M.process(c.data(), 48000);
    CHECK(std::fabs(c[47999 * 2] - 0.1f) < 1e-5f, "limiter released: %f", c[47999 * 2]);
    Limiter T; T.init(48000, 6);
    std::vector<float> t(6 * 4800), ref;
    for (size_t i = 0; i < t.size(); ++i) t[i] = 0.9f * (float)std::sin(i * 0.013);   // below 0.98
    ref = t; T.process(t.data(), 4800);
    const uint32_t d = T.delay_frames();
    bool exact = true;
    for (uint32_t i = d; i < 4800; ++i) for (int c2 = 0; c2 < 6; ++c2) exact = exact && t[(size_t)i * 6 + c2] == ref[(size_t)(i - d) * 6 + c2];
    CHECK(exact && d == 48, "below the ceiling the limiter must be a bit-exact %u-frame delay", d);
    Limiter N; N.init(48000, 2);
    float nn[4] = {NAN, INFINITY, 0.5f, -0.5f};
    std::vector<float> nb(2 * 200, 0.0f); nb[0] = nn[0]; nb[1] = nn[1];
    N.process(nb.data(), 200);
    bool finite = true; for (float v : nb) finite = finite && std::isfinite(v);
    CHECK(finite, "NaN / inf input must come out finite");
  }

  // ---- compressor: Full is bit-exact; Night lifts quiet passages and holds loud ones; switching has no jump
  {
    Compressor C; C.init(48000);
    std::vector<float> a(2 * 4800), ref;
    for (size_t i = 0; i < a.size(); ++i) a[i] = 0.3f * (float)std::sin(i * 0.02);
    ref = a; C.process(a.data(), 2, 4800);
    CHECK(a == ref, "Full range must leave the signal untouched");
    auto steady_gain_db = [](Range r, float amp) {
      Compressor K; K.init(48000); K.set_range(r);
      std::vector<float> x(2 * 96000);
      for (int i = 0; i < 96000; ++i) x[i * 2] = x[i * 2 + 1] = amp * (float)std::sin(2 * kPi * 200.0 * i / 48000.0);
      std::vector<float> y = x; K.process(y.data(), 2, 96000);
      double ei = 0, eo = 0; for (int i = 72000; i < 96000; ++i) { ei += (double)x[i * 2] * x[i * 2]; eo += (double)y[i * 2] * y[i * 2]; }
      return 10.0 * std::log10(eo / ei);
    };
    const double quiet = steady_gain_db(Range::Night, 0.01f), loud = steady_gain_db(Range::Night, 0.8f), reducedQ = steady_gain_db(Range::Reduced, 0.01f);
    std::printf("night gain: quiet (-40 dBFS) %+.1f dB, loud (-2 dBFS) %+.1f dB; reduced quiet %+.1f dB\n", quiet, loud, reducedQ);
    CHECK(quiet > 7.0 && quiet < 10.0, "night quiet gain %.1f", quiet);
    CHECK(loud < quiet - 8.0, "night loud gain %.1f must sit well below quiet %.1f", loud, quiet);
    CHECK(reducedQ > 2.0 && reducedQ < 4.0, "reduced quiet gain %.1f", reducedQ);
    // switching on is ramped: no sample-to-sample step beyond a normal sine step
    Compressor S; S.init(48000); S.set_range(Range::Night);
    std::vector<float> s(2 * 4800);
    for (int i = 0; i < 4800; ++i) s[i * 2] = s[i * 2 + 1] = 0.02f * (float)std::sin(2 * kPi * 100.0 * i / 48000.0);
    S.process(s.data(), 2, 4800);
    double maxStep = 0; for (int i = 1; i < 4800; ++i) maxStep = std::max(maxStep, (double)std::fabs(s[i * 2] - s[(i - 1) * 2]));
    CHECK(maxStep < 0.01, "switch-on step %.4f", maxStep);
  }

  // ---- gain slew: same result for any block size, 30 ms from 1 to 0
  {
    auto run = [](uint32_t block) {
      GainSlew g; g.init(48000, 30.0f, 1.0f);
      std::vector<float> v(4800, 1.0f);
      for (uint32_t i = 0; i < 4800; i += block) g.apply(v.data() + i, 1, std::min<uint32_t>(block, 4800 - i), 0.0f);
      return v;
    };
    const auto a = run(128), b = run(1056), c = run(4800);
    CHECK(a == b && b == c, "slew depends on the block size");
    CHECK(a[0] < 1.0f && a[0] > 0.99f && a[1439] < 0.001f && a[1500] == 0.0f, "30 ms ramp: %f %f %f", a[0], a[1439], a[1500]);
  }

  // ---- edge fades
  {
    std::vector<float> v(2 * 100, 1.0f);
    fade_out(v.data(), 2, 100, 10); fade_in(v.data(), 2, 100, 10);
    CHECK(v[0] < 0.1f && v[2 * 9] < 1.0f && v[2 * 10] == 1.0f && v[2 * 89] == 1.0f && v[2 * 99] < 0.1f && v[2 * 99] > 0.0f, "fades");
    std::vector<float> w(2 * 4, 1.0f); fade_in(w.data(), 2, 4, 96);   // shorter than the fade: still ends below 1, no overrun
    CHECK(w[0] < w[2] && w[6] < 1.0f, "short block fade");
  }

  std::printf(fails ? "dsp_test: %d FAILED\n" : "dsp_test: all passed\n", fails);
  return fails ? 1 : 0;
}
