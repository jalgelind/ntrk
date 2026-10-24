#include "bridge.cc"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <algorithm>

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
  if (argc > 1) {
    assert(web_demo(48000));
    uint8_t orders[128] = {}; song.order = orders; song.order_count = 128;
    song.instrument_count = 64;
    save_fixture(argv[1], "long", 7);
  }
  std::puts("web bridge: bounds, roundtrip, transactional load, transport, mute, finite audio, effect allocation passed");
}
