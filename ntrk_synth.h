// ntrk_synth -- the SYNTH instrument: four analog drum voices and a TB-303.
//
// **Header-only, and every function `inline`, for a reason rather than by
// habit.** All of this runs *per sample*, from inside `channel_sample`'s mixing
// loop, so a call here is not amortised over anything — which is the same line
// the effect modules are split along, in the other direction. Block-rate code
// (the delay, the reverb, the mixer) lives in a translation unit; per-sample
// code lives in a header so it can inline. See README.md.
//
// **An ordinary header, but not an independent module.** The voices read a
// `Channel` and an `Instrument` from `ntrk_types.h` and are named by the
// player's own `channel_trigger`/`channel_sample`, so in practice `ntrk.h` is
// the only thing that wants them. It is a separate file because the kit and the
// bass are half again as long as the replayer they hang off, not because they
// are separable from it.
//
// It compiles on its own, and that is checked rather than assumed. This file
// used to be spliced into the middle of `ntrk.h` — which closed its namespace,
// included this, and reopened it, because the structs were declared halfway
// down. That made it a fragment valid at exactly one insertion point, and
// nothing said so: it built green everywhere it was actually used.
//
// Same terms as the rest of the replayer: no allocation, no I/O, and no libm at
// all — the decays are iterated multiplies and the curves are polynomials,
// because the project pins hashes over rendered audio and a transcendental need
// not agree between Apple's libm and emscripten's.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_SYNTH_H_
#define NTRK_SYNTH_H_

#include "ntrk_fx_shape.h"
#include "ntrk_types.h"

#include <stdint.h>

namespace ntrk {

// ---- The SYNP block, id 0x0003 ---------------------------------------------
//
// Per-instrument synth parameters: **one fixed 16-byte record per SYNTH
// instrument, in ascending instrument order.** Nothing names an instrument
// index -- the nth record belongs to the nth instrument whose `type` is SYNTH,
// so the block's length and `instrument_count` cannot disagree without the
// loader noticing, and there is no index a hostile file can point somewhere
// else.
//
// | Off | W | Field | Notes |
// |-----|---|-------|-------|
// | 0 | 1 | `voice` | 0 kick, 1 snare, 2 hihat, 3 clap, 4 bass; above refused |
// | 1 | 1 | `tune` | body frequency, 0.5x..2x the voice's own, at note 25 |
// | 2 | 1 | `decay` | the VCA, over the voice's own range; the bass: the VCF |
// | 3 | 1 | `sweep` | pitch sweep depth, 0..4x over the first 20 ms |
// | 4 | 1 | `tone` | noise band cutoff, 0.5x..2x the voice's own |
// | 5 | 1 | `noise` | noise against body, 0 body only .. 255 noise only |
// | 6 | 1 | `noise_decay` | noise VCA, the voice's own range |
// | 7 | 1 | `drive` | saturation, 0 is an exact bypass |
// | 8 | 1 | `cutoff` | bass: the ladder's own cutoff, cubic over 30..8000 Hz |
// | 9 | 1 | `reso` | bass: quadratic onto the 64 measured ladder settings |
// | 10 | 1 | `env_mod` | bass: how far the envelope opens the cutoff, 0..16x |
// | 11 | 1 | `accent` | bass: level and env-mod boost together, as a 303's is |
// | 12 | 1 | `dist` | bass: shaper kind, 0 warm 1 crunch 2 tape 3 fold |
// | 13 | 1 | `dist_mix` | bass: dry against shaped; `drive` 0 is still a bypass |
// | 14 | 1 | `wave` | bass: 0 saw, 1 square -- what a 303 has, and no more |
// | 15 | 1 | reserved | must be zero, so the byte stays free to mean something |
//
// **Bytes 8..14 are read only by the bass voice**, the way `noise` is read only
// by a voice with a body: the record is one shape for every voice and the voice
// picks which of it means anything. Their *ranges* are checked whatever the
// voice, because a range is a property of the record and not of the reader --
// a drum record carrying a shaper kind of 9 is a file that has gone wrong
// somewhere, and finding out later is worse than finding out here.
//
// **Written critical.** A reader that skipped it would play a SYNTH instrument
// with no parameters at all, which is silence rather than a degraded tune --
// and the flag exists precisely to say which of those a block is.
//
// `bytes` must equal `16 * (number of SYNTH instruments)` exactly, in both
// directions: a file naming synths with no records is as malformed as one
// carrying records for instruments that are not synths.
const uint16_t kBlockSynp = 0x0003;
const int kSynthParamBytes = 16;

// Which analog voice a SYNTH instrument is. **Structure rather than settings is
// what separates them**: a kick and a hihat handed the same bytes must not
// sound alike, so the voice picks the signal path and the bytes only tune it.
// Kick and snare have a resonant body; hihat and clap are noise alone, and a
// clap is three fast re-strikes and a tail. The bass is none of those -- it is
// an oscillator through a resonant ladder, which is a different program and not
// a differently tuned drum.
//
// **Scoped, and `Instrument::synth_voice` stays a `uint8_t`** for the same
// reason `Instrument::type` does: it is byte 0 of a SYNP record, and the value
// the file supplies is only one of these five once the loader's
// `>= kSynthVoiceCount` test has passed it.
enum class SynthVoice : uint8_t {
  kKick = 0,
  kSnare = 1,
  kHihat = 2,
  kClap = 3,
  kBass = 4,       // the 303
};
const int kSynthVoiceCount = 5;

// The drums, which are the voices `kSynthVoiceSpecs` has a row for. Separate
// from `kSynthVoiceCount` because the bass has no spec: every field in one is
// about a body and a noise band, and a fifth row of zeroes nothing reads would
// be an invitation to read it.
const int kSynthDrumCount = 4;

// The shapers the bass may name, which is `fx::ShapeKind` and nothing else.
// Spelled again here so the loader's bound is a format constant rather than a
// reach into the effects header for a number that is not part of the file.
const int kSynthDistKinds = 4;

// Saw or square, and that is the whole list: it is what a 303 has.
const int kSynthWaveCount = 2;

// ----------------------------------------------------------------------------
// -- Synth instruments: the analog drum voices
// ----------------------------------------------------------------------------
//
// **A drum body is a ringing resonator, not an oscillator.** An 808's kick is a
// bridged-T network that the trigger pulse *pings*; its pitch, its decay and its
// downward sweep all fall out of the network rather than out of a waveform. So
// the body here is the SVF from `ntrk_fx_shape.h` at full resonance, hit with a
// one-sample impulse — which is the historically correct model and also the
// cheapest available: no sine table, no phase accumulator, and no new
// approximation to keep identical between ARM64 and wasm. The filter was
// already in the build; the drum costs an impulse and an envelope.
//
// **The impulse is scaled by `1 / a2`.** The bandpass gain of a unit impulse is
// exactly `a2`, which falls with cutoff, so a fixed impulse would make a low
// kick a fraction of the level of a high one and the pitch sweep would be a
// volume sweep as well. Normalising there means every ping starts at 1.0.
//
// Everything a voice needs beyond the filters and three envelopes is derived
// from the `Instrument` per sample rather than stored, because `channel_sample`
// already holds one. The exceptions are the three decay coefficients and the
// sample rate itself, which the mixer never passes — those are computed at the
// trigger, where a rate is in hand.

// A synth instrument's `tune` is defined at note 25 -- C-3, two octaves under
// note 1 -- so a pattern can play a drum an octave down the way it plays a
// sample an octave down. Vibrato and portamento move `period` and come along
// for free.
//
// **The number is the period, not the note**, and it is the one an octave error
// hides in: every voice's frequency is a ratio against it, so getting it wrong
// puts the whole kit out by an octave and only shows up as a hihat clamped
// against Nyquist at 22 kHz.
//
// **Derived from the anchor rather than written down.** It was the literal 214,
// which was note 25's period when note 1 was ProTracker's 856. Moving the
// anchor to concert pitch left it behind, and every drum in the kit rang 17.64
// cents sharp of the Hz its own spec names -- silently, because a ratio against
// a wrong reference is still a ratio and nothing sounded broken. `synth_hz`
// below is the check that would have caught it, and now does.
const double kSynthRefPeriod = kPeriodC1 * 0.25;   // note 25, two octaves down

// How long the pitch sweep takes, whatever its depth. Fixed rather than a ninth
// parameter: an 808 kick's sweep is over in a few tens of milliseconds and the
// knob players actually reach for is the depth.
const float kSynthSweepSeconds = 0.020f;

// Roughly a clap's slap interval. Three re-strikes at 11 ms is the sound; the
// spacing is not a parameter because a clap with adjustable spacing stops being
// a clap and becomes a delay.
const float kSynthClapBurstSeconds = 0.011f;

// What separates one voice from the next, at the level the parameters cannot
// reach. `body_hz` of zero means the voice has no resonator at all — a hihat is
// filtered noise and nothing else — and for those the noise/body mix is ignored
// rather than obeyed, or a hihat with `noise` at zero would be silence.
struct SynthVoiceSpec {
  float body_hz;            // 0 for a voice that is noise alone
  float noise_hz;
  float noise_q;
  float body_tau_min;       // seconds, at decay 0
  float body_tau_max;       // seconds, at decay 255
  float noise_tau_min;
  float noise_tau_max;
  uint8_t noise_highpass;   // else a bandpass
  uint8_t bursts;           // re-strikes after the first, the clap's three
};

const SynthVoiceSpec kSynthVoiceSpecs[kSynthDrumCount] = {
    //  body   noise      Q       body tau           noise tau      hp  burst
    {  55.f, 3000.f, 0.20f, 0.020f, 0.350f, 0.001f, 0.030f, 1u, 0u },  // kick
    { 190.f, 1800.f, 0.30f, 0.015f, 0.250f, 0.020f, 0.300f, 0u, 0u },  // snare
    {   0.f, 7000.f, 0.20f, 0.000f, 0.000f, 0.003f, 0.300f, 1u, 0u },  // hihat
    {   0.f, 1100.f, 0.50f, 0.000f, 0.000f, 0.030f, 0.400f, 0u, 3u },  // clap
};

// A one-pole decay coefficient for a time constant in seconds. **Exponential by
// iteration rather than by `exp`**: this header has no libm, and a repeated
// multiply is exactly an exponential. The ceiling keeps the pole strictly
// inside the unit circle, so no setting can produce a decay that does not.
inline float
synth_decay_coef(float tau_seconds, float rate) {
  const float n = tau_seconds * rate;
  if (!(n > 1.f))                     // false for NaN too, which a divide must
    return 0.f;                       // never see
  const float c = 1.f - 1.f / n;
  return c > 0.999999f ? 0.999999f : c;
}

// One step of an amplitude envelope, snapped to zero at -100 dB.
//
// **A snap rather than `flush_denorm`, and the difference is what it is for.**
// Flushing a denormal is about cost; this is about *ending*: a multiplicative
// decay never reaches zero, and a voice that never ends is one the mixer
// carries for the rest of the tune. Waiting for the denormal range would mean
// forty-six time constants of inaudible tail. One part in 100,000 is three
// orders of magnitude below a 16-bit sample's last bit, so nothing that could
// be heard is being thrown away — and it is still a compare-and-select at the
// store, never an add, for the reason `flush_denorm` is.
inline float
synth_env_step(float env, float coef) {
  const float v = env * coef;
  return v < 1.0e-5f ? 0.f : v;
}

inline float
synth_param_scale(uint8_t v) {
  return 0.5f + (float) v * (1.5f / 255.f);   // 0.5x .. 2x
}

inline float
synth_param_tau(uint8_t v, float lo, float hi) {
  return lo + (hi - lo) * ((float) v * (1.f / 255.f));
}

// The channel's own generator, the one the random vibrato waveform uses. Per
// channel rather than shared so two players rendering the same module produce
// the same noise; a global would couple the channels to each other's call
// order and make the render depend on how it was chunked.
//
// Bits 16..31, recentred by subtraction: converting a `uint16_t` above 32767 to
// `int16_t` is implementation-defined before C++20, and a value that varies by
// compiler is exactly what the determinism rule forbids.
inline float
synth_noise_sample(Channel *ch) {
  ch->rng = ch->rng * 1664525u + 1013904223u;
  const int32_t n = (int32_t) ((ch->rng >> 16) & 0xffffu) - 32768;
  return (float) n * (1.f / 32768.f);
}

// ----------------------------------------------------------------------------
// -- The bass voice: a TB-303
// ----------------------------------------------------------------------------
//
// An oscillator into a four-pole ladder into a saturator, with two envelopes:
// one opening the filter and a different one setting the level. That is the
// whole machine, and it is the *two* that matters -- see below.
//
// - **The oscillator is the wavetable path**, the same 32-frame cycle and the
//   same `pos`/`step` walk a WAVE_BUILTIN instrument takes -- so vibrato, tone
//   portamento and arpeggio reach the bass line for nothing, which for a 303 is
//   most of what playing one *is*. Saw and square only: that is what the
//   instrument has, and a triangle option would be a different synth wearing
//   its name.
// - **The filter is `fx::Ladder303`**, which is a1k0n's measured x0xb0x poles
//   realised as two ZDF sections with a one-pole highpass in front. It is not
//   the SVF the drums use, and swapping the SVF for it is the single largest
//   thing that makes this voice sound like the instrument: a 303 is four
//   ladder poles and a highpass in the feedback path, and a two-pole
//   state-variable filter is not a near miss for that, it is a different
//   filter. See `ntrk_303.h`.
// - **The distortion is `fx::shape_sample`**, the four shapers, with `drive` 0
//   an exact bypass and a dry/wet after it.
//
// **Two envelopes, because a 303 has two.** The VCF envelope is what `decay`
// sets and what `env_mod` scales; the VCA envelope is fixed hardware, a fast
// transient into a slower body. Driving both from one envelope locks the
// filter movement to the amplitude, and the whole character of an acid line is
// that they are *not* locked -- a long filter sweep under a note that has
// already dropped to a third of its peak is the sound. `decay` therefore no
// longer decides how long a note lasts; the VCA does, and the sequencer's next
// note does.
//
// **The coefficients are recomputed once every sixteen samples and glided in
// between.** The pole maths costs three exponentials, two cosines and two
// square roots; the per-sample filter costs a dozen adds. The boundary is
// `synth_age`, which is zeroed at the trigger, so the same note renders the
// same samples whatever chunk size the host asks for -- tying it to a
// free-running counter would make the render depend on how it was buffered,
// which is what this project's pinned fingerprints exist to catch.
//
// **No oversampling anywhere.** The saw is a 32-point table read at the note's
// rate and already aliases the way every other wavetable instrument in this
// format does; oversampling the filter and the shaper would fix the smaller
// half of that while doubling the cost of the voice.
//
// ponytail: three things from the reference are deliberately not here. The
// in-loop saturator (drive into the resonant state, so overdrive interacts
// with resonance) would need a byte of its own, since `drive` already names
// the post-filter shaper. The onset Q-dip is anti-ring for a retrigger, and a
// struck note resets the ladder outright instead. The accent capacitor that
// makes consecutive accents pump is per-note history; accent here is what the
// row wrote and nothing else. Each is additive if the voice wants it.

// The VCF envelope's range of time constants -- what `decay` picks from. This
// is the filter's decay, not the note's length.
const float kSynth303TauMin = 0.040f;
const float kSynth303TauMax = 1.200f;

// The VCA, which is not a knob on the instrument and is not one here. Two
// stages: a fast drop to the knee, which is the pluck, then a much slower body
// under it. One coefficient would give either a note that stops dead or one
// with no attack; the knee is what makes it a bass line.
const float kSynth303VcaTauFast = 0.02211f;
const float kSynth303VcaTauSlow = 0.2171f;
const float kSynth303VcaKnee = 0.65f;

// An accented note's filter envelope is forced toward a fixed, short decay --
// the accent circuit does not scale the decay, it replaces it. `accent` at
// full moves the VCF decay all the way here whatever `decay` says.
const float kSynth303AccentTau = 0.200f;

// The accent circuit's other half: the filter envelope, lowpassed by an RC and
// added to the VCA. This is what gives an accent a shape of its own rather
// than making it simply louder -- the level follows the filter for a moment,
// so an accented note punches where its filter is opening.
const float kSynth303AccentRcTau = 0.00155f;
const float kSynth303AccentVca = 0.3f;    // how much of it reaches the VCA
const float kSynth303AccentLevel = 0.5f;  // and the flat level boost beside it
const float kSynth303AccentMod = 1.5f;    // accent's lift on the env-mod depth

// Cutoff in Hz for a 0..255 byte, cubic so the knob spends most of its travel
// where a 303's does -- under a kilohertz, where the resonance is the sound.
// A linear map would put nine tenths of the range above the point the filter
// stops being interesting.
inline float
synth303_cutoff_hz(uint8_t v) {
  const float t = (float) v * (1.f / 255.f);
  return 30.f + 7970.f * t * t * t;
}

// How far the envelope may open the cutoff, at env mod 255 before accent.
// Sixteen octaves is not the claim -- `ladder303_set` caps at 0.45 of the rate
// long before this does, and that cap is what a fully open filter means.
const float kSynth303EnvModMax = 15.f;

// The `reso` byte, mapped onto the 64 rows of the pole table. Quadratic, the
// same law the measurement was taken with, so the knob spends its travel where
// the resonance is doing something; 255 lands on the last row, which is where
// the real ladder stops moving and starts self-oscillating.
inline float
synth303_reso(uint8_t v) {
  const float t = (float) v * (1.f / 255.f);
  return 2.52f * t * t;
}

inline void
synth303_trigger(Channel *ch, const Instrument &ins, float rate) {
  fx::ladder303_reset(&ch->synth_ladder);

  // The drums' state, put back rather than left where it was. A row that
  // changes the instrument without playing a note hands this channel's next
  // sample to another voice's code, and a stale noise envelope is then a hiss
  // under a bass line that nothing in the file asked for.
  fx::svf_reset(&ch->synth_body);
  fx::svf_reset(&ch->synth_band);
  ch->synth_nenv = 0.f;
  ch->synth_nenv_coef = 0.f;
  ch->synth_pitch = 0.f;
  ch->synth_pitch_coef = 0.f;
  ch->synth_burst = 0u;

  ch->synth_rate = rate;
  ch->synth_age = 0u;
  ch->synth_env = 1.f;
  ch->synth_venv = 1.f;
  ch->synth_acc_vca = 0.f;
  (void) ins;

  // **9xx has already written a sample offset into `pos`.** On a 32-frame cycle
  // that is an arbitrary starting phase rather than an offset into anything, so
  // it is dropped here -- a bass note whose waveform starts somewhere different
  // depending on an effect nobody aimed at it is not a feature.
  ch->pos = 0.0;
}

// Everything the voice needs that is a function of the instrument and the
// trigger rather than of the sample. It runs on the coefficient boundary, not
// at the trigger, and that is deliberate: **a slid note never reaches the
// trigger.** A slide ties the previous note and only glides its pitch, so a
// coefficient computed once at the strike would leave the tied note's accent,
// decay and envelope shape belonging to the note before it.
inline void
synth303_recoef(Channel *ch, const Instrument &ins, float accent) {
  const float rate = ch->synth_rate;
  float tau = synth_param_tau(ins.synth_decay, kSynth303TauMin, kSynth303TauMax);
  tau += (kSynth303AccentTau - tau) * accent;
  ch->synth_env_coef = synth_decay_coef(tau, rate);
  ch->synth_venv_fast = synth_decay_coef(kSynth303VcaTauFast, rate);
  ch->synth_venv_slow = synth_decay_coef(kSynth303VcaTauSlow, rate);
  // A one-pole's per-sample coefficient as `dt / (tau + dt)`, which needs no
  // exponential and is what the reference's RC network uses.
  const float n = kSynth303AccentRcTau * rate;
  ch->synth_acc_rc = n > 0.f ? 1.f / (n + 1.f) : 1.f;
}

inline float
synth303_sample(Channel *ch, const Instrument &ins) {
  // Saw is built-in shape 1 and square is shape 0; the record's 0/1 is the
  // player's order, not the table's, because a saw is what a 303 is for.
  const int8_t *table = builtin_wave(ins.synth_wave == 0u ? 1 : 0);
  const uint32_t i0 = (uint32_t) ch->pos & (uint32_t) (kBuiltinWaveFrames - 1);
  const uint32_t i1 = (i0 + 1u) & (uint32_t) (kBuiltinWaveFrames - 1);
  const double frac = ch->pos - (double) (uint32_t) ch->pos;
  const float a = (float) table[i0];
  const float b = (float) table[i1];
  const float osc =
      (float) ((double) a + ((double) b - (double) a) * frac) * (1.f / 128.f);

  // Accent is a 303's one dynamic: it lifts the level and opens the filter
  // further in the same gesture, which is why it is one knob and not two.
  //
  // **Two things are called accent, and they are a product rather than
  // alternatives.** `ins.synth_accent` is the SYNP record's byte: the
  // instrument's accent *depth*, how much an accent does to this voice.
  // `ch->accent` is the per-note *trigger* the row wrote with FXPL 0x30:
  // whether the note that was struck is accented, and how hard. So a patch
  // with no depth is never accented however the pattern is written, and a
  // pattern that accents nothing plays a deep patch flat -- which is what a
  // 303's accent switch and its accent knob do.
  const float accent =
      (float) ins.synth_accent * (1.f / 255.f) * ch->accent;

  // The amortised coefficient update. The ladder walks toward what this
  // computes over the next sixteen samples rather than stepping to it, so a
  // sweep is a sweep and not a stair; see `ntrk_303.h`.
  if ((ch->synth_age & (fx::kLadder303Stride - 1u)) == 0u) {
    synth303_recoef(ch, ins, accent);
    const float mod = (float) ins.synth_env_mod * (1.f / 255.f) *
                      kSynth303EnvModMax * (1.f + kSynth303AccentMod * accent);
    fx::ladder303_set(&ch->synth_ladder,
                      synth303_cutoff_hz(ins.synth_cutoff) *
                          (1.f + mod * ch->synth_env),
                      synth303_reso(ins.synth_reso), ch->synth_rate);
  }
  float s = fx::ladder303_process(&ch->synth_ladder, osc);

  // Exactly a passthrough at drive 0 whatever the mix says, which is what
  // `shape_sample` guarantees -- so the four kinds collapse to one signal there
  // and the mix knob cannot introduce a difference of its own.
  // `synth_dist` is refused above `kSynthDistKinds` at load and at save, so the
  // byte is already one of the four by the time it gets here.
  const float wet = fx::shape_sample((fx::ShapeKind) ins.synth_dist, s,
                                     (float) ins.synth_drive * (1.f / 255.f));
  s += (wet - s) * ((float) ins.synth_dist_mix * (1.f / 255.f));

  // The accent circuit's shape: the filter envelope through an RC, added to
  // the VCA. Run unconditionally and scaled by `accent` at the use site rather
  // than gated on it, so there is no discontinuity between an accent of 0.01
  // and one of 0 -- and the term is exactly nothing when the row wrote nothing.
  ch->synth_acc_vca += (ch->synth_env - ch->synth_acc_vca) * ch->synth_acc_rc;
  const float level =
      (ch->synth_venv + kSynth303AccentVca * accent * ch->synth_acc_vca) *
      (1.f + kSynth303AccentLevel * accent);
  s = s * level;

  // **The output stage is the house saturator, and it is load-bearing.** A
  // ladder at maximum resonance has a peak gain in the tens; a saw's harmonic
  // sitting on that peak, times an accent, leaves the voice well outside the
  // range the mixer's scaling assumes. Bounding it here is both what the
  // hardware does after its VCA and the reason "finite across the whole
  // parameter grid" is a property rather than a hope.
  s = fx::soft_clip(s);

  ch->pos += ch->step;
  // A while rather than an if, for the reason the sample path's is: a high note
  // at a low rate can step past a 32-frame cycle more than once in a frame.
  while (ch->pos >= (double) kBuiltinWaveFrames)
    ch->pos -= (double) kBuiltinWaveFrames;

  // The -100 dB snap, not a denormal flush: this is the only thing that ends
  // the voice, and a multiplicative decay never reaches zero on its own. It is
  // also stricter than `flush_denorm` and subsumes it -- the state cannot reach
  // denormal range without passing through the snap first.
  //
  // **The VCA ends the voice and the VCF does not.** A filter envelope that has
  // run out leaves the cutoff at its base, which is a duller note; a level that
  // has run out is silence, and only silence should stop the channel being
  // mixed.
  ch->synth_env = synth_env_step(ch->synth_env, ch->synth_env_coef);
  ch->synth_venv = synth_env_step(
      ch->synth_venv,
      ch->synth_venv > kSynth303VcaKnee ? ch->synth_venv_fast
                                        : ch->synth_venv_slow);
  ++ch->synth_age;
  if (ch->synth_venv == 0.f)
    ch->playing = false;

  return s * 100.f;
}

inline void
synth_trigger(Channel *ch, const Instrument &ins, double sample_rate) {
  const float rate = (float) (sample_rate > 1.0 ? sample_rate : 48000.0);
  if (ins.synth_voice == (uint8_t) SynthVoice::kBass) {
    synth303_trigger(ch, ins, rate);
    return;
  }
  const int vi = (int) ins.synth_voice < kSynthDrumCount
                     ? (int) ins.synth_voice
                     : 0;    // the loader refuses the rest; an editor may not
  const SynthVoiceSpec &v = kSynthVoiceSpecs[vi];

  // A new note gets clean filters, or the previous hit rings into this one's
  // attack — the same reason the mixer resets a channel's instrument filter.
  fx::svf_reset(&ch->synth_body);
  fx::svf_reset(&ch->synth_band);

  ch->synth_rate = rate;
  ch->synth_age = 0u;
  // Bounded before the conversion: a float too large for a `uint32_t` converts
  // to nothing defined, and the rate is whatever the caller passed.
  const float burst = rate * kSynthClapBurstSeconds;
  ch->synth_burst = (burst > 0.f && burst < 1.0e9f) ? (uint32_t) burst : 0u;
  ch->synth_env = v.body_hz > 0.f ? 1.f : 0.f;
  ch->synth_nenv = 1.f;
  ch->synth_pitch = 1.f;
  ch->synth_env_coef = synth_decay_coef(
      synth_param_tau(ins.synth_decay, v.body_tau_min, v.body_tau_max), rate);
  ch->synth_nenv_coef = synth_decay_coef(
      synth_param_tau(ins.synth_noise_decay, v.noise_tau_min, v.noise_tau_max),
      rate);
  ch->synth_pitch_coef = synth_decay_coef(kSynthSweepSeconds, rate);
}

// One frame of a drum, on the same 8-bit scale `instrument_frame` returns —
// so the volume, tremolo and envelope tail below is literally the sample path's,
// and a synth instrument gets Cxx, tremolo and an ADSR for nothing.
inline float
synth_sample(Channel *ch, const Instrument &ins) {
  if (ins.synth_voice == (uint8_t) SynthVoice::kBass)
    return synth303_sample(ch, ins);
  const int vi = (int) ins.synth_voice < kSynthDrumCount
                     ? (int) ins.synth_voice
                     : 0;
  const SynthVoiceSpec &v = kSynthVoiceSpecs[vi];
  const float rate = ch->synth_rate;

  // The note, as a multiplier on every frequency in the voice. `period` is what
  // vibrato and portamento move, so reading it here rather than a value cached
  // at the trigger is what makes those effects work on a drum.
  const float track =
      ch->period > 0.0 ? (float) (kSynthRefPeriod / ch->period) : 1.f;

  // The clap's re-strikes, before anything is read: three slaps and a tail is
  // the whole of what makes a clap one. They run four times faster than the
  // tail — `c^4`, which is two multiplies and no second parameter.
  const bool bursting =
      v.bursts > 0u && ch->synth_burst > 0u &&
      ch->synth_age < (uint32_t) v.bursts * ch->synth_burst;
  if (v.bursts > 0u && ch->synth_burst > 0u) {
    for (uint32_t b = 1u; b <= (uint32_t) v.bursts; ++b)
      if (ch->synth_age == b * ch->synth_burst)
        ch->synth_nenv = 1.f;
  }

  float body = 0.f;
  if (v.body_hz > 0.f && ch->synth_env > 0.f) {
    const float sweep =
        1.f + (float) ins.synth_sweep * (4.f / 255.f) * ch->synth_pitch;
    fx::svf_set(&ch->synth_body,
                v.body_hz * synth_param_scale(ins.synth_tune) * track * sweep,
                1.f, rate);
    float x = 0.f;
    if (ch->synth_age == 0u) {
      // Never a divide by whatever `a2` happens to be: at an absurd rate or a
      // cutoff pinned against the floor it is small, and a voice that returns
      // an infinity poisons the whole mix rather than one channel.
      const float a2 = ch->synth_body.a2 > 1.0e-9f ? ch->synth_body.a2 : 1.0e-9f;
      x = 1.f / a2;
    }
    body = fx::svf_bandpass(&ch->synth_body, x) * ch->synth_env;
  }

  float noise = 0.f;
  if (ch->synth_nenv > 0.f) {
    fx::svf_set(&ch->synth_band,
                v.noise_hz * synth_param_scale(ins.synth_tone) * track,
                v.noise_q, rate);
    const float n = synth_noise_sample(ch);
    noise = (v.noise_highpass != 0u ? fx::svf_highpass(&ch->synth_band, n)
                                    : fx::svf_bandpass(&ch->synth_band, n)) *
            ch->synth_nenv;
  }

  const float mix =
      v.body_hz > 0.f ? (float) ins.synth_noise * (1.f / 255.f) : 1.f;
  float s = body * (1.f - mix) + noise * mix;
  // Exactly a passthrough at drive 0, which is what `shape_sample` guarantees —
  // so an undriven drum is the filter's own output and nothing else.
  s = fx::shape_sample(fx::ShapeKind::kWarm, s,
                       (float) ins.synth_drive * (1.f / 255.f));

  float nc = ch->synth_nenv_coef;
  if (bursting) {
    nc = nc * nc;
    nc = nc * nc;
  }
  ch->synth_env = synth_env_step(ch->synth_env, ch->synth_env_coef);
  ch->synth_nenv = synth_env_step(ch->synth_nenv, nc);
  // The sweep only has to stop being audible, not stop existing, so this is the
  // ordinary denormal flush rather than the snap the envelopes take.
  ch->synth_pitch = fx::flush_denorm(ch->synth_pitch * ch->synth_pitch_coef);
  ++ch->synth_age;

  // **Both envelopes at exactly zero is the end of the voice**, and the snap in
  // `synth_env_step` is what makes "exactly" mean it: the channel stops being
  // mixed rather than going on adding an inaudible tail for the rest of the
  // tune. A clap is not finished until its last slap has been struck, however
  // far its envelope has fallen between two of them.
  if (ch->synth_env == 0.f && ch->synth_nenv == 0.f &&
      ch->synth_age > (uint32_t) v.bursts * ch->synth_burst)
    ch->playing = false;

  return s * 100.f;
}

}  // namespace ntrk

#endif  // NTRK_SYNTH_H_
