// ntrk_fx_reverb -- the tank's arithmetic. See ntrk_fx_reverb.h for the API,
// the topology and the reasoning behind it.
//
// A translation unit rather than a header because every function here is called
// once per *block*: the per-sample work is a loop inside `reverb_process`, so
// there is nothing for an inline to save and a call is amortised over a whole
// buffer. The per-sample primitives it uses stay inline, in ntrk_dsp.h.
//
// This file may also be `#include`d into one translation unit of your own — that
// is what ntrk_unity.h does. Compile it separately or include it, not both.
//
// **There is no `<cmath>` here, and that is load-bearing rather than tidy.**
// The coefficients this file computes are exponentials, `std::exp` is not
// correctly rounded, and Apple's libm and emscripten's musl are entitled to
// differ in the last bits. This project pins reference hashes over rendered
// audio and requires ARM64 and wasm to agree, so the exponential comes off a
// table of literals — see `kReverbExp2Neg` — and every rounding is spelled out
// as arithmetic. `tools/check_ntrk_crosstarget.sh` is what proves it.
//
// Public domain / CC0. Written for the no2 project.

#include "ntrk_fx_reverb.h"

namespace ntrk {
namespace fx {

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

// The eight FDN delay lengths, in samples at 48 kHz, at the largest size the
// `size` knob reaches divided out — that is, these are the lengths at size
// scale 1.0. All eight are prime, which matters far less in an FDN than it did
// for parallel combs (every line is mixed into every other line on every
// sample, so a shared factor cannot build a shared resonance the way two
// independent combs can) but costs nothing to keep.
//
// The spread is 18 ms to 63 ms, which is a room rather than a hall: long enough
// that the modes are dense at the bottom, short enough that the first echoes
// arrive while the ear still reads them as the same event. The `size` knob
// takes them from half that to one and a half times it, so 9 ms to 94 ms across
// the whole sweep.
//
// ponytail: the lines are not modulated. The reference wobbles its read
// pointers by a fraction of a sample at well under a hertz, which is what stops
// a sparser FDN sounding metallic; this one is dense enough not to need it.
// Adding it means a fractional read and an LFO without libm — a parabolic sine
// or a two-pole resonator — and it would move every pinned hash.
const int kReverbFdnTuning[kReverbLines] = {
  863, 1093, 1289, 1531, 1811, 2141, 2531, 3023,
};

// The Gardner diffuser: four *series* allpasses, not the nested ones his paper
// draws, because a nested pair costs a second read and write per stage for
// density this already has. Lengths in samples at 48 kHz — 3.0 ms, 7.9 ms,
// 2.2 ms and 5.8 ms. The long-short-long-short ordering is the point: two long
// stages in a row multiply their echo counts against a sparse input and leave
// audible repeats.
const int kReverbDiffuserTuning[kReverbDiffusers] = {142, 379, 107, 277};

// One short allpass per output channel, on the wet path only. Different lengths
// on the two sides is the whole mechanism: the tank is fed a mono sum, so
// without this the two outputs differ only by their tap signs and the image
// collapses to the middle the moment the taps happen to agree.
const int kReverbDecorrTuning[2] = {211, 179};

// Where along each line the output taps sit, as a fraction of that line's
// delay. The last is the line's own output, which the feedback path has already
// read — so 32 taps cost 24 extra reads rather than 32.
//
// ponytail: these are also the early reflections. The reference runs a separate
// 200 ms FIR off baked image-source tables for that, which is a table per room
// and a second buffer; the taps here reach 41% of the shortest line, which is
// 7.4 ms at size 0.5, and that is early enough to do the job. A real image
// source list is the upgrade if a specific room is ever wanted.
const float kReverbTapFraction[kReverbTaps] = {0.41f, 0.67f, 0.91f, 1.f};

// 1/sqrt(32): the taps are 32 roughly-uncorrelated signals summed, so their sum
// grows as the square root of the count rather than as the count.
const float kReverbTapScale = 0.176776695f;

// 1/sqrt(8), the Hadamard butterfly's normalisation. **This is why the matrix
// is a Hadamard and not a general orthogonal one**: the butterfly is adds and
// subtracts plus this single scale, so it is exactly orthogonal *in floating
// point* — an add and a subtract of the same pair lose nothing between them.
// A general rotation would be orthogonal in exact arithmetic and only nearly so
// in float, and "nearly" is not a property a cross-target hash check accepts.
const float kReverbHadamardNorm = 0.353553391f;

// The two shelves' corner frequencies. 250 Hz splits "bass" from the body of an
// instrument; 5 kHz is where air absorption actually starts to bite in a real
// room, and putting it lower makes damping sound like a closed lid instead of
// like distance.
const float kReverbBassCorner = 250.f;
const float kReverbTrebleCorner = 5000.f;

// How much of each band the damping knobs may take away per pass, at full
// travel. Neither may exceed 1: a shelf gain above unity would put the loop
// gain above the line gain and the stability argument in the header depends on
// it not doing that.
const float kReverbDampHfDepth = 0.9f;
const float kReverbDampLfDepth = 0.85f;

// RT60 range for the decay knob, logarithmic between them.
// kReverbRt60Octaves is log2(30 / 0.15).
const float kReverbRt60Min = 0.15f;
const float kReverbRt60Octaves = 7.64385619f;

// A line gain of exactly 1 is an oscillator and there is nothing useful between
// this and one.
const float kReverbMaxLoopGain = 0.99995f;

// How far the `size` knob moves the delay lengths. The bottom is a small live
// room, the top a large one; the ratio is what the ear reads as size, so a
// linear sweep of the knob is a linear sweep of the room's longest dimension.
const float kReverbSizeMin = 0.5f;
const float kReverbSizeMax = 1.5f;

// The diffuser coefficient, from the diffusion knob. Below about 0.5 the
// allpasses stop smearing and start sounding like four discrete echoes; above
// 0.99 an allpass rings for long enough to be a resonator of its own.
const float kReverbDiffusionMin = 0.5f;
const float kReverbDiffusionRange = 0.35f;

// Input gain into the tank. Measured rather than derived: with 32 taps summed
// at 1/sqrt(32) and eight lines fed in parallel, this is what lines the wet
// level up with what the comb bank this replaced produced, so a caller's
// existing send level still means what it did.
//
// **That is not unity, and the wet path can exceed full scale.** Peak / RMS
// against ten seconds of continuous full-scale noise at `mix = 1`, across the
// size and damping grid:
//
//        damp 0.0     damp 0.5     damp 1.0
//   size 0.0   1.70/0.34    1.34/0.25    1.20/0.23
//   size 0.5   2.23/0.46    1.21/0.27    1.20/0.24
//   size 1.0   3.69/0.74    1.43/0.30    1.23/0.27
//
// So the worst case is a large, undamped tank at about 3.7x — bounded and
// steady over half a minute, and still well past what a caller may add to a mix
// unattenuated. Damping is what pulls it back; it costs more than size does.
// Attenuate at the send: a reverb that quietly rescales itself as its knobs
// move is not the reverb its tuning describes.
//
// The grid is written out rather than summarised because a single number here
// was wrong once already, and a wrong number in a comment costs more than no
// number at all.
const float kReverbInputGain = 0.22f;

// The lengths above are a *duration*, so the reference rate they were measured
// at has to be divided back out. This is the classic bug in the family: taken
// literally at another rate every line is out by the rate ratio, and since the
// decay gain is computed from the line length in samples, so is every RT60 the
// decay knob promises.
const float kReverbTuningRate = 48000.f;

// Refused outside these. Both bounds exist to keep the sample counts below in
// int range and the buffer request something a caller might plausibly satisfy.
const float kReverbMinRate = 8000.f;
const float kReverbMaxRate = 384000.f;
const float kReverbMaxPredelay = 4.f;

// ---------------------------------------------------------------------------
// The exponential, without libm
// ---------------------------------------------------------------------------
//
// Everything this file computes at parameter-set time is an exponential: a
// shelf pole is `exp(-2 pi fc / sr)`, a Jot decay gain is
// `pow(10, -3 L / (rt60 sr))`, and an RT60 knob is `pow(200, decay)`. All three
// are `2^-x` with a scale on x, so there is one table and one helper rather
// than three of each.
//
// **Only the fractional part is tabled.** 2^-x for x in [0, 1] is smooth and
// nearly straight; the integer part is exact halving, which in binary floating
// point is exact until it reaches subnormals and then goes to zero, which is
// where a decay gain wants to end up anyway. 257 points at x = i/256, linear
// interpolation between: **worst case 1.0e-6 relative**, measured end to end —
// the helper below against `pow(2, -x)` in double over x in [0, 20], so the
// halvings and the lerp's own rounding are in the figure. On an RT60 that is a
// relative
// error of about 2e-4 at the long end — a fifth of a millisecond in a second —
// which is not a quantity a room has. 257 floats, 1028 bytes.
//
// This is the same argument as `kSvfTanQuarter` in ntrk_fx_shape.h, and it is
// made twice on purpose: neither module may include the other.
const float kReverbExp2Neg[257] = {
    1.0f, 0.997296056f, 0.994599423f, 0.991910082f, 0.989228013f, 0.986553196f,
    0.983885612f, 0.98122524f, 0.978572062f, 0.975926058f, 0.973287209f, 0.970655495f,
    0.968030897f, 0.965413395f, 0.962802972f, 0.960199607f, 0.957603281f, 0.955013975f,
    0.952431671f, 0.949856349f, 0.947287991f, 0.944726577f, 0.94217209f, 0.939624509f,
    0.937083817f, 0.934549995f, 0.932023024f, 0.929502886f, 0.926989563f, 0.924483035f,
    0.921983284f, 0.919490293f, 0.917004043f, 0.914524516f, 0.912051693f, 0.909585556f,
    0.907126088f, 0.90467327f, 0.902227084f, 0.899787512f, 0.897354538f, 0.894928141f,
    0.892508306f, 0.890095013f, 0.887688246f, 0.885287987f, 0.882894218f, 0.880506922f,
    0.87812608f, 0.875751677f, 0.873383693f, 0.871022113f, 0.868666918f, 0.866318091f,
    0.863975615f, 0.861639474f, 0.859309649f, 0.856986124f, 0.854668882f, 0.852357905f,
    0.850053177f, 0.847754681f, 0.8454624f, 0.843176317f, 0.840896415f, 0.838622679f,
    0.83635509f, 0.834093633f, 0.83183829f, 0.829589046f, 0.827345884f, 0.825108787f,
    0.822877739f, 0.820652724f, 0.818433725f, 0.816220726f, 0.814013711f, 0.811812664f,
    0.809617568f, 0.807428407f, 0.805245166f, 0.803067828f, 0.800896378f, 0.798730799f,
    0.796571076f, 0.794417192f, 0.792269133f, 0.790126881f, 0.787990423f, 0.785859741f,
    0.78373482f, 0.781615645f, 0.7795022f, 0.77739447f, 0.775292439f, 0.773196092f,
    0.771105413f, 0.769020387f, 0.766940999f, 0.764867233f, 0.762799075f, 0.760736509f,
    0.758679521f, 0.756628094f, 0.754582214f, 0.752541866f, 0.750507035f, 0.748477706f,
    0.746453864f, 0.744435495f, 0.742422583f, 0.740415114f, 0.738413073f, 0.736416445f,
    0.734425217f, 0.732439372f, 0.730458897f, 0.728483777f, 0.726513998f, 0.724549545f,
    0.722590403f, 0.72063656f, 0.718687999f, 0.716744707f, 0.714806669f, 0.712873872f,
    0.710946301f, 0.709023942f, 0.707106781f, 0.705194804f, 0.703287997f, 0.701386346f,
    0.699489836f, 0.697598455f, 0.695712188f, 0.693831021f, 0.691954941f, 0.690083934f,
    0.688217985f, 0.686357083f, 0.684501211f, 0.682650359f, 0.68080451f, 0.678963653f,
    0.677127773f, 0.675296858f, 0.673470893f, 0.671649866f, 0.669833762f, 0.668022569f,
    0.666216274f, 0.664414862f, 0.662618322f, 0.660826639f, 0.659039801f, 0.657257794f,
    0.655480606f, 0.653708223f, 0.651940633f, 0.650177822f, 0.648419777f, 0.646666487f,
    0.644917937f, 0.643174115f, 0.641435008f, 0.639700604f, 0.637970889f, 0.636245852f,
    0.634525479f, 0.632809757f, 0.631098675f, 0.62939222f, 0.627690379f, 0.625993139f,
    0.624300489f, 0.622612415f, 0.620928906f, 0.619249949f, 0.617575532f, 0.615905642f,
    0.614240268f, 0.612579397f, 0.610923016f, 0.609271115f, 0.60762368f, 0.6059807f,
    0.604342162f, 0.602708055f, 0.601078366f, 0.599453084f, 0.597832196f, 0.596215691f,
    0.594603558f, 0.592995783f, 0.591392355f, 0.589793264f, 0.588198496f, 0.58660804f,
    0.585021885f, 0.583440018f, 0.581862429f, 0.580289106f, 0.578720037f, 0.57715521f,
    0.575594615f, 0.574038239f, 0.572486072f, 0.570938102f, 0.569394317f, 0.567854707f,
    0.56631926f, 0.564787964f, 0.563260809f, 0.561737784f, 0.560218876f, 0.558704076f,
    0.557193371f, 0.555686752f, 0.554184206f, 0.552685723f, 0.551191292f, 0.549700901f,
    0.548214541f, 0.5467322f, 0.545253866f, 0.54377953f, 0.542309181f, 0.540842807f,
    0.539380399f, 0.537921945f, 0.536467434f, 0.535016856f, 0.5335702f, 0.532127456f,
    0.530688614f, 0.529253661f, 0.527822589f, 0.526395387f, 0.524972043f, 0.523552548f,
    0.522136891f, 0.520725062f, 0.519317051f, 0.517912847f, 0.51651244f, 0.515115819f,
    0.513722975f, 0.512333896f, 0.510948574f, 0.509566998f, 0.508189157f, 0.506815042f,
    0.505444643f, 0.504077949f, 0.502714951f, 0.501355638f, 0.5f,
};

// log2(e) and log2(10), so `exp(-a)` and `pow(10, -a)` both reach the one table.
const float kReverbLog2E = 1.44269504f;
const float kReverbLog2Ten = 3.32192809f;

// 2^-x for x >= 0. Negatives and NaN come back as 1, which is the identity for
// every caller here — a NaN parameter then produces a flat shelf and a silent
// tank rather than a NaN that the feedback path keeps for ever.
static float
reverb_exp2_neg(float x) {
  if (!(x > 0.f))
    return 1.f;
  if (x >= 126.f)               // below any float that survives the halving
    return 0.f;
  int n = (int) x;
  float t = (x - (float) n) * 256.f;
  int i = (int) t;
  if (i > 255)                  // a fraction of exactly 1 cannot happen, but a
    i = 255;                    // rounding of one that is nearly 1 can
  const float f = t - (float) i;
  float y = kReverbExp2Neg[i] + f * (kReverbExp2Neg[i + 1] - kReverbExp2Neg[i]);
  while (n-- > 0)
    y *= 0.5f;                  // exact in binary floating point, until zero
  return y;
}

// Round-half-up for a non-negative float. `(int)` of a NaN is undefined
// behaviour and a NaN sample rate would otherwise reach it.
static int
reverb_round_pos(float x) {
  if (!(x > 0.f))
    return 0;
  return (int) (x + 0.5f);
}

int
reverb_scale_len(int tuned, float sample_rate) {
  const int n = reverb_round_pos((float) tuned * sample_rate / kReverbTuningRate);
  return n < 1 ? 1 : n;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

bool
reverb_layout(ReverbLayout *l, float sample_rate, float max_predelay_seconds) {
  // Written as negated comparisons so a NaN falls through to the refusal. A NaN
  // rate would otherwise produce a NaN length, an int conversion that is
  // undefined, and a layout nobody can check.
  if (!(sample_rate >= kReverbMinRate) || !(sample_rate <= kReverbMaxRate))
    return false;
  if (!(max_predelay_seconds >= 0.f) ||
      !(max_predelay_seconds <= kReverbMaxPredelay))
    return false;

  int total = 0;

  // **Laid out at the largest size, not the current one.** `size` moves the
  // read pointer inside these, so the knob costs nothing to turn and a caller
  // never has to re-init to change rooms.
  for (int i = 0; i < kReverbLines; ++i) {
    const int base = reverb_scale_len(kReverbFdnTuning[i], sample_rate);
    int cap = reverb_round_pos((float) base * kReverbSizeMax);
    if (cap < 1)
      cap = 1;
    l->line[i] = cap;
    total += cap;
  }
  for (int i = 0; i < kReverbDiffusers; ++i) {
    l->diffuser[i] = reverb_scale_len(kReverbDiffuserTuning[i], sample_rate);
    total += l->diffuser[i];
  }
  for (int i = 0; i < 2; ++i) {
    l->decorr[i] = reverb_scale_len(kReverbDecorrTuning[i], sample_rate);
    total += l->decorr[i];
  }

  // One predelay line, not two: the tank is fed a mono sum, so the delay happens
  // before the signal splits. The extra element is what lets a delay of exactly
  // `max_predelay_seconds` still read a slot the write has not reached.
  l->predelay = reverb_round_pos(max_predelay_seconds * sample_rate) + 1;
  total += l->predelay;

  l->total = total;
  return true;
}

size_t
reverb_bytes_needed(float sample_rate, float max_predelay_seconds) {
  ReverbLayout l = ReverbLayout();
  if (!reverb_layout(&l, sample_rate, max_predelay_seconds))
    return 0;
  return (size_t) l.total * sizeof(float);
}

void
reverb_reset(Reverb *r) {
  if (r == nullptr || r->buffer == nullptr)
    return;
  for (int i = 0; i < r->capacity; ++i)
    r->buffer[i] = 0.f;
  for (int i = 0; i < kReverbLines; ++i) {
    ReverbLine &ln = r->line[i];
    ln.pos = 0;
    ln.lo_x = 0.f;
    ln.lo_y = 0.f;
    ln.hi_x = 0.f;
    ln.hi_y = 0.f;
  }
  for (int i = 0; i < kReverbDiffusers; ++i)
    r->diffuser[i].pos = 0;
  for (int i = 0; i < 2; ++i)
    r->decorr[i].pos = 0;
  r->pre_pos = 0;
}

float
reverb_clamp01(float x) {
  if (!(x > 0.f))               // negatives and NaN both land here
    return 0.f;
  return x > 1.f ? 1.f : x;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

// One first-order shelf: `(b0 + b1 z^-1) / (1 - p z^-1)`, exactly `g_lo` at DC,
// exactly `g_hi` at Nyquist, monotone between. That last property is the whole
// stability argument for the tank — with both gains at or below 1 the shelf can
// only ever attenuate, so the loop gain never exceeds the line gain and an
// orthogonal mixing matrix cannot make energy the lines did not have.
static void
reverb_shelf(float corner_hz, float sample_rate, float g_lo, float g_hi,
             float *p_out, float *b0_out, float *b1_out) {
  float p = reverb_exp2_neg(6.28318531f * corner_hz / sample_rate * kReverbLog2E);
  if (p > 0.9999f)              // a pole at 1 is an integrator, not a shelf
    p = 0.9999f;
  const float lo = g_lo * (1.f - p);
  const float hi = g_hi * (1.f + p);
  *p_out = p;
  *b0_out = 0.5f * (lo + hi);
  *b1_out = 0.5f * (lo - hi);
}

void
reverb_set_full(Reverb *r, float size, float decay, float damping_hf,
                float damping_lf, float diffusion, float predelay_seconds,
                float width, float mix) {
  if (r == nullptr)
    return;
  size = reverb_clamp01(size);
  decay = reverb_clamp01(decay);
  damping_hf = reverb_clamp01(damping_hf);
  damping_lf = reverb_clamp01(damping_lf);
  diffusion = reverb_clamp01(diffusion);
  width = reverb_clamp01(width);
  mix = reverb_clamp01(mix);

  const float rate = r->sample_rate;
  if (rate > 0.f) {
    const float scale = kReverbSizeMin + (kReverbSizeMax - kReverbSizeMin) * size;

    // Jot's normalisation: `g = 10^(-3 L / (rt60 sr))` gives every line the
    // same decay *in seconds* regardless of its length, which is what makes
    // RT60 a knob rather than an emergent property of the tuning.
    const float rt60 = kReverbRt60Min /
                       reverb_exp2_neg(decay * kReverbRt60Octaves);
    const float per_sample = 3.f * kReverbLog2Ten / (rt60 * rate);

    int previous = 0;
    for (int i = 0; i < kReverbLines; ++i) {
      ReverbLine &ln = r->line[i];
      const int base = reverb_scale_len(kReverbFdnTuning[i], rate);
      int d = reverb_round_pos((float) base * scale);
      if (d < 1)
        d = 1;
      // Two lines of equal length are one line with twice the gain, and at the
      // smallest sizes rounding can make neighbours collide. Nudging is enough
      // — the tuning is already sorted, so this only ever moves by a sample.
      if (d <= previous)
        d = previous + 1;
      if (d > ln.cap)
        d = ln.cap;
      previous = d;
      ln.delay = d;

      for (int k = 0; k < kReverbTaps - 1; ++k) {
        int t = reverb_round_pos(kReverbTapFraction[k] * (float) d);
        if (t < 1)
          t = 1;
        if (t > d)
          t = d;
        ln.tap[k] = t;
      }
      ln.tap[kReverbTaps - 1] = d;   // the line's own output, already read

      float g = reverb_exp2_neg(per_sample * (float) d);
      if (g > kReverbMaxLoopGain)
        g = kReverbMaxLoopGain;
      ln.gain = g;
    }

    // The bass bank touches only the bass; the treble bank only the treble.
    reverb_shelf(kReverbBassCorner, rate, 1.f - kReverbDampLfDepth * damping_lf,
                 1.f, &r->lo_p, &r->lo_b0, &r->lo_b1);
    reverb_shelf(kReverbTrebleCorner, rate, 1.f,
                 1.f - kReverbDampHfDepth * damping_hf, &r->hi_p, &r->hi_b0,
                 &r->hi_b1);
  }

  r->diffusion = kReverbDiffusionMin + kReverbDiffusionRange * diffusion;

  int pre = 0;
  if (predelay_seconds > 0.f && rate > 0.f)
    pre = reverb_round_pos(predelay_seconds * rate);
  if (pre > r->pre_max)
    pre = r->pre_max;
  r->predelay = pre;

  // width 1 keeps the two outputs apart, width 0 folds them to the same mono
  // signal on both. Anything between is a crossfade, so the total wet energy
  // holds still as the image narrows.
  r->wet1 = mix * (width * 0.5f + 0.5f);
  r->wet2 = mix * ((1.f - width) * 0.5f);
  r->dry = 1.f - mix;
  r->mix = mix;
}

// What the three controls the five-knob call has no room for are set to.
//
// The decay range is deliberately narrower than the eight-knob call's: `size`
// has to move the tail as well as the room, because RT60 is a property of the
// decay knob alone and a `size` that only changed the delay lengths would leave
// a caller's "bigger" sounding like the same tail in a different box. 0.25 to
// 0.70 of the decay knob is 0.56 s to 7.1 s, which is the range the Freeverb
// version's size knob covered.
const float kReverbDerivedDecayBase = 0.22f;
const float kReverbDerivedDecaySpan = 0.55f;

// A real room loses its bass slowly, so this is small — but not zero. Without
// any bass damping at all a large tank with a long tail booms, and the
// five-knob caller has no way to ask it not to.
const float kReverbDerivedDampLf = 0.25f;

// The reference's default, and about the mean of Gardner's own per-stage
// coefficients.
const float kReverbDerivedDiffusion = 0.7f;

void
reverb_derived(float size, float *decay, float *damping_lf, float *diffusion) {
  const float s = reverb_clamp01(size);
  if (decay != nullptr)
    *decay = kReverbDerivedDecayBase + kReverbDerivedDecaySpan * s;
  if (damping_lf != nullptr)
    *damping_lf = kReverbDerivedDampLf;
  if (diffusion != nullptr)
    *diffusion = kReverbDerivedDiffusion;
}

void
reverb_set(Reverb *r, float size, float damping, float predelay_seconds,
           float width, float mix) {
  const float s = reverb_clamp01(size);
  float decay = 0.f, damping_lf = 0.f, diffusion = 0.f;
  reverb_derived(s, &decay, &damping_lf, &diffusion);
  reverb_set_full(r, s, decay, damping, damping_lf, diffusion,
                  predelay_seconds, width, mix);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool
reverb_init(Reverb *r, float *buffer, size_t bytes, float sample_rate,
            float max_predelay_seconds) {
  if (r == nullptr)
    return false;
  *r = Reverb();
  if (buffer == nullptr)
    return false;

  ReverbLayout l = ReverbLayout();
  if (!reverb_layout(&l, sample_rate, max_predelay_seconds))
    return false;
  if (bytes < (size_t) l.total * sizeof(float))
    return false;

  int at = 0;
  for (int i = 0; i < kReverbLines; ++i) {
    r->line[i].at = at;
    r->line[i].cap = l.line[i];
    r->line[i].delay = l.line[i];
    for (int k = 0; k < kReverbTaps; ++k)
      r->line[i].tap[k] = l.line[i];
    at += l.line[i];
  }
  for (int i = 0; i < kReverbDiffusers; ++i) {
    r->diffuser[i].at = at;
    r->diffuser[i].len = l.diffuser[i];
    at += l.diffuser[i];
  }
  for (int i = 0; i < 2; ++i) {
    r->decorr[i].at = at;
    r->decorr[i].len = l.decorr[i];
    at += l.decorr[i];
  }
  r->pre_at = at;
  r->pre_len = l.predelay;
  r->pre_max = l.predelay - 1;
  at += l.predelay;

  r->buffer = buffer;
  r->capacity = at;             // == l.total, by construction
  r->sample_rate = sample_rate;

  reverb_reset(r);
  // Starts switched off, so a reverb that is initialised and then forgotten is
  // provably silent rather than quietly colouring everything.
  reverb_set(r, 0.5f, 0.5f, 0.f, 1.f, 0.f);
  return true;
}

// ---------------------------------------------------------------------------
// Processing
// ---------------------------------------------------------------------------

// A true Schroeder allpass: flat magnitude at every frequency, so a chain of
// them multiplies the echo count without colouring anything. Freeverb's version
// uses a feedforward gain of -1 instead of `g`, which is not an allpass and is
// audibly not one — it is the reason four of them in series still sounded like
// a metal box.
static inline float
reverb_allpass(float *buf, ReverbAllpass *ap, float x, float g) {
  float *const line = buf + ap->at;
  const float d = line[ap->pos];
  const float v = x - g * d;
  line[ap->pos] = flush_denorm(v);
  if (++ap->pos >= ap->len)
    ap->pos = 0;
  return g * v + d;
}

void
reverb_process(Reverb *r, float *left, float *right, int frames) {
  if (r == nullptr || r->buffer == nullptr || left == nullptr ||
      right == nullptr || frames <= 0)
    return;

  // The exact passthrough. Returning is the only way to guarantee it: a dry gain
  // of 1.0 and a wet gain of 0.0 is an identity for every finite float except
  // -0.0, which comes back as +0.0 and changes the hash.
  if (r->mix <= 0.f)
    return;

  float *const buf = r->buffer;
  const float diffusion = r->diffusion;
  const float lo_p = r->lo_p, lo_b0 = r->lo_b0, lo_b1 = r->lo_b1;
  const float hi_p = r->hi_p, hi_b0 = r->hi_b0, hi_b1 = r->hi_b1;
  const float wet1 = r->wet1;
  const float wet2 = r->wet2;
  const float dry = r->dry;

  for (int i = 0; i < frames; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    float input = (in_l + in_r) * kReverbInputGain;

    // Predelay. No flush here: a plain delay has no recursion, so it can only
    // carry out what was put in, and flushing would quietly zero a genuinely
    // quiet input instead of a decayed one.
    {
      float *const line = buf + r->pre_at;
      int read = r->pre_pos - r->predelay;
      if (read < 0)
        read += r->pre_len;
      line[r->pre_pos] = input;
      input = line[read];       // predelay 0 reads back what was just written
      if (++r->pre_pos >= r->pre_len)
        r->pre_pos = 0;
    }

    // The Gardner diffuser. **This runs before the tank, not after it**, which
    // is the structural difference from Freeverb: the FDN is fed something that
    // is already noise, so its very first reflection is dense rather than a
    // single click that four allpasses behind it have to smear afterwards.
    for (int k = 0; k < kReverbDiffusers; ++k)
      input = reverb_allpass(buf, &r->diffuser[k], input, diffusion);
    const float diffused = input;

    // Read every line, damp it, and take its four output taps on the way past.
    float v[kReverbLines];
    float wl = 0.f;
    float wr = 0.f;
    for (int n = 0; n < kReverbLines; ++n) {
      ReverbLine &ln = r->line[n];
      float *const line = buf + ln.at;

      int read = ln.pos - ln.delay;
      if (read < 0)
        read += ln.cap;
      const float x = line[read];

      // The taps alternate sign along k, and the whole line's contribution
      // flips sign with the line index — so the 32 tap gains collapse to one
      // signed sum per line and two multiplies, rather than 64 of them. Spelled
      // out rather than looped because the alternation is what is being folded,
      // and a loop would hide that this depends on `kReverbTaps` being 4.
      int t0 = ln.pos - ln.tap[0];
      int t1 = ln.pos - ln.tap[1];
      int t2 = ln.pos - ln.tap[2];
      if (t0 < 0)
        t0 += ln.cap;
      if (t1 < 0)
        t1 += ln.cap;
      if (t2 < 0)
        t2 += ln.cap;
      const float s = line[t0] - line[t1] + line[t2] - x;
      // Even lines lean left, odd lines lean right, and the two channels take
      // opposite signs: that is what makes the wet pair uncorrelated before the
      // decorrelation allpasses ever run.
      if ((n & 1) == 0) {
        wl += 1.5f * s;
        wr -= 0.5f * s;
      } else {
        wl -= 0.5f * s;
        wr += 1.5f * s;
      }

      // Bass shelf then treble shelf, in the loop. Both states are flushed:
      // a shelf fed exactly zero decays through the subnormals on its own, and
      // the whole point of the flush is that nothing in a feedback path is
      // allowed to spend thousands of samples down there.
      const float y_lo = lo_b0 * x + lo_b1 * ln.lo_x + lo_p * ln.lo_y;
      ln.lo_x = x;
      ln.lo_y = flush_denorm(y_lo);
      const float y_hi = hi_b0 * y_lo + hi_b1 * ln.hi_x + hi_p * ln.hi_y;
      ln.hi_x = y_lo;
      ln.hi_y = flush_denorm(y_hi);

      v[n] = y_hi * ln.gain;
    }

    // The Hadamard butterfly: three passes of add-and-subtract, then one scale.
    // Exactly orthogonal, and exactly orthogonal *in float* — see the note on
    // kReverbHadamardNorm.
    for (int span = 1; span < kReverbLines; span <<= 1) {
      for (int base = 0; base < kReverbLines; base += span << 1) {
        for (int k = base; k < base + span; ++k) {
          const float a = v[k];
          const float b = v[k + span];
          v[k] = a + b;
          v[k + span] = a - b;
        }
      }
    }

    for (int n = 0; n < kReverbLines; ++n) {
      ReverbLine &ln = r->line[n];
      float *const line = buf + ln.at;
      line[ln.pos] = flush_denorm(v[n] * kReverbHadamardNorm + diffused);
      if (++ln.pos >= ln.cap)
        ln.pos = 0;
    }

    wl = reverb_allpass(buf, &r->decorr[0], wl * kReverbTapScale, 0.5f);
    wr = reverb_allpass(buf, &r->decorr[1], wr * kReverbTapScale, 0.5f);

    left[i] = wl * wet1 + wr * wet2 + in_l * dry;
    right[i] = wr * wet1 + wl * wet2 + in_r * dry;
  }
}

}  // namespace fx
}  // namespace ntrk
