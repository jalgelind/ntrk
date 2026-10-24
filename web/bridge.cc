// Standalone browser host. Only this adapter owns memory; ntrk stays a view.
// Public domain / CC0, like ntrk. No browser headers or desktop dependencies.
#include "../ntrk_unity.h"
#include <cstring>
#include <cstdio>
#include <vector>

namespace {
const int kFileCap = 16 * 1024 * 1024;
uint8_t files[2][kFileCap];
// Conversion never writes either live backing buffer. Fixed caps bound memory.
uint8_t imported[kFileCap], import_scratch[kFileCap];
int active = 0;
ntrk::Module song;
ntrk::Note notes[ntrk::kMaxPatterns * ntrk::kMaxRows * ntrk::kMaxChannels];
ntrk::Player player;
ntrk::mix::Mixer mixer;
// Fixed effect arena keeps arbitrary-file configuration inside the WASM budget.
const size_t kEffectCap = 40 * 1024 * 1024;
float effect_memory[kEffectCap / sizeof(float)];
const int kEffectSlots = ntrk::kMaxChannels + ntrk::mix::kSends + ntrk::kMixrMasterSlots;
double audio[128 * 2];
float rate = 48000;
bool running = false;
uint8_t demo_order[] = {0, 1};

void reset() {
  ntrk::player_start(&player, &song);
  ntrk::mix::mixer_reset(&mixer);
  ntrk::mix::fxpl_sync(&mixer, &player);
}

ntrk::mix::Slot *effect_slot(ntrk::mix::Mixer &mix, int i) {
  return i < ntrk::kMaxChannels ? &mix.insert[i] :
      i < ntrk::kMaxChannels + ntrk::mix::kSends ? &mix.send[i - ntrk::kMaxChannels] :
      &mix.master_fx[i - ntrk::kMaxChannels - ntrk::mix::kSends];
}
size_t effect_bytes(const ntrk::mix::Slot &slot) {
  size_t bytes = 0;
  if (slot.kind == ntrk::mix::FxKind::kDelay)
    bytes = ntrk::fx::delay_bytes_needed(ntrk::mix::kSlotDelayMaxSeconds, rate);
  if (slot.kind == ntrk::mix::FxKind::kReverb)
    bytes = ntrk::fx::reverb_bytes_needed(rate, ntrk::mix::kSlotPredelayMaxSeconds);
  return (bytes + sizeof(float) - 1) / sizeof(float) * sizeof(float);
}
bool effects_fit(ntrk::mix::Mixer &mix) {
  size_t used = 0;
  for (int i = 0; i < kEffectSlots; ++i) {
    const size_t bytes = effect_bytes(*effect_slot(mix, i));
    if (bytes > kEffectCap - used) return false;
    used += bytes;
  }
  return true;
}
void configure() {
  running = false;
  mixer = ntrk::mix::Mixer();
  ntrk::mix::mixer_config_read(&mixer, &song);
  size_t offset = 0;
  for (int i = 0; i < kEffectSlots; ++i) {
    auto *slot = effect_slot(mixer, i);
    const size_t bytes = effect_bytes(*slot);
    float *memory = effect_memory + offset / sizeof(float);
    if (slot->kind == ntrk::mix::FxKind::kDelay)
      ntrk::fx::delay_init(&slot->delay, memory, bytes, rate, ntrk::mix::kSlotDelayMaxSeconds);
    else if (slot->kind == ntrk::mix::FxKind::kReverb)
      ntrk::fx::reverb_init(&slot->reverb, memory, bytes, rate, ntrk::mix::kSlotPredelayMaxSeconds);
    offset += bytes;
  }
  for (int c = 0; c < ntrk::kMaxChannels; ++c) player.muted[c] = false;
  reset();
}
}

extern "C" {
int web_capacity() { return kFileCap; }
uint8_t *web_input() { return files[1 - active]; }
const uint8_t *web_patterns() { return reinterpret_cast<uint8_t *>(notes); }
const uint8_t *web_orders() { return song.order; }
double *web_audio() { return audio; }
// Inspection only: names and values come from the loaded module, never UI aliases.
const char *web_instrument_name(int slot) {
  return slot >= 1 && slot <= song.instrument_count ? song.instruments[slot - 1].name : "";
}
int web_instrument_volume(int slot) {
  return slot >= 1 && slot <= song.instrument_count ? song.instruments[slot - 1].volume : -1;
}

int web_instrument_pan(int slot) {
  return slot >= 1 && slot <= song.instrument_count ? song.instruments[slot - 1].pan : -1;
}
// Formatting belongs beside the library APIs. lane -1 is the primary note
// command, 0..fx_columns-1 channel FXPL, channel -1 addresses meta lanes.
const char *web_cell_text(int pattern, int row, int channel, int lane, int description) {
  static char out[512];
  out[0] = 0;
  if (pattern < 0 || pattern >= song.pattern_count || row < 0 || row >= song.rows) return out;
  const int at = pattern * song.rows + row;
  ntrk::FxCell fx;
  if (channel >= 0) {
    if (channel >= song.channels) return out;
    if (lane == -1) {
      const auto &n = song.patterns[at * song.channels + channel];
      if (description) ntrk::note_fx_describe(n.effect, n.param, out, sizeof out);
      else if (n.effect || n.param) ntrk::note_fx_mnemonic(n.effect, n.param, out);
      else std::strcpy(out, "...");
      return out;
    }
    if (lane < 0 || lane >= song.fx_columns) return out;
    if (song.fx) fx = song.fx[at * ntrk::module_lanes(&song) + channel * song.fx_columns + lane];
  } else {
    if (lane < 0 || lane >= song.meta_columns) return out;
    if (song.fx) fx = song.fx[at * ntrk::module_lanes(&song) + song.channels * song.fx_columns + lane];
  }
  if (description) ntrk::mix::fxpl_describe(&mixer, &song, channel, fx, out, sizeof out);
  else {
    char mnemonic[4];
    if (channel < 0 && fx.cmd && fx.cmd < ntrk::mix::kFxplSlotSet)
      std::snprintf(mnemonic, sizeof mnemonic, "M%02X", fx.cmd);
    else ntrk::mix::fxpl_mnemonic(fx, mnemonic);
    if (fx.cmd || fx.param) std::snprintf(out, sizeof out, "%s %02X", mnemonic, fx.param);
    else std::strcpy(out, "... ..");
  }
  return out;
}
const uint8_t *web_fx() { return reinterpret_cast<const uint8_t *>(song.fx); }

int web_demo(int sample_rate) {
  if (sample_rate < 8000 || sample_rate > 96000) return 0;
  rate = static_cast<float>(sample_rate);
  song = ntrk::Module();
  song.channels = ntrk::kMaxChannels; song.rows = 32; song.pattern_count = 2;
  song.order_count = 2; song.instrument_count = 3; song.note_max = 96;
  song.bpm = 128; song.order = demo_order; song.patterns = notes;
  std::memset(notes, 0, sizeof(ntrk::Note) * 2 * 32 * song.channels);
  const ntrk::SynthVoice voices[] = {ntrk::SynthVoice::kBass,
      ntrk::SynthVoice::kKick, ntrk::SynthVoice::kHihat};
  const char *names[] = {"Night circuit / bass", "Kick", "Closed hat"};
  for (int i = 0; i < 3; ++i) {
    auto &ins = song.instruments[i];
    ins.type = static_cast<uint8_t>(ntrk::InstrumentType::kSynth);
    ntrk::synth_voice_defaults(&ins, voices[i]);
    ins.volume = i == 0 ? 40 : i == 1 ? 48 : 22;
    std::strncpy(ins.name, names[i], ntrk::kNameBytes);
  }
  const int riff[] = {13, 13, 25, 20, 16, 13, 23, 20};
  for (int p = 0; p < 2; ++p) for (int r = 0; r < 32; ++r) {
    auto *row = notes + (p * 32 + r) * song.channels;
    if (r % 2 == 0) { row[0].note = riff[(r / 2 + p * 3) % 8]; row[0].instrument = 1; }
    if (r % 8 == 0) { row[1].note = 25; row[1].instrument = 2; }
    if (r % 4 == 2) { row[2].note = 25; row[2].instrument = 3; }
  }
  configure();
  return 1;
}

int web_load(int size) {
  if (size <= 0 || size > kFileCap) return 0;
  ntrk::Module candidate;
  const bool foreign = ntrk::import_detect(web_input(), size) != ntrk::ImportFormat::kUnknown;
  const uint8_t *bytes = web_input();
  size_t length = static_cast<size_t>(size);
  if (foreign) {
    const size_t need = ntrk::import_scratch_needed(bytes, length);
    if (!need || need > sizeof(import_scratch) ||
        !ntrk::import_convert(bytes, length, import_scratch, sizeof(import_scratch),
                              imported, sizeof(imported), &length)) return 0;
    bytes = imported;
  }
  ntrk::ModuleLoadError load_error;
  if (!ntrk::module_load_ex(&candidate, bytes, length, ntrk::ModuleLoadMode::Ordinary, &load_error))
    return load_error == ntrk::ModuleLoadError::InteractiveRequired ? -2 : 0;
  ntrk::mix::Mixer checked;
  if (!ntrk::mix::mixer_config_read(&checked, &candidate) || !effects_fit(checked)) return 0;
  // Failed loads leave the playing module and all its backing storage intact.
  if (foreign) {
    std::memcpy(web_input(), imported, length);
    if (!ntrk::module_load(&candidate, web_input(), length)) return 0;
  }
  active = 1 - active;
  song = candidate;
  const size_t count = song.pattern_count * song.rows * song.channels;
  std::memcpy(notes, song.patterns, count * sizeof(ntrk::Note));
  song.patterns = notes;
  configure();
  return 1;
}

int web_save() {
  size_t size = 0;
  return ntrk::module_save(&song, web_input(), kFileCap, &size) ? static_cast<int>(size) : 0;
}

// Musical use across every stored pattern, including patterns outside the order
// list. Mixer defaults and row-wide meta automation do not occupy a channel.
int web_used_channels() {
  unsigned mask = 0;
  for (int p = 0; p < song.pattern_count; ++p) {
    for (int r = 0; r < song.rows; ++r) {
      for (int c = 0; c < song.channels; ++c) {
        const auto &note = song.patterns[(p * song.rows + r) * song.channels + c];
        bool used = note.note || note.instrument || note.effect || note.param;
        if (song.fx) {
          const auto *fx = song.fx + (p * song.rows + r) * ntrk::module_lanes(&song)
                                  + c * song.fx_columns;
          for (int col = 0; col < song.fx_columns; ++col)
            used = used || fx[col].cmd || fx[col].param;
        }
        if (used) mask |= 1u << c;
      }
    }
  }
  return static_cast<int>(mask);
}

// Fixed numeric metadata ABI, consumed by engine.js.
int web_info(int field) {
  switch (field) {
    case 0: return song.channels; case 1: return song.rows;
    case 2: return song.pattern_count; case 3: return song.order_count;
    case 4: return song.instrument_count; case 5: return song.note_max;
    case 6: return player.order; case 7: return player.row;
    case 8: return running; case 9: return player.bpm;
    case 10: return player.speed;
    case 11: {
      int bits = 0;
      for (int c = 0; c < song.channels; ++c)
        if (player.muted[c]) bits |= 1 << c;
      return bits;
    }
    case 12: return song.fx ? song.fx_columns : 0;
    case 13: return song.fx ? song.meta_columns : 0;
    default: return -1;
  }
}

int web_edit(int pattern, int row, int channel, int field, int value) {
  if (pattern < 0 || pattern >= song.pattern_count || row < 0 || row >= song.rows ||
      channel < 0 || channel >= song.channels || field < 0 || field > 3 || value < 0)
    return 0;
  if ((field == 0 && value > song.note_max && value != ntrk::kNoteOff) ||
      (field == 1 && value > song.instrument_count) ||
      (field == 2 && value > ntrk::kFxLast) || value > 255) return 0;
  auto &cell = notes[(pattern * song.rows + row) * song.channels + channel];
  switch (field) {
    case 0: cell.note = value; break; case 1: cell.instrument = value; break;
    case 2: cell.effect = value; break; case 3: cell.param = value; break;
  }
  return 1;
}

int web_transport(int command, int order, int row) {
  if (command == 0) { running = false; reset(); }
  else if (command == 1) running = true;
  else if (command == 2) running = false;
  else if (command == 3) {
    if (order < 0 || order >= song.order_count || row < 0 || row >= song.rows) return 0;
    ntrk::mix::mixer_reset(&mixer);
    ntrk::player_seek(&player, order, row);
    ntrk::mix::fxpl_sync(&mixer, &player);
  } else return 0;
  return 1;
}
int web_mute(int channel) {
  if (channel < 0 || channel >= song.channels) return 0;
  player.muted[channel] = !player.muted[channel]; return 1;
}
int web_render(int frames) {
  if (frames < 1 || frames > 128) return 0;
  std::memset(audio, 0, frames * 2 * sizeof(double));
  if (running) ntrk::mix::mixer_render_add(&mixer, &player, audio, frames, 2, rate);
  return 1;
}
}
