#include "bridge.cc"
#include "../test_support/interactive_fixture.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <algorithm>

static size_t g_allocations = 0;
void *operator new(size_t n) {
  ++g_allocations;
  if (void *p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }

void save_fixture(const char *directory, const char *name, int mask) {
  assert(web_used_channels() == mask);
  const int size = web_save(); assert(size > 0);
  const std::vector<uint8_t> before(web_input(), web_input() + size);
  if (directory) {
    const std::string path = std::string(directory) + "/" + name + ".ntrk";
    FILE *file = std::fopen(path.c_str(), "wb"); assert(file);
    assert(std::fwrite(before.data(), 1, before.size(), file) == before.size());
    assert(std::fclose(file) == 0);
  }
  assert(web_load(size) && web_info(0) == 16 && web_used_channels() == mask);
  assert(web_save() == size && !std::memcmp(before.data(), web_input(), size));
}

void test_channel_usage(const char *directory) {
  assert(web_demo(48000) && web_used_channels() == 7);
  std::memset(notes, 0, sizeof(ntrk::Note) * 2 * 32 * song.channels);
  assert(web_used_channels() == 0); // Default gain/pan are not musical use.
  // Every primary byte matters, including a parameter with no command.
  for (int field = 0; field < 4; ++field) {
    assert(web_edit(1, 31, 8, field, 1)); assert(web_used_channels() == (1 << 8));
    assert(web_edit(1, 31, 8, field, 0)); assert(web_used_channels() == 0);
  }
  song.fx_columns = 2; song.meta_columns = 1; song.macro_count = 1;
  std::vector<ntrk::FxCell> fx(2 * 32 * ntrk::module_lanes(&song));
  song.fx = fx.data();
  // Row-wide macro automation must not turn every track into a used track.
  fx[16 * 2].cmd = 1; fx[16 * 2].param = 64;
  assert(web_used_channels() == 0);
  save_fixture(directory, "empty", 0);

  // The second pattern is stored but not referenced by the order list.
  assert(web_demo(48000));
  std::memset(notes, 0, sizeof(ntrk::Note) * 2 * 32 * song.channels);
  song.order_count = 1; song.hidden_channels = 0xffff;
  notes[0].note = 25;
  notes[3].instrument = 1;
  notes[(1 * 32 + 31) * 16 + 11].param = 127;
  song.fx_columns = 2; song.meta_columns = 1; song.macro_count = 1;
  std::fill(fx.begin(), fx.end(), ntrk::FxCell()); song.fx = fx.data();
  const int automation = (1 * 32 + 31) * ntrk::module_lanes(&song) + 14 * 2 + 1;
  fx[automation].param = 64; assert(web_used_channels() == ((1<<0)|(1<<3)|(1<<11)|(1<<14)));
  fx[automation].param = 0; fx[automation].cmd = ntrk::mix::kFxplSendSet;
  fx[16 * 2].cmd = 1; fx[16 * 2].param = 64;
  save_fixture(directory, "sparse", (1<<0)|(1<<3)|(1<<11)|(1<<14));

  assert(web_demo(48000));
  std::memset(notes, 0, sizeof(ntrk::Note) * 2 * 32 * song.channels);
  int mask = 0;
  for (int channel : {0, 1, 3, 4, 6, 7, 9, 11, 13, 15}) {
    notes[channel].note = 25; mask |= 1 << channel;
  }
  save_fixture(directory, "wide", mask);
  std::puts("web channel usage: primary fields, sparse IDs, unreferenced patterns, multi-column FXPL, meta exclusion, empty/default mixer, exact save/load passed");
}

void test_imports(const char *directory) {
  if (!directory) return;
  for (const char *name : {"sampled.mod", "sampled.xm"}) {
    const std::string path = std::string(directory) + "/" + name;
    FILE *file = std::fopen(path.c_str(), "rb"); assert(file);
    std::vector<uint8_t> bytes(kFileCap);
    const size_t length = std::fread(bytes.data(), 1, bytes.size(), file);
    assert(length && !std::ferror(file)); std::fclose(file); bytes.resize(length);
    assert(web_demo(48000));
    const int old_size = web_save();
    const std::vector<uint8_t> old(web_input(), web_input() + old_size);
    // Every truncated foreign header/sample leaves the complete prior song intact.
    for (size_t size : {size_t(1), size_t(60), length / 2, length - 1}) {
      std::memcpy(web_input(), bytes.data(), size);
      assert(!web_load(static_cast<int>(size)));
      assert(web_save() == old_size && !std::memcmp(old.data(), web_input(), old_size));
    }
    std::memcpy(web_input(), bytes.data(), length);
    assert(web_load(static_cast<int>(length)));
    assert(song.instrument_count > 0 && song.instruments[0].length > 0);
    const int size = web_save(); assert(size > 0);
    const std::vector<uint8_t> converted(web_input(), web_input() + size);
    assert(web_load(size) && web_save() == size);
    assert(!std::memcmp(converted.data(), web_input(), size));
    assert(web_transport(1, 0, 0));
    double energy = 0;
    for (int block = 0; block < 100; ++block) {
      web_render(128);
      for (double value : audio) { assert(std::isfinite(value)); energy += value * value; }
    }
    assert(energy > 0);
    // Also expose arbitrary PCM ntrk modules, not just generated synth music.
    const std::string ntrk_path = path + ".ntrk";
    file = std::fopen(ntrk_path.c_str(), "wb"); assert(file);
    assert(std::fwrite(converted.data(), 1, converted.size(), file) == converted.size());
    assert(!std::fclose(file));
  }
  std::puts("web imports: sampled 8-channel MOD/XM, PCM8/16, truncation preservation, roundtrip and finite audio passed");
}

void test_command_metadata(const char *directory) {
  assert(web_demo(48000));
  song.fx_columns=3;song.meta_columns=5;song.macro_count=1;
  std::vector<ntrk::FxCell> fx(song.pattern_count*song.rows*ntrk::module_lanes(&song));
  song.fx=fx.data();
  char expected[512],mnemonic[4];
  for(int command=0;command<=ntrk::kFxLast;++command) for(int param:{0,1,128,255}) {
    notes[0].effect=command;notes[0].param=param;
    ntrk::note_fx_mnemonic(command,param,mnemonic);
    assert(!std::strcmp(web_cell_text(0,0,0,-1,0),command||param?mnemonic:"..."));
    ntrk::note_fx_describe(command,param,expected,sizeof expected);
    assert(!std::strcmp(web_cell_text(0,0,0,-1,1),expected));
  }
  for(int channel:{0,15,-1}) for(int lane=0;lane<(channel<0?5:3);++lane)
    for(int command=0;command<256;++command) for(int param:{0,127,255}) {
      auto &cell=fx[channel<0?song.channels*3+lane:channel*3+lane];
      cell.cmd=command;cell.param=param;
      ntrk::mix::fxpl_describe(&mixer,&song,channel,cell,expected,sizeof expected);
      assert(!std::strcmp(web_cell_text(0,0,channel,lane,1),expected));
      assert(std::strlen(web_cell_text(0,0,channel,lane,0))==6);
    }
  fx.assign(fx.size(),ntrk::FxCell());
  fx[song.channels*3].cmd=1; // meta 01 is macro 1, never channel PAN
  fx[song.channels*3+1].param=64; // meta 00 has no macro invocation
  assert(!std::strcmp(web_cell_text(0,0,-1,0,0),"M01 00"));
  assert(!std::strcmp(web_cell_text(0,0,-1,1,1),"None"));
  assert(!std::strcmp(web_cell_text(0,0,-1,1,0),"... 40"));
  assert(!*web_cell_text(0,0,-1,5,0));
  notes[0].effect=8;notes[0].param=0;
  notes[1].effect=ntrk::kFxSlice;notes[1].param=0;
  notes[2].effect=ntrk::kFxOffset;notes[2].param=0;
  notes[3].effect=0xE;notes[3].param=0x80;
  song.instruments[0].pan=32;
  assert(web_instrument_pan(1)==32 && web_instrument_pan(0)==-1);
  save_fixture(directory,"commands",web_used_channels());
  const int size=web_save();assert(web_load(size) && web_instrument_pan(1)==32);
  std::puts("web metadata: all note/FXPL bytes, every column/meta lane, macro/00 distinction and pan passed");
}

void test_large_geometry(const char *directory) {
  assert(web_demo(48000));
  assert(web_info(12)==0 && web_info(13)==0); // no fictional FXPL column
  song.rows=ntrk::kMaxRows;song.pattern_count=198;song.order_count=198;
  song.fx_columns=ntrk::kMaxFxColumns;song.meta_columns=ntrk::kMaxMetaColumns;song.macro_count=1;
  std::vector<uint8_t> order(song.order_count);
  for(int p=0;p<song.order_count;++p)order[p]=p;
  song.order=order.data();
  std::memset(notes,0,sizeof(ntrk::Note)*song.pattern_count*song.rows*song.channels);
  std::vector<ntrk::FxCell> fx(song.pattern_count*song.rows*ntrk::module_lanes(&song));song.fx=fx.data();
  for(int p=0;p<song.pattern_count;++p)for(int r=0;r<song.rows;++r){
    for(int c=0;c<song.channels;++c){auto &note=notes[(p*song.rows+r)*song.channels+c];
      note.effect=p%2?8:ntrk::kFxOffset;note.param=(p*31+r+c)%256;}
    for(int lane=0;lane<ntrk::module_lanes(&song);++lane){auto &cell=fx[(p*song.rows+r)*ntrk::module_lanes(&song)+lane];
      cell.cmd=lane<song.channels*song.fx_columns?1+(p+r+lane)%23:0x40+(p+r+lane)%64;
      cell.param=(p*17+r+lane*3)%256;}
  }
  save_fixture(directory,"large",0xFFFF);
  const int bytes=web_save();assert(bytes>16000000 && bytes<=kFileCap);
  assert(web_load(bytes));assert(web_info(2)==198 && web_info(1)==256 && web_info(12)==8 && web_info(13)==5);
  assert(web_edit(197,255,15,3,123));
  assert(web_save()>16000000);
  assert(web_demo(48000));
  // Short successive patterns with unmistakably different command labels.
  song.rows=2;song.speed=3;song.bpm=125;
  std::memset(notes,0,sizeof(ntrk::Note)*2*2*song.channels);
  notes[0].effect=ntrk::kFxOffset;notes[0].param=16;
  notes[2*song.channels].effect=8;notes[2*song.channels].param=255;
  save_fixture(directory,"pattern-transition",1);
  std::puts("web large geometry: 198 patterns x256 rows,8 FXPL+5 meta,16MiB-cap save/load/edit passed");
}

void test_active_counts(const char* directory) {
  for(int channels=1;channels<=16;++channels) {
    assert(web_demo(48000)); song.channels=channels;
    std::memset(notes,0,sizeof(ntrk::Note)*song.pattern_count*song.rows*channels);
    notes[0].note=25; notes[0].instrument=1;
    notes[(song.rows-1)*channels+channels-1].note=29; notes[(song.rows-1)*channels+channels-1].instrument=1;
    uint8_t opaque[]={0,255,3,4};
    uint8_t fields[64]; size_t fields_bytes=0;
    ntrk::MetaField f; f.tag=1; f.data=(const uint8_t*)"Small song"; f.bytes=10;
    assert(ntrk::meta_field_encode(f,fields,sizeof fields,&fields_bytes));
    f.tag=0x8000; f.data=opaque; f.bytes=4; size_t tail=0;
    assert(ntrk::meta_field_encode(f,fields+fields_bytes,sizeof fields-fields_bytes,&tail)); fields_bytes+=tail;
    ntrk::MetaEntry entries[2];
    entries[0].name=(const uint8_t*)"ntrk.names"; entries[0].name_bytes=10; entries[0].version=1;
    entries[0].data=fields; entries[0].bytes=(uint32_t)fields_bytes;
    entries[1].name=(const uint8_t*)"vendor.future"; entries[1].name_bytes=13; entries[1].version=99;
    entries[1].data=opaque; entries[1].bytes=4;
    uint8_t meta[256]; size_t n=0; assert(ntrk::meta_encode(entries,2,meta,sizeof meta,&n));
    song.metadata=meta; song.metadata_bytes=(uint32_t)n;
    song.extension_count=1; song.extensions[0].id=0x7331;
    song.extensions[0].data=opaque; song.extensions[0].bytes=4;
    song.hidden_channels=0x8000; song.slot_mute[0]=0x8000;
    const int size=web_save(); assert(size>0);
    std::vector<uint8_t> original(web_input(),web_input()+size);
    assert(web_load(size)); assert(web_info(0)==channels);
    assert(web_save()==size && !memcmp(original.data(),web_input(),size));
    char name[32]; snprintf(name,sizeof name,"count-%02d",channels);
    if(directory) {
      std::string path=std::string(directory)+"/"+name+".ntrk";
      FILE* file=fopen(path.c_str(),"wb"); assert(file);
      assert(fwrite(original.data(),1,original.size(),file)==original.size()); fclose(file);
      path=std::string(directory)+"/"+name+".f32";
      file=fopen(path.c_str(),"wb"); assert(file);
      assert(web_transport(1,0,0));
      for(int quantum=0;quantum<100;++quantum) {
        assert(web_render(128)); float samples[256];
        for(int i=0;i<256;++i) { assert(std::isfinite(audio[i])); samples[i]=(float)audio[i]; }
        assert(fwrite(samples,sizeof(float),256,file)==256);
      }
      fclose(file); assert(web_transport(0,0,0));
    }
    assert(web_edit(0,1,channels-1,3,27));
    assert(!web_edit(0,1,channels,3,27));
    assert(web_save()>0); assert(song.metadata_bytes==n && !memcmp(song.metadata,meta,n));
    assert(song.extension_count==1 && !memcmp(song.extensions[0].data,opaque,4));
  }
}

// One procedural score and one command script drive both the standalone
// runtime reference and the bridge; the WASM test replays the written script.
const char *kInteractiveScript =
    "0 play\n20 layer 2 0.25 40\n45 control 1 0\n60 section 4\n140 pause\n"
    "150 layer 2 1 0\n160 play\n200 section 3\n230 control 1 0.5\n260 stop\n270 play\n";
const int kInteractiveQuanta = 320, kCueFields = 8, kMaxTraceCues = 1024;
struct Step { int quantum; char op[16]; double a, b, c; };
std::vector<Step> interactive_steps() {
  std::vector<Step> steps;
  for (const char *line = kInteractiveScript; *line;) {
    Step s = Step();
    assert(std::sscanf(line, "%d %15s %lf %lf %lf", &s.quantum, s.op, &s.a, &s.b, &s.c) >= 2);
    steps.push_back(s); line = std::strchr(line, '\n') + 1;
  }
  return steps;
}
struct Trace {
  std::vector<float> audio = std::vector<float>(kInteractiveQuanta * 256);
  uint64_t cues[kMaxTraceCues][kCueFields] = {};
  int count = 0;
  void cue(const ia::CueEvent &e) {
    assert(count < kMaxTraceCues);
    const uint64_t row[kCueFields] = {e.clock, e.id, e.section, e.traversal, e.sequence, e.sample_offset, e.type, e.strength};
    std::memcpy(cues[count++], row, sizeof row);
  }
};
ntrk::Player ref_player; ntrk::mix::Mixer ref_mixer; ia::Runtime ref_runtime; ia::RuntimeBinding ref_binding;
// Direct runtime calls only: always renders, so paused/stopped silence comes
// from the runtime itself rather than the bridge's transport gate.
void standalone_trace(const ntrk::Module &m, Trace *t) {
  const uint64_t gen = 7; uint64_t seq = 0; bool started = false;
  assert(ref_binding.compile(m, gen, capabilities(), 3) == ia::Status::Ok);
  ref_player = ntrk::Player(); ref_mixer = ntrk::mix::Mixer();
  assert(ref_runtime.bind(ref_binding, ref_player, ref_mixer, gen, 48000) == ia::Status::Ok);
  const std::vector<Step> steps = interactive_steps();
  for (int q = 0; q < kInteractiveQuanta; ++q) {
    for (const Step &s : steps) {
      if (s.quantum != q) continue;
      const std::string op = s.op; uint16_t v = 0;
      if (op == "play") {
        const ia::Position p = ref_runtime.position();
        if (!started || (!p.playing && !p.draining)) {
          ref_runtime.unbind();
          assert(ref_runtime.bind(ref_binding, ref_player, ref_mixer, gen, 48000) == ia::Status::Ok);
          started = true;
        }
        ref_runtime.pause(false);
      } else if (op == "pause") ref_runtime.pause(true);
      else if (op == "stop") { assert(ref_runtime.stop(false) == ia::Status::Ok); started = false; }
      else if (op == "control") { assert(ia::normalized(float(s.b), &v)); assert(ref_runtime.control(gen, ++seq, uint32_t(s.a), v) == ia::Status::Ok); }
      else if (op == "layer") { assert(ia::normalized(float(s.b), &v)); assert(ref_runtime.layer(gen, ++seq, uint32_t(s.a), v, uint16_t(s.c)) == ia::Status::Ok); }
      else if (op == "section") assert(ref_runtime.request_section(gen, ++seq, uint32_t(s.a)) == ia::Status::Ok);
      else assert(!"unknown script op");
    }
    double out[256] = {};
    assert(ref_runtime.render(out, 128, 2, 48000) == ia::Status::Ok);
    for (int i = 0; i < 256; ++i) t->audio[q * 256 + i] = float(out[i]);
    ia::CueEvent e;
    while (ref_runtime.pop_cue(&e)) t->cue(e);
  }
  ref_runtime.unbind();
}
// The bridge path, measured for heap allocation across commands, render and feedback.
void bridge_trace(Trace *t) {
  const std::vector<Step> steps = interactive_steps();
  const double gen = web_ia_info(0, 0); double seq = 0;
  const size_t allocations = g_allocations;
  for (int q = 0; q < kInteractiveQuanta; ++q) {
    for (const Step &s : steps) {
      if (s.quantum != q) continue;
      if (!std::strcmp(s.op, "play")) assert(web_transport(1, 0, 0));
      else if (!std::strcmp(s.op, "pause")) assert(web_transport(2, 0, 0));
      else if (!std::strcmp(s.op, "stop")) assert(web_transport(0, 0, 0));
      else if (!std::strcmp(s.op, "control")) assert(web_ia_command(0, gen, ++seq, int(s.a), s.b, 0) == 0);
      else if (!std::strcmp(s.op, "layer")) assert(web_ia_command(1, gen, ++seq, int(s.a), s.b, int(s.c)) == 0);
      else if (!std::strcmp(s.op, "section")) assert(web_ia_command(2, gen, ++seq, int(s.a), 0, 0) == 0);
    }
    assert(web_render(128));
    for (int i = 0; i < 256; ++i) t->audio[q * 256 + i] = float(audio[i]);
    while (web_ia_pop()) t->cue(cue);
    web_ia_snapshot();
  }
  assert(g_allocations == allocations);
}
std::vector<uint8_t> saved_module(const ntrk::Module &m) {
  size_t size = 0; assert(ntrk::module_save(&m, nullptr, 0, &size));
  std::vector<uint8_t> bytes(size); assert(ntrk::module_save(&m, bytes.data(), size, &size));
  return bytes;
}
int load_bytes(const std::vector<uint8_t> &bytes, bool score_path) {
  std::memcpy(web_input(), bytes.data(), bytes.size());
  return score_path ? web_load_interactive(int(bytes.size())) : web_load(int(bytes.size()));
}
bool current_song_is(const std::vector<uint8_t> &bytes) {
  const int size = web_save();
  return size == int(bytes.size()) && !std::memcmp(web_input(), bytes.data(), bytes.size());
}
void write_file(const char *directory, const char *name, const void *data, size_t size) {
  const std::string path = std::string(directory) + "/" + name;
  FILE *file = std::fopen(path.c_str(), "wb"); assert(file);
  assert(std::fwrite(data, 1, size, file) == size); assert(!std::fclose(file));
}
static ntrk_test::InteractiveFixture *score_fixture(bool loop) {
  // Fixtures point into themselves: construct in place, never copy.
  static ntrk_test::InteractiveFixture looping(2, 2, true, true), ending(2, 2, false, false);
  ntrk_test::InteractiveFixture *f = loop ? &looping : &ending;
  f->notes[1 * 2 + 1].note = 20; f->notes[1 * 2 + 1].instrument = 1;
  f->notes[3 * 2 + 0].note = 18; f->notes[3 * 2 + 0].instrument = 1;
  f->encode();
  return f;
}
void test_interactive(const char *directory) {
  using ia::Status;
  const std::vector<uint8_t> score = saved_module(score_fixture(true)->module);
  assert(web_demo(48000));
  const std::vector<uint8_t> demo(web_input(), web_input() + web_save());
  // Ordinary loading still refuses; the explicit path adopts without playing.
  assert(load_bytes(score, false) == -2 && current_song_is(demo));
  assert(load_bytes(demo, true) == 0 && current_song_is(demo)); // no ordinary song as a score
  assert(web_ia_command(0, 1, 1, 1, 0.5, 0) == int(Status::Stale));
  assert(load_bytes(score, true) == 1 && web_info(14) == 1 && current_song_is(score));
  assert(!web_ia_snapshot() && !web_transport(1, 0, 0)); // sections need an explicit initial choice
  assert(web_score_count() == 8 && web_score_info(2, 0) == ntrk::kImusSection && web_score_info(2, 1) == 3);
  assert(!std::strcmp(web_score_name(2), "test") && web_score_info(2, 2) == 0 && web_score_info(2, 3) == 2);
  assert(web_score_info(1, 0) == ntrk::kImusLayer && web_score_info(1, 2) == 3 && web_score_info(9, 0) == -1);
  assert(web_ia_select(99) == int(Status::MissingSection) && !web_ia_snapshot());
  assert(web_ia_select(3) == 0 && web_ia_snapshot() && web_ia_info(25, 0) == 3);
  assert(!web_edit(0, 0, 0, 0, 25) && !web_transport(3, 0, 0) && current_song_is(score));
  // Bridge output and cues equal direct runtime calls on the same saved bytes.
  ntrk::Module reference; ntrk::ModuleLoadError error;
  assert(ntrk::module_load_ex(&reference, score.data(), score.size(), ntrk::ModuleLoadMode::InteractiveV1, &error));
  static Trace expected, actual;
  standalone_trace(reference, &expected);
  bridge_trace(&actual);
  assert(actual.audio == expected.audio && actual.count == expected.count);
  assert(!std::memcmp(actual.cues, expected.cues, sizeof(uint64_t) * kCueFields * size_t(expected.count)));
  double energy = 0; for (float x : expected.audio) { assert(std::isfinite(x)); energy += double(x) * x; }
  bool fourth = false, retraversed = false;
  for (int i = 0; i < expected.count; ++i) {
    fourth = fourth || expected.cues[i][2] == 4;
    retraversed = retraversed || (expected.cues[i][2] == 3 && expected.cues[i][3] > 1);
  }
  assert(energy > 0 && expected.count >= 6 && fourth && retraversed);
  // Silence while paused (quanta 140..159) and right after the stop at 260.
  for (int i = 141 * 256; i < 160 * 256; ++i) assert(expected.audio[i] == 0);
  if (directory) {
    write_file(directory, "interactive.ntrk", score.data(), score.size());
    write_file(directory, "interactive-script.txt", kInteractiveScript, std::strlen(kInteractiveScript));
    write_file(directory, "interactive.f32", expected.audio.data(), expected.audio.size() * sizeof(float));
    write_file(directory, "interactive-cues.bin", expected.cues, sizeof(uint64_t) * kCueFields * size_t(expected.count));
  }
  // Sequencing, normalization and selection bounds.
  const double gen = web_ia_info(0, 0);
  assert(web_ia_command(0, gen, 100, 1, 0.5, 0) == 0);
  assert(web_ia_command(0, gen, 100, 1, 0.5, 0) == int(Status::InvalidSequence));
  assert(web_ia_command(0, gen, 101, 1, std::nan(""), 0) == int(Status::Invalid));
  assert(web_ia_command(1, gen, 101, 2, 1.5, 0) == int(Status::Invalid));
  assert(web_ia_command(1, gen, 101, 2, 1, 70000) == int(Status::Invalid));
  assert(web_ia_command(0, gen, 101, 99, 0.5, 0) == int(Status::Invalid));
  assert(web_ia_command(0, gen, 0.5, 1, 0.5, 0) == int(Status::Invalid));
  assert(web_ia_select(4) == -1); // initial selection only while stopped
  // Reload publishes a new generation; commands for the old one are refused.
  const uint32_t stale = uint32_t(web_ia_info(27, 0));
  assert(load_bytes(score, true) == 1 && web_ia_select(3) == 0 && web_ia_info(0, 0) > gen);
  assert(web_ia_command(0, gen, 200, 1, 0.5, 0) == int(Status::Stale) && web_ia_info(27, 0) == stale + 1);
  assert(web_ia_command(0, web_ia_info(0, 0), 1, 1, 0.5, 0) == 0);
  // Unsupported required profiles refuse distinctly and keep the current score.
  ntrk_test::InteractiveFixture future(2, 2, true, true);
  future.records[0].version = 2; future.records[0].flags = 1; future.encode();
  const std::vector<uint8_t> unsupported = saved_module(future.module);
  assert(load_bytes(unsupported, false) == -2 && load_bytes(unsupported, true) == -3);
  if (directory) write_file(directory, "interactive-future.ntrk", unsupported.data(), unsupported.size());
  assert(current_song_is(score) && web_ia_snapshot());
  // A section that stops at its end leaves playback ended; Play re-enters it.
  const std::vector<uint8_t> ending = saved_module(score_fixture(false)->module);
  assert(load_bytes(ending, true) == 1 && web_ia_select(3) == 0 && web_transport(1, 0, 0));
  for (int q = 0; q < 40; ++q) web_render(128);
  web_ia_snapshot(); assert(!web_ia_info(10, 0) && web_info(8));
  for (double x : audio) assert(x == 0);
  assert(web_transport(1, 0, 0) && web_render(128)); web_ia_snapshot();
  assert(web_ia_info(10, 0) && web_ia_info(1, 0) == 128);
  // Section-less scores bind at load and control the ordinary arrangement.
  ntrk_test::InteractiveFixture plain(2, 2, true, true); plain.count = 2; plain.encode();
  assert(load_bytes(saved_module(plain.module), true) == 1 && web_ia_snapshot() && web_ia_info(25, 0) == 0);
  assert(web_transport(1, 0, 0)); energy = 0;
  for (int q = 0; q < 20; ++q) { web_render(128); for (double x : audio) energy += x * x; }
  assert(energy > 0);
  // Unsupported device rate refuses before adoption.
  assert(web_demo(22050));
  const std::vector<uint8_t> slow(web_input(), web_input() + web_save());
  assert(load_bytes(score, true) == -5 && current_song_is(slow));
  // Ordinary adoption retires the runtime; later score commands are stale.
  assert(web_demo(48000) && load_bytes(score, true) == 1 && web_ia_select(3) == 0 && web_transport(1, 0, 0));
  assert(load_bytes(demo, false) == 1 && !web_ia_snapshot() && !web_info(14));
  assert(web_ia_command(0, web_ia_info(0, 0), 999, 1, 0.5, 0) == int(Status::Stale) && !web_score_count());
  assert(web_transport(1, 0, 0) && web_render(128));
  std::puts("web interactive: explicit path/initial section, standalone-runtime audio+cue parity, zero render allocation, sequencing, stale reload, unsupported profile/rate, end/restart, section-less, ordinary retirement passed");
}

int main(int argc, char **argv) {
  assert(!web_demo(0));
  assert(web_demo(48000));
  assert(web_info(0) == ntrk::kMaxChannels && web_info(1) == 32 && web_info(3) == 2);
  assert(!std::strcmp(web_instrument_name(1), "Night circuit / bass"));
  assert(web_instrument_volume(1) == 40);
  assert(!std::strcmp(web_instrument_name(0), "") && !std::strcmp(web_instrument_name(4), ""));
  assert(web_instrument_volume(0) == -1 && web_instrument_volume(4) == -1);
  assert(web_render(128));
  for (double x : audio) assert(x == 0);
  assert(!web_render(129) && !web_render(0));
  assert(!web_edit(-1, 0, 0, 0, 25));
  assert(!web_edit(0, 32, 0, 0, 25));
  assert(!web_edit(0, 0, ntrk::kMaxChannels, 0, 25));
  assert(!web_edit(0, 0, 0, 0, 98));
  assert(!web_edit(0, 0, 0, 1, 4));
  assert(!web_edit(0, 0, 0, 2, ntrk::kFxLast + 1));
  assert(!web_edit(0, 0, 0, 3, 256));
  assert(web_edit(0, 0, 0, 0, 96));
  assert(web_edit(0, 1, 0, 0, ntrk::kNoteOff));
  assert(web_edit(0, 2, 0, 2, ntrk::kFxLast));
  int size = web_save(); assert(size > 0);
  std::vector<uint8_t> saved(web_input(), web_input() + size);
  assert(web_load(size));
  assert(notes[0].note == 96 && notes[ntrk::kMaxChannels].note == ntrk::kNoteOff);
  assert(web_save() == size);
  assert(!std::memcmp(saved.data(), web_input(), size));
  // Interactive scores refuse distinctly and leave the current song intact.
  {
    ntrk::Module interactive=song;
    uint8_t score[12]={1,0,1,0,0,0,0,0,1,0,0,0};
    interactive.imus=score; interactive.imus_bytes=12;
    size_t required=0;
    assert(ntrk::module_save(&interactive,nullptr,0,&required));
    assert(ntrk::module_save(&interactive,web_input(),web_capacity(),&required));
    assert(web_load(int(required))==-2);
    assert(web_save()==size && !std::memcmp(saved.data(),web_input(),size));
  }
  // Repeated loads exercise both backing buffers and failed-load preservation.
  assert(web_load(size));
  web_input()[0] = 0;
  assert(!web_load(size) && !web_load(-1) && !web_load(kFileCap + 1));
  assert(web_save() == size);
  assert(!std::memcmp(saved.data(), web_input(), size));
  assert(!web_transport(3, 2, 0) && !web_transport(3, 0, 32));
  assert(web_transport(3, 1, 16));
  assert(web_info(6) == 1 && web_info(7) == 16);
  assert(web_demo(48000));
  assert(web_transport(1, 0, 0));
  double energy = 0;
  for (int block = 0; block < 1000; ++block) {
    assert(web_render(128));
    for (double x : audio) { assert(std::isfinite(x) && std::abs(x) <= 1.0); energy += x * x; }
  }
  assert(energy > 1 && web_info(7) > 0);
  assert(web_transport(2, 0, 0));
  const int paused = web_info(7);
  web_render(128); assert(web_info(7) == paused);
  for (double x : audio) assert(x == 0);
  for (int c = 0; c < ntrk::kMaxChannels; ++c) assert(web_mute(c));
  assert(!web_mute(ntrk::kMaxChannels));
  assert(web_transport(0, 0, 0));
  assert(web_info(6) == 0 && web_info(7) == 0 && web_info(11) == 65535);
  web_transport(1, 0, 0); web_render(128);
  for (double x : audio) assert(x == 0);
  // Exercise MIXR allocation/configuration with delay and reverb and PCM data.
  assert(web_demo(96000));
  int8_t sample[32];
  for (int i = 0; i < 32; ++i) sample[i] = static_cast<int8_t>(i * 8 - 128);
  auto &pcm = song.instruments[2];
  pcm = ntrk::Instrument(); pcm.data = sample; pcm.length = 32; pcm.loop_len = 32;
  uint8_t slices[] = {0, 0, 0, 0, 16, 0, 0, 0};
  pcm.slices = slices; pcm.slice_count = 2;
  std::vector<ntrk::FxCell> plane(song.pattern_count * song.rows * song.channels);
  plane[0].cmd = ntrk::mix::kFxplSendSet; plane[0].param = 64;
  song.fx = plane.data();
  song.hidden_channels = 0xfff8;
  ntrk::slot_set_muted(&song, 1, 2, true);
  mixer.send[0].param[4] = 1;
  ntrk::mix::slot_set_kind(&mixer.send[0], ntrk::mix::FxKind::kReverb);
  mixer.send[1].param[0] = .1f; mixer.send[1].param[3] = 1;
  ntrk::mix::slot_set_kind(&mixer.send[1], ntrk::mix::FxKind::kDelay);
  ntrk::mix::mixer_config_write(&mixer, &song);
  size = web_save(); assert(size > 0 && web_load(size));
  assert(mixer.send[0].reverb.buffer != nullptr);
  assert(mixer.send[1].delay.buffer != nullptr);
  assert(song.hidden_channels == 0xfff8 && ntrk::slot_muted(&song, 1, 2));
  assert(song.fx[0].cmd == plane[0].cmd && song.fx[0].param == 64);
  assert(song.instruments[2].slice_count == 2);
  assert(!std::memcmp(song.instruments[2].data, sample, sizeof(sample)));
  std::vector<uint8_t> with_samples(web_input(), web_input() + web_save());
  assert(web_load(static_cast<int>(with_samples.size())));
  assert(web_save() == static_cast<int>(with_samples.size()));
  assert(!std::memcmp(with_samples.data(), web_input(), with_samples.size()));
  // Worst supported rate with every configured effect slot consumes only its arena.
  for (auto kind : {ntrk::mix::FxKind::kDelay, ntrk::mix::FxKind::kReverb}) {
    assert(web_demo(96000));
    for (int i = 0; i < kEffectSlots; ++i)
      ntrk::mix::slot_set_kind(effect_slot(mixer, i), kind);
    assert(effects_fit(mixer));
    ntrk::mix::mixer_config_write(&mixer, &song);
    const int bytes = web_save(); assert(bytes > 0 && web_load(bytes));
    assert(web_transport(1, 0, 0) && web_render(128));
    for (double value : audio) assert(std::isfinite(value));
  }
  test_active_counts(argc > 1 ? argv[1] : nullptr);
  test_imports(argc > 1 ? argv[1] : nullptr);
  test_channel_usage(argc > 1 ? argv[1] : nullptr);
  test_command_metadata(argc > 1 ? argv[1] : nullptr);
  test_large_geometry(argc > 1 ? argv[1] : nullptr);
  test_interactive(argc > 1 ? argv[1] : nullptr);
  if (argc > 1) {
    assert(web_demo(48000));
    uint8_t orders[128] = {}; song.order = orders; song.order_count = 128;
    song.instrument_count = 64;
    save_fixture(argv[1], "long", 7);
  }
  std::puts("web bridge: bounds, roundtrip, transactional load, transport, mute, finite audio, effect allocation passed");
}
