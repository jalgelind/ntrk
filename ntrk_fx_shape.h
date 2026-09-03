// ntrk_fx_shape -- memoryless waveshapers and a TPT state-variable filter.
//
// Header-only and dependency-free, the same terms as ntrk.h: no allocation,
// no globals with dynamic initialisation, nothing that can throw. Everything
// here is a pure function of its arguments plus, for the filter, the small
// POD state the caller owns.
//
// **Every shaper is an exact passthrough at drive == 0.** Each one branches on
// `drive <= 0.f` and returns `x` unchanged before touching a single float, so
// bypassing an effect and never calling it produce bit-identical renders --
// which is what lets a test hash a render with the effect "off" and compare
// it against one where the effect is never invoked at all.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_FX_SHAPE_H_
#define NTRK_FX_SHAPE_H_

#include "ntrk_dsp.h"

#include <stddef.h>
#include <stdint.h>

namespace ntrk {
namespace fx {

// ----------------------------------------------------------------------------
// -- Waveshapers
// ----------------------------------------------------------------------------
//
// All four are memoryless: same input, same drive, same output, every time,
// on any target. None of them assume `x` stays within [-1,1] to stay bounded
// themselves -- a hot signal folds or saturates rather than escaping the
// range -- but they are tuned for ordinary audio, not for abuse.

// Scoped, and `uint8_t` because a stored byte -- a SYNTH instrument's
// `synth_dist` -- is what names one. The underlying type is the format's, so a
// cast from the byte the loader already range-checked is a reinterpretation
// and never a conversion.
enum class ShapeKind : uint8_t { kWarm = 0, kCrunch = 1, kTape = 2, kFold = 3 };

inline float clamp11(float x) { return x > 1.f ? 1.f : (x < -1.f ? -1.f : x); }

// `soft_clip` -- the house saturator, shared by the shapers below and by the
// delay's feedback path -- is in `ntrk_dsp.h`, along with why it is a Pade form
// of tanh spelled out in arithmetic rather than a call to `std::tanh`.

// Gentle odd-harmonic soft saturation. The curve is odd, so zero in is exactly
// zero out whatever the drive, and dividing by its value at `k` is what keeps
// it reaching all the way to +-1 at full scale rather than falling short as
// drive rises. `shape_sample` guarantees `drive > 0`, so `k` is in (1, 5] and
// the divisor is never below about 0.78 -- there is no zero to divide by here,
// which there would be at k = 0.
inline float shape_warm(float x, float k) {
  const float norm = soft_clip(k);
  return clamp11(soft_clip(k * x) / norm);
}

// Harder, asymmetric clip: the negative half is driven harder than the
// positive, the way an overdriven single-ended stage clips unevenly. The
// cubic knee (`y - y^3/3`) gives a flatter shoulder than a tanh would, which
// is the "crunch" against warm's "soft".
inline float shape_crunch(float x, float drive) {
  const float k = 1.f + drive * 9.f;
  float y = (x >= 0.f) ? k * x : k * 1.35f * x;
  if (y > 1.f) y = 1.f;
  else if (y < -1.f) y = -1.f;
  return y - (y * y * y) * (1.f / 3.f);
}

// Compressive soft-knee, rational rather than exponential -- a hair cheaper
// than tanh and shaped differently (a longer shoulder, no hard limit). The
// denominator differs slightly by sign for a touch of even-harmonic
// asymmetry, the tape-saturation character the other three don't have.
inline float shape_tape(float x, float drive) {
  const float k = drive * 3.f;
  const float d = (x >= 0.f) ? k : k * 0.85f;
  // Hand-written rather than std::fabs, so this header needs no <cmath> and can
  // be included by the replayer. It differs from fabs only on -0.0f, and here
  // that is arithmetically invisible: 1 + d * -0.0f is 1.
  const float mag = x < 0.f ? -x : x;
  return x / (1.f + d * mag);
}

// A wavefolder: past +-1 the signal reflects back into range rather than
// clipping flat, so driving it harder adds folds instead of a flatter top.
// fmod() turns any input into a point on a period-4 triangle wave; the
// two branches are its rising and falling halves. Bounded in [-1,1] for any
// input, not just |x| <= 1, because the fold is periodic rather than a
// one-sided limit.
inline float shape_fold(float x, float drive) {
  const float y = x * (1.f + drive * 3.f);

  // **Truncation toward zero, because that is what fmod does.** The obvious
  // `a - 4 * floorf(a * 0.25f)` is not the same function: for a = -1.5 fmod
  // gives -1.5 and the floor form gives 2.5, and this folder leans on the
  // negative branch. A cast to int truncates, which is the behaviour wanted.
  //
  // Bounded first: converting an out-of-range float to int is undefined, and
  // nothing promises a sample is in range. Past a million the fold is already
  // meaningless, so clamping there costs nothing real.
  float a = y + 1.f;
  if (a > 1.0e6f) a = 1.0e6f;
  if (a < -1.0e6f) a = -1.0e6f;

  float p = a - 4.f * (float) (int) (a * 0.25f);
  if (p < 0.f) p += 4.f;
  return (p < 2.f) ? (p - 1.f) : (3.f - p);
}

inline float shape_sample(ShapeKind kind, float x, float drive) {
  if (drive <= 0.f) return x;  // the no-op path: bit-identical to bypass
  switch (kind) {
    case ShapeKind::kWarm:   return shape_warm(x, 1.f + drive * 4.f);
    case ShapeKind::kCrunch: return shape_crunch(x, drive);
    case ShapeKind::kTape:   return shape_tape(x, drive);
    case ShapeKind::kFold:   return shape_fold(x, drive);
    // An unknown kind is a no-op, not a guess.
    default:                 return x;
  }
}

// ----------------------------------------------------------------------------
// -- A TPT state-variable filter
// ----------------------------------------------------------------------------
//
// Zero-delay-feedback / "topology-preserving transform" form (Andrew Simper's
// derivation of the trapezoidal-integrated SVF). The point of this topology
// over a direct-form biquad is that `svf_set` may be called every sample with
// a different cutoff and the filter never blows up doing it -- there is no
// buried assumption that the coefficients were constant since the last
// sample, which is exactly the assumption a swept biquad violates.
//
// **Call exactly one of `svf_lowpass` / `svf_highpass` / `svf_bandpass` per
// sample per `Svf` instance.** Each one runs the whole state update and
// advances `ic1`/`ic2`; calling two of them on the same sample would advance
// the state twice and mix outputs from different ticks, not give two views of
// one. Run three instances in parallel (same `svf_set` on each) if a signal
// genuinely needs more than one tap out of the same filter.

// `flush_denorm` is in `ntrk_dsp.h`, with the reasoning: it is a
// compare-and-select and never an add, because this project hashes the render
// and a flush that perturbed the output would itself look like a regression.

// ---- The tan table ---------------------------------------------------------
//
// `g = tan(pi * fc / sr)` is not tabled directly: it has two arguments, and
// std::tan is not correctly rounded, so Apple's libm and emscripten's musl are
// free to disagree in the last bits. This project pins reference hashes over
// rendered audio and applies `-ffp-contract=off` everywhere to keep ARM64 and
// wasm bit-identical, and a libm call in the coefficient path is a silent hole
// in that. A table of literals plus a lerp is identical by construction.
//
// **Parameterisation: normalised frequency `w = fc / sr`, in [0, 0.5).** That
// is what makes it separable -- g depends on fc and sr only through their
// ratio, so one axis covers every sample rate. The table is *uniform* in w
// rather than log-spaced, because a log axis would need a logarithm to find
// the index and that is the libm call we came here to remove.
//
// Uniform spacing on its own is poor near Nyquist, where tan runs into its
// pole and a straight line between two samples of it is nowhere near it. So
// only the smooth quarter is tabled -- 257 points at w = i/1024, covering
// [0, 0.25], where g runs 0 to 1 -- and the top half is read off the same
// table backwards, using cot(pi*w) == tan(pi*(0.5 - w)): above a quarter of
// the sample rate, look up the *complement* and take the reciprocal. Both ends
// then sit in the flat part of the curve and neither has to interpolate across
// the pole.
//
// Worst case measured against std::tan over 5 Hz..20 kHz at ten sample rates
// from 8 kHz to 192 kHz: **5.6e-6 relative**, which is 0.0097 cents of cutoff
// (the cutoff error is never larger than the error in g). A cent is already
// inaudible on a filter; a hundredth of one is not a quantity a filter has.
// 257 floats, 1028 bytes.
const float kSvfTanQuarter[257] = {
    0.f,          0.00306797121f, 0.00613600016f, 0.00920414459f, 0.0122724622f, 0.0153410109f,
    0.0184098482f, 0.0214790329f, 0.0245486218f, 0.0276186727f, 0.0306892451f, 0.0337603949f,
    0.0368321799f, 0.0399046615f, 0.0429778956f, 0.0460519381f, 0.0491268486f, 0.0522026904f,
    0.055279512f, 0.0583573803f, 0.0614363514f, 0.0645164847f, 0.0675978363f, 0.0706804618f,
    0.0737644285f, 0.0768497959f, 0.0799366161f, 0.0830249414f, 0.0861148536f, 0.0892063901f,
    0.0922996253f, 0.0953946039f, 0.0984914005f, 0.101590075f, 0.104690671f, 0.107793264f,
    0.110897914f, 0.114004672f, 0.117113605f, 0.120224774f, 0.123338237f, 0.126454055f,
    0.129572302f, 0.132693022f, 0.135816276f, 0.138942137f, 0.142070681f, 0.145201936f,
    0.148335993f, 0.151472896f, 0.15461272f, 0.157755524f, 0.160901368f, 0.164050311f,
    0.167202443f, 0.170357794f, 0.173516467f, 0.176678479f, 0.179843932f, 0.183012888f,
    0.186185405f, 0.189361542f, 0.192541361f, 0.195724949f, 0.198912367f, 0.202103674f,
    0.205298945f, 0.20849824f, 0.211701617f, 0.214909181f, 0.218120962f, 0.22133705f,
    0.224557504f, 0.227782413f, 0.231011823f, 0.234245807f, 0.237484455f, 0.240727812f,
    0.243975967f, 0.247228995f, 0.25048697f, 0.253749937f, 0.257018f, 0.260291219f,
    0.263569653f, 0.266853422f, 0.270142555f, 0.273437142f, 0.276737273f, 0.280043006f,
    0.283354431f, 0.286671609f, 0.289994627f, 0.293323576f, 0.296658516f, 0.299999505f,
    0.303346694f, 0.306700081f, 0.310059816f, 0.313425928f, 0.316798538f, 0.320177674f,
    0.323563486f, 0.326956034f, 0.330355376f, 0.333761632f, 0.337174863f, 0.340595156f,
    0.344022602f, 0.34745729f, 0.350899309f, 0.354348779f, 0.357805729f, 0.361270279f,
    0.364742517f, 0.368222535f, 0.37171042f, 0.375206292f, 0.378710181f, 0.382222265f,
    0.385742575f, 0.38927123f, 0.392808318f, 0.39635393f, 0.399908185f, 0.403471202f,
    0.40704301f, 0.410623759f, 0.414213568f, 0.417812496f, 0.421420664f, 0.425038159f,
    0.428665102f, 0.432301611f, 0.435947776f, 0.439603716f, 0.443269521f, 0.44694531f,
    0.450631201f, 0.454327285f, 0.458033681f, 0.461750507f, 0.465477914f, 0.469215959f,
    0.472964764f, 0.476724505f, 0.480495214f, 0.484277099f, 0.48807022f, 0.491874725f,
    0.495690703f, 0.499518335f, 0.503357708f, 0.507208943f, 0.511072218f, 0.514947593f,
    0.518835247f, 0.522735298f, 0.526647866f, 0.53057307f, 0.534511149f, 0.538462102f,
    0.542426169f, 0.546403408f, 0.550394058f, 0.554398179f, 0.558415949f, 0.562447488f,
    0.566492975f, 0.570552588f, 0.574626386f, 0.578714609f, 0.582817376f, 0.586934805f,
    0.591067135f, 0.595214427f, 0.599376917f, 0.603554785f, 0.607748091f, 0.611957073f,
    0.61618191f, 0.620422781f, 0.624679744f, 0.628953159f, 0.633243024f, 0.637549579f,
    0.641873062f, 0.646213591f, 0.650571346f, 0.654946566f, 0.659339368f, 0.663749993f,
    0.668178618f, 0.672625482f, 0.677090645f, 0.681574464f, 0.686077058f, 0.690598667f,
    0.695139468f, 0.69969964f, 0.704279482f, 0.708879113f, 0.713498831f, 0.718138814f,
    0.722799242f, 0.727480412f, 0.732182562f, 0.736905873f, 0.741650522f, 0.746416867f,
    0.751205087f, 0.75601542f, 0.760848165f, 0.765703499f, 0.770581663f, 0.775482953f,
    0.780407667f, 0.785355985f, 0.790328205f, 0.795324624f, 0.800345421f, 0.805391014f,
    0.810461581f, 0.81555742f, 0.820678771f, 0.825826049f, 0.830999434f, 0.836199284f,
    0.841425896f, 0.846679509f, 0.85196054f, 0.857269228f, 0.862605929f, 0.867970943f,
    0.873364627f, 0.878787279f, 0.884239197f, 0.889720857f, 0.895232499f, 0.900774479f,
    0.906347156f, 0.911950946f, 0.917586207f, 0.923253238f, 0.928952456f, 0.934684277f,
    0.940449059f, 0.94624722f, 0.952079117f, 0.957945228f, 0.963845909f, 0.969781578f,
    0.975752652f, 0.981759608f, 0.987802863f, 0.993882835f, 1.f,
};

// tan(pi * v) for v in [0, 0.25], by linear interpolation in the table above.
// The NaN guard is not decoration: `(int)` of a NaN is undefined behaviour,
// and a NaN cutoff would otherwise reach it.
inline float svf_tan_quarter(float v) {
  float t = v * 1024.f;                 // 256 intervals across [0, 0.25]
  if (!(t >= 0.f)) t = 0.f;             // false for NaN as well as for negatives
  int i = (int) t;
  if (i > 255) i = 255;                 // v == 0.25 lands exactly on the last entry
  const float f = t - (float) i;
  return kSvfTanQuarter[i] + f * (kSvfTanQuarter[i + 1] - kSvfTanQuarter[i]);
}

struct Svf {
  float ic1 = 0.f;
  float ic2 = 0.f;
  // Cached by svf_set so a fixed cutoff doesn't recompute the coefficients
  // every sample; safe to recompute on every call even so, which is what a
  // fast sweep does -- it is now a table read and a divide, not a tan().
  float a1 = 0.f;
  float a2 = 0.f;
  float a3 = 0.f;
  float k = 0.f;   // 1/Q, needed again by svf_highpass
};

inline void svf_reset(Svf *s) {
  s->ic1 = 0.f;
  s->ic2 = 0.f;
}

inline void svf_set(Svf *s, float cutoff_hz, float resonance, float sample_rate) {
  const float sr = sample_rate > 1.f ? sample_rate : 1.f;

  // tan() diverges at fs/2; 0.49*fs keeps g finite with headroom to spare
  // even when cutoff is swept hard against the ceiling.
  float fc = cutoff_hz;
  const float fc_max = sr * 0.49f;
  if (fc > fc_max) fc = fc_max;
  if (fc < 5.f) fc = 5.f;

  float reso = resonance;
  if (reso < 0.f) reso = 0.f;
  if (reso > 1.f) reso = 1.f;
  // k is the damping term (1/Q): 2 is a critically-damped two-pole, and the
  // floor above zero at resonance == 1 keeps the filter always losing a
  // little energy, so maximum resonance is a sharp peak rather than a
  // literal, never-decaying self-oscillation.
  const float k = 2.f - 1.98f * reso;

  // Normalised frequency is the table's axis; see kSvfTanQuarter. The clamp on
  // w is unreachable at any real sample rate -- fc is already capped at
  // 0.49*sr -- and exists only because the 5 Hz floor is applied *after* that
  // cap, so a sample rate below ~10 Hz can still hand this a w above Nyquist.
  // Clamping keeps g finite there instead of dividing by a zero off the end of
  // the table.
  float w = fc / sr;
  if (w > 0.49f) w = 0.49f;
  // Below a quarter of the sample rate the table is read forwards; above it,
  // cot(pi*w) == tan(pi*(0.5 - w)) reads the same table from the other end, so
  // neither branch interpolates near tan's pole.
  const float g = (w <= 0.25f) ? svf_tan_quarter(w)
                               : 1.f / svf_tan_quarter(0.5f - w);
  const float a1 = 1.f / (1.f + g * (g + k));
  const float a2 = g * a1;
  const float a3 = g * a2;

  s->a1 = a1;
  s->a2 = a2;
  s->a3 = a3;
  s->k = k;
}

// Shared trapezoidal update. v1 is the bandpass tap, v2 the lowpass tap;
// highpass is derived from both without its own state.
inline void svf_update(Svf *s, float x, float *v1_out, float *v2_out) {
  const float v3 = x - s->ic2;
  const float v1 = s->a1 * s->ic1 + s->a2 * v3;
  const float v2 = s->ic2 + s->a2 * s->ic1 + s->a3 * v3;
  // Flushed at the store, not the load: state that decays into denormal
  // range on a quiet tail must not go on costing denormal-handling cycles,
  // and this is a compare-and-select rather than the add that would perturb
  // the very output the flush is trying to leave alone.
  s->ic1 = flush_denorm(2.f * v1 - s->ic1);
  s->ic2 = flush_denorm(2.f * v2 - s->ic2);
  *v1_out = v1;
  *v2_out = v2;
}

inline float svf_lowpass(Svf *s, float x) {
  float v1, v2;
  svf_update(s, x, &v1, &v2);
  return v2;
}

inline float svf_bandpass(Svf *s, float x) {
  float v1, v2;
  svf_update(s, x, &v1, &v2);
  return v1;
}

inline float svf_highpass(Svf *s, float x) {
  float v1, v2;
  svf_update(s, x, &v1, &v2);
  return x - s->k * v1 - v2;
}

}  // namespace fx
}  // namespace ntrk

#endif  // NTRK_FX_SHAPE_H_
