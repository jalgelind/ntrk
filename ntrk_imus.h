// IMUS borrowed wire views and validation. No allocation, I/O, or runtime evaluator.
// Public domain / CC0. See FORMAT.md for timing and host obligations.
#ifndef NTRK_IMUS_H_
#define NTRK_IMUS_H_
#include "ntrk_types.h"
#include <stddef.h>
#include <stdint.h>
namespace ntrk {
const uint16_t kBlockImus = 0x0012;
const size_t kMaxImusBytes = 65536;
const int kMaxImusRecords = 512;
const int kMaxImusControls = 32, kMaxImusLayers = 16;
const int kMaxImusSections = 64, kMaxImusCues = 256;
const uint16_t kMaxImusFadeMs = 60000;
enum class ImusStatus { Invalid, Supported, Opaque };
enum class ModuleLoadMode { Ordinary, EditorPreserve, InteractiveV1 };
enum class PlaybackMode { Ordinary, LinearPreview };
enum class ModuleLoadError { None, Invalid, InteractiveRequired, UnsupportedProfile };
enum ImusKind { kImusControl = 1, kImusLayer = 2, kImusSection = 3, kImusCue = 4 };
inline uint16_t imus_u16(const uint8_t *p) { return uint16_t(p[0] | uint16_t(p[1]) << 8); }
inline uint32_t imus_u32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void imus_w16(uint8_t *p, uint16_t x) { p[0]=uint8_t(x); p[1]=uint8_t(x>>8); }
inline void imus_w32(uint8_t *p, uint32_t x) {
  for (int i=0;i<4;++i) p[i]=uint8_t(x>>(i*8));
}
struct ImusRecord {
  bool understood_container = true; // iterators disable typed access for future containers
  uint8_t kind = 0, flags = 0;
  uint16_t version = 1;
  uint32_t id = 0;
  const uint8_t *body = nullptr;
  uint32_t bytes = 0;
};
struct ImusView {
  const uint8_t *data = nullptr;
  size_t bytes = 0;
  uint16_t version = 0, profile = 0;
  uint32_t count = 0, next_id = 0;
};
// Cursor begins at byte 12; false means end or malformed framing.
inline bool imus_next(const ImusView &v, size_t *at, ImusRecord *r) {
  if (!at || !r || !v.data || *at > v.bytes || v.bytes-*at < 12) return false;
  const uint8_t *p=v.data+*at;
  const uint32_t n=imus_u32(p+4);
  if (n > v.bytes-*at-12) return false;
  r->understood_container=v.version==1 && v.profile==1;
  r->kind=p[0]; r->flags=p[1]; r->version=imus_u16(p+2);
  r->bytes=n; r->id=imus_u32(p+8); r->body=p+12;
  *at+=12+n; return true;
}
inline bool imus_utf8(const uint8_t *p, size_t n) {
  if (!p || !n || n>63) return false;
  for (size_t i=0;i<n;) {
    uint32_t x=p[i++]; int more=0; uint32_t min=0;
    if (x<128) { if (x<32 || x==127) return false; continue; }
    if (x>=0xc2 && x<=0xdf) { more=1; min=0x80; x&=31; }
    else if (x>=0xe0 && x<=0xef) { more=2; min=0x800; x&=15; }
    else if (x>=0xf0 && x<=0xf4) { more=3; min=0x10000; x&=7; }
    else return false;
    if (size_t(more)>n-i) return false;
    while (more--) { if ((p[i]&0xc0)!=0x80) return false; x=(x<<6)|(p[i++]&63); }
    if (x<min || x>0x10ffff || (x>=0xd800 && x<=0xdfff)) return false;
  }
  return true;
}
struct ImusNamedView {
  const uint8_t *name = nullptr;
  uint8_t name_bytes = 0;
  const uint8_t *fields = nullptr;
  const uint8_t *extra = nullptr;
  size_t extra_bytes = 0;
};
inline bool imus_named(const ImusRecord &r, int kind, size_t fixed, ImusNamedView *v) {
  if (!v || !r.understood_container || r.kind!=kind || r.version!=1 || !r.body || !r.bytes) return false;
  const size_t n=r.body[0];
  if (n>r.bytes-1 || fixed>r.bytes-1-n || !imus_utf8(r.body+1,n)) return false;
  v->name=r.body+1; v->name_bytes=uint8_t(n); v->fields=r.body+1+n;
  v->extra=v->fields+fixed; v->extra_bytes=r.bytes-1-n-fixed; return true;
}
struct ImusControlView : ImusNamedView {
  uint8_t macro_index = 0; uint16_t default_value = 0, smoothing_ms = 0;
};
struct ImusLayerView : ImusNamedView {
  uint16_t channel_mask = 0, default_gain = 65535, fade_ms = 0;
};
struct ImusSectionView : ImusNamedView {
  uint16_t start_order=0, start_row=0, end_order=0, end_row=0, entry_bpm=125, entry_speed=6;
  uint8_t quantization=0, end_action=0, tail_policy=0;
};
struct ImusCueView : ImusNamedView {
  uint32_t section_id=0, row_offset=0; uint16_t type=1, strength=0;
};
inline bool imus_control(const ImusRecord &r, ImusControlView *v) {
  if (!imus_named(r,kImusControl,5,v)) return false;
  v->macro_index=v->fields[0]; v->default_value=imus_u16(v->fields+1);
  v->smoothing_ms=imus_u16(v->fields+3); return true;
}
inline bool imus_layer(const ImusRecord &r, ImusLayerView *v) {
  if (!imus_named(r,kImusLayer,6,v)) return false;
  v->channel_mask=imus_u16(v->fields); v->default_gain=imus_u16(v->fields+2);
  v->fade_ms=imus_u16(v->fields+4); return true;
}
inline bool imus_section(const ImusRecord &r, ImusSectionView *v) {
  if (!imus_named(r,kImusSection,15,v)) return false;
  const uint8_t *p=v->fields;
  v->start_order=imus_u16(p); v->start_row=imus_u16(p+2);
  v->end_order=imus_u16(p+4); v->end_row=imus_u16(p+6);
  v->entry_bpm=imus_u16(p+8); v->entry_speed=imus_u16(p+10);
  v->quantization=p[12]; v->end_action=p[13]; v->tail_policy=p[14]; return true;
}
inline bool imus_cue(const ImusRecord &r, ImusCueView *v) {
  if (!imus_named(r,kImusCue,12,v)) return false;
  v->section_id=imus_u32(v->fields); v->row_offset=imus_u32(v->fields+4);
  v->type=imus_u16(v->fields+8); v->strength=imus_u16(v->fields+10); return true;
}
// Unknown optional field TLVs are retained; required ones require a future host.
inline ImusStatus imus_fields(const ImusNamedView &v) {
  if ((v.extra_bytes && !v.extra) || v.extra_bytes>kMaxImusBytes) return ImusStatus::Invalid;
  size_t at=0; bool opaque=false;
  while (at<v.extra_bytes) {
    if (v.extra_bytes-at<8) return ImusStatus::Invalid;
    const uint8_t *p=v.extra+at;
    const uint32_t n=imus_u32(p+4);
    if (!imus_u16(p) || (imus_u16(p+2)&~1u) || n>v.extra_bytes-at-8) return ImusStatus::Invalid;
    opaque=opaque || (imus_u16(p+2)&1); at+=8+n;
  }
  return opaque ? ImusStatus::Opaque : ImusStatus::Supported;
}
inline bool imus_control_valid(const ImusControlView &v) {
  return v.macro_index && v.macro_index<=kMaxMacros && !v.smoothing_ms;
}
inline bool imus_layer_valid(const ImusLayerView &v) {
  return v.channel_mask && v.fade_ms<=kMaxImusFadeMs;
}
inline bool imus_section_valid(const ImusSectionView &v) {
  return v.entry_bpm>=32 && v.entry_bpm<=255 && v.entry_speed && v.entry_speed<=31 &&
      v.quantization<=2 && v.end_action<=1 && v.tail_policy<=1 &&
      (v.start_order<v.end_order || (v.start_order==v.end_order && v.start_row<v.end_row));
}
inline bool imus_cue_valid(const ImusCueView &v) {
  return v.section_id && v.type>=1 && v.type<=2 && (v.type!=1 || !v.strength);
}
// Typed body writers preserve trailing field TLVs. Size query and short-buffer
// refusal have the same contract as the container writer; source/output must not overlap.
inline bool imus_body_write(const ImusNamedView &v, const uint8_t *fixed, size_t n,
                            uint8_t *out, size_t cap, size_t *written) {
  if (!written) return false;
  *written=0;
  if (!imus_utf8(v.name,v.name_bytes) || (n && !fixed) ||
      n>kMaxImusBytes-1-v.name_bytes || (v.extra_bytes && !v.extra) ||
      v.extra_bytes>kMaxImusBytes-1-v.name_bytes-n || imus_fields(v)==ImusStatus::Invalid) return false;
  *written=1+v.name_bytes+n+v.extra_bytes;
  if (!out) return true;
  if (cap<*written) return false;
  size_t at=0; out[at++]=v.name_bytes;
  for (size_t i=0;i<v.name_bytes;++i) out[at++]=v.name[i];
  for (size_t i=0;i<n;++i) out[at++]=fixed[i];
  for (size_t i=0;i<v.extra_bytes;++i) out[at++]=v.extra[i];
  return true;
}
inline bool imus_body_encode(const ImusControlView &v, uint8_t *out, size_t cap, size_t *written) {
  if (written) *written=0;
  if (!imus_control_valid(v)) return false;
  uint8_t f[5]; f[0]=v.macro_index; imus_w16(f+1,v.default_value); imus_w16(f+3,v.smoothing_ms);
  return imus_body_write(v,f,5,out,cap,written);
}
inline bool imus_body_encode(const ImusLayerView &v, uint8_t *out, size_t cap, size_t *written) {
  if (written) *written=0;
  if (!imus_layer_valid(v)) return false;
  uint8_t f[6]; imus_w16(f,v.channel_mask); imus_w16(f+2,v.default_gain); imus_w16(f+4,v.fade_ms);
  return imus_body_write(v,f,6,out,cap,written);
}
inline bool imus_body_encode(const ImusSectionView &v, uint8_t *out, size_t cap, size_t *written) {
  if (written) *written=0;
  if (!imus_section_valid(v)) return false;
  uint8_t f[15]; imus_w16(f,v.start_order); imus_w16(f+2,v.start_row);
  imus_w16(f+4,v.end_order); imus_w16(f+6,v.end_row); imus_w16(f+8,v.entry_bpm);
  imus_w16(f+10,v.entry_speed); f[12]=v.quantization; f[13]=v.end_action; f[14]=v.tail_policy;
  return imus_body_write(v,f,15,out,cap,written);
}
inline bool imus_body_encode(const ImusCueView &v, uint8_t *out, size_t cap, size_t *written) {
  if (written) *written=0;
  if (!imus_cue_valid(v)) return false;
  uint8_t f[12]; imus_w32(f,v.section_id); imus_w32(f+4,v.row_offset);
  imus_w16(f+8,v.type); imus_w16(f+10,v.strength);
  return imus_body_write(v,f,12,out,cap,written);
}

inline uint8_t imus_macro_byte(uint16_t v) { return uint8_t((uint32_t(v)*255+32767)/65535); }
// Uniform borrowed source lets encoder and decoder use exactly the same validator.
struct ImusRecords {
  ImusView wire;
  const ImusRecord *records = nullptr;
  bool next(size_t *at, uint32_t i, ImusRecord *r) const {
    if (!records) return imus_next(wire,at,r);
    *r=records[i]; return true;
  }
};
inline bool imus_span(const Module &m, const ImusSectionView &s, uint32_t *first, uint32_t *end) {
  if (s.start_order>=m.order_count || s.start_row>=m.rows || s.end_order>m.order_count ||
      (s.end_order==m.order_count ? s.end_row!=0 : s.end_row>=m.rows)) return false;
  *first=uint32_t(s.start_order)*uint32_t(m.rows)+s.start_row;
  *end=uint32_t(s.end_order)*uint32_t(m.rows)+s.end_row;
  return *first<*end;
}
inline bool imus_score_command(uint8_t effect, uint8_t param) {
  return effect!=0x0b && effect!=0x0d && effect!=0x0f &&
      !(effect==0x0e && ((param>>4)==6 || (param>>4)==14));
}
// Only the current articulation commands inhabit the player's FXPL namespace.
// Reject future player commands in spans; mixer commands are a different namespace.
inline bool imus_plane_command(uint8_t cmd) { return cmd<0x30 || cmd==0x30 || cmd==0x31 || cmd>=0x40; }
inline bool imus_scopes_overlap(const MacroTarget &a, const MacroTarget &b) {
  if (a.target!=b.target) return false;
  if (macro_target_is_global(a.target)) return true;
  return a.scope==b.scope || a.scope==kMacroScopeAll || b.scope==kMacroScopeAll;
}
inline ImusStatus imus_validate(const ImusRecords &src, const Module *m=nullptr) {
  const ImusView &v=src.wire;
  if (!v.version || !v.profile || !v.next_id || v.count>kMaxImusRecords ||
      v.bytes<12 || v.bytes>kMaxImusBytes) return ImusStatus::Invalid;
  if (m && (m->channels<1 || m->channels>kMaxChannels || m->rows<1 || m->rows>kMaxRows ||
      m->order_count<1 || m->order_count>kMaxOrders || m->pattern_count<1 || m->pattern_count>kMaxPatterns ||
      m->macro_count<0 || m->macro_count>kMaxMacros || m->fx_columns<1 || m->fx_columns>kMaxFxColumns ||
      m->meta_columns<0 || m->meta_columns>kMaxMetaColumns)) return ImusStatus::Invalid;
  bool opaque=v.version!=1 || v.profile!=1;
  uint32_t ids[kMaxImusRecords]; uint8_t kinds[kMaxImusRecords];
  int counts[5]={}; uint16_t layer_mask=0; uint8_t macros[kMaxImusControls]; int macro_count=0;
  size_t at=12;
  for (uint32_t i=0;i<v.count;++i) {
    ImusRecord r;
    if (!src.next(&at,i,&r) || !r.id || r.id>=v.next_id || !r.version ||
        (r.flags&~1u) || (r.bytes && !r.body)) return ImusStatus::Invalid;
    for (uint32_t j=0;j<i;++j) if (ids[j]==r.id) return ImusStatus::Invalid;
    ids[i]=r.id; kinds[i]=r.kind;
    if (r.kind>=1 && r.kind<=4) ++counts[r.kind];
    if (counts[1]>kMaxImusControls || counts[2]>kMaxImusLayers ||
        counts[3]>kMaxImusSections || counts[4]>kMaxImusCues) return ImusStatus::Invalid;
    if (v.version!=1 || v.profile!=1) continue; // never reinterpret a future container
    if (r.version!=1 || r.kind<1 || r.kind>4) { opaque=opaque || (r.flags&1); continue; }
    ImusNamedView named;
    if (r.kind==kImusControl) {
      ImusControlView c;
      if (!imus_control(r,&c) || !imus_control_valid(c))
        return ImusStatus::Invalid;
      named=c;
      for (int j=0;j<macro_count;++j) if (macros[j]==c.macro_index) return ImusStatus::Invalid;
      if (m) {
        if (!m->fx || c.macro_index>m->macro_count) return ImusStatus::Invalid;
        const Macro &mac=m->macros[c.macro_index-1];
        if (!mac.target_count || mac.target_count>kMaxMacroTargets || mac.flags) return ImusStatus::Invalid;
        for (int j=0;j<mac.target_count;++j) {
          const MacroTarget &t=mac.targets[j];
          if (!macro_target_valid(t.target) || t.scope==kMacroScopeSelf ||
              (t.scope!=kMacroScopeAll && t.scope>=m->channels)) return ImusStatus::Invalid;
          for (int k=0;k<j;++k) if (imus_scopes_overlap(t,mac.targets[k])) return ImusStatus::Invalid;
          for (int k=0;k<macro_count;++k) {
            const Macro &other=m->macros[macros[k]-1];
            for (int z=0;z<other.target_count;++z)
              if (imus_scopes_overlap(t,other.targets[z])) return ImusStatus::Invalid;
          }
        }
      }
      macros[macro_count++]=c.macro_index;
    } else if (r.kind==kImusLayer) {
      ImusLayerView l;
      if (!imus_layer(r,&l) || !imus_layer_valid(l) || (l.channel_mask&layer_mask) || (m && (uint32_t(l.channel_mask)>>m->channels))) return ImusStatus::Invalid;
      layer_mask|=l.channel_mask; named=l;
    } else if (r.kind==kImusSection) {
      ImusSectionView s; uint32_t first=0,end=0;
      if (!imus_section(r,&s) || !imus_section_valid(s))
        return ImusStatus::Invalid;
      named=s;
      if (m) {
        if (!imus_span(*m,s,&first,&end) || m->swing || !m->order || !m->patterns)
          return ImusStatus::Invalid;
        for (uint32_t pos=first;pos<end;++pos) {
          const int pat=m->order[pos/uint32_t(m->rows)];
          if (pat>=m->pattern_count) return ImusStatus::Invalid;
          const size_t row=size_t(pat)*size_t(m->rows)+pos%uint32_t(m->rows);
          for (int c=0;c<m->channels;++c) {
            const Note &n=m->patterns[row*size_t(m->channels)+size_t(c)];
            if (!imus_score_command(n.effect,n.param)) return ImusStatus::Invalid;
          }
          if (m->fx) {
            const int lanes=m->channels*m->fx_columns+m->meta_columns;
            for (int c=0;c<m->channels*m->fx_columns;++c)
              if (!imus_plane_command(m->fx[row*size_t(lanes)+size_t(c)].cmd)) return ImusStatus::Invalid;
          }
        }
      }
    } else {
      ImusCueView c;
      if (!imus_cue(r,&c) || !imus_cue_valid(c))
        return ImusStatus::Invalid;
      named=c;
    }
    const ImusStatus fields=imus_fields(named);
    if (fields==ImusStatus::Invalid) return fields;
    opaque=opaque || fields==ImusStatus::Opaque;
  }
  if (!src.records && at!=v.bytes) return ImusStatus::Invalid;
  // References can point forward. Future section bodies stay opaque, even when referenced.
  if (v.version==1 && v.profile==1) {
    at=12;
    for (uint32_t i=0;i<v.count;++i) {
      ImusRecord r; src.next(&at,i,&r);
      if (r.kind!=kImusCue || r.version!=1) continue;
      ImusCueView c; if (!imus_cue(r,&c)) return ImusStatus::Invalid;
      uint32_t j=0;
      while (j<v.count && ids[j]!=c.section_id) ++j;
      if (j==v.count || kinds[j]!=kImusSection) return ImusStatus::Invalid;
      size_t cursor=12; ImusRecord s;
      for (uint32_t k=0;k<=j;++k) src.next(&cursor,k,&s);
      if (s.version!=1) opaque=true;
      if (s.version==1 && m) {
        ImusSectionView section; uint32_t first=0,end=0;
        if (!imus_section(s,&section) || !imus_span(*m,section,&first,&end) || c.row_offset>=end-first)
          return ImusStatus::Invalid;
      }
    }
  }
  return opaque ? ImusStatus::Opaque : ImusStatus::Supported;
}
inline ImusStatus imus_decode(const uint8_t *p, size_t n, ImusView *out, const Module *m=nullptr) {
  if (out) *out=ImusView();
  if (!p || n<12 || n>kMaxImusBytes) return ImusStatus::Invalid;
  ImusRecords src;
  src.wire.data=p; src.wire.bytes=n; src.wire.version=imus_u16(p); src.wire.profile=imus_u16(p+2);
  src.wire.count=imus_u32(p+4); src.wire.next_id=imus_u32(p+8);
  const ImusStatus status=imus_validate(src,m);
  if (status!=ImusStatus::Invalid && out) *out=src.wire;
  return status;
}
inline bool imus_encode(uint16_t version, uint16_t profile, uint32_t next_id,
                        const ImusRecord *records, uint32_t count, uint8_t *out,
                        size_t cap, size_t *written, const Module *m=nullptr) {
  if (!written) return false;
  *written=0;
  if (count>kMaxImusRecords || (count && !records)) return false;
  size_t total=12;
  for (uint32_t i=0;i<count;++i) {
    if (records[i].bytes>kMaxImusBytes-total || kMaxImusBytes-total-records[i].bytes<12) return false;
    total+=12+records[i].bytes;
  }
  ImusRecords src;
  src.records=records; src.wire.bytes=total; src.wire.count=count;
  src.wire.version=version; src.wire.profile=profile; src.wire.next_id=next_id;
  // Empty sources need no wire iterator.
  if (imus_validate(src,m)==ImusStatus::Invalid) return false;
  *written=total;
  if (!out) return true;
  if (cap<total) return false;
  imus_w16(out,version); imus_w16(out+2,profile); imus_w32(out+4,count); imus_w32(out+8,next_id);
  size_t at=12;
  for (uint32_t i=0;i<count;++i) {
    const ImusRecord &r=records[i];
    out[at]=r.kind; out[at+1]=r.flags; imus_w16(out+at+2,r.version);
    imus_w32(out+at+4,r.bytes); imus_w32(out+at+8,r.id); at+=12;
    for (size_t k=0;k<r.bytes;++k) out[at++]=r.body[k];
  }
  return true;
}
} // namespace ntrk
#endif
