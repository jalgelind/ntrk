#include "interactive_fixture.h"
#include <vector>
#include <algorithm>
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
  r.render(out,1,2,48000); CHECK(r.position().current==3 && r.position().pending==0);
  r.render(out,959,2,48000); CHECK(r.position().current==3);
  // Receipt exactly at row boundary excludes that boundary, even if not rendered yet.
  CHECK(r.request_section(42,1,4)==Status::Ok);
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
// frames_until oracle: a second, identical runtime stepped one frame at a time
// records where rows really start (change of traversal/row_offset/section).
namespace {
struct SyncAct { uint64_t clock; int kind; double rate; }; // 0 request section 4, 1 pause+unpause, 2 rate
struct SyncRun { uint64_t checks=0, bad=0, hash=1469598103934665603ull; };
static void sync_setup(ntrk_test::InteractiveFixture &f,int speed,int bpm,int rpb) {
  f.module.bpm=uint16_t(bpm); f.module.speed=speed; f.module.rows_per_beat=rpb; f.module.rows_per_bar=rpb*2;
  for (int i=2;i<4;++i) {
    ImusSectionView v; imus_section(f.records[i],&v);
    v.entry_bpm=uint16_t(bpm); v.entry_speed=uint16_t(speed); v.end_action=1; v.quantization=i==2 ? 2 : 1;
    if (i==2) { v.end_order=1; v.end_row=0; } // section 3: 4 rows, bar quantized; 4: 4 rows, beat
    size_t bytes=0; imus_body_encode(v,f.bodies[i],96,&bytes); f.records[i].bytes=uint32_t(bytes);
  }
  f.encode();
}
// bias != 0 is the negative control: a deliberately wrong answer must be caught.
static SyncRun sync_run(double rate,int speed,int bpm,int rpb,int chunk,uint64_t horizon,const std::vector<SyncAct> &acts,uint32_t bias) {
  SyncRun res;
  ntrk_test::InteractiveFixture fa(1,speed),fb(1,speed); sync_setup(fa,speed,bpm,rpb); sync_setup(fb,speed,bpm,rpb);
  RuntimeBinding ba,bb; ba.compile(fa.module,1,fa.caps,3); bb.compile(fb.module,1,fb.caps,3);
  Player pa,pb; mix::Mixer ma,mb; Runtime a,b; a.bind(ba,pa,ma,1,rate); b.bind(bb,pb,mb,1,rate);
  // Oracle: starts[i] = {clock, row_offset} of every row start; paused spans add no clock.
  struct Start { uint64_t clock; uint32_t ro; };
  std::vector<Start> starts; std::vector<double> sink(2*512,0.0);
  uint64_t seq=0;
  auto act_at=[&](Runtime &r,double &cur,uint64_t clock,bool oracle) {
    for (size_t i=0;i<acts.size();++i) if (acts[i].clock==clock) {
      if (acts[i].kind==0) r.request_section(1,++seq,4);
      else if (acts[i].kind==2) { r.change_rate(acts[i].rate); cur=acts[i].rate; }
      (void)oracle;
    }
  };
  { double cur=rate; seq=0; uint64_t last=~0ull; uint64_t key=~0ull;
    for (uint64_t c=0;c<horizon+4*uint64_t(rate);++c) {
      if (c<horizon) { act_at(b,cur,c,true); } else if (c==horizon) {}
      sink.assign(2,0.0); b.render(sink.data(),1,2,cur);
      const Position p=b.position();
      const uint64_t k=(p.traversal<<40)^(uint64_t(p.row_offset)<<8)^p.current;
      if (k!=key) { key=k; starts.push_back(Start{c,p.row_offset}); } (void)last;
      if (!p.playing) break;
    }
  }
  seq=0; double cur=rate; std::vector<double> out(2*600,0.0); int call=0; const int irregular[]={1,7,97,512,3,128,64};
  uint64_t done=0;
  auto query=[&](uint64_t clock) {
    const Quant qs[3]={Quant::Row,Quant::Beat,Quant::Bar};
    const uint32_t grids[3]={1,uint32_t(rpb),uint32_t(rpb*2)};
    for (int q=0;q<3;++q) {
      uint64_t want=~0ull;
      for (size_t i=0;i<starts.size();++i) if (starts[i].clock>=clock && starts[i].ro%grids[q]==0) { want=starts[i].clock-clock; break; }
      const uint32_t got=a.frames_until(qs[q])+bias;
      res.hash^=got; res.hash*=1099511628211ull; ++res.checks;
      // A later request or rate change legitimately moves the boundary: only compare absent those.
      bool later=false; for (size_t i=0;i<acts.size();++i) if (acts[i].kind!=1 && acts[i].clock>clock && acts[i].clock<=clock+want) later=true;
      if (want!=~0ull && !later && uint64_t(got)!=want) { if (!res.bad && !bias) printf("sync mismatch rate %.0f speed %d bpm %d clock %llu q %d got %u want %llu\n",cur,speed,bpm,(unsigned long long)clock,q,got,(unsigned long long)want); ++res.bad; }
    }
  };
  while (done<horizon) {
    act_at(a,cur,done,false);
    for (size_t i=0;i<acts.size();++i) if (acts[i].clock==done && acts[i].kind==1) {
      a.pause(true); CHECK(a.frames_until(Quant::Row)==UINT32_MAX && a.frames_until(Quant::Bar)==UINT32_MAX);
      a.render(out.data(),64,2,cur); CHECK(a.position().clock==done); a.pause(false);
    }
    query(done);
    uint64_t n=uint64_t(chunk==97 ? irregular[call++%7] : chunk);
    for (size_t i=0;i<acts.size();++i) if (acts[i].clock>done && acts[i].clock-done<n) n=acts[i].clock-done;
    if (n>horizon-done) n=horizon-done;
    out.assign(size_t(n)*2,0.0); a.render(out.data(),int(n),2,cur); done+=n;
  }
  return res;
}
}
static void test_interactive_frames_until() {
  struct Case { int speed,bpm,rpb; };
  const Case cases[]={{6,125,4},{1,255,4},{3,170,2},{31,255,4},{6,32,2},{3,125,3},{1,32,4},{31,125,2}};
  uint64_t hash=1469598103934665603ull,total=0;
  for (double rate : {44100.,48000.,96000.}) for (const Case &c : cases) {
    const double fpt=rate*2.5/c.bpm; const uint64_t row=uint64_t(fpt*c.speed);
    if (row>60000 && rate!=48000.) continue; // keep the slow tempos at one rate
    const uint64_t horizon=row*20+3;
    // Request lands mid-row of section 3 (bar-quantized), later a pause and a rate change.
    const std::vector<SyncAct> acts={{row*3+row/3,0,0},{row*9+5,1,0},{row*13+7,2,rate==44100. ? 48000. : 44100.}};
    for (int chunk : {1,64,128,512,97}) {
      if (chunk==1 && row>3000) continue;
      SyncRun r=sync_run(rate,c.speed,c.bpm,c.rpb,chunk,horizon,acts,0);
      CHECK(r.bad==0 && r.checks>0);
      if (chunk==97) { hash^=r.hash; hash*=1099511628211ull; total+=r.checks; }
      // Negative control: the same comparison with a one-frame error must fail.
      if (chunk==128) CHECK(sync_run(rate,c.speed,c.bpm,c.rpb,chunk,horizon,acts,1).bad>0);
    }
  }
  printf("interactive sync %016llx %llu\n",(unsigned long long)hash,(unsigned long long)total);
  // Unavailable answers: unbound, paused, no section for Beat/Bar, stopped.
  Runtime idle; CHECK(idle.frames_until(Quant::Row)==UINT32_MAX);
  ntrk_test::InteractiveFixture f; RuntimeBinding b; CHECK(b.compile(f.module,1,f.caps,3)==Status::Ok);
  Player p; mix::Mixer mx; Runtime r; CHECK(r.bind(b,p,mx,1,48000)==Status::Ok);
  CHECK(r.frames_until(Quant::Row)==0 && r.frames_until(Quant::Bar)==0);
  r.stop(false); CHECK(r.frames_until(Quant::Row)==UINT32_MAX);
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
static void test_interactive_stop_capability_and_defaults() {
  ntrk_test::InteractiveFixture f;
  Runtime cold; Player untouched; mix::Mixer empty;
  RuntimeBinding invalid, binding;
  CHECK(cold.stop(true)==Status::Stale); CHECK(cold.stop(false)==Status::Ok);
  CHECK(!cold.position().playing && cold.position().clock==0);
  CHECK(cold.bind(invalid,untouched,empty,1,48000)==Status::Stale);
  CHECK(!untouched.playing && untouched.module==nullptr);
  Capabilities defaults; CHECK(binding.compile(f.module,1,defaults,3)==Status::Unsupported);
  for (int missing=0;missing<8;++missing) {
    auto caps=f.caps;
    switch (missing) {
      case 0: caps.held_controls=false; break; case 1: caps.layer_ramps=false; break;
      case 2: caps.safe_entry=false; break; case 3: caps.cut=false; break;
      case 4: caps.pause_freezes=false; break; case 5: caps.rate_remaining_time=false; break;
      case 6: caps.tail_deadline_ms=0; break; case 7: caps.tail_deadline_ms=60001; break;
    }
    CHECK(binding.compile(f.module,1,caps,3)==Status::Unsupported);
  }
  ntrk_test::InteractiveFixture required_carry(1,1,true,true);
  auto cut_only=f.caps; cut_only.carry=false;
  CHECK(binding.compile(required_carry.module,1,cut_only,3)==Status::Unsupported);
  mix::Mixer seed; seed.send[0].param[0]=0.0005f; seed.send[0].param[1]=0.8f; seed.send[0].param[3]=1.f;
  mix::slot_set_kind(&seed.send[0],mix::FxKind::kDelay); seed.send_level[0][0]=1.f; mix::mixer_config_write(&seed,&f.module);
  CHECK(binding.compile(f.module,1,cut_only,3)==Status::Ok);
  Player pa,pb; mix::Mixer ma,mb; Runtime ra,rb;
  const size_t count=fx::delay_bytes_needed(2.f,48000.f)/sizeof(float)+1;
  std::vector<float> ta(count),tb(count);
  CHECK(fx::delay_init(&ma.send[0].delay,ta.data(),ta.size()*sizeof(float),48000.f,2.f));
  CHECK(fx::delay_init(&mb.send[0].delay,tb.data(),tb.size()*sizeof(float),48000.f,2.f));
  CHECK(ra.bind(binding,pa,ma,1,48000)==Status::Ok); CHECK(rb.bind(binding,pb,mb,1,48000)==Status::Ok);
  CHECK(ra.position().playing && ra.position().clock==0 && ra.position().current==3);
  double a[1024]={},b[1024]={}; ra.render(a,512,2,48000); rb.render(b,512,2,48000);
  CHECK(pa.channels[0].playing && pa.pan[0]==1.f);
  CHECK(ra.layer(1,1,2,12345,100)==Status::Ok); CHECK(rb.layer(1,1,2,12345,100)==Status::Ok);
  ra.render(a,128,2,48000); rb.render(b,128,2,48000);
  CHECK(ra.request_section(1,2,4)==Status::Ok); CHECK(rb.request_section(1,2,4)==Status::Ok);
  std::vector<unsigned char> runtime_before(sizeof ra),player_before(sizeof pa),mixer_before(sizeof ma);
  std::memcpy(runtime_before.data(),&ra,sizeof ra); std::memcpy(player_before.data(),&pa,sizeof pa); std::memcpy(mixer_before.data(),&ma,sizeof ma);
  const auto tail_before=ta;
  CHECK(ra.stop(true)==Status::Unsupported);
  CHECK(std::memcmp(runtime_before.data(),&ra,sizeof ra)==0);
  CHECK(std::memcmp(player_before.data(),&pa,sizeof pa)==0);
  CHECK(std::memcmp(mixer_before.data(),&ma,sizeof ma)==0 && ta==tail_before);
  std::memset(a,0,sizeof a); std::memset(b,0,sizeof b);
  ra.render(a,512,2,48000); rb.render(b,512,2,48000);
  CHECK(std::memcmp(a,b,sizeof a)==0 && ta==tb);
  CHECK(ra.position().current==4 && ra.position().clock==rb.position().clock);
  double energy=0; for (double v : a) energy+=std::fabs(v); CHECK(energy>0);
  CHECK(ra.stop(false)==Status::Ok); CHECK(!ra.position().playing && !ra.position().draining);
  CHECK(!pa.channels[0].playing); CHECK(std::all_of(ta.begin(),ta.end(),[](float v){ return v==0.f; }));
  std::memset(a,0,sizeof a); ra.render(a,512,2,48000);
  CHECK(std::all_of(a,a+1024,[](double v){ return v==0.0; }));
}
static void test_interactive_drain_freezes_automation() {
  for (bool section_end : {false,true}) {
  ntrk_test::InteractiveFixture f(1,1,!section_end,true);
  mix::Mixer seed; seed.send[0].param[0]=0.0005f; seed.send[0].param[1]=0.05f; seed.send[0].param[3]=1.f;
  mix::slot_set_kind(&seed.send[0],mix::FxKind::kDelay); seed.send_level[0][0]=1.f; mix::mixer_config_write(&seed,&f.module);
  f.fx[1].cmd=0x41; f.fx[1].param=192; // authored send-0 feedback, different from MIXR
  RuntimeBinding binding; CHECK(binding.compile(f.module,1,f.caps,3)==Status::Ok);
  Player p; mix::Mixer mx; Runtime r;
  const size_t count=fx::delay_bytes_needed(2.f,48000.f)/sizeof(float)+1;
  std::vector<float> memory(count);
  CHECK(fx::delay_init(&mx.send[0].delay,memory.data(),memory.size()*sizeof(float),48000.f,2.f));
  CHECK(r.bind(binding,p,mx,1,48000)==Status::Ok);
  const int lead=section_end ? 1920 : 512;
  double warm[3840]={}; r.render(warm,lead,2,48000);
  const float automated=mx.send[0].param[1]; CHECK(automated>0.7f && mx.send[0].base[1]<0.1f);
  // Independent reference: leave the mixer's current knobs/buffers alone and
  // feed silence with the ordinary, nonsequencing player. A reset is a negative control.
  Module linear=f.module; linear.imus=nullptr; linear.imus_bytes=0;
  Player ref=p; ref.module=&linear; ref.sequencing=false;
  for (int c=0;c<kMaxChannels;++c) ref.channels[c].playing=false;
  mix::Mixer reference=mx,reset=mx; auto ref_memory=memory,reset_memory=memory;
  reference.send[0].delay.buffer=ref_memory.data(); reset.send[0].delay.buffer=reset_memory.data();
  mix::slot_revert(&reset.send[0]);
  const auto before=memory;
  if (!section_end) {
    CHECK(r.stop(true)==Status::Ok); CHECK(memory==before && mx.send[0].param[1]==automated);
  }
  double actual[800]={},expected[800]={},negative[800]={};
  r.render(actual,400,2,48000);
  CHECK(mx.fxpl_cmd[0][0]==0 && mx.fxpl_mem[0][0]==0 && mx.macro_pending==0);
  mix::mixer_render_add(&reference,&ref,expected,400,2,48000);
  Player ref_reset=p; ref_reset.module=&linear; ref_reset.playing=true; ref_reset.sequencing=false;
  mix::mixer_render_add(&reset,&ref_reset,negative,400,2,48000);
  CHECK(std::memcmp(actual,expected,sizeof actual)==0);
  CHECK(std::memcmp(actual,negative,sizeof actual)!=0);
  printf("interactive trace drain-%s %016llx\n",section_end ? "section" : "public",
         (unsigned long long)interactive_hash(std::vector<double>(actual,actual+800)));
  double energy=0; for (double v : actual) energy+=std::fabs(v); CHECK(energy>0);
  CHECK(mx.send[0].param[1]==automated && r.position().draining);
  r.render(warm,80,2,48000); CHECK(!r.position().draining && r.position().clock==uint64_t(lead+480));
  }
}
static void test_interactive_idempotent_section_selection() {
  ntrk_test::InteractiveFixture f; RuntimeBinding binding;
  CHECK(binding.compile(f.module,1,f.caps,3)==Status::Ok);
  Player pa,pb; mix::Mixer ma,mb; Runtime ra,rb;
  CHECK(ra.bind(binding,pa,ma,1,48000)==Status::Ok); CHECK(rb.bind(binding,pb,mb,1,48000)==Status::Ok);
  double a[2048]={},b[2048]={}; ra.render(a,400,2,48000); rb.render(b,400,2,48000);
  CHECK(ra.request_section(1,1,4)==Status::Ok);
  CHECK(ra.request_section(1,2,3)==Status::Ok); CHECK(ra.position().pending==0);
  // Selecting current repeatedly neither restarts nor emits extra row-zero cues.
  for (uint64_t seq=3;seq<8;++seq) CHECK(ra.request_section(1,seq,3)==Status::Ok);
  std::memset(a,0,sizeof a); std::memset(b,0,sizeof b); ra.render(a,800,2,48000); rb.render(b,800,2,48000);
  CHECK(std::memcmp(a,b,sizeof a)==0 && pa.channels[0].pos==pb.channels[0].pos);
  CHECK(ra.position().current==3 && ra.position().traversal==rb.position().traversal);
  CueEvent ca,cb; int cues=0;
  while (ra.pop_cue(&ca)) { CHECK(rb.pop_cue(&cb)); CHECK(ca.id==cb.id && ca.clock==cb.clock && ca.traversal==cb.traversal); ++cues; }
  CHECK(cues==3 && !rb.pop_cue(&cb));
  printf("interactive trace selection %016llx\n",
         (unsigned long long)interactive_hash(std::vector<double>(a,a+1600)));
  CHECK(ra.request_section(1,8,4)==Status::Ok);
  ra.render(a,720,2,48000); // first receipt at 1200, eligible end boundary at 1920
  CHECK(ra.request_section(1,9,4)==Status::Ok); // duplicate at boundary cannot postpone it
  ra.render(a,1,2,48000); CHECK(ra.position().current==4 && ra.position().clock==1921);
  CHECK(ra.position().accepted_sequence==9);
}
static void test_interactive_observable_state() {
  ntrk_test::InteractiveFixture f; RuntimeBinding binding; Player p; mix::Mixer mx; Runtime r;
  CHECK(binding.compile(f.module,81,f.caps,3)==Status::Ok);
  CHECK(r.bind(binding,p,mx,81,48000)==Status::Ok);
  double out[256]={}; CHECK(r.render(out,32,2,48000)==Status::Ok);
  CHECK(r.control(81,1,1,0)==Status::Ok); CHECK(r.layer(81,2,2,0,100)==Status::Ok);
  CHECK(r.render(out,64,2,48000)==Status::Ok);
  Position pos=r.position(); CHECK(pos.controls==1 && pos.layers==1);
  CHECK(pos.control[0].id==1 && pos.control[0].held==0 && pos.control[0].applied==255);
  CHECK(pos.layer[0].id==2 && pos.layer[0].target==0 && pos.layer[0].remaining==4736);
  CHECK(r.set_pause(81,3,true)==Status::Ok);
  CHECK(r.layer(81,4,2,65535,50)==Status::Ok); CHECK(r.render(out,64,2,48000)==Status::Ok);
  const Position paused=r.position(); CHECK(paused.clock==pos.clock);
  CHECK(paused.layer[0].current==pos.layer[0].current && paused.layer[0].held);
  CHECK(paused.layer[0].held_value==65535 && paused.layer[0].held_fade_ms==50);
}
static void test_interactive() {
  test_interactive_observable_state();
  test_interactive_stop_capability_and_defaults();
  test_interactive_drain_freezes_automation();
  test_interactive_idempotent_section_selection();
  test_interactive_loop_continuity();
  test_interactive_terminal_and_preview();
  test_interactive_macro_gain_tail();
  test_interactive_binding(); test_interactive_ramps(); test_interactive_timing();
  test_interactive_requests_pause(); test_interactive_partition(); test_interactive_saturation_tail();
  test_interactive_frames_until();
}
