// Shared worklet/test adapter. C++ owns the song and all playback state.
export class Engine {
  constructor(module, rate) {
    this.api = new WebAssembly.Instance(module, {
      wasi_snapshot_preview1: {proc_exit: code => { throw new Error(`WASM exit ${code}`); }},
      env: {emscripten_notify_memory_growth() {}},
    }).exports;
    this.api._initialize?.();
    this.rate = rate;
    if (!this.api.web_demo(rate)) throw new Error('Supported sample rates: 8–96 kHz');
    this.audio = new Float64Array(this.api.memory.buffer, this.api.web_audio(), 256);
  }
  state() { return Array.from({length: 14}, (_, i) => this.api.web_info(i)); }
  string(ptr) {
    const bytes = new Uint8Array(this.api.memory.buffer, ptr);
    // AudioWorkletGlobalScope has no TextDecoder. Library labels are UTF-8;
    // decode here so worklet and native Node harness share the same bridge.
    let out='';
    for(let i=0;bytes[i];) {
      const b=bytes[i++];
      if(b<128){out+=String.fromCharCode(b);continue;}
      const count=b<224?1:b<240?2:3;
      let code=b & (count===1?31:count===2?15:7),valid=true;
      for(let k=0;k<count;k++){
        if((bytes[i]&192)!==128){valid=false;break;}
        code=(code<<6)|(bytes[i++]&63);
      }
      out+=valid&&code<=0x10FFFF?String.fromCodePoint(code):'�';
    }
    return out;
  }
  // Only the displayed pattern is formatted. The cache is local and capped;
  // snapshots never contain formatted song-wide objects or descriptions.
  pattern(pattern) {
    const s=this.state();
    if(!Number.isInteger(pattern)||pattern<0||pattern>=s[2])throw new Error('Pattern out of range');
    const rows=[],cache=new Map(),notes=new Uint8Array(this.api.memory.buffer,this.api.web_patterns());
    const fxPtr=this.api.web_fx(),fx=new Uint8Array(this.api.memory.buffer,fxPtr);
    const text=(r,c,lane)=>{
      const row=pattern*s[1]+r;
      const at=lane<0?(row*s[0]+c)*4+2:(row*(s[0]*s[12]+s[13])+(c<0?s[0]*s[12]+lane:c*s[12]+lane))*2;
      const bytes=lane<0?notes:fx;
      const key=`${c}:${lane}:${bytes[at]}:${bytes[at+1]}`;
      if(cache.has(key))return cache.get(key);
      const label=this.string(this.api.web_cell_text(pattern,r,c,lane,0));
      if(cache.size===512)cache.delete(cache.keys().next().value);
      cache.set(key,label);return label;
    };
    for(let r=0;r<s[1];r++){
      const row=[];
      for(let c=0;c<s[0];c++)for(let lane=-1;lane<s[12];lane++)row.push(text(r,c,lane));
      for(let lane=0;lane<s[13];lane++)row.push(text(r,-1,lane));
      rows.push(row);
    }
    return {pattern,rows};
  }

  formatted(p,r,c,lane) {
    const a=this.api;
    return {text:this.string(a.web_cell_text(p,r,c,lane,0)),
      description:this.string(a.web_cell_text(p,r,c,lane,1)),channel:c,lane,readOnly:lane>=0};
  }
  snapshot() {
    const state = this.state();
    const instruments = Array.from({length: state[4]}, (_, i) => {
      const ptr = this.api.web_instrument_name(i + 1);
      const bytes = new Uint8Array(this.api.memory.buffer, ptr);
      const end = bytes.indexOf(0);
      return {nameBytes: bytes.subarray(0, end < 0 ? bytes.length : end).slice(),
        pan: this.api.web_instrument_pan(i + 1), volume: this.api.web_instrument_volume(i + 1)};
    });
    const usedMask = this.api.web_used_channels();
    const channels = Array.from({length: state[0]}, (_, c) => c).filter(c => usedMask & (1 << c));
    // An empty song still needs a place to enter its first note.
    if (!channels.length) channels.push(0);
    return {state, instruments, usedMask, channels,
      fx: this.api.web_fx() ? new Uint8Array(this.api.memory.buffer,this.api.web_fx(),
        state[1]*state[2]*(state[0]*state[12]+state[13])*2).slice() : new Uint8Array(),
      orders: new Uint8Array(this.api.memory.buffer, this.api.web_orders(), state[3]).slice(),
      cells: new Uint8Array(this.api.memory.buffer, this.api.web_patterns(),
        state[0] * state[1] * state[2] * 4).slice()};
  }
  command(type, args = []) {
    const a = this.api;
    if (type === 'state') return this.state();
    if (type === 'pattern') return this.pattern(args[0]);
    if (type === 'formatted') return this.formatted(...args);
    if (type === 'snapshot') return this.snapshot();
    if (type === 'load') {
      const bytes = new Uint8Array(args[0]);
      if (bytes.length > a.web_capacity()) throw new Error('File exceeds 16 MiB limit');
      new Uint8Array(a.memory.buffer, a.web_input(), bytes.length).set(bytes);
      const loaded = a.web_load(bytes.length);
      if (loaded === -2) throw new Error('This module requires interactive music support, which is not available in this player yet.');
      if (!loaded) throw new Error('Invalid, unsupported or over-budget ntrk/MOD/XM file');
      return this.snapshot();
    }
    if (type === 'save') {
      const size = a.web_save();
      if (!size) throw new Error('Cannot serialize this module within the 16 MiB limit');
      return new Uint8Array(a.memory.buffer, a.web_input(), size).slice();
    }
    if (type === 'demo') {
      a.web_demo(this.rate); return this.snapshot();
    }
    if (!['edit', 'transport', 'mute'].includes(type)) throw new Error('Unknown command');
    if (!a[`web_${type}`](...args)) throw new Error('Value or position is out of range');
    return this.state();
  }
  render(left, right) {
    // AudioWorklet currently uses 128 frames; also handle future larger quanta.
    for (let offset = 0; offset < left.length; offset += 128) {
      const frames = Math.min(128, left.length - offset);
      this.api.web_render(frames);
      for (let i = 0; i < frames; ++i) {
        left[offset + i] = this.audio[i * 2];
        right[offset + i] = this.audio[i * 2 + 1];
      }
    }
  }
}
