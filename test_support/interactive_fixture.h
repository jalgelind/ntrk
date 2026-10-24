#ifndef NTRK_TEST_INTERACTIVE_FIXTURE_H_
#define NTRK_TEST_INTERACTIVE_FIXTURE_H_
#include "ntrk_interactive.h"
#include <cstdlib>
#include <cstring>
namespace ntrk_test {
struct InteractiveFixture {
  ntrk::Module module;
  uint8_t order[2]={0,0};
  ntrk::Note notes[4*16]={};
  ntrk::FxCell fx[4*17]={};
  int8_t pcm[32]={};
  uint8_t wire[4096]={}, bodies[8][96]={};
  ntrk::ImusRecord records[8];
  int count=0;
  ntrk::interactive::Capabilities caps;
  explicit InteractiveFixture(int channels=1,int speed=1,bool loop=true,bool carry=false) {
    module.channels=channels; module.rows=4; module.order_count=2; module.pattern_count=1;
    module.order=order; module.patterns=notes; module.speed=speed; module.bpm=125;
    module.note_max=36; module.instrument_count=1; module.fx_columns=1; module.fx=fx;
    module.meta_columns=1; module.macro_count=1;
    module.macros[0].target_count=1;
    module.macros[0].targets[0].scope=0; module.macros[0].targets[0].target=0;
    module.macros[0].targets[0].scale=256;
    for (int i=0;i<32;++i) pcm[i]=i<16 ? 30 : -30;
    auto &ins=module.instruments[0]; ins.data=pcm; ins.length=32; ins.loop_len=32; ins.volume=32;
    notes[0].note=13; notes[0].instrument=1;
    caps.held_controls=caps.layer_ramps=caps.safe_entry=caps.cut=caps.carry=true;
    caps.pause_freezes=caps.rate_remaining_time=true; caps.tail_deadline_ms=10;
    ntrk::ImusControlView c; name(c); c.macro_index=1; c.default_value=65535;
    add(1,ntrk::kImusControl,c);
    ntrk::ImusLayerView l; name(l); l.channel_mask=uint16_t((1u<<channels)-1);
    add(2,ntrk::kImusLayer,l);
    ntrk::ImusSectionView s; name(s); s.end_order=0; s.end_row=2; s.entry_speed=uint16_t(speed); s.end_action=loop ? 1 : 0; s.tail_policy=carry ? 1 : 0;
    add(3,ntrk::kImusSection,s);
    s.start_order=1; s.start_row=0; s.end_order=2; s.end_row=0; s.quantization=2;
    add(4,ntrk::kImusSection,s);
    ntrk::ImusCueView q; name(q); q.section_id=3;
    add(9,ntrk::kImusCue,q);
    add(7,ntrk::kImusCue,q);
    q.row_offset=1; add(8,ntrk::kImusCue,q);
    q.section_id=4; q.row_offset=0; add(10,ntrk::kImusCue,q);
    encode();
  }
  static void name(ntrk::ImusNamedView& v) { v.name=reinterpret_cast<const uint8_t*>("test"); v.name_bytes=4; }
  template<class T> void add(uint32_t id,uint8_t kind,const T& view) {
    size_t n=0; if (!ntrk::imus_body_encode(view,bodies[count],96,&n)) std::abort();
    auto &r=records[count]; r.id=id; r.kind=kind; r.body=bodies[count]; r.bytes=uint32_t(n); ++count;
  }
  void encode() {
    size_t n=0; if (!ntrk::imus_encode(1,1,11,records,uint32_t(count),wire,sizeof wire,&n,&module)) std::abort();
    module.imus=wire; module.imus_bytes=uint32_t(n);
  }
};
}
#endif
