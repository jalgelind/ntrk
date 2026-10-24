#include "interactive_fixture.h"
#include <vector>
using namespace ntrk::interactive;
static void test_interactive_binding() {
  ntrk_test::InteractiveFixture f;
  RuntimeBinding b; auto caps=f.caps;
  CHECK(b.compile(f.module,1,caps,0)==Status::MissingSection);
  caps.carry=false;
  CHECK(b.compile(f.module,1,caps,3)==Status::Ok);
  caps.layer_ramps=false; CHECK(b.compile(f.module,1,caps,3)==Status::Unsupported);
  CHECK(b.compile(f.module,1,f.caps,3)==Status::Ok);
  ntrk_test::InteractiveFixture future;
  future.count=3; future.records[2].version=2; future.encode();
  RuntimeBinding future_binding;
  CHECK(future_binding.compile(future.module,1,future.caps,0)==Status::MissingSection);
  CHECK(future_binding.compile(future.module,1,future.caps,3)==Status::MissingSection);
  Player p; mix::Mixer mx; Runtime r;
  CHECK(r.bind(b,p,mx,2,48000)==Status::Stale);
  CHECK(r.bind(b,p,mx,1,22050)==Status::InvalidRate);
  CHECK(r.bind(b,p,mx,1,48000)==Status::Ok);
  CHECK(!player_playback_allowed(&p));
  double out[128]={};
  mix::mixer_render_add(&mx,&p,out,64,2,48000);
  render_add(&p,out,64,2,48000);
  CHECK(p.ticks_elapsed==0 && out[0]==0);
  CHECK(r.control(2,1,1,0)==Status::Stale);
  CHECK(r.layer(1,1,2,0,0)==Status::Ok);
  CHECK(r.layer(1,1,2,0,0)==Status::InvalidSequence);
  CHECK(r.control(1,2,999,0)==Status::Invalid);
  const uint8_t *wire=f.module.imus; f.module.imus=wire+1;
  CHECK(r.render(out,64,2,48000)==Status::Stale); f.module.imus=wire;
  CHECK(r.render(out,64,2,48000)==Status::Ok);
  uint16_t value=42;
  CHECK(!normalized(std::numeric_limits<float>::infinity(),&value) && value==42);
  CHECK(!normalized(std::numeric_limits<float>::quiet_NaN(),&value));
  CHECK(!normalized(-0.01f,&value)); CHECK(normalized(1.f,&value) && value==65535);
}
static void test_interactive_ramps() {
  CHECK(Ramp::remaining_frames(13,44100,44100)==13);
  CHECK(Ramp::remaining_frames(7,48000,96000)==14);
  CHECK(Ramp::remaining_frames(2793,44100,48000)==3040);
  CHECK(Ramp::remaining_frames(3040,48000,44100)==2793);
  for (double rate : {44100.,48000.,96000.}) {
    Ramp ramp; ramp.request(0,1,rate);
    const uint64_t n=uint64_t(std::ceil(rate/1000.0));
    for (uint64_t k=1;k<=n;++k) CHECK(ramp.next()==float(1.0-double(k)/double(n)));
    CHECK(ramp.current==0.0); ramp.request(65535,0,rate); CHECK(ramp.next()==1.f);
    ramp.request(0,100,rate); ramp.next(); const double current=ramp.current;
    ramp.request(32768,2,rate); CHECK(ramp.start==current);
    const uint64_t remaining=ramp.total-ramp.elapsed;
    ramp.change_rate(rate,96000.);
    CHECK(ramp.total==uint64_t(std::ceil(double(remaining)/rate*96000.)));
    CHECK(ramp.current==current);
  }
}
static void test_interactive_timing() {
  for (int channels : {1,16}) for (int speed : {1,6}) for (double rate : {44100.,48000.,96000.}) {
    ntrk_test::InteractiveFixture f(channels,speed,false);
    RuntimeBinding b; CHECK(b.compile(f.module,1,f.caps,3)==Status::Ok);
    Player p; mix::Mixer mx; Runtime r; CHECK(r.bind(b,p,mx,1,rate)==Status::Ok);
    const int row_frames=int(rate*0.02*speed);
    std::vector<double> out(size_t(row_frames*2+10)*2,0.0);
    CHECK(r.render(out.data(),row_frames*2-1,2,rate)==Status::Ok);
    CHECK(r.position().playing && r.position().clock==uint64_t(row_frames*2-1));
    // The full final tick must sound, although Player.row has already advanced.
    CHECK(p.channels[0].playing);
    CHECK(r.position().row==1);
    CHECK(r.render(out.data(),1,2,rate)==Status::Ok);
    CHECK(r.position().clock==uint64_t(row_frames*2));
    CHECK(r.render(out.data(),1,2,rate)==Status::Ok);
    CHECK(!r.position().playing && r.position().clock==uint64_t(row_frames*2));
    CueEvent e; CHECK(r.pop_cue(&e) && e.id==7 && e.clock==0 && e.sample_offset==0);
    CHECK(r.pop_cue(&e) && e.id==9 && e.clock==0);
    CHECK(r.pop_cue(&e) && e.id==8 && e.clock==uint64_t(row_frames));
    CHECK(!r.pop_cue(&e));
  }
}
static void test_interactive_requests_pause() {
  ntrk_test::InteractiveFixture f; RuntimeBinding b;
  CHECK(b.compile(f.module,42,f.caps,3)==Status::Ok);
  Player p; mix::Mixer mx; Runtime r; CHECK(r.bind(b,p,mx,42,48000)==Status::Ok);
  double out[2048]={};
  CHECK(r.request_section(42,1,4)==Status::Ok);
  r.render(out,1,2,48000); CHECK(r.position().current==3 && r.position().pending==4);
  r.render(out,959,2,48000); CHECK(r.position().current==3);
  // Receipt exactly at row boundary excludes that boundary, even if not rendered yet.
  CHECK(r.request_section(42,2,4)==Status::Ok);
  r.render(out,1,2,48000); CHECK(r.position().current==3);
  r.render(out,959,2,48000); r.render(out,1,2,48000);
  CHECK(r.position().current==4 && r.position().clock==1921);
  const double pos=p.channels[0].pos, until=p.until_tick;
  const auto clock=r.position().clock;
  r.pause(true); r.layer(42,3,2,0,10); r.render(out,512,2,48000);
  CHECK(r.position().clock==clock && p.channels[0].pos==pos && p.until_tick==until);
  CHECK(r.change_rate(96000)==Status::Ok); r.pause(false); r.render(out,1,2,96000);
  CHECK(r.position().clock==clock+1);
}
static uint64_t interactive_hash(const std::vector<double>& data) {
  uint64_t hash=1469598103934665603ull;
  for (size_t i=0;i<data.size();++i) { uint64_t bits; std::memcpy(&bits,&data[i],8); hash^=bits; hash*=1099511628211ull; }
  return hash;
}
static void test_interactive_partition() {
  for (double rate : {44100.,48000.,96000.}) {
    std::vector<double> reference; std::vector<uint64_t> reference_cues;
    for (int chunk : {1,64,128,512,97}) {
      ntrk_test::InteractiveFixture f(16,6);
      // 170 BPM leaves a fractional tick residual at all three rates. Partition
      // parity must survive it across repeated two-row loop wraps.
      f.module.bpm=170;
      for (int i=2;i<4;++i) {
        ImusSectionView section; CHECK(imus_section(f.records[i],&section)); section.entry_bpm=170;
        size_t bytes=0; CHECK(imus_body_encode(section,f.bodies[i],96,&bytes));
      }
      f.encode();
      RuntimeBinding b; CHECK(b.compile(f.module,1,f.caps,3)==Status::Ok);
      Player p; mix::Mixer mx; Runtime r; CHECK(r.bind(b,p,mx,1,rate)==Status::Ok);
      CHECK(r.layer(1,1,2,8192,70)==Status::Ok);
      std::vector<double> out(30000*2,0.0);
      const int irregular[]={1,7,97,512,3,128,64};
      std::vector<uint64_t> cues; uint64_t previous=0; int events=0,call=0;
      for (int done=0;done<30000;) {
        const int want=chunk==97 ? irregular[call++%7] : chunk;
        const int n=std::min(want,30000-done); r.render(out.data()+done*2,n,2,rate);
        CueEvent e;
        while (r.pop_cue(&e)) {
          CHECK(e.clock==uint64_t(done)+e.sample_offset);
          CHECK(e.clock>=previous && e.sample_offset<uint32_t(n)); previous=e.clock; ++events;
          cues.push_back(e.clock); cues.push_back(e.id); cues.push_back(e.traversal); cues.push_back(e.sequence);
        }
        done+=n;
      }
      if (reference.empty()) { reference=out; reference_cues=cues; printf("interactive trace %.0f %016llx\n",rate,(unsigned long long)interactive_hash(out)); }
      else { CHECK(out==reference); CHECK(cues==reference_cues); }
      CHECK(events>=3);
    }
  }
}
static void test_interactive_saturation_tail() {
  ntrk_test::InteractiveFixture f(1,1,true,true); RuntimeBinding b; CHECK(b.compile(f.module,1,f.caps,3)==Status::Ok);
  Player p; mix::Mixer mx; Runtime r; CHECK(r.bind(b,p,mx,1,48000)==Status::Ok);
  std::vector<double> out(1920*100*2,0.0); r.render(out.data(),1920*100,2,48000);
  CHECK(r.position().cue_drops==44);
  CueEvent e; int count=0; while (r.pop_cue(&e)) ++count; CHECK(count==256);
  const uint64_t clock=r.position().clock; r.stop(true); r.render(out.data(),512,2,48000);
  CHECK(!r.position().playing && !r.position().draining && r.position().clock==clock+480);
}
static void test_interactive_macro_gain_tail() {
  // Host defaults and updates outrank the row's meta lane every row, not once.
  ntrk_test::InteractiveFixture f;
  for (int row=0;row<4;++row) { f.fx[row*2+1].cmd=1; f.fx[row*2+1].param=0; }
  RuntimeBinding b; CHECK(b.compile(f.module,1,f.caps,3)==Status::Ok);
  Player p; mix::Mixer mx; Runtime r; CHECK(r.bind(b,p,mx,1,48000)==Status::Ok);
  double output[2048]={}; r.render(output,1,2,48000); CHECK(p.pan[0]==1.f);
  CHECK(r.control(1,1,1,0)==Status::Ok); r.render(output,960,2,48000); CHECK(p.pan[0]==-1.f);
  r.render(output,960,2,48000); CHECK(p.pan[0]==-1.f);

  // Identical mono samples in both stereo sides; compare the full ramp against
  // an independent unscaled render. Every frame advances voices even at zero.
  ntrk_test::InteractiveFixture g;
  int8_t stereo[64]; for (int i=0;i<32;++i) stereo[2*i]=stereo[2*i+1]=g.pcm[i];
  g.module.instruments[0].data=stereo; g.module.instruments[0].flags|=kInstrumentStereo;
  RuntimeBinding bg; CHECK(bg.compile(g.module,2,g.caps,3)==Status::Ok);
  Player pa,pb; mix::Mixer ma,mb; Runtime ra,rb;
  CHECK(ra.bind(bg,pa,ma,2,48000)==Status::Ok); CHECK(rb.bind(bg,pb,mb,2,48000)==Status::Ok);
  CHECK(ra.control(2,1,1,32896)==Status::Ok); CHECK(rb.control(2,1,1,32896)==Status::Ok);
  CHECK(rb.layer(2,2,2,0,10)==Status::Ok);
  double a[1024]={},bb[1024]={}; ra.render(a,512,2,48000); rb.render(bb,512,2,48000);
  bool signal=false;
  for (int i=0;i<512;++i) {
    const double gain=i<480 ? 1.0-double(i+1)/480.0 : 0.0;
    CHECK(std::fabs(bb[i*2]-a[i*2]*gain)<2e-8);
    CHECK(bb[i*2]==bb[i*2+1]); signal=signal || a[i*2]!=0.0;
  }
  CHECK(signal && pa.channels[0].pos==pb.channels[0].pos);

  // A wet delay is pre-filled before a transition. Carry keeps its actual samples;
  // cut clears them. A zero host layer stops new dry AND send input while the
  // existing shared delay continues, independently of the advancing voice.
  double tail_energy[2]={};
  for (int carry=0;carry<2;++carry) {
    ntrk_test::InteractiveFixture t(1,1,true,carry!=0);
    mix::Mixer seed;
    seed.send[0].param[0]=0.0005f; seed.send[0].param[1]=0.8f; seed.send[0].param[3]=1.f;
    mix::slot_set_kind(&seed.send[0],mix::FxKind::kDelay); seed.send_level[0][0]=1.f;
    mix::mixer_config_write(&seed,&t.module);
    RuntimeBinding bt; CHECK(bt.compile(t.module,3,t.caps,3)==Status::Ok);
    Player pt; mix::Mixer mt; Runtime rt;
    std::vector<float> memory(fx::delay_bytes_needed(2.f,48000.f)/sizeof(float)+1,0.f);
    CHECK(fx::delay_init(&mt.send[0].delay,memory.data(),memory.size()*sizeof(float),48000.f,2.f));
    CHECK(rt.bind(bt,pt,mt,3,48000)==Status::Ok);
    rt.render(output,512,2,48000);
    const double voice_before=pt.channels[0].pos;
    CHECK(rt.layer(3,1,2,0,0)==Status::Ok);
    std::memset(output,0,sizeof output); rt.render(output,128,2,48000);
    double ringing=0; for (int i=0;i<256;++i) ringing+=std::fabs(output[i]); CHECK(ringing>0);
    CHECK(pt.channels[0].pos!=voice_before);
    CHECK(rt.request_section(3,2,4)==Status::Ok);
    rt.render(output,320,2,48000); // now exactly at the next row boundary
    std::memset(output,0,sizeof output); rt.render(output,128,2,48000);
    CHECK(rt.position().current==4);
    for (int i=0;i<256;++i) tail_energy[carry]+=std::fabs(output[i]);
  }
  CHECK(tail_energy[0]==0.0 && tail_energy[1]>0.0);
}
static void test_interactive_terminal_and_preview() {
  for (int speed : {1,6}) {
    ntrk_test::InteractiveFixture f(1,speed);
    RuntimeBinding b; CHECK(b.compile(f.module,1,f.caps,4)==Status::Ok);
    Player p; mix::Mixer mx; Runtime r; CHECK(r.bind(b,p,mx,1,48000)==Status::Ok);
    const int duration=4*960*speed;
    std::vector<double> out(size_t(duration+1)*2,0.0);
    r.render(out.data(),duration,2,48000);
    CHECK(r.position().traversal==0 && r.position().clock==uint64_t(duration));
    const uint64_t ticks=p.ticks_elapsed;
    r.render(out.data(),1,2,48000);
    CHECK(r.position().traversal==1 && p.ticks_elapsed==ticks+1);
    CueEvent cue; CHECK(r.pop_cue(&cue) && cue.id==10 && cue.clock==0);
    CHECK(r.pop_cue(&cue) && cue.id==10 && cue.clock==uint64_t(duration));
  }
  ntrk_test::InteractiveFixture f;
  // No sections: controls and layers accompany the ordinary arrangement, including Fxx.
  f.count=2; f.module.imus=nullptr; f.module.imus_bytes=0;
  f.notes[0].effect=0x0f; f.notes[0].param=3; f.encode();
  RuntimeBinding b; CHECK(b.compile(f.module,3,f.caps,0)==Status::Ok);
  Player p; mix::Mixer mx; Runtime r; CHECK(r.bind(b,p,mx,3,48000)==Status::Ok);
  double out[128]={}; r.render(out,64,2,48000); CHECK(p.speed==3);
  Player preview,ordinary; mix::Mixer ma,mb;
  Module linear=f.module; linear.imus=nullptr; linear.imus_bytes=0;
  player_start_ex(&preview,&f.module,PlaybackMode::LinearPreview); player_start(&ordinary,&linear);
  double a[2048]={},bb[2048]={};
  mix::mixer_render_add(&ma,&preview,a,1024,2,48000); mix::mixer_render_add(&mb,&ordinary,bb,1024,2,48000);
  CHECK(std::memcmp(a,bb,sizeof a)==0);
  // An off-grid section end always wins over the outgoing bar quantization.
  ntrk_test::InteractiveFixture g;
  ImusSectionView section; CHECK(imus_section(g.records[2],&section));
  section.start_row=1; section.end_row=3; section.quantization=2;
  size_t bytes=0; CHECK(imus_body_encode(section,g.bodies[2],96,&bytes)); g.encode();
  CHECK(b.compile(g.module,4,g.caps,3)==Status::Ok);
  p.module=nullptr; CHECK(r.bind(b,p,mx,4,48000)==Status::Ok);
  r.render(a,1,2,48000); CHECK(r.request_section(4,1,3)==Status::Ok);
  CHECK(r.request_section(4,2,4)==Status::Ok); // latest before the boundary wins
  r.render(a,960,2,48000); CHECK(r.position().current==3);
  r.render(a,959,2,48000); r.render(a,1,2,48000); CHECK(r.position().current==4);
}
static void test_interactive_loop_continuity() {
  ntrk_test::InteractiveFixture f;
  f.notes[0]=Note();
  mix::Mixer seed; seed.send[0].param[0]=0.001f; seed.send[0].param[1]=0.9f; seed.send[0].param[3]=1.f;
  mix::slot_set_kind(&seed.send[0],mix::FxKind::kDelay); seed.send_level[0][0]=1.f; mix::mixer_config_write(&seed,&f.module);
  RuntimeBinding a,b; CHECK(a.compile(f.module,1,f.caps,3)==Status::Ok); CHECK(b.compile(f.module,1,f.caps,4)==Status::Ok);
  Player pa,pb; mix::Mixer ma,mb; Runtime ra,rb;
  const size_t count=fx::delay_bytes_needed(2.f,48000.f)/sizeof(float)+1;
  std::vector<float> ta(count),tb(count);
  CHECK(fx::delay_init(&ma.send[0].delay,ta.data(),ta.size()*sizeof(float),48000.f,2.f));
  CHECK(fx::delay_init(&mb.send[0].delay,tb.data(),tb.size()*sizeof(float),48000.f,2.f));
  CHECK(ra.bind(a,pa,ma,1,48000)==Status::Ok); CHECK(rb.bind(b,pb,mb,1,48000)==Status::Ok);
  pa.channels[0].instrument=pb.channels[0].instrument=1;
  channel_trigger(&f.module,&pa.channels[0],13,0,48000); channel_trigger(&f.module,&pb.channels[0],13,0,48000);
  double oa[4096]={},ob[4096]={}; ra.render(oa,2048,2,48000); rb.render(ob,2048,2,48000);
  CHECK(ra.position().traversal==1 && rb.position().traversal==0);
  CHECK(pa.channels[0].playing && pa.channels[0].pos==pb.channels[0].pos);
  CHECK(pa.ticks_elapsed==pb.ticks_elapsed && pa.until_tick==pb.until_tick);
  CHECK(std::memcmp(oa,ob,sizeof oa)==0 && ta==tb);
}
static void test_interactive() {
  test_interactive_loop_continuity();
  test_interactive_terminal_and_preview();
  test_interactive_macro_gain_tail();
  test_interactive_binding(); test_interactive_ramps(); test_interactive_timing();
  test_interactive_requests_pause(); test_interactive_partition(); test_interactive_saturation_tail();
}
