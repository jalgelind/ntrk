// ntrk_dsp -- the arithmetic primitives the effect modules share.
//
// Header-only, and inline for a reason rather than by habit: both of these run
// once or twice *per sample*, inside the innermost loop of a filter, where a
// call would cost more than the work. That is the line the rest of this
// directory is split along — per-sample code lives in a header so it can
// inline, block-rate code lives in a translation unit where the call is
// amortised over a whole buffer. See README.md.
//
// No allocation, no libm, no dependence on rounding mode or on fast-math.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_DSP_H_
#define NTRK_DSP_H_

namespace ntrk {
namespace fx {

// **One definition each, and no `#ifndef` guard around them.** These were once
// copied into every effect header so each could compile alone, guarded so the
// copies would still compose; a guard makes whichever header is reached first
// win, which is a silent way for two edited-apart copies to disagree by include
// order. Everything that wants them includes this file instead.

// Subnormal floats cost hundreds of cycles a sample on x86, and a feedback path
// decaying towards silence is precisely where one crosses into them. A delay
// tail does it a few hundred samples after the input stops; a reverb at a comb
// feedback of 0.7 then spends *thousands* of samples down there, on every tail,
// for as long as the game runs. On hardware that traps subnormal arithmetic into
// microcode this is a cliff rather than a cost.
//
// Three things about the fix, and all three are deliberate:
//
// - **No DC offset.** The traditional dodge adds a tiny constant to the feedback
//   path so the signal never reaches subnormal magnitudes. This project's tests
//   hash the rendered audio, so injected noise — however inaudible — becomes part
//   of the fingerprint, and every reference hash then depends on the exact
//   constant and where it was added. Compare-and-select changes nothing that was
//   not already going to round to zero.
// - **Unconditional, not measured.** The penalty is an x86 phenomenon. It will
//   not reproduce on the Apple Silicon machine this was developed on, where
//   subnormals run at full speed, so a profile taken here would say the flush is
//   free and could be removed — and the removal would only show up on somebody
//   else's desktop. It stays in because of where it runs, not because of what it
//   measures.
// - **Apply it at the store, never at the load.** The value that has to be
//   killed is the one going back into the recursion; flushing on the way out
//   just pays for the branch twice.
inline float
flush_denorm(float x) {
  return (x > -1e-20f && x < 1e-20f) ? 0.f : x;
}

// Bounded, monotone, and slope exactly 1 at the origin: the Padé form of tanh,
// spelled out so it is arithmetic rather than a libm call. The project pins
// hashes over rendered audio and requires ARM64 and wasm to compute identical
// floats — arithmetic cannot disagree, a transcendental can.
//
// It saturates at ±1 instead of diverging, which is what lets a feedback path
// run past unity on purpose: a runaway settles into clipped self-oscillation
// rather than reaching infinity and then NaN. See `delay_set`.
inline float
soft_clip(float x) {
  if (x <= -3.f)
    return -1.f;
  if (x >= 3.f)
    return 1.f;
  const float x2 = x * x;
  return x * (27.f + x2) / (27.f + 9.f * x2);
}

}  // namespace fx
}  // namespace ntrk

#endif  // NTRK_DSP_H_
