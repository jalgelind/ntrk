// ntrk_mix -- a channel strip and a master bus over the ntrk player.
//
// The API and the reasoning are here; the arithmetic is in `ntrk_mix.cc`.
// Compile that file alongside your own, or `#include "ntrk_unity.h"` for a
// single-translation-unit build -- both are supported and both are tested, see
// README.md.
//
// `render_add` is the whole mixer ntrk ships: pan, gain, clip. This is the
// other caller `player_run_begin` was separated out for -- it drives the same
// sequencer and replaces the mix stage entirely, so a tune gets a per-channel
// insert, four effect sends, a stereo width control and a limiter without the
// player knowing anything about any of it.
//
//     ntrk::mix::Mixer mixer;                 // wherever your state lives
//     ntrk::mix::mixer_reset(&mixer);
//     ntrk::fx::reverb_init(&mixer.send[0].reverb, mem, bytes, 48000.f, 0.f);
//     mixer.send[0].param[0] = 0.7f;          // size, normalised like all of them
//     mixer.send[0].param[4] = 1.f;           // mix -- a send is all wet
//     ntrk::mix::slot_set_kind(&mixer.send[0], ntrk::mix::FxKind::kReverb);
//     mixer.send_level[1][0] = 0.25f;         // channel 1 into send 0
//     ntrk::mix::mixer_render_add(&mixer, &player, buf, frames, 2, 48000.f);
//
// **The strip is `channel_sample -> instrument filter -> insert -> send taps ->
// pan -> master`, and the insert is pre-pan and mono.** An Amiga voice is one
// mono stream, so a stereo insert would run the same filter twice over two
// identical signals; a send tapped there is one multiply per send rather than
// two; and "the insert is off" is then a single branch rather than an argument
// about two channels staying identical. The cost is that a mono send arrives at
// the reverb centred, whatever the channel's pan -- which is what a send has
// always been.
//
// **The v2 effect plane is read here too, and for the same reason.** A send
// level and a master gain mean nothing to a replayer, so `player_run_begin`
// never learns what one is: the plane is public data on `Module`, and this
// walks it a tick at a time from out here, writing pan, gain, cutoff and the
// four send levels. See the FXPL section below -- and note that a module with
// no plane runs none of it, which is what keeps the claim two paragraphs down
// exact.
//
// The instrument's own filter is ahead of the insert because it is part of the
// voice: it is what the v2 file asked for, and the insert is the user's effect
// on top of that. A caller using `render_add` alone gets no filter at all, and
// that is honest rather than a gap -- `svf_set` wants a tan() and ntrk.h
// includes <stddef.h> and <stdint.h> and nothing else. The replayer replays.
//
// **Nothing here allocates.** The scratch buffers are inside `Mixer` and the
// delay and reverb lines are the caller's, exactly as those effects require.
// A `Mixer` is about 25 KB and a stereo reverb tank about 107 KB at 48 kHz with
// the 0.1 s of predelay the header's example asks for -- 136 KB at 0.25 s, since
// the predelay is the part that scales. Neither belongs on a render thread's
// stack: put the struct in your mode's state and the tank in your pool.
//
// Deterministic on ARM64 and wasm, the same terms as everything else here: no
// libm in the audio path, no fast-math, no reliance on flush-to-zero, no
// oversampling. Denormals are handled by the effects themselves.
//
// **Everything is a provable no-op when it is off.** With every send level at
// zero, every insert and the master effect at `FxKind::kNone`, `width == 1`
// and no instrument carrying a filter flag, no effect code runs at all and the
// output is the pan and gain path alone --
// which, at `master_gain == player->gain`, is bit for bit what `render_add`
// produces below the limiter's knee. A default-constructed `Slot` is also an
// identity whatever its kind, because every effect's off position is its
// zero: drive 0, mix 0, cutoff 0.
//
// Public domain / CC0. Written for the no2 project.

#ifndef NTRK_MIX_H_
#define NTRK_MIX_H_

#include "ntrk.h"
#include "ntrk_fx_delay.h"
#include "ntrk_fx_reverb.h"
#include "ntrk_fx_shape.h"

#include <stddef.h>
#include <stdint.h>

namespace ntrk {
namespace mix {

// Four is what fits the parameter budget of a tracker-shaped UI and is already
// more than any tune here uses. Sends are stereo buses because reverb and delay
// produce stereo from a mono input; the *taps* into them are mono.
const int kSends = 4;

// ----------------------------------------------------------------------------
// -- Effect slots
// ----------------------------------------------------------------------------

// **Neither of these crosses the format**, so both are the type of the field
// that holds them rather than a number stored in one. What a slot holds is
// caller configuration, and the plane addresses a slot's *parameters* by index
// without ever naming its kind -- see `kFxplSlotSet`. `uint8_t` all the same,
// so a `Slot` costs what it did.
enum class FxKind : uint8_t { kNone = 0, kShape, kFilter, kDelay, kReverb };

enum class FilterMode : uint8_t { kLowpass = 0, kHighpass = 1, kBandpass = 2 };

// **Side by side, not a union.** `Delay` and `Reverb` have default member
// initialisers, which makes them illegal C++11 union members, and the saving
// would be illusory anyway: two `Svf` at 24 bytes, a `Delay` at 56 and a
// `Reverb` at 568 come to 672 side by side against 568 for the largest alone.
// That is 104 bytes -- and it stays 104 whatever the `Reverb` grows to, since
// the difference is the other three -- against the ~107 KB tank the `Reverb`
// points at in the caller's own memory. Not a trade worth thinking about twice.
//
// `param` is eight floats whatever the kind, so saving a slot, automating one
// or interpolating between two is the same code for every effect.
//
// **Every one of them is normalised 0..1, and the effect's own setter scales
// it.** A pattern command is a `u8` and the format is opaque about what a slot
// holds, so a byte cannot mean seconds here and hertz there; the unit has to
// live at the one end that knows the kind. `*_set` in ntrk_fx_*.h keeps its
// real units for a caller reaching past the slot layer -- it is the *slot* that
// normalises, not the effect.
//
//   FxKind::kShape   0 kind (4 ways)   1 drive         2 dry
//   FxKind::kFilter  0 mode (3 ways)   1 cutoff (exp, 20 Hz..20 kHz; 0 is off)
//                                2 resonance
//   FxKind::kDelay   0 time (x kSlotDelayMaxSeconds)   1 feedback  2 damping
//                                3 mix           4 ping-pong (>0.5)
//   FxKind::kReverb  0 size            1 damping (HF)  2 predelay
//                                  (x kSlotPredelayMaxSeconds)
//                                3 width         4 mix
//                                5 decay         6 damping LF   7 diffusion
//
// A discrete parameter is a band of the same 0..1, through `slot_choice` --
// which is what lets a slide sweep a shaper through its four kinds without the
// plane needing to know that this one is not continuous.
//
// The trailing indices are spare on purpose: an effect that grows a knob must
// not move the meaning of the ones a caller already stores.
//
// **A knob added to a spare index must read its own zero as the behaviour the
// slot had before it existed**, because zero is what every `Slot` and every
// module written before it holds there. That is the rule `slot_filter_on`
// already states for a cutoff, and the four knobs added since are wired to it:
//
//   kShape 2   is *dry*, not wet, so zero is the fully wet shaper a send has
//              always been -- and unlike a sentinel it leaves the whole range
//              reachable, including a fully dry bypass at 1.
//   kReverb 5,6,7  zero means "as `fx::reverb_derived` derives it from size",
//              the same three values the five-knob `reverb_set` supplies. A
//              decay of exactly 0 is the only setting this costs, and it is one
//              parameter step from 1/255.
struct Slot {
  FxKind kind = FxKind::kNone;
  float param[8] = {};

  // **What a plane resync restores `param` to.** `fxpl_sync` re-seeds the
  // plane's own accumulators from the knobs they write, and a slot parameter
  // has no such knob to read back -- so without this a pattern that automated
  // a slot would eat the caller's configuration permanently, and a seek
  // backwards would not give it back. Written by `slot_set_kind`, so the
  // documented "set the parameters, then choose the kind" order is all a
  // caller needs; see `slot_hold` for the other order.
  float base[8] = {};

  // The last `param` handed to the effect's setter, and the kind and rate it
  // was handed at. `slot_process_*` calls `slot_apply_params` unconditionally,
  // so an unautomated slot re-derives the same coefficients every block --
  // a tan() per filter, a predelay round trip per reverb -- for nothing.
  //
  // `applied_kind` starts at an impossible kind rather than the comparison
  // resting on `applied` alone: a slot whose parameters are all zero would
  // otherwise match on the first apply after a kind change, and the new effect
  // would run on the old one's coefficients.
  float applied[8] = {};
  int   applied_kind = -1;
  float applied_rate = 0.f;

  // Two filters, not one: a stereo pair needs its own state per channel, and
  // `svf_lowpass` runs the whole state update per call -- pushing left and then
  // right through one instance advances it twice a frame and folds each channel
  // into the other's history. The mono paths use `svf[0]` and leave `svf[1]`
  // untouched.
  fx::Svf    svf[2];
  fx::Delay  delay;    // caller must delay_init it; uninitialised is a passthrough
  fx::Reverb reverb;   // likewise reverb_init
};

// How far a normalised delay time and reverb predelay reach. **Powers of two,
// and that is not cosmetic**: a caller storing a real-unit setting divides by
// these to get the normalised one, and a division by a power of two is exact --
// so a tune pinned to a fingerprint keeps the sample-for-sample delay time it
// was blessed with.
const float kSlotDelayMaxSeconds = 2.f;
const float kSlotPredelayMaxSeconds = 0.25f;

// A normalised parameter to one of `n` choices, `n` bands of equal width. 1.0
// lands on the last choice rather than one past it, so `param[p] = 1.f` is a
// legal way to say "the last one". NaN and negatives land on 0 -- `(int)` of a
// NaN is undefined behaviour, and the guard is written so a NaN fails it.
int slot_choice(float v, int n);

// The inverse, for a caller naming a choice: the centre of its band, so neither
// end of the rounding can fall out of it.
float slot_choice_param(int i, int n);

// Selects the effect and drops whatever the previous one was still ringing, so
// switching kinds cannot leak a tail from an effect that is no longer there.
// The parameters are left alone -- they are the caller's, and a slot is often
// configured before it is switched on. The delay and reverb buffer pointers
// survive: resetting a line is emptying it, not forgetting where it is.
//
// **It also takes `param` as the slot's configuration**, which is what a plane
// resync restores. So the order in this header's example -- parameters, then
// `slot_set_kind` -- is the one that works; a parameter written after it is a
// parameter the first resync throws away unless `slot_hold` follows.
void slot_set_kind(Slot *s, FxKind kind);

// Takes `param` as the configuration without disturbing the effect. For a
// caller that has to set a parameter after choosing the kind, or that changes
// one while a planed module is playing and means it to survive a seek.
inline void
slot_hold(Slot *s) {
  for (int i = 0; i < 8; ++i)
    s->base[i] = s->param[i];
}

// The other direction: throws away whatever a pattern automated and puts the
// configuration back. This is what a plane resync does, and what makes a seek
// backwards restore a slot instead of leaving it wherever the rows it skipped
// had pushed it.
inline void
slot_revert(Slot *s) {
  for (int i = 0; i < 8; ++i)
    s->param[i] = s->base[i];
}

// A cutoff of zero is off rather than the bottom of the range. `svf_set` clamps
// up to 5 Hz, which is the right thing for a swept knob and the wrong thing for
// a default-constructed slot: switching a channel to FxKind::kFilter and
// setting no parameters would silence it, and that looks like the filter
// being broken.
bool slot_filter_on(const Slot *s);

float slot_filter_sample(fx::Svf *f, FilterMode mode, float x);

// Pushes the slot's parameters into the effect's own setter. Once per block,
// not once per sample: `svf_set` costs a tan(), and a run is short enough
// (kMaxBlock frames) that stepping a parameter at its boundary is smooth.
void slot_apply_params(Slot *s, float rate);

void slot_process_stereo(Slot *s, float *l, float *r, int frames, float rate);

// **Only FxKind::kShape and FxKind::kFilter, and the others are refused rather
// than approximated.** The obvious way to run a stereo effect over one mono
// buffer is to pass it as both channels, and that is wrong twice over:
// `delay_process` would read and write the same array through `left` and
// `right`, so the second half of every sample's work overwrites the first, and
// the feedback line would be fed each sample twice. A silent refusal leaves the
// signal alone, which is the same thing every other "off" in this library does.
void slot_process_mono(Slot *s, float *m, int frames, float rate);

// ----------------------------------------------------------------------------
// -- The instrument's own filter
// ----------------------------------------------------------------------------
//
// **It lives in the mixer because it cannot live in the player.** `svf_set`
// calls `std::tan`, and ntrk.h includes <stddef.h> and <stdint.h> and nothing
// else by contract. So `render_add` renders a v2 module unfiltered and this
// renders it as the instrument asks: the replayer replays, and a caller who
// wants what the file says buys the mixer.
//
// Instrument flags bit 1 switches it on; bits 2-3 pick the type.

// **The notch is derived, not a fourth tap on the filter.** ntrk_fx_shape.h
// offers lowpass, highpass and bandpass, and a notch is exactly what is left
// when the bandpass is taken out of the input: LP + HP == x - k*BP, with the
// same `k` the highpass already needs. One `svf_update` per sample either way,
// which is the rule that header states -- so this is a subtract rather than a
// reason to widen somebody else's module.
//
// **`type` is the instrument's two-bit field and not a `FilterMode`.** That
// enum has three values; this has four, because the notch is one of them here
// and is not an SVF tap. Typing this parameter would mean either a fourth
// `FilterMode` the filter cannot serve or a cast that lies about which of the
// two lists the number came from, so it stays the field's own width. 0..3 is
// the whole domain and a two-bit field cannot leave it.
float voice_filter_sample(fx::Svf *f, int type, float x);

// ----------------------------------------------------------------------------
// -- The effect plane (FXPL)
// ----------------------------------------------------------------------------
//
// **The mixer owns the plane, and the player never learns what a send is.** A
// send level and a master gain have no meaning in a replayer, so `render_add`
// ignores the block; the plane is public data on `Module` and this reads it
// from out here. A module whose `fx` is null -- any file without the FXPL
// block -- runs none of the code below, which is what keeps the bit-identity
// claim at the top of this header exact rather than approximate.
//
// A command this reader does not know is ignored
// rather than guessed at: a plane written by a later writer must not make an
// older one invent a meaning for it. Parameters are clamped, never refused.
//
// **The plane's minor dimension is lanes, not channels** -- `module_lanes()` in
// ntrk_types.h, the one place it is computed. Channel `c` owns the contiguous
// run of `fx_columns` lanes starting at `c * fx_columns`, and the `meta_columns`
// whole-row lanes follow every channel lane.
//
// **Stored last, evaluated first**, and the two orders differ on purpose: a
// macro on a meta lane can sweep one parameter across four channels, and a
// channel's own column then trims its own. Meta-last would make that trim
// inexpressible, because a set is last-lane-wins. Order decides *sets* only --
// deltas sum, and a sum does not care.
//
// **The one enum here that is deliberately not scoped**, and the reason is that
// it does not describe its own domain. `FxCell::cmd` is a byte with a far wider
// range than this list: 0x30..0x3F belong to the player, 0x40..0xBF are slot
// set and slide, and none of those are `FxplCmd` values at all. Every reader
// below is a range test on that byte -- `cmd >= kFxplSendSet && cmd <
// kFxplSendSet + kSends` -- or a switch whose default case is "a command this
// reader does not know". Scoping it would type the *nine* names and then cast
// at every one of those tests and at every `+ kSends`, and each cast would put
// a byte that is very often not one of these inside a type claiming it is. The
// arithmetic is the interface here; a scoped enum would hide it behind casts
// without making one comparison safer.
enum FxplCmd {
  kFxplPanSet      = 0x01,
  kFxplPanSlide    = 0x02,
  kFxplGainSet     = 0x03,
  kFxplGainSlide   = 0x04,
  kFxplCutoffSet   = 0x05,
  kFxplCutoffSlide = 0x06,
  kFxplResSet      = 0x07,
  kFxplResSlide    = 0x08,
  kFxplTypeSet     = 0x09,
  kFxplSendSet     = 0x10,        // 0x10..0x13, one per send
  kFxplSendSlide   = 0x14         // 0x14..0x17
};

// One past the highest command carrying a "parameter zero means the last one"
// memory, which is what that table is sized on. **A slot command is past it and
// so carries no such memory**: zero is a legitimate value for a normalised
// parameter and a zero slide step is a legitimate way to hold one still, so
// there is nothing for "last one" to fill in that absence does not already say.
const int kFxplCmdCount = 0x18;

// ----------------------------------------------------------------------------
// -- Slot parameters in the plane
// ----------------------------------------------------------------------------
//
// **Flat, with the pair packed into the command byte**, so a cell is stateless:
//
//   0x40 + (slot << 3 | param)   set    the parameter to `cell.param / 255`
//   0x80 + (slot << 3 | param)   slide  it by `(int8_t) cell.param / 255` a tick
//
// A stateful "select the slot, then set it" pair and a two-cell encoding are
// both refused, and for the same reason: the mixer reconstructs the plane from
// `Player::ticks_elapsed` and re-seeds on a resync, so a selector would have to
// join that snapshot, and a seek landing between the two cells of a pair would
// be undefined. Decode is `(cmd - 0x40) >> 3` and `& 7`, and set against slide
// is one bit.
const int kFxplSlotSet = 0x40;
const int kFxplSlotSlide = 0x80;
const int kFxplSlotEnd = 0xc0;

// Which slot a command names. **`kSlotInsert` means the lane's own channel**,
// which is why no channel is encoded anywhere: a channel lane already says
// which one, and a macro says it through `scope`.
const int kSlotSend = 0;          // 0 .. kSends-1
const int kSlotMaster = 4;
const int kSlotInsert = 5;
const int kSlotCount = 8;         // 6 and 7 are spare and are ignored at use

// **Where a slot command has a referent, and it is a slot-id range check rather
// than an effect-semantics one** -- so it costs the format none of its opacity:
//
//   channel lane   only kSlotInsert. A send or the master written from four
//                  channel lanes is the master-gain wart: four columns each
//                  sliding one reverb size sum four deltas into one value.
//   meta lane      sends and the master. A meta lane is the whole row, so it
//                  writes a global thing once. kSlotInsert names no channel
//                  there and is ignored; a macro with scope 0xFF is how an
//                  insert is reached from a global place.
//   macro target   sends and the master once, whatever the scope -- a global
//                  slot is not per channel. kSlotInsert walks `scope` as every
//                  other target does.
bool slot_is_global(int slot);

// Splits a slot command. False for anything that is not one, which is every
// command outside 0x40..0xBF.
bool fxpl_slot_decode(int cmd, int *slot, int *param, bool *slide);

// **`Slot::kind` is deliberately not reachable from a pattern, and it is the
// obvious next request.** Two reasons, and the second is the hard one: a kind
// change resets the effect's state, so a pattern could drop a reverb tail on
// any row; and the delay line and the reverb tank are the *caller's* memory,
// laid out by `delay_init`/`reverb_init` for the kind that was chosen then. A
// row that switched a shaper to a reverb would point the tank at whatever the
// caller happened to provide, which for a slot that never held a reverb is
// nothing at all. Automate the parameters; choose the kind from the host.

// The plane's slidable values, held in the command's own 0..255 parameter space
// rather than in the units they end up in. **A slide accumulates where the
// command is written, not where it lands**: a cutoff slide adding hertz would
// crawl at the bottom of the range and leap at the top, which is the one thing
// an exponential control exists to avoid.
//
// These are the index space of `Mixer::fxpl_val`, which is public, so they stay
// here rather than moving to the .cc with the conversions that read them.
const int kFxplPan = 0;
const int kFxplGain = 1;
const int kFxplCutoff = 2;
const int kFxplRes = 3;
const int kFxplSend = 4;          // 4 .. 4 + kSends - 1
const int kFxplValues = kFxplSend + kSends;

// **A macro target below 0x40 is an index into this array and nothing else, so
// the two counts are one number written twice.** `ntrk_types.h` is the bottom
// of the dependency order and cannot see this header, so the loader
// range-checks a target against its own copy; this is the only place that sees
// both, and without it the two drift apart into a file whose targets index
// `fxpl_val` out of bounds.
//
// Widening the target byte to reach slot parameters does not move this number.
// A slot parameter has an accumulator of its own -- `Slot::param`, which
// persists across ticks exactly as `fxpl_val` does -- so it never enters this
// array and the two ranges stay disjoint.
static_assert(kFxplValues == kFxplValueCount,
              "a macro target below 0x40 indexes fxpl_val; the loader checks "
              "the copy in ntrk_types.h");

float fxpl_clamp(float v);

// 128 is centre, and both ends reach the speaker rather than stopping just
// short of it: 0 maps past -1 and is clamped there, which costs one parameter
// step at each end and buys a command that can say "hard left" with a round
// number.
float fxpl_to_pan(float v);

// 128 is unity, so the command reaches a shade under +6 dB. The limiter is what
// stands behind that; the gain is staged before the master effect either way.
float fxpl_to_gain(float v);

// Sends and resonance are plain 0..1. Neither has a unity to centre on, and
// resonance agreeing with the instrument entry's own `filter_res / 255` is
// worth more than symmetry with pan.
float fxpl_to_unit(float v);

// 20 Hz to 20 kHz, exponential so equal steps are equal intervals. The table it
// interpolates is in the .cc, with why it is a table and not `std::pow`.
float fxpl_to_cutoff(float v);

// Which value a slide command moves, or -1 for a command that is not a slide.
int fxpl_slide_index(int cmd);

// The player's own balance law, unity in the centre so a centred tune is not
// quietly 3 dB down against the renderer it replaced. Hoisted per run rather
// than per frame -- and recomputed per run only when a plane exists to move it.
void mixer_pan_gains(const Player *player, bool stereo, float *gain_l,
                     float *gain_r);

// ----------------------------------------------------------------------------
// -- Gain staging and the limiter
// ----------------------------------------------------------------------------
//
// The headroom, the ceiling, the knee and the release are in the .cc: nothing
// outside the mixer reads them, and each carries the reasoning for its number.

// A static soft knee, bounded by construction: `soft_clip` never exceeds 1, so
// the return never exceeds the ceiling, and it reaches the ceiling exactly once
// the input is three knee-widths past the knee (about +2.1 dBFS). `soft_clip`
// rather than tanh() because it is arithmetic -- libm is not bit-identical
// across platforms and this output gets hashed.
float limiter_curve(float peak);

float limiter_ceil(float x);

// ----------------------------------------------------------------------------
// -- The mixer
// ----------------------------------------------------------------------------

struct Mixer {
  // Zero everywhere, so a fresh mixer sends nothing. An effect a caller has not
  // asked for must not appear in the mix because the struct defaulted into it.
  float send_level[kMaxChannels][kSends] = {};
  Slot  send[kSends];

  Slot  insert[kMaxChannels];

  // The instrument's filter, one per channel and one filter each: a voice is a
  // single mono stream, so there is no second state to keep the way a stereo
  // slot needs. Untouched -- not even `svf_set` -- for an instrument with no
  // filter flag, which is what keeps a module that asks for nothing bit for bit
  // the unfiltered path.
  fx::Svf voice_filter[kMaxChannels];

  // Where each channel's sample position and synth age were left at the end of
  // the last run, which is how a note trigger is spotted from out here. Two,
  // because one of them holds still for a voice that has no sample to walk.
  // See the use site.
  double voice_pos[kMaxChannels] = {};
  uint32_t voice_age[kMaxChannels] = {};

  // -- The effect plane's state. **Not one field of it is touched for a module
  // whose `fx` is null**, which is the whole of the bit-identity guarantee; see
  // `fxpl_run` for the rest.
  //
  // Values in parameter space, 0..255. `fxpl_write` puts each one through to
  // wherever it actually lives -- `player->pan`, `master_gain`, `send_level` --
  // bar the two the voice filter reads back from here directly.
  float    fxpl_val[kMaxChannels][kFxplValues] = {};

  // "Parameter zero means the last one", per channel and per command, the same
  // memory every slide in the player already carries. Indexed by the command
  // number itself rather than through a mapping: 24 bytes a channel, against a
  // second table that could get out of step with the enum.
  uint8_t  fxpl_mem[kMaxChannels][kFxplCmdCount] = {};

  // What the row put in each of this channel's cells, latched on its tick 0 and
  // used by every tick after it. **Latched rather than re-read**, because the
  // tick that finishes a row is rendered after the sequencer has already
  // stepped past it -- so by then the cell at (order, row) is the next row's.
  //
  // Per column, because each column slides on its own: two columns stepping a
  // parameter opposite ways have to both be in flight for the tick that cancels
  // them to happen at all.
  uint8_t  fxpl_cmd[kMaxChannels][kMaxFxColumns] = {};
  uint8_t  fxpl_param[kMaxChannels][kMaxFxColumns] = {};

  // The same latch for the meta lanes: which macro a lane fired and the input
  // byte it fired with. A delta macro is a slide and needs its invocation to
  // survive the row's first tick exactly as `fxpl_cmd` does.
  uint8_t  fxpl_meta_cmd[kMaxMetaColumns] = {};     // one-based; 0 is no macro
  uint8_t  fxpl_meta_param[kMaxMetaColumns] = {};

  // The plane's filter override: cutoff, resonance and type for a channel,
  // switched on by the first of those commands to arrive. It takes the place of
  // the instrument's own setting rather than sitting beside it -- one SVF per
  // channel is one SVF, and a second in series would be a filter the format
  // never asked for.
  uint8_t  fxpl_type[kMaxChannels] = {};
  bool     fxpl_filter[kMaxChannels] = {};

  // `Player::ticks_elapsed` as of the last tick applied. **The only clock the
  // plane has, and (order, row, tick) is emphatically not one**: a pattern
  // delay pins `tick` at `speed - 1` and returns, so a dozen held ticks report
  // the same triple; at speed 1 on a one-row loop none of the three ever moves;
  // and a seek to the row already playing is invisible in all three.
  uint64_t fxpl_seen = 0;
  bool     fxpl_synced = false;

  // -- External macro input: a macro invoked from outside the pattern.
  //
  // A game parameter is a macro whose input byte came from the host rather than
  // from a meta-lane cell, which is the whole of the mechanism -- the format
  // gains nothing and there is no second automation system. `mixer_set_macro`
  // writes these; `fxpl_row` applies and clears them.
  //
  // **A mask, and it is what keeps the guarantee.** With nothing pending,
  // `fxpl_row` reads one integer and touches nothing -- the same bit-identity a
  // null `fx` gets. A per-macro "changed" flag scanned every row would cost 32
  // reads to say the same thing.
  //
  // Latched rather than applied on the spot because a mixer is not a sequencer:
  // applying mid-row would put a parameter change on the audio thread's block
  // schedule instead of the music's.
  uint8_t  macro_in[kMaxMacros] = {};
  uint32_t macro_pending = 0;     // bit i is macro i+1, one-based as a cell is
  static_assert(kMaxMacros <= 32,
                "macro_pending is a uint32_t mask, one bit per macro");

  // Replaces `Player::gain`; the player's own is not applied here, or the two
  // would multiply and the tune would come out at half the level the player
  // renders it. Defaulted to the same 0.7 for exactly that reason.
  float master_gain = 0.7f;

  // 1 keeps the mix as panned, 0 folds it to mono, above 1 pushes the sides out
  // past the speakers. Exactly 1 is skipped rather than computed: the mid/side
  // round trip is algebraically the identity and is not one in floating point.
  float width = 1.f;

  Slot  master_fx;

  // The limiter's whole state. One gain for both channels -- a per-channel gain
  // shifts the stereo image every time one side goes loud.
  float limit_gain = 1.f;

  // **Scratch, and it lives here rather than on the stack.** A render thread
  // gets 512 KB on macOS and every one of these is written before it is read
  // within a run, so none of them needs clearing between calls.
  float voice[kMaxChannels][kMaxBlock];
  float send_bus[kSends][2][kMaxBlock];
  float mix_l[kMaxBlock];
  float mix_r[kMaxBlock];
};

// The slot a command names, or null where it has no referent. `channel` is the
// lane's own channel and is read only by `kSlotInsert`.
Slot *slot_at(Mixer *mx, int slot, int channel);

// Drops every tail and returns the limiter to unity gain. Settings are kept:
// this is the "stop the sound" call, not the "forget the patch" one -- so a
// slot the plane had automated goes back to its `base`, which is the setting.
void mixer_reset(Mixer *mx);

// ---- MIXR, the mixer's half of the file ------------------------------------
//
// **The two things the effect plane cannot say.** A slot's *kind* is host-only
// by design -- a row that swapped a shaper for a reverb would point the tank at
// memory laid out for something else -- and the stereo width has no command at
// all. Master gain and the slot parameters *are* plane commands and are saved
// as automation already; they are in the block as well because a block is the
// starting state and automation is what moves from it. A tune whose master sits
// at 1.9 should not need a command on row 0 to say so.
//
// The layout is in `ntrk_types.h`, where the loader can length-check it without
// seeing this header. These pin the two ends together.
static_assert(kMixrSlots == kSends + 1, "MIXR carries the sends, then the master");
static_assert(kMixrSlotParams == 8, "a slot has eight parameters");

// The slot a MIXR index names: 0..kSends-1 are the sends, kSends is the master.
// Inserts are not in the block.
// ponytail: no insert slots. They are per channel and nothing has wanted one
// saved yet; widening `kMixrSlots` and bumping `kMixrVersion` is the way in.
Slot *mixr_slot_at(Mixer *mx, int index);

// Read `module->mix` into the mixer. False, touching nothing, if the module
// carries no block or the block names an effect this build does not have.
//
// **Parameters land before the kind**, which is the order `slot_set_kind`
// documents: it is where `Slot::base` is taken, and a parameter written after
// it is one the first plane resync throws away.
//
// The caller still owns the delay line and the reverb tank. A kind whose memory
// was never laid out is a passthrough rather than a fault, so this may be
// called before or after `delay_init`/`reverb_init` -- but the tune is silent
// on that send until it has been.
// Read `module->mix` into the mixer — the slots, the master, the width, and from v2 the
// per-channel send levels. **The levels land in `send_level[c][s]`**, which is where the
// plane's own `kFxplSend` command writes and where the render loop reads: the block is the
// starting state and automation moves from it, exactly as the master gain does.
bool mixer_config_read(Mixer *mx, const uint8_t *block);

// What kind of effect a saved slot names, WITHOUT building a mixer to find out.
//
// **An editor needs this and a `Mixer` is the wrong price for it.** Drawing five
// squares that say which sends a module uses would otherwise mean instantiating
// one -- four reverb tanks and four delay lines, megabytes, on the UI thread --
// to read five bytes back out of it.
//
// Answers `kNone` for a block that is absent, malformed, or names an effect this
// build does not have, which is the same refusal `mixer_config_read` makes: a
// caller that drew "reverb" for a block the player will reject would be
// describing a tune nobody can hear.
FxKind config_slot_kind(const uint8_t *block, int slot);

// One saved parameter of a saved slot, 0..1, without building a mixer.
//
// The sibling of `config_slot_kind` and for the same reason: an editor drawing
// a slot's eight knobs would otherwise instantiate a `Mixer` to read sixteen
// bytes back out of it. Answers 0 for a block the reader would refuse, so an
// editor cannot draw a value out of a file the player will not load.
float config_slot_param(const uint8_t *block, int slot, int param);

// How much of `channel` reaches `send`, 0..1 — the number `mixer_render_add` multiplies a
// voice by on its way to the bus, and the one thing a send needs that the block could not
// say before v2. Zero for a block that does not carry them, which is what a v1 file meant.
float config_send_level(const uint8_t *block, int channel, int send);
void config_set_send_level(uint8_t *block, int channel, int send, float v);

// Whether any level is non-zero. The writer asks this to choose the version, so a module
// that feeds no send is written as v1 and an untouched file keeps its bytes.
bool config_has_send_levels(const uint8_t *block);

// The master gain and the stereo width the block carries, 0..1 and 0..1.
float config_master_gain(const uint8_t *block);
float config_width(const uint8_t *block);

// ---- and the editor's half of the same ------------------------------------
//
// **Symmetric with the readers above, and for the identical reason.**
// `mixer_config_write` takes a `Mixer` -- four reverb tanks and four delay
// lines, megabytes -- which is exactly the price `config_slot_kind` exists to
// avoid. An editor changing one knob should not instantiate a mixer to do it.
//
// Each refuses a block `mixer_config_read` would refuse, touching **nothing**:
// a half-written block is a file the player rejects whole, so there is no
// partial success worth having. A value out of range is CLAMPED rather than
// refused, on the same terms `instrument_param_set` gives -- a bool return would
// hand the decision to every caller and one of them would drop it.

// A valid, empty block: version, slot count, and unity master gain and width.
// **For the modules that carry no mixer at all**, which is most of them --
// without this, "configure the sends" is impossible for every file that never
// had one. False only for a null pointer.
bool config_init(uint8_t *block);

// A slot's kind. Refused (not clamped) past `kReverb`, which is the same ceiling
// `config_readable` enforces: a set that left a block this file would refuse to
// read back would be a mixer the editor can see and the player cannot.
void config_set_slot_kind(uint8_t *block, int slot, FxKind kind);

// One saved parameter, 0..1, clamped.
void config_set_slot_param(uint8_t *block, int slot, int param, float v);

// The master gain and the stereo width, clamped to what Q12 can carry.
void config_set_master_gain(uint8_t *block, float v);
void config_set_width(uint8_t *block, float v);

// Where a knob starts. The sibling of `fx_param_name` -- what a knob is called and where it
// starts are two halves of one question, and an editor's own table of starting points would
// be the half that goes stale when an effect grows a knob.
//
// `wet` is whether the slot's output stands alone: a SEND carries only the effect and the dry
// path is the channel itself, while an insert at full wet replaces what it is inserted into.
// The only default that depends on where a slot sits rather than on what is in it.
float fx_param_default(FxKind kind, int param, bool wet);

// How many values a parameter really has, or 0 when it is continuous.
//
// **Three of them are not knobs.** A shaper's Kind picks one of four curves, a filter's Mode
// one of three, and a delay's Ping-pong is on or off — `slot_apply` puts each through
// `slot_choice`, so the byte between the steps means nothing and an editor dragging them as
// 0..1 shows "Mode 0.33" while the filter changes type at invisible thresholds.
//
// The counts are the ones `slot_apply` itself passes, so this reads the decode rather than
// offering a second opinion about it.
int fx_param_choice_count(FxKind kind, int param);

// What choice `i` is called, or null when the parameter is continuous or `i` is out of range.
// The filter's four words are `filter_mode_names`', not a copy.
const char *fx_param_choice_name(FxKind kind, int param, int i);

// Put every parameter of `slot` at its kind's default, taking `wet` from the slot's own
// position. **What a slot should be given when its kind CHANGES** -- a reverb switched on with
// every knob at zero is a reverb nobody can hear, and the eight bytes are shared across kinds,
// so a delay's Time would otherwise arrive as a reverb's Size.
void config_seed_slot(uint8_t *block, int slot);

// The same, for a module: `kNone` when it carries no `MIXR` block at all.
inline FxKind
config_slot_kind(const ntrk::Module *module, int slot) {
  return module != nullptr && module->has_mix ? config_slot_kind(module->mix, slot)
                                              : FxKind::kNone;
}

inline float
config_slot_param(const ntrk::Module *module, int slot, int param) {
  return module != nullptr && module->has_mix
             ? config_slot_param(module->mix, slot, param)
             : 0.f;
}

inline float
config_master_gain(const ntrk::Module *module) {
  return module != nullptr && module->has_mix ? config_master_gain(module->mix) : 1.f;
}

inline float
config_width(const ntrk::Module *module) {
  return module != nullptr && module->has_mix ? config_width(module->mix) : 1.f;
}

// The mirror: the mixer's current configuration into `module->mix`, and
// `has_mix` set. What a slide has moved is deliberately *not* what is written
// -- `Slot::base` is, which is the setting the automation departs from.
void mixer_config_write(const Mixer *mx, uint8_t *block);

// The same two against a module, which is where a block usually lives. Separate
// from the byte forms because a caller comparing a *candidate* block -- a host
// deciding whether the configuration it is about to apply is the one already
// running -- has bytes and no module to put them in.
inline bool
mixer_config_read(Mixer *mx, const ntrk::Module *module) {
  return module != nullptr && module->has_mix &&
         mixer_config_read(mx, module->mix);
}

inline void
mixer_config_write(const Mixer *mx, ntrk::Module *module) {
  if (module == nullptr)
    return;
  mixer_config_write(mx, module->mix);
  module->has_mix = true;
}

// Takes the plane's accumulators from wherever the knobs currently stand, so a
// slide arriving before any `set` continues from the value the caller -- or
// `player_start`'s LRRL panning -- actually left, rather than jumping to a
// default first.
void fxpl_sync(Mixer *mx, const Player *player);

// **Pan is written into `player->pan`, a public knob that effect `8xx` also
// writes.** Both are row-level, so the ordinary rule settles it: the last
// writer in a row wins, and the plane runs after the effect column. The one
// seam is that a plane *slide* resumes from `fxpl_val` rather than from a pan
// `8xx` set in between, because the accumulator is only reseeded by
// `fxpl_sync`. Documented rather than reconciled: a tune driving one channel's
// pan from both places at once is asking for two different things.
//
// **The master gain is one knob written from per-channel data**, so the last
// channel to touch it in a row wins. Privileging a channel would be worse; a
// tune that wants the fader automated writes it on one channel.
//
// Cutoff and resonance are absent on purpose: the voice filter reads those out
// of `fxpl_val` where it sets its coefficients, so there is nowhere to put them.
void fxpl_write(Mixer *mx, Player *player, int c, int index);

// One tick's worth of deltas, gathered before any of them is stored.
//
// **Slides sum and are clamped once, not clamped as they arrive.** Two columns
// stepping one parameter opposite ways must cancel, and per-step clamping makes
// that depend on which column happened to be read first: at 250, +20 then -20
// lands on 235 and -20 then +20 lands on 250. A delta is order-independent by
// construction and this is what makes it so.
struct FxplDeltas {
  float    value[kMaxChannels][kFxplValues];
  uint16_t touched[kMaxChannels];   // one bit per value index; kFxplValues is 8

  // Slot parameters gather the same way and for the same reason. One row per
  // global slot, then one per channel's insert -- `slot_delta_row` is the only
  // place that layout is spelled out.
  float    slot[kSlotInsert + kMaxChannels][8];
  uint8_t  slot_touched[kSlotInsert + kMaxChannels];
};

// Which row of `FxplDeltas::slot` a (slot, channel) pair accumulates into, or
// -1 for a pair with no referent.
int slot_delta_row(int slot, int channel);

// One macro invocation, from a meta lane's `{macro, input}` cell.
//
// `deltas` selects which half of the tick this is: null applies a macro whose
// flags say *absolute*, as a set on the row's first tick; non-null gathers a
// *delta* macro's per-tick step. A macro is wholly one or the other, so exactly
// one of the two calls per tick does anything and the other returns.
//
// **There is no second accumulator for macros.** A delta macro adds into the
// same `fxpl_val` a plain slide adds into, which is what lets a macro and a
// slide on one target sum, and what makes the resync path need nothing new:
// `fxpl_sync` re-seeds `fxpl_val` and a macro resumes from the reseeded base.
void fxpl_macro(Mixer *mx, Player *player, const Macro *mac, int input,
                FxplDeltas *deltas);

// Tick 0 of a row: latch every lane's cell, apply the sets, then apply whatever
// external input is pending.
void fxpl_row(Mixer *mx, Player *player, int order, int row);

// Invoke `macro` (one-based, as a meta cell is) with `input` at the next row
// boundary.
//
// **Same thread as `mixer_render_add`, like every other call on a Mixer.** This
// is a plain read-modify-write of two fields and nothing here is atomic: a
// caller that sets it from a game thread while a block is rendering can have the
// mask cleared out from under the bit it just set, and the parameter is then
// dropped for good. A host with its own thread posts through the command queue
// it already drains at the block boundary; that is where this belongs. **Queued, not applied**: a parameter that landed mid-row would be
// heard on the audio thread's block schedule rather than the music's, and two
// calls between rows would be two different renders of the same input.
//
// The last call before a row wins, which is what a parameter means: a game
// setting "intensity" three times between two rows wants the third value, not
// three invocations.
//
// **After the row's own cells**, so a game parameter outranks the pattern for
// that row. The other order would make a tune able to ignore its host, which is
// not what a host asks a parameter for.
//
// An index outside 1..kMaxMacros is ignored, and one past the module's
// `macro_count` is ignored when it is applied -- the same terms a meta lane's
// index gets, rather than a second answer about what an unknown macro means.
//
// **A delta macro cannot be invoked from here.** It is a per-tick step that the
// meta-lane latch re-fires for the length of a row; external input has no latch,
// so a single step would be an arbitrary fraction of a slide. The queued input
// is dropped at the row, not held. A host parameter is a value: point it at an
// absolute macro.
//
// Requires the module to carry an effect plane: a macro writes plane state, and
// the plane does not run for a module whose `fx` is null. A v1 file has no
// macros to invoke either, and a queued input is DROPPED while such a module
// plays rather than waiting for one that has a plane -- held, it would fire on
// the first row of the next tune at an index that means something else there.
// `mixer_reset` drops it too, for the same reason.
void mixer_set_macro(Mixer *mx, int macro, int input);

// Any tick that is not a row's first.
void fxpl_slide(Mixer *mx, Player *player);

// Applies the plane for every sequencer tick that has passed since the last
// call.
//
// `order`, `row` and `tick` are the player's from *before* `player_run_begin`
// advanced it, and they have to be. Afterwards, `tick` is the index of the tick
// about to run rather than the one just rendered -- and at the end of a row it
// has wrapped to 0 with the row already stepped on, which at speed 1 is every
// tick there is. Before the call, `tick == 0` is the one unambiguous statement
// that what runs next is a row's first, and (order, row) is that row.
void fxpl_run(Mixer *mx, Player *player, int order, int row, int tick);

void limiter_process(Mixer *mx, float *l, float *r, int frames, float rate);

// Adds into `buffer` rather than over it, exactly as `render_add` does and for
// the same reason: a renderer that assigns drops whatever else was already
// mixed there. Interleaved, `channels` per frame; odd outputs take the right
// bus and even the left, so a mono caller gets the left one.
void mixer_render_add(Mixer *mx, Player *player, double *buffer, int frames,
                      int channels, float sample_rate);

// ----------------------------------------------------------------------------
// -- Describing a cell
// ----------------------------------------------------------------------------
//
// **The mixer's half of the pretty-printer, and it is here because the format
// is opaque and this is the end that can see through it.** `param[0]` is delay
// time on a delay and drive on a shaper; the discriminator is `Slot::kind`,
// which is runtime state no file carries. So ntrk.h describes what the format
// fixes — ProTracker's column and the player's 0x30/0x31 — and this describes
// the mixer range, the slot range, and a macro's targets.
//
// The reason both halves are in the library at all rather than in an editor:
// otherwise every editor keeps its own copy of the command table, and a copy
// that drifts from the format is the failure this whole codebase keeps
// catching. One source of truth, beside the thing it describes.
//
// The buffer contract is ntrk.h's: write into the caller's bytes, truncate
// rather than overrun, always NUL-terminate, return the characters written
// excluding the NUL, and write nothing at all for a `cap` of zero. Nothing
// allocates, nothing calls stdio, nothing touches libm.

// Static strings, so nothing allocates. Null is the honest answer for an index
// this build has no name for, and every caller below degrades on it rather than
// guessing — "Send 2 param 5" is what the *format* knows, and it is correct.
const char *fx_kind_name(FxKind kind);
const char *fx_param_name(FxKind kind, int param);
const char *fxpl_slot_name(int slot);

// A macro target below 0x40, which indexes `Mixer::fxpl_val`.
const char *fxpl_value_name(int index);

// **The one entry point.** Any FXPL cell, in either kind of lane: it takes the
// mixer range and the slot range itself, delegates 0x30..0x3F to
// `fxpl_player_describe` rather than repeating it, and names an unknown command
// as unknown rather than inventing a meaning for it.
//
// `channel` is the lane's own channel, and **a negative one means a meta lane**
// — which is the whole discriminator, because a meta cell's first byte is a
// macro index where a channel cell's is a command. It also resolves
// `kSlotInsert` against that channel's own slot.
//
// `mx` may be null and `m` may be null, and both degrade honestly: without a
// mixer a slot parameter is "Send 2 param 2" instead of "Send 2 (Reverb)
// Predelay", and without a module a macro invocation names the macro without
// its targets. An editor may well want to describe a cell with neither to hand.
size_t fxpl_describe(const Mixer *mx, const Module *m, int channel, FxCell cell,
                     char *out, size_t cap);

// The same cell as a three-letter mnemonic, for the pattern grid, where
// `fxpl_describe`'s sentence does not fit.
//
// **Display only.** The stored byte is untouched and no fingerprint moves.
//
// Set and slide are paired by their last letter -- `PAN`/`PNS`, `CUT`/`CTS` --
// so a column of slides reads as slides at a glance. Sends are numbered
// (`SN1`..`SN4`, `SS1`..`SS4`) rather than named after the effect loaded in the
// slot: the slot's kind is a mixer setting a tune can change, and a grid whose
// command names moved when a send was reassigned would be unreadable.
//
// Slot parameters get `Sxy` for a set and `Dxy` for a slide, x the slot and y
// the parameter, because there are sixty-four of them and no three letters
// distinguish them usefully. `fxpl_describe` is what resolves those into words.
//
// A `channel` is needed for nothing here, which is why this takes none: unlike
// the prose, a mnemonic never resolves against context.
//
// Always writes exactly three characters and a terminator.
void fxpl_mnemonic(FxCell cell, char out[4]);

// A meta cell's `{macro, input}`, targets and all — which is the point, because
// the targets are the part of a macro nothing else can show. `macro` is
// one-based as the cell is, so 0 is no invocation.
//
// The value each target lands on is computed for this `input`, rather than the
// scale and offset being printed raw: an editor's reader wants the number the
// row produces, and `(scale * input + offset) / 256` is not arithmetic to do in
// one's head at a status bar.
size_t macro_describe(const Mixer *mx, const Module *m, int macro, int input,
                      char *out, size_t cap);

// ----------------------------------------------------------------------------
// -- Enumerating commands
// ----------------------------------------------------------------------------
//
// **The other half of describing a cell: what an editor may put in one.** See
// the section of the same name in ntrk.h for why there is no per-command
// summary string and no min/max/unit; this is where the three lanes are
// enumerated and where a slot parameter is resolved against the mixer.

// Which column a command byte was read from. **The lane is the discriminator,
// not the byte**: `0x08` is `8xx` set panning in a note column and a resonance
// slide in the plane, and nothing about the number says which.
enum class Lane {
  NoteEffect,    // `Note::effect` / `Note::param`, ProTracker's own column
  PlaneChannel,  // an FXPL channel lane
  PlaneMeta      // an FXPL meta lane
};

// One command, resolved. Three fields differ from a bare `CmdInfo`, and each
// difference is forced by something the format does:
//
//   `lane`   because a command byte does not name its own column, and
//            `command_value_text` has only this struct to render from.
//   `name`   a buffer rather than a static string, because a slot parameter's
//            name is *built* — "Send 2 (Reverb) Diffusion" comes from the kind
//            loaded in the slot, and there are 128 such commands.
//   `choice` a resolved pointer rather than a list this struct's reader has to
//            find again: which four strings a slot's parameter 0 names depends
//            on that same kind.
struct CommandInfo {
  Lane        lane;
  uint8_t     cmd;
  char        mnemonic[4];
  char        name[48];
  ParamShape  shape;
  int         choice_count;     // 0 unless shape == Choice
  const char *const *choice;    // `choice_count` strings, else null
};

// How many commands a lane offers.
//
//   NoteEffect    32 — sixteen base effects and the sixteen `E` holds.
//   PlaneChannel  35 — the mixer range, the player's two, and this channel's
//                 own insert. A send or the master has no referent here.
//   PlaneMeta     80 — set and slide for the eight parameters of each of the
//                 four sends and the master.
//
// **A meta lane's macro invocations are not commands and are not counted.** A
// macro's name and targets are the *module's*, not this library's, so they have
// no row in a static table; `macro_describe` is what reads one.
int command_count(Lane lane);

// The command at `index`, 0 .. `command_count(lane) - 1`, or the command with
// this byte. False for an index out of range, or a byte the lane does not
// carry — which is how an editor validates a cell as well as offering one.
//
// `mx` may be null and degrades exactly as `fxpl_describe` does: with a mixer a
// slot parameter is "Send 2 (Reverb) Diffusion", without one "Send 2 param 4".
// It is the same resolution, called rather than copied.
//
// `channel` resolves `kSlotInsert` on a `PlaneChannel` lane, and is the one
// argument this API adds to the four the caller already gives: an insert is
// *some* channel's, and without knowing which there is no kind to read. -1 is
// the honest unresolved form and the default, so a caller with no channel in
// hand gets the index form rather than channel zero's guess.
bool command_at(Lane lane, int index, const Mixer *mx, CommandInfo *out,
                int channel = -1);
bool command_lookup(Lane lane, uint8_t cmd, const Mixer *mx, CommandInfo *out,
                    int channel = -1);

// **The value alone** — "centre", "+0.12/tick", "1024 frames", "square" — and
// not a sentence about the cell. That is `fxpl_describe`'s job, and the two do
// not overlap: this is what goes beside a knob or in a column, where the name
// is already on screen and repeating it wastes the width.
//
// A `ParamShape::Unused` command writes nothing at all and returns zero: there
// is no value to show, and a zero would be a lie about a byte nothing reads.
//
// The buffer contract is ntrk.h's, as everywhere here: truncate rather than
// overrun, always NUL-terminate, return the characters written excluding it.
size_t command_value_text(const CommandInfo *ci, uint8_t param, char *out,
                          size_t cap);

// The name of one of a `Choice` command's alternatives, 0 .. `choice_count-1`.
// Empty for any other shape or an index out of range.
size_t command_choice_text(const CommandInfo *ci, int choice, char *out,
                           size_t cap);

}  // namespace mix
}  // namespace ntrk

#endif  // NTRK_MIX_H_
