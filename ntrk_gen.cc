// Write the coverage module the cross-target check renders.
//
//     c++ -std=c++11 -O2 -o ntrk_gen ntrk_gen.cc
//     ./ntrk_gen coverage.ntrk
//
// **A coverage instrument, not music.** `assets/music/circuit.ntrk` is a v1
// import: no synth voice, no envelope, no per-voice filter, no second effect
// plane, no macro table. That is very nearly everything the format has grown
// since, and it is also the most table-driven arithmetic in the library — the
// 303's ladder, the drum voices, the macro fixed point. Until this file existed
// the native-against-wasm render match, which is the measurement the whole
// no-libm policy rests on, never touched a line of any of it.
//
// **Generated once, natively, and then rendered by both targets.**
// `tools/check_ntrk_crosstarget.sh` runs this on the host and hands the bytes
// it wrote to each build of `ntrk_render`. Generating per target would compare
// two copies of *this* program and say nothing at all about the renderer.
//
// So every number below is arbitrary but fixed: what matters is that a feature
// is reached, not that it sounds like anything. Changing one changes the
// fingerprint the two targets agree on, which is harmless — they are compared
// with each other and never against a stored value.
//
// stdio and the library. `module_save` is the writer; nothing here re-implements
// the format.
//
// `ntrk_mix.h` is included for its command and value numbers alone — the plane
// the mixer reads is written here and the mixer's own enum is where those live.
// Nothing in this file calls a mixer function, so no translation unit of it has
// to be linked.

#include "ntrk_mix.h"

#include <stdio.h>
#include <string.h>

// Four channels, two patterns of thirty-two rows. Two bars of sixteen, so a
// riff written once per bar repeats and the second pattern can differ.
static const int kChannels = 4;
static const int kRows = 32;
static const int kPatterns = 2;
static const int kOrders = 2;
static const int kBarRows = 16;

// Two effect columns per channel and one whole-row meta lane. Column 0 carries
// the player's half of the plane (accent, slide) and column 1 the mixer's
// (pan, sends, the voice filter); the meta lane carries macro invocations.
// **One purpose per column** — the format allows any command in any column, but
// a file somebody reads to find out what a command does should not scatter them.
static const int kFxColumns = 2;
static const int kMetaColumns = 1;
static const int kLanes = kChannels * kFxColumns + kMetaColumns;

static const int kFxPlay = 0;   // accent and slide
static const int kFxMix = 1;    // pan, sends, cutoff

// Which channel is which. Channel 3 changes instrument between the patterns,
// which is how one channel covers both the wavetable and the PCM path.
static const int kChBass = 0;
static const int kChKick = 1;
static const int kChHat = 2;
static const int kChLead = 3;

static const int kInsBass = 1;   // SYNTH, the 303
static const int kInsKick = 2;   // SYNTH, kick
static const int kInsHat = 3;    // SYNTH, hihat
static const int kInsWave = 4;   // WAVE_BUILTIN + envelope
static const int kInsPcm = 5;    // PCM8 + envelope + per-voice filter
static const int kInsCount = 5;

// A drum's parameters are defined at C-3, note 25, so that is where every drum
// is struck. A kit written an octave up is a kit an octave up, and for a hihat
// that is a noise band against Nyquist rather than a note anybody hears as
// wrong. See `kSynthRefPeriod`.
static const int kDrumNote = 25;

// The macro table, one-based as a meta cell names them.
static const int kMacroSweep = 1;   // absolute: two cutoffs from one byte
static const int kMacroPush = 2;    // delta: a per-tick step

static ntrk::Note g_notes[kPatterns * kRows * kChannels];
static ntrk::FxCell g_fx[kPatterns * kRows * kLanes];
static uint8_t g_order[kOrders];

// A 64-frame ramp for the PCM instrument. Generated rather than a table of
// literals, and integer, for the same reason everything else in this directory
// is: it has to be the same bytes wherever it is built.
static const int kSampleFrames = 64;
static int8_t g_sample[kSampleFrames];

static uint8_t g_bytes[64 * 1024];

static ntrk::Note *
note_at(int pattern, int row, int channel) {
  return &g_notes[(pattern * kRows + row) * kChannels + channel];
}

static ntrk::FxCell *
fx_at(int pattern, int row, int lane) {
  return &g_fx[(pattern * kRows + row) * kLanes + lane];
}

static void
put_note(int pattern, int row, int channel, int note, int instrument) {
  ntrk::Note *n = note_at(pattern, row, channel);
  n->note = (uint8_t) note;
  n->instrument = (uint8_t) instrument;
}

static void
put_effect(int pattern, int row, int channel, int effect, int param) {
  ntrk::Note *n = note_at(pattern, row, channel);
  n->effect = (uint8_t) effect;
  n->param = (uint8_t) param;
}

static void
put_fx(int pattern, int row, int channel, int column, int cmd, int param) {
  ntrk::FxCell *cell = fx_at(pattern, row, channel * kFxColumns + column);
  cell->cmd = (uint8_t) cmd;
  cell->param = (uint8_t) param;
}

static void
put_meta(int pattern, int row, int macro, int input) {
  ntrk::FxCell *cell = fx_at(pattern, row, kChannels * kFxColumns);
  cell->cmd = (uint8_t) macro;
  cell->param = (uint8_t) input;
}

// One step of the acid line. **A 303 with no accent and no slide is a filtered
// saw**, so these are what makes the voice worth covering at all: the accent
// drives its second envelope and the slide drives the glide in `player_row`.
struct AcidStep {
  uint8_t row;
  int8_t degree;
  uint8_t accent;
  uint8_t slide;
};

static const AcidStep kRiff[] = {
  {  0,  0, 0xff, 0x00 },
  {  3, 12, 0x00, 0x03 },   // an octave up, glided
  {  6,  0, 0xa0, 0x04 },
  {  8,  7, 0x00, 0x00 },
  { 11,  3, 0xff, 0x00 },
  { 14,  0, 0xc0, 0x05 },   // a long glide across the bar line
};
static const int kRiffSteps = (int) (sizeof(kRiff) / sizeof(kRiff[0]));

// The lead's notes. **Above thirty-six on purpose**: the v1 ceiling is 36 and
// these reach the octaves past ProTracker's three, which is the one thing
// `note_max` changes in the tick path.
static const uint8_t kLeadNotes[8] = { 37, 44, 49, 44, 52, 49, 44, 37 };

static void
build_instruments(ntrk::Module *m) {
  // 1 — the 303. Enough resonance and env mod that the ladder is doing the
  // work, and a shaper on the end, so the voice's whole signal path is reached.
  ntrk::Instrument *bass = &m->instruments[kInsBass - 1];
  bass->type = (uint8_t) ntrk::InstrumentType::kSynth;
  bass->volume = 52;
  bass->synth_voice = (uint8_t) ntrk::SynthVoice::kBass;
  bass->synth_cutoff = 60;
  bass->synth_reso = 205;
  bass->synth_env_mod = 160;
  bass->synth_decay = 90;
  bass->synth_accent = 200;     // depth: what the plane's 0x30 scales
  bass->synth_drive = 70;
  bass->synth_dist = 2;         // tape, so the shaper is not the default kind
  bass->synth_dist_mix = 110;
  bass->synth_wave = 0;         // saw
  // The gate: a synth voice takes the instrument ADSR like any other, and this
  // is the shape a 303 line uses it in — no attack, no decay, full sustain, so
  // a held note is untouched and only a `^^^` changes anything.
  bass->flags = ntrk::kInstrumentEnvelope;
  bass->env_sustain = 64;
  bass->env_release_ms = 16;

  // 2 and 3 — two drum voices with different structure, not one voice twice: a
  // kick has a resonant body and a hihat is filtered noise alone, so the two
  // between them reach both halves of `channel_sample`'s drum path.
  ntrk::Instrument *kick = &m->instruments[kInsKick - 1];
  kick->type = (uint8_t) ntrk::InstrumentType::kSynth;
  kick->volume = 56;
  kick->synth_voice = (uint8_t) ntrk::SynthVoice::kKick;
  kick->synth_tune = 110;
  kick->synth_decay = 170;
  kick->synth_sweep = 120;
  kick->synth_tone = 100;
  kick->synth_noise = 25;
  kick->synth_noise_decay = 25;
  kick->synth_drive = 70;

  ntrk::Instrument *hat = &m->instruments[kInsHat - 1];
  hat->type = (uint8_t) ntrk::InstrumentType::kSynth;
  hat->volume = 30;
  hat->synth_voice = (uint8_t) ntrk::SynthVoice::kHihat;
  hat->synth_tone = 140;
  hat->synth_noise_decay = 30;
  hat->synth_drive = 40;

  // 4 — a built-in wavetable with a volume envelope. The cycle is regenerated
  // at load and stores nothing in the blob, which is its own path through both
  // the reader and the writer.
  ntrk::Instrument *wave = &m->instruments[kInsWave - 1];
  wave->type = (uint8_t) ntrk::InstrumentType::kWaveBuiltin;
  wave->wave_index = 3;                       // 25% pulse
  wave->data = ntrk::builtin_wave(3);
  wave->length = (uint32_t) ntrk::kBuiltinWaveFrames;
  wave->loop_len = (uint32_t) ntrk::kBuiltinWaveFrames;
  wave->volume = 34;
  wave->flags = ntrk::kInstrumentEnvelope;
  wave->env_attack_ms = 6;
  wave->env_decay_ms = 120;
  wave->env_sustain = 28;
  wave->env_release_ms = 180;

  // 5 — PCM8 out of the blob, with an envelope *and* the per-voice filter, so
  // the two flags are covered together and the SVF is reached from the
  // instrument entry rather than only from the plane. `transpose` brings the
  // ramp back down an octave, which is the field the low end of the range is reached
  // through.
  for (int i = 0; i < kSampleFrames; ++i)
    g_sample[i] = (int8_t) (i * 4 - 128);
  ntrk::Instrument *pcm = &m->instruments[kInsPcm - 1];
  pcm->type = (uint8_t) ntrk::InstrumentType::kPcm8;
  pcm->data = g_sample;
  pcm->length = (uint32_t) kSampleFrames;
  pcm->loop_len = (uint32_t) kSampleFrames;
  pcm->volume = 40;
  pcm->finetune = -3;
  pcm->transpose = -12;
  pcm->flags = (uint8_t) (ntrk::kInstrumentEnvelope | ntrk::kInstrumentFilter);
  pcm->env_attack_ms = 2;
  pcm->env_decay_ms = 90;
  pcm->env_sustain = 20;
  pcm->env_release_ms = 140;
  pcm->filter_cutoff_hz = 1400;
  pcm->filter_res = 180;
}

// **The macro is the whole reason a meta lane is here**, and the two records
// cover the two halves of `fxpl_macro`: an absolute one is a set on a row's
// first tick, a delta one is a step on every tick after it. Both scales and
// both signs of offset are used, because `macro_value` is i32 arithmetic on
// 8.8 fixed point and a sign error in it is silent.
static void
build_macros(ntrk::Module *m) {
  m->macro_count = 2;

  ntrk::Macro *sweep = &m->macros[kMacroSweep - 1];
  sweep->target_count = 2;
  sweep->flags = 0;
  sweep->targets[0].target = (uint8_t) ntrk::mix::kFxplCutoff;
  sweep->targets[0].scope = (uint8_t) kChBass;
  sweep->targets[0].scale = 0x0100;           // unity: the input byte, as is
  sweep->targets[0].offset = 0;
  sweep->targets[1].target = (uint8_t) ntrk::mix::kFxplCutoff;
  sweep->targets[1].scope = (uint8_t) kChLead;
  sweep->targets[1].scale = 0x00c0;           // three quarters of the knob...
  sweep->targets[1].offset = 0x1800;          // ...lifted twenty-four steps

  ntrk::Macro *push = &m->macros[kMacroPush - 1];
  push->target_count = 2;
  push->flags = ntrk::kMacroDelta;
  push->targets[0].target = (uint8_t) ntrk::mix::kFxplGain;
  push->targets[0].scope = ntrk::kMacroScopeAll;
  push->targets[0].scale = 0x0040;
  push->targets[0].offset = 0;
  push->targets[1].target = (uint8_t) ntrk::mix::kFxplCutoff;
  push->targets[1].scope = (uint8_t) kChHat;
  push->targets[1].scale = 0x0020;
  push->targets[1].offset = -0x0100;          // negative, so the divide signs
}

static void
build_patterns() {
  for (int p = 0; p < kPatterns; ++p) {
    // The kit: four to the floor and an offbeat hat, in both patterns.
    for (int r = 0; r < kRows; ++r) {
      if (r % 4 == 0)
        put_note(p, r, kChKick, kDrumNote, kInsKick);
      if (r % 4 == 2)
        put_note(p, r, kChHat, kDrumNote, kInsHat);
    }

    // The acid line, once a bar, with its two articulations in column 0.
    // A parameter of zero is a real value in this plane — `30 00` cancels an
    // accent — so absence is an empty cell rather than a written zero.
    for (int bar = 0; bar * kBarRows < kRows; ++bar) {
      for (int i = 0; i < kRiffSteps; ++i) {
        const int row = bar * kBarRows + (int) kRiff[i].row;
        put_note(p, row, kChBass, 13 + (int) kRiff[i].degree, kInsBass);
        if (kRiff[i].accent != 0u)
          put_fx(p, row, kChBass, kFxPlay, 0x30, kRiff[i].accent);
        if (kRiff[i].slide != 0u)
          put_fx(p, row, kChBass, kFxPlay, 0x31, kRiff[i].slide);
      }
    }

    // The lead, four rows apart: the wavetable in the first pattern and the
    // PCM instrument in the second, so one channel covers both.
    const int lead_ins = p == 0 ? kInsWave : kInsPcm;
    for (int i = 0; i < 8; ++i)
      put_note(p, i * 4, kChLead, (int) kLeadNotes[i], lead_ins);
  }

  // Two of the pattern's own effects, which share the tick path with the plane
  // and must keep working beside it: an arpeggio on the lead and a vibrato on
  // the bass.
  put_effect(0, 8, kChLead, 0x0, 0x47);
  put_effect(1, 20, kChBass, 0x4, 0x83);

  // `^^^` — above `note_max` by construction, so it is the one note value only
  // the format can carry, and it starts the envelope's release rather than
  // cutting. On both a sampled voice and the 303, because the release runs
  // outside the voice: the synth path reaches it through the same tail, and a
  // note-off written only on the lead left that half of the tail uncovered.
  put_note(1, 30, kChLead, ntrk::kNoteOff, 0);
  put_note(1, 26, kChBass, ntrk::kNoteOff, 0);

  // The mixer's half of the plane, in column 1. Pan and a send on the way in;
  // resonance and a filter type on the hat, which is what makes the mixer's own
  // per-channel SVF run on a channel whose instrument never asked for one.
  put_fx(0, 0, kChBass, kFxMix, ntrk::mix::kFxplPanSet, 0x40);
  put_fx(0, 0, kChLead, kFxMix, ntrk::mix::kFxplSendSet, 0x30);
  // Send 1 as well as send 0, because `ntrk_render --mix` puts the reverb on
  // one and the delay on the other: a module that only ever fed send 0 left
  // half the effect path out of the cross-target check.
  put_fx(0, 0, kChBass, kFxMix, ntrk::mix::kFxplSendSet + 1, 0x28);
  put_fx(0, 0, kChHat, kFxMix, ntrk::mix::kFxplResSet, 0xb0);
  put_fx(0, 1, kChHat, kFxMix, ntrk::mix::kFxplTypeSet, 0x01);

  // A slide lives only for the row it is written on, so the command is repeated
  // on each row it should run for rather than written once and left.
  for (int r = 16; r < 20; ++r) {
    put_fx(0, r, kChBass, kFxMix, ntrk::mix::kFxplPanSlide, 0x06);
    put_fx(0, r, kChLead, kFxMix, ntrk::mix::kFxplSendSlide, 0x08);
  }

  // The meta lane. Pattern 0 sets the sweep twice; pattern 1 hands it to the
  // delta macro, which steps every tick of the rows it is written on, and then
  // puts the knob back with an absolute set so the loop starts where it did.
  put_meta(0, 0, kMacroSweep, 0x60);
  put_meta(0, 16, kMacroSweep, 0xa0);
  for (int r = 0; r < 24; r += 8)
    put_meta(1, r, kMacroPush, 0x0c);
  put_meta(1, 28, kMacroSweep, 0x60);
}

int
main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <out.ntrk>\n", argv[0]);
    return 2;
  }

  memset(g_notes, 0, sizeof(g_notes));
  memset(g_fx, 0, sizeof(g_fx));

  ntrk::Module module;
  module.channels = kChannels;
  module.rows = kRows;
  module.speed = 6;
  module.bpm = 140;
  module.order_count = kOrders;
  module.pattern_count = kPatterns;
  module.instrument_count = kInsCount;
  module.restart = 0;
  module.note_max = ntrk::kMaxNote;
  module.fx_columns = kFxColumns;
  module.meta_columns = kMetaColumns;
  module.order = g_order;
  module.patterns = g_notes;
  module.fx = g_fx;

  for (int i = 0; i < kOrders; ++i)
    g_order[i] = (uint8_t) i;

  build_instruments(&module);
  build_macros(&module);
  build_patterns();

  size_t wrote = 0;
  if (!ntrk::module_save(&module, g_bytes, sizeof g_bytes, &wrote)) {
    fprintf(stderr, "module_save refused the module this built\n");
    return 1;
  }

  // Loaded straight back, because a generator that writes a file the library
  // will not open produces a check that fails on both targets identically and
  // says nothing about either.
  ntrk::Module check;
  if (!ntrk::module_load(&check, g_bytes, wrote)) {
    fprintf(stderr, "the module this wrote does not load back\n");
    return 1;
  }

  FILE *out = fopen(argv[1], "wb");
  if (out == NULL) {
    fprintf(stderr, "cannot write %s\n", argv[1]);
    return 1;
  }
  if (fwrite(g_bytes, 1, wrote, out) != wrote) {
    fprintf(stderr, "short write to %s\n", argv[1]);
    fclose(out);
    return 1;
  }
  fclose(out);

  printf("%s: %zu bytes, %d channels, %d patterns, %d instruments, "
         "%d macros, %d lanes\n",
         argv[1], wrote, check.channels, check.pattern_count,
         check.instrument_count, check.macro_count, ntrk::module_lanes(&check));
  return 0;
}
