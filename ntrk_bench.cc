// ntrk_bench -- what each layer of this library costs, in milliseconds of CPU
// per second of rendered audio.
//
//     c++ -std=c++11 -Wall -Wextra -ffp-contract=off -O2 -I. -o ntrk_bench \
//         ntrk_bench.cc
//     ./ntrk_bench
//
// or `make bench-ntrk` from the project root, which is that command and then
// this binary. It is not part of `make test` on purpose: a timing check in a
// correctness suite is a slow, flaky test that fails on a busy machine.
//
// **Build it at -O2 or do not build it.** A debug build measures a program that
// is never shipped -- the SVF, the shapers and the synth voices are all headers
// precisely so they inline into the loop that calls them (see README.md), and
// at -O0 not one of them does. The numbers from an unoptimised build are not
// pessimistic, they are meaningless.
//
// ## Why this exists
//
// The build-order plan splits effects into "affordable now" -- delay, chorus,
// flanger, tremolo, bitcrusher, saturation, a state-variable filter, a comb-and-
// allpass reverb -- and "not yet": spectral effects, multi-band compression, the
// reverb character models. That split, the no-oversampling policy and the
// 64-frame block cap are every one of them justified by cost, and until this
// file nothing had ever measured cost. Roadmap C11.
//
// **The number that matters is the real-time factor**: milliseconds of CPU per
// second of audio, as a percentage. A budget only means something against the
// frame it shares -- on the web build this runs on the main thread beside the
// renderer, feeding a worklet with roughly 43 ms of queue -- so "6 % of real
// time" is the useful form and "0.06 ms per block" is not.
//
// ## What it does not tell you
//
// One machine, one compiler, one set of flags. A figure here is comparable
// against another figure here and against nothing else. In particular the x86
// denormal cliff (roadmap C3) does not reproduce on Apple Silicon, so a reverb
// tail measured on ARM is a *lower bound* for x86 rather than an answer for it.
//
// stdio, <chrono> and libm. This is a tool and not part of the library, the same
// terms `ntrk_render.cc` states about itself: the no-libm, no-allocation,
// no-standard-library rules are the library's, and nothing here is linked into
// anything that ships.
//
// Public domain / CC0. Written for the no2 project.

// The unity header, so this is one compiler invocation with no build rules --
// the same route test_ntrk_fx.cc and test_ntrk_mix.cc take, and the reason for
// the `-I.`.
#include "ntrk_unity.h"

#include <chrono>
#include <stdio.h>
#include <string.h>

using namespace ntrk;

// ----------------------------------------------------------------------------
// -- The workload
// ----------------------------------------------------------------------------

static const int kRate = 48000;
static const int kOut = 2;            // interleaved stereo, as a host wants it

// A plausible audio callback rather than the mixer's own block. Both the player
// and the mixer chunk internally at kMaxBlock, so the size here changes how
// often a *caller* is entered and nothing about how the sequencer is stepped --
// which is what makes the block cap's cost visible instead of hidden.
static const int kCallback = 1024;

// Five seconds of audio per run. Long enough that a reverb tank is full and the
// tune has looped, short enough that seventeen cases times nine runs is a tool
// somebody actually runs.
static const int kSeconds = 5;
static const long kFrames = (long) kRate * kSeconds;

// Odd, and the median is taken rather than the mean: one scheduler hiccup in
// nine runs moves a mean and cannot move a median.
static const int kRuns = 9;

#if defined(__wasm__) || defined(__EMSCRIPTEN__)
static const char *const kArch = "wasm";
#elif defined(__aarch64__)
static const char *const kArch = "aarch64";
#elif defined(__x86_64__)
static const char *const kArch = "x86_64";
#else
static const char *const kArch = "unknown";
#endif

static double g_out[kCallback * kOut];

// Caller-owned effect memory, as those two modules require. Static rather than
// stack: a reverb tank is about 127 KB and a render thread gets 512 KB.
static const float kDelayMax = 0.5f;      // seconds
static const float kPredelayMax = 0.05f;  // seconds
static float g_delay_mem[64 * 1024];
static float g_reverb_mem[64 * 1024];

static mix::Mixer g_mixer;

// ----------------------------------------------------------------------------
// -- A module built in memory
// ----------------------------------------------------------------------------
//
// No file on disk and nothing from `testdata/`, which is gitignored -- a
// benchmark that needs somebody else's music is a benchmark nobody else can
// run. Deliberately plain: this is a carrier for the cost of the layers above
// it, not a test of the loader.

static const int kRows = 16;
static const int kSampleLen = 64;
static const size_t kInstBytes = 32u;     // one instrument entry

static uint8_t g_bytes[4096];
static size_t g_size = 0;

struct Spec {
  int  channels  = 4;
  int  voice     = -1;      // -1: no SYNTH instrument at all; else a SynthVoice
  bool voice_all = false;   // every channel plays it, rather than the last one
  bool plane     = false;   // an FXPL plane and the MACR table behind it
};

static void
put_u16(size_t at, uint16_t x) {
  g_bytes[at] = (uint8_t) (x & 0xffu);
  g_bytes[at + 1] = (uint8_t) (x >> 8);
}

static void
put_u32(size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i)
    g_bytes[at + (size_t) i] = (uint8_t) ((x >> (8 * i)) & 0xffu);
}

static void
build(const Spec &s) {
  const int instruments = s.voice >= 0 ? 2 : 1;
  const int meta_columns = s.plane ? 1 : 0;
  const int lanes = s.channels * 1 + meta_columns;

  const size_t inst_at = 33u;                                   // one order byte
  const size_t pat_at = inst_at + (size_t) instruments * kInstBytes;
  const size_t blob_at = pat_at + (size_t) kRows * (size_t) s.channels * 4u;
  const size_t dir_at = blob_at + kSampleLen;

  const int blocks = (s.voice >= 0 ? 1 : 0) + (s.plane ? 2 : 0);
  const size_t synp_at = dir_at + (size_t) blocks * 12u;
  const size_t fxpl_at = synp_at + (s.voice >= 0 ? 16u : 0u);
  const size_t fxpl_bytes = 4u + (size_t) kRows * (size_t) lanes * 2u;
  const size_t macr_at = fxpl_at + (s.plane ? fxpl_bytes : 0u);

  g_size = s.plane ? macr_at + 4u + 52u : fxpl_at;
  memset(g_bytes, 0, sizeof g_bytes);

  memcpy(g_bytes, "NTRK", 4);
  put_u16(4, 2);
  put_u16(6, (uint16_t) s.channels);
  put_u16(8, (uint16_t) kRows);
  put_u16(10, 6);                                   // speed, ticks per row
  put_u16(12, 125);                                 // bpm
  put_u16(14, 1);                                   // orders
  put_u16(16, 1);                                   // patterns
  put_u16(18, (uint16_t) instruments);
  put_u16(20, 0);                                   // restart
  put_u32(22, kSampleLen);                          // blob bytes
  put_u16(26, (uint16_t) blocks);
  g_bytes[28] = 96u;                                // note_max

  // Instrument 1 is the PCM baseline every non-synth case plays; instrument 2,
  // when it exists, is the SYNTH the record below tunes.
  put_u32(inst_at + 0, 0u);
  put_u32(inst_at + 4, kSampleLen);
  put_u32(inst_at + 8, 0u);
  put_u32(inst_at + 12, kSampleLen);                // looped, so a note holds
  g_bytes[inst_at + 16] = 64u;                      // volume
  g_bytes[inst_at + 18] = (uint8_t) InstrumentType::kPcm8;
  if (s.voice >= 0) {
    const size_t e = inst_at + kInstBytes;
    g_bytes[e + 16] = 64u;
    g_bytes[e + 18] = (uint8_t) InstrumentType::kSynth;   // no blob at all
  }

  // A note on every row of every channel. **Retriggering matters to what this
  // measures**: a drum voice decays to exactly zero and then costs nothing, so
  // a module that strikes one note and waits reports the cost of silence.
  for (int r = 0; r < kRows; ++r) {
    for (int c = 0; c < s.channels; ++c) {
      const size_t cell =
          pat_at + ((size_t) r * (size_t) s.channels + (size_t) c) * 4u;
      // Spread across the channels so the voices are not in phase, and inside
      // 1..36 so a v1-range note table would play the same thing.
      g_bytes[cell + 0] = (uint8_t) (25 + c * 3);
      const bool synth_here =
          s.voice >= 0 && (s.voice_all || c == s.channels - 1);
      g_bytes[cell + 1] = synth_here ? 2u : 1u;
    }
  }

  // A square wave: unambiguously not silence, and every frame far from zero, so
  // nothing downstream is measured against an accidentally quiet input.
  for (int i = 0; i < kSampleLen; ++i)
    g_bytes[blob_at + (size_t) i] =
        (uint8_t) (int8_t) ((i < kSampleLen / 2) ? 96 : -96);

  size_t dir = dir_at;
  if (s.voice >= 0) {
    put_u16(dir + 0, 0x0003u);                      // SYNP
    put_u16(dir + 2, 1u);                           // critical
    put_u32(dir + 4, (uint32_t) synp_at);
    put_u32(dir + 8, 16u);
    dir += 12u;

    g_bytes[synp_at + 0] = (uint8_t) s.voice;
    g_bytes[synp_at + 1] = 128u;                    // tune
    g_bytes[synp_at + 2] = 90u;                     // decay
    g_bytes[synp_at + 3] = 160u;                    // sweep
    g_bytes[synp_at + 4] = 128u;                    // tone
    g_bytes[synp_at + 5] = 60u;                     // noise against body
    g_bytes[synp_at + 6] = 40u;                     // noise decay
    g_bytes[synp_at + 7] = 80u;                     // drive
    g_bytes[synp_at + 8] = 170u;                    // bass: cutoff
    g_bytes[synp_at + 9] = 200u;                    // bass: resonance
    g_bytes[synp_at + 10] = 140u;                   // bass: env mod
    g_bytes[synp_at + 11] = 60u;                    // bass: accent
    g_bytes[synp_at + 12] = 2u;                     // bass: tape shaper
    g_bytes[synp_at + 13] = 255u;                   // bass: dist mix
    g_bytes[synp_at + 14] = 1u;                     // bass: square
  }

  if (!s.plane)
    return;

  put_u16(dir + 0, 0x0001u);                        // FXPL
  put_u16(dir + 2, 0u);                             // optional
  put_u32(dir + 4, (uint32_t) fxpl_at);
  put_u32(dir + 8, (uint32_t) fxpl_bytes);
  dir += 12u;

  put_u16(dir + 0, 0x0004u);                        // MACR
  put_u16(dir + 2, 1u);                             // critical
  put_u32(dir + 4, (uint32_t) macr_at);
  put_u32(dir + 8, 4u + 52u);

  put_u16(fxpl_at + 0, 1u);                         // fx_columns
  put_u16(fxpl_at + 2, (uint16_t) meta_columns);

  // A pan slide on every channel lane and a macro on the meta lane, on every
  // row -- so the plane's row latch, its per-tick slide and a macro invocation
  // all run for the whole render. The direction flips halfway down the pattern
  // so the value sweeps rather than parking against the clamp, which would
  // leave the write path measuring a value that never changes.
  //
  // **Pan and not cutoff, deliberately.** A cutoff command switches the voice
  // filter on, and the plane would then be charged for an SVF per channel that
  // the "plane absent" case does not run. This measures the plane.
  for (int r = 0; r < kRows; ++r) {
    for (int lane = 0; lane < lanes; ++lane) {
      const size_t cell =
          fxpl_at + 4u + ((size_t) r * (size_t) lanes + (size_t) lane) * 2u;
      if (lane < s.channels) {
        g_bytes[cell + 0] = (uint8_t) mix::kFxplPanSlide;
        g_bytes[cell + 1] = (uint8_t) (r < kRows / 2 ? 4 : -4);
      } else {
        g_bytes[cell + 0] = 1u;                     // macro 1, one-based
        g_bytes[cell + 1] = 2u;                     // its input byte
      }
    }
  }

  put_u16(macr_at + 0, 1u);                         // macro_count
  const size_t rec = macr_at + 4u;
  g_bytes[rec + 0] = 1u;                            // target_count
  g_bytes[rec + 1] = 1u;                            // flags: delta
  g_bytes[rec + 4] = (uint8_t) mix::kFxplPan;       // target
  g_bytes[rec + 5] = 0xffu;                         // scope: every channel
  put_u16(rec + 6, 0x0100u);                        // scale, 8.8, unity
}

static bool g_failed = false;

static bool
prepare(const Spec &s, Module *m, Player *p) {
  build(s);
  if (!module_load(m, g_bytes, g_size)) {
    printf("  the module this benchmark builds does not load\n");
    g_failed = true;
    return false;
  }
  player_start(p, m);
  return true;
}

// ----------------------------------------------------------------------------
// -- Configuration
// ----------------------------------------------------------------------------

struct Cfg {
  Spec spec;
  bool  mixer     = true;      // false: render_add alone, the floor
  mix::FxKind send = mix::FxKind::kNone;   // on send 0, fed by every channel
  bool  all_sends = false;     // shaper, filter, delay and reverb at once
  mix::FxKind insert = mix::FxKind::kNone;   // on every channel
  mix::FxKind master = mix::FxKind::kNone;
  float width     = 1.f;
};

// `wet` is what separates a send from an insert: a send is all wet by
// definition, an insert is a blend. Every other parameter is picked to be a
// real setting rather than a cheap one -- a reverb at size 0 measures a reverb
// nobody would use.
static void
slot_setup(mix::Slot *s, mix::FxKind kind, bool wet) {
  if (kind == mix::FxKind::kNone)
    return;
  if (kind == mix::FxKind::kDelay &&
      !fx::delay_init(&s->delay, g_delay_mem, sizeof g_delay_mem,
                      (float) kRate, kDelayMax)) {
    g_failed = true;
    return;
  }
  if (kind == mix::FxKind::kReverb &&
      !fx::reverb_init(&s->reverb, g_reverb_mem, sizeof g_reverb_mem,
                       (float) kRate, kPredelayMax)) {
    g_failed = true;
    return;
  }

  // Parameters first: `slot_set_kind` is where a slot's configuration is taken.
  // All of them are normalised 0..1 -- see `Slot::param`.
  switch (kind) {
    case mix::FxKind::kShape:
      s->param[0] = mix::slot_choice_param((int) fx::ShapeKind::kTape, 4);
      s->param[1] = 0.6f;
      break;
    case mix::FxKind::kFilter:
      s->param[0] = mix::slot_choice_param((int) mix::FilterMode::kLowpass, 3);
      s->param[1] = 0.72f;          // about 2 kHz on the exponential curve
      s->param[2] = 0.4f;
      break;
    case mix::FxKind::kDelay:
      s->param[0] = 0.25f / mix::kSlotDelayMaxSeconds;
      s->param[1] = 0.45f;
      s->param[2] = 0.3f;
      s->param[3] = wet ? 1.f : 0.35f;
      s->param[4] = 0.f;
      break;
    case mix::FxKind::kReverb:
      s->param[0] = 0.75f;
      s->param[1] = 0.4f;
      s->param[2] = 0.02f / mix::kSlotPredelayMaxSeconds;
      s->param[3] = 1.f;
      s->param[4] = wet ? 1.f : 0.3f;
      break;
    default:
      break;
  }
  mix::slot_set_kind(s, kind);
}

// A default-constructed `Mixer`, kept once. Assigning from it is how the mixer
// is returned to nothing-switched-on between cases: `mixer_reset` deliberately
// keeps the settings, and a `memset` would flatten the three members whose
// off position is not zero (`master_gain`, `width`, `limit_gain`).
static const mix::Mixer kMixerOff = {};

static void
mixer_setup(const Cfg &cfg) {
  g_mixer = kMixerOff;
  g_mixer.width = cfg.width;

  if (cfg.all_sends) {
    const mix::FxKind kinds[mix::kSends] = {
        mix::FxKind::kShape, mix::FxKind::kFilter, mix::FxKind::kDelay,
        mix::FxKind::kReverb};
    for (int s = 0; s < mix::kSends; ++s) {
      slot_setup(&g_mixer.send[s], kinds[s], true);
      for (int c = 0; c < cfg.spec.channels; ++c)
        g_mixer.send_level[c][s] = 0.2f;
    }
  } else if (cfg.send != mix::FxKind::kNone) {
    slot_setup(&g_mixer.send[0], cfg.send, true);
    for (int c = 0; c < cfg.spec.channels; ++c)
      g_mixer.send_level[c][0] = 0.3f;
  }

  if (cfg.insert != mix::FxKind::kNone) {
    for (int c = 0; c < cfg.spec.channels; ++c)
      slot_setup(&g_mixer.insert[c], cfg.insert, false);
  }

  slot_setup(&g_mixer.master_fx, cfg.master, false);
}

// ----------------------------------------------------------------------------
// -- Timing
// ----------------------------------------------------------------------------

static double g_last_peak = 0.0;

static double
now_ms() {
  // Monotonic. `system_clock` is not, and a benchmark that can be moved by NTP
  // is a benchmark that reports a negative duration once a year.
  const std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
}

// One run: `kFrames` of audio, a callback at a time, with the buffer zeroed the
// way a real caller must -- both renderers *add*.
//
// Setup is outside the timed region, so what is reported is rendering and not
// the cost of building a mixer.
static double
run_once(const Cfg &cfg) {
  Module m;
  Player p;
  if (!prepare(cfg.spec, &m, &p))
    return 0.0;
  if (cfg.mixer)
    mixer_setup(cfg);

  const double t0 = now_ms();
  long done = 0;
  while (done < kFrames) {
    int want = kCallback;
    if (want > (int) (kFrames - done))
      want = (int) (kFrames - done);
    for (int i = 0; i < want * kOut; ++i)
      g_out[i] = 0.0;
    if (cfg.mixer)
      mix::mixer_render_add(&g_mixer, &p, g_out, want, kOut, (float) kRate);
    else
      render_add(&p, g_out, want, kOut, (float) kRate);
    done += want;
  }
  const double ms = now_ms() - t0;

  // **The peak of the last callback, taken after the clock has stopped.** A
  // case that renders silence costs whatever silence costs and reports it as
  // the price of an effect, which is the exact shape of wrong number this file
  // exists to replace -- a mistyped SYNP byte or a send level left at zero
  // looks like a fast case rather than a broken one. Outside the timed region
  // because scanning 480000 frames inside it would be measured as the mixer.
  g_last_peak = 0.0;
  for (int i = 0; i < kCallback * kOut; ++i) {
    const double a = g_out[i] < 0.0 ? -g_out[i] : g_out[i];
    if (a > g_last_peak)
      g_last_peak = a;
  }
  return ms;
}

static double
median_ms(const Cfg &cfg, bool *silent) {
  run_once(cfg);                      // warm up: caches, the first tank fill
  *silent = g_last_peak < 1.0e-4;

  double ms[kRuns];
  for (int i = 0; i < kRuns; ++i)
    ms[i] = run_once(cfg);

  for (int i = 1; i < kRuns; ++i) {   // insertion sort; kRuns is nine
    const double v = ms[i];
    int j = i - 1;
    for (; j >= 0 && ms[j] > v; --j)
      ms[j + 1] = ms[j];
    ms[j + 1] = v;
  }
  return ms[kRuns / 2];
}

// ----------------------------------------------------------------------------
// -- The cases
// ----------------------------------------------------------------------------

struct Case {
  const char *name;
  Cfg cfg;
};

static Cfg
plain() {
  Cfg c;
  c.mixer = false;
  return c;
}

static Cfg
bypass() {
  return Cfg();
}

static Cfg
with_send(mix::FxKind kind) {
  Cfg c;
  c.send = kind;
  return c;
}

static Cfg
with_insert(mix::FxKind kind) {
  Cfg c;
  c.insert = kind;
  return c;
}

static Cfg
with_plane(bool on) {
  Cfg c;
  c.spec.plane = on;
  return c;
}

// One channel, so what is reported is the cost of *one* voice rather than four
// of them. The PCM row is the baseline every synth row is read against.
static Cfg
one_voice(int voice) {
  Cfg c;
  c.mixer = false;
  c.spec.channels = 1;
  c.spec.voice = voice;
  c.spec.voice_all = voice >= 0;
  return c;
}

// Four channels, a 303 on the last of them, every send live, an insert filter on
// every channel, a master shaper, a stereo width that is not 1 and the plane
// driving pan. This is the configuration the budget is actually about.
static Cfg
everything() {
  Cfg c;
  c.spec.voice = (int) SynthVoice::kBass;
  c.spec.plane = true;
  c.all_sends = true;
  c.insert = mix::FxKind::kFilter;
  c.master = mix::FxKind::kShape;
  c.width = 1.2f;
  return c;
}

static void
row(const char *name, double ms, bool silent) {
  const double per_second = ms / (double) kSeconds;
  printf("  %-30s %9.2f %12.3f %10.2f %%%s\n", name, ms, per_second,
         100.0 * per_second / 1000.0, silent ? "   SILENT" : "");
}

int
main(void) {
  const Case cases[] = {
    {"replayer, 4 ch (render_add)", plain()},
    {"mixer, nothing switched on", bypass()},

    {"send: shaper", with_send(mix::FxKind::kShape)},
    {"send: filter", with_send(mix::FxKind::kFilter)},
    {"send: delay", with_send(mix::FxKind::kDelay)},
    {"send: reverb", with_send(mix::FxKind::kReverb)},

    {"insert: filter, every ch", with_insert(mix::FxKind::kFilter)},
    {"insert: shaper, every ch", with_insert(mix::FxKind::kShape)},

    {"FXPL plane + macros", with_plane(true)},
    {"the same, no plane", with_plane(false)},

    {"voice: PCM8 (the baseline)", one_voice(-1)},
    {"voice: kick", one_voice((int) SynthVoice::kKick)},
    {"voice: snare", one_voice((int) SynthVoice::kSnare)},
    {"voice: hihat", one_voice((int) SynthVoice::kHihat)},
    {"voice: clap", one_voice((int) SynthVoice::kClap)},
    {"voice: 303 bass", one_voice((int) SynthVoice::kBass)},

    {"everything on", everything()},
  };
  const int count = (int) (sizeof cases / sizeof cases[0]);

  printf("ntrk CPU budget: %d Hz, %d-frame block cap, %d-frame callback, %s\n",
         kRate, kMaxBlock, kCallback, kArch);
  printf("%d s of audio per run, median of %d runs, -O2\n\n", kSeconds, kRuns);
  printf("  %-30s %9s %12s %12s\n", "case", "total ms", "ms/s audio",
         "real time");

  bool silent_any = false;
  for (int i = 0; i < count; ++i) {
    bool silent = false;
    const double ms = median_ms(cases[i].cfg, &silent);
    silent_any = silent_any || silent;
    row(cases[i].name, ms, silent);
  }

  if (g_failed) {
    printf("\nsomething refused to initialise; the numbers above are not the "
           "workload they name\n");
    return 1;
  }
  if (silent_any) {
    printf("\na case marked SILENT rendered nothing, so its number is the cost "
           "of silence rather than of the thing it names\n");
    return 1;
  }
  return 0;
}
