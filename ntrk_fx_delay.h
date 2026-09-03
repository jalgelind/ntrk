// ntrk_fx_delay -- a stereo delay, over memory somebody else owns.
//
// The API and the reasoning are here; the arithmetic is in `ntrk_fx_delay.cc`.
// Compile that file alongside your own, or `#include "ntrk_unity.h"` for a
// single-translation-unit build — both are supported and both are tested, see
// README.md.
//
// Platform-free either way, the same as ntrk.h: standard headers only, no I/O,
// no threads, and **no allocation of any kind**. A `Delay` is a view over a
// buffer the caller supplies, exactly as a loaded `Module` is a view over the
// file bytes — so the buffer must outlive the `Delay`, and resetting is
// forgetting.
//
//     size_t bytes = ntrk::fx::delay_bytes_needed(0.5f, 48000.f);
//     float *mem = ...;                  // wherever your audio memory comes from
//     ntrk::fx::Delay delay;
//     if (!ntrk::fx::delay_init(&delay, mem, bytes, 48000.f, 0.5f))
//       ...                              // too small; nothing was written
//     ntrk::fx::delay_set(&delay, 0.25f, 0.6f, 0.3f, 0.35f, false);
//     ntrk::fx::delay_process(&delay, left, right, frames);
//
// `delay_process` works in place on two separate channel arrays.
//
// **`mix == 0` is a bit-identical passthrough.** The samples are not touched at
// all, so the effect is a provable no-op when it is off rather than one that
// merely sounds like one. The cost of that guarantee is that the line also stops
// filling: raising the mix again starts a fresh tail instead of revealing one
// that was running silently. That is the right trade for a build whose tests
// hash the rendered audio.
//
// Deterministic on ARM64 and wasm. Everything here is add, multiply, divide and
// compare — no libm calls in the audio path, because `tanhf` and friends are not
// bit-identical between platforms and a hashed render would disagree. Nothing
// relies on fast-math or on flush-to-zero being set; denormals are handled by
// hand, in ntrk_dsp.h.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_FX_DELAY_H_
#define NTRK_FX_DELAY_H_

#include "ntrk_dsp.h"

#include <stddef.h>
#include <stdint.h>

namespace ntrk {
namespace fx {

// The bounds and the glide length are in `ntrk_fx_delay.cc`: nothing outside
// the delay reads them, `delay_set` clamps to them rather than refusing, and
// the ranges a caller has to know are written out at `delay_set` below.

// **The buffer is two halves, not interleaved**: `capacity` frames of left
// followed by `capacity` frames of right, which is what `delay_process` is
// handed and so costs no deinterleaving. One write cursor serves both.
struct Delay {
  float *buffer = nullptr;  // caller-owned; must outlive the Delay
  int capacity = 0;         // frames per channel, so 2 * capacity floats
  int write = 0;            // shared cursor into both halves

  float rate = 0.f;         // sample rate the times were computed against
  float time_target = 0.f;  // frames, what delay_set asked for
  float time_cur = 0.f;     // frames, glided towards the target per sample
  float glide = 0.f;        // per-sample approach coefficient

  float feedback = 0.f;
  float damping = 0.f;
  float mix = 0.f;
  bool ping_pong = false;

  // A fresh line has nothing to bend, so the first block after a reset arrives
  // at whatever time was asked for instead of gliding up to it from the last
  // one. Without this an impulse into a just-reset delay comes back late, which
  // looks exactly like the time being wrong.
  bool snap = true;

  float lp_left = 0.f;      // one-pole damping state, one per line
  float lp_right = 0.f;
};

float delay_clamp(float v, float lo, float hi);

// What `delay_init` will demand for these arguments. `max_seconds` is clamped to
// the same ceiling init uses, so a caller asking for ten seconds is sized for two
// rather than being handed a buffer init then refuses to fill.
size_t delay_bytes_needed(float max_seconds, float sample_rate);

// Zeroes the line and the filter state. Cheap, and the only way to drop a tail.
void delay_reset(Delay *d);

// Refuses rather than trusts: the buffer is measured against what `max_seconds`
// at this rate actually needs before a single float is written through it, the
// same way module_load checks each block against what is left of the file. A
// refused Delay is left zeroed, and `delay_process` on one is a passthrough
// rather than a crash.
//
// Extra bytes are ignored — capacity follows `max_seconds`, so the tap range does
// not silently change with the size of the allocation behind it.
bool delay_init(Delay *d, float *buffer, size_t bytes, float sample_rate,
                float max_seconds);

// time_seconds  1 ms .. 2 s, further clamped to what the buffer can hold.
// feedback      0 .. 1.05. Past unity is deliberate and is not a mistake to
//               guard against: the feedback path is soft-saturated, so a runaway
//               settles into clipped self-oscillation instead of diverging to
//               infinity and then to NaN.
// damping       0 .. 0.95, a one-pole lowpass inside the feedback path. This is
//               what makes repeats darken as they decay rather than hand back a
//               harsh copy of the source for ever.
// mix           0 .. 1, linear. 0 is an exact passthrough; see the file header.
// ping_pong     repeats alternate across the stereo field.
//
// Time is glided (see kDelayGlideSeconds), so moving it pitch-bends the tail the
// way tape does rather than clicking. Only a change to a *running* delay glides:
// the first block after init or delay_reset takes the time as given. The gain
// parameters take effect at once, which is inaudible at a block boundary and is
// a step if it is done per sample.
void delay_set(Delay *d, float time_seconds, float feedback, float damping,
               float mix, bool ping_pong);

// ponytail: linear interpolation on the fractional tap. It loses a little top
// end while the time glides and nothing while it is still; move to allpass or
// cubic only if a glide is ever heard to dull.
float delay_tap(const float *line, int capacity, int write, float frames_back);

void delay_process(Delay *d, float *left, float *right, int frames);

}  // namespace fx
}  // namespace ntrk

#endif  // NTRK_FX_DELAY_H_
