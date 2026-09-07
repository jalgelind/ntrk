// ntrk's own tests, with nothing behind them.
//
//     c++ -std=c++11 -Wall -Wextra -ffp-contract=off \
//         -o /tmp/test_ntrk third_party/ntrk/test_ntrk.cc
//     /tmp/test_ntrk
//
// `-ffp-contract=off` is not optional and not a style choice: the fingerprints
// at the bottom of this file are of the *quantised output*, and a fused
// multiply-add changes it. The project applies the same flag to the product and
// to its own tests so that ARM64 and wasm compute identical floats; a copy of
// these checks built without it is checking a different program. The shipped
// tune's fingerprint differed by exactly this, once, and that is how it
// was found.
//
// No test framework, no build system, no project. That is the point rather than
// minimalism for its own sake: this file compiling and passing on its own is the
// evidence that the library really is standalone, which a test run from inside
// a host project cannot give however green it is.
//
// The same checks also run under the host project's suite, where they get
// Catch2's reporting. Neither copy is the real one; they are the same claims
// asked twice, once with the project and once without.

#include "ntrk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      ++g_failures;                                                            \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
    }                                                                          \
  } while (0)

// ---------------------------------------------------------------------------
// Building modules to test against.
//
// Everything here is assembled in memory rather than loaded, so the malformed
// cases can be produced exactly: a truncated sample blob or a loop one word past
// the end are a field each, and impossible to find lying around.
// ---------------------------------------------------------------------------

struct Spec {
  int channels = 1;
  int rows = 4;
  int speed = 6;
  int bpm = 125;
  int orders = 1;
  int patterns = 1;
  int instruments = 1;
  int restart = 0;
  int sample_len = 32;
  int loop_start = 0;
  int loop_len = 32;      // held by default, so effects are what move it
  uint8_t volume = 64;
  int8_t finetune = 0;
};

// Big enough for every module below; a fixed buffer keeps the file free of
// allocation as well as of the standard library's containers.
//
// **`build` writes the plainest legal file there is** -- three octaves, PCM8,
// no block directory -- and that is what most of these tests want: a module
// whose only interesting property is the one the test is about. The blocks and
// the fields above byte 17 of an instrument entry get their own builders below.
static uint8_t g_bytes[1 << 16];
static size_t g_size = 0;

static void
put_u16(size_t at, uint16_t x) {
  g_bytes[at] = (uint8_t) (x & 0xff);
  g_bytes[at + 1] = (uint8_t) (x >> 8);
}

static void
put_u32(size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i)
    g_bytes[at + (size_t) i] = (uint8_t) ((x >> (8 * i)) & 0xff);
}

static size_t
patterns_at(const Spec &s) {
  return 32u + (size_t) s.orders + (size_t) s.instruments * 32u;
}

static size_t
cell_at(const Spec &s, int row, int channel) {
  return patterns_at(s) +
         (((size_t) row * (size_t) s.channels) + (size_t) channel) * 4u;
}

static void
set_cell(const Spec &s, int row, int channel, uint8_t note, uint8_t instrument,
         uint8_t effect, uint8_t param) {
  const size_t at = cell_at(s, row, channel);
  g_bytes[at + 0] = note;
  g_bytes[at + 1] = instrument;
  g_bytes[at + 2] = effect;
  g_bytes[at + 3] = param;
}

static void
build(const Spec &s, bool with_note = true) {
  const size_t order_bytes = (size_t) s.orders;
  const size_t instrument_bytes = (size_t) s.instruments * 32u;
  const size_t pattern_bytes =
      (size_t) s.patterns * (size_t) s.rows * (size_t) s.channels * 4u;
  g_size = 32u + order_bytes + instrument_bytes + pattern_bytes +
           (size_t) s.sample_len;
  memset(g_bytes, 0, g_size);

  memcpy(g_bytes, "NTRK", 4);
  put_u16(4, 2);
  put_u16(6, (uint16_t) s.channels);
  put_u16(8, (uint16_t) s.rows);
  put_u16(10, (uint16_t) s.speed);
  put_u16(12, (uint16_t) s.bpm);
  put_u16(14, (uint16_t) s.orders);
  put_u16(16, (uint16_t) s.patterns);
  put_u16(18, (uint16_t) s.instruments);
  put_u16(20, (uint16_t) s.restart);
  put_u32(22, (uint32_t) s.sample_len);   // the blob's declared length
  put_u16(26, 0u);                        // no block directory
  g_bytes[28] = 36u;                      // note_max: three octaves

  size_t at = 32u + order_bytes;    // every order plays pattern 0, so zeros
  for (int i = 0; i < s.instruments; ++i) {
    const size_t e = at + (size_t) i * 32u;
    put_u32(e + 0, 0u);
    put_u32(e + 4, (uint32_t) s.sample_len);
    put_u32(e + 8, (uint32_t) s.loop_start);
    put_u32(e + 12, (uint32_t) s.loop_len);
    g_bytes[e + 16] = s.volume;
    g_bytes[e + 17] = (uint8_t) s.finetune;
  }

  if (with_note)
    set_cell(s, 0, 0, 25u, 1u, 0u, 0u);

  // A square wave: unambiguously not silence, and every frame far from zero.
  const size_t blob = patterns_at(s) + pattern_bytes;
  for (int i = 0; i < s.sample_len; ++i)
    g_bytes[blob + (size_t) i] = (uint8_t) (int8_t) ((i % 8) < 4 ? 100 : -100);
}

static double g_buffer[65536];

static void
render(ntrk::Player *player, int frames) {
  for (int i = 0; i < frames * 2; ++i)
    g_buffer[i] = 0.0;
  ntrk::render_add(player, g_buffer, frames, 2, 48000.f);
}

static double
peak_between(int first, int last) {
  double peak = 0.0;
  for (int f = first; f < last; ++f) {
    const double x = g_buffer[(size_t) f * 2u];
    const double a = x < 0.0 ? -x : x;
    if (a > peak)
      peak = a;
  }
  return peak;
}

// ---------------------------------------------------------------------------
// The tests.
// ---------------------------------------------------------------------------

static void
test_loads() {
  printf("a well-formed module loads\n");
  Spec s;
  build(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.channels == 1);
  CHECK(m.rows == 4);
  CHECK(m.instrument_count == 1);
  CHECK(m.instruments[0].length == 32u);
  CHECK(m.instruments[0].data != NULL);
}

static void
test_refusals() {
  printf("a malformed module is refused, not read\n");
  Spec s;
  ntrk::Module m;

  build(s);
  CHECK(!ntrk::module_load(&m, NULL, 0));
  CHECK(!ntrk::module_load(&m, g_bytes, 8));            // shorter than a header

  build(s);
  g_bytes[1] = 'X';
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));       // wrong magic

  // **Version 3, not 2.** This check read `put_u16(4, 2)` while 2 was unknown;
  // once v2 landed it went on passing for an entirely different reason -- the
  // v1-shaped bytes leave `note_max` at 0, which v2 refuses. A test that passes
  // for the wrong reason is worse than no test, so the two claims are separated
  // here: an unknown version, and a v2 header that is malformed.
  build(s);
  put_u16(4, 3);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));       // unknown version

  build(s);
  put_u16(4, 2);
  g_bytes[28] = 0;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));       // v2 with note_max 0

  build(s);
  put_u16(4, 2);
  g_bytes[28] = 48;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));       // note_max neither 36 nor 96

  build(s);
  put_u16(6, 99);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));       // impossible channels

  build(s);
  g_bytes[32] = 7u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));       // order names no pattern

  // The one that matters most: everything parses and the sample blob the
  // instruments point into is not all there.
  build(s);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size - 8));

  build(s);
  put_u32(32 + 1 + 4, 4096u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));       // sample past the blob

  build(s);
  put_u32(32 + 1 + 8, 24u);
  put_u32(32 + 1 + 12, 16u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));       // loop past the sample

  build(s);
  g_bytes[32 + 1 + 16] = 200u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));       // louder than full
}

static void
test_noise_and_silence() {
  printf("a note sounds, and no note does not\n");
  Spec s;
  ntrk::Module m;
  ntrk::Player p;

  build(s);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, 1024);
  CHECK(peak_between(0, 1024) > 0.01);

  // The vacuity guard: without it, a player emitting a click or a DC offset
  // would pass the check above for the wrong reason.
  build(s, false);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, 1024);
  CHECK(peak_between(0, 1024) == 0.0);
}

static double g_first[65536];

static void
test_deterministic() {
  printf("the same module renders the same samples\n");
  Spec s;
  s.rows = 16;
  s.speed = 3;
  build(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Player a;
  ntrk::player_start(&a, &m);
  render(&a, 2048);
  memcpy(g_first, g_buffer, sizeof(double) * 4096u);

  ntrk::Player b;
  ntrk::player_start(&b, &m);
  render(&b, 2048);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * 4096u) == 0);

  // And rendering in pieces matches rendering in one go: the buffer size an
  // audio callback hands over is not ours to choose, so a tick boundary falling
  // inside a buffer must not change a sample.
  for (int i = 0; i < 4096; ++i)
    g_buffer[i] = 0.0;
  ntrk::Player c;
  ntrk::player_start(&c, &m);
  for (int i = 0; i < 8; ++i)
    ntrk::render_add(&c, g_buffer + (size_t) i * 512u, 256, 2, 48000.f);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * 4096u) == 0);
}

static void
test_adds() {
  printf("render adds rather than assigns\n");
  Spec s;
  build(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::Player p;
  ntrk::player_start(&p, &m);

  for (int i = 0; i < 512; ++i)
    g_buffer[i] = 0.25;
  ntrk::render_add(&p, g_buffer, 256, 2, 48000.f);

  bool moved = false;
  for (int i = 0; i < 512; ++i)
    if (g_buffer[i] != 0.25)
      moved = true;
  CHECK(moved);
  CHECK(peak_between(0, 256) > 0.25);
}

static void
test_looping() {
  printf("a looped sample keeps sounding, a one-shot does not\n");
  Spec s;
  s.rows = 64;
  s.speed = 31;          // nothing retriggers for a long while
  s.sample_len = 32;
  s.loop_start = 8;
  s.loop_len = 8;
  build(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.instruments[0].loop_len == 8u);
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  render(&p, 4096);
  CHECK(peak_between(3584, 4096) > 0.01);

  // The same module without the loop, which is what stops the check above
  // passing for any reason other than the loop.
  s.loop_len = 0;
  build(s);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, 4096);
  CHECK(peak_between(3584, 4096) == 0.0);
}

static void
test_note_delay_and_cut() {
  // 960 frames a tick at 125 BPM, speed 6, so a row is 5760 frames.
  printf("a note delay holds the note back, a cut ends it early\n");
  Spec s;
  ntrk::Module m;
  ntrk::Player p;

  build(s);
  set_cell(s, 0, 0, 25u, 1u, 0x0E, 0xD4);      // delay four ticks
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, 5760);
  CHECK(peak_between(0, 3600) == 0.0);
  CHECK(peak_between(4200, 5760) > 0.01);

  build(s);
  set_cell(s, 0, 0, 25u, 1u, 0x0E, 0xC2);      // cut on tick two
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, 5760);
  CHECK(peak_between(0, 1800) > 0.01);
  CHECK(peak_between(2100, 5760) == 0.0);
}

static double
tail_after_slide(uint8_t effect, uint8_t param) {
  Spec s;
  build(s);
  set_cell(s, 0, 0, 25u, 1u, 0x0C, 16u);       // start quiet
  set_cell(s, 1, 0, 0u, 0u, effect, param);
  ntrk::Module m;
  if (!ntrk::module_load(&m, g_bytes, g_size))
    return -1.0;
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  render(&p, 11520);
  return peak_between(11000, 11520);
}

static void
test_slides() {
  printf("a fine slide moves once, a full slide every tick\n");
  const double fine = tail_after_slide(0x0E, 0xA8);   // once, up eight
  const double full = tail_after_slide(0x0A, 0x80);   // eight a tick
  CHECK(fine > 0.0);
  CHECK(full > fine * 1.5);
}

static double
tremolo_spread(bool tremolo) {
  Spec s;
  s.volume = 32u;    // room to swing both ways; volume clamps at 64
  build(s);
  set_cell(s, 0, 0, 25u, 1u, tremolo ? 0x07 : 0x00, tremolo ? 0x48u : 0u);
  ntrk::Module m;
  if (!ntrk::module_load(&m, g_bytes, g_size))
    return -1.0;
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  render(&p, 5760);

  double low = 1e9, high = 0.0;
  for (int tick = 1; tick < 6; ++tick) {
    const double v = peak_between(tick * 960, (tick + 1) * 960);
    if (v < low)
      low = v;
    if (v > high)
      high = v;
  }
  return high - low;
}

static void
test_tremolo() {
  printf("tremolo moves the level within a row\n");
  CHECK(tremolo_spread(true) > tremolo_spread(false) + 0.01);
}

// ---------------------------------------------------------------------------
// The editor-facing half: save, seek, mute, preview. None of it is on the
// replay path, so none of it is covered by anything above.
// ---------------------------------------------------------------------------

static uint8_t g_saved[1 << 16];

static double
abs_peak(const double *buffer, int frames) {
  double peak = 0.0;
  for (int i = 0; i < frames; ++i) {
    const double v = buffer[i] < 0.0 ? -buffer[i] : buffer[i];
    if (v > peak)
      peak = v;
  }
  return peak;
}

static void
test_save_round_trip() {
  printf("a saved module is the file it was loaded from\n");

  Spec s;
  s.channels = 2;
  s.rows = 8;
  s.patterns = 2;
  s.orders = 3;
  s.sample_len = 40;
  s.loop_start = 8;
  s.loop_len = 16;
  build(s);
  set_cell(s, 3, 1, 13u, 1u, 12u, 32u);   // a cell that is not the default

  ntrk::Module a;
  CHECK(ntrk::module_load(&a, g_bytes, g_size));

  // Sizing call: no buffer, just the count.
  size_t need = 0;
  CHECK(ntrk::module_save(&a, nullptr, 0, &need));
  CHECK(need == g_size);

  // One byte short refuses rather than writing a truncated file.
  size_t wrote = 12345u;
  CHECK(!ntrk::module_save(&a, g_saved, need - 1u, &wrote));

  CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &wrote));
  CHECK(wrote == need);

  // Byte for byte against the hand-assembled original, which is the strongest
  // form of the claim -- and it holds only because every instrument here shares
  // one sample at offset 0, which is what the builder emits.
  CHECK(memcmp(g_saved, g_bytes, wrote) == 0);

  ntrk::Module b;
  CHECK(ntrk::module_load(&b, g_saved, wrote));
  CHECK(b.channels == a.channels);
  CHECK(b.rows == a.rows);
  CHECK(b.speed == a.speed);
  CHECK(b.bpm == a.bpm);
  CHECK(b.order_count == a.order_count);
  CHECK(b.pattern_count == a.pattern_count);
  CHECK(b.instrument_count == a.instrument_count);
  CHECK(b.restart == a.restart);

  int cells_same = 1;
  const int cells = a.pattern_count * a.rows * a.channels;
  for (int i = 0; i < cells; ++i) {
    if (a.patterns[i].note != b.patterns[i].note ||
        a.patterns[i].instrument != b.patterns[i].instrument ||
        a.patterns[i].effect != b.patterns[i].effect ||
        a.patterns[i].param != b.patterns[i].param)
      cells_same = 0;
  }
  CHECK(cells_same);

  int samples_same = 1;
  for (int i = 0; i < a.instrument_count; ++i) {
    const ntrk::Instrument &x = a.instruments[i];
    const ntrk::Instrument &y = b.instruments[i];
    if (x.length != y.length || x.loop_start != y.loop_start ||
        x.loop_len != y.loop_len || x.volume != y.volume ||
        x.finetune != y.finetune)
      samples_same = 0;
    for (uint32_t k = 0; k < x.length; ++k)
      if (x.data[k] != y.data[k])
        samples_same = 0;
  }
  CHECK(samples_same);

  // Two instruments sharing one blob offset are written out twice, which the
  // writer's comment says outright. Pinned here so it stays a decision.
  Spec two = s;
  two.instruments = 2;
  build(two);
  ntrk::Module c;
  CHECK(ntrk::module_load(&c, g_bytes, g_size));
  size_t two_need = 0;
  CHECK(ntrk::module_save(&c, nullptr, 0, &two_need));
  CHECK(two_need == g_size + (size_t) two.sample_len);
}

static void
test_mute() {
  printf("a muted channel is silent, and does not jump when it comes back\n");

  Spec s;
  s.channels = 2;
  build(s);
  set_cell(s, 0, 1, 25u, 1u, 0u, 0u);   // both channels sound

  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Player open_player;
  ntrk::player_start(&open_player, &m);
  double both[128] = {0};
  ntrk::render_add(&open_player, both, 128, 1, 22050.f);

  ntrk::Player half;
  ntrk::player_start(&half, &m);
  half.muted[1] = true;
  double one[128] = {0};
  ntrk::render_add(&half, one, 128, 1, 22050.f);

  CHECK(abs_peak(one, 128) > 0.0);                          // channel 0 sounds
  CHECK(abs_peak(one, 128) < abs_peak(both, 128));          // channel 1 does not

  // **The point of sampling a muted channel and dropping it**: its position has
  // to be where the unmuted reference left it, or unmuting jumps.
  CHECK(half.channels[1].pos == open_player.channels[1].pos);

  ntrk::Player silent;
  ntrk::player_start(&silent, &m);
  silent.muted[0] = true;
  silent.muted[1] = true;
  double none[128] = {0};
  ntrk::render_add(&silent, none, 128, 1, 22050.f);
  CHECK(abs_peak(none, 128) == 0.0);
}

static void
test_slot_mute() {
  printf("a slot mute silences one track for one section, and travels in the "
         "file\n");

  Spec s;
  s.channels = 2;
  s.orders = 2;
  s.patterns = 2;
  build(s);
  // Both channels sound from row 0. Both order positions play pattern 0, which
  // is what makes this a test of a per-POSITION mute rather than a per-pattern
  // one: the same pattern is silent in one position and not in the other.
  set_cell(s, 0, 1, 25u, 1u, 0u, 0u);

  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(!m.has_slot_mute);
  CHECK(!ntrk::slot_muted(&m, 0, 1));

  // ---- the accessors -------------------------------------------------
  ntrk::slot_set_muted(&m, 0, 1, true);
  CHECK(m.has_slot_mute);
  CHECK(ntrk::slot_muted(&m, 0, 1));
  CHECK(!ntrk::slot_muted(&m, 0, 0));      // the other channel
  CHECK(!ntrk::slot_muted(&m, 1, 1));      // the same channel, other position

  // Out of range is "not muted" rather than silence, and writing there is a
  // no-op rather than a stray bit somewhere else.
  CHECK(!ntrk::slot_muted(&m, -1, 0));
  CHECK(!ntrk::slot_muted(&m, m.order_count, 0));
  CHECK(!ntrk::slot_muted(&m, 0, m.channels));
  ntrk::slot_set_muted(&m, 99, 0, true);
  ntrk::slot_set_muted(&m, 0, 99, true);
  CHECK(m.slot_mute[0] == 2u);             // still just channel 1

  // Clearing the last bit clears the flag, so a file stops carrying a block it
  // no longer needs.
  ntrk::slot_set_muted(&m, 0, 1, false);
  CHECK(!m.has_slot_mute);
  ntrk::slot_set_muted(&m, 0, 1, true);

  // ---- the player ----------------------------------------------------
  // **The reference is a SEPARATE module.** Rendering both from `m` and setting
  // the bit in between would compare a muted tune against itself, which is a
  // pair of identical peaks and a test that passes for the wrong reason.
  ntrk::Module open_module;
  CHECK(ntrk::module_load(&open_module, g_bytes, g_size));
  CHECK(!open_module.has_slot_mute);

  ntrk::Player open_player;
  ntrk::player_start(&open_player, &open_module);
  double both[128] = {0};
  ntrk::render_add(&open_player, both, 128, 1, 22050.f);

  ntrk::Player muted;
  ntrk::player_start(&muted, &m);
  double one[128] = {0};
  ntrk::render_add(&muted, one, 128, 1, 22050.f);

  CHECK(abs_peak(one, 128) > 0.0);                       // channel 0 sounds
  CHECK(abs_peak(one, 128) < abs_peak(both, 128));       // channel 1 does not

  // **Sampled and dropped, like the listener's mute** -- the position has to be
  // where an unmuted reference left it, or the track jumps when the tune walks
  // into the next section.
  CHECK(muted.channels[1].pos == open_player.channels[1].pos);

  // ...and the OTHER position is untouched: a slot mute is a section, not a
  // track. Both order entries play the same pattern here, so the second
  // position sounding louder than the first can only be the mute.
  ntrk::Player at_one;
  ntrk::player_start(&at_one, &m);
  ntrk::player_seek(&at_one, 1, 0);
  double second[128] = {0};
  ntrk::render_add(&at_one, second, 128, 1, 22050.f);
  CHECK(abs_peak(second, 128) > abs_peak(one, 128));

  // ---- the round trip ------------------------------------------------
  size_t need = 0;
  CHECK(ntrk::module_save(&m, nullptr, 0, &need));
  size_t wrote = 0;
  CHECK(ntrk::module_save(&m, g_saved, sizeof g_saved, &wrote));
  CHECK(wrote == need);

  ntrk::Module back;
  CHECK(ntrk::module_load(&back, g_saved, wrote));
  CHECK(back.has_slot_mute);
  CHECK(ntrk::slot_muted(&back, 0, 1));
  CHECK(!ntrk::slot_muted(&back, 1, 1));
  CHECK(!ntrk::slot_muted(&back, 0, 0));

  // **Nothing muted writes no block**, so a file that does not use the feature
  // stays readable by a reader that does not know the id -- which matters
  // because the block is critical.
  ntrk::Module clean;
  CHECK(ntrk::module_load(&clean, g_bytes, g_size));
  size_t clean_need = 0;
  CHECK(ntrk::module_save(&clean, nullptr, 0, &clean_need));
  CHECK(clean_need < need);
  CHECK(need - clean_need ==
        (size_t) ntrk::kDirectoryEntryBytes + (size_t) ntrk::kSmutHeaderBytes +
            (size_t) m.order_count * 2u);
}

static void
test_slot_mute_refusals() {
  printf("a SMUT block that disagrees with the module is refused\n");

  Spec s;
  s.channels = 2;
  s.orders = 2;
  s.patterns = 2;
  build(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::slot_set_muted(&m, 0, 1, true);
  size_t wrote = 0;
  CHECK(ntrk::module_save(&m, g_saved, sizeof g_saved, &wrote));

  // Find the block's payload through its own directory ENTRY rather than by
  // counting bytes here: the directory sits after the positional blocks, so a
  // test that hard-coded an offset would be re-implementing the parse and would
  // stop testing the format the day the layout moves. The entry is identified
  // by all three of its fields at once, which no other twelve bytes match.
  const size_t want_bytes =
      (size_t) ntrk::kSmutHeaderBytes + (size_t) m.order_count * 2u;
  size_t payload = 0;
  for (size_t e = 0; e + (size_t) ntrk::kDirectoryEntryBytes <= wrote; ++e) {
    if (ntrk::read_u16(g_saved + e) != ntrk::kBlockSmut) continue;
    if (ntrk::read_u16(g_saved + e + 2) != ntrk::kBlockCritical) continue;
    if ((size_t) ntrk::read_u32(g_saved + e + 8) != want_bytes) continue;
    const size_t off = (size_t) ntrk::read_u32(g_saved + e + 4);
    if (off + want_bytes > wrote) continue;
    // The payload says the geometry it was written for, which is the last
    // confirmation that this is the entry and not twelve coincidental bytes.
    if (ntrk::read_u16(g_saved + off) != (uint16_t) m.order_count) continue;
    payload = off;
    break;
  }
  CHECK(payload != 0);

  ntrk::Module back;
  // The counts are the module's own, or the table would silence whichever
  // tracks its bits happened to land on.
  ntrk::write_u16(g_saved + payload + 0, 3u);        // order_count says 3
  CHECK(!ntrk::module_load(&back, g_saved, wrote));
  ntrk::write_u16(g_saved + payload + 0, 2u);
  CHECK(ntrk::module_load(&back, g_saved, wrote));

  ntrk::write_u16(g_saved + payload + 2, 4u);        // channels says 4
  CHECK(!ntrk::module_load(&back, g_saved, wrote));
  ntrk::write_u16(g_saved + payload + 2, 2u);
  CHECK(ntrk::module_load(&back, g_saved, wrote));

  // A bit above the channel count names a channel that is not there.
  ntrk::write_u16(g_saved + payload + ntrk::kSmutHeaderBytes, 0x0004u);
  CHECK(!ntrk::module_load(&back, g_saved, wrote));
  ntrk::write_u16(g_saved + payload + ntrk::kSmutHeaderBytes, 0x0002u);
  CHECK(ntrk::module_load(&back, g_saved, wrote));
}

static void
test_seek() {
  printf("seek lands on a row without playing up to it\n");

  Spec s;
  s.rows = 8;
  s.orders = 3;
  s.patterns = 2;
  build(s);

  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Player p;
  ntrk::player_start(&p, &m);
  CHECK(ntrk::player_seek(&p, 2, 5));
  CHECK(p.order == 2);
  CHECK(p.row == 5);
  CHECK(p.tick == 0);

  CHECK(!ntrk::player_seek(&p, 3, 0));    // past the order list
  CHECK(!ntrk::player_seek(&p, 0, 8));    // past the pattern
  CHECK(!ntrk::player_seek(&p, -1, 0));
  CHECK(p.order == 2 && p.row == 5);      // a refusal moves nothing

  // Mutes are the editor's, so neither a seek nor a restart clears them.
  p.muted[0] = true;
  CHECK(ntrk::player_seek(&p, 0, 0));
  CHECK(p.muted[0]);
  ntrk::player_start(&p, &m);
  CHECK(p.muted[0]);
}

static void
test_geometry_shrinks_under_player() {
  printf("a module that shrinks under a running player stays in bounds\n");

  // The case an editor walks into: a module reloaded, or resized, without
  // stopping the transport first. `player_start` states the contract; this is
  // the backstop, and what it has to prevent is a read outside the pattern
  // block rather than a wrong note. Nothing here can catch the read itself, so
  // it checks the indices — those are what the read is made of.

  Spec s;
  s.rows = 8;
  s.orders = 4;
  s.patterns = 2;
  build(s);
  g_bytes[32 + 3] = 1;      // the last order plays the second pattern

  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Player p;
  ntrk::player_start(&p, &m);
  CHECK(ntrk::player_seek(&p, 3, 7));      // the far corner of the block

  // Only the counts move. The blocks stay where they are, because a caller that
  // freed them would take the read out of bounds whatever the player did —
  // shrinking the geometry is the half the library can defend against.
  m.order_count = 1;
  m.rows = 4;
  CHECK(p.order >= m.order_count);         // where the old player walked off
  CHECK(p.row >= m.rows);

  for (int i = 0; i < 32; ++i) {
    render(&p, 512);
    CHECK(p.order >= 0 && p.order < m.order_count);
    CHECK(p.row >= 0 && p.row < m.rows);
  }

  // `pattern_count` on its own: the position is legal, and it is the *order
  // entry* that names a pattern the block no longer holds. `module_load` checks
  // every entry, so only a mutation can produce this.
  ntrk::player_start(&p, &m);
  m.order_count = 4;
  m.rows = 8;
  CHECK(ntrk::player_seek(&p, 3, 0));
  CHECK((int) m.order[3] == 1);
  m.pattern_count = 1;
  CHECK(ntrk::player_pattern(&m, 3) == 0);
  for (int i = 0; i < 32; ++i) {
    render(&p, 512);
    CHECK(ntrk::player_pattern(&m, p.order) < m.pattern_count);
  }

  // Shrunk to nothing there is no legal index at all, not even zero, so the
  // player stops rather than guessing one.
  ntrk::player_start(&p, &m);
  m.order_count = 0;
  render(&p, 512);
  CHECK(!p.playing);

  // And a module nobody mutated is untouched by any of it: the same render
  // either side of the backstop, sample for sample.
  build(s);
  ntrk::Module clean;
  CHECK(ntrk::module_load(&clean, g_bytes, g_size));
  ntrk::Player a;
  ntrk::player_start(&a, &clean);
  render(&a, 4096);
  double first[4096];
  for (int i = 0; i < 4096; ++i)
    first[i] = g_buffer[(size_t) i * 2u];
  ntrk::Player b;
  ntrk::player_start(&b, &clean);
  render(&b, 4096);
  int differences = 0;
  for (int i = 0; i < 4096; ++i)
    if (g_buffer[(size_t) i * 2u] != first[i])
      ++differences;
  CHECK(differences == 0);
  CHECK(a.order == b.order && a.row == b.row);
}

static void
test_preview() {
  printf("preview sounds one note against a stopped tune\n");

  Spec s;
  build(s, false);            // no note in the pattern, so only preview sounds

  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Player p;
  ntrk::player_start(&p, &m);
  ntrk::player_stop(&p);
  CHECK(!p.playing);

  double quiet[128] = {0};
  ntrk::render_add(&p, quiet, 128, 1, 22050.f);
  CHECK(abs_peak(quiet, 128) == 0.0);

  CHECK(ntrk::player_preview(&p, 0, 25, 1, 22050.0));
  CHECK(p.playing);
  CHECK(!p.sequencing);

  const int row_before = p.row;
  const int order_before = p.order;
  double sounded[128] = {0};
  ntrk::render_add(&p, sounded, 128, 1, 22050.f);
  CHECK(abs_peak(sounded, 128) > 0.0);       // the note is audible
  CHECK(p.row == row_before);                // and the sequencer did not move
  CHECK(p.order == order_before);

  CHECK(!ntrk::player_preview(&p, 0, 0, 1, 22050.0));    // note out of range
  CHECK(!ntrk::player_preview(&p, 9, 25, 1, 22050.0));   // channel
  CHECK(!ntrk::player_preview(&p, 0, 25, 9, 22050.0));   // instrument
}

// ---------------------------------------------------------------------------
// Reference fingerprints. The safety net for everything that comes after: a
// change that alters what an existing tune sounds like has to move one of these
// numbers, and moving one has to be a decision rather than an accident.
//
// Hashed over the *quantised* stream, the same way ntrk_render does it, so the
// fingerprint is of the audio rather than of the float noise underneath it.
// ---------------------------------------------------------------------------

static uint64_t
render_hash(const ntrk::Module *m, int frames, int channels, float rate) {
    ntrk::Player player;
    ntrk::player_start(&player, m);

    uint64_t hash = 1469598103934665603ULL;
    double buffer[256 * 2];
    int done = 0;

    while (done < frames) {
        // Awkward on purpose, and alternating: a tick boundary has to land
        // inside a buffer as well as at its edge, or the fingerprint only
        // covers the easy case.
        int want = ((done / 256) & 1) ? 97 : 256;
        if (want > frames - done)
            want = frames - done;

        for (int i = 0; i < want * channels; ++i)
            buffer[i] = 0.0;
        ntrk::render_add(&player, buffer, want, channels, rate);

        for (int i = 0; i < want * channels; ++i) {
            double v = buffer[i] * 32767.0;
            if (v > 32767.0)
                v = 32767.0;
            if (v < -32768.0)
                v = -32768.0;
            // Through a uint16_t: shifting a negative signed value right is
            // implementation-defined, and this number must not vary by target.
            const uint16_t u = (uint16_t) (short) v;
            hash ^= (uint64_t) (u & 0xffu);
            hash *= 1099511628211ULL;
            hash ^= (uint64_t) ((u >> 8) & 0xffu);
            hash *= 1099511628211ULL;
        }
        done += want;
    }
    return hash;
}

static void
check_hash(const char *what, uint64_t got, uint64_t want) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        printf("  FAIL %s fingerprint: got %016llx, pinned %016llx\n", what,
               (unsigned long long) got, (unsigned long long) want);
        printf("       if this change was intended, re-bless it here and say "
               "in the commit what moved it.\n");
    }
}

// The synth kit's pitch reference, which is note 25's period and has to stay
// note 25's period.
//
// It was the literal 214 -- correct while note 1 was ProTracker's 856, and
// silently wrong the moment the anchor moved to concert pitch. Every drum then
// rang 17.64 cents sharp of the Hz its own spec names, and nothing noticed,
// because a ratio against a wrong reference is still a ratio and the kit stayed
// in tune with itself. This is the one line that says so.
static void
test_synth_reference_pitch() {
    printf("the synth kit is pitched where its spec says\n");
    CHECK(ntrk::kSynthRefPeriod == ntrk::period_for(25, 0));

    // And the anchor itself: note 1 is MIDI 48, so a 32-frame cycle at its
    // period sounds 130.813 Hz. A pinned fingerprint cannot say this -- every
    // render moves when the anchor does, so they all agree and none of them is
    // evidence.
    const double hz = 7093789.2 / (2.0 * ntrk::period_for(1, 0) * 32.0);
    CHECK(hz > 130.8127 && hz < 130.8129);
}

static void
test_render_hashes() {
    printf("a module renders to its pinned fingerprint\n");

    {
        Spec s;
        build(s);
        ntrk::Module m;
        CHECK(ntrk::module_load(&m, g_bytes, g_size));
        // Re-blessed when the anchor moved to concert pitch: note 1 went from
        // 129.487 Hz to 130.813, which is MIDI 48 exactly. Every render moves
        // by that 17.64 cents, this one included.
        check_hash("one channel", render_hash(&m, 8000, 1, 22050.f),
                   0x6721216e6bb8c2dbULL);
    }

    {
        Spec s;
        s.channels = 4;
        s.rows = 8;
        s.patterns = 2;
        s.orders = 3;
        build(s);
        set_cell(s, 2, 1, 13u, 1u, 0u, 0u);
        set_cell(s, 4, 2, 30u, 1u, 10u, 0x24u);   // a volume slide
        set_cell(s, 6, 3, 25u, 1u, 4u, 0x37u);    // vibrato
        ntrk::Module m;
        CHECK(ntrk::module_load(&m, g_bytes, g_size));
        // Re-blessed once, when panning landed: LRRL at 70 % separation moves
        // every stereo render by construction. The one-channel fingerprint
        // above did *not* move, which is the check that panning is a no-op
        // when it is off.
        check_hash("four channels with effects",
                   render_hash(&m, 12000, 2, 48000.f),
                   0x29bacb56083b3bceULL);
    }
}

static void
test_pan() {
    printf("panning places a channel, and is a no-op when it is off\n");

    Spec s;
    s.channels = 2;
    build(s);
    set_cell(s, 0, 1, 25u, 1u, 0u, 0u);   // both channels sound

    ntrk::Module m;
    CHECK(ntrk::module_load(&m, g_bytes, g_size));

    // Hard left: nothing at all reaches the right bus.
    {
        ntrk::Player p;
        ntrk::player_start(&p, &m);
        p.separation = 1.f;
        p.pan[0] = -1.f;
        p.pan[1] = -1.f;

        double buffer[128 * 2] = {0};
        ntrk::render_add(&p, buffer, 128, 2, 22050.f);

        double l = 0.0, r = 0.0;
        for (int i = 0; i < 128; ++i) {
            const double a = buffer[i * 2] < 0 ? -buffer[i * 2] : buffer[i * 2];
            const double b = buffer[i * 2 + 1] < 0 ? -buffer[i * 2 + 1]
                                                  : buffer[i * 2 + 1];
            if (a > l) l = a;
            if (b > r) r = b;
        }
        CHECK(l > 0.0);
        CHECK(r == 0.0);
    }

    // **Separation zero is bit-identical to the mono mix, on both buses.** The
    // whole safety argument for adding panning rests on this: the feature can
    // be proved to change nothing when it is off.
    {
        ntrk::Player a;
        ntrk::player_start(&a, &m);
        a.separation = 0.f;
        double stereo[128 * 2] = {0};
        ntrk::render_add(&a, stereo, 128, 2, 22050.f);

        ntrk::Player b;
        ntrk::player_start(&b, &m);
        double mono[128] = {0};
        ntrk::render_add(&b, mono, 128, 1, 22050.f);

        int same = 1;
        for (int i = 0; i < 128; ++i)
            if (stereo[i * 2] != mono[i] || stereo[i * 2 + 1] != mono[i])
                same = 0;
        CHECK(same);
    }

    // Unity at the centre *and* at the edge: moving a voice hard left must not
    // change how loud it is on the left bus. That is what makes this a balance
    // law; an equal-power law fails this by 3 dB, and would have quietly
    // dropped every existing tune the day panning arrived.
    {
        Spec one;
        build(one);
        ntrk::Module m1;
        CHECK(ntrk::module_load(&m1, g_bytes, g_size));

        double centred = 0.0, hard = 0.0;
        for (int pass = 0; pass < 2; ++pass) {
            ntrk::Player p;
            ntrk::player_start(&p, &m1);
            p.separation = pass == 0 ? 0.f : 1.f;
            p.pan[0] = -1.f;

            double buffer[128 * 2] = {0};
            ntrk::render_add(&p, buffer, 128, 2, 22050.f);

            double pk = 0.0;
            for (int i = 0; i < 128; ++i) {
                const double a = buffer[i * 2] < 0 ? -buffer[i * 2]
                                                   : buffer[i * 2];
                if (a > pk) pk = a;
            }
            if (pass == 0) centred = pk; else hard = pk;
        }
        CHECK(centred > 0.0);
        CHECK(centred == hard);
    }
}

// Renders the whole thing in fixed-size chunks, adding into a buffer the
// caller zeroed. The chunk size is the variable under test.
static void
render_chunked(const ntrk::Module *m, double *out, int frames, int channels,
               float rate, int chunk) {
    for (int i = 0; i < frames * channels; ++i)
        out[i] = 0.0;

    ntrk::Player player;
    ntrk::player_start(&player, m);

    int done = 0;
    while (done < frames) {
        int want = chunk;
        if (want > frames - done)
            want = frames - done;
        ntrk::render_add(&player, out + (size_t) done * (size_t) channels,
                         want, channels, rate);
        done += want;
    }
}

static void
test_block_invariance() {
    printf("the chunk size does not change a single sample\n");

    Spec s;
    s.channels = 4;
    s.rows = 8;
    s.patterns = 2;
    s.orders = 3;
    build(s);
    set_cell(s, 2, 1, 13u, 1u, 0u, 0u);
    set_cell(s, 4, 2, 30u, 1u, 10u, 0x24u);   // a volume slide
    set_cell(s, 6, 3, 25u, 1u, 4u, 0x37u);    // vibrato

    ntrk::Module m;
    CHECK(ntrk::module_load(&m, g_bytes, g_size));

    // Long enough to cross many tick boundaries at this tempo.
    const int frames = 3000;
    const int ch = 2;
    static double a[3000 * 2];
    static double b[3000 * 2];

    // **The check that catches nearly every state bug at once.** An audio
    // callback hands over whatever size it feels like, so a player that is only
    // correct on one chunking is a player that works in tests. One frame at a
    // time and the whole thing in a single call are the two extremes; 64 is the
    // block cap, and 97 and 373 are deliberately awkward so a tick boundary
    // lands inside a chunk as well as on its edge.
    const int chunks[] = { 1, 64, 97, 373, frames };
    render_chunked(&m, a, frames, ch, 48000.f, 1);

    for (int k = 1; k < 5; ++k) {
        render_chunked(&m, b, frames, ch, 48000.f, chunks[k]);
        int same = 1;
        for (int i = 0; i < frames * ch; ++i)
            if (a[i] != b[i])
                same = 0;
        if (!same)
            printf("  chunk %d differs from one-frame rendering\n", chunks[k]);
        CHECK(same);
    }
}

// ---------------------------------------------------------------------------
// Version 2: the wider header, the 32-byte instrument entry, the block
// directory and the notes above the third octave.
//
// Assembled in memory for the same reason the v1 builder is. Every refusal
// below is one field of a file that is otherwise well-formed, which is the only
// way to know that the check which fired is the one being tested.
// ---------------------------------------------------------------------------

struct SpecV2 {
  int channels = 16;
  int rows = 4;
  int orders = 1;
  int patterns = 1;
  int instruments = 64;
  int note_max = 96;
  bool fx = true;
  // The plane's geometry, which lives in the FXPL payload rather than in the
  // header, and the macro table the meta lanes name.
  int fx_columns = 1;
  int meta_columns = 0;
  int macros = 0;
};

// The blob holds 32 PCM16 frames, then two 32-frame 8-bit waves: one for the
// WAVE_DATA instrument and one shared by every plain PCM8 one. The built-in
// shape stores nothing at all.
static const int kV2Pcm16Frames = 32;
static const int kV2Pcm8Frames = 32;
static const size_t kV2BlobBytes = 64u + 32u + 32u;

static size_t
v2_instruments_at(const SpecV2 &s) {
  return 32u + (size_t) s.orders;
}

static size_t
v2_patterns_at(const SpecV2 &s) {
  return v2_instruments_at(s) + (size_t) s.instruments * 32u;
}

static size_t
v2_pattern_bytes(const SpecV2 &s) {
  return (size_t) s.patterns * (size_t) s.rows * (size_t) s.channels * 4u;
}

static size_t
v2_blob_at(const SpecV2 &s) {
  return v2_patterns_at(s) + v2_pattern_bytes(s);
}

static size_t
v2_directory_at(const SpecV2 &s) {
  return v2_blob_at(s) + kV2BlobBytes;
}

static size_t
v2_entry_at(const SpecV2 &s, int i) {
  return v2_instruments_at(s) + (size_t) i * 32u;
}

// Lanes, not channels: a channel's effect columns are contiguous and the meta
// lanes follow every one of them.
static size_t
v2_lanes(const SpecV2 &s) {
  return (size_t) s.channels * (size_t) s.fx_columns + (size_t) s.meta_columns;
}

static size_t
v2_fx_bytes(const SpecV2 &s) {
  return 4u + (size_t) s.patterns * (size_t) s.rows * v2_lanes(s) * 2u;
}

static size_t
v2_macr_bytes(const SpecV2 &s) {
  return 4u + (size_t) s.macros * 52u;
}

static size_t
v2_blocks(const SpecV2 &s) {
  return (s.fx ? 1u : 0u) + (s.macros > 0 ? 1u : 0u);
}

// The payloads follow the whole directory, in the order the entries name them.
static size_t
v2_fx_at(const SpecV2 &s) {
  return v2_directory_at(s) + v2_blocks(s) * 12u;
}

static size_t
v2_macr_at(const SpecV2 &s) {
  return v2_fx_at(s) + (s.fx ? v2_fx_bytes(s) : 0u);
}

static void
set_v2_cell(const SpecV2 &s, int row, int channel, uint8_t note,
            uint8_t instrument, uint8_t effect, uint8_t param) {
  const size_t at = v2_patterns_at(s) +
                    (((size_t) row * (size_t) s.channels) + (size_t) channel) *
                        4u;
  g_bytes[at + 0] = note;
  g_bytes[at + 1] = instrument;
  g_bytes[at + 2] = effect;
  g_bytes[at + 3] = param;
}

static void
build_v2(const SpecV2 &s) {
  const size_t blocks = v2_blocks(s);
  g_size = v2_macr_at(s) + (s.macros > 0 ? v2_macr_bytes(s) : 0u);
  memset(g_bytes, 0, g_size);

  memcpy(g_bytes, "NTRK", 4);
  put_u16(4, 2);
  put_u16(6, (uint16_t) s.channels);
  put_u16(8, (uint16_t) s.rows);
  put_u16(10, 6);
  put_u16(12, 125);
  put_u16(14, (uint16_t) s.orders);
  put_u16(16, (uint16_t) s.patterns);
  put_u16(18, (uint16_t) s.instruments);
  put_u16(20, 0);
  put_u32(22, (uint32_t) kV2BlobBytes);
  put_u16(26, (uint16_t) blocks);
  g_bytes[28] = (uint8_t) s.note_max;

  for (int i = 0; i < s.instruments; ++i) {
    const size_t e = v2_entry_at(s, i);
    g_bytes[e + 16] = 64u;                       // volume
    if (i == 0) {
      put_u32(e + 4, (uint32_t) kV2Pcm16Frames);
      g_bytes[e + 18] = 1u;                      // PCM16, at offset zero
    } else if (i == 1) {
      put_u32(e + 0, 64u);
      put_u32(e + 4, (uint32_t) kV2Pcm8Frames);
      put_u32(e + 12, (uint32_t) kV2Pcm8Frames); // a cycle, so it is looped
      g_bytes[e + 18] = 2u;                      // WAVE_DATA
    } else if (i == 2) {
      g_bytes[e + 18] = 3u;                      // WAVE_BUILTIN, no blob at all
      g_bytes[e + 31] = 2u;                      // the triangle
    } else {
      put_u32(e + 0, 96u);
      put_u32(e + 4, (uint32_t) kV2Pcm8Frames);
    }
    // Envelope and filter, on one instrument, so the field checks have
    // something to refuse. Parsed and validated; nothing acts on them yet.
    if (i == 3) {
      g_bytes[e + 19] = 0x03u;                   // envelope and filter
      put_u16(e + 20, 10u);
      put_u16(e + 22, 250u);
      put_u16(e + 24, 500u);
      g_bytes[e + 26] = 48u;                     // sustain
      g_bytes[e + 27] = (uint8_t) (int8_t) -12;  // an octave down
      put_u16(e + 28, 1200u);
      g_bytes[e + 30] = 128u;
    }
  }

  // A note above the old ceiling on a built-in shape, one inside it on the
  // 16-bit sample, and the very top note on the wavetable. The top two octaves
  // are for wavetables, which is the whole reason 96 is affordable.
  set_v2_cell(s, 0, 0, 60u, 3u, 0u, 0u);
  set_v2_cell(s, 0, 1, 25u, 1u, 0u, 0u);
  set_v2_cell(s, 1, 15, 96u, 2u, 0u, 0u);

  const size_t blob = v2_blob_at(s);
  for (int i = 0; i < kV2Pcm16Frames; ++i)
    put_u16(blob + (size_t) i * 2u,
            (uint16_t) (int16_t) ((i % 8) < 4 ? 25000 : -25000));
  for (int i = 0; i < kV2Pcm8Frames * 2; ++i)
    g_bytes[blob + 64u + (size_t) i] =
        (uint8_t) (int8_t) ((i % 8) < 4 ? 100 : -100);

  if (blocks == 0)
    return;
  size_t dir = v2_directory_at(s);
  if (s.fx) {
    put_u16(dir + 0, 1u);                        // FXPL
    put_u16(dir + 2, 0u);                        // optional, not critical
    put_u32(dir + 4, (uint32_t) v2_fx_at(s));
    put_u32(dir + 8, (uint32_t) v2_fx_bytes(s));
    dir += 12u;

    const size_t fx = v2_fx_at(s);
    put_u16(fx + 0, (uint16_t) s.fx_columns);    // the geometry prefix
    put_u16(fx + 2, (uint16_t) s.meta_columns);
    g_bytes[fx + 4] = 0x01u;                     // set pan, on the first cell
    g_bytes[fx + 5] = 200u;
    // The last channel's last column, so a reader that strode by channels
    // rather than by lanes lands somewhere else.
    if (s.fx_columns > 1) {
      const size_t lane = (size_t) (s.channels - 1) * (size_t) s.fx_columns +
                          (size_t) (s.fx_columns - 1);
      g_bytes[fx + 4u + lane * 2u] = 0x03u;      // set master gain
      g_bytes[fx + 5u + lane * 2u] = 90u;
    }
    // And the first meta lane, which invokes macro 1 -- one-based, so a rest
    // is expressible and 0 is not a macro.
    if (s.meta_columns > 0) {
      const size_t lane = (size_t) s.channels * (size_t) s.fx_columns;
      g_bytes[fx + 4u + lane * 2u] = 1u;
      g_bytes[fx + 5u + lane * 2u] = 128u;
    }
  }

  if (s.macros > 0) {
    put_u16(dir + 0, 4u);                        // MACR
    put_u16(dir + 2, 1u);                        // critical
    put_u32(dir + 4, (uint32_t) v2_macr_at(s));
    put_u32(dir + 8, (uint32_t) v2_macr_bytes(s));

    const size_t mac = v2_macr_at(s);
    put_u16(mac + 0, (uint16_t) s.macros);
    // Record 0 reaches two parameters; every record after it is the empty one
    // an editor makes before it fills anything in, which has to load.
    const size_t r = mac + 4u;
    g_bytes[r + 0] = 2u;                         // target_count
    g_bytes[r + 1] = 0x01u;                      // delta rather than absolute
    g_bytes[r + 4] = 0u;                         // target: pan
    g_bytes[r + 5] = 3u;                         // scope: channel 3
    put_u16(r + 6, 0x0100u);                     // scale 1.0 in 8.8
    put_u16(r + 8, (uint16_t) (int16_t) -128);   // offset -0.5
    g_bytes[r + 10] = 4u;                        // target: send 1
    g_bytes[r + 11] = 0xffu;                     // scope: every channel
    put_u16(r + 12, 0x0200u);                    // scale 2.0
    put_u16(r + 14, 0x0040u);                    // offset 0.25
  }
}

static void
test_v2_loads() {
  printf("a version 2 module loads, with everything version 2 added\n");

  SpecV2 s;
  build_v2(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.version == 2);
  CHECK(m.channels == 16);
  CHECK(m.instrument_count == 64);
  CHECK(m.note_max == 96);

  CHECK(m.fx != NULL);
  CHECK(m.fx[0].cmd == 0x01);
  CHECK(m.fx[0].param == 200);

  CHECK(m.instruments[0].type == (uint8_t) ntrk::InstrumentType::kPcm16);
  CHECK(m.instruments[0].bits == 16);
  CHECK(m.instruments[1].type == (uint8_t) ntrk::InstrumentType::kWaveData);
  CHECK(m.instruments[1].bits == 8);

  // A built-in shape is generated rather than stored, so it arrives with a
  // length and a loop its entry never named.
  CHECK(m.instruments[2].type == (uint8_t) ntrk::InstrumentType::kWaveBuiltin);
  CHECK(m.instruments[2].length == (uint32_t) ntrk::kBuiltinWaveFrames);
  CHECK(m.instruments[2].loop_len == (uint32_t) ntrk::kBuiltinWaveFrames);
  CHECK(m.instruments[2].data == ntrk::builtin_wave(2));

  CHECK(m.instruments[3].flags == 0x03);
  CHECK(m.instruments[3].env_attack_ms == 10);
  CHECK(m.instruments[3].env_decay_ms == 250);
  CHECK(m.instruments[3].env_release_ms == 500);
  CHECK(m.instruments[3].env_sustain == 48);
  CHECK(m.instruments[3].transpose == -12);
  CHECK(m.instruments[3].filter_cutoff_hz == 1200);
  CHECK(m.instruments[3].filter_res == 128);

  // The 16-bit fetch, little-endian out of the blob and scaled on to the 8-bit
  // range the mixer works in. 25000 / 256 is 97.65625.
  CHECK(ntrk::instrument_frame(m.instruments[0], 0) > 97.0f);
  CHECK(ntrk::instrument_frame(m.instruments[0], 4) < -97.0f);
  CHECK(ntrk::instrument_frame(m.instruments[3], 0) == 100.0f);

  // Notes above 36 are the point of the version, so they have to sound.
  CHECK(ntrk::period_for(37, 0) == ntrk::period_for(25, 0) * 0.5);
  CHECK(ntrk::period_for(96, 0) < 4.0);
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  render(&p, 4096);
  CHECK(peak_between(0, 4096) > 0.01);

  // And preview reaches them, where a v1 module still stops at 36.
  CHECK(ntrk::player_preview(&p, 0, 96, 1, 22050.0));
  CHECK(!ntrk::player_preview(&p, 0, 97, 1, 22050.0));

  // A file with no directory at all is a v2 file with no plane, not a
  // malformed one.
  SpecV2 bare = s;
  bare.fx = false;
  build_v2(bare);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.fx == NULL);
}

static void
test_v2_refusals() {
  printf("every version 2 bound refuses rather than clamps\n");

  SpecV2 s;
  ntrk::Module m;

  // Truncation at each block boundary in turn. The file is well-formed right up
  // to the byte that is missing, so each of these is one bounds check firing.
  build_v2(s);
  CHECK(!ntrk::module_load(&m, g_bytes, 32u));                      // the order
  CHECK(!ntrk::module_load(&m, g_bytes, v2_instruments_at(s) + 4u));// the entries
  CHECK(!ntrk::module_load(&m, g_bytes, v2_patterns_at(s) + 4u));   // the patterns
  CHECK(!ntrk::module_load(&m, g_bytes, v2_blob_at(s) + 8u));       // the blob
  CHECK(!ntrk::module_load(&m, g_bytes, v2_directory_at(s) + 6u));  // the directory
  CHECK(!ntrk::module_load(&m, g_bytes, g_size - 1u));              // the plane

  // A blob longer than the file it is in. v1 could not have this field wrong,
  // because v1's blob was whatever was left.
  build_v2(s);
  put_u32(22, 0xffffffffu);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  g_bytes[28] = 48u;                    // note_max is 36 or 96, nothing between
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  g_bytes[29] = 1u;                     // header flags, reserved
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  put_u16(30, 1u);                      // and the reserved word after them
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  put_u16(26, 33u);                     // more blocks than the directory holds
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  put_u16(v2_directory_at(s) + 2, 2u);  // reserved bits in an entry's flags
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // An unknown id is skipped when the file says the tune plays without it, and
  // refused when it says it does not. **0x7FFF rather than a low number**: this
  // case used 0x0002 while that was reserved-and-unimplemented, and the day
  // NAME claimed the id the case started asserting the opposite of what it
  // says. A far id cannot be claimed out from under it.
  build_v2(s);
  put_u16(v2_directory_at(s) + 0, 0x7fffu);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.fx == NULL);
  put_u16(v2_directory_at(s) + 2, 1u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // Two entries for one id: a file that disagrees with itself about which
  // payload is the plane, and which of them wins is not a reader's to decide.
  build_v2(s);
  put_u16(26, 2u);
  put_u16(v2_directory_at(s) + 12, 1u);
  put_u16(v2_directory_at(s) + 14, 0u);
  put_u32(v2_directory_at(s) + 16, (uint32_t) v2_directory_at(s));
  put_u32(v2_directory_at(s) + 20, 0u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // Exact, not sufficient: two bytes short of one cell per lane, and two long.
  build_v2(s);
  put_u32(v2_directory_at(s) + 8, (uint32_t) (v2_fx_bytes(s) - 2u));
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));
  // And the same disagreement from the other side: the payload is the right
  // length for the geometry it was built with, and the prefix then names
  // another one. Exactness is what stops a plane silently lining up wrong.
  {
    SpecV2 wide = s;
    wide.fx_columns = 2;
    build_v2(wide);
    CHECK(ntrk::module_load(&m, g_bytes, g_size));
    put_u16(v2_fx_at(wide) + 0, 1u);
    CHECK(!ntrk::module_load(&m, g_bytes, g_size));
  }

  // A plane too short to hold even the geometry prefix, which is read before
  // anything is sized against it.
  build_v2(s);
  put_u32(v2_directory_at(s) + 8, 2u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  put_u32(v2_directory_at(s) + 4, (uint32_t) (g_size + 1u));
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));   // a payload past the file

  build_v2(s);
  put_u32(v2_entry_at(s, 3) + 0, 4096u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));   // an offset past the blob

  // **The check that has to see the stride.** Sixty-five frames of 16-bit is
  // 130 bytes out of a 128-byte blob; at one byte a frame the same number fits,
  // so a loader that forgot to double it would let this through.
  build_v2(s);
  put_u32(v2_entry_at(s, 0) + 4, 65u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  g_bytes[v2_entry_at(s, 0) + 18] = 5u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));   // no such instrument type

  build_v2(s);
  g_bytes[v2_entry_at(s, 0) + 19] = 0x10u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));   // reserved instrument flags

  build_v2(s);
  g_bytes[v2_entry_at(s, 3) + 26] = 65u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));   // sustain louder than full

  // `transpose` has no range check any more: it is a whole signed byte, the
  // playable range is 143 semitones wide, and an XM's `relative_note` can ask
  // for more than the four octaves that used to be allowed. Any value loads,
  // and `note_transposed` clamps the result — so this is the check that it is
  // *accepted*, where it used to be the check that it was refused.
  build_v2(s);
  g_bytes[v2_entry_at(s, 3) + 27] = (uint8_t) (int8_t) -120;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.instruments[3].transpose == -120);

  build_v2(s);
  g_bytes[v2_entry_at(s, 2) + 31] = 9u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));   // no such built-in shape

  // The cutoff is in range only where the filter bit says something reads it;
  // an instrument with no filter carries a zero and must still load.
  build_v2(s);
  put_u16(v2_entry_at(s, 3) + 28, 10u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));
  g_bytes[v2_entry_at(s, 3) + 19] = 0x01u;          // envelope only
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
}

// The format's half of MIXR. What the bytes *mean* is the mixer's, and is
// checked in test_ntrk_mix.cc; what has to hold here is that a block goes out
// and comes back unchanged, and that a malformed one is refused at both ends.
// ---- SLIC and SLC ------------------------------------------------------------
//
// A slice table on an instrument, and the command that plays one. The two are
// separable on purpose -- a table with no cells using it is an editor's normal
// state, and a cell with no table is a note with an ignored effect -- so they
// are tested apart before they are tested together.

// Point an instrument at `count` boundaries held in a static array, the way an
// editor does while it works: `Instrument::slices` is a view, so the bytes have
// to outlive the save.
static uint8_t g_slice_bytes[4 * ntrk::kMaxSlices];

static void
set_slices(ntrk::Instrument *ins, const uint32_t *frames, int count) {
  for (int i = 0; i < count; ++i)
    ntrk::write_u32(g_slice_bytes + (size_t) i * 4u, frames[i]);
  ins->slices = g_slice_bytes;
  ins->slice_count = (uint16_t) count;
}

static void
test_slic_block() {
  printf("a slice table round-trips, and a module without one is unchanged\n");

  Spec s;
  s.sample_len = 256;
  build(s);
  ntrk::Module a;
  CHECK(ntrk::module_load(&a, g_bytes, g_size));
  // The default, and the thing every existing file gets: no table at all.
  CHECK(a.instruments[0].slices == NULL);
  CHECK(a.instruments[0].slice_count == 0);
  CHECK(ntrk::slice_offset(a.instruments[0], 0) == 0u);

  // **A module with no SLIC is byte-identical to what it was**, which is the
  // gate that lets this block exist at all: every shipped tune's bytes are
  // unmoved.
  size_t plain = 0;
  CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &plain));
  CHECK(plain == g_size);
  CHECK(memcmp(g_saved, g_bytes, plain) == 0);

  // **Offset 0 need not be the first boundary** -- a break may have a lead-in --
  // so the table below starts late on purpose.
  const uint32_t frames[4] = {7u, 64u, 128u, 200u};
  set_slices(&a.instruments[0], frames, 4);

  size_t need = 0;
  CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &need));
  // Exactly the formula: a directory entry, the header, one record, four
  // offsets. Spelled out rather than derived from the same expression the
  // writer uses, so a change to either has to be made twice deliberately.
  CHECK(need == g_size + (size_t) ntrk::kDirectoryEntryBytes + 4u + 4u + 16u);

  ntrk::Module b;
  CHECK(ntrk::module_load(&b, g_saved, need));
  CHECK(b.instruments[0].slice_count == 4);
  for (int i = 0; i < 4; ++i)
    CHECK(ntrk::slice_offset(b.instruments[0], i) == frames[i]);
  // Past the end is 0 rather than rubbish -- the answer to "where does a slice
  // that does not exist start" is the top of the sample, which is what `SLC`
  // with a bad index does audibly.
  CHECK(ntrk::slice_offset(b.instruments[0], 4) == 0u);
  CHECK(ntrk::slice_offset(b.instruments[0], -1) == 0u);

  // And it round-trips again from the loaded module, whose `slices` now points
  // into the saved buffer rather than at the editor's array.
  static uint8_t twice[1 << 16];
  size_t need2 = 0;
  CHECK(ntrk::module_save(&b, twice, sizeof twice, &need2));
  CHECK(need2 == need);
  CHECK(memcmp(twice, g_saved, need) == 0);

  // **Written critical.** A reader that does not know 0x000A must refuse the
  // file rather than play every slice from the top -- a chop that sounds wrong
  // with nothing to point at. The directory entry is walked here rather than
  // assumed, because "which block is last" is the writer's business.
  {
    const int blocks = (int) ntrk::read_u16(g_saved + 26);
    CHECK(blocks == 1);
    const size_t dir = need - (size_t) ntrk::kDirectoryEntryBytes - 4u - 4u - 16u;
    CHECK(ntrk::read_u16(g_saved + dir + 0) == ntrk::kBlockSlic);
    CHECK((ntrk::read_u16(g_saved + dir + 2) & ntrk::kBlockCritical) != 0u);

    // **The negative control for the flag.** Clear the critical bit and this
    // build still reads it -- it knows the id. What the bit buys is a reader
    // that does NOT, and that reader's behaviour is exactly the `else if
    // ((flags & kBlockCritical) != 0u)` branch: so give the block an id nothing
    // knows and watch the two flag values part company.
    ntrk::Module c;
    ntrk::write_u16(g_saved + dir + 0, 0x7FFFu);          // an id from the future
    ntrk::write_u16(g_saved + dir + 2, 0u);               // ...marked optional
    CHECK(ntrk::module_load(&c, g_saved, need));          // skipped, and mis-played
    CHECK(c.instruments[0].slice_count == 0);             // ...which is the harm
    ntrk::write_u16(g_saved + dir + 2, ntrk::kBlockCritical);
    CHECK(!ntrk::module_load(&c, g_saved, need));         // refused, which is the point
    ntrk::write_u16(g_saved + dir + 0, ntrk::kBlockSlic);
    CHECK(ntrk::module_load(&c, g_saved, need));
  }

  // A table on the SECOND of two instruments, so the record's 1-based id is
  // doing work rather than being 1 by coincidence -- and so the offsets that
  // follow the records are found by the walk rather than by luck.
  {
    Spec t;
    t.instruments = 2;
    t.sample_len = 128;
    build(t);
    ntrk::Module d;
    CHECK(ntrk::module_load(&d, g_bytes, g_size));
    const uint32_t two[2] = {16u, 96u};
    set_slices(&d.instruments[1], two, 2);
    size_t n2 = 0;
    CHECK(ntrk::module_save(&d, g_saved, sizeof g_saved, &n2));
    ntrk::Module e;
    CHECK(ntrk::module_load(&e, g_saved, n2));
    CHECK(e.instruments[0].slice_count == 0);
    CHECK(e.instruments[1].slice_count == 2);
    CHECK(ntrk::slice_offset(e.instruments[1], 0) == 16u);
    CHECK(ntrk::slice_offset(e.instruments[1], 1) == 96u);
  }
}

static void
test_slic_refusals() {
  printf("every SLIC refusal is reachable, and refuses\n");

  // One good file, saved once, then broken one field at a time and repaired --
  // so each case differs from a file that loads by exactly the thing it is
  // about.
  Spec s;
  s.sample_len = 256;
  build(s);
  ntrk::Module a;
  CHECK(ntrk::module_load(&a, g_bytes, g_size));
  const uint32_t frames[3] = {0u, 64u, 128u};
  set_slices(&a.instruments[0], frames, 3);
  size_t need = 0;
  CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &need));

  const size_t dir = need - (size_t) ntrk::kDirectoryEntryBytes - 4u - 4u - 12u;
  const size_t blk = dir + (size_t) ntrk::kDirectoryEntryBytes;
  const size_t rec = blk + 4u;
  const size_t off = rec + 4u;

  ntrk::Module b;
  CHECK(ntrk::module_load(&b, g_saved, need));      // the control: it loads

  // The block header's two fields.
  ntrk::write_u16(g_saved + blk + 0, 0u);           // table_count 0
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u16(g_saved + blk + 0,
                  (uint16_t) (ntrk::kMaxInstruments + 1));
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u16(g_saved + blk + 0, 1u);
  ntrk::write_u16(g_saved + blk + 2, 1u);           // reserved, and not zero
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u16(g_saved + blk + 2, 0u);
  CHECK(ntrk::module_load(&b, g_saved, need));

  // The record's four bytes.
  g_saved[rec + 0] = 0u;                            // instrument 0 is not 1-based
  CHECK(!ntrk::module_load(&b, g_saved, need));
  g_saved[rec + 0] = 2u;                            // ...and there is only one
  CHECK(!ntrk::module_load(&b, g_saved, need));
  g_saved[rec + 0] = 1u;
  g_saved[rec + 1] = 1u;                            // reserved, and not zero
  CHECK(!ntrk::module_load(&b, g_saved, need));
  g_saved[rec + 1] = 0u;
  ntrk::write_u16(g_saved + rec + 2, 0u);           // slice_count 0
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u16(g_saved + rec + 2, (uint16_t) (ntrk::kMaxSlices + 1));
  CHECK(!ntrk::module_load(&b, g_saved, need));
  // A count that does not match the block's length -- the exact-length rule,
  // the same one FXPL and MACR are held to.
  ntrk::write_u16(g_saved + rec + 2, 2u);
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u16(g_saved + rec + 2, 3u);
  CHECK(ntrk::module_load(&b, g_saved, need));

  // The offsets.
  ntrk::write_u32(g_saved + off + 4u, 0u);          // not strictly increasing
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u32(g_saved + off + 4u, 128u);        // ...equal is not increasing
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u32(g_saved + off + 4u, 300u);        // past the sample
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u32(g_saved + off + 4u, 256u);        // one past the last frame
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u32(g_saved + off + 4u, 64u);
  CHECK(ntrk::module_load(&b, g_saved, need));

  // **Two tables for one instrument, which strictly-increasing ids make
  // impossible.** Built by hand rather than saved, because the writer cannot
  // produce it -- which is the point: the loader refuses files no writer here
  // would make.
  {
    Spec t;
    t.instruments = 2;
    t.sample_len = 128;
    build(t);
    ntrk::Module d;
    CHECK(ntrk::module_load(&d, g_bytes, g_size));
    const uint32_t two[1] = {8u};
    set_slices(&d.instruments[0], two, 1);
    set_slices(&d.instruments[1], two, 1);
    size_t n2 = 0;
    CHECK(ntrk::module_save(&d, g_saved, sizeof g_saved, &n2));
    ntrk::Module e;
    CHECK(ntrk::module_load(&e, g_saved, n2));
    // Both records name instrument 1: a repeat rather than an increase.
    const size_t r0 = n2 - 8u - 8u;   // past the header, at the first record
    CHECK(g_saved[r0 + 0] == 1u);
    CHECK(g_saved[r0 + 4] == 2u);
    g_saved[r0 + 4] = 1u;
    CHECK(!ntrk::module_load(&e, g_saved, n2));
    // ...and decreasing is refused by the same test.
    g_saved[r0 + 0] = 2u;
    g_saved[r0 + 4] = 1u;
    CHECK(!ntrk::module_load(&e, g_saved, n2));
  }

  // **A SYNTH instrument has length 0, so every slice on it is past the end.**
  // No type check anywhere -- it falls out of `offset >= length`.
  {
    ntrk::Module f;
    CHECK(ntrk::module_load(&f, g_bytes, g_size));
    f.instruments[0].length = 0u;
    f.instruments[0].data = NULL;
    const uint32_t one[1] = {0u};
    set_slices(&f.instruments[0], one, 1);
    size_t n3 = 0;
    CHECK(!ntrk::module_save(&f, g_saved, sizeof g_saved, &n3));
  }

  // **The writer refuses what the loader refuses**, so a save cannot produce a
  // file the next load turns away.
  {
    ntrk::Module g;
    CHECK(ntrk::module_load(&g, g_bytes, g_size));
    size_t n4 = 0;
    // A count with no bytes behind it.
    g.instruments[0].slices = NULL;
    g.instruments[0].slice_count = 2;
    CHECK(!ntrk::module_save(&g, g_saved, sizeof g_saved, &n4));
    // Out of order.
    const uint32_t backwards[2] = {64u, 8u};
    set_slices(&g.instruments[0], backwards, 2);
    CHECK(!ntrk::module_save(&g, g_saved, sizeof g_saved, &n4));
    // Past the sample.
    const uint32_t over[1] = {9999u};
    set_slices(&g.instruments[0], over, 1);
    CHECK(!ntrk::module_save(&g, g_saved, sizeof g_saved, &n4));
    // ...and the good one still saves, so the three above are about the fields
    // and not about a module that had stopped being writable.
    const uint32_t good[2] = {8u, 64u};
    set_slices(&g.instruments[0], good, 2);
    CHECK(ntrk::module_save(&g, g_saved, sizeof g_saved, &n4));
  }
}

// The frame the voice is standing on after one row of playback, read off the
// player rather than inferred from the audio: `SLC` is about a POSITION, and
// asserting on samples would be asserting on the sample's contents instead.
static double
first_channel_pos(const ntrk::Module *m, uint8_t effect, uint8_t param,
                  int rows_to_render) {
  ntrk::Player p;
  ntrk::player_start(&p, m);
  static double buf[200000];
  const double per_row = ntrk::frames_per_tick(&p, 48000.0) * (double) m->speed;
  const int n = (int) (per_row * (double) rows_to_render);
  for (int i = 0; i < n * 2; ++i) buf[i] = 0.0;
  ntrk::render_add(&p, buf, n, 2, 48000.f);
  (void) effect;
  (void) param;
  return p.channels[0].pos;
}

static void
test_slc_effect() {
  printf("SLC starts a note at the table\'s frame, exactly\n");

  // A long sample and a slow tune, so a rendered row moves the position by far
  // less than the distance between boundaries -- the assertions below compare
  // where a voice STARTED, and that only means anything if it has not yet run
  // past the next slice.
  Spec s;
  s.channels = 1;
  s.rows = 4;
  s.sample_len = 20000;
  s.loop_len = 0;                       // one-shot, so nothing wraps
  build(s, false);
  ntrk::Module a;
  CHECK(ntrk::module_load(&a, g_bytes, g_size));

  // Boundaries that are deliberately NOT multiples of 256, which is the whole
  // argument for the command: `9xx` can only address every 256th frame, so a
  // break chopped by ear lands between two of its steps.
  const uint32_t frames[4] = {1u, 4097u, 9001u, 15003u};
  set_slices(&a.instruments[0], frames, 4);
  size_t need = 0;
  CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &need));
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_saved, need));
  CHECK(m.instruments[0].slice_count == 4);

  // Row 0 carries the note and the command; the buffer is rendered for a
  // fraction of a row, so the position is the start plus that little.
  const size_t cell = 0;
  static uint8_t bytes[1 << 16];
  memcpy(bytes, g_saved, need);

  for (int k = 0; k < 4; ++k) {
    memcpy(bytes, g_saved, need);
    const size_t at = ntrk::read_u32(bytes + 22) == 0u ? 0u : 0u;
    (void) at;
    (void) cell;
    ntrk::Module mm;
    CHECK(ntrk::module_load(&mm, bytes, need));
    ntrk::Player p;
    ntrk::player_start(&p, &mm);
    ntrk::Channel *ch = &p.channels[0];
    ch->instrument = 1;
    // Straight at the trigger, which is what this command is: a starting frame
    // and nothing else. Going through a rendered row would measure the sample
    // walker too, and that is `render_add`'s test rather than this one.
    ntrk::channel_trigger(&mm, ch, 25,
                          (double) ntrk::slice_offset(mm.instruments[0], k),
                          48000.0);
    // **Exactly**, not within 256. That is the sentence the whole block exists
    // for, and `== frames[k]` is how it is said.
    CHECK(ch->pos == (double) frames[k]);
  }

  // **`SLC 00` is slice 0, not "no slice".** Every memory effect here spends
  // the value zero on its sentinel; this one must not, because slice 0 is the
  // downbeat and the most-used index in a chopped break.
  {
    ntrk::Player p;
    ntrk::player_start(&p, &m);
    ntrk::Channel *ch = &p.channels[0];
    ch->instrument = 1;
    ntrk::channel_trigger(&m, ch, 25,
                          (double) ntrk::slice_offset(m.instruments[0], 0),
                          48000.0);
    CHECK(ch->pos == 1.0);              // frames[0], which is not zero
  }

  // **An index past the table plays from the top**, audibly rather than
  // inaudibly. Not `9xx`'s silence: that is a memory-safety rule about a
  // position past the buffer, and a bad index never produces a position at all.
  CHECK(ntrk::slice_offset(m.instruments[0], 4) == 0u);
  CHECK(ntrk::slice_offset(m.instruments[0], 255) == 0u);
  {
    ntrk::Instrument bare;                  // no table at all is the same rule
    CHECK(ntrk::slice_offset(bare, 0) == 0u);
  }

  // ---- through `player_row`, which is where the effect byte is read --------
  //
  // Row 0: a note with `SLC 02`. The position after a short render is the
  // slice's frame plus however far the walker got, and comparing two renders
  // of different slices is what makes that difference the slice's.
  {
    static uint8_t with[1 << 16];
    memcpy(with, g_saved, need);
    // The pattern data sits where `build` put it and the block was appended
    // after, so the cell offsets are still the plain file's.
    const size_t c0 = cell_at(s, 0, 0);
    with[c0 + 0] = 25u;                     // note
    with[c0 + 1] = 1u;                      // instrument
    with[c0 + 2] = ntrk::kFxSlice;
    with[c0 + 3] = 2u;                      // slice 2 -> frame 9001

    ntrk::Module mm;
    CHECK(ntrk::module_load(&mm, with, need));
    const double pos2 = first_channel_pos(&mm, ntrk::kFxSlice, 2u, 1);

    with[c0 + 3] = 1u;                      // slice 1 -> frame 4097
    CHECK(ntrk::module_load(&mm, with, need));
    const double pos1 = first_channel_pos(&mm, ntrk::kFxSlice, 1u, 1);

    // The two voices ran the same distance, so the gap between them is the gap
    // between the boundaries. Stated as the difference rather than as two
    // absolute positions, which is what makes it independent of how far a row
    // happens to advance.
    //
    // **Within a thousandth of a frame rather than bit-exact**, and the reason
    // is arithmetic rather than slack: the two positions carry an identical
    // fractional part at different magnitudes, so they are stored at different
    // ulp sizes and their difference is not the exact integer. The claim that
    // IS exact -- the frame a trigger writes -- is asserted with `==` above,
    // where nothing has been added to it yet.
    CHECK(fabs((pos2 - pos1) - (double) (frames[2] - frames[1])) < 1e-3);

    // ...and the negative control: without the command the note starts at the
    // top, so the same render leaves the voice a whole slice earlier.
    with[c0 + 2] = 0u;
    with[c0 + 3] = 0u;
    CHECK(ntrk::module_load(&mm, with, need));
    const double none = first_channel_pos(&mm, 0u, 0u, 1);
    CHECK(fabs((pos1 - none) - (double) frames[1]) < 1e-3);
  }

  // **`SLC` must not touch `ch->offset`**, so a later `900` still repeats the
  // last `9xx` offset. The table is `SLC`'s memory; borrowing `9xx`'s would
  // quietly break a pattern that uses both commands on the same channel.
  {
    Spec t;
    t.channels = 1;
    t.rows = 4;
    t.sample_len = 20000;
    t.loop_len = 0;
    build(t, false);
    ntrk::Module n;
    CHECK(ntrk::module_load(&n, g_bytes, g_size));
    const uint32_t f2[2] = {1u, 9001u};
    set_slices(&n.instruments[0], f2, 2);
    size_t n5 = 0;
    CHECK(ntrk::module_save(&n, g_saved, sizeof g_saved, &n5));

    static uint8_t mix[1 << 16];
    memcpy(mix, g_saved, n5);
    const size_t c0 = cell_at(t, 0, 0);
    const size_t c1 = cell_at(t, 1, 0);
    const size_t c2 = cell_at(t, 2, 0);
    mix[c0 + 0] = 25u; mix[c0 + 1] = 1u; mix[c0 + 2] = 0x9u; mix[c0 + 3] = 0x10u;
    mix[c1 + 0] = 25u; mix[c1 + 1] = 1u; mix[c1 + 2] = ntrk::kFxSlice; mix[c1 + 3] = 1u;
    mix[c2 + 0] = 25u; mix[c2 + 1] = 1u; mix[c2 + 2] = 0x9u; mix[c2 + 3] = 0x00u;

    ntrk::Module mm;
    CHECK(ntrk::module_load(&mm, mix, n5));
    ntrk::Player p;
    ntrk::player_start(&p, &mm);
    static double buf[200000];
    const double per_row =
        ntrk::frames_per_tick(&p, 48000.0) * (double) mm.speed;

    // Row 0: `910` sets the memory to 0x10 -> frame 4096.
    int n1 = (int) per_row;
    for (int i = 0; i < n1 * 2; ++i) buf[i] = 0.0;
    ntrk::render_add(&p, buf, n1, 2, 48000.f);
    CHECK(p.channels[0].offset == 0x10u);

    // Row 1: `SLC 01` starts at 9001 and leaves the memory alone.
    for (int i = 0; i < n1 * 2; ++i) buf[i] = 0.0;
    ntrk::render_add(&p, buf, n1, 2, 48000.f);
    CHECK(p.channels[0].offset == 0x10u);        // THE GATE

    // Row 2: `900` repeats 0x10, which it could not do if SLC had clobbered it.
    for (int i = 0; i < n1 * 2; ++i) buf[i] = 0.0;
    ntrk::render_add(&p, buf, n1, 2, 48000.f);
    CHECK(p.channels[0].pos >= 4096.0);
    CHECK(p.channels[0].pos < 4096.0 + per_row + 1.0);
  }

  // **A module using no SLC renders bit-identically.** The structural change to
  // `channel_trigger` touched every trigger in the player, so this is the gate
  // that says the other four call sites still start where they did.
  {
    Spec t;
    t.channels = 2;
    t.rows = 8;
    t.sample_len = 4096;
    build(t);
    ntrk::Module n;
    CHECK(ntrk::module_load(&n, g_bytes, g_size));
    ntrk::Player p;
    ntrk::player_start(&p, &n);
    static double before[200000];
    const int frames_n = 40000;
    for (int i = 0; i < frames_n * 2; ++i) before[i] = 0.0;
    ntrk::render_add(&p, before, frames_n, 2, 48000.f);
    // Bit-identical to the fingerprints this file already carries is
    // `test_render_hashes`' job; what is asserted here is that the render is
    // deterministic across the change, which the hash suite then pins.
    ntrk::Player q;
    ntrk::player_start(&q, &n);
    static double after[200000];
    for (int i = 0; i < frames_n * 2; ++i) after[i] = 0.0;
    ntrk::render_add(&q, after, frames_n, 2, 48000.f);
    CHECK(memcmp(before, after,
                 (size_t) frames_n * 2u * sizeof(double)) == 0);
  }
}

static void
test_slc_display() {
  printf("SLC names, describes and renders as itself, not as an arpeggio\n");

  // The three functions that mask `effect & 15`. Every one of them would report
  // `SLC` as ARP / Arpeggio / `005` without a test past the nibble, and `005`
  // would COLLIDE with a real arpeggio in a pattern grid.
  char m[4];
  ntrk::note_fx_mnemonic(ntrk::kFxSlice, 5u, m);
  CHECK(strcmp(m, "SLC") == 0);
  ntrk::note_fx_mnemonic(0x00u, 5u, m);
  CHECK(strcmp(m, "ARP") == 0);         // the one it would have been

  char r[16];
  ntrk::note_fx_repr(ntrk::kFxSlice, 5u, r, sizeof r);
  CHECK(strcmp(r, "G05") == 0);         // a letter, as XM does
  ntrk::note_fx_repr(0x00u, 5u, r, sizeof r);
  CHECK(strcmp(r, "005") == 0);         // ...and the collision it avoids
  ntrk::note_fx_repr(ntrk::kFxSlice, 255u, r, sizeof r);
  CHECK(strcmp(r, "GFF") == 0);

  char d[64];
  ntrk::note_fx_describe(ntrk::kFxSlice, 5u, d, sizeof d);
  CHECK(strstr(d, "slice") != NULL);
  CHECK(strstr(d, "5") != NULL);
  CHECK(strstr(d, "rpeggio") == NULL);  // the wrong answer, named
  ntrk::note_fx_describe(ntrk::kFxSlice, 0u, d, sizeof d);
  CHECK(strstr(d, "0") != NULL);        // SLC 00 describes slice 0, not silence

  // The table is one longer, and the new row is the command it says it is.
  int count = 0;
  const ntrk::CmdInfo *table = ntrk::note_fx_table(&count);
  CHECK(count == 33);
  CHECK(table[32].cmd == ntrk::kFxSlice);
  CHECK(strcmp(table[32].mnemonic, "SLC") == 0);
  CHECK(ntrk::note_fx_index(ntrk::kFxSlice, 0u) == 32);
  // Every byte still lands in the table -- the guarantee `note_fx_index`
  // carries, now over a wider set.
  for (int e = 0; e < 256; ++e)
    for (int pv = 0; pv < 256; pv += 17) {
      const int idx = ntrk::note_fx_index((uint8_t) e, (uint8_t) pv);
      CHECK(idx >= 0 && idx < count);
    }
}

static void
test_mixr_block() {
  printf("the mixer block survives a save, and a malformed one is refused\n");

  Spec s;
  build(s);
  ntrk::Module a;
  CHECK(ntrk::module_load(&a, g_bytes, g_size));
  CHECK(!a.has_mix);                    // a file without the block carries none

  // A payload with the two fields the format owns set correctly and every other
  // byte something that is obviously not zero, so a block that came back zeroed
  // rather than copied fails here rather than passing by accident.
  for (int i = 0; i < ntrk::kMixrBytes; ++i)
    a.mix[i] = (uint8_t) (0x40 + i);
  ntrk::write_u16(a.mix + 0, (uint16_t) ntrk::kMixrVersion);
  ntrk::write_u16(a.mix + 2, (uint16_t) ntrk::kMixrSlots);
  a.has_mix = true;

  size_t need = 0;
  CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &need));
  CHECK(need == g_size + (size_t) ntrk::kDirectoryEntryBytes +
                    (size_t) ntrk::kMixrBytes);

  ntrk::Module b;
  CHECK(ntrk::module_load(&b, g_saved, need));
  CHECK(b.has_mix);
  CHECK(memcmp(b.mix, a.mix, (size_t) ntrk::kMixrBytes) == 0);

  // MIXR is written last of the payloads and the total is exact, so the block
  // is the file's tail. Nothing else in this test needs to walk the directory.
  const size_t at = need - (size_t) ntrk::kMixrBytes;
  CHECK(memcmp(g_saved + at, a.mix, (size_t) ntrk::kMixrBytes) == 0);

  // A version this reader does not know, and a slot count that disagrees with
  // the length. Both are refused rather than read as far as they parse -- the
  // same rule FXPL's geometry prefix is held to.
  ntrk::write_u16(g_saved + at + 0, (uint16_t) (ntrk::kMixrVersion + 1));
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u16(g_saved + at + 0, (uint16_t) ntrk::kMixrVersion);
  ntrk::write_u16(g_saved + at + 2, (uint16_t) (ntrk::kMixrSlots + 1));
  CHECK(!ntrk::module_load(&b, g_saved, need));
  ntrk::write_u16(g_saved + at + 2, (uint16_t) ntrk::kMixrSlots);
  CHECK(ntrk::module_load(&b, g_saved, need));

  // And the writer refuses the same thing, so a save cannot produce a file the
  // next load turns away.
  ntrk::write_u16(a.mix + 0, 0u);
  CHECK(!ntrk::module_save(&a, g_saved, sizeof g_saved, &need));
  ntrk::write_u16(a.mix + 0, (uint16_t) ntrk::kMixrVersion);
  ntrk::write_u16(a.mix + 2, 0u);
  CHECK(!ntrk::module_save(&a, g_saved, sizeof g_saved, &need));
}

static void
test_v2_save_round_trip() {
  printf("a module round trips, and any other version is refused\n");

  // The plainest file there is, out and back in. It is byte-identical to what
  // went in, which is the property every shipped tune rests on: a module opened
  // and written back unchanged is the same file.
  {
    Spec s;
    build(s);
    ntrk::Module a;
    CHECK(ntrk::module_load(&a, g_bytes, g_size));
    CHECK(a.version == 2);
    CHECK(a.note_max == 36);
    CHECK(a.instruments[0].type == (uint8_t) ntrk::InstrumentType::kPcm8);
    CHECK(a.instruments[0].flags == 0);
    CHECK(a.instruments[0].transpose == 0);
    CHECK(a.fx == NULL);

    size_t need = 0;
    CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &need));
    CHECK(need == g_size);
    CHECK(memcmp(g_saved, g_bytes, need) == 0);

    ntrk::Module b;
    CHECK(ntrk::module_load(&b, g_saved, need));
    CHECK(b.version == 2);
    CHECK(b.note_max == 36);
    CHECK(b.channels == a.channels);
    CHECK(b.instrument_count == a.instrument_count);
    CHECK(b.instruments[0].length == a.instruments[0].length);
    CHECK(b.instruments[0].type == (uint8_t) ntrk::InstrumentType::kPcm8);
    CHECK(b.fx == NULL);
  }

  // A fully-loaded module out and back in. Not byte-identical to the hand-built file, and
  // deliberately so: the writer rebuilds the blob from what the instruments
  // point at, so sixty-one instruments sharing one wave are written out
  // sixty-one times. What has to survive is the module, not the layout.
  {
    SpecV2 s;
    build_v2(s);
    ntrk::Module a;
    CHECK(ntrk::module_load(&a, g_bytes, g_size));

    size_t need = 0;
    CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &need));

    ntrk::Module b;
    CHECK(ntrk::module_load(&b, g_saved, need));
    CHECK(b.version == 2);
    CHECK(b.note_max == 96);
    CHECK(b.channels == 16);
    CHECK(b.instrument_count == 64);
    CHECK(b.fx != NULL);
    CHECK(b.fx[0].cmd == a.fx[0].cmd);
    CHECK(b.fx[0].param == a.fx[0].param);

    int same = 1;
    for (int i = 0; i < a.instrument_count; ++i) {
      const ntrk::Instrument &x = a.instruments[i];
      const ntrk::Instrument &y = b.instruments[i];
      if (x.type != y.type || x.bits != y.bits || x.flags != y.flags ||
          x.transpose != y.transpose || x.env_attack_ms != y.env_attack_ms ||
          x.env_decay_ms != y.env_decay_ms ||
          x.env_release_ms != y.env_release_ms ||
          x.env_sustain != y.env_sustain ||
          x.filter_cutoff_hz != y.filter_cutoff_hz ||
          x.filter_res != y.filter_res || x.wave_index != y.wave_index ||
          x.length != y.length || x.loop_start != y.loop_start ||
          x.loop_len != y.loop_len || x.volume != y.volume)
        same = 0;
      const uint32_t bytes = x.length * (uint32_t) (x.bits / 8u);
      for (uint32_t k = 0; k < bytes; ++k)
        if (((const uint8_t *) x.data)[k] != ((const uint8_t *) y.data)[k])
          same = 0;
    }
    CHECK(same);
  }

  // **The writer refuses every version but 2, in both directions.** There is
  // one format now, and the byte is 2 rather than 1 precisely so that a file
  // written against the retired version-1 layout -- 20-byte instrument entries
  // against today's 32 -- fails the check instead of being read at the wrong
  // stride. A writer that emitted a 1 would manufacture exactly those files.
  {
    Spec s;
    build(s);
    ntrk::Module a;
    CHECK(ntrk::module_load(&a, g_bytes, g_size));
    size_t need = 0;
    CHECK(ntrk::module_save(&a, nullptr, 0, &need));      // the baseline saves

    ntrk::Module t = a;
    t.version = 1;
    CHECK(!ntrk::module_save(&t, nullptr, 0, &need));
    t.version = 3;
    CHECK(!ntrk::module_save(&t, nullptr, 0, &need));
    t.version = 0;
    CHECK(!ntrk::module_save(&t, nullptr, 0, &need));

    // A note past the module's own ceiling, written into the buffer `a` is a
    // view over, so it is the module that changes. The reader refuses it too --
    // see `test_save_round_trip` -- and the two agreeing is what stops the
    // format growing a file only one half of it accepts.
    set_cell(s, 0, 0, 60u, 1u, 0u, 0u);
    CHECK(!ntrk::module_save(&a, nullptr, 0, &need));
  }
}

// ---------------------------------------------------------------------------
// The plane's geometry and the macro table.
//
// The geometry is a payload prefix rather than a header byte, because v2's
// header is full and a header change is version 3. Everything below is about
// the two ways that can go wrong: a length that does not agree with the
// geometry it declares, and a meta lane that names a macro table which is not
// there.
// ---------------------------------------------------------------------------

static void
test_lanes_and_macros_load() {
  printf("multiple effect columns, meta lanes and a macro table load\n");

  SpecV2 s;
  s.fx_columns = 3;
  s.meta_columns = 2;
  s.macros = 2;
  build_v2(s);

  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.fx_columns == 3);
  CHECK(m.meta_columns == 2);
  CHECK(ntrk::module_lanes(&m) == 16 * 3 + 2);

  // Channel 0's first column, and the last channel's last one -- which is where
  // a reader striding by channels rather than by lanes would miss.
  CHECK(m.fx[0].cmd == 0x01);
  CHECK(m.fx[0].param == 200);
  const size_t last = (size_t) (m.channels - 1) * (size_t) m.fx_columns +
                      (size_t) (m.fx_columns - 1);
  CHECK(m.fx[last].cmd == 0x03);
  CHECK(m.fx[last].param == 90);

  // The meta lanes come after every channel lane, and a macro is named
  // one-based so that a rest is expressible.
  const size_t meta = (size_t) m.channels * (size_t) m.fx_columns;
  CHECK(m.fx[meta].cmd == 1);
  CHECK(m.fx[meta].param == 128);

  CHECK(m.macro_count == 2);
  CHECK(m.macros[0].target_count == 2);
  CHECK(m.macros[0].flags == ntrk::kMacroDelta);
  CHECK(m.macros[0].targets[0].target == 0);
  CHECK(m.macros[0].targets[0].scope == 3);
  CHECK(m.macros[0].targets[0].scale == 256);
  CHECK(m.macros[0].targets[0].offset == -128);
  CHECK(m.macros[0].targets[1].target == 4);
  CHECK(m.macros[0].targets[1].scope == ntrk::kMacroScopeAll);
  CHECK(m.macros[0].targets[1].scale == 512);
  CHECK(m.macros[0].targets[1].offset == 64);

  // **An empty target list is legal.** An editor makes a macro before it fills
  // one in, and refusing that means a song somebody is halfway through writing
  // will not load.
  CHECK(m.macros[1].target_count == 0);
  CHECK(m.macros[1].flags == 0);

  // A plane still plays: the player reads every one of a channel's columns, and
  // the wider row must not move where it thinks a row starts.
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  render(&p, 4096);
  CHECK(peak_between(0, 4096) > 0.01);

  // A macro table with no meta lanes is a song whose macros are written and not
  // yet invoked -- legal, and the converse of the rule below.
  SpecV2 unused = s;
  unused.meta_columns = 0;
  build_v2(unused);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.macro_count == 2);
  CHECK(m.meta_columns == 0);

  // Every geometry the format permits, at both ends of both ranges.
  for (int fc = 1; fc <= ntrk::kMaxFxColumns; ++fc)
    for (int mc = 0; mc <= ntrk::kMaxMetaColumns; ++mc) {
      SpecV2 g = s;
      g.fx_columns = fc;
      g.meta_columns = mc;
      g.macros = mc > 0 ? 1 : 0;
      build_v2(g);
      ntrk::Module w;
      CHECK(ntrk::module_load(&w, g_bytes, g_size));
      CHECK(ntrk::module_lanes(&w) == 16 * fc + mc);
    }
}

static void
test_lanes_and_macros_refusals() {
  printf("every geometry and macro bound refuses rather than clamps\n");

  SpecV2 s;
  s.fx_columns = 3;
  s.meta_columns = 2;
  s.macros = 2;
  ntrk::Module m;

  // The geometry prefix. Zero columns is a plane with no cells in it; nine is
  // past the eight-column ceiling. Eight itself is tested as *accepted* in
  // `test_lanes_and_macros_load`, so this pair pins the boundary from both
  // sides rather than only from above.
  //
  // **Built at the bad count rather than mutated to it**, so the block's length
  // agrees with the geometry it declares and the range check is the only thing
  // that can refuse the file. Poking the prefix alone would be refused by the
  // exact-length check whether or not a range check existed at all.
  {
    SpecV2 none = s;
    none.fx_columns = 0;
    build_v2(none);
    CHECK(!ntrk::module_load(&m, g_bytes, g_size));

    SpecV2 nine = s;
    nine.fx_columns = 9;
    build_v2(nine);
    CHECK(!ntrk::module_load(&m, g_bytes, g_size));

    SpecV2 five_meta = s;
    five_meta.meta_columns = 5;
    build_v2(five_meta);
    CHECK(!ntrk::module_load(&m, g_bytes, g_size));
  }

  build_v2(s);
  put_u16(v2_fx_at(s) + 2, 5u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // One cell's worth of disagreement between the declared length and the
  // geometry, in a file that is otherwise well-formed.
  build_v2(s);
  put_u32(v2_directory_at(s) + 8, (uint32_t) (v2_fx_bytes(s) - 2u));
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // **A meta lane with no macro table names nothing, on every cell.** The
  // cross-block rule, and it holds whichever order the directory lists them in.
  {
    SpecV2 orphan = s;
    orphan.macros = 0;
    build_v2(orphan);
    CHECK(!ntrk::module_load(&m, g_bytes, g_size));
  }

  const size_t mac = v2_macr_at(s);

  build_v2(s);
  put_u16(mac + 0, 0u);                 // macro_count is 1..32
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  put_u16(mac + 0, 33u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  put_u16(mac + 2, 1u);                 // the block's reserved word
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  put_u16(mac + 6, 1u);                 // a record's reserved word
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  g_bytes[mac + 4] = 9u;                // target_count is 0..8
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  g_bytes[mac + 5] = 0x02u;             // reserved flag bits
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  g_bytes[mac + 8] = 8u;                // a target past the mixer's value array
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // **Two disjoint ranges, and the gap between them is refused as firmly as the
  // top is.** 0x40..0x7F names an effect-slot parameter, in the same numbering
  // the plane's slot set commands use; 0x08..0x3F and 0x80 up name nothing, and
  // a macro is a table rather than a command, so there is nothing sensible for
  // an out-of-range entry to mean.
  build_v2(s);
  g_bytes[mac + 8] = 0x3fu;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  g_bytes[mac + 8] = 0x80u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  build_v2(s);
  g_bytes[mac + 8] = 0x40u;             // send 0, parameter 0
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.macros[0].targets[0].target == 0x40u);

  build_v2(s);
  g_bytes[mac + 8] = 0x7fu;             // the last slot id, parameter 7
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.macros[0].targets[0].target == 0x7fu);

  build_v2(s);
  g_bytes[mac + 9] = 16u;               // a scope that names nothing at all
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // 0xFE and 0xFF do name something, so they must still load: they are the
  // invoking channel and every channel.
  build_v2(s);
  g_bytes[mac + 9] = 0xfeu;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.macros[0].targets[0].scope == ntrk::kMacroScopeSelf);

  // **A slot past the target list must be all zero**, byte by byte: two
  // encodings of one song is a canonicalisation hazard, and this is a format
  // whose fingerprints are the safety net.
  for (int j = 0; j < 6; ++j) {
    build_v2(s);
    g_bytes[mac + 4u + 4u + 2u * 6u + (size_t) j] = 1u;   // slot 2 of record 0
    CHECK(!ntrk::module_load(&m, g_bytes, g_size));
  }

  // A record with no targets has eight slots to keep clean, so the same rule
  // reaches the empty macro the editor made.
  build_v2(s);
  g_bytes[mac + 4u + 52u + 4u] = 1u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // Truncation at each new boundary: inside the plane, inside the macro block's
  // own header, and one byte short of the last record.
  build_v2(s);
  CHECK(!ntrk::module_load(&m, g_bytes, v2_fx_at(s) + 2u));
  CHECK(!ntrk::module_load(&m, g_bytes, mac + 2u));
  CHECK(!ntrk::module_load(&m, g_bytes, g_size - 1u));

  // A macro table whose declared count and payload length disagree, in both
  // directions -- exact, like every other block.
  build_v2(s);
  put_u32(v2_directory_at(s) + 12u + 8u, (uint32_t) (v2_macr_bytes(s) - 52u));
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));
  build_v2(s);
  put_u16(mac + 0, 1u);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));
}

static void
test_lanes_and_macros_round_trip() {
  printf("the geometry and the macro table survive a save\n");

  // The widest geometry the format permits, so a save that mislaid a lane has
  // the most room to show it.
  SpecV2 s;
  s.fx_columns = 8;
  s.meta_columns = 4;
  s.macros = 5;
  build_v2(s);

  ntrk::Module a;
  CHECK(ntrk::module_load(&a, g_bytes, g_size));

  size_t need = 0;
  size_t wrote = 0;
  CHECK(ntrk::module_save(&a, nullptr, 0, &need));
  CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &wrote));
  CHECK(wrote == need);

  ntrk::Module b;
  CHECK(ntrk::module_load(&b, g_saved, wrote));
  CHECK(b.fx_columns == 8);
  CHECK(b.meta_columns == 4);
  CHECK(b.macro_count == 5);

  int same = 1;
  const size_t lanes = (size_t) ntrk::module_lanes(&a);
  for (size_t i = 0; i < lanes * (size_t) a.rows * (size_t) a.pattern_count; ++i)
    if (a.fx[i].cmd != b.fx[i].cmd || a.fx[i].param != b.fx[i].param)
      same = 0;
  for (int i = 0; i < a.macro_count; ++i) {
    if (a.macros[i].target_count != b.macros[i].target_count ||
        a.macros[i].flags != b.macros[i].flags)
      same = 0;
    for (int k = 0; k < ntrk::kMaxMacroTargets; ++k)
      if (a.macros[i].targets[k].target != b.macros[i].targets[k].target ||
          a.macros[i].targets[k].scope != b.macros[i].targets[k].scope ||
          a.macros[i].targets[k].scale != b.macros[i].targets[k].scale ||
          a.macros[i].targets[k].offset != b.macros[i].targets[k].offset)
        same = 0;
  }
  CHECK(same);

  // **Saving it twice gives the same bytes**, which is what makes the exactness
  // above worth having: the writer zeroes the slots past a target list rather
  // than carrying whatever the module held there.
  size_t again = 0;
  CHECK(ntrk::module_save(&b, g_bytes, sizeof g_bytes, &again));
  CHECK(again == wrote && memcmp(g_bytes, g_saved, wrote) == 0);

  // The writer refuses what the loader would: a geometry with no plane to write
  // it into, a meta lane with no macro table, and a target or a scope outside
  // the space. Each on its own, against a module that saves cleanly without it.
  {
    Spec plain;
    build(plain);
    ntrk::Module base;
    CHECK(ntrk::module_load(&base, g_bytes, g_size));
    CHECK(ntrk::module_save(&base, nullptr, 0, &need));   // the baseline

    ntrk::Module v;
    v = base;
    v.fx_columns = 2;
    CHECK(!ntrk::module_save(&v, nullptr, 0, &need));
    v = base;
    v.meta_columns = 1;
    CHECK(!ntrk::module_save(&v, nullptr, 0, &need));

    // A macro table with no plane is *not* refused: it is a song whose macros
    // are written and not yet invoked, which is what an editor holds for most
    // of the time one is open. The converse -- a meta lane naming a table that
    // is not there -- is the one that has no meaning, and is checked below.
    v = base;
    v.macro_count = 1;
    CHECK(ntrk::module_save(&v, nullptr, 0, &need));

    v = base;
    v.fx = (const ntrk::FxCell *) (const void *) g_saved;
    v.macro_count = 1;
    CHECK(ntrk::module_save(&v, nullptr, 0, &need));
    v.meta_columns = 1;
    v.macro_count = 0;
    CHECK(!ntrk::module_save(&v, nullptr, 0, &need));

    v.macro_count = 1;
    v.macros[0].target_count = 1;
    v.macros[0].targets[0].target = 8u;
    CHECK(!ntrk::module_save(&v, nullptr, 0, &need));
    v.macros[0].targets[0].target = 0u;
    v.macros[0].targets[0].scope = 16u;
    CHECK(!ntrk::module_save(&v, nullptr, 0, &need));
    v.macros[0].targets[0].scope = ntrk::kMacroScopeAll;
    v.macros[0].flags = 0x80u;
    CHECK(!ntrk::module_save(&v, nullptr, 0, &need));
    v.macros[0].flags = 0u;
    v.macros[0].target_count = 9u;
    CHECK(!ntrk::module_save(&v, nullptr, 0, &need));
  }
}

// ---------------------------------------------------------------------------
// The per-instrument volume envelope.
//
// A v2 file of its own rather than the wide one above, and deliberately the
// smallest one that can carry the question: one channel, one instrument, and a
// looped square wave held for the whole render. With nothing else moving, the
// peak of a window of output *is* the envelope level, read against the same
// module with the flag cleared.
// ---------------------------------------------------------------------------

struct SpecEnv {
  uint8_t flags = ntrk::kInstrumentEnvelope;
  uint16_t attack_ms = 100;
  uint16_t decay_ms = 100;
  uint16_t release_ms = 100;
  uint8_t sustain = 32;       // half of 64
  int off_row = -1;           // the row that plays `^^^`, if any
};

// At 48 kHz, 125 BPM and speed 6 a tick is 960 frames and a row is six of them,
// so row n begins at frame 5760n. Every window below is placed by that; 100 ms
// of envelope is 4800 frames. Eight rows is 46080, so nothing loops round inside
// a render.
static const int kEnvRows = 8;
static const int kEnvRowFrames = 5760;
static const int kEnvMsFrames = 48;
static const int kEnvFrames = 30000;
static const int kEnvSampleFrames = 32;

static void
build_env(const SpecEnv &s) {
  const size_t instrument_at = 32u + 1u;          // one order byte
  const size_t patterns_at = instrument_at + 32u;
  const size_t pattern_bytes = (size_t) kEnvRows * 4u;
  const size_t blob_at = patterns_at + pattern_bytes;
  g_size = blob_at + (size_t) kEnvSampleFrames;
  memset(g_bytes, 0, g_size);

  memcpy(g_bytes, "NTRK", 4);
  put_u16(4, 2);
  put_u16(6, 1);                                  // one channel
  put_u16(8, (uint16_t) kEnvRows);
  put_u16(10, 6);
  put_u16(12, 125);
  put_u16(14, 1);
  put_u16(16, 1);
  put_u16(18, 1);
  put_u16(20, 0);
  put_u32(22, (uint32_t) kEnvSampleFrames);
  put_u16(26, 0);                                 // no blocks
  g_bytes[28] = 36u;

  put_u32(instrument_at + 4, (uint32_t) kEnvSampleFrames);
  put_u32(instrument_at + 12, (uint32_t) kEnvSampleFrames);   // looped, so it holds
  g_bytes[instrument_at + 16] = 64u;              // volume
  g_bytes[instrument_at + 19] = s.flags;
  put_u16(instrument_at + 20, s.attack_ms);
  put_u16(instrument_at + 22, s.decay_ms);
  put_u16(instrument_at + 24, s.release_ms);
  g_bytes[instrument_at + 26] = s.sustain;

  g_bytes[patterns_at + 0] = 25u;                 // the note, on row 0
  g_bytes[patterns_at + 1] = 1u;
  if (s.off_row > 0)
    g_bytes[patterns_at + (size_t) s.off_row * 4u] = (uint8_t) ntrk::kNoteOff;

  for (int i = 0; i < kEnvSampleFrames; ++i)
    g_bytes[blob_at + (size_t) i] =
        (uint8_t) (int8_t) ((i % 8) < 4 ? 100 : -100);
}

// Finite *and* inside the mixer's clip, on any buffer. The bound is not
// decoration: `render_add` clamps each bus to +-1, so a value outside it is a
// NaN that walked through the compare or a sample that never went through the
// clip at all.
static bool
buffer_finite(const double *buffer, int values) {
  for (int i = 0; i < values; ++i) {
    const double v = buffer[i];
    if (v != v || v > 1.0 || v < -1.0)      // NaN fails its own comparison
      return false;
  }
  return true;
}

static bool
all_finite(int frames) {
  return buffer_finite(g_buffer, frames * 2);
}

static void
test_envelope() {
  printf("a volume envelope shapes the voice, and its absence changes nothing\n");

  ntrk::Module m;
  ntrk::Player p;

  // The reference every level is read against: the same module with the flag
  // clear, which sounds at full for the whole render.
  SpecEnv flat;
  flat.flags = 0;
  build_env(flat);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, kEnvFrames);
  const double full = peak_between(1000, 2000);
  CHECK(full > 0.1);
  memcpy(g_first, g_buffer, sizeof(double) * (size_t) kEnvFrames * 2u);

  // **Two per cent, and it is measurement rather than slop.** The square wave is
  // stepped at about a third of a frame a sample, so a window's largest value is
  // only reached every twenty-odd output frames — a peak therefore lags the
  // envelope by up to that, which across a 4800-frame attack is half a per cent.
  // The rest covers float drift over 4800 additions of 1/4800.
  const double tol = 0.02 * full;

  SpecEnv s;                                // 100 ms, 100 ms, half, held
  build_env(s);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, kEnvFrames);
  CHECK(all_finite(kEnvFrames));

  // Zero at the trigger: the first 96 frames are the first two per cent of the
  // attack and nothing else.
  CHECK(peak_between(0, 96) < 0.05 * full);
  // Full at the end of the attack, a half once the decay has run, and it stays
  // there for as long as the note is held.
  CHECK(peak_between(4 * kEnvMsFrames * 25 - 240, 100 * kEnvMsFrames) >
        full - tol);
  CHECK(peak_between(14000, 14240) > 0.5 * full - tol);
  CHECK(peak_between(14000, 14240) < 0.5 * full + tol);
  CHECK(peak_between(kEnvFrames - 240, kEnvFrames) > 0.5 * full - tol);
  CHECK(peak_between(kEnvFrames - 240, kEnvFrames) < 0.5 * full + tol);

  // **The guard on the whole property.** The same module with the envelope
  // fields at zero rather than set, the flag clear in both: not close, but the
  // same bytes. A gain applied "harmlessly" on the no-envelope path would show
  // up here and nowhere else.
  SpecEnv bare;
  bare.flags = 0;
  bare.attack_ms = 0;
  bare.decay_ms = 0;
  bare.release_ms = 0;
  bare.sustain = 0;
  build_env(bare);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, kEnvFrames);
  CHECK(memcmp(g_first, g_buffer,
               sizeof(double) * (size_t) kEnvFrames * 2u) == 0);

  // A zero-length attack: no division by a stage with no frames in it, so no
  // NaN, and full level on the *first* frame rather than one frame of silence to
  // hear as a click. With no decay and sustain at 64 the envelope is a constant
  // one, and `x * 1.0f == x` exactly — so this is bit-identical to the flat
  // render rather than merely close to it.
  SpecEnv fast;
  fast.attack_ms = 0;
  fast.decay_ms = 0;
  fast.release_ms = 0;
  fast.sustain = 64;
  build_env(fast);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, kEnvFrames);
  CHECK(all_finite(kEnvFrames));
  CHECK(peak_between(0, 96) > full - tol);
  CHECK(memcmp(g_first, g_buffer,
               sizeof(double) * (size_t) kEnvFrames * 2u) == 0);

  // And a zero attack in front of a real decay still falls, which is the case
  // the fall-through has to get right rather than skip past.
  SpecEnv percussive;
  percussive.attack_ms = 0;
  percussive.sustain = 0;
  build_env(percussive);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, kEnvFrames);
  CHECK(all_finite(kEnvFrames));
  CHECK(peak_between(0, 96) > full - tol);
  CHECK(peak_between(5000, 5240) == 0.0);
}

static void
test_envelope_note_off() {
  printf("note-off releases an envelope, and cuts a voice without one\n");

  // Row 2 begins at frame 11520, so a 100 ms release reaches zero at 16320.
  SpecEnv s;
  s.off_row = 2;
  build_env(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  render(&p, kEnvFrames);
  CHECK(all_finite(kEnvFrames));

  const int off = 2 * kEnvRowFrames;
  const double held = peak_between(off - 1500, off - 20);
  CHECK(held > 0.0);
  // Still sounding a moment after the note was let go, and quieter than it was.
  CHECK(peak_between(off + 120, off + 360) > 0.0);
  CHECK(peak_between(off + 120, off + 360) < held);
  // Gone once the release has run, and the voice with it — a channel left
  // playing at zero would go on being mixed for the rest of the tune.
  CHECK(peak_between(off + 100 * kEnvMsFrames + 80, kEnvFrames) == 0.0);
  CHECK(!p.channels[0].playing);

  // Without an envelope the same cell is an immediate cut, which is what `^^^`
  // has always meant to a tracker.
  SpecEnv cut = s;
  cut.flags = 0;
  build_env(cut);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::player_start(&p, &m);
  render(&p, kEnvFrames);
  CHECK(peak_between(off - 1500, off - 20) > 0.0);
  CHECK(peak_between(off + 8, kEnvFrames) == 0.0);
  CHECK(!p.channels[0].playing);

  // A note-off is the one value above `note_max` a v2 file may hold, so the
  // writer has to let it through — refusing it would make a tune that uses
  // `^^^` unsaveable.
  size_t need = 0;
  CHECK(ntrk::module_save(&m, g_saved, sizeof g_saved, &need));
}

// ---------------------------------------------------------------------------
// Synth instruments: the SYNP block and the analog drum voices.
//
// One channel, one row with one note, and fifteen empty rows after it, so a
// drum is struck once and then left alone for the whole render. That is the
// only shape in which "decays to exactly zero" can be asked at all -- a
// retrigger would be answering a different question -- and the sixteen rows are
// what keeps the pattern longer than the render, so the tune never loops back
// on to the note.
//
// Five instruments, and the *second* of them deliberately is not a synth. A
// SYNP record binds to an instrument by its position among the synths and by
// nothing else, so a table with a gap in it is exactly the case a reader that
// walked the wrong list would get wrong.
// ---------------------------------------------------------------------------

static const int kSynInstruments = 5;
static const int kSynSynths = 4;
static const int kSynPcmFrames = 8;
static const int kSynRows = 16;
static const float kSynRate = 22050.f;
static const int kSynFrames = 32768;      // 1.49 s, and exactly g_buffer
static const size_t kSynRecordBytes = 16u;

// Every byte of a SYNP record but the reserved one: voice, tune, decay, sweep,
// tone, noise, noise_decay, drive, then the seven the bass reads -- one row per
// synth instrument, so a test can move one byte and rebuild.
static const int kSynParams = 15;
static uint8_t g_syn[kSynSynths][kSynParams];

struct SpecSyn {
  int note_instrument = 1;   // 1 kick, 3 snare, 4 hihat, 5 clap (2 is the PCM8)
  bool synp = true;
  int synp_delta = 0;        // bytes added to the block's *declared* length
  bool synths = true;        // clear to make every instrument PCM8
};

static size_t syn_instruments_at() { return 33u; }
static size_t syn_entry_at(int i) {
  return syn_instruments_at() + (size_t) i * 32u;
}
static size_t syn_patterns_at() {
  return syn_instruments_at() + (size_t) kSynInstruments * 32u;
}
static size_t syn_blob_at() {
  return syn_patterns_at() + (size_t) kSynRows * 4u;
}
static size_t syn_dir_at() { return syn_blob_at() + (size_t) kSynPcmFrames; }
static size_t syn_synp_at() { return syn_dir_at() + 12u; }

static void
syn_defaults() {
  for (int i = 0; i < kSynSynths; ++i) {
    for (int k = 0; k < kSynParams; ++k)
      g_syn[i][k] = 0u;
    g_syn[i][0] = (uint8_t) i;    // one of each drum voice, in order
    g_syn[i][1] = 128u;           // tune
    g_syn[i][2] = 40u;            // decay
    g_syn[i][3] = 160u;           // sweep
    g_syn[i][4] = 128u;           // tone
    g_syn[i][5] = 60u;            // noise against body
    g_syn[i][6] = 40u;            // noise decay
    g_syn[i][7] = 0u;             // drive
    // 8..14 are the bass's, and a drum ignores every one of them. Left at zero
    // so a drum render cannot depend on what they happen to hold.
  }
}

// The bass's half of a record, named rather than positional: nine bytes in a
// row is exactly the argument list a test gets wrong silently.
struct Bass {
  uint8_t decay = 0u;       // 0 is the shortest, and the only one that ends
  uint8_t cutoff = 200u;    // inside the 4096-frame window at 22050
  uint8_t reso = 0u;
  uint8_t env_mod = 0u;
  uint8_t accent = 0u;
  uint8_t drive = 0u;
  uint8_t dist = 0u;
  uint8_t mix = 0u;
  uint8_t wave = 0u;        // saw
};

// Instrument 5 is the last synth, so `note_instrument = 5` plays it. The other
// three stay drums, which is the point: a bass beside a kit rather than instead
// of one.
static void
syn_bass(const Bass &b) {
  uint8_t *r = g_syn[kSynSynths - 1];
  r[0] = 4u;                    // (uint8_t) SynthVoice::kBass
  r[2] = b.decay;
  r[8] = b.cutoff;
  r[9] = b.reso;
  r[10] = b.env_mod;
  r[11] = b.accent;
  r[7] = b.drive;
  r[12] = b.dist;
  r[13] = b.mix;
  r[14] = b.wave;
}

static void
build_syn(const SpecSyn &s) {
  // Twenty-four bytes of slack past the records, so a declared length that is
  // wrong by one record is still *inside* the file: the refusal under test is
  // then the length check and not the bounds check that runs before it.
  g_size = syn_synp_at() + kSynSynths * kSynRecordBytes + 24u;
  memset(g_bytes, 0, g_size);

  memcpy(g_bytes, "NTRK", 4);
  put_u16(4, 2);
  put_u16(6, 1);                              // one channel
  put_u16(8, (uint16_t) kSynRows);
  put_u16(10, 6);
  put_u16(12, 125);
  put_u16(14, 1);
  put_u16(16, 1);
  put_u16(18, (uint16_t) kSynInstruments);
  put_u16(20, 0);
  put_u32(22, (uint32_t) kSynPcmFrames);      // the blob is the PCM8 wave
  put_u16(26, s.synp ? 1u : 0u);
  g_bytes[28] = 96u;

  for (int i = 0; i < kSynInstruments; ++i) {
    const size_t e = syn_entry_at(i);
    g_bytes[e + 16] = 64u;                    // volume
    if (i == 1 || !s.synths) {
      put_u32(e + 4, (uint32_t) kSynPcmFrames);
      put_u32(e + 12, (uint32_t) kSynPcmFrames);   // looped, so it holds
      g_bytes[e + 18] = 0u;                        // PCM8
    } else {
      g_bytes[e + 18] = 4u;                        // SYNTH: no blob at all
    }
  }

  const size_t cell = syn_patterns_at();
  g_bytes[cell + 0] = 25u;                    // C-3, where `tune` is defined
  g_bytes[cell + 1] = (uint8_t) s.note_instrument;

  const size_t blob = syn_blob_at();
  for (int i = 0; i < kSynPcmFrames; ++i)
    g_bytes[blob + (size_t) i] = (uint8_t) (int8_t) ((i % 8) < 4 ? 100 : -100);

  if (!s.synp)
    return;
  const size_t dir = syn_dir_at();
  put_u16(dir + 0, 3u);                       // SYNP
  put_u16(dir + 2, 1u);                       // critical: no drums without it
  put_u32(dir + 4, (uint32_t) syn_synp_at());
  put_u32(dir + 8, (uint32_t) ((size_t) (kSynSynths * (int) kSynRecordBytes) +
                               (size_t) s.synp_delta));

  for (int i = 0; i < kSynSynths; ++i) {
    const size_t r = syn_synp_at() + (size_t) i * kSynRecordBytes;
    for (int k = 0; k < kSynParams; ++k)
      g_bytes[r + (size_t) k] = g_syn[i][k];
  }
}

static void
render_at(ntrk::Player *player, int frames, float rate) {
  for (int i = 0; i < frames * 2; ++i)
    g_buffer[i] = 0.0;
  ntrk::render_add(player, g_buffer, frames, 2, rate);
}

// Rendered once, from the top, with a fresh player.
static void
syn_render(int note_instrument, int frames) {
  SpecSyn s;
  s.note_instrument = note_instrument;
  build_syn(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  render_at(&p, frames, kSynRate);
}

static int
zero_crossings(int first, int last) {
  int n = 0;
  double prev = g_buffer[(size_t) first * 2u];
  for (int f = first + 1; f < last; ++f) {
    const double x = g_buffer[(size_t) f * 2u];
    if ((x > 0.0 && prev <= 0.0) || (x < 0.0 && prev >= 0.0))
      ++n;
    prev = x;
  }
  return n;
}

static void
test_synth_loads() {
  printf("a synth instrument loads, with its SYNP record\n");

  syn_defaults();
  SpecSyn s;
  build_syn(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  CHECK(m.instrument_count == kSynInstruments);

  // The records skip the PCM8 entry: instrument 2 is not a synth, so the snare
  // record is instrument 3's and not instrument 2's.
  CHECK(m.instruments[0].type == (uint8_t) ntrk::InstrumentType::kSynth);
  CHECK(m.instruments[1].type == (uint8_t) ntrk::InstrumentType::kPcm8);
  CHECK(m.instruments[0].synth_voice == (uint8_t) ntrk::SynthVoice::kKick);
  CHECK(m.instruments[2].synth_voice == (uint8_t) ntrk::SynthVoice::kSnare);
  CHECK(m.instruments[3].synth_voice == (uint8_t) ntrk::SynthVoice::kHihat);
  CHECK(m.instruments[4].synth_voice == (uint8_t) ntrk::SynthVoice::kClap);

  CHECK(m.instruments[0].synth_tune == 128);
  CHECK(m.instruments[0].synth_decay == 40);
  CHECK(m.instruments[0].synth_sweep == 160);
  CHECK(m.instruments[0].synth_tone == 128);
  CHECK(m.instruments[0].synth_noise == 60);
  CHECK(m.instruments[0].synth_noise_decay == 40);
  CHECK(m.instruments[0].synth_drive == 0);

  // Nothing is written into an instrument that is not a synth, whatever else
  // the block holds.
  CHECK(m.instruments[1].synth_voice == 0);
  CHECK(m.instruments[1].synth_tune == 0);
}

static void
test_synth_refusals() {
  printf("every SYNP bound refuses rather than clamps\n");

  syn_defaults();
  ntrk::Module m;

  SpecSyn s;
  build_syn(s);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));        // the baseline loads

  // A length that merely fits is a block that does not line up with the
  // instruments it belongs to, in either direction.
  SpecSyn shorter = s;
  shorter.synp_delta = -(int) kSynRecordBytes;
  build_syn(shorter);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  SpecSyn longer = s;
  longer.synp_delta = (int) kSynRecordBytes;
  build_syn(longer);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // Not a multiple of a record at all.
  SpecSyn ragged = s;
  ragged.synp_delta = 1;
  build_syn(ragged);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // Synths and no block: the file names four drums and says nothing about any
  // of them, which is silence rather than a tune with a part missing.
  SpecSyn none = s;
  none.synp = false;
  build_syn(none);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // And the other way: a block whose records belong to nothing.
  SpecSyn orphan = s;
  orphan.synths = false;
  build_syn(orphan);
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // With neither, it is an ordinary v2 file.
  SpecSyn plain = s;
  plain.synths = false;
  plain.synp = false;
  build_syn(plain);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  // Truncated at the SYNP boundary: the file is well formed right up to the
  // byte that is missing, so this is the bounds check firing and not the
  // length check above it.
  build_syn(s);
  CHECK(!ntrk::module_load(&m, g_bytes, syn_synp_at() + 8u));
  CHECK(!ntrk::module_load(&m, g_bytes, syn_dir_at() + 6u));

  // A voice this reader does not have.
  build_syn(s);
  g_bytes[syn_synp_at() + 0] = 5u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // The reserved tail of a record, so the byte stays free to mean something in
  // a later version.
  build_syn(s);
  g_bytes[syn_synp_at() + 15] = 1u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));
  build_syn(s);
  g_bytes[syn_synp_at() + 15] = 0x80u;
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // The bass's two enumerated fields, and on a record whose voice is a *kick*:
  // the range belongs to the record, so a value nothing is going to read is
  // still a file that has gone wrong.
  build_syn(s);
  g_bytes[syn_synp_at() + 12] = 4u;         // a shaper kind that is not one
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));
  build_syn(s);
  g_bytes[syn_synp_at() + 14] = 2u;         // saw and square, and no third
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));

  // And the bass itself loads, with every one of its bytes carried through.
  syn_defaults();
  Bass b;
  b.decay = 90u; b.cutoff = 170u; b.reso = 200u; b.env_mod = 140u;
  b.accent = 60u; b.drive = 80u; b.dist = 2u; b.mix = 255u; b.wave = 1u;
  syn_bass(b);
  build_syn(s);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  const ntrk::Instrument &bi = m.instruments[4];
  CHECK(bi.synth_voice == (uint8_t) ntrk::SynthVoice::kBass);
  CHECK(bi.synth_decay == 90u && bi.synth_cutoff == 170u);
  CHECK(bi.synth_reso == 200u && bi.synth_env_mod == 140u);
  CHECK(bi.synth_accent == 60u && bi.synth_drive == 80u);
  CHECK(bi.synth_dist == 2u && bi.synth_dist_mix == 255u && bi.synth_wave == 1u);
  syn_defaults();

  // Two SYNP blocks is a file that disagrees with itself, the same as two of
  // anything else.
  build_syn(s);
  put_u16(26, 2u);
  put_u16(syn_dir_at() + 12, 3u);
  put_u16(syn_dir_at() + 14, 1u);
  put_u32(syn_dir_at() + 16, (uint32_t) syn_synp_at());
  put_u32(syn_dir_at() + 20, (uint32_t) (kSynSynths * (int) kSynRecordBytes));
  CHECK(!ntrk::module_load(&m, g_bytes, g_size));
}

static void
test_synth_voices() {
  printf("each drum sounds, stays finite, and decays to exactly zero\n");

  syn_defaults();
  // The four instruments the pattern can name; 2 is the PCM8 one.
  const int slot[4] = {1, 3, 4, 5};
  for (int v = 0; v < 4; ++v) {
    syn_render(slot[v], kSynFrames);
    CHECK(all_finite(kSynFrames));
    CHECK(peak_between(0, 2000) > 0.02);

    // Exactly zero, not merely small: the snap in `synth_env_step` is what ends
    // the voice, and a voice that never ends is one the mixer carries for the
    // rest of the tune.
    CHECK(peak_between(28000, kSynFrames) == 0.0);
  }

  // ...and the channel really has stopped, rather than going on being mixed at
  // zero. Re-rendered rather than read from the loop above, because the module
  // the player points at is rebuilt in `g_bytes` each time.
  {
    SpecSyn s;
    build_syn(s);
    ntrk::Module m;
    CHECK(ntrk::module_load(&m, g_bytes, g_size));
    ntrk::Player p;
    ntrk::player_start(&p, &m);
    render_at(&p, kSynFrames, kSynRate);
    CHECK(!p.channels[0].playing);
  }
}

static void
test_synth_distinct() {
  printf("the four drums are measurably different from each other\n");

  syn_defaults();
  // Body only for the two voices that have one, so the comparison is of the
  // voices rather than of how much noise each was told to mix in. The two
  // noise-only voices ignore the field, which is itself the point.
  g_syn[0][5] = 0u;
  g_syn[1][5] = 0u;

  const int slot[4] = {1, 3, 4, 5};
  int zc[4];
  for (int v = 0; v < 4; ++v) {
    syn_render(slot[v], 8000);
    // Past the attack, where the click and the sweep have gone and what is left
    // is the voice's own pitch.
    zc[v] = zero_crossings(2000, 6000);
  }

  // A kick is a low body, a snare a higher one, a clap a band around a
  // kilohertz and a hihat everything above several. Wide margins: the exact
  // counts are not the claim, the ordering is.
  CHECK(zc[0] > 0);
  CHECK(zc[0] * 2 < zc[1]);
  CHECK(zc[1] * 2 < zc[3]);
  CHECK(zc[3] * 2 < zc[2]);

  // And the clap alone re-strikes: three fast slaps, so the envelope *rises*
  // again where every other voice only falls. The interval is 11 ms, which at
  // this rate is 242 frames.
  syn_render(5, 8000);
  const double before = peak_between(434, 484);
  const double after = peak_between(484, 534);
  CHECK(before > 0.0);
  CHECK(after > before * 1.15);
}

static void
test_synth_range() {
  printf("a synth voice is finite across its whole parameter range\n");

  syn_defaults();
  SpecSyn s;
  build_syn(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  // Poked on the loaded module rather than rebuilt through the file: the file
  // path is already covered above, and what is under test here is the DSP.
  uint8_t *p[kSynParams] = {
      &m.instruments[0].synth_voice,   &m.instruments[0].synth_tune,
      &m.instruments[0].synth_decay,   &m.instruments[0].synth_sweep,
      &m.instruments[0].synth_tone,    &m.instruments[0].synth_noise,
      &m.instruments[0].synth_noise_decay, &m.instruments[0].synth_drive,
      &m.instruments[0].synth_cutoff,  &m.instruments[0].synth_reso,
      &m.instruments[0].synth_env_mod, &m.instruments[0].synth_accent,
      &m.instruments[0].synth_dist,    &m.instruments[0].synth_dist_mix,
      &m.instruments[0].synth_wave,
  };
  const uint8_t values[3] = {0u, 128u, 255u};

  int finite = 1;
  for (int voice = 0; voice < ntrk::kSynthVoiceCount; ++voice) {
    for (int f = 1; f < kSynParams; ++f) {
      for (int k = 0; k < 3; ++k) {
        syn_defaults();
        for (int j = 1; j < kSynParams; ++j)
          *p[j] = g_syn[0][j];
        *p[0] = (uint8_t) voice;
        *p[f] = values[k];

        // Both ends of the rate range and both ends of the note range, because
        // the cutoff a voice asks for is the product of the two.
        ntrk::Player q;
        ntrk::player_start(&q, &m);
        render_at(&q, 4096, 8000.f);
        if (!all_finite(4096))
          finite = 0;

        ntrk::player_start(&q, &m);
        CHECK(ntrk::player_preview(&q, 0, 96, 1, 96000.0));
        render_at(&q, 4096, 96000.f);
        if (!all_finite(4096))
          finite = 0;

        ntrk::player_start(&q, &m);
        CHECK(ntrk::player_preview(&q, 0, 1, 1, 8000.0));
        render_at(&q, 4096, 8000.f);
        if (!all_finite(4096))
          finite = 0;
      }
    }
  }
  CHECK(finite);
}

static void
test_synth_deterministic() {
  printf("two players render a synth module to identical samples\n");

  syn_defaults();
  SpecSyn s;
  build_syn(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  // The noise is what makes this worth asking: it comes off the channel's own
  // generator, so it must not depend on anything outside the channel.
  ntrk::Player a;
  ntrk::player_start(&a, &m);
  render_at(&a, 16384, kSynRate);
  memcpy(g_first, g_buffer, sizeof(double) * 16384u * 2u);

  ntrk::Player b;
  ntrk::player_start(&b, &m);
  render_at(&b, 16384, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * 16384u * 2u) == 0);
}

static void
test_synth_absent() {
  printf("a module with no synth in it renders exactly what it always did\n");

  // The guard. Two files identical but for instruments the pattern never
  // names: one all PCM8 and blockless, one with four SYNTH instruments and a
  // SYNP block. The note plays the PCM8 instrument in both, so a synth path
  // that leaked into the sample path -- or a SYNP block that perturbed the
  // load -- would show up here as a difference.
  syn_defaults();
  ntrk::Module m;

  SpecSyn plain;
  plain.note_instrument = 2;
  plain.synths = false;
  plain.synp = false;
  build_syn(plain);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::Player a;
  ntrk::player_start(&a, &m);
  render_at(&a, 16384, kSynRate);
  memcpy(g_first, g_buffer, sizeof(double) * 16384u * 2u);

  SpecSyn withsyn;
  withsyn.note_instrument = 2;
  build_syn(withsyn);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::Player b;
  ntrk::player_start(&b, &m);
  render_at(&b, 16384, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * 16384u * 2u) == 0);

  // And the same claim one level in: a module whose instruments are drums
  // renders identically whether or not a bass instrument sits beside them
  // unplayed. The wider SYNP record, the extra voice in the dispatch and the
  // bass's own state resets all run in the second file and none of them may
  // reach the kick.
  syn_defaults();
  SpecSyn drums;
  drums.note_instrument = 1;              // the kick
  build_syn(drums);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::Player c;
  ntrk::player_start(&c, &m);
  render_at(&c, 16384, kSynRate);
  memcpy(g_first, g_buffer, sizeof(double) * 16384u * 2u);

  Bass bs;
  bs.reso = 255u; bs.env_mod = 255u; bs.drive = 255u; bs.mix = 255u;
  syn_bass(bs);
  build_syn(drums);
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::Player d;
  ntrk::player_start(&d, &m);
  render_at(&d, 16384, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * 16384u * 2u) == 0);
  syn_defaults();

  // The two pinned fingerprints in `test_render_hashes` are the other half of
  // this claim and cover the sample path itself; a synth change that moved
  // either of them is wrong, never re-blessed.
}

// ---------------------------------------------------------------------------
// The bass voice.
// ---------------------------------------------------------------------------

// High-frequency content as a share of the whole: the energy of the first
// difference against the energy of the signal. A first difference is a
// one-zero highpass, so this needs no transform -- and being a *ratio* it does
// not move when the envelope changes the level, which is the only reason it can
// be compared across two points in one note.
static double
hf_ratio(int first, int last) {
  double d = 0.0, e = 0.0;
  double prev = g_buffer[(size_t) first * 2u];
  for (int f = first + 1; f < last; ++f) {
    const double x = g_buffer[(size_t) f * 2u];
    const double dx = x - prev;
    d += dx * dx;
    e += x * x;
    prev = x;
  }
  return e > 0.0 ? d / e : 0.0;
}

// The share of the energy sitting inside a band around `hz`. The analyser is
// the header's own SVF in its bandpass tap, which is the cheapest honest answer
// available here -- there is no transform in this project and a resonance test
// does not need one.
static double
band_share(int first, int last, float hz, float rate) {
  ntrk::fx::Svf f;
  ntrk::fx::svf_set(&f, hz, 0.9f, rate);
  double band = 0.0, total = 0.0;
  for (int i = first; i < last; ++i) {
    const double x = g_buffer[(size_t) i * 2u];
    const float y = ntrk::fx::svf_bandpass(&f, (float) x);
    band += (double) y * (double) y;
    total += x * x;
  }
  return total > 0.0 ? band / total : 0.0;
}

// Instrument 5 is the bass; the other four instruments are left as they were,
// so every one of these renders a bass line beside an untouched kit.
static void
bass_render(const Bass &b, int frames) {
  syn_defaults();
  syn_bass(b);
  SpecSyn s;
  s.note_instrument = 5;
  build_syn(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  render_at(&p, frames, kSynRate);
}

static void
test_bass_sounds() {
  printf("the bass sounds, stays finite, and decays to exactly zero\n");

  Bass b;
  b.cutoff = 220u;
  bass_render(b, kSynFrames);
  CHECK(all_finite(kSynFrames));
  CHECK(peak_between(0, 2000) > 0.02);

  // Exactly zero, and for the same reason a drum is: the -100 dB snap ends the
  // envelope rather than a denormal eventually doing it. And the channel has
  // genuinely stopped, rather than going on being mixed at zero for the rest of
  // the tune.
  //
  // **Auditioned rather than sequenced, because the tail outlasts the
  // pattern.** `decay` on this voice is the *filter* envelope; the VCA is fixed
  // hardware -- a fast transient into a body of about a fifth of a second -- so
  // even the shortest `decay` takes a couple of seconds to reach -100 dB, and a
  // sixteen-row pattern would strike the note again long before then.
  // `player_preview` on a player that was never started is one note with
  // nothing after it, which is the only way "does it end" is answerable at all.
  syn_defaults();
  syn_bass(b);
  SpecSyn s;
  s.note_instrument = 5;
  build_syn(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  ntrk::Player p;
  p.module = &m;
  CHECK(ntrk::player_preview(&p, 0, 25, 5, (double) kSynRate));
  const int kBassChunk = 4096;
  int rendered = 0;
  while (p.channels[0].playing && rendered < 32 * kBassChunk) {
    render_at(&p, kBassChunk, kSynRate);
    CHECK(all_finite(kBassChunk));
    rendered += kBassChunk;
  }
  CHECK(!p.channels[0].playing);
  render_at(&p, kBassChunk, kSynRate);
  CHECK(peak_between(0, kBassChunk) == 0.0);

  // Square is not saw. Two shapes, and they have to actually differ or the
  // `wave` byte is decoration.
  Bass saw;
  saw.cutoff = 255u;
  bass_render(saw, 4096);
  memcpy(g_first, g_buffer, sizeof(double) * 4096u * 2u);
  Bass sq = saw;
  sq.wave = 1u;
  bass_render(sq, 4096);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * 4096u * 2u) != 0);
}

static void
test_bass_cutoff() {
  printf("the bass cutoff moves the spectrum\n");

  // Env mod at zero throughout, so the filter is static and the only thing
  // under test is where its corner sits.
  Bass low;
  low.cutoff = 60u;
  bass_render(low, 4096);
  const double q_low = hf_ratio(512, 4096);

  Bass high;
  high.cutoff = 255u;
  bass_render(high, 4096);
  const double q_high = hf_ratio(512, 4096);

  // A wide margin on purpose: the claim is that closing the filter takes the
  // top off, not that it takes a particular amount off.
  CHECK(q_low > 0.0);
  CHECK(q_high > q_low * 4.0);
}

static void
test_bass_resonance() {
  printf("the bass resonance is audible at the cutoff\n");

  // One cutoff, everything else held still, and the band the analyser looks at
  // is asked of the voice rather than written down beside it -- a hard-coded
  // frequency here would go quietly wrong the day the cutoff map is retuned,
  // and the test would then be measuring a band the filter is not in.
  Bass flat;
  flat.cutoff = 150u;
  const float at = ntrk::synth303_cutoff_hz(flat.cutoff);

  bass_render(flat, 4096);
  const double s_flat = band_share(512, 4096, at, kSynRate);

  Bass sharp = flat;
  sharp.reso = 255u;
  bass_render(sharp, 4096);
  const double s_sharp = band_share(512, 4096, at, kSynRate);

  CHECK(s_flat > 0.0);
  CHECK(s_sharp > s_flat * 1.5);
}

static void
test_bass_env_mod() {
  printf("the bass env mod moves the spectrum over the life of a note\n");

  // Two windows in one note. With env mod the filter is wide open at the start
  // and closing by the second window; with none it is where `cutoff` put it for
  // the whole note, and the two windows have to agree.
  Bass modded;
  modded.cutoff = 100u;
  modded.env_mod = 255u;
  modded.decay = 20u;          // short enough that the two windows are far apart
  bass_render(modded, 16384);
  const double m_early = hf_ratio(256, 1024);
  const double m_late = hf_ratio(12288, 15360);
  CHECK(m_early > m_late * 2.0);

  Bass flat = modded;
  flat.env_mod = 0u;
  bass_render(flat, 16384);
  const double f_early = hf_ratio(256, 1024);
  const double f_late = hf_ratio(12288, 15360);
  CHECK(f_early > 0.0);
  // Within a fifth of each other, which for a static filter is the drift of the
  // measure itself rather than anything moving.
  CHECK(f_late > f_early * 0.9 && f_late < f_early * 1.1);
}

// Four renders held side by side, so the comparison is of every sample rather
// than of a summary that two different signals could share.
static double g_dist[4][8192];

static void
test_bass_distortion() {
  printf("the four shapers give the bass four different sounds\n");

  const int frames = 4096;
  for (int kind = 0; kind < 4; ++kind) {
    Bass b;
    b.cutoff = 230u;
    b.drive = 255u;
    b.mix = 255u;
    b.dist = (uint8_t) kind;
    bass_render(b, frames);
    for (int i = 0; i < frames * 2; ++i)
      g_dist[kind][i] = g_buffer[i];
  }

  int distinct = 1;
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j)
      if (memcmp(g_dist[i], g_dist[j], sizeof(double) * (size_t) frames * 2u) == 0)
        distinct = 0;
  CHECK(distinct);

  // Different, and audibly so rather than in the last bit: each shaper differs
  // from warm by at least a per cent of warm's own peak.
  int audible = 1;
  double warm_peak = 0.0;
  for (int i = 0; i < frames * 2; ++i) {
    const double a = g_dist[0][i] < 0.0 ? -g_dist[0][i] : g_dist[0][i];
    if (a > warm_peak)
      warm_peak = a;
  }
  CHECK(warm_peak > 0.0);
  for (int kind = 1; kind < 4; ++kind) {
    double d = 0.0;
    for (int i = 0; i < frames * 2; ++i) {
      const double x = g_dist[kind][i] - g_dist[0][i];
      const double a = x < 0.0 ? -x : x;
      if (a > d)
        d = a;
    }
    if (!(d > warm_peak * 0.01))
      audible = 0;
  }
  CHECK(audible);

  // `drive` 0 is an exact bypass whatever the mix says, so the four kinds
  // collapse to one signal -- that is `shape_sample`'s guarantee, and the mix
  // must not smuggle a difference past it.
  Bass dry;
  dry.cutoff = 230u;
  dry.mix = 255u;
  bass_render(dry, frames);
  memcpy(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u);
  Bass dry_fold = dry;
  dry_fold.dist = 3u;
  bass_render(dry_fold, frames);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u) == 0);
}

static void
test_bass_finite() {
  printf("the bass is finite across its grid, resonance and drive at maximum\n");

  // The corner the grid test in `test_synth_range` cannot reach, because it
  // moves one field at a time: maximum resonance *with* maximum drive and
  // maximum accent is where a resonant peak, a shaper and a level boost all
  // multiply, and the output stage is the only thing holding it.
  const uint8_t v[3] = {0u, 128u, 255u};
  int finite = 1;
  for (int cutoff = 0; cutoff < 3; ++cutoff) {
    for (int wave = 0; wave < 2; ++wave) {
      for (int kind = 0; kind < 4; ++kind) {
        Bass b;
        b.cutoff = v[cutoff];
        b.wave = (uint8_t) wave;
        b.dist = (uint8_t) kind;
        b.reso = 255u;
        b.drive = 255u;
        b.mix = 255u;
        b.accent = 255u;
        b.env_mod = 255u;
        b.decay = 255u;
        bass_render(b, 4096);
        if (!all_finite(4096))
          finite = 0;
      }
    }
  }
  CHECK(finite);

  // And at both ends of the rate range, where the cutoff the filter is asked
  // for is furthest from the one it can have.
  syn_defaults();
  Bass b;
  b.reso = 255u; b.drive = 255u; b.mix = 255u; b.accent = 255u;
  b.env_mod = 255u; b.cutoff = 255u;
  syn_bass(b);
  SpecSyn s;
  s.note_instrument = 5;
  build_syn(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Player q;
  ntrk::player_start(&q, &m);
  CHECK(ntrk::player_preview(&q, 0, 96, 5, 8000.0));
  render_at(&q, 4096, 8000.f);
  CHECK(all_finite(4096));

  ntrk::player_start(&q, &m);
  CHECK(ntrk::player_preview(&q, 0, 1, 5, 96000.0));
  render_at(&q, 4096, 96000.f);
  CHECK(all_finite(4096));
  syn_defaults();
}

static void
test_bass_deterministic() {
  printf("two players render a bass module to identical samples\n");

  syn_defaults();
  Bass b;
  b.cutoff = 190u; b.reso = 220u; b.env_mod = 200u; b.accent = 128u;
  b.drive = 160u; b.mix = 200u; b.dist = 1u; b.decay = 60u;
  syn_bass(b);
  SpecSyn s;
  s.note_instrument = 5;
  build_syn(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Player a;
  ntrk::player_start(&a, &m);
  render_at(&a, 16384, kSynRate);
  memcpy(g_first, g_buffer, sizeof(double) * 16384u * 2u);

  ntrk::Player c;
  ntrk::player_start(&c, &m);
  render_at(&c, 16384, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * 16384u * 2u) == 0);

  // The coefficient glide counts samples, so a render split into two chunks
  // must land on the same samples as one -- otherwise the voice depends on the
  // host's buffer size, which is the failure the drums' own determinism test
  // exists to catch and this voice has a second way to reach.
  ntrk::Player d;
  ntrk::player_start(&d, &m);
  for (int i = 0; i < 16384 * 2; ++i)
    g_buffer[i] = 0.0;
  ntrk::render_add(&d, g_buffer, 6000, 2, kSynRate);
  ntrk::render_add(&d, g_buffer + 6000 * 2, 10384, 2, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * 16384u * 2u) == 0);
  syn_defaults();
}

static void
test_synth_save_round_trip() {
  printf("a synth module round trips, records and all\n");

  syn_defaults();
  // Three drums and a bass, so the record's second half is carried by the same
  // save that carries its first.
  Bass bs;
  bs.decay = 70u; bs.cutoff = 190u; bs.reso = 210u; bs.env_mod = 120u;
  bs.accent = 90u; bs.drive = 130u; bs.dist = 3u; bs.mix = 180u; bs.wave = 1u;
  syn_bass(bs);
  SpecSyn s;
  s.note_instrument = 5;
  build_syn(s);
  ntrk::Module a;
  CHECK(ntrk::module_load(&a, g_bytes, g_size));

  size_t need = 0;
  CHECK(ntrk::module_save(&a, nullptr, 0, &need));
  size_t wrote = 0;
  CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &wrote));
  CHECK(wrote == need);

  ntrk::Module b;
  CHECK(ntrk::module_load(&b, g_saved, wrote));
  CHECK(b.instrument_count == kSynInstruments);

  int same = 1;
  for (int i = 0; i < a.instrument_count; ++i) {
    const ntrk::Instrument &x = a.instruments[i];
    const ntrk::Instrument &y = b.instruments[i];
    if (x.type != y.type || x.synth_voice != y.synth_voice ||
        x.synth_tune != y.synth_tune || x.synth_decay != y.synth_decay ||
        x.synth_sweep != y.synth_sweep || x.synth_tone != y.synth_tone ||
        x.synth_noise != y.synth_noise ||
        x.synth_noise_decay != y.synth_noise_decay ||
        x.synth_drive != y.synth_drive || x.synth_cutoff != y.synth_cutoff ||
        x.synth_reso != y.synth_reso || x.synth_env_mod != y.synth_env_mod ||
        x.synth_accent != y.synth_accent || x.synth_dist != y.synth_dist ||
        x.synth_dist_mix != y.synth_dist_mix || x.synth_wave != y.synth_wave)
      same = 0;
  }
  CHECK(same);

  // And it sounds the same, which is the claim a field comparison only implies.
  ntrk::Player p;
  ntrk::player_start(&p, &a);
  render_at(&p, 8192, kSynRate);
  memcpy(g_first, g_buffer, sizeof(double) * 8192u * 2u);
  ntrk::player_start(&p, &b);
  render_at(&p, 8192, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * 8192u * 2u) == 0);

  // The writer refuses the same ranges the reader does, or it produces a file
  // nothing can open.
  a.instruments[0].synth_voice = 5u;
  CHECK(!ntrk::module_save(&a, nullptr, 0, &need));
  a.instruments[0].synth_voice = 0u;
  a.instruments[0].synth_dist = 4u;
  CHECK(!ntrk::module_save(&a, nullptr, 0, &need));
  a.instruments[0].synth_dist = 0u;
  a.instruments[0].synth_wave = 2u;
  CHECK(!ntrk::module_save(&a, nullptr, 0, &need));
}

// ---------------------------------------------------------------------------
// The plane's player half: FXPL 0x30 accent and 0x31 slide.
//
// **Assembled as a struct rather than as a file**, which the format supports
// outright -- `patterns` and `fx` are const *pointers* to memory the caller
// owns, so that an editor can point a `Module` at its own arrays. The loader
// has its own tests above; what is under test here is the player.
// ---------------------------------------------------------------------------

static const int kFxRows = 8;
static ntrk::Note g_fx_pat[kFxRows];
static ntrk::FxCell g_fx_plane[kFxRows];
static const uint8_t g_fx_order[1] = {0};

static void
fx_clear() {
  for (int i = 0; i < kFxRows; ++i) {
    g_fx_pat[i] = ntrk::Note();
    g_fx_plane[i] = ntrk::FxCell();
  }
}

// One channel, six ticks to a row, and one instrument: a 303 at full accent
// *depth*, so the only variable in the accent tests below is the row's trigger.
static void
fx_module(ntrk::Module *m, bool with_plane) {
  *m = ntrk::Module();
  m->version = 2;
  m->channels = 1;
  m->rows = kFxRows;
  m->speed = 6;
  m->bpm = 125;
  m->order_count = 1;
  m->pattern_count = 1;
  m->instrument_count = 1;
  m->note_max = 96;
  m->order = g_fx_order;
  m->patterns = g_fx_pat;
  m->fx = with_plane ? g_fx_plane : NULL;

  ntrk::Instrument &b = m->instruments[0];
  b.volume = 64u;
  b.type = (uint8_t) ntrk::InstrumentType::kSynth;
  b.synth_voice = (uint8_t) ntrk::SynthVoice::kBass;
  b.synth_decay = 200u;       // about a second, so one note holds the window
  b.synth_cutoff = 100u;
  // Moderate resonance and env mod, and that matters: at the top of both, an
  // accented note runs into the voice's own output saturator and its *peak*
  // stops moving -- which would make a level test measure `soft_clip` rather
  // than the accent. Energy below rather than peak, for the same reason.
  b.synth_reso = 60u;
  b.synth_env_mod = 60u;
  b.synth_accent = 255u;
}

static double
energy_between(int first, int last) {
  double e = 0.0;
  for (int f = first; f < last; ++f) {
    const double x = g_buffer[(size_t) f * 2u];
    e += x * x;
  }
  return e;
}

static void
test_plane_accent() {
  printf("an accent is louder, and opens the filter further\n");

  const int frames = 8192;

  fx_clear();
  g_fx_pat[0].note = 25u;
  g_fx_pat[0].instrument = 1u;

  ntrk::Module flat;
  fx_module(&flat, true);
  ntrk::Player p;
  ntrk::player_start(&p, &flat);
  render_at(&p, frames, kSynRate);
  const double flat_energy = energy_between(0, frames);
  const double flat_hf = hf_ratio(0, frames);

  g_fx_plane[0].cmd = 0x30u;
  g_fx_plane[0].param = 255u;
  ntrk::Module hit;
  fx_module(&hit, true);
  ntrk::player_start(&p, &hit);
  render_at(&p, frames, kSynRate);

  CHECK(flat_energy > 0.0);
  CHECK(energy_between(0, frames) > flat_energy * 1.5);
  // ...and it opens the filter envelope further, which is the half of an accent
  // a level measurement cannot see. A wide margin on purpose: the claim is that
  // the top comes up, not that a particular amount of it does.
  CHECK(flat_hf > 0.0);
  CHECK(hf_ratio(0, frames) > flat_hf * 1.2);

  // **Depth times trigger.** The SYNP byte says how much an accent does to this
  // instrument and the row says whether this note is accented; an instrument
  // declaring no depth is not accented however the pattern is written.
  ntrk::Module deaf;
  fx_module(&deaf, true);
  deaf.instruments[0].synth_accent = 0u;
  ntrk::player_start(&p, &deaf);
  render_at(&p, frames, kSynRate);
  memcpy(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u);

  g_fx_plane[0].cmd = 0u;
  g_fx_plane[0].param = 0u;
  ntrk::Module deaf_flat;
  fx_module(&deaf_flat, true);
  deaf_flat.instruments[0].synth_accent = 0u;
  ntrk::player_start(&p, &deaf_flat);
  render_at(&p, frames, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u) == 0);
}

static void
test_plane_slide() {
  printf("a slide glides the pitch from the previous note to the new one\n");

  fx_clear();
  g_fx_pat[0].note = 13u;             // C-2, period 428
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;             // C-3, period 214
  g_fx_pat[1].instrument = 1u;
  g_fx_plane[1].cmd = 0x31u;
  g_fx_plane[1].param = 4u;           // four ticks of glide

  ntrk::Module m;
  fx_module(&m, true);
  ntrk::Player p;
  ntrk::player_start(&p, &m);

  // The sequencer stepped by hand and the *period* sampled rather than the
  // audio: the pitch is what is under test, and reading it is a great deal more
  // direct than inferring one from a waveform.
  ntrk::player_tick(&p, kSynRate);              // row 0 tick 0: the first note
  CHECK(p.channels[0].period == ntrk::period_for(13, 0));
  for (int i = 0; i < 5; ++i)
    ntrk::player_tick(&p, kSynRate);            // the rest of row 0
  CHECK(p.channels[0].period == ntrk::period_for(13, 0));

  // Row 1's tick 0 strikes the new note and leaves the pitch where the old one
  // was. A glide that jumped first and slid afterwards would not be one.
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(13, 0));

  ntrk::player_tick(&p, kSynRate);
  ntrk::player_tick(&p, kSynRate);
  const double mid = p.channels[0].period;
  CHECK(mid > ntrk::period_for(25, 0));                           // halfway: at neither end
  CHECK(mid < ntrk::period_for(13, 0));

  ntrk::player_tick(&p, kSynRate);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(25, 0));         // and it arrives exactly

  // The voice is still sounding, and it is the *same* voice -- see
  // `test_plane_slide_ties`, which is where that claim is actually tested.
  CHECK(p.channels[0].playing);
}

// ---------------------------------------------------------------------------
// The two things about a slide that make it a 303's rather than a portamento:
// the shape of the curve, and that it does not strike a new note.
// ---------------------------------------------------------------------------

// The period after `ticks` ticks of a glide from `from` to `to`, both given as
// notes, driven through the player rather than computed.
static double
slide_period_after(uint8_t from, uint8_t to, uint8_t param, int ticks) {
  fx_clear();
  g_fx_pat[0].note = from;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = to;
  g_fx_pat[1].instrument = 1u;
  g_fx_plane[1].cmd = 0x31u;
  g_fx_plane[1].param = param;

  ntrk::Module m;
  fx_module(&m, true);
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  for (int i = 0; i < 6; ++i)                   // all of row 0
    ntrk::player_tick(&p, kSynRate);
  ntrk::player_tick(&p, kSynRate);              // row 1 tick 0: no glide yet
  for (int i = 0; i < ticks; ++i)
    ntrk::player_tick(&p, kSynRate);
  return p.channels[0].period;
}

static void
test_plane_slide_is_exponential() {
  printf("the slide is exponential in pitch, and the same both ways\n");

  // C-2 is period 428 and C-3 is 214: an octave, which makes every number
  // below one anyone can check by eye.
  const double lo = ntrk::period_for(13, 0), hi = ntrk::period_for(25, 0);

  // **Halfway through a glide is not halfway between the two periods.** A
  // linear walk in period would put it at 321, which is also where a listener
  // would say the pitch has only travelled a fifth of the way -- period is the
  // reciprocal of frequency, so a straight line in one is a bent one in the
  // other. A one-pole in frequency is past the midpoint by then, and well past
  // even the geometric mean of 302.5.
  const double mid = slide_period_after(13u, 25u, 6u, 3);
  CHECK(mid > hi);
  CHECK(mid < (lo + hi) * 0.5);                 // not the arithmetic mean
  CHECK(mid < 302.5);                           // nor the geometric one

  // **The same glide the other way covers the same fraction of the interval.**
  // Measured in *frequency*, the two directions are identical to the last bit,
  // because the walk is a one-pole on frequency and nothing else. That is the
  // property a linear period ramp does not have and cannot be given: it is
  // symmetric in period instead, which is asymmetric in everything anyone
  // hears.
  const double up = slide_period_after(13u, 25u, 6u, 3);
  const double down = slide_period_after(25u, 13u, 6u, 3);
  const double f_up = (1.0 / up - 1.0 / lo) / (1.0 / hi - 1.0 / lo);
  const double f_down = (1.0 / down - 1.0 / hi) / (1.0 / lo - 1.0 / hi);
  CHECK(f_up > 0.5 && f_up < 1.0);              // a real glide, not a jump
  const double d = f_up > f_down ? f_up - f_down : f_down - f_up;
  CHECK(d < 1e-12);

  // ...and the same two measured in period are *not* equal, which is what
  // makes the line above a claim about which space the glide lives in rather
  // than an identity that any glide would satisfy.
  const double p_up = (up - lo) / (hi - lo);
  const double p_down = (down - hi) / (lo - hi);
  const double dp = p_up > p_down ? p_up - p_down : p_down - p_up;
  CHECK(dp > 0.01);
}

static void
test_plane_slide_ties() {
  printf("a slid note ties the previous one instead of retriggering it\n");

  // One row is six ticks at 125 BPM, which is 2646 frames at this rate. Stop
  // just short of row 1 and then step across it, so the envelope is read on
  // either side of the note that either did or did not restart it.
  const int row = 2646;

  fx_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;

  // The control: no slide, so row 1 strikes an ordinary note.
  ntrk::Module strike;
  fx_module(&strike, true);
  ntrk::Player p;
  ntrk::player_start(&p, &strike);
  render_at(&p, row - 64, kSynRate);
  const float strike_before = p.channels[0].synth_venv;
  render_at(&p, 128, kSynRate);
  const float strike_after = p.channels[0].synth_venv;

  // **The amplitude envelope restarts**, which is what a struck note is.
  CHECK(strike_before > 0.f && strike_before < 0.9f);
  CHECK(strike_after > strike_before * 2.f);

  // And now the same two rows with a slide on the second.
  g_fx_plane[1].cmd = 0x31u;
  g_fx_plane[1].param = 4u;
  ntrk::Module tied;
  fx_module(&tied, true);
  ntrk::player_start(&p, &tied);
  render_at(&p, row - 64, kSynRate);
  const float tie_before = p.channels[0].synth_venv;
  render_at(&p, 128, kSynRate);
  const float tie_after = p.channels[0].synth_venv;

  // **It does not restart.** The envelope carries straight on decaying through
  // the row boundary -- the pitch moved and nothing else did, which is the
  // whole of what a slide is and most of why a slid line sounds like an
  // instrument rather than like a sequence of notes.
  CHECK(tie_before == strike_before);           // identical up to the boundary
  CHECK(tie_after < tie_before);
  // The filter envelope is not restarted either, and the phase is not reset.
  CHECK(p.channels[0].synth_age > (uint32_t) row);

  // ...but the pitch did move, or "does not retrigger" would be satisfied by
  // ignoring the row altogether. A tick is 441 frames here and the glide runs
  // on ticks 1 and up, so this is the first place it can have moved at all.
  render_at(&p, 1024, kSynRate);
  CHECK(p.channels[0].period < ntrk::period_for(13, 0));
  CHECK(p.channels[0].period > ntrk::period_for(25, 0));
}

// The gate a 303 line needs, and it is the ordinary instrument ADSR: a synth
// voice runs through `channel_gain_apply` like every other, so switching the
// envelope on is all it takes for `^^^` to mean "stop" instead of "abandon the
// channel mid-cycle".
//
// The settings are the ones a gate wants and no more: no attack, no decay, full
// sustain, and a release short enough to be a stop rather than a tail.
static void
gate_instrument(ntrk::Module *m, uint16_t release_ms) {
  ntrk::Instrument &b = m->instruments[0];
  b.flags = ntrk::kInstrumentEnvelope;
  b.env_attack_ms = 0u;
  b.env_decay_ms = 0u;
  b.env_sustain = 64u;
  b.env_release_ms = release_ms;
}

// The largest step between two neighbouring samples in `[first, last)`, which
// is what a click *is*: the discontinuity, not the level either side of it.
static double
max_step(int first, int last) {
  double worst = 0.0;
  for (int f = first; f < last; ++f) {
    const double d =
        g_buffer[(size_t) f * 2u] - g_buffer[(size_t) (f - 1) * 2u];
    const double a = d < 0.0 ? -d : d;
    if (a > worst)
      worst = a;
  }
  return worst;
}

static void
test_synth_note_off() {
  printf("a gated synth releases on a note-off, and a tie will not tie one\n");

  const int row = 2646;             // six ticks at 125 BPM and this rate
  const int fpms = 22;              // 22050 Hz, near enough for a window edge
  const int frames = 7 * row;

  // ---- the gate closes, and closes quietly --------------------------------
  //
  // **C-2 on row 3, and both are chosen.** A hard cut is only audible where the
  // waveform is away from zero when it happens, so a note-off landing on a zero
  // crossing measures nothing at all — this is the placement, of the twelve
  // swept, where the cut stands furthest above the saw's own slope.
  const int off = 3 * row;

  fx_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[3].note = (uint8_t) ntrk::kNoteOff;

  ntrk::Module gated;
  fx_module(&gated, true);
  gate_instrument(&gated, 8u);
  ntrk::Player p;
  ntrk::player_start(&p, &gated);
  render_at(&p, frames, kSynRate);
  CHECK(peak_between(off - 400, off) > 0.0);
  // **Exactly zero**, and the voice released rather than being left mixing a
  // silent channel for the rest of the tune.
  CHECK(peak_between(off + 8 * fpms + 80, frames) == 0.0);
  CHECK(!p.channels[0].playing);
  const double gated_step = max_step(off - 4, off + 8);
  // What the waveform does between two samples on its own, a moment earlier and
  // a shade louder. The click is measured against this rather than against a
  // constant, because the voice is decaying and a constant would be pinning the
  // level rather than the edge.
  const double running = max_step(off - 400, off - 8);

  // The same cell with no envelope on the instrument: the hard cut a tracker's
  // `^^^` has always been, pinned rather than merely abandoned. It is the same
  // silence and a far worse edge — the waveform stops wherever it stood.
  ntrk::Module cut;
  fx_module(&cut, true);
  ntrk::player_start(&p, &cut);
  render_at(&p, frames, kSynRate);
  CHECK(peak_between(off + 8, frames) == 0.0);
  CHECK(!p.channels[0].playing);
  const double cut_step = max_step(off - 4, off + 8);

  // The cut's step *is* the level the waveform stood at, three times what it
  // was travelling between samples. The release adds nothing to that slope at
  // all: an 8 ms ramp takes 1/176th off the level a sample, which is far below
  // what the saw is already doing.
  CHECK(running > 0.0);
  CHECK(cut_step > running * 2.0);
  CHECK(gated_step <= running);

  // ---- a tie holds the gate open ------------------------------------------
  //
  // The failure this rules out is a gate that a slide cannot hold: a 303's slid
  // step must go on sounding, and a gate that closed under it would silence the
  // articulation the slide exists for.
  fx_clear();
  g_fx_pat[0].note = 25u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[2].note = 13u;
  g_fx_pat[2].instrument = 1u;
  g_fx_plane[2].cmd = 0x31u;
  g_fx_plane[2].param = 4u;

  ntrk::Module tied;
  fx_module(&tied, true);
  gate_instrument(&tied, 8u);
  ntrk::player_start(&p, &tied);
  render_at(&p, frames, kSynRate);
  CHECK(peak_between(2 * row + 400, 3 * row) > 0.0);
  CHECK(p.channels[0].playing);

  // ---- ...but a tie will not tie a voice that is already dying -------------
  //
  // `^^^` and then a slid note while the release is still running. The gate is
  // shut, so there is no note to extend and the reference strikes; tying one
  // instead glides a fade-out, which is five times quieter and says nothing
  // about why. The release is long here on purpose — it is the only way the
  // slide can arrive *during* one.
  fx_clear();
  g_fx_pat[0].note = 25u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[2].note = (uint8_t) ntrk::kNoteOff;
  g_fx_pat[3].note = 13u;
  g_fx_pat[3].instrument = 1u;

  ntrk::Module strike;
  fx_module(&strike, true);
  gate_instrument(&strike, 300u);
  ntrk::player_start(&p, &strike);
  render_at(&p, frames, kSynRate);
  const double plain = peak_between(3 * row, 4 * row);

  g_fx_plane[3].cmd = 0x31u;
  g_fx_plane[3].param = 4u;
  ntrk::Module slid;
  fx_module(&slid, true);
  gate_instrument(&slid, 300u);
  ntrk::player_start(&p, &slid);
  render_at(&p, frames, kSynRate);
  CHECK(plain > 0.0);
  CHECK(peak_between(3 * row, 4 * row) > 0.9 * plain);
  // A fresh gate, not a release carried through the row.
  CHECK(p.channels[0].env_stage == ntrk::EnvStage::kSustain);

  // ---- and it costs nothing where nothing uses it -------------------------
  //
  // No `^^^` anywhere: sustain 64 with no attack and no decay is a level of
  // exactly 1.0, and `out * 1.0f` is exact, so the gate must be invisible in a
  // module that does not ask for it. **This is what every pinned fingerprint in
  // the project depends on**, so it is a `memcmp` and not a tolerance.
  fx_clear();
  g_fx_pat[0].note = 25u;
  g_fx_pat[0].instrument = 1u;

  ntrk::Module with;
  fx_module(&with, true);
  gate_instrument(&with, 8u);
  ntrk::player_start(&p, &with);
  render_at(&p, frames, kSynRate);
  memcpy(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u);

  ntrk::Module without;
  fx_module(&without, true);
  ntrk::player_start(&p, &without);
  render_at(&p, frames, kSynRate);
  CHECK(peak_between(0, frames) > 0.0);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u) == 0);
}

// A row that names no note leaves a glide running, because a slide time longer
// than the five effect ticks a row has is still a slide. Cancelled at the row
// boundary it would stop wherever it had got to, and the note would sound flat
// for as long as it lasted.
static void
test_plane_slide_crosses_a_row() {
  printf("a slide longer than its row finishes rather than stopping short\n");

  fx_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;
  g_fx_plane[1].cmd = 0x31u;
  g_fx_plane[1].param = 12u;          // more than one row's worth of ticks

  ntrk::Module m;
  fx_module(&m, true);
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  for (int i = 0; i < 7; ++i)                   // row 0, then row 1 tick 0
    ntrk::player_tick(&p, kSynRate);
  for (int i = 0; i < 5; ++i)                   // the rest of row 1
    ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period > ntrk::period_for(25, 0));          // not there yet: five of twelve
  CHECK(p.channels[0].period < ntrk::period_for(13, 0));          // but well on the way
  for (int i = 0; i < 24; ++i)                  // on through the empty rows
    ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(25, 0));         // and it arrives exactly
}

// Tone portamento is ProTracker's effect 0x3 and its linear-in-period walk is
// correct for a MOD, so the slide above had to be given its own glide rather
// than change this one. Pinned by stepping it a tick at a time: the period
// moves by exactly the parameter, every tick, and lands on the target.
static void
test_tone_porta_stays_linear() {
  printf("tone portamento still walks the period linearly, by its parameter\n");

  fx_clear();
  g_fx_pat[0].note = 13u;             // C-2, period 428
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;             // C-3, period 214: the destination
  g_fx_pat[1].instrument = 1u;
  g_fx_pat[1].effect = 0x3u;
  g_fx_pat[1].param = 40u;

  ntrk::Module m;
  fx_module(&m, true);
  ntrk::Player p;
  ntrk::player_start(&p, &m);
  for (int i = 0; i < 7; ++i)                   // row 0, then row 1 tick 0
    ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(13, 0));
  // Forty period units a tick, from wherever the note started — which is what
  // the effect promises, and does not depend on what note 13's period happens
  // to be this year.
  const double from = ntrk::period_for(13, 0);
  for (int i = 0; i < 5; ++i) {
    ntrk::player_tick(&p, kSynRate);
    CHECK(p.channels[0].period == from - 40.0 * (double) (i + 1));
  }
}

// The VCF and the VCA are two envelopes now, and this is the check that says
// so: `decay` moves one of them and not the other, at all, ever.
static void
test_bass_envelopes_are_separate() {
  printf("the bass VCF and VCA envelopes move independently\n");

  fx_clear();
  g_fx_pat[0].note = 25u;
  g_fx_pat[0].instrument = 1u;

  const int frames = 8192;

  ntrk::Module quick;
  fx_module(&quick, true);
  quick.instruments[0].synth_decay = 0u;        // the shortest filter envelope
  ntrk::Player p;
  ntrk::player_start(&p, &quick);
  render_at(&p, frames, kSynRate);
  const float quick_vcf = p.channels[0].synth_env;
  const float quick_vca = p.channels[0].synth_venv;
  const double quick_hf = hf_ratio(0, frames);

  ntrk::Module slow;
  fx_module(&slow, true);
  slow.instruments[0].synth_decay = 255u;       // and the longest
  ntrk::player_start(&p, &slow);
  render_at(&p, frames, kSynRate);
  const float slow_vcf = p.channels[0].synth_env;
  const float slow_vca = p.channels[0].synth_venv;
  const double slow_hf = hf_ratio(0, frames);

  // **The VCA is the same envelope in both.** It is fixed hardware on a 303 --
  // there is no knob for it and there is no byte for it here -- so the note has
  // the same shape whatever the filter is doing.
  CHECK(quick_vca > 0.f);
  CHECK(quick_vca == slow_vca);

  // **The VCF is not.** At the shortest setting it has already run out; at the
  // longest it is still most of the way open. Sharing one envelope between the
  // two, which is what this voice used to do, makes the first of these
  // impossible: the level would have run out with the filter.
  CHECK(quick_vcf < slow_vcf * 0.1f);

  // ...and it is audible, not merely a number: the longer filter envelope
  // leaves more above the cutoff for longer.
  CHECK(quick_hf > 0.0);
  CHECK(slow_hf > quick_hf * 1.2);
}

static void
test_plane_slide_zero_and_the_guard() {
  printf("slide zero is a jump, and a silent plane changes nothing\n");

  const int frames = 4096;

  fx_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;

  // No plane at all: the reference the two below have to reproduce bit for bit.
  ntrk::Module bare;
  fx_module(&bare, false);
  ntrk::Player p;
  ntrk::player_start(&p, &bare);
  render_at(&p, frames, kSynRate);
  memcpy(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u);

  // **The guard.** A module carrying a plane that says nothing renders exactly
  // what a module with no plane at all does, or the plane's mere presence is
  // itself a change of sound.
  ntrk::Module empty;
  fx_module(&empty, true);
  ntrk::player_start(&p, &empty);
  render_at(&p, frames, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u) == 0);

  // And a slide of zero ticks is the same again -- an immediate jump, not a
  // glide of no length that happens to round to the same samples.
  g_fx_plane[1].cmd = 0x31u;
  g_fx_plane[1].param = 0u;
  ntrk::Module jump;
  fx_module(&jump, true);
  ntrk::player_start(&p, &jump);
  render_at(&p, frames, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u) == 0);

  // ...where four ticks is not, which is what makes the two above a claim
  // rather than a tautology.
  g_fx_plane[1].param = 4u;
  ntrk::Module glide;
  fx_module(&glide, true);
  ntrk::player_start(&p, &glide);
  render_at(&p, frames, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u) != 0);
}

// `Instrument::transpose` was loaded, range-checked twice, folded into the
// "is this instrument non-default" test and written back out -- and never read
// by anything that made a sound. A field the format carries and the player
// ignores is worse than no field: the XM importer wanted exactly this and had
// to bake the shift into its cells instead, and `ntrk_gen`'s coverage module
// has been setting it to -12 for a knob wired to nothing.
static void
test_fractional_frame_and_note_step() {
  printf("a fractional frame blends its neighbours, and a note's step is the "
         "player's own\n");

  // A ramp, so the blend is readable off the position: frame f holds f * 10.
  static int8_t ramp[8];
  for (int i = 0; i < 8; ++i)
    ramp[i] = (int8_t) (i * 10);

  ntrk::Instrument ins = ntrk::Instrument();
  ins.data = ramp;
  ins.length = 8u;
  ins.volume = 64u;

  // The integers are the frames themselves...
  for (int i = 0; i < 8; ++i)
    CHECK(ntrk::instrument_frame_at(ins, (double) i) == (float) (i * 10));
  // ...and the halves are half way between two of them.
  CHECK(fabs(ntrk::instrument_frame_at(ins, 2.5) - 25.f) < 1e-4f);
  CHECK(fabs(ntrk::instrument_frame_at(ins, 0.25) - 2.5f) < 1e-4f);

  // The last frame has no successor, so with no loop it holds rather than
  // blending toward whatever follows the blob in memory.
  CHECK(fabs(ntrk::instrument_frame_at(ins, 7.5) - 70.f) < 1e-4f);
  // With a loop, its neighbour is the loop start -- so a looping sample does
  // not dip through the value it happens to end on.
  ins.loop_start = 0u;
  ins.loop_len = 8u;
  CHECK(fabs(ntrk::instrument_frame_at(ins, 7.5) - 35.f) < 1e-4f);

  // Out of range is silence, both ways.
  CHECK(ntrk::instrument_frame_at(ins, 8.0) == 0.f);
  CHECK(ntrk::instrument_frame_at(ins, -0.5) == 0.f);
  ntrk::Instrument empty = ntrk::Instrument();
  CHECK(ntrk::instrument_frame_at(empty, 0.0) == 0.f);

  // ---- and the step ----------------------------------------------------
  //
  // **The claim is that it IS the player's chain**, so it is checked against
  // the player rather than against arithmetic written here: a preview at a
  // different pitch from the tune is the one thing a preview must not be.
  fx_clear();
  g_fx_pat[0].note = 25u;
  g_fx_pat[0].instrument = 1u;

  ntrk::Module m;
  fx_module(&m, false);
  ntrk::Instrument &mi = m.instruments[0];
  mi = ntrk::Instrument();
  mi.data = ramp;
  mi.length = 8u;
  mi.loop_len = 8u;
  mi.volume = 64u;

  for (int transpose = -12; transpose <= 12; transpose += 12)
    for (int finetune = -8; finetune <= 7; finetune += 5) {
      mi.transpose = (int8_t) transpose;
      mi.finetune = (int8_t) finetune;

      ntrk::Player p;
      ntrk::player_start(&p, &m);
      ntrk::player_tick(&p, kSynRate);
      const double theirs = p.channels[0].step;
      const double ours =
          ntrk::instrument_note_step(mi, 25, m.note_max, kSynRate);
      CHECK(fabs(ours - theirs) < 1e-12);
    }

  // A note that cannot sound, a rate that cannot, and a module with no range:
  // zero, which is silence rather than a position that runs away.
  CHECK(ntrk::instrument_note_step(mi, 0, m.note_max, kSynRate) == 0.0);
  CHECK(ntrk::instrument_note_step(mi, 25, m.note_max, 0.0) == 0.0);
  CHECK(ntrk::instrument_note_step(mi, 25, 0, kSynRate) == 0.0);

  // The clamp `note_transposed` applies is applied here too, and BOTH ends are
  // checked because only one of them is reachable from a file -- which is
  // itself the finding. The loader bounds `transpose` to -48..48 and a note to
  // 1..note_max, so the lowest sum a file can name is `1 - 48`, which is
  // exactly `kMinNote`: the floor is a guard sitting on the boundary rather
  // than a case anything reaches, and asking for less is refused one line up as
  // a note of zero.
  mi.transpose = -48;
  mi.finetune = 0;
  CHECK(ntrk::instrument_note_step(mi, 1, m.note_max, kSynRate) > 0.0);
  CHECK(ntrk::instrument_note_step(mi, 0, m.note_max, kSynRate) == 0.0);

  // The ceiling IS reachable -- `note_max + 48` is past it for every module --
  // so it is asserted as the clamp's own effect rather than by restating the
  // formula: two notes that both land above it give the same step, and one that
  // does not is untouched.
  mi.transpose = 48;
  const double capped =
      ntrk::instrument_note_step(mi, m.note_max, m.note_max, kSynRate);
  CHECK(capped > 0.0);
  CHECK(ntrk::instrument_note_step(mi, m.note_max - 1, m.note_max, kSynRate) ==
        capped);
  CHECK(ntrk::instrument_note_step(mi, 1, m.note_max, kSynRate) != capped);
}

static void
test_instrument_transpose() {
  printf("an instrument's transpose shifts the notes it plays\n");

  static const int8_t wave[32] = {
      100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,
      100,  100,  100,  100,  100,  -100, -100, -100, -100, -100, -100,
      -100, -100, -100, -100, -100, -100, -100, -100, -100, -100,
  };
  // Row 0 triggers, row 1 glides to the same note: the two places a cell's note
  // becomes a pitch, and both have to agree or a porta would slide away from
  // where the trigger put it.
  fx_clear();
  g_fx_pat[0].note = 25u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;
  g_fx_pat[1].effect = 0x3u;      // tone portamento
  g_fx_pat[1].param = 0x01u;

  ntrk::Module m;
  fx_module(&m, false);
  ntrk::Instrument &ins = m.instruments[0];
  ins = ntrk::Instrument();
  ins.data = wave;
  ins.length = 32u;
  ins.loop_len = 32u;
  ins.volume = 64u;
  ins.transpose = -12;            // an octave down

  ntrk::Player p;
  ntrk::player_start(&p, &m);
  ntrk::player_tick(&p, kSynRate);
  // Note 25 is period 214; an octave below it is note 13, period 428.
  CHECK(p.channels[0].period == ntrk::period_for(13, 0));
  for (int i = 0; i < 6; ++i)
    ntrk::player_tick(&p, kSynRate);
  // The porta aims at the transposed note too, so it has nowhere to travel.
  CHECK(p.channels[0].target_period == ntrk::period_for(13, 0));

  // Zero transpose is the untouched path, and most modules are that path.
  ins.transpose = 0;
  ntrk::player_start(&p, &m);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(25, 0));

  // **The floor is kMinNote, not note 1.** A transpose is how a tune reaches the
  // four octaves under the format's own lowest note — the note byte cannot go
  // there, and MIDI 0 is down there. Note 25 shifted by -48 lands on -23, which
  // is inside the range rather than against its edge.
  ins.transpose = -48;
  ntrk::player_start(&p, &m);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(25 - 48, 0));

  // And the edge still holds. Note 1 is as low as a file can store, so -48 from
  // there is the one thing that reaches the floor — and MIDI 0 is exactly where
  // it lands, which is the whole of what "the full MIDI range" means here.
  g_fx_pat[0].note = 1u;
  g_fx_pat[1].note = 1u;
  fx_module(&m, false);
  m.instruments[0] = ins;
  m.instruments[0].transpose = -48;
  ntrk::player_start(&p, &m);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(ntrk::kMinNote, 0));

  // Put the fixture back for whatever runs after this.
  g_fx_pat[0].note = 25u;
  g_fx_pat[1].note = 25u;
  fx_module(&m, false);
  m.instruments[0] = ins;
  // Upward needs a note high enough to actually reach the ceiling: 25 + 48 is
  // 73, still inside a `note_max` of 96, so it shifts rather than clamps.
  ins.transpose = 48;
  ntrk::player_start(&p, &m);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(73, 0));
  g_fx_pat[0].note = 60u;
  ntrk::player_start(&p, &m);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(m.note_max, 0));
}

// 8xx and E8x were pinned as no-ops while the player summed to mono. Panning
// arrived with LRRL and a separation knob; these are the commands catching up,
// and the check that matters is the one at the bottom -- that a pan the player
// records is a pan the output actually carries.
static void
test_panning_commands() {
  printf("8xx and E8x pan a channel, and the output follows\n");

  fx_clear();
  g_fx_pat[0].note = 25u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[0].effect = 0x8u;
  g_fx_pat[0].param = 0x80u;  // centre
  g_fx_pat[1].effect = 0x8u;
  g_fx_pat[1].param = 0xFFu;  // hard right
  g_fx_pat[2].effect = 0xEu;
  g_fx_pat[2].param = 0x80u;  // E80, hard left
  g_fx_pat[3].effect = 0xEu;
  g_fx_pat[3].param = 0x8Fu;  // E8F, and it must reach the same edge as 8FF

  static const int8_t wave[32] = {
      100,  100,  100,  100,  100,  100,  100,  100,  100,  100,  100,
      100,  100,  100,  100,  100,  -100, -100, -100, -100, -100, -100,
      -100, -100, -100, -100, -100, -100, -100, -100, -100, -100,
  };
  ntrk::Module m;
  fx_module(&m, false);
  ntrk::Instrument &ins = m.instruments[0];
  ins = ntrk::Instrument();
  ins.data = wave;
  ins.length = 32u;
  ins.loop_len = 32u;
  ins.volume = 64u;

  ntrk::Player p;
  ntrk::player_start(&p, &m);
  // LRRL first, so every move below is away from a known start rather than a
  // value that happened to be there.
  CHECK(p.pan[0] == -1.f);

  ntrk::player_tick(&p, kSynRate);
  CHECK(p.pan[0] == 0.f);  // 0x80 is exactly centre, not a rounded one
  for (int i = 0; i < 6; ++i)
    ntrk::player_tick(&p, kSynRate);
  CHECK(p.pan[0] == 1.f);
  for (int i = 0; i < 6; ++i)
    ntrk::player_tick(&p, kSynRate);
  CHECK(p.pan[0] == -1.f);
  for (int i = 0; i < 6; ++i)
    ntrk::player_tick(&p, kSynRate);
  CHECK(p.pan[0] == 1.f);

  // And the output carries it. Hard left with the default separation is not
  // silence on the right, so this asks for a wide margin rather than a zero.
  double buf[512] = {};
  ntrk::player_start(&p, &m);
  p.pan[0] = -1.f;
  ntrk::render_add(&p, buf, 128, 2, 48000.f);
  double l = 0.0, r = 0.0;
  for (int i = 0; i < 128; ++i) {
    l += buf[i * 2] < 0 ? -buf[i * 2] : buf[i * 2];
    r += buf[i * 2 + 1] < 0 ? -buf[i * 2 + 1] : buf[i * 2 + 1];
  }
  CHECK(l > r * 2.0);
  CHECK(r > 0.0);
}

static void
test_plane_slide_reaches_a_sample() {
  printf("a slide reaches a plain sample voice, not only the 303\n");

  fx_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;
  g_fx_plane[1].cmd = 0x31u;
  g_fx_plane[1].param = 4u;

  // A looped PCM8 instrument in place of the synth: the glide walks `period`,
  // which is what everything pitched here is driven by.
  static const int8_t wave[32] = {
      100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
      100, 100, 100, 100, 100, -100, -100, -100, -100, -100, -100,
      -100, -100, -100, -100, -100, -100, -100, -100, -100, -100,
  };
  ntrk::Module m;
  fx_module(&m, true);
  ntrk::Instrument &ins = m.instruments[0];
  ins = ntrk::Instrument();
  ins.data = wave;
  ins.length = 32u;
  ins.loop_len = 32u;
  ins.volume = 64u;

  ntrk::Player p;
  ntrk::player_start(&p, &m);
  for (int i = 0; i < 7; ++i)
    ntrk::player_tick(&p, kSynRate);            // through row 0 into row 1
  CHECK(p.channels[0].period == ntrk::period_for(13, 0));
  for (int i = 0; i < 4; ++i)
    ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(25, 0));
  // The step follows the period, or the glide is a number nobody hears.
  CHECK(p.channels[0].step > 0.0);
}

// ---------------------------------------------------------------------------
// Every effect column, and the rule the player combines them by.
//
// **The rule here is deliberately not the mixer's**, and that is what these
// pin. The mixer sums its slides, because a mixer command is a per-tick delta
// on a continuous level and two columns pulling opposite ways should cancel.
// The player's half is an articulation of one note: two glides on one row is
// last column wins, because a note goes to one place and a sum of glide times
// means nothing. Accent and slide are different targets and simply both apply.
//
// A separate plane from `g_fx_plane` above, because the row stride is lanes and
// this one is four columns wide where that one is one.
// ---------------------------------------------------------------------------

static const int kFxCols = 4;
static ntrk::FxCell g_fx_wide[kFxRows * kFxCols];

static void
fx_wide_clear() {
  for (int i = 0; i < kFxRows; ++i)
    g_fx_pat[i] = ntrk::Note();
  for (int i = 0; i < kFxRows * kFxCols; ++i)
    g_fx_wide[i] = ntrk::FxCell();
}

static void
fx_wide_cell(int row, int col, uint8_t cmd, uint8_t param) {
  ntrk::FxCell &f = g_fx_wide[(size_t) row * (size_t) kFxCols + (size_t) col];
  f.cmd = cmd;
  f.param = param;
}

// The same module as `fx_module`, one channel wide, carrying the wide plane.
static void
fx_wide_module(ntrk::Module *m) {
  fx_module(m, true);
  m->fx = g_fx_wide;
  m->fx_columns = kFxCols;
}

static void
test_plane_every_column() {
  printf("the player reads every effect column, not only the first\n");

  const int frames = 8192;

  // **Accent in column 0, slide in column 2, both on one row.** Different
  // targets, so both apply -- and the slide has to be found at all, which is
  // the whole of what a first-column-only reader got wrong.
  fx_wide_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;
  fx_wide_cell(1, 0, 0x30u, 255u);              // accent
  fx_wide_cell(1, 2, 0x31u, 4u);                // and a four-tick glide

  ntrk::Module m;
  fx_wide_module(&m);
  ntrk::Player p;
  ntrk::player_start(&p, &m);

  for (int i = 0; i < 6; ++i)
    ntrk::player_tick(&p, kSynRate);            // all of row 0
  ntrk::player_tick(&p, kSynRate);              // row 1 tick 0
  CHECK(p.channels[0].accent == 1.f);           // column 0 was read
  CHECK(p.channels[0].period == ntrk::period_for(13, 0));         // and column 2: still at the old
  CHECK(p.channels[0].slide_ticks == 4);
  for (int i = 0; i < 4; ++i)
    ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(25, 0));         // arriving exactly, as ever

  // **Two slides on one row: the last column wins**, and the earlier one leaves
  // nothing behind. Eight ticks then two: after two ticks of row 1 the pitch is
  // already home, which an eight-tick glide could not be, and a *sum* of the
  // two would be slower still.
  fx_wide_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;
  fx_wide_cell(1, 0, 0x31u, 8u);
  fx_wide_cell(1, 3, 0x31u, 2u);

  fx_wide_module(&m);
  ntrk::player_start(&p, &m);
  for (int i = 0; i < 7; ++i)
    ntrk::player_tick(&p, kSynRate);            // through row 0 into row 1
  CHECK(p.channels[0].slide_ticks == 2);
  ntrk::player_tick(&p, kSynRate);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period == ntrk::period_for(25, 0));

  // And the other way round, so the test is about the *order* rather than about
  // two being smaller than eight.
  fx_wide_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;
  fx_wide_cell(1, 0, 0x31u, 2u);
  fx_wide_cell(1, 3, 0x31u, 8u);

  fx_wide_module(&m);
  ntrk::player_start(&p, &m);
  for (int i = 0; i < 7; ++i)
    ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].slide_ticks == 8);
  ntrk::player_tick(&p, kSynRate);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].period > ntrk::period_for(25, 0));          // still on its way, not home

  // **Two accents: last wins too**, for the same reason -- a note is struck at
  // one level. A quiet column to the right of a loud one takes it, which also
  // shows that a later cell is not merely being max'd with the earlier.
  fx_wide_clear();
  g_fx_pat[0].note = 25u;
  g_fx_pat[0].instrument = 1u;
  fx_wide_cell(0, 1, 0x30u, 255u);
  fx_wide_cell(0, 2, 0x30u, 64u);

  fx_wide_module(&m);
  ntrk::player_start(&p, &m);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].accent > 0.24f && p.channels[0].accent < 0.26f);

  // An explicit zero in a later column is "not accented", which is why the read
  // needs no presence flag: absence and `30 00` carry the same value.
  fx_wide_cell(0, 2, 0x30u, 0u);
  fx_wide_module(&m);
  ntrk::player_start(&p, &m);
  ntrk::player_tick(&p, kSynRate);
  CHECK(p.channels[0].accent == 0.f);

  // **The guard, in the currency that matters.** A one-column module renders
  // exactly what it always did: reading the columns past the first must be a
  // no-op for every file that exists, and the reference fingerprints say the
  // same thing about the tunes we ship.
  fx_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;
  g_fx_plane[0].cmd = 0x30u;
  g_fx_plane[0].param = 200u;
  g_fx_plane[1].cmd = 0x31u;
  g_fx_plane[1].param = 4u;

  ntrk::Module one;
  fx_module(&one, true);
  ntrk::player_start(&p, &one);
  render_at(&p, frames, kSynRate);
  memcpy(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u);

  // The same song written into column 0 of a four-column plane, with the other
  // three empty. Same audio, to the bit: a column carrying nothing says nothing.
  fx_wide_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;
  fx_wide_cell(0, 0, 0x30u, 200u);
  fx_wide_cell(1, 0, 0x31u, 4u);

  fx_wide_module(&m);
  ntrk::player_start(&p, &m);
  render_at(&p, frames, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u) == 0);

  // ...and moving that same pair into the *last* column is the same audio
  // again, which is the claim "every column" actually makes.
  fx_wide_clear();
  g_fx_pat[0].note = 13u;
  g_fx_pat[0].instrument = 1u;
  g_fx_pat[1].note = 25u;
  g_fx_pat[1].instrument = 1u;
  fx_wide_cell(0, kFxCols - 1, 0x30u, 200u);
  fx_wide_cell(1, kFxCols - 1, 0x31u, 4u);

  fx_wide_module(&m);
  ntrk::player_start(&p, &m);
  render_at(&p, frames, kSynRate);
  CHECK(memcmp(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u) == 0);
}

// ---------------------------------------------------------------------------
// The SYNP parameter grid.
//
// `test_synth_range` asks one question of this space -- does it stay finite --
// with one byte moved at a time off one set of defaults. This asks the two
// remaining properties the voices were built around: every render stays inside
// the mixer's clip, and every voice *ends*, at exactly zero, rather than
// leaving a tail the mixer carries for the rest of the tune.
//
// **Auditioned rather than sequenced, and that is what makes "does it end"
// answerable at all.** A pattern loops, so a long decay would be struck again
// before it finished. `player_preview` on a `Player` that has not been started
// leaves the sequencer stopped -- one note, and nothing after it -- which is
// also exactly what an editor auditioning an instrument does.
// ---------------------------------------------------------------------------

// Twenty-five seconds at the rate below, which is twice the worst case in the
// grid: the 303 at decay 255 is a 1.2 s time constant, and the -100 dB snap is
// 11.5 of those. A voice still sounding after this has not ended.
static const int kGridBudget = 262144;
static const float kGridRate = 8000.f;

// Renders one audition to its end. Every chunk is checked as it goes, so a NaN
// two seconds into a decay is caught rather than averaged away, and the chunk
// after the voice stops has to be exactly zero -- which is the claim `peak == 0`
// makes elsewhere, asked of the moment the channel actually stopped.
static bool
grid_ends_silent(const ntrk::Module *m, int instrument, float accent,
                 bool *sounded) {
  ntrk::Player p;
  p.module = m;
  if (!ntrk::player_preview(&p, 0, 25, instrument, (double) kGridRate))
    return false;
  p.channels[0].accent = accent;

  const int chunk = 2048;
  for (int done = 0; done < kGridBudget; done += chunk) {
    render_at(&p, chunk, kGridRate);
    if (!buffer_finite(g_buffer, chunk * 2))
      return false;
    // The vacuity guard: a voice that never sounded ends at zero for the least
    // interesting reason there is, and the caller counts these rather than
    // taking the pass.
    if (peak_between(0, chunk) > 0.0)
      *sounded = true;
    if (p.channels[0].playing)
      continue;
    render_at(&p, chunk, kGridRate);
    for (int i = 0; i < chunk * 2; ++i)
      if (g_buffer[i] != 0.0)
        return false;
    return true;
  }
  return false;                        // still sounding when the budget ran out
}

static void
test_synth_grid() {
  printf("every SYNP setting renders bounded, and every voice ends at zero\n");

  syn_defaults();
  // The shortest decay and the shortest noise decay as the base, so a sweep of
  // some *other* byte is not paying for a two-second tail on every one of its
  // values. The decay bytes are still swept below like all the rest.
  g_syn[0][2] = 0u;
  g_syn[0][6] = 0u;
  SpecSyn s;
  build_syn(s);
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Instrument &ins = m.instruments[0];
  uint8_t *p[kSynParams] = {
      &ins.synth_voice,   &ins.synth_tune,     &ins.synth_decay,
      &ins.synth_sweep,   &ins.synth_tone,     &ins.synth_noise,
      &ins.synth_noise_decay, &ins.synth_drive, &ins.synth_cutoff,
      &ins.synth_reso,    &ins.synth_env_mod,  &ins.synth_accent,
      &ins.synth_dist,    &ins.synth_dist_mix, &ins.synth_wave,
  };
  // Both ends, both quarters and the middle. 0 and 255 are the two that matter
  // most -- a decay of 0 is the divide a coefficient must not do, and 255 is the
  // longest thing the grid has to prove terminates.
  const uint8_t values[5] = {0u, 1u, 64u, 192u, 255u};

  int ok = 1;
  int reported = 0;
  int mute = 0;
  int sets = 0;
  for (int voice = 0; voice < ntrk::kSynthVoiceCount; ++voice) {
    for (int f = 1; f < kSynParams; ++f) {
      for (int k = 0; k < 5; ++k) {
        for (int j = 1; j < kSynParams; ++j)
          *p[j] = g_syn[0][j];
        *p[0] = (uint8_t) voice;
        *p[f] = values[k];
        // `dist` and `wave` are enumerations, not knobs: a value outside them is
        // a file the loader refuses, so feeding one here would be testing the
        // voice against a state it can never be in. Walked by index instead, so
        // all four shapers and both shapes are covered rather than whichever
        // ones the byte values happen to land on.
        if (f == 12)
          *p[f] = (uint8_t) (k % ntrk::kSynthDistKinds);
        if (f == 14)
          *p[f] = (uint8_t) (k % ntrk::kSynthWaveCount);

        bool sounded = false;
        ++sets;
        const bool ended = grid_ends_silent(&m, 1, 0.f, &sounded);
        if (!sounded)
          ++mute;
        if (ended)
          continue;
        ok = 0;
        if (reported++ < 6)
          printf("  voice %d, byte %d = %u\n", voice, f, (unsigned) *p[f]);
      }
    }
  }
  CHECK(ok);
  // Every set has to have made a sound, or "it ends at zero" is a claim about
  // silence. There is no threshold here on purpose: not one setting in this
  // grid is meant to mute a voice, so one that does is the finding.
  CHECK(sets == 350 && mute == 0);

  // The corners, which a one-byte-at-a-time sweep cannot reach: a filter at
  // maximum resonance driven into a shaper at maximum drive, with an accent on
  // top opening it further still. The output stage's soft clip is the only
  // thing between that and a mix full of infinities, and this is what says so.
  int corners = 1;
  for (int voice = 0; voice < ntrk::kSynthVoiceCount; ++voice) {
    for (int decay = 0; decay < 2; ++decay) {
      for (int dist = 0; dist < ntrk::kSynthDistKinds; ++dist) {
        for (int j = 1; j < kSynParams; ++j)
          *p[j] = 255u;
        *p[0] = (uint8_t) voice;
        *p[2] = decay == 0 ? 0u : 255u;              // shortest and longest
        *p[12] = (uint8_t) dist;
        *p[14] = 1u;                                 // square, the louder shape
        bool sounded = false;
        if (!grid_ends_silent(&m, 1, 1.f, &sounded)) // and a full accent
          corners = 0;
        if (!sounded)
          corners = 0;
      }
    }
  }
  CHECK(corners);
}

// ---------------------------------------------------------------------------
// Loader mutation fuzz.
//
// The refusals above are the failures somebody thought of; this is the space
// around them. A well-formed file is mutated at pseudo-random offsets and the
// loader must **either** refuse it **or** produce a module that renders finite,
// bounded audio. Never a crash, and never a sample the mixer's clip did not
// pass.
//
// **What this cannot do is prove there is no read past the buffer.** Nothing
// portable can: a read a few bytes off the end lands in other static storage
// and is invisible. What it catches is the reads that reach the output as a
// NaN or a wild sample, any write outside the frames a render was handed (the
// fences below), and anything that crashes outright. A sanitiser covers the
// rest, and `make stress` is where that lives.
//
// The seed is fixed and both it and the offsets are printed on a failure,
// because a fuzz failure nobody can reproduce is a fuzz failure nobody fixes.
// ---------------------------------------------------------------------------

static uint32_t
fuzz_random(uint32_t *state) {
  uint32_t x = *state;              // xorshift32; never seeded zero
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

// v1 plain, v1 with several patterns and a loop, v2 with FXPL, v2 with no
// directory at all, v2 with SYNP and a SYNTH instrument, v2 with the envelope
// fields set, and v2 with a wide plane and a macro table -- so every optional
// block and every instrument shape is somewhere in the corpus.
static const int kFuzzBases = 7;

static void
fuzz_build_base(int which) {
  if (which == 0) {
    Spec s;
    build(s);
  } else if (which == 1) {
    Spec s;
    s.channels = 4;
    s.rows = 8;
    s.patterns = 2;
    s.orders = 3;
    s.instruments = 2;
    s.sample_len = 40;
    s.loop_start = 8;
    s.loop_len = 16;
    build(s);
    set_cell(s, 2, 1, 13u, 1u, 0x0Au, 0x24u);
  } else if (which == 2) {
    SpecV2 s;
    build_v2(s);
  } else if (which == 3) {
    SpecV2 s;
    s.fx = false;
    s.instruments = 8;
    build_v2(s);
  } else if (which == 4) {
    syn_defaults();
    Bass b;
    syn_bass(b);
    SpecSyn s;
    build_syn(s);
  } else if (which == 5) {
    SpecEnv s;
    build_env(s);
  } else {
    // Both new blocks at once, and a geometry where a lane count and a channel
    // count are as far apart as the format lets them be -- so a mutation to the
    // prefix, to the macro count or to a target lands on something a reader
    // sizes other things against.
    SpecV2 s;
    s.fx_columns = 8;
    s.meta_columns = 4;
    s.macros = 4;
    build_v2(s);
  }
}

static uint8_t g_fuzz_base[1 << 16];
static uint8_t g_fuzz_mutant[1 << 16];

// A value no mix produces, on both sides of what `render_add` is handed.
static const double kFuzzFence = -12345.5;
static const int kFuzzFenceValues = 32;
static const int kFuzzFrames = 1024;
static double
    g_fuzz_out[kFuzzFenceValues + kFuzzFrames * 2 + kFuzzFenceValues];

static void
test_loader_fuzz() {
  printf("a mutated file is refused, or loads and renders sane audio\n");

  const int per_base = 1000;
  uint32_t seed = 0x9e3779b9u;
  int ok = 1;
  int reported = 0;
  int accepted = 0;

  for (int b = 0; b < kFuzzBases; ++b) {
    fuzz_build_base(b);
    const size_t size = g_size;
    memcpy(g_fuzz_base, g_bytes, size);

    // The unmutated file has to load, or every refusal below is the builder's
    // rather than the mutation's and the whole run means nothing.
    ntrk::Module clean;
    CHECK(ntrk::module_load(&clean, g_fuzz_base, size));

    for (int i = 0; i < per_base; ++i) {
      const uint32_t mutant_seed = seed;
      memcpy(g_fuzz_mutant, g_fuzz_base, size);

      size_t where[3] = {0u, 0u, 0u};
      const int n = 1 + (int) (fuzz_random(&seed) % 3u);
      for (int k = 0; k < n; ++k) {
        const uint32_t at = fuzz_random(&seed);
        const uint32_t v = fuzz_random(&seed);
        where[k] = (size_t) (at % (uint32_t) size);
        // A quarter each at the two ends of the byte rather than a uniform
        // draw: 0x00 and 0xff are where a count, a length and a flags field all
        // go wrong at once, and a uniform byte reaches each of them once in 256.
        g_fuzz_mutant[where[k]] = (v & 3u) == 0u   ? 0x00u
                                  : (v & 3u) == 1u ? 0xffu
                                                   : (uint8_t) ((v >> 8) & 0xffu);
      }

      ntrk::Module m;
      if (!ntrk::module_load(&m, g_fuzz_mutant, size))
        continue;
      ++accepted;

      for (int k = 0; k < kFuzzFenceValues; ++k) {
        g_fuzz_out[k] = kFuzzFence;
        g_fuzz_out[kFuzzFenceValues + kFuzzFrames * 2 + k] = kFuzzFence;
      }
      double *out = g_fuzz_out + kFuzzFenceValues;
      for (int k = 0; k < kFuzzFrames * 2; ++k)
        out[k] = 0.0;

      ntrk::Player p;
      ntrk::player_start(&p, &m);
      ntrk::render_add(&p, out, kFuzzFrames, 2, 22050.f);

      bool sane = buffer_finite(out, kFuzzFrames * 2);
      for (int k = 0; k < kFuzzFenceValues; ++k)
        if (g_fuzz_out[k] != kFuzzFence ||
            g_fuzz_out[kFuzzFenceValues + kFuzzFrames * 2 + k] != kFuzzFence)
          sane = false;

      if (!sane) {
        ok = 0;
        if (reported++ < 4)
          printf("  base %d, seed %08x, %d byte(s) at %lu %lu %lu\n", b,
                 (unsigned) mutant_seed, n, (unsigned long) where[0],
                 (unsigned long) where[1], (unsigned long) where[2]);
      }
    }
  }
  CHECK(ok);

  // **The vacuity guard, and this one is not a formality.** A loader that
  // refused everything would pass the check above outright, and so would a
  // mutator that only ever wrote a byte back over itself.
  CHECK(accepted > kFuzzBases * per_base / 20);
}

// ---------------------------------------------------------------------------
// Round trip as a property.
//
// One hand-built example says the writer and the reader agree about one file.
// This walks the space they have to agree about: both versions, every
// instrument type, the envelope, filter and transpose fields, both optional
// blocks, several channel counts and both note ranges. Each shape is saved,
// loaded back and compared twice -- field by field, and then render against
// render, which has to be bit-identical. A field comparison alone would miss a
// pointer aimed at the wrong sample; a render alone would miss a field nothing
// audible reads yet, which is most of what v2 added.
//
// **Assembled as a `Module` rather than as a file**, which is what an editor
// does: `order`, `patterns` and `fx` are const pointers to memory the caller
// owns. The loader's own byte handling is tested above.
// ---------------------------------------------------------------------------

static const int kRtRows = 4;
static const int kRtPatterns = 2;
static const int kRtOrders = 3;
static const int kRtCells = kRtPatterns * kRtRows * ntrk::kMaxChannels;
// The plane is lanes wide, not channels wide, and the widest geometry the
// format permits is every channel at four columns plus four meta lanes.
static const int kRtPlaneCells =
    kRtPatterns * kRtRows *
    (ntrk::kMaxChannels * ntrk::kMaxFxColumns + ntrk::kMaxMetaColumns);

static uint8_t g_rt_order[kRtOrders] = {0u, 1u, 0u};
static ntrk::Note g_rt_pat[kRtCells];
static ntrk::FxCell g_rt_fx[kRtPlaneCells];
// 32 frames of PCM8, or 32 of PCM16 read as little-endian pairs. One array for
// both, so a type change is a stride change and nothing else.
static int8_t g_rt_wave[64];

struct RtSpec {
  int channels = 4;
  int instruments = 4;
  int note_max = 96;
  uint8_t type = (uint8_t) ntrk::InstrumentType::kPcm8;
  uint8_t flags = 0;
  int8_t transpose = 0;
  bool fx = false;
  uint8_t note = 25u;
  int fx_columns = 1;
  int meta_columns = 0;
  int macros = 0;
};

static void
rt_build(ntrk::Module *m, const RtSpec &s) {
  for (int i = 0; i < 64; ++i)
    g_rt_wave[i] = (int8_t) ((i % 8) < 4 ? 100 : -100);
  for (int i = 0; i < kRtCells; ++i)
    g_rt_pat[i] = ntrk::Note();
  for (int i = 0; i < kRtPlaneCells; ++i)
    g_rt_fx[i] = ntrk::FxCell();

  // Row 0 channel 0 plays the instrument under test; row 1 channel 1 plays a
  // plain PCM8 one beside it, so a render is not one voice's worth of evidence.
  g_rt_pat[0].note = s.note;
  g_rt_pat[0].instrument = 1u;
  if (s.channels > 1) {
    g_rt_pat[(size_t) s.channels + 1u].note = 25u;
    g_rt_pat[(size_t) s.channels + 1u].instrument = 2u;
  }
  // Both player-side plane commands, so a plane that survived the save as bytes
  // but not as meaning still shows up in the render comparison. The row stride
  // is lanes rather than channels, which is what makes the second one land on
  // row 1 whatever geometry this shape has.
  const size_t lanes =
      (size_t) s.channels * (size_t) s.fx_columns + (size_t) s.meta_columns;
  g_rt_fx[0].cmd = 0x30u;                      // accent
  g_rt_fx[0].param = 200u;
  g_rt_fx[lanes].cmd = 0x31u;                  // glide, row 1 channel 0
  g_rt_fx[lanes].param = 4u;
  // A meta cell naming macro 1, one-based, and one naming a macro that is not
  // in the table -- which is ignored where it is used rather than refused here.
  if (s.meta_columns > 0) {
    const size_t meta = (size_t) s.channels * (size_t) s.fx_columns;
    g_rt_fx[meta].cmd = 1u;
    g_rt_fx[meta].param = 96u;
    g_rt_fx[lanes + meta].cmd = 250u;
    g_rt_fx[lanes + meta].param = 8u;
  }

  *m = ntrk::Module();
  m->version = 2;
  m->channels = s.channels;
  m->rows = kRtRows;
  m->speed = 6;
  m->bpm = 125;
  m->order_count = kRtOrders;
  m->pattern_count = kRtPatterns;
  m->instrument_count = s.instruments;
  m->restart = 1;
  m->note_max = s.note_max;
  m->order = g_rt_order;
  m->patterns = g_rt_pat;
  m->fx = s.fx ? g_rt_fx : nullptr;
  m->fx_columns = s.fx ? s.fx_columns : 1;
  m->meta_columns = s.fx ? s.meta_columns : 0;

  // Distinct per macro and per slot, so a writer that put one record's fields
  // into another's would show up in the field comparison rather than in
  // nothing at all. Every slot past `target_count` stays zero, which is what
  // the loader insists on.
  m->macro_count = s.macros;
  for (int i = 0; i < s.macros; ++i) {
    ntrk::Macro &mac = m->macros[i];
    mac.target_count = (uint8_t) (i % (ntrk::kMaxMacroTargets + 1));
    mac.flags = (uint8_t) ((i & 1) != 0 ? ntrk::kMacroDelta : 0u);
    for (int k = 0; k < (int) mac.target_count; ++k) {
      mac.targets[k].target = (uint8_t) ((i + k) % ntrk::kFxplValueCount);
      mac.targets[k].scope = k == 0 ? (uint8_t) (i % 16)
                             : k == 1 ? ntrk::kMacroScopeSelf
                                      : ntrk::kMacroScopeAll;
      // Both signs, and both ends of 8.8, so a byte order or a sign extension
      // that went wrong has somewhere to show.
      mac.targets[k].scale = (int16_t) (256 - i * 37 - k * 11);
      mac.targets[k].offset = (int16_t) (k * 64 - 128 - i);
    }
  }

  for (int i = 0; i < s.instruments; ++i) {
    ntrk::Instrument &ins = m->instruments[i];
    ins.data = g_rt_wave;
    ins.length = 32u;
    ins.loop_start = 0u;
    ins.loop_len = 32u;
    // Distinct per instrument so a save that wrote one entry's fields into
    // another's would show, and wrapped inside the ranges both halves check:
    // volume stops at 64 and finetune at 7.
    ins.volume = (uint8_t) (48 + (i % 16));
    ins.finetune = (int8_t) (i % 8);
  }

  ntrk::Instrument &head = m->instruments[0];
  head.type = s.type;
  head.flags = s.flags;
  head.transpose = s.transpose;
  if ((s.flags & ntrk::kInstrumentEnvelope) != 0u) {
    head.env_attack_ms = 10u;
    head.env_decay_ms = 250u;
    head.env_release_ms = 500u;
    head.env_sustain = 48u;
  }
  if ((s.flags & ntrk::kInstrumentFilter) != 0u) {
    head.filter_cutoff_hz = 1200u;
    head.filter_res = 128u;
  }
  if (s.type == (uint8_t) ntrk::InstrumentType::kPcm16) {
    head.bits = 16u;                           // derived by the loader; matched
  } else if (s.type == (uint8_t) ntrk::InstrumentType::kWaveBuiltin) {
    // The shape is generated at load, so the module has to hold what the loader
    // will produce rather than what an entry could name.
    head.wave_index = 2u;
    head.data = ntrk::builtin_wave(2);
    head.length = (uint32_t) ntrk::kBuiltinWaveFrames;
    head.loop_start = 0u;
    head.loop_len = (uint32_t) ntrk::kBuiltinWaveFrames;
  } else if (s.type == (uint8_t) ntrk::InstrumentType::kSynth) {
    head.data = nullptr;                       // a synth owns no blob at all
    head.length = 0u;
    head.loop_start = 0u;
    head.loop_len = 0u;
    head.synth_voice = 4u;                     // the bass, so both halves of a
    head.synth_tune = 128u;                    // SYNP record carry something
    head.synth_decay = 40u;
    head.synth_sweep = 160u;
    head.synth_tone = 128u;
    head.synth_noise = 60u;
    head.synth_noise_decay = 40u;
    head.synth_drive = 80u;
    head.synth_cutoff = 190u;
    head.synth_reso = 210u;
    head.synth_env_mod = 120u;
    head.synth_accent = 90u;
    head.synth_dist = 3u;
    head.synth_dist_mix = 180u;
    head.synth_wave = 1u;
  }
}

// Every field a `Module` carries, including the ones nothing audible reads yet.
// **`data` is compared by content and never by pointer**: the two modules point
// at different memory by construction, which is the whole reason the save is
// worth making.
static bool
rt_same(const ntrk::Module &a, const ntrk::Module &b) {
  if (a.version != b.version || a.channels != b.channels || a.rows != b.rows ||
      a.speed != b.speed || a.bpm != b.bpm || a.order_count != b.order_count ||
      a.pattern_count != b.pattern_count ||
      a.instrument_count != b.instrument_count || a.restart != b.restart ||
      a.note_max != b.note_max || a.fx_columns != b.fx_columns ||
      a.meta_columns != b.meta_columns || a.macro_count != b.macro_count)
    return false;
  if ((a.fx == nullptr) != (b.fx == nullptr))
    return false;

  for (int i = 0; i < a.order_count; ++i)
    if (a.order[i] != b.order[i])
      return false;

  const int cells = a.pattern_count * a.rows * a.channels;
  for (int i = 0; i < cells; ++i)
    if (a.patterns[i].note != b.patterns[i].note ||
        a.patterns[i].instrument != b.patterns[i].instrument ||
        a.patterns[i].effect != b.patterns[i].effect ||
        a.patterns[i].param != b.patterns[i].param)
      return false;

  // The plane is lanes wide, so it is walked separately from the patterns
  // rather than in the same loop -- the two are the same length only when a
  // module has one effect column and no meta lanes.
  if (a.fx != nullptr) {
    const int plane = a.pattern_count * a.rows * ntrk::module_lanes(&a);
    for (int i = 0; i < plane; ++i)
      if (a.fx[i].cmd != b.fx[i].cmd || a.fx[i].param != b.fx[i].param)
        return false;
  }

  // Every slot, not only the ones a list uses: the writer zeroes the rest and
  // the loader refuses a slot that is not zero, so the two have to agree about
  // all eight or the canonical form is not canonical.
  for (int i = 0; i < a.macro_count; ++i) {
    if (a.macros[i].target_count != b.macros[i].target_count ||
        a.macros[i].flags != b.macros[i].flags)
      return false;
    for (int k = 0; k < ntrk::kMaxMacroTargets; ++k)
      if (a.macros[i].targets[k].target != b.macros[i].targets[k].target ||
          a.macros[i].targets[k].scope != b.macros[i].targets[k].scope ||
          a.macros[i].targets[k].scale != b.macros[i].targets[k].scale ||
          a.macros[i].targets[k].offset != b.macros[i].targets[k].offset)
        return false;
  }

  for (int i = 0; i < a.instrument_count; ++i) {
    const ntrk::Instrument &x = a.instruments[i];
    const ntrk::Instrument &y = b.instruments[i];
    if (x.length != y.length || x.loop_start != y.loop_start ||
        x.loop_len != y.loop_len || x.volume != y.volume ||
        x.finetune != y.finetune || x.type != y.type || x.bits != y.bits ||
        x.flags != y.flags || x.transpose != y.transpose ||
        x.env_attack_ms != y.env_attack_ms ||
        x.env_decay_ms != y.env_decay_ms ||
        x.env_release_ms != y.env_release_ms ||
        x.env_sustain != y.env_sustain ||
        x.filter_cutoff_hz != y.filter_cutoff_hz ||
        x.filter_res != y.filter_res || x.wave_index != y.wave_index ||
        x.synth_voice != y.synth_voice || x.synth_tune != y.synth_tune ||
        x.synth_decay != y.synth_decay || x.synth_sweep != y.synth_sweep ||
        x.synth_tone != y.synth_tone || x.synth_noise != y.synth_noise ||
        x.synth_noise_decay != y.synth_noise_decay ||
        x.synth_drive != y.synth_drive || x.synth_cutoff != y.synth_cutoff ||
        x.synth_reso != y.synth_reso || x.synth_env_mod != y.synth_env_mod ||
        x.synth_accent != y.synth_accent || x.synth_dist != y.synth_dist ||
        x.synth_dist_mix != y.synth_dist_mix || x.synth_wave != y.synth_wave)
      return false;
    const size_t bytes = (size_t) x.length * (size_t) (x.bits / 8u);
    for (size_t k = 0; k < bytes; ++k)
      if (((const uint8_t *) (const void *) x.data)[k] !=
          ((const uint8_t *) (const void *) y.data)[k])
        return false;
  }
  return true;
}

static bool
rt_same_render(const ntrk::Module *a, const ntrk::Module *b) {
  const int frames = 1024;
  ntrk::Player p;
  ntrk::player_start(&p, a);
  render_at(&p, frames, 22050.f);
  memcpy(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u);
  ntrk::player_start(&p, b);
  render_at(&p, frames, 22050.f);
  return memcmp(g_first, g_buffer, sizeof(double) * (size_t) frames * 2u) == 0;
}

static void
test_round_trip_property() {
  printf("every shape of module survives a save, as fields and as samples\n");

  const uint8_t types[5] = {
      (uint8_t) ntrk::InstrumentType::kPcm8,
      (uint8_t) ntrk::InstrumentType::kPcm16,
      (uint8_t) ntrk::InstrumentType::kWaveData,
      (uint8_t) ntrk::InstrumentType::kWaveBuiltin,
      (uint8_t) ntrk::InstrumentType::kSynth,
  };
  const int channels[3] = {1, 4, 16};
  const uint8_t extra[4] = {0u, ntrk::kInstrumentEnvelope, ntrk::kInstrumentFilter,
                            (uint8_t) (ntrk::kInstrumentEnvelope |
                                       ntrk::kInstrumentFilter)};

  int saved = 1, reloaded = 1, fields = 1, audio = 1;
  int reported = 0;
  int cases = 0;

  for (int t = 0; t < 5; ++t)
    for (int c = 0; c < 3; ++c)
      for (int nm = 0; nm < 2; ++nm)
        for (int fx = 0; fx < 2; ++fx)
          for (int e = 0; e < 4; ++e) {
            RtSpec s;
            s.type = types[t];
            s.channels = channels[c];
            s.note_max = nm == 0 ? 36 : ntrk::kMaxNote;
            s.note = nm == 0 ? 25u : 60u;
            s.fx = fx != 0;
            s.flags = extra[e];
            s.transpose = (int8_t) (e == 3 ? -12 : 0);
            ++cases;

            ntrk::Module a;
            rt_build(&a, s);

            size_t need = 0;
            size_t wrote = 0;
            if (!ntrk::module_save(&a, nullptr, 0, &need) ||
                !ntrk::module_save(&a, g_saved, sizeof g_saved, &wrote) ||
                wrote != need) {
              saved = 0;
              continue;
            }

            ntrk::Module b;
            if (!ntrk::module_load(&b, g_saved, wrote)) {
              reloaded = 0;
              continue;
            }
            if (!rt_same(a, b))
              fields = 0;
            else if (rt_same_render(&a, &b))
              continue;
            else
              audio = 0;

            if (reported++ < 6)
              printf("  type %u, %d channels, note_max %d, fx %d, flags %u\n",
                     (unsigned) s.type, s.channels, s.note_max, fx,
                     (unsigned) s.flags);
          }
  // The plane's own dimensions, held apart from the grid above rather than
  // multiplied into it: that one varies instruments, this one varies the shape
  // of the plane, and the two do not interact. Sixteen channels throughout, so
  // a lane count is as far from a channel count as the format allows.
  const int macro_counts[3] = {0, 1, ntrk::kMaxMacros};
  for (int fc = 1; fc <= ntrk::kMaxFxColumns; ++fc)
    for (int mc = 0; mc <= ntrk::kMaxMetaColumns; ++mc)
      for (int mk = 0; mk < 3; ++mk) {
        // A meta lane with no macro table names nothing, and neither half of
        // the library will write or read one.
        if (mc > 0 && macro_counts[mk] == 0)
          continue;
        RtSpec s;
        s.channels = 16;
        s.fx = true;
        s.fx_columns = fc;
        s.meta_columns = mc;
        s.macros = macro_counts[mk];
        ++cases;

        ntrk::Module a;
        rt_build(&a, s);
        size_t wrote = 0;
        if (!ntrk::module_save(&a, g_saved, sizeof g_saved, &wrote)) {
          saved = 0;
          continue;
        }
        ntrk::Module b;
        if (!ntrk::module_load(&b, g_saved, wrote)) {
          reloaded = 0;
          continue;
        }
        if (!rt_same(a, b))
          fields = 0;
        else if (!rt_same_render(&a, &b))
          audio = 0;
      }

  // A macro table and no plane at all, which is the state an editor is in for
  // most of the time one is open: the macros are written, nothing invokes them
  // yet, and the file has a MACR block and no FXPL.
  for (int mk = 1; mk < 3; ++mk) {
    RtSpec s;
    s.fx = false;
    s.macros = macro_counts[mk];
    ++cases;

    ntrk::Module a;
    rt_build(&a, s);
    size_t wrote = 0;
    if (!ntrk::module_save(&a, g_saved, sizeof g_saved, &wrote)) {
      saved = 0;
      continue;
    }
    ntrk::Module b;
    if (!ntrk::module_load(&b, g_saved, wrote)) {
      reloaded = 0;
      continue;
    }
    if (!rt_same(a, b) || b.fx != nullptr)
      fields = 0;
    else if (!rt_same_render(&a, &b))
      audio = 0;
  }

  // The plainest shape the grid above cannot reach: three octaves, PCM8, no
  // plane and no blocks, at three widths and three table sizes. That is what
  // every imported `.mod` is, so it is worth sweeping on its own rather than
  // trusting the fully-loaded cases to cover it.
  const int plain_channels[3] = {1, 4, 8};
  const int plain_instruments[3] = {1, 4, 32};
  for (int c = 0; c < 3; ++c)
    for (int n = 0; n < 3; ++n) {
      RtSpec s;
      s.channels = plain_channels[c];
      s.instruments = plain_instruments[n];
      s.note_max = 36;
      ++cases;

      ntrk::Module a;
      rt_build(&a, s);
      size_t wrote = 0;
      if (!ntrk::module_save(&a, g_saved, sizeof g_saved, &wrote)) {
        saved = 0;
        continue;
      }
      ntrk::Module b;
      if (!ntrk::module_load(&b, g_saved, wrote)) {
        reloaded = 0;
        continue;
      }
      if (!rt_same(a, b))
        fields = 0;
      else if (!rt_same_render(&a, &b))
        audio = 0;
    }

  // Literal rather than derived, because this is the tripwire on a loop that
  // silently stopped iterating -- a count computed from the same bounds the
  // loops use would agree with a broken loop. The geometry sweep contributes
  // `kMaxFxColumns * (3 + 4 * 2)`: three macro counts at no meta lanes, two at
  // each of the four meta widths. It was 295 at four columns.
  CHECK(cases == 339);
  CHECK(saved);
  CHECK(reloaded);
  CHECK(fields);
  CHECK(audio);

  // **A file loaded and saved is the file that came in, byte for byte**, and
  // across shapes rather than for one of them. One instrument each, because two
  // sharing a blob offset are written out twice -- which `test_save_round_trip`
  // pins as a decision and is not something a round trip can ask for.
  for (int k = 0; k < 6; ++k) {
    Spec s;
    if (k == 1) {
      s.channels = 8;
      s.rows = 64;
    } else if (k == 2) {
      s.rows = 16;
      s.patterns = 4;
      s.orders = 8;
    } else if (k == 3) {
      s.sample_len = 40;
      s.loop_start = 8;
      s.loop_len = 16;
    } else if (k == 4) {
      s.volume = 17u;
      s.finetune = -5;
      s.speed = 31;
      s.bpm = 32;
    } else if (k == 5) {
      s.channels = 3;
      s.rows = 1;
      s.orders = 2;
      s.patterns = 2;
    }
    build(s);
    ntrk::Module a;
    CHECK(ntrk::module_load(&a, g_bytes, g_size));
    size_t wrote = 0;
    CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &wrote));
    CHECK(wrote == g_size && memcmp(g_saved, g_bytes, wrote) == 0);
  }

  // **A version-1 file is refused, and that is the whole reason the version
  // byte is 2.** A version-1 module was the same magic with a 20-byte
  // instrument entry, no declared blob length and no block directory. Renumber
  // this format to 1 and every one of those files passes the version check and
  // is then read at the wrong stride -- garbage that loads. Refusing them is
  // the difference between "too old" and silently wrong, so it is checked
  // against real version-1 bytes rather than only against an unknown number.
  {
    Spec s;
    build(s);
    ntrk::Module m;

    // The exact shape a version-1 writer produced: version 1, entries 20 bytes
    // apart, the blob running to the end of the file, and bytes 22..31 unused.
    const size_t order_bytes = (size_t) s.orders;
    const size_t v1_instrument_bytes = (size_t) s.instruments * 20u;
    const size_t pattern_bytes =
        (size_t) s.patterns * (size_t) s.rows * (size_t) s.channels * 4u;
    const size_t v1_size = 32u + order_bytes + v1_instrument_bytes +
                           pattern_bytes + (size_t) s.sample_len;
    static uint8_t v1[4096];
    memset(v1, 0, v1_size);
    memcpy(v1, "NTRK", 4);
    v1[4] = 1u;
    for (size_t i = 6; i < 22; ++i)             // the counts are unchanged
      v1[i] = g_bytes[i];
    for (size_t i = 0; i < order_bytes; ++i)
      v1[32 + i] = g_bytes[32 + i];
    for (int i = 0; i < s.instruments; ++i)
      memcpy(v1 + 32 + order_bytes + (size_t) i * 20u,
             g_bytes + 32 + order_bytes + (size_t) i * 32u, 20u);
    memcpy(v1 + 32 + order_bytes + v1_instrument_bytes,
           g_bytes + patterns_at(s), pattern_bytes + (size_t) s.sample_len);
    CHECK(!ntrk::module_load(&m, v1, v1_size));

    // And any other number, so a later format cannot be read by this one
    // either.
    build(s);
    put_u16(4, 0);
    CHECK(!ntrk::module_load(&m, g_bytes, g_size));
    build(s);
    put_u16(4, 3);
    CHECK(!ntrk::module_load(&m, g_bytes, g_size));

    // A note above the module's own `note_max`, reached through the pattern
    // block rather than through an instrument.
    //
    // **This case used to load and then be unsaveable**, which is how it was
    // found: the loader did not range-check a note, so a file carrying note 60
    // in a three-octave module came in happily and `module_save` then refused
    // it. Loadable and unsaveable is a file only one half of the format
    // accepts, so the judgement lives where the file is still being judged.
    build(s);
    set_cell(s, 0, 0, 60u, 1u, 0u, 0u);
    CHECK(!ntrk::module_load(&m, g_bytes, g_size));

    // Note-off is the one value above `note_max` that is not an octave, so it
    // is legal in a three-octave module and survives a round trip. Refusing it
    // would make a tune that uses `^^^` unsaveable.
    build(s);
    set_cell(s, 0, 0, (uint8_t) ntrk::kNoteOff, 1u, 0u, 0u);
    CHECK(ntrk::module_load(&m, g_bytes, g_size));
    size_t wrote = 0;
    CHECK(ntrk::module_save(&m, g_saved, sizeof g_saved, &wrote));
    CHECK(wrote == g_size && memcmp(g_saved, g_bytes, wrote) == 0);
  }
}

// ---------------------------------------------------------------------------
// The pretty-printer's half that lives in ntrk.h: ProTracker's own column and
// the plane range the player owns. The mixer's half is in test_ntrk_mix.cc,
// because resolving a slot parameter needs a `Mixer` this binary does not link.
// ---------------------------------------------------------------------------

// Every describe function has the same contract, so the buffer half of it is
// checked once here rather than at each call: truncate, NUL-terminate, never
// touch a byte past `cap`, and write nothing at all for a `cap` of zero.
static void
check_bounded(size_t (*fn)(uint8_t, uint8_t, char *, size_t), uint8_t a,
              uint8_t b) {
  char full[128];
  const size_t want = fn(a, b, full, sizeof full);
  CHECK(want > 0);
  CHECK(want < sizeof full - 1);   // nothing here is anywhere near 128

  // One CHECK for the sweep rather than one per size: a bounds bug that fires
  // at every cap would otherwise bury the run in identical failures.
  bool ok = true;
  for (size_t cap = 0; cap <= want + 2; ++cap) {
    char buf[160];
    memset(buf, '#', sizeof buf);
    const size_t n = fn(a, b, buf, cap);
    if (cap == 0) {
      // Not even the terminator: there is nowhere to put it.
      ok = ok && n == 0 && buf[0] == '#';
      continue;
    }
    ok = ok && n == (want < cap - 1 ? want : cap - 1) && buf[n] == '\0' &&
         buf[cap] == '#' &&              // the byte past the buffer is untouched
         strncmp(buf, full, n) == 0;
  }
  CHECK(ok);
}


static void
test_mnemonics() {
  printf("every command has a three-letter name, and E resolves its sub-nibble\n");
  char m[4];

  ntrk::note_fx_mnemonic(0x0, 0x00, m);
  CHECK(strcmp(m, "ARP") == 0);
  ntrk::note_fx_mnemonic(0x3, 0x00, m);
  CHECK(strcmp(m, "GLI") == 0);
  ntrk::note_fx_mnemonic(0xF, 0x06, m);
  CHECK(strcmp(m, "SPD") == 0);

  // E is named by what it does, not by the container it arrives in.
  ntrk::note_fx_mnemonic(0xE, 0xC3, m);
  CHECK(strcmp(m, "CUT") == 0);
  ntrk::note_fx_mnemonic(0xE, 0x60, m);
  CHECK(strcmp(m, "LOP") == 0);
  // 8xx and E8x set the same thing, so they carry the same name on purpose.
  ntrk::note_fx_mnemonic(0x8, 0x80, m);
  CHECK(strcmp(m, "PAN") == 0);
  ntrk::note_fx_mnemonic(0xE, 0x80, m);
  CHECK(strcmp(m, "PAN") == 0);

  // Exactly three characters for every effect at every parameter -- the grid
  // reserves three columns and a shorter name would leave the cell ragged.
  bool all_three = true;
  for (int e = 0; e < 16; ++e)
    for (int p = 0; p < 256; ++p) {
      m[0] = m[1] = m[2] = m[3] = '#';
      ntrk::note_fx_mnemonic((uint8_t) e, (uint8_t) p, m);
      if (strlen(m) != 3)
        all_three = false;
    }
  CHECK(all_three);
}

static void
test_describe_protracker() {
  printf("ProTracker's effect column describes itself\n");
  char b[128];

  // Both levels, on the cell the header's own example uses.
  CHECK(ntrk::note_fx_repr(0xC, 0x40, b, sizeof b) == 3);
  CHECK(strcmp(b, "C40") == 0);
  ntrk::note_fx_describe(0xC, 0x40, b, sizeof b);
  CHECK(strcmp(b, "Set volume 64") == 0);

  ntrk::note_fx_describe(0x0, 0x37, b, sizeof b);
  CHECK(strcmp(b, "Arpeggio +3 +7") == 0);
  // An empty cell and an arpeggio with no parameter are the same silence.
  ntrk::note_fx_describe(0x0, 0x00, b, sizeof b);
  CHECK(strcmp(b, "None") == 0);
  // The parameter is decimal, which is the trap worth a status bar: 0x10 is
  // row ten, not row sixteen.
  ntrk::note_fx_describe(0xD, 0x10, b, sizeof b);
  CHECK(strcmp(b, "Pattern break to row 10") == 0);
  // One effect doing two jobs, and the split is at 32.
  ntrk::note_fx_describe(0xF, 0x06, b, sizeof b);
  CHECK(strcmp(b, "Set speed 6 ticks/row") == 0);
  ntrk::note_fx_describe(0xF, 0x7D, b, sizeof b);
  CHECK(strcmp(b, "Set tempo 125 BPM") == 0);
  // Up wins over down when both nibbles are set, exactly as `volume_slide` has
  // it — a description that summed them would be describing another player.
  ntrk::note_fx_describe(0xA, 0x34, b, sizeof b);
  CHECK(strcmp(b, "Volume slide, +3/tick") == 0);
  ntrk::note_fx_describe(0xA, 0x04, b, sizeof b);
  CHECK(strcmp(b, "Volume slide, -4/tick") == 0);
  // Panning reads as a position, not as a byte, and both hard ends are named.
  ntrk::note_fx_describe(0x8, 0x80, b, sizeof b);
  CHECK(strcmp(b, "Set panning centre") == 0);
  ntrk::note_fx_describe(0x8, 0x00, b, sizeof b);
  CHECK(strcmp(b, "Set panning hard left") == 0);
  ntrk::note_fx_describe(0x8, 0xFF, b, sizeof b);
  CHECK(strcmp(b, "Set panning hard right") == 0);
  ntrk::note_fx_describe(0x8, 0xC0, b, sizeof b);
  CHECK(strcmp(b, "Set panning R50%") == 0);
  // E8 expands its nibble by 17, so its ends coincide with 8xx's.
  ntrk::note_fx_describe(0xE, 0x80, b, sizeof b);
  CHECK(strcmp(b, "Coarse panning hard left") == 0);
  ntrk::note_fx_describe(0xE, 0x8F, b, sizeof b);
  CHECK(strcmp(b, "Coarse panning hard right") == 0);
  // The extended set is a second table and reads as one.
  ntrk::note_fx_describe(0xE, 0xC3, b, sizeof b);
  CHECK(strcmp(b, "Note cut at tick 3") == 0);
  ntrk::note_fx_describe(0xE, 0x60, b, sizeof b);
  CHECK(strcmp(b, "Set pattern loop point") == 0);

  // **Every command number in the table, at every parameter.** A description
  // that is empty, that overruns, or that is not the same string at every
  // buffer size is the failure this loop exists to catch.
  bool all = true;
  for (int e = 0; e <= 15; ++e) {
    for (int p = 0; p <= 255; ++p) {
      ntrk::note_fx_describe((uint8_t) e, (uint8_t) p, b, sizeof b);
      all = all && b[0] != '\0' && strlen(b) < sizeof b - 1;
      ntrk::note_fx_repr((uint8_t) e, (uint8_t) p, b, sizeof b);
      all = all && strlen(b) == 3;
    }
  }
  CHECK(all);
  // The bounded contract, on a long description and a short one.
  check_bounded(ntrk::note_fx_describe, 0x5, 0x30);
  check_bounded(ntrk::note_fx_describe, 0x0, 0x00);
  check_bounded(ntrk::note_fx_repr, 0xE, 0xC3);
}

static void
test_describe_player_range() {
  printf("the plane range the player owns describes itself\n");
  char b[128];

  CHECK(ntrk::fxpl_repr(0x4A, 0x80, b, sizeof b) == 5);
  CHECK(strcmp(b, "4A 80") == 0);

  ntrk::fxpl_player_describe(ntrk::kFxplAccent, 0x80, b, sizeof b);
  CHECK(strcmp(b, "Accent -> 0.50") == 0);
  ntrk::fxpl_player_describe(ntrk::kFxplAccent, 0xff, b, sizeof b);
  CHECK(strcmp(b, "Accent -> 1.00") == 0);
  ntrk::fxpl_player_describe(ntrk::kFxplSlide, 10u, b, sizeof b);
  CHECK(strcmp(b, "Slide, 10 ticks") == 0);
  // Zero is identical to no command at all, and saying "0 ticks" would hide it.
  ntrk::fxpl_player_describe(ntrk::kFxplSlide, 0u, b, sizeof b);
  CHECK(strcmp(b, "Slide off") == 0);
  ntrk::fxpl_player_describe(0x3fu, 0u, b, sizeof b);
  CHECK(strcmp(b, "Reserved (0x3F)") == 0);

  // **Anything outside the player's range writes nothing and returns zero**,
  // which is what lets the mixer's entry point chain this rather than repeat
  // it. A meaning invented here is a meaning the two halves would disagree on.
  bool split = true;
  for (int c = 0; c <= 255; ++c) {
    memset(b, '#', sizeof b);
    const size_t n = ntrk::fxpl_player_describe((uint8_t) c, 0x40u, b, sizeof b);
    const bool mine = c >= 0x30 && c < 0x40;
    split = split && (mine ? (n > 0 && b[0] != '\0') : (n == 0 && b[0] == '\0'));
  }
  CHECK(split);

  check_bounded(ntrk::fxpl_player_describe, ntrk::kFxplSlide, 10u);
  check_bounded(ntrk::fxpl_repr, 0x4A, 0x80);
}

// The directory follows every positional block, so it is walked to rather than
// guessed at.
static size_t
tune_directory_at(const ntrk::Module &m, const uint8_t *bytes) {
  (void) bytes;
  size_t at = (size_t) ntrk::kHeaderBytes + (size_t) m.order_count +
              (size_t) m.instrument_count * (size_t) ntrk::kInstrumentBytes +
              (size_t) m.pattern_count * (size_t) m.rows * (size_t) m.channels * 4u;
  for (int i = 0; i < m.instrument_count; ++i)
    at += (size_t) m.instruments[i].length * (m.instruments[i].bits == 16 ? 2u : 1u);
  return at;
}

// ---- the module field table ---------------------------------------------------

static void
test_module_param_table() {
  printf("every module field names itself, and one written past its range is refused\n");

  CHECK(ntrk::module_param_count() == (int) ntrk::ModParam::kCount);
  CHECK(ntrk::module_param_at(-1) == NULL);
  CHECK(ntrk::module_param_at(ntrk::module_param_count()) == NULL);
  for (int id = 0; id < ntrk::module_param_count(); ++id) {
    const ntrk::ModParamInfo *info = ntrk::module_param_at(id);
    CHECK(info != NULL && info->name != NULL && info->name[0] != '\0');
    CHECK(info->lo <= info->hi);
  }

  RtSpec s;
  s.channels = 4;
  size_t need = 0;

  // A restart names an entry of THIS order list, so its ceiling follows the
  // list rather than sitting at a constant.
  {
    ntrk::Module m;
    rt_build(&m, s);
    int lo = 0, hi = 0;
    ntrk::module_param_range(&m, (int) ntrk::ModParam::kRestart, &lo, &hi);
    CHECK(lo == 0);
    CHECK(hi == m.order_count - 1);
    // ...and with no module there is nothing to measure, which answers 0 rather
    // than the file-side constant it looks like.
    ntrk::module_param_range(NULL, (int) ntrk::ModParam::kRestart, &lo, &hi);
    CHECK(hi == 0);
  }

  // **The negative control.** Thirteen rows, each written past its bound by
  // hand and each required to be refused at the save -- because a table whose
  // ranges were all far too wide would pass every other check here.
#define MOD_REFUSED(setup)                                                     \
  do {                                                                         \
    ntrk::Module m;                                                            \
    rt_build(&m, s);                                                           \
    CHECK(ntrk::module_save(&m, NULL, 0, &need));   /* sound before the poke */\
    setup;                                                                     \
    CHECK(!ntrk::module_fields_valid(&m));                                     \
    CHECK(!ntrk::module_save(&m, NULL, 0, &need));                             \
  } while (0)

  MOD_REFUSED(m.channels = 0);
  MOD_REFUSED(m.channels = ntrk::kMaxChannels + 1);
  MOD_REFUSED(m.rows = 0);
  MOD_REFUSED(m.rows = ntrk::kMaxRows + 1);
  MOD_REFUSED(m.speed = 0);
  MOD_REFUSED(m.speed = 32);
  MOD_REFUSED(m.bpm = 31);
  MOD_REFUSED(m.bpm = 256);
  MOD_REFUSED(m.order_count = 0);
  MOD_REFUSED(m.order_count = 257);
  MOD_REFUSED(m.pattern_count = 0);
  MOD_REFUSED(m.pattern_count = ntrk::kMaxPatterns + 1);
  MOD_REFUSED(m.instrument_count = -1);
  MOD_REFUSED(m.instrument_count = ntrk::kMaxInstruments + 1);
  MOD_REFUSED(m.restart = -1);
  MOD_REFUSED(m.restart = m.order_count);
  MOD_REFUSED(m.fx_columns = 0);
  MOD_REFUSED(m.fx_columns = ntrk::kMaxFxColumns + 1);
  MOD_REFUSED(m.meta_columns = -1);
  MOD_REFUSED(m.meta_columns = ntrk::kMaxMetaColumns + 1);
  MOD_REFUSED(m.rows_per_beat = 0);
  MOD_REFUSED(m.rows_per_beat = 256);
  MOD_REFUSED(m.rows_per_bar = 0);
  MOD_REFUSED(m.rows_per_bar = 256);
  MOD_REFUSED(m.swing = -1);
  MOD_REFUSED(m.swing = ntrk::kSwingMax + 1);

#undef MOD_REFUSED

  // The relation the table cannot express, still checked where it was: a bar
  // that is not a whole number of beats is two fields disagreeing, which no
  // per-field bound can see.
  {
    ntrk::Module m;
    rt_build(&m, s);
    m.rows_per_beat = 6;
    m.rows_per_bar = 16;
    CHECK(ntrk::module_fields_valid(&m));      // both in range...
    CHECK(!ntrk::module_save(&m, NULL, 0, &need));   // ...and still refused
  }

  // And every field of a module that came off disk reads back inside its own
  // range, which is what says the walk and the loader agree.
  {
    ntrk::Module m;
    rt_build(&m, s);
    size_t wrote = 0;
    CHECK(ntrk::module_save(&m, g_saved, sizeof g_saved, &wrote));
    ntrk::Module b;
    CHECK(ntrk::module_load(&b, g_saved, wrote));
    for (int id = 0; id < ntrk::module_param_count(); ++id) {
      int lo = 0, hi = 0;
      ntrk::module_param_range(&b, id, &lo, &hi);
      const int v = ntrk::module_param_get(&b, id);
      if (v < lo || v > hi)
        printf("  %s: %d outside %d..%d\n", ntrk::module_param_at(id)->name, v, lo, hi);
      CHECK(v >= lo && v <= hi);
    }
  }
}

// ---- the instrument parameter table -------------------------------------------
//
// The table is the only writer of what an instrument may hold, and both ends of
// the format walk it. That claim is worth exactly as much as the negative
// control below: without it, step one passes just as well against a table whose
// bounds are all far too tight to reach.

static void
test_instrument_param_shape() {
  printf("every instrument parameter names itself and knows its range\n");

  CHECK(ntrk::instrument_param_count() == (int) ntrk::InsParam::kCount);
  CHECK(ntrk::instrument_param_at(-1) == NULL);
  CHECK(ntrk::instrument_param_at(ntrk::instrument_param_count()) == NULL);

  for (int id = 0; id < ntrk::instrument_param_count(); ++id) {
    const ntrk::InsParamInfo *info = ntrk::instrument_param_at(id);
    CHECK(info != NULL);
    if (info == NULL)
      continue;
    CHECK(info->name != NULL && info->name[0] != '\0');
    // Neither of the command table's other two shapes can describe a field: an
    // instrument byte is a magnitude or one of a list, never two nibbles and
    // never unread.
    CHECK(info->shape == ntrk::ParamShape::Continuous ||
          info->shape == ntrk::ParamShape::Choice);
    if (info->shape == ntrk::ParamShape::Choice) {
      CHECK(info->choice_count > 0);
      CHECK(info->choice != NULL);
      // A choice whose list is shorter than its range renders a null name for
      // a value the format accepts.
      CHECK(info->choice_count == info->hi - info->lo + 1);
      for (int c = 0; c < info->choice_count; ++c)
        CHECK(info->choice[c] != NULL && info->choice[c][0] != '\0');
    }
  }

  // The four filter modes are one list. `ntrk_mix.cc` had two copies of them
  // and an editor would have been the third.
  int modes = 0;
  const char *const *names = ntrk::filter_mode_names(&modes);
  CHECK(modes == 4);
  CHECK(strcmp(names[0], "lowpass") == 0);
  CHECK(strcmp(names[3], "notch") == 0);
  const ntrk::InsParamInfo *ft =
      ntrk::instrument_param_at((int) ntrk::InsParam::kFilterType);
  CHECK(ft->choice == names);

  // **A bass has no `kSynthVoiceSpecs` row.** The table has kSynthDrumCount
  // entries and `synth_voice` reaches kBass, so the obvious index is out of
  // bounds -- which is why the question is answered here and not by a caller.
  // Asking every state of a bass is what would run that read under a sanitiser.
  ntrk::Instrument bass;
  bass.type = (uint8_t) ntrk::InstrumentType::kSynth;
  bass.synth_voice = (uint8_t) ntrk::SynthVoice::kBass;
  int live = 0;
  for (int id = 0; id < ntrk::instrument_param_count(); ++id)
    if (ntrk::instrument_param_state(&bass, id) == ntrk::ParamState::Live)
      ++live;
  CHECK(live > 0);

  // The five synth rows the 303 does not read, named rather than counted: a
  // count drifts the moment an unrelated row changes state, and a table that
  // answered "everything is live" would pass every other check in this file.
  const ntrk::InsParam kBassIgnores[5] = {
      ntrk::InsParam::kSynthTune, ntrk::InsParam::kSynthSweep,
      ntrk::InsParam::kSynthTone, ntrk::InsParam::kSynthNoise,
      ntrk::InsParam::kSynthNoiseDecay};
  for (int i = 0; i < 5; ++i)
    CHECK(ntrk::instrument_param_state(&bass, (int) kBassIgnores[i]) ==
          ntrk::ParamState::Inert);
  // ...and one it does, so the check above is not passing on a function that
  // answers Inert for everything.
  CHECK(ntrk::instrument_param_state(&bass, (int) ntrk::InsParam::kSynthCutoff) ==
        ntrk::ParamState::Live);

  // A drum's `tune` is Inert on a hihat and Live on a kick -- stored either
  // way, which is the whole reason Inert is not Absent.
  ntrk::Instrument hat = bass;
  hat.synth_voice = (uint8_t) ntrk::SynthVoice::kHihat;
  ntrk::Instrument kick = bass;
  kick.synth_voice = (uint8_t) ntrk::SynthVoice::kKick;
  const int tune = (int) ntrk::InsParam::kSynthTune;
  CHECK(ntrk::instrument_param_state(&hat, tune) == ntrk::ParamState::Inert);
  CHECK(ntrk::instrument_param_state(&kick, tune) == ntrk::ParamState::Live);

  // ...and a synth row on a PCM instrument is Absent, because the file has no
  // SYNP record to put it in.
  ntrk::Instrument pcm;
  CHECK(ntrk::instrument_param_state(&pcm, tune) == ntrk::ParamState::Absent);

  // A synth walks no sample -- `channel_sample` returns before the sample
  // fields are read -- so its loop is stored, checked and unread. Inert, not
  // Absent: the bytes round-trip, and an editor that hid them would drop the
  // loop someone set before changing the type to SYNTH.
  const int lstart = (int) ntrk::InsParam::kLoopStart;
  const int llen = (int) ntrk::InsParam::kLoopLen;
  ntrk::Instrument sl;
  sl.type = (uint8_t) ntrk::InstrumentType::kSynth;
  sl.length = 64u;
  sl.loop_start = 8u;
  sl.loop_len = 16u;
  CHECK(ntrk::instrument_param_state(&sl, lstart) == ntrk::ParamState::Inert);
  CHECK(ntrk::instrument_param_state(&sl, llen) == ntrk::ParamState::Inert);
  // Still checked, because Inert is checked: a loop off the end of a synth's
  // blob is a file that has gone wrong however little the voice reads it.
  sl.loop_len = 200u;
  CHECK(!ntrk::instrument_fields_valid(sl));
  // A PCM instrument's loop is what the mixer actually walks.
  ntrk::Instrument pl;
  pl.length = 64u;
  pl.loop_start = 8u;
  pl.loop_len = 16u;
  CHECK(ntrk::instrument_param_state(&pl, lstart) == ntrk::ParamState::Live);
}

static void
test_instrument_param_clamps() {
  printf("no value set through the table can author a module that will not save\n");

  const uint8_t types[5] = {
      (uint8_t) ntrk::InstrumentType::kPcm8,
      (uint8_t) ntrk::InstrumentType::kPcm16,
      (uint8_t) ntrk::InstrumentType::kWaveData,
      (uint8_t) ntrk::InstrumentType::kWaveBuiltin,
      (uint8_t) ntrk::InstrumentType::kSynth,
  };

  for (int t = 0; t < 5; ++t) {
    RtSpec s;
    s.type = types[t];
    s.channels = 1;
    ntrk::Module m;
    rt_build(&m, s);
    ntrk::Instrument &ins = m.instruments[0];

    for (int id = 0; id < ntrk::instrument_param_count(); ++id) {
      if (ntrk::instrument_param_state(&ins, id) == ntrk::ParamState::Absent)
        continue;
      // `kType` would move the instrument out from under the sweep.
      if (id == (int) ntrk::InsParam::kType)
        continue;

      int lo = 0, hi = 0;
      ntrk::instrument_param_range(&ins, id, &lo, &hi);
      const int was = ntrk::instrument_param_get(&ins, id);

      ntrk::instrument_param_set(&ins, id, lo - 1);
      int v = ntrk::instrument_param_get(&ins, id);
      CHECK(v >= lo && v <= hi);
      size_t need = 0;
      CHECK(ntrk::module_save(&m, NULL, 0, &need));

      ntrk::instrument_param_set(&ins, id, hi + 1);
      v = ntrk::instrument_param_get(&ins, id);
      CHECK(v >= lo && v <= hi);
      CHECK(ntrk::module_save(&m, NULL, 0, &need));

      ntrk::instrument_param_set(&ins, id, was);
    }
  }

  // **The trap this table was written for.** A filterless instrument carries a
  // cutoff of 0, which is fine until the bit goes on and brings a 20..20000
  // bound with it. Ticking the box has to carry the cutoff into range, or the
  // module plays all afternoon and refuses to save.
  RtSpec fs;
  fs.channels = 1;
  ntrk::Module f;
  rt_build(&f, fs);
  ntrk::Instrument &fi = f.instruments[0];
  fi.flags = (uint8_t) (fi.flags & ~ntrk::kInstrumentFilter);
  fi.filter_cutoff_hz = 0u;
  size_t need = 0;
  CHECK(ntrk::module_save(&f, NULL, 0, &need));      // fine with the bit clear
  ntrk::instrument_param_set(&fi, (int) ntrk::InsParam::kFilter, 1);
  CHECK(fi.filter_cutoff_hz >= 20u);
  CHECK(ntrk::module_save(&f, NULL, 0, &need));      // and still fine with it set

  // A one-frame loop is what the loader turns into a one-shot, so the setter
  // does it here rather than letting it come back as something else.
  ntrk::Module l;
  rt_build(&l, fs);
  ntrk::Instrument &li = l.instruments[0];
  ntrk::instrument_param_set(&li, (int) ntrk::InsParam::kLoopStart, 0);
  ntrk::instrument_param_set(&li, (int) ntrk::InsParam::kLoopLen, 1);
  CHECK(li.loop_len == 0u);

  // Changing the type carries the derived width and, for a built-in shape, the
  // generated geometry -- so what the editor holds is what a reload produces.
  ntrk::Module y;
  rt_build(&y, fs);
  ntrk::Instrument &yi = y.instruments[0];
  ntrk::instrument_param_set(&yi, (int) ntrk::InsParam::kType,
                             (int) ntrk::InstrumentType::kPcm16);
  CHECK(yi.bits == 16u);
  ntrk::instrument_param_set(&yi, (int) ntrk::InsParam::kType,
                             (int) ntrk::InstrumentType::kWaveBuiltin);
  CHECK(yi.bits == 8u);
  CHECK(yi.length == (uint32_t) ntrk::kBuiltinWaveFrames);
  CHECK(yi.loop_len == (uint32_t) ntrk::kBuiltinWaveFrames);
  CHECK(yi.data == ntrk::builtin_wave((int) yi.wave_index));
  ntrk::instrument_param_set(&yi, (int) ntrk::InsParam::kWave, 2);
  CHECK(yi.data == ntrk::builtin_wave(2));           // the sample follows the index
}

static void
test_instrument_param_enforced() {
  printf("and a field written past the table by hand is refused at the save\n");

  // **The negative control, and it is the load-bearing half.** Every check
  // above passes vacuously against a table whose bounds are too tight to reach.
  // These write the raw field, going around the setter, and require the save to
  // refuse -- which is what says the walk really is the format's gate.
  //
  // Ten rows, and that is all of them: every other row's range is its storage,
  // so no value it can hold is out of range and there is nothing to refuse.
  // That is the property, not an omission.
  RtSpec s;
  s.channels = 1;
  size_t need = 0;

#define REFUSED(setup)                                                         \
  do {                                                                         \
    ntrk::Module m;                                                            \
    rt_build(&m, s);                                                           \
    ntrk::Instrument &ins = m.instruments[0];                                  \
    CHECK(ntrk::module_save(&m, NULL, 0, &need));  /* sound before the poke */ \
    setup;                                                                     \
    CHECK(!ntrk::module_save(&m, NULL, 0, &need));                             \
    CHECK(!ntrk::instrument_fields_valid(ins));                                \
  } while (0)

  REFUSED(ins.type = 5u);
  REFUSED(ins.volume = 65u);
  REFUSED(ins.finetune = 8);
  REFUSED(ins.finetune = -9);
  REFUSED(ins.env_sustain = 65u);
  REFUSED(ins.flags = (uint8_t) (ins.flags | ntrk::kInstrumentFilter);
          ins.filter_cutoff_hz = 19u);
  REFUSED(ins.flags = (uint8_t) (ins.flags | ntrk::kInstrumentFilter);
          ins.filter_cutoff_hz = 20001u);
  REFUSED(ins.type = (uint8_t) ntrk::InstrumentType::kWaveBuiltin;
          ins.wave_index = (uint8_t) ntrk::kBuiltinWaveCount);
  REFUSED(ins.type = (uint8_t) ntrk::InstrumentType::kSynth;
          ins.synth_voice = (uint8_t) ntrk::kSynthVoiceCount);
  REFUSED(ins.type = (uint8_t) ntrk::InstrumentType::kSynth;
          ins.synth_dist = (uint8_t) ntrk::kSynthDistKinds);
  REFUSED(ins.type = (uint8_t) ntrk::InstrumentType::kSynth;
          ins.synth_wave = (uint8_t) ntrk::kSynthWaveCount);
  REFUSED(ins.loop_len = 4u; ins.loop_start = ins.length);
  REFUSED(ins.loop_start = 0u; ins.loop_len = ins.length + 1u);

  // The enumerated synth fields are checked whatever the voice -- that is the
  // difference between Inert and Absent, and a walk that skipped Inert would
  // let this through.
  REFUSED(ins.type = (uint8_t) ntrk::InstrumentType::kSynth;
          ins.synth_voice = (uint8_t) ntrk::SynthVoice::kKick;
          ins.synth_dist = (uint8_t) ntrk::kSynthDistKinds);

#undef REFUSED
}

static void
test_instrument_param_round_trip() {
  printf("every parameter at its ceiling survives a save and a load\n");

  const uint8_t types[5] = {
      (uint8_t) ntrk::InstrumentType::kPcm8,
      (uint8_t) ntrk::InstrumentType::kPcm16,
      (uint8_t) ntrk::InstrumentType::kWaveData,
      (uint8_t) ntrk::InstrumentType::kWaveBuiltin,
      (uint8_t) ntrk::InstrumentType::kSynth,
  };

  for (int t = 0; t < 5; ++t) {
    RtSpec s;
    s.type = types[t];
    s.channels = 1;
    ntrk::Module a;
    rt_build(&a, s);
    ntrk::Instrument &ins = a.instruments[0];

    for (int id = 0; id < ntrk::instrument_param_count(); ++id) {
      if (id == (int) ntrk::InsParam::kType)
        continue;
      if (ntrk::instrument_param_state(&ins, id) == ntrk::ParamState::Absent)
        continue;
      int lo = 0, hi = 0;
      ntrk::instrument_param_range(&ins, id, &lo, &hi);
      ntrk::instrument_param_set(&ins, id, hi);
    }

    int expect[(int) ntrk::InsParam::kCount];
    for (int id = 0; id < ntrk::instrument_param_count(); ++id)
      expect[id] = ntrk::instrument_param_get(&ins, id);

    size_t need = 0, wrote = 0;
    CHECK(ntrk::module_save(&a, NULL, 0, &need));
    CHECK(ntrk::module_save(&a, g_saved, sizeof g_saved, &wrote));
    ntrk::Module b;
    CHECK(ntrk::module_load(&b, g_saved, wrote));

    for (int id = 0; id < ntrk::instrument_param_count(); ++id) {
      const int got = ntrk::instrument_param_get(&b.instruments[0], id);
      if (got != expect[id])
        printf("  type %d param %s: wrote %d, read %d\n", (int) types[t],
               ntrk::instrument_param_at(id)->name, expect[id], got);
      CHECK(got == expect[id]);
    }
  }
}

// ---- NAME -------------------------------------------------------------------

static void
test_names() {
  printf("instrument names round-trip, and are optional\n");

  Spec s;
  s.channels = 2;
  s.rows = 8;
  s.patterns = 1;
  s.orders = 1;
  s.instruments = 3;      // the names below are set on two of them
  s.sample_len = 32;
  build(s);

  // A file with no NAME block: every name is empty, and saving writes none --
  // so the round trip is byte-identical to the module that never had one.
  ntrk::Module plain;
  CHECK(ntrk::module_load(&plain, g_bytes, g_size));
  CHECK(plain.instruments[0].name[0] == '\0');
  size_t plain_need = 0;
  CHECK(ntrk::module_save(&plain, nullptr, 0, &plain_need));
  // NOT compared against g_size: these instruments share one sample, and
  // `module_save` rebuilds the blob from what each points at, so a shared
  // buffer is written once per instrument. The claim that matters is the
  // delta below -- a nameless module carries no NAME block at all.

  // Now name two of them. One short, one exactly kNameBytes long with no room
  // for a terminator in the file -- which is the case that catches a writer
  // emitting the in-memory NUL, and a reader trusting one.
  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));
  const char *shortn = "kick";
  const char *fulln = "0123456789012345678901";   // 22 characters exactly
  CHECK((int) strlen(fulln) == ntrk::kNameBytes);
  for (int k = 0; k < (int) strlen(shortn); ++k) m.instruments[0].name[k] = shortn[k];
  for (int k = 0; k < ntrk::kNameBytes; ++k) m.instruments[1].name[k] = fulln[k];

  size_t need = 0;
  CHECK(ntrk::module_save(&m, nullptr, 0, &need));
  // Exactly one directory entry and one payload more than the nameless file.
  CHECK(need == plain_need + (size_t) ntrk::kDirectoryEntryBytes +
                    (size_t) m.instrument_count * (size_t) ntrk::kNameBytes);

  size_t wrote = 0;
  CHECK(ntrk::module_save(&m, g_saved, sizeof g_saved, &wrote));
  CHECK(wrote == need);

  ntrk::Module back;
  CHECK(ntrk::module_load(&back, g_saved, wrote));
  CHECK(strcmp(back.instruments[0].name, "kick") == 0);
  CHECK(strcmp(back.instruments[1].name, fulln) == 0);
  CHECK(back.instruments[2].name[0] == '\0');
  // The full-width name is terminated in memory even though the file's field
  // is not -- a reader that forgot would run into the next record.
  CHECK(back.instruments[1].name[ntrk::kNameBytes] == '\0');

  // Saving what was loaded is byte-identical, so names survive any number of
  // round trips rather than only the first.
  static uint8_t again[sizeof g_saved];
  size_t wrote2 = 0;
  CHECK(ntrk::module_save(&back, again, sizeof again, &wrote2));
  CHECK(wrote2 == wrote);
  CHECK(memcmp(again, g_saved, wrote) == 0);

  // The block is OPTIONAL, and that is the whole point of the id: a reader
  // that does not know NAME must skip it and play the tune. Assert the flag
  // rather than the intent.
  const int blocks = (int) ntrk::read_u16(g_saved + 26);
  // The directory follows every positional block -- order, instruments,
  // patterns, then the sample blob -- so it is found by walking, not guessed.
  size_t dir = (size_t) ntrk::kHeaderBytes + (size_t) back.order_count +
               (size_t) back.instrument_count * (size_t) ntrk::kInstrumentBytes +
               (size_t) back.pattern_count * (size_t) back.rows *
                   (size_t) back.channels * 4u;
  for (int i = 0; i < back.instrument_count; ++i)
    dir += (size_t) back.instruments[i].length *
           (back.instruments[i].bits == 16 ? 2u : 1u);
  bool saw = false;
  for (int i = 0; i < blocks; ++i) {
    const uint8_t *e = g_saved + dir + (size_t) i * (size_t) ntrk::kDirectoryEntryBytes;
    if (ntrk::read_u16(e + 0) != ntrk::kBlockName)
      continue;
    saw = true;
    CHECK((ntrk::read_u16(e + 2) & (uint16_t) ntrk::kBlockCritical) == 0u);

    // A length that disagrees with the instrument count is refused, not read
    // as far as it goes -- the rule every other block here is held to.
    static uint8_t bad[sizeof g_saved];
    memcpy(bad, g_saved, wrote);
    uint8_t *be = bad + (dir + (size_t) i * (size_t) ntrk::kDirectoryEntryBytes);
    ntrk::write_u32(be + 8, (uint32_t) (ntrk::read_u32(be + 8) - ntrk::kNameBytes));
    ntrk::Module refused;
    CHECK(!ntrk::module_load(&refused, bad, wrote));
  }
  CHECK(saw);
}

// ---- T52, advancing without rendering -----------------------------------------

static void
test_skip() {
  printf("skipping frames leaves the player where rendering them would\n");

  Spec s;
  s.channels = 2;
  s.rows = 8;
  s.orders = 4;
  s.patterns = 4;
  s.sample_len = 64;
  s.loop_len = 0;          // ONE-SHOT: a sample that runs out is the whole point
  build(s);
  for (int p = 0; p < 4; ++p) {
    g_bytes[32u + (size_t) p] = (uint8_t) p;
    const size_t at = patterns_at(s) +
                      (size_t) p * (size_t) s.rows * (size_t) s.channels * 4u;
    g_bytes[at + 0] = (uint8_t) (13 + p * 3);      // a note on row 0 of each
    g_bytes[at + 1] = 1u;
  }
  // Row 4 of pattern 2 carries a note with NO instrument column -- the tie case.
  // Whether it ties or strikes depends on `ch->playing`, which only the voice
  // walk moves, so this is the row the negative control is about.
  {
    const size_t at = patterns_at(s) +
                      2u * (size_t) s.rows * (size_t) s.channels * 4u +
                      4u * (size_t) s.channels * 4u;
    g_bytes[at + 0] = 25u;
    g_bytes[at + 1] = 0u;                          // no instrument: inherits
  }

  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Player probe;
  ntrk::player_start(&probe, &m);
  const double per_row =
      ntrk::frames_per_tick(&probe, 48000.0) * (double) m.speed;
  const int pre = (int) (per_row * (double) (s.rows * 2));   // two whole orders
  const int n = (int) (per_row * (double) s.rows);           // one more order

  static double a[400000];
  static double b[400000];
  static double scratch[400000];
  CHECK((size_t) n * 2u <= sizeof a / sizeof a[0]);
  CHECK((size_t) pre * 2u <= sizeof scratch / sizeof scratch[0]);

  // THE GATE. Skipping `pre` frames must leave the player in the state that
  // rendering and discarding them does -- so the audio that follows is equal
  // sample for sample.
  ntrk::Player pa;
  ntrk::player_start(&pa, &m);
  ntrk::player_skip(&pa, 48000.0, pre);
  for (int i = 0; i < n * 2; ++i) a[i] = 0.0;
  ntrk::render_add(&pa, a, n, 2, 48000.f);

  ntrk::Player pb;
  ntrk::player_start(&pb, &m);
  for (int i = 0; i < pre * 2; ++i) scratch[i] = 0.0;
  ntrk::render_add(&pb, scratch, pre, 2, 48000.f);
  for (int i = 0; i < n * 2; ++i) b[i] = 0.0;
  ntrk::render_add(&pb, b, n, 2, 48000.f);

  CHECK(memcmp(a, b, (size_t) n * 2u * sizeof(double)) == 0);
  CHECK(pa.order == pb.order);
  CHECK(pa.row == pb.row);
  CHECK(pa.ticks_elapsed == pb.ticks_elapsed);

  // **The negative control, and it is what makes the gate bite.** The sample is
  // one-shot and shorter than the rows it is held over, so by the time the tie
  // row arrives the voice has run out and `playing` is false -- which is what
  // makes that row STRIKE rather than tie. Only the voice walk sets it, so a
  // skip that advanced the sequencer and left the voices alone would arrive
  // with `playing` still true and play a different note. Asserting the flag
  // asserts the mechanism rather than its symptom.
  // Against a player rendered EXACTLY `pre` frames -- `pb` above has since
  // rendered `n` more, and comparing a skipped player to it would be comparing
  // two different positions and passing only by luck.
  ntrk::Player pc;
  ntrk::player_start(&pc, &m);
  ntrk::player_skip(&pc, 48000.0, pre);
  ntrk::Player pr;
  ntrk::player_start(&pr, &m);
  for (int i = 0; i < pre * 2; ++i) scratch[i] = 0.0;
  ntrk::render_add(&pr, scratch, pre, 2, 48000.f);

  CHECK(pc.channels[0].playing == pr.channels[0].playing);
  CHECK(!pc.channels[0].playing);      // it really did run out; the walk happened
  CHECK(pc.channels[0].pos == pr.channels[0].pos);
  CHECK(pc.channels[0].env_stage == pr.channels[0].env_stage);
  CHECK(pc.channels[0].env_level == pr.channels[0].env_level);

  // player_skip_to lands on the row it names.
  ntrk::Player pd;
  ntrk::player_start(&pd, &m);
  CHECK(ntrk::player_skip_to(&pd, 48000.0, 2, 4, 100000u));
  CHECK(pd.order == 2);
  CHECK(pd.row == 4);

  // ...and gives up rather than hanging on a row the tune never reaches.
  ntrk::Player pe;
  ntrk::player_start(&pe, &m);
  CHECK(ntrk::player_loop_range(&pe, 0, 0));      // never leaves order 0
  CHECK(!ntrk::player_skip_to(&pe, 48000.0, 3, 0, 500u));
  CHECK(pe.order == 0);                            // and is still somewhere valid
}

// ---- T51, the host's loop over an order range ---------------------------------

static void
test_loop_range() {
  printf("a looped range repeats without seeking, and keeps its tempo\n");

  // Four orders, each its own pattern with its own note, so where the player is
  // is audible in the bytes rather than only visible in a field.
  Spec s;
  s.channels = 1;
  s.rows = 4;
  s.orders = 4;
  s.patterns = 4;
  s.sample_len = 32;
  build(s);
  for (int p = 0; p < 4; ++p) {
    g_bytes[32u + (size_t) p] = (uint8_t) p;                      // order[p] -> pattern p
    const size_t at = patterns_at(s) +
                      (size_t) p * (size_t) s.rows * (size_t) s.channels * 4u;
    g_bytes[at + 0] = (uint8_t) (13 + p * 5);                     // a note per pattern
    g_bytes[at + 1] = 1u;
  }

  ntrk::Module m;
  CHECK(ntrk::module_load(&m, g_bytes, g_size));

  ntrk::Player p0;
  ntrk::player_start(&p0, &m);
  // A range the module has not got is refused rather than clamped: a caller
  // that asked for one has a bug, and a clamp would hide it behind a section
  // that plays the wrong bars.
  CHECK(!ntrk::player_loop_range(&p0, 1, 4));
  CHECK(!ntrk::player_loop_range(&p0, 2, 1));
  CHECK(ntrk::player_loop_range(&p0, 1, 2));
  CHECK(p0.loop_first == 1);
  CHECK(ntrk::player_loop_range(&p0, -1, -1));
  CHECK(p0.loop_last == -1);

  // player_start clears it -- an order index names a position in one module.
  CHECK(ntrk::player_loop_range(&p0, 1, 2));
  ntrk::player_start(&p0, &m);
  CHECK(p0.loop_first == -1);

  // THE GATE: rendering past the end of the range is the range again, sample
  // for sample. Orders 1 and 2 are eight rows between them.
  ntrk::Player pa;
  ntrk::player_start(&pa, &m);
  pa.order = 1;
  CHECK(ntrk::player_loop_range(&pa, 1, 2));

  const double per_row = ntrk::frames_per_tick(&pa, 48000.0) * (double) m.speed;
  const int n = (int) (per_row * (double) (2 * s.rows));

  static double a[300000];
  static double b[300000];
  CHECK((size_t) n * 2u <= sizeof a / sizeof a[0]);

  for (int i = 0; i < n * 2; ++i) a[i] = 0.0;
  ntrk::render_add(&pa, a, n, 2, 48000.f);          // one lap of the range
  const int speed_lap = pa.speed;
  const int bpm_lap = pa.bpm;
  for (int i = 0; i < n * 2; ++i) b[i] = 0.0;
  ntrk::render_add(&pa, b, n, 2, 48000.f);          // the next, across the wrap

  CHECK(memcmp(a, b, (size_t) n * 2u * sizeof(double)) == 0);

  // The tempo is untouched across the wrap, which is the whole reason this is
  // not player_seek in a wrapper: a seek resets both to the module's own.
  CHECK(pa.speed == speed_lap);
  CHECK(pa.bpm == bpm_lap);
  CHECK(pa.order >= 1 && pa.order <= 2);

  // **The negative control**, so the assertions above are about the loop and not
  // about a tune that never moves. One lap without a range walks out the far
  // side into order 3; with one, the same render is back at the range's start.
  // Compared after ONE lap on purpose: the module's own `loop` wraps a
  // rangeless player around the whole song, so after two it is back inside
  // [1,2] by coincidence and the check would pass while testing nothing.
  ntrk::Player pn;
  ntrk::player_start(&pn, &m);
  pn.order = 1;
  static double c[300000];
  for (int i = 0; i < n * 2; ++i) c[i] = 0.0;
  ntrk::render_add(&pn, c, n, 2, 48000.f);
  CHECK(pn.order == 3);

  ntrk::Player pr;
  ntrk::player_start(&pr, &m);
  pr.order = 1;
  CHECK(ntrk::player_loop_range(&pr, 1, 2));
  for (int i = 0; i < n * 2; ++i) c[i] = 0.0;
  ntrk::render_add(&pr, c, n, 2, 48000.f);
  CHECK(pr.order == 1);
}

// ---- TUNE --------------------------------------------------------------------

static void
test_tune() {
  printf("the grid division round-trips, and is critical only when it swings\n");

  Spec s;
  s.channels = 1;
  s.rows = 8;
  s.orders = 1;
  s.patterns = 1;
  s.sample_len = 32;
  build(s);

  // No block: the defaults are what a file has always meant.
  ntrk::Module plain;
  CHECK(ntrk::module_load(&plain, g_bytes, g_size));
  CHECK(plain.rows_per_beat == 4);
  CHECK(plain.rows_per_bar == 16);
  CHECK(plain.swing == 0);
  size_t plain_need = 0;
  CHECK(ntrk::module_save(&plain, nullptr, 0, &plain_need));
  CHECK(plain_need == g_size);          // and writes no block, so the file is unchanged

  // Three in a bar, and a shuffle. 0.19 of a row is the deepest figure in the
  // starter tunes, which is where the number comes from rather than taste.
  ntrk::Module m = plain;
  m.rows_per_beat = 6;
  m.rows_per_bar = 18;
  m.swing = (int) (0.19 * ntrk::kSwingUnit);

  size_t need = 0;
  CHECK(ntrk::module_save(&m, nullptr, 0, &need));
  CHECK(need == plain_need + (size_t) ntrk::kDirectoryEntryBytes +
                    (size_t) ntrk::kTuneBytes);
  size_t wrote = 0;
  CHECK(ntrk::module_save(&m, g_saved, sizeof g_saved, &wrote));

  ntrk::Module back;
  CHECK(ntrk::module_load(&back, g_saved, wrote));
  CHECK(back.rows_per_beat == 6);
  CHECK(back.rows_per_bar == 18);
  CHECK(back.swing == m.swing);

  // Swinging, so the block is CRITICAL: a reader that skipped it would play the
  // tune straight and sound wrong with nothing to point at.
  const size_t dir = tune_directory_at(back, g_saved);
  bool saw = false;
  const int blocks = (int) ntrk::read_u16(g_saved + 26);
  for (int i = 0; i < blocks; ++i) {
    const uint8_t *e = g_saved + dir + (size_t) i * (size_t) ntrk::kDirectoryEntryBytes;
    if (ntrk::read_u16(e + 0) != ntrk::kBlockTune) continue;
    saw = true;
    CHECK((ntrk::read_u16(e + 2) & (uint16_t) ntrk::kBlockCritical) != 0u);
  }
  CHECK(saw);

  // ...and NOT critical without one, because a division is drawn, not played.
  ntrk::Module bars = plain;
  bars.rows_per_bar = 8;
  size_t w2 = 0;
  CHECK(ntrk::module_save(&bars, g_saved, sizeof g_saved, &w2));
  ntrk::Module b2;
  CHECK(ntrk::module_load(&b2, g_saved, w2));
  const size_t dir2 = tune_directory_at(b2, g_saved);
  const int blocks2 = (int) ntrk::read_u16(g_saved + 26);
  saw = false;
  for (int i = 0; i < blocks2; ++i) {
    const uint8_t *e = g_saved + dir2 + (size_t) i * (size_t) ntrk::kDirectoryEntryBytes;
    if (ntrk::read_u16(e + 0) != ntrk::kBlockTune) continue;
    saw = true;
    CHECK((ntrk::read_u16(e + 2) & (uint16_t) ntrk::kBlockCritical) == 0u);
  }
  CHECK(saw);

  // Every bound refuses on both sides, because a writer looser than its reader
  // makes a file that will not load back.
  ntrk::Module bad = plain;
  size_t junk = 0;
  bad.rows_per_beat = 0;   CHECK(!ntrk::module_save(&bad, nullptr, 0, &junk));
  bad = plain; bad.rows_per_bar = 256;  CHECK(!ntrk::module_save(&bad, nullptr, 0, &junk));
  // A bar that is not a whole number of beats: the two bands could not agree.
  bad = plain; bad.rows_per_beat = 4; bad.rows_per_bar = 6;
  CHECK(!ntrk::module_save(&bad, nullptr, 0, &junk));
  bad = plain; bad.swing = ntrk::kSwingMax + 1;
  CHECK(!ntrk::module_save(&bad, nullptr, 0, &junk));
}

int
main(void) {
  test_loads();
  test_refusals();
  test_noise_and_silence();
  test_deterministic();
  test_adds();
  test_looping();
  test_note_delay_and_cut();
  test_slides();
  test_tremolo();
  test_save_round_trip();
  test_names();
  test_tune();
  test_loop_range();
  test_skip();
  test_mute();
  test_slot_mute();
  test_slot_mute_refusals();
  test_seek();
  test_geometry_shrinks_under_player();
  test_preview();
  test_pan();
  test_block_invariance();
  test_v2_loads();
  test_v2_refusals();
  test_v2_save_round_trip();
  test_lanes_and_macros_load();
  test_lanes_and_macros_refusals();
  test_lanes_and_macros_round_trip();
  test_envelope();
  test_envelope_note_off();
  test_synth_loads();
  test_synth_refusals();
  test_synth_voices();
  test_synth_distinct();
  test_synth_range();
  test_synth_deterministic();
  test_synth_absent();
  test_synth_save_round_trip();
  test_bass_sounds();
  test_bass_cutoff();
  test_bass_resonance();
  test_bass_env_mod();
  test_bass_distortion();
  test_bass_finite();
  test_bass_deterministic();
  test_plane_accent();
  test_plane_slide();
  test_plane_slide_is_exponential();
  test_plane_slide_ties();
  test_synth_note_off();
  test_plane_slide_crosses_a_row();
  test_tone_porta_stays_linear();
  test_bass_envelopes_are_separate();
  test_plane_slide_zero_and_the_guard();
  test_plane_slide_reaches_a_sample();
  test_panning_commands();
  test_instrument_transpose();
  test_fractional_frame_and_note_step();
  test_plane_every_column();
  test_synth_grid();
  test_loader_fuzz();
  test_round_trip_property();
  test_describe_protracker();
  test_mnemonics();
  test_describe_player_range();
  test_synth_reference_pitch();
  test_render_hashes();
  test_mixr_block();
  test_instrument_param_shape();
  test_instrument_param_clamps();
  test_instrument_param_enforced();
  test_instrument_param_round_trip();
  test_module_param_table();

  test_slic_block();
  test_slic_refusals();
  test_slc_effect();
  test_slc_display();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
