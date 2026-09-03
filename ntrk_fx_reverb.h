// ntrk_fx_reverb -- a Gardner diffuser into an eight-line feedback delay network.
//
// The API and the reasoning are here; the arithmetic is in `ntrk_fx_reverb.cc`.
// Compile that file alongside your own, or `#include "ntrk_unity.h"` for a
// single-translation-unit build — both are supported and both are tested, see
// README.md.
//
// The chain, in order: a mono sum, a predelay, four series allpasses that smear
// the input into noise before the tank ever sees it, eight delay lines mixed by
// an 8x8 Hadamard matrix with a two-band shelf and a decay gain in each loop,
// thirty-two output taps read back out of those same lines, and one short
// allpass per output channel to pull left and right apart.
//
// **Why this and not Schroeder's combs.** Eight parallel combs produce eight
// independent periodic echo trains, and for the first hundred milliseconds that
// is exactly what you hear — a fluttering grid of repeats that only turns into
// noise once the four allpasses behind them have had time to smear it. An FDN
// mixes every line into every other line on every single sample, so the echo
// count squares rather than adding, and the Gardner diffuser in front means the
// tank is fed noise rather than an impulse from the first sample. The measured
// difference is in README.md; the short version is that density at 20 ms is
// about thirty times higher and there is no flutter to smear.
//
//     size_t need = ntrk::fx::reverb_bytes_needed(48000.f, 0.1f);
//     float *mem = ...;               // caller's, however it likes
//     ntrk::fx::Reverb reverb;
//     if (!ntrk::fx::reverb_init(&reverb, mem, need, 48000.f, 0.1f))
//       ...                           // a buffer too small is refused
//     ntrk::fx::reverb_set(&reverb, 0.7f, 0.4f, 0.02f, 1.f, 0.3f);
//     ntrk::fx::reverb_process(&reverb, left, right, frames);
//
// **Nothing here allocates, and the caller owns the delay memory.** A `Reverb`
// is a view over a buffer somebody else made, the same way a loaded `Module` is
// a view over the file bytes, so **the buffer must outlive the reverb**. Ask
// `reverb_bytes_needed` first and hand `reverb_init` what it asked for; it
// refuses anything short rather than working in a buffer it does not fit. A
// stereo instance at 48 kHz with 100 ms of predelay comes to about 108 KB —
// *less* than the comb bank this replaced, because an FDN needs one set of
// lines for the pair rather than one set each.
//
// **The tuning is in samples at 48 kHz and is rescaled at init.** Those delay
// lengths are a duration, not a count, and a build that takes them literally at
// 44.1 kHz is 9% long on every line — a different room, and every RT60 the
// decay knob promises is then wrong by the same 9%. Getting this wrong is the
// usual bug in this family, and it sounds like a slightly different room rather
// than like a mistake, which is why it survives.
//
// **`size` moves the delay lengths, not just a feedback gain.** That is the
// whole reason it is called size: a room's modal density is the sum of its
// delay lengths over the sample rate, so a knob that only changed feedback
// changed how long the same room rang rather than which room it was. The lines
// are laid out at the largest size and the read pointer moves within them, so
// the knob costs no memory and no re-init.
//
// **Decay is separately controlled, and it is Jot's normalisation.** Each
// line's feedback gain is `10^(-3 L / (rt60 sr))`, so every line decays at the
// same rate in seconds no matter how long it is. RT60 is therefore a property
// of the decay knob alone — which is why `reverb_set`, whose five-knob API has
// no decay, derives one from `size`: a size knob that did not lengthen the tail
// would not be wired to the thing its label promises.
//
// **`mix == 0` is an exact passthrough**, bit for bit: `reverb_process` returns
// before it touches a sample. That makes "the effect is off" provable by hashing
// the output rather than by listening, and it is why the dry path is a return and
// not a multiply by 1.0 — `-0.0f + 0.0f` is `+0.0f`, so even the identity gains
// are not quite an identity. The tank freezes rather than running silently while
// off, so turning `mix` back up resumes the tail it was holding.
//
// **The first wet sample arrives at the shortest output tap**, which is 7.4 ms
// at 48 kHz and size 0.5 — the taps sit part way along the lines rather than at
// their ends. That gap is part of the topology, not latency to be tuned out,
// and `predelay` adds to it rather than filling it. It used to be 25 ms, and
// the whole of it was silence.
//
// Deterministic: no clock, no allocation, no fast-math, no flush-to-zero mode,
// **and no libm at all** — the exponentials the shelf and decay coefficients
// need come off a table in the `.cc`, because `std::exp` is not correctly
// rounded and Apple's libm and emscripten's musl are free to disagree in the
// last bits. The same samples in produce the same samples out on ARM64 and on
// wasm. Denormals are handled by hand, in ntrk_dsp.h.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_FX_REVERB_H_
#define NTRK_FX_REVERB_H_

#include "ntrk_dsp.h"

#include <stddef.h>
#include <stdint.h>

namespace ntrk {
namespace fx {

// ----------------------------------------------------------------------------
// -- Tuning
// ----------------------------------------------------------------------------
//
// **How many lines there are is structure and stays here**, because `Reverb`
// and `ReverbLayout` below are arrays of exactly this many. Everything else
// about the tuning -- the delay lengths, the crossovers, the RT60 range, the
// rate and predelay bounds -- is in `ntrk_fx_reverb.cc` with the reasoning for
// each number, because only the tank reads them.
//
// `kReverbLines` must stay a power of two: the mixing matrix is a Hadamard
// butterfly, and a butterfly on a non-power-of-two is not a matrix.
const int kReverbLines = 8;
const int kReverbDiffusers = 4;
const int kReverbTaps = 4;      // output taps per line, so 32 in total

int reverb_scale_len(int tuned, float sample_rate);

// One description of the memory layout, computed once and used by both
// `reverb_bytes_needed` and `reverb_init`. They have to agree exactly — a
// request that is a sample short of what init lays out is a buffer overrun in a
// build where the assertion is compiled out — so they run the same code rather
// than the same arithmetic written twice.
//
// `line[]` is the capacity at the *largest* size, not the delay in use.
struct ReverbLayout {
  int line[kReverbLines];
  int diffuser[kReverbDiffusers];
  int decorr[2];
  int predelay;                 // line length, one greater than the max delay
  int total;                    // floats
};

bool reverb_layout(ReverbLayout *l, float sample_rate,
                   float max_predelay_seconds);

// Bytes the caller must provide for these settings. Zero means the settings
// themselves are refused, and no buffer size will make them work.
size_t reverb_bytes_needed(float sample_rate, float max_predelay_seconds);

// ----------------------------------------------------------------------------
// -- The reverb
// ----------------------------------------------------------------------------

// Offsets rather than pointers, so the struct says nothing about where the
// buffer is and a copy of a `Reverb` is still a copy of the same tank.
struct ReverbLine {
  int at = 0;
  int cap = 1;                  // laid out for the largest size
  int pos = 0;
  int delay = 1;                // the delay in use; 1 <= delay <= cap
  int tap[kReverbTaps] = {1, 1, 1, 1};  // read-back offsets; the last is `delay`

  float gain = 0.f;             // Jot's per-line decay gain

  // The two shelves' state. Their *coefficients* are shared by all eight lines
  // and live on `Reverb`: with one RT60 for the whole tank the shelf is the
  // same filter everywhere, and only the broadband gain above differs by
  // length.
  float lo_x = 0.f, lo_y = 0.f;
  float hi_x = 0.f, hi_y = 0.f;
};

// The Gardner diffuser's stages and the two output decorrelators are the same
// structure: a true Schroeder allpass, unlike Freeverb's, whose feedforward
// gain of -1 makes it something else with a name it did not earn.
struct ReverbAllpass {
  int at = 0;
  int len = 1;
  int pos = 0;
};

struct Reverb {
  float *buffer = nullptr;      // caller-owned; must outlive the reverb
  int capacity = 0;             // floats of `buffer` this instance uses

  ReverbLine line[kReverbLines];
  ReverbAllpass diffuser[kReverbDiffusers];
  ReverbAllpass decorr[2];

  int pre_at = 0;
  int pre_len = 0;
  int pre_pos = 0;
  int pre_max = 0;              // largest delay the line can hold, in samples
  int predelay = 0;             // the delay in use

  float sample_rate = 0.f;
  float diffusion = 0.f;        // the diffuser stages' shared coefficient

  // Two first-order shelves in series, bass then treble. Each is
  // `(b0 + b1 z^-1) / (1 - p z^-1)`, which is exactly `g_lf` at DC and exactly
  // `g_hf` at Nyquist and monotone between — so with both gains at or below 1
  // the pair is passive, the loop gain never exceeds the line gain, and the
  // stability argument stays one line long.
  float lo_p = 0.f, lo_b0 = 1.f, lo_b1 = 0.f;
  float hi_p = 0.f, hi_b0 = 1.f, hi_b1 = 0.f;

  float wet1 = 0.f;             // same-channel wet gain
  float wet2 = 0.f;             // cross-channel wet gain
  float dry = 1.f;
  float mix = 0.f;
};

// Clears the tank without disturbing the settings. NaN in, silence out — a tank
// that has taken a NaN never recovers on its own, because every feedback path
// keeps multiplying it.
void reverb_reset(Reverb *r);

float reverb_clamp01(float x);

// Knobs clamp where `reverb_init` refuses. The difference is who is calling: a
// bad buffer size is a programming error worth reporting once, a parameter is
// swept from a slider or an envelope every frame and there is nothing useful to
// do with a refusal in the middle of one.
//
// The five-knob call is the old one and is unchanged, so an existing caller
// gets the new tank with no edit. It derives the three controls it has no room
// for: `size` sets both the room and a decay long enough that the tail still
// tracks the label, damping drives the treble shelf, and the bass shelf and the
// diffusion sit at the values the eight-knob call defaults to.
void reverb_set(Reverb *r, float size, float damping, float predelay_seconds,
                float width, float mix);

// Eight controls, which is what a mixer slot's `param[8]` holds. Every one is
// 0..1 except the predelay, which is seconds, and every damping knob points the
// same way: 0 is no damping, 1 is as dark (or as thin) as the shelf goes.
//
//   size       the room: delay lengths, and so modal density
//   decay      RT60, logarithmic from 0.15 s to 30 s
//   damping_hf how much treble the tank loses per pass
//   damping_lf how much bass it loses per pass
//   diffusion  the Gardner stages' allpass coefficient
void reverb_set_full(Reverb *r, float size, float decay, float damping_hf,
                     float damping_lf, float diffusion, float predelay_seconds,
                     float width, float mix);

// The three controls the five-knob call has no room for, exactly as it derives
// them. Any of the three pointers may be null.
//
// **This exists so a caller can override some of them and keep the rest.** The
// mixer's reverb slot reaches the eight-knob call but its `param[5..7]` were
// spare until decay, LF damping and diffusion were wired to them, so every slot
// configured before that holds zero there and has to keep the reverb it had.
// `reverb_set` is itself one call to this, so the derivation is written once and
// a slot at rest and a five-knob caller cannot drift apart.
void reverb_derived(float size, float *decay, float *damping_lf,
                    float *diffusion);

// Points `r` at `bytes` of caller memory, which must be at least what
// `reverb_bytes_needed` asked for with the same two arguments. Returns false and
// leaves `r` inert — a zeroed struct, safe to pass to everything here — for a
// null buffer, an out-of-range rate or predelay, or a buffer that is too small.
// Nothing is clamped into fitting: a tank laid out over memory the caller did not
// promise writes past the end on the first tail, long after the call that was
// wrong.
bool reverb_init(Reverb *r, float *buffer, size_t bytes, float sample_rate,
                 float max_predelay_seconds);

// In place, on two separate arrays of `frames` samples each.
void reverb_process(Reverb *r, float *left, float *right, int frames);

}  // namespace fx
}  // namespace ntrk

#endif  // NTRK_FX_REVERB_H_
