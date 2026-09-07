// ntrk -- a small tracker replayer, and a format for it.
//
// Header-only and platform-free: two standard headers for the integer types,
// two headers of our own, no libm, no allocation, no threads, no I/O. Drop it
// in, call
// `ntrk::module_load` on a buffer you loaded yourself and `ntrk::render_add` from
// wherever your audio comes from.
//
//     ntrk::Module module;
//     if (!ntrk::module_load(&module, bytes, size))
//       ...                       // a bad file is refused, never read past
//     ntrk::Player player;
//     ntrk::player_start(&player, &module);
//     ntrk::render_add(&player, buffer, frames, 2, 48000.f);
//
// **The buffer must outlive the module.** A loaded module is a view over the
// file: patterns and samples are pointed at in place rather than copied, which
// is why nothing here allocates and why unloading is forgetting.
//
// `render_add` *adds*, so a tune can share a buffer with whatever else is
// playing. Zero the buffer first if it is yours alone.
//
// The format is `.ntrk` and `ntrk_import.{h,cc}` beside this file converts a
// ProTracker `.mod` or a `.xm` into one. README.md has the layout, the effect
// coverage and what is deliberately missing; `module_load` below is the format.
//
// Deterministic: the same module rendered twice produces identical samples,
// including the random vibrato waveform, which runs off a per-channel
// generator rather than anything shared or seeded from a clock.
//
// **The structs live in `ntrk_types.h` and the SYNTH instrument in
// `ntrk_synth.h`**, both included at the top in the ordinary way. The synth
// voices read a `Channel` and an `Instrument`, so those two headers are the
// dependency order: types, then voices, then the loader and the player here.
// The kit and the 303 are a file of their own only because they are half again
// as long as the replayer they hang off.
//
// It was not always ordinary. The structs used to be declared halfway down this
// file, so the only way to give the voices their types was to close the
// namespace, include the synth, and reopen it — which made `ntrk_synth.h` an
// include fragment that compiled at one point in one file and nowhere else. The
// trap is that nothing reports it: every test passed, and the header simply had
// no other legal use. `make test` now compiles every header here on its own.
//
// **The one outside dependency is `ntrk_fx_shape.h`, and it costs none of the
// above.** The synth instruments need a resonant filter and a saturator, and
// that header already has both -- written under exactly these terms. It reaches
// no further than `ntrk_dsp.h` and the same two standard headers this one does,
// has no libm call anywhere (the SVF's tan is a table plus a lerp, the shapers
// truncate by hand rather than calling `fmod`), allocates nothing, opens
// nothing, and is pure arithmetic throughout, so ARM64 and wasm still compute
// identical floats. `c++ -std=c++11 test_ntrk.cc` remains one command; the
// headers ship together.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_H_
#define NTRK_H_

#include "ntrk_fx_shape.h"
#include "ntrk_synth.h"
#include "ntrk_types.h"

#include <stddef.h>
#include <stdint.h>

namespace ntrk {

// ----------------------------------------------------------------------------
// -- A tracker replayer, and a format of our own to feed it
// ----------------------------------------------------------------------------
//
// Music as a pattern list and a handful of samples, which is what a 60K module
// buys over a 4 MB render of the same tune — the reason the Amiga did it this
// way and the reason it still suits a build that ships over the web.
//
// **The runtime format is ours; ProTracker is an import.** `ntrk_import.cc`
// turns a `.mod` into a `.ntrk`, and nothing here parses a `.mod` at all. Two
// reasons, and the second is the one that matters:
//
// - A `.mod` is 1988's on-disk format — big-endian, sample lengths in words,
//   periods rather than notes, thirty-one fixed instrument slots whether or not
//   a tune uses them. Everything about it wants byte-swapping and rescaling at
//   load, on a thread that should be doing neither.
// - **Authoring is the point, not just importing.** Music written for this
//   project should not have to pretend to be a 1988 Amiga module to be played
//   by it. The format carries what a player needs and nothing else, so a
//   generator can emit one directly; the importer is how a tune that already
//   exists as a `.mod` gets in.
//
// The usual route for new music is still a tracker: write it, export a `.mod`,
// run the importer. That is a real workflow rather than a compromise — and it
// is why the importer exists at all rather than an authoring format of our own
// invention that no editor can write.
//
// **Nothing here allocates.** A loaded module is a view over the file buffer:
// patterns and sample data are pointed at in place, so a module costs its file
// and a `Module` struct, and unloading is forgetting it. That also means **the
// buffer must outlive the module**, which is the one rule a caller has to keep.
//
// Platform-free and deterministic: no clock, no allocation, no GL. Two players
// given the same module and the same number of frames produce the same samples,
// which is what lets `tests/modules/tracker.cc` check any of this at all.

// ----------------------------------------------------------------------------
// -- Instrument parameters: everything the format has an opinion about
// ----------------------------------------------------------------------------
//
// **One decision, one implementation.** `module_load` and `module_save` each
// used to spell the same six field bounds, and the writer's comment admitted it
// twice: "the same field checks the loader makes, so a file this writes is a
// file that loads". Two copies of a range write a file the reader refuses the
// day one of them moves -- and one of them already had. `transpose` was
// narrowed to +/-48 in the loader, that guess turned out to be wrong and was
// deleted, and three copies of the old number outlived it, one of them under a
// comment claiming it was the format's. Both ends now walk the table below,
// which is therefore the only writer of what an instrument may hold.
//
// It is also exactly what an editor needs, and for a sharper reason than the
// command table next door. A drifted mnemonic mislabels a cell. A drifted bound
// lets someone type a value the format refuses, so the module plays all
// afternoon and will not save -- the same failure as a note above `note_max`,
// which is the bug this table exists to make unwritable.
//
// It lives here, above the loader, rather than beside `note_fx_table` at the
// bottom, because the loader and the writer are its first two callers.

// Every value of an instrument the format checks, in the order
// `instrument_param_table` lists them.
enum class InsParam {
  kType = 0, kVolume, kFinetune, kTranspose, kLoopStart, kLoopLen,
  kEnvelope, kAttackMs, kDecayMs, kSustain, kReleaseMs,
  kFilter, kCutoffHz, kResonance, kFilterType, kWave,
  kSynthVoice, kSynthTune, kSynthDecay, kSynthSweep, kSynthTone,
  kSynthNoise, kSynthNoiseDecay, kSynthDrive, kSynthCutoff, kSynthReso,
  kSynthEnvMod, kSynthAccent, kSynthDist, kSynthDistMix, kSynthWave,
  kCount
};

// Whether a value is in this instrument at all, and whether anything reads it.
//
// **`Absent` and `Inert` are different facts, and only one of them is the
// format's.** Absent means the format has no opinion: either the byte is not
// written -- a SYNP record exists only for a SYNTH instrument -- or it is
// written and nothing derives from it, which is what a loop start is on a
// one-shot. Inert means the byte IS stored, range-checked and round-tripped,
// but the voice currently selected does not read it: a hihat's `tune` is still
// there and becomes live again the moment the voice becomes a kick.
//
// So an editor **omits** an Absent row and **greys** an Inert one. Hiding a
// hihat's `tune` loses the kick setting the moment someone auditions a hihat,
// and does not give it back.
enum class ParamState { Absent, Inert, Live };

// One row. `ParamShape` is reused unchanged -- Continuous and Choice are the
// only two an instrument field is -- so an editor draws a synth-voice dropdown
// and an FXPL choice dropdown through one code path.
//
// `lo`/`hi` are what the command table deliberately has NOT got: a command
// parameter is always a byte, so a range there would have said nothing, while
// an instrument field is an int8, a u8, a u16 or a flag bit and the range is
// the fact `module_save` enforces. Two rows carry a range that is the sample's
// rather than the format's; `instrument_param_range` is the only honest reader
// of those, and it says so.
struct InsParamInfo {
  const char       *name;
  ParamShape        shape;        // Continuous or Choice; never SplitNibble
  int               lo, hi;
  int               choice_count; // 0 unless shape == Choice
  const char *const *choice;      // choice_count strings, else null
};

// The four filter modes, in the order `voice_filter_sample` switches on them.
// One list: the strings were written twice inside `ntrk_mix.cc` before this,
// and an editor's own copy would have been the third.
inline const char *const *
filter_mode_names(int *count) {
  static const char *const kNames[4] = {"lowpass", "highpass", "bandpass",
                                        "notch"};
  if (count != nullptr)
    *count = 4;
  return kNames;
}

inline const InsParamInfo *
instrument_param_table(int *count) {
  static const char *const kTypes[5] = {"pcm8", "pcm16", "wave data",
                                        "wave builtin", "synth"};
  static const char *const kOffOn[2] = {"off", "on"};
  static const char *const kVoices[kSynthVoiceCount] = {"kick", "snare",
                                                        "hihat", "clap",
                                                        "bass"};
  static const char *const kDistKinds[kSynthDistKinds] = {"warm", "crunch",
                                                          "tape", "fold"};
  // **Order trap:** 0 is saw. `synth303_sample` reads `synth_wave == 0` as the
  // saw and reaches for built-in shape 1, so this list is not the wave table's
  // order and must not be sorted into it.
  static const char *const kSynthWaves[kSynthWaveCount] = {"saw", "square"};
  // In `BuiltinWaves`' own order: square, saw, triangle, then the two pulses at
  // a quarter and an eighth of the cycle.
  static const char *const kWaves[kBuiltinWaveCount] = {
      "square", "saw", "triangle", "pulse 25%", "pulse 12%"};

  static const InsParamInfo kTable[(int) InsParam::kCount] = {
      {"type", ParamShape::Choice, 0, 4, 5, kTypes},
      {"volume", ParamShape::Continuous, 0, 64, 0, nullptr},
      {"finetune", ParamShape::Continuous, -8, 7, 0, nullptr},
      // A whole signed byte on purpose: the playable range is
      // kMinNote..note_max and an XM's `relative_note` lands here, so four
      // octaves is not enough to express what a file can ask for.
      // `note_transposed` clamps the RESULT, which is what makes any value safe.
      {"transpose", ParamShape::Continuous, -128, 127, 0, nullptr},
      // The two whose ceiling is the sample's. `instrument_param_range`
      // overrides both; the zeroes here are what a caller gets with no
      // instrument to measure, and reading `hi` off the row is a bug.
      {"loop start", ParamShape::Continuous, 0, 0, 0, nullptr},
      {"loop len", ParamShape::Continuous, 0, 0, 0, nullptr},
      {"envelope", ParamShape::Choice, 0, 1, 2, kOffOn},
      {"attack ms", ParamShape::Continuous, 0, 65535, 0, nullptr},
      {"decay ms", ParamShape::Continuous, 0, 65535, 0, nullptr},
      {"sustain", ParamShape::Continuous, 0, 64, 0, nullptr},
      {"release ms", ParamShape::Continuous, 0, 65535, 0, nullptr},
      {"filter", ParamShape::Choice, 0, 1, 2, kOffOn},
      {"cutoff hz", ParamShape::Continuous, 20, 20000, 0, nullptr},
      {"resonance", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"filter type", ParamShape::Choice, 0, 3, 4, filter_mode_names(nullptr)},
      {"wave", ParamShape::Choice, 0, kBuiltinWaveCount - 1, kBuiltinWaveCount,
       kWaves},
      {"synth voice", ParamShape::Choice, 0, kSynthVoiceCount - 1,
       kSynthVoiceCount, kVoices},
      {"synth tune", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth decay", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth sweep", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth tone", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth noise", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth noise decay", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth drive", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth cutoff", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth reso", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth env mod", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth accent", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth dist", ParamShape::Choice, 0, kSynthDistKinds - 1,
       kSynthDistKinds, kDistKinds},
      {"synth dist mix", ParamShape::Continuous, 0, 255, 0, nullptr},
      {"synth wave", ParamShape::Choice, 0, kSynthWaveCount - 1,
       kSynthWaveCount, kSynthWaves},
  };
  static_assert(sizeof(kTable) / sizeof(*kTable) == (size_t) InsParam::kCount,
                "the table and InsParam are one list");
  if (count != nullptr)
    *count = (int) InsParam::kCount;
  return kTable;
}

inline int
instrument_param_count() {
  return (int) InsParam::kCount;
}

inline const InsParamInfo *
instrument_param_at(int id) {
  if (id < 0 || id >= (int) InsParam::kCount)
    return nullptr;
  return instrument_param_table(nullptr) + id;
}

// Which synth rows the selected voice actually reads.
//
// **This is here rather than in a caller because the obvious way to ask it is
// out of bounds:** `kSynthVoiceSpecs` has `kSynthDrumCount` rows and
// `synth_voice` reaches `kBass`, so `kSynthVoiceSpecs[ins->synth_voice]` reads
// past the end for a bass. Answering it once is what stops that index being
// written somewhere else.
inline bool
synth_param_read_by_voice(const Instrument *ins, InsParam p) {
  if (p == InsParam::kSynthVoice)
    return true;
  if (ins->synth_voice == (uint8_t) SynthVoice::kBass) {
    // `synth303_sample` reads exactly these and nothing else.
    switch (p) {
      case InsParam::kSynthWave:
      case InsParam::kSynthCutoff:
      case InsParam::kSynthReso:
      case InsParam::kSynthEnvMod:
      case InsParam::kSynthAccent:
      case InsParam::kSynthDecay:
      case InsParam::kSynthDrive:
      case InsParam::kSynthDist:
      case InsParam::kSynthDistMix:
        return true;
      default:
        return false;
    }
  }
  // A drum reads none of the 303's.
  switch (p) {
    case InsParam::kSynthCutoff:
    case InsParam::kSynthReso:
    case InsParam::kSynthEnvMod:
    case InsParam::kSynthAccent:
    case InsParam::kSynthDist:
    case InsParam::kSynthDistMix:
    case InsParam::kSynthWave:
      return false;
    default:
      break;
  }
  // ...and a drum with no resonator reads nothing about a body: `body_hz` is 0
  // for the hihat and the clap, which switches the body block off entirely and
  // pins the noise/body mix at all-noise.
  const int voice = (int) ins->synth_voice;
  if (voice >= 0 && voice < kSynthDrumCount &&
      kSynthVoiceSpecs[voice].body_hz <= 0.f) {
    switch (p) {
      case InsParam::kSynthTune:
      case InsParam::kSynthSweep:
      case InsParam::kSynthNoise:
      case InsParam::kSynthDecay:
        return false;
      default:
        break;
    }
  }
  return true;
}

inline ParamState
instrument_param_state(const Instrument *ins, int id) {
  if (ins == nullptr || id < 0 || id >= (int) InsParam::kCount)
    return ParamState::Absent;
  const InsParam p = (InsParam) id;
  const uint8_t type = ins->type;

  // A built-in shape names one of the generated cycles; every other type does
  // not carry the field at all.
  if (p == InsParam::kWave)
    return type == (uint8_t) InstrumentType::kWaveBuiltin ? ParamState::Live
                                                          : ParamState::Absent;
  // The cutoff is bounded only when the bit is set. With it clear the field is
  // unread and unchecked -- which is the trap `instrument_param_set` closes,
  // because switching the bit on brings a 0 that was fine under a bound that
  // refuses it.
  if (p == InsParam::kCutoffHz)
    return (ins->flags & kInstrumentFilter) != 0u ? ParamState::Live
                                                  : ParamState::Absent;
  if (p == InsParam::kLoopStart || p == InsParam::kLoopLen) {
    // A built-in shape's geometry is generated at load and whatever the entry
    // stored is discarded, so neither is a round-trip field.
    if (type == (uint8_t) InstrumentType::kWaveBuiltin)
      return ParamState::Absent;
    // Without a loop there is nothing for a start to be inside of, and the
    // format checks it only when there is one. The length is the field to set
    // first, which is also how a sample editor's loop drag works.
    if (p == InsParam::kLoopStart && ins->loop_len == 0u)
      return ParamState::Absent;
    // **A synth walks no sample.** `channel_sample` returns before the sample
    // fields are read and `channel_trigger` skips the length test outright, so
    // a loop on a SYNTH instrument is stored, range-checked and round-tripped
    // and nothing plays it -- which is Inert exactly. Not Absent: the bytes are
    // in the entry and come back, so an editor greys these and does not drop
    // the loop someone set before changing the type.
    if (type == (uint8_t) InstrumentType::kSynth)
      return ParamState::Inert;
    return ParamState::Live;
  }
  if (p >= InsParam::kSynthVoice) {
    if (type != (uint8_t) InstrumentType::kSynth)
      return ParamState::Absent;
    return synth_param_read_by_voice(ins, p) ? ParamState::Live
                                             : ParamState::Inert;
  }
  return ParamState::Live;
}

inline void
instrument_param_range(const Instrument *ins, int id, int *lo, int *hi) {
  const InsParamInfo *info = instrument_param_at(id);
  int l = 0, h = 0;
  if (info != nullptr) {
    l = info->lo;
    h = info->hi;
    // The two rows the sample sizes. Computed in a width that cannot wrap: a
    // loop start at or past the end would underflow the subtraction, and that
    // wrap is exactly how a loop running off the blob gets past a check.
    if (ins != nullptr) {
      const long long len = (long long) ins->length;
      if ((InsParam) id == InsParam::kLoopStart)
        h = len > 0 ? (int) (len - 1) : 0;
      else if ((InsParam) id == InsParam::kLoopLen)
        h = len > (long long) ins->loop_start
                ? (int) (len - (long long) ins->loop_start)
                : 0;
    }
  }
  if (lo != nullptr)
    *lo = l;
  if (hi != nullptr)
    *hi = h;
}

inline int
instrument_param_get(const Instrument *ins, int id) {
  if (ins == nullptr)
    return 0;
  switch ((InsParam) id) {
    case InsParam::kType:       return (int) ins->type;
    case InsParam::kVolume:     return (int) ins->volume;
    case InsParam::kFinetune:   return (int) ins->finetune;
    case InsParam::kTranspose:  return (int) ins->transpose;
    case InsParam::kLoopStart:  return (int) ins->loop_start;
    case InsParam::kLoopLen:    return (int) ins->loop_len;
    case InsParam::kEnvelope:
      return (ins->flags & kInstrumentEnvelope) != 0u ? 1 : 0;
    case InsParam::kAttackMs:   return (int) ins->env_attack_ms;
    case InsParam::kDecayMs:    return (int) ins->env_decay_ms;
    case InsParam::kSustain:    return (int) ins->env_sustain;
    case InsParam::kReleaseMs:  return (int) ins->env_release_ms;
    case InsParam::kFilter:
      return (ins->flags & kInstrumentFilter) != 0u ? 1 : 0;
    case InsParam::kCutoffHz:   return (int) ins->filter_cutoff_hz;
    case InsParam::kResonance:  return (int) ins->filter_res;
    case InsParam::kFilterType:
      return (int) ((ins->flags & kInstrumentFilterType) >> 2);
    case InsParam::kWave:       return (int) ins->wave_index;
    case InsParam::kSynthVoice: return (int) ins->synth_voice;
    case InsParam::kSynthTune:  return (int) ins->synth_tune;
    case InsParam::kSynthDecay: return (int) ins->synth_decay;
    case InsParam::kSynthSweep: return (int) ins->synth_sweep;
    case InsParam::kSynthTone:  return (int) ins->synth_tone;
    case InsParam::kSynthNoise: return (int) ins->synth_noise;
    case InsParam::kSynthNoiseDecay: return (int) ins->synth_noise_decay;
    case InsParam::kSynthDrive: return (int) ins->synth_drive;
    case InsParam::kSynthCutoff: return (int) ins->synth_cutoff;
    case InsParam::kSynthReso:  return (int) ins->synth_reso;
    case InsParam::kSynthEnvMod: return (int) ins->synth_env_mod;
    case InsParam::kSynthAccent: return (int) ins->synth_accent;
    case InsParam::kSynthDist:  return (int) ins->synth_dist;
    case InsParam::kSynthDistMix: return (int) ins->synth_dist_mix;
    case InsParam::kSynthWave:  return (int) ins->synth_wave;
    case InsParam::kCount:      break;
  }
  return 0;
}

// **Clamps rather than rejects, and that is the contract.** No sequence of
// calls to this can author an instrument `module_save` refuses; a bool return
// would hand that decision back to every caller and one of them would drop it.
// Three rows carry a companion derivation, and each is a copy of a rule the
// loader already applies -- which is the point: the loader is where they came
// from, so what this writes is what a reload produces.
inline void
instrument_param_set(Instrument *ins, int id, int value) {
  if (ins == nullptr || instrument_param_at(id) == nullptr)
    return;
  int lo = 0, hi = 0;
  instrument_param_range(ins, id, &lo, &hi);
  const int v = value < lo ? lo : (value > hi ? hi : value);

  switch ((InsParam) id) {
    case InsParam::kType:
      ins->type = (uint8_t) v;
      ins->bits = instrument_bits_for(ins->type);
      // The geometry the loader gives a built-in shape, applied here so the
      // instrument in hand is the one a reload produces rather than one that
      // silently changes shape on the way back in.
      if (ins->type == (uint8_t) InstrumentType::kWaveBuiltin) {
        if (ins->wave_index >= (uint8_t) kBuiltinWaveCount)
          ins->wave_index = 0;
        ins->data = builtin_wave((int) ins->wave_index);
        ins->length = (uint32_t) kBuiltinWaveFrames;
        ins->loop_start = 0;
        ins->loop_len = (uint32_t) kBuiltinWaveFrames;
      }
      break;
    case InsParam::kVolume:    ins->volume = (uint8_t) v; break;
    case InsParam::kFinetune:  ins->finetune = (int8_t) v; break;
    case InsParam::kTranspose: ins->transpose = (int8_t) v; break;
    case InsParam::kLoopStart:
      ins->loop_start = (uint32_t) v;
      // A start moved forward shortens the loop. Its own range keeps it inside
      // the sample, but the format's rule is about the PAIR -- `loop_start +
      // loop_len <= length` -- and moving one end without the other is how a
      // setter that clamps every field authors a module that will not save.
      if ((long long) ins->loop_start + (long long) ins->loop_len >
          (long long) ins->length)
        ins->loop_len = ins->length - ins->loop_start;
      if (ins->loop_len == 1u)
        ins->loop_len = 0u;
      break;
    case InsParam::kLoopLen:
      // A loop of one frame is what the loader turns into a one-shot, so
      // authoring one would come back as something else.
      ins->loop_len = (uint32_t) (v == 1 ? 0 : v);
      break;
    case InsParam::kEnvelope:
      ins->flags = (uint8_t) (v != 0 ? (ins->flags | kInstrumentEnvelope)
                                     : (ins->flags & ~kInstrumentEnvelope));
      break;
    case InsParam::kAttackMs:  ins->env_attack_ms = (uint16_t) v; break;
    case InsParam::kDecayMs:   ins->env_decay_ms = (uint16_t) v; break;
    case InsParam::kSustain:   ins->env_sustain = (uint8_t) v; break;
    case InsParam::kReleaseMs: ins->env_release_ms = (uint16_t) v; break;
    case InsParam::kFilter:
      if (v != 0) {
        ins->flags = (uint8_t) (ins->flags | kInstrumentFilter);
        // Switching the filter on brings the cutoff under a bound that did not
        // apply a moment ago, and the value sitting there is 0 on every
        // instrument that never had one. Without this, ticking the box authors
        // a module that plays and will not save.
        if (ins->filter_cutoff_hz < 20u)
          ins->filter_cutoff_hz = 20u;
        else if (ins->filter_cutoff_hz > 20000u)
          ins->filter_cutoff_hz = 20000u;
      } else {
        ins->flags = (uint8_t) (ins->flags & ~kInstrumentFilter);
      }
      break;
    case InsParam::kCutoffHz:  ins->filter_cutoff_hz = (uint16_t) v; break;
    case InsParam::kResonance: ins->filter_res = (uint8_t) v; break;
    case InsParam::kFilterType:
      ins->flags = (uint8_t) ((ins->flags & ~kInstrumentFilterType) |
                              (uint8_t) ((v << 2) & kInstrumentFilterType));
      break;
    case InsParam::kWave:
      ins->wave_index = (uint8_t) v;
      // A built-in instrument's data IS its wave index; the two cannot be set
      // apart without the sample going stale.
      if (ins->type == (uint8_t) InstrumentType::kWaveBuiltin)
        ins->data = builtin_wave(v);
      break;
    case InsParam::kSynthVoice: ins->synth_voice = (uint8_t) v; break;
    case InsParam::kSynthTune:  ins->synth_tune = (uint8_t) v; break;
    case InsParam::kSynthDecay: ins->synth_decay = (uint8_t) v; break;
    case InsParam::kSynthSweep: ins->synth_sweep = (uint8_t) v; break;
    case InsParam::kSynthTone:  ins->synth_tone = (uint8_t) v; break;
    case InsParam::kSynthNoise: ins->synth_noise = (uint8_t) v; break;
    case InsParam::kSynthNoiseDecay: ins->synth_noise_decay = (uint8_t) v; break;
    case InsParam::kSynthDrive: ins->synth_drive = (uint8_t) v; break;
    case InsParam::kSynthCutoff: ins->synth_cutoff = (uint8_t) v; break;
    case InsParam::kSynthReso:  ins->synth_reso = (uint8_t) v; break;
    case InsParam::kSynthEnvMod: ins->synth_env_mod = (uint8_t) v; break;
    case InsParam::kSynthAccent: ins->synth_accent = (uint8_t) v; break;
    case InsParam::kSynthDist:  ins->synth_dist = (uint8_t) v; break;
    case InsParam::kSynthDistMix: ins->synth_dist_mix = (uint8_t) v; break;
    case InsParam::kSynthWave:  ins->synth_wave = (uint8_t) v; break;
    case InsParam::kCount:      break;
  }
}

// Every per-field bound the format has, in one walk. `module_load` and
// `module_save` both call this and neither spells a range of its own, which is
// what makes a file this writes a file that loads.
//
// The structural checks are NOT here and stay at their ends: a blob offset, a
// null `data` behind a non-zero length and the one-frame-loop coercion are not
// per-field ranges, and only one end has the context for each.
inline bool
instrument_fields_valid(const Instrument &ins) {
  // The loop rows are the sample's own indices and the table reports them as
  // `int`. Nothing addressable by this walk is longer, and no .ntrk holds such
  // a sample -- the blob is a buffer in memory that was read in one piece.
  if (ins.length > (uint32_t) 0x7fffffff)
    return false;
  for (int id = 0; id < (int) InsParam::kCount; ++id) {
    // **`Absent` is the only state that skips.** `Inert` still checks: the
    // loader validates a kick's `synth_dist` whatever the voice, because a
    // value outside the enum is a file that has gone wrong rather than a
    // setting this voice happens not to be reading today.
    if (instrument_param_state(&ins, id) == ParamState::Absent)
      continue;
    int lo = 0, hi = 0;
    instrument_param_range(&ins, id, &lo, &hi);
    const int v = instrument_param_get(&ins, id);
    if (v < lo || v > hi)
      return false;
  }
  return true;
}

// ----------------------------------------------------------------------------
// -- Module fields: the header's own bounds, in one place
// ----------------------------------------------------------------------------
//
// **The same fix as `InsParam`, one struct up.** `module_load` and `module_save`
// spelled these thirteen ranges verbatim in both directions, and the writer's
// comment admitted it in as many words: "the TUNE bounds, mirrored from the
// loader -- a writer stricter or looser than its reader breaks the round trip
// in one direction or the other". A comment naming a duplication is a
// duplication with a note attached; this is the version with one writer.
//
// Only genuine RANGES are here. `note_max` is deliberately two values rather
// than a range -- a file that could choose its own ceiling would be a file that
// chooses its own bounds -- and `rows_per_bar % rows_per_beat`, a null `order`
// and the order-names-a-pattern sweep are relations, not bounds. Those stay at
// the end that has the context for them, exactly as the instrument table leaves
// the blob offset and the one-frame-loop coercion where they are.
//
// An editor gets the ranges as a consequence: a BPM stepper that knows 32..255
// because it asked, rather than because someone typed it in a widget.

enum class ModParam {
  kChannels = 0, kRows, kSpeed, kBpm,
  kOrderCount, kPatternCount, kInstrumentCount, kRestart,
  kFxColumns, kMetaColumns,
  kRowsPerBeat, kRowsPerBar, kSwing,
  kCount
};

// The format's own word for the field, and the range it accepts. The name is
// the format's -- an editor's three-letter band label ("BPM", "LEN") is its own
// abbreviation for a 26 px strip and not a second copy of anything.
struct ModParamInfo {
  const char *name;
  int         lo, hi;
};

inline const ModParamInfo *
module_param_table(int *count) {
  static const ModParamInfo kTable[(int) ModParam::kCount] = {
      {"channels", 1, kMaxChannels},
      {"rows", 1, kMaxRows},
      {"speed", 1, 31},
      {"bpm", 32, 255},
      {"order count", 1, 256},
      {"pattern count", 1, kMaxPatterns},
      {"instrument count", 0, kMaxInstruments},
      // The one whose ceiling is another field's. `module_param_range` is the
      // only honest reader of it; the zero here is what a caller gets with no
      // module to measure.
      {"restart", 0, 0},
      {"fx columns", 1, kMaxFxColumns},
      {"meta columns", 0, kMaxMetaColumns},
      {"rows per beat", 1, 255},
      {"rows per bar", 1, 255},
      {"swing", 0, kSwingMax},
  };
  static_assert(sizeof(kTable) / sizeof(*kTable) == (size_t) ModParam::kCount,
                "the table and ModParam are one list");
  if (count != nullptr)
    *count = (int) ModParam::kCount;
  return kTable;
}

inline int
module_param_count() {
  return (int) ModParam::kCount;
}

inline const ModParamInfo *
module_param_at(int id) {
  if (id < 0 || id >= (int) ModParam::kCount)
    return nullptr;
  return module_param_table(nullptr) + id;
}

inline int
module_param_get(const Module *m, int id) {
  if (m == nullptr)
    return 0;
  switch ((ModParam) id) {
    case ModParam::kChannels:       return m->channels;
    case ModParam::kRows:           return m->rows;
    case ModParam::kSpeed:          return m->speed;
    case ModParam::kBpm:            return m->bpm;
    case ModParam::kOrderCount:     return m->order_count;
    case ModParam::kPatternCount:   return m->pattern_count;
    case ModParam::kInstrumentCount: return m->instrument_count;
    case ModParam::kRestart:        return m->restart;
    case ModParam::kFxColumns:      return m->fx_columns;
    case ModParam::kMetaColumns:    return m->meta_columns;
    case ModParam::kRowsPerBeat:    return m->rows_per_beat;
    case ModParam::kRowsPerBar:     return m->rows_per_bar;
    case ModParam::kSwing:          return m->swing;
    case ModParam::kCount:          break;
  }
  return 0;
}

inline void
module_param_range(const Module *m, int id, int *lo, int *hi) {
  const ModParamInfo *info = module_param_at(id);
  int l = 0, h = 0;
  if (info != nullptr) {
    l = info->lo;
    h = info->hi;
    // A restart names an entry of THIS order list, so its ceiling moves with
    // the list. An editor shortening the order list has to bring it down, which
    // is what makes the range dynamic rather than the constant 255 it looks
    // like from the file's side.
    if (m != nullptr && (ModParam) id == ModParam::kRestart)
      h = m->order_count > 0 ? m->order_count - 1 : 0;
  }
  if (lo != nullptr)
    *lo = l;
  if (hi != nullptr)
    *hi = h;
}

// Every module-level range, in one walk, called by both ends of the format.
inline bool
module_fields_valid(const Module *m) {
  if (m == nullptr)
    return false;
  for (int id = 0; id < (int) ModParam::kCount; ++id) {
    int lo = 0, hi = 0;
    module_param_range(m, id, &lo, &hi);
    const int v = module_param_get(m, id);
    if (v < lo || v > hi)
      return false;
  }
  return true;
}

// ----------------------------------------------------------------------------
// -- Loading
// ----------------------------------------------------------------------------
//
// **A trust boundary, so every field is checked and a bad file is refused.**
// This reads a buffer that came off a disk, and `Assert` compiles to nothing in
// a release build — the four bounds checks in this codebase that were only
// assertions all read memory they did not own. Every return below is a file
// this player will not play, and none of them is a crash.

inline uint16_t
read_u16(const uint8_t *p) {
  return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

inline uint32_t
read_u32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
         ((uint32_t) p[3] << 24);
}

// **Two's complement by construction rather than by conversion.** Converting a
// `uint16_t` above 32767 to `int16_t` is implementation-defined before C++20,
// and this library's cross-target identity is a measured property rather than a
// hope — the same reason the fingerprint quantiser goes through a `short`
// instead of shifting a negative value right.
inline int16_t
read_i16(const uint8_t *p) {
  const uint16_t v = read_u16(p);
  return v < 0x8000u ? (int16_t) v : (int16_t) ((int32_t) v - 65536);
}

// The writers exist for module_save below. Little-endian by hand, the same as
// the readers, so neither depends on the host's byte order.
inline void
write_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t) (v & 0xffu);
  p[1] = (uint8_t) ((v >> 8) & 0xffu);
}

inline void
write_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) (v & 0xffu);
  p[1] = (uint8_t) ((v >> 8) & 0xffu);
  p[2] = (uint8_t) ((v >> 16) & 0xffu);
  p[3] = (uint8_t) ((v >> 24) & 0xffu);
}

// The mirror of `read_i16`, and negative values are folded up rather than cast
// down for the same reason.
inline void
write_i16(uint8_t *p, int16_t v) {
  write_u16(p, (uint16_t) (v < 0 ? (int32_t) v + 65536 : (int32_t) v));
}

// The header is fixed at 32 bytes and every count in it is a u16, so the
// arithmetic below cannot overflow a size_t on any machine this runs on. The
// largest file the ranges permit is 4 MB of patterns and 16.5 MB of plane:
// 256 patterns of 256 rows, sixteen channels of eight effect columns plus four
// meta lanes, two bytes a cell. The plane figure was 2 MB when a lane was a
// lane and a channel had one; it grew with `kMaxFxColumns`.
//
// **32 bytes stays enough, and a header change is version 3.** Every byte of
// the header is spent; anything else that wants header room goes in the block
// directory instead, so there is no "how does a reader tell the header grew"
// problem to solve.
//
// The header byte map, since nothing else records it now:
//
//     0  4  "NTRK"          14  2  order_count      26  2  block_count
//     4  2  version == 2    16  2  pattern_count    28  1  note_max (36|96)
//     6  2  channels 1..16  18  2  instrument_count 29  1  flags, must be 0
//     8  2  rows 1..256     20  2  restart          30  2  reserved, must be 0
//    10  2  speed 1..31     22  4  blob_bytes
//    12  2  bpm 32..255
//
// **`blob_bytes` is why the version byte is 2 and not 1.** A version 1 format
// existed: same magic, a 20-byte instrument entry instead of 32, no declared
// blob length (the blob was "whatever is left of the file", which stops working
// the instant anything follows it), no block directory, 8 channels and 32
// instruments, and no note above 36. It is refused by the version check below.
// Renumbering this format to 1 would make every one of those files *pass* that
// check and then be read against the wrong instrument stride, which is the one
// failure a loader must not have — so the byte stays 2 and an old file fails
// as "too old" rather than as silent garbage.
const int kHeaderBytes = 32;
const int kInstrumentBytes = 32;

// The block directory: `id` (u16), `flags` (u16), `offset` (u32 from the start
// of the file), `bytes` (u32).
const int kDirectoryEntryBytes = 12;
const int kMaxBlocks = 32;

// Block ids. FXPL, NAME, SYNP, MACR, MIXR and TUNE are understood; 0x0010 AUTO is reserved, — automation lanes were what MACR replaced, and the id
// stays spent rather than reused so a file written against the older sketch
// cannot be misread as something else. An id this reader does not know is
// skipped when the file says it is optional and refused when it says it is
// critical, which is how this reader stays honest against a file written by a
// later one.
const uint16_t kBlockFxpl = 0x0001;
const uint16_t kBlockName = 0x0002;
const uint16_t kBlockTune = 0x0006;
const uint16_t kBlockMacr = 0x0004;
const uint16_t kBlockMixr = 0x0005;
const uint16_t kBlockSlic = 0x000A;
const uint16_t kBlockSmut = 0x000B;
const uint16_t kBlockCritical = 0x0001;   // a directory entry's flags, bit 0

// SMUT: `u16 order_count`, `u16 channels`, then one `u16 LE` per order position
// -- bit `c` set means channel `c` is silent for that position. `kMaxChannels`
// is 16, so a position is exactly one u16 and there is no packing arithmetic
// anywhere in the format, the player or an editor.
//
// **Critical, and written only when a bit is set.** A tune whose arrangement
// depends on slot mutes plays WRONG without them -- a part that should be
// silent is heard, which is not a degraded rendering but a different piece of
// music. That is unlike MIXR, where a reader that skips the block still plays
// the tune with the host's own sends. Writing the block only when it says
// something means files that do not use the feature stay readable by every
// reader, and only the files that genuinely need it refuse to be misread.
const int kSmutHeaderBytes = 4;

// SLIC: `u16 table_count`, `u16` reserved, then one four-byte record per
// instrument that has slices -- `u8 instrument` (1-based), `u8` reserved,
// `u16 slice_count` -- and then, in the same order, every table's `u32 LE`
// frame offsets end to end.
//
// **Deliberately NOT MACR's fixed-max record.** MACR's is 52 bytes because
// `kMaxMacroTargets` is 8; a fixed slice record would be `4 + kMaxSlices * 4`
// and would bake the ceiling into the wire format, where it could never be
// raised without a version bump. Two loops buy that.
//
// The exact length is `4 + table_count*4 + total*4`, so the whole size is known
// before any offset is dereferenced -- the loader's own discipline.
const int kSlicHeaderBytes = 4;
const int kSlicRecordBytes = 4;
const int kSlicOffsetBytes = 4;

// `SLC` — start the note at a slice from the instrument's `SLIC` table.
//
// **Past the nibble on purpose.** ProTracker's effect column is a nibble and
// every one of its sixteen values is spent; this format's `Note::effect` is a
// whole byte, so a new command starts at 0x10 rather than stealing `Exx`'s
// sub-nibble space. The consequence is that **three display functions which
// mask `effect & 15` have to know about it** -- `note_fx_index`,
// `note_fx_describe` and `note_fx_repr` -- or `SLC 05` prints as `ARP`,
// describes as *Arpeggio*, and renders as `005`, colliding with arpeggio.
const uint8_t kFxSlice = 0x10;

// The FXPL payload's geometry prefix: `u16 fx_columns`, `u16 meta_columns`,
// then the cells. In the payload rather than in the header because the header
// is full and a header change is version 3.
//
// **An FXPL block whose length disagrees with its prefix is refused, never
// guessed at.** The exact-length check below is what "a reader does not
// silently misread a file of its own version" means, and it is what retired an
// earlier one-column layout cleanly rather than misreading it.
const int kFxplPrefixBytes = 4;

// MACR: `u16 macro_count`, `u16` reserved, then fixed-size records. Fixed size
// rather than packed to `target_count`, so the block's length is one multiply
// to check and a record is one multiply to reach.
const int kMacroHeaderBytes = 4;    // the block's count and its reserved word
const int kMacroTargetsAt = 4;      // where a record's target slots start
const int kMacroTargetBytes = 6;
const int kMacroRecordBytes =
    kMacroTargetsAt + kMaxMacroTargets * kMacroTargetBytes;   // 52

inline bool
module_load(Module *module, const uint8_t *data, size_t size) {
  *module = Module();
  if (data == nullptr || size < (size_t) kHeaderBytes)
    return false;
  if (data[0] != 'N' || data[1] != 'T' || data[2] != 'R' || data[3] != 'K')
    return false;
  // One version, and version 1 is refused here rather than converted. See the
  // header byte map above for why the byte is 2.
  const uint16_t version = read_u16(data + 4);
  if (version != 2u)
    return false;
  module->version = (int) version;

  module->channels = (int) read_u16(data + 6);
  module->rows = (int) read_u16(data + 8);
  module->speed = (int) read_u16(data + 10);
  module->bpm = (int) read_u16(data + 12);
  module->order_count = (int) read_u16(data + 14);
  module->pattern_count = (int) read_u16(data + 16);
  module->instrument_count = (int) read_u16(data + 18);
  module->restart = (int) read_u16(data + 20);

  // Every header range, from the one table `module_save` also walks. The block
  // fields it covers -- the plane's geometry and TUNE's -- are still at their
  // defaults here and are checked again once their blocks have been read.
  if (!module_fields_valid(module))
    return false;

  // The tail of the header. `note_max` is one of two values rather than a
  // range: it is the ceiling every note in the file is checked against, so a
  // file that could choose its own would be a file that chooses its own bounds.
  const size_t declared_blob = (size_t) read_u32(data + 22);
  const int block_count = (int) read_u16(data + 26);
  module->note_max = (int) data[28];
  if (block_count > kMaxBlocks)
    return false;
  if (module->note_max != 36 && module->note_max != kMaxNote)
    return false;
  if (data[29] != 0u)                 // header flags, reserved
    return false;
  if (read_u16(data + 30) != 0u)      // reserved
    return false;

  // The positional blocks, in order, each checked against what is left of the
  // file before it is pointed at.
  size_t at = (size_t) kHeaderBytes;

  const size_t order_bytes = (size_t) module->order_count;
  if (size - at < order_bytes)
    return false;
  module->order = data + at;
  at += order_bytes;

  // Every entry has to name a pattern that exists, or a jump lands outside the
  // pattern block and reads whatever follows it.
  for (int i = 0; i < module->order_count; ++i)
    if ((int) module->order[i] >= module->pattern_count)
      return false;

  const size_t instrument_stride = (size_t) kInstrumentBytes;
  const size_t instrument_bytes =
      (size_t) module->instrument_count * instrument_stride;
  if (size - at < instrument_bytes)
    return false;
  const uint8_t *instrument_table = data + at;
  at += instrument_bytes;

  const size_t pattern_bytes = (size_t) module->pattern_count *
                               (size_t) module->rows *
                               (size_t) module->channels * 4u;
  if (size - at < pattern_bytes)
    return false;
  module->patterns = (const Note *) (const void *) (data + at);
  at += pattern_bytes;

  // **Every note has to be one this module can name.** A file carrying a note
  // above its own `note_max` would load happily here and then be refused by
  // `module_save`, and loadable-but-unsaveable is a file only one half of the
  // format accepts. The refusal belongs here, where the file is still being
  // judged, and it is the deliberate mirror of the check in `module_save`.
  {
    const size_t cells = (size_t) module->pattern_count *
                         (size_t) module->rows * (size_t) module->channels;
    for (size_t i = 0; i < cells; ++i) {
      const int note = (int) module->patterns[i].note;
      if (note > module->note_max && note != kNoteOff)
        return false;
    }
  }

  // The sample blob, which every instrument indexes into. **Its length is
  // declared rather than inferred from what is left of the file**: the block
  // directory follows it, so "the rest" was never an answer that could survive
  // anything being appended.
  const size_t blob_bytes = declared_blob;
  if (size - at < blob_bytes)
    return false;
  const uint8_t *blob = data + at;
  at += blob_bytes;

  // The block directory. **Every bound is a subtraction from what remains**:
  // `offset + bytes` wraps on a hostile pair and passes, which is the one
  // arithmetic mistake a loader cannot afford to make.
  if (size - at < (size_t) block_count * (size_t) kDirectoryEntryBytes)
    return false;
  const uint8_t *directory = data + at;

  uint16_t seen[kMaxBlocks];
  int seen_count = 0;
  const uint8_t *synp = nullptr;
  size_t synp_bytes = 0;
  const uint8_t *name = nullptr;
  size_t name_bytes = 0;
  const uint8_t *tune = nullptr;
  size_t tune_bytes = 0;
  const uint8_t *smut = nullptr;
  size_t smut_bytes = 0;
  const uint8_t *macr = nullptr;
  size_t macr_bytes = 0;
  const uint8_t *slic = nullptr;
  size_t slic_bytes = 0;
  for (int i = 0; i < block_count; ++i) {
    const uint8_t *e = directory + (size_t) i * (size_t) kDirectoryEntryBytes;
    const uint16_t id = read_u16(e + 0);
    const uint16_t flags = read_u16(e + 2);
    const uint32_t offset = read_u32(e + 4);
    const uint32_t bytes = read_u32(e + 8);

    if ((flags & (uint16_t) ~kBlockCritical) != 0u)
      return false;

    // One payload per id. Two is a file that disagrees with itself, and which
    // of them wins would be a reader's private business rather than the
    // format's.
    for (int k = 0; k < seen_count; ++k)
      if (seen[k] == id)
        return false;
    seen[seen_count++] = id;

    if ((size_t) offset > size)
      return false;
    if ((size_t) bytes > size - (size_t) offset)
      return false;

    if (id == kBlockFxpl) {
      // The geometry comes out of the payload's own first four bytes, so it is
      // read before anything is sized against it -- and the block has to be at
      // least that long before those four bytes are looked at.
      if ((size_t) bytes < (size_t) kFxplPrefixBytes)
        return false;
      const uint8_t *p = data + offset;
      module->fx_columns = (int) read_u16(p + 0);
      module->meta_columns = (int) read_u16(p + 2);
      // The plane's geometry, from the same table -- the fields were at their
      // defaults when the header was walked and carry the file's values now.
      if (!module_fields_valid(module))
        return false;

      // **Exact rather than sufficient.** One `{cmd, param}` per lane per row
      // and no more: a length that merely fits is a plane that does not line up
      // with the patterns it belongs to, which is malformed rather than
      // generous, and exactness costs nothing. Lanes rather than channels is
      // the whole of what multiple columns cost this check.
      const size_t want = (size_t) kFxplPrefixBytes +
                          (size_t) module->pattern_count *
                              (size_t) module->rows *
                              (size_t) module_lanes(module) * 2u;
      if ((size_t) bytes != want)
        return false;
      // `FxCell` has no alignment of its own -- two uint8_t -- so the cells
      // start at an arbitrary offset plus four and the cast stays sound.
      module->fx = (const FxCell *) (const void *) (p + kFxplPrefixBytes);
    } else if (id == kBlockMacr) {
      // Recorded and parsed below rather than here: the meta lanes and the
      // macro table judge each other, and a directory may name the two blocks
      // in either order.
      macr = data + offset;
      macr_bytes = (size_t) bytes;
    } else if (id == kBlockTune) {
      // Fixed size and self-contained -- it is judged against nothing else, so
      // unlike NAME and SYNP it could be parsed here. It is parsed with them
      // anyway, so that every optional block is validated in one place rather
      // than half here and half below.
      tune = data + offset;
      tune_bytes = (size_t) bytes;
    } else if (id == kBlockName) {
      // Judged against the instrument count, which is parsed below -- the same
      // reason SYNP is checked there rather than here.
      name = data + offset;
      name_bytes = (size_t) bytes;
    } else if (id == kBlockSynp) {
      // The payload is already inside the file -- that was the subtraction two
      // checks up, and it ran before this pointer was formed. Its *length* can
      // only be judged against the number of SYNTH instruments, and the
      // instrument table is parsed below, so the check is made there rather
      // than a second copy of the parse made here.
      synp = data + offset;
      synp_bytes = (size_t) bytes;
    } else if (id == kBlockSmut) {
      // Judged against `order_count` and `channels`, which the header has
      // already given -- but so has every other block's check been deferred to
      // one place, and this one is validated below with them rather than half
      // here.
      smut = data + offset;
      smut_bytes = (size_t) bytes;
    } else if (id == kBlockSlic) {
      // Judged against the instrument table -- both the count and every
      // instrument's `length` -- which is parsed below, so the check is made
      // there for the same reason SYNP's and NAME's are.
      slic = data + offset;
      slic_bytes = (size_t) bytes;
    } else if (id == kBlockMixr) {
      // **Optional, and it has to be**: the mixer's configuration is what a
      // replayer has no concept of, so a reader that skips it plays the tune
      // with the sends its host set up rather than not at all. The length is
      // exact -- a block that disagrees with the layout is refused rather than
      // read as far as it goes, the same rule FXPL and MACR are held to.
      // **Two lengths, one per version.** v2 added the per-channel send levels; a v1 block
      // is still read and means "nothing feeds the sends but the plane", which is what a
      // file written before them meant. Each length is still exact for its own version --
      // the rule is unchanged, there are simply two layouts rather than one.
      {
        const uint16_t v = read_u16(data + offset);
        const bool v1 = v == (uint16_t) kMixrVersion1 && bytes == (uint32_t) kMixrBytesV1;
        const bool v2 = v == (uint16_t) kMixrVersion && bytes == (uint32_t) kMixrBytes;
        if (!v1 && !v2)
          return false;
      }
      if (read_u16(data + offset + 2) != (uint16_t) kMixrSlots)
        return false;
      // **Upgraded to v2 in memory, whichever came in.** A v1 block is copied and the
      // level region zeroed -- nothing fed the sends -- then the version stamped, so every
      // accessor downstream reads one shape and none has to ask which version this is. The
      // writer decides the version again from the levels themselves, so a file that carries
      // none goes back out as v1 and is unchanged.
      for (int k = 0; k < kMixrBytes; ++k)
        module->mix[k] = k < (int) bytes ? data[offset + (uint32_t) k] : (uint8_t) 0u;
      write_u16(module->mix, (uint16_t) kMixrVersion);
      module->has_mix = true;
    } else if ((flags & kBlockCritical) != 0u) {
      // A block the writer said the file cannot be played without, and this
      // reader does not know it. Playing anyway would be playing something else.
      return false;
    }
    // ponytail: an unknown optional block is skipped, and lost on the next
    // save. Carrying one through wants a table on Module; add it when something
    // actually writes one.
  }

  // Payloads are bounds-checked but not checked for overlap. An overlapping
  // file is nonsense rather than unsafe: every byte read is inside the file,
  // and the commands in a plane are validated where they are used.

  // MACR. The payload is already known to be inside the file -- that was the
  // subtraction in the loop above, and it ran before this pointer was formed.
  if (macr != nullptr) {
    if (macr_bytes < (size_t) kMacroHeaderBytes)
      return false;
    const int count = (int) read_u16(macr + 0);
    if (count < 1 || count > kMaxMacros)
      return false;
    if (read_u16(macr + 2) != 0u)     // reserved, so it stays free to mean
      return false;                   // something later
    // Exact, like every other block: fixed-size records, so the length is one
    // multiply and a file whose count and payload disagree is malformed.
    if (macr_bytes != (size_t) kMacroHeaderBytes +
                          (size_t) count * (size_t) kMacroRecordBytes)
      return false;
    module->macro_count = count;

    for (int i = 0; i < count; ++i) {
      const uint8_t *r =
          macr + (size_t) kMacroHeaderBytes + (size_t) i * (size_t) kMacroRecordBytes;
      Macro &mac = module->macros[i];
      mac.target_count = r[0];
      mac.flags = r[1];
      if (mac.target_count > (uint8_t) kMaxMacroTargets)
        return false;
      if ((mac.flags & (uint8_t) ~kMacroDelta) != 0u)
        return false;
      if (read_u16(r + 2) != 0u)
        return false;

      for (int k = 0; k < kMaxMacroTargets; ++k) {
        const uint8_t *t =
            r + (size_t) kMacroTargetsAt + (size_t) k * (size_t) kMacroTargetBytes;
        // **A slot past the list must be all zero.** Otherwise two different
        // byte sequences encode the same song, and a format with a
        // canonicalisation hazard in it is a format whose fingerprints stop
        // meaning anything the day an editor leaves rubbish behind a shortened
        // list.
        if (k >= (int) mac.target_count) {
          for (int j = 0; j < kMacroTargetBytes; ++j)
            if (t[j] != 0u)
              return false;
          continue;
        }
        MacroTarget &mt = mac.targets[k];
        mt.target = t[0];
        mt.scope = t[1];
        mt.scale = read_i16(t + 2);
        mt.offset = read_i16(t + 4);
        // The target names either a mixer value or an effect-slot parameter,
        // so it is refused here rather than clamped at use: a macro is a
        // table, not a command, and there is nothing sensible for an
        // out-of-range entry to mean. See `macro_target_valid`.
        if (!macro_target_valid(mt.target))
          return false;
        // A scope naming a channel this module does not have is ignored at use
        // -- see `kMacroScopeSelf`. What is refused is a value that names
        // nothing at all.
        if (mt.scope > (uint8_t) 15 && mt.scope != kMacroScopeSelf &&
            mt.scope != kMacroScopeAll)
          return false;
      }
    }
  }

  // **A meta lane with no macro table names nothing, on every cell.** The same
  // shape of cross-block rule as SYNP's length against the instrument count: a
  // block whose meaning depends on another one is checked once both have been
  // read, and the two orders a directory may list them in do not matter.
  //
  // The converse is legal. A macro table with no meta lanes is a song whose
  // macros are written and not yet invoked, which is what an editor holds for
  // most of the time one is open.
  if (module->meta_columns > 0 && module->macro_count == 0)
    return false;

  for (int i = 0; i < module->instrument_count; ++i) {
    const uint8_t *e = instrument_table + (size_t) i * instrument_stride;
    Instrument &ins = module->instruments[i];
    const uint32_t offset = read_u32(e + 0);
    ins.length = read_u32(e + 4);
    ins.loop_start = read_u32(e + 8);
    ins.loop_len = read_u32(e + 12);
    ins.volume = e[16];
    ins.finetune = (int8_t) e[17];

    ins.type = e[18];
    ins.flags = e[19];
    ins.env_attack_ms = read_u16(e + 20);
    ins.env_decay_ms = read_u16(e + 22);
    ins.env_release_ms = read_u16(e + 24);
    ins.env_sustain = e[26];
    ins.transpose = (int8_t) e[27];
    ins.filter_cutoff_hz = read_u16(e + 28);
    ins.filter_res = e[30];
    ins.wave_index = e[31];

    if ((ins.flags & 0xf0u) != 0u)      // reserved, so they stay free to mean
      return false;                     // something in a later version
    // Every per-field bound, from the table `module_save` also walks. The
    // conditional ones come with it: a cutoff is checked only behind its bit, a
    // wave index only on a built-in shape, a loop only when there is one.
    if (!instrument_fields_valid(ins))
      return false;

    ins.bits = instrument_bits_for(ins.type);
    const size_t bps = (size_t) (ins.bits / 8u);

    // `length * bps` is a multiply that can wrap on a 32-bit size_t; the
    // division cannot, and for integers the two say the same thing.
    if ((size_t) offset > blob_bytes ||
        (size_t) ins.length > (blob_bytes - (size_t) offset) / bps)
      return false;

    // A loop of one frame is a denormal-rate buzz and, more to the point, a
    // trivial infinite loop in the mixer's advance. Treat it as one-shot.
    //
    // A coercion, not a bound, which is why it is here and not in the table:
    // the containment check -- a loop running off the end reads past the blob
    // on every repeat, for as long as the tune plays -- is `InsParam::kLoopLen`
    // and has already run above.
    if (ins.loop_len > 0 && ins.loop_len < 2)
      ins.loop_len = 0;
    ins.data = (const int8_t *) (const void *) (blob + offset);

    // A built-in shape carries nothing in the blob: whatever the entry claimed
    // has been bounds-checked above and is then replaced by the generated
    // cycle, which loops for as long as the note is held. `module_save` writes
    // this entry back with a zero offset and length, so the two agree.
    if (ins.type == (uint8_t) InstrumentType::kWaveBuiltin) {
      ins.data = builtin_wave((int) ins.wave_index);
      ins.length = (uint32_t) kBuiltinWaveFrames;
      ins.loop_start = 0;
      ins.loop_len = (uint32_t) kBuiltinWaveFrames;
    }
  }

  // TUNE. Exact length, like every block here.
  if (tune != nullptr) {
    if (tune_bytes != (size_t) kTuneBytes)
      return false;
    if (read_u16(tune + 6) != 0u)             // reserved
      return false;
    const int rpbeat = (int) read_u16(tune + 0);
    const int rpbar = (int) read_u16(tune + 2);
    const int sw = (int) read_u16(tune + 4);
    if (rpbeat < 1 || rpbeat > 255 || rpbar < 1 || rpbar > 255)
      return false;
    // **A bar is a whole number of beats.** Without this the beat band and the
    // bar band an editor draws cannot agree, and one of them is wrong on every
    // row where they cross.
    if (rpbar % rpbeat != 0)
      return false;
    if (sw > kSwingMax)
      return false;
    module->rows_per_beat = rpbeat;
    module->rows_per_bar = rpbar;
    module->swing = sw;
  }

  // SMUT. Exact length, like every block here, and judged against the geometry
  // the header already gave.
  if (smut != nullptr) {
    if (smut_bytes < (size_t) kSmutHeaderBytes)
      return false;
    const int so = (int) read_u16(smut + 0);
    const int sc = (int) read_u16(smut + 2);
    // **The counts must be the module's own.** A table sized for a different
    // order list would silence whichever tracks its bits happened to land on --
    // a wrong tune rather than a refused file, which is the failure every block
    // here is checked to prevent.
    if (so != module->order_count || sc != module->channels)
      return false;
    if (smut_bytes != (size_t) kSmutHeaderBytes + (size_t) so * 2u)
      return false;
    for (int i = 0; i < so; ++i) {
      const uint16_t bits = read_u16(smut + kSmutHeaderBytes + (size_t) i * 2u);
      // A bit above the channel count names a channel that is not there. It
      // would be harmless to ignore and is refused anyway: it means the writer
      // and this reader disagree about the geometry, and the next thing that
      // disagreement touches may not be harmless.
      if (sc < 16 && (bits >> sc) != 0u)
        return false;
      module->slot_mute[i] = bits;
      if (bits != 0u)
        module->has_slot_mute = true;
    }
  }

  // NAME, once the instrument table has said how many instruments there are.
  // **Exact, like every other block** -- a block that disagrees with the count
  // is refused rather than read as far as it goes. Absent is legal and is the
  // normal case: every instrument keeps the empty name it was born with.
  if (name != nullptr) {
    if (name_bytes != (size_t) module->instrument_count * (size_t) kNameBytes)
      return false;
    for (int i = 0; i < module->instrument_count; ++i) {
      const uint8_t *src = name + (size_t) i * (size_t) kNameBytes;
      for (int k = 0; k < kNameBytes; ++k)
        module->instruments[i].name[k] = (char) src[k];
      // The field is fixed-width and not NUL-terminated in the file; the extra
      // byte here is what makes it a C string.
      module->instruments[i].name[kNameBytes] = '\0';
    }
  }

  // SYNP, once the instrument table has said how many SYNTH instruments there
  // are. **Exact rather than sufficient, and in both directions**: a file that
  // names synths and carries no records is as malformed as one carrying records
  // for instruments that are not synths, and neither is something a reader gets
  // to paper over. A module with no synths carries no block, so this is 0 == 0.
  int synth_count = 0;
  for (int i = 0; i < module->instrument_count; ++i)
    if (module->instruments[i].type == (uint8_t) InstrumentType::kSynth)
      ++synth_count;
  if (synp_bytes != (size_t) synth_count * (size_t) kSynthParamBytes)
    return false;

  int record = 0;
  for (int i = 0; i < module->instrument_count; ++i) {
    Instrument &ins = module->instruments[i];
    if (ins.type != (uint8_t) InstrumentType::kSynth)
      continue;
    const uint8_t *r = synp + (size_t) record * (size_t) kSynthParamBytes;
    ++record;
    ins.synth_voice = r[0];
    ins.synth_tune = r[1];
    ins.synth_decay = r[2];
    ins.synth_sweep = r[3];
    ins.synth_tone = r[4];
    ins.synth_noise = r[5];
    ins.synth_noise_decay = r[6];
    ins.synth_drive = r[7];
    ins.synth_cutoff = r[8];
    ins.synth_reso = r[9];
    ins.synth_env_mod = r[10];
    ins.synth_accent = r[11];
    ins.synth_dist = r[12];
    ins.synth_dist_mix = r[13];
    ins.synth_wave = r[14];
    // The same walk again, now that the record is in: a voice this reader does
    // not have is a drum it would play as another one, which is the quiet kind
    // of wrong. The enumerated fields are checked whatever the voice, because a
    // value outside them is a file that has gone wrong rather than a setting
    // the bass happens not to be reading today -- which is why they are `Inert`
    // and not `Absent`, and why this walk still tests them.
    if (!instrument_fields_valid(ins))
      return false;
    for (int k = 15; k < kSynthParamBytes; ++k)  // reserved, so it stays free
      if (r[k] != 0u)                            // to mean something later
        return false;
  }

  // SLIC, once the instrument table has said how many instruments there are and
  // how long each sample is. Here rather than in the directory loop for the
  // same reason SYNP and NAME are: the offsets are judged against `length`, and
  // a directory may name the blocks in either order.
  //
  // Two refusals fall out of the general rules with no case of their own. A
  // SYNTH instrument has `length == 0`, so `offset >= length` refuses every
  // slice on it -- no type test needed. And **offset 0 need not be the first
  // boundary**, because a break may have a lead-in, so the first offset is
  // checked against `length` like every other and not against zero.
  if (slic != nullptr) {
    if (slic_bytes < (size_t) kSlicHeaderBytes)
      return false;
    const int table_count = (int) read_u16(slic + 0);
    if (table_count < 1 || table_count > kMaxInstruments)
      return false;
    if (read_u16(slic + 2) != 0u)     // reserved, so it stays free to mean
      return false;                   // something later

    // The records first, and the total they claim, so the block's exact length
    // is settled before a single offset is read.
    size_t total = 0;
    int previous = 0;                 // 1-based; 0 is "no record yet"
    for (int i = 0; i < table_count; ++i) {
      const size_t rec = (size_t) kSlicHeaderBytes +
                         (size_t) i * (size_t) kSlicRecordBytes;
      // The header plus the records has to be inside the block before the
      // records are read -- the length formula below cannot be trusted until
      // the numbers it is built from have themselves been read safely.
      if (slic_bytes < rec + (size_t) kSlicRecordBytes)
        return false;
      const int instrument = (int) slic[rec + 0];
      if (slic[rec + 1] != 0u)
        return false;
      const int count = (int) read_u16(slic + rec + 2);
      // **Strictly increasing**, which makes one-table-per-instrument free: a
      // repeat is a file that disagrees with itself about which table an
      // instrument has, and which one wins would be a reader's private
      // business rather than the format's.
      if (instrument <= previous || instrument > module->instrument_count)
        return false;
      previous = instrument;
      if (count < 1 || count > kMaxSlices)
        return false;
      total += (size_t) count;
    }

    if (slic_bytes != (size_t) kSlicHeaderBytes +
                          (size_t) table_count * (size_t) kSlicRecordBytes +
                          total * (size_t) kSlicOffsetBytes)
      return false;

    // Now the offsets, which start where the records end.
    size_t at_offset = (size_t) kSlicHeaderBytes +
                       (size_t) table_count * (size_t) kSlicRecordBytes;
    for (int i = 0; i < table_count; ++i) {
      const size_t rec = (size_t) kSlicHeaderBytes +
                         (size_t) i * (size_t) kSlicRecordBytes;
      const int instrument = (int) slic[rec + 0];
      const int count = (int) read_u16(slic + rec + 2);
      Instrument &ins = module->instruments[instrument - 1];

      uint32_t last = 0;
      bool first = true;
      for (int k = 0; k < count; ++k) {
        const uint32_t off = read_u32(slic + at_offset +
                                      (size_t) k * (size_t) kSlicOffsetBytes);
        // Inside the sample, which is what makes `slice_offset` safe to hand
        // straight to the mixer as a position.
        if (off >= ins.length)
          return false;
        // Strictly increasing, so a table is an ordered chop rather than a bag
        // of positions -- and so an editor cannot write two boundaries at one
        // frame, which would be two slices one of which is empty.
        if (!first && off <= last)
          return false;
        last = off;
        first = false;
      }
      ins.slices = slic + at_offset;
      ins.slice_count = (uint16_t) count;
      at_offset += (size_t) count * (size_t) kSlicOffsetBytes;
    }
  }

  return true;
}

/// The frame a slice starts at. **The one reader of a boundary**, because the
/// bytes are unaligned and little-endian wherever the block happened to land.
///
/// Out of range is 0 rather than an error: every caller is asking "where does
/// this slice start", and the answer for a slice that does not exist is the top
/// of the sample -- which is what `SLC` with a bad index does, audibly.
inline uint32_t
slice_offset(const Instrument &ins, int index) {
  if (ins.slices == nullptr || index < 0 || index >= (int) ins.slice_count)
    return 0u;
  return read_u32(ins.slices + (size_t) index * (size_t) kSlicOffsetBytes);
}

// The mirror of module_load: the same blocks, in the same order, refusing
// exactly what the loader refuses -- so a file this writes is a file that
// loads, and `tests` checks that as a round trip rather than by reading both.
//
// **This is for an editor, and it is deliberately not on the replay path.** A
// player never writes. It lives beside the loader anyway, because a format
// whose reader and writer are in two files is a format whose halves drift.
//
// Nothing here allocates, so the caller sizes the buffer: call once with
// `out == nullptr` to be told how many bytes are needed, then again with that
// much. `written` is set on both paths.
//
// The sample blob is rebuilt from what the instruments point at, wherever that
// is -- which is what lets an editor keep its samples in its own arrays rather
// than in a loaded file. Two instruments sharing one buffer are written twice;
// deduplicating is not worth a pass for a format this size.
inline bool
module_save(const Module *module, uint8_t *out, size_t cap, size_t *written) {
  if (module == nullptr || written == nullptr)
    return false;
  *written = 0;

  if (module->version != 2)
    return false;

  // **The same table the loader walks**, so a file this writes is a file that
  // loads. These were the loader's ranges written out a second time, and the
  // comment that used to sit here said as much.
  if (!module_fields_valid(module))
    return false;
  // A relation rather than a range, so it stays here and at the loader's TUNE
  // parse: a bar that is not a whole number of beats is two fields disagreeing,
  // which no per-field bound can see.
  if (module->rows_per_bar % module->rows_per_beat != 0)
    return false;
  if (module->order == nullptr || module->patterns == nullptr)
    return false;
  if (module->note_max != 36 && module->note_max != kMaxNote)
    return false;

  for (int i = 0; i < module->order_count; ++i)
    if ((int) module->order[i] >= module->pattern_count)
      return false;

  // The plane's geometry, and the macro table, judged exactly as the loader
  // judges them -- so a file this writes is a file that loads, which is the
  // whole contract between these two functions.
  // A geometry with no plane to describe has nowhere to be written down: the
  // counts live in the FXPL payload's prefix, so a module carrying columns and
  // no plane would save and load back as something else.
  if (module->fx == nullptr &&
      (module->fx_columns != 1 || module->meta_columns != 0))
    return false;
  if (module->macro_count < 0 || module->macro_count > kMaxMacros)
    return false;
  if (module->meta_columns > 0 && module->macro_count == 0)
    return false;
  for (int i = 0; i < module->macro_count; ++i) {
    const Macro &mac = module->macros[i];
    if (mac.target_count > (uint8_t) kMaxMacroTargets)
      return false;
    if ((mac.flags & (uint8_t) ~kMacroDelta) != 0u)
      return false;
    for (int k = 0; k < (int) mac.target_count; ++k) {
      const MacroTarget &mt = mac.targets[k];
      if (!macro_target_valid(mt.target))
        return false;
      if (mt.scope > (uint8_t) 15 && mt.scope != kMacroScopeSelf &&
          mt.scope != kMacroScopeAll)
        return false;
    }
  }

  const size_t cells = (size_t) module->pattern_count *
                       (size_t) module->rows * (size_t) module->channels;

  // Note-off is above `note_max` by construction and is not an octave, so it is
  // the one value the range check has to let past. Refusing it would make a
  // tune that uses `^^^` unsaveable.
  for (size_t i = 0; i < cells; ++i) {
    const int note = (int) module->patterns[i].note;
    if (note > module->note_max && note != kNoteOff)
      return false;
  }

  size_t blob_bytes = 0;
  int synth_count = 0;
  for (int i = 0; i < module->instrument_count; ++i) {
    const Instrument &ins = module->instruments[i];
    // Structural, and only this end has the context for it: a length with
    // nothing behind it is a writer bug, not a file that has gone wrong.
    if (ins.length > 0 && ins.data == nullptr)
      return false;
    if ((ins.flags & 0xf0u) != 0u)   // reserved, exactly as the loader has it
      return false;
    // **The same table the loader walks, so a file this writes is a file that
    // loads.** These were the loader's checks written out a second time, and a
    // writer and a reader that disagree about a range produce a save nobody can
    // open -- which is what happened to `transpose`.
    if (!instrument_fields_valid(ins))
      return false;

    if (ins.type == (uint8_t) InstrumentType::kSynth) {
      // A synth's sample fields are written and read back like anything else --
      // a SYNTH instrument names no blob in practice, but nothing here forces
      // `length` to zero, and carrying what is there costs one entry.
      ++synth_count;
    }
    if (ins.type == (uint8_t) InstrumentType::kWaveBuiltin)
      continue;         // generated at load, so it stores nothing at all
    const bool wide = ins.type == (uint8_t) InstrumentType::kPcm16;
    blob_bytes += (size_t) ins.length * (size_t) (wide ? 2u : 1u);
  }

  // The two fields the loader will check on the way back in. Everything else in
  // the block is the mixer's to validate; these two are the format's, and a
  // writer that emits a block its own reader refuses has written a file nobody
  // can open.
  if (module->has_mix &&
      (read_u16(module->mix) != (uint16_t) kMixrVersion ||
       read_u16(module->mix + 2) != (uint16_t) kMixrSlots))
    return false;

  const size_t order_bytes = (size_t) module->order_count;
  const size_t instrument_bytes =
      (size_t) module->instrument_count * (size_t) kInstrumentBytes;
  const size_t pattern_bytes = cells * 4u;
  const bool want_fx = module->fx != nullptr;
  const bool want_synp = synth_count > 0;
  const bool want_macr = module->macro_count > 0;
  const bool want_mixr = module->has_mix;

  // SLIC. **Every load-side invariant again**, plus one the loader gets for
  // free: a `slice_count` with no `slices` pointer is a module that cannot be
  // written, because there is nothing to write.
  //
  // Not checked, deliberately: whether any cell actually uses `SLC`. Which
  // instrument an `SLC` applies to depends on the channel's RUNNING instrument
  // -- a note with an empty instrument column inherits the last one -- so the
  // writer would have to replay the order list to know. A rule the writer
  // cannot evaluate is not a rule. A table with no cells using it is an
  // editor's normal working state, exactly as a macro table with no meta lanes
  // is.
  int slic_tables = 0;
  size_t slic_offsets = 0;
  for (int i = 0; i < module->instrument_count; ++i) {
    const Instrument &ins = module->instruments[i];
    if (ins.slice_count == 0)
      continue;
    if (ins.slices == nullptr)
      return false;
    if ((int) ins.slice_count > kMaxSlices)
      return false;
    uint32_t last = 0;
    for (int k = 0; k < (int) ins.slice_count; ++k) {
      const uint32_t off = slice_offset(ins, k);
      if (off >= ins.length)
        return false;
      if (k > 0 && off <= last)
        return false;
      last = off;
    }
    ++slic_tables;
    slic_offsets += (size_t) ins.slice_count;
  }
  const bool want_slic = slic_tables > 0;

  // A NAME block is written only where a name is actually set. All-empty names
  // are what a module without one already has, so writing 22 zero bytes per
  // instrument would grow every file to say nothing.
  // Written only where it says something the default does not.
  const bool want_tune = module->rows_per_beat != 4 ||
                         module->rows_per_bar != 16 || module->swing != 0;

  bool want_name = false;
  for (int i = 0; i < module->instrument_count && !want_name; ++i)
    if (module->instruments[i].name[0] != '\0')
      want_name = true;

  // Written only where a bit is actually set -- see the block's own comment for
  // why that matters more here than for the others: an empty SMUT would make
  // every file refuse to open in a reader that does not know the id, to say
  // that nothing is muted.
  bool want_smut = false;
  for (int i = 0; i < module->order_count && !want_smut; ++i)
    if (module->slot_mute[i] != 0u)
      want_smut = true;

  const int block_count = (want_fx ? 1 : 0) + (want_tune ? 1 : 0) +
                          (want_name ? 1 : 0) + (want_synp ? 1 : 0) +
                          (want_macr ? 1 : 0) + (want_mixr ? 1 : 0) +
                          (want_slic ? 1 : 0) + (want_smut ? 1 : 0);
  const size_t directory_bytes =
      (size_t) block_count * (size_t) kDirectoryEntryBytes;
  // Lanes rather than channels, and the four-byte prefix that says how many.
  const size_t plane_cells = (size_t) module->pattern_count *
                             (size_t) module->rows *
                             (size_t) module_lanes(module);
  const size_t fx_bytes =
      want_fx ? (size_t) kFxplPrefixBytes + plane_cells * 2u : 0u;
  const size_t tune_bytes = want_tune ? (size_t) kTuneBytes : 0u;
  const size_t name_bytes =
      want_name ? (size_t) module->instrument_count * (size_t) kNameBytes : 0u;
  const size_t synp_bytes =
      want_synp ? (size_t) synth_count * (size_t) kSynthParamBytes : 0u;
  const size_t macr_bytes =
      want_macr ? (size_t) kMacroHeaderBytes +
                      (size_t) module->macro_count * (size_t) kMacroRecordBytes
                : 0u;
  // **The narrowest block that says what this module means**, which is the policy TUNE and
  // SMUT already follow: a module that feeds no send carries nothing v1 could not express,
  // so it is written as v1 and a file that never used the feature round-trips byte for byte.
  // `module->mix` is always v2 in memory (the loader upgrades one), so this is a decision
  // about the FILE and not about the state.
  bool mixr_v2 = false;
  if (want_mixr)
    for (int k = 0; k < kMixrSendLevelBytes && !mixr_v2; ++k)
      if (module->mix[kMixrBytesV1 + k] != 0u)
        mixr_v2 = true;
  const size_t mixr_bytes =
      want_mixr ? (size_t) (mixr_v2 ? kMixrBytes : kMixrBytesV1) : 0u;
  const size_t slic_bytes =
      want_slic ? (size_t) kSlicHeaderBytes +
                      (size_t) slic_tables * (size_t) kSlicRecordBytes +
                      slic_offsets * (size_t) kSlicOffsetBytes
                : 0u;
  const size_t smut_bytes =
      want_smut ? (size_t) kSmutHeaderBytes + (size_t) module->order_count * 2u
                : 0u;
  const size_t total = (size_t) kHeaderBytes + order_bytes + instrument_bytes +
                       pattern_bytes + blob_bytes + directory_bytes + fx_bytes +
                       tune_bytes + name_bytes + synp_bytes + macr_bytes +
                       mixr_bytes + slic_bytes + smut_bytes;

  *written = total;
  if (out == nullptr)
    return true;
  if (cap < total)
    return false;

  for (size_t i = 0; i < (size_t) kHeaderBytes; ++i)
    out[i] = 0;
  out[0] = 'N'; out[1] = 'T'; out[2] = 'R'; out[3] = 'K';
  write_u16(out + 4, (uint16_t) module->version);
  write_u16(out + 6, (uint16_t) module->channels);
  write_u16(out + 8, (uint16_t) module->rows);
  write_u16(out + 10, (uint16_t) module->speed);
  write_u16(out + 12, (uint16_t) module->bpm);
  write_u16(out + 14, (uint16_t) module->order_count);
  write_u16(out + 16, (uint16_t) module->pattern_count);
  write_u16(out + 18, (uint16_t) module->instrument_count);
  write_u16(out + 20, (uint16_t) module->restart);
  write_u32(out + 22, (uint32_t) blob_bytes);
  write_u16(out + 26, (uint16_t) block_count);
  out[28] = (uint8_t) module->note_max;
  // Bytes 29..31 are the header's own flags and reserved word, and the zero
  // fill above has already written them.

  size_t at = (size_t) kHeaderBytes;
  for (int i = 0; i < module->order_count; ++i)
    out[at++] = module->order[i];

  const size_t entry_bytes = (size_t) kInstrumentBytes;
  uint32_t offset = 0;
  for (int i = 0; i < module->instrument_count; ++i) {
    const Instrument &ins = module->instruments[i];
    const bool builtin = ins.type == (uint8_t) InstrumentType::kWaveBuiltin;
    uint8_t *e = out + at;
    for (size_t k = 0; k < entry_bytes; ++k)
      e[k] = 0;
    // A built-in shape is regenerated on load, so its entry names no blob at
    // all: writing the cycle's own length here would be a sample the file does
    // not contain.
    if (!builtin) {
      write_u32(e + 0, offset);
      write_u32(e + 4, ins.length);
      write_u32(e + 8, ins.loop_start);
      write_u32(e + 12, ins.loop_len);
      const bool wide = ins.type == (uint8_t) InstrumentType::kPcm16;
      offset += ins.length * (uint32_t) (wide ? 2u : 1u);
    }
    e[16] = ins.volume;
    e[17] = (uint8_t) ins.finetune;
    e[18] = ins.type;
    e[19] = ins.flags;
    write_u16(e + 20, ins.env_attack_ms);
    write_u16(e + 22, ins.env_decay_ms);
    write_u16(e + 24, ins.env_release_ms);
    e[26] = ins.env_sustain;
    e[27] = (uint8_t) ins.transpose;
    write_u16(e + 28, ins.filter_cutoff_hz);
    e[30] = ins.filter_res;
    e[31] = ins.wave_index;
    at += entry_bytes;
  }

  // Byte at a time rather than a memcpy of the struct: the loader casts the
  // block to `Note *` and that is only sound because every field is a uint8_t.
  // Writing the four fields by name is what keeps the two halves agreeing if
  // that ever stops being true -- and the static_assert catches the rest.
  for (size_t i = 0; i < cells; ++i) {
    const Note &n = module->patterns[i];
    out[at + 0] = n.note;
    out[at + 1] = n.instrument;
    out[at + 2] = n.effect;
    out[at + 3] = n.param;
    at += 4;
  }

  // Byte at a time here too, and out of a `uint8_t *` view: a PCM16 frame is
  // two little-endian bytes wherever the host puts its own, and copying them as
  // bytes is the only version of this that does not care. For 8 bits it is the
  // loop that was always here.
  for (int i = 0; i < module->instrument_count; ++i) {
    const Instrument &ins = module->instruments[i];
    if (ins.type == (uint8_t) InstrumentType::kWaveBuiltin)
      continue;
    const uint8_t *d = (const uint8_t *) (const void *) ins.data;
    const bool wide = ins.type == (uint8_t) InstrumentType::kPcm16;
    const size_t n = (size_t) ins.length * (size_t) (wide ? 2u : 1u);
    for (size_t k = 0; k < n; ++k)
      out[at++] = d[k];
  }

  // The directory, then its payloads, in the same order as each other. FXPL is
  // written optional rather than critical: a reader that does not know the
  // plane still plays the tune, which is what the flag is for. SYNP is written
  // *critical*, and the difference is the point of the flag -- a reader that
  // skips it plays a SYNTH instrument with no parameters, which is silence
  // where a drum was, not a tune with one embellishment missing.
  size_t entry = at;
  at += directory_bytes;

  if (want_fx) {
    write_u16(out + entry + 0, kBlockFxpl);
    write_u16(out + entry + 2, 0u);
    write_u32(out + entry + 4, (uint32_t) at);
    write_u32(out + entry + 8, (uint32_t) fx_bytes);
    entry += (size_t) kDirectoryEntryBytes;

    // The geometry prefix, then `lanes` cells a row. The header is full, so
    // this is where the two counts live.
    write_u16(out + at + 0, (uint16_t) module->fx_columns);
    write_u16(out + at + 2, (uint16_t) module->meta_columns);
    at += (size_t) kFxplPrefixBytes;

    for (size_t i = 0; i < plane_cells; ++i) {
      out[at + 0] = module->fx[i].cmd;
      out[at + 1] = module->fx[i].param;
      at += 2;
    }
  }

  if (want_tune) {
    // **Critical exactly when the swing is non-zero**, which is the rule every
    // block here follows: a block is critical when its contents change what is
    // heard. A division of the grid is drawn and not played, so a reader that
    // skips it renders the tune correctly; a swing figure is played, so a
    // reader that skips one would play the tune straight and sound wrong with
    // nothing to point at.
    write_u16(out + entry + 0, kBlockTune);
    write_u16(out + entry + 2,
              module->swing != 0 ? (uint16_t) kBlockCritical : (uint16_t) 0u);
    write_u32(out + entry + 4, (uint32_t) at);
    write_u32(out + entry + 8, (uint32_t) tune_bytes);
    entry += (size_t) kDirectoryEntryBytes;

    write_u16(out + at + 0, (uint16_t) module->rows_per_beat);
    write_u16(out + at + 2, (uint16_t) module->rows_per_bar);
    write_u16(out + at + 4, (uint16_t) module->swing);
    write_u16(out + at + 6, 0u);
    at += (size_t) kTuneBytes;
  }

  if (want_smut) {
    // **Critical, always** -- unlike TUNE, which is critical only when its
    // swing is non-zero. There is no such condition here: the block is written
    // only when a bit is set, and a set bit is by definition something that
    // changes what is heard.
    write_u16(out + entry + 0, kBlockSmut);
    write_u16(out + entry + 2, (uint16_t) kBlockCritical);
    write_u32(out + entry + 4, (uint32_t) at);
    write_u32(out + entry + 8, (uint32_t) smut_bytes);
    entry += (size_t) kDirectoryEntryBytes;

    write_u16(out + at + 0, (uint16_t) module->order_count);
    write_u16(out + at + 2, (uint16_t) module->channels);
    at += (size_t) kSmutHeaderBytes;
    for (int i = 0; i < module->order_count; ++i) {
      write_u16(out + at, module->slot_mute[i]);
      at += 2;
    }
  }

  if (want_name) {
    // **Not critical, and that is the whole point of the id.** A reader that
    // does not know NAME skips it and plays the tune correctly, because a name
    // changes nothing that is heard.
    write_u16(out + entry + 0, kBlockName);
    write_u16(out + entry + 2, 0u);
    write_u32(out + entry + 4, (uint32_t) at);
    write_u32(out + entry + 8, (uint32_t) name_bytes);
    entry += (size_t) kDirectoryEntryBytes;

    // One fixed-width record per instrument, in instrument order -- which is
    // the only thing binding a name to an instrument, so the loop that reads
    // them and this one have to walk the table the same way. The in-memory
    // field carries a NUL the file's does not, so only the first kNameBytes go
    // out and a short name is zero-padded to the width.
    for (int i = 0; i < module->instrument_count; ++i) {
      const char *nm = module->instruments[i].name;
      for (int k = 0; k < kNameBytes; ++k)
        out[at + (size_t) k] = (uint8_t) nm[k];
      at += (size_t) kNameBytes;
    }
  }

  if (want_synp) {
    write_u16(out + entry + 0, kBlockSynp);
    write_u16(out + entry + 2, kBlockCritical);
    write_u32(out + entry + 4, (uint32_t) at);
    write_u32(out + entry + 8, (uint32_t) synp_bytes);
    entry += (size_t) kDirectoryEntryBytes;

    // One record per SYNTH instrument, in instrument order -- which is the only
    // thing that binds a record to an instrument, so the loop that reads them
    // and this one have to walk the table the same way.
    for (int i = 0; i < module->instrument_count; ++i) {
      const Instrument &ins = module->instruments[i];
      if (ins.type != (uint8_t) InstrumentType::kSynth)
        continue;
      out[at + 0] = ins.synth_voice;
      out[at + 1] = ins.synth_tune;
      out[at + 2] = ins.synth_decay;
      out[at + 3] = ins.synth_sweep;
      out[at + 4] = ins.synth_tone;
      out[at + 5] = ins.synth_noise;
      out[at + 6] = ins.synth_noise_decay;
      out[at + 7] = ins.synth_drive;
      out[at + 8] = ins.synth_cutoff;
      out[at + 9] = ins.synth_reso;
      out[at + 10] = ins.synth_env_mod;
      out[at + 11] = ins.synth_accent;
      out[at + 12] = ins.synth_dist;
      out[at + 13] = ins.synth_dist_mix;
      out[at + 14] = ins.synth_wave;
      for (int k = 15; k < kSynthParamBytes; ++k)    // reserved, and zeroed
        out[at + (size_t) k] = 0u;                   // rather than left over
      at += (size_t) kSynthParamBytes;
    }
  }

  // SLIC, written **critical**. A reader that predates `SLC` falls through its
  // switch and plays every slice from the top: a chop that sounds wrong with
  // nothing to point at. The block's own contents are what decide where a note
  // starts, so skipping it changes what is heard -- which is this format's one
  // test for the flag.
  if (want_slic) {
    write_u16(out + entry + 0, kBlockSlic);
    write_u16(out + entry + 2, kBlockCritical);
    write_u32(out + entry + 4, (uint32_t) at);
    write_u32(out + entry + 8, (uint32_t) slic_bytes);
    entry += (size_t) kDirectoryEntryBytes;

    write_u16(out + at + 0, (uint16_t) slic_tables);
    write_u16(out + at + 2, 0u);                 // reserved, and zeroed
    size_t rec = at + (size_t) kSlicHeaderBytes;
    size_t off = rec + (size_t) slic_tables * (size_t) kSlicRecordBytes;

    // **One walk of the instrument table, in order**, which is what makes the
    // ids strictly increasing without sorting anything -- and is the same
    // argument that binds a SYNP record or a NAME to its instrument.
    for (int i = 0; i < module->instrument_count; ++i) {
      const Instrument &ins = module->instruments[i];
      if (ins.slice_count == 0)
        continue;
      out[rec + 0] = (uint8_t) (i + 1);          // 1-based, like a Note's
      out[rec + 1] = 0u;                         // reserved, and zeroed
      write_u16(out + rec + 2, ins.slice_count);
      rec += (size_t) kSlicRecordBytes;
      for (int k = 0; k < (int) ins.slice_count; ++k) {
        write_u32(out + off, slice_offset(ins, k));
        off += (size_t) kSlicOffsetBytes;
      }
    }
    at += slic_bytes;
  }

  // MACR, written **critical**: a meta cell whose macro table a reader skipped
  // is a cell that names nothing, and a tune playing on with every macro
  // silently absent is not the tune. The unused target slots are zeroed rather
  // than left as whatever the module holds -- the loader refuses a dirty slot,
  // and two byte sequences for one song is a canonicalisation hazard the
  // fingerprints would pay for.
  if (want_macr) {
    write_u16(out + entry + 0, kBlockMacr);
    write_u16(out + entry + 2, kBlockCritical);
    write_u32(out + entry + 4, (uint32_t) at);
    write_u32(out + entry + 8, (uint32_t) macr_bytes);
    entry += (size_t) kDirectoryEntryBytes;

    write_u16(out + at + 0, (uint16_t) module->macro_count);
    write_u16(out + at + 2, 0u);                 // reserved, and zeroed
    at += (size_t) kMacroHeaderBytes;

    for (int i = 0; i < module->macro_count; ++i) {
      const Macro &mac = module->macros[i];
      uint8_t *r = out + at;
      for (int k = 0; k < kMacroRecordBytes; ++k)
        r[k] = 0u;
      r[0] = mac.target_count;
      r[1] = mac.flags;
      for (int k = 0; k < (int) mac.target_count; ++k) {
        uint8_t *t = r + (size_t) kMacroTargetsAt +
                     (size_t) k * (size_t) kMacroTargetBytes;
        t[0] = mac.targets[k].target;
        t[1] = mac.targets[k].scope;
        write_i16(t + 2, mac.targets[k].scale);
        write_i16(t + 4, mac.targets[k].offset);
      }
      at += (size_t) kMacroRecordBytes;
    }
  }

  // MIXR, written **optional**: a reader with no mixer plays the tune, which is
  // exactly what the flag is for. The payload is copied through verbatim --
  // `ntrk_mix` is what encodes it and the format's own layer never learns what
  // a send is -- so the two fields the loader will check on the way back in are
  // checked here too. A writer that emits a block its own reader refuses has
  // written a file nobody can open, and the caller would find out at the next
  // load rather than at the save that caused it.
  if (want_mixr) {
    write_u16(out + entry + 0, kBlockMixr);
    write_u16(out + entry + 2, 0u);
    write_u32(out + entry + 4, (uint32_t) at);
    write_u32(out + entry + 8, (uint32_t) mixr_bytes);
    entry += (size_t) kDirectoryEntryBytes;

    for (size_t k = 0; k < mixr_bytes; ++k)
      out[at + k] = module->mix[k];
    // The version field says which layout the LENGTH is, so it is stamped here rather than
    // taken from memory — where it is always v2.
    write_u16(out + at, (uint16_t) (mixr_v2 ? kMixrVersion : kMixrVersion1));
    at += mixr_bytes;
  }

  return at == total;
}

// ----------------------------------------------------------------------------
// -- Playing
// ----------------------------------------------------------------------------

// A panning byte as the pattern effects write it: 0x00 hard left, 0x80 centre,
// 0xFF hard right.
//
// **The divisor is 127 rather than 128, so 0xFF reaches hard right exactly**;
// that leaves 0x00 a hair beyond hard left, which the clamp takes. Losing the
// endpoint would be the worse trade -- a tune asking for hard right is asking
// for the edge, and 127/128 of the way there is a channel that never quite
// arrives.
//
// This is `fxpl_to_pan` in the mixer, spelled again because `ntrk.h` is the
// standalone replayer and cannot reach the mixer. The two writers of
// `player->pan` have to agree on what a byte means, so if one moves, move both.
inline float
pan_from_byte(int v) {
  const float p = ((float) v - 128.f) * (1.f / 127.f);
  return p < -1.f ? -1.f : (p > 1.f ? 1.f : p);
}

// Point a player at a module and play it from the top.
//
// **The module must outlive the player, and its geometry must not change while
// the player is running.** `rows`, `channels`, `order_count` and
// `pattern_count` are what index the pattern block, and the player carries a
// position into it between calls — so shrinking any of them, or repointing
// `order`/`patterns` at something smaller, moves the block out from under a
// position that was legal when it was set. Editing *cells* is safe, and so is
// anything that leaves the counts alone; only the geometry is dangerous. A
// caller that reloads or resizes a module stops the transport first and starts
// again afterwards, which is the whole of the contract.
//
// `player_tick` clamps its position against the module it is actually holding
// rather than trusting it, so a caller that breaks the rule gets wrong notes
// instead of a read outside the pattern block. That is a backstop, not a
// licence: the notes it plays are meaningless.
inline void
player_start(Player *player, const Module *module) {
  // **Mutes are the editor's state, not the tune's**, so pressing play must not
  // silently unmute what somebody muted. Everything else is reset; this is the
  // one field that survives, and it survives player_seek for the same reason.
  bool muted[kMaxChannels];
  for (int i = 0; i < kMaxChannels; ++i)
    muted[i] = player->muted[i];

  *player = Player();

  for (int i = 0; i < kMaxChannels; ++i)
    player->muted[i] = muted[i];

  player->module = module;

  // **A refused module is a silent player, not a crash.** `module_load` clears
  // the struct before it validates anything, so a caller that ignores its
  // result hands us a `Module` with null pattern and order pointers — and the
  // first render would dereference them on the audio thread. The load path is
  // careful never to read past a bad file; this is the other half of that, and
  // the half that costs one line.
  if (module == nullptr || module->order == nullptr ||
      module->patterns == nullptr || module->channels < 1 ||
      module->rows < 1 || module->order_count < 1)
    return;

  // LRRL, repeating every four: the Amiga's channel arrangement, and the
  // reason these tunes place their parts the way they do.
  for (int i = 0; i < kMaxChannels; ++i) {
    const int slot = i & 3;
    player->pan[i] = (slot == 0 || slot == 3) ? -1.f : 1.f;
  }

  player->speed = module->speed;
  player->bpm = module->bpm;
  player->playing = true;
  // Zero, so the first tick is processed by the first frame rendered rather
  // than a tick's worth of silence later.
  player->until_tick = 0.0;
}

// Set the host's loop over an inclusive range of order entries, or clear it with
// any negative argument. Refuses a range the module does not have rather than
// clamping: a caller that asked for one it has not got has a bug, and a clamped
// loop would hide it behind a section that plays the wrong bars.
inline bool
player_loop_range(Player *player, int first, int last) {
  if (player == nullptr || player->module == nullptr)
    return false;
  if (first < 0 || last < 0) {
    player->loop_first = -1;
    player->loop_last = -1;
    return true;
  }
  if (first > last || last >= player->module->order_count)
    return false;
  player->loop_first = first;
  player->loop_last = last;
  return true;
}

inline void
player_stop(Player *player) {
  player->playing = false;
  for (int i = 0; i < kMaxChannels; ++i)
    player->channels[i].playing = false;
}

// Jump to a position without playing up to it. An editor needs this for every
// "play from this row" there is; nothing on the replay path calls it.
//
// **Speed and tempo go back to the module's own, and that is a real
// limitation rather than an oversight.** They are things a tune *sets* as it
// plays, with Fxx, so where they stand at any row is a function of every row
// before it. Landing on a row cannot know them without playing the tune up to
// there, which is exactly what seeking is avoiding. Same for the effect memory
// each channel carries. A tune that changes tempo mid-way will therefore play
// its later patterns at the wrong speed until the next Fxx, and an editor that
// wants better has to render from the start and throw the audio away.
inline bool
player_seek(Player *player, int order, int row) {
  if (player == nullptr)
    return false;
  const Module *m = player->module;
  if (m == nullptr)
    return false;
  if (order < 0 || order >= m->order_count)
    return false;
  if (row < 0 || row >= m->rows)
    return false;

  player->order = order;
  player->row = row;
  player->tick = 0;
  player->until_tick = 0.0;
  player->break_row = -1;
  player->jump_order = -1;
  player->extra_ticks = 0;
  player->loop_row = 0;
  player->loop_count = 0;
  player->speed = m->speed;
  player->bpm = m->bpm;
  player->playing = true;
  player->sequencing = true;

  // Every channel back to nothing sounding: what they were playing belongs to
  // the row we just left, not the one we are landing on.
  for (int i = 0; i < kMaxChannels; ++i)
    player->channels[i] = Channel();

  return true;
}

// Ticks per second is `bpm * 2 / 5` — the tracker convention, where 125 BPM is
// the 50 Hz of a PAL vertical blank. Everything about a module's timing is that
// one line.
// How many frames may be rendered before the sequencer must tick again, which
// is `ceil(until_tick)` -- a frame is rendered while `until_tick > 0`, and each
// one costs 1.0, so the tick fires after exactly that many.
//
// Spelled out rather than calling `ceil`, because this header includes
// <stddef.h> and <stdint.h> and nothing else. Pulling in libm for one rounding
// would cost the standalone property the README makes a point of.
inline int
frames_until_tick(double until_tick) {
  if (until_tick <= 0.0)
    return 1;
  int n = (int) until_tick;          // positive, so a truncation is a floor
  if ((double) n < until_tick)
    ++n;
  return n < 1 ? 1 : n;
}

inline double
frames_per_tick(const Player *player, double sample_rate) {
  const double bpm = player->bpm > 0 ? (double) player->bpm : 125.0;
  return sample_rate * 2.5 / bpm;
}

// Equal temperament, computed, anchored on C-1's historical period.
//
// **The five entries that made ProTracker's table not derivable are exactly the
// five that were out of tune.** Scaling octave 3 down disagreed with the table
// on 762 against 760, 678 against 680, 570 against 572, 538 against 540 and 453
// against 452 — D, E, G, G# and B — which was written up here as a reason the
// table had to stay authoritative. It is the same fact seen from the other side:
// the table disagrees with *itself* between octaves, by up to 6.4 cents, and
// since everything above it was derived by halving from its top octave that
// error repeated for ever. See kSemitoneRatio for the whole argument.
//
// Nothing is rounded any more either. Rounding to an integer period was
// ProTracker's, and it is worth up to 15 cents at the top of the old table,
// where one unit is most of a semitone.
inline double
period_for(int note, int finetune) {
  // Notes below 1 are real: a period is only a step size here, so the scale
  // runs downward as far as `kMinNote` and an instrument transpose can reach it.
  int n = note - 1;

  int octave = 0;
  while (n >= 12) {
    n -= 12;
    ++octave;
  }
  while (n < 0) {
    n += 12;
    --octave;
  }

  double period = kPeriodC1 * kSemitoneRatio[n];
  if (octave > 0) {
    for (int i = 0; i < octave; ++i)
      period *= 0.5;
  } else {
    for (int i = 0; i < -octave; ++i)
      period *= 2.0;
  }

  return period * kFinetune[(finetune & 15)];
}

inline void
channel_set_step(Channel *ch, double sample_rate) {
  if (ch->period <= 0.0) {
    ch->step = 0.0;
    return;
  }
  const double rate = kAmigaClock / (2.0 * ch->period);
  ch->step = rate / sample_rate;
}

// Clamped to the module's own range rather than to ProTracker's two literals.
// Outside it a period is either inaudible or fast enough to walk a sample in a
// handful of frames, and modules do rely on the clamp — a porta that runs into
// the end of the range is expected to stop there rather than to keep climbing.
//
// **The bounds come from `note_max`, and for a three-octave module the formula
// gives exactly 113 and 856.** Taking them from the size of the period table
// instead would silently extend such a tune's porta range the day the table
// grew.
inline double
clamp_period(double period, int note_max) {
  const double low = period_for(note_max, 0);
  const double high = period_for(kMinNote, 0);
  if (period < low)
    return low;
  if (period > high)
    return high;
  return period;
}

// The shape vibrato and tremolo trace, as a magnitude 0..255. The sign comes
// from which half of the 64-step position we are in and is the caller's.
//
// Four of them, because E4x and E7x select between them and a module that asks
// for a square vibrato and gets a sine is a module playing the wrong thing.
// The random one is a per-channel generator rather than a shared table so it
// stays deterministic: the same module rendered twice must sound the same, and
// a global would couple the channels to each other's call order.
inline int
wave_amplitude(Channel *ch, uint8_t waveform, uint8_t pos) {
  const int i = (int) (pos & 31u);
  switch (waveform & 3u) {
    case 1:
      return i * 8;        // a ramp
    case 2:
      return 255;          // a square
    case 3: {
      ch->rng = ch->rng * 1664525u + 1013904223u;
      return (int) ((ch->rng >> 16) & 255u);
    }
    default:
      return (int) kSine[i];
  }
}

// Snap a period to the nearest note in the table, which is what glissando does
// to a tone portamento: the slide still takes the same time, it just arrives in
// steps rather than through the pitches between them.
inline double
snap_to_note(double period, int finetune, int note_max) {
  double best = period;
  double best_distance = -1.0;
  // From the floor rather than from note 1, or a glissando on a channel a
  // transpose has taken below it would snap two octaves upward.
  for (int i = kMinNote; i <= note_max; ++i) {
    const double candidate = period_for(i, finetune);
    const double distance = candidate > period ? candidate - period
                                               : period - candidate;
    if (best_distance < 0.0 || distance < best_distance) {
      best_distance = distance;
      best = candidate;
    }
  }
  return best;
}

// Tone portamento's glide: `base_period` walks toward `target_period` at so
// many *period units* a tick. Linear in period, which is lopsided in pitch —
// and correct, because it is what ProTracker does and a MOD written against it
// is written against that lopsidedness. Effect 0x3 and 0x5 only; the plane's
// slide has its own walk below and does not touch this one.
inline void
period_glide(Channel *ch, double speed, int note_max) {
  if (ch->target_period <= 0)
    return;
  if (ch->base_period < ch->target_period) {
    ch->base_period += speed;
    if (ch->base_period > ch->target_period)
      ch->base_period = ch->target_period;
  } else if (ch->base_period > ch->target_period) {
    ch->base_period -= speed;
    if (ch->base_period < ch->target_period)
      ch->base_period = ch->target_period;
  }
  ch->period = clamp_period(
      ch->glissando ? snap_to_note(ch->base_period, ch->finetune, note_max)
                    : ch->base_period,
      note_max);
}

// The plane's slide (FXPL 0x31), which is a 303's glide and not a MOD's.
//
// **A one-pole toward the target frequency, not a linear walk in period.**
// Period is the reciprocal of frequency, so a straight period ramp is not a
// straight anything in pitch: the same interval covered upward and downward
// takes two audibly different paths, fast at one end and crawling at the
// other. A 303's slide is an RC charging the pitch control voltage, which is
// exponential in frequency — and `f += (target - f) * k` is exponential by
// construction, so it needs no exponential to compute. Symmetric in direction,
// because a ratio is.
//
// `slide_k` is the fraction of the remaining interval closed each tick, set at
// the row from the glide time the plane asked for; see `player_row`.
//
// `arrive` is the last tick of the glide. A one-pole is asymptotic and never
// quite lands, and a note left a couple of cents flat for as long as it sounds
// is a tuning bug rather than a glide — so the final tick sets the destination
// outright. The step it takes is a thousandth of the interval, by construction
// of `slide_k`, which is nothing anyone can hear.
inline void
slide_glide(Channel *ch, int note_max, bool arrive) {
  if (ch->target_period <= 0.0 || ch->base_period <= 0.0)
    return;
  if (arrive) {
    ch->base_period = ch->target_period;
  } else {
    const double f = 1.0 / ch->base_period;
    const double target_f = 1.0 / ch->target_period;
    const double next = f + (target_f - f) * ch->slide_k;
    ch->base_period = next > 0.0 ? 1.0 / next : ch->target_period;
  }
  ch->period = clamp_period(
      ch->glissando ? snap_to_note(ch->base_period, ch->finetune, note_max)
                    : ch->base_period,
      note_max);
}

inline void
volume_slide(Channel *ch, uint8_t param) {
  const int up = (param >> 4) & 15;
  const int down = param & 15;
  // Up wins when both nibbles are set, which is what ProTracker does; a module
  // that sets both is relying on whichever its player picked.
  float v = ch->volume + (float) (up > 0 ? up : -down);
  if (v < 0.f)
    v = 0.f;
  if (v > 64.f)
    v = 64.f;
  ch->volume = v;
}

// Move an envelope to a stage, computing the ramp it runs at.
//
// **Every zero-length stage is taken here rather than divided by**, which is the
// one place a NaN could come from: a zero attack falls straight through to the
// decay at full level, a zero decay to the sustain, and a zero release to
// silence. Falling through rather than stepping means an instrument with no
// attack sounds at full on its *first* frame, so there is no frame of silence at
// the front of every note to hear as a click.
//
// The sustain level is read from the instrument each time rather than stored:
// it is `env_sustain / 64`, on the same 0..64 scale as every other volume here.
inline void
env_enter(Channel *ch, const Instrument &ins, EnvStage stage) {
  const float sustain = (float) ins.env_sustain * (1.f / 64.f);

  if (stage == EnvStage::kAttack) {
    if (ins.env_attack_ms > 0u) {
      ch->env_stage = EnvStage::kAttack;
      ch->env_step =
          (1.f - ch->env_level) / ((float) ins.env_attack_ms * ch->env_fpms);
      return;
    }
    ch->env_level = 1.f;
    stage = EnvStage::kDecay;
  }

  if (stage == EnvStage::kDecay) {
    if (ins.env_decay_ms > 0u && ch->env_level > sustain) {
      ch->env_stage = EnvStage::kDecay;
      ch->env_step =
          (ch->env_level - sustain) / ((float) ins.env_decay_ms * ch->env_fpms);
      return;
    }
    ch->env_level = sustain;
    stage = EnvStage::kSustain;
  }

  if (stage == EnvStage::kSustain) {
    ch->env_stage = EnvStage::kSustain;
    ch->env_step = 0.f;
    return;
  }

  // Release, and it runs from wherever the level stands to zero in
  // `env_release_ms` — not from full. A note let go during its attack has less
  // distance to cover and must not take the whole release time to cover it.
  if (ins.env_release_ms > 0u && ch->env_level > 0.f) {
    ch->env_stage = EnvStage::kRelease;
    ch->env_step = ch->env_level / ((float) ins.env_release_ms * ch->env_fpms);
    return;
  }
  ch->env_level = 0.f;
  ch->env_stage = EnvStage::kRelease;
  ch->playing = false;
}

// One frame of envelope. **The level the mixer used is the one from before this
// runs**, so a note starts from silence rather than a frame into its attack.
//
// A finished release stays in `EnvStage::kRelease` at zero rather than going
// back to `EnvStage::kOff`: off means "this instrument has no envelope", and a
// voice that E9x-retriggered into it would come back at full volume with
// nothing to bring it down again.
inline void
env_advance(Channel *ch, const Instrument &ins) {
  switch (ch->env_stage) {
    case EnvStage::kAttack:
      ch->env_level += ch->env_step;
      if (ch->env_level >= 1.f) {
        ch->env_level = 1.f;
        env_enter(ch, ins, EnvStage::kDecay);
      }
      break;
    case EnvStage::kDecay: {
      const float sustain = (float) ins.env_sustain * (1.f / 64.f);
      ch->env_level -= ch->env_step;
      if (ch->env_level <= sustain) {
        ch->env_level = sustain;
        ch->env_stage = EnvStage::kSustain;
        ch->env_step = 0.f;
      }
      break;
    }
    case EnvStage::kRelease:
      ch->env_level -= ch->env_step;
      if (ch->env_level <= 0.f) {
        ch->env_level = 0.f;
        ch->playing = false;
      }
      break;
    default:      // sustain holds, and off is not an envelope at all
      break;
  }
}

// A note let go. The release, or the cut that a channel with no envelope has
// always had — and the cut is also what an envelope-less instrument's `^^^`
// means to a tracker.
inline void
channel_note_off(const Module *m, Channel *ch) {
  if (ch->instrument > 0) {
    const Instrument &ins = m->instruments[ch->instrument - 1];
    // Both halves: the stage alone would be stale if a row set a new instrument
    // without a note, and the flag alone would fire on a channel that has never
    // sounded.
    if ((ins.flags & kInstrumentEnvelope) != 0u &&
        ch->env_stage != EnvStage::kOff) {
      env_enter(ch, ins, EnvStage::kRelease);
      return;
    }
  }
  ch->playing = false;
}

// A cell's note shifted by its instrument's `transpose`, clamped into range.
//
// **The clamp is not tidiness.** `period_for` reads a semitone ratio at `note - 1`
// and says outright that its caller has already range-checked; `transpose` is
// file data. The loader bounds it to -48..48 and the note to `note_max`, and
// neither bound says anything about their sum -- -48 on a low note indexes
// behind the table.
//
// Only the two places that turn a *cell's* note into a pitch call this. The
// arpeggio and glissando searches pass table indices to `period_for`, and
// transposing those would apply the shift twice: `base_period` already carries
// it, which is exactly what those searches are searching against.
inline int
note_transposed(const Module *m, const Channel *ch, int note) {
  if (ch->instrument <= 0)
    return note;
  const int shifted = note + (int) m->instruments[ch->instrument - 1].transpose;
  // The floor is `kMinNote` rather than 1: a transpose is how a tune reaches
  // the two octaves under the format's own lowest note, which is where a bass
  // line lives and where the note byte cannot go.
  if (shifted < kMinNote)
    return kMinNote;
  return shifted > m->note_max ? m->note_max : shifted;
}

// Start a note on a channel. Split out because EDx plays the same note some
// ticks later, and a delayed note that took a different path to sounding would
// be a second implementation to keep in step with this one.
inline void
channel_trigger(const Module *m, Channel *ch, int note, double start_frame,
                double sample_rate) {
  const double period =
      period_for(note_transposed(m, ch, note), ch->finetune);
  ch->base_period = period;
  ch->period = period;
  // **One frame count, not an effect's parameter.** This is the only writer of
  // the starting position and the `pos >= length` check sits right under it;
  // taking a frame keeps both facts here, where taking a slice INDEX would need
  // a second position computation and a second copy of that check.
  ch->pos = start_frame;

  // Bit 2 of the waveform selector means the wave keeps running across a new
  // note instead of restarting, which is the difference between a vibrato that
  // pulses with the notes and one that rides over them.
  if ((ch->vib_wave & 4u) == 0u)
    ch->vib_pos = 0;
  if ((ch->trem_wave & 4u) == 0u)
    ch->trem_pos = 0;

  ch->playing = ch->instrument > 0;

  if (ch->instrument > 0) {
    const Instrument &ins = m->instruments[ch->instrument - 1];
    if (ins.type == (uint8_t) InstrumentType::kSynth) {
      // **A synth walks no sample, so the length test below does not apply to
      // it** — and a SYNTH instrument's length is legitimately zero, which
      // would silence every drum in the file if it did.
      synth_trigger(ch, ins, sample_rate);
    } else if (ch->pos >= (double) ins.length) {
      // An offset past the end is a note that does not sound, not a read off
      // the end of the buffer.
      ch->playing = false;
    }
  }

  // **Every new note starts its envelope from zero**, and every path to a note
  // comes through here — the row, EDx's delayed one, and an editor's preview —
  // which is why this is the only place it is set up.
  ch->env_stage = EnvStage::kOff;
  ch->env_level = 0.f;
  ch->env_step = 0.f;
  if (ch->instrument > 0) {
    const Instrument &ins = m->instruments[ch->instrument - 1];
    if ((ins.flags & kInstrumentEnvelope) != 0u) {
      ch->env_fpms = (float) (sample_rate / 1000.0);
      env_enter(ch, ins, EnvStage::kAttack);
    }
  }

  channel_set_step(ch, sample_rate);
}

// Sound one note now, on one channel, without the sequencer touching it.
// Clicking an instrument in an editor and hearing it is this function, and it
// is the only reason it exists -- nothing on the replay path calls it.
//
// **Works against a stopped tune**, which is the whole point: if the player is
// not running it is switched on with `sequencing` false, so the channels mix
// but no rows advance. Previewing while the tune *is* playing leaves it
// playing, and the next row on that channel takes it back.
inline bool
player_preview(Player *player, int channel, int note, int instrument,
               double sample_rate) {
  if (player == nullptr)
    return false;
  const Module *m = player->module;
  if (m == nullptr)
    return false;
  if (channel < 0 || channel >= m->channels)
    return false;
  if (note < 1 || note > m->note_max)
    return false;
  if (instrument < 1 || instrument > m->instrument_count)
    return false;

  if (!player->playing) {
    player->playing = true;
    player->sequencing = false;
  }

  const Instrument &ins = m->instruments[instrument - 1];
  Channel *ch = &player->channels[channel];
  ch->instrument = instrument;
  ch->finetune = ins.finetune;
  ch->volume = (float) ins.volume;
  ch->trem_offset = 0.f;
  // An audition is one note on its own, so it inherits neither the accent nor
  // the glide of whatever row last played on this channel.
  ch->accent = 0.f;
  ch->slide_ticks = 0;

  channel_trigger(m, ch, note, 0.0, sample_rate);
  return true;
}

// The player's half of the FXPL plane. **The plane is read by whichever module
// owns the target, split by command range**: the mixer takes 0x01..0x17 — pan,
// sends, master gain, the voice filter — and the player takes 0x30..0x3F,
// because slide and accent are voice *generation* rather than mixing. Two
// consequences, and both are the reason for the split rather than a side effect
// of it: a caller using `render_add` with no mixer at all still gets these, and
// the player is the sequencer, so it has the row and tick timing a glide needs
// natively instead of counting or inferring them. The ranges are disjoint, so
// both reading one plane is not an ownership conflict.
const uint8_t kFxplAccent = 0x30;   // 0..255, this note's accent
const uint8_t kFxplSlide = 0x31;    // glide time in ticks, 0 off

// The pattern an order entry names, which is the second half of the geometry
// backstop `player_tick` starts. `module_load` refuses a file whose order names
// a pattern past `pattern_count`, so the fallback here is reachable only for a
// module whose counts were changed under a running player — see player_start.
inline int
player_pattern(const Module *m, int order) {
  const int pattern = (int) m->order[order];
  return pattern < m->pattern_count ? pattern : 0;
}

// One row, on tick zero: the notes start and the effects that fire once are
// applied. Everything that continues per tick is set up here and run below.
inline void
player_row(Player *player, double sample_rate) {
  const Module *m = player->module;
  const int pattern = player_pattern(m, player->order);
  const size_t at =
      ((size_t) pattern * (size_t) m->rows + (size_t) player->row) *
      (size_t) m->channels;
  const Note *row = m->patterns + at;

  // The same row of the second plane, when the file carries one. Read here,
  // where the row's cells are already being read: a second traversal of the
  // pattern would be a second place for the two to disagree about which row is
  // playing.
  //
  // **The plane's row is `lanes` wide, not `channels` wide** -- a channel may
  // own several effect columns and the meta lanes follow all of them, so this
  // row offset is not the pattern's.
  const size_t plane_row =
      ((size_t) pattern * (size_t) m->rows + (size_t) player->row) *
      (size_t) module_lanes(m);
  const FxCell *plane = m->fx != nullptr ? m->fx + plane_row : nullptr;

  for (int c = 0; c < m->channels; ++c) {
    Channel *ch = &player->channels[c];
    const Note &n = row[c];
    const int effect = n.effect;
    const uint8_t param = n.param;

    // **Every one of this channel's effect columns, and the rule here is not
    // the mixer's.** The mixer *sums* its half of the plane, because a mixer
    // command is a per-tick delta on a continuous value and two columns sliding
    // a pan opposite ways ought to cancel. The player's half is not a level at
    // all: accent and slide are articulations of one note, so
    //
    //   - accent and slide in different columns both apply -- different
    //     targets, no interaction;
    //   - two of the *same* command on one row is last column wins, because a
    //     note glides to one destination and summing two glide times means
    //     nothing.
    //
    // Written down in both halves because a reader who knows one rule will read
    // the other as a bug. The mixer's half is on `FxplCmd` in ntrk_mix.h.
    //
    // No presence flag is needed for either: a parameter of zero already means
    // "not accented" and "no glide", so an explicit `30 00` in a later column
    // cancels an earlier one by carrying the same value absence does.
    uint8_t fx_accent = 0u;
    uint8_t fx_slide = 0u;
    if (plane != nullptr) {
      const FxCell *cols = plane + (size_t) c * (size_t) m->fx_columns;
      for (int col = 0; col < m->fx_columns; ++col) {
        if (cols[col].cmd == kFxplAccent)
          fx_accent = cols[col].param;
        else if (cols[col].cmd == kFxplSlide)
          fx_slide = cols[col].param;
      }
    }

    // Tone portamento is the exception to everything below: it uses the note as
    // a destination and must not restart the sample, which is the whole point
    // of it. Checked before the note is acted on rather than after.
    const bool tone_porta = (effect == 0x3 || effect == 0x5);

    // Cleared every row, all of it: these last exactly as long as the effect
    // that asked for them, and a row without one must not inherit the last
    // row's. `retrig` especially — it is written only by E9x, so leaving it set
    // makes one retrigger machine-gun that channel for the rest of the tune.
    ch->trem_offset = 0.f;
    ch->cut_pending = false;
    ch->delay_pending = false;
    ch->retrig = 0;

    // **The glide is note-scoped, not row-scoped**, which is the one effect
    // here that is. A slide time is a property of the note being slid to, and
    // at six ticks to a row a row only has five of them — so a glide cancelled
    // at the bar line would stop wherever it had got to and leave the note
    // permanently a few cents flat, which is a tuning bug rather than a short
    // slide. It runs to its end across rows that say nothing about the pitch,
    // and is dropped by any row that does: a note owns the pitch, and a tone
    // portamento owns `target_period`, so neither may find a glide already
    // walking it.
    if (n.note > 0 || tone_porta)
      ch->slide_ticks = 0;

    // And the pitch goes back to what the row asked for. Vibrato and arpeggio
    // write `period` every tick and leave it wherever the last tick of the row
    // happened to land; without this the channel stays detuned for every row
    // after — and the next portamento copies the drift into `base_period` and
    // makes it permanent. ProTracker reloads the period each row; so does this.
    ch->period = ch->base_period;

    // Read before the row can change it, because a tie is only a tie within
    // one instrument -- see below.
    const int prev_instrument = ch->instrument;

    if (n.instrument > 0 && n.instrument <= m->instrument_count) {
      ch->instrument = n.instrument;
      ch->volume = (float) m->instruments[n.instrument - 1].volume;
      ch->finetune = m->instruments[n.instrument - 1].finetune;
    }

    // E5x overrides the instrument's finetune, and has to be read before the
    // note is turned into a period rather than after.
    if (effect == 0xE && (param >> 4) == 0x5) {
      const int8_t tune = (int8_t) (param & 15);
      ch->finetune = (int8_t) (tune > 7 ? tune - 16 : tune);
    }

    const bool delayed = (effect == 0xE && (param >> 4) == 0xD);

    // Where the pitch stands *before* the note is acted on, which is where a
    // glide has to start from. Zero means this channel has never sounded, and
    // there is nothing to glide from.
    const double from = ch->base_period;

    // **A slide ties the sounding note; it does not strike a new one.** That is
    // the whole of what a 303's slide is, and getting it wrong is most of why
    // a slid line does not sound like one: the envelope has to *continue* while
    // the pitch moves, or every step of the slide is audibly a fresh note with
    // a fresh attack. So the note here is a destination and nothing else — no
    // trigger, no envelope, no phase reset.
    //
    // Refused unless there is something to tie: a channel that is not sounding
    // has no note to extend, and a row that changes the instrument is asking
    // for a different voice rather than for more of this one. Both fall back to
    // an ordinary strike, which is what the reference does when a slide arrives
    // with no note active. Refused on a tone porta row too, so `target_period`
    // keeps one owner and one thing walks the period a tick.
    //
    // A voice in release is refused for the same reason as one that is silent:
    // its gate is shut, so there is no note to extend and the reference strikes.
    // Tying one instead glides a fade-out — the tied note peaks five times
    // quieter than the same note struck, and says nothing about why.
    const bool tie = fx_slide > 0u && !tone_porta && !delayed && ch->playing &&
                     ch->env_stage != EnvStage::kRelease &&
                     from > 0.0 && n.note > 0 && (int) n.note <= m->note_max &&
                     (n.instrument == 0 ||
                      (int) n.instrument == prev_instrument);

    // **Per note**: the accent belongs to the note it is written on, so a row
    // carrying no note leaves a still-sounding one at the level it was struck
    // with rather than dropping it back to unaccented halfway through.
    if (n.note > 0)
      ch->accent = (float) fx_accent * (1.f / 255.f);

    // Note-off first, because it is out of the note range by construction and
    // the test below would drop it as a stray byte rather than act on it.
    if ((int) n.note == kNoteOff) {
      channel_note_off(m, ch);
    } else if (n.note > 0 && (int) n.note <= m->note_max) {
      if (tone_porta || tie) {
        ch->target_period =
            period_for(note_transposed(m, ch, (int) n.note), ch->finetune);
      } else if (delayed) {
        // Held back until tick x. The instrument has already been taken above,
        // which is what a tracker does — the note waits, not the whole cell.
        ch->delay_pending = true;
        ch->delay_tick = (uint8_t) (param & 15);
        ch->delay_note = n.note;
        ch->playing = false;
      } else {
        // Where the note starts, in frames. Three sources and they cannot
        // collide: a `Note` carries ONE effect byte and this reads one cell per
        // channel, so `9xx` and `SLC` on the same row of the same channel is
        // not a case -- there is no precedence rule here because there is
        // nothing to order.
        double start = 0.0;
        if (effect == 0x9) {
          // 9xx remembers its parameter like every other effect here: `900`
          // repeats the last offset rather than restarting at zero, which is
          // the idiom a sample chopped by offset depends on. The memory has to
          // be read here rather than in the switch below, because the switch
          // runs after the note has already started.
          if (param != 0)
            ch->offset = param;
          start = (double) ((int) ch->offset * 256);
        } else if (effect == kFxSlice && ch->instrument > 0) {
          // **No memory, so `SLC 00` is slice 0.** Every memory effect here
          // uses `param != 0` as its sentinel, which costs the command the
          // value zero -- and slice 0 is the downbeat, the most-used index in a
          // chopped break. `9xx` needs memory because its parameter means
          // nothing on its own; `SLC` indexes a table the file carries, so the
          // TABLE is the memory. `ch->offset` is deliberately untouched, so a
          // later `900` still repeats the last `9xx` offset.
          //
          // An index past the count -- or no table at all, which is the same
          // thing with `slice_count == 0` -- yields 0 and the note plays from
          // the top. Not `9xx`'s silence: that is a memory-safety rule about a
          // position past the buffer, and a bad index never produces a position
          // at all. This fails audibly rather than inaudibly.
          start = (double) slice_offset(m->instruments[ch->instrument - 1],
                                        (int) param);
        }
        channel_trigger(m, ch, n.note, start, sample_rate);
      }
    }

    switch (effect) {
      case 0x0:   // arpeggio, or nothing at all when the parameter is zero
        ch->arpeggio = param;
        break;
      case 0x1:   // portamento up
      case 0x2:   // portamento down
        if (param != 0)
          ch->porta_speed = param;
        break;
      case 0x3:   // tone portamento
        if (param != 0)
          ch->tone_speed = param;
        break;
      case 0x4:   // vibrato
        if ((param >> 4) != 0)
          ch->vib_speed = (uint8_t) (param >> 4);
        if ((param & 15) != 0)
          ch->vib_depth = (uint8_t) (param & 15);
        break;
      case 0x7:   // tremolo, vibrato's twin on the volume
        if ((param >> 4) != 0)
          ch->trem_speed = (uint8_t) (param >> 4);
        if ((param & 15) != 0)
          ch->trem_depth = (uint8_t) (param & 15);
        break;
      case 0x5:   // tone portamento + volume slide
      case 0x6:   // vibrato + volume slide
      case 0xA:   // volume slide
        if (param != 0)
          ch->volslide = param;
        break;
      case 0x9:   // sample offset
        if (param != 0)
          ch->offset = param;
        break;
      case 0xB:   // position jump
        player->jump_order = (int) param;
        break;
      case 0xC:   // set volume
        ch->volume = (float) (param > 64 ? 64 : param);
        break;
      case 0xD:   // pattern break, and the parameter is decimal on purpose
        player->break_row = (int) ((param >> 4) * 10 + (param & 15));
        break;
      case 0xF:
        // Below 32 it is ticks per row; at 32 and above it is beats per minute.
        // One effect doing two jobs is ProTracker's, not ours, and a module
        // that sets 0x1F expects the first meaning.
        if (param == 0)
          break;
        if (param < 32)
          player->speed = (int) param;
        else
          player->bpm = (int) param;
        break;
      case 0xE: {
        // The extended set: the high nibble picks the effect and the low one is
        // its whole parameter, so every one of these has four bits to work
        // with. The slides here are *fine* — they happen once, on this tick,
        // rather than on every tick of the row like their full-sized versions.
        const int which = param >> 4;
        const int value = param & 15;
        switch (which) {
          case 0x1:   // fine portamento up
            ch->period = clamp_period(ch->period - value, m->note_max);
            ch->base_period = ch->period;
            break;
          case 0x2:   // fine portamento down
            ch->period = clamp_period(ch->period + value, m->note_max);
            ch->base_period = ch->period;
            break;
          case 0x3:   // glissando on or off
            ch->glissando = value != 0;
            break;
          case 0x4:   // vibrato waveform
            ch->vib_wave = (uint8_t) value;
            break;
          case 0x6:   // pattern loop
            if (value == 0) {
              player->loop_row = player->row;
            } else if (player->loop_count == 0) {
              player->loop_count = value;
              player->break_row = player->loop_row;
              player->jump_order = player->order;
            } else if (--player->loop_count > 0) {
              player->break_row = player->loop_row;
              player->jump_order = player->order;
            }
            break;
          case 0x7:   // tremolo waveform
            ch->trem_wave = (uint8_t) value;
            break;
          case 0x8:   // coarse panning
            // The nibble expanded over the whole byte by 17, so E80 and E8F
            // land exactly on the hard positions 8xx reaches. The consequence
            // is that no nibble is exactly centre -- E87 and E88 straddle it --
            // which is what every tracker that reads this command does, and
            // preferable to keeping centre and losing an edge.
            player->pan[c] = pan_from_byte(value * 17);
            break;
          case 0x9:   // retrigger every n ticks
            ch->retrig = (uint8_t) value;
            break;
          case 0xA:   // fine volume slide up
            volume_slide(ch, (uint8_t) (value << 4));
            break;
          case 0xB:   // fine volume slide down
            volume_slide(ch, (uint8_t) value);
            break;
          case 0xC:   // note cut on tick n
            ch->cut_pending = true;
            ch->cut_tick = (uint8_t) value;
            // A cut at tick zero is silent immediately rather than after a
            // tick, which is what makes EC0 usable as a staccato.
            if (value == 0)
              ch->volume = 0.f;
            break;
          case 0xD:   // note delay, handled at the trigger above
            break;
          case 0x5:   // finetune, already read before the note
            break;
          default:
            // E0 filter, E8 unused, EE pattern delay (below), EF invert loop.
            break;
        }
        // Pattern delay is the one extended effect that is the player's rather
        // than the channel's, so it is applied outside the switch: it holds the
        // whole row, not this voice.
        if (which == 0xE)
          player->extra_ticks = value * player->speed;
        break;
      }
      case 0x8:
        // Set panning, 0x00 hard left through 0x80 centre to 0xFF hard right.
        //
        // **Written into `player->pan`, which the second effect plane also
        // writes.** Both are row-level, so the rule is the ordinary one: the
        // last writer in a row wins, and the plane runs after this. The one
        // seam is that a plane *slide* resumes from the plane's own
        // accumulator rather than from a pan this command set in between --
        // documented rather than reconciled, because a tune driving one
        // channel's pan from both places at once is asking two things.
        player->pan[c] = pan_from_byte(param);
        break;
      default:
        break;
    }

    // The plane's slide, last, so it has the period the row finally settled on
    // to aim at. **Any voice with a pitch**, not only the 303: it walks
    // `period`, which is what everything pitched here is driven by.
    //
    // `fx_slide` is the glide *time*, in ticks. Turning it into a one-pole
    // coefficient: closing `1 - exp(-6.9/N)` of what is left each tick leaves a
    // thousandth of the interval after N of them, so the pitch has arrived by
    // ear in the time the row asked for, and the last tick closes the rest
    // exactly. **`expm1` and not `exp`**, because at long glide times the
    // coefficient is a small number obtained by subtracting two near-ones, and
    // in float that is most of its significant digits gone.
    if (tie) {
      const float ticks = (float) fx_slide;
      ch->slide_k = (double) -fx::ntrk303_expm1(-6.9f / ticks);
      ch->slide_ticks = (int) fx_slide;
    }

    channel_set_step(ch, sample_rate);
  }
}

// Everything that moves between rows. Runs on ticks 1..speed-1; tick zero is
// the row above, and ProTracker deliberately does not slide on it.
inline void
player_effects(Player *player, double sample_rate) {
  const Module *m = player->module;
  // Hoisted: the row does not move inside this loop, and reading the order
  // entry once per channel was the same byte fetched eight times.
  const Note *row = m->patterns + ((size_t) player_pattern(m, player->order) *
                                       (size_t) m->rows +
                                   (size_t) player->row) *
                                      (size_t) m->channels;
  for (int c = 0; c < m->channels; ++c) {
    Channel *ch = &player->channels[c];
    const Note &n = row[c];

    // The three "on tick n" effects, which are not a slide and do not belong in
    // the switch: they are the same shape as each other and none of them cares
    // what the row's main effect is.
    if (ch->delay_pending && player->tick >= (int) ch->delay_tick) {
      ch->delay_pending = false;
      channel_trigger(m, ch, (int) ch->delay_note, 0.0, sample_rate);
    }
    if (ch->cut_pending && player->tick >= (int) ch->cut_tick) {
      ch->cut_pending = false;
      ch->volume = 0.f;
    }
    if (ch->retrig > 0 && (player->tick % (int) ch->retrig) == 0) {
      ch->pos = 0.0;
      ch->playing = ch->instrument > 0;
    }

    // The plane's glide, before the switch and outside it: it is nobody's
    // ProTracker effect, so a row may carry one beside a volume slide or a
    // tremolo. It cannot double up with the tone portamento below — `player_row`
    // refuses to start one on a tone porta row.
    if (ch->slide_ticks > 0) {
      --ch->slide_ticks;
      slide_glide(ch, m->note_max, ch->slide_ticks == 0);
    }

    switch (n.effect) {
      case 0x0: {
        if (ch->arpeggio == 0)
          break;
        // Three-tick cycle: the note, then two offsets in semitones. Done
        // through the period table rather than by multiplying the period,
        // because that is what makes it land on the notes rather than near
        // them.
        const int which = player->tick % 3;
        int semitones = 0;
        if (which == 1)
          semitones = (ch->arpeggio >> 4) & 15;
        else if (which == 2)
          semitones = ch->arpeggio & 15;
        // Find where the base period sits and step from there. `period_for`
        // rather than a table directly, because the notes an arpeggio can
        // walk on to now run past the end of the table.
        // From `kMinNote`, not from note 1: a channel a transpose has taken
        // below the format's own floor would otherwise root its arpeggio two
        // octaves above where it is actually playing. `snap_to_note` had the
        // same bug and was fixed when the floor moved; this is its sibling.
        int note = kMinNote;
        while (note < m->note_max && period_for(note, 0) > ch->base_period)
          ++note;
        int target = note + semitones;
        // **The ceiling is the module's, never the note range's.** An arpeggio
        // in a three-octave module saturated at note 36 has to go on saturating
        // there, and it would quietly climb five more octaves the day the
        // range grew.
        if (target > m->note_max)
          target = m->note_max;
        // `ch->finetune`, not the instrument's: E5x overrides it per channel,
        // and every other pitch path here already reads the override. Taking
        // the instrument's puts the arpeggio's outer notes an eighth of a
        // semitone away from its own root.
        ch->period = period_for(target, ch->finetune);
        break;
      }
      case 0x1:
        ch->period = clamp_period(ch->period - (int) ch->porta_speed,
                                  m->note_max);
        ch->base_period = ch->period;
        break;
      case 0x2:
        ch->period = clamp_period(ch->period + (int) ch->porta_speed,
                                  m->note_max);
        ch->base_period = ch->period;
        break;
      case 0x3:
      case 0x5: {
        if (n.effect == 0x5)
          volume_slide(ch, ch->volslide);
        period_glide(ch, (double) (int) ch->tone_speed, m->note_max);
        break;
      }
      case 0x4:
      case 0x6: {
        if (n.effect == 0x6)
          volume_slide(ch, ch->volslide);
        const int depth =
            wave_amplitude(ch, ch->vib_wave, ch->vib_pos) * (int) ch->vib_depth;
        const int delta = depth / 128;
        ch->period = clamp_period(ch->vib_pos < 32 ? ch->base_period + delta
                                                  : ch->base_period - delta,
                                  m->note_max);
        // Six bits of position, so the wave runs 0..63 and the second half is
        // the negative one — the table only holds the first.
        ch->vib_pos = (uint8_t) ((ch->vib_pos + ch->vib_speed) & 63);
        break;
      }
      case 0x7: {
        // Tremolo. The same wave against the volume, and left in `trem_offset`
        // rather than folded into `volume`, or every tick would compound the
        // last one and the note would walk away from the level it was set to.
        const int depth = wave_amplitude(ch, ch->trem_wave, ch->trem_pos) *
                          (int) ch->trem_depth;
        const int delta = depth / 64;
        ch->trem_offset = (float) (ch->trem_pos < 32 ? delta : -delta);
        ch->trem_pos = (uint8_t) ((ch->trem_pos + ch->trem_speed) & 63);
        break;
      }
      case 0xA:
        volume_slide(ch, ch->volslide);
        break;
      default:
        break;
    }
    channel_set_step(ch, sample_rate);
  }
}

// Advance the row and, when the pattern runs out, the order. Jumps written by
// this row are applied here, after it has played, so a break and a jump on the
// same row combine the way a tracker's do.
inline void
player_advance(Player *player) {
  const Module *m = player->module;

  if (player->jump_order >= 0 || player->break_row >= 0) {
    const int next_order =
        player->jump_order >= 0 ? player->jump_order : player->order + 1;
    const int next_row = player->break_row >= 0 ? player->break_row : 0;
    player->jump_order = -1;
    player->break_row = -1;
    player->order = next_order;
    player->row = next_row >= m->rows ? 0 : next_row;
  } else {
    player->row++;
    if (player->row >= m->rows) {
      player->row = 0;
      player->order++;
    }
  }

  // **The host's loop, and it wins over everything above.** A `Bxx` that jumps
  // out of the section, a `Dxx` break at its last row, and the ordinary walk off
  // the end all land here, and a section a jump could escape is not a section.
  // Checked before the end-of-song wrap so a range ending on the last entry
  // loops rather than restarting the tune.
  if (player->loop_last >= 0 &&
      (player->order > player->loop_last || player->order < player->loop_first)) {
    player->order = player->loop_first;
    player->row = 0;
  }

  if (player->order >= m->order_count) {
    if (!player->loop) {
      player_stop(player);
      return;
    }
    player->order = m->restart;
    player->row = 0;
  }
}

inline void
player_tick(Player *player, double sample_rate) {
  // First, and before every early return below: a held tick is still a tick.
  player->ticks_elapsed++;

  // **The position is re-checked against the module actually held.** Nothing
  // else between rows does, and a caller that shrank the module's geometry
  // under us — see player_start's contract — leaves a position that indexes
  // outside the pattern block, which is a read of memory we do not own rather
  // than a wrong note. `module_load` validates every count and every order
  // entry, and `player_advance` keeps the position inside them, so for a module
  // nobody mutated neither branch below can fire: this moves no audio.
  const Module *m = player->module;
  if (m->order_count < 1 || m->pattern_count < 1 || m->rows < 1 ||
      m->channels < 1) {
    // Nothing left to play. Stopping is the only honest answer — every index
    // into the block is out of range, including zero.
    player_stop(player);
    return;
  }
  if (player->order < 0 || player->order >= m->order_count)
    player->order = player->order < 0 ? 0 : m->order_count - 1;
  if (player->row < 0 || player->row >= m->rows)
    player->row = player->row < 0 ? 0 : m->rows - 1;

  if (player->tick == 0)
    player_row(player, sample_rate);
  else
    player_effects(player, sample_rate);

  player->tick++;

  // EEx holds the row by extending it, so the effects on it keep running and
  // the notes are not struck again. Spending the extra ticks here rather than
  // repeating the row is what gets that: a repeat would call `player_row` and
  // retrigger everything, which is a stutter and not a delay.
  if (player->tick >= player->speed && player->extra_ticks > 0) {
    player->extra_ticks--;
    player->tick = player->speed - 1;
    return;
  }

  if (player->tick >= player->speed) {
    player->tick = 0;
    player_advance(player);
  }
}

// One frame of an instrument, on the 8-bit scale the mixer's `1 / 128` expects.
//
// **Never cast the blob to `const int16_t *`.** An instrument's offset is a byte
// count out of a file and need not be even, and the file is little-endian
// whatever the host is; `read_u16` is neither of those problems. Scaling 16-bit
// down by 256 rather than widening the mixer is what keeps the 8-bit path
// literally the arithmetic it has always been — and 256 is a power of two, so
// the 16-bit path loses nothing to it either.
inline float
instrument_frame(const Instrument &ins, uint32_t frame) {
  if (ins.bits == 16) {
    const uint8_t *d = (const uint8_t *) (const void *) ins.data;
    return (float) (int16_t) read_u16(d + (size_t) frame * 2u) * (1.f / 256.f);
  }
  return (float) ins.data[frame];
}

// One frame at a FRACTIONAL position: the two neighbours, linearly blended.
//
// **Split out of `channel_sample` so an editor can hear what the player will.**
// A preview voice that read nearest-neighbour while the player interpolates is
// a preview at a different timbre from the tune -- audibly so on anything
// transposed up, which is the difference this file's own comment about modems
// describes. One reader of a fractional position, and both callers use it.
//
// The neighbour of the last frame is the loop start when there is a loop, so a
// looping sample does not dip through the value it happens to end on.
// Out of range is silence rather than a read past the blob: a caller that has
// walked off the end is the one that decides whether that ends the note.
inline float
instrument_frame_at(const Instrument &ins, double pos) {
  if (ins.data == nullptr || ins.length == 0 || pos < 0.0)
    return 0.f;
  const uint32_t index = (uint32_t) pos;
  if (index >= ins.length)
    return 0.f;
  const uint32_t next_index =
      (index + 1u < ins.length)
          ? index + 1u
          : (ins.loop_len > 0 ? ins.loop_start : index);
  const double frac = pos - (double) index;
  const float a = instrument_frame(ins, index);
  const float b = instrument_frame(ins, next_index);
  return (float) ((double) a + ((double) b - (double) a) * frac);
}

// The step a note plays an instrument at, with no channel to hold it.
//
// **The pitch chain, in one place.** `channel_trigger` walks it for the player:
// the instrument's `transpose` shifts the cell's note, `note_transposed` clamps
// the sum because neither bound says anything about it, `period_for` takes the
// instrument's `finetune`, and `channel_set_step` turns a period into a step.
// An editor auditioning a note has to walk the same chain or the preview sounds
// at a different pitch from the tune, which is the one thing a preview must not
// do -- and a second spelling of it is the copy that drifts when a format field
// joins the chain.
//
// `note_max` is the module's, for the same reason `note_transposed` takes it.
// Zero for a note or a rate that cannot sound, which is silence rather than a
// position that runs away.
inline double
instrument_note_step(const Instrument &ins, int note, int note_max,
                     double sample_rate) {
  if (note <= 0 || note_max <= 0 || sample_rate <= 0.0)
    return 0.0;
  int shifted = note + (int) ins.transpose;
  if (shifted < kMinNote)
    shifted = kMinNote;
  if (shifted > note_max)
    shifted = note_max;
  const double period = period_for(shifted, ins.finetune);
  if (period <= 0.0)
    return 0.0;
  return (kAmigaClock / (2.0 * period)) / sample_rate;
}

// One channel's contribution, and the sample walk that goes with it.
//
// **Linearly interpolated rather than nearest.** A module's samples are a few
// kilobytes played back at whatever rate the period asks for, so nearest
// neighbour aliases audibly on anything high — it is the difference between a
// lead sounding like a lead and sounding like a modem. Two multiplies a frame.
// The volume, tremolo and envelope tail every voice shares.
//
// **Split out rather than duplicated, and written as the arithmetic that was
// already here, in the order it was already in.** A synth voice wants the same
// tail as a sample; two copies of it would drift, and a tail rewritten "more
// cleanly" would move the pinned fingerprints — which is a claim about the
// float operations, not about the result.
inline float
channel_gain_apply(Channel *ch, const Instrument &ins, float s) {
  // 8-bit signed, and 64 is full volume. Tremolo rides on top of the level the
  // row set, and is clamped here rather than where it was computed so the
  // underlying volume is left where a slide can go on moving it.
  float volume = ch->volume + ch->trem_offset;
  if (volume < 0.f)
    volume = 0.f;
  if (volume > 64.f)
    volume = 64.f;
  const float out = s * (1.f / 128.f) * (volume * (1.f / 64.f));

  // **Skipped outright when the instrument carries no envelope**, rather than
  // multiplied by a gain of one: that is the whole of "a module with no
  // envelope renders bit-for-bit what a module without envelopes always did".
  // The level used is the one from before the advance, so a note
  // starts at silence and reaches full exactly `env_attack_ms` later.
  if (ch->env_stage == EnvStage::kOff)
    return out;
  const float level = ch->env_level;
  env_advance(ch, ins);
  return out * level;
}

inline float
channel_sample(Channel *ch, const Instrument &ins) {
  if (!ch->playing)
    return 0.f;

  // Checked before the sample fields are, because a synth has none of them: it
  // owns no blob, its `length` is normally zero, and it stops itself.
  if (ins.type == (uint8_t) InstrumentType::kSynth)
    return channel_gain_apply(ch, ins, synth_sample(ch, ins));

  if (ins.data == nullptr || ins.length == 0)
    return 0.f;

  const uint32_t index = (uint32_t) ch->pos;
  if (index >= ins.length) {
    ch->playing = false;
    return 0.f;
  }

  const float s = instrument_frame_at(ins, ch->pos);

  ch->pos += ch->step;

  if (ins.loop_len > 0) {
    const double loop_end = (double) (ins.loop_start + ins.loop_len);
    // A while rather than an if: a very short loop at a very high period can be
    // stepped past more than once in a frame, and an `if` would leave the
    // position outside the loop for ever after.
    while (ch->pos >= loop_end)
      ch->pos -= (double) ins.loop_len;
  } else if (ch->pos >= (double) ins.length) {
    ch->playing = false;
  }

  return channel_gain_apply(ch, ins, s);
}

// Mixes into `buffer` rather than over it, so a tune and a mode's effects share
// an output the way `audio_bus.cc`'s own `render_add` does — and for the same
// reason: a renderer that assigns silently drops whatever was already there.
//
// Mono: every output channel gets the same value. ponytail: no stereo, and the
// upgrade is ProTracker's LRRL panning with a separation knob — the Amiga's
// hard-panned channels are a big part of how these sound, and a tune played
// dead centre is a tune with something missing.
// **The sequencer, separated from the mix so more than one mixer can drive it.**
// `render_add` below is one caller; a mixer that needs each channel's signal
// before it is summed is the other, and it cannot reuse the mix stage at all --
// it replaces panning, gain and clipping wholesale. What the two do share is
// exactly this: advance to the tick, and be told how many frames may be
// rendered before the next one.
//
// Returns 0 when the tune has stopped, and nothing further may be rendered.
//
// Every frame in a run must be rendered, and `player_run_end` called with the
// same count, or the sequencer's idea of where it is drifts from the audio's.
inline int
player_run_begin(Player *player, double rate, int max_frames) {
  // A `while`, because a tempo high enough to put a tick inside one frame would
  // otherwise fall permanently behind.
  while (player->sequencing && player->until_tick <= 0.0) {
    player_tick(player, rate);
    if (!player->playing)
      return 0;
    player->until_tick += frames_per_tick(player, rate);
  }

  int run = max_frames;
  if (player->sequencing) {
    const int until = frames_until_tick(player->until_tick);
    if (until < run)
      run = until;
  }
  return run > kMaxBlock ? kMaxBlock : run;
}

// **The single subtraction the bit-identity argument rests on.** Taking
// `(double) run` off once equals taking 1.0 off `run` times only while every
// intermediate stays positive, which is exactly what bounding a run by the next
// tick guarantees. A run that crossed a tick would put the `+= frames_per_tick`
// inside the chain, and its result rounds against the value it lands on.
inline void
player_run_end(Player *player, int run) {
  if (player->sequencing)
    player->until_tick -= (double) run;
}

// Walk every sounding voice `run` frames and throw the samples away.
//
// **One copy, called by both skip paths.** `render_add` keeps its own walk
// because it needs each value to mix; these two need only the state the walk
// leaves behind, and two copies of "what a frame does to a voice" is precisely
// the drift T52 was warned about.
inline void
player_walk_run(Player *player, int run) {
  const Module *m = player->module;
  for (int i = 0; i < run; ++i) {
    for (int c = 0; c < m->channels; ++c) {
      Channel *ch = &player->channels[c];
      if (ch->instrument <= 0)
        continue;
      // Muted channels are walked too, exactly as in `render_add` -- a channel
      // that stopped being read would freeze and jump back in when unmuted.
      (void) channel_sample(ch, m->instruments[ch->instrument - 1]);
    }
  }
}

// ---- Advancing without rendering (T52) -------------------------------------
//
// **The same loop `render_add` runs, with the audio half not written.** It calls
// `player_run_begin`/`player_run_end` for the tick logic and `channel_sample`
// for the voice walk -- the same two functions, so there is no second sequencer
// to drift and no second copy of what a frame does to a voice.
//
// **The voices are still walked, and that is the point rather than an
// oversight.** Everything a frame moves -- `pos`, `playing`, and the envelope --
// is a function of `step` and the frame count and never of a sample's value, so
// walking them reproduces the state exactly; skipping them would not merely
// lose amplitude, it would play DIFFERENT NOTES. `player_row`'s tie test reads
// `ch->playing` and `ch->env_stage`, both written by the walk, so a tie that
// should have struck (because a long sample ran out on the way) strikes only if
// the walk happened. That read is the only coupling of its kind in the
// sequencer, which is what makes it both real and cheap to honour.
//
// What is skipped is everything outside the voice: the pan gains, the summing,
// the clip, the buffer, and -- for a caller using the mixer -- the whole effect
// chain.
//
// **A caller using `ntrk_mix` must `mixer_reset` before skipping.** `fxpl_run`
// catches up by tick delta, so a skip of thousands of ticks leaves it thousands
// of slide steps behind, and the next block would run every one of them and
// slam each pan, gain and cutoff to its rail.
inline void
player_skip(Player *player, double rate, int frames) {
  if (player == nullptr || player->module == nullptr || !player->playing)
    return;
  if (frames <= 0)
    return;

  int frame = 0;
  while (frame < frames) {
    const int run = player_run_begin(player, rate, frames - frame);
    if (run == 0)
      return;
    player_walk_run(player, run);
    frame += run;
    player_run_end(player, run);
  }
}

// Advance to the first tick of `(order, row)`, or give up.
//
// **Bounded, and the bound is the answer to a real question**: a row can be one
// a tune never reaches, because a `Bxx` jumps over it or a loop never leaves the
// section it is in. Without a cap that is a hang; with one it is a `false` the
// caller can report. The cap counts TICKS rather than iterations, so it means
// the same thing at every tempo.
//
// Returns false and leaves the player where it got to, which is a position the
// caller may still render from -- there is nothing invalid about it.
inline bool
player_skip_to(Player *player, double rate, int order, int row,
               uint64_t max_ticks) {
  if (player == nullptr || player->module == nullptr || !player->playing)
    return false;
  const Module *m = player->module;
  if (order < 0 || order >= m->order_count || row < 0 || row >= m->rows)
    return false;

  const uint64_t start = player->ticks_elapsed;
  while (player->playing) {
    if (player->order == order && player->row == row)
      return true;
    if (player->ticks_elapsed - start > max_ticks)
      return false;
    // One run, which `player_run_begin` bounds by the next tick -- so the
    // position is re-examined at every tick boundary and never overshot by
    // more than the frames inside one.
    const int run = player_run_begin(player, rate, kMaxBlock);
    if (run == 0)
      return false;
    player_walk_run(player, run);
    player_run_end(player, run);
  }
  return false;
}

inline void
render_add(Player *player, double *buffer, int frames, int channels,
           float sample_rate) {
  if (player == nullptr || player->module == nullptr || !player->playing)
    return;
  if (buffer == nullptr || frames <= 0 || channels <= 0)
    return;

  const double rate = sample_rate > 0.f ? (double) sample_rate : 48000.0;
  const Module *m = player->module;

  // Pan gains, hoisted: neither pan nor separation moves inside a call, and
  // recomputing them per frame would be the same numbers 48000 times a second.
  //
  // **A balance law with unity centre, not equal power.** A centred voice has
  // always reached every output at full value here, so any law with a centre
  // gain below one would quietly drop the whole tune by 3 to 6 dB the day
  // panning arrived -- which reads as the update having broken the music
  // rather than as a pan control. What balance costs instead: two voices
  // panned hard apart sum 3 dB louder than the same two centred. That is the
  // behaviour a tracker has always had.
  //
  // The useful consequence is that this is a provable no-op when it is off: at
  // `separation == 0`, or for a mono caller, every gain is exactly 1.0f and
  // `s * 1.0f == s` in IEEE, so the output is bit-for-bit what it was before.
  const bool stereo = channels >= 2;
  float gain_l[kMaxChannels];
  float gain_r[kMaxChannels];
  for (int c = 0; c < kMaxChannels; ++c) {
    float p = 0.f;
    if (stereo) {
      float sep = player->separation;
      sep = sep < 0.f ? 0.f : (sep > 1.f ? 1.f : sep);
      p = player->pan[c] * sep;
      p = p < -1.f ? -1.f : (p > 1.f ? 1.f : p);
    }
    gain_l[c] = p > 0.f ? 1.f - p : 1.f;
    gain_r[c] = p < 0.f ? 1.f + p : 1.f;
  }

  // **Runs of frames, bounded by the next tick, rather than one frame at a
  // time.** This is the shape effects are written against: a filter coefficient
  // or a send level is computed once per run instead of once per sample, and at
  // 125 BPM a tick is 460 frames, which is far too long to hold a parameter
  // still. The cap is what makes a run short enough to interpolate over.
  //
  // **It is bit-identical to the per-frame version, and that is a proof rather
  // than a hope.** Subtracting `(double) run` once equals subtracting 1.0
  // exactly `run` times *provided every intermediate stays positive*: for a
  // double `x = M * 2^E`, each `x - 1.0` in that range is exactly
  // representable, so the chain reaches the exact real `u - run` with no
  // rounding at any step, and one subtraction reaches the same real value. The
  // condition is why a run must never cross a tick boundary -- the `+=
  // frames_per_tick` inside the while would land mid-chain, and its result is
  // round-dependent on the value it is added to.
  //
  // Nothing else moves: voices are still summed per frame in ascending order,
  // so the float addition order inside a frame is untouched.
  int frame = 0;
  while (frame < frames) {
    const int run = player_run_begin(player, rate, frames - frame);
    if (run == 0)
      return;

    for (int i = 0; i < run; ++i) {
    const int at = frame + i;
    float left = 0.f;
    float right = 0.f;
    for (int c = 0; c < m->channels; ++c) {
      Channel *ch = &player->channels[c];
      if (ch->instrument <= 0)
        continue;
      // Sampled even when muted, and then dropped. See Player::muted.
      const float s = channel_sample(ch, m->instruments[ch->instrument - 1]);
      // **`channel_silent`, not a test written here.** It answers for both the
      // listener's mute and the tune's, and it is asked from the mixer's own
      // render loop too -- one rule, one implementation.
      if (channel_silent(m, player, c))
        continue;
      left += s * gain_l[c];
      right += s * gain_r[c];
    }

    // Four channels at full tilt reach 4.0; the divide keeps the common case
    // well under one and the clip catches the rest without colouring anything
    // quieter, which a straight tanh over the whole range would.
    //
    // **Clipped per bus, after panning.** Clipping the pre-pan sum would
    // flatten a bus that never went hot -- four voices summing to 4.0 may be
    // 2.0 and 2.0 across the two, and neither earned the distortion.
    const float k = player->gain * 0.25f;
    left *= k;
    right *= k;
    left = left > 1.f ? 1.f : (left < -1.f ? -1.f : left);
    right = right > 1.f ? 1.f : (right < -1.f ? -1.f : right);

    // Odd outputs take the right bus, even the left, so mono takes the left --
    // which at unity gains is the mix this function has always produced.
    for (int c = 0; c < channels; ++c)
      buffer[at * channels + c] += (double) ((c & 1) ? right : left);
    }

    player_run_end(player, run);
    frame += run;
  }
}

// ----------------------------------------------------------------------------
// -- Describing a cell
// ----------------------------------------------------------------------------
//
// **A command's meaning lives beside the command, because otherwise every
// editor carries its own copy of this table** — and a table in an editor that
// drifts from the format is exactly the failure this codebase keeps catching.
// One source of truth, next to the thing it describes.
//
// Two levels, because an editor wants both: a *representation*, which is what
// the cell looks like in the grid, and a *meaning*, which is the sentence that
// goes in a status bar.
//
//     representation:  "4A 80"
//     meaning:         "Send 2 (Reverb) Predelay -> 0.50"
//
// **Split across two headers by the format's own opacity, not by tidiness.**
// This file knows ProTracker's sixteen effects and the plane range the *player*
// owns (0x30, 0x31) — everything whose meaning the format fixes. It cannot know
// what `param[2]` of a slot is, because that depends on the slot's `kind`,
// which is runtime state no file carries. `ntrk_mix.h` holds the `Mixer` and so
// answers that half; its `fxpl_describe` is the one entry point, and it
// delegates the player range back here rather than repeating it.
//
// Nothing here allocates, calls stdio or touches libm. Every function writes
// into the caller's buffer, truncates rather than overruns, always
// NUL-terminates, and returns the characters written excluding that NUL — so a
// `cap` of zero writes nothing at all and returns zero.

// A bounded builder. One of these behind every describe function below, which
// is why truncation is a property of the writer rather than a rule each caller
// has to remember.
struct TextOut {
  char  *buf;
  size_t cap;
  size_t len;
};

inline void
text_init(TextOut *t, char *buf, size_t cap) {
  t->buf = buf;
  t->cap = cap;
  t->len = 0;
  // A zero cap has nowhere to put the terminator either, so nothing is written
  // at all — an editor asking for no bytes must get none.
  if (buf != nullptr && cap != 0)
    buf[0] = '\0';
}

inline void
text_add(TextOut *t, const char *s) {
  if (t->buf == nullptr || t->cap == 0 || s == nullptr)
    return;
  while (*s != '\0' && t->len + 1 < t->cap)
    t->buf[t->len++] = *s++;
  t->buf[t->len] = '\0';
}

inline void
text_char(TextOut *t, char c) {
  const char s[2] = {c, '\0'};
  text_add(t, s);
}

inline void
text_hex(TextOut *t, unsigned value, int digits) {
  for (int i = digits - 1; i >= 0; --i)
    text_char(t, "0123456789ABCDEF"[(value >> (unsigned) (i * 4)) & 15u]);
}

inline void
text_int(TextOut *t, long value) {
  // Negated through unsigned so LONG_MIN is not undefined behaviour.
  unsigned long v = value < 0 ? (unsigned long) -(value + 1) + 1ul
                              : (unsigned long) value;
  char tmp[24];
  int n = 0;
  if (value < 0)
    text_char(t, '-');
  do {
    tmp[n++] = (char) ('0' + (int) (v % 10ul));
    v /= 10ul;
  } while (v != 0ul);
  while (n > 0)
    text_char(t, tmp[--n]);
}

// `num / den` to two decimals, rounded half up on the magnitude. **Integer
// arithmetic rather than a printf of a float**: this library carries no stdio
// and no libm, and both are exactly what the cross-target match forbids.
inline void
text_fixed2(TextOut *t, long num, long den) {
  if (den <= 0)
    return;
  const bool neg = num < 0;
  const unsigned long a =
      neg ? (unsigned long) -(num + 1) + 1ul : (unsigned long) num;
  const unsigned long h =
      (a * 100ul + (unsigned long) den / 2ul) / (unsigned long) den;
  if (neg && h != 0ul)
    text_char(t, '-');
  text_int(t, (long) (h / 100ul));
  text_char(t, '.');
  text_char(t, (char) ('0' + (int) ((h / 10ul) % 10ul)));
  text_char(t, (char) ('0' + (int) (h % 10ul)));
}

// A byte as the normalised 0..1 that a *set* means.
inline void
text_unit(TextOut *t, uint8_t v) {
  text_fixed2(t, (long) v, 255);
}

// The sign is always shown, because a step is a direction as much as a size.
inline void
text_signed2(TextOut *t, long num, long den) {
  if (num >= 0)
    text_char(t, '+');
  text_fixed2(t, num, den);
}

// The same byte read as the signed per-tick step a *slide* means.
inline void
text_step(TextOut *t, uint8_t v) {
  text_signed2(t, (long) (int8_t) v, 255);
}

// "C40" — ProTracker's own notation, one nibble of effect and a byte of
// parameter.
//
// **A LETTER for the command past the nibble**, which is what XM does with the
// same problem: `SLC 05` as `005` would be indistinguishable from `ARP 05`, and
// two commands rendering identically in a pattern grid is worse than an unusual
// character.
inline size_t
note_fx_repr(uint8_t effect, uint8_t param, char *out, size_t cap) {
  TextOut t;
  text_init(&t, out, cap);
  if (effect == kFxSlice)
    text_char(&t, 'G');
  else
    text_hex(&t, effect & 15u, 1);
  text_hex(&t, param, 2);
  return t.len;
}

// "4A 80" — a plane cell, whichever lane it sits in. A meta cell's two bytes
// print the same way; what they *mean* is the lane's business.
inline size_t
fxpl_repr(uint8_t cmd, uint8_t param, char *out, size_t cap) {
  TextOut t;
  text_init(&t, out, cap);
  text_hex(&t, cmd, 2);
  text_char(&t, ' ');
  text_hex(&t, param, 2);
  return t.len;
}

// Up wins over down when both nibbles are set, which is ProTracker's rule and
// `volume_slide`'s — a module setting both is relying on exactly that.
inline void
text_volslide(TextOut *t, uint8_t param) {
  if (param == 0) {
    text_add(t, " (unchanged)");
    return;
  }
  const int up = (param >> 4) & 15;
  const int down = param & 15;
  text_add(t, up > 0 ? ", +" : ", -");
  text_int(t, up > 0 ? up : down);
  text_add(t, "/tick");
}

// A parameter of zero means "keep the last one" for every slide ProTracker
// carries a memory for. Saying so beats printing a zero the player never uses.
inline void
text_speed_or_last(TextOut *t, uint8_t param) {
  if (param == 0) {
    text_add(t, " (unchanged)");
    return;
  }
  text_add(t, ", speed ");
  text_int(t, (long) param);
}

// A panning byte in words, for a status bar or an info field.
//
// The percentage comes off the same 127 the player divides by, so this reports
// where the channel actually sits rather than offering a second opinion about
// the byte. That is also why both 0x00 and 0x01 read as hard left: the clamp in
// `pan_from_byte` puts them in the same place, and a printer that disagreed
// with the audio would be worse than one that repeats itself.
inline void
text_pan(TextOut *t, long v) {
  if (v == 128) {
    text_add(t, "centre");
    return;
  }
  long pct = ((v < 128 ? 128 - v : v - 128) * 100 + 63) / 127;
  if (pct > 100)
    pct = 100;
  if (pct >= 100) {
    text_add(t, v < 128 ? "hard left" : "hard right");
    return;
  }
  text_add(t, v < 128 ? "L" : "R");
  text_int(t, pct);
  text_add(t, "%");
}

// The `0xE` sub-nibbles. Split out because the extended set is a second table
// and reads as one.
inline void
note_fx_describe_extended(TextOut *t, uint8_t param) {
  const int which = (param >> 4) & 15;
  const long value = (long) (param & 15);
  switch (which) {
    case 0x0: text_add(t, "Amiga LED filter (parsed, no-op)"); return;
    case 0x1: text_add(t, "Fine portamento up "); break;
    case 0x2: text_add(t, "Fine portamento down "); break;
    case 0x3:
      text_add(t, value != 0 ? "Glissando on" : "Glissando off");
      return;
    case 0x4: text_add(t, "Vibrato waveform "); break;
    case 0x5: text_add(t, "Set finetune "); break;
    case 0x6:
      if (value == 0) {
        text_add(t, "Set pattern loop point");
        return;
      }
      text_add(t, "Pattern loop ");
      text_int(t, value);
      text_add(t, " times");
      return;
    case 0x7: text_add(t, "Tremolo waveform "); break;
    case 0x8:
      text_add(t, "Coarse panning ");
      text_pan(t, value * 17);
      return;
    case 0x9:
      text_add(t, "Retrigger every ");
      text_int(t, value);
      text_add(t, " ticks");
      return;
    case 0xA: text_add(t, "Fine volume slide up "); break;
    case 0xB: text_add(t, "Fine volume slide down "); break;
    case 0xC:
      text_add(t, "Note cut at tick ");
      text_int(t, value);
      return;
    case 0xD:
      text_add(t, "Note delay ");
      text_int(t, value);
      text_add(t, " ticks");
      return;
    case 0xE:
      text_add(t, "Pattern delay ");
      text_int(t, value);
      text_add(t, " rows");
      return;
    default: text_add(t, "Invert loop (parsed, no-op)"); return;
  }
  text_int(t, value);
}

// ----------------------------------------------------------------------------
// -- Enumerating commands
// ----------------------------------------------------------------------------
//
// Describing answers "what does this cell say". An editor offering
// autocompletion asks the other question — **"what may go in this column"** —
// and until this table existed nothing here could answer it, so every editor
// would have kept its own list of the commands. That is the same drift the
// describe functions exist to prevent, arriving one door along.
//
// **There is no static sentence per command**, and that is a decision rather
// than an omission: a hundred hand-written summaries would duplicate what
// `note_fx_describe` and `mix::fxpl_describe` already *generate* from the cell
// in front of them, and the copy would be the half that goes stale. An
// autocomplete list shows the mnemonic and the name; an info line calls
// describe for a sentence about the actual cell.
//
// **And no min, max or unit.** A parameter is a byte, 0..255 is its range
// nearly everywhere, and "unit" means nothing for a command whose parameter is
// two independent nibbles. `ParamShape` says which of those a command is and
// `mix::command_value_text` renders the byte accordingly, which is the honest
// version of the same information.

// One row of a command table: everything static about a command number.
// `mix::CommandInfo` is this resolved for a lane and a mixer.
//
// **The mnemonic and the name sit in the same row on purpose.** They were two
// lists before, and two lists of commands are two lists to keep in step.
struct CmdInfo {
  uint8_t     cmd;
  const char *mnemonic;         // exactly three characters
  const char *name;
  ParamShape  shape;
  int         choice_count;     // 0 unless shape == Choice
  const char *const *choice;    // `choice_count` strings, else null
};

// **Every command in ProTracker's effect column, base and extended, in one
// table.** `cmd` is the effect nibble 0x00..0x0F for the first sixteen and
// `0xE0 | sub` for the sixteen the `E` container holds — two ranges that cannot
// collide, so one byte addresses the lot and `E6` is a command an editor can
// offer rather than a nibble it has to know to split.
//
// Row 14 is the container itself, kept so that the base effects are the first
// sixteen rows in their own order; its parameter is a `SplitNibble` whose high
// half chooses among rows 16..31.
inline const CmdInfo *
note_fx_table(int *count) {
  static const char *const kOnOff[2] = {"off", "on"};
  // Four waveforms in the low two bits; bit 2 says the wave runs on across a
  // new note instead of restarting, which `wave_amplitude` and `player_note`
  // read as one selector between them.
  static const char *const kWave[8] = {
      "sine", "ramp", "square", "random",
      "sine, no retrigger", "ramp, no retrigger",
      "square, no retrigger", "random, no retrigger"};
  static const CmdInfo kTable[33] = {
      {0x00, "ARP", "Arpeggio", ParamShape::SplitNibble, 0, nullptr},
      {0x01, "PTU", "Portamento up", ParamShape::Continuous, 0, nullptr},
      {0x02, "PTD", "Portamento down", ParamShape::Continuous, 0, nullptr},
      {0x03, "GLI", "Tone portamento", ParamShape::Continuous, 0, nullptr},
      {0x04, "VIB", "Vibrato", ParamShape::SplitNibble, 0, nullptr},
      {0x05, "GLV", "Tone portamento + volume slide", ParamShape::SplitNibble,
       0, nullptr},
      {0x06, "VBV", "Vibrato + volume slide", ParamShape::SplitNibble, 0,
       nullptr},
      {0x07, "TRM", "Tremolo", ParamShape::SplitNibble, 0, nullptr},
      {0x08, "PAN", "Set panning", ParamShape::Continuous, 0, nullptr},
      {0x09, "OFS", "Sample offset", ParamShape::Continuous, 0, nullptr},
      {0x0A, "VSL", "Volume slide", ParamShape::SplitNibble, 0, nullptr},
      {0x0B, "JMP", "Position jump", ParamShape::Continuous, 0, nullptr},
      {0x0C, "VOL", "Set volume", ParamShape::Continuous, 0, nullptr},
      {0x0D, "BRK", "Pattern break", ParamShape::Continuous, 0, nullptr},
      {0x0E, "EXT", "Extended", ParamShape::SplitNibble, 0, nullptr},
      {0x0F, "SPD", "Set speed / tempo", ParamShape::Continuous, 0, nullptr},
      {0xE0, "FLT", "Amiga LED filter (no-op)", ParamShape::Unused, 0, nullptr},
      {0xE1, "FPU", "Fine portamento up", ParamShape::Continuous, 0, nullptr},
      {0xE2, "FPD", "Fine portamento down", ParamShape::Continuous, 0, nullptr},
      {0xE3, "GLS", "Glissando", ParamShape::Choice, 2, kOnOff},
      {0xE4, "VBW", "Vibrato waveform", ParamShape::Choice, 8, kWave},
      {0xE5, "FTN", "Set finetune", ParamShape::Continuous, 0, nullptr},
      {0xE6, "LOP", "Pattern loop", ParamShape::Continuous, 0, nullptr},
      {0xE7, "TRW", "Tremolo waveform", ParamShape::Choice, 8, kWave},
      {0xE8, "PAN", "Coarse panning", ParamShape::Continuous, 0, nullptr},
      {0xE9, "RTG", "Retrigger", ParamShape::Continuous, 0, nullptr},
      {0xEA, "FVU", "Fine volume slide up", ParamShape::Continuous, 0, nullptr},
      {0xEB, "FVD", "Fine volume slide down", ParamShape::Continuous, 0,
       nullptr},
      {0xEC, "CUT", "Note cut", ParamShape::Continuous, 0, nullptr},
      {0xED, "DLY", "Note delay", ParamShape::Continuous, 0, nullptr},
      {0xEE, "PDL", "Pattern delay", ParamShape::Continuous, 0, nullptr},
      {0xEF, "IVL", "Invert loop (no-op)", ParamShape::Unused, 0, nullptr},
      // Past the nibble. See `kFxSlice`.
      {0x10, "SLC", "Play slice", ParamShape::Continuous, 0, nullptr},
  };
  if (count != nullptr)
    *count = 33;
  return kTable;
}

// Where a cell's `(effect, param)` sits in that table. Always in range: the
// byte is either one of the seventeen commands or it is masked to a nibble, and
// the sub-nibble likewise.
//
// **The `SLC` test comes first**, because everything after it masks. A version
// of this that masked and then asked would report `SLC` as `ARP` and take the
// other two functions with it -- precisely the bug `kFxSlice` names.
//
// **Equality, not `>=`.** One command lives past the nibble; a byte above it is
// not "the next one", it is a byte this format has not defined, and it masks
// exactly as it always did. Making the test a range would have handed 0xFF an
// index of 271 into a table of 33 -- which is how the in-range guarantee above
// got broken and then caught.
inline int
note_fx_index(uint8_t effect, uint8_t param) {
  if (effect == kFxSlice)
    return 32;
  return (effect & 15u) == 0xEu ? 16 + (int) ((param >> 4) & 15u)
                                : (int) (effect & 15u);
}

// The same cell as a three-letter mnemonic, for the pattern grid.
//
// **Display only.** The stored byte is untouched, so an imported `.mod` or
// `.xm` is unaffected and no fingerprint moves; this is a second reading of the
// same number, next to `note_fx_describe`'s prose reading of it.
//
// `E` resolves through its sub-nibble rather than printing `EXT`, because the
// extended set is sixteen unrelated effects and naming the container tells a
// reader nothing. `PAN` therefore appears for both `8xx` and `E8x`: they set
// the same thing at different resolutions, and inventing a second name for one
// of them would suggest a difference that is not there.
//
// Always writes exactly three characters and a terminator.
inline void
note_fx_mnemonic(uint8_t effect, uint8_t param, char out[4]) {
  const char *m = note_fx_table(nullptr)[note_fx_index(effect, param)].mnemonic;
  out[0] = m[0];
  out[1] = m[1];
  out[2] = m[2];
  out[3] = '\0';
}


// ProTracker's own effect column, `Note::effect` and `Note::param`.
//
// A speed or depth of zero is reported as "unchanged" rather than as a zero,
// because that is what the player does with it. The one nuance not spelled out
// is that vibrato and tremolo keep their memory *per nibble*, so `450` holds
// the depth and sets the speed; the whole-parameter case is the one worth a
// status bar's width.
inline size_t
note_fx_describe(uint8_t effect, uint8_t param, char *out, size_t cap) {
  TextOut t;
  text_init(&t, out, cap);
  const long hi = (long) ((param >> 4) & 15);
  const long lo = (long) (param & 15);

  // **Before the mask**, for the reason `note_fx_index` tests it first: this
  // switch is over a nibble, and `SLC 05` reaching it would be described as an
  // arpeggio.
  if (effect == kFxSlice) {
    text_add(&t, "Play slice ");
    text_int(&t, (long) param);
    return t.len;
  }

  switch (effect & 15u) {
    case 0x0:
      // An empty cell and an arpeggio with no parameter are the same silence,
      // and the player takes the same branch for both.
      if (param == 0) {
        text_add(&t, "None");
        break;
      }
      text_add(&t, "Arpeggio +");
      text_int(&t, hi);
      text_add(&t, " +");
      text_int(&t, lo);
      break;
    case 0x1:
      text_add(&t, "Portamento up");
      text_speed_or_last(&t, param);
      break;
    case 0x2:
      text_add(&t, "Portamento down");
      text_speed_or_last(&t, param);
      break;
    case 0x3:
      text_add(&t, "Tone portamento");
      text_speed_or_last(&t, param);
      break;
    case 0x4:
    case 0x7:
      text_add(&t, (effect & 15u) == 0x4 ? "Vibrato" : "Tremolo");
      if (param == 0) {
        text_add(&t, " (unchanged)");
        break;
      }
      text_add(&t, ", speed ");
      text_int(&t, hi);
      text_add(&t, " depth ");
      text_int(&t, lo);
      break;
    case 0x5:
      text_add(&t, "Tone portamento + volume slide");
      text_volslide(&t, param);
      break;
    case 0x6:
      text_add(&t, "Vibrato + volume slide");
      text_volslide(&t, param);
      break;
    case 0x8:
      text_add(&t, "Set panning ");
      text_pan(&t, param);
      break;
    case 0x9:
      if (param == 0) {
        text_add(&t, "Sample offset (unchanged)");
        break;
      }
      text_add(&t, "Sample offset ");
      text_int(&t, (long) param * 256);
      text_add(&t, " frames");
      break;
    case 0xA:
      text_add(&t, "Volume slide");
      text_volslide(&t, param);
      break;
    case 0xB:
      text_add(&t, "Position jump to order ");
      text_int(&t, (long) param);
      break;
    case 0xC:
      text_add(&t, "Set volume ");
      text_int(&t, param > 64 ? 64 : (long) param);
      break;
    case 0xD:
      // The parameter is read as decimal, which is ProTracker's and is the
      // trap: 0x10 is row ten, not row sixteen.
      text_add(&t, "Pattern break to row ");
      text_int(&t, hi * 10 + lo);
      break;
    case 0xE:
      note_fx_describe_extended(&t, param);
      break;
    case 0xF:
      // One effect doing two jobs is ProTracker's: below 32 it is ticks per
      // row, at 32 and above it is beats per minute.
      if (param == 0)
        text_add(&t, "Set speed/tempo (ignored)");
      else if (param < 32) {
        text_add(&t, "Set speed ");
        text_int(&t, (long) param);
        text_add(&t, " ticks/row");
      } else {
        text_add(&t, "Set tempo ");
        text_int(&t, (long) param);
        text_add(&t, " BPM");
      }
      break;
    default:
      // Unreachable: the switch is on `effect & 15`, and all sixteen are above.
      // Present so a future edit that drops one leaves a description rather
      // than an empty string an editor would draw as a blank status bar.
      text_add(&t, "Unknown effect");
      break;
  }
  return t.len;
}

// The plane range the player owns, 0x30..0x3F. **Anything else writes nothing
// and returns zero**, which is what lets `mix::fxpl_describe` chain it: a
// command outside this range is not the player's to name, and inventing a
// meaning here is how the two halves of the plane start disagreeing.
inline size_t
fxpl_player_describe(uint8_t cmd, uint8_t param, char *out, size_t cap) {
  TextOut t;
  text_init(&t, out, cap);
  if (cmd == kFxplAccent) {
    // The depth is the instrument's; this is the trigger, and it multiplies.
    text_add(&t, "Accent -> ");
    text_unit(&t, param);
  } else if (cmd == kFxplSlide) {
    if (param == 0) {
      text_add(&t, "Slide off");
    } else {
      text_add(&t, "Slide, ");
      text_int(&t, (long) param);
      text_add(&t, " ticks");
    }
  } else if (cmd >= 0x32u && cmd < 0x40u) {
    text_add(&t, "Reserved (0x");
    text_hex(&t, cmd, 2);
    text_add(&t, ")");
  }
  return t.len;
}

}  // namespace ntrk

#endif  // NTRK_H_
