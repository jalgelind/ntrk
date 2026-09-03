// ntrk_fx_delay -- the stereo delay's arithmetic. See ntrk_fx_delay.h for the
// API, the parameter ranges and the reasoning behind them.
//
// A translation unit rather than a header because every function here is called
// once per *block*: the per-sample work is a loop inside `delay_process`, so
// there is nothing for an inline to save and a call is amortised over a whole
// buffer. The per-sample primitives it uses stay inline, in ntrk_dsp.h.
//
// This file may also be `#include`d into one translation unit of your own — that
// is what ntrk_unity.h does. Compile it separately or include it, not both.
//
// Public domain / CC0. Written for the no2 project.

#include "ntrk_fx_delay.h"

#include <cmath>

namespace ntrk {
namespace fx {

// A tap has to lie strictly inside the line, and the interpolator reads one
// frame beyond the integer tap, so two frames of the buffer are never delay time.
const int kDelayGuardFrames = 2;

const float kDelayMinSeconds = 0.001f;
const float kDelayMaxSeconds = 2.f;
const float kDelayMaxFeedback = 1.05f;

// Damping is a one-pole coefficient, and 1.0 would freeze the filter on its own
// state: with feedback that is a DC drone that never decays, not a dark repeat.
const float kDelayMaxDamping = 0.95f;

// The time glide, in seconds. Long enough that a moved tap sounds like tape
// rather than a step, short enough to arrive before anyone notices it is late.
const float kDelayGlideSeconds = 0.02f;

float
delay_clamp(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

size_t
delay_bytes_needed(float max_seconds, float sample_rate) {
  if (!(sample_rate > 0.f) || !(max_seconds > 0.f))
    return 0;
  const float secs = delay_clamp(max_seconds, kDelayMinSeconds, kDelayMaxSeconds);
  const size_t frames =
      (size_t) std::ceil(secs * sample_rate) + (size_t) kDelayGuardFrames;
  return frames * 2u * sizeof(float);
}

void
delay_reset(Delay *d) {
  if (d == nullptr || d->buffer == nullptr)
    return;
  const size_t floats = (size_t) d->capacity * 2u;
  for (size_t i = 0; i < floats; ++i)
    d->buffer[i] = 0.f;
  d->write = 0;
  d->lp_left = 0.f;
  d->lp_right = 0.f;
  d->time_cur = d->time_target;
  d->snap = true;
}

bool
delay_init(Delay *d, float *buffer, size_t bytes, float sample_rate,
           float max_seconds) {
  if (d == nullptr)
    return false;
  *d = Delay();
  if (buffer == nullptr || !(sample_rate > 0.f) || !(max_seconds > 0.f))
    return false;

  const size_t needed = delay_bytes_needed(max_seconds, sample_rate);
  if (needed == 0 || bytes < needed)
    return false;

  const float secs = delay_clamp(max_seconds, kDelayMinSeconds, kDelayMaxSeconds);
  const int capacity =
      (int) ((size_t) std::ceil(secs * sample_rate) + (size_t) kDelayGuardFrames);
  if (capacity <= kDelayGuardFrames)
    return false;

  d->buffer = buffer;
  d->capacity = capacity;
  d->rate = sample_rate;

  // One over the glide length in samples: a plain linear approach, because exp()
  // is the usual coefficient and is not bit-identical across platforms.
  d->glide = delay_clamp(1.f / (kDelayGlideSeconds * sample_rate), 0.f, 1.f);

  d->time_target = delay_clamp(kDelayMinSeconds * sample_rate, 1.f,
                               (float) (capacity - kDelayGuardFrames));
  delay_reset(d);
  return true;
}

void
delay_set(Delay *d, float time_seconds, float feedback, float damping, float mix,
          bool ping_pong) {
  if (d == nullptr || d->buffer == nullptr)
    return;

  const float secs =
      delay_clamp(time_seconds, kDelayMinSeconds, kDelayMaxSeconds);
  const float max_frames = (float) (d->capacity - kDelayGuardFrames);
  d->time_target = delay_clamp(secs * d->rate, 1.f, max_frames);

  d->feedback = delay_clamp(feedback, 0.f, kDelayMaxFeedback);
  d->damping = delay_clamp(damping, 0.f, kDelayMaxDamping);
  d->mix = delay_clamp(mix, 0.f, 1.f);
  d->ping_pong = ping_pong;
}

float
delay_tap(const float *line, int capacity, int write, float frames_back) {
  const int whole = (int) frames_back;
  const float frac = frames_back - (float) whole;

  int i0 = write - whole;
  while (i0 < 0)
    i0 += capacity;
  int i1 = i0 - 1;
  if (i1 < 0)
    i1 += capacity;

  return line[i0] * (1.f - frac) + line[i1] * frac;
}

void
delay_process(Delay *d, float *left, float *right, int frames) {
  if (d == nullptr || d->buffer == nullptr || d->capacity <= kDelayGuardFrames)
    return;
  if (left == nullptr || right == nullptr || frames <= 0)
    return;

  // The passthrough. Returning before anything is read or written is the only
  // way to promise the samples come back bit for bit: `x * 1 + y * 0` is not the
  // identity for -0.0, and is not one at all if y is not finite.
  if (d->mix <= 0.f)
    return;

  float *line_l = d->buffer;
  float *line_r = d->buffer + d->capacity;

  const float max_frames = (float) (d->capacity - kDelayGuardFrames);
  const float wet = d->mix;
  const float dry = 1.f - d->mix;
  const float fb = d->feedback;
  const float damp = d->damping;
  const float keep = 1.f - damp;

  if (d->snap) {
    d->time_cur = d->time_target;
    d->snap = false;
  }

  int w = d->write;
  float t = d->time_cur;
  float lp_l = d->lp_left;
  float lp_r = d->lp_right;

  for (int i = 0; i < frames; ++i) {
    t += (d->time_target - t) * d->glide;
    const float tap_frames = delay_clamp(t, 1.f, max_frames);

    const float out_l = delay_tap(line_l, d->capacity, w, tap_frames);
    const float out_r = delay_tap(line_r, d->capacity, w, tap_frames);

    // The lowpass sits in the feedback path, not on the output, so each pass
    // round the line is darker than the last and the tail closes down.
    lp_l = flush_denorm(out_l * keep + lp_l * damp);
    lp_r = flush_denorm(out_r * keep + lp_r * damp);

    // Ping-pong crosses the feedback rather than summing the input to one side:
    // a hit on the left returns left, then right, then left, and a stereo source
    // stays stereo going in.
    const float fed_l = d->ping_pong ? lp_r : lp_l;
    const float fed_r = d->ping_pong ? lp_l : lp_r;

    const float in_l = left[i];
    const float in_r = right[i];

    line_l[w] = flush_denorm(in_l + soft_clip(fed_l * fb));
    line_r[w] = flush_denorm(in_r + soft_clip(fed_r * fb));

    left[i] = in_l * dry + out_l * wet;
    right[i] = in_r * dry + out_r * wet;

    if (++w >= d->capacity)
      w = 0;
  }

  d->write = w;
  d->time_cur = t;
  d->lp_left = lp_l;
  d->lp_right = lp_r;
}

}  // namespace fx
}  // namespace ntrk
