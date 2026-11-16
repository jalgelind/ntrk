static ntrk::Note poly_note(uint8_t n,uint8_t i,uint8_t e,uint8_t p) { ntrk::Note out;out.note=n;out.instrument=i;out.effect=e;out.param=p;return out; }
static void test_note_column_layout() {
  Module m;uint8_t order[1]={0};Note note=poly_note(13,1,0,0),extra[3]={};
  int8_t pcm[2]={32,-32};
  m.channels=1;m.rows=1;m.pattern_count=1;m.order_count=1;m.instrument_count=1;
  m.order=order;m.patterns=&note;
  m.instruments[0].data=pcm;m.instruments[0].length=m.instruments[0].loop_len=2;m.instruments[0].volume=32;
  CHECK(module_column_layout_valid(&m));CHECK(module_columns_valid(&m));
  Player p;player_start(&p,&m);CHECK(p.playing);
  double output[128]={};render_add(&p,output,64,2,44100);
  CHECK(p.channels[0].playing);CHECK(output[2]!=0);
  for(int nc=1;nc<4;++nc) CHECK(!player_voice(&p,0,nc)->playing);
  for(auto mode : {PlaybackMode::Ordinary,PlaybackMode::LinearPreview}) {
    for(int count : {0,5,2}) {
      Module bad=m;bad.note_columns[0]=uint8_t(count);bad.extra_notes=count==2 ? nullptr : extra;
      CHECK(!module_column_layout_valid(&bad));CHECK(!module_columns_valid(&bad));
      player_start(&p,&m);CHECK(p.playing);
      CHECK(player_start_check(&bad,mode)==PlayerStartResult::InvalidColumnLayout);
      CHECK(player_start_result(&p,&bad,mode)==PlayerStartResult::InvalidColumnLayout);CHECK(!p.playing);
      memset(output,0,sizeof output);render_add(&p,output,64,2,44100);
      for(double value:output) CHECK(value==0);
      CHECK(!player_preview(&p,0,13,1,44100));
      CHECK(!player_preview(&p,0,13,1,44100,4));CHECK(!p.playing);
      for(int nc=0;nc<4;++nc) CHECK(!player_voice(&p,0,nc)->playing);
      CHECK(!module_note(&bad,0,0,0,4));
      if(count!=2) CHECK(!module_note(&bad,0,0,0));
      else CHECK(!module_note(&bad,0,0,0,1));
    }
  }
  CHECK(!module_column_layout_valid(nullptr));CHECK(!module_note(nullptr,0,0,0));
  CHECK(!module_note(&m,0,0,kMaxChannels));CHECK(!module_note(&m,0,0,0,-1));
  CHECK(!player_voice(nullptr,0));CHECK(!player_voice(&p,kMaxChannels));CHECK(!player_voice(&p,0,kMaxNoteColumns));
  for(int count : {0,5,2}) {
    Module bad=m;bad.note_columns[kMaxChannels-1]=uint8_t(count);bad.extra_notes=extra;
    CHECK(!module_column_layout_valid(&bad));
    CHECK(player_start_result(&p,&bad)==PlayerStartResult::InvalidColumnLayout);
  }
  m.note_columns[0]=4;m.extra_notes=extra;
  for(int nc=0;nc<3;++nc) extra[nc]=poly_note(uint8_t(20+nc*4),1,0,0);
  CHECK(module_column_layout_valid(&m));CHECK(module_columns_valid(&m));
  CHECK(player_start_result(&p,&m)==PlayerStartResult::Ok);CHECK(p.playing);
  memset(output,0,sizeof output);render_add(&p,output,64,2,44100);
  CHECK(output[2]!=0);
  for(int nc=0;nc<4;++nc) CHECK(player_voice(&p,0,nc)->playing);
  player_stop(&p);CHECK(!player_preview(&p,0,13,1,44100,4));CHECK(!p.playing);
  CHECK(player_preview(&p,0,13,1,44100,3));CHECK(player_voice(&p,0,3)->playing);
  // Startup checks geometry only; the save/load validator still owns cell content.
  extra[0].effect=0xF;
  CHECK(!module_columns_valid(&m));
  CHECK(player_start_result(&p,&m)==PlayerStartResult::Ok);
}
// Shared native/WASM mixer suite: independently owned columns and wire format.
static void test_note_columns() {
  test_note_column_layout();
  printf("bounded note columns: codec, lifetimes, mixing and block partition\n");
  Module m; uint8_t order[1]={0}; Note notes[8]={}; Note extra[24]={};
  int8_t wave[16]; for(int i=0;i<16;++i) wave[i]=i<8 ? 40 : -40;
  m.channels=1;m.rows=8;m.pattern_count=1;m.order_count=1;m.instrument_count=2;
  m.order=order;m.patterns=notes;m.extra_notes=extra;m.note_columns[0]=4;
  for(int i=0;i<2;++i) { Instrument &in=m.instruments[i];
    in.data=wave;in.length=16;in.loop_len=16;in.volume=16;
  }
  notes[0]=poly_note(13,1,0,0);extra[0]=poly_note(20,2,0,0);extra[1]=poly_note(25,1,0,0);extra[2]=poly_note(32,2,0,0);
  uint8_t bytes[8192],again[8192];size_t size=0,size2=0;
  CHECK(module_save(&m,bytes,sizeof bytes,&size)); Module loaded;
  CHECK(module_load(&loaded,bytes,size));CHECK(loaded.note_columns[0]==4);
  CHECK(module_note(&loaded,0,0,0,3)->note==32);CHECK(!module_note(&loaded,0,0,0,4));
  CHECK(module_save(&loaded,again,sizeof again,&size2));CHECK(size==size2);CHECK(!memcmp(bytes,again,size));
  // The primary keeps unknown-byte compatibility; an extra column does not.
  notes[1].effect=0xff;
  CHECK(module_save(&m,again,sizeof again,&size2));CHECK(module_load(&loaded,again,size2));
  CHECK(module_note(&loaded,0,1,0)->effect==0xff);
  extra[3]=poly_note(0,0,0xE,0x50);
  CHECK(module_save(&m,again,sizeof again,&size2));
  extra[3].param=0x60;CHECK(!module_save(&m,again,sizeof again,&size2));
  extra[3].effect=0xff;CHECK(!module_save(&m,again,sizeof again,&size2));
  extra[3]={};notes[1]={};
  const size_t directory=32+1+2*32+8*4+32;
  size_t nc_entry=0;
  for(int i=0;i<read_u16(bytes+26);++i) if(read_u16(bytes+directory+i*12)==kBlockNcol) nc_entry=directory+i*12;
  CHECK(nc_entry!=0);
  if(nc_entry) {
    const uint32_t payload=read_u32(bytes+nc_entry+4);
    bytes[payload+4]=5;CHECK(!module_load(&loaded,bytes,size));bytes[payload+4]=4;
    bytes[payload+20+2]=0xF;CHECK(!module_load(&loaded,bytes,size));bytes[payload+20+2]=0;
    write_u16(bytes+nc_entry,0x7fff);CHECK(!module_load(&loaded,bytes,size));write_u16(bytes+nc_entry,kBlockNcol);
    write_u16(bytes+nc_entry+2,0);CHECK(!module_load(&loaded,bytes,size));write_u16(bytes+nc_entry+2,kBlockCritical);
    CHECK(!module_load(&loaded,bytes,size-1));
  }
  Player p;player_start(&p,&m);player_row(&p,44100);
  for(int nc=0;nc<4;++nc) CHECK(player_voice(&p,0,nc)->playing);
  CHECK(p.channels[0].instrument==1);CHECK(player_voice(&p,0,1)->instrument==2);
  // Off, delayed strike and cut address independent columns on the same row.
  extra[3]=poly_note(kNoteOff,0,0,0);extra[4]=poly_note(29,1,0xE,0xD2);extra[5]=poly_note(0,0,0xE,0xC1);
  p.row=1;player_row(&p,44100);CHECK(p.channels[0].playing);
  CHECK(!player_voice(&p,0,1)->playing);CHECK(player_voice(&p,0,2)->delay_pending);
  p.tick=1;player_effects(&p,44100);CHECK(player_voice(&p,0,3)->volume==0);
  CHECK(p.channels[0].volume==16);p.tick=2;player_effects(&p,44100);
  CHECK(!player_voice(&p,0,2)->delay_pending);CHECK(player_voice(&p,0,2)->playing);
  notes[2]=poly_note(0,0,1,2);extra[6]=poly_note(20,2,1,7);p.row=2;player_row(&p,44100);
  CHECK(p.channels[0].porta_speed==2);CHECK(player_voice(&p,0,1)->porta_speed==7);
  player_stop(&p);for(int nc=0;nc<4;++nc) CHECK(!player_voice(&p,0,nc)->playing);
  CHECK(player_preview(&p,0,25,2,44100,3));CHECK(player_voice(&p,0,3)->playing);CHECK(!p.channels[0].playing);
  CHECK(player_seek(&p,0,0));for(int nc=0;nc<4;++nc) CHECK(!player_voice(&p,0,nc)->playing);
  // Every voice/filter advances while muted; arbitrary block partition leaves identical PCM.
  static mix::Mixer mx;
  double reference[8192]={},partitioned[8192]={};
  m.instruments[0].flags=kInstrumentFilter;m.instruments[0].filter_cutoff_hz=1800;
  for(int partition : {1,64,128,512,137}) {
    player_start(&p,&m);mix::mixer_reset(&mx);mix::mixer_config_read(&mx,&m);
    memset(partitioned,0,sizeof partitioned);
    for(int at=0;at<4096;) { int run=partition; if(run>4096-at)run=4096-at;
      mix::mixer_render_add(&mx,&p,partitioned+at*2,run,2,44100);at+=run;
    }
    if(partition==1) memcpy(reference,partitioned,sizeof reference);
    else for(int i=0;i<8192;++i) CHECK(reference[i]==partitioned[i]);
    for(int i=0;i<8192;++i) CHECK(std::isfinite(partitioned[i]));
  }
  Player walked,rendered; player_start(&walked,&m);player_start(&rendered,&m);
  player_skip(&walked,44100,4096);double dry[8192]={};render_add(&rendered,dry,4096,2,44100);
  for(int nc=0;nc<4;++nc) { const auto* a=player_voice(&walked,0,nc);const auto* b=player_voice(&rendered,0,nc);
    CHECK(a->pos==b->pos);CHECK(a->period==b->period);CHECK(a->env_stage==b->env_stage);
  }
  // Independent voice filters equal the same voices on separate strips when strips are neutral.
  Module pair=m; pair.channels=2;pair.extra_notes=nullptr;pair.note_columns[0]=1;
  Note paired[16]={};pair.patterns=paired;
  Note plain[8]={},additional[24]={};m.patterns=plain;m.extra_notes=additional;m.note_columns[0]=2;
  plain[0]=paired[0]=poly_note(13,1,0,0);additional[0]=paired[1]=poly_note(20,2,0,0);
  m.instruments[1].flags=kInstrumentFilter;m.instruments[1].filter_cutoff_hz=3600;
  pair.instruments[1]=m.instruments[1];
  static mix::Mixer other;
  for(int outputs : {1,2}) {
    Player a,b;player_start(&a,&m);player_start(&b,&pair);
    mix::mixer_reset(&mx);mix::mixer_reset(&other);mix::mixer_config_read(&mx,&m);mix::mixer_config_read(&other,&pair);
    memset(reference,0,sizeof reference);memset(partitioned,0,sizeof partitioned);
    mix::mixer_render_add(&mx,&a,reference,1024,outputs,44100);
    mix::mixer_render_add(&other,&b,partitioned,1024,outputs,44100);
    for(int i=0;i<1024*outputs;++i) CHECK(reference[i]==partitioned[i]);
  }
  // A muted strip keeps every voice and both filter histories evolving.
  Player audible,muted;player_start(&audible,&m);player_start(&muted,&m);muted.muted[0]=true;
  mix::mixer_reset(&mx);mix::mixer_reset(&other);mix::mixer_config_read(&mx,&m);mix::mixer_config_read(&other,&m);
  memset(reference,0,sizeof reference);memset(partitioned,0,sizeof partitioned);
  mix::mixer_render_add(&mx,&audible,reference,1024,2,44100);
  mix::mixer_render_add(&other,&muted,partitioned,1024,2,44100);
  for(int i=0;i<2048;++i) CHECK(partitioned[i]==0);
  muted.muted[0]=false;memset(reference,0,sizeof reference);memset(partitioned,0,sizeof partitioned);
  mix::mixer_render_add(&mx,&audible,reference,512,2,44100);
  mix::mixer_render_add(&other,&muted,partitioned,512,2,44100);
  for(int i=0;i<1024;++i) CHECK(reference[i]==partitioned[i]);
  // A bass synth and a sustained pad have separate release and pan ownership.
  m.instruments[0].type=uint8_t(InstrumentType::kSynth);synth_voice_defaults(&m.instruments[0],SynthVoice::kBass);
  m.instruments[0].pan=0;m.instruments[1].pan=255;m.instruments[1].flags=kInstrumentEnvelope;
  m.instruments[1].env_sustain=64;m.instruments[1].env_release_ms=100;
  player_start(&p,&m);player_row(&p,44100);CHECK(p.pan[0]==-1.f);
  CHECK(p.channels[0].instrument==1);CHECK(player_voice(&p,0,1)->instrument==2);
  player_walk_run(&p,128);const uint32_t bass_age=p.channels[0].synth_age;
  channel_note_off(&m,player_voice(&p,0,1));CHECK(player_voice(&p,0,1)->env_stage==EnvStage::kRelease);
  CHECK(p.channels[0].playing);CHECK(p.channels[0].synth_age==bass_age);
  // Interactive rate changes and both tail policies visit every column.
  for(bool carry : {false,true}) {
    ntrk_test::InteractiveFixture fixture(1,1,true,carry);
    Note plane[12]={};fixture.module.note_columns[0]=4;fixture.module.extra_notes=plane;
    for(int nc=0;nc<3;++nc) plane[nc]=poly_note(uint8_t(20+nc*3),1,0,0);
    RuntimeBinding binding;CHECK(binding.compile(fixture.module,7,fixture.caps,3)==Status::Ok);
    Runtime runtime;Player voices;mix::mixer_reset(&mx);
    CHECK(runtime.bind(binding,voices,mx,7,48000)==Status::Ok);
    double output[256]={};CHECK(runtime.render(output,64,2,48000)==Status::Ok);
    double steps[4];for(int nc=0;nc<4;++nc) {CHECK(player_voice(&voices,0,nc)->playing);steps[nc]=player_voice(&voices,0,nc)->step;}
    CHECK(runtime.change_rate(96000)==Status::Ok);
    for(int nc=0;nc<4;++nc) CHECK(player_voice(&voices,0,nc)->step==steps[nc]/2);
    CHECK(runtime.stop(carry)==Status::Ok);
    for(int nc=0;nc<4;++nc) CHECK(!player_voice(&voices,0,nc)->playing);
    CHECK(runtime.render(output,64,2,96000)==Status::Ok);
    plane[0].effect=0xB;CHECK(binding.compile(fixture.module,8,fixture.caps,3)!=Status::Ok);
  }
  // One-column default emits no NCOL; unused nonzero slots cannot be silently discarded.
  m.instruments[0].type=uint8_t(InstrumentType::kPcm8);
  m.note_columns[0]=1;CHECK(!module_save(&m,bytes,sizeof bytes,&size));m.extra_notes=nullptr;
  CHECK(module_save(&m,bytes,sizeof bytes,&size));CHECK(module_load(&loaded,bytes,size));CHECK(!loaded.extra_notes);
}
