// Standalone browser host. Only this adapter owns memory; ntrk stays a view.
// Public domain / CC0, like ntrk. No browser headers or desktop dependencies.
#include "../ntrk_unity.h"
#include "../ntrk_interactive.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {
namespace ia = ntrk::interactive;
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

// Interactive scores play only through the capability-bound runtime. The worklet
// is the single audio owner: message handlers and web_render never overlap, so
// a handler retires the old binding before any buffer it borrows is replaced.
bool interactive = false; // adopted song carries a supported IMUS score
bool bound = false;       // runtime bound to `binding` and `generation`
bool started = false;     // Play entered the initial section; Stop clears it
uint64_t generation = 0;  // monotonic, never reused; one per compiled binding
uint32_t initial_section = 0, stale_commands = 0;
ia::RuntimeBinding binding, probe; // probe validates without touching `binding`
ia::Runtime runtime;
ia::Position position;
ia::CueEvent cue;
// Off-render description of the adopted score; names are copied, never borrowed.
struct ScoreEntry { uint8_t kind; uint32_t id; int field[7]; char name[64]; };
ScoreEntry score[ntrk::kMaxImusRecords];
int score_count = 0;

ia::Capabilities capabilities() {
  ia::Capabilities caps;
  caps.held_controls = caps.layer_ramps = caps.safe_entry = caps.cut = caps.carry = true;
  caps.pause_freezes = caps.rate_remaining_time = true;
  caps.tail_deadline_ms = 2000;
  return caps;
}
void retire() {
  if (bound) runtime.unbind();
  bound = started = false;
}
void describe() {
  score_count = 0;
  ntrk::ImusView view;
  if (!song.imus_bytes ||
      ntrk::imus_decode(song.imus, song.imus_bytes, &view, &song) != ntrk::ImusStatus::Supported) return;
  size_t at = 12; ntrk::ImusRecord r;
  while (ntrk::imus_next(view, &at, &r)) {
    ScoreEntry e = ScoreEntry(); e.kind = r.kind; e.id = r.id;
    const ntrk::ImusNamedView *named = nullptr;
    ntrk::ImusControlView c; ntrk::ImusLayerView l; ntrk::ImusSectionView s; ntrk::ImusCueView q;
    uint32_t first = 0, end = 0;
    if (r.version != 1) continue; // optional future records are not part of this host
    if (r.kind == ntrk::kImusControl && ntrk::imus_control(r, &c)) {
      named = &c; e.field[0] = c.macro_index; e.field[1] = c.default_value;
    } else if (r.kind == ntrk::kImusLayer && ntrk::imus_layer(r, &l)) {
      named = &l; e.field[0] = l.channel_mask; e.field[1] = l.default_gain; e.field[2] = l.fade_ms;
    } else if (r.kind == ntrk::kImusSection && ntrk::imus_section(r, &s) && ntrk::imus_span(song, s, &first, &end)) {
      named = &s; e.field[0] = int(first); e.field[1] = int(end); e.field[2] = s.entry_bpm;
      e.field[3] = s.entry_speed; e.field[4] = s.quantization; e.field[5] = s.end_action; e.field[6] = s.tail_policy;
    } else if (r.kind == ntrk::kImusCue && ntrk::imus_cue(r, &q)) {
      named = &q; e.field[0] = int(q.section_id); e.field[1] = int(q.row_offset);
      e.field[2] = q.type; e.field[3] = q.strength;
    }
    if (!named) continue;
    std::memcpy(e.name, named->name, named->name_bytes); e.name[named->name_bytes] = 0;
    score[score_count++] = e;
  }
}
bool whole(double x) { return x >= 1 && x <= 9007199254740991.0 && x == std::floor(x); }

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
// Commit point of a validated load: the old runtime is retired before its
// borrowed buffer becomes the next input buffer.
void adopt(const ntrk::Module &candidate, bool score_file) {
  retire();
  active = 1 - active;
  song = candidate;
  const size_t count = song.pattern_count * song.rows * song.channels;
  std::memcpy(notes, song.patterns, count * sizeof(ntrk::Note));
  song.patterns = notes;
  interactive = score_file;
  configure();
  describe();
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
  retire(); interactive = false; score_count = 0;
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
  adopt(candidate, false);
  return 1;
}

int web_ia_select(int section);
// Explicit interactive host path; web_load keeps refusing scores. 1 adopted,
// 0 invalid or not a score, -3 unsupported profile/capability, -5 device rate
// outside the runtime's rates. Every refusal leaves the current song intact.
// Sections exist: Play waits for web_ia_select. None: bound over the arrangement.
int web_load_interactive(int size) {
  if (size <= 0 || size > kFileCap) return 0;
  if (!ia::rate_supported(rate)) return -5;
  ntrk::Module candidate;
  ntrk::ModuleLoadError load_error;
  if (!ntrk::module_load_ex(&candidate, web_input(), static_cast<size_t>(size),
                            ntrk::ModuleLoadMode::InteractiveV1, &load_error))
    return load_error == ntrk::ModuleLoadError::UnsupportedProfile ? -3 : 0;
  if (!candidate.imus_bytes) return 0;
  ntrk::mix::Mixer checked;
  if (!ntrk::mix::mixer_config_read(&checked, &candidate) || !effects_fit(checked)) return 0;
  const ia::Status status = probe.compile(candidate, 1, capabilities(), 0);
  if (status == ia::Status::Unsupported) return -3;
  if (status != ia::Status::Ok && status != ia::Status::MissingSection) return 0;
  adopt(candidate, true);
  if (status == ia::Status::Ok) web_ia_select(0);
  return 1;
}

// Chooses the initial section while stopped (0 only for section-less scores).
// Compiles a fresh generation off render; refusals keep the previous binding.
// Returns the runtime Status value (0 Ok), or -1 when not interactive/stopped.
int web_ia_select(int section) {
  if (!interactive || started || section < 0) return -1;
  const ia::Status checked = probe.compile(song, generation + 1, capabilities(), uint32_t(section));
  if (checked != ia::Status::Ok) return int(checked);
  retire();
  binding.compile(song, ++generation, capabilities(), uint32_t(section));
  const ia::Status status = runtime.bind(binding, player, mixer, generation, rate);
  bound = status == ia::Status::Ok;
  initial_section = uint32_t(section);
  return int(status);
}

// Generation-tagged, strictly sequenced score commands applied at the next
// render boundary: 0 control (value 0..1), 1 layer (value 0..1, fade ms),
// 2 section request. Returns the runtime Status value; stale ones are counted.
int web_ia_command(int kind, double gen, double seq, int id, double value, int fade_ms) {
  if (!bound) { if (stale_commands != UINT32_MAX) ++stale_commands; return int(ia::Status::Stale); }
  if (!whole(gen) || !whole(seq) || id < 0) return int(ia::Status::Invalid);
  const uint64_t g = uint64_t(gen), q = uint64_t(seq);
  uint16_t normalized = 0;
  ia::Status status = ia::Status::Invalid;
  if ((kind == 0 || kind == 1) && !(value >= 0 && value <= 1 && ia::normalized(float(value), &normalized)))
    return int(status);
  if (kind == 0) status = runtime.control(g, q, uint32_t(id), normalized);
  else if (kind == 1) {
    if (fade_ms < 0 || fade_ms > 65535) return int(status);
    status = runtime.layer(g, q, uint32_t(id), normalized, uint16_t(fade_ms));
  } else if (kind == 2) status = runtime.request_section(g, q, uint32_t(id));
  if (status == ia::Status::Stale && stale_commands != UINT32_MAX) ++stale_commands;
  return int(status);
}

// Audio-owner feedback, read between render calls. web_ia_snapshot copies one
// consistent Position; web_ia_info reads only that copy.
int web_ia_snapshot() {
  position = bound ? runtime.position() : ia::Position();
  return bound;
}
double web_ia_info(int field, int index) {
  const ia::Position &p = position;
  const bool c = index >= 0 && index < p.controls, l = index >= 0 && index < p.layers;
  switch (field) {
    case 0: return double(bound ? generation : 0); case 1: return double(p.clock);
    case 2: return double(p.traversal); case 3: return double(p.accepted_sequence);
    case 4: return p.current; case 5: return p.pending; case 6: return p.row_offset;
    case 7: return p.cue_drops; case 8: return p.order; case 9: return p.row;
    case 10: return p.playing; case 11: return p.paused; case 12: return p.draining;
    case 13: return p.controls; case 14: return p.layers;
    case 15: return c ? p.control[index].id : -1; case 16: return c ? p.control[index].held : -1;
    case 17: return c ? p.control[index].applied : -1;
    case 18: return l ? p.layer[index].id : -1; case 19: return l ? p.layer[index].current : -1;
    case 20: return l ? p.layer[index].target : -1; case 21: return l ? double(p.layer[index].remaining) : -1;
    case 22: return l ? p.layer[index].held : -1; case 23: return l ? p.layer[index].held_value : -1;
    case 24: return l ? p.layer[index].held_fade_ms : -1;
    case 25: return initial_section; case 26: return started; case 27: return stale_commands;
    default: return -1;
  }
}
int web_ia_pop() { return bound && runtime.pop_cue(&cue); }
double web_ia_cue(int field) {
  switch (field) {
    case 0: return double(cue.generation); case 1: return double(cue.sequence);
    case 2: return double(cue.clock); case 3: return double(cue.traversal);
    case 4: return cue.id; case 5: return cue.section; case 6: return cue.sample_offset;
    case 7: return cue.type; case 8: return cue.strength;
    default: return -1;
  }
}
int web_score_count() { return interactive ? score_count : 0; }
int web_score_info(int i, int field) {
  if (!interactive || i < 0 || i >= score_count || field < 0 || field > 8) return -1;
  return field == 0 ? score[i].kind : field == 1 ? int(score[i].id) : score[i].field[field - 2];
}
const char *web_score_name(int i) { return interactive && i >= 0 && i < score_count ? score[i].name : ""; }

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
    case 14: return interactive;
    default: return -1;
  }
}

// A bound score borrows these cells; edits would need a new generation.
int web_edit(int pattern, int row, int channel, int field, int value) {
  if (interactive || pattern < 0 || pattern >= song.pattern_count || row < 0 || row >= song.rows ||
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

// Interactive: Play enters the selected initial section (fresh defaults) or
// resumes; Pause freezes the runtime; Stop cuts. Sections own position: no seek.
int interactive_transport(int command) {
  if (!bound) return 0;
  if (command == 0) { runtime.stop(false); started = running = false; }
  else if (command == 1) {
    const ia::Position now = runtime.position();
    if (!started || (!now.playing && !now.draining)) { // fresh, stopped or ended by its section
      runtime.unbind();
      if (runtime.bind(binding, player, mixer, generation, rate) != ia::Status::Ok) { bound = false; return 0; }
      started = true;
    }
    runtime.pause(false); running = true;
  } else if (command == 2) { runtime.pause(true); running = false; }
  else return 0;
  return 1;
}
int web_transport(int command, int order, int row) {
  if (interactive) return interactive_transport(command);
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
  // Neither path allocates, formats or decodes; feedback is read by handlers.
  if (running && interactive) runtime.render(audio, frames, 2, rate);
  else if (running) ntrk::mix::mixer_render_add(&mixer, &player, audio, frames, 2, rate);
  return 1;
}
}
