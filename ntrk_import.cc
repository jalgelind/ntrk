// ntrk_import -- see ntrk_import.h for the interface and why it is shaped so.
//
// Everything 1988 put in a `.mod` that a player should not be doing at load --
// big-endian fields, lengths in words, periods instead of notes, thirty-one
// instrument slots whether or not the tune uses them -- is undone here, once.
//
// This is a port rather than a re-derivation: every decision below was made
// first by an offline Python tool that had already shipped working files, and
// the two were held byte-identical by a test until that tool was retired with
// version 1 of the format.
//
// **A truncated sample is refused, never padded**, which is where this parted
// company with that tool. It padded and warned a person who was watching; this
// is a library a browser hands an arbitrary file to, and inventing the tail of
// a sample is a guess.
//
// FastTracker 2's `.xm` is below the `.mod` half, and it is the same job with a
// different set of 1994 decisions to undo: delta-encoded samples, a per-pattern
// row count where the format has one, a note space three octaves above ours,
// and a second effect column. There is no reference tool to compare against for
// it, so the checks are the synthetic files in `test_ntrk_import.cc` plus a
// render of the real ones -- see README.md.

#include "ntrk_import.h"

#include <string.h>

namespace ntrk {
namespace {

// ProTracker's finetune-0 periods, three octaves. A note in the output is a
// one-based index into this.
const int kModPeriods[36] = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};

// The 31-instrument variants. **The 15-instrument format that predates them has
// no signature at all and is deliberately not read**: guessing at a headerless
// file is how an importer silently produces noise.
struct ModSignature {
  char tag[4];
  int channels;
};

const ModSignature kModSignatures[] = {
    {{'M', '.', 'K', '.'}, 4}, {{'M', '!', 'K', '!'}, 4},
    {{'F', 'L', 'T', '4'}, 4}, {{'4', 'C', 'H', 'N'}, 4},
    {{'6', 'C', 'H', 'N'}, 6}, {{'8', 'C', 'H', 'N'}, 8},
};

const size_t kModHeaderBytes = 1084;   // 20 title + 31*30 instruments + 134
const int kModInstruments = 31;
const int kModRows = 64;

// **The header alone, judged.** Both public entry points need exactly this much
// before they can say anything, and deriving it twice is how a size query and
// the conversion it sizes end up disagreeing about how big the patterns are.
struct ModGeometry {
  int channels;
  int rows;
  int pattern_count;
  int order_count;
  int restart;
  size_t pattern_bytes;
};

int
mod_channels_for(const uint8_t *data) {
  const int count = (int) (sizeof(kModSignatures) / sizeof(kModSignatures[0]));
  for (int i = 0; i < count; ++i) {
    const ModSignature &sig = kModSignatures[i];
    if (data[1080] == (uint8_t) sig.tag[0] && data[1081] == (uint8_t) sig.tag[1] &&
        data[1082] == (uint8_t) sig.tag[2] && data[1083] == (uint8_t) sig.tag[3])
      return sig.channels;
  }
  return 0;
}

// Big-endian, because that is what an Amiga wrote. Every other integer this
// project reads is little-endian, which is precisely why it is spelled out.
uint16_t
read_be16(const uint8_t *p) {
  return (uint16_t) (((uint16_t) p[0] << 8) | (uint16_t) p[1]);
}

// Nearest table entry, one-based; 0 when the cell has no note.
//
// Nearest rather than exact because some trackers write the *finetuned* period
// into the pattern rather than the nominal one, and an exact match would drop
// those notes silently. A period nowhere near the table is not a note: 8 is
// about a third of a semitone at the top of the range, well outside any
// finetune, so anything further out is left as an empty cell.
uint8_t
mod_note_for(int period) {
  if (period == 0)
    return 0;
  int best = 0;
  int best_d = -1;
  for (int i = 0; i < 36; ++i) {
    const int d = period > kModPeriods[i] ? period - kModPeriods[i]
                                          : kModPeriods[i] - period;
    if (best_d < 0 || d < best_d) {
      best = i + 1;
      best_d = d;
    }
  }
  return best_d >= 0 && best_d <= 8 ? (uint8_t) best : (uint8_t) 0;
}

bool
mod_geometry(const uint8_t *data, size_t size, ModGeometry *geom) {
  if (data == nullptr || size < kModHeaderBytes)
    return false;
  const int channels = mod_channels_for(data);
  if (channels < 1 || channels > 8)
    return false;

  const int song_length = (int) data[950];
  if (song_length < 1 || song_length > 128)
    return false;

  // **Count the patterns from the *whole* order table, before truncating it.**
  // A `.mod`'s pattern block is sized by the highest pattern number anywhere in
  // the 128-byte table, and stale entries past `song_length` are common. Take
  // the maximum over the truncated list instead and the block is under-counted:
  // the read stops early, every sample offset after it is wrong by that many
  // bytes, and the tune imports "successfully" as noise.
  int highest = 0;
  for (int i = 0; i < 128; ++i)
    if ((int) data[952 + i] > highest)
      highest = (int) data[952 + i];
  const int pattern_count = highest + 1;

  // The restart byte is 127 in most modules, meaning "no restart"; anything
  // past the order list would make the player loop to nothing.
  int restart = (int) data[951];
  if (restart >= song_length)
    restart = 0;

  const size_t pattern_bytes = (size_t) pattern_count * (size_t) kModRows *
                               (size_t) channels * 4u;
  if (size - kModHeaderBytes < pattern_bytes)
    return false;

  geom->channels = channels;
  geom->rows = kModRows;
  geom->pattern_count = pattern_count;
  geom->order_count = song_length;
  geom->restart = restart;
  geom->pattern_bytes = pattern_bytes;
  return true;
}

// ---- FastTracker 2 `.xm` -----------------------------------------------------

const char kXmTag[] = "Extended Module: ";      // 17 bytes, at offset 0
const size_t kXmTagBytes = 17;

// Through the eight `uint16_t` counts at 64. The order table follows at 80 and
// its length is the song length, so nothing past here is a fixed offset -- the
// pattern block starts at `60 + header_size`, wherever that lands.
const size_t kXmMinHeader = 80;

const size_t kXmPatternHeaderMin = 9;   // u32 length, u8 packing, u16 rows, u16 packed
const int kXmMaxSamples = 16;           // one instrument's note map is 4 bits wide
const size_t kXmSampleHeaderMin = 40;   // through `reserved`; the name follows
const int kXmNoteOff = 97;              // ours is 97 too, so this is an identity

// **XM's note space sits three octaves above ours, and that is arithmetic
// rather than taste.** An XM sample with `relative_note == 0` plays at its
// recorded rate on note 49; a ProTracker sample plays at (near enough) its
// recorded rate on period 428, which is note 13 -- and ntrk is a period machine
// whose notes 1..36 are ProTracker's by construction. So 49 - 13 = 36 has to
// come off every note or the whole tune imports three octaves high.
//
// **That residue is gone, and the arithmetic above is why it is worth saying.**
// It used to be the 8287 Hz the Amiga clock put at period 428 against XM's
// 8363: a uniform 15.8 cents flat across the whole module. The player's anchor
// is concert pitch now rather than ProTracker's 856, so period 428's equivalent
// carries 8372.02 Hz — 1.87 cents *sharp* of 8363, and that is XM's own
// rounding, since a true C-4 times 32 is 8372.02. An XM imports at the pitch it
// was written at, to within the format's own error.
//
// **The linear frequency flag is therefore not read at all.** Both of XM's
// frequency tables put note *n* at the same semitone; they differ in what a
// portamento does between two notes, and a portamento is the player's, not the
// importer's.
const int kXmNoteShift = 36;

// The geometry, judged from the header and the pattern headers alone -- the
// same reason `ModGeometry` exists. `sample_bytes` is what the decoded frames
// come to, which is the other half of what the caller has to allocate, so the
// size query and the conversion it sizes cannot disagree about either.
struct XmGeometry {
  int channels;
  int rows;                 // the longest pattern; the short ones are padded
  int pattern_count;
  int order_count;
  int restart;
  int speed;
  int bpm;
  int instrument_count;
  size_t pattern_at;        // first pattern header
  size_t instrument_at;     // first instrument header
  size_t cell_bytes;        // the `Note` array
  size_t sample_bytes;      // the decoded frames
};

bool
xm_signature(const uint8_t *data) {
  for (size_t i = 0; i < kXmTagBytes; ++i)
    if (data[i] != (uint8_t) kXmTag[i])
      return false;
  return true;
}

// Which of an instrument's samples the note map points at most often.
//
// **The majority rather than sample 0**, because an instrument whose map sends
// eighty of ninety-six notes to sample 1 is an instrument whose sound is sample
// 1; taking the first would import the wrong one and sound like a bug in the
// player. See the note at the head of `xm_instruments` for what is lost.
int
xm_chosen_sample(const uint8_t *map, bool have_map, int sample_count) {
  if (!have_map || sample_count <= 1)
    return 0;
  int count[kXmMaxSamples];
  for (int i = 0; i < kXmMaxSamples; ++i)
    count[i] = 0;
  for (int i = 0; i < 96; ++i) {
    const int s = (int) map[i];
    if (s < sample_count)
      ++count[s];
  }
  int best = 0;
  for (int i = 1; i < sample_count; ++i)
    if (count[i] > count[best])
      best = i;
  return best;
}

// The instrument block, walked once. `out_ins`, `out_rel` and `dest` are all
// optional and all three are null on the sizing pass, which is what keeps the
// two passes from being two different walks -- the same arithmetic decides how
// big the decoded blob is and where inside it every instrument lands.
//
// **One sample an instrument.** XM lets a note map choose between sixteen; ntrk
// has no such indirection, so the majority sample is taken and the others are
// dropped. Splitting an instrument in two and renumbering the cells that reach
// it would fit -- `kMaxInstruments` is 64 -- but nothing in the corpus needs it:
// the one multi-sample instrument there has a map of ninety-six zeroes.
//
// ponytail: majority sample per instrument, split into separate ntrk
// instruments if a real file ever maps notes to more than one.
bool
xm_instruments(const uint8_t *data, size_t size, size_t at, int count,
               Instrument *out_ins, int8_t *out_rel, uint8_t *dest,
               size_t *bytes) {
  size_t written = 0;
  for (int i = 0; i < count; ++i) {
    // Every test below subtracts from what is left rather than adding to an
    // offset, so this is the invariant the rest of them rest on.
    if (at > size || size - at < 4u)
      return false;
    const uint32_t header_bytes = read_u32(data + at);
    if (header_bytes < 4u || (size_t) (size - at) < (size_t) header_bytes)
      return false;

    // The sample count is at +27, so a record shorter than that does not
    // contain one. FT2 writes 29 for an empty slot and 263 for a used one;
    // treating a short record as empty is what the tracker itself does, and it
    // is the difference between reading a field and inventing it.
    const int sample_count =
        header_bytes >= 29u ? (int) read_u16(data + at + 27) : 0;
    if (sample_count == 0) {
      if (out_rel != nullptr)
        out_rel[i] = 0;
      at += (size_t) header_bytes;
      continue;                    // a slot with no sample: length 0, data null
    }
    if (sample_count > kXmMaxSamples)
      return false;
    if (header_bytes < 33u)
      return false;                // no room for the sample header size at +29

    const uint32_t sample_header_bytes = read_u32(data + at + 29);
    if (sample_header_bytes < (uint32_t) kXmSampleHeaderMin)
      return false;
    // The note map is 96 bytes at +33. A record too short to hold one is not
    // refused -- it simply names no mapping, and one sample is what that means.
    const bool have_map = header_bytes >= 33u + 96u;
    const int chosen =
        xm_chosen_sample(data + at + 33, have_map, sample_count);

    // **All the sample headers, then all the payloads.** The one we want sits
    // behind the payloads of the samples we are dropping, so every length has
    // to be read whether or not it is used.
    const size_t headers_at = at + (size_t) header_bytes;
    // By division, so the product cannot wrap however large the file claims the
    // headers are.
    if ((size - headers_at) / (size_t) sample_count <
        (size_t) sample_header_bytes)
      return false;
    const size_t payload_at =
        headers_at + (size_t) sample_count * (size_t) sample_header_bytes;

    size_t before = 0;             // payload bytes ahead of the chosen sample
    size_t total = 0;
    uint32_t chosen_bytes = 0;
    for (int s = 0; s < sample_count; ++s) {
      const uint32_t length = read_u32(
          data + headers_at + (size_t) s * (size_t) sample_header_bytes);
      // **Truncation is refused, never padded**, exactly as in the `.mod` half.
      if ((size - payload_at) - total < (size_t) length)
        return false;
      if (s < chosen)
        before += (size_t) length;
      else if (s == chosen)
        chosen_bytes = length;
      total += (size_t) length;
    }

    const uint8_t *head =
        data + headers_at + (size_t) chosen * (size_t) sample_header_bytes;
    const uint8_t type = head[14];
    // Bit 4 is the width and bits 0..1 the loop mode; 3 is not a mode.
    const bool wide = (type & 0x10u) != 0u;
    const int loop_mode = (int) (type & 0x03u);
    const uint32_t stride = wide ? 2u : 1u;

    // **Length, loop start and loop length are all in bytes on disk**, for
    // sixteen-bit samples too. Read as frames a 16-bit sample is twice as long
    // as it is and its loop is in the wrong place.
    const uint32_t frames = chosen_bytes / stride;
    uint32_t loop_start = read_u32(head + 4) / stride;
    uint32_t loop_len = read_u32(head + 8) / stride;
    if (loop_mode == 0) {
      loop_start = 0;
      loop_len = 0;
    }
    // Clamped rather than refused, the same choice the `.mod` half makes: a
    // loop a frame past the end is common and `module_save` would reject the
    // whole file for it.
    if (loop_len > 0u &&
        (loop_start >= frames || loop_len > frames - loop_start)) {
      loop_len = loop_start < frames ? frames - loop_start : 0u;
      if (loop_len == 0u)
        loop_start = 0u;
    }

    if (out_rel != nullptr)
      out_rel[i] = (int8_t) head[16];

    if (dest != nullptr && out_ins != nullptr) {
      // **Delta decoded, and the accumulator is unsigned on purpose.** A sample
      // that wraps is the format working as designed rather than a corrupt
      // file, and signed overflow is undefined -- so the sum is taken in
      // `uint8_t`/`uint16_t` and reinterpreted, which is defined and is the
      // same bit pattern.
      const uint8_t *src = data + payload_at + before;
      uint8_t *out = dest + written;
      if (wide) {
        uint16_t acc = 0;
        for (uint32_t f = 0; f < frames; ++f) {
          acc = (uint16_t) (acc + read_u16(src + (size_t) f * 2u));
          out[(size_t) f * 2u + 0] = (uint8_t) (acc & 0xffu);
          out[(size_t) f * 2u + 1] = (uint8_t) ((acc >> 8) & 0xffu);
        }
      } else {
        uint8_t acc = 0;
        for (uint32_t f = 0; f < frames; ++f) {
          acc = (uint8_t) (acc + src[f]);
          out[f] = acc;
        }
      }

      Instrument &ins = out_ins[i];
      ins.data = (const int8_t *) (const void *) out;
      ins.length = frames;
      ins.loop_start = loop_start;
      ins.loop_len = loop_len;
      ins.volume = head[12] > 64u ? (uint8_t) 64 : head[12];
      // XM finetune is a signed byte over one semitone; ours is eighths of a
      // semitone over the same range, so sixteen XM units are one of ours and
      // the division is exact rather than a rounding.
      ins.finetune = (int8_t) ((int) (int8_t) head[13] / 16);
      ins.type =
          (uint8_t) (wide ? InstrumentType::kPcm16 : InstrumentType::kPcm8);
      ins.bits = wide ? (uint8_t) 16 : (uint8_t) 8;
      // **`transpose` carries `relative_note`, and the -36 with it.** XM note 49
      // is C-4 and ours is note 13, so the numberings differ by 36 before any
      // sample's own shift is applied; both go here.
      //
      // This used to be folded into the cells instead, because with the
      // player's old floor at note 1 an XM's bottom three octaves landed on
      // notes -35..0 and could not be *stored* — a cell is a byte. Whole
      // instruments were shifted by octaves until they fitted and the strays
      // were dropped, which changed the music to fit a limit that no longer
      // exists. The player reaches MIDI 0 now, so the cells keep the file's own
      // notes and this carries the shift.
      const int shift = (int) (int8_t) head[16] - kXmNoteShift;  // relative_note
      ins.transpose = (int8_t) (shift < -127 ? -127 : (shift > 127 ? 127 : shift));
    }

    written += (size_t) frames * (size_t) stride;
    at = payload_at + total;
  }

  if (bytes != nullptr)
    *bytes = written;
  return true;
}

bool
xm_geometry(const uint8_t *data, size_t size, XmGeometry *geom) {
  if (data == nullptr || size < kXmMinHeader || !xm_signature(data))
    return false;

  const uint32_t header_bytes = read_u32(data + 60);
  // The order table is at 80 and belongs to the header, so a header that does
  // not reach past it does not carry one.
  if (header_bytes < 20u || (size_t) (size - 60u) < (size_t) header_bytes)
    return false;

  const int order_count = (int) read_u16(data + 64);
  const int channels = (int) read_u16(data + 68);
  const int pattern_count = (int) read_u16(data + 70);
  const int instrument_count = (int) read_u16(data + 72);
  const int speed = (int) read_u16(data + 76);
  const int bpm = (int) read_u16(data + 78);
  int restart = (int) read_u16(data + 66);

  if (order_count < 1 || order_count > 256)
    return false;
  if ((size_t) header_bytes < 20u + (size_t) order_count)
    return false;
  // **Refused rather than mixed down.** XM allows thirty-two channels and ntrk
  // holds sixteen; a converter that folded the extra ones together would be
  // making up a mix, which is not an import.
  if (channels < 1 || channels > kMaxChannels)
    return false;
  if (pattern_count < 1 || pattern_count > 256)
    return false;
  if (instrument_count < 0 || instrument_count > kMaxInstruments)
    return false;
  // The ranges `module_save` will accept anyway. Checked here so the refusal
  // names the file rather than arriving from three layers down.
  if (speed < 1 || speed > 31)
    return false;
  if (bpm < 32 || bpm > 255)
    return false;
  if (restart >= order_count)
    restart = 0;

  // Every order entry has to name a pattern that exists: `module_save` refuses
  // the module otherwise, and a stale entry past the count is a real thing in
  // real files.
  for (int i = 0; i < order_count; ++i)
    if ((int) data[80 + i] >= pattern_count)
      return false;

  // **The pattern block is a chain, not an array.** Each header says how long
  // it is and how much packed data follows, so the only way to the instruments
  // is through all of them.
  size_t at = 60u + (size_t) header_bytes;
  int rows_max = 0;
  for (int p = 0; p < pattern_count; ++p) {
    if (at > size || size - at < kXmPatternHeaderMin)
      return false;
    const uint32_t head_bytes = read_u32(data + at);
    const int packing = (int) data[at + 4];
    const int rows = (int) read_u16(data + at + 5);
    const size_t packed = (size_t) read_u16(data + at + 7);
    if (packing != 0)
      return false;              // the only packing XM ever defined
    if (rows < 1 || rows > 256)
      return false;
    if (head_bytes < kXmPatternHeaderMin ||
        (size_t) (size - at) < (size_t) head_bytes)
      return false;
    if (size - (at + (size_t) head_bytes) < packed)
      return false;
    if (rows > rows_max)
      rows_max = rows;
    at += (size_t) head_bytes + packed;
  }

  geom->channels = channels;
  geom->rows = rows_max;
  geom->pattern_count = pattern_count;
  geom->order_count = order_count;
  geom->restart = restart;
  geom->speed = speed;
  geom->bpm = bpm;
  geom->instrument_count = instrument_count;
  geom->pattern_at = 60u + (size_t) header_bytes;
  geom->instrument_at = at;
  geom->cell_bytes = (size_t) pattern_count * (size_t) rows_max *
                     (size_t) channels * 4u;
  return xm_instruments(data, size, at, instrument_count, nullptr, nullptr,
                        nullptr, &geom->sample_bytes);
}

// One cell as the file spells it, before any of it is mapped onto a `Note`.
struct XmRawCell {
  uint8_t note, instrument, volume, effect, param;
};

// One packed cell, advancing `byte`. False when the cell runs past `end`, which
// is the only bound there is -- a pattern's own declared length.
//
// **Two walks read this block**: the unpack below and the global-volume walk
// under it. One decoder, because two that disagreed about where a cell ends
// would disagree about which row every effect after it is on.
bool
xm_read_cell(const uint8_t *data, size_t *byte, size_t end, XmRawCell *cell) {
  cell->note = 0;
  cell->instrument = 0;
  cell->volume = 0;
  cell->effect = 0;
  cell->param = 0;
  if (*byte >= end)
    return false;
  const uint8_t lead = data[(*byte)++];
  if ((lead & 0x80u) != 0u) {
    // The compressed form: one bit per column, in column order, and an absent
    // column is a zero rather than a repeat of the last one.
    uint8_t *columns[5] = {&cell->note, &cell->instrument, &cell->volume,
                           &cell->effect, &cell->param};
    for (int i = 0; i < 5; ++i) {
      if ((lead & (uint8_t) (1u << i)) == 0u)
        continue;
      if (*byte >= end)
        return false;
      *columns[i] = data[(*byte)++];
    }
    return true;
  }
  // The uncompressed form: the lead byte *is* the note and four more follow it.
  if (end - *byte < 4u)
    return false;
  cell->note = lead;
  cell->instrument = data[*byte + 0];
  cell->volume = data[*byte + 1];
  cell->effect = data[*byte + 2];
  cell->param = data[*byte + 3];
  *byte += 4u;
  return true;
}

// One cell's effect column and volume column, folded into the one effect slot a
// `Note` has.
//
// **The effect column wins when both are set.** A tone portamento with a volume
// beside it is a glide; keeping the volume instead retriggers the note, which
// is a different thing happening rather than a quieter version of the same one.
// The volume column takes the slot only when the effect column is empty, which
// in the corpus is most of it -- one tune's entire dynamic range is the volume
// column and it carries no effects at all.
//
// Four volume-column ranges map exactly and the rest are skipped rather than
// approximated: 0x80..0xBF's fine slides and vibrato and 0xD0..0xFF's pan
// slides and tone portamento have no single-slot equivalent, and an approximate
// effect is worse than a missing one because nothing sounds wrong enough to
// look at.
void
xm_effect_for(uint8_t xm_effect, uint8_t xm_param, uint8_t volume,
              uint8_t *effect, uint8_t *param) {
  uint8_t e = 0;
  uint8_t p = 0;
  if (xm_effect <= 0x0fu) {
    // 0x00..0x0F are ProTracker's, same numbers and same parameters, which is
    // the whole reason `.mod` and `.xm` share this file.
    e = xm_effect;
    p = xm_param;
  } else if (xm_effect == 0x1bu && (xm_param & 0xf0u) == 0u &&
             (xm_param & 0x0fu) != 0u) {
    // `Rxy` is multi retrigger with a volume change in `x`. With `x` zero it is
    // exactly `E9y`, and that is the only form the corpus uses; with a volume
    // change there is nothing to map it to.
    e = 0x0eu;
    p = (uint8_t) (0x90u | (xm_param & 0x0fu));
  }
  // Everything from 0x10 up that is not the retrigger above leaves this slot
  // empty. `G` and `H` -- global volume and its slide -- are not lost with it:
  // they carry no effect number of their own here and are folded into the
  // volumes of the cells they govern instead, by `xm_bake_global_volume` below.

  if (e == 0u && p == 0u) {
    if (volume >= 0x10u && volume <= 0x50u) {
      e = 0x0cu;                                   // set volume 0..64
      p = (uint8_t) (volume - 0x10u);
    } else if (volume >= 0x60u && volume <= 0x6fu) {
      e = 0x0au;                                   // volume slide down
      p = (uint8_t) (volume & 0x0fu);
    } else if (volume >= 0x70u && volume <= 0x7fu) {
      e = 0x0au;                                   // volume slide up
      p = (uint8_t) ((volume & 0x0fu) << 4);
    } else if (volume >= 0xc0u && volume <= 0xcfu) {
      e = 0x08u;                                   // set panning, 0..255
      p = (uint8_t) ((volume & 0x0fu) * 17u);
    }
  }

  *effect = e;
  *param = p;
}

// Which instrument a cell's note belongs to. A note with no instrument column
// keeps the channel's last one, so the cells have to be walked in order with a
// little state -- and both passes below walk them the same way, because two
// walks that resolved this differently would shift half a tune.
//
// **Reset at every pattern rather than carried across the order list.** A
// pattern is imported once and played wherever the order names it, so there is
// no single predecessor to inherit from.
void
xm_track_instrument(const Note &cell, int *current) {
  if (cell.instrument != 0)
    *current = (int) cell.instrument;
}

// The packed cells, into a `Note` array `geom.rows` deep whatever the pattern's
// own depth is.
//
// **A short pattern is padded and given a pattern break, not stretched.** ntrk
// has one row count for the whole module and XM has one per pattern; a
// four-row pattern replayed as forty is thirty-six rows of silence in the
// middle of the tune. `Dxx` on its last row is what a tracker musician would
// write to mean the same thing, and it costs the row nothing -- the break goes
// in the first channel whose cell has no effect of its own, and failing that on
// the first padding row, which is empty by construction.
// `size` is not a parameter: every offset walked here was bounds-checked
// against it by `xm_geometry`, which is the only caller's precondition.
bool
xm_unpack_cells(const uint8_t *data, const XmGeometry &geom, Note *notes) {
  for (size_t i = 0; i < geom.cell_bytes; ++i)
    ((uint8_t *) (void *) notes)[i] = 0;

  size_t at = geom.pattern_at;
  for (int p = 0; p < geom.pattern_count; ++p) {
    const uint32_t head_bytes = read_u32(data + at);
    const int rows = (int) read_u16(data + at + 5);
    const size_t packed = (size_t) read_u16(data + at + 7);
    size_t byte = at + (size_t) head_bytes;
    const size_t end = byte + packed;
    const size_t base =
        (size_t) p * (size_t) geom.rows * (size_t) geom.channels;
    const size_t limit = (size_t) rows * (size_t) geom.channels;

    // A pattern may stop early -- the rest of it is empty -- but it may not run
    // past the rows it declared.
    size_t index = 0;
    while (byte < end) {
      if (index >= limit)
        return false;
      XmRawCell raw;
      if (!xm_read_cell(data, &byte, end, &raw))
        return false;

      Note &cell = notes[base + index];
      // 97 is note-off in both formats, so it passes through untouched and is
      // deliberately not shifted below. Anything above it is not a note.
      cell.note = raw.note <= (uint8_t) kXmNoteOff ? raw.note : (uint8_t) 0;
      // An instrument number no slot can carry would index past the table on
      // every row that named it, so it becomes "no instrument" here.
      cell.instrument = (int) raw.instrument <= geom.instrument_count
                            ? raw.instrument
                            : (uint8_t) 0;
      xm_effect_for(raw.effect, raw.param, raw.volume, &cell.effect,
                    &cell.param);
      ++index;
    }

    if (rows < geom.rows) {
      size_t row = (size_t) rows - 1u;
      int column = -1;
      for (int c = 0; c < geom.channels; ++c) {
        const Note &cell =
            notes[base + row * (size_t) geom.channels + (size_t) c];
        if (cell.effect == 0u && cell.param == 0u) {
          column = c;
          break;
        }
      }
      if (column < 0) {
        row = (size_t) rows;          // the first padding row, and it is empty
        column = 0;
      }
      Note &cell = notes[base + row * (size_t) geom.channels + (size_t) column];
      cell.effect = 0x0du;
      cell.param = 0u;
    }

    at += (size_t) head_bytes + packed;
  }
  return true;
}

// ---- global volume -----------------------------------------------------------
//
// XM's `Gxx` sets a master volume over 0..64 and `Hxy` slides it, and a tune
// that fades out does it here. ntrk has no such scalar: `Module` carries no
// master, and the one command that could stand in -- `kFxplGainSet` -- is the
// *mixer's*, which `render_add` does not read at all. A tune imported through
// the plane would still not fade for half its players.
//
// **So the value is folded into the cells instead**, exactly the move
// `relative_note` gets a few functions up and for the same reason: a field the
// player does not read is a field the tune loses. Global volume is a scalar
// multiplying every voice, so a note struck while it stands at 32/64 is a note
// struck at half its volume, and a `Cxx` says that in the format as it is. It
// costs no plane, no new field and not one byte of output -- a `Note` already
// has the two bytes, and `module_save` writes patterns uncompressed.
//
// **What this reproduces exactly**: any change to the global volume that lands
// between notes, which is what a fade written at a tracker is -- and what all
// three real files in `testdata/` are. Every note struck after the change plays
// at the level the change asked for.
//
// **What it approximates**: a change *during* a note. Global volume is applied
// continuously by FT2 and takes a ringing voice with it; a baked cell can only
// reach the notes struck after it, so a note already sounding holds the level it
// was struck at until it is struck again. A slow ramp is barely wrong -- an
// eightieth of full volume per step, against notes a row or two long. An abrupt
// `G00` under a held note is wrong outright: the note rings on where FT2 would
// cut it. Nothing here can fix that without a scalar the player does not have.
//
// **And a slide within one row is taken at the row's opening value.** `Hxy`
// moves once a tick; a cell is struck on the first of them.
const uint8_t kXmFxSpeed = 0x0f;          // `Fxx`, ticks a row under 0x20
const uint8_t kXmFxGlobalVolume = 0x10;   // `Gxx`
const uint8_t kXmFxGlobalSlide = 0x11;    // `Hxy`
const int kXmGlobalFull = 64;

int
xm_clamp_global(int v) {
  if (v < 0)
    return 0;
  return v > kXmGlobalFull ? kXmGlobalFull : v;
}

// What the walk carries from one pattern to the next, and what it found in the
// one it just read. The first two are the running state a tune builds up; the
// last two are what stands in the way of baking this particular pattern.
struct XmGlobalWalk {
  int speed;          // ticks a row, which `Fxx` may change anywhere
  uint8_t memory;     // `Hxy`'s last parameter
  bool loops;         // the pattern replays rows of its own, with `E6x`
  bool varies;        // and its volume is not the same on every row
};

// The global volume through one pattern, row by row, from the value the pattern
// is entered with; the return is what it leaves behind. `rowgv` is filled for
// `fill_rows` entries when it is not null, the tail beyond the pattern's own
// depth taking the value it ended on.
//
// **A row is worth `speed - 1` slide steps**, because FT2 runs `Hxy` on every
// tick of a row but the first. The speed is tracked here rather than read once
// from the header: `Fxx` may change it mid-tune, and a slide counted at the
// wrong tick rate fades at the wrong speed.
//
// `size` is not a parameter for the same reason `xm_unpack_cells` does not take
// one -- every offset here was bounds-checked by `xm_geometry`, and this runs
// only after `xm_unpack_cells` has walked the same bytes and refused anything
// that overran. The `xm_read_cell` failure below is therefore unreachable, and
// is honoured rather than asserted because an assertion is not a bounds check.
//
// ponytail: one `Hxy` parameter memory for the whole module rather than one per
// channel, which is what FT2 keeps. Every file measured writes its global
// volume from a single channel, where the two are the same thing; give it a
// `[kMaxChannels]` if one ever writes it from two.
uint8_t
xm_pattern_global_volume(const uint8_t *data, const XmGeometry &geom, int p,
                         uint8_t entry, int fill_rows, XmGlobalWalk *walk,
                         uint8_t *rowgv) {
  // The block is a chain, so the way to pattern `p` is through the headers of
  // everything before it. Their payloads are stepped over rather than read.
  size_t at = geom.pattern_at;
  for (int i = 0; i < p; ++i)
    at += (size_t) read_u32(data + at) + (size_t) read_u16(data + at + 7);

  const int rows = (int) read_u16(data + at + 5);
  size_t byte = at + (size_t) read_u32(data + at);
  const size_t end = byte + (size_t) read_u16(data + at + 7);

  walk->loops = false;
  walk->varies = false;

  int gv = (int) entry;
  for (int r = 0; r < rows; ++r) {
    int slide = 0;
    for (int c = 0; c < geom.channels; ++c) {
      XmRawCell cell;
      if (byte >= end || !xm_read_cell(data, &byte, end, &cell))
        break;                       // the pattern stopped early; the rest is
                                     // empty and changes nothing
      if (cell.effect == kXmFxSpeed && cell.param >= 1u && cell.param < 0x20u)
        walk->speed = (int) cell.param;
      else if (cell.effect == kXmFxGlobalVolume)
        gv = xm_clamp_global((int) cell.param);
      else if (cell.effect == kXmFxGlobalSlide) {
        // A parameter of zero means the last one, per the format's usual
        // effect memory. Up wins over down when a file sets both, which is
        // what FT2 does with the nibbles.
        const uint8_t par = cell.param != 0u ? cell.param : walk->memory;
        walk->memory = par;
        slide = (par >> 4) != 0u ? (int) (par >> 4) : -(int) (par & 0x0fu);
      } else if (cell.effect == 0x0eu && (cell.param >> 4) == 0x06u &&
                 (cell.param & 0x0fu) != 0u) {
        // `E6x` jumps back to the loop point, so this pattern's rows are played
        // more than once from one copy of them. Noted rather than followed --
        // see `xm_bake_global_volume` for what it costs.
        walk->loops = true;
      }
    }
    // The row is struck at the value it opened with; the slide is what the rest
    // of its ticks do, and lands on the row after.
    if (rowgv != nullptr && r < fill_rows)
      rowgv[r] = (uint8_t) gv;
    gv = xm_clamp_global(gv + slide * (walk->speed - 1));
    // Anywhere the pattern has moved off the value it was entered at, a second
    // playing of these same rows would not repeat the first.
    if (gv != (int) entry)
      walk->varies = true;
  }
  if (rowgv != nullptr)
    for (int r = rows; r < fill_rows; r++)
      rowgv[r] = (uint8_t) gv;       // the padding rows, which hold no cells
  return (uint8_t) gv;
}

// Does this effect move a channel's volume somewhere that cannot be predicted
// from the cell alone? A slide's destination is a tick count and a tremolo's is
// a waveform, and guessing at either is worse than admitting the level is no
// longer known.
//
// **`ECx`, the note cut, is in here for a sharper reason than the rest.** It
// zeroes the channel's volume outright, so a level restated over the top of one
// would un-cut a note the tune deliberately stopped -- the one case where
// carrying a stale level does something audible rather than nothing.
bool
xm_effect_moves_volume(uint8_t effect, uint8_t param) {
  const uint8_t sub = (uint8_t) (param >> 4);
  return effect == 0x05u || effect == 0x06u || effect == 0x07u ||
         effect == 0x0au ||
         (effect == 0x0eu && (sub == 0x0au || sub == 0x0bu || sub == 0x0cu));
}

// One pattern's cells, scaled by the global volume standing over each row.
//
// The rule is one sentence: **keep every channel sounding at its own volume
// times the global one**, and say so with the fewest cells that can. Two levels
// are carried per channel to do it -- what the tune asks the channel to sound
// at, and what it has actually been left sounding at -- and a cell is written
// only where those two disagree. That is what makes a tune whose global volume
// never leaves 64 come out byte for byte as it did before: the two agree on
// every row, so nothing is written.
//
// It also does the thing a fade *in* needs and a naive scale-the-note-cells
// pass cannot: when the global volume climbs back, the channels still ringing
// from underneath it are restated at the new level rather than staying quiet
// until they are next struck. Without it a five-second fade-in left six seconds
// of the tune after it at a third of its volume, which is a worse wrong than
// the one this function exists to fix.
//
// **A cell whose effect slot is taken is left alone.** The slot is the only one
// there is, and a tone portamento replaced by a volume is a different thing
// happening rather than a quieter version of the same one -- the same rule
// `xm_effect_for` follows for the volume column. Such a channel simply stays
// stale until a cell it can use comes round.
void
xm_scale_pattern(const XmGeometry &geom, const Instrument *instruments,
                 Note *notes, int p, const uint8_t *rowgv) {
  int current[kMaxChannels];   // the instrument the channel last named
  int want[kMaxChannels];      // what it should sound at before scaling, -1 unknown
  int have[kMaxChannels];      // what it has been left sounding at, -1 unknown
  for (int c = 0; c < kMaxChannels; ++c) {
    current[c] = 0;
    want[c] = -1;
    have[c] = -1;
  }

  // **`geom.rows` is the padded height, so a short pattern is walked past its
  // own end** and the bake can write a `Cxx` into rows after the `D00` that
  // breaks out of it -- cells nothing plays. Found by fuzzing; no real module
  // reaches it, because the only corpus file using `Gxx` has no short patterns.
  // Left alone because the real height is not in `XmGeometry` and re-reading the
  // pattern header to bound one cosmetic loop costs more than it saves. Bound it
  // if this bake is reworked.
  for (int r = 0; r < geom.rows; ++r) {
    const int gv = (int) rowgv[r];
    for (int c = 0; c < geom.channels; ++c) {
      Note &cell = notes[((size_t) p * (size_t) geom.rows + (size_t) r) *
                             (size_t) geom.channels + (size_t) c];
      // Tracked before anything else: which instrument a later row inherits
      // does not depend on how loud this one was.
      xm_track_instrument(cell, &current[c]);
      const bool known_instrument =
          current[c] >= 1 && current[c] <= geom.instrument_count;

      // What this cell leaves the channel wanting, and what it leaves it
      // actually at. The two are the same everywhere the cell is not scaled.
      if (cell.effect == 0x0cu) {
        want[c] = (int) cell.param;
      } else if (cell.instrument != 0u && known_instrument) {
        // **The instrument column is what reloads a channel's volume**, not the
        // note: a note struck with no instrument beside it keeps whatever the
        // channel was at, in this player and in FT2 both.
        want[c] = (int) instruments[current[c] - 1].volume;
        have[c] = want[c];
      } else if (xm_effect_moves_volume(cell.effect, cell.param)) {
        want[c] = -1;
        have[c] = -1;
      }
      if (want[c] < 0)
        continue;

      const int target =
          (want[c] * gv + kXmGlobalFull / 2) / kXmGlobalFull;
      if (cell.effect == 0x0cu) {
        cell.param = (uint8_t) target;      // identity while `gv` is full
        have[c] = target;
      } else if (target != have[c] && cell.effect == 0u && cell.param == 0u) {
        cell.effect = 0x0cu;
        cell.param = (uint8_t) target;
        have[c] = target;
      }
    }
  }
}

// The global volume, folded into every pattern the order list reaches.
//
// **Driven by the order list rather than by the pattern table**, because the
// value is a running one: a pattern's opening volume is whatever the pattern
// before it left behind, and a pattern nothing plays has no opening volume at
// all and is left exactly as it was.
//
// **And a pattern is baked only where its rows have one volume however often
// they play.** A pattern is stored once and played from that one copy, so a
// pattern the tune plays at two different volumes cannot carry both, and baking
// one of them is a fade that is audibly wrong for the rest of the tune rather
// than a fade that is missing. Two shapes are refused for it, and the second
// was found by measuring rather than by reading the format:
//
//   - **the order list names the pattern twice at different volumes**, and
//   - **the pattern loops rows of its own with `E6x` while its volume moves**.
//     One real file fades *in* exactly this way -- twenty rows played three
//     times, climbing by a third of full each pass -- and baking the first
//     pass into cells the other two also play left fifteen seconds of the tune
//     at a third of its volume. That is worse than the fade being absent.
//
// Both are refusals, not approximations: the pattern keeps every byte it had.
//
// ponytail: the walk follows the order list straight through, so a `Bxx` or
// `Dxx` that jumps out of a pattern early still counts that pattern's remaining
// rows, and an `E6x` loop is noted rather than followed. Only `Hxy`'s running
// total can be moved by either -- a `Gxx` sets outright -- and no file measured
// slides across a jump.
void
xm_bake_global_volume(const uint8_t *data, const XmGeometry &geom,
                      const Instrument *instruments, Note *notes) {
  bool seen[256];
  bool conflict[256];
  uint8_t entry[256];
  for (int i = 0; i < 256; ++i) {
    seen[i] = false;
    conflict[i] = false;
    entry[i] = 0;
  }

  // **The same walk, twice.** Whether a pattern is entered at two volumes is
  // not known until the whole order list has been read, and the walk carries
  // enough state -- speed, the slide memory -- that re-deriving it is safer
  // than storing a copy of it per pattern.
  for (int pass = 0; pass < 2; ++pass) {
    XmGlobalWalk walk;
    walk.speed = geom.speed;
    walk.memory = 0;
    walk.loops = false;
    walk.varies = false;

    uint8_t gv = (uint8_t) kXmGlobalFull;   // what FT2 starts a tune at
    for (int i = 0; i < geom.order_count; ++i) {
      // Every entry names a pattern that exists: `xm_geometry` refused the file
      // otherwise.
      const int p = (int) data[80 + i];
      const bool first = !seen[p];

      if (pass == 0) {
        if (first)
          entry[p] = gv;
        else if (entry[p] != gv)
          conflict[p] = true;
        seen[p] = true;
        gv = xm_pattern_global_volume(data, geom, p, gv, 0, &walk, nullptr);
        continue;
      }

      uint8_t rowgv[256];             // XM's ceiling, which `geom.rows` is
                                      // checked against
      gv = xm_pattern_global_volume(data, geom, p, gv, geom.rows, &walk,
                                    first ? rowgv : nullptr);
      if (first) {
        seen[p] = true;
        if (!conflict[p] && !(walk.loops && walk.varies))
          xm_scale_pattern(geom, instruments, notes, p, rowgv);
      }
    }

    if (pass == 0)
      for (int i = 0; i < 256; ++i)
        seen[i] = false;              // the second walk repeats the first
  }
}

bool
xm_convert(const uint8_t *data, size_t size, uint8_t *scratch,
           size_t scratch_size, uint8_t *out, size_t out_cap, size_t *written) {
  XmGeometry geom;
  if (!xm_geometry(data, size, &geom))
    return false;
  // By subtraction, and the smaller half tested first so the subtraction cannot
  // wrap on a caller who passed too little.
  if (scratch == nullptr || scratch_size < geom.cell_bytes ||
      scratch_size - geom.cell_bytes < geom.sample_bytes)
    return false;

  Module module;
  module.note_max = kMaxNote;  // an `.xm` reaches well past three octaves
  module.channels = geom.channels;
  module.rows = geom.rows;
  module.speed = geom.speed;
  module.bpm = geom.bpm;
  module.order_count = geom.order_count;
  module.pattern_count = geom.pattern_count;
  module.instrument_count = geom.instrument_count;
  module.restart = geom.restart;
  module.order = data + 80;    // the file's own bytes, truncated by the count

  Note *notes = (Note *) (void *) scratch;
  if (!xm_unpack_cells(data, geom, notes))
    return false;
  module.patterns = notes;

  // The samples decode into the scratch behind the cells, and the instruments
  // point at that; `module_save` copies it into `out` from there.
  int8_t relnote[kMaxInstruments];
  for (int i = 0; i < kMaxInstruments; ++i)
    relnote[i] = 0;
  size_t decoded = 0;
  if (!xm_instruments(data, size, geom.instrument_at, geom.instrument_count,
                      module.instruments, relnote, scratch + geom.cell_bytes,
                      &decoded))
    return false;
  if (decoded != geom.sample_bytes)
    return false;              // the two walks disagreed; nothing else can say

  // The notes stay exactly as the file wrote them. Every instrument's shift is
  // on the instrument now (`transpose`, set in `xm_instruments`), which is both
  // where the format puts it and the only place that can express it: an XM's
  // bottom octaves are negative notes in our numbering and a cell is a byte.
  (void) relnote;

  // Last, so the instrument volumes it reads are the ones `xm_instruments`
  // settled on.
  xm_bake_global_volume(data, geom, module.instruments, notes);

  return module_save(&module, out, out_cap, written);
}

}  // namespace

ImportFormat
import_detect(const uint8_t *data, size_t size) {
  if (data == nullptr)
    return ImportFormat::kUnknown;
  // The `.xm` tag is at offset 0 and a `.mod`'s is at 1080, so the two cannot
  // be confused and the order here is only about which test is cheaper.
  if (size >= kXmMinHeader && xm_signature(data))
    return ImportFormat::kXm;
  if (size >= kModHeaderBytes && mod_channels_for(data) != 0)
    return ImportFormat::kMod;
  return ImportFormat::kUnknown;
}

size_t
import_scratch_needed(const uint8_t *data, size_t size) {
  const ImportFormat format = import_detect(data, size);
  if (format == ImportFormat::kXm) {
    XmGeometry geom;
    if (!xm_geometry(data, size, &geom))
      return 0;
    // The cells, then the decoded frames behind them. Both are sized for this
    // exact file, and the sum is what `import_convert` will divide back up.
    return geom.cell_bytes + geom.sample_bytes;
  }
  if (format != ImportFormat::kMod)
    return 0;
  ModGeometry geom;
  if (!mod_geometry(data, size, &geom))
    return 0;
  // The `Note` array the cells become. Same shape as the packed block they come
  // from, so the same arithmetic sizes both -- and `Note` is four `uint8_t`, so
  // the caller's `uint8_t` buffer is already aligned for it.
  return geom.pattern_bytes;
}

bool
import_convert(const uint8_t *data, size_t size, uint8_t *scratch,
               size_t scratch_size, uint8_t *out, size_t out_cap,
               size_t *written) {
  if (written == nullptr)
    return false;
  *written = 0;

  const ImportFormat format = import_detect(data, size);
  if (format == ImportFormat::kXm)
    return xm_convert(data, size, scratch, scratch_size, out, out_cap, written);

  ModGeometry geom;
  if (format != ImportFormat::kMod)
    return false;
  if (!mod_geometry(data, size, &geom))
    return false;
  if (scratch == nullptr || scratch_size < geom.pattern_bytes)
    return false;

  // The defaults are what a `.mod` is: three octaves, PCM8, no plane, no
  // blocks. Nothing below has to switch any of that off.
  Module module;
  module.channels = geom.channels;
  module.rows = geom.rows;
  module.speed = 6;
  module.bpm = 125;
  module.order_count = geom.order_count;
  module.pattern_count = geom.pattern_count;
  module.restart = geom.restart;
  module.order = data + 952;     // the file's own bytes, truncated by the count

  // Unpack the cells. `b0`'s high nibble and `b2`'s high nibble are the two
  // halves of the sample number; `b0`'s low nibble and `b1` are the period.
  Note *notes = (Note *) (void *) scratch;
  const uint8_t *cells = data + kModHeaderBytes;
  const size_t cell_count = geom.pattern_bytes / 4u;
  int named = 0;                 // the highest instrument any cell asks for
  for (size_t i = 0; i < cell_count; ++i) {
    const uint8_t *c = cells + i * 4u;
    int sample = (int) ((c[0] & 0xf0u) | (uint8_t) (c[2] >> 4));
    const int period = (int) (((uint16_t) (c[0] & 0x0fu) << 8) | c[1]);
    if (sample > kModInstruments)
      sample = 0;                // a number no instrument slot can carry
    if (sample > named)
      named = sample;
    notes[i].note = mod_note_for(period);
    notes[i].instrument = (uint8_t) sample;
    notes[i].effect = (uint8_t) (c[2] & 0x0fu);
    notes[i].param = c[3];
  }
  module.patterns = notes;

  // The instrument records, then the sample blob they point into. Both are read
  // for all thirty-one slots whatever the tune uses, because the blob is laid
  // out in slot order and skipping one would misplace every sample after it.
  size_t at = kModHeaderBytes + geom.pattern_bytes;
  int last_used = 0;
  for (int i = 0; i < kModInstruments; ++i) {
    const uint8_t *rec = data + 20 + (size_t) i * 30u + 22u;
    // Everything is in **words** on disk -- length, loop start and loop length
    // alike. A length read as bytes is half a sample and a blob that walks off
    // the end of the file.
    const uint32_t length = (uint32_t) read_be16(rec + 0) * 2u;
    // Finetune is a **signed nibble**: 8..15 are -8..-1, and reading it as a
    // byte detunes the instrument by most of a semitone the wrong way.
    const int nibble = (int) (rec[2] & 0x0fu);
    const int8_t finetune = (int8_t) (nibble > 7 ? nibble - 16 : nibble);
    const uint8_t volume = rec[3] > 64u ? (uint8_t) 64 : rec[3];
    uint32_t loop_start = (uint32_t) read_be16(rec + 4) * 2u;
    uint32_t loop_len = (uint32_t) read_be16(rec + 6) * 2u;

    // A loop of one word is ProTracker's "no loop", not a two-byte loop.
    if (loop_len <= 2u) {
      loop_len = 0u;
      loop_start = 0u;
    }
    // A loop that runs off the end is clamped rather than refused: modules with
    // one word too many are common, and `module_save` would reject the lot.
    if (loop_len > 0u &&
        (loop_start >= length || loop_len > length - loop_start)) {
      loop_len = loop_start < length ? length - loop_start : 0u;
      if (loop_len < 2u) {
        loop_len = 0u;
        loop_start = 0u;
      }
    }

    // **Truncation is refused, never padded.** See the note at the top: this is
    // where a hostile file would otherwise hand us a sample that is mostly
    // whatever follows the buffer.
    if (size - at < (size_t) length)
      return false;

    Instrument &ins = module.instruments[i];
    ins.data = (const int8_t *) (const void *) (data + at);
    ins.length = length;
    ins.loop_start = loop_start;
    ins.loop_len = loop_len;
    ins.volume = volume;
    ins.finetune = finetune;
    at += (size_t) length;

    if (length > 0u)
      last_used = i + 1;
  }

  // Drop the trailing slots the tune never uses. A `.mod` always carries
  // thirty-one whether or not it needs them, and at 20 bytes each that is 600
  // bytes of nothing in a file this project ships over the web.
  //
  // **Only trailing ones, and only when no cell names them**: an instrument
  // number in a cell is an index into this table, so dropping one from the
  // middle would renumber every instrument after it.
  module.instrument_count = named > last_used ? named : last_used;
  if (module.instrument_count > kMaxInstruments)
    return false;                // unreachable from a `.mod`'s 31 slots

  // `module_save` re-checks every field above and refuses rather than dropping
  // anything, so a module built wrong here is a refusal rather than a file
  // nothing can open.
  return module_save(&module, out, out_cap, written);
}

}  // namespace ntrk
