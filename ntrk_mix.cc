// ntrk_mix -- the mixer's arithmetic. See ntrk_mix.h for the strip's shape, the
// effect plane and the reasoning behind them.
//
// A translation unit rather than a header because every function here is called
// once per *block* at most: the per-sample work is inside `mixer_render_add`'s
// own loops, so there is nothing for an inline to save and a call is amortised
// over a whole run. The per-sample primitives it uses stay inline, in
// ntrk_dsp.h, ntrk_fx_shape.h and ntrk_synth.h.
//
// This file may also be `#include`d into one translation unit of your own --
// that is what ntrk_unity.h does. Compile it separately or include it, not both.
//
// Public domain / CC0. Written for the no2 project.

#include "ntrk_mix.h"

#include <cmath>
#include <string.h>

namespace ntrk {
namespace mix {

int
slot_choice(float v, int n) {
  // Written so a NaN fails it: every comparison against one is false, and
  // `(int)` of a NaN is undefined behaviour.
  if (!(v > 0.f))
    return 0;
  int i = (int) (v * (float) n);
  return i >= n ? n - 1 : i;
}

float
slot_choice_param(int i, int n) {
  return ((float) i + 0.5f) / (float) n;
}

void
slot_set_kind(Slot *s, FxKind kind) {
  if (s == nullptr)
    return;
  // **The type does not make this redundant**: nothing stops a caller casting
  // a byte in, and an enum class holding a value it has no name for is exactly
  // the case this existed for. One test rather than two now, because the
  // underlying type is unsigned and there is no longer a below-zero to reject.
  if (kind > FxKind::kReverb)
    kind = FxKind::kNone;   // an unknown kind is off, not a guess
  s->kind = kind;
  slot_hold(s);
  // Not `applied` itself: a slot whose parameters are all zero would match the
  // cleared array, the first apply would be skipped, and the new effect would
  // run on the old effect's coefficients.
  s->applied_kind = -1;
  fx::svf_reset(&s->svf[0]);
  fx::svf_reset(&s->svf[1]);
  fx::delay_reset(&s->delay);
  fx::reverb_reset(&s->reverb);
}

bool
slot_filter_on(const Slot *s) {
  return s->param[1] > 0.f;
}

// How much of a shaper's input survives it. **Stored as the dry amount rather
// than as a wet/dry mix**, so that zero -- what every slot configured before
// this knob existed holds in `param[2]` -- is the fully wet shaper a send has
// always been. A mix knob would have needed a sentinel at zero instead, and a
// sentinel would have made a fully dry bypass the one setting it could not say.
//
// Clamped here rather than trusted: `param` is the caller's array and a NaN
// fails both comparisons, which lands on the wet path.
static float
slot_shape_dry(const Slot *s) {
  return s->param[2] > 0.f ? (s->param[2] < 1.f ? s->param[2] : 1.f) : 0.f;
}

float
slot_filter_sample(fx::Svf *f, FilterMode mode, float x) {
  switch (mode) {
    case FilterMode::kHighpass: return fx::svf_highpass(f, x);
    case FilterMode::kBandpass: return fx::svf_bandpass(f, x);
    default:              return fx::svf_lowpass(f, x);
  }
}

void
slot_apply_params(Slot *s, float rate) {
  // **The whole cost of a slot that nobody is automating.** `slot_process_*`
  // calls this every block, so an idle reverb re-derived every FDN shelf and Jot gain
  // and a predelay round trip a thousand times a second for a set of numbers
  // that had not moved. Bit-comparing floats is exactly right here: the
  // question is whether the setter would be handed the same bits, not whether
  // two parameters are close.
  if (s->applied_kind == (int) s->kind && s->applied_rate == rate &&
      memcmp(s->param, s->applied, sizeof s->param) == 0)
    return;
  s->applied_kind = (int) s->kind;
  s->applied_rate = rate;
  memcpy(s->applied, s->param, sizeof s->param);

  // **This is where 0..1 becomes seconds and hertz.** The slot layer owns the
  // units because a `u8` in a pattern cannot carry them; `*_set` keeps its real
  // ones for a caller that reaches past this.
  switch (s->kind) {
    case FxKind::kFilter:
      if (slot_filter_on(s)) {
        // Exponential, through the plane's own curve, so equal steps of the
        // parameter are equal intervals -- a linear sweep in hertz crawls at
        // the bottom of the range and leaps at the top.
        const float hz = fxpl_to_cutoff(s->param[1] * 255.f);
        // Twice rather than copying svf[0] over svf[1]: `svf_set` writes the
        // coefficients and leaves `ic1`/`ic2` where they were, and a struct
        // copy would take the left channel's history with it every block.
        fx::svf_set(&s->svf[0], hz, s->param[2], rate);
        fx::svf_set(&s->svf[1], hz, s->param[2], rate);
      }
      break;
    case FxKind::kDelay:
      fx::delay_set(&s->delay, s->param[0] * kSlotDelayMaxSeconds, s->param[1],
                    s->param[2], s->param[3], s->param[4] > 0.5f);
      break;
    case FxKind::kReverb: {
      // **Zero on one of the three late knobs means "as the five-knob call
      // derived it", not the bottom of its range.** They live in indices that
      // were spare, so every slot configured before they existed holds zero
      // there and must keep the reverb it had -- the same rule, and the same
      // reason, as a cutoff of zero being off rather than 20 Hz.
      float decay = 0.f, damping_lf = 0.f, diffusion = 0.f;
      fx::reverb_derived(s->param[0], &decay, &damping_lf, &diffusion);
      fx::reverb_set_full(&s->reverb, s->param[0],
                          s->param[5] > 0.f ? s->param[5] : decay,
                          s->param[1],
                          s->param[6] > 0.f ? s->param[6] : damping_lf,
                          s->param[7] > 0.f ? s->param[7] : diffusion,
                          s->param[2] * kSlotPredelayMaxSeconds, s->param[3],
                          s->param[4]);
      break;
    }
    default:
      break;      // FxKind::kNone and FxKind::kShape hold no coefficients
  }
}

void
slot_process_stereo(Slot *s, float *l, float *r, int frames, float rate) {
  if (s == nullptr || l == nullptr || r == nullptr || frames <= 0)
    return;
  if (s->kind == FxKind::kNone)
    return;                         // the bypass: not one sample is touched

  slot_apply_params(s, rate);

  switch (s->kind) {
    case FxKind::kShape: {
      // `slot_choice` returns 0..n-1 for any float including a NaN, so the
      // cast is bounded by its second argument rather than by trust.
      const fx::ShapeKind kind = (fx::ShapeKind) slot_choice(s->param[0], 4);
      const float drive = s->param[1];
      const float dry = slot_shape_dry(s);
      // **Branched, not blended at a gain of zero**, because the identity gains
      // are not the identity: `0.f * x` is a NaN for an infinite input and
      // carries x's sign for a finite one, so `-0.0f + 0.0f` would turn a
      // negative zero positive. The fully wet path is what it always was, bit
      // for bit, which is what a fingerprint blessed before this knob rests on.
      if (dry <= 0.f) {
        for (int i = 0; i < frames; ++i) {
          l[i] = fx::shape_sample(kind, l[i], drive);
          r[i] = fx::shape_sample(kind, r[i], drive);
        }
        break;
      }
      const float wet = 1.f - dry;
      for (int i = 0; i < frames; ++i) {
        l[i] = dry * l[i] + wet * fx::shape_sample(kind, l[i], drive);
        r[i] = dry * r[i] + wet * fx::shape_sample(kind, r[i], drive);
      }
      break;
    }
    case FxKind::kFilter: {
      if (!slot_filter_on(s))
        break;
      const FilterMode mode = (FilterMode) slot_choice(s->param[0], 3);
      for (int i = 0; i < frames; ++i) {
        l[i] = slot_filter_sample(&s->svf[0], mode, l[i]);
        r[i] = slot_filter_sample(&s->svf[1], mode, r[i]);
      }
      break;
    }
    case FxKind::kDelay:
      fx::delay_process(&s->delay, l, r, frames);
      break;
    case FxKind::kReverb:
      fx::reverb_process(&s->reverb, l, r, frames);
      break;
    default:
      break;
  }
}

void
slot_process_mono(Slot *s, float *m, int frames, float rate) {
  if (s == nullptr || m == nullptr || frames <= 0)
    return;
  if (s->kind != FxKind::kShape && s->kind != FxKind::kFilter)
    return;

  slot_apply_params(s, rate);

  if (s->kind == FxKind::kShape) {
    const fx::ShapeKind kind = (fx::ShapeKind) slot_choice(s->param[0], 4);
    const float drive = s->param[1];
    const float dry = slot_shape_dry(s);
    if (dry <= 0.f) {           // see slot_process_stereo for why it branches
      for (int i = 0; i < frames; ++i)
        m[i] = fx::shape_sample(kind, m[i], drive);
      return;
    }
    const float wet = 1.f - dry;
    for (int i = 0; i < frames; ++i)
      m[i] = dry * m[i] + wet * fx::shape_sample(kind, m[i], drive);
    return;
  }

  if (!slot_filter_on(s))
    return;
  const FilterMode mode = (FilterMode) slot_choice(s->param[0], 3);
  for (int i = 0; i < frames; ++i)
    m[i] = slot_filter_sample(&s->svf[0], mode, m[i]);
}

float
voice_filter_sample(fx::Svf *f, int type, float x) {
  switch (type) {
    case 1:  return fx::svf_highpass(f, x);
    case 2:  return fx::svf_bandpass(f, x);
    case 3: {
      const float bp = fx::svf_bandpass(f, x);
      return x - f->k * bp;
    }
    default: return fx::svf_lowpass(f, x);
  }
}

float
fxpl_clamp(float v) {
  return v < 0.f ? 0.f : (v > 255.f ? 255.f : v);
}

float
fxpl_to_pan(float v) {
  const float p = (v - 128.f) * (1.f / 127.f);
  return p < -1.f ? -1.f : (p > 1.f ? 1.f : p);
}

float
fxpl_to_gain(float v) {
  return v * (1.f / 128.f);
}

float
fxpl_to_unit(float v) {
  return v * (1.f / 255.f);
}

// 20 Hz to 20 kHz across the parameter's whole range, exponential so equal
// steps are equal intervals.
//
// **A table rather than std::pow, for the reason `svf_set` reads a tan table
// rather than calling tan**: this project pins reference hashes over rendered
// audio and libm is not bit-identical between Apple's and musl's. Sixteen
// segments of five eighths of an octave each; a chord across one sits at worst
// 2.3% under the curve, a quarter of a semitone of cutoff. It is monotonic,
// which is the property a slide actually depends on.
const float kFxplCutoffHz[17] = {
  20.0000f,    30.7985f,    47.4275f,    73.0348f,   112.4683f,
  173.1929f,  266.7043f,   410.7050f,   632.4555f,   973.9351f,
  1499.7884f, 2309.5640f,  3556.5588f,  5476.8393f, 8433.9301f,
  12987.6326f, 20000.0000f
};

float
fxpl_to_cutoff(float v) {
  const float x = fxpl_clamp(v) * (16.f / 255.f);
  int i = (int) x;
  if (i > 15)
    i = 15;                       // the last knot is an endpoint, not a segment
  const float f = x - (float) i;
  return kFxplCutoffHz[i] + (kFxplCutoffHz[i + 1] - kFxplCutoffHz[i]) * f;
}

int
fxpl_slide_index(int cmd) {
  switch (cmd) {
    case kFxplPanSlide:    return kFxplPan;
    case kFxplGainSlide:   return kFxplGain;
    case kFxplCutoffSlide: return kFxplCutoff;
    case kFxplResSlide:    return kFxplRes;
    default: break;
  }
  if (cmd >= kFxplSendSlide && cmd < kFxplSendSlide + kSends)
    return kFxplSend + (cmd - kFxplSendSlide);
  return -1;
}

bool
slot_is_global(int slot) {
  return slot >= 0 && slot <= kSlotMaster;
}

bool
fxpl_slot_decode(int cmd, int *slot, int *param, bool *slide) {
  if (cmd < kFxplSlotSet || cmd >= kFxplSlotEnd)
    return false;
  const int at = (cmd - kFxplSlotSet) & 0x3f;
  *slide = cmd >= kFxplSlotSlide;
  *slot = at >> 3;
  *param = at & 7;
  return true;
}

Slot *
slot_at(Mixer *mx, int slot, int channel) {
  if (slot >= kSlotSend && slot < kSlotSend + kSends)
    return &mx->send[slot - kSlotSend];
  if (slot == kSlotMaster)
    return &mx->master_fx;
  if (slot == kSlotInsert && channel >= 0 && channel < kMaxChannels)
    return &mx->insert[channel];
  return nullptr;                   // 6 and 7 are spare; an insert with no lane
}

int
slot_delta_row(int slot, int channel) {
  if (slot_is_global(slot))
    return slot;
  if (slot == kSlotInsert && channel >= 0 && channel < kMaxChannels)
    return kSlotInsert + channel;
  return -1;
}

// A normalised parameter never leaves 0..1: a slide that ran off the end would
// come back as a shaper kind out of range or a mix above unity, and the clamp
// is what makes "what the float means is the slot's business" safe to say.
static float
slot_clamp(float v) {
  return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

// The plane's parameter byte in the slot's own space. A set is 0..255 across
// the whole range; a slide is the same step signed, so one command goes both
// ways and a full sweep is two ticks at the extreme -- exactly the resolution
// every other slide in the plane has.
static float
slot_from_param(int param) {
  return (float) param * (1.f / 255.f);
}

void
mixer_pan_gains(const Player *player, bool stereo, float *gain_l,
                float *gain_r) {
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
}

// **The mixer's own headroom, which happens to be the number `render_add`
// uses.** Eight channels at full tilt reach 8.0 and four reach 4.0, so the
// master sum needs scaling down whatever else is in the chain. It is 0.25 here
// so that a mixer at `master_gain == player->gain` is exactly as loud as the
// player's own renderer -- swapping one for the other must not change the
// level of the music. It is not a borrowing: nothing keeps the two in step, and
// if `render_add` ever retunes its own, this stays where it is.
const float kHeadroom = 0.25f;

// **And the hard clip is deliberately not reused.** `render_add` clamps each
// bus to +-1 after the gain; doing that here would mean the limiter never sees
// a sample above full scale, which is every peak it exists to catch. A clip in
// front of a limiter is a limiter that does nothing.

// -1 dBFS, the usual delivery ceiling: a little under full scale so that a
// resampler or a lossy encoder downstream has somewhere to put its overshoot.
const float kLimiterCeiling = 0.891250938f;

// Where the knee starts, about -3.1 dBFS. Below this the limiter multiplies by
// exactly 1.0f, which is an identity in IEEE -- that is what makes "no effects"
// provably the pan and gain path alone rather than merely inaudibly close to it.
const float kLimiterKnee = 0.7f;

// Long enough that a bass note does not pump the whole mix under it, short
// enough to hand the level back before the next phrase.
const float kLimiterReleaseSeconds = 0.05f;

float
limiter_curve(float peak) {
  if (peak <= kLimiterKnee)
    return peak;
  const float range = kLimiterCeiling - kLimiterKnee;
  return kLimiterKnee + range * fx::soft_clip((peak - kLimiterKnee) / range);
}

float
limiter_ceil(float x) {
  if (x > kLimiterCeiling)
    return kLimiterCeiling;
  return x < -kLimiterCeiling ? -kLimiterCeiling : x;
}

void
mixer_reset(Mixer *mx) {
  if (mx == nullptr)
    return;
  // **`slot_revert` before `slot_set_kind`, and the order is the whole point.**
  // `slot_set_kind` takes `param` as the configuration, so on a mixer the plane
  // has been automating it would take the automation instead -- and this call
  // keeps settings. Reverting first makes the pair a no-op for a slot nobody
  // has moved and a restore for one somebody has.
  for (int c = 0; c < kMaxChannels; ++c) {
    slot_revert(&mx->insert[c]);
    slot_set_kind(&mx->insert[c], mx->insert[c].kind);
    fx::svf_reset(&mx->voice_filter[c]);
    mx->voice_pos[c] = 0.0;
    mx->voice_age[c] = 0u;
  }
  for (int s = 0; s < kSends; ++s) {
    slot_revert(&mx->send[s]);
    slot_set_kind(&mx->send[s], mx->send[s].kind);
  }
  slot_revert(&mx->master_fx);
  slot_set_kind(&mx->master_fx, mx->master_fx.kind);
  mx->limit_gain = 1.f;
  // The plane re-seeds itself from the knobs on the next tick rather than being
  // cleared to defaults here: those knobs are settings, and this call keeps
  // settings.
  mx->fxpl_synced = false;
  // A queued external input is NOT a setting -- it is an invocation that has not
  // happened yet, and a reset is where the caller says the last tune is over.
  // Carried across, it would fire on the first row of whatever comes next, at a
  // macro index that means something else there.
  mx->macro_pending = 0u;
}

// ---- MIXR ------------------------------------------------------------------

Slot *
mixr_slot_at(Mixer *mx, int index) {
  if (mx == nullptr || index < 0 || index >= kMixrSlots)
    return nullptr;
  return index < kSends ? &mx->send[index] : &mx->master_fx;
}

namespace {

// Q12 both ways, saturating rather than wrapping: a gain past the field's reach
// comes back at the ceiling, which is wrong by a knowable amount. A wrap would
// come back near zero and read as a bug in the mixer.
uint16_t
mixr_q12(float v) {
  if (!(v > 0.f))                       // and NaN, which fails every compare
    return 0u;
  const float q = v * (float) kMixrQUnit + 0.5f;
  return q >= 65535.f ? (uint16_t) 65535 : (uint16_t) q;
}

// A slot parameter is normalised 0..1 over the whole u16. 65535 rather than
// 65536 so that 1.0 survives the round trip exactly, which matters: a send's
// `mix` sits at exactly 1 -- all wet -- and a send that came back at 0.99998
// would be a dry copy of the channel arriving a few samples late.
uint16_t
mixr_unit(float v) {
  if (!(v > 0.f))
    return 0u;
  return v >= 1.f ? (uint16_t) 65535 : (uint16_t) (v * 65535.f + 0.5f);
}

}  // namespace

namespace {

// Whether a block is one `mixer_config_read` would accept. **Asked before every
// read below**, so an editor cannot draw a value out of a file the player will
// refuse whole -- which would describe a mixer nobody can hear.
bool
config_readable(const uint8_t *block) {
  if (block == nullptr)
    return false;
  if (ntrk::read_u16(block) != (uint16_t) kMixrVersion ||
      ntrk::read_u16(block + 2) != (uint16_t) kMixrSlots)
    return false;
  for (int i = 0; i < kMixrSlots; ++i)
    if (block[kMixrHeaderBytes + i * kMixrSlotBytes] > (uint8_t) FxKind::kReverb)
      return false;
  return true;
}

}  // namespace

FxKind
config_slot_kind(const uint8_t *block, int slot) {
  if (!config_readable(block) || slot < 0 || slot >= kMixrSlots)
    return FxKind::kNone;
  return (FxKind) block[kMixrHeaderBytes + slot * kMixrSlotBytes];
}

float
config_slot_param(const uint8_t *block, int slot, int param) {
  if (!config_readable(block) || slot < 0 || slot >= kMixrSlots || param < 0 ||
      param >= kMixrSlotParams)
    return 0.f;
  const uint8_t *r = block + kMixrHeaderBytes + slot * kMixrSlotBytes;
  return (float) ntrk::read_u16(r + 2 + param * 2) / 65535.f;
}

float
config_master_gain(const uint8_t *block) {
  if (!config_readable(block))
    return 1.f;
  return (float) ntrk::read_u16(block + 4) / (float) kMixrQUnit;
}

float
config_width(const uint8_t *block) {
  if (!config_readable(block))
    return 1.f;
  return (float) ntrk::read_u16(block + 6) / (float) kMixrQUnit;
}

bool
config_init(uint8_t *block) {
  if (block == nullptr)
    return false;
  for (int k = 0; k < kMixrBytes; ++k)
    block[k] = 0u;
  ntrk::write_u16(block + 0, (uint16_t) kMixrVersion);
  ntrk::write_u16(block + 2, (uint16_t) kMixrSlots);
  // **Unity, not zero.** A block whose master gain is a literal zero is a tune
  // that plays silently, and "I added a mixer and the sound went away" is the
  // one first impression a mixer must not make. `mixer_config_write` of a fresh
  // Mixer would write these two the same way.
  ntrk::write_u16(block + 4, mixr_q12(1.f));
  ntrk::write_u16(block + 6, mixr_q12(1.f));
  return true;
}

void
config_set_slot_kind(uint8_t *block, int slot, FxKind kind) {
  if (!config_readable(block) || slot < 0 || slot >= kMixrSlots)
    return;
  // The same ceiling `config_readable` enforces, so a set can never leave a
  // block this file would then refuse to read back.
  if ((uint8_t) kind > (uint8_t) FxKind::kReverb)
    return;
  block[kMixrHeaderBytes + slot * kMixrSlotBytes] = (uint8_t) kind;
}

void
config_set_slot_param(uint8_t *block, int slot, int param, float v) {
  if (!config_readable(block) || slot < 0 || slot >= kMixrSlots || param < 0 ||
      param >= kMixrSlotParams)
    return;
  uint8_t *r = block + kMixrHeaderBytes + slot * kMixrSlotBytes;
  ntrk::write_u16(r + 2 + param * 2, mixr_unit(v));
}

void
config_set_master_gain(uint8_t *block, float v) {
  if (!config_readable(block))
    return;
  ntrk::write_u16(block + 4, mixr_q12(v));
}

void
config_set_width(uint8_t *block, float v) {
  if (!config_readable(block))
    return;
  ntrk::write_u16(block + 6, mixr_q12(v));
}

bool
mixer_config_read(Mixer *mx, const uint8_t *block) {
  if (mx == nullptr || block == nullptr)
    return false;
  const uint8_t *p = block;
  if (ntrk::read_u16(p) != (uint16_t) kMixrVersion ||
      ntrk::read_u16(p + 2) != (uint16_t) kMixrSlots)
    return false;

  // **Every kind is checked before anything is written.** A block naming an
  // effect this build does not have is refused whole rather than applied as far
  // as it parsed -- half a mixer is a tune that plays, sounds wrong, and says
  // nothing about why.
  for (int i = 0; i < kMixrSlots; ++i)
    if (p[kMixrHeaderBytes + i * kMixrSlotBytes] > (uint8_t) FxKind::kReverb)
      return false;

  mx->master_gain = (float) ntrk::read_u16(p + 4) / (float) kMixrQUnit;
  mx->width = (float) ntrk::read_u16(p + 6) / (float) kMixrQUnit;

  for (int i = 0; i < kMixrSlots; ++i) {
    const uint8_t *r = p + kMixrHeaderBytes + i * kMixrSlotBytes;
    Slot *slot = mixr_slot_at(mx, i);
    for (int k = 0; k < kMixrSlotParams; ++k)
      slot->param[k] = (float) ntrk::read_u16(r + 2 + k * 2) / 65535.f;
    // Last, so it is what takes `base` -- see the header. A parameter written
    // after this is one the first plane resync throws away.
    slot_set_kind(slot, (FxKind) r[0]);
  }
  return true;
}

void
mixer_config_write(const Mixer *mx, uint8_t *block) {
  if (mx == nullptr || block == nullptr)
    return;
  uint8_t *p = block;
  for (int k = 0; k < kMixrBytes; ++k)
    p[k] = 0u;
  ntrk::write_u16(p + 0, (uint16_t) kMixrVersion);
  ntrk::write_u16(p + 2, (uint16_t) kMixrSlots);
  ntrk::write_u16(p + 4, mixr_q12(mx->master_gain));
  ntrk::write_u16(p + 6, mixr_q12(mx->width));

  for (int i = 0; i < kMixrSlots; ++i) {
    uint8_t *r = p + kMixrHeaderBytes + i * kMixrSlotBytes;
    const Slot *slot = i < kSends ? &mx->send[i] : &mx->master_fx;
    r[0] = (uint8_t) slot->kind;
    // **`base`, not `param`.** `param` is where a slide currently stands, so
    // saving mid-automation would bake the tune's own movement into its
    // starting state and the next load would begin somewhere else.
    for (int k = 0; k < kMixrSlotParams; ++k)
      ntrk::write_u16(r + 2 + k * 2, mixr_unit(slot->base[k]));
  }
}

void
fxpl_sync(Mixer *mx, const Player *player) {
  for (int c = 0; c < kMaxChannels; ++c) {
    mx->fxpl_val[c][kFxplPan] = fxpl_clamp(128.f + player->pan[c] * 127.f);
    mx->fxpl_val[c][kFxplGain] = fxpl_clamp(mx->master_gain * 128.f);
    // Wide open, so a bare resonance command cannot silently close a filter
    // nobody asked to close.
    mx->fxpl_val[c][kFxplCutoff] = 255.f;
    mx->fxpl_val[c][kFxplRes] = 0.f;
    for (int s = 0; s < kSends; ++s)
      mx->fxpl_val[c][kFxplSend + s] = fxpl_clamp(mx->send_level[c][s] * 255.f);
    for (int i = 0; i < kFxplCmdCount; ++i)
      mx->fxpl_mem[c][i] = 0;
    for (int col = 0; col < kMaxFxColumns; ++col) {
      mx->fxpl_cmd[c][col] = 0;
      mx->fxpl_param[c][col] = 0;
    }
    mx->fxpl_type[c] = 0;
    mx->fxpl_filter[c] = false;
  }
  // The meta lanes' latch belongs to a row nobody is on any more, exactly as
  // the channel columns' does, and is dropped in the same place for the same
  // reason -- a delta macro left in flight would keep stepping a parameter the
  // reseed above has just taken back to where the knobs stand.
  for (int t = 0; t < kMaxMetaColumns; ++t) {
    mx->fxpl_meta_cmd[t] = 0;
    mx->fxpl_meta_param[t] = 0;
  }
  // **And every slot goes back to its configuration**, which is the whole
  // reason `Slot::base` exists. The values above re-seed from the knobs they
  // write -- `player->pan`, `master_gain`, `send_level` -- and a slot parameter
  // has no such knob to read back from, so without this a pattern that touched
  // one would keep it for good and a seek backwards would not undo it.
  for (int c = 0; c < kMaxChannels; ++c)
    slot_revert(&mx->insert[c]);
  for (int s = 0; s < kSends; ++s)
    slot_revert(&mx->send[s]);
  slot_revert(&mx->master_fx);
  mx->fxpl_synced = true;
}

void
fxpl_write(Mixer *mx, Player *player, int c, int index) {
  const float v = mx->fxpl_val[c][index];
  if (index == kFxplPan)
    player->pan[c] = fxpl_to_pan(v);
  else if (index == kFxplGain)
    mx->master_gain = fxpl_to_gain(v);
  else if (index >= kFxplSend)
    mx->send_level[c][index - kFxplSend] = fxpl_to_unit(v);
}

// The value a target lands on, and it is deliberately not one line.
//
// **The arithmetic is i32 and reaches `fxpl_val` through a single cast.** An
// 8.8 scale times a u8 input is a five-digit number before the offset is even
// added -- unity, 0x0100, times an input of 255 is 65280 -- so an i16
// intermediate wraps and a loud macro goes silent at an arbitrary input rather
// than saturating at the top. Doing the scaling in float instead would cost the
// library its ARM64/wasm render identity, which is a *measured* property here
// (tools/check_ntrk_crosstarget.sh) and not an aspiration.
//
// Both scale and offset are 8.8, so one divide by 256 puts the pair back into
// the command's own 0..255 parameter space. Division rather than a shift: a
// right shift of a negative value is implementation-defined, and this number
// must not vary by target.
static int32_t
macro_value(const MacroTarget &mt, int input) {
  return ((int32_t) mt.scale * (int32_t) input + (int32_t) mt.offset) / 256;
}

void
fxpl_macro(Mixer *mx, Player *player, const Macro *mac, int input,
           FxplDeltas *deltas) {
  const Module *m = player->module;
  // Absolute macros are sets and belong to a row's first tick; delta macros are
  // slides and belong to the ticks after it. One call per half of the tick, and
  // the half this macro does not belong to returns here.
  const bool is_delta = (mac->flags & kMacroDelta) != 0u;
  if (is_delta != (deltas != nullptr))
    return;

  for (int k = 0; k < (int) mac->target_count; ++k) {
    const MacroTarget &mt = mac->targets[k];
    const int32_t value = macro_value(mt, input);
    const int32_t clamped = value < 0 ? 0 : (value > 255 ? 255 : value);

    // **A target of 0x40 and up names a slot parameter**, in exactly the
    // numbering the set commands use. Two disjoint ranges rather than a
    // renumbering or a mapping table: no byte in a MACR block that already
    // ships moves, and `fxpl_val`'s eight indices keep theirs.
    int slot = 0;
    int sparam = 0;
    bool slot_slide = false;
    const bool is_slot =
        mt.target < (uint8_t) kFxplSlotSlide &&
        fxpl_slot_decode((int) mt.target, &slot, &sparam, &slot_slide);

    if (is_slot && slot_is_global(slot)) {
      // **Once, whatever the scope.** A send and the master are one thing, not
      // one per channel, so a delta macro at scope 0xFF must not sum a reverb
      // size eight times -- which is the master-gain wart with an effect in it.
      const int row = slot_delta_row(slot, 0);
      if (deltas != nullptr) {
        deltas->slot[row][sparam] += slot_from_param((int) value);
        deltas->slot_touched[row] |= (uint8_t) (1u << sparam);
      } else {
        Slot *s = slot_at(mx, slot, 0);
        s->param[sparam] = slot_from_param((int) clamped);
      }
      continue;
    }

    int first;
    int last;
    if (mt.scope == kMacroScopeAll) {
      // **`m->channels`, not `kMaxChannels`.** A lane the module does not have
      // is a value nothing mixes -- and the next `fxpl_sync` would read the
      // stale write back out of it as though a row had asked for it.
      first = 0;
      last = m->channels;
    } else if (mt.scope == kMacroScopeSelf) {
      // ponytail: a meta lane belongs to no channel, so "the invoking channel"
      // names nothing and this is a no-op. It becomes reachable the day a
      // *channel* lane gains an invocation command; a meaning invented for it
      // here is a meaning that command would then be stuck with.
      continue;
    } else {
      // A scope naming a channel this module does not have is ignored at use,
      // the plane's rule for everything: a macro table must survive a song
      // being narrowed while somebody is still arranging it.
      if ((int) mt.scope >= m->channels)
        continue;
      first = (int) mt.scope;
      last = first + 1;
    }

    for (int c = first; c < last; ++c) {
      // An insert is per channel, so it walks `scope` like every other target
      // -- which is what makes scope 0xFF the way to reach every channel's
      // insert from a meta lane, and `kMacroScopeSelf` still nothing at all.
      if (is_slot) {
        const int row = slot_delta_row(slot, c);
        if (row < 0)
          continue;                 // a spare slot id, ignored at use
        if (deltas != nullptr) {
          deltas->slot[row][sparam] += slot_from_param((int) value);
          deltas->slot_touched[row] |= (uint8_t) (1u << sparam);
        } else {
          slot_at(mx, slot, c)->param[sparam] = slot_from_param((int) clamped);
        }
        continue;
      }
      if (deltas != nullptr) {
        deltas->value[c][mt.target] += (float) value;
        deltas->touched[c] |= (uint16_t) (1u << mt.target);
        continue;
      }
      // Clamped on store, so exactly one int-to-float cast reaches `fxpl_val`.
      mx->fxpl_val[c][mt.target] = (float) clamped;
      if (mt.target == kFxplCutoff || mt.target == kFxplRes)
        mx->fxpl_filter[c] = true;
      else
        fxpl_write(mx, player, c, mt.target);
    }
  }
}

void
mixer_set_macro(Mixer *mx, int macro, int input) {
  if (macro < 1 || macro > kMaxMacros)
    return;
  const int i = macro - 1;
  // Clamped rather than masked: an input is a byte and a caller that computed
  // 300 meant "as far as it goes", not 44.
  mx->macro_in[i] = (uint8_t) (input < 0 ? 0 : (input > 255 ? 255 : input));
  mx->macro_pending |= (uint32_t) 1u << i;
}

void
fxpl_row(Mixer *mx, Player *player, int order, int row) {
  const Module *m = player->module;

  // **Clamped here on the same terms as `player_pattern`, and it has to be
  // here.** `mixer_render_add` takes `player->order` at the top of a block,
  // *before* the tick that would clamp it, so a caller that shrank the module
  // between one block and the next hands this a position the player has not
  // corrected yet. `player_start`'s contract says stop the transport first; a
  // module nobody mutated never reaches either branch.
  if (order < 0 || order >= m->order_count)
    order = 0;
  if (row < 0 || row >= m->rows)
    row = 0;
  const FxCell *cells =
      m->fx + ((size_t) player_pattern(m, order) * (size_t) m->rows +
               (size_t) row) * (size_t) module_lanes(m);

  // **Meta lanes first, then the channel columns left to right.** They are
  // stored the other way round; see the header for why the two orders differ.
  const int meta_at = m->channels * m->fx_columns;
  for (int t = 0; t < m->meta_columns; ++t) {
    const uint8_t macro = cells[meta_at + t].cmd;
    mx->fxpl_meta_cmd[t] = macro;
    mx->fxpl_meta_param[t] = cells[meta_at + t].param;

    // **A meta lane's byte is a macro index or a slot command, and the two
    // cannot collide**: a macro index is 1..32 and a slot command starts at
    // 0x40. This is the lane a send or the master is written from, because a
    // meta lane is the whole row and so writes a global thing exactly once.
    int slot = 0;
    int sparam = 0;
    bool slide = false;
    if (fxpl_slot_decode((int) macro, &slot, &sparam, &slide)) {
      // A slide belongs to the ticks after this one. An insert names no channel
      // from here, so it is ignored -- a macro with scope 0xFF is the way to
      // reach every channel's insert from a global place.
      if (!slide && slot_is_global(slot))
        slot_at(mx, slot, -1)->param[sparam] =
            slot_from_param((int) mx->fxpl_meta_param[t]);
      continue;
    }

    // One-based, so that 0 can mean "no invocation": `input` cannot be the
    // discriminator, 0 being a perfectly good input. A macro past the table is
    // ignored here rather than refused at load, like every other command in the
    // plane -- refusing would need a scan of the whole thing.
    if (macro == 0u || (int) macro > m->macro_count)
      continue;
    fxpl_macro(mx, player, &m->macros[macro - 1],
               (int) mx->fxpl_meta_param[t], nullptr);
  }

  for (int c = 0; c < m->channels; ++c) {
    for (int col = 0; col < m->fx_columns; ++col) {
      const FxCell &cell =
          cells[(size_t) c * (size_t) m->fx_columns + (size_t) col];
      const int cmd = (int) cell.cmd;
      uint8_t param = cell.param;

      // An empty cell stops whatever that column was sliding, which is what an
      // empty cell means in every tracker there is.
      mx->fxpl_cmd[c][col] = (uint8_t) cmd;
      if (cmd == 0)
        continue;

      // **The "parameter zero means the last one" memory is per (channel,
      // command), not per column, and the columns are read left to right** --
      // so a zero parameter in column 1 picks up what column 0 wrote for the
      // same command on this same row. At one column that was implicit; at four
      // it is a decision, and the other one (a memory per column) would make
      // the same two cells mean different things depending on which column an
      // editor happened to put them in.
      if (cmd < kFxplCmdCount) {
        if (param == 0)
          param = mx->fxpl_mem[c][cmd];
        else
          mx->fxpl_mem[c][cmd] = param;
      }
      mx->fxpl_param[c][col] = param;

      // A slide does nothing on tick 0 -- it runs on 1..speed-1, beside the
      // player's own. Everything else is a set and lands here; a set from a
      // later lane overwrites one from an earlier, which is what makes a
      // channel's own column able to trim a macro.
      if (fxpl_slide_index(cmd) >= 0)
        continue;

      switch (cmd) {
        case kFxplPanSet:
          mx->fxpl_val[c][kFxplPan] = (float) param;
          fxpl_write(mx, player, c, kFxplPan);
          break;
        case kFxplGainSet:
          mx->fxpl_val[c][kFxplGain] = (float) param;
          fxpl_write(mx, player, c, kFxplGain);
          break;
        case kFxplCutoffSet:
          mx->fxpl_val[c][kFxplCutoff] = (float) param;
          mx->fxpl_filter[c] = true;
          break;
        case kFxplResSet:
          mx->fxpl_val[c][kFxplRes] = (float) param;
          mx->fxpl_filter[c] = true;
          break;
        case kFxplTypeSet:
          // Masked rather than refused: four types, and a parameter is clamped.
          mx->fxpl_type[c] = (uint8_t) (param & 3);
          mx->fxpl_filter[c] = true;
          break;
        default: {
          if (cmd >= kFxplSendSet && cmd < kFxplSendSet + kSends) {
            const int index = kFxplSend + (cmd - kFxplSendSet);
            mx->fxpl_val[c][index] = (float) param;
            fxpl_write(mx, player, c, index);
            break;
          }
          // **A channel lane reaches its own insert and nothing else.** A send
          // or the master written from four channel lanes is the master-gain
          // wart with an effect in it: four columns each sliding one reverb
          // size would sum four deltas into one value. It is a slot-id range
          // check, so it costs the format none of its opacity about what the
          // parameter behind that id means.
          int slot = 0;
          int sparam = 0;
          bool slide = false;
          if (fxpl_slot_decode(cmd, &slot, &sparam, &slide) && !slide &&
              slot == kSlotInsert)
            mx->insert[c].param[sparam] = slot_from_param((int) param);
          // Anything else is a command this reader does not know. Ignored.
          break;
        }
      }
    }
  }

  // **External input, after the row's own cells.** A macro invoked from outside
  // the pattern is the host's parameter, and a host that could be overruled by
  // the tune it is driving is not a parameter -- so the game's value lands last
  // and wins for this row.
  //
  // One integer read when nothing is pending, which is the bit-identity
  // guarantee: a mixer nobody has called `mixer_set_macro` on runs exactly the
  // program it ran before this existed.
  if (mx->macro_pending != 0u) {
    const uint32_t pending = mx->macro_pending;
    mx->macro_pending = 0u;
    // **In index order, and a plain scan of 32 bits.** A count-trailing-zeros
    // intrinsic is a compiler builtin with no portable spelling, and this file
    // may not reach for one: the render match across targets is a measured
    // property, so nothing in the path gets to depend on the toolchain.
    for (int i = 0; i < kMaxMacros; ++i) {
      if ((pending & ((uint32_t) 1u << i)) == 0u)
        continue;
      // Past the module's table is ignored here rather than refused at the
      // setter, exactly as a meta lane's index is: the setter has no module to
      // ask, and two answers about what an unknown macro means is one too many.
      if (i >= m->macro_count)
        continue;
      // **A delta macro is not reachable from outside, and the skip is written
      // rather than left to `fxpl_macro`'s early return.** A delta macro is a
      // per-tick step re-fired from the meta latch for the length of a row;
      // external input has no latch, so one step would be an arbitrary fraction
      // of a slide nobody asked for. A host parameter is a value -- invoke an
      // absolute macro with it.
      if ((m->macros[i].flags & kMacroDelta) != 0u)
        continue;
      fxpl_macro(mx, player, &m->macros[i], (int) mx->macro_in[i], nullptr);
    }
  }
}

void
fxpl_slide(Mixer *mx, Player *player) {
  const Module *m = player->module;

  FxplDeltas d;
  for (int c = 0; c < m->channels; ++c) {
    d.touched[c] = 0u;
    for (int i = 0; i < kFxplValues; ++i)
      d.value[c][i] = 0.f;
  }
  for (int rowi = 0; rowi < kSlotInsert + kMaxChannels; ++rowi) {
    d.slot_touched[rowi] = 0u;
    for (int i = 0; i < 8; ++i)
      d.slot[rowi][i] = 0.f;
  }

  // Meta first here too, though for a delta nothing turns on it: a sum is a sum
  // whichever order it is taken in, and matching `fxpl_row` costs nothing.
  for (int t = 0; t < m->meta_columns; ++t) {
    const int macro = (int) mx->fxpl_meta_cmd[t];

    int slot = 0;
    int sparam = 0;
    bool slide = false;
    if (fxpl_slot_decode(macro, &slot, &sparam, &slide)) {
      if (slide && slot_is_global(slot)) {
        const int rowi = slot_delta_row(slot, -1);
        d.slot[rowi][sparam] +=
            slot_from_param((int) (int8_t) mx->fxpl_meta_param[t]);
        d.slot_touched[rowi] |= (uint8_t) (1u << sparam);
      }
      continue;
    }

    if (macro == 0 || macro > m->macro_count)
      continue;
    fxpl_macro(mx, player, &m->macros[macro - 1],
               (int) mx->fxpl_meta_param[t], &d);
  }

  for (int c = 0; c < m->channels; ++c) {
    for (int col = 0; col < m->fx_columns; ++col) {
      int slot = 0;
      int sparam = 0;
      bool slide = false;
      if (fxpl_slot_decode((int) mx->fxpl_cmd[c][col], &slot, &sparam,
                           &slide)) {
        if (slide && slot == kSlotInsert) {
          const int rowi = slot_delta_row(slot, c);
          d.slot[rowi][sparam] +=
              slot_from_param((int) (int8_t) mx->fxpl_param[c][col]);
          d.slot_touched[rowi] |= (uint8_t) (1u << sparam);
        }
        continue;
      }
      const int index = fxpl_slide_index((int) mx->fxpl_cmd[c][col]);
      if (index < 0)
        continue;
      // **Signed, so one command slides both ways.** Eight bits of parameter is
      // what the plane exists to offer -- the nibble pair every ProTracker slide
      // uses is four, and steps audibly across a long sweep -- and the direction
      // has to come out of the same byte.
      const float step = (float) (int) (int8_t) mx->fxpl_param[c][col];
      d.value[c][index] += step;
      d.touched[c] |= (uint16_t) (1u << index);
    }
  }

  // And stored once per value, so a macro and a plain slide on one target sum
  // rather than the second overwriting the first.
  for (int c = 0; c < m->channels; ++c) {
    if (d.touched[c] == 0u)
      continue;
    for (int i = 0; i < kFxplValues; ++i) {
      if ((d.touched[c] & (uint16_t) (1u << i)) == 0u)
        continue;
      mx->fxpl_val[c][i] = fxpl_clamp(mx->fxpl_val[c][i] + d.value[c][i]);
      if (i == kFxplCutoff || i == kFxplRes)
        mx->fxpl_filter[c] = true;
      else
        fxpl_write(mx, player, c, i);
    }
  }

  // The slots' half of the same store. **`Slot::param` is the accumulator** --
  // it persists across ticks exactly as `fxpl_val` does, which is why widening
  // the macro target range needed no second one.
  for (int rowi = 0; rowi < kSlotInsert + kMaxChannels; ++rowi) {
    if (d.slot_touched[rowi] == 0u)
      continue;
    const int slot = rowi < kSlotInsert ? rowi : kSlotInsert;
    Slot *s = slot_at(mx, slot, rowi - kSlotInsert);
    for (int i = 0; i < 8; ++i) {
      if ((d.slot_touched[rowi] & (uint8_t) (1u << i)) == 0u)
        continue;
      s->param[i] = slot_clamp(s->param[i] + d.slot[rowi][i]);
    }
  }
}

void
fxpl_run(Mixer *mx, Player *player, int order, int row, int tick) {
  const uint64_t n = player->ticks_elapsed;
  uint64_t ticks;
  if (!mx->fxpl_synced || n < mx->fxpl_seen) {
    // Backwards means the player restarted or seeked; an unsynced mixer against
    // an already-running player is the same problem. Drop every slide in flight
    // rather than replaying the gap -- those accumulators belong to a row
    // nobody is on any more.
    fxpl_sync(mx, player);
    ticks = 1;
  } else {
    ticks = n - mx->fxpl_seen;
    if (ticks == 0)
      return;                     // a run that did not cross a tick
  }
  mx->fxpl_seen = n;

  for (uint64_t k = 0; k < ticks; ++k) {
    if (k == 0 && tick == 0)
      fxpl_row(mx, player, order, row);
    else
      fxpl_slide(mx, player);
  }
  // ponytail: `ticks` is 1 at every tempo and sample rate a tune is played at
  // -- it passes 1 only once a whole tick fits inside one output frame -- and
  // the ones after the first are taken as slide ticks on the latched row. Carry
  // a per-tick row identity here when something really renders at 400 Hz.
}

void
limiter_process(Mixer *mx, float *l, float *r, int frames, float rate) {
  // **A linear ramp, not the usual one-pole.** exp() is not bit-identical
  // across platforms and this output is hashed, which rules the textbook
  // coefficient out; and a one-pole towards unity *stalls* a few parts in
  // 100000 short of it, because the remaining step falls under half an ulp
  // long before the difference does. A gain of 0.99993 for ever afterwards is
  // inaudible and is not 1.0f, so a mixer that limited once would never again
  // be bit-for-bit the unprocessed path. A ramp lands on the target exactly.
  float step = (rate > 1.f) ? 1.f / (kLimiterReleaseSeconds * rate) : 1.f;
  if (step > 1.f)
    step = 1.f;
  if (!(step > 0.f))
    step = 1.f;                 // a NaN rate lands here rather than freezing

  float g = mx->limit_gain;
  for (int i = 0; i < frames; ++i) {
    float peak = std::fabs(l[i]);
    const float peak_r = std::fabs(r[i]);
    if (peak_r > peak)
      peak = peak_r;

    float target = 1.f;
    if (peak > kLimiterKnee)
      target = limiter_curve(peak) / peak;

    if (target < g) {
      // **Instantaneous attack, and with no lookahead it has to be.** Smoothing
      // the way down lets the sample that asked for the reduction through at
      // full height, which is precisely the peak the ceiling was promised
      // against. The knee is what keeps that from sounding like a clip: by the
      // time the gain moves quickly it has already been moving gently for
      // 3 dB.
      g = target;
    } else {
      // Never past the target: overshooting it while the signal is still over
      // the knee is the one way a gain computed from the peak stops bounding
      // the peak it was computed from.
      g += step;
      if (g > target)
        g = target;
    }

    // The clamp is the last ulp of the limiter's own promise, not a clip in
    // front of it: `g` is `curve(peak)/peak`, so a divide and a multiply later
    // the product can land one ulp the wrong side of the ceiling, and a
    // ceiling that holds to within a rounding error is not a ceiling anything
    // can be tested against. It cannot fire on a sample the gain already
    // brought under, so it colours nothing.
    l[i] = limiter_ceil(l[i] * g);
    r[i] = limiter_ceil(r[i] * g);
  }
  mx->limit_gain = g;
}

void
mixer_render_add(Mixer *mx, Player *player, double *buffer, int frames,
                 int channels, float sample_rate) {
  if (mx == nullptr || player == nullptr || player->module == nullptr ||
      !player->playing)
    return;
  if (buffer == nullptr || frames <= 0 || channels <= 0)
    return;

  const double rate = sample_rate > 0.f ? (double) sample_rate : 48000.0;
  const float frate = (float) rate;
  const Module *m = player->module;

  const bool stereo = channels >= 2;
  float gain_l[kMaxChannels];
  float gain_r[kMaxChannels];
  mixer_pan_gains(player, stereo, gain_l, gain_r);

  // **Null means the plane machinery does not run at all**, rather than running
  // with commands that happen to be no-ops. That is the difference between the
  // mixer being bit for bit `render_add` for a v1 file and being merely very
  // close to it.
  const bool plane = m->fx != nullptr;

  bool send_on[kSends];
  for (int s = 0; s < kSends; ++s)
    send_on[s] = mx->send[s].kind != FxKind::kNone;

  int frame = 0;
  while (frame < frames) {
    // Where the sequencer stands *before* the tick, which is the only place the
    // plane can learn which row is starting. See fxpl_run.
    const int at_order = player->order;
    const int at_row = player->row;
    const int at_tick = player->tick;

    int run = player_run_begin(player, rate, frames - frame);
    if (run == 0)
      return;
    // `player_run_begin` promises kMaxBlock, and the scratch above is sized on
    // that promise. Clamping is what makes it a bound rather than a comment --
    // an assertion here compiles to nothing in the build that ships.
    if (run > kMaxBlock)
      run = kMaxBlock;

    // **A module with no plane drops what was queued rather than holding it.** The
    // plane is where a macro is applied, so a pending input has nowhere to go --
    // and held, it would fire on the first row of the next module that DOES have
    // one, at an index that means something else there. Guarded on the mask, so a
    // mixer nobody has queued anything on is not written to at all.
    if (!plane && mx->macro_pending != 0u)
      mx->macro_pending = 0u;

    if (plane) {
      fxpl_run(mx, player, at_order, at_row, at_tick);
      // Pan is the one thing the plane writes that was hoisted out of the loop,
      // so it is the one thing that has to be taken again.
      mixer_pan_gains(player, stereo, gain_l, gain_r);
    }

    for (int i = 0; i < run; ++i) {
      mx->mix_l[i] = 0.f;
      mx->mix_r[i] = 0.f;
    }
    for (int s = 0; s < kSends; ++s) {
      if (!send_on[s])
        continue;
      // Only the left half is accumulated into; it is copied across below. That
      // is what keeps a send tap one multiply per channel per send instead of
      // two, and a mono send is centred by definition anyway.
      for (int i = 0; i < run; ++i)
        mx->send_bus[s][0][i] = 0.f;
    }

    for (int c = 0; c < m->channels; ++c) {
      Channel *ch = &player->channels[c];
      if (ch->instrument <= 0)
        continue;

      float *v = mx->voice[c];
      const Instrument &ins = m->instruments[ch->instrument - 1];

      // **A new note gets a clean filter**, or a resonant one rings the
      // previous note's tail into the attack of the next. `Channel` carries no
      // trigger flag to read, so there are two tells and a voice needs whichever
      // of them moves for it:
      //
      // - `pos`, for anything that walks a sample. A run never crosses a tick,
      //   so nothing but a trigger (or a 9xx offset, which is one) can move it
      //   between runs, and a loop wrap happens inside the run and never shows
      //   up here.
      // - `synth_age`, for a synth. **A drum voice never touches `pos` at all**,
      //   so the first tell cannot fire for one — harmless only while no synth
      //   instrument carries a filter flag, and a bug the day one does.
      //
      // `synth_age` cannot miss a retrigger: `synth_trigger` is the only thing
      // that zeroes it and `channel_sample` is the only thing that changes it
      // otherwise, one step per rendered sample. So a trigger's zero can equal
      // the stored value only if the voice has rendered nothing since its last
      // trigger — and a voice triggered inside `player_run_begin` above is
      // playing by construction and renders the whole of the run that triggered
      // it, so the stored value is at least one.
      if (ch->pos != mx->voice_pos[c] || ch->synth_age != mx->voice_age[c])
        fx::svf_reset(&mx->voice_filter[c]);

      for (int i = 0; i < run; ++i)
        v[i] = channel_sample(ch, ins);
      mx->voice_pos[c] = ch->pos;
      mx->voice_age[c] = ch->synth_age;

      // **Before the insert, and before the mute.** The instrument's filter is
      // part of the voice -- the insert is the user's effect on top of the
      // sound the module asks for -- and it runs for a muted channel for the
      // same reason channel_sample does: a filter that stopped being advanced
      // would hand back the state from before the mute when the channel comes
      // out of it.
      //
      // Coefficients once per run, never per sample: `svf_set` costs a tan()
      // and a run is at most kMaxBlock frames, which is what that cap is for.
      //
      // **The plane takes this filter over rather than adding a second one.**
      // A channel is one voice with one SVF; a plane cutoff on a channel whose
      // instrument carries no filter flag switches this on, and one on a
      // channel whose instrument does replaces what the instrument asked for.
      // Two in series would be a filter the format never described.
      const bool plane_filter = plane && mx->fxpl_filter[c];
      if (plane_filter || (ins.flags & kInstrumentFilter) != 0u) {
        fx::Svf *f = &mx->voice_filter[c];
        int type;
        if (plane_filter) {
          fx::svf_set(f, fxpl_to_cutoff(mx->fxpl_val[c][kFxplCutoff]),
                      fxpl_to_unit(mx->fxpl_val[c][kFxplRes]), frate);
          type = (int) mx->fxpl_type[c];
        } else {
          fx::svf_set(f, (float) ins.filter_cutoff_hz,
                      (float) ins.filter_res * (1.f / 255.f), frate);
          type = (int) ((ins.flags & kInstrumentFilterType) >> 2);
        }
        for (int i = 0; i < run; ++i)
          v[i] = voice_filter_sample(f, type, v[i]);
      }

      // Sampled even when muted, because channel_sample is what advances the
      // position and a channel that stopped being read would freeze and then
      // jump back in when it unmuted. Everything downstream is skipped, sends
      // included: a mute you can still hear through the reverb is not a mute.
      if (player->muted[c])
        continue;

      slot_process_mono(&mx->insert[c], v, run, frate);

      for (int s = 0; s < kSends; ++s) {
        if (!send_on[s])
          continue;
        const float level = mx->send_level[c][s];
        if (level == 0.f)
          continue;
        float *bus = mx->send_bus[s][0];
        for (int i = 0; i < run; ++i)
          bus[i] += v[i] * level;
      }

      for (int i = 0; i < run; ++i) {
        mx->mix_l[i] += v[i] * gain_l[c];
        mx->mix_r[i] += v[i] * gain_r[c];
      }
    }

    // The sends run whether or not anything fed them this block. A reverb whose
    // source has stopped is a reverb in the middle of its tail, and a bus that
    // is only processed while signal is arriving cuts every tail off at the
    // last note.
    for (int s = 0; s < kSends; ++s) {
      if (!send_on[s])
        continue;
      float *bus_l = mx->send_bus[s][0];
      float *bus_r = mx->send_bus[s][1];
      for (int i = 0; i < run; ++i)
        bus_r[i] = bus_l[i];

      slot_process_stereo(&mx->send[s], bus_l, bus_r, run, frate);

      // A send with no effect in it returns nothing -- the `FxKind::kNone`
      // skip above is the whole behaviour. It is an effect send, not a second
      // copy of the dry mix, and returning the dry would just double every
      // voice feeding it.
      for (int i = 0; i < run; ++i) {
        mx->mix_l[i] += bus_l[i];
        mx->mix_r[i] += bus_r[i];
      }
    }

    // Gain before the master effect, because a saturator's character is a
    // function of the level going into it -- staging it after would make the
    // master fader a distortion control.
    const float k = mx->master_gain * kHeadroom;
    for (int i = 0; i < run; ++i) {
      mx->mix_l[i] *= k;
      mx->mix_r[i] *= k;
    }

    slot_process_stereo(&mx->master_fx, mx->mix_l, mx->mix_r, run, frate);

    // Mid/side. Skipped at exactly 1 and for a mono caller: `(l+r)*0.5 +
    // (l-r)*0.5` is `l` in arithmetic and not always in floats, so computing it
    // would cost the bit-identity that "width is off" is supposed to mean.
    if (stereo && mx->width != 1.f) {
      const float w = mx->width;
      for (int i = 0; i < run; ++i) {
        const float mid = (mx->mix_l[i] + mx->mix_r[i]) * 0.5f;
        const float side = (mx->mix_l[i] - mx->mix_r[i]) * 0.5f * w;
        mx->mix_l[i] = mid + side;
        mx->mix_r[i] = mid - side;
      }
    }

    limiter_process(mx, mx->mix_l, mx->mix_r, run, frate);

    for (int i = 0; i < run; ++i) {
      const int at = frame + i;
      for (int c = 0; c < channels; ++c)
        buffer[at * channels + c] +=
            (double) ((c & 1) ? mx->mix_r[i] : mx->mix_l[i]);
    }

    player_run_end(player, run);
    frame += run;
  }
}

// ----------------------------------------------------------------------------
// -- Describing a cell
// ----------------------------------------------------------------------------
//
// See ntrk_mix.h for the split and the buffer contract. Nothing below allocates
// or reads anything the caller did not hand in, so an editor may call it on
// every keystroke.

const char *
fx_kind_name(FxKind kind) {
  switch (kind) {
    case FxKind::kNone:   return "None";
    case FxKind::kShape:  return "Shaper";
    case FxKind::kFilter: return "Filter";
    case FxKind::kDelay:  return "Delay";
    case FxKind::kReverb: return "Reverb";
    default:        return nullptr;
  }
}

const char *
fx_param_name(FxKind kind, int param) {
  if (param < 0 || param >= 8)
    return nullptr;
  // The layout `Slot`'s own comment tabulates, and this is the only other place
  // it is written. **The trailing nulls are not a gap**: those indices are spare
  // on purpose, so an effect can grow a knob without moving the meaning of one
  // a caller already stores, and "param 6" is the right thing to say about one.
  static const char *const shape[8]  = {"Kind", "Drive", "Dry"};
  static const char *const filter[8] = {"Mode", "Cutoff", "Resonance"};
  static const char *const delay[8]  = {"Time", "Feedback", "Damping", "Mix",
                                        "Ping-pong"};
  // Index 1 keeps the bare name it has always printed. It is the treble shelf
  // and index 6 is the bass one, but renaming it would move a string an editor
  // and this library's own tests already read, for no new information.
  static const char *const reverb[8] = {"Size", "Damping", "Predelay", "Width",
                                        "Mix", "Decay", "Damping LF",
                                        "Diffusion"};
  switch (kind) {
    case FxKind::kShape:  return shape[param];
    case FxKind::kFilter: return filter[param];
    case FxKind::kDelay:  return delay[param];
    case FxKind::kReverb: return reverb[param];
    default:        return nullptr;
  }
}

int
fx_param_choice_count(FxKind kind, int param) {
  // **The counts `slot_apply` itself passes to `slot_choice`**, which is what makes this a
  // reading of the decode rather than a second opinion about it. A parameter with no choices
  // is continuous and answers 0.
  if (kind == FxKind::kShape && param == 0)
    return 4;                            // slot_choice(param[0], 4) -> fx::ShapeKind
  if (kind == FxKind::kFilter && param == 0)
    return 3;                            // slot_choice(param[0], 3) -> FilterMode
  if (kind == FxKind::kDelay && param == 4)
    return 2;                            // param[4] > 0.5f -> ping-pong on/off
  return 0;
}

const char *
fx_param_choice_name(FxKind kind, int param, int i) {
  const int n = fx_param_choice_count(kind, param);
  if (i < 0 || i >= n)
    return nullptr;
  static const char *const shapes[4] = {"warm", "crunch", "tape", "fold"};
  static const char *const offOn[2]  = {"off", "on"};
  if (kind == FxKind::kShape)
    return shapes[i];
  // The filter's own list, which `instrument_param_table` already reads — an editor's copy
  // would have been the third place these four words were written.
  if (kind == FxKind::kFilter)
    return filter_mode_names(nullptr)[i];
  return offOn[i];
}

float
fx_param_default(FxKind kind, int param, bool wet) {
  if (param < 0 || param >= kMixrSlotParams)
    return 0.f;
  // **Beside `fx_param_name`, and in the same shape**, because they answer two halves of one
  // question: what a knob is called and where it starts. A table in an editor would be the
  // second answer, and the one that goes stale when an effect grows a knob here.
  //
  // A slot switched on with every knob at zero is an effect that does nothing -- a reverb of
  // size 0 and mix 0 -- so the tune sounds identical and the panel says "Reverb". These are
  // the numbers that make "switch it on" mean "hear it".
  static const float shape[kMixrSlotParams]  = {0.f, 0.3f, 0.f};
  static const float filter[kMixrSlotParams] = {0.f, 0.7f, 0.2f};
  static const float delay[kMixrSlotParams]  = {0.3f, 0.35f, 0.3f, 1.f, 0.f};
  static const float reverb[kMixrSlotParams] = {0.5f, 0.4f, 0.1f, 1.f,
                                                1.f,  0.5f, 0.3f, 0.7f};
  const float *t;
  switch (kind) {
    case FxKind::kShape:  t = shape;  break;
    case FxKind::kFilter: t = filter; break;
    case FxKind::kDelay:  t = delay;  break;
    case FxKind::kReverb: t = reverb; break;
    default: return 0.f;
  }
  float v = t[param];
  // **A send is fully wet and an insert is not**, which is the one default that depends on
  // where the slot sits rather than on what is in it: a send carries only the effect and the
  // dry path is the channel itself, while a master slot at full wet replaces the whole mix.
  if (!wet && fx_param_name(kind, param) != nullptr) {
    const char *n = fx_param_name(kind, param);
    if (n[0] == 'M' && n[1] == 'i' && n[2] == 'x' && n[3] == '\0')
      v = 0.25f;
  }
  return v;
}

void
config_seed_slot(uint8_t *block, int slot) {
  if (!config_readable(block) || slot < 0 || slot >= kMixrSlots)
    return;
  const FxKind kind = config_slot_kind(block, slot);
  // The master is the last slot; everything before it is a send. `kMixrSlots == kSends + 1`
  // is asserted where the layout is, so this is reading that fact rather than restating it.
  const bool wet = slot < kSends;
  for (int p = 0; p < kMixrSlotParams; ++p)
    config_set_slot_param(block, slot, p, fx_param_default(kind, p, wet));
}

// The send names below are spelled out rather than counted, so this is the
// place that notices if the bus count ever moves.
static_assert(kSends == 4, "the slot and value names below name four sends");

const char *
fxpl_slot_name(int slot) {
  static const char *const names[kSlotCount] = {
      "Send 1", "Send 2", "Send 3", "Send 4", "Master FX", "Insert"};
  if (slot < 0 || slot >= kSlotCount)
    return nullptr;
  return names[slot];   // 6 and 7 are spare, and null says so
}

const char *
fxpl_value_name(int index) {
  static const char *const names[kFxplValues] = {
      "Pan",          "Master gain",  "Cutoff",       "Resonance",
      "Send 1 level", "Send 2 level", "Send 3 level", "Send 4 level"};
  if (index < 0 || index >= kFxplValues)
    return nullptr;
  return names[index];
}

// "Send 2 (Reverb) Damping", or "Send 2 param 1" where the kind is not to hand.
// **The degraded form is not a placeholder** — it is exactly what the format
// itself knows, and an editor showing it is telling the truth about a file
// whose meaning depends on a slot the caller has not configured.
//
// `channel` is only read by `kSlotInsert`, and a negative one leaves the kind
// unresolved rather than guessing at channel zero.
// `slot_at` wants a mutable mixer and both callers here only read, so the slot
// is reached directly. Same three cases, and the same range check.
//
// **A null mixer and an unresolvable slot are one answer**, because they mean
// the same thing to everything above: the kind is not to hand, so the format's
// own index form is what there is to say.
static const Slot *
slot_const_at(const Mixer *mx, int slot, int channel) {
  if (mx == nullptr)
    return nullptr;
  if (slot >= kSlotSend && slot < kSlotSend + kSends)
    return &mx->send[slot];
  if (slot == kSlotMaster)
    return &mx->master_fx;
  if (slot == kSlotInsert && channel >= 0 && channel < kMaxChannels)
    return &mx->insert[channel];
  return nullptr;
}

static void
slot_param_text(TextOut *t, const Mixer *mx, int slot, int param, int channel) {
  const char *sn = fxpl_slot_name(slot);
  if (sn != nullptr) {
    text_add(t, sn);
  } else {
    text_add(t, "Slot ");
    text_int(t, (long) slot);
  }

  const Slot *s = slot_const_at(mx, slot, channel);

  const char *pn = nullptr;
  if (s != nullptr) {
    const char *kn = fx_kind_name(s->kind);
    text_add(t, " (");
    text_add(t, kn != nullptr ? kn : "unknown kind");
    text_add(t, ")");
    pn = fx_param_name(s->kind, param);
  }

  if (pn != nullptr) {
    text_char(t, ' ');
    text_add(t, pn);
  } else {
    text_add(t, " param ");
    text_int(t, (long) param);
  }
}

// The mixer range's "parameter zero means the last one" memory, said rather
// than printed as a zero: a zero pan is hard left and a zero step holds a value
// still, so an editor showing 0.00 for a cell that repeats the previous one is
// showing a number the player never uses.
static void
text_mixer_value(TextOut *t, uint8_t param, bool slide) {
  if (param == 0) {
    text_add(t, slide ? " (last step)" : " -> (last parameter)");
    return;
  }
  if (slide) {
    text_char(t, ' ');
    text_step(t, param);
    text_add(t, "/tick");
  } else {
    text_add(t, " -> ");
    text_unit(t, param);
  }
}

size_t
macro_describe(const Mixer *mx, const Module *m, int macro, int input,
               char *out, size_t cap) {
  TextOut t;
  text_init(&t, out, cap);

  if (macro <= 0) {
    text_add(&t, "None");
    return t.len;
  }

  text_add(&t, "Macro ");
  text_int(&t, (long) macro);

  // A cell naming a macro past the table is ignored where it is used rather
  // than refused at load, so this is a cell an editor really does see.
  if (m == nullptr || macro > m->macro_count || macro > kMaxMacros) {
    text_add(&t, " (not in the table), input ");
    text_int(&t, (long) input);
    return t.len;
  }

  const Macro &mac = m->macros[macro - 1];
  const bool delta = (mac.flags & kMacroDelta) != 0u;
  text_add(&t, delta ? " (delta), input " : " (absolute), input ");
  text_int(&t, (long) input);

  // Legal, and what an editor holds while somebody is still filling one in.
  if (mac.target_count == 0) {
    text_add(&t, ": no targets");
    return t.len;
  }

  for (int k = 0; k < (int) mac.target_count && k < kMaxMacroTargets; ++k) {
    const MacroTarget &mt = mac.targets[k];
    text_add(&t, k == 0 ? ": " : ", ");

    int slot = 0;
    int param = 0;
    bool slide = false;
    const bool is_slot =
        mt.target < (uint8_t) kFxplSlotSlide &&
        fxpl_slot_decode((int) mt.target, &slot, &param, &slide);

    if (is_slot) {
      // Only a scope naming one channel can resolve an insert's kind; "all" and
      // "self" reach several slots or none, so the index form is the honest
      // answer for both.
      slot_param_text(&t, mx, slot, param,
                      mt.scope < (uint8_t) kMaxChannels ? (int) mt.scope : -1);
    } else {
      const char *vn = fxpl_value_name((int) mt.target);
      if (vn != nullptr) {
        text_add(&t, vn);
      } else {
        text_add(&t, "target 0x");
        text_hex(&t, mt.target, 2);
      }
    }

    text_add(&t, " [");
    if (is_slot && slot_is_global(slot))
      text_add(&t, "global");        // written once, whatever the scope says
    else if (mt.scope == kMacroScopeAll)
      text_add(&t, "all");
    else if (mt.scope == kMacroScopeSelf)
      text_add(&t, "self");
    else {
      text_add(&t, "ch");
      text_int(&t, (long) mt.scope);
    }
    text_add(&t, "]");

    // **The value this input lands on, not the scale and offset raw.**
    // `(scale * input + offset) / 256` is not arithmetic to do in one's head at
    // a status bar, and the input is right there in the cell.
    const long v = ((long) mt.scale * (long) input + (long) mt.offset) / 256;
    if (delta) {
      // A delta is gathered unclamped and clamped once at the store, so the
      // step shown here is the step, not what a clamp might leave of it.
      text_char(&t, ' ');
      text_signed2(&t, v, 255);
      text_add(&t, "/tick");
    } else {
      text_add(&t, " -> ");
      text_fixed2(&t, v < 0 ? 0 : (v > 255 ? 255 : v), 255);
    }
  }
  return t.len;
}

// `Sxy` for a set and `Dxy` for a slide, x the slot and y the parameter. Not a
// table: there are 128 of them and the two digits are the decode.
static void
slot_cmd_mnemonic(uint8_t cmd, char out[4]) {
  // 0x40..0x7F set, 0x80..0xBF slide, both `slot << 3 | param`.
  const uint8_t rel = (uint8_t) ((cmd - 0x40u) & 0x3Fu);
  out[0] = cmd < 0x80u ? 'S' : 'D';
  out[1] = (char) ('0' + (rel >> 3));
  out[2] = (char) ('0' + (rel & 7u));
  out[3] = '\0';
}

// **Every plane command with a fixed meaning, in one table** — the mixer range,
// and the two the player owns. `fxpl_mnemonic` reads it and so does
// `command_at`, which is the whole point: the mnemonic an editor draws in the
// grid and the name it offers in a completion list cannot name two different
// commands if they are the same row.
//
// The slot range is not here and cannot be: its name depends on the kind loaded
// in the slot, which is runtime state. `command_slot_fill` builds those.
static const CmdInfo *
plane_fixed_table(int *count) {
  // One list, in `ntrk.h`. These four names were written out twice in this file
  // before, and an editor's instrument panel would have been the third copy.
  const char *const *kFilterTypes = filter_mode_names(nullptr);
  static const CmdInfo kTable[19] = {
      {0x01, "PAN", "Set pan", ParamShape::Continuous, 0, nullptr},
      {0x02, "PNS", "Pan slide", ParamShape::Continuous, 0, nullptr},
      {0x03, "VOL", "Set master gain", ParamShape::Continuous, 0, nullptr},
      {0x04, "VLS", "Master gain slide", ParamShape::Continuous, 0, nullptr},
      {0x05, "CUT", "Set cutoff", ParamShape::Continuous, 0, nullptr},
      {0x06, "CTS", "Cutoff slide", ParamShape::Continuous, 0, nullptr},
      {0x07, "RES", "Set resonance", ParamShape::Continuous, 0, nullptr},
      {0x08, "RSS", "Resonance slide", ParamShape::Continuous, 0, nullptr},
      {0x09, "FLT", "Set filter type", ParamShape::Choice, 4, kFilterTypes},
      {0x10, "SN1", "Set send 1 level", ParamShape::Continuous, 0, nullptr},
      {0x11, "SN2", "Set send 2 level", ParamShape::Continuous, 0, nullptr},
      {0x12, "SN3", "Set send 3 level", ParamShape::Continuous, 0, nullptr},
      {0x13, "SN4", "Set send 4 level", ParamShape::Continuous, 0, nullptr},
      {0x14, "SS1", "Send 1 slide", ParamShape::Continuous, 0, nullptr},
      {0x15, "SS2", "Send 2 slide", ParamShape::Continuous, 0, nullptr},
      {0x16, "SS3", "Send 3 slide", ParamShape::Continuous, 0, nullptr},
      {0x17, "SS4", "Send 4 slide", ParamShape::Continuous, 0, nullptr},
      {0x30, "ACC", "Accent", ParamShape::Continuous, 0, nullptr},
      {0x31, "GLI", "Glide", ParamShape::Continuous, 0, nullptr},
  };
  if (count != nullptr)
    *count = 19;
  return kTable;
}

void
fxpl_mnemonic(FxCell cell, char out[4]) {
  const uint8_t c = cell.cmd;
  const char *m = "???";
  char buf[4];
  if (c == 0u) {
    m = "...";
  } else if (c >= (uint8_t) kFxplSlotSet && c < (uint8_t) kFxplSlotEnd) {
    slot_cmd_mnemonic(c, buf);
    m = buf;
  } else {
    int n = 0;
    const CmdInfo *tab = plane_fixed_table(&n);
    for (int i = 0; i < n; ++i) {
      if (tab[i].cmd == c) {
        m = tab[i].mnemonic;
        break;
      }
    }
  }
  out[0] = m[0];
  out[1] = m[1];
  out[2] = m[2];
  out[3] = '\0';
}

size_t
fxpl_describe(const Mixer *mx, const Module *m, int channel, FxCell cell,
              char *out, size_t cap) {
  TextOut t;
  text_init(&t, out, cap);
  const bool meta = channel < 0;
  const int cmd = (int) cell.cmd;

  if (cmd == 0) {
    text_add(&t, "None");
    return t.len;
  }

  // **A meta lane's first byte is a macro index below 0x40 and a slot command
  // above it, and the two cannot collide** — a macro index is 1..32. So the
  // lane decides, and only for the low range.
  if (meta && cmd < kFxplSlotSet)
    return macro_describe(mx, m, cmd, (int) cell.param, out, cap);

  int slot = 0;
  int param = 0;
  bool slide = false;
  if (fxpl_slot_decode(cmd, &slot, &param, &slide)) {
    slot_param_text(&t, mx, slot, param, channel);
    if (slide) {
      text_char(&t, ' ');
      text_step(&t, cell.param);
      text_add(&t, "/tick");
    } else {
      text_add(&t, " -> ");
      text_unit(&t, cell.param);
    }
    // **Which lane may carry which slot is a slot-id range check**, so saying
    // it here costs the format none of its opacity — and it is the thing an
    // editor most needs said, because a send written into a channel lane looks
    // exactly like a command that works.
    if (slot > kSlotInsert)
      text_add(&t, " (spare slot, ignored)");
    else if (meta && !slot_is_global(slot))
      text_add(&t, " (an insert names no channel here, ignored)");
    else if (!meta && slot_is_global(slot))
      text_add(&t, " (a global slot is meta-lane only, ignored)");
    return t.len;
  }

  if (cmd >= kFxplSendSet && cmd < kFxplSendSet + kSends) {
    text_add(&t, "Set send ");
    text_int(&t, (long) (cmd - kFxplSendSet + 1));
    text_add(&t, " level");
    text_mixer_value(&t, cell.param, false);
    return t.len;
  }
  if (cmd >= kFxplSendSlide && cmd < kFxplSendSlide + kSends) {
    text_add(&t, "Send ");
    text_int(&t, (long) (cmd - kFxplSendSlide + 1));
    text_add(&t, " slide");
    text_mixer_value(&t, cell.param, true);
    return t.len;
  }
  if (cmd == kFxplTypeSet) {
    const char *const *types = filter_mode_names(nullptr);
    text_add(&t, "Set filter type");
    if (cell.param == 0) {
      text_add(&t, " -> (last parameter)");
    } else {
      // Masked rather than refused, exactly as `fxpl_row` masks it.
      text_add(&t, " -> ");
      text_add(&t, types[cell.param & 3u]);
    }
    return t.len;
  }

  const char *name = nullptr;
  bool is_slide = false;
  switch (cmd) {
    case kFxplPanSet:      name = "Set pan"; break;
    case kFxplPanSlide:    name = "Pan slide"; is_slide = true; break;
    case kFxplGainSet:     name = "Set master gain"; break;
    case kFxplGainSlide:   name = "Master gain slide"; is_slide = true; break;
    case kFxplCutoffSet:   name = "Set cutoff"; break;
    case kFxplCutoffSlide: name = "Cutoff slide"; is_slide = true; break;
    case kFxplResSet:      name = "Set resonance"; break;
    case kFxplResSlide:    name = "Resonance slide"; is_slide = true; break;
    default: break;
  }
  if (name != nullptr) {
    text_add(&t, name);
    text_mixer_value(&t, cell.param, is_slide);
    return t.len;
  }

  // **Delegated rather than repeated.** The player owns 0x30..0x3F, and a
  // second copy of that table here is the drift this file exists to prevent.
  if (cmd >= (int) kFxplAccent && cmd < 0x40) {
    const size_t n = fxpl_player_describe(cell.cmd, cell.param, out, cap);
    if (n != 0)
      return n;
    text_init(&t, out, cap);
  }

  text_add(&t, "Unknown command 0x");
  text_hex(&t, (unsigned) cmd, 2);
  return t.len;
}

// ----------------------------------------------------------------------------
// -- Enumerating commands
// ----------------------------------------------------------------------------

// Global slots are 0..kSlotMaster, so a meta lane carries eight parameters for
// each of them, set and slide. Written from the constants rather than as 80.
static const int kMetaSetCount = (kSlotMaster + 1) * 8;

static bool
command_from_row(Lane lane, const CmdInfo &row, CommandInfo *out) {
  out->lane = lane;
  out->cmd = row.cmd;
  for (int i = 0; i < 3; ++i)
    out->mnemonic[i] = row.mnemonic[i];
  out->mnemonic[3] = '\0';
  TextOut t;
  text_init(&t, out->name, sizeof out->name);
  text_add(&t, row.name);
  out->shape = row.shape;
  out->choice_count = row.choice_count;
  out->choice = row.choice;
  return true;
}

// A slot set or slide, resolved against whatever kind the mixer has loaded.
// **The name is `slot_param_text`'s, called rather than copied**, so the
// completion list and the status bar say the same words about the same cell.
static bool
command_slot_fill(Lane lane, uint8_t cmd, const Mixer *mx, CommandInfo *out,
                  int channel) {
  static const char *const kShapeKinds[4] = {"warm", "crunch", "tape", "fold"};
  static const char *const kFilterModes[3] = {"lowpass", "highpass",
                                              "bandpass"};
  static const char *const kOnOff[2] = {"off", "on"};

  int slot = 0;
  int param = 0;
  bool slide = false;
  if (!fxpl_slot_decode((int) cmd, &slot, &param, &slide))
    return false;
  // Which slots a lane may name, which is `fxpl_row`'s own range check: a
  // channel lane reaches its own insert and a meta lane the global slots.
  if (lane == Lane::PlaneChannel ? slot != kSlotInsert : !slot_is_global(slot))
    return false;

  out->lane = lane;
  out->cmd = cmd;
  slot_cmd_mnemonic(cmd, out->mnemonic);
  TextOut t;
  text_init(&t, out->name, sizeof out->name);
  slot_param_text(&t, mx, slot, param, channel);
  out->shape = ParamShape::Continuous;
  out->choice_count = 0;
  out->choice = nullptr;

  const Slot *s = slot_const_at(mx, slot, channel);
  if (s == nullptr)
    return true;      // no kind to hand: a byte is all the format itself knows
  if (fx_param_name(s->kind, param) == nullptr) {
    // A spare index of a known kind, or a slot switched off. Nothing reads the
    // byte, and `Unused` says so rather than offering a range that does nothing.
    out->shape = ParamShape::Unused;
    return true;
  }
  // **A slide stays continuous even where the parameter it moves is a choice.**
  // That is the point of a discrete parameter being a band of the same 0..1: a
  // slide sweeps a shaper through its four kinds, and a dropdown of four is the
  // wrong thing to offer for the step that does it.
  if (slide)
    return true;
  if (s->kind == FxKind::kShape && param == 0) {
    out->shape = ParamShape::Choice;
    out->choice_count = 4;
    out->choice = kShapeKinds;
  } else if (s->kind == FxKind::kFilter && param == 0) {
    out->shape = ParamShape::Choice;
    out->choice_count = 3;
    out->choice = kFilterModes;
  } else if (s->kind == FxKind::kDelay && param == 4) {
    out->shape = ParamShape::Choice;
    out->choice_count = 2;
    out->choice = kOnOff;
  }
  return true;
}

int
command_count(Lane lane) {
  int n = 0;
  switch (lane) {
    case Lane::NoteEffect:
      note_fx_table(&n);
      return n;
    case Lane::PlaneChannel:
      plane_fixed_table(&n);
      return n + 16;              // this channel's insert, eight set and eight slide
    case Lane::PlaneMeta:
      return kMetaSetCount * 2;
  }
  return 0;
}

bool
command_at(Lane lane, int index, const Mixer *mx, CommandInfo *out,
           int channel) {
  if (out == nullptr || index < 0 || index >= command_count(lane))
    return false;

  if (lane == Lane::NoteEffect)
    return command_from_row(lane, note_fx_table(nullptr)[index], out);

  if (lane == Lane::PlaneChannel) {
    int n = 0;
    const CmdInfo *tab = plane_fixed_table(&n);
    if (index < n)
      return command_from_row(lane, tab[index], out);
    const int k = index - n;                 // 0..7 the sets, 8..15 the slides
    const int base = k < 8 ? kFxplSlotSet : kFxplSlotSlide - 8;
    return command_slot_fill(lane, (uint8_t) (base + kSlotInsert * 8 + k), mx,
                             out, channel);
  }

  // Meta: every global slot's eight sets in slot order, then its eight slides.
  const bool slide = index >= kMetaSetCount;
  const int k = slide ? index - kMetaSetCount : index;
  return command_slot_fill(lane,
                           (uint8_t) ((slide ? kFxplSlotSlide : kFxplSlotSet) + k),
                           mx, out, channel);
}

bool
command_lookup(Lane lane, uint8_t cmd, const Mixer *mx, CommandInfo *out,
               int channel) {
  if (out == nullptr)
    return false;
  if (lane != Lane::NoteEffect && cmd >= (uint8_t) kFxplSlotSet &&
      cmd < (uint8_t) kFxplSlotEnd)
    return command_slot_fill(lane, cmd, mx, out, channel);
  // **A meta lane's bytes below 0x40 are macro indices, not commands.** A
  // macro's name and targets belong to the module, so there is no row to find
  // and `macro_describe` is what reads one.
  if (lane == Lane::PlaneMeta)
    return false;

  int n = 0;
  const CmdInfo *tab =
      lane == Lane::NoteEffect ? note_fx_table(&n) : plane_fixed_table(&n);
  for (int i = 0; i < n; ++i) {
    if (tab[i].cmd == cmd)
      return command_from_row(lane, tab[i], out);
  }
  return false;
}

size_t
command_choice_text(const CommandInfo *ci, int choice, char *out, size_t cap) {
  TextOut t;
  text_init(&t, out, cap);
  if (ci == nullptr || ci->choice == nullptr || choice < 0 ||
      choice >= ci->choice_count)
    return t.len;
  text_add(&t, ci->choice[choice]);
  return t.len;
}

// Which alternative a parameter byte selects. Three different readings, and
// each one is the reading the code that acts on the byte already does.
static int
command_choice_index(const CommandInfo *ci, uint8_t param) {
  if (ci->choice_count <= 0)
    return 0;
  if (ci->lane == Lane::NoteEffect) {
    // Every choice in that lane is an `E` sub-command, so the value is the low
    // nibble. Glissando is on for anything but zero; a waveform selector is
    // three bits, two of shape and one for the retrigger.
    const int v = (int) (param & 15u);
    if (ci->cmd == 0xE3u)
      return v != 0 ? 1 : 0;
    return v % ci->choice_count;
  }
  if (ci->cmd == (uint8_t) kFxplTypeSet)
    return (int) (param & 3u);        // masked exactly as `fxpl_row` masks it
  return slot_choice(slot_from_param((int) param), ci->choice_count);
}

static void
note_value_text(TextOut *t, const CommandInfo *ci, uint8_t param) {
  const long hi = (long) ((param >> 4) & 15u);
  const long lo = (long) (param & 15u);

  if (ci->shape == ParamShape::SplitNibble) {
    switch (ci->cmd) {
      case 0x00:
        if (param == 0u) {
          text_add(t, "none");
          return;
        }
        text_char(t, '+');
        text_int(t, hi);
        text_add(t, " +");
        text_int(t, lo);
        return;
      case 0x04:
      case 0x07:
        if (param == 0u) {
          text_add(t, "(unchanged)");
          return;
        }
        text_add(t, "speed ");
        text_int(t, hi);
        text_add(t, ", depth ");
        text_int(t, lo);
        return;
      case 0x0E: {
        // **The container renders through the sub-command it selects.** Two
        // nibbles would be a number a reader has to split again, and the split
        // is exactly what the table already knows how to do.
        CommandInfo sub;
        if (!command_lookup(Lane::NoteEffect,
                            (uint8_t) (0xE0u | (unsigned) hi), nullptr, &sub))
          return;
        char b[48];
        command_value_text(&sub, param, b, sizeof b);
        text_add(t, sub.mnemonic);
        if (b[0] != '\0') {
          text_char(t, ' ');
          text_add(t, b);
        }
        return;
      }
      default:
        // 0x05, 0x06 and 0x0A: a volume slide, up winning over down exactly as
        // `volume_slide` resolves it.
        if (param == 0u) {
          text_add(t, "(unchanged)");
          return;
        }
        text_char(t, hi > 0 ? '+' : '-');
        text_int(t, hi > 0 ? hi : lo);
        text_add(t, "/tick");
        return;
    }
  }

  switch (ci->cmd) {
    case 0x01:
    case 0x02:
    case 0x03:
      if (param == 0u) {
        text_add(t, "(unchanged)");
        return;
      }
      text_add(t, "speed ");
      text_int(t, (long) param);
      return;
    case 0x08: text_pan(t, (long) param); return;
    case 0x09:
      if (param == 0u) {
        text_add(t, "(unchanged)");
        return;
      }
      text_int(t, (long) param * 256);
      text_add(t, " frames");
      return;
    case 0x0B:
      text_add(t, "order ");
      text_int(t, (long) param);
      return;
    case 0x0C: text_int(t, param > 64u ? 64 : (long) param); return;
    case 0x0D:
      // Decimal, which is ProTracker's: 0x10 is row ten, not row sixteen.
      text_add(t, "row ");
      text_int(t, hi * 10 + lo);
      return;
    case 0x0F:
      if (param == 0u) {
        text_add(t, "(ignored)");
        return;
      }
      text_int(t, (long) param);
      text_add(t, param < 32u ? " ticks/row" : " BPM");
      return;
    case 0xE6:
      if (lo == 0) {
        text_add(t, "set loop point");
        return;
      }
      text_int(t, lo);
      text_add(t, " times");
      return;
    case 0xE8: text_pan(t, lo * 17); return;
    case 0xE9:
      text_int(t, lo);
      text_add(t, " ticks");
      return;
    case 0xEC:
      text_add(t, "tick ");
      text_int(t, lo);
      return;
    case 0xED:
      text_int(t, lo);
      text_add(t, " ticks");
      return;
    case 0xEE:
      text_int(t, lo);
      text_add(t, " rows");
      return;
    default:
      // The rest is a plain number: a fine slide, a finetune, a retrigger.
      text_int(t, ci->cmd >= 0xE0u ? lo : (long) param);
      return;
  }
}

static void
plane_value_text(TextOut *t, const CommandInfo *ci, uint8_t param) {
  const uint8_t c = ci->cmd;

  // **A slot command carries no "parameter zero means the last one" memory**, so
  // it is the one range that prints its zero: zero is a legitimate normalised
  // value and a zero step a legitimate way to hold one still.
  if (c >= (uint8_t) kFxplSlotSet && c < (uint8_t) kFxplSlotEnd) {
    if (c >= (uint8_t) kFxplSlotSlide) {
      text_step(t, param);
      text_add(t, "/tick");
    } else {
      text_unit(t, param);
    }
    return;
  }

  const bool slide = fxpl_slide_index((int) c) >= 0;
  if (c < (uint8_t) kFxplCmdCount && param == 0u) {
    text_add(t, slide ? "(last step)" : "(last parameter)");
    return;
  }
  if (slide) {
    text_step(t, param);
    text_add(t, "/tick");
    return;
  }
  if (c == (uint8_t) kFxplPanSet) {
    text_pan(t, (long) param);
    return;
  }
  if (c == kFxplSlide) {              // 0x31, the player's glide time in ticks
    if (param == 0u) {
      text_add(t, "off");
      return;
    }
    text_int(t, (long) param);
    text_add(t, " ticks");
    return;
  }
  // A gain, a cutoff, a resonance, a send level, an accent: all 0..1 in the
  // command's own parameter space, which is where a slide accumulates.
  text_unit(t, param);
}

size_t
command_value_text(const CommandInfo *ci, uint8_t param, char *out,
                   size_t cap) {
  TextOut t;
  text_init(&t, out, cap);
  if (ci == nullptr || ci->shape == ParamShape::Unused)
    return t.len;      // nothing reads the byte; a number here would be a lie
  if (ci->shape == ParamShape::Choice)
    return command_choice_text(ci, command_choice_index(ci, param), out, cap);
  if (ci->lane == Lane::NoteEffect)
    note_value_text(&t, ci, param);
  else
    plane_value_text(&t, ci, param);
  return t.len;
}

}  // namespace mix
}  // namespace ntrk
