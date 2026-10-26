// Capability-bound profile-1 runtime. C++11; no allocation, locks or I/O.
// Compile/bind off audio. Borrow Module/IMUS for the complete binding lifetime.
// Only render() grants the mixer a temporary capability; direct render/skip refuses.
// See FORMAT.md for wire semantics and INTERACTIVE.md for host ownership/limits.
#ifndef NTRK_INTERACTIVE_H_
#define NTRK_INTERACTIVE_H_
#include "ntrk_mix.h"
#include <cmath>
#include <cassert>
#include <limits>
namespace ntrk { namespace interactive {
// Boundary grids for Runtime::frames_until: next row, beat or bar of the active section.
enum class Quant { Row, Beat, Bar };
enum class Status { Ok, Invalid, Unsupported, Stale, MissingSection, InvalidRate, InvalidSequence };
struct Capabilities {
  bool held_controls=false, layer_ramps=false, safe_entry=false, cut=false, carry=false;
  bool pause_freezes=false, rate_remaining_time=false;
  uint32_t tail_deadline_ms=0;
};
inline bool rate_supported(double rate) { return rate==44100.0 || rate==48000.0 || rate==96000.0; }
inline bool normalized(float v, uint16_t *out) {
  if (!out || !std::isfinite(v) || v<0.f || v>1.f) return false;
  *out=uint16_t(double(v)*65535.0+0.5); return true;
}
struct Control { uint32_t id=0; uint8_t macro=0; uint16_t value=0; };
struct Layer { uint32_t id=0; uint16_t mask=0, gain=65535, fade_ms=0; };
struct Section {
  uint32_t id=0, first=0, end=0;
  uint16_t bpm=125, speed=6;
  uint8_t quantization=0, action=0, tail=0;
};
struct Cue { uint32_t id=0, section=0, row=0; uint16_t type=1, strength=0; };
class Runtime;
// Compiler output has no borrowed names or TLVs. Publish as const, never mutate while bound.
class RuntimeBinding {
public:
  Status compile(const Module &m, uint64_t generation, const Capabilities &caps, uint32_t initial) {
    valid_=false;
    if (!generation || !m.order || !m.patterns || m.channels<1 || m.channels>kMaxChannels)
      return Status::Invalid;
    ImusView v;
    const ImusStatus status=imus_decode(m.imus,m.imus_bytes,&v,&m);
    if (status==ImusStatus::Invalid) return Status::Invalid;
    if (status!=ImusStatus::Supported) return Status::Unsupported;
    if (!caps.pause_freezes || !caps.rate_remaining_time || !caps.safe_entry || !caps.cut ||
        !caps.tail_deadline_ms || caps.tail_deadline_ms>60000) return Status::Unsupported;
    controls_=layers_=sections_=cues_=0;
    bool has_sections=false;
    size_t at=12; ImusRecord r;
    while (imus_next(v,&at,&r)) {
      if (r.kind==kImusSection) has_sections=true;
      if (r.version!=1) continue;
      if (r.kind==kImusControl) {
        if (!caps.held_controls) return Status::Unsupported;
        ImusControlView x; if (!imus_control(r,&x)) return Status::Invalid;
        Control &c=control_[controls_++]; c.id=r.id; c.macro=x.macro_index; c.value=x.default_value;
      } else if (r.kind==kImusLayer) {
        if (!caps.layer_ramps) return Status::Unsupported;
        ImusLayerView x; if (!imus_layer(r,&x)) return Status::Invalid;
        Layer &l=layer_[layers_++]; l.id=r.id; l.mask=x.channel_mask; l.gain=x.default_gain; l.fade_ms=x.fade_ms;
      } else if (r.kind==kImusSection) {
        ImusSectionView x; if (!imus_section(r,&x)) return Status::Invalid;
        if (x.tail_policy && !caps.carry) return Status::Unsupported;
        Section &s=section_[sections_++]; s.id=r.id; imus_span(m,x,&s.first,&s.end);
        s.bpm=x.entry_bpm; s.speed=x.entry_speed; s.quantization=x.quantization;
        s.action=x.end_action; s.tail=x.tail_policy;
      } else if (r.kind==kImusCue) {
        ImusCueView x; if (!imus_cue(r,&x)) return Status::Invalid;
        Cue &c=cue_[cues_++]; c.id=r.id; c.section=x.section_id; c.row=x.row_offset;
        c.type=x.type; c.strength=x.strength;
      }
    }
    // Bounded insertion sort off audio; runtime only scans immutable ID order.
    for (int i=1;i<cues_;++i) { Cue x=cue_[i]; int j=i; while (j && cue_[j-1].id>x.id) { cue_[j]=cue_[j-1]; --j; } cue_[j]=x; }
    initial_=-1;
    for (int i=0;i<sections_;++i) if (section_[i].id==initial) initial_=i;
    if (has_sections && initial_<0) return Status::MissingSection;
    if (!sections_ && initial) return Status::MissingSection;
    module_=&m; payload_=m.imus; bytes_=m.imus_bytes; generation_=generation; caps_=caps; valid_=true;
    return Status::Ok;
  }
  bool matches(const Module *m, uint64_t generation) const {
    return valid_ && m==module_ && generation==generation_ && m && m->imus==payload_ && m->imus_bytes==bytes_;
  }
  uint64_t generation() const { return generation_; }
private:
  friend class Runtime;
  bool valid_=false;
  const Module *module_=nullptr;
  const uint8_t *payload_=nullptr;
  uint32_t bytes_=0; uint64_t generation_=0;
  Capabilities caps_;
  Control control_[kMaxImusControls]; Layer layer_[kMaxImusLayers];
  Section section_[kMaxImusSections]; Cue cue_[kMaxImusCues];
  int controls_=0,layers_=0,sections_=0,cues_=0,initial_=-1;
};
struct CueEvent {
  uint64_t generation=0, sequence=0, clock=0, traversal=0;
  uint32_t id=0, section=0, sample_offset=0;
  uint16_t type=0, strength=0;
};
struct ControlPosition { uint32_t id=0; uint16_t held=0, applied=0; };
struct LayerPosition {
  uint32_t id=0; double current=1.0,target=1.0; uint64_t remaining=0;
  bool held=false; uint16_t held_value=0,held_fade_ms=0;
};
struct Position {
  uint64_t generation=0, clock=0, traversal=0, accepted_sequence=0;
  uint32_t current=0,pending=0,row_offset=0,cue_drops=0,order=0,row=0;
  bool playing=false,paused=false,draining=false;
  uint8_t controls=0,layers=0;
  ControlPosition control[kMaxImusControls]; LayerPosition layer[kMaxImusLayers];
};
struct Ramp {
  double current=1.0, start=1.0, target=1.0;
  uint64_t total=0, elapsed=0;
  void request(uint16_t value, uint16_t ms, double rate) {
    start=current; target=double(value)/65535.0; elapsed=0;
    total=uint64_t(std::ceil(double(ms)*rate/1000.0));
    if (!total) current=target;
  }
  // Rates are the runtime's supported integer Hz values; durations are bounded
  // to 60 seconds. Integer ceiling avoids a spurious frame at exact ratios.
  static uint64_t remaining_frames(uint64_t frames,double old_rate,double new_rate) {
    return (frames*uint64_t(new_rate)+uint64_t(old_rate)-1)/uint64_t(old_rate);
  }
  void change_rate(double old_rate,double new_rate) {
    if (elapsed<total) { total=remaining_frames(total-elapsed,old_rate,new_rate); elapsed=0; start=current; }
  }
  float next() {
    if (elapsed<total) { ++elapsed; current=elapsed==total ? target : start+(target-start)*double(elapsed)/double(total); }
    return float(current);
  }
};
// Single audio owner. UI commands arrive via a host transport, not concurrent calls here.
class Runtime {
public:
  Runtime() = default;
  Runtime(const Runtime&)=delete; Runtime& operator=(const Runtime&)=delete;
  Status bind(const RuntimeBinding &b, Player &p, mix::Mixer &mx, uint64_t generation,double rate) {
    if (!rate_supported(rate)) return Status::InvalidRate;
    if (!b.matches(p.module ? p.module : b.module_,generation)) return Status::Stale;
    binding_=&b; player_=&p; mixer_=&mx; rate_=rate; generation_=generation;
    clock_=traversal_=sequence_=cue_sequence_=0; drops_=0; head_=tail_=0;
    paused_=false; draining_=false; active_=true; rendering_=false;
    section_=b.initial_; pending_=-1; row_offset_=0; first_=true;
    for (int i=0;i<b.controls_;++i) held_[i]=b.control_[i].value;
    for (int i=0;i<b.layers_;++i) {
      ramp_[i]=Ramp(); ramp_[i].current=ramp_[i].target=ramp_[i].start=double(b.layer_[i].gain)/65535.0;
      layer_pending_[i]=false;
    }
    entry_(section_); audible_order_=uint32_t(p.order); audible_row_=uint32_t(p.row); return Status::Ok;
  }
  void unbind() {
    if (player_) { player_->runtime_allowed=nullptr; player_->runtime_cookie=nullptr; player_stop(player_); }
    binding_=nullptr; active_=false; draining_=false;
  }
  Status control(uint64_t gen,uint64_t seq,uint32_t id,uint16_t value) {
    Status s=check_(gen,seq); if (s!=Status::Ok) return s;
    for (int i=0;i<binding_->controls_;++i) if (binding_->control_[i].id==id) { held_[i]=value; sequence_=seq; return Status::Ok; }
    return Status::Invalid;
  }
  Status layer(uint64_t gen,uint64_t seq,uint32_t id,uint16_t value,uint16_t fade_ms) {
    Status s=check_(gen,seq); if (s!=Status::Ok) return s;
    if (fade_ms>kMaxImusFadeMs) return Status::Invalid;
    for (int i=0;i<binding_->layers_;++i) if (binding_->layer_[i].id==id) {
      sequence_=seq;
      if (paused_) { layer_pending_[i]=true; layer_value_[i]=value; layer_ms_[i]=fade_ms; }
      else ramp_[i].request(value,fade_ms,rate_);
      return Status::Ok;
    }
    return Status::Invalid;
  }
  Status request_section(uint64_t gen,uint64_t seq,uint32_t id) {
    Status s=check_(gen,seq); if (s!=Status::Ok) return s;
    for (int i=0;i<binding_->sections_;++i) if (binding_->section_[i].id==id) {
      sequence_=seq;
      // Selection is state: current cancels a pending transition; repeated
      // pending targets retain their original strictly-later boundary anchor.
      if (i==section_) pending_=-1;
      else if (i!=pending_) { pending_=i; request_clock_=clock_; }
      return Status::Ok;
    }
    return Status::Invalid;
  }
  Status set_pause(uint64_t gen,uint64_t seq,bool on) {
    Status s=check_(gen,seq); if (s!=Status::Ok) return s;
    sequence_=seq; pause(on); return Status::Ok;
  }
  void pause(bool on) {
    paused_=on;
    if (!on && binding_) for (int i=0;i<binding_->layers_;++i) if (layer_pending_[i]) {
      ramp_[i].request(layer_value_[i],layer_ms_[i],rate_); layer_pending_[i]=false;
    }
  }
  Status change_rate(double rate) {
    if (!rate_supported(rate)) return Status::InvalidRate;
    if (binding_) {
      for (int i=0;i<binding_->layers_;++i) ramp_[i].change_rate(rate_,rate);
      player_->until_tick=player_->until_tick/rate_*rate;
      if (draining_) tail_left_=Ramp::remaining_frames(tail_left_,rate_,rate);
      for (int i=0;i<player_->module->channels;++i) {
        Channel &ch=player_->channels[i];
        channel_set_step(&ch,rate);
        ch.env_step=float(double(ch.env_step)*rate_/rate); ch.env_fpms=float(rate/1000.0);
        if (ch.synth_rate>0.f && ch.instrument>0) {
          const Instrument &ins=player_->module->instruments[ch.instrument-1];
          Channel derived=ch; synth_trigger(&derived,ins,rate,true);
          const double age=double(ch.synth_age)/rate_*rate;
          ch.synth_age=age>=double(UINT32_MAX) ? UINT32_MAX : uint32_t(age);
          ch.synth_burst=derived.synth_burst; ch.synth_rate=float(rate);
          ch.synth_nenv_coef=derived.synth_nenv_coef; ch.synth_pitch_coef=derived.synth_pitch_coef;
          ch.synth_venv_coef=derived.synth_venv_coef;
          if (ins.synth_voice==uint8_t(SynthVoice::kBass)) {
            fx::fixed303_set(&ch.synth_fixed,float(rate));
            ch.synth_rc1_a=derived.synth_rc1_a; ch.synth_rc2_a=derived.synth_rc2_a;
            synth303_recoef(&ch,ins,ch.accent);
          } else ch.synth_env_coef=derived.synth_env_coef;
          ch.synth_step=ch.synth_step*rate_/rate;
          ch.synth_level_i=float(double(ch.synth_level_i)*rate_/rate);
          ch.synth_glide=ch.synth_glide*rate_/rate;
        }
      }

    }
    rate_=rate; return Status::Ok;
  }
  Status stop(bool carry) {
    if (!binding_) return carry ? Status::Stale : Status::Ok;
    if (carry && !binding_->caps_.carry) return Status::Unsupported;
    clear_voices_(); pending_=-1;
    draining_=carry;
    tail_left_=draining_ ? uint64_t(std::ceil(double(binding_->caps_.tail_deadline_ms)*rate_/1000.0)) : 0;
    active_=draining_;
    if (!draining_) { mix::mixer_reset(mixer_); player_->playing=false; }
    else {
      // Stop is not entry: freeze current automated parameters and buffers.
      // Clear only commands, then acknowledge the last musical tick so drain
      // cannot execute an authored row or restore the slot configuration.
      mix::fxpl_clear_commands(mixer_);
      mixer_->fxpl_synced=true;
      mixer_->fxpl_epoch=player_->pan_epoch; mixer_->fxpl_seen=player_->ticks_elapsed;
      player_->playing=true; player_->sequencing=false;
    }
    return Status::Ok;
  }
  // Adds into interleaved output. Musical clock counts active unpaused rendering
  // including tail drain, not paused/inactive host output or speaker latency.
  Status render(double *out,int frames,int channels,double rate) {
    if (!binding_ || !binding_->matches(player_->module,generation_)) return Status::Stale;
    if (rate!=rate_ || !rate_supported(rate)) return Status::InvalidRate;
    if (!out || frames<0 || channels<1) return Status::Invalid;
    if (paused_ || !active_) return Status::Ok;
    rendering_=true;
    for (int done=0;done<frames && active_;) {
      if (!draining_ && player_->sequencing && player_->until_tick<=0.0 && player_->tick==0) boundary_(uint32_t(done));
      if (!active_) break;
      // Do not pre-call player_run_begin: mixer must own its pre-tick FXPL snapshot.
      // Render one frame at a new tick, then bound runs by the authoritative residual.
      int n=player_->sequencing && player_->until_tick<=0.0 ? 1 : frames-done;
      if (player_->sequencing && player_->until_tick>0.0) {
        const int until=frames_until_tick(player_->until_tick); if (n>until) n=until;
      }
      if (n>kMaxBlock) n=kMaxBlock;
      if (draining_ && uint64_t(n)>tail_left_) n=int(tail_left_);
      if (n<=0) break;
      for (int f=0;f<n;++f) {
        for (int c=0;c<kMaxChannels;++c) gains_[f][c]=1.f;
        for (int i=0;i<binding_->layers_;++i) {
          const float g=ramp_[i].next();
          for (int c=0;c<player_->module->channels;++c) if (binding_->layer_[i].mask&(1u<<c)) gains_[f][c]=g;
        }
      }
      mix::mixer_render_add(mixer_,player_,out+size_t(done)*channels,n,channels,float(rate_),&gains_[0][0]);
      done+=n; clock_+=uint64_t(n);
      if (draining_) { tail_left_-=uint64_t(n); if (!tail_left_) { draining_=active_=false; mix::mixer_reset(mixer_); player_->playing=false; } }
      else if (!player_->playing) active_=false;
    }
    rendering_=false; return Status::Ok;
  }
  bool pop_cue(CueEvent *event) {
    if (!event || tail_==head_) return false;
    *event=events_[tail_%256]; ++tail_; return true;
  }
  Position position() const {
    Position p; p.accepted_sequence=sequence_; p.generation=generation_; p.clock=clock_; p.traversal=traversal_;
    p.current=binding_ && section_>=0 ? binding_->section_[section_].id : 0;
    p.pending=binding_ && pending_>=0 ? binding_->section_[pending_].id : 0;
    p.order=audible_order_; p.row=audible_row_;
    if (binding_) {
      p.controls=uint8_t(binding_->controls_); p.layers=uint8_t(binding_->layers_);
      for (int i=0;i<p.controls;++i) { p.control[i].id=binding_->control_[i].id; p.control[i].held=held_[i]; p.control[i].applied=mixer_->macro_in[binding_->control_[i].macro-1]; }
      for (int i=0;i<p.layers;++i) {
        LayerPosition &l=p.layer[i]; l.id=binding_->layer_[i].id; l.current=ramp_[i].current;
        l.target=ramp_[i].target; l.remaining=ramp_[i].total-ramp_[i].elapsed;
        l.held=layer_pending_[i]; l.held_value=layer_value_[i]; l.held_fade_ms=layer_ms_[i];
      }
    }
    p.row_offset=row_offset_; p.cue_drops=drops_; p.playing=active_ && !draining_; p.paused=paused_; p.draining=draining_; return p;
  }
  // Output frames from the start of the next render call to the first frame of
  // the next row / beat / bar of the active section's clock, or UINT32_MAX when
  // not exact (see INTERACTIVE.md). Const, allocation-free; replicates the
  // player's tick arithmetic fire by fire (a closed form drifts by a frame).
  uint32_t frames_until(Quant q) const {
    const uint32_t none=UINT32_MAX;
    if (!binding_ || !active_ || paused_ || draining_ || !player_->sequencing || !player_->playing) return none;
    if (q!=Quant::Row && section_<0) return none;
    const Section *s=section_>=0 ? &binding_->section_[section_] : nullptr;
    const uint32_t len=s ? s->end-s->first : 0;
    const uint32_t grid=q==Quant::Row ? 1u : uint32_t(q==Quant::Beat ? player_->module->rows_per_beat : player_->module->rows_per_bar);
    const uint32_t qgrid=s ? (s->quantization==0 ? 1u : uint32_t(s->quantization==1 ? player_->module->rows_per_beat : player_->module->rows_per_bar)) : 1u;
    if (!grid) return none;
    const double fpt=frames_per_tick(player_,rate_);
    const int spd=player_->speed<1 ? 1 : player_->speed;
    int t=player_->tick, extra=player_->extra_ticks;
    double u=player_->until_tick; uint64_t f=0;
    uint32_t k=first_ ? row_offset_ : row_offset_+1; // row that starts at the next tick==0 fire
    for (;;) {
      const uint64_t w=u<=0.0 ? 0u : uint64_t(frames_until_tick(u));
      f+=w; u-=double(w);
      if (t==0) {
        bool hit=!s;
        if (s) {
          const bool end=k>=len;
          if (clock_+f>request_clock_ && pending_>=0 && (end || k%qgrid==0)) hit=true; // new section row 0
          else if (end) { if (!s->action) return none; hit=true; }                  // loop wrap row 0
          else hit=k%grid==0;
        }
        if (hit) return f>=none ? none-1 : uint32_t(f);
        ++k;
      }
      u+=fpt;
      if (++t>=spd) { if (extra>0) { --extra; t=spd-1; } else t=0; }
    }
  }
private:
  static bool allowed_(const Player *p,const void *cookie) {
    const Runtime *r=static_cast<const Runtime*>(cookie);
    return r && r->rendering_ && r->player_==p && r->binding_ && r->binding_->matches(p->module,r->generation_);
  }
  Status check_(uint64_t gen,uint64_t seq) const {
    if (!binding_ || !binding_->matches(player_->module,gen)) return Status::Stale;
    return seq && seq>sequence_ ? Status::Ok : Status::InvalidSequence;
  }
  void clear_voices_() {
    for (int c=0;c<kMaxChannels;++c) {
      player_->channels[c]=Channel();
      fx::svf_reset(&mixer_->voice_filter[c]); fx::svf_reset(&mixer_->voice_filter_r[c]);
      mixer_->voice_pos[c]=0; mixer_->voice_age[c]=0;
    }
    player_->break_row=player_->jump_order=-1; player_->extra_ticks=0; player_->loop_row=player_->loop_count=0;
    mixer_->macro_pending=0; mixer_->fxpl_synced=false;
  }
  void entry_(int index) {
    const uint64_t ticks=player_->ticks_elapsed;
    player_start(player_,binding_->module_);
    player_->runtime_allowed=allowed_; player_->runtime_cookie=this;
    player_reset_pan(player_); player_->playing=true; player_->loop=true;
    player_->speed=binding_->module_->speed; player_->bpm=binding_->module_->bpm;
    player_->ticks_elapsed=ticks;
    // Same-kind slot buffers survive config_read (slot_hold). Carry never calls mixer_reset.
    mix::mixer_config_read(mixer_,binding_->module_);
    clear_voices_();
    if (index<0 || !binding_->section_[index].tail) mix::mixer_reset(mixer_);
    if (index>=0) {
      const Section &s=binding_->section_[index];
      player_->order=int(s.first/uint32_t(player_->module->rows)); player_->row=int(s.first%uint32_t(player_->module->rows));
      player_->speed=s.speed; player_->bpm=s.bpm;
    }
  }
  void boundary_(uint32_t sample_offset) {
    if (section_>=0) {
      const Section &s=binding_->section_[section_];
      // Own the span clock: the player's arrangement may already have wrapped at terminal end.
      if (!first_) ++row_offset_;
      const bool end=row_offset_>=s.end-s.first;
      const uint32_t grid=s.quantization==0 ? 1u : uint32_t(s.quantization==1 ? player_->module->rows_per_beat : player_->module->rows_per_bar);
      if (pending_>=0 && clock_>request_clock_ && (end || (grid && row_offset_%grid==0))) {
        section_=pending_; pending_=-1; row_offset_=0; ++traversal_; entry_(section_);
      } else if (end) {
        if (!s.action) {
          const Status status=stop(s.tail!=0); // policy was checked by compile
          assert(status==Status::Ok); (void)status; return;
        }
        row_offset_=0; ++traversal_;
        player_->order=int(s.first/uint32_t(player_->module->rows)); player_->row=int(s.first%uint32_t(player_->module->rows));
        // Keep tick residual, monotonic tick count, voices, commands and all tails.
      }
      for (int i=0;i<binding_->cues_;++i) {
        const Cue &c=binding_->cue_[i];
        if (c.section!=binding_->section_[section_].id || c.row!=row_offset_) continue;
        CueEvent e; e.generation=generation_; e.sequence=++cue_sequence_; e.clock=clock_; e.traversal=traversal_;
        e.id=c.id; e.section=c.section; e.sample_offset=sample_offset; e.type=c.type; e.strength=c.strength;
        if (head_-tail_<256) events_[head_++%256]=e;
        else if (drops_!=std::numeric_limits<uint32_t>::max()) ++drops_;
      }
    }
    audible_order_=uint32_t(player_->order); audible_row_=uint32_t(player_->row);
    first_=false;
    for (int i=0;i<binding_->controls_;++i)
      mix::mixer_set_macro(mixer_,binding_->control_[i].macro,int(imus_macro_byte(held_[i])));
  }
  const RuntimeBinding *binding_=nullptr; Player *player_=nullptr; mix::Mixer *mixer_=nullptr;
  uint64_t generation_=0,sequence_=0,clock_=0,traversal_=0,request_clock_=0,cue_sequence_=0,tail_left_=0;
  double rate_=48000;
  int section_=-1,pending_=-1;
  uint32_t row_offset_=0,drops_=0,audible_order_=0,audible_row_=0;
  bool active_=false,paused_=false,draining_=false,rendering_=false,first_=true;
  uint16_t held_[kMaxImusControls]={}; Ramp ramp_[kMaxImusLayers];
  bool layer_pending_[kMaxImusLayers]={}; uint16_t layer_value_[kMaxImusLayers]={},layer_ms_[kMaxImusLayers]={};
  float gains_[kMaxBlock][kMaxChannels];
  CueEvent events_[256]; uint32_t head_=0,tail_=0;
};
} }
#endif
