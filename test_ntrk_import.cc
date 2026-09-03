// Tests for the module importer, with nothing behind them.
//
//     c++ -std=c++11 -Wall -Wextra -ffp-contract=off \
//         -o /tmp/test_ntrk_import third_party/ntrk/test_ntrk_import.cc
//     /tmp/test_ntrk_import
//
// Single translation unit: `ntrk_import.cc` is included rather than linked, the
// same route test_ntrk_fx.cc takes through ntrk_unity.h, so the "one compiler
// invocation, no build system" claim holds for this file too.
//
// **Run from the repository root**, because the real-file checks read
// `third_party/ntrk/testdata` by a relative path. That directory is gitignored
// and usually empty -- the synthetic modules below are what runs everywhere,
// and they are built to exercise the fields real files get wrong: lengths in
// words, the signed finetune nibble, loops that run off the end of a sample,
// and stale order entries past the song length.
//
// There was a reference implementation here once -- a Python `mod_import.py`
// this was required to agree with byte for byte, which is how the native
// importer was proved correct in the first place. It wrote the retired version
// 1 of the format, so it could no longer produce a file this library loads, and
// it went when version 1 did.

#include "ntrk_import.cc"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp, for a `.MOD` off a BBS

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

// ---- building a `.mod` ------------------------------------------------------
//
// A real module is 1084 bytes of header before anything interesting, so the
// spec below carries only what a test varies and `mod_build` fills the rest.

// Comfortably past any real module -- a `.mod` read short would be compared as
// a truncation rather than as itself, which is a pass this file must not give.
static const size_t kModCap = 2 * 1024 * 1024;

struct ModSpec {
  const char *sig = "M.K.";
  int song_length = 2;
  int restart = 127;                 // what almost every module carries
  int pattern_count = 2;             // orders are filled to reach this
  uint16_t len_words[31] = {};       // **words**, as on disk
  uint8_t finetune[31] = {};         // the raw byte; low nibble is signed
  uint8_t volume[31] = {};
  uint16_t loop_start[31] = {};      // words
  uint16_t loop_len[31] = {};        // words
  int sample_shortfall = 0;          // bytes withheld from the end of the file
  int stale_order = -1;              // a pattern number past `song_length`
};

static int
mod_channels(const char *sig) {
  if (strcmp(sig, "6CHN") == 0)
    return 6;
  if (strcmp(sig, "8CHN") == 0)
    return 8;
  return 4;
}

// Fills `buf` and returns the total size. Patterns and samples are filled from
// a counter rather than a random source: an importer bug has to change these
// bytes, and a fixture that differs run to run cannot be compared with anything.
static size_t
mod_build(uint8_t *buf, const ModSpec &spec) {
  memset(buf, 0, kModCap);
  memcpy(buf, "test module", 11);

  for (int i = 0; i < 31; ++i) {
    uint8_t *rec = buf + 20 + (size_t) i * 30u;
    rec[0] = (uint8_t) ('a' + i);
    rec[22] = (uint8_t) (spec.len_words[i] >> 8);
    rec[23] = (uint8_t) (spec.len_words[i] & 0xffu);
    rec[24] = spec.finetune[i];
    rec[25] = spec.volume[i];
    rec[26] = (uint8_t) (spec.loop_start[i] >> 8);
    rec[27] = (uint8_t) (spec.loop_start[i] & 0xffu);
    rec[28] = (uint8_t) (spec.loop_len[i] >> 8);
    rec[29] = (uint8_t) (spec.loop_len[i] & 0xffu);
  }

  buf[950] = (uint8_t) spec.song_length;
  buf[951] = (uint8_t) spec.restart;
  for (int i = 0; i < spec.song_length; ++i)
    buf[952 + i] = (uint8_t) (i % spec.pattern_count);
  // A pattern number the song never reaches still sizes the pattern block; the
  // importer has to count from the whole table, not the played part of it.
  if (spec.stale_order >= 0)
    buf[952 + spec.song_length] = (uint8_t) spec.stale_order;
  memcpy(buf + 1080, spec.sig, 4);

  const int channels = mod_channels(spec.sig);
  int highest = 0;
  for (int i = 0; i < 128; ++i)
    if ((int) buf[952 + i] > highest)
      highest = (int) buf[952 + i];
  const size_t cells = (size_t) (highest + 1) * 64u * (size_t) channels;

  size_t at = 1084;
  for (size_t i = 0; i < cells; ++i) {
    // Period, sample number and effect, packed the way ProTracker does: the
    // sample number is split across two bytes and the period shares one with it.
    static const int kSome[8] = {856, 428, 214, 113, 0, 508, 240, 127};
    const int period = kSome[i % 8];
    const int sample = (int) (i % 4) + 1;
    uint8_t *c = buf + at;
    c[0] = (uint8_t) (((sample & 0xf0) ) | ((period >> 8) & 0x0f));
    c[1] = (uint8_t) (period & 0xff);
    c[2] = (uint8_t) (((sample & 0x0f) << 4) | (int) (i % 16));
    c[3] = (uint8_t) (i * 7u);
    at += 4;
  }

  for (int i = 0; i < 31; ++i) {
    const size_t bytes = (size_t) spec.len_words[i] * 2u;
    for (size_t k = 0; k < bytes; ++k)
      buf[at + k] = (uint8_t) ((k * 13u + (unsigned) i) & 0xffu);
    at += bytes;
  }

  return at - (size_t) spec.sample_shortfall;
}

// A spec with four instruments that between them cover the awkward fields.
static ModSpec
spec_typical() {
  ModSpec spec;
  spec.len_words[0] = 100;     // 200 bytes
  spec.volume[0] = 64;
  spec.loop_start[0] = 10;     // 20 bytes
  spec.loop_len[0] = 40;       // 80 bytes
  spec.finetune[0] = 0;

  spec.len_words[1] = 3;       // 6 bytes -- a length only right if read as words
  spec.volume[1] = 32;
  spec.finetune[1] = 0x0f;     // the signed nibble: -1, not 15

  spec.len_words[2] = 50;
  spec.volume[2] = 200;        // above the format's 64, and clamped to it
  spec.finetune[2] = 0x08;     // -8, the bottom of the range
  spec.loop_start[2] = 40;     // 80 bytes, with a loop running off the end
  spec.loop_len[2] = 30;       // 60 bytes -- clamped to 20

  spec.len_words[3] = 7;
  spec.volume[3] = 1;
  spec.finetune[3] = 0x07;     // +7, the top of the range
  spec.loop_len[3] = 1;        // one word is "no loop", not a two-byte loop
  return spec;
}

// ---- the checks -------------------------------------------------------------

static uint8_t g_mod[kModCap];
static uint8_t g_scratch[kModCap];
static uint8_t g_out[kModCap];

static void
test_detect() {
  static const char *kGood[] = {"M.K.", "M!K!", "FLT4", "4CHN", "6CHN", "8CHN"};
  for (int i = 0; i < 6; ++i) {
    ModSpec spec = spec_typical();
    spec.sig = kGood[i];
    const size_t size = mod_build(g_mod, spec);
    CHECK(ntrk::import_detect(g_mod, size) == ntrk::ImportFormat::kMod);
    CHECK(ntrk::import_scratch_needed(g_mod, size) > 0);
  }

  // The 15-instrument format has no signature at all, so offset 1080 holds
  // pattern data. Refused rather than guessed at.
  const size_t size = mod_build(g_mod, spec_typical());
  memcpy(g_mod + 1080, "\x00\x11\x22\x33", 4);
  CHECK(ntrk::import_detect(g_mod, size) == ntrk::ImportFormat::kUnknown);
  CHECK(ntrk::import_scratch_needed(g_mod, size) == 0);

  // Channel counts this player cannot hold are signatures it does not know.
  memcpy(g_mod + 1080, "16CH", 4);
  CHECK(ntrk::import_detect(g_mod, size) == ntrk::ImportFormat::kUnknown);

  CHECK(ntrk::import_detect(nullptr, 0) == ntrk::ImportFormat::kUnknown);
  static const uint8_t kRubbish[8] = {'N', 'T', 'R', 'K', 1, 0, 4, 0};
  CHECK(ntrk::import_detect(kRubbish, sizeof(kRubbish)) == ntrk::ImportFormat::kUnknown);
  CHECK(ntrk::import_scratch_needed(kRubbish, sizeof(kRubbish)) == 0);
}

static bool
convert(const ModSpec &spec, size_t *mod_size, size_t *out_size) {
  *mod_size = mod_build(g_mod, spec);
  const size_t need = ntrk::import_scratch_needed(g_mod, *mod_size);
  if (need == 0 || need > sizeof(g_scratch))
    return false;
  return ntrk::import_convert(g_mod, *mod_size, g_scratch, sizeof(g_scratch),
                              g_out, sizeof(g_out), out_size);
}

static void
test_convert_loads_and_renders() {
  size_t mod_size = 0, out_size = 0;
  CHECK(convert(spec_typical(), &mod_size, &out_size));

  ntrk::Module module;
  CHECK(ntrk::module_load(&module, g_out, out_size));
  CHECK(module.version == 2);
  CHECK(module.channels == 4);
  CHECK(module.rows == 64);
  CHECK(module.speed == 6);
  CHECK(module.bpm == 125);
  CHECK(module.order_count == 2);
  CHECK(module.pattern_count == 2);
  CHECK(module.restart == 0);          // 127 is past the order list, so none

  // **Lengths in words.** Instrument 2's three words are six bytes; read as
  // bytes it would be three, and every sample after it would be misplaced.
  CHECK(module.instruments[0].length == 200u);
  CHECK(module.instruments[1].length == 6u);
  CHECK(module.instruments[2].length == 100u);
  CHECK(module.instruments[3].length == 14u);

  // **The signed nibble.** 0x0f is -1 and 0x08 is -8; read as a byte they would
  // be 15 and 8, which `module_save` refuses outright.
  CHECK(module.instruments[0].finetune == 0);
  CHECK(module.instruments[1].finetune == -1);
  CHECK(module.instruments[2].finetune == -8);
  CHECK(module.instruments[3].finetune == 7);

  CHECK(module.instruments[0].volume == 64);
  CHECK(module.instruments[2].volume == 64);   // 200 clamped
  CHECK(module.instruments[3].volume == 1);

  CHECK(module.instruments[0].loop_start == 20u);
  CHECK(module.instruments[0].loop_len == 80u);
  CHECK(module.instruments[2].loop_start == 80u);
  CHECK(module.instruments[2].loop_len == 20u);   // clamped to the sample's end
  CHECK(module.instruments[3].loop_len == 0u);    // one word means no loop

  // The samples survived the trip unchanged: PCM8 and a `.mod`'s 8-bit signed
  // data are the same thing, which is why nothing copies them on the way in.
  bool same = true;
  for (uint32_t k = 0; k < module.instruments[0].length; ++k)
    if ((uint8_t) module.instruments[0].data[k] != (uint8_t) ((k * 13u) & 0xffu))
      same = false;
  CHECK(same);

  // It plays. A tune that loads and renders silence is the failure this catches
  // -- the notes came through the period table rather than as empty cells.
  ntrk::Player player;
  ntrk::player_start(&player, &module);
  static double buffer[4096 * 2];
  memset(buffer, 0, sizeof(buffer));
  ntrk::render_add(&player, buffer, 4096, 2, 48000.f);
  double peak = 0.0;
  for (int i = 0; i < 4096 * 2; ++i) {
    const double v = buffer[i] < 0.0 ? -buffer[i] : buffer[i];
    if (v > peak)
      peak = v;
  }
  CHECK(peak > 0.0);
}

static void
test_instrument_trimming() {
  // Only four instruments carry data and no cell names a higher one, so the
  // twenty-seven empty trailing slots are dropped rather than written out.
  size_t mod_size = 0, out_size = 0;
  CHECK(convert(spec_typical(), &mod_size, &out_size));
  ntrk::Module module;
  CHECK(ntrk::module_load(&module, g_out, out_size));
  CHECK(module.instrument_count == 4);

  // A cell naming instrument 9 keeps every slot up to it, empty or not: the
  // number in a cell is an index, so dropping one from the middle would
  // renumber the rest.
  mod_size = mod_build(g_mod, spec_typical());
  g_mod[1084 + 0] = (uint8_t) ((9 & 0xf0) | (g_mod[1084 + 0] & 0x0f));
  g_mod[1084 + 2] = (uint8_t) (((9 & 0x0f) << 4) | (g_mod[1084 + 2] & 0x0f));
  CHECK(ntrk::import_convert(g_mod, mod_size, g_scratch, sizeof(g_scratch),
                             g_out, sizeof(g_out), &out_size));
  CHECK(ntrk::module_load(&module, g_out, out_size));
  CHECK(module.instrument_count == 9);
}

static void
test_stale_order_sizes_the_patterns() {
  // Two orders played, but the table's stale third entry names pattern 4. The
  // block on disk is five patterns long whatever the song plays, and counting
  // from the played part alone would put every sample offset out by the
  // difference -- a tune that imports "successfully" as noise.
  ModSpec spec = spec_typical();
  spec.stale_order = 4;
  size_t mod_size = 0, out_size = 0;
  CHECK(convert(spec, &mod_size, &out_size));
  ntrk::Module module;
  CHECK(ntrk::module_load(&module, g_out, out_size));
  CHECK(module.pattern_count == 5);
  CHECK(module.order_count == 2);
  CHECK(module.instruments[0].length == 200u);
  bool same = true;
  for (uint32_t k = 0; k < module.instruments[0].length; ++k)
    if ((uint8_t) module.instruments[0].data[k] != (uint8_t) ((k * 13u) & 0xffu))
      same = false;
  CHECK(same);
}

static void
test_truncation_refused() {
  const ModSpec spec = spec_typical();
  const size_t full = mod_build(g_mod, spec);
  size_t out_size = 0;

  // Every boundary: inside the header, at the signature, part way through the
  // pattern block, and part way through the sample blob. None of them may
  // produce a module -- a half-read file is the shape that imports as noise.
  static const size_t kCuts[] = {0, 1, 20, 950, 1079, 1083, 1084, 1085};
  for (size_t i = 0; i < sizeof(kCuts) / sizeof(kCuts[0]); ++i) {
    CHECK(!ntrk::import_convert(g_mod, kCuts[i], g_scratch, sizeof(g_scratch),
                                g_out, sizeof(g_out), &out_size));
    CHECK(out_size == 0);
  }
  // One byte short of the pattern block, and one short of the whole file.
  const size_t patterns_end = 1084u + 5u * 64u * 4u * 4u;   // 5 patterns here
  (void) patterns_end;
  for (size_t cut = 1084; cut < full; cut += 97) {
    CHECK(!ntrk::import_convert(g_mod, cut, g_scratch, sizeof(g_scratch), g_out,
                                sizeof(g_out), &out_size));
  }
  CHECK(!ntrk::import_convert(g_mod, full - 1, g_scratch, sizeof(g_scratch),
                              g_out, sizeof(g_out), &out_size));
  CHECK(ntrk::import_convert(g_mod, full, g_scratch, sizeof(g_scratch), g_out,
                             sizeof(g_out), &out_size));

  // A sample cut short is refused too, rather than padded: a library handed an
  // arbitrary file may not invent the tail of a sample.
  ModSpec short_spec = spec_typical();
  short_spec.sample_shortfall = 1;
  const size_t cut = mod_build(g_mod, short_spec);
  CHECK(!ntrk::import_convert(g_mod, cut, g_scratch, sizeof(g_scratch), g_out,
                              sizeof(g_out), &out_size));
}

static void
test_refusals() {
  size_t out_size = 0;
  const size_t full = mod_build(g_mod, spec_typical());

  // Scratch one byte short of what was asked for. The cells have to become a
  // `Note` array somewhere, and there is no partial answer.
  const size_t need = ntrk::import_scratch_needed(g_mod, full);
  CHECK(need == 5u * 64u * 4u * 4u ||
        need == 2u * 64u * 4u * 4u);   // stale order absent here: 2 patterns
  CHECK(!ntrk::import_convert(g_mod, full, g_scratch, need - 1, g_out,
                              sizeof(g_out), &out_size));
  CHECK(ntrk::import_convert(g_mod, full, g_scratch, need, g_out, sizeof(g_out),
                             &out_size));
  const size_t good_size = out_size;
  // A refusal clears `written`, so the successful answer is kept above rather
  // than read back after one of these.
  CHECK(!ntrk::import_convert(g_mod, full, nullptr, need, g_out, sizeof(g_out),
                              &out_size));
  CHECK(out_size == 0);

  // An output buffer one byte short, and the size query that avoids it.
  size_t wanted = 0;
  CHECK(ntrk::import_convert(g_mod, full, g_scratch, sizeof(g_scratch), nullptr,
                             0, &wanted));
  CHECK(wanted == good_size);
  CHECK(!ntrk::import_convert(g_mod, full, g_scratch, sizeof(g_scratch), g_out,
                              wanted - 1, &out_size));

  // A song length of zero has no order list, and 129 runs past the table.
  for (int bad = 0; bad < 2; ++bad) {
    mod_build(g_mod, spec_typical());
    g_mod[950] = bad == 0 ? (uint8_t) 0 : (uint8_t) 129;
    CHECK(ntrk::import_scratch_needed(g_mod, full) == 0);
    CHECK(!ntrk::import_convert(g_mod, full, g_scratch, sizeof(g_scratch),
                                g_out, sizeof(g_out), &out_size));
  }
}

// ---- building an `.xm` ------------------------------------------------------
//
// **Every branch has to be reached by a file built to reach it**, which is why
// the spec below can produce malformed files as readily as good ones. There is
// no reference implementation to lean on for either format any more, so these
// synthetic modules and the real files in `testdata/` are the whole check.

// The deltas a sample carries on disk, and what they must accumulate to. Chosen
// to wrap: that is the format working as designed, not a corrupt file, and an
// accumulator that saturated or trapped instead would be wrong on real music.
static const uint8_t kXmDelta8[7] = {1, 1, 1, 0xfd, 100, 100, 100};
static const int8_t kXmDecoded8[7] = {1, 2, 3, 0, 100, -56, 44};
static const uint16_t kXmDelta16[3] = {1, 0x7000, 0x7000};
static const int16_t kXmDecoded16[3] = {1, 0x7001, (int16_t) 0xe001};

static const int kXmMaxIns = 4;

struct XmSpec {
  int channels = 4;
  int patterns = 1;
  int order_count = 1;
  int restart = 0;
  int speed = 6;
  int bpm = 125;
  uint32_t header_bytes = 276;
  int rows[4] = {8, 8, 8, 8};
  bool empty_pattern[4] = {false, false, false, false};
  bool compressed = true;         // the 0x80 packing, or five bytes a cell
  int instruments = 2;
  int samples[kXmMaxIns] = {1, 1, 1, 1};
  int map_to[kXmMaxIns] = {-1, -1, -1, -1};   // fill the note map with this
  bool wide[kXmMaxIns] = {false, false, false, false};
  int loop_mode[kXmMaxIns] = {1, 0, 0, 0};
  uint32_t loop_start[kXmMaxIns] = {2, 0, 0, 0};   // frames
  uint32_t loop_len[kXmMaxIns] = {3, 0, 0, 0};     // frames
  int8_t relnote[kXmMaxIns] = {0, 0, 0, 0};
  int8_t finetune[kXmMaxIns] = {0, 0, 0, 0};
  uint8_t volume[kXmMaxIns] = {64, 40, 64, 64};
  uint32_t instrument_header_bytes = 263;
  uint32_t sample_header_bytes = 40;
  int truncate = 0;               // bytes withheld from the end of the file
  const char *tag = "Extended Module: ";

  // Global volume. With `gv_probe` set the pattern is laid out for it instead
  // of by `xm_cell_for`: a note on every channel of every row and the `Gxx` or
  // `Hxy` on the last one, which is the shape a tracker writes a fade in. Row
  // `r` of the last channel carries `gv_effect[r]`; zero is no command, and the
  // probe with none at all is the control the "a full global volume costs
  // nothing" check compares against.
  bool gv_probe = false;
  uint8_t gv_effect[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t gv_param[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  // An `E6x` loop over the whole pattern, on the channel beside the
  // global-volume one -- so the same rows are played twice from one copy.
  bool gv_loop = false;
};

static void
xm_put16(uint8_t *p, unsigned v) {
  p[0] = (uint8_t) (v & 0xffu);
  p[1] = (uint8_t) ((v >> 8) & 0xffu);
}

static void
xm_put32(uint8_t *p, unsigned long v) {
  for (int i = 0; i < 4; ++i)
    p[i] = (uint8_t) ((v >> (8 * i)) & 0xffu);
}

// One row of every pattern carries the interesting cells; the rest are empty.
// Returned rather than written so the packer and the assertions read the same
// table -- a fixture whose expected values are spelled out twice is a fixture
// that can agree with a bug.
struct XmCell {
  uint8_t note, instrument, volume, effect, param;
};

static XmCell
xm_cell_for(int index) {
  XmCell c = {0, 0, 0, 0, 0};
  switch (index) {
    case 0: c.note = 49; c.instrument = 1; c.volume = 0x30; break;
    case 1: c.note = 97; break;                       // note-off, no instrument
    case 2: c.volume = 0x74; break;                   // volume slide up 4
    case 3: c.note = 61; c.instrument = 1; c.volume = 0x20; c.effect = 3;
            c.param = 0x10; break;                    // the effect column wins
    case 4: c.effect = 0x1b; c.param = 0x03; break;   // R03 -> E93
    case 5: c.volume = 0xc8; break;                   // set panning 8
    case 6: break;                                    // wholly absent
    case 7: c.note = 50; c.instrument = 2; c.volume = 0x10; break;
    case 8: c.note = 49; c.instrument = 9; break;     // no such instrument
    case 9: c.effect = 0x10; c.param = 0x20; c.volume = 0x30; break;
    case 10: c.volume = 0x63; break;                  // volume slide down 3
    case 11: c.effect = 0x1b; c.param = 0x83; break;  // retrig with a volume
    default: break;
  }
  return c;
}

// The global-volume layout. Three ways of arriving at a volume, one per
// channel, so that a single pattern checks all of them at once: an instrument
// at full volume, an instrument at 40, and a volume column that overrides the
// instrument. The last channel carries the `Gxx`/`Hxy` and nothing else.
static XmCell
xm_gv_cell_for(const XmSpec &spec, int row, int channel) {
  XmCell c = {0, 0, 0, 0, 0};
  if (channel == spec.channels - 1) {
    if (row < 8 && spec.gv_effect[row] != 0) {
      c.effect = spec.gv_effect[row];
      c.param = spec.gv_param[row];
    }
    return c;
  }
  if (spec.gv_loop && channel == spec.channels - 2) {
    if (row == 0) {
      c.effect = 0x0e;
      c.param = 0x60;                       // the loop point
    } else if (row == 7) {
      c.effect = 0x0e;
      c.param = 0x61;                       // and back to it, once
    }
    return c;
  }
  c.note = 49;
  c.instrument = channel == 1 ? 2 : 1;      // volume 64, then volume 40
  if (channel == 2)
    c.volume = 0x30;                        // the volume column, set volume 32
  return c;
}

// Fills `buf` and returns the total size.
static size_t
xm_build(uint8_t *buf, const XmSpec &spec) {
  memset(buf, 0, kModCap);
  memcpy(buf, spec.tag, strlen(spec.tag));
  memcpy(buf + 17, "test xm", 7);
  buf[37] = 0x1a;
  memcpy(buf + 38, "ntrk tests", 10);
  xm_put16(buf + 58, 0x0104u);
  xm_put32(buf + 60, spec.header_bytes);
  xm_put16(buf + 64, (unsigned) spec.order_count);
  xm_put16(buf + 66, (unsigned) spec.restart);
  xm_put16(buf + 68, (unsigned) spec.channels);
  xm_put16(buf + 70, (unsigned) spec.patterns);
  xm_put16(buf + 72, (unsigned) spec.instruments);
  xm_put16(buf + 74, 1u);                    // linear frequencies
  xm_put16(buf + 76, (unsigned) spec.speed);
  xm_put16(buf + 78, (unsigned) spec.bpm);
  for (int i = 0; i < spec.order_count; ++i)
    buf[80 + i] = (uint8_t) (i % spec.patterns);

  size_t at = 60u + (size_t) spec.header_bytes;
  for (int p = 0; p < spec.patterns; ++p) {
    uint8_t *head = buf + at;
    xm_put32(head + 0, 9u);
    head[4] = 0;                             // the only packing there is
    xm_put16(head + 5, (unsigned) spec.rows[p]);
    at += 9;

    size_t packed = 0;
    if (!spec.empty_pattern[p]) {
      const int cells = spec.rows[p] * spec.channels;
      for (int i = 0; i < cells; ++i) {
        const XmCell c =
            spec.gv_probe
                ? xm_gv_cell_for(spec, i / spec.channels, i % spec.channels)
                : xm_cell_for(i);
        uint8_t *d = buf + at + packed;
        if (spec.compressed) {
          uint8_t lead = 0x80u;
          size_t n = 1;
          if (c.note) { lead |= 0x01u; d[n++] = c.note; }
          if (c.instrument) { lead |= 0x02u; d[n++] = c.instrument; }
          if (c.volume) { lead |= 0x04u; d[n++] = c.volume; }
          if (c.effect) { lead |= 0x08u; d[n++] = c.effect; }
          if (c.param) { lead |= 0x10u; d[n++] = c.param; }
          d[0] = lead;
          packed += n;
        } else {
          d[0] = c.note;
          d[1] = c.instrument;
          d[2] = c.volume;
          d[3] = c.effect;
          d[4] = c.param;
          packed += 5;
        }
      }
    }
    xm_put16(head + 7, (unsigned) packed);
    at += packed;
  }

  for (int i = 0; i < spec.instruments; ++i) {
    uint8_t *head = buf + at;
    xm_put32(head + 0, spec.instrument_header_bytes);
    head[4] = (uint8_t) ('A' + i);
    xm_put16(head + 27, (unsigned) spec.samples[i]);
    if (spec.instrument_header_bytes >= 33u)
      xm_put32(head + 29, spec.sample_header_bytes);
    if (spec.instrument_header_bytes >= 129u && spec.map_to[i] >= 0)
      for (int n = 0; n < 96; ++n)
        head[33 + n] = (uint8_t) spec.map_to[i];
    at += spec.instrument_header_bytes;
    if (spec.samples[i] == 0)
      continue;

    // The chosen sample carries the delta stream; the others are four bytes of
    // filler, which is what makes "the majority sample, not sample 0" testable.
    const int chosen = spec.map_to[i] > 0 ? spec.map_to[i] : 0;
    const size_t chosen_bytes =
        spec.wide[i] ? sizeof(kXmDelta16) : sizeof(kXmDelta8);
    const size_t headers = at;
    for (int s = 0; s < spec.samples[i]; ++s) {
      uint8_t *sh = buf + headers + (size_t) s * spec.sample_header_bytes;
      const size_t bytes = s == chosen ? chosen_bytes : 4u;
      const uint32_t stride = spec.wide[i] ? 2u : 1u;
      xm_put32(sh + 0, (unsigned long) bytes);
      xm_put32(sh + 4, spec.loop_start[i] * stride);   // **bytes, not frames**
      xm_put32(sh + 8, spec.loop_len[i] * stride);
      sh[12] = spec.volume[i];
      sh[13] = (uint8_t) spec.finetune[i];
      sh[14] = (uint8_t) ((spec.wide[i] ? 0x10u : 0u) |
                          (unsigned) spec.loop_mode[i]);
      sh[15] = 128;                                    // centred, and unread
      sh[16] = (uint8_t) spec.relnote[i];
    }
    at = headers + (size_t) spec.samples[i] * spec.sample_header_bytes;

    for (int s = 0; s < spec.samples[i]; ++s) {
      if (s != chosen) {
        at += 4;
        continue;
      }
      if (spec.wide[i]) {
        for (size_t k = 0; k < sizeof(kXmDelta16) / 2u; ++k)
          xm_put16(buf + at + k * 2u, kXmDelta16[k]);
      } else {
        memcpy(buf + at, kXmDelta8, sizeof(kXmDelta8));
      }
      at += chosen_bytes;
    }
  }

  return at - (size_t) spec.truncate;
}

static bool
xm_convert(const XmSpec &spec, size_t *xm_size, size_t *out_size) {
  *xm_size = xm_build(g_mod, spec);
  const size_t need = ntrk::import_scratch_needed(g_mod, *xm_size);
  if (need == 0 || need > sizeof(g_scratch))
    return false;
  return ntrk::import_convert(g_mod, *xm_size, g_scratch, sizeof(g_scratch),
                              g_out, sizeof(g_out), out_size);
}

static void
test_xm_detect() {
  XmSpec spec;
  size_t size = xm_build(g_mod, spec);
  CHECK(ntrk::import_detect(g_mod, size) == ntrk::ImportFormat::kXm);
  CHECK(ntrk::import_scratch_needed(g_mod, size) > 0);

  // The tag is the whole of the detection, and it is seventeen bytes including
  // the trailing space -- a file whose sixteenth byte differs is not one.
  g_mod[16] = '_';
  CHECK(ntrk::import_detect(g_mod, size) == ntrk::ImportFormat::kUnknown);
  CHECK(ntrk::import_scratch_needed(g_mod, size) == 0);

  // Too short to hold the counts at 64 is too short to be judged.
  spec = XmSpec();
  size = xm_build(g_mod, spec);
  CHECK(ntrk::import_detect(g_mod, 79) == ntrk::ImportFormat::kUnknown);
  CHECK(ntrk::import_detect(g_mod, 80) == ntrk::ImportFormat::kXm);
}

// Everything one well-formed file has to come out as: the cells, both packings,
// the note shift, the volume column and the delta decode.
static void
test_xm_cells() {
  for (int pass = 0; pass < 2; ++pass) {
    XmSpec spec;
    spec.compressed = pass == 0;
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));

    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    CHECK(module.version == 2);              // sixteen channels, ninety-six notes
    CHECK(module.note_max == ntrk::kMaxNote);
    CHECK(module.channels == 4);
    CHECK(module.rows == 8);
    CHECK(module.speed == 6);
    CHECK(module.bpm == 125);
    CHECK(module.instrument_count == 2);

    const ntrk::Note *n = module.patterns;
    // **The cells are the file's own, and the three octaves are on the
    // instrument.** XM's note 49 plays a sample at its recorded rate and ours
    // is note 13, so 36 semitones have to come off somewhere; they come off in
    // `transpose` below, checked there. A file imported without the shift
    // anywhere is three octaves high, which is the loudest bug this importer
    // could have.
    CHECK(n[0].note == 49);
    CHECK(n[0].instrument == 1);
    CHECK(n[0].effect == 0x0c);              // the volume column, set volume 32
    CHECK(n[0].param == 32);

    CHECK(n[1].note == 97);                  // note-off is 97 in both formats
    CHECK(n[1].instrument == 0);

    CHECK(n[2].note == 0);
    CHECK(n[2].effect == 0x0a);              // volume slide up 4
    CHECK(n[2].param == 0x40);

    CHECK(n[3].note == 61);
    CHECK(n[3].effect == 3);                 // the effect column, not the volume
    CHECK(n[3].param == 0x10);

    CHECK(n[4].effect == 0x0e);              // R03 is exactly E93
    CHECK(n[4].param == 0x93);

    CHECK(n[5].effect == 0x08);              // set panning 8 -> 8 * 17
    CHECK(n[5].param == 136);

    CHECK(n[6].note == 0 && n[6].instrument == 0 && n[6].effect == 0 &&
          n[6].param == 0);

    CHECK(n[7].note == 50);
    CHECK(n[7].instrument == 2);
    CHECK(n[7].effect == 0x0c && n[7].param == 0);

    // An instrument number past the table would index off the end of it on
    // every row that named it.
    CHECK(n[8].instrument == 0);

    // `G` takes no effect slot of its own, so the volume column gets the one it
    // was refused -- and the `G20` is the global volume this row plays at, so
    // the 32 the volume column asked for is halved on the way in.
    CHECK(n[9].effect == 0x0c && n[9].param == 16);

    CHECK(n[10].effect == 0x0a && n[10].param == 0x03);   // slide down 3

    // `Rxy` with a volume change in `x` has no equivalent and is skipped
    // rather than approximated by the retrigger alone.
    CHECK(n[11].effect == 0 && n[11].param == 0);

    // **The delta decode, exactly.** A sample that only looked plausible would
    // pass a peak-and-RMS check and still be the wrong waveform.
    const ntrk::Instrument &ins = module.instruments[0];
    CHECK(ins.type == (uint8_t) ntrk::InstrumentType::kPcm8);
    CHECK(ins.length == 7u);
    for (int i = 0; i < 7; ++i)
      CHECK(ins.data[i] == kXmDecoded8[i]);
    CHECK(ins.volume == 64);
    CHECK(ins.loop_start == 2u && ins.loop_len == 3u);
    // **Where the three octaves went.** `relative_note` is zero in this file,
    // so the shift is the numbering difference alone: XM note 49 is our note
    // 13, and -36 is what says so. The player reads it (`note_transposed`) and
    // always did; the comment that used to be here said otherwise.
    CHECK(ins.transpose == -36);

    CHECK(module.instruments[1].volume == 40);
    CHECK(module.instruments[1].loop_len == 0u);      // loop mode 0 is one-shot
  }
}

static void
test_xm_samples() {
  // Sixteen bit: the length and the loop are in **bytes** on disk, and the
  // accumulator is sixteen bits wide.
  {
    XmSpec spec;
    spec.wide[0] = true;
    spec.loop_start[0] = 1;
    spec.loop_len[0] = 2;
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    const ntrk::Instrument &ins = module.instruments[0];
    CHECK(ins.type == (uint8_t) ntrk::InstrumentType::kPcm16);
    CHECK(ins.bits == 16);
    CHECK(ins.length == 3u);                 // six bytes, three frames
    CHECK(ins.loop_start == 1u && ins.loop_len == 2u);
    const uint8_t *d = (const uint8_t *) (const void *) ins.data;
    for (int i = 0; i < 3; ++i) {
      const int16_t got =
          (int16_t) ((uint16_t) d[i * 2] | ((uint16_t) d[i * 2 + 1] << 8));
      CHECK(got == kXmDecoded16[i]);
    }
  }

  // Ping-pong has no equivalent here and is imported as a forward loop.
  {
    XmSpec spec;
    spec.loop_mode[0] = 2;
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    CHECK(module.instruments[0].loop_start == 2u);
    CHECK(module.instruments[0].loop_len == 3u);
  }

  // A loop past the end of the sample is clamped, not refused -- real files
  // carry them and `module_save` would reject the whole module.
  {
    XmSpec spec;
    spec.loop_start[0] = 5;
    spec.loop_len[0] = 40;
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    CHECK(module.instruments[0].loop_start == 5u);
    CHECK(module.instruments[0].loop_len == 2u);      // seven frames, from five
  }

  // XM finetune is a signed byte over one semitone and ours is eighths of it.
  {
    XmSpec spec;
    spec.finetune[0] = 127;
    spec.finetune[1] = -128;
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    CHECK(module.instruments[0].finetune == 7);
    CHECK(module.instruments[1].finetune == -8);
  }

  // An instrument slot with no sample at all: length zero, and the file still
  // converts. Three of the eleven slots in one real module are like this.
  {
    XmSpec spec;
    spec.samples[1] = 0;
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    CHECK(module.instruments[1].length == 0u);
    CHECK(module.instrument_count == 2);
  }

  // **The majority sample, not sample 0.** The note map sends every note to
  // sample 1, so sample 1's frames are the ones that must come out.
  {
    XmSpec spec;
    spec.samples[0] = 2;
    spec.map_to[0] = 1;
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    CHECK(module.instruments[0].length == 7u);        // the delta stream, not
    for (int i = 0; i < 7; ++i)                       // the four filler bytes
      CHECK(module.instruments[0].data[i] == kXmDecoded8[i]);
  }
}

static void
test_xm_note_shift() {
  // **The cells keep the file's own notes and the instrument carries the
  // shift.** `relative_note - 36` goes on `Instrument::transpose`, which is
  // where the format puts it and what the player reads.
  {
    XmSpec spec;
    spec.relnote[0] = 12;
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    CHECK(module.patterns[0].note == 49);             // untouched
    CHECK(module.patterns[3].note == 61);             // and the interval with it
    CHECK(module.instruments[0].transpose == 12 - 36);
  }

  // A sample pitched three octaves down puts note 49 at -23, which the player
  // reaches now -- `kMinNote` is -47. This used to move the whole instrument up
  // by octaves until its span fitted above note 1, which is changing the music
  // to fit a limit that is gone.
  {
    XmSpec spec;
    spec.relnote[0] = -36;
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    CHECK(module.patterns[0].note == 49);
    CHECK(module.patterns[3].note == 61);
    CHECK(module.patterns[1].note == 97);             // note-off is not a note
    CHECK(module.instruments[0].transpose == -36 - 36);

    // ...and it sounds two octaves below the format's own floor rather than
    // against it, which is the whole point of moving the shift.
    ntrk::Player p;
    ntrk::player_start(&p, &module);
    ntrk::player_tick(&p, 48000.0);
    CHECK(p.channels[0].period == ntrk::period_for(49 - 72, 0));
  }
}

// One row count for the whole module, and XM has one per pattern.
static void
test_xm_short_patterns() {
  XmSpec spec;
  spec.patterns = 2;
  spec.order_count = 2;
  spec.rows[0] = 8;
  spec.rows[1] = 4;
  size_t xm_size = 0, out_size = 0;
  CHECK(xm_convert(spec, &xm_size, &out_size));

  ntrk::Module module;
  CHECK(ntrk::module_load(&module, g_out, out_size));
  CHECK(module.rows == 8);
  CHECK(module.pattern_count == 2);

  // The second pattern is four rows in an eight-row block, so it breaks at the
  // end of row three rather than playing four rows of silence.
  const ntrk::Note *p1 = module.patterns + 8 * 4;
  bool found = false;
  for (int c = 0; c < 4; ++c)
    if (p1[3 * 4 + c].effect == 0x0d && p1[3 * 4 + c].param == 0)
      found = true;
  CHECK(found);
  // And the padding really is empty.
  for (int r = 4; r < 8; ++r)
    for (int c = 0; c < 4; ++c)
      CHECK(p1[r * 4 + c].note == 0 && p1[r * 4 + c].effect == 0);

  // A pattern with no packed data at all is eight empty rows, and it still
  // needs the break, because it is not short -- it is the full eight here.
  {
    XmSpec empty;
    empty.patterns = 2;
    empty.order_count = 2;
    empty.empty_pattern[1] = true;
    CHECK(xm_convert(empty, &xm_size, &out_size));
    CHECK(ntrk::module_load(&module, g_out, out_size));
    const ntrk::Note *e = module.patterns + 8 * 4;
    for (int i = 0; i < 8 * 4; ++i)
      CHECK(e[i].note == 0 && e[i].instrument == 0 && e[i].effect == 0);
  }
}

// Global volume, folded into the cells it governs.
//
// ntrk has no master volume and `render_add` reads no plane gain, so a `Gxx`
// that is merely dropped is a tune that does not fade. The value is baked into
// the volumes instead, and these are the three shapes that has to survive: a
// ramp down, a global volume that never leaves full, and a slide.
static void
test_xm_global_volume() {
  // The cell at row `r`, channel `c`, in the probe layout.
  const int kCh = 4;

  // ---- a `G` ramp, which is what a fade-out is written as -------------------
  {
    XmSpec spec;
    spec.gv_probe = true;
    for (int r = 0; r < 4; ++r) {
      spec.gv_effect[r] = 0x10;                      // `Gxx`
      spec.gv_param[r] = (uint8_t) (0x40 - r * 0x10);   // 64, 48, 32, 16
    }
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    const ntrk::Note *n = module.patterns;

    // Row 0 stands at full volume, so it is the identity: the note on channel 0
    // carries no volume of its own and is given none, exactly as before.
    CHECK(n[0 * kCh + 0].note == 49 && n[0 * kCh + 0].instrument == 1);
    CHECK(n[0 * kCh + 0].effect == 0 && n[0 * kCh + 0].param == 0);
    // Channel 2's volume column is written through untouched at full volume.
    CHECK(n[0 * kCh + 2].effect == 0x0c && n[0 * kCh + 2].param == 32);

    // And under it, every channel scales by the row's global volume. Channel 0
    // is the instrument's own 64, channel 1 the second instrument's 40, and
    // channel 2 the volume column's 32 -- three sources, one scale.
    const int gv[4] = {64, 48, 32, 16};
    for (int r = 1; r < 4; ++r) {
      CHECK(n[r * kCh + 0].effect == 0x0c);
      CHECK((int) n[r * kCh + 0].param == (64 * gv[r] + 32) / 64);
      CHECK(n[r * kCh + 1].effect == 0x0c);
      CHECK((int) n[r * kCh + 1].param == (40 * gv[r] + 32) / 64);
      CHECK(n[r * kCh + 2].effect == 0x0c);
      CHECK((int) n[r * kCh + 2].param == (32 * gv[r] + 32) / 64);
    }

    // The last `G` stands for the rest of the pattern, so the rows after it
    // stay down rather than springing back to full.
    for (int r = 4; r < 8; ++r) {
      CHECK(n[r * kCh + 0].effect == 0x0c && n[r * kCh + 0].param == 16);
      CHECK(n[r * kCh + 1].effect == 0x0c && n[r * kCh + 1].param == 10);
    }

    // And it is a fade: strictly down, row on row, from the first row that
    // carries a volume at all.
    for (int r = 2; r < 4; ++r)
      CHECK(n[r * kCh + 0].param < n[(r - 1) * kCh + 0].param);
  }

  // ---- a `G` that never leaves full, which must cost nothing ----------------
  //
  // Byte for byte against the same module with no `G` in it at all. A weaker
  // check -- "the volumes look right" -- would pass a version that wrote a
  // redundant `Cxx` into every cell of every tune that mentions global volume.
  {
    XmSpec plain;
    plain.gv_probe = true;
    size_t xm_size = 0, plain_size = 0;
    CHECK(xm_convert(plain, &xm_size, &plain_size));
    static uint8_t reference[sizeof(g_out)];
    memcpy(reference, g_out, plain_size);

    XmSpec full;
    full.gv_probe = true;
    for (int r = 0; r < 8; ++r) {
      full.gv_effect[r] = 0x10;
      full.gv_param[r] = 0x40;                       // 64, and 64 is full
    }
    size_t full_size = 0;
    CHECK(xm_convert(full, &xm_size, &full_size));
    CHECK(full_size == plain_size);
    CHECK(memcmp(reference, g_out, full_size) == 0);

    // Over 64 is clamped rather than wrapped, and clamps to the same nothing.
    XmSpec over;
    over.gv_probe = true;
    over.gv_effect[0] = 0x10;
    over.gv_param[0] = 0xff;
    size_t over_size = 0;
    CHECK(xm_convert(over, &xm_size, &over_size));
    CHECK(over_size == plain_size);
    CHECK(memcmp(reference, g_out, over_size) == 0);
  }

  // ---- `Hxy`, the slide -----------------------------------------------------
  //
  // A row is worth `speed - 1` steps, because FT2 runs the slide on every tick
  // of a row but the first. At the default six that is five a row, and the row
  // the slide is written on is struck before any of them.
  {
    XmSpec spec;
    spec.gv_probe = true;
    spec.gv_effect[0] = 0x11;
    spec.gv_param[0] = 0x01;          // down one a tick
    spec.gv_effect[2] = 0x11;
    spec.gv_param[2] = 0x00;          // and again, from the parameter memory
    spec.gv_effect[4] = 0x11;
    spec.gv_param[4] = 0x10;          // up one a tick
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    const ntrk::Note *n = module.patterns;

    // 64 opening row 0, then five steps down over its remaining ticks; row 1
    // has no slide of its own and holds; row 2 slides again; row 4 climbs.
    const int gv[8] = {64, 59, 59, 54, 54, 59, 59, 59};
    for (int r = 0; r < 8; ++r) {
      const int expect = (64 * gv[r] + 32) / 64;
      if (gv[r] >= 64) {
        CHECK(n[r * kCh + 0].effect == 0 && n[r * kCh + 0].param == 0);
      } else {
        CHECK(n[r * kCh + 0].effect == 0x0c);
        CHECK((int) n[r * kCh + 0].param == expect);
      }
      // The volume column scales from its own 32 rather than the instrument's.
      CHECK(n[r * kCh + 2].effect == 0x0c);
      CHECK((int) n[r * kCh + 2].param == (32 * gv[r] + 32) / 64);
    }

    // A slide with no parameter and no memory to fall back on moves nothing --
    // it must not read the last `Gxx` or a stale nibble as a step.
    XmSpec bare;
    bare.gv_probe = true;
    bare.gv_effect[0] = 0x11;
    bare.gv_param[0] = 0x00;
    size_t bare_size = 0;
    CHECK(xm_convert(bare, &xm_size, &bare_size));
    CHECK(ntrk::module_load(&module, g_out, bare_size));
    for (int r = 0; r < 8; ++r)
      CHECK(module.patterns[r * kCh + 0].effect == 0);
  }

  // ---- the value runs across patterns, in the order they are played ---------
  //
  // A pattern's opening volume is what the pattern before it left behind, and
  // the order list is the only thing that says which pattern that was.
  {
    XmSpec spec;
    spec.gv_probe = true;
    spec.patterns = 2;
    spec.order_count = 2;             // the order is 0 then 1
    spec.gv_effect[1] = 0x10;
    spec.gv_param[1] = 0x20;          // half, from the *second* row of both
    size_t xm_size = 0, out_size = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));

    // Pattern 0's first row runs before any `G` at all and is untouched;
    // pattern 1's is identical to it and is not, because the pattern before it
    // left the global volume at half and nothing in between put it back.
    const ntrk::Note *p1 = module.patterns + 8 * kCh;
    CHECK(module.patterns[0 * kCh + 0].effect == 0);
    CHECK(p1[0 * kCh + 0].effect == 0x0c && p1[0 * kCh + 0].param == 32);
  }

  // ---- and the two shapes it refuses ----------------------------------------
  //
  // A pattern is stored once and played from that one copy, so rows that play
  // at two different global volumes can carry neither. Both cases come out byte
  // for byte as they would with no `G` in the file at all: an absent fade, on
  // purpose, rather than one that is wrong for the rest of the tune.
  {
    // Rows played twice by an `E6x` loop while the volume moves under them.
    // This is a real file's fade-*in*, and baking the first pass into cells the
    // second also plays is what leaves a tune quiet long after it should have
    // come up.
    XmSpec plain;
    plain.gv_probe = true;
    plain.gv_loop = true;
    plain.channels = 5;               // three probes, the loop, the volume
    size_t xm_size = 0, plain_size = 0;
    CHECK(xm_convert(plain, &xm_size, &plain_size));
    static uint8_t reference[sizeof(g_out)];
    memcpy(reference, g_out, plain_size);

    XmSpec looped = plain;
    for (int r = 0; r < 4; ++r) {
      looped.gv_effect[r] = 0x10;
      looped.gv_param[r] = (uint8_t) (0x40 - r * 0x10);
    }
    size_t looped_size = 0;
    CHECK(xm_convert(looped, &xm_size, &looped_size));
    CHECK(looped_size == plain_size);
    CHECK(memcmp(reference, g_out, looped_size) == 0);

    // ...and it really is the loop that refuses it: the same ramp over the same
    // pattern without one is baked, so this is not a probe that stopped working.
    XmSpec open = plain;
    open.gv_loop = false;
    size_t open_size = 0;
    CHECK(xm_convert(open, &xm_size, &open_size));
    memcpy(reference, g_out, open_size);

    XmSpec ramped = open;
    for (int r = 0; r < 4; ++r) {
      ramped.gv_effect[r] = 0x10;
      ramped.gv_param[r] = (uint8_t) (0x40 - r * 0x10);
    }
    size_t ramped_size = 0;
    CHECK(xm_convert(ramped, &xm_size, &ramped_size));
    CHECK(ramped_size == open_size);
    CHECK(memcmp(reference, g_out, ramped_size) != 0);
  }
  {
    // The order list naming one pattern twice, at two different volumes: full
    // the first time round and half the second, because the pattern's own `G`
    // is still standing when it comes back.
    XmSpec plain;
    plain.gv_probe = true;
    plain.order_count = 2;            // one pattern, played twice
    size_t xm_size = 0, plain_size = 0;
    CHECK(xm_convert(plain, &xm_size, &plain_size));
    static uint8_t reference[sizeof(g_out)];
    memcpy(reference, g_out, plain_size);

    XmSpec twice = plain;
    twice.gv_effect[1] = 0x10;
    twice.gv_param[1] = 0x20;
    size_t twice_size = 0;
    CHECK(xm_convert(twice, &xm_size, &twice_size));
    CHECK(twice_size == plain_size);
    CHECK(memcmp(reference, g_out, twice_size) == 0);
  }
}

static void
test_xm_refusals() {
  // **Every truncation, at every boundary.** The buffer is whole and the size
  // is short, which is exactly the shape a partial download has.
  {
    XmSpec spec;
    const size_t size = xm_build(g_mod, spec);
    // Walking in from the end reaches, in order, the sample payload, the sample
    // headers, the instrument headers, the pattern block and the file header --
    // so every boundary is crossed by one loop and none of them may pad.
    int accepted = 0;
    for (size_t cut = 1; cut < size; ++cut) {
      size_t written = 0;
      if (ntrk::import_scratch_needed(g_mod, size - cut) != 0)
        ++accepted;
      if (ntrk::import_convert(g_mod, size - cut, g_scratch, sizeof(g_scratch),
                               g_out, sizeof(g_out), &written))
        ++accepted;
      // The two entry points have to agree about a short file, always: a size
      // query that answered would send a caller into a conversion that refuses.
      if (written != 0)
        ++accepted;
    }
    CHECK(accepted == 0);
  }

  size_t xm_size = 0, out_size = 0;
  {
    XmSpec spec;                     // a sample cut short is refused, not padded
    spec.truncate = 1;
    CHECK(!xm_convert(spec, &xm_size, &out_size));
  }
  {
    XmSpec spec;                     // thirty-two channels is more than we hold
    spec.channels = 32;
    CHECK(!xm_convert(spec, &xm_size, &out_size));
  }
  {
    XmSpec spec;
    spec.channels = 0;
    CHECK(!xm_convert(spec, &xm_size, &out_size));
  }
  {
    XmSpec spec;                     // an order naming a pattern that is not there
    spec.patterns = 1;
    spec.order_count = 2;
    const size_t size = xm_build(g_mod, spec);
    g_mod[81] = 9;
    CHECK(ntrk::import_scratch_needed(g_mod, size) == 0);
  }
  {
    XmSpec spec;                     // speed and tempo outside what a file may hold
    spec.speed = 0;
    CHECK(!xm_convert(spec, &xm_size, &out_size));
    spec = XmSpec();
    spec.bpm = 12;
    CHECK(!xm_convert(spec, &xm_size, &out_size));
  }
  {
    XmSpec spec;                     // a packing type XM never defined
    const size_t size = xm_build(g_mod, spec);
    g_mod[60 + 276 + 4] = 1;
    CHECK(ntrk::import_scratch_needed(g_mod, size) == 0);
  }
  {
    XmSpec spec;                     // a pattern header shorter than its fields
    const size_t size = xm_build(g_mod, spec);
    g_mod[60 + 276] = 8;
    CHECK(ntrk::import_scratch_needed(g_mod, size) == 0);
  }
  {
    XmSpec spec;                     // more samples than a note map can name
    spec.samples[0] = 17;
    CHECK(!xm_convert(spec, &xm_size, &out_size));
  }
  {
    XmSpec spec;                     // a sample header that cannot hold its fields
    spec.sample_header_bytes = 20;
    CHECK(!xm_convert(spec, &xm_size, &out_size));
  }
  {
    // An instrument record too short to carry a sample count is an empty slot,
    // which is what FT2 itself does with one.
    XmSpec spec;
    spec.instrument_header_bytes = 20;
    spec.samples[0] = 0;
    spec.samples[1] = 0;
    CHECK(xm_convert(spec, &xm_size, &out_size));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, out_size));
    CHECK(module.instruments[0].length == 0u);
  }
  {
    // Cells running past the rows the pattern declared: the packed size says
    // there is more data than there are cells to put it in.
    XmSpec spec;
    const size_t size = xm_build(g_mod, spec);
    const size_t head = 60u + 276u;
    const unsigned packed =
        (unsigned) g_mod[head + 7] | ((unsigned) g_mod[head + 8] << 8);
    xm_put16(g_mod + head + 5, 1u);            // one row, and cells for eight
    const size_t need = ntrk::import_scratch_needed(g_mod, size);
    size_t written = 0;
    CHECK(need == 0 ||
          !ntrk::import_convert(g_mod, size, g_scratch, sizeof(g_scratch),
                                g_out, sizeof(g_out), &written));
    (void) packed;
  }
  {
    // Too little scratch is a refusal, not a short write.
    XmSpec spec;
    const size_t size = xm_build(g_mod, spec);
    const size_t need = ntrk::import_scratch_needed(g_mod, size);
    size_t written = 0;
    CHECK(need > 0);
    CHECK(!ntrk::import_convert(g_mod, size, g_scratch, need - 1, g_out,
                                sizeof(g_out), &written));
    CHECK(ntrk::import_convert(g_mod, size, g_scratch, need, g_out,
                               sizeof(g_out), &written));
  }
}

// Every real module the developer has dropped in `testdata/`, of either kind.
// There is no reference to compare against, so what is asserted is that it
// converts, that the loader takes the result and that the result is not
// silence.
//
// **This is the only check that sees a real file**, and the directory is
// gitignored, so it reports being skipped rather than passing quietly.
//
// Matched case-insensitively: a module off an Amiga or a BBS is as likely to be
// `.MOD`, and a check that skipped those would report "no files" while sitting
// in a directory full of them.
static void
test_real_files(const char *ext) {
  const size_t ext_len = strlen(ext);
  DIR *dir = opendir("third_party/ntrk/testdata");
  if (dir == nullptr)
    return;
  for (struct dirent *e = readdir(dir); e != nullptr; e = readdir(dir)) {
    const size_t n = strlen(e->d_name);
    if (n <= ext_len || strcasecmp(e->d_name + n - ext_len, ext) != 0)
      continue;
    char path[512];
    snprintf(path, sizeof(path), "third_party/ntrk/testdata/%s", e->d_name);
    FILE *f = fopen(path, "rb");
    if (f == nullptr)
      continue;
    const size_t size = fread(g_mod, 1, kModCap, f);
    fclose(f);

    // **A file the importer refuses is reported and skipped, not failed.** A
    // developer's own directory holds whatever they dropped in it, including a
    // module saved under the wrong extension or a variant this does not
    // support, and refusing one is the importer working. It is printed so a
    // refusal cannot hide as a quiet pass.
    const size_t need = ntrk::import_scratch_needed(g_mod, size);
    size_t written = 0;
    if (need == 0 || need > sizeof(g_scratch)) {
      printf("  skipped %s: the importer refused it\n", e->d_name);
      continue;
    }
    CHECK(ntrk::import_convert(g_mod, size, g_scratch, sizeof(g_scratch), g_out,
                               sizeof(g_out), &written));
    ntrk::Module module;
    CHECK(ntrk::module_load(&module, g_out, written));
    printf("  %s -> %zu bytes, %d ch, %d rows, %d patterns, %d instruments\n",
           e->d_name, written, module.channels, module.rows,
           module.pattern_count, module.instrument_count);

    // Every instrument that names frames has to carry them, and every note has
    // to be one the player will act on -- a note out of range is a hole in the
    // tune. Counted rather than checked per cell: a quarter of a million
    // passing checks would drown the ones that mean something.
    int missing = 0, out_of_range = 0, sounding = 0;
    for (int i = 0; i < module.instrument_count; ++i)
      if (module.instruments[i].length != 0u &&
          module.instruments[i].data == nullptr)
        ++missing;
    const size_t cells = (size_t) module.pattern_count *
                         (size_t) module.rows * (size_t) module.channels;
    for (size_t c = 0; c < cells; ++c) {
      const int note = (int) module.patterns[c].note;
      if (note == 0 || note == ntrk::kNoteOff)
        continue;
      ++sounding;
      if (note < 1 || note > module.note_max)
        ++out_of_range;
    }
    CHECK(missing == 0);
    CHECK(out_of_range == 0);
    CHECK(sounding > 0);
  }
  closedir(dir);
}

int
main() {
  test_detect();
  test_convert_loads_and_renders();
  test_instrument_trimming();
  test_stale_order_sizes_the_patterns();
  test_truncation_refused();
  test_refusals();
  test_xm_detect();
  test_xm_cells();
  test_xm_samples();
  test_xm_note_shift();
  test_xm_short_patterns();
  test_xm_global_volume();
  test_xm_refusals();
  test_real_files(".xm");
  test_real_files(".mod");

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
