// ntrk_types -- the structs the replayer and the synth voices both read.
//
// `Note`, `Instrument`, `FxCell`, `Module`, `Channel` and `Player`, plus the
// enums and tables they are built out of. Plain data and two constructors'
// worth of arithmetic; no allocation, no I/O, no libm, on the same terms as the
// rest of the library.
//
// **This file exists so that `ntrk_synth.h` can be a header.** The synth voices
// read a `Channel` and an `Instrument`, and those used to be declared halfway
// down `ntrk.h` -- so the only way to give the voices their types was for
// `ntrk.h` to close its namespace, include the synth, and reopen it. That made
// `ntrk_synth.h` an include fragment that compiled at exactly one point in one
// file and nowhere else, which is a thing no compiler will ever complain about
// and no reader will guess. The types have their own file now and both of them
// include it in the ordinary way.
//
// Nothing here depends on the loader or the player, which is what makes the
// split hold rather than merely tidy: this is the bottom of the dependency
// order, and anything that grows a call back up into `ntrk.h` belongs there
// instead.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_TYPES_H_
#define NTRK_TYPES_H_

// `Channel` holds two `fx::Svf` filters for the synth voices, so the shaper
// header is a real dependency of the data rather than of the code that uses it.
#include "ntrk_303.h"
#include "ntrk_fx_shape.h"

#include <stdint.h>

namespace ntrk {

const int kMaxChannels = 16;

// The format's own ceilings on geometry. The header's fields are u16 and would
// carry more; these are where the format stops, and the loader and the writer
// both refuse past them. Named rather than written as 256 in four places, so a
// caller can size itself to the format instead of to a number that matches by
// coincidence.
const int kMaxPatterns = 256;
const int kMaxRows = 256;
// The order list's own ceiling, which `module_param_range` has always enforced
// as a literal 256 in the param table. Named here because SMUT sizes itself to
// it: a table indexed by order position needs the format's bound, not a number
// that happens to match.
const int kMaxOrders = 256;

// The longest run of frames rendered between two looks at the sequencer. A tick
// at 125 BPM is around 460 frames, which is far too long to hold an effect
// parameter still, so this is the granularity a caller gets to interpolate at.
const int kMaxBlock = 64;
const int kMaxInstruments = 64;

// Slices per instrument. **Forced by `Note::param` being one byte**: an index
// past 255 is data no cell could name, so a larger ceiling would be storage for
// boundaries nothing can reach. `slice_count` is a `u16` in the file precisely
// so that 256 is representable.
const int kMaxSlices = 256;

// The highest note a file may name. **The new octaves go upward and that is
// forced rather than chosen**: notes 1..36 must keep their exact meaning,
// because a pattern block is a view over the caller's bytes and can never be
// renumbered. The low end is reached with an instrument's `transpose` instead,
// which is why that field exists.
//
// Above roughly note 60 a PCM sample plays back over 100 kHz and aliases hard.
// That is inherent to sample playback with no oversampling; the top two octaves
// are for wavetable instruments, and an editor should say so.
const int kMaxNote = 96;

// PAL. The player is a period-based machine because the effects are — porta and
// vibrato move a *period*, not a frequency, and a semitone is not a fixed number
// of either. Doing it in Hz means reimplementing the curve the period table is.
const double kAmigaClock = 7093789.2;

// The lowest note the player will sound, as an offset from note 1.
//
// **ProTracker stopped at note 1 because the Amiga's period was a hardware
// divider; this steps a double and has no such floor.** Note 1 is MIDI 48 on a
// 32-frame wave, so everything below middle C was simply unreachable, and a
// TB-303 pattern had to be transposed up two octaves to play at all. It sounded
// exactly as high as that says.
//
// **-47 is the full MIDI range.** Note 1 sits at MIDI 48, so 47 semitones under
// it is MIDI 0, and `note_max` at 96 reaches MIDI 143 — past MIDI 127 at the
// top already. A file still cannot *store* a note below 1, because the format's
// note byte starts there; the way down is `Instrument::transpose`, whose own
// floor of -48 lands a stored note 1 exactly here. The two limits meeting is
// not a coincidence worth relying on, but it is a pleasant one.
//
// At the bottom the period is 856 * 2^47/12 ≈ 13134, which a double steps as
// happily as any other, and the pitch is 8.4 Hz.
const int kMinNote = -47;

// The anchor: what note 1's period is, and therefore where everything sits.
//
// **Concert pitch, not the Amiga's.** ProTracker's 856 puts note 1 at 129.487 Hz
// on a 32-frame wave, which is 17.64 cents flat of MIDI 48 — the same 17.64 at
// every pitch, because it is one constant. Internally consistent and out of tune
// with everything else in the world, which matters the moment a tune is played
// beside anything: a synth voice, a sample recorded to a tuning fork, another
// instrument.
//
// So this is `kAmigaClock / (2 * 32 * 130.8127827)` — the period at which a
// 32-frame builtin cycle sounds MIDI 48 exactly. The 32 is kBuiltinWaveFrames,
// spelled out because it is declared further down this file.
//
// **It improves XM and costs MOD.** XM calls 8363 Hz its C-4 rate; ntrk used to
// put 8287.14 there, 15.78 cents flat. It now puts 8372.02 there, which is 1.87
// cents *sharp* of 8363 — and that residual is XM's own rounding, since a true
// C-4 times 32 is 8372.02. A .mod, meanwhile, now plays 17.64 cents above what
// an Amiga would have played it at. That is the same trade as computing the
// scale instead of reading ProTracker's table, taken for the same reason.
const double kPeriodC1 = 847.3212936025169;

// The twelve semitone ratios, 2^(-k/12). A period is C's scaled by one of these
// and then halved once per octave, which is equal temperament exactly.
//
// **This replaced ProTracker's period table, and it is a deliberate break with
// it.** That table is integers, and integers are not a scale: at the top of its
// three octaves one unit is 15 cents, so the same pitch class came out tuned
// differently in different octaves — D, E, G, G# and B all disagree between
// octave 2 and octave 3 by up to 6.4 cents. Since everything above the table
// was derived by halving from its top octave, that error then repeated for
// ever, and a lead two octaves over a bass beat against it about twice a second.
// It was audible, it was measured, and no tune wants it.
//
// The cost, stated plainly: a ProTracker module plays up to 6.9 cents from what
// ProTracker would have played it at, on five of the twelve pitch classes. That
// is a real difference in fidelity to the original hardware and it was chosen
// anyway — being in tune is worth more than reproducing a rounding error from
// 1987. The importer keeps its own copy of the historical table (`kModPeriods`
// in ntrk_import.cc) for reading a .mod's raw periods back into notes, which is
// the one place the old values are still the right answer.
const double kSemitoneRatio[12] = {
  1.0000000000000000, 0.9438743126816935, 0.8908987181403393,
  0.8408964152537145, 0.7937005259840998, 0.7491535384383408,
  0.7071067811865476, 0.6674199270850172, 0.6299605249474366,
  0.5946035575013605, 0.5612310241546865, 0.5297315471796477,
};

// Finetune is eighths of a semitone, and it *lowers* the period as it rises.
// A multiplier rather than the sixteen tables ProTracker ships, because the
// tables are that multiplier rounded to integers and we have floats.
// Index 0..7 is finetune 0..7, index 8..15 is -8..-1, the module's own order.
const double kFinetune[16] = {
  1.000000, 0.992806, 0.985663, 0.978572, 0.971532, 0.964542, 0.957603,
  0.950714,
  1.059463, 1.051841, 1.044274, 1.036761, 1.029302, 1.021897, 1.014545,
  1.007246,
};

// ProTracker's sine, a quarter wave mirrored into 32 entries. Vibrato and
// tremolo read it; the shape is part of how a module sounds and is not a
// sine we may as well compute.
const uint8_t kSine[32] = {
    0,  24,  49,  74,  97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250,
  253, 255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120,  97,  74,
   49,  24,
};

// A single cycle, the length Amiga chip tunes have always used for one: at
// period 428 a 32-frame cycle sounds at 259 Hz, so a wavetable instrument sits
// where a note asks it to without a table of its own per octave.
const int kBuiltinWaveFrames = 32;
const int kBuiltinWaveCount = 5;

// Square, saw, triangle and two pulse widths, on the same 8-bit scale as
// everything else the mixer reads.
//
// **Generated once, into storage that already exists.** Nothing here allocates,
// so this is a static filled by a constructor rather than a buffer; the
// alternative is a hundred and sixty literals, and every shape is one line of
// integer arithmetic. All of it is integer on purpose — this header has no libm.
struct BuiltinWaves {
  int8_t frame[kBuiltinWaveCount][kBuiltinWaveFrames];

  BuiltinWaves() {
    const int n = kBuiltinWaveFrames;
    for (int i = 0; i < n; ++i) {
      const int fold = i < n / 2 ? i : n - i;    // 0..16..1, for the triangle
      frame[0][i] = (int8_t) (i < n / 2 ? 127 : -128);
      frame[1][i] = (int8_t) (i * 8 - 128);
      frame[2][i] = (int8_t) (fold * 255 / (n / 2) - 128);
      frame[3][i] = (int8_t) (i < n / 4 ? 127 : -128);
      frame[4][i] = (int8_t) (i < n / 8 ? 127 : -128);
    }
  }
};

// A function-local static rather than a namespace-scope one: C++11 has no
// inline variables, and this header is included from more than one translation
// unit. It is also the only initialisation order this can have.
inline const int8_t *
builtin_wave(int index) {
  static const BuiltinWaves waves;
  return waves.frame[index];
}

// What an instrument's `type` says its frames are. **Only PCM16 is two bytes a
// frame**, and the loader derives the stride from that rather than trusting a
// width field, so there is one place a hostile file can disagree with itself
// and it is checked.
//
// **Scoped, and `Instrument::type` stays a `uint8_t`.** The field is byte 18 of
// a saved instrument record and the loader range-checks it there; widening it
// to the enum would put an unchecked file byte inside a type that claims it is
// one of these five. The underlying type is the format's, so every comparison
// is a cast in the direction the format already fixed.
enum class InstrumentType : uint8_t {
  kPcm8 = 0,
  kPcm16 = 1,
  kWaveData = 2,
  kWaveBuiltin = 3,
  kSynth = 4,
};

// Instrument `flags`, bit 0 upward. The top nibble is reserved and a file that
// sets any of it is refused, so those bits stay free to mean something.
const uint8_t kInstrumentEnvelope = 0x01;
const uint8_t kInstrumentFilter = 0x02;
const uint8_t kInstrumentFilterType = 0x0c;

// Where a channel's volume envelope has got to. **`EnvStage::kOff` is the
// absence of a stage rather than one of them**: an instrument with the envelope
// bit clear leaves a channel here for the whole note, and the amplitude
// multiply in `channel_sample` is then skipped outright rather than done
// against a gain of one. Skipped rather than unity is the whole of "a v1 file
// renders bit-for-bit what it always did" — a multiply by 1.0f happens to be
// exact, but a gain that is only *nearly* one the day a stage boundary rounds
// differently would not be, and nothing would notice.
//
// **Unlike `InstrumentType` this one is the field's own type.** A stage is
// player state and never crosses the format, so there is no file byte to
// range-check and nothing to be gained by storing it as a number.
enum class EnvStage : uint8_t {
  kOff = 0,
  kAttack,
  kDecay,
  kSustain,
  kRelease,
};

// The note that stops one, a tracker's `^^^`: it starts the release rather than
// cutting, and with no envelope on the instrument it is the cut. **Acted on
// before the `<= note_max` test that drops everything above the range**, which
// is the trap — 97 is out of range by construction and would otherwise be
// ignored as a stray byte. A v1 file cannot name it; 98..255 are reserved.
const int kNoteOff = 97;

// How many bytes an instrument name occupies in a NAME block.
//
// **Twenty-two because that is what a tracker name has always been** -- it is
// ProTracker's sample-name field and XM's instrument-name field alike, so an
// importer that starts preserving names has somewhere to put them that is
// exactly the right size, and a name that survived a `.mod` round trip through
// another program still fits. Fixed width rather than counted: the block's
// length is then one multiply against the instrument count, which is the same
// check SYNP already makes.
const int kNameBytes = 22;

// TUNE: how the grid is divided, and how far the odd rows of a pair are pushed.
//
// **Eight bytes, and the swing field is here from the first day even though no
// player reads it until T50.** A block that grows a field later is a layout
// change; one that gains a reader later is not — and swing zero is the straight
// grid every file already has, so carrying it costs nothing.
const int kTuneBytes = 8;

// Q12, as MIXR's gain and width are: 4096 is one whole row. Bounded strictly
// below half a row, because a swing that reached one would put the odd row on
// top of the next one, which is not a groove but a missing row.
const int kSwingUnit = 4096;
const int kSwingMax = 2047;

// What a command does with its parameter byte, and what an instrument field is.
//
// **Shared by both tables on purpose.** An FXPL filter-type dropdown and an
// instrument filter-type dropdown are the same control over the same four
// names; giving them two shapes would be two code paths drawing one thing.
enum class ParamShape {
  Continuous,   // a magnitude: a level, a speed, a number of ticks
  Choice,       // one of `choice_count` named alternatives
  SplitNibble,  // two independent numbers packed into the one byte
  Unused        // read by nothing; the command is the whole cell
};

struct Instrument {
  const int8_t *data = nullptr;   // frames, into the file buffer or a built-in
  uint32_t length = 0;            // frames
  uint32_t loop_start = 0;
  uint32_t loop_len = 0;          // 0 means one-shot

  // The SLIC table's boundaries for this instrument: `slice_count` frame
  // offsets, little-endian, **pointed at rather than copied**. 64 × 256 × 4 is
  // 65 KB, ten times the whole `Module`, which is the same argument that makes
  // `data` a view.
  //
  // **Raw bytes rather than `const uint32_t *`**, because a block's offset in
  // the file is arbitrary: a cast would be unaligned and host-endian. One
  // accessor, `slice_offset`, is the single reader of a boundary.
  const uint8_t *slices = nullptr;
  uint16_t slice_count = 0;
  uint8_t volume = 64;            // 0..64
  int8_t finetune = 0;            // -8..7

  // The rest of the entry. **An imported `.mod` carries exactly these
  // defaults**, so every feature below is a flag test that is false rather than
  // a test of what kind of module this is — which is the same test a file with
  // no envelope needs anyway.
  uint8_t type = (uint8_t) InstrumentType::kPcm8;
  uint8_t bits = 8;               // derived from `type`, never read from a file
  uint8_t flags = 0;
  int8_t transpose = 0;           // a whole signed byte; see InsParam::kTranspose
  uint16_t env_attack_ms = 0;
  uint16_t env_decay_ms = 0;
  uint16_t env_release_ms = 0;
  uint8_t env_sustain = 0;        // 0..64
  uint16_t filter_cutoff_hz = 0;  // 20..20000, checked only when the bit is set
  uint8_t filter_res = 0;         // resonance = filter_res / 255
  uint8_t wave_index = 0;         // which built-in shape, for WAVE_BUILTIN

  // The SYNP record, copied out the way every other instrument field is rather
  // than pointed at -- sixteen bytes is cheaper to copy than to bounds-check
  // twice. Zero on every instrument that is not SYNTH and in every v1 file, and
  // nothing reads them unless `type` says to.
  uint8_t synth_voice = 0;
  uint8_t synth_tune = 0;
  uint8_t synth_decay = 0;
  uint8_t synth_sweep = 0;
  uint8_t synth_tone = 0;
  uint8_t synth_noise = 0;
  uint8_t synth_noise_decay = 0;
  uint8_t synth_drive = 0;
  uint8_t synth_cutoff = 0;
  uint8_t synth_reso = 0;
  uint8_t synth_env_mod = 0;
  uint8_t synth_accent = 0;
  uint8_t synth_dist = 0;
  uint8_t synth_dist_mix = 0;
  uint8_t synth_wave = 0;

  // The instrument's name, out of an optional NAME block, and **copied rather
  // than pointed at** for the reason the SYNP record is: twenty-two bytes is
  // cheaper to copy once than to bounds-check at every use.
  //
  // `kNameBytes + 1`, and the extra byte is always zero, so this is directly
  // usable as a C string -- the file's field is fixed-width and NOT
  // NUL-terminated, and a reader that forgot would run into the next name.
  // Empty on every instrument of a file that carries no block.
  //
  // **The bytes are whatever the file said.** A `.mod` name is Latin-1 or
  // somebody's ASCII art, so this is not guaranteed to be UTF-8 and a caller
  // that draws it has to say what it does about that -- the format has no
  // business having an opinion on an encoding.
  char name[kNameBytes + 1] = {};
};

// The sample width `type` implies. **Derived, never read from a file and never
// spelled twice:** one width field that could disagree with `type` is one way
// for a file to talk a reader into a stride it did not check, and one copy of
// this expression that drifts is the same bug with an editor holding the pen.
inline uint8_t
instrument_bits_for(uint8_t type) {
  return type == (uint8_t) InstrumentType::kPcm16 ? (uint8_t) 16u : (uint8_t) 8u;
}

// One cell. Four bytes, because that is what a pattern is mostly made of and a
// tune is thousands of them: 64 rows by 4 channels by 4 bytes is a 1K pattern.
struct Note {
  uint8_t note = 0;         // 0 none, else 1..note_max
  uint8_t instrument = 0;   // 0 none, else 1..instrument_count
  uint8_t effect = 0;       // 0..15
  uint8_t param = 0;
};

// One cell of the second effect plane. **A parallel plane rather than a wider
// `Note`, and that is forced twice over**: a pattern block is a view over the
// caller's bytes with no allocation anywhere, so widening the cell would need a
// conversion pass that cannot exist — and all sixteen top-level effect numbers
// are already taken. Command 0 means nothing here, and the namespace is new, so
// a collision with ProTracker's is structurally impossible.
struct FxCell {
  uint8_t cmd = 0;
  uint8_t param = 0;
};

// The pattern block is bounds-checked as `... * 4` and then cast to `Note *`,
// so the two have to agree. Adding a field here without the check noticing is
// how a reader ends up striding past what it validated, in a build where
// assertions do nothing.
static_assert(sizeof(Note) == 4, "a cell is four bytes, and the loader says so");
static_assert(sizeof(FxCell) == 2,
              "a plane cell is two bytes, and the loader says so");

// The plane's geometry. **It travels in the FXPL payload, not in the header**:
// the header's thirty-two bytes are all spent and a header change is version 3,
// so a four-byte prefix in front of the cells is where the two counts can go
// without making one.
//
// **Lanes rather than channels is the plane's minor dimension.** A channel's
// effect columns are contiguous, and the meta lanes follow every channel lane,
// which keeps the block to one array, one stride and one exact length check
// however many columns a file asks for.
// **Eight columns is Renoise's number, and it is a plane budget rather than a
// player one.** A column costs two bytes per cell per row: sixteen channels at
// sixty-four rows over nineteen patterns is ~39 KB a column, so eight is
// ~311 KB of plane. Nothing on a desktop; real against the web build's budget,
// which is why a file declares what it uses rather than always paying for the
// ceiling.
//
// The meta lanes stay at four. They are whole-row automation, and four
// simultaneous macro invocations on one row is already more than a song has
// ever wanted -- widening them costs every channel's row, not one channel's.
const int kMaxFxColumns = 8;
const int kMaxMetaColumns = 4;

// How many macros a file may name, and how many parameters one may reach.
// **Eight targets is not a budget, it is the whole space**: the mixer's
// `kFxplValues` is 8, so a macro may touch every parameter it has, once.
const int kMaxMacros = 32;
const int kMaxMacroTargets = 8;

// **This and the mixer's `kFxplValues` have to agree, and no compiler makes
// them.** This header is the bottom of the dependency order and cannot see
// `ntrk_mix.h`; a macro target is range-checked against this at load, so the
// two drifting apart is a file whose targets index the mixer's value array out
// of bounds. The static_assert that pins them belongs beside `kFxplValues`,
// which is the end that can see both.
const int kFxplValueCount = 8;

// ---- MIXR, the mixer's own configuration -----------------------------------
//
// **What the effect plane cannot reach, and therefore what the file had nowhere
// to put.** Every level, pan, filter and slot *parameter* is an FXPL command
// and has always been saved; a slot's *kind* is deliberately not one -- a row
// that switched a shaper to a reverb would point the tank at memory the caller
// laid out for something else -- and the stereo width has no command at all. So
// those are a block rather than a plane, and they are the mixer's starting
// state, which automation moves from.
//
// The layout is here for the same reason `kFxplValueCount` is: the loader
// length-checks the block exactly, and the loader cannot see `ntrk_mix.h`. The
// static_asserts that pin these to the mixer's own counts are in that header,
// which is the end that can see both.
//
//   u16 version (2)   u16 slots   u16 master gain   u16 width
//   then per slot: u8 kind, u8 reserved, u16 param[8]
//   then, at v2 only: u8 send_level[kMaxChannels][kMixrSends]
//
// Gain and width are Q12 -- 4096 is unity -- which reaches 16 with a resolution
// of 0.00024, far finer than a fader moves. A parameter is normalised 0..1 over
// the whole u16, because a slot's own ceiling is what denormalises it and a
// delay time against a two-second ceiling wants better than a byte's 8 ms.
// **Version 2 adds the send levels.** A send only sounds if a channel feeds it --
// `bus[i] += v[i] * send_level[c][s]` -- and until v2 the only thing that could write
// that number was an effect-plane command, so a tune had to carry a command on row 0
// to say which channels go to the reverb. That is exactly what this block's own
// argument about the master gain says a file should not have to do.
//
// **A v1 block still loads**, with every level at zero, which is what a file written
// before this meant: nothing fed the sends except what the plane said. And a module
// whose levels are ALL zero is written back as v1, so a file that does not use the
// feature round-trips byte-identical and an older reader keeps it -- the same policy
// TUNE and SMUT follow, for the same reason.
const int kMixrVersion = 2;
const int kMixrVersion1 = 1;       // before the send levels; still read, never written new
const int kMixrSlots = 5;          // the four sends, then the master
const int kMixrSends = kMixrSlots - 1;
const int kMixrSlotParams = 8;
const int kMixrSlotBytes = 2 + kMixrSlotParams * 2;
const int kMixrHeaderBytes = 8;
const int kMixrBytesV1 = kMixrHeaderBytes + kMixrSlots * kMixrSlotBytes;
// One byte per (channel, send). A level is a gain in 0..1 and a byte's 1/255 is finer
// than the plane's own command resolution, which is also a byte -- so the block can say
// anything an automation of it could.
const int kMixrSendLevelBytes = kMaxChannels * kMixrSends;
const int kMixrBytes = kMixrBytesV1 + kMixrSendLevelBytes;
const int kMixrQUnit = 4096;       // Q12: what 1.0 is stored as


// A macro's `flags`. Everything above bit 0 is reserved and a file that sets
// any of it is refused, so those bits stay free to mean something.
const uint8_t kMacroDelta = 0x01;   // add to the value rather than replace it

// A target's `scope`, which says where the value lands. 0..15 name a channel
// outright. A scope naming a channel the module does not have is ignored at
// use, like every other command in the plane — a macro table that refused to
// load because a song was narrowed to fewer channels would be a table an editor
// cannot hold while somebody is still arranging.
const uint8_t kMacroScopeSelf = 0xfe;   // the channel whose cell invoked it
const uint8_t kMacroScopeAll = 0xff;

// **Fixed point rather than float, and that is the library's identity rather
// than a preference**: nothing in the format path may depend on libm or on a
// target's floating-point contraction, because the cross-target render match is
// a measured property. 8.8 — 256 is unity, 0x0100.
struct MacroTarget {
  // Two disjoint ranges; see `macro_target_valid`.
  uint8_t target = 0;   // 0..kFxplValueCount-1, or 0x40..0x7F for a slot param
  uint8_t scope = 0;    // a channel, kMacroScopeSelf or kMacroScopeAll
  int16_t scale = 0;
  int16_t offset = 0;
};

// **Two disjoint ranges, and no byte in a MACR block that already ships
// moves.** `0x00..0x07` index the mixer's `fxpl_val` as they always have;
// `0x40..0x7F` name an effect-slot parameter as `0x40 + (slot << 3 | param)`,
// which is exactly the numbering the plane's slot *set* commands use -- so an
// editor has one table rather than two, and there is no mapping to keep in
// step. A slot parameter needs no widening of `kFxplValueCount` because it has
// an accumulator already: `Slot::param` persists across ticks the way
// `fxpl_val` does.
//
// Refused at load rather than ignored at use. A macro is a table, not a
// command, and there is nothing sensible for an out-of-range entry to mean;
// MACR is fixed-size and already walked, so the check costs nothing.
inline bool
macro_target_valid(uint8_t target) {
  return target < (uint8_t) kFxplValueCount ||
         (target >= 0x40u && target < 0x80u);
}

// One macro record. **An empty target list is legal**: an editor makes a macro
// before it fills one in, and refusing that means a song somebody is halfway
// through writing will not load.
//
// A cell naming a macro past `macro_count` is ignored where it is used rather
// than refused at load — finding one needs a scan of the whole plane, and the
// policy here has always been that commands are validated at use.
struct Macro {
  uint8_t target_count = 0;   // 0..kMaxMacroTargets
  uint8_t flags = 0;
  MacroTarget targets[kMaxMacroTargets];
};

struct Module {
  int version = 2;           // the only one; module_save refuses any other

  // The tune's own division of its grid, out of an optional TUNE block. **A
  // default rather than a sentinel**: four rows to a beat and sixteen to a bar
  // is what a tracker means by a sixteenth, so a file with no block behaves as
  // every file always has and an editor has a number to draw its bands from
  // without asking whether one was set.
  //
  // A bar is a whole number of beats, checked at load — otherwise the two
  // bands an editor draws cannot line up and one of them is lying.
  int rows_per_beat = 4;
  int rows_per_bar = 16;

  // How far the odd row of a pair is pushed late, in kSwingUnit. **Nothing
  // reads this yet.** It is carried so that the day a player learns swing, no
  // file layout changes and no existing file means something different.
  int swing = 0;
  int channels = 0;
  int rows = 0;              // per pattern
  int speed = 6;             // ticks per row
  int bpm = 125;
  int order_count = 0;
  int pattern_count = 0;
  int instrument_count = 0;
  int restart = 0;           // order to loop back to

  // 36 for a file that stays inside ProTracker's three octaves, 96 otherwise.
  //
  // **Every note ceiling in the player comes from here rather than from the
  // size of the period table.** That is what keeps a three-octave tune's
  // arpeggio saturating at note 36 the day the table grows: the ceiling travels
  // as data, so no code path has to ask what a module is.
  int note_max = 36;

  // The FXPL plane's geometry, out of the block's own prefix. One column and no
  // meta lanes is what a file without a plane carries, so `module_lanes` is one
  // formula and the player needs no test for whether a plane is there.
  int fx_columns = 1;        // per channel, 1..kMaxFxColumns
  int meta_columns = 0;      // whole-row lanes, after every channel lane

  const uint8_t *order = nullptr;
  const Note *patterns = nullptr;   // pattern_count * rows * channels
  const FxCell *fx = nullptr;       // FXPL, null unless the file carried one
  Instrument instruments[kMaxInstruments];

  // The macro table. Copied out of the file the way `instruments` is rather
  // than pointed at: a `Module` is a view that allocates nothing, and a
  // fixed-size record is cheaper to copy once than to bounds-check at every
  // use. Zero on every file that carries no MACR block.
  int macro_count = 0;
  Macro macros[kMaxMacros];

  // The MIXR block, copied out of the file rather than pointed at -- 98 bytes,
  // the same trade `macros` makes, and it means an editor can write a fader
  // straight into the module it is about to save. **Nothing in the replayer
  // reads a byte of it**: a mixer's configuration is not something `render_add`
  // can act on, and `ntrk_mix` is where it is decoded.
  bool has_mix = false;
  uint8_t mix[kMixrBytes] = {};

  // SMUT: which (order position, channel) pairs are silenced. One u16 per
  // position, bit `c` for channel `c` -- `kMaxChannels` is 16, so a position
  // fits a u16 exactly and no packing arithmetic is needed anywhere.
  //
  // **This is the tune's own state, not the listener's.** `Player::muted`
  // silences a channel for whoever is listening and survives a `player_start`;
  // this silences a track for a SECTION and is part of the composition, which
  // is why it lives in the module, is saved, and is undoable in an editor.
  //
  // Copied out of the file rather than pointed at, for the reason `mix` gives
  // one field up: an editor toggles these directly in the module it is about to
  // save, and a view into the file's bytes would be read-only.
  bool has_slot_mute = false;
  uint16_t slot_mute[kMaxOrders] = {};
};

// Whether channel `c` is silenced at order position `o`. **The one reader**, so
// the bit arithmetic is spelled once rather than at the player, the writer and
// every editor that draws a matrix.
//
// Out-of-range is "not muted": a caller asking about a position that is not
// there is asking about a slot that does not exist, and silence is the wrong
// answer to give it.
inline bool
slot_muted(const Module *m, int o, int c) {
  if (m == nullptr || o < 0 || o >= m->order_count || c < 0 || c >= m->channels)
    return false;
  return (m->slot_mute[o] & (uint16_t) (1u << c)) != 0u;
}

// Silence channel `c` at order position `o`, or let it sound. **The one
// writer**, and it maintains `has_slot_mute` so nothing else has to remember
// to: the flag is what decides whether a block is written at all.
inline void
slot_set_muted(Module *m, int o, int c, bool on) {
  if (m == nullptr || o < 0 || o >= m->order_count || c < 0 || c >= m->channels)
    return;
  const uint16_t bit = (uint16_t) (1u << c);
  if (on)
    m->slot_mute[o] = (uint16_t) (m->slot_mute[o] | bit);
  else
    m->slot_mute[o] = (uint16_t) (m->slot_mute[o] & (uint16_t) ~bit);
  // Recomputed rather than set, so clearing the last bit clears the flag and a
  // file that no longer needs the block stops carrying one.
  m->has_slot_mute = false;
  for (int i = 0; i < m->order_count; ++i)
    if (m->slot_mute[i] != 0u) {
      m->has_slot_mute = true;
      break;
    }
}

// The plane's row stride, and the one place it is computed. A cell is at
// `((pattern * rows + row) * module_lanes(m)) + lane`; channel `c`'s columns
// start at lane `c * fx_columns`, and the meta lanes at `channels *
// fx_columns`. **Two readers deriving this separately is how a plane and the
// patterns it belongs to end up disagreeing about which row is playing.**
inline int
module_lanes(const Module *m) {
  return m->channels * m->fx_columns + m->meta_columns;
}

struct Channel {
  int instrument = 0;        // 0 none
  // **Double rather than int, and it costs v1 nothing.** Above the period table
  // a note is 3.53 rather than 4, where one integer unit is most of a semitone;
  // below it every value is an integer no larger than 856 and every operation
  // on one is an add, a subtract or a compare, all of which are exact in double.
  double period = 0.0;       // what is sounding, after vibrato and arpeggio
  double base_period = 0.0;  // what the row asked for
  double target_period = 0.0;  // where tone portamento is heading
  float volume = 0.f;        // 0..64
  double pos = 0.0;          // sample position, frames
  double step = 0.0;         // frames per output frame
  bool playing = false;

  // The volume envelope, advanced once per rendered frame.
  //
  // **The per-millisecond frame count is carried here rather than the sample
  // rate being threaded into the mixer**, and that is what keeps
  // `channel_sample` taking the two arguments it always has: every stage's step
  // is derived from `env_fpms` and the instrument, and the rate is only known
  // where a note is triggered. Linear segments throughout — this header has no
  // libm, and a straight ramp is what a tracker does anyway.
  float env_level = 0.f;     // 0..1, the voice's amplitude
  float env_step = 0.f;      // per frame, always positive; the stage says which way
  float env_fpms = 0.f;      // sample_rate / 1000, from the trigger
  EnvStage env_stage = EnvStage::kOff;

  // Effect memory. A tracker parameter of zero means "the last one", per
  // effect and per channel, which is why each of these exists separately.
  uint8_t porta_speed = 0;
  uint8_t tone_speed = 0;
  uint8_t volslide = 0;
  uint8_t offset = 0;
  uint8_t vib_speed = 0;
  uint8_t vib_depth = 0;
  uint8_t vib_pos = 0;
  uint8_t arpeggio = 0;

  // Tremolo is vibrato applied to volume, and it is an *offset* rather than a
  // new level: the row's volume is what a slide or a Cxx changed, and tremolo
  // rides on top of it without consuming it. Kept apart from `volume` for that
  // reason, and added back in the mixer.
  uint8_t trem_speed = 0;
  uint8_t trem_depth = 0;
  uint8_t trem_pos = 0;
  float trem_offset = 0.f;

  // Which shape vibrato and tremolo trace. Low two bits pick it; bit 2 means
  // "do not restart the wave on a new note", which is what makes a held
  // vibrato continue across a retrigger rather than jumping.
  uint8_t vib_wave = 0;
  uint8_t trem_wave = 0;
  uint32_t rng = 0x2545f491u;   // for the random waveform, per channel

  // The instrument's, unless E5x overrode it for this channel.
  int8_t finetune = 0;

  // Glissando: a tone portamento that steps in semitones rather than sliding
  // through the pitches between them.
  bool glissando = false;

  // The player's half of the FXPL plane -- commands 0x30 and up, read in
  // `player_row` beside the pattern's own cells. See `kFxplAccent`.
  //
  // **Two things are called accent and they are not the same.** The SYNP
  // record's `accent` byte is the instrument's *depth*: how much an accent does
  // to this voice. This is the per-note *trigger* the row wrote: whether the
  // note that was struck is accented, and how hard. The voice multiplies them,
  // so an instrument declaring no depth is never accented whatever a row says.
  //
  // Latched at the note it is written on rather than cleared every row, or a
  // note still sounding two rows later would drop back to unaccented mid-decay.
  float accent = 0.f;         // 0..1

  // The glide. `target_period` is shared with tone portamento and only one of
  // the two ever runs on a row -- `player_row` will not set a glide up on a
  // tone porta row -- so there is exactly one thing walking the period a tick.
  //
  // **A coefficient, not a step.** Tone portamento walks the period by a fixed
  // number of units a tick, which is ProTracker's and is correct for a MOD.
  // This one is a one-pole toward the target *frequency*, so `slide_k` is the
  // fraction of the remaining interval it closes each tick. See `slide_glide`.
  double slide_k = 0.0;
  int slide_ticks = 0;        // 0 means no glide is running

  // E9x retrigger, ECx cut and EDx delay are all "do this on tick n", so they
  // are all the same shape: a tick to act on, and for delay the note that is
  // waiting for it.
  uint8_t retrig = 0;
  uint8_t cut_tick = 0;
  bool cut_pending = false;
  uint8_t delay_tick = 0;
  bool delay_pending = false;
  uint8_t delay_note = 0;

  // The synth voice. **Only what a note genuinely carries lives here**;
  // everything derivable from the instrument is derived per sample, because
  // `channel_sample` already has the `Instrument &`. What is left is the two
  // filters, the three recursive envelopes, and the three things that need a
  // sample rate the mixer never passes -- so they are computed once, at the
  // trigger, from the rate that *is* passed there.
  fx::Svf synth_body;        // the resonator that is the drum's body
  fx::Svf synth_band;        // the noise's own filter
  float synth_env = 0.f;     // drums: body VCA. the 303: the *filter* envelope
  float synth_nenv = 0.f;    // noise VCA
  float synth_pitch = 0.f;   // the sweep, 1 -> 0
  float synth_env_coef = 0.f;
  float synth_nenv_coef = 0.f;
  float synth_pitch_coef = 0.f;
  float synth_rate = 0.f;    // sample rate, captured at the trigger
  uint32_t synth_age = 0;    // frames since the trigger; the clap counts on it
  uint32_t synth_burst = 0;  // frames between a clap's re-strikes

  // The 303's, and only the 303's. Its filter is a four-pole ladder placed
  // from measured poles rather than the drums' two-pole SVF, and its amplitude
  // envelope is a *different* envelope from the one that opens that filter --
  // which on the real instrument is the whole reason the filter can still be
  // sweeping under a note that has already got quiet.
  fx::Ladder303 synth_ladder;
  float synth_venv = 0.f;      // VCA, exactly 0 once the voice is done
  float synth_venv_fast = 0.f; // its coefficient above the knee
  float synth_venv_slow = 0.f; // and below it
  float synth_acc_vca = 0.f;   // accent's filter envelope, on its way to the VCA
  float synth_acc_rc = 0.f;
};

struct Player {
  const Module *module = nullptr;
  int order = 0;
  int row = 0;
  int tick = 0;
  int speed = 6;
  int bpm = 125;
  double until_tick = 0.0;   // output frames left in this tick
  Channel channels[kMaxChannels];
  bool playing = false;
  bool loop = true;          // start the tune again rather than stopping

  // A host's loop over a range of the ORDER LIST, inclusive, or -1 for none.
  //
  // **Looping by not advancing, which is why this is not `player_seek` in a
  // wrapper.** A seek resets speed and tempo to the module's own — they are
  // things a tune *sets* with Fxx as it plays, so where they stand at a row is
  // a function of every row before it — and a loop built on one is a section
  // that will not hold its own tempo, wrong every time round. Not advancing
  // past the end touches neither.
  //
  // Cleared by `player_start`, unlike `muted`: an order index names a position
  // in one module and means nothing in the next.
  int loop_first = -1;
  int loop_last = -1;
  float gain = 0.7f;

  // **Both of these are for an editor and inert in a replay-only build.**
  //
  // A muted channel is still sampled and the result thrown away, never skipped:
  // channel_sample is what advances the position, so a channel that stopped
  // being read would freeze and then jump back in when it unmuted.
  bool muted[kMaxChannels] = {};

  // False mixes whatever the channels are already sounding without advancing
  // the sequencer -- which is what auditioning an instrument against a stopped
  // tune is. player_start sets it; only player_preview clears it.
  bool sequencing = true;

  // **Ticks since `player_start`, and the only monotonic clock the sequencer
  // has.** `tick` is emphatically not one: a pattern delay pins it at
  // `speed - 1` and returns (see `player_tick`), so six held ticks all report
  // the same number — and a one-row loop at speed 1 never moves `tick`, `row`
  // or `order` at all. Anything outside the player that needs to know a tick
  // passed has to read this instead of inferring it, or it silently drops every
  // update during exactly the passages where they are most audible.
  //
  // Nothing downstream of this counter produces audio, so it cannot move a
  // fingerprint. It going *backwards* is a free signal that the player was
  // restarted or seeked.
  uint64_t ticks_elapsed = 0;

  // Where each channel sits, -1 hard left to +1 hard right, scaled by
  // `separation`. player_start fills these with ProTracker's LRRL, which is
  // the arrangement the tunes were written for.
  //
  // Reset by player_start, unlike `muted`: panning is musical data that a tune
  // may rewrite as it plays, where a mute is a monitoring control belonging to
  // whoever is listening.
  float pan[kMaxChannels] = {};

  // 0 collapses to the mono the player produced before panning existed; 1 is
  // the Amiga's hard separation, which is fatiguing on headphones and loses a
  // channel entirely when summed to mono.
  float separation = 0.7f;

  // Set when a row asked to jump, applied once the row has finished — a break
  // and a jump on the same row both count, and the order they are written in
  // must not decide the result.
  int break_row = -1;
  int jump_order = -1;

  // EEx holds the row for extra ticks. Counted in ticks rather than rows so the
  // effects on the row go on running through the delay, which is what the
  // effect is for; retriggering the notes instead would make it a stutter.
  int extra_ticks = 0;

  // E6x, pattern loop. One per player rather than per channel: modules that use
  // it put it on one channel, and a per-channel loop point is a different
  // effect that no tracker of this era actually had.
  int loop_row = 0;
  int loop_count = 0;
};

// Whether channel `c` should be dropped where the player currently is -- the
// listener's mute, or the tune's own for the section it is in.
//
// **The one answer, because there are two render loops.** `render_add` in
// ntrk.h and `mixer_render_add` in ntrk_mix.cc each walk the channels and each
// used to spell the mute test for itself; adding the slot mute to one of them
// made it work through the plain renderer and do nothing through the mixer,
// which is the path every host with sends actually takes. A rule with two
// implementations is a rule that holds in one of them.
//
// The CALLER samples first and then drops, in both loops: `channel_sample` is
// what advances the position, so a channel that stopped being read would freeze
// and jump back in when it came off mute. This answers what to drop, never
// whether to sample.
inline bool
channel_silent(const Module *m, const Player *p, int c) {
  if (p != nullptr && c >= 0 && c < kMaxChannels && p->muted[c])
    return true;
  return slot_muted(m, p != nullptr ? p->order : -1, c);
}

}  // namespace ntrk

#endif  // NTRK_TYPES_H_
