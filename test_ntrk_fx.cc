// Tests for the effect modules, with nothing behind them.
//
//     c++ -std=c++11 -Wall -Wextra -ffp-contract=off \
//         -o /tmp/test_ntrk_fx third_party/ntrk/test_ntrk_fx.cc
//     /tmp/test_ntrk_fx
//
// Separate from test_ntrk.cc because the modules are separate: the replayer
// links none of these, and a caller that only wants playback should be able to
// run its tests without compiling a reverb. Same rules as the other file — no
// framework, no build system, no fixtures — because each of these headers makes
// the same standalone claim ntrk.h does, and this is the evidence for it.
//
// `-ffp-contract=off` is not optional here either. These modules are meant to
// produce identical output on ARM64 and wasm, and a fused multiply-add breaks
// that quietly; the project applies the flag to everything it builds.

// The delay and the reverb arrive through ntrk_unity.h, which pulls in their
// `.cc` files: this file has to build with one compiler invocation, so it takes
// the single-translation-unit route. `make test-ntrk-fx-tu` is the other half of
// the claim — it compiles those same two files as separate objects.
#include "ntrk_fx_shape.h"
#include "ntrk_unity.h"

#include <cmath>
#include <stdio.h>
#include <string.h>

using namespace ntrk::fx;

// Lives here rather than in the header: nothing in ntrk_fx_shape.h computes a
// trigonometric function any more, so a pi in it was an unused constant and a
// warning. The reference below is the only thing that still wants one.
static const double kPi = 3.14159265358979323846;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      ++g_failures;                                                            \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
    }                                                                          \
  } while (0)

// Fixed buffers rather than allocation, so this file keeps the same promise the
// modules do. The delay wants two halves of a second at 48 kHz; the reverb
// about 108 KB.
static const float kRate = 48000.f;
static float g_delay_mem[262144];
static float g_reverb_mem[131072];
static float g_left[4800];
static float g_right[4800];

static uint32_t g_rng = 1u;

static float
noise() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return (float) (g_rng & 0xffffu) / 32767.5f - 1.f;
}

static float
absf(float x) {
  return x < 0.f ? -x : x;
}

static bool
finite(float x) {
  return x == x && x < 1e6f && x > -1e6f;
}

// Bit comparison rather than `==`, because the passthrough claims below are
// about the exact bits: -0.0f == 0.0f is true and is not what is being asked.
static bool
same_bits(float a, float b) {
  return memcmp(&a, &b, sizeof a) == 0;
}

// ---------------------------------------------------------------------------
// Shapers
// ---------------------------------------------------------------------------

static void
test_shapers() {
  printf("a shaper at zero drive is the identity, and bounded above it\n");

  for (int kind = 0; kind < 4; ++kind) {
    // **Bit-identical at zero drive**, which is what lets an unused slot cost
    // nothing and change nothing. `==` would not be strong enough: it would
    // accept -0.0f for 0.0f.
    int passthrough = 1;
    for (int i = -1000; i <= 1000; ++i) {
      const float x = (float) i / 500.f;
      if (!same_bits(shape_sample((ShapeKind) kind, x, 0.f), x))
        passthrough = 0;
    }
    CHECK(passthrough);

    // Silence in, silence out, at every drive. A shaper with an offset would
    // put a DC step into a channel that is not playing.
    int silent = 1;
    for (int d = 0; d <= 10; ++d)
      if (shape_sample((ShapeKind) kind, 0.f, (float) d / 10.f) != 0.f)
        silent = 0;
    CHECK(silent);

    // Bounded, and finite, well past full scale on the way in.
    int bounded = 1;
    for (int i = -2000; i <= 2000; ++i) {
      const float y = shape_sample((ShapeKind) kind, (float) i / 500.f, 0.85f);
      if (!finite(y) || y > 1.001f || y < -1.001f)
        bounded = 0;
    }
    CHECK(bounded);
  }

  // Four *different* shapers. The cheap way to get this wrong is one curve with
  // four constants, which would pass everything above.
  double energy[4] = { 0, 0, 0, 0 };
  for (int kind = 0; kind < 4; ++kind)
    for (int i = -2000; i <= 2000; ++i) {
      const float y = shape_sample((ShapeKind) kind, (float) i / 500.f, 0.85f);
      energy[kind] += (double) y * (double) y;
    }
  for (int a = 0; a < 4; ++a)
    for (int b = a + 1; b < 4; ++b)
      CHECK(energy[a] != energy[b]);
}

static void
test_svf() {
  printf("the filter is stable at full resonance and passes DC at unity\n");

  // Maximum resonance, driven at Nyquist, for long enough that an unstable
  // pole would have run away.
  {
    Svf s;
    svf_reset(&s);
    svf_set(&s, 1000.f, 1.f, kRate);
    int ok = 1;
    for (long i = 0; i < 400000; ++i)
      if (!finite(svf_lowpass(&s, (i & 1) ? 1.f : -1.f)))
        ok = 0;
    CHECK(ok);
  }

  // A lowpass has to pass DC at unity, or every filtered voice changes level
  // the moment the filter is switched on.
  {
    Svf s;
    svf_reset(&s);
    svf_set(&s, 1000.f, 0.f, kRate);
    float y = 0.f;
    for (int i = 0; i < 200000; ++i)
      y = svf_lowpass(&s, 1.f);
    CHECK(y > 0.99f && y < 1.01f);
  }

  // Sweeping the cutoff every sample is what an envelope does, and it is where
  // a filter that is only stable at rest falls over.
  {
    Svf s;
    svf_reset(&s);
    int ok = 1;
    for (int i = 0; i < 100000; ++i) {
      const float hz = 60.f + (float) (i % 12000) * 1.5f;
      svf_set(&s, hz, 0.9f, kRate);
      if (!finite(svf_lowpass(&s, noise())))
        ok = 0;
    }
    CHECK(ok);
  }
}

// The header may not call libm; this file may, and that is the whole point of
// the comparison. `svf_tan_quarter` plus the complement rule is what svf_set
// uses to get `g` without a tan(), and the claim it makes is that the answer
// is within a hundredth of a cent of the tan() it replaced -- on every target,
// because a table of literals and a lerp cannot disagree between two libms.
static float
table_g(float fc, float sr) {
  float w = fc / sr;
  if (w > 0.49f) w = 0.49f;
  return (w <= 0.25f) ? svf_tan_quarter(w) : 1.f / svf_tan_quarter(0.5f - w);
}

static void
test_svf_tan_table() {
  printf("the tan table matches std::tan across the cutoff range at every rate\n");

  // The endpoints of the table and of each branch, which is where an off-by-one
  // in the index or a wrong complement shows up as a large error rather than a
  // small one.
  CHECK(svf_tan_quarter(0.f) == 0.f);
  CHECK(absf(svf_tan_quarter(0.25f) - 1.f) < 1e-6f);

  const float rates[] = { 8000.f,  11025.f, 22050.f,  32000.f,  44100.f,
                          48000.f, 88200.f, 96000.f, 176400.f, 192000.f };
  double worst = 0.0;
  float worst_sr = 0.f, worst_fc = 0.f;

  for (size_t r = 0; r < sizeof rates / sizeof rates[0]; ++r) {
    const float sr = rates[r];
    // The same span svf_set will actually see: its own 5 Hz floor up to its
    // own 0.49*sr ceiling, so both branches of the lookup are covered at every
    // rate rather than only the low one.
    const float hi = sr * 0.49f < 20000.f ? sr * 0.49f : 20000.f;
    for (int j = 0; j <= 4000; ++j) {
      const float fc = 5.f * (float) exp(log((double) hi / 5.0) * j / 4000.0);
      const double got = (double) table_g(fc, sr);
      const double ref = (double) std::tan(kPi * fc / sr);
      const double rel = (got - ref < 0 ? ref - got : got - ref) / ref;
      if (rel > worst) {
        worst = rel;
        worst_sr = sr;
        worst_fc = fc;
      }
    }
  }

  // 1e-5 relative is 0.017 cents of cutoff -- the measured worst case is
  // 5.6e-6 (0.0097 cents), so this has about a factor of two of slack and
  // still fails long before the error reaches anything a filter could show.
  printf("  worst relative error vs std::tan: %.3e (at %.0f Hz, %.0f Hz rate)\n",
         worst, (double) worst_fc, (double) worst_sr);
  CHECK(worst < 1e-5);
}

// ---------------------------------------------------------------------------
// Delay
// ---------------------------------------------------------------------------

static void
test_delay() {
  printf("a delay repeats at the time it was given, and refuses what it cannot hold\n");

  const size_t need = delay_bytes_needed(1.f, kRate);
  CHECK(need > 0 && need <= sizeof g_delay_mem);

  // **A buffer one byte short is refused, and a refused delay passes audio
  // through untouched.** The same discipline module_load keeps: validate the
  // caller's memory before writing through it, and fail into something inert.
  {
    Delay d;
    CHECK(!delay_init(&d, g_delay_mem, need - 1, kRate, 1.f));

    float l[8], r[8];
    for (int i = 0; i < 8; ++i) {
      l[i] = 0.5f;
      r[i] = -0.25f;
    }
    delay_process(&d, l, r, 8);
    int untouched = 1;
    for (int i = 0; i < 8; ++i)
      if (l[i] != 0.5f || r[i] != -0.25f)
        untouched = 0;
    CHECK(untouched);
  }

  // mix == 0 is bit-identical, including for -0.0f and a subnormal input.
  {
    Delay d;
    CHECK(delay_init(&d, g_delay_mem, need, kRate, 1.f));
    delay_set(&d, 0.25f, 0.5f, 0.3f, 0.f, false);

    float l[4] = { -0.0f, 1e-40f, 0.7f, -0.7f };
    float r[4] = { 0.3f, -0.0f, 1e-40f, 0.1f };
    float l0[4], r0[4];
    memcpy(l0, l, sizeof l);
    memcpy(r0, r, sizeof r);

    delay_process(&d, l, r, 4);
    int identical = 1;
    for (int i = 0; i < 4; ++i)
      if (!same_bits(l[i], l0[i]) || !same_bits(r[i], r0[i]))
        identical = 0;
    CHECK(identical);
  }

  // **The echo lands on the exact frame.** 10 ms at 48 kHz is 480, and getting
  // this wrong by a smoothing ramp reads as the time being wrong rather than as
  // a ramp — which is how it was got wrong once.
  {
    Delay d;
    CHECK(delay_init(&d, g_delay_mem, need, kRate, 1.f));
    delay_reset(&d);
    delay_set(&d, 0.01f, 0.f, 0.f, 1.f, false);

    static float l[2000], r[2000];
    memset(l, 0, sizeof l);
    memset(r, 0, sizeof r);
    l[0] = 1.f;
    r[0] = 1.f;
    delay_process(&d, l, r, 2000);

    int at = -1;
    float peak = 0.f;
    for (int i = 1; i < 2000; ++i)
      if (l[i] > peak) {
        peak = l[i];
        at = i;
      }
    CHECK(at == 480);

    // Zero feedback is exactly one repeat, not a decaying train.
    int extra = 0;
    for (int i = at + 1; i < 2000; ++i)
      if (l[i] > 0.05f)
        ++extra;
    CHECK(extra == 0);
  }

  // Feedback past unity is allowed on purpose: the saturator in the loop has to
  // hold it, so this must settle rather than diverge.
  {
    Delay d;
    CHECK(delay_init(&d, g_delay_mem, need, kRate, 1.f));
    delay_reset(&d);
    delay_set(&d, 0.05f, 1.05f, 0.2f, 1.f, false);

    float worst = 0.f;
    int ok = 1;
    for (int b = 0; b < 200 && ok; ++b) {
      memset(g_left, 0, sizeof g_left);
      memset(g_right, 0, sizeof g_right);
      if (b == 0) {
        g_left[0] = 1.f;
        g_right[0] = 1.f;
      }
      delay_process(&d, g_left, g_right, 4800);
      for (int i = 0; i < 4800; ++i) {
        if (!finite(g_left[i])) {
          ok = 0;
          break;
        }
        if (absf(g_left[i]) > worst)
          worst = absf(g_left[i]);
      }
    }
    CHECK(ok);
    CHECK(worst < 1.5f);
  }
}

// ---------------------------------------------------------------------------
// Reverb
// ---------------------------------------------------------------------------

static void
test_reverb() {
  printf("a reverb is dense early, tracks its knobs, and reaches real zero\n");

  const size_t need = reverb_bytes_needed(kRate, 0.1f);
  CHECK(need > 0 && need <= sizeof g_reverb_mem);

  {
    Reverb r;
    CHECK(reverb_init(&r, g_reverb_mem, need, kRate, 0.1f));
    reverb_set(&r, 0.8f, 0.3f, 0.02f, 1.f, 0.f);

    float l[4] = { -0.0f, 1e-40f, 0.6f, -0.6f };
    float rr[4] = { 0.2f, -0.0f, 1e-40f, 0.4f };
    float l0[4], r0[4];
    memcpy(l0, l, sizeof l);
    memcpy(r0, rr, sizeof rr);

    reverb_process(&r, l, rr, 4);
    int identical = 1;
    for (int i = 0; i < 4; ++i)
      if (!same_bits(l[i], l0[i]) || !same_bits(rr[i], r0[i]))
        identical = 0;
    CHECK(identical);
  }

  // Ten seconds of full-scale noise into the largest, most reflective setting.
  // The wet path is louder than unity here by design -- see the grid in the
  // header -- so this asks for finite and bounded rather than for quiet.
  {
    Reverb r;
    CHECK(reverb_init(&r, g_reverb_mem, need, kRate, 0.1f));
    reverb_reset(&r);
    reverb_set(&r, 1.f, 1.f, 0.f, 1.f, 1.f);

    g_rng = 1u;
    int ok = 1;
    float worst = 0.f;
    for (int b = 0; b < 100 && ok; ++b) {
      for (int i = 0; i < 4800; ++i) {
        g_left[i] = noise();
        g_right[i] = noise();
      }
      reverb_process(&r, g_left, g_right, 4800);
      for (int i = 0; i < 4800; ++i) {
        if (!finite(g_left[i]) || !finite(g_right[i])) {
          ok = 0;
          break;
        }
        if (absf(g_left[i]) > worst)
          worst = absf(g_left[i]);
      }
    }
    CHECK(ok);
    CHECK(worst < 8.f);
  }

  // The tail gets longer as size does, monotonically. A reverb whose size knob
  // does not do this is not wired to the thing it is labelled with.
  {
    double previous = 0.0;
    int monotonic = 1;
    for (int k = 0; k < 4; ++k) {
      const float size = 0.2f + 0.25f * (float) k;

      Reverb r;
      CHECK(reverb_init(&r, g_reverb_mem, need, kRate, 0.1f));
      reverb_reset(&r);
      reverb_set(&r, size, 0.2f, 0.f, 1.f, 1.f);

      memset(g_left, 0, sizeof g_left);
      memset(g_right, 0, sizeof g_right);
      g_left[0] = 1.f;
      g_right[0] = 1.f;
      reverb_process(&r, g_left, g_right, 4800);

      long last = 0;
      for (int b = 0; b < 120; ++b) {
        for (int i = 0; i < 4800; ++i)
          if (absf(g_left[i]) > 1e-4f)
            last = (long) b * 4800 + i;
        memset(g_left, 0, sizeof g_left);
        memset(g_right, 0, sizeof g_right);
        reverb_process(&r, g_left, g_right, 4800);
      }

      const double seconds = (double) last / (double) kRate;
      if (seconds < previous)
        monotonic = 0;
      previous = seconds;
    }
    CHECK(monotonic);
    CHECK(previous > 1.0);
  }

  // **The diffuser is in front of the tank, and this is how you can tell.**
  // Freeverb produced its first wet sample when the shortest comb wrapped —
  // 25 ms at 48 kHz, and nothing at all before it — because no signal reaches
  // the output until a comb has been round once. Here the output taps sit part
  // way along each line and the input is already noise by the time it arrives,
  // so the first 20 ms is populated rather than empty. A regression to a
  // comb bank scores zero on this, not merely less.
  {
    Reverb r;
    CHECK(reverb_init(&r, g_reverb_mem, need, kRate, 0.1f));
    reverb_reset(&r);
    reverb_set(&r, 0.5f, 0.2f, 0.f, 1.f, 1.f);

    memset(g_left, 0, sizeof g_left);
    memset(g_right, 0, sizeof g_right);
    g_left[0] = 1.f;
    g_right[0] = 1.f;
    reverb_process(&r, g_left, g_right, 4800);

    int early = 0;
    for (int i = 0; i < 960; ++i)   // the first 20 ms at 48 kHz
      if (absf(g_left[i]) > 1e-4f)
        ++early;
    CHECK(early > 50);
  }

  // The decay knob is the one that owns RT60, and the eight-knob call is the
  // only way to reach it. Size is held still here on purpose: with Jot's
  // normalisation the line gains already compensate for length, so a tail that
  // did not move with this would mean the normalisation had eaten the knob.
  {
    double previous = 0.0;
    int monotonic = 1;
    for (int k = 0; k < 3; ++k) {
      const float decay = 0.1f + 0.3f * (float) k;

      Reverb r;
      CHECK(reverb_init(&r, g_reverb_mem, need, kRate, 0.1f));
      reverb_reset(&r);
      reverb_set_full(&r, 0.5f, decay, 0.f, 0.f, 0.7f, 0.f, 1.f, 1.f);

      memset(g_left, 0, sizeof g_left);
      memset(g_right, 0, sizeof g_right);
      g_left[0] = 1.f;
      g_right[0] = 1.f;
      reverb_process(&r, g_left, g_right, 4800);

      long last = 0;
      for (int b = 0; b < 80; ++b) {
        for (int i = 0; i < 4800; ++i)
          if (absf(g_left[i]) > 1e-4f)
            last = (long) b * 4800 + i;
        memset(g_left, 0, sizeof g_left);
        memset(g_right, 0, sizeof g_right);
        reverb_process(&r, g_left, g_right, 4800);
      }

      const double seconds = (double) last / (double) kRate;
      if (seconds <= previous)
        monotonic = 0;
      previous = seconds;
    }
    CHECK(monotonic);
    CHECK(previous > 1.0);
  }

  // **The tail reaches exactly zero, and that is the denormal policy's test.**
  // Without the flush at the store this decays into subnormals and stays there
  // — inaudible, and on x86 expensive on every single tail. It will not show up
  // as slowness on an Apple Silicon machine, so this asks for the bits instead
  // of for a timing.
  {
    Reverb r;
    CHECK(reverb_init(&r, g_reverb_mem, need, kRate, 0.1f));
    reverb_reset(&r);
    reverb_set(&r, 0.5f, 0.5f, 0.f, 1.f, 1.f);

    memset(g_left, 0, sizeof g_left);
    memset(g_right, 0, sizeof g_right);
    g_left[0] = 1.f;
    g_right[0] = 1.f;
    reverb_process(&r, g_left, g_right, 4800);

    for (int b = 0; b < 600; ++b) {
      memset(g_left, 0, sizeof g_left);
      memset(g_right, 0, sizeof g_right);
      reverb_process(&r, g_left, g_right, 4800);
    }

    int nonzero = 0;
    for (int i = 0; i < 4800; ++i)
      if (g_left[i] != 0.f || g_right[i] != 0.f)
        ++nonzero;
    CHECK(nonzero == 0);
  }
}

// ---------------------------------------------------------------------------
// The parameter grid
// ---------------------------------------------------------------------------

// Nothing here is a musical claim. It is the cheap check that no corner of the
// parameter space produces a NaN, an infinity or a runaway — which is the
// failure that takes a whole mix out rather than one voice.
static void
test_parameter_grid() {
  printf("no corner of the parameter space goes non-finite\n");

  int ok = 1;

  for (int kind = 0; kind < 4 && ok; ++kind)
    for (int d = 0; d <= 8 && ok; ++d)
      for (int i = -40; i <= 40; ++i)
        if (!finite(shape_sample((ShapeKind) kind, (float) i / 8.f, (float) d / 8.f))) {
          ok = 0;
          break;
        }
  CHECK(ok);

  const size_t dneed = delay_bytes_needed(1.f, kRate);
  ok = 1;
  for (int t = 1; t <= 4 && ok; ++t)
    for (int f = 0; f <= 4 && ok; ++f)
      for (int dm = 0; dm <= 2 && ok; ++dm)
        for (int pp = 0; pp < 2 && ok; ++pp) {
          Delay d;
          if (!delay_init(&d, g_delay_mem, dneed, kRate, 1.f))
            continue;
          delay_reset(&d);
          delay_set(&d, (float) t * 0.2f, (float) f * 0.26f, (float) dm * 0.5f,
                    1.f, pp != 0);
          g_rng = 7u;
          for (int b = 0; b < 6; ++b) {
            for (int i = 0; i < 4800; ++i) {
              g_left[i] = noise();
              g_right[i] = noise();
            }
            delay_process(&d, g_left, g_right, 4800);
            for (int i = 0; i < 4800; ++i)
              if (!finite(g_left[i]) || !finite(g_right[i])) {
                ok = 0;
                break;
              }
          }
        }
  CHECK(ok);

  const size_t rneed = reverb_bytes_needed(kRate, 0.1f);
  ok = 1;
  for (int sz = 0; sz <= 2 && ok; ++sz)
    for (int dm = 0; dm <= 2 && ok; ++dm)
      for (int w = 0; w <= 2 && ok; ++w) {
        Reverb r;
        if (!reverb_init(&r, g_reverb_mem, rneed, kRate, 0.1f))
          continue;
        reverb_reset(&r);
        reverb_set(&r, (float) sz * 0.5f, (float) dm * 0.5f, 0.05f,
                   (float) w * 0.5f, 1.f);
        g_rng = 11u;
        for (int b = 0; b < 6; ++b) {
          for (int i = 0; i < 4800; ++i) {
            g_left[i] = noise();
            g_right[i] = noise();
          }
          reverb_process(&r, g_left, g_right, 4800);
          for (int i = 0; i < 4800; ++i)
            if (!finite(g_left[i]) || !finite(g_right[i])) {
              ok = 0;
              break;
            }
        }
      }
  CHECK(ok);

  // The same again over the three knobs the five-knob call cannot reach. It is
  // a coarser grid because it is a bigger space, and the corners are what
  // matter: decay at the top is a loop gain of 0.99995 and diffusion at the top
  // is an allpass that rings for a second on its own.
  ok = 1;
  for (int dc = 0; dc <= 2 && ok; ++dc)
    for (int hf = 0; hf <= 1 && ok; ++hf)
      for (int lf = 0; lf <= 1 && ok; ++lf)
        for (int df = 0; df <= 1 && ok; ++df) {
          Reverb r;
          if (!reverb_init(&r, g_reverb_mem, rneed, kRate, 0.1f))
            continue;
          reverb_reset(&r);
          reverb_set_full(&r, 1.f, (float) dc * 0.5f, (float) hf,
                          (float) lf, (float) df, 0.05f, 1.f, 1.f);
          g_rng = 13u;
          for (int b = 0; b < 6; ++b) {
            for (int i = 0; i < 4800; ++i) {
              g_left[i] = noise();
              g_right[i] = noise();
            }
            reverb_process(&r, g_left, g_right, 4800);
            for (int i = 0; i < 4800; ++i)
              if (!finite(g_left[i]) || !finite(g_right[i])) {
                ok = 0;
                break;
              }
          }
        }
  CHECK(ok);
}

int
main(void) {
  test_shapers();
  test_svf();
  test_svf_tan_table();
  test_delay();
  test_reverb();
  test_parameter_grid();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
