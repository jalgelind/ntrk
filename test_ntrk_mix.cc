// Tests for the mixer, with nothing behind them.
//
//     c++ -std=c++11 -Wall -Wextra -ffp-contract=off \
//         -Ithird_party/ntrk -o /tmp/test_ntrk_mix third_party/ntrk/test_ntrk_mix.cc
//     /tmp/test_ntrk_mix
//
// A third binary, because the mixer is a third module boundary: the replayer
// links none of it and the effect modules link none of it either. One binary
// per boundary is what keeps each standalone claim checkable on its own.
//
// `-ffp-contract=off` is required here for the same reason as in the other two
// — and more so, because the central claim below is bit-identity against
// `render_add`, which a fused multiply-add would break silently.

// The unity header rather than `ntrk_mix.h`: the mixer, the delay and the
// reverb are translation units now, and this is the single-TU way to build them
// -- which is also what keeps the one-command claim above true. The other route
// (the plain headers plus the three `.cc` files in a build) is what
// `make test-ntrk-fx-tu` checks.
#include "ntrk_unity.h"

#include <stdio.h>
#include <string.h>

using namespace ntrk;

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

// ---------------------------------------------------------------------------
// A module built in memory, so these tests need no file on disk. Deliberately
// small — this is a carrier for the mixer's claims, not a test of the loader,
// which test_ntrk.cc covers at length.
// ---------------------------------------------------------------------------

static const int kChannels = 4;
static const int kRows = 8;
static const int kSampleLen = 64;

static uint8_t g_bytes[4096];
static size_t g_size = 0;

static void
put_u16(size_t at, uint16_t x) {
  g_bytes[at] = (uint8_t) (x & 0xff);
  g_bytes[at + 1] = (uint8_t) (x >> 8);
}

static void
put_u32(size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i)
    g_bytes[at + (size_t) i] = (uint8_t) ((x >> (8 * i)) & 0xff);
}

static void
build() {
  const size_t orders = 1;
  const size_t instruments = 1;
  const size_t patterns = (size_t) kRows * kChannels * 4u;
  g_size = 32u + orders + instruments * 32u + patterns + kSampleLen;
  memset(g_bytes, 0, g_size);

  memcpy(g_bytes, "NTRK", 4);
  put_u16(4, 2);
  put_u16(6, kChannels);
  put_u16(8, kRows);
  put_u16(10, 6);
  put_u16(12, 125);
  put_u16(14, (uint16_t) orders);
  put_u16(16, 1);
  put_u16(18, (uint16_t) instruments);
  put_u16(20, 0);
  put_u32(22, kSampleLen);          // the blob's declared length
  put_u16(26, 0u);                  // no block directory
  g_bytes[28] = 36u;                // note_max: three octaves

  const size_t itable = 32u + orders;
  put_u32(itable + 0, 0u);
  put_u32(itable + 4, kSampleLen);
  put_u32(itable + 8, 0u);
  put_u32(itable + 12, kSampleLen);
  g_bytes[itable + 16] = 64;

  // A note on every channel, spread down the pattern so the channels are not
  // in phase — a mixer bug that sums wrongly hides when every voice matches.
  const size_t pat = itable + instruments * 32u;
  for (int c = 0; c < kChannels; ++c) {
    const size_t cell = pat + ((size_t) (c * 2) * kChannels + (size_t) c) * 4u;
    g_bytes[cell + 0] = (uint8_t) (13 + c * 3);
    g_bytes[cell + 1] = 1;
  }

  // A square wave: unambiguously not silence, every frame far from zero.
  const size_t blob = pat + patterns;
  for (int i = 0; i < kSampleLen; ++i)
    g_bytes[blob + (size_t) i] = (uint8_t) (int8_t) ((i < kSampleLen / 2) ? 96 : -96);
}

static mix::Mixer g_mixer;
static double g_a[24000 * 2];
static double g_b[24000 * 2];
static float g_l[1024];
static float g_r[1024];

static uint32_t g_rng = 1u;

static float
noise() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return (float) (g_rng & 0xffffu) / 32767.5f - 1.f;
}

// ---------------------------------------------------------------------------

// **The claim the whole mixer rests on.** With every send at zero and every
// slot empty, the mixer must produce exactly what render_add produces — not
// approximately, bit for bit. If this drifts, every reference fingerprint in
// the project is measuring a different program from the one that ships.
static void
test_bypass_is_render_add() {
  printf("a mixer with nothing switched on is render_add, bit for bit\n");

  build();
  Module m;
  CHECK(module_load(&m, g_bytes, g_size));

  const int frames = 24000;

  Player p;
  player_start(&p, &m);
  memset(g_a, 0, sizeof g_a);
  int done = 0;
  while (done < frames) {
    int want = ((done / 256) & 1) ? 97 : 256;
    if (want > frames - done)
      want = frames - done;
    render_add(&p, g_a + (size_t) done * 2, want, 2, 48000.f);
    done += want;
  }

  Player q;
  player_start(&q, &m);
  mix::mixer_reset(&g_mixer);
  // The mixer's master gain replaces the player's rather than compounding it.
  g_mixer.master_gain = q.gain;
  memset(g_b, 0, sizeof g_b);
  done = 0;
  while (done < frames) {
    int want = ((done / 256) & 1) ? 97 : 256;
    if (want > frames - done)
      want = frames - done;
    mix::mixer_render_add(&g_mixer, &q, g_b + (size_t) done * 2, want, 2,
                          48000.f);
    done += want;
  }

  long differ = 0;
  for (int i = 0; i < frames * 2; ++i)
    if (memcmp(&g_a[i], &g_b[i], sizeof(double)) != 0)
      ++differ;
  CHECK(differ == 0);
}

// A limiter that does not bound its output is decoration. The ceiling is
// -1 dBFS, and the input here is driven four times over full scale.
static void
test_limiter_bounds() {
  printf("the limiter bounds a signal driven 12 dB hot\n");

  mix::mixer_reset(&g_mixer);
  float peak = 0.f;
  g_rng = 1u;
  for (int block = 0; block < 200; ++block) {
    for (int i = 0; i < 1024; ++i) {
      g_l[i] = noise() * 4.f;
      g_r[i] = noise() * 4.f;
    }
    mix::limiter_process(&g_mixer, g_l, g_r, 1024, 48000.f);
    for (int i = 0; i < 1024; ++i) {
      const float a = g_l[i] < 0.f ? -g_l[i] : g_l[i];
      const float b = g_r[i] < 0.f ? -g_r[i] : g_r[i];
      if (a > peak) peak = a;
      if (b > peak) peak = b;
    }
  }
  // 10^(-1/20). The tolerance is for the compare, not for the limiter: the
  // measured value lands on the ceiling exactly.
  CHECK(peak <= 0.891251f + 1e-6f);
  CHECK(peak > 0.5f);   // and it is limiting, not muting
}

// An empty slot of every kind has to be an exact identity, or a mixer with one
// effect switched on cannot be reasoned about relative to one with none.
static void
test_empty_slots_are_identity() {
  printf("a default slot of every kind passes audio through untouched\n");

  for (int k = 0; k <= (int) mix::FxKind::kReverb; ++k) {
    const mix::FxKind kind = (mix::FxKind) k;
    mix::Slot s;
    mix::slot_set_kind(&s, kind);
    mix::slot_apply_params(&s, 48000.f);

    g_rng = 7u;
    for (int i = 0; i < 1024; ++i) {
      g_l[i] = noise();
      g_r[i] = noise();
    }
    float l0[1024], r0[1024];
    memcpy(l0, g_l, sizeof l0);
    memcpy(r0, g_r, sizeof r0);

    mix::slot_process_stereo(&s, g_l, g_r, 1024, 48000.f);

    int changed = 0;
    for (int i = 0; i < 1024; ++i)
      if (memcmp(&g_l[i], &l0[i], sizeof(float)) != 0 ||
          memcmp(&g_r[i], &r0[i], sizeof(float)) != 0)
        ++changed;
    CHECK(changed == 0);
  }
}

// **Two filter instances, not one.** svf_lowpass runs the whole state update
// per call, so a single instance pushed L then R advances twice per frame and
// folds each channel into the other's history. Silence on one side has to stay
// exactly silent.
static void
test_stereo_filter_isolation() {
  printf("a stereo filter keeps the channels apart\n");

  mix::Slot s;
  s.param[0] = mix::slot_choice_param((int) mix::FilterMode::kLowpass, 3);
  s.param[1] = 0.5f;                  // normalised cutoff, about 630 Hz
  s.param[2] = 0.5f;
  mix::slot_set_kind(&s, mix::FxKind::kFilter);
  mix::slot_apply_params(&s, 48000.f);

  g_rng = 3u;
  for (int i = 0; i < 1024; ++i) {
    g_l[i] = noise();
    g_r[i] = 0.f;
  }
  mix::slot_process_stereo(&s, g_l, g_r, 1024, 48000.f);

  float leak = 0.f;
  for (int i = 0; i < 1024; ++i) {
    const float a = g_r[i] < 0.f ? -g_r[i] : g_r[i];
    if (a > leak)
      leak = a;
  }
  CHECK(leak == 0.f);
}

// Delay and reverb are stereo-only: handing one mono buffer to delay_process
// twice would alias and process the line twice over. The mono path must refuse
// rather than do that quietly.
static void
test_mono_refuses_stereo_only_effects() {
  printf("the mono path refuses the effects it cannot run\n");

  for (int k = (int) mix::FxKind::kDelay; k <= (int) mix::FxKind::kReverb; ++k) {
    const mix::FxKind kind = (mix::FxKind) k;
    mix::Slot s;
    mix::slot_set_kind(&s, kind);
    mix::slot_apply_params(&s, 48000.f);

    g_rng = 5u;
    for (int i = 0; i < 256; ++i)
      g_l[i] = noise();
    float before[256];
    memcpy(before, g_l, sizeof before);

    mix::slot_process_mono(&s, g_l, 256, 48000.f);

    int changed = 0;
    for (int i = 0; i < 256; ++i)
      if (memcmp(&g_l[i], &before[i], sizeof(float)) != 0)
        ++changed;
    CHECK(changed == 0);
  }
}

// ---------------------------------------------------------------------------
// The instrument's own filter.
//
// The module is built as v1 and its instrument is then edited in memory rather
// than a v2 file being laid out here: `Instrument` is what the mixer reads, and
// whether the loader fills those fields from bytes 19, 28 and 30 correctly is
// test_ntrk.cc's claim, not this binary's. Duplicating the entry layout here
// would mean two places to change and only one of them checked.
// ---------------------------------------------------------------------------

static void
set_filter(Module *m, int type, int cutoff_hz, int res) {
  Instrument &ins = m->instruments[0];
  ins.flags = (uint8_t) (kInstrumentFilter | ((type & 3) << 2));
  ins.filter_cutoff_hz = (uint16_t) cutoff_hz;
  ins.filter_res = (uint8_t) res;
}

static void
render_mix(Module *m, double *out, int frames) {
  Player p;
  player_start(&p, m);
  mix::mixer_reset(&g_mixer);
  g_mixer.master_gain = p.gain;
  memset(out, 0, (size_t) frames * 2u * sizeof(double));
  int done = 0;
  while (done < frames) {
    int want = 256;
    if (want > frames - done)
      want = frames - done;
    mix::mixer_render_add(&g_mixer, &p, out + (size_t) done * 2, want, 2,
                          48000.f);
    done += want;
  }
}

// **The measure: the energy of the first difference against the energy of the
// signal.** `x[i] - x[i-1]` is a first-order highpass whose response climbs
// monotonically from nothing at DC to its maximum at Nyquist, so the ratio says
// where a render's energy sits in the spectrum without an FFT, a window or a
// bin to argue about. Dividing by the signal's own energy is what makes it a
// statement about *shape* rather than level -- a lowpass takes energy away as
// well as moving it, and a bare difference energy would fall for both reasons.
static double
high_ratio(const double *buf, int frames) {
  double energy = 0.0;
  double diff = 0.0;
  double prev = 0.0;
  for (int i = 0; i < frames; ++i) {
    const double x = buf[(size_t) i * 2];
    const double d = x - prev;
    energy += x * x;
    diff += d * d;
    prev = x;
  }
  return energy > 0.0 ? diff / energy : 0.0;
}

// The property: a lowpass takes the top off. Measured against the same module
// rendered with the flag clear, so the comparison is one flag wide.
static void
test_voice_filter_takes_the_top_off() {
  printf("a lowpassed instrument renders with less energy up top\n");

  build();
  Module m;
  CHECK(module_load(&m, g_bytes, g_size));

  const int frames = 12000;
  render_mix(&m, g_a, frames);
  const double open = high_ratio(g_a, frames);

  // 300 Hz sits under every harmonic here and above the fundamentals (the
  // 64-frame square at these periods sounds between about 130 and 220 Hz), so
  // the filter has something to remove and something to leave behind.
  set_filter(&m, 0, 300, 128);
  render_mix(&m, g_b, frames);
  const double closed = high_ratio(g_b, frames);

  CHECK(closed < open * 0.5);
  CHECK(closed > 0.0);   // and it is filtering, not silencing
}

// Four types, four different sounds. A highpass and a lowpass at one cutoff
// producing the same samples would mean the type bits were being dropped, which
// is exactly the bug that looks like "the filter works" until somebody uses it.
static void
test_filter_types_differ() {
  printf("each filter type renders differently at the same cutoff\n");

  build();
  Module m;
  CHECK(module_load(&m, g_bytes, g_size));

  const int frames = 8000;
  double ratio[4];
  double energy[4];
  for (int type = 0; type < 4; ++type) {
    set_filter(&m, type, 600, 100);
    const double *out = type == 0 ? g_a : g_b;
    render_mix(&m, type == 0 ? g_a : g_b, frames);
    ratio[type] = high_ratio(out, frames);
    energy[type] = 0.0;
    for (int i = 0; i < frames; ++i)
      energy[type] += out[(size_t) i * 2] * out[(size_t) i * 2];
    if (type > 0) {
      long same = 0;
      for (int i = 0; i < frames * 2; ++i)
        if (memcmp(&g_a[i], &g_b[i], sizeof(double)) == 0)
          ++same;
      CHECK(same < frames);   // not the lowpass render under another name
    }
  }

  // Direction, not just difference: a highpass must leave more energy up top
  // than a lowpass does.
  CHECK(ratio[1] > ratio[0]);

  // The notch is checked by total energy rather than by `high_ratio`, and it
  // has to be: a notch keeps the whole spectrum bar one band, so it keeps the
  // fundamental -- which is *low*, and pushes the ratio down even as the
  // filter removes less than the bandpass does. Energy is the honest measure
  // for a pair whose difference is how much of the signal each throws away.
  CHECK(energy[3] > energy[2] * 2.0);
}

// **The guard on the property above.** The mixer's central claim is bit
// identity with render_add, and a filter that ran "with a neutral setting" for
// an instrument that asked for none would break it while still sounding right.
static void
test_no_filter_flag_is_untouched() {
  printf("a module with no filter flags is bit for bit the unfiltered path\n");

  build();
  Module m;
  CHECK(module_load(&m, g_bytes, g_size));
  CHECK((m.instruments[0].flags & kInstrumentFilter) == 0u);

  const int frames = 12000;

  Player p;
  player_start(&p, &m);
  memset(g_a, 0, sizeof g_a);
  int done = 0;
  while (done < frames) {
    int want = 256;
    if (want > frames - done)
      want = frames - done;
    render_add(&p, g_a + (size_t) done * 2, want, 2, 48000.f);
    done += want;
  }

  render_mix(&m, g_b, frames);

  long differ = 0;
  for (int i = 0; i < frames * 2; ++i)
    if (memcmp(&g_a[i], &g_b[i], sizeof(double)) != 0)
      ++differ;
  CHECK(differ == 0);
}

// Maximum resonance is a sharp peak by construction (svf_set floors the damping
// term above zero), but "by construction" is what every blown-up filter was
// before it blew up. A long render at res 255 has to stay finite and inside the
// limiter's ceiling -- and a NaN would sail through that ceiling untouched,
// since every comparison against it is false, so it is checked for by name.
static void
test_max_resonance_stays_finite() {
  printf("maximum resonance stays finite over a long render\n");

  build();
  Module m;
  CHECK(module_load(&m, g_bytes, g_size));
  set_filter(&m, 0, 200, 255);

  // Rendered in windows rather than in one go so the tune loops several times
  // inside it: 240000 frames is five passes of the pattern, which is the only
  // way this test reaches a *retrigger* into an already-ringing filter -- the
  // case the state reset exists for.
  Player p;
  player_start(&p, &m);
  mix::mixer_reset(&g_mixer);
  g_mixer.master_gain = p.gain;

  long bad = 0;
  double peak = 0.0;
  for (int window = 0; window < 100; ++window) {
    const int frames = 2400;
    memset(g_a, 0, (size_t) frames * 2u * sizeof(double));
    mix::mixer_render_add(&g_mixer, &p, g_a, frames, 2, 48000.f);
    for (int i = 0; i < frames * 2; ++i) {
      const double x = g_a[i];
      if (!(x == x) || !(x >= -1.0 && x <= 1.0))
        ++bad;
      const double a = x < 0.0 ? -x : x;
      if (a > peak)
        peak = a;
    }
  }
  CHECK(bad == 0);
  CHECK(peak > 0.01);   // silence is finite too, and would pass the check above
}

// ---------------------------------------------------------------------------
// The effect plane.
//
// A second builder, because the plane needs a v2 file: a block directory, a
// declared `blob_bytes`, and 32-byte instrument entries. One channel and one
// looping note, so every measurement below is about one voice — four coming and
// going would put "where does the sound sit" at the mercy of which of them
// happened to be sounding in the window.
// ---------------------------------------------------------------------------

static const int kV2Rows = 8;
static const int kTickFrames = 960;   // 125 BPM at 48 kHz, and it divides exactly

static uint8_t g_plane[kV2Rows * 2];  // one channel, so {cmd, param} per row
static size_t g_pat2 = 0;             // where the v2 pattern block landed

static void
plane_clear() {
  memset(g_plane, 0, sizeof g_plane);
}

static void
plane_put(int row, uint8_t cmd, uint8_t param) {
  g_plane[(size_t) row * 2u + 0] = cmd;
  g_plane[(size_t) row * 2u + 1] = param;
}

static void
build_v2(bool with_plane) {
  const size_t orders = 1;
  const size_t instruments = 1;
  const size_t patterns = (size_t) kV2Rows * 4u;
  // Four bytes of geometry in front of the cells. **Not optional and not
  // defaulted**: an FXPL payload without it is four bytes short of what every
  // reader now computes, and the exact-length check that was always there
  // refuses it -- which is what stops a v2 reader reading the first two cells
  // as a geometry. One column, no meta lanes, which is what a plane written
  // before the prefix existed meant.
  const size_t plane_bytes = 4u + (size_t) kV2Rows * 2u;

  const size_t itable = 32u + orders;
  const size_t pat = itable + instruments * 32u;
  const size_t blob = pat + patterns;
  const size_t dir = blob + kSampleLen;
  const size_t payload = dir + (with_plane ? 12u : 0u);
  g_size = payload + (with_plane ? plane_bytes : 0u);
  memset(g_bytes, 0, g_size);

  memcpy(g_bytes, "NTRK", 4);
  put_u16(4, 2);
  put_u16(6, 1);                      // one channel
  put_u16(8, kV2Rows);
  put_u16(10, 6);                     // speed: six ticks to a row
  put_u16(12, 125);
  put_u16(14, (uint16_t) orders);
  put_u16(16, 1);
  put_u16(18, (uint16_t) instruments);
  put_u16(20, 0);
  // Declared rather than "whatever is left of the file", which is the field the
  // whole of v2 rests on: a directory follows the blob.
  put_u32(22, kSampleLen);
  put_u16(26, (uint16_t) (with_plane ? 1 : 0));
  g_bytes[28] = 36;                   // note_max

  put_u32(itable + 0, 0u);
  put_u32(itable + 4, kSampleLen);
  put_u32(itable + 8, 0u);
  put_u32(itable + 12, kSampleLen);   // looped, so one note holds for the render
  g_bytes[itable + 16] = 64;

  g_pat2 = pat;
  g_bytes[pat + 0] = 13;              // one note, row 0, and it never stops
  g_bytes[pat + 1] = 1;

  for (int i = 0; i < kSampleLen; ++i)
    g_bytes[blob + (size_t) i] =
        (uint8_t) (int8_t) ((i < kSampleLen / 2) ? 96 : -96);

  if (!with_plane)
    return;
  put_u16(dir + 0, 0x0001);           // FXPL
  put_u16(dir + 2, 0);                // optional
  put_u32(dir + 4, (uint32_t) payload);
  put_u32(dir + 8, (uint32_t) plane_bytes);
  put_u16(payload + 0, 1);            // fx_columns
  put_u16(payload + 2, 0);            // meta_columns
  memcpy(g_bytes + payload + 4, g_plane, sizeof g_plane);
}

// A ProTracker effect in the *old* plane. EEx is what the new plane's tick
// counting has to survive, and it is written here rather than there.
static void
note_effect(int row, uint8_t effect, uint8_t param) {
  g_bytes[g_pat2 + (size_t) row * 4u + 2] = effect;
  g_bytes[g_pat2 + (size_t) row * 4u + 3] = param;
}

// Where the signal sits across the field over one window: -1 hard left, +1 hard
// right. Energy rather than amplitude, so the square wave's sign stays out of it.
static double
balance(const double *buf, int from, int frames) {
  double l = 0.0;
  double r = 0.0;
  for (int i = from; i < from + frames; ++i) {
    l += buf[(size_t) i * 2] * buf[(size_t) i * 2];
    r += buf[(size_t) i * 2 + 1] * buf[(size_t) i * 2 + 1];
  }
  const double sum = l + r;
  return sum > 0.0 ? (r - l) / sum : 0.0;
}

static double
energy(const double *buf, int frames) {
  double e = 0.0;
  for (int i = 0; i < frames * 2; ++i)
    e += buf[i] * buf[i];
  return e;
}

static void
test_plane_pan_sets() {
  printf("a set pan command moves the voice across the field\n");

  const int frames = kTickFrames * 5;

  plane_clear();
  build_v2(true);
  Module left_default;
  CHECK(module_load(&left_default, g_bytes, g_size));
  render_mix(&left_default, g_a, frames);
  // player_start's LRRL puts channel 0 on the left, and the plane has said
  // nothing — so this is the baseline the two commands below move away from.
  CHECK(balance(g_a, 0, frames) < -0.5);

  plane_clear();
  plane_put(0, 0x01, 255);
  build_v2(true);
  Module right;
  CHECK(module_load(&right, g_bytes, g_size));
  render_mix(&right, g_a, frames);
  CHECK(balance(g_a, 0, frames) > 0.5);

  plane_clear();
  plane_put(0, 0x01, 128);
  build_v2(true);
  Module centre;
  CHECK(module_load(&centre, g_bytes, g_size));
  render_mix(&centre, g_a, frames);
  // 128 is centre by definition, and centre is where the pan law is unity.
  CHECK(balance(g_a, 0, frames) > -0.01);
  CHECK(balance(g_a, 0, frames) < 0.01);
}

// **Progressively, over the ticks of the row, and that is the whole claim.** A
// slide that applied its parameter once on tick 0 would move the sound just as
// far and would not be a slide.
static void
test_plane_pan_slide_walks() {
  printf("a pan slide walks across the ticks of a row\n");

  plane_clear();
  plane_put(0, 0x01, 0);              // hard left on row 0
  plane_put(1, 0x02, 40);             // then walk right, a step a tick
  build_v2(true);

  Module m;
  CHECK(module_load(&m, g_bytes, g_size));
  const int frames = kTickFrames * 12;    // rows 0 and 1
  render_mix(&m, g_a, frames);

  double b[6];
  for (int k = 0; k < 6; ++k)
    b[k] = balance(g_a, kTickFrames * (6 + k), kTickFrames);

  // Tick 0 of the row is still where the set left it: a slide runs on 1..speed-1.
  CHECK(b[0] < -0.8);

  int rising = 0;
  for (int k = 1; k < 6; ++k)
    if (b[k] > b[k - 1])
      ++rising;
  CHECK(rising == 5);
  CHECK(b[5] > b[0] + 0.5);

  // And it arrives in steps rather than in one jump — no single tick accounts
  // for most of the move.
  CHECK(b[1] - b[0] < (b[5] - b[0]) * 0.75);
}

static void
test_plane_send_level_opens_a_send() {
  printf("a send level command routes the channel into that send\n");

  // A shaper on send 0, so what comes back is audible and is not silence: the
  // send returns wet only, and a send nothing feeds returns nothing at all.
  g_mixer.send[0].param[0] = mix::slot_choice_param((int) fx::ShapeKind::kWarm, 4);
  g_mixer.send[0].param[1] = 0.8f;
  mix::slot_set_kind(&g_mixer.send[0], mix::FxKind::kShape);

  const int frames = kTickFrames * 5;

  plane_clear();
  build_v2(true);
  Module shut;
  CHECK(module_load(&shut, g_bytes, g_size));
  render_mix(&shut, g_a, frames);

  plane_clear();
  plane_put(0, 0x10, 255);            // send 1 wide open
  build_v2(true);
  Module open;
  CHECK(module_load(&open, g_bytes, g_size));
  render_mix(&open, g_b, frames);

  CHECK(energy(g_b, frames) > energy(g_a, frames) * 1.3);
  CHECK(energy(g_a, frames) > 0.0);   // and the closed one is not silence

  mix::slot_set_kind(&g_mixer.send[0], mix::FxKind::kNone);
  for (int s = 0; s < mix::kSends; ++s)
    g_mixer.send_level[0][s] = 0.f;
}

// **The guard on the mixer's central claim.** A module with no FXPL block is
// every v1 file there is, and the plane must not cost it one bit — which means
// not running the machinery with no-op commands, but not running it at all.
static void
test_plane_absent_is_render_add() {
  printf("a module with no FXPL block is bit for bit the plainly mixed path\n");

  const int frames = 12000;

  for (int pass = 0; pass < 2; ++pass) {
    // Pass 1 is the same file *with* an all-zero plane: the machinery runs and
    // still has to come out bit-identical, or "the plane is idle" and "the
    // plane is absent" are two different renders of one tune.
    plane_clear();
    build_v2(pass == 1);
    Module m;
    CHECK(module_load(&m, g_bytes, g_size));
    CHECK((m.fx == nullptr) == (pass == 0));

    Player p;
    player_start(&p, &m);
    memset(g_a, 0, sizeof g_a);
    int done = 0;
    while (done < frames) {
      int want = 256;
      if (want > frames - done)
        want = frames - done;
      render_add(&p, g_a + (size_t) done * 2, want, 2, 48000.f);
      done += want;
    }

    render_mix(&m, g_b, frames);

    long differ = 0;
    for (int i = 0; i < frames * 2; ++i)
      if (memcmp(&g_a[i], &g_b[i], sizeof(double)) != 0)
        ++differ;
    CHECK(differ == 0);
  }
}

// **The test that proves the tick counter was necessary.** EEx pins `tick` at
// `speed - 1` and returns, so twelve held ticks all report the same
// (order, row, tick) — an implementation that spotted ticks by comparing that
// triple would drop every one of them, and the pan would freeze halfway through
// a row that is on screen for four times as long as usual.
static void
test_plane_slide_survives_pattern_delay() {
  printf("a slide keeps advancing through an EEx pattern delay\n");

  plane_clear();
  plane_put(0, 0x01, 0);              // hard left
  plane_put(1, 0x02, 12);             // and walk right for the whole held row
  build_v2(true);
  note_effect(1, 0xE, 0xE2);          // hold row 1 for two rows' worth of ticks

  Module m;
  CHECK(module_load(&m, g_bytes, g_size));

  // Row 1 runs 6 + 2 * 6 == 18 ticks. Anything past tick 5 of it is a tick the
  // naive detection cannot see.
  const int held = 18;
  const int frames = kTickFrames * (6 + held);
  render_mix(&m, g_a, frames);

  double b[18];
  for (int k = 0; k < held; ++k)
    b[k] = balance(g_a, kTickFrames * (6 + k), kTickFrames);

  CHECK(b[0] < -0.8);

  int rising = 0;
  for (int k = 1; k < held; ++k)
    if (b[k] > b[k - 1])
      ++rising;
  CHECK(rising == held - 1);

  // Where a dropped tick would show: the slide is still moving long after the
  // row would have ended without the delay.
  CHECK(b[held - 1] > b[5] + 0.3);
}

// ---------------------------------------------------------------------------
// A synth voice's retrigger, and the instrument filter that has to be reset by
// it.
//
// **A drum never touches `pos`**, so the `pos` moved between runs tell that
// spots a sample's retrigger can never fire for one. The filter would then
// carry the first note's ringing into the second note's attack -- silently, and
// only for synth instruments, which is the worst shape a bug can have.
//
// Assembled as a struct rather than as a file: a synth instrument in a file
// needs a SYNP block, and what is under test here is the mixer. `patterns` is a
// const *pointer* to memory the caller owns, which is the same property an
// editor relies on.
// ---------------------------------------------------------------------------

static const int kRetrigRows = 4;
static Note g_retrig_pat[kRetrigRows];
static const uint8_t g_retrig_order[1] = {0};

// A kick with the noise mix at zero, so the voice is the body resonator alone.
// **That is what makes a bit-for-bit comparison possible**: the channel's noise
// generator is not reset by a trigger, so a voice reading it would render the
// second note differently from the first for a reason that is nothing to do
// with the filter. At mix zero the noise term is multiplied by exactly 0.f and
// the generator's state cannot reach the output.
static void
retrig_module(Module *m, bool with_filter) {
  *m = Module();
  m->version = 2;
  m->channels = 1;
  m->rows = kRetrigRows;
  m->speed = 6;
  m->bpm = 125;
  m->order_count = 1;
  m->pattern_count = 1;
  m->instrument_count = 1;
  m->note_max = 96;
  m->order = g_retrig_order;
  m->patterns = g_retrig_pat;

  Instrument &ins = m->instruments[0];
  // Low enough that the limiter never engages: a gain reduction still releasing
  // at the second note would scale one window against the other.
  ins.volume = 48u;
  ins.type = (uint8_t) InstrumentType::kSynth;
  ins.synth_voice = (uint8_t) SynthVoice::kKick;
  ins.synth_tune = 128u;
  ins.synth_noise = 0u;
  if (!with_filter)
    return;
  ins.flags = kInstrumentFilter;      // lowpass; the type bits stay clear
  ins.filter_cutoff_hz = 400u;
  ins.filter_res = 240u;              // resonant, so a stale state rings
}

static void
test_synth_retrigger_resets_the_filter() {
  printf("a synth retrigger resets the instrument filter\n");

  // 960 frames a tick at 125 BPM and 48 kHz, so row 1 begins at 5760 -- and
  // that is a multiple of the 64-frame run, which is what lets the two windows
  // be compared sample against sample at all.
  const int second = kTickFrames * 6;
  const int window = 4096;
  const int frames = second + window;

  for (int i = 0; i < kRetrigRows; ++i)
    g_retrig_pat[i] = Note();
  g_retrig_pat[0].note = 25u;
  g_retrig_pat[0].instrument = 1u;

  // The tail that would do the polluting: with row 1 empty, the first note is
  // still plainly sounding where the second one is about to be struck. Without
  // that this whole test would pass by having nothing to carry over.
  Module tail;
  retrig_module(&tail, true);
  render_mix(&tail, g_a, frames);
  CHECK(energy(g_a + (size_t) second * 2, window) > 0.0);
  double head = 0.0;
  for (int i = second; i < second + 64; ++i)
    head += g_a[(size_t) i * 2] * g_a[(size_t) i * 2];
  CHECK(head > 0.0);

  g_retrig_pat[1].note = 25u;
  g_retrig_pat[1].instrument = 1u;

  Module m;
  retrig_module(&m, true);
  render_mix(&m, g_b, frames);

  // The filter is doing something, or the comparison below is about nothing.
  Module plain;
  retrig_module(&plain, false);
  render_mix(&plain, g_a, frames);
  CHECK(memcmp(g_a, g_b, sizeof(double) * (size_t) window * 2u) != 0);

  // **The claim.** The same note struck twice on the same channel, and the
  // second one sounds exactly like the first -- which it can only do if the
  // filter it runs through was returned to rest by the trigger. Before the
  // synth's own tell was added this compared unequal by the width of the first
  // note's ring, and nothing said so.
  CHECK(memcmp(g_b, g_b + (size_t) second * 2, sizeof(double) *
                                                   (size_t) window * 2u) == 0);
  // And no limiter gain was in flight to scale one window against the other.
  CHECK(g_mixer.limit_gain == 1.f);
}

// ---------------------------------------------------------------------------
// One tune that uses everything at once, and the two claims that need it.
//
// Every v2 feature above is checked on its own, with everything else switched
// off, which is what makes each of those tests readable. What none of them
// reaches is *interaction*: per-block state that leaks across a chunk boundary
// is invisible at one chunking and wrong at another, and a chain whose layers
// have only ever been measured apart has no statement at all about what it
// sounds like together.
//
// Assembled as a struct rather than as a file, for the reason the retrigger
// module above gives: what is under test is the mixer, and laying out a SYNP
// block and a 32-byte instrument table here would be a second copy of a format
// `test_ntrk.cc` already owns -- two places to change and only one of them
// checked.
// ---------------------------------------------------------------------------

static const int kBigChannels = 6;
static const int kBigRows = 8;

// A shade over two seconds at 48 kHz: two whole passes of the eight rows and a
// little of a third. The wrap is what the third pass is for -- a retrigger into
// a filter still ringing, a limiter still releasing, and slide accumulators
// that carry across it rather than resetting -- and a render stopping at the
// end of the first pass would meet none of it.
static const int kBigFrames = 100000;

// Two effect columns a channel and one meta lane, because "everything at once"
// has to mean everything: the fingerprint below is the only check that notices
// a change to how the plane is *walked*, and a one-column plane with no macro
// in it walks none of the geometry the format now has.
static const int kBigFxColumns = 2;
static const int kBigMetaColumns = 1;
static const int kBigLanes = kBigChannels * kBigFxColumns + kBigMetaColumns;

static Note g_big_pat[kBigRows * kBigChannels];
static FxCell g_big_fx[kBigRows * kBigLanes];
static const uint8_t g_big_order[1] = {0};
static Module g_big;

static int8_t g_big_pcm8[64];
static int8_t g_big_pcm16[128];        // 64 frames, little-endian pairs

// The tank and the line the two sends point at. Static because a `Mixer` holds
// neither -- 127 KB of reverb has no business on a render thread's stack, which
// is the whole reason those effects take the caller's memory.
static float g_reverb_mem[64 * 1024];
static float g_delay_mem[64 * 1024];

static double g_ref[kBigFrames * 2];
static double g_cur[kBigFrames * 2];

static void
big_note(int row, int c, int note, int instrument) {
  Note &n = g_big_pat[(size_t) row * (size_t) kBigChannels + (size_t) c];
  n.note = (uint8_t) note;
  n.instrument = (uint8_t) instrument;
}

// The plane's row is `kBigLanes` wide, not `kBigChannels` -- a channel owns a
// contiguous run of columns and the meta lanes follow all of them.
static void
big_cell_at(int row, int lane, int cmd, int param) {
  FxCell &f = g_big_fx[(size_t) row * (size_t) kBigLanes + (size_t) lane];
  f.cmd = (uint8_t) cmd;
  f.param = (uint8_t) param;
}

static void
big_cell(int row, int c, int cmd, int param) {
  big_cell_at(row, c * kBigFxColumns, cmd, param);
}

static void
big_col(int row, int c, int col, int cmd, int param) {
  big_cell_at(row, c * kBigFxColumns + col, cmd, param);
}

static void
big_meta(int row, int macro, int input) {
  big_cell_at(row, kBigChannels * kBigFxColumns, macro, input);
}

static void
big_target(int macro, int k, int target, int scope, int scale, int offset) {
  Macro &mc = g_big.macros[macro - 1];
  MacroTarget &mt = mc.targets[k];
  mt.target = (uint8_t) target;
  mt.scope = (uint8_t) scope;
  mt.scale = (int16_t) scale;
  mt.offset = (int16_t) offset;
  if (k + 1 > (int) mc.target_count)
    mc.target_count = (uint8_t) (k + 1);
  if (macro > g_big.macro_count)
    g_big.macro_count = macro;
}

static void
big_build() {
  for (int i = 0; i < 64; ++i)
    g_big_pcm8[i] = (int8_t) ((i < 32) ? 96 : -96);

  // A triangle, written a byte at a time on purpose. `instrument_frame` reads a
  // 16-bit blob with `read_u16` rather than casting it, because an offset need
  // not be even and a file is little-endian whatever the host is; a test that
  // built this as an `int16_t` array would exercise neither.
  for (int i = 0; i < 64; ++i) {
    const int fold = (i < 32) ? i : 64 - i;
    const uint16_t u = (uint16_t) (int16_t) ((fold - 16) * 1600);
    g_big_pcm16[i * 2 + 0] = (int8_t) (uint8_t) (u & 0xffu);
    g_big_pcm16[i * 2 + 1] = (int8_t) (uint8_t) (u >> 8);
  }

  Module *m = &g_big;
  *m = Module();
  m->version = 2;
  m->channels = kBigChannels;
  m->rows = kBigRows;
  m->speed = 6;
  m->bpm = 125;
  m->order_count = 1;
  m->pattern_count = 1;
  m->instrument_count = 5;
  m->note_max = 96;
  m->fx_columns = kBigFxColumns;
  m->meta_columns = kBigMetaColumns;
  m->order = g_big_order;
  m->patterns = g_big_pat;
  m->fx = g_big_fx;

  // 1 -- PCM8, and the only instrument carrying its own envelope and filter.
  Instrument &pcm8 = m->instruments[0];
  pcm8.data = g_big_pcm8;
  pcm8.length = 64u;
  pcm8.loop_start = 0u;
  pcm8.loop_len = 64u;
  pcm8.volume = 52u;
  pcm8.type = (uint8_t) InstrumentType::kPcm8;
  pcm8.bits = 8u;
  pcm8.flags = (uint8_t) (kInstrumentEnvelope | kInstrumentFilter);
  pcm8.env_attack_ms = 6u;
  pcm8.env_decay_ms = 140u;
  pcm8.env_sustain = 40u;
  pcm8.env_release_ms = 220u;
  pcm8.filter_cutoff_hz = 1400u;
  pcm8.filter_res = 190u;

  // 2 -- PCM16. `bits` is derived from `type` by the loader and never read from
  // a file, so a hand-built instrument has to agree with itself here.
  Instrument &pcm16 = m->instruments[1];
  pcm16.data = g_big_pcm16;
  pcm16.length = 64u;
  pcm16.loop_start = 0u;
  pcm16.loop_len = 64u;
  pcm16.volume = 44u;
  pcm16.type = (uint8_t) InstrumentType::kPcm16;
  pcm16.bits = 16u;

  // 3 -- a built-in wavetable, whose filter the plane switches on from outside.
  Instrument &wave = m->instruments[2];
  wave.wave_index = 1u;
  wave.data = builtin_wave(1);
  wave.length = (uint32_t) kBuiltinWaveFrames;
  wave.loop_start = 0u;
  wave.loop_len = (uint32_t) kBuiltinWaveFrames;
  wave.volume = 36u;
  wave.type = (uint8_t) InstrumentType::kWaveBuiltin;
  wave.bits = 8u;

  // 4 -- a drum. 5 -- the 303, with an accent depth, so the row's own accent
  // trigger has something to multiply.
  Instrument &kick = m->instruments[3];
  kick.volume = 52u;
  kick.type = (uint8_t) InstrumentType::kSynth;
  kick.synth_voice = (uint8_t) SynthVoice::kKick;
  kick.synth_tune = 128u;
  kick.synth_decay = 150u;
  kick.synth_sweep = 160u;
  kick.synth_tone = 120u;
  kick.synth_noise = 48u;
  kick.synth_noise_decay = 60u;
  kick.synth_drive = 96u;

  Instrument &bass = m->instruments[4];
  bass.volume = 48u;
  bass.type = (uint8_t) InstrumentType::kSynth;
  bass.synth_voice = (uint8_t) SynthVoice::kBass;
  bass.synth_decay = 96u;
  bass.synth_cutoff = 112u;
  bass.synth_reso = 200u;
  bass.synth_env_mod = 170u;
  bass.synth_accent = 210u;
  bass.synth_dist = 1u;
  bass.synth_dist_mix = 120u;
  bass.synth_wave = 1u;

  for (int i = 0; i < kBigRows * kBigChannels; ++i)
    g_big_pat[i] = Note();
  for (int i = 0; i < kBigRows * kBigLanes; ++i)
    g_big_fx[i] = FxCell();
  for (int i = 0; i < kMaxMacros; ++i)
    m->macros[i] = Macro();
  m->macro_count = 0;

  big_note(0, 0, 25, 1);
  big_note(2, 0, 28, 1);
  big_note(4, 0, 25, 1);
  big_note(6, 0, 32, 1);

  big_note(0, 1, 13, 2);
  big_note(4, 1, 20, 2);

  big_note(1, 2, 37, 3);
  big_note(3, 2, 40, 3);
  big_note(5, 2, 44, 3);
  big_note(7, 2, 37, 3);

  for (int r = 0; r < kBigRows; r += 2)
    big_note(r, 3, 25, 4);

  static const int kBassLine[kBigRows] = {13, 13, 16, 13, 18, 13, 20, 13};
  for (int r = 0; r < kBigRows; ++r)
    big_note(r, 4, kBassLine[r], 5);

  big_note(0, 5, 8, 2);
  big_note(4, 5, 10, 2);

  // The mixer's half of the plane: pan and a pan slide on channel 0, two sends
  // and their slides on channel 1, the whole filter override on channel 2, and
  // the master fader on channel 5. Both slide directions appear, because a
  // parameter is read as signed and one command has to walk both ways.
  big_cell(0, 0, 0x01, 20);
  big_cell(1, 0, 0x02, 8);
  big_cell(2, 0, 0x02, 8);
  big_cell(4, 0, 0x01, 235);
  big_cell(5, 0, 0x02, 0xF8);

  big_cell(0, 1, 0x10, 200);
  big_cell(2, 1, 0x14, 0xF0);
  big_cell(4, 1, 0x11, 160);
  big_cell(6, 1, 0x15, 12);

  big_cell(0, 2, 0x05, 90);
  big_cell(1, 2, 0x09, 2);
  big_cell(2, 2, 0x07, 200);
  big_cell(3, 2, 0x06, 10);
  big_cell(4, 2, 0x06, 10);
  big_cell(6, 2, 0x06, 0xF6);
  big_cell(7, 2, 0x08, 4);

  big_cell(0, 5, 0x03, 200);
  big_cell(4, 5, 0x04, 0xFE);

  // The player's half, 0x30 and 0x31, which the mixer never sees. They are here
  // because a tune using everything uses these too, and because the two halves
  // reading one plane is exactly the arrangement worth rendering.
  big_cell(0, 4, 0x30, 255);
  big_cell(2, 4, 0x31, 4);
  big_cell(4, 4, 0x30, 128);
  big_cell(6, 4, 0x31, 8);

  // Column 1. Channel 0 already carries pan and a pan slide in column 0, so its
  // second column is a send it could not otherwise have had on the same rows --
  // which is the whole reason columns exist. Channel 4's column 0 belongs to the
  // player (accent and glide), so the mixer's command for it can only be here:
  // one channel, two readers, two columns.
  big_col(0, 0, 1, 0x11, 180);
  big_col(2, 0, 1, 0x15, 0xF4);
  big_col(0, 4, 1, 0x01, 60);

  // The meta lane and the two macros it fires. Macro 1 is absolute -- one byte
  // opening the reverb send on every channel at once, which is the case a
  // per-channel command cannot express in one cell. Macro 2 is a delta, and its
  // offset is what lets an unsigned input walk a parameter both ways: the step
  // is `input - 6`, so 10 climbs and 2 falls.
  g_big.macros[0].flags = 0u;
  big_target(1, 0, mix::kFxplSend + 0, kMacroScopeAll, 128, 0);
  g_big.macros[1].flags = kMacroDelta;
  big_target(2, 0, mix::kFxplPan, 3, 256, -6 * 256);

  big_meta(0, 1, 120);
  big_meta(2, 2, 10);
  big_meta(4, 1, 40);
  big_meta(6, 2, 2);

  CHECK(fx::reverb_bytes_needed(48000.f, 0.08f) <= sizeof g_reverb_mem);
  CHECK(fx::reverb_init(&g_mixer.send[0].reverb, g_reverb_mem,
                        sizeof g_reverb_mem, 48000.f, 0.08f));
  CHECK(fx::delay_bytes_needed(0.4f, 48000.f) <= sizeof g_delay_mem);
  CHECK(fx::delay_init(&g_mixer.send[1].delay, g_delay_mem, sizeof g_delay_mem,
                       48000.f, 0.4f));
}

// Every layer switched on at once: an insert, two sends with a tail apiece, the
// stereo width, a master effect and a master gain hot enough that the limiter
// is working rather than present.
//
// Called before every render rather than once, because a render moves settings:
// the plane writes `master_gain` and `send_level`, so a second render starting
// from where the first finished would not be a second render of the same tune.
static void
big_mixer_setup() {
  for (int c = 0; c < kMaxChannels; ++c) {
    mix::slot_set_kind(&g_mixer.insert[c], mix::FxKind::kNone);
    for (int s = 0; s < mix::kSends; ++s)
      g_mixer.send_level[c][s] = 0.f;
  }

  g_mixer.insert[0].param[0] = mix::slot_choice_param((int) fx::ShapeKind::kCrunch, 4);
  g_mixer.insert[0].param[1] = 0.55f;
  mix::slot_set_kind(&g_mixer.insert[0], mix::FxKind::kShape);

  mix::Slot &rv = g_mixer.send[0];
  rv.param[0] = 0.62f;                // size
  rv.param[1] = 0.35f;                // damping
  rv.param[2] =                       // predelay, 0.02 s
      0.02f / mix::kSlotPredelayMaxSeconds;
  rv.param[3] = 0.8f;                 // width
  rv.param[4] = 1.f;                  // a send is all wet
  mix::slot_set_kind(&rv, mix::FxKind::kReverb);

  mix::Slot &dl = g_mixer.send[1];
  dl.param[0] =                       // time, 0.17 s
      0.17f / mix::kSlotDelayMaxSeconds;
  dl.param[1] = 0.45f;                // feedback
  dl.param[2] = 0.35f;                // damping
  dl.param[3] = 1.f;                  // mix
  dl.param[4] = 1.f;                  // ping-pong
  mix::slot_set_kind(&dl, mix::FxKind::kDelay);

  mix::slot_set_kind(&g_mixer.send[2], mix::FxKind::kNone);
  mix::slot_set_kind(&g_mixer.send[3], mix::FxKind::kNone);

  g_mixer.master_fx.param[0] = mix::slot_choice_param((int) fx::ShapeKind::kTape, 4);
  g_mixer.master_fx.param[1] = 0.3f;
  mix::slot_set_kind(&g_mixer.master_fx, mix::FxKind::kShape);

  g_mixer.master_gain = 1.5f;
  g_mixer.width = 1.25f;
  mix::mixer_reset(&g_mixer);
}

// `chunk` of 0 means one call for the whole render, which is the case a caller
// asking for a whole file at once produces and the one every other chunking is
// compared against.
static void
big_render(double *out, int frames, int chunk) {
  Player p;
  player_start(&p, &g_big);
  big_mixer_setup();
  memset(out, 0, (size_t) frames * 2u * sizeof(double));

  int done = 0;
  while (done < frames) {
    int want = (chunk > 0) ? chunk : frames;
    if (want > frames - done)
      want = frames - done;
    mix::mixer_render_add(&g_mixer, &p, out + (size_t) done * 2, want, 2,
                          48000.f);
    done += want;
  }
}

// **The claim: how a caller chops the buffer up must not change one sample.**
// `test_ntrk.cc` makes it of the replayer, whose only per-block state is the
// sequencer's own. Every layer on top of it carries state of its own -- the
// mixer's hoisted pan gains and its limiter, the per-channel inserts, the four
// send buses, the per-voice filter's coefficients, the plane's slide
// accumulators and the synth voices' envelopes -- and a block-length dependency
// in any of them is silent at whatever block length it was developed at and
// wrong at the one an audio device happens to ask for.
//
// 64 is `kMaxBlock` exactly; 97 and 373 divide neither it nor the 960-frame
// tick; 1 puts a run boundary between every pair of samples there are.
static void
test_block_size_invariance() {
  printf("the whole chain renders the same audio at every chunk size\n");

  big_build();
  big_render(g_ref, kBigFrames, 0);
  CHECK(energy(g_ref, kBigFrames) > 1.0);   // or every compare below is of two
                                            // empty buffers
  static const int kChunk[4] = {1, 64, 97, 373};
  for (int k = 0; k < 4; ++k) {
    big_render(g_cur, kBigFrames, kChunk[k]);

    long differ = 0;
    int first = -1;
    for (int i = 0; i < kBigFrames * 2; ++i)
      // Bits, not `==`: two renders that agree to a rounding error have not
      // agreed, and -0.0 == 0.0 is the case that hides a sign flip.
      if (memcmp(&g_ref[i], &g_cur[i], sizeof(double)) != 0) {
        if (first < 0)
          first = i;
        ++differ;
      }
    if (differ != 0)
      printf("  chunk %d: %ld of %d samples differ, first at %d\n", kChunk[k],
             differ, kBigFrames * 2, first);
    CHECK(differ == 0);
  }
}

// Hashed over the *quantised* stream, exactly as `test_ntrk.cc` does it, so the
// fingerprint is of the audio rather than of the float noise underneath it --
// and so the two files' numbers mean the same kind of thing.
static uint64_t
hash_stream(const double *buf, int samples) {
  uint64_t hash = 1469598103934665603ULL;
  for (int i = 0; i < samples; ++i) {
    double v = buf[i] * 32767.0;
    if (v > 32767.0)
      v = 32767.0;
    if (v < -32768.0)
      v = -32768.0;
    // Through a uint16_t: shifting a negative signed value right is
    // implementation-defined, and this number must not vary by target.
    const uint16_t u = (uint16_t) (short) v;
    hash ^= (uint64_t) (u & 0xffu);
    hash *= 1099511628211ULL;
    hash ^= (uint64_t) ((u >> 8) & 0xffu);
    hash *= 1099511628211ULL;
  }
  return hash;
}

static void
check_hash(const char *what, uint64_t got, uint64_t want) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL %s fingerprint: got %016llx, pinned %016llx\n", what,
           (unsigned long long) got, (unsigned long long) want);
    printf("       if this change was intended, re-bless it here and say "
           "in the commit what moved it.\n");
  }
}

// **The one number that moves when the sound does.** Every property above pins
// a direction or a bound -- less energy up top, a balance that rises, a bit
// identity against a path with the feature switched off -- and all of them keep
// passing through a change that alters what every tune in the library sounds
// like. This does not, which is what a fingerprint is for.
static void
test_everything_at_once() {
  printf("one tune using every feature at once holds its fingerprint\n");

  big_build();
  big_render(g_ref, kBigFrames, 0);

  long bad = 0;
  double peak = 0.0;
  for (int i = 0; i < kBigFrames * 2; ++i) {
    const double x = g_ref[i];
    // Named rather than left to the bound: every comparison against a NaN is
    // false, so a NaN sails through `-1 <= x <= 1` untouched.
    if (!(x == x) || !(x >= -1.0 && x <= 1.0))
      ++bad;
    const double a = x < 0.0 ? -x : x;
    if (a > peak)
      peak = a;
  }
  CHECK(bad == 0);
  CHECK(peak > 0.05);                 // silence is finite and bounded too

  // And a constant is finite, bounded and has a peak. This is what says the
  // render is a signal: it moves between most neighbouring samples.
  long moves = 0;
  for (int i = 1; i < kBigFrames * 2; ++i)
    if (memcmp(&g_ref[i], &g_ref[i - 1], sizeof(double)) != 0)
      ++moves;
  CHECK(moves > kBigFrames);

  // Re-blessed twice, deliberately. **Both times what moved it is the tune, not
  // the mixer.**
  //
  // From 0x979b73d521ed9312: the module gained a second effect column, a meta
  // lane and two macros, so it is a different arrangement being rendered.
  //
  // From 0xc294326c85730b6a: the 303 in it became a 303. Its filter is now the
  // measured four-pole ladder rather than a two-pole state-variable filter, its
  // amplitude and its cutoff run on separate envelopes, and the `0x31` slide on
  // its line ties the note it slides from instead of striking a new one. Every
  // one of those changes what this tune sounds like on purpose, and the module
  // uses all three. The paths with no 303 in them are unchanged: `test_ntrk.cc`
  // still holds all three of its version 1 fingerprints, and
  // `test_plane_absent_is_render_add` still pins the plane-free render bit for
  // bit.
  // Re-blessed when the reverb was replaced: Freeverb's comb-and-allpass box
  // became a Gardner diffuser into an 8-line FDN. This tune carries a reverb
  // send, so a better reverb *must* move it -- echo density at 20 ms went
  // 0.01 to 0.64 and the first wet sample 25.3 ms to 7.4 ms. The four
  // fingerprints in test_ntrk.cc did not move, which is the discrimination
  // that says this was the reverb and not something else.
  //
  // Re-blessed again when the player stopped reading ProTracker's period table
  // and started computing equal temperament. That table is integers, and its
  // top octave disagreed with its lower two by up to 6.4 cents on D, E, G, G#
  // and B — an error everything above the table inherited by halving, so a lead
  // two octaves over a bass beat against it about twice a second. This tune has
  // notes off C, so it *must* move. The two fingerprints in test_ntrk.cc did
  // not, and that is the discrimination again: their module plays note 25, a C,
  // whose period is 214 under both schemes.
  //
  // And again when the anchor moved to concert pitch: note 1 went from 129.487
  // Hz to 130.813, MIDI 48 exactly. That is one constant and it moves every
  // render, so this one moving says nothing on its own — the discrimination
  // this time is that it moved by a pitch shift and not a timbre one, which the
  // period checks above it pin note by note.
  //
  // And when kSynthRefPeriod was re-derived from the anchor. It had been the
  // literal 214, so after the move to concert pitch every drum in the kit rang
  // 17.64 cents sharp of its own spec. This tune has a kit in it, so it moves.
  check_hash("everything at once", hash_stream(g_ref, kBigFrames * 2),
             0xf53416a7306196c8ULL);
}

// ---------------------------------------------------------------------------
// The plane's command space.
//
// `test_ntrk_fx.cc` fuzzes the shapers, the SVF, the delay and the reverb, and
// `test_ntrk.cc` fuzzes the synth voices. The plane is the one input surface
// nothing walks exhaustively -- and it is a byte out of a file, so every value
// of it is reachable by a module somebody else wrote.
// ---------------------------------------------------------------------------

static const int kFuzzChannels = 3;
static const int kFuzzRows = 8;

// 8 kHz, which puts a tick at exactly 160 frames. A low rate rather than a
// short tune: what this test needs is every command run over *many ticks* --
// the slides especially, which only move on ticks 1..speed-1 -- and the cost of
// that is the number of frames inside a tick, which is the one thing here that
// changes nothing about the command handling.
static const float kFuzzRate = 8000.f;
static const int kFuzzFrames = kFuzzRows * 6 * 160;

static Note g_fuzz_pat[kFuzzRows * kFuzzChannels];
static FxCell g_fuzz_fx[kFuzzRows * kFuzzChannels];
static const uint8_t g_fuzz_order[1] = {0};
static Module g_fuzz;

static void
fuzz_build() {
  Module *m = &g_fuzz;
  *m = Module();
  m->version = 2;
  m->channels = kFuzzChannels;
  m->rows = kFuzzRows;
  m->speed = 6;
  m->bpm = 125;
  m->order_count = 1;
  m->pattern_count = 1;
  m->instrument_count = 3;
  m->note_max = 96;
  m->order = g_fuzz_order;
  m->patterns = g_fuzz_pat;
  m->fx = g_fuzz_fx;

  for (int i = 0; i < 2; ++i) {
    Instrument &ins = m->instruments[i];
    ins.wave_index = (uint8_t) (i * 2);
    ins.data = builtin_wave(i * 2);
    ins.length = (uint32_t) kBuiltinWaveFrames;
    ins.loop_start = 0u;
    ins.loop_len = (uint32_t) kBuiltinWaveFrames;
    ins.volume = (uint8_t) (56 - i * 8);
    ins.type = (uint8_t) InstrumentType::kWaveBuiltin;
  }

  // The 303 is here so 0x30 and 0x31 have somewhere to land: accent and slide
  // are the player's half of the plane, and against a module of samples alone
  // they would be two commands this test could not tell from ignored.
  Instrument &bass = m->instruments[2];
  bass.volume = 48u;
  bass.type = (uint8_t) InstrumentType::kSynth;
  bass.synth_voice = (uint8_t) SynthVoice::kBass;
  bass.synth_decay = 110u;
  bass.synth_cutoff = 120u;
  bass.synth_reso = 190u;
  bass.synth_env_mod = 160u;
  bass.synth_accent = 220u;

  for (int i = 0; i < kFuzzRows * kFuzzChannels; ++i)
    g_fuzz_pat[i] = Note();
  for (int c = 0; c < kFuzzChannels; ++c) {
    // Two notes, at different pitches: a glide needs a pitch to leave as well
    // as one to arrive at.
    Note &a = g_fuzz_pat[(size_t) c];
    a.note = (uint8_t) (25 - c * 5);
    a.instrument = (uint8_t) (c + 1);
    Note &b = g_fuzz_pat[(size_t) 4 * kFuzzChannels + (size_t) c];
    b.note = (uint8_t) (31 - c * 5);
    b.instrument = (uint8_t) (c + 1);
  }
}

// The same command on every row of every channel, so a slide runs for the whole
// tune rather than for the five ticks after the one row that carried it. The
// parameter is offset per channel: a command whose effect depends on which
// channel wrote it cannot hide behind all three being handed the same byte.
static void
fuzz_plane(int cmd, int param) {
  for (int row = 0; row < kFuzzRows; ++row)
    for (int c = 0; c < kFuzzChannels; ++c) {
      FxCell &f = g_fuzz_fx[(size_t) row * (size_t) kFuzzChannels + (size_t) c];
      f.cmd = (uint8_t) cmd;
      f.param = (uint8_t) ((param + c * 37) & 0xff);
    }
}

static void
fuzz_mixer_setup() {
  // **Every parameter is cleared, not only the ones set below.**
  // `slot_set_kind` takes `param` as the configuration rather than clearing
  // it, so a case that wrote an insert parameter from the plane left it there
  // for every case after it -- invisible while the shaper read only param 0
  // and 1, and a leak the moment param 2 became its dry knob.
  for (int c = 0; c < kMaxChannels; ++c) {
    for (int i = 0; i < 8; ++i)
      g_mixer.insert[c].param[i] = 0.f;
    mix::slot_set_kind(&g_mixer.insert[c], mix::FxKind::kNone);
    for (int s = 0; s < mix::kSends; ++s)
      g_mixer.send_level[c][s] = 0.f;
  }
  for (int s = 0; s < mix::kSends; ++s)
    for (int i = 0; i < 8; ++i)
      g_mixer.send[s].param[i] = 0.f;
  for (int i = 0; i < 8; ++i)
    g_mixer.master_fx.param[i] = 0.f;

  g_mixer.insert[0].param[0] = mix::slot_choice_param((int) fx::ShapeKind::kFold, 4);
  g_mixer.insert[0].param[1] = 0.5f;
  mix::slot_set_kind(&g_mixer.insert[0], mix::FxKind::kShape);

  // Every send carries something, because a send level command routes into a
  // bus that returns nothing at all when the slot behind it is empty -- and a
  // command that reaches an empty bus is a command this test cannot see.
  // Shapers and one filter rather than the tails: this runs 1280 renders.
  for (int s = 0; s < mix::kSends; ++s) {
    if (s == 1) {
      g_mixer.send[s].param[0] = mix::slot_choice_param((int) mix::FilterMode::kLowpass, 3);
      g_mixer.send[s].param[1] = 0.55f;   // about 900 Hz
      g_mixer.send[s].param[2] = 0.6f;
      mix::slot_set_kind(&g_mixer.send[s], mix::FxKind::kFilter);
    } else {
      g_mixer.send[s].param[0] = mix::slot_choice_param((int) fx::ShapeKind::kCrunch, 4);
      g_mixer.send[s].param[1] = 0.7f;
      mix::slot_set_kind(&g_mixer.send[s], mix::FxKind::kShape);
    }
  }

  g_mixer.master_fx.param[0] = mix::slot_choice_param((int) fx::ShapeKind::kTape, 4);
  g_mixer.master_fx.param[1] = 0.35f;
  mix::slot_set_kind(&g_mixer.master_fx, mix::FxKind::kShape);

  g_mixer.master_gain = 1.4f;
  g_mixer.width = 1.2f;
  mix::mixer_reset(&g_mixer);
}

static void
fuzz_render(double *out) {
  Player p;
  player_start(&p, &g_fuzz);
  fuzz_mixer_setup();
  memset(out, 0, (size_t) kFuzzFrames * 2u * sizeof(double));
  mix::mixer_render_add(&g_mixer, &p, out, kFuzzFrames, 2, kFuzzRate);
}

// Which commands the two readers between them claim: the mixer takes
// 0x01..0x09 and 0x10..0x17, the player 0x30 and 0x31. Everything else is a
// byte from a writer this reader does not know, and the format says it is
// ignored rather than guessed at -- so it must render what an empty cell does,
// bit for bit, and not merely something plausible.
static bool
fuzz_cmd_known(int cmd) {
  if (cmd >= 0x01 && cmd <= 0x09)
    return true;
  if (cmd >= 0x10 && cmd <= 0x17)
    return true;
  if (cmd == 0x30 || cmd == 0x31)
    return true;
  // **The slot commands, and only the insert's eight of each.** This plane is
  // all channel lanes, so a send or the master named from one has no referent
  // and has to render as an empty cell -- which is what the `guessed` count
  // below is checking for the other 96 of them.
  int slot = 0;
  int param = 0;
  bool slide = false;
  return mix::fxpl_slot_decode(cmd, &slot, &param, &slide) &&
         slot == mix::kSlotInsert;
}

static void
test_plane_command_space() {
  printf("every plane command byte renders bounded, and an unknown one is "
         "ignored\n");

  fuzz_build();
  fuzz_plane(0, 0);
  fuzz_render(g_ref);
  CHECK(energy(g_ref, kFuzzFrames) > 0.0);   // the baseline is not silence

  // 0 and 1 are the ends a step of nothing and a step of everything share; 127
  // and 128 are the two halves of the signed parameter at their extremes, which
  // is where a slide walks off the top of its range and off the bottom of it
  // within the eight rows below; 255 is -1, the slowest walk down there is.
  static const int kParam[5] = {0, 1, 127, 128, 255};

  long bad = 0;
  long guessed = 0;
  long moved = 0;
  for (int cmd = 0; cmd <= 0xff; ++cmd) {
    for (int k = 0; k < 5; ++k) {
      fuzz_plane(cmd, kParam[k]);
      fuzz_render(g_cur);

      for (int i = 0; i < kFuzzFrames * 2; ++i) {
        const double x = g_cur[i];
        if (!(x == x) || !(x >= -1.0 && x <= 1.0))
          ++bad;
      }

      const bool same = memcmp(g_cur, g_ref,
                               sizeof(double) * (size_t) kFuzzFrames * 2u) == 0;
      if (fuzz_cmd_known(cmd)) {
        if (!same)
          ++moved;
      } else if (!same) {
        ++guessed;
        if (guessed == 1)
          printf("  command 0x%02x param %d is not ignored\n", cmd, kParam[k]);
      }
    }
  }
  CHECK(bad == 0);
  CHECK(guessed == 0);
  // And the run is capable of hearing a difference at all: without this the
  // check above would pass on a mixer that ignored the whole plane.
  // 104 of the 175 pairs move it. The ones that do not are the slides whose
  // parameter came out zero on the channel that got it unoffset, and the
  // insert commands past the shaper's two parameters -- every channel but the
  // first carries FxKind::kNone here, and a shaper reads only param 0 and 1.
  if (moved < 95)
    printf("  only %ld of the 175 known command/parameter pairs moved the "
           "render\n", moved);
  CHECK(moved >= 95);
}

// ---------------------------------------------------------------------------
// Multiple columns, meta lanes and macros.
//
// Assembled as `Module` structs rather than as files, for the reason the
// retrigger module above gives: what is under test is the mixer's walk of the
// plane, and laying out an FXPL prefix and a MACR block here would be a second
// copy of a format `test_ntrk.cc` already owns.
//
// **These read `Mixer::fxpl_val` rather than the audio.** The plane's
// accumulators are public and are exactly what "the two columns cancelled" is a
// claim about; inferring the same thing from a balance measurement would pass
// on a mixer that had cancelled them for the wrong reason.
// ---------------------------------------------------------------------------

static const int kMacChannels = 4;
static const int kMacRows = 4;
static const int kMacLanes = kMacChannels * kMaxFxColumns + kMaxMetaColumns;

static Note g_mac_pat[kMacRows * kMacChannels];
static FxCell g_mac_fx[kMacRows * kMacLanes];
static const uint8_t g_mac_order[1] = {0};
static Module g_mac;
static Player g_mac_player;

static void
mac_build(int fx_columns, int meta_columns) {
  Module *m = &g_mac;
  *m = Module();
  m->version = 2;
  m->channels = kMacChannels;
  m->rows = kMacRows;
  m->speed = 6;
  m->bpm = 125;
  m->order_count = 1;
  m->pattern_count = 1;
  m->instrument_count = 1;
  m->note_max = 96;
  m->fx_columns = fx_columns;
  m->meta_columns = meta_columns;
  m->order = g_mac_order;
  m->patterns = g_mac_pat;
  m->fx = g_mac_fx;

  Instrument &ins = m->instruments[0];
  ins.wave_index = 0u;
  ins.data = builtin_wave(0);
  ins.length = (uint32_t) kBuiltinWaveFrames;
  ins.loop_start = 0u;
  ins.loop_len = (uint32_t) kBuiltinWaveFrames;
  ins.volume = 48u;
  ins.type = (uint8_t) InstrumentType::kWaveBuiltin;

  // One held note per channel, so every channel is a voice the plane can be
  // heard on and nothing retriggers inside the window.
  for (int i = 0; i < kMacRows * kMacChannels; ++i)
    g_mac_pat[i] = Note();
  for (int c = 0; c < kMacChannels; ++c) {
    g_mac_pat[c].note = (uint8_t) (25 + c);
    g_mac_pat[c].instrument = 1u;
  }
  for (int i = 0; i < kMacRows * kMacLanes; ++i)
    g_mac_fx[i] = FxCell();
}

static void
mac_chan(int row, int c, int col, int cmd, int param) {
  const size_t lane = (size_t) c * (size_t) g_mac.fx_columns + (size_t) col;
  FxCell &f =
      g_mac_fx[(size_t) row * (size_t) module_lanes(&g_mac) + lane];
  f.cmd = (uint8_t) cmd;
  f.param = (uint8_t) param;
}

static void
mac_meta(int row, int t, int macro, int input) {
  const size_t lane =
      (size_t) (g_mac.channels * g_mac.fx_columns) + (size_t) t;
  FxCell &f =
      g_mac_fx[(size_t) row * (size_t) module_lanes(&g_mac) + lane];
  f.cmd = (uint8_t) macro;
  f.param = (uint8_t) input;
}

// `macro` is the one-based index a meta cell writes, so the arithmetic here is
// the same the mixer does and a test that got it wrong would say so.
static void
mac_target(int macro, int k, int target, int scope, int scale, int offset) {
  Macro &mc = g_mac.macros[macro - 1];
  MacroTarget &mt = mc.targets[k];
  mt.target = (uint8_t) target;
  mt.scope = (uint8_t) scope;
  mt.scale = (int16_t) scale;
  mt.offset = (int16_t) offset;
  if (k + 1 > (int) mc.target_count)
    mc.target_count = (uint8_t) (k + 1);
  if (macro > g_mac.macro_count)
    g_mac.macro_count = macro;
}

// Renders whole sequencer ticks and leaves the player and the mixer standing
// where they finished. 960 frames a tick at 125 BPM and 48 kHz, exactly.
static void
mac_render(int ticks, double *out) {
  player_start(&g_mac_player, &g_mac);
  mix::mixer_reset(&g_mixer);
  for (int c = 0; c < kMaxChannels; ++c) {
    mix::slot_set_kind(&g_mixer.insert[c], mix::FxKind::kNone);
    for (int s = 0; s < mix::kSends; ++s)
      g_mixer.send_level[c][s] = 0.f;
  }
  mix::slot_set_kind(&g_mixer.master_fx, mix::FxKind::kNone);
  g_mixer.master_gain = g_mac_player.gain;
  g_mixer.width = 1.f;
  const int frames = kTickFrames * ticks;
  memset(out, 0, (size_t) frames * 2u * sizeof(double));
  mix::mixer_render_add(&g_mixer, &g_mac_player, out, frames, 2, 48000.f);
}

// More ticks, on the player and mixer `mac_render` left standing. **Without the
// reset**, which is the point: an external input is set between blocks, and a
// helper that restarted would clear the very mask under test.
static void
mac_render_resume(int ticks, double *out) {
  const int frames = kTickFrames * ticks;
  memset(out, 0, (size_t) frames * 2u * sizeof(double));
  mix::mixer_render_add(&g_mixer, &g_mac_player, out, frames, 2, 48000.f);
}

static float
mac_val(int c, int index) {
  return g_mixer.fxpl_val[c][index];
}

// **Two columns on one channel are two commands on one row**, which is the
// whole point of the geometry: at one column a channel could pan or open a
// send on a given row and not both.
static void
test_columns_both_take_effect() {
  printf("two effect columns on one channel both take effect on one row\n");

  mac_build(2, 0);
  mac_chan(0, 0, 0, 0x01, 200);       // pan, from column 0
  mac_chan(0, 0, 1, 0x10, 255);       // send 1 wide open, from column 1
  mac_render(2, g_a);

  CHECK(mac_val(0, mix::kFxplPan) == 200.f);
  CHECK(mac_val(0, mix::kFxplSend + 0) == 255.f);
  CHECK(g_mac_player.pan[0] == mix::fxpl_to_pan(200.f));
  CHECK(g_mixer.send_level[0][0] == mix::fxpl_to_unit(255.f));

  // And a *set* is last-lane-wins, which is what makes the ordering below a
  // decision rather than an accident.
  mac_build(2, 0);
  mac_chan(0, 0, 0, 0x01, 10);
  mac_chan(0, 0, 1, 0x01, 240);
  mac_render(2, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 240.f);
}

// **Deltas sum, so two columns stepping one parameter opposite ways cancel.**
// Last-wins would silently kill a column somebody typed, and clamping each step
// as it arrived would make the answer depend on which column came first.
static void
test_columns_opposite_slides_cancel() {
  printf("two columns sliding one parameter opposite ways cancel\n");

  // The control first: one column alone plainly moves it, or the cancellation
  // below would pass on a mixer that ran neither.
  mac_build(2, 0);
  mac_chan(0, 0, 0, 0x01, 128);       // centre, so neither rail is in reach
  mac_chan(1, 0, 0, 0x02, 20);
  mac_render(12, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 128.f + 5.f * 20.f);

  mac_build(2, 0);
  mac_chan(0, 0, 0, 0x01, 128);
  mac_chan(1, 0, 0, 0x02, 20);
  mac_chan(1, 0, 1, 0x02, 0xEC);      // -20, the same byte read as signed
  mac_render(12, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 128.f);
  CHECK(g_mac_player.pan[0] == mix::fxpl_to_pan(128.f));
}

// The ceiling is eight, so the eighth column has to be reached and its latch
// has to have somewhere to live -- `Mixer::fxpl_cmd` is sized by the same
// constant, and a walk that stopped short would look exactly like an empty cell.
//
// **This is also the mixer's half of the asymmetry.** Eight columns of the same
// slide sum to eight steps a tick here; the player's half takes the last column
// and ignores the other seven, because a glide time is not a delta. Both halves
// are written down where they are implemented: `FxplCmd` in ntrk_mix.h and the
// column walk in ntrk.h's `player_tick`.
static void
test_columns_reach_the_ceiling() {
  printf("all eight effect columns are read, and their slides sum\n");

  mac_build(kMaxFxColumns, 0);
  mac_chan(0, 0, kMaxFxColumns - 1, 0x01, 200);   // a set, in the last column
  mac_render(2, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 200.f);

  mac_build(kMaxFxColumns, 0);
  mac_chan(0, 0, 0, 0x01, 100);
  for (int col = 0; col < kMaxFxColumns; ++col)
    mac_chan(1, 0, col, 0x02, 1);                 // +1 a tick, eight times over
  mac_render(12, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 100.f + 5.f * (float) kMaxFxColumns);
}

// One input byte, three parameters, three scales. This is the thing a macro
// exists for and the thing a per-channel command cannot say.
static void
test_macro_drives_three_targets() {
  printf("one macro turns one input into three targets at three scales\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);            // unity: value = input
  mac_target(1, 1, mix::kFxplSend + 0, 0, 128, 0);       // half
  mac_target(1, 2, mix::kFxplCutoff, 0, 64, 32 * 256);   // a quarter, plus 32
  mac_meta(0, 0, 1, 200);
  mac_render(2, g_a);

  CHECK(mac_val(0, mix::kFxplPan) == 200.f);
  CHECK(mac_val(0, mix::kFxplSend + 0) == 100.f);
  CHECK(mac_val(0, mix::kFxplCutoff) == 82.f);
  // A cutoff target switches the plane's filter override on, exactly as the
  // cutoff *command* does -- or a macro could set a cutoff nothing then read.
  CHECK(g_mixer.fxpl_filter[0]);
  // And it reached the knobs, not just the accumulators.
  CHECK(g_mixer.send_level[0][0] == mix::fxpl_to_unit(100.f));
}

// A delta macro is a slide: nothing on the row's first tick, one step on each
// tick after it, and it keeps stepping for as long as the row lasts.
static void
test_macro_delta_ramps() {
  printf("a delta macro ramps a target a step a tick\n");

  mac_build(1, 1);
  g_mac.macros[0].flags = kMacroDelta;
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_chan(0, 0, 0, 0x01, 100);       // the base it ramps away from
  mac_meta(0, 0, 1, 4);

  mac_render(1, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 100.f);    // tick 0 is the set alone
  mac_render(3, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 108.f);    // two slide ticks
  mac_render(6, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 120.f);    // five, the whole row
}

// **`m->channels`, not `kMaxChannels`.** A scope of 0xFF writing sixteen lanes
// into a four-channel module leaves twelve values nothing mixes -- which the
// next `fxpl_sync` would read back out as though a row had asked for them.
static void
test_macro_scope_all_reaches_every_channel() {
  printf("a macro scoped to all channels reaches every one of them, and no "
         "more\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, kMacroScopeAll, 256, 0);
  mac_meta(0, 0, 1, 220);
  mac_render(2, g_a);

  for (int c = 0; c < kMacChannels; ++c) {
    CHECK(mac_val(c, mix::kFxplPan) == 220.f);
    CHECK(g_mac_player.pan[c] == mix::fxpl_to_pan(220.f));
  }
  for (int c = kMacChannels; c < kMaxChannels; ++c)
    CHECK(mac_val(c, mix::kFxplPan) != 220.f);
}

// ---------------------------------------------------------------------------
// External macro input -- a macro invoked from outside the pattern.
// ---------------------------------------------------------------------------

// **At the row boundary, not on the spot.** A parameter applied the moment the
// host set it would land on the audio thread's block schedule instead of the
// music's, so the same game input would render differently depending on the
// buffer size.
static void
test_external_macro_lands_on_the_row() {
  printf("an external macro input lands on the next row, not mid-row\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_render(1, g_a);                        // row 0, tick 0
  // The plane seeds itself from where the player already stands, so the base
  // here is centre pan and not zero.
  const float base = mac_val(0, mix::kFxplPan);

  mix::mixer_set_macro(&g_mixer, 1, 200);
  CHECK(g_mixer.macro_pending == 1u);
  mac_render_resume(5, g_a);                 // the rest of row 0
  CHECK(mac_val(0, mix::kFxplPan) == base);  // still nothing: it is queued
  CHECK(g_mixer.macro_pending == 1u);

  mac_render_resume(1, g_a);                 // row 1, tick 0
  CHECK(mac_val(0, mix::kFxplPan) == 200.f);
  CHECK(g_mixer.macro_pending == 0u);
  // And it reached the knob, not only the accumulator.
  CHECK(g_mac_player.pan[0] == mix::fxpl_to_pan(200.f));
}

// **The host outranks the tune for the row it speaks on.** The other order
// would make a pattern able to ignore its game, which is not what a parameter
// is for -- and it is the reason the pending set is applied after the cells
// rather than before them.
static void
test_external_macro_outranks_the_row() {
  printf("an external macro input outranks the row's own cell\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_chan(1, 0, 0, 0x01, 50);               // row 1 sets pan 50 itself
  mac_render(1, g_a);

  mix::mixer_set_macro(&g_mixer, 1, 200);
  mac_render_resume(6, g_a);                 // through row 1's tick 0
  CHECK(mac_val(0, mix::kFxplPan) == 200.f);
}

// **Applied once, not held.** A pending input that re-fired every row would be
// a parameter the tune could never take back, and for a delta macro it would
// ramp for ever on one call.
static void
test_external_macro_applies_once() {
  printf("an external macro input applies once, and the tune has the row "
         "after\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_chan(2, 0, 0, 0x01, 50);               // row 2 takes it back
  mac_render(1, g_a);

  mix::mixer_set_macro(&g_mixer, 1, 200);
  mac_render_resume(6, g_a);                 // row 1
  CHECK(mac_val(0, mix::kFxplPan) == 200.f);
  mac_render_resume(6, g_a);                 // row 2
  CHECK(mac_val(0, mix::kFxplPan) == 50.f);
}

// A parameter is a value, not a stream of invocations: a host setting one three
// times between rows means the third.
static void
test_external_macro_last_call_wins() {
  printf("two external inputs between rows apply once, at the later value\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_render(1, g_a);

  mix::mixer_set_macro(&g_mixer, 1, 40);
  mix::mixer_set_macro(&g_mixer, 1, 210);
  mac_render_resume(6, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 210.f);
}

// Two macros pending at once, applied in index order -- so a later macro's
// target overwrites an earlier one's, which is the same last-writer rule every
// other set in the plane follows.
static void
test_external_macro_two_at_once() {
  printf("two external inputs both apply, in index order\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_target(2, 0, mix::kFxplGain, 0, 256, 0);
  mac_target(2, 1, mix::kFxplPan, 0, 0, 90 * 256);   // constant 90, applied last
  mac_render(1, g_a);

  mix::mixer_set_macro(&g_mixer, 2, 150);
  mix::mixer_set_macro(&g_mixer, 1, 200);
  CHECK(g_mixer.macro_pending == 3u);
  mac_render_resume(6, g_a);
  CHECK(mac_val(0, mix::kFxplGain) == 150.f);
  CHECK(mac_val(0, mix::kFxplPan) == 90.f);
}

// An index outside the table is ignored, and it is ignored *without queueing* --
// a bit set for a macro that will never be applied would leave the plane doing
// work on every row for ever.
static void
test_external_macro_index_is_checked() {
  printf("an external macro index outside the table is ignored, and queues "
         "nothing\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_render(1, g_a);
  const float base = mac_val(0, mix::kFxplPan);

  mix::mixer_set_macro(&g_mixer, 0, 200);            // one-based: 0 is no macro
  mix::mixer_set_macro(&g_mixer, kMaxMacros + 1, 200);
  mix::mixer_set_macro(&g_mixer, -3, 200);
  CHECK(g_mixer.macro_pending == 0u);

  // Inside the mixer's range but past the *module's* table: queued, and dropped
  // when it is applied -- the same terms a meta lane's index gets.
  mix::mixer_set_macro(&g_mixer, kMaxMacros, 200);
  CHECK(g_mixer.macro_pending != 0u);
  mac_render_resume(6, g_a);
  CHECK(g_mixer.macro_pending == 0u);
  CHECK(mac_val(0, mix::kFxplPan) == base);
}

// An input is a byte, and a caller that computed 300 meant "as far as it goes".
static void
test_external_macro_input_is_clamped() {
  printf("an external macro input is clamped to a byte\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_render(1, g_a);

  mix::mixer_set_macro(&g_mixer, 1, 4000);
  mac_render_resume(6, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 255.f);

  mix::mixer_set_macro(&g_mixer, 1, -1);
  mac_render_resume(6, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == 0.f);
}

// **The guarantee, and it is written so it can fail.** With nothing pending the
// plane runs the program it ran before any of this existed. The way to say that
// with polarity is to leave an input in `macro_in` that WOULD move a target and
// clear only the mask: a block that scanned the inputs rather than the mask, or
// that lost its `!= 0` guard, moves the target and this goes red.
static void
test_external_macro_absent_touches_nothing() {
  printf("with the mask clear, a value left in macro_in changes nothing\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_render(1, g_a);
  const float base = mac_val(0, mix::kFxplPan);

  mix::mixer_set_macro(&g_mixer, 1, 200);
  CHECK(g_mixer.macro_in[0] == 200u);
  g_mixer.macro_pending = 0u;          // the input stays; only the mask is clear

  mac_render_resume(12, g_a);          // two whole rows
  CHECK(mac_val(0, mix::kFxplPan) == base);
  CHECK(g_mixer.macro_in[0] == 200u);  // and the row did not consume it either
}

// A queued input belongs to the tune that was playing. `mixer_reset` is where a
// caller says that tune is over, so it drops -- carried across, it fires on the
// first row of the next one at an index that means something else there.
static void
test_external_macro_reset_drops_the_queue() {
  printf("a reset drops a queued external input\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_render(1, g_a);
  const float base = mac_val(0, mix::kFxplPan);

  mix::mixer_set_macro(&g_mixer, 1, 200);
  mix::mixer_reset(&g_mixer);
  CHECK(g_mixer.macro_pending == 0u);
  mac_render_resume(6, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == base);
}

// Same reason, the other way a tune ends: a module with no plane has nowhere to
// apply a macro, so what was queued is dropped while it plays rather than
// waiting for a module that does have one.
static void
test_external_macro_drops_without_a_plane() {
  printf("a module with no effect plane drops a queued external input\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  mac_render(1, g_a);

  const FxCell *saved_fx = g_mac.fx;
  g_mac.fx = nullptr;                  // a v1 module, on the same mixer
  mix::mixer_set_macro(&g_mixer, 1, 200);
  mac_render_resume(6, g_a);
  CHECK(g_mixer.macro_pending == 0u);

  // And now a plane comes back: the first row must not fire the stale input.
  g_mac.fx = saved_fx;
  const float base = mac_val(0, mix::kFxplPan);
  mac_render_resume(6, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == base);
}

// **A delta macro is not reachable from outside.** It is a per-tick step the
// meta-lane latch re-fires for a whole row; external input has no latch, so one
// step would be an arbitrary fraction of a slide. Skipped, and the queued input
// is dropped rather than left pending for ever.
static void
test_external_macro_skips_a_delta() {
  printf("an external input on a delta macro is skipped, not half-applied\n");

  mac_build(1, 1);
  g_mac.macros[0].flags = kMacroDelta;
  mac_target(1, 0, mix::kFxplPan, 0, 256, 0);
  // A second, absolute macro queued in the same breath: the delta is skipped and
  // this one still applies, so the skip is a `continue` and not a `break`.
  mac_target(2, 0, mix::kFxplGain, 0, 256, 0);
  mac_render(1, g_a);
  const float base = mac_val(0, mix::kFxplPan);

  mix::mixer_set_macro(&g_mixer, 1, 40);
  mix::mixer_set_macro(&g_mixer, 2, 180);
  mac_render_resume(12, g_a);
  CHECK(mac_val(0, mix::kFxplPan) == base);
  CHECK(mac_val(0, mix::kFxplGain) == 180.f);
  CHECK(g_mixer.macro_pending == 0u);
}

// **Meta lanes are stored last and evaluated first, and that is what makes
// specific beat general.** A macro sweeps one parameter across four channels;
// one channel's own column then trims its own. Meta-last would make that trim
// inexpressible, because a set is last-lane-wins.
static void
test_meta_first_lets_a_column_override() {
  printf("a channel's own column overrides a macro on the same target\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, kMacroScopeAll, 256, 0);
  mac_meta(0, 0, 1, 220);
  mac_chan(0, 1, 0, 0x01, 40);
  mac_render(2, g_a);

  CHECK(mac_val(0, mix::kFxplPan) == 220.f);
  CHECK(mac_val(1, mix::kFxplPan) == 40.f);     // the trim
  CHECK(mac_val(2, mix::kFxplPan) == 220.f);
  CHECK(mac_val(3, mix::kFxplPan) == 220.f);
}

// Ignored at use, not refused at load: finding one needs a scan of the whole
// plane, and the plane's rule has always been that a command is validated where
// it is read. So it has to render what an empty meta cell renders, bit for bit.
static void
test_macro_index_past_the_table_is_ignored() {
  printf("a macro index past the table is ignored, bit for bit\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, kMacroScopeAll, 256, 0);
  mac_meta(0, 0, 5, 220);             // there is one macro, and this is not it
  mac_render(6, g_a);
  CHECK(g_mac.macro_count == 1);

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, kMacroScopeAll, 256, 0);
  mac_render(6, g_b);                 // the same tune with an empty meta cell

  const int frames = kTickFrames * 6;
  long differ = 0;
  for (int i = 0; i < frames * 2; ++i)
    if (memcmp(&g_a[i], &g_b[i], sizeof(double)) != 0)
      ++differ;
  CHECK(differ == 0);
  CHECK(energy(g_b, frames) > 0.0);   // and neither of them is silence
}

// **The reason the arithmetic is i32.** An 8.8 scale of 0x7FFF times an input
// of 255 is 8355585, which in an i16 intermediate wraps -- and a wrap here is
// not a clipped macro, it is a loud one going silent at an arbitrary input.
// Both rails, because a negative scale wraps the other way.
static void
test_macro_extremes_stay_bounded() {
  printf("a macro at maximum scale and input saturates rather than wrapping\n");

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, kMacroScopeAll, 32767, 32767);
  mac_meta(0, 0, 1, 255);
  mac_render(6, g_a);
  for (int c = 0; c < kMacChannels; ++c)
    CHECK(mac_val(c, mix::kFxplPan) == 255.f);

  mac_build(1, 1);
  mac_target(1, 0, mix::kFxplPan, kMacroScopeAll, -32768, -32768);
  mac_meta(0, 0, 1, 255);
  mac_render(6, g_b);
  for (int c = 0; c < kMacChannels; ++c)
    CHECK(mac_val(c, mix::kFxplPan) == 0.f);

  // A delta at the same extreme is clamped on the store rather than on the
  // step, which is the one place the two differ.
  mac_build(1, 1);
  g_mac.macros[0].flags = kMacroDelta;
  mac_target(1, 0, mix::kFxplSend + 0, 0, 32767, 32767);
  mac_meta(0, 0, 1, 255);
  mac_render(6, g_a);
  CHECK(mac_val(0, mix::kFxplSend + 0) == 255.f);
  CHECK(g_mixer.send_level[0][0] == 1.f);

  const int frames = kTickFrames * 6;
  long bad = 0;
  for (int i = 0; i < frames * 2; ++i) {
    const double x = g_a[i];
    if (!(x == x) || !(x >= -1.0 && x <= 1.0))
      ++bad;
  }
  CHECK(bad == 0);
}

// ---------------------------------------------------------------------------
// Effect-slot parameters in the plane.
//
// The slot layer is where a normalised 0..1 becomes seconds and hertz, and the
// plane is where a `u8` out of a file reaches it. Everything below is about the
// join: the units, which lane may carry which slot, and what a resync does with
// a parameter the caller configured and a pattern moved.
// ---------------------------------------------------------------------------

// Their own memory rather than the tanks `big_build` laid out: `slot_set_kind`
// zeroes a line, and borrowing one would empty a tank another test is still
// pinning.
static float g_slot_reverb_mem[64 * 1024];
static float g_slot_delay_mem[128 * 1024];

static int
slot_cmd_byte(int slot, int param, bool slide) {
  return (slide ? mix::kFxplSlotSlide : mix::kFxplSlotSet) + (slot << 3) + param;
}

static bool
about(float a, float b) {
  const float d = a - b;
  return (d < 0.f ? -d : d) < 1.f / 4096.f;
}

// Every slot off and every parameter zero, so a test sees only what it wrote.
static void
slot_clear(void) {
  for (int c = 0; c < kMaxChannels; ++c) {
    for (int i = 0; i < 8; ++i)
      g_mixer.insert[c].param[i] = 0.f;
    mix::slot_set_kind(&g_mixer.insert[c], mix::FxKind::kNone);
    for (int s = 0; s < mix::kSends; ++s)
      g_mixer.send_level[c][s] = 0.f;
  }
  for (int s = 0; s < mix::kSends; ++s) {
    for (int i = 0; i < 8; ++i)
      g_mixer.send[s].param[i] = 0.f;
    mix::slot_set_kind(&g_mixer.send[s], mix::FxKind::kNone);
  }
  for (int i = 0; i < 8; ++i)
    g_mixer.master_fx.param[i] = 0.f;
  mix::slot_set_kind(&g_mixer.master_fx, mix::FxKind::kNone);
  g_mixer.width = 1.f;
}

// Renders the macro-test module without touching the slots, which is what
// separates this from `mac_render`: the slot configuration *is* the thing under
// test here.
static void
slot_render(int ticks, double *out) {
  player_start(&g_mac_player, &g_mac);
  g_mixer.master_gain = g_mac_player.gain;
  mix::mixer_reset(&g_mixer);
  const int frames = kTickFrames * ticks;
  memset(out, 0, (size_t) frames * 2u * sizeof(double));
  mix::mixer_render_add(&g_mixer, &g_mac_player, out, frames, 2, 48000.f);
}

// **The parameter is 0..1 and the effect is not.** A `u8` in a pattern cannot
// carry seconds, so the slot layer owns the conversion -- and the two real-unit
// ceilings are powers of two precisely so a caller's division by them is exact
// and a pinned tune keeps the delay time it was blessed with.
static void
test_slot_units_are_normalised() {
  printf("a normalised slot parameter reaches the effect in its own units\n");

  mix::Slot d;
  CHECK(fx::delay_bytes_needed(0.4f, 48000.f) <= sizeof g_slot_delay_mem);
  CHECK(fx::delay_init(&d.delay, g_slot_delay_mem, sizeof g_slot_delay_mem,
                       48000.f, 0.4f));
  d.param[0] = 0.17f / mix::kSlotDelayMaxSeconds;
  d.param[3] = 1.f;                   // all wet, or the line is a passthrough
  mix::slot_set_kind(&d, mix::FxKind::kDelay);
  mix::slot_apply_params(&d, 48000.f);
  CHECK(d.delay.time_target == 0.17f * 48000.f);
  CHECK(d.delay.mix == 1.f);
  // The ping-pong flag is a discrete parameter on the same 0..1: it flips at
  // the halfway mark rather than on any particular value.
  CHECK(!d.delay.ping_pong);
  d.param[4] = 0.51f;
  mix::slot_apply_params(&d, 48000.f);
  CHECK(d.delay.ping_pong);

  mix::Slot r;
  CHECK(fx::reverb_bytes_needed(48000.f, 0.25f) <= sizeof g_slot_reverb_mem);
  CHECK(fx::reverb_init(&r.reverb, g_slot_reverb_mem, sizeof g_slot_reverb_mem,
                        48000.f, 0.25f));
  r.param[2] = 0.02f / mix::kSlotPredelayMaxSeconds;
  r.param[4] = 1.f;
  mix::slot_set_kind(&r, mix::FxKind::kReverb);
  mix::slot_apply_params(&r, 48000.f);
  CHECK(r.reverb.predelay == 960);    // 0.02 s at 48 kHz, to the sample

  // The cutoff is exponential across 20 Hz..20 kHz, which is the plane's own
  // curve -- a linear sweep in hertz crawls at the bottom and leaps at the top.
  // Monotonic is the property a slide depends on, so that is what is checked.
  mix::Slot f;
  f.param[0] = mix::slot_choice_param((int) mix::FilterMode::kLowpass, 3);
  f.param[1] = 0.25f;
  f.param[2] = 0.3f;
  mix::slot_set_kind(&f, mix::FxKind::kFilter);
  mix::slot_apply_params(&f, 48000.f);
  const float low = f.svf[0].a1;
  f.param[1] = 0.75f;
  mix::slot_apply_params(&f, 48000.f);
  CHECK(f.svf[0].a1 != low);
  // Zero is off rather than the bottom of the range, which is what keeps a
  // default-constructed filter slot from silencing the channel it is put on.
  CHECK(mix::slot_filter_on(&f));
  f.param[1] = 0.f;
  CHECK(!mix::slot_filter_on(&f));

  // And the discrete parameters round-trip through their own band.
  for (int k = 0; k < 4; ++k)
    CHECK(mix::slot_choice(mix::slot_choice_param(k, 4), 4) == k);
  for (int k = 0; k < 3; ++k)
    CHECK(mix::slot_choice(mix::slot_choice_param(k, 3), 3) == k);
  CHECK(mix::slot_choice(0.f, 4) == 0);
  CHECK(mix::slot_choice(1.f, 4) == 3);     // the top lands on the last, not past it
  CHECK(mix::slot_choice(-1.f, 4) == 0);
}

// One set and one slide on each of the three kinds of slot, through the lane
// each of them is reachable from.
static void
test_slot_commands_set_and_slide() {
  printf("a slot parameter is set and slid from the plane\n");

  mac_build(2, 2);
  slot_clear();
  mac_chan(0, 2, 0, slot_cmd_byte(mix::kSlotInsert, 3, false), 255);
  mac_meta(0, 0, slot_cmd_byte(mix::kSlotSend + 1, 4, false), 128);
  mac_meta(0, 1, slot_cmd_byte(mix::kSlotMaster, 1, false), 64);
  slot_render(2, g_a);
  CHECK(g_mixer.insert[2].param[3] == 1.f);
  CHECK(about(g_mixer.send[1].param[4], 128.f / 255.f));
  CHECK(about(g_mixer.master_fx.param[1], 64.f / 255.f));
  // And nothing else moved: the pair is packed into the command byte, so a
  // decode off by one bit would land on a neighbour.
  CHECK(g_mixer.insert[2].param[2] == 0.f);
  CHECK(g_mixer.insert[3].param[3] == 0.f);
  CHECK(g_mixer.send[0].param[4] == 0.f);
  CHECK(g_mixer.send[1].param[3] == 0.f);

  // A slide beside the set, in a second lane of the same row: five ticks of
  // step on a row at speed 6, exactly as every other slide in the plane.
  mac_build(2, 2);
  slot_clear();
  mac_chan(0, 2, 0, slot_cmd_byte(mix::kSlotInsert, 3, false), 100);
  mac_chan(0, 2, 1, slot_cmd_byte(mix::kSlotInsert, 3, true), 10);
  mac_meta(0, 0, slot_cmd_byte(mix::kSlotSend + 1, 4, false), 100);
  mac_meta(0, 1, slot_cmd_byte(mix::kSlotSend + 1, 4, true), 0xf6);  // -10
  slot_render(6, g_a);
  CHECK(about(g_mixer.insert[2].param[3], (100.f + 5.f * 10.f) / 255.f));
  CHECK(about(g_mixer.send[1].param[4], (100.f - 5.f * 10.f) / 255.f));

  // A slide is clamped to the normalised range on the store, not left to run
  // off the end -- a shaper kind out of range or a mix above unity is what a
  // runaway would look like downstream.
  mac_build(1, 1);
  slot_clear();
  for (int row = 0; row < kMacRows; ++row)
    mac_chan(row, 0, 0, slot_cmd_byte(mix::kSlotInsert, 0, true), 127);
  slot_render(kMacRows * 6, g_a);
  CHECK(g_mixer.insert[0].param[0] == 1.f);
}

// **A slot id says which lane may carry it, and it is a range check rather than
// anything about what the parameter means** -- so the format keeps its opacity
// and still cannot express the master-gain wart with a reverb in it.
static void
test_slot_lane_restrictions() {
  printf("a send from a channel lane and an insert from a meta lane are "
         "ignored\n");

  // The baseline: an ordinary plane, so this is not comparing two silences.
  mac_build(1, 1);
  slot_clear();
  mac_chan(0, 0, 0, 0x01, 200);
  slot_render(6, g_a);

  // The same plane with two slot commands added that have no referent. **Bit
  // for bit**, which is the same standard the whole library holds "ignored" to.
  mac_build(1, 1);
  slot_clear();
  mac_chan(0, 0, 0, 0x01, 200);
  mac_chan(0, 1, 0, slot_cmd_byte(mix::kSlotSend + 0, 0, false), 255);
  mac_meta(0, 0, slot_cmd_byte(mix::kSlotInsert, 0, false), 255);
  slot_render(6, g_b);
  CHECK(memcmp(g_a, g_b, sizeof(double) * (size_t) (kTickFrames * 6) * 2u) == 0);
  CHECK(g_mixer.send[0].param[0] == 0.f);
  for (int c = 0; c < kMacChannels; ++c)
    CHECK(g_mixer.insert[c].param[0] == 0.f);

  // The spare slot ids are ignored from either lane, which is what keeps 6 and
  // 7 free to mean something later.
  mac_build(1, 1);
  slot_clear();
  mac_chan(0, 0, 0, 0x01, 200);
  mac_chan(0, 1, 0, slot_cmd_byte(6, 0, false), 255);
  mac_meta(0, 0, slot_cmd_byte(7, 0, false), 255);
  slot_render(6, g_b);
  CHECK(memcmp(g_a, g_b, sizeof(double) * (size_t) (kTickFrames * 6) * 2u) == 0);
}

// The widened macro target range, which is the same numbering as the set
// commands and needed no second accumulator: `Slot::param` already persists
// across ticks the way `fxpl_val` does.
static void
test_slot_macro_targets() {
  printf("a macro reaches a slot parameter, and a global slot only once\n");

  mac_build(1, 1);
  slot_clear();
  mac_target(1, 0, slot_cmd_byte(mix::kSlotSend + 2, 1, false),
             kMacroScopeAll, 256, 0);
  mac_target(1, 1, slot_cmd_byte(mix::kSlotInsert, 2, false),
             kMacroScopeAll, 128, 0);
  mac_meta(0, 0, 1, 200);
  slot_render(2, g_a);
  CHECK(about(g_mixer.send[2].param[1], 200.f / 255.f));
  for (int c = 0; c < kMacChannels; ++c)
    CHECK(about(g_mixer.insert[c].param[2], 100.f / 255.f));
  // An insert walks `scope`, so a channel the scope does not name is untouched.
  CHECK(g_mixer.insert[kMacChannels].param[2] == 0.f);

  // **A delta macro on a global slot steps once a tick, whatever the scope.** A
  // send is one thing, not one per channel: summing it across four channels is
  // the master-gain wart with a reverb size in it, and would land on 4x here.
  mac_build(1, 1);
  slot_clear();
  g_mac.macros[0].flags = kMacroDelta;
  mac_target(1, 0, slot_cmd_byte(mix::kSlotSend + 2, 1, false),
             kMacroScopeAll, 256, 0);
  mac_meta(0, 0, 1, 10);
  slot_render(6, g_a);
  CHECK(about(g_mixer.send[2].param[1], 5.f * 10.f / 255.f));

  // An insert target under the same delta macro *does* sum per channel, because
  // each channel has its own.
  mac_build(1, 1);
  slot_clear();
  g_mac.macros[0].flags = kMacroDelta;
  mac_target(1, 0, slot_cmd_byte(mix::kSlotInsert, 1, false),
             kMacroScopeAll, 256, 0);
  mac_meta(0, 0, 1, 10);
  slot_render(6, g_a);
  for (int c = 0; c < kMacChannels; ++c)
    CHECK(about(g_mixer.insert[c].param[1], 5.f * 10.f / 255.f));
}

// **The resync gap `Slot::base` exists to close.** `fxpl_sync` re-seeds the
// plane's values from the knobs they write, and a slot parameter has no such
// knob -- so without a base a pattern would eat the caller's configuration for
// good, and a seek backwards would not give it back.
static void
test_slot_resync_restores_the_configuration() {
  printf("a resync puts a slot back to its configuration, not the pattern's "
         "value\n");

  mac_build(1, 1);
  slot_clear();
  g_mixer.send[0].param[0] = 0.25f;
  mix::slot_hold(&g_mixer.send[0]);   // the caller's configuration
  mac_meta(0, 0, slot_cmd_byte(mix::kSlotSend + 0, 0, false), 255);
  slot_render(2, g_a);
  CHECK(g_mixer.send[0].param[0] == 1.f);      // the pattern won, as it should
  CHECK(g_mixer.send[0].base[0] == 0.25f);     // and the setting survived it

  mix::fxpl_sync(&g_mixer, &g_mac_player);
  CHECK(g_mixer.send[0].param[0] == 0.25f);

  // The same through the render path: a restart re-seeds, so a row that says
  // nothing about the slot leaves it at the configuration rather than at
  // whatever the previous pass pushed it to.
  mac_build(1, 1);
  slot_clear();
  g_mixer.send[0].param[0] = 0.25f;
  mix::slot_hold(&g_mixer.send[0]);
  mac_meta(1, 0, slot_cmd_byte(mix::kSlotSend + 0, 0, false), 255);
  slot_render(6, g_a);                // row 0 only, which says nothing
  CHECK(g_mixer.send[0].param[0] == 0.25f);
  slot_render(12, g_a);               // rows 0 and 1
  CHECK(g_mixer.send[0].param[0] == 1.f);
}

// **The dirty check, and the bug it invites.** `slot_process_*` calls
// `slot_apply_params` every block, so an idle slot re-derived the same
// coefficients a thousand times a second; skipping that is free. What is not
// free is forgetting the kind: a slot whose parameters are all zero would match
// a cleared `applied` array and the new effect would run on the old one's
// numbers.
static void
test_slot_apply_skips_and_fires() {
  printf("an unmoved slot skips its setter, and a kind change does not\n");

  mix::Slot f;
  f.param[0] = mix::slot_choice_param((int) mix::FilterMode::kLowpass, 3);
  f.param[1] = 0.5f;
  f.param[2] = 0.3f;
  mix::slot_set_kind(&f, mix::FxKind::kFilter);
  mix::slot_apply_params(&f, 48000.f);

  // Poked behind the setter's back, which is the only way to see a skip: if the
  // second apply ran, it would put the coefficient back.
  f.svf[0].a1 = -1.f;
  mix::slot_apply_params(&f, 48000.f);
  CHECK(f.svf[0].a1 == -1.f);

  f.param[1] = 0.6f;
  mix::slot_apply_params(&f, 48000.f);
  CHECK(f.svf[0].a1 != -1.f);

  // A rate that moves is a coefficient that moves, so it counts as dirty too.
  f.svf[0].a1 = -1.f;
  mix::slot_apply_params(&f, 44100.f);
  CHECK(f.svf[0].a1 != -1.f);

  // The trap, at the one setting that reaches it: every parameter zero, so
  // nothing in `param` distinguishes the state before the kind change from the
  // state after it.
  mix::Slot z;
  CHECK(fx::delay_init(&z.delay, g_slot_delay_mem, sizeof g_slot_delay_mem,
                       48000.f, 0.4f));
  mix::slot_set_kind(&z, mix::FxKind::kDelay);
  mix::slot_apply_params(&z, 48000.f);
  z.delay.mix = 0.5f;
  mix::slot_set_kind(&z, mix::FxKind::kDelay);
  mix::slot_apply_params(&z, 48000.f);
  CHECK(z.delay.mix == 0.f);
}

// **A slide on a discrete parameter is useful, which is why there is no
// discrete/continuous distinction to draw.** The format is opaque about what a
// parameter means and the meaning depends on `kind`, which is runtime state --
// so neither a load refusal nor an ignore-at-use is even expressible. A slide
// is a per-tick delta on a float; what the float means is the slot's business.
static void
test_slot_slide_steps_a_shaper_through_its_kinds() {
  printf("a slide walks a shaper through every one of its kinds\n");

  mac_build(1, 1);
  slot_clear();
  g_mixer.insert[0].param[1] = 0.5f;            // drive, so it is audible
  mix::slot_set_kind(&g_mixer.insert[0], mix::FxKind::kShape);
  for (int row = 0; row < kMacRows; ++row)
    mac_chan(row, 0, 0, slot_cmd_byte(mix::kSlotInsert, 0, true), 26);

  player_start(&g_mac_player, &g_mac);
  g_mixer.master_gain = g_mac_player.gain;
  mix::mixer_reset(&g_mixer);

  int seen[4] = {0, 0, 0, 0};
  for (int k = 0; k < kMacRows * 6; ++k) {
    memset(g_a, 0, (size_t) kTickFrames * 2u * sizeof(double));
    mix::mixer_render_add(&g_mixer, &g_mac_player, g_a, kTickFrames, 2,
                          48000.f);
    seen[mix::slot_choice(g_mixer.insert[0].param[0], 4)] = 1;
  }
  CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
}

// ---------------------------------------------------------------------------
// The pretty-printer's mixer half: the slot range resolved against a live
// `Mixer`, the lane rules, and a macro's targets. ntrk.h's half — ProTracker's
// column and the player's 0x30/0x31 — is checked in test_ntrk.cc.
// ---------------------------------------------------------------------------

static Module g_desc_mod;

static void
desc_build_module() {
  g_desc_mod = Module();
  g_desc_mod.channels = 4;
  g_desc_mod.macro_count = 2;

  // Absolute, three targets, one of each shape a target can be: an `fxpl_val`
  // index, a global slot parameter and a per-channel insert one.
  Macro &a = g_desc_mod.macros[0];
  a.target_count = 3;
  a.flags = 0;
  a.targets[0].target = (uint8_t) mix::kFxplPan;
  a.targets[0].scope = kMacroScopeAll;
  a.targets[0].scale = 0x0100;          // unity
  a.targets[1].target = 0x49;           // send 2, param 1
  a.targets[1].scope = 3;
  a.targets[1].scale = 0x0080;          // a half
  a.targets[2].target = 0x69;           // insert, param 1
  a.targets[2].scope = 2;
  a.targets[2].scale = 0x0100;

  Macro &d = g_desc_mod.macros[1];
  d.target_count = 1;
  d.flags = kMacroDelta;
  d.targets[0].target = (uint8_t) (mix::kFxplSend + 1);
  d.targets[0].scope = kMacroScopeSelf;
  d.targets[0].scale = -0x0020;
}

static void
desc_build_mixer(mix::Mixer *mx) {
  // `mixer_reset` is the "stop the sound" call and keeps the patch, so the
  // kinds an earlier test chose survive it. These tests read the kinds, so
  // every slot is named here rather than inherited.
  mix::mixer_reset(mx);
  for (int i = 0; i < mix::kSends; ++i)
    mx->send[i].kind = mix::FxKind::kNone;
  for (int i = 0; i < kMaxChannels; ++i)
    mx->insert[i].kind = mix::FxKind::kNone;
  mx->send[0].kind = mix::FxKind::kDelay;
  mx->send[1].kind = mix::FxKind::kReverb;
  mx->master_fx.kind = mix::FxKind::kShape;
  mx->insert[2].kind = mix::FxKind::kFilter;
}

static FxCell
desc_cell(uint8_t cmd, uint8_t param) {
  FxCell c;
  c.cmd = cmd;
  c.param = param;
  return c;
}

static void
test_describe_slot_params() {
  printf("a slot parameter names itself from the kind loaded in the slot\n");
  desc_build_mixer(&g_mixer);
  char b[192];

  // **The whole point of the split.** With a mixer the slot's own kind names
  // the parameter; without one the index form is what the format itself knows,
  // and it is correct rather than a placeholder.
  mix::fxpl_describe(&g_mixer, nullptr, -1, desc_cell(0x49, 0x80), b, sizeof b);
  CHECK(strcmp(b, "Send 2 (Reverb) Damping -> 0.50") == 0);
  mix::fxpl_describe(nullptr, nullptr, -1, desc_cell(0x49, 0x80), b, sizeof b);
  CHECK(strcmp(b, "Send 2 param 1 -> 0.50") == 0);

  // The same parameter index means something else behind a different kind,
  // which is the opacity this function exists to see through.
  mix::fxpl_describe(&g_mixer, nullptr, -1, desc_cell(0x41, 0x80), b, sizeof b);
  CHECK(strcmp(b, "Send 1 (Delay) Feedback -> 0.50") == 0);

  // An insert resolves against the lane's own channel, and only there.
  mix::fxpl_describe(&g_mixer, nullptr, 2, desc_cell(0x69, 0xff), b, sizeof b);
  CHECK(strcmp(b, "Insert (Filter) Cutoff -> 1.00") == 0);
  mix::fxpl_describe(&g_mixer, nullptr, 0, desc_cell(0x69, 0xff), b, sizeof b);
  CHECK(strcmp(b, "Insert (None) param 1 -> 1.00") == 0);

  // A spare parameter index is not a gap: an effect may grow a knob, and until
  // it does, "param 6" is the right thing to say about one.
  mix::fxpl_describe(&g_mixer, nullptr, 2, desc_cell(0x6e, 0x00), b, sizeof b);
  CHECK(strcmp(b, "Insert (Filter) param 6 -> 0.00") == 0);

  // A slide is a signed per-tick step, and the sign is always shown.
  mix::fxpl_describe(&g_mixer, nullptr, -1, desc_cell(0x89, 0x80), b, sizeof b);
  CHECK(strcmp(b, "Send 2 (Reverb) Damping -0.50/tick") == 0);
  mix::fxpl_describe(&g_mixer, nullptr, -1, desc_cell(0x89, 0x40), b, sizeof b);
  CHECK(strcmp(b, "Send 2 (Reverb) Damping +0.25/tick") == 0);

  // **The lane rules, said rather than left to be discovered.** A send written
  // into a channel lane looks exactly like a command that works.
  mix::fxpl_describe(&g_mixer, nullptr, 2, desc_cell(0x49, 0x80), b, sizeof b);
  CHECK(strstr(b, "meta-lane only, ignored") != nullptr);
  mix::fxpl_describe(&g_mixer, nullptr, -1, desc_cell(0x69, 0x80), b, sizeof b);
  CHECK(strstr(b, "names no channel here, ignored") != nullptr);
  mix::fxpl_describe(&g_mixer, nullptr, 2, desc_cell(0x76, 0x80), b, sizeof b);
  CHECK(strstr(b, "spare slot, ignored") != nullptr);

  // The mixer's own range, and its "parameter zero means the last one" memory,
  // which must not be printed as a 0.00 the player never uses.
  mix::fxpl_describe(&g_mixer, nullptr, 0, desc_cell(0x01, 0x80), b, sizeof b);
  CHECK(strcmp(b, "Set pan -> 0.50") == 0);
  mix::fxpl_describe(&g_mixer, nullptr, 0, desc_cell(0x01, 0x00), b, sizeof b);
  CHECK(strcmp(b, "Set pan -> (last parameter)") == 0);
  mix::fxpl_describe(&g_mixer, nullptr, 0, desc_cell(0x02, 0x00), b, sizeof b);
  CHECK(strcmp(b, "Pan slide (last step)") == 0);
  mix::fxpl_describe(&g_mixer, nullptr, 0, desc_cell(0x12, 0xff), b, sizeof b);
  CHECK(strcmp(b, "Set send 3 level -> 1.00") == 0);
  mix::fxpl_describe(&g_mixer, nullptr, 0, desc_cell(0x17, 0x01), b, sizeof b);
  CHECK(strcmp(b, "Send 4 slide +0.00/tick") == 0);
  mix::fxpl_describe(&g_mixer, nullptr, 0, desc_cell(0x09, 0x02), b, sizeof b);
  CHECK(strcmp(b, "Set filter type -> bandpass") == 0);

  // Delegated, not repeated: the player owns 0x30..0x3F and this must be the
  // same sentence ntrk.h produces, or the two halves have started to drift.
  char p[64];
  ntrk::fxpl_player_describe(0x31, 0x08, p, sizeof p);
  mix::fxpl_describe(&g_mixer, nullptr, 0, desc_cell(0x31, 0x08), b, sizeof b);
  CHECK(strcmp(b, p) == 0);
}

static void
test_describe_macro() {
  printf("a macro invocation lists its targets\n");
  desc_build_mixer(&g_mixer);
  desc_build_module();
  char b[256];

  // **A macro's targets are the part nothing else can show**, which is the
  // whole reason a meta cell needs describing at all.
  mix::macro_describe(&g_mixer, &g_desc_mod, 1, 128, b, sizeof b);
  CHECK(strcmp(b,
               "Macro 1 (absolute), input 128: Pan [all] -> 0.50, "
               "Send 2 (Reverb) Damping [global] -> 0.25, "
               "Insert (Filter) Cutoff [ch2] -> 0.50") == 0);

  // No mixer: every slot target degrades, and the `fxpl_val` ones do not —
  // those the format does know.
  mix::macro_describe(nullptr, &g_desc_mod, 1, 128, b, sizeof b);
  CHECK(strcmp(b,
               "Macro 1 (absolute), input 128: Pan [all] -> 0.50, "
               "Send 2 param 1 [global] -> 0.25, "
               "Insert param 1 [ch2] -> 0.50") == 0);

  // A delta macro is a slide, so its targets read as per-tick steps.
  mix::macro_describe(&g_mixer, &g_desc_mod, 2, 200, b, sizeof b);
  CHECK(strcmp(b, "Macro 2 (delta), input 200: Send 2 level [self] "
                  "-0.10/tick") == 0);

  // Reached through the entry point, which is how an editor gets here: a meta
  // lane's first byte below 0x40 is a macro index, and cannot collide with a
  // slot command because a macro index is 1..32.
  char m[256];
  mix::fxpl_describe(&g_mixer, &g_desc_mod, -1, desc_cell(1u, 128u), m,
                     sizeof m);
  mix::macro_describe(&g_mixer, &g_desc_mod, 1, 128, b, sizeof b);
  CHECK(strcmp(b, m) == 0);
  // The same byte in a channel lane is a command instead, not a macro.
  mix::fxpl_describe(&g_mixer, &g_desc_mod, 0, desc_cell(1u, 128u), m,
                     sizeof m);
  CHECK(strcmp(m, "Set pan -> 0.50") == 0);

  // A macro past the table is ignored where it is used rather than refused at
  // load, so this is a cell an editor really does see.
  mix::macro_describe(&g_mixer, &g_desc_mod, 9, 12, b, sizeof b);
  CHECK(strcmp(b, "Macro 9 (not in the table), input 12") == 0);
  mix::macro_describe(&g_mixer, nullptr, 1, 12, b, sizeof b);
  CHECK(strcmp(b, "Macro 1 (not in the table), input 12") == 0);
  mix::macro_describe(&g_mixer, &g_desc_mod, 0, 12, b, sizeof b);
  CHECK(strcmp(b, "None") == 0);

  // An empty target list is legal, and is what an editor holds while somebody
  // is still filling one in.
  g_desc_mod.macros[0].target_count = 0;
  mix::macro_describe(&g_mixer, &g_desc_mod, 1, 12, b, sizeof b);
  CHECK(strcmp(b, "Macro 1 (absolute), input 12: no targets") == 0);
  g_desc_mod.macros[0].target_count = 3;
}


static void
test_fxpl_mnemonic() {
  printf("every plane command has a three-letter name\n");
  char m[4];
  ntrk::FxCell cell;

  cell.cmd = 0u;              ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "...") == 0);
  cell.cmd = 0x01u;           ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "PAN") == 0);
  cell.cmd = 0x02u;           ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "PNS") == 0);
  cell.cmd = 0x05u;           ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "CUT") == 0);
  // Sends are numbered, not named after the effect in the slot: the kind is a
  // mixer setting, and names that moved when a send was reassigned would make
  // the grid unreadable.
  cell.cmd = 0x10u;           ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "SN1") == 0);
  cell.cmd = 0x13u;           ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "SN4") == 0);
  cell.cmd = 0x14u;           ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "SS1") == 0);
  cell.cmd = 0x30u;           ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "ACC") == 0);
  cell.cmd = 0x31u;           ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "GLI") == 0);
  // Slot set and slide, `slot << 3 | param`: slot 1 param 2, both ranges.
  cell.cmd = (uint8_t) (0x40u + (1u << 3 | 2u));
  ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "S12") == 0);
  cell.cmd = (uint8_t) (0x80u + (1u << 3 | 2u));
  ntrk::mix::fxpl_mnemonic(cell, m); CHECK(strcmp(m, "D12") == 0);

  // Three characters for every byte, so no cell is ever ragged.
  bool all_three = true;
  for (int c = 0; c < 256; ++c) {
    cell.cmd = (uint8_t) c;
    m[0] = m[1] = m[2] = m[3] = '#';
    ntrk::mix::fxpl_mnemonic(cell, m);
    if (strlen(m) != 3)
      all_three = false;
  }
  CHECK(all_three);
}

// ---------------------------------------------------------------------------
// The three reverb knobs and the shaper's dry blend, which live in indices that
// were spare. The claim each of them has to carry is the same one: audible when
// driven, and *bit for bit* absent when not -- because every module written
// before they existed stores a zero there.
// ---------------------------------------------------------------------------

static const int kRevFrames = 96 * 256;         // two seconds at 48 kHz
static float g_rev_mem_a[64 * 1024];
static float g_rev_mem_b[64 * 1024];
static float g_rev_l[kRevFrames];
static float g_rev_r[kRevFrames];
static float g_rev_base_l[kRevFrames];
static float g_rev_base_r[kRevFrames];

// An impulse in, two seconds of tail out, through the slot layer. Rendered in
// blocks because that is how `mixer_render_add` calls it, and the parameter
// cache turns over at a block boundary.
static bool
reverb_slot_render(const float p[8], float *mem, size_t bytes, float *l,
                   float *r) {
  mix::Slot s;
  if (!fx::reverb_init(&s.reverb, mem, bytes, 48000.f, 0.25f))
    return false;
  for (int i = 0; i < 8; ++i)
    s.param[i] = p[i];
  mix::slot_set_kind(&s, mix::FxKind::kReverb);
  for (int i = 0; i < kRevFrames; ++i)
    l[i] = r[i] = 0.f;
  l[0] = r[0] = 1.f;
  for (int at = 0; at < kRevFrames; at += 256)
    mix::slot_process_stereo(&s, l + at, r + at, 256, 48000.f);
  return true;
}

static double
tail_energy(const float *x, int from) {
  double e = 0.0;
  for (int i = from; i < kRevFrames; ++i)
    e += (double) x[i] * (double) x[i];
  return e;
}

static bool
rev_same(const float *a, const float *b) {
  return memcmp(a, b, sizeof(float) * (size_t) kRevFrames) == 0;
}

static void
test_reverb_slot_late_knobs() {
  printf("the reverb's decay, LF damping and diffusion move the tail, and "
         "their zero does not\n");

  // size, damping, predelay, width, mix, then the three that were spare.
  float p[8] = {0.6f, 0.2f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f};
  CHECK(reverb_slot_render(p, g_rev_mem_a, sizeof g_rev_mem_a, g_rev_base_l,
                           g_rev_base_r));
  CHECK(tail_energy(g_rev_base_l, 0) > 0.0);      // the tank is not silent

  // **The claim the whole change rests on**: a slot with the three at zero is
  // the five-knob `reverb_set` this used to call, sample for sample.
  {
    fx::Reverb ref;
    CHECK(fx::reverb_init(&ref, g_rev_mem_b, sizeof g_rev_mem_b, 48000.f,
                          0.25f));
    fx::reverb_set(&ref, p[0], p[1], p[2] * mix::kSlotPredelayMaxSeconds, p[3],
                   p[4]);
    for (int i = 0; i < kRevFrames; ++i)
      g_rev_l[i] = g_rev_r[i] = 0.f;
    g_rev_l[0] = g_rev_r[0] = 1.f;
    for (int at = 0; at < kRevFrames; at += 256)
      fx::reverb_process(&ref, g_rev_l + at, g_rev_r + at, 256);
    CHECK(rev_same(g_rev_l, g_rev_base_l));
    CHECK(rev_same(g_rev_r, g_rev_base_r));
  }

  // And zero means exactly the derivation, not merely something near it:
  // writing the derived values into the three knobs changes nothing at all.
  {
    float q[8];
    for (int i = 0; i < 8; ++i)
      q[i] = p[i];
    fx::reverb_derived(p[0], &q[5], &q[6], &q[7]);
    CHECK(q[5] > 0.f && q[6] > 0.f && q[7] > 0.f);
    CHECK(reverb_slot_render(q, g_rev_mem_b, sizeof g_rev_mem_b, g_rev_l,
                             g_rev_r));
    CHECK(rev_same(g_rev_l, g_rev_base_l));
    CHECK(rev_same(g_rev_r, g_rev_base_r));
  }

  // Decay is RT60 and nothing else, so a long one is a louder tail a second in.
  const double base_late = tail_energy(g_rev_base_l, kRevFrames / 2);
  {
    float q[8];
    for (int i = 0; i < 8; ++i)
      q[i] = p[i];
    q[5] = 0.9f;
    CHECK(reverb_slot_render(q, g_rev_mem_b, sizeof g_rev_mem_b, g_rev_l,
                             g_rev_r));
    CHECK(tail_energy(g_rev_l, kRevFrames / 2) > base_late * 2.0);
    q[5] = 0.05f;
    CHECK(reverb_slot_render(q, g_rev_mem_b, sizeof g_rev_mem_b, g_rev_l,
                             g_rev_r));
    CHECK(tail_energy(g_rev_l, kRevFrames / 2) < base_late * 0.05);
  }

  // The other two change the tail's character rather than its length, so what
  // is checked is that they are heard at all -- and that each is heard on its
  // own, which is what says the two are wired to different knobs.
  {
    float q[8];
    for (int i = 0; i < 8; ++i)
      q[i] = p[i];
    q[6] = 0.95f;                     // as much bass damping as the shelf has
    CHECK(reverb_slot_render(q, g_rev_mem_b, sizeof g_rev_mem_b, g_rev_l,
                             g_rev_r));
    CHECK(!rev_same(g_rev_l, g_rev_base_l));
    // Bass damping takes energy out of the loop; it cannot put any in.
    CHECK(tail_energy(g_rev_l, kRevFrames / 2) < base_late);

    for (int i = 0; i < 8; ++i)
      q[i] = p[i];
    q[7] = 0.05f;                     // almost no diffusion in the Gardner stages
    CHECK(reverb_slot_render(q, g_rev_mem_b, sizeof g_rev_mem_b, g_rev_l,
                             g_rev_r));
    CHECK(!rev_same(g_rev_l, g_rev_base_l));
  }

  // The names moved into the spare indices with them, and the ones already
  // there did not move.
  CHECK(strcmp(mix::fx_param_name(mix::FxKind::kReverb, 0), "Size") == 0);
  CHECK(strcmp(mix::fx_param_name(mix::FxKind::kReverb, 1), "Damping") == 0);
  CHECK(strcmp(mix::fx_param_name(mix::FxKind::kReverb, 2), "Predelay") == 0);
  CHECK(strcmp(mix::fx_param_name(mix::FxKind::kReverb, 3), "Width") == 0);
  CHECK(strcmp(mix::fx_param_name(mix::FxKind::kReverb, 4), "Mix") == 0);
  CHECK(strcmp(mix::fx_param_name(mix::FxKind::kReverb, 5), "Decay") == 0);
  CHECK(strcmp(mix::fx_param_name(mix::FxKind::kReverb, 6), "Damping LF") == 0);
  CHECK(strcmp(mix::fx_param_name(mix::FxKind::kReverb, 7), "Diffusion") == 0);
}

static void
test_shaper_dry() {
  printf("a shaper's dry knob blends, and its zero is the wet path bit for "
         "bit\n");

  const int n = 64;
  float in[64], wet[64], half[64], mono[64];
  for (int i = 0; i < n; ++i)
    in[i] = (float) (i - 32) * (1.f / 24.f);   // past full scale at both ends

  mix::Slot s;
  s.param[0] = mix::slot_choice_param((int) fx::ShapeKind::kFold, 4);
  s.param[1] = 0.6f;
  mix::slot_set_kind(&s, mix::FxKind::kShape);

  for (int i = 0; i < n; ++i)
    wet[i] = mono[i] = in[i];
  mix::slot_process_stereo(&s, wet, mono, n, 48000.f);

  // Zero dry is the shaper alone, and the comparison is against the primitive
  // rather than against a remembered array: a blend at a gain of one is not
  // the identity in floating point, which is why the wet path branches.
  bool exact = true;
  for (int i = 0; i < n; ++i)
    exact = exact && wet[i] == fx::shape_sample((fx::ShapeKind) mix::slot_choice(
                                                    s.param[0], 4),
                                                in[i], s.param[1]);
  CHECK(exact);

  // Half dry sits between the two, and closer to the input than the wet path.
  s.param[2] = 0.5f;
  for (int i = 0; i < n; ++i)
    half[i] = mono[i] = in[i];
  mix::slot_process_stereo(&s, half, mono, n, 48000.f);
  double d_wet = 0.0, d_half = 0.0;
  bool between = true;
  for (int i = 0; i < n; ++i) {
    d_wet += (double) (wet[i] - in[i]) * (double) (wet[i] - in[i]);
    d_half += (double) (half[i] - in[i]) * (double) (half[i] - in[i]);
    if (wet[i] != in[i])
      between = between && half[i] != wet[i];
  }
  CHECK(d_half > 0.0 && d_half < d_wet);
  CHECK(between);

  // The mono path is the same knob, not a second one that forgot it.
  s.param[2] = 0.f;
  for (int i = 0; i < n; ++i)
    mono[i] = in[i];
  mix::slot_process_mono(&s, mono, n, 48000.f);
  CHECK(memcmp(mono, wet, sizeof(float) * (size_t) n) == 0);
  s.param[2] = 0.5f;
  for (int i = 0; i < n; ++i)
    mono[i] = in[i];
  mix::slot_process_mono(&s, mono, n, 48000.f);
  CHECK(memcmp(mono, half, sizeof(float) * (size_t) n) == 0);

  CHECK(strcmp(mix::fx_param_name(mix::FxKind::kShape, 2), "Dry") == 0);
}

// ---------------------------------------------------------------------------
// Enumerating commands, which is the question describing a cell cannot answer:
// what may go in this column.
// ---------------------------------------------------------------------------

static void
test_command_enumeration() {
  printf("every command in every lane enumerates, round-trips and renders "
         "bounded\n");
  desc_build_mixer(&g_mixer);

  static const mix::Lane kLanes[3] = {mix::Lane::NoteEffect,
                                      mix::Lane::PlaneChannel,
                                      mix::Lane::PlaneMeta};

  // Sixteen base effects, the sixteen `E` holds and `SLC` past the nibble; the
  // mixer range, the player's two and this channel's insert; every global
  // slot's eight parameters, set and slid.
  CHECK(mix::command_count(mix::Lane::NoteEffect) == 33);
  CHECK(mix::command_count(mix::Lane::PlaneChannel) == 19 + 16);
  CHECK(mix::command_count(mix::Lane::PlaneMeta) == 80);

  bool ok = true;
  int choices = 0;
  int unused = 0;
  for (int li = 0; li < 3; ++li) {
    const mix::Lane lane = kLanes[li];
    const int n = mix::command_count(lane);

    mix::CommandInfo edge;
    CHECK(!mix::command_at(lane, -1, &g_mixer, &edge, 2));
    CHECK(!mix::command_at(lane, n, &g_mixer, &edge, 2));
    CHECK(!mix::command_at(lane, 0, &g_mixer, nullptr, 2));

    for (int i = 0; i < n; ++i) {
      mix::CommandInfo a, b;
      if (!mix::command_at(lane, i, &g_mixer, &a, 2)) {
        ok = false;
        continue;
      }
      ok = ok && a.lane == lane && strlen(a.mnemonic) == 3 && a.name[0] != '\0';

      // **Round-trip**: the byte an editor stores finds the same command back.
      ok = ok && mix::command_lookup(lane, a.cmd, &g_mixer, &b, 2) &&
           b.cmd == a.cmd && b.lane == a.lane &&
           strcmp(b.mnemonic, a.mnemonic) == 0 &&
           strcmp(b.name, a.name) == 0 && b.shape == a.shape &&
           b.choice_count == a.choice_count && b.choice == a.choice;

      if (a.shape == ParamShape::Choice) {
        ++choices;
        ok = ok && a.choice_count >= 2 && a.choice_count <= 8 &&
             a.choice != nullptr;
        // Every choice names itself, and no two name the same thing -- a
        // dropdown with two identical rows is a dropdown that cannot be used.
        char names[8][48];
        for (int k = 0; k < a.choice_count && k < 8; ++k) {
          const size_t m = mix::command_choice_text(&a, k, names[k],
                                                    sizeof names[k]);
          ok = ok && m > 0 && strlen(names[k]) == m;
          for (int j = 0; j < k; ++j)
            ok = ok && strcmp(names[j], names[k]) != 0;
        }
        char junk[48];
        ok = ok && mix::command_choice_text(&a, -1, junk, sizeof junk) == 0;
        ok = ok && mix::command_choice_text(&a, a.choice_count, junk,
                                            sizeof junk) == 0;
      } else {
        ok = ok && a.choice_count == 0 && a.choice == nullptr;
        if (a.shape == ParamShape::Unused)
          ++unused;
      }

      // Every parameter byte renders inside the buffer it was given.
      for (int v = 0; v <= 255; ++v) {
        char big[128];
        memset(big, '#', sizeof big);
        const size_t m =
            mix::command_value_text(&a, (uint8_t) v, big, sizeof big);
        ok = ok && m + 1 < sizeof big && big[m] == '\0' && strlen(big) == m;
        if (a.shape == ParamShape::Unused)
          ok = ok && m == 0;
      }

      // ...and truncates rather than overrunning a small one. One parameter,
      // every cap from nothing up: a zero cap must write no byte at all.
      char full[128];
      const size_t want = mix::command_value_text(&a, 0xA5u, full, sizeof full);
      for (size_t cap = 0; cap <= want + 2 && cap < 64u; ++cap) {
        char buf[96];
        memset(buf, '#', sizeof buf);
        const size_t m = mix::command_value_text(&a, 0xA5u, buf, cap);
        if (cap == 0) {
          ok = ok && m == 0 && buf[0] == '#';
          continue;
        }
        ok = ok && m < cap && buf[m] == '\0' && buf[cap] == '#' &&
             strncmp(buf, full, m) == 0;
      }
    }
  }
  CHECK(ok);
  // The `E` pair, glissando, the plane's filter type, and the three a slot's
  // kind resolves -- a shaper's four, a filter's three and the delay's flag.
  CHECK(choices >= 7);
  CHECK(unused > 0);            // the delay's and the filter's spare indices

  // **A byte a lane does not carry is refused**, which is how a cell is
  // validated as well as offered.
  mix::CommandInfo ci;
  // 0x10 was a byte this lane did not carry until `SLC` took it, so the
  // negative control moves up rather than being dropped: 0x11 is the next one
  // past the nibble that nothing claims, and the point of the case is that a
  // byte outside the set is refused rather than clamped into it.
  CHECK(!mix::command_lookup(mix::Lane::NoteEffect, 0x11u, &g_mixer, &ci));
  CHECK(!mix::command_lookup(mix::Lane::NoteEffect, 0xF0u, &g_mixer, &ci));
  // ...and `SLC` itself is carried, which is the other half of the same claim.
  CHECK(mix::command_lookup(mix::Lane::NoteEffect, 0x10u, &g_mixer, &ci));
  CHECK(!mix::command_lookup(mix::Lane::PlaneChannel, 0x0Au, &g_mixer, &ci, 2));
  CHECK(!mix::command_lookup(mix::Lane::PlaneChannel, 0x40u, &g_mixer, &ci, 2));
  CHECK(!mix::command_lookup(mix::Lane::PlaneMeta, 0x01u, &g_mixer, &ci));
  CHECK(!mix::command_lookup(mix::Lane::PlaneMeta, 0x68u, &g_mixer, &ci));
  CHECK(!mix::command_lookup(mix::Lane::PlaneChannel, 0xC0u, &g_mixer, &ci, 2));

  // The mnemonic is the grid's, and it comes from the same row.
  char m[4];
  CHECK(mix::command_lookup(mix::Lane::NoteEffect, 0x0Cu, nullptr, &ci));
  ntrk::note_fx_mnemonic(0x0Cu, 0u, m);
  CHECK(strcmp(ci.mnemonic, m) == 0 && strcmp(ci.name, "Set volume") == 0);
  CHECK(mix::command_lookup(mix::Lane::NoteEffect, 0xE6u, nullptr, &ci));
  ntrk::note_fx_mnemonic(0x0Eu, 0x64u, m);
  CHECK(strcmp(ci.mnemonic, m) == 0 && strcmp(ci.mnemonic, "LOP") == 0);

  FxCell cell = desc_cell(0x11u, 0u);
  CHECK(mix::command_lookup(mix::Lane::PlaneChannel, 0x11u, &g_mixer, &ci, 2));
  mix::fxpl_mnemonic(cell, m);
  CHECK(strcmp(ci.mnemonic, m) == 0);

  // **The slot resolution is `fxpl_describe`'s, called rather than copied.**
  // With a mixer the kind names the parameter; without one the index form is
  // what the format itself knows, and it is correct rather than a placeholder.
  CHECK(mix::command_lookup(mix::Lane::PlaneMeta, 0x49u, &g_mixer, &ci));
  CHECK(strcmp(ci.name, "Send 2 (Reverb) Damping") == 0);
  CHECK(mix::command_lookup(mix::Lane::PlaneMeta, 0x49u, nullptr, &ci));
  CHECK(strcmp(ci.name, "Send 2 param 1") == 0);
  CHECK(ci.shape == ParamShape::Continuous);      // no kind, so no choice

  // The reverb's new knobs are reachable and named, which is the point of
  // wiring them: 0x40 + (1 << 3 | 7) is send 2's diffusion.
  CHECK(mix::command_lookup(mix::Lane::PlaneMeta, 0x4Fu, &g_mixer, &ci));
  CHECK(strcmp(ci.name, "Send 2 (Reverb) Diffusion") == 0);
  CHECK(ci.shape == ParamShape::Continuous);

  // A choice a kind resolves: the master's shaper, and an insert's filter.
  char b[64];
  CHECK(mix::command_lookup(mix::Lane::PlaneMeta, 0x60u, &g_mixer, &ci));
  CHECK(ci.shape == ParamShape::Choice && ci.choice_count == 4);
  mix::command_value_text(&ci, 255u, b, sizeof b);
  CHECK(strcmp(b, "fold") == 0);
  CHECK(mix::command_lookup(mix::Lane::PlaneChannel, 0x68u, &g_mixer, &ci, 2));
  CHECK(ci.shape == ParamShape::Choice && ci.choice_count == 3);
  mix::command_value_text(&ci, 0u, b, sizeof b);
  CHECK(strcmp(b, "lowpass") == 0);
  // The same command with no channel to resolve it against is a byte.
  CHECK(mix::command_lookup(mix::Lane::PlaneChannel, 0x68u, &g_mixer, &ci, -1));
  CHECK(ci.shape == ParamShape::Continuous);
  // A slide over a choice stays a step: that is what sweeps a shaper through
  // its kinds, and a dropdown is the wrong thing to offer for it.
  CHECK(mix::command_lookup(mix::Lane::PlaneMeta, 0xA0u, &g_mixer, &ci));
  CHECK(ci.shape == ParamShape::Continuous);
  // And a parameter the loaded kind does not read says so.
  CHECK(mix::command_lookup(mix::Lane::PlaneMeta, 0x47u, &g_mixer, &ci));
  CHECK(ci.shape == ParamShape::Unused);
  CHECK(mix::command_value_text(&ci, 128u, b, sizeof b) == 0);

  // A value renders as the value alone -- no name, because the name is already
  // on screen wherever this is drawn.
  CHECK(mix::command_lookup(mix::Lane::NoteEffect, 0x08u, nullptr, &ci));
  mix::command_value_text(&ci, 128u, b, sizeof b);
  CHECK(strcmp(b, "centre") == 0);
  CHECK(mix::command_lookup(mix::Lane::PlaneChannel, 0x02u, &g_mixer, &ci, 0));
  mix::command_value_text(&ci, 0u, b, sizeof b);
  CHECK(strcmp(b, "(last step)") == 0);
  mix::command_value_text(&ci, 0xFFu, b, sizeof b);
  CHECK(strcmp(b, "0.00/tick") == 0);   // -1/255 rounds to nothing at two decimals
  CHECK(mix::command_lookup(mix::Lane::NoteEffect, 0x0Eu, nullptr, &ci));
  mix::command_value_text(&ci, 0x31u, b, sizeof b);
  CHECK(strcmp(b, "GLS on") == 0);            // the container names its own
}

static void
test_describe_command_space() {
  printf("every command number describes itself, and truncates safely\n");
  desc_build_mixer(&g_mixer);
  desc_build_module();
  char b[256];

  // **Every command number, in both kinds of lane.** One CHECK for the sweep:
  // a bug that fires at every number would otherwise bury the run.
  bool all = true;
  int unknown_channel = 0;
  for (int c = 0; c <= 255; ++c) {
    for (int lane = -1; lane <= 0; ++lane) {
      const size_t n = mix::fxpl_describe(&g_mixer, &g_desc_mod, lane,
                                          desc_cell((uint8_t) c, 0x80u), b,
                                          sizeof b);
      all = all && n > 0 && b[0] != '\0' && strlen(b) == n &&
            n < sizeof b - 1;
    }
    // An unknown command says so rather than guessing. In a channel lane those
    // are the gaps the format leaves free: 0x0A..0x0F, 0x18..0x2F, 0xC0..0xFF.
    mix::fxpl_describe(&g_mixer, &g_desc_mod, 0, desc_cell((uint8_t) c, 0u), b,
                       sizeof b);
    if (strncmp(b, "Unknown command 0x", 18) == 0) {
      ++unknown_channel;
      const bool free_range = (c >= 0x0a && c <= 0x0f) ||
                              (c >= 0x18 && c <= 0x2f) || c >= 0xc0;
      all = all && free_range;
    }
  }
  CHECK(all);
  CHECK(unknown_channel == 6 + 24 + 64);

  // Truncation, on the entry point rather than only on ntrk.h's functions:
  // one byte too small still terminates, and a zero cap writes nothing at all.
  char full[256];
  const size_t want = mix::fxpl_describe(&g_mixer, nullptr, -1,
                                         desc_cell(0x49, 0x80), full,
                                         sizeof full);
  bool bounded = true;
  for (size_t cap = 0; cap <= want + 2; ++cap) {
    char buf[288];
    memset(buf, '#', sizeof buf);
    const size_t n = mix::fxpl_describe(&g_mixer, nullptr, -1,
                                        desc_cell(0x49, 0x80), buf, cap);
    if (cap == 0) {
      bounded = bounded && n == 0 && buf[0] == '#';
      continue;
    }
    bounded = bounded && n == (want < cap - 1 ? want : cap - 1) &&
              buf[n] == '\0' && buf[cap] == '#' &&
              strncmp(buf, full, n) == 0;
  }
  CHECK(bounded);

  // The same for a macro, which is the longest thing here by far.
  const size_t mwant =
      mix::macro_describe(&g_mixer, &g_desc_mod, 1, 128, full, sizeof full);
  bool mbounded = true;
  for (size_t cap = 0; cap <= mwant + 2; ++cap) {
    char buf[288];
    memset(buf, '#', sizeof buf);
    const size_t n =
        mix::macro_describe(&g_mixer, &g_desc_mod, 1, 128, buf, cap);
    if (cap == 0) {
      mbounded = mbounded && n == 0 && buf[0] == '#';
      continue;
    }
    mbounded = mbounded && n == (mwant < cap - 1 ? mwant : cap - 1) &&
               buf[n] == '\0' && buf[cap] == '#' &&
               strncmp(buf, full, n) == 0;
  }
  CHECK(mbounded);

  // The name tables refuse to guess past their own range, which is what keeps
  // the degraded form honest at the one place it is produced.
  CHECK(mix::fx_kind_name(mix::FxKind::kReverb) != nullptr);
  CHECK(mix::fx_kind_name((mix::FxKind) 99) == nullptr);
  CHECK(mix::fx_param_name(mix::FxKind::kReverb, 1) != nullptr);
  // The reverb fills all eight now, so the spare-index case is checked on a
  // kind that still has one -- the delay's 5..7.
  CHECK(mix::fx_param_name(mix::FxKind::kReverb, 7) != nullptr);
  CHECK(mix::fx_param_name(mix::FxKind::kDelay, 7) == nullptr);
  CHECK(mix::fx_param_name(mix::FxKind::kDelay, 8) == nullptr);
  CHECK(mix::fx_param_name(mix::FxKind::kNone, 0) == nullptr);
  CHECK(mix::fxpl_slot_name(mix::kSlotInsert) != nullptr);
  CHECK(mix::fxpl_slot_name(6) == nullptr);
  CHECK(mix::fxpl_slot_name(mix::kSlotCount) == nullptr);
  CHECK(mix::fxpl_value_name(mix::kFxplPan) != nullptr);
  CHECK(mix::fxpl_value_name(mix::kFxplValues) == nullptr);
}

// ---------------------------------------------------------------------------
// MIXR: the two things the plane cannot say -- a slot's kind and the stereo
// width -- plus the gain and the parameters it can, kept as the state that
// automation departs from. The block's *carriage* is test_ntrk.cc's; this is
// what the bytes mean.
// ---------------------------------------------------------------------------

static void
mixr_configure(mix::Mixer *mx) {
  mix::mixer_reset(mx);
  mx->master_gain = 1.9f;
  mx->width = 1.25f;

  mx->send[0].param[0] = 0.74f;
  mx->send[0].param[1] = 0.38f;
  mx->send[0].param[4] = 1.f;          // all wet, and it has to survive exactly
  mix::slot_set_kind(&mx->send[0], mix::FxKind::kReverb);

  mx->send[1].param[0] = 0.173f;
  mx->send[1].param[1] = 0.42f;
  mx->send[1].param[4] = 1.f;
  mix::slot_set_kind(&mx->send[1], mix::FxKind::kDelay);

  mx->master_fx.param[0] = 0.6f;
  mix::slot_set_kind(&mx->master_fx, mix::FxKind::kShape);
}

static void
test_mixr_round_trip() {
  printf("the mixer's configuration round trips through a module\n");

  static mix::Mixer a;
  mixr_configure(&a);

  Module m;
  CHECK(!mix::mixer_config_read(&a, &m));      // nothing to read yet
  mix::mixer_config_write(&a, &m);
  CHECK(m.has_mix);

  static mix::Mixer b;
  mix::mixer_reset(&b);
  CHECK(b.master_gain != a.master_gain);       // so the compare below means it
  CHECK(mix::mixer_config_read(&b, &m));

  // Q12 on the gain and the width: within half a step, which is 1/8192.
  CHECK(fabsf(b.master_gain - a.master_gain) < 1.f / 8192.f);
  CHECK(fabsf(b.width - a.width) < 1.f / 8192.f);

  for (int i = 0; i < kMixrSlots; ++i) {
    const mix::Slot *x = mix::mixr_slot_at(&a, i);
    const mix::Slot *y = mix::mixr_slot_at(&b, i);
    CHECK(x->kind == y->kind);
    for (int k = 0; k < kMixrSlotParams; ++k)
      CHECK(fabsf(x->base[k] - y->base[k]) < 1.f / 65534.f);
  }

  // **Exactly 1, not nearly.** A send's mix is all wet, and 0.99998 would put a
  // quiet copy of the dry channel a few samples late into the send bus.
  CHECK(mix::mixr_slot_at(&b, 0)->param[4] == 1.f);

  // The slots the configuration never touched come back empty rather than as
  // whatever the receiving mixer held.
  CHECK(mix::mixr_slot_at(&b, 2)->kind == mix::FxKind::kNone);
  CHECK(mix::mixr_slot_at(&b, 3)->kind == mix::FxKind::kNone);

  // Past the end is null both ways, so a caller cannot walk off the block.
  CHECK(mix::mixr_slot_at(&b, -1) == NULL);
  CHECK(mix::mixr_slot_at(&b, kMixrSlots) == NULL);
}

static void
test_mixr_refuses_an_unknown_kind() {
  printf("a block naming an effect this build has not got is refused whole\n");

  static mix::Mixer a;
  mixr_configure(&a);
  Module m;
  mix::mixer_config_write(&a, &m);

  // The master's kind byte, past the last effect there is.
  m.mix[kMixrHeaderBytes + mix::kSends * kMixrSlotBytes] = 200u;

  static mix::Mixer b;
  mix::mixer_reset(&b);
  const float before = b.master_gain;
  CHECK(!mix::mixer_config_read(&b, &m));

  // **Nothing was applied**, not even the header that parsed cleanly. Half a
  // mixer is a tune that plays, sounds wrong and says nothing about why.
  CHECK(b.master_gain == before);
  CHECK(mix::mixr_slot_at(&b, 0)->kind == mix::FxKind::kNone);

  // A version or slot count the mixer does not know is refused the same way.
  mix::mixer_config_write(&a, &m);
  ntrk::write_u16(m.mix, (uint16_t) (kMixrVersion + 1));
  CHECK(!mix::mixer_config_read(&b, &m));
}

static void
test_mixr_writes_the_setting_not_the_slide() {
  printf("what is saved is the slot's setting, not where a slide left it\n");

  static mix::Mixer a;
  mixr_configure(&a);

  // A slide moves `param` and leaves `base` alone -- that is what a resync
  // restores from. Saving `param` would bake the tune's own automation into the
  // state the next load starts at.
  a.send[0].param[0] = 0.11f;

  Module m;
  mix::mixer_config_write(&a, &m);

  static mix::Mixer b;
  mix::mixer_reset(&b);
  CHECK(mix::mixer_config_read(&b, &m));
  CHECK(fabsf(mix::mixr_slot_at(&b, 0)->base[0] - 0.74f) < 1.f / 65534.f);
}

static void
test_config_slot_kind_reads_without_a_mixer() {
  printf("a slot names its kind without a mixer being built to ask\n");

  static mix::Mixer a;
  mixr_configure(&a);
  Module m;
  mix::mixer_config_write(&a, &m);
  CHECK(m.has_mix);

  // **It agrees with the reader, slot for slot.** That is the whole contract: an
  // editor drawing what a module's sends are must not describe a different
  // mixer from the one the player will build.
  static mix::Mixer b;
  mix::mixer_reset(&b);
  CHECK(mix::mixer_config_read(&b, &m));
  for (int i = 0; i < kMixrSlots; ++i)
    CHECK(mix::config_slot_kind(&m, i) == mix::mixr_slot_at(&b, i)->kind);

  // Out of range, and a module with no block at all: kNone rather than a read
  // past the end or a kind out of uninitialised bytes.
  CHECK(mix::config_slot_kind(&m, -1) == mix::FxKind::kNone);
  CHECK(mix::config_slot_kind(&m, kMixrSlots) == mix::FxKind::kNone);
  Module bare;
  CHECK(mix::config_slot_kind(&bare, 0) == mix::FxKind::kNone);
  CHECK(mix::config_slot_kind((const uint8_t *) 0, 0) == mix::FxKind::kNone);

  // **A block the player would refuse describes nothing.** `mixer_config_read`
  // rejects the whole block when any slot names an effect this build has not
  // got, so reporting a kind out of one would label a tune nobody can hear.
  Module bad = m;
  bad.mix[kMixrHeaderBytes] = 200u;              // slot 0: not a kind we have
  static mix::Mixer c;
  mix::mixer_reset(&c);
  CHECK(!mix::mixer_config_read(&c, &bad));
  for (int i = 0; i < kMixrSlots; ++i)
    CHECK(mix::config_slot_kind(&bad, i) == mix::FxKind::kNone);
}

// ---------------------------------------------------------------------------
// The editor's half: writing a configuration without building a Mixer.
// ---------------------------------------------------------------------------

// A module that never had a mixer is the common case, so an editor has to be
// able to make one. Unity gain and width, or "I added a mixer" would mean "the
// sound went away".
static void
test_config_init_makes_a_readable_block() {
  printf("a fresh config block is readable, at unity gain and width\n");

  Module m;
  CHECK(mix::config_init(m.mix));
  m.has_mix = true;
  CHECK(fabsf(mix::config_master_gain(&m) - 1.f) < 1.f / 1000.f);
  CHECK(fabsf(mix::config_width(&m) - 1.f) < 1.f / 1000.f);
  for (int s = 0; s < kMixrSlots; ++s) {
    CHECK(mix::config_slot_kind(&m, s) == mix::FxKind::kNone);
    for (int p = 0; p < kMixrSlotParams; ++p)
      CHECK(mix::config_slot_param(&m, s, p) == 0.f);
  }
  // And the player accepts it, which is the only thing "readable" can mean.
  static mix::Mixer b;
  mix::mixer_reset(&b);
  CHECK(mix::mixer_config_read(&b, &m));
  CHECK(mix::config_init(nullptr) == false);
}

// Set, then get, at both ends and in between -- for every slot and every
// parameter, because a record offset that was wrong for slot 3 alone is exactly
// what a spot check misses.
static void
test_config_set_round_trips() {
  printf("every slot and parameter set-then-gets, at both bounds\n");

  Module m;
  CHECK(mix::config_init(m.mix));
  m.has_mix = true;

  const mix::FxKind kinds[] = {mix::FxKind::kNone, mix::FxKind::kDelay,
                              mix::FxKind::kReverb};
  for (int s = 0; s < kMixrSlots; ++s) {
    for (const mix::FxKind k : kinds) {
      mix::config_set_slot_kind(m.mix, s, k);
      CHECK(mix::config_slot_kind(&m, s) == k);
    }
    for (int p = 0; p < kMixrSlotParams; ++p) {
      const float want[] = {0.f, 0.5f, 1.f};
      for (const float v : want) {
        mix::config_set_slot_param(m.mix, s, p, v);
        CHECK(fabsf(mix::config_slot_param(&m, s, p) - v) < 2.f / 65534.f);
      }
      // Out of range clamps rather than wrapping.
      mix::config_set_slot_param(m.mix, s, p, 4.f);
      CHECK(mix::config_slot_param(&m, s, p) == 1.f);
      mix::config_set_slot_param(m.mix, s, p, -1.f);
      CHECK(mix::config_slot_param(&m, s, p) == 0.f);
      mix::config_set_slot_param(m.mix, s, p, 0.f);
    }
  }

  mix::config_set_master_gain(m.mix, 1.9f);
  CHECK(fabsf(mix::config_master_gain(&m) - 1.9f) < 1.f / 1000.f);
  mix::config_set_width(m.mix, 0.f);
  CHECK(mix::config_width(&m) == 0.f);

  // **And the player agrees, slot for slot.** An editor that wrote a mixer the
  // player builds differently is the whole failure this pairing prevents.
  for (int s = 0; s < kMixrSlots; ++s) {
    mix::config_set_slot_kind(m.mix, s, mix::FxKind::kDelay);
    for (int p = 0; p < kMixrSlotParams; ++p)
      mix::config_set_slot_param(m.mix, s, p, 0.125f * (float) (p + 1));
  }
  static mix::Mixer b;
  mix::mixer_reset(&b);
  CHECK(mix::mixer_config_read(&b, &m));
  for (int s = 0; s < kMixrSlots; ++s) {
    CHECK(mix::mixr_slot_at(&b, s)->kind == mix::FxKind::kDelay);
    for (int p = 0; p < kMixrSlotParams; ++p)
      CHECK(fabsf(mix::config_slot_param(&m, s, p) -
                  mix::mixr_slot_at(&b, s)->base[p]) < 2.f / 65534.f);
  }
}

// **A slot switched on must be audible.** Every knob at zero is a reverb of size 0 and mix 0
// -- an effect the panel names and the ear cannot find. The claim with polarity is that at
// least one knob of every kind starts away from zero, and that the wet ones differ between a
// send and the master.
static void
test_config_seed_gives_an_audible_effect() {
  printf("a slot seeded for its kind starts somewhere audible\n");

  uint8_t block[kMixrBytes];
  CHECK(mix::config_init(block));

  const mix::FxKind kinds[] = {mix::FxKind::kShape, mix::FxKind::kFilter,
                              mix::FxKind::kDelay, mix::FxKind::kReverb};
  for (const mix::FxKind k : kinds) {
    mix::config_set_slot_kind(block, 0, k);
    mix::config_seed_slot(block, 0);
    float sum = 0.f;
    for (int p = 0; p < kMixrSlotParams; ++p) {
      const float v = mix::config_slot_param(block, 0, p);
      CHECK(v >= 0.f);
      CHECK(v <= 1.f);
      // A knob the kind does not have stays at zero rather than picking up a neighbour's
      // default -- the eight bytes are shared and the spare ones must read as spare.
      if (mix::fx_param_name(k, p) == nullptr)
        CHECK(v == 0.f);
      sum += v;
    }
    CHECK(sum > 0.f);
  }

  // **Changing kind reseeds**, which is what stops a delay's Time arriving as a reverb's
  // Size: the eight bytes are shared, so without this a user sees a number they never typed.
  mix::config_set_slot_kind(block, 0, mix::FxKind::kDelay);
  mix::config_seed_slot(block, 0);
  const float delayTime = mix::config_slot_param(block, 0, 0);
  mix::config_set_slot_kind(block, 0, mix::FxKind::kReverb);
  mix::config_seed_slot(block, 0);
  CHECK(mix::config_slot_param(block, 0, 0) != delayTime);

  // A send is fully wet and the master is not: a master reverb at full wet replaces the mix.
  mix::config_set_slot_kind(block, 0, mix::FxKind::kReverb);
  mix::config_seed_slot(block, 0);
  mix::config_set_slot_kind(block, kMixrSlots - 1, mix::FxKind::kReverb);
  mix::config_seed_slot(block, kMixrSlots - 1);
  int mixIndex = -1;
  for (int p = 0; p < kMixrSlotParams; ++p) {
    const char *n = mix::fx_param_name(mix::FxKind::kReverb, p);
    if (n != nullptr && strcmp(n, "Mix") == 0) mixIndex = p;
  }
  CHECK(mixIndex >= 0);
  CHECK(mix::config_slot_param(block, 0, mixIndex) >
        mix::config_slot_param(block, kMixrSlots - 1, mixIndex));

  // Seeding a block the reader would refuse touches nothing.
  uint8_t bad[kMixrBytes];
  memcpy(bad, block, sizeof bad);
  bad[0] = 0xffu;
  uint8_t before[kMixrBytes];
  memcpy(before, bad, sizeof before);
  mix::config_seed_slot(bad, 0);
  CHECK(memcmp(before, bad, sizeof bad) == 0);
  mix::config_seed_slot(nullptr, 0);
}

// **Nothing at all, not "as far as it parsed".** A block the reader refuses is
// a file the player refuses whole, so a partial write would produce a mixer the
// editor can see and nobody can hear.
static void
test_config_set_refuses_a_bad_block() {
  printf("a set on a block the reader would refuse leaves every byte alone\n");

  uint8_t good[kMixrBytes];
  CHECK(mix::config_init(good));

  uint8_t bad[kMixrBytes];
  memcpy(bad, good, sizeof bad);
  bad[0] = 0xffu;                       // a version nothing reads
  uint8_t before[kMixrBytes];
  memcpy(before, bad, sizeof before);

  mix::config_set_slot_kind(bad, 0, mix::FxKind::kReverb);
  mix::config_set_slot_param(bad, 0, 0, 1.f);
  mix::config_set_master_gain(bad, 0.25f);
  mix::config_set_width(bad, 0.25f);
  CHECK(memcmp(before, bad, sizeof bad) == 0);

  // A kind past the ceiling is refused for the same reason: it would leave a
  // block that reads back as nothing at all.
  memcpy(bad, good, sizeof bad);
  mix::config_set_slot_kind(bad, 0, (mix::FxKind) 99);
  CHECK(memcmp(good, bad, sizeof bad) == 0);

  // And an index outside the record touches nothing either.
  mix::config_set_slot_kind(bad, kMixrSlots, mix::FxKind::kDelay);
  mix::config_set_slot_param(bad, 0, kMixrSlotParams, 1.f);
  mix::config_set_slot_param(bad, -1, 0, 1.f);
  CHECK(memcmp(good, bad, sizeof bad) == 0);
  mix::config_set_slot_kind(nullptr, 0, mix::FxKind::kDelay);
  mix::config_set_master_gain(nullptr, 1.f);
}

static void
test_config_reads_params_without_a_mixer() {
  printf("a slot's parameters and the master read back without a mixer\n");

  static mix::Mixer a;
  mixr_configure(&a);
  a.send[0].param[0] = 0.25f;
  mix::slot_set_kind(&a.send[0], a.send[0].kind);   // base takes the value
  a.master_gain = 0.5f;
  a.width = 0.75f;

  Module m;
  mix::mixer_config_write(&a, &m);

  // **Agrees with the reader, parameter for parameter.** An editor drawing a
  // slot's knobs must not describe a different mixer from the one the player
  // will build.
  static mix::Mixer b;
  mix::mixer_reset(&b);
  CHECK(mix::mixer_config_read(&b, &m));
  for (int s = 0; s < kMixrSlots; ++s)
    for (int p = 0; p < kMixrSlotParams; ++p)
      CHECK(fabsf(mix::config_slot_param(&m, s, p) -
                  mix::mixr_slot_at(&b, s)->base[p]) < 2.f / 65534.f);

  CHECK(fabsf(mix::config_master_gain(&m) - b.master_gain) < 1.f / 1000.f);
  CHECK(fabsf(mix::config_width(&m) - b.width) < 1.f / 1000.f);

  // Out of range answers zero rather than reading past the record.
  CHECK(mix::config_slot_param(&m, -1, 0) == 0.f);
  CHECK(mix::config_slot_param(&m, kMixrSlots, 0) == 0.f);
  CHECK(mix::config_slot_param(&m, 0, -1) == 0.f);
  CHECK(mix::config_slot_param(&m, 0, kMixrSlotParams) == 0.f);

  // A module with no block, and a block the reader would refuse: nothing is
  // described, because describing it would name a mixer nobody can hear.
  Module bare;
  CHECK(mix::config_slot_param(&bare, 0, 0) == 0.f);
  Module bad = m;
  bad.mix[kMixrHeaderBytes] = 200u;
  CHECK(mix::config_slot_param(&bad, 0, 0) == 0.f);
  CHECK(mix::config_master_gain(&bad) == 1.f);
  CHECK(mix::config_width(&bad) == 1.f);
}

int
main(void) {
  test_bypass_is_render_add();
  test_limiter_bounds();
  test_empty_slots_are_identity();
  test_stereo_filter_isolation();
  test_mono_refuses_stereo_only_effects();
  test_voice_filter_takes_the_top_off();
  test_filter_types_differ();
  test_no_filter_flag_is_untouched();
  test_max_resonance_stays_finite();
  test_plane_pan_sets();
  test_plane_pan_slide_walks();
  test_plane_send_level_opens_a_send();
  test_plane_absent_is_render_add();
  test_plane_slide_survives_pattern_delay();
  test_synth_retrigger_resets_the_filter();
  test_block_size_invariance();
  test_everything_at_once();
  test_plane_command_space();
  test_columns_both_take_effect();
  test_columns_opposite_slides_cancel();
  test_columns_reach_the_ceiling();
  test_macro_drives_three_targets();
  test_macro_delta_ramps();
  test_macro_scope_all_reaches_every_channel();
  test_meta_first_lets_a_column_override();
  test_macro_index_past_the_table_is_ignored();
  test_external_macro_lands_on_the_row();
  test_external_macro_outranks_the_row();
  test_external_macro_applies_once();
  test_external_macro_last_call_wins();
  test_external_macro_two_at_once();
  test_external_macro_index_is_checked();
  test_external_macro_input_is_clamped();
  test_external_macro_absent_touches_nothing();
  test_external_macro_reset_drops_the_queue();
  test_external_macro_drops_without_a_plane();
  test_external_macro_skips_a_delta();
  test_macro_extremes_stay_bounded();
  test_slot_units_are_normalised();
  test_slot_commands_set_and_slide();
  test_slot_lane_restrictions();
  test_slot_macro_targets();
  test_slot_resync_restores_the_configuration();
  test_slot_apply_skips_and_fires();
  test_slot_slide_steps_a_shaper_through_its_kinds();
  test_describe_slot_params();
  test_describe_macro();
  test_describe_command_space();
  test_fxpl_mnemonic();
  test_reverb_slot_late_knobs();
  test_shaper_dry();
  test_command_enumeration();
  test_mixr_round_trip();
  test_mixr_refuses_an_unknown_kind();
  test_mixr_writes_the_setting_not_the_slide();
  test_config_slot_kind_reads_without_a_mixer();
  test_config_reads_params_without_a_mixer();
  test_config_init_makes_a_readable_block();
  test_config_set_round_trips();
  test_config_set_refuses_a_bad_block();
  test_config_seed_gives_an_audible_effect();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
