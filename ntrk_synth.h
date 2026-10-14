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
// | 2 | 1 | `decay` | the VCA, over the voice's own range; the bass: the VCF, 30 ms..2 s exponential |
// | 3 | 1 | `sweep` | pitch sweep depth, 0..4x over the first 20 ms |
// | 4 | 1 | `tone` | noise band cutoff, 0.5x..2x the voice's own |
// | 5 | 1 | `noise` | noise against body, 0 body only .. 255 noise only |
// | 6 | 1 | `noise_decay` | noise VCA, the voice's own range |
// | 7 | 1 | `drive` | saturation, 0 is an exact bypass |
// | 8 | 1 | `cutoff` | bass: the ladder's cutoff, exponential over 314..2394 Hz |
// | 9 | 1 | `reso` | bass: a fitted taper onto the measured ladder settings |
// | 10 | 1 | `env_mod` | bass: how many octaves the envelope sweeps, measured law |
// | 11 | 1 | `accent` | bass: accent depth -- level, a shorter sweep and more of it |
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

// The bass voice's transpose, so a fresh bass sounds the note its cell is NAMED: C-2 is C2
// (MIDI 36), which is where a 303 line lives. Note 1 is MIDI 48 on the 32-frame cycle this
// voice steps (`ntrk_types.h`), so at transpose 0 a "C-1" bass sounded C3 and the lowest cell
// anyone could enter was the top of the bass register. The way below note 1 is the
// transpose -- see `note_transposed` -- and nothing set one. A default rather than a change
// to the voice's pitch law: a saved bass keeps the transpose it was saved with and sounds as
// it was written, and the pinned fingerprints are about the law, not about the default.
const int8_t kSynthBassTranspose = -24;

// Where a voice's own bytes start: audible, and each voice's character rather than a knob at
// zero. **One writer**: an editor that switches an instrument to a voice seeds it here, and
// the demo tune (`ntrk_gen.cc`) builds its voices from it -- so a fresh kick and the demo's
// kick are the same drum. Every synth byte is set, the ones this voice does not read to zero,
// so nothing of the previous voice is left reading as this one's.
inline void
synth_voice_defaults(Instrument *ins, SynthVoice voice) {
  if (ins == nullptr)
    return;
  ins->synth_voice = (uint8_t) voice;
  ins->synth_tune = 0; ins->synth_decay = 0; ins->synth_sweep = 0; ins->synth_tone = 0;
  ins->synth_noise = 0; ins->synth_noise_decay = 0; ins->synth_drive = 0;
  ins->synth_cutoff = 0; ins->synth_reso = 0; ins->synth_env_mod = 0; ins->synth_accent = 0;
  ins->synth_dist = 0; ins->synth_dist_mix = 0; ins->synth_wave = 0;
  // Every voice sets it, so a bass switched to a kick does not keep playing two octaves down.
  ins->transpose = voice == SynthVoice::kBass ? kSynthBassTranspose : 0;
  switch (voice) {
    case SynthVoice::kKick:
      ins->synth_tune = 110; ins->synth_decay = 170; ins->synth_sweep = 120;
      ins->synth_tone = 100; ins->synth_noise = 25; ins->synth_noise_decay = 25;
      ins->synth_drive = 70;
      break;
    case SynthVoice::kSnare:
      ins->synth_tune = 128; ins->synth_decay = 90; ins->synth_sweep = 60;
      ins->synth_tone = 140; ins->synth_noise = 150; ins->synth_noise_decay = 70;
      ins->synth_drive = 40;
      break;
    case SynthVoice::kHihat:
      ins->synth_tone = 140; ins->synth_noise_decay = 30; ins->synth_drive = 40;
      break;
    case SynthVoice::kClap:
      ins->synth_tone = 128; ins->synth_noise_decay = 50; ins->synth_drive = 40;
      break;
    case SynthVoice::kBass:
      // Enough resonance and env mod that the ladder is doing the work, and tape on the end.
      // Cutoff 60 is 507 Hz and decay 150 a 0.35 s sweep: a line that squelches as it plays.
      ins->synth_cutoff = 60; ins->synth_reso = 205; ins->synth_env_mod = 160;
      ins->synth_decay = 150; ins->synth_accent = 200; ins->synth_drive = 70;
      ins->synth_dist = 2; ins->synth_dist_mix = 110; ins->synth_wave = 0;
      break;
  }
}

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
// - **The oscillator walks the wavetable path's phase**, the same 32-frame
//   cycle and the same `pos`/`step` a WAVE_BUILTIN instrument takes, but
//   computes a PolyBLEP saw or the 303's shaped square from it rather than
//   reading the 8-bit table -- so vibrato, tone portamento and arpeggio reach
//   the bass line for nothing, which for a 303 is most of what playing one
//   *is*. Saw and square only: that is what the instrument has, and a
//   triangle option would be a different synth wearing its name.
// - **The filter is `fx::Ladder303`**, which is a1k0n's measured x0xb0x poles
//   realised as two ZDF sections behind the feedback highpass's pole and zero,
//   inside Open303's fixed filters (`fx::Fixed303`). See `ntrk_303.h`.
// - **The distortion is `fx::shape_sample`**, the four shapers, with `drive` 0
//   an exact bypass and a dry/wet after it.
//
// **Two envelopes, because a 303 has two.** The VCF envelope is what `decay`
// sets and what `env_mod` scales; the VCA is fixed hardware, one long decay
// that a struck note restarts. Driving both from one envelope locks the filter
// movement to the amplitude, and the whole character of an acid line is that
// they are *not* locked. The VCF envelope does reach the level too -- 0.45 of
// it rides on the VCA, and an accent adds four more -- which is where a
// plucked note gets its pluck and an accented one its punch. How long a note
// lasts is the gate's: the instrument's own envelope and the next note.
//
// **The envelope opens the cutoff in octaves.** The 303's filter takes its
// cutoff through an exponential converter, so an exponentially decaying
// envelope is a sweep that falls fastest at its top -- the "pew" of every
// acid line. Driving the cutoff in hertz instead, `cutoff * (1 + mod * env)`,
// falls in octaves *slowest* at the top: measured on the same decay, that
// left a note 2.7 octaves up after 400 ms, where the converter has it
// 0.6 above the knob. The envelope reaches the converter through a
// 3 ms RC, which is the attack the hardware's sweep has rather than a step,
// and how far it swings and where the knob sits in that swing is Open303's
// measured law, `synth303_env` -- under which even env mod 0 sweeps three
// quarters of an octave, because the hardware does.
//
// **Accent** is a 303's one dynamic: it shortens the filter envelope to
// 200 ms, adds the envelope through a 15 ms RC on top of the cutoff, and adds
// it four times over to the level. The RC is not cleared between notes, so a
// run of accents opens the filter further each time -- the accent sweep.
//
// **A note struck while one is sounding keeps the filter warm.** The ladder,
// the decimator, the fixed filters, the two RCs and the oscillator's phase all
// run on into it, as they do on the hardware, and only a note out of silence
// starts them cold. Resetting them under a sounding note cut its resonance and
// its waveform mid-cycle, which is a click on every legato note.
//
// **A slide glides every frame.** The player moves the pitch once a tick, and
// a 303's slide is an RC on the pitch voltage: at three ticks a slide closed
// 90% of its interval on the first one, which is a jump. So while one runs the
// oscillator walks its own step to the destination at the slide's rate,
// `synth_glide`, which the player sets from the same glide time the tick-rate
// glide is set from, at the row's own tempo -- and plays it under any vibrato
// or arpeggio on the period. See `ntrk_types.h` and `player_row`.
//
// **The coefficients are recomputed once every sixteen samples and glided in
// between.** The pole maths costs three exponentials, two cosines and two
// square roots; the per-sample filter costs a dozen adds. The boundary is
// `synth_age`, which is zeroed at the trigger, so the same note renders the
// same samples whatever chunk size the host asks for -- tying it to a
// free-running counter would make the render depend on how it was buffered,
// which is what this project's pinned fingerprints exist to catch.
//
// **Not the 32-point table, and at twice the rate.** A 32-frame cycle with
// linear interpolation carries about fifteen harmonics: a 55 Hz saw measured
// 15-20 dB short of a real one from 1.3 kHz up, which is exactly the band a
// resonant sweep lives in, so the ladder had nothing to squelch on. The PolyBLEP
// saw has every harmonic, and running oscillator, ladder, shaper and output
// clip at 2x (`fx::Decim303` brings it back) keeps its edges and the two
// nonlinearities from folding back as grit. This voice is the exception to the
// no-oversampling policy; nothing else in the player is oversampled.
//
// The envelope law, the VCA, the accent's shape, the square and the fixed
// filters are Open303's (Robin Schmidt, https://sourceforge.net/projects/open303,
// MIT), whose numbers this takes and whose code it does not.
//
// ponytail: the in-loop saturator is not here. The ladder is placed from
// measured poles rather than built from stages, so there is no loop to put
// one in; drive into resonance would need a ladder built from its circuit.

// The cutoff knob's range, the stock 303's: 314 Hz to 2394 Hz, exponentially,
// which is the range the envelope law below was measured over.
const float kSynth303CutoffHz = 314.f;
const float kSynth303CutoffOctaves = 2.930586688f;   // log2(2394 / 314)
// Where the converter may take it: Open303's bounds.
const float kSynth303CutoffFloor = 200.f;
const float kSynth303CutoffCeiling = 20000.f;

// The VCF envelope's time constant, `decay`'s: 30 ms (the Devilfish's floor)
// to 2 s (the stock top), exponentially. This is the filter's decay, not the
// note's length.
const float kSynth303DecayMin = 0.030f;
const float kSynth303DecayOctaves = 6.058894f;       // log2(2 / 0.03)

// The VCA: one decay, restarted by a struck note, and the VCF envelope riding
// on it -- 0.45 of it always, and four more at full accent.
const float kSynth303VcaTau = 1.230f;
const float kSynth303AmpEnv = 0.45f;
const float kSynth303AmpAccent = 4.f;
// The VCA's full swing, a full accent's strike: 1 + 0.45 + 4. The level is
// divided by it, so the VCA runs 0..1 and can sit *after* the output's clip.
const float kSynth303AmpFull = 5.45f;

// How hard the ladder drives the output's soft clip. **The clip sits before
// the VCA**: its job is to bound the ladder's resonant peaks, which is a
// property of the filter rather than of the note, so every note meets it at
// the same drive and an accent is then exactly as much louder as the VCA
// makes it: about 10 dB at full accent. A clip behind the VCA would compress a
// note the harder it was struck, which is the accent it would eat. The cost is
// headroom: under one ceiling an accent's ten decibels come out of the plain
// notes, which sit 4-7 dB under where a VCA that barely accented put them. More
// drive is more level for every note and more saturation, and costs the
// accents nothing.
const float kSynth303Drive = 0.763f;

// An accented note's filter envelope is moved toward a fixed, short decay --
// the accent circuit does not scale the decay, it replaces it. `accent` at
// full moves it all the way here whatever `decay` says.
const float kSynth303AccentTau = 0.200f;

// The two RCs the envelope reaches the converter through.
const float kSynth303EnvRcTau = 0.003f;
const float kSynth303AccentRcTau = 0.015f;

// Cutoff in Hz for a 0..255 byte, exponential across the stock knob's range.
inline float
synth303_cutoff_hz(uint8_t v) {
  return kSynth303CutoffHz *
         fx::ntrk303_exp2((float) v * (kSynth303CutoffOctaves / 255.f));
}

// The VCF envelope's time constant for a `decay` byte, in seconds.
inline float
synth303_decay_tau(uint8_t v) {
  return kSynth303DecayMin *
         fx::ntrk303_exp2((float) v * (kSynth303DecayOctaves / 255.f));
}

// Open303's measured env-mod law (`calculateEnvModScalerAndOffset`): how many
// octaves the envelope's full swing spans, and how far below the knob a
// decayed envelope leaves the cutoff -- both straight lines in the env mod
// knob and in where the cutoff knob sits across its range, `c`, 0 to 1.
struct Env303 {
  float scaler;   // octaves per unit of envelope
  float offset;   // the envelope value the knob's cutoff corresponds to
};

inline Env303
synth303_env(uint8_t cutoff, uint8_t env_mod) {
  // log2(cutoff / 313.815) / log2(2394 / 313.815), the law's own axis; 314 is
  // not quite its corner, which is the 0.00085.
  const float c = ((float) cutoff * (kSynth303CutoffOctaves / 255.f) + 0.00084896f) *
                  (1.f / 2.931683907f);
  const float e = (float) env_mod * (1.f / 255.f);
  const float lo = 3.773996325f * e + 0.736965594f;
  const float hi = 4.194548788f * e + 0.864344901f;
  Env303 out;
  out.scaler = (1.f - c) * lo + c * hi;
  out.offset = 0.048292931f * c + 0.294391201f;
  return out;
}

// The converter: the knob's cutoff moved by the envelope's RC in octaves, and
// by an accent's on top. Equal steps of the envelope are equal intervals here,
// which is the whole difference between a 303's sweep and a hertz ramp.
inline float
synth303_cutoff_at(float base_hz, const Env303 &e, float rc1, float accent,
                   float rc2) {
  float fc = base_hz * fx::ntrk303_exp2(e.scaler * (rc1 - e.offset) + accent * rc2);
  if (!(fc > kSynth303CutoffFloor))      // false for NaN too
    fc = kSynth303CutoffFloor;
  if (fc > kSynth303CutoffCeiling)
    fc = kSynth303CutoffCeiling;
  return fc;
}

// The `reso` byte, mapped onto the pole table's rows -- at the knob's eighths,
// linearly between. **Fitted, not a formula.** Each row is placed so the peak
// the ladder raises over its passband follows a 303's knob: most of the
// resonance in the first half of the travel and a slower climb after, least
// squares at 300 Hz, 1 kHz and 3 kHz against Open303's measured law. A
// quadratic law put under 2 dB of peak in the first half of the knob and then
// overshot the top by 7, because it ran to row 63, where the ladder
// self-oscillates; the knob stops short of that now.
const float kSynth303ResoRows[9] = {
    0.f, 17.85f, 30.15f, 39.45f, 46.25f, 51.05f, 54.60f, 57.10f, 58.85f,
};

// The table's mapped resonance, `row / 25`, which is what `ladder303_set` reads.
inline float
synth303_reso(uint8_t v) {
  const float x = (float) v * (8.f / 255.f);
  int i = (int) x;
  if (i > 7)
    i = 7;
  const float t = x - (float) i;
  return (kSynth303ResoRows[i] +
          t * (kSynth303ResoRows[i + 1] - kSynth303ResoRows[i])) * (1.f / 25.f);
}

// PolyBLEP's correction for a downward step of 2 at phase 0, `dt` the phase
// advance per sub-sample. Zero outside one sub-sample either side of the edge,
// so it never divides by a `dt` of zero.
inline float
synth303_blep(float t, float dt) {
  if (t < dt) {
    const float x = t / dt;
    return x + x - x * x - 1.f;
  }
  if (t > 1.f - dt) {
    const float x = (t - 1.f) / dt;
    return x * x + x + x + 1.f;
  }
  return 0.f;
}

// The 303's square, which is not a square. The hardware makes it from the saw
// with a one-transistor shaper, `tanh(F (2t - 1) + O)` with F = 36.9 dB and
// O = 4.37 (Open303's fillWithSquare303): one edge is the saw's own hard drop
// at the wrap, which gets the PolyBLEP, and the other is the shaper's steep
// but continuous rise a little before the half -- a duty of about 53% and one
// rounded corner, which is the hollow, nasal thing a 303's square sounds like
// and a 50% square does not. Its mean, O / F, is taken out, and it runs at
// half the saw's level, both as Open303 does.
const float kSquare303F = 69.9841996f;
const float kSquare303O = 4.37f;
const float kSquare303Mean = 0.0624427f;

// The oscillator at phase `pos` of a `kBuiltinWaveFrames` cycle: a rising saw,
// or the shaped square.
inline float
synth303_osc(double pos, double step, bool square) {
  const float t = (float) (pos * (1.0 / (double) kBuiltinWaveFrames));
  float dt = (float) (step * (1.0 / (double) kBuiltinWaveFrames));
  if (dt > 0.5f)    // a note above a quarter of the rate: the edges would overlap
    dt = 0.5f;
  if (!square)
    return 2.f * t - 1.f - synth303_blep(t, dt);
  return 0.5f * (fx::ntrk303_tanh(kSquare303F * (2.f * t - 1.f) + kSquare303O) -
                 kSquare303Mean - synth303_blep(t, dt));
}

// `legato`: the channel was sounding this voice right up to the note, so the
// note runs on from its state rather than starting it cold. See above.
inline void
synth303_trigger(Channel *ch, const Instrument &ins, float rate, bool legato) {
  (void) ins;
  if (!legato) {
    fx::ladder303_reset(&ch->synth_ladder);
    fx::decim303_reset(&ch->synth_decim);
    fx::fixed303_reset(&ch->synth_fixed);
    ch->synth_rc1 = 0.f;
    ch->synth_rc2 = 0.f;
    ch->synth_level = 0.f;
    ch->synth_level_i = 0.f;
  }
  fx::fixed303_set(&ch->synth_fixed, rate);

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
  ch->synth_venv_coef = synth_decay_coef(kSynth303VcaTau, rate);
  // A one-pole's per-sample coefficient as `dt / (tau + dt)`, which needs no
  // exponential and is what the reference's RC network uses.
  ch->synth_rc1_a = 1.f / (kSynth303EnvRcTau * rate + 1.f);
  ch->synth_rc2_a = 1.f / (kSynth303AccentRcTau * rate + 1.f);
  ch->synth_glide = 0.0;

  // **9xx has already written a sample offset into `pos`.** On a 32-frame cycle
  // that is an arbitrary starting phase rather than an offset into anything, so
  // it is dropped here -- a bass note whose waveform starts somewhere different
  // depending on an effect nobody aimed at it is not a feature. A legato note
  // takes the phase the oscillator had reached, which is what keeps its wave
  // continuous across the strike.
  ch->pos = legato ? ch->synth_pos : 0.0;
}

// Everything the voice needs that is a function of the instrument and the
// note rather than of the sample. It runs on the coefficient boundary, not at
// the trigger, and that is deliberate: **a slid note never reaches the
// trigger.** A slide ties the previous note and only glides its pitch, so a
// coefficient computed once at the strike would leave the tied note's accent
// and decay belonging to the note before it.
inline void
synth303_recoef(Channel *ch, const Instrument &ins, float accent) {
  float tau = synth303_decay_tau(ins.synth_decay);
  tau += (kSynth303AccentTau - tau) * accent;
  ch->synth_env_coef = synth_decay_coef(tau, ch->synth_rate);
}

inline float
synth303_sample(Channel *ch, const Instrument &ins) {
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

  // The envelope through its two RCs, before the converter reads them. The
  // accent's only charges while the note is accented, and otherwise lets go.
  // Flushed at the store like every other state here: once the envelope has
  // snapped to zero they decay into denormal range and would sit there for
  // the rest of the note, at half again the voice's cost.
  ch->synth_rc1 = fx::flush_denorm(
      ch->synth_rc1 + (ch->synth_env - ch->synth_rc1) * ch->synth_rc1_a);
  ch->synth_rc2 = fx::flush_denorm(
      ch->synth_rc2 +
      ((accent > 0.f ? ch->synth_env : 0.f) - ch->synth_rc2) * ch->synth_rc2_a);

  // The amortised coefficient update. The ladder walks toward what this
  // computes over the next `kLadder303Stride` of its own samples rather than
  // stepping to it, so a sweep is a sweep and not a stair; see `ntrk_303.h`.
  // The ladder runs at 2x, so that is every half-stride output frames.
  if ((ch->synth_age & (fx::kLadder303Stride / 2u - 1u)) == 0u) {
    synth303_recoef(ch, ins, accent);
    fx::ladder303_set(
        &ch->synth_ladder,
        synth303_cutoff_at(synth303_cutoff_hz(ins.synth_cutoff),
                           synth303_env(ins.synth_cutoff, ins.synth_env_mod),
                           ch->synth_rc1, accent, ch->synth_rc2),
        synth303_reso(ins.synth_reso), 2.f * ch->synth_rate);
  }

  // The level: the VCA with the filter envelope riding on it, through the
  // de-clicker, 0..1. Run unconditionally and scaled by `accent` rather than
  // gated on it, so there is no discontinuity between an accent of 0.01 and
  // one of 0 -- and the accent's term is exactly nothing when the row wrote
  // nothing. Capped at 1 because a Butterworth's step overshoots by 4%, and the
  // VCA is what keeps the clip's bound the voice's bound.
  //
  // **The de-clicker runs at the control rate**, every `kDeclick303Stride`
  // frames, and the level walks to it in between: a 200 Hz lowpass on an
  // envelope has nothing above a few hundred hertz to keep, and running it per
  // frame was a seventh of the voice. Keyed on `synth_age`, so a note renders
  // the same whatever the buffer size.
  if ((ch->synth_age & (fx::kDeclick303Stride - 1u)) == 0u) {
    const float target = fx::fixed303_declick(
        &ch->synth_fixed,
        (ch->synth_venv +
         (kSynth303AmpEnv + kSynth303AmpAccent * accent) * ch->synth_env) *
            (1.f / kSynth303AmpFull));
    ch->synth_level_i =
        (target - ch->synth_level) * (1.f / (float) fx::kDeclick303Stride);
  }
  ch->synth_level = fx::flush_denorm(ch->synth_level + ch->synth_level_i);
  float level = ch->synth_level;
  if (level > 1.f)
    level = 1.f;

  // The pitch, per frame. While a slide runs, the oscillator walks its own
  // step to the slide's destination every frame, at the slide's rate -- and
  // plays it under whatever vibrato or arpeggio the player has put on the
  // period, which is the ratio of the period it modulated to the one it
  // glided. When the slide ends, or a tone portamento or a note-less row
  // cancels it, the glide closes what is left at that rate rather than
  // jumping there, and lets go once it has. Glissando asked for semitone
  // steps, so it keeps the player's.
  double step = ch->step;
  if (ch->synth_glide > 0.0 && !ch->glissando && ch->period > 0.0 &&
      ch->base_period > 0.0) {
    const bool sliding = ch->slide_ticks > 0 && ch->target_period > 0.0;
    // The unmodulated pitch to glide to: the destination while sliding, and
    // the player's own glided period once it is not.
    const double want =
        ch->step * ch->period / (sliding ? ch->target_period : ch->base_period);
    ch->synth_step += (want - ch->synth_step) * ch->synth_glide;
    step = ch->synth_step * (ch->base_period / ch->period);
    const double gap = want - ch->synth_step;
    if (!sliding && (gap < 0.0 ? -gap : gap) <= want * 1e-6)
      ch->synth_glide = 0.0;    // arrived: a millionth, under a fiftieth of a cent
  } else {
    ch->synth_glide = 0.0;
    ch->synth_step = step;
  }

  // The record's wave 0 is the saw, because a saw is what a 303 is for.
  const bool square = ins.synth_wave != 0u;
  const float drive = (float) ins.synth_drive * (1.f / 255.f);
  const float mix = (float) ins.synth_dist_mix * (1.f / 255.f);
  const double half = step * 0.5;
  float sub[2];
  for (int k = 0; k < 2; ++k) {
    float s = fx::ladder303_process(
        &ch->synth_ladder,
        fx::fixed303_pre(&ch->synth_fixed, synth303_osc(ch->pos, half, square)));

    // Exactly a passthrough at drive 0 whatever the mix says, which is what
    // `shape_sample` guarantees -- so the four kinds collapse to one signal
    // there and the mix knob cannot introduce a difference of its own.
    // `synth_dist` is refused above `kSynthDistKinds` at load and at save, so
    // the byte is already one of the four by the time it gets here.
    const float wet = fx::shape_sample((fx::ShapeKind) ins.synth_dist, s, drive);
    s += (wet - s) * mix;

    // **The output stage is the house saturator, and it is load-bearing.** A
    // ladder at maximum resonance has a peak gain in the tens; a saw's
    // harmonic sitting on that peak leaves the voice well outside the range the
    // mixer's scaling assumes. Bounding it here, and the VCA's 0..1 after it,
    // are the reason "finite across the whole parameter grid" is a property
    // rather than a hope. Before the VCA, so it bounds the filter the same for
    // every note; see `kSynth303Drive`.
    sub[k] = fx::soft_clip(s * kSynth303Drive) * level;

    ch->pos += half;
    // A while rather than an if, for the reason the sample path's is: a high
    // note at a low rate can step past a 32-frame cycle more than once.
    while (ch->pos >= (double) kBuiltinWaveFrames)
      ch->pos -= (double) kBuiltinWaveFrames;
  }
  const float s = fx::fixed303_post(
      &ch->synth_fixed, fx::decim303_process(&ch->synth_decim, sub[0], sub[1]));

  // The -100 dB snap, not a denormal flush: this is the only thing that ends
  // the voice, and a multiplicative decay never reaches zero on its own. It is
  // also stricter than `flush_denorm` and subsumes it -- the state cannot reach
  // denormal range without passing through the snap first.
  //
  // **Both envelopes end the voice**, because both reach the level: a VCA that
  // has run out under a filter envelope still riding on it is not silence.
  ch->synth_env = synth_env_step(ch->synth_env, ch->synth_env_coef);
  ch->synth_venv = synth_env_step(ch->synth_venv, ch->synth_venv_coef);
  ++ch->synth_age;
  ch->synth_pos = ch->pos;
  if (ch->synth_venv == 0.f && ch->synth_env == 0.f)
    ch->playing = false;

  return s * 100.f;
}

// `legato`: the channel was sounding the 303 right up to this note. Only the
// 303 has state worth running on; a drum is struck cold every time.
inline void
synth_trigger(Channel *ch, const Instrument &ins, double sample_rate,
              bool legato) {
  const float rate = (float) (sample_rate > 1.0 ? sample_rate : 48000.0);
  if (ins.synth_voice == (uint8_t) SynthVoice::kBass) {
    synth303_trigger(ch, ins, rate, legato);
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
