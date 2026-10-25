// Shared worklet/test adapter. C++ owns the song and all playback state.
export const PATTERN_CHUNK_ROWS = 8;
// Score commands per message: every control (32) and layer (16) plus one section
// request, i.e. the coalesced identities a UI transaction can hold.
export const MAX_SCORE_COMMANDS = 49;
export const STATUS = ['ok', 'invalid', 'unsupported', 'stale', 'missing-section', 'invalid-rate', 'invalid-sequence'];
const KIND = {control: 0, layer: 1, section: 2};
const RECORD = {1: 'controls', 2: 'layers', 3: 'sections', 4: 'cues'};
// Checked before any number reaches WASM, so no conversion can alias a value.
// IDs are u32 (IMUS); generations/sequences are positive safe integers.
const isU32 = x => Number.isInteger(x) && x >= 0 && x <= 0xffffffff;
const isCounter = x => Number.isSafeInteger(x) && x >= 1;
const isUnit = x => typeof x === 'number' && Number.isFinite(x) && x >= 0 && x <= 1;
export const MAX_FADE_MS = 60000;
export function validScoreCommand(c) {
  if (!c || typeof c !== 'object' || KIND[c.kind] === undefined) return false;
  if (!isCounter(c.generation) || !isCounter(c.sequence) || !isU32(c.id) || !c.id) return false;
  if (c.kind === 'section') return true;
  const fade = c.fadeMs ?? 0;
  return isUnit(c.value) && (c.kind === 'control' ? fade === 0 :
    Number.isInteger(fade) && fade >= 0 && fade <= MAX_FADE_MS);
}
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
  state() { return Array.from({length: 15}, (_, i) => this.api.web_info(i)); }
  // Names and fields come from the adopted module's score, decoded off render.
  score() {
    const a = this.api, out = {generation: 0, controls: [], layers: [], sections: [], cues: []};
    for (let i = 0; i < a.web_score_count(); i++) {
      const f = k => a.web_score_info(i, k), kind = RECORD[f(0)];
      const entry = {id: f(1), name: this.string(a.web_score_name(i))};
      if (kind === 'controls') Object.assign(entry, {macro: f(2), value: f(3) / 65535});
      if (kind === 'layers') Object.assign(entry, {mask: f(2), gain: f(3) / 65535, fadeMs: f(4)});
      if (kind === 'sections') Object.assign(entry, {first: f(2), end: f(3), bpm: f(4), speed: f(5), quantization: f(6), loops: !!f(7), carry: !!f(8)});
      if (kind === 'cues') Object.assign(entry, {section: f(2), row: f(3), type: f(4), strength: f(5)});
      out[kind].push(entry);
    }
    return out;
  }
  // One audio-owned Position copy plus every queued cue (at most the runtime's 256).
  feedback() {
    const a = this.api, bound = !!a.web_ia_snapshot(), i = (f, k = 0) => a.web_ia_info(f, k);
    const cues = [];
    while (cues.length < 256 && a.web_ia_pop()) {
      const c = f => a.web_ia_cue(f);
      cues.push({generation: c(0), sequence: c(1), clock: c(2), traversal: c(3), id: c(4), section: c(5), offset: c(6), type: c(7), strength: c(8)});
    }
    return {bound, generation: i(0), clock: i(1), traversal: i(2), accepted: i(3), current: i(4), pending: i(5),
      rowOffset: i(6), cueDrops: i(7), order: i(8), row: i(9), playing: !!i(10), paused: !!i(11), draining: !!i(12),
      initial: i(25), started: !!i(26), stale: i(27),
      controls: Array.from({length: i(13)}, (_, k) => ({id: i(15, k), held: i(16, k) / 65535, applied: i(17, k)})),
      layers: Array.from({length: i(14)}, (_, k) => ({id: i(18, k), current: i(19, k), target: i(20, k), remaining: i(21, k),
        held: !!i(22, k), heldValue: i(23, k) / 65535, heldFadeMs: i(24, k)})),
      cues};
  }
  // Applies a bounded, already-coalesced batch in producer order before the next
  // render quantum. Each result is the runtime's verdict; nothing is retried.
  scoreCommands(commands) {
    if (!Array.isArray(commands) || commands.length > MAX_SCORE_COMMANDS) throw new Error('Score command batch out of bounds');
    // A refused command never reaches the runtime: state and accepted sequence stay.
    return commands.map(c => !validScoreCommand(c) ? 'invalid' :
      STATUS[this.api.web_ia_command(KIND[c.kind], c.generation, c.sequence, c.id, c.kind === 'section' ? 0 : c.value, c.fadeMs ?? 0)] ?? 'invalid');
  }
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
  // Only a bounded row window of the displayed pattern is formatted per call so
  // the AudioWorklet message handler never holds the audio thread for a whole
  // 256x149 pattern; callers page through rows. The cache is local and capped;
  // snapshots never contain formatted song-wide objects or descriptions.
  pattern(pattern,start=0,count=PATTERN_CHUNK_ROWS) {
    const s=this.state();
    if(!Number.isInteger(pattern)||pattern<0||pattern>=s[2])throw new Error('Pattern out of range');
    if(!Number.isInteger(start)||!Number.isInteger(count)||start<0||count<1||count>PATTERN_CHUNK_ROWS||start>=s[1])throw new Error('Row window out of range');
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
    for(let r=start;r<Math.min(s[1],start+count);r++){
      const row=[];
      for(let c=0;c<s[0];c++)for(let lane=-1;lane<s[12];lane++)row.push(text(r,c,lane));
      for(let lane=0;lane<s[13];lane++)row.push(text(r,-1,lane));
      rows.push(row);
    }
    return {pattern,start,rows};
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
    return {state, instruments, usedMask, channels, score: state[14] ? this.score() : null,
      fx: this.api.web_fx() ? new Uint8Array(this.api.memory.buffer,this.api.web_fx(),
        state[1]*state[2]*(state[0]*state[12]+state[13])*2).slice() : new Uint8Array(),
      orders: new Uint8Array(this.api.memory.buffer, this.api.web_orders(), state[3]).slice(),
      cells: new Uint8Array(this.api.memory.buffer, this.api.web_patterns(),
        state[0] * state[1] * state[2] * 4).slice()};
  }
  command(type, args = []) {
    const a = this.api;
    if (type === 'state') return this.state();
    if (type === 'pattern') return this.pattern(...args);
    if (type === 'formatted') return this.formatted(...args);
    if (type === 'snapshot') return this.snapshot();
    if (type === 'load') {
      const bytes = new Uint8Array(args[0]);
      if (bytes.length > a.web_capacity()) throw new Error('File exceeds 16 MiB limit');
      new Uint8Array(a.memory.buffer, a.web_input(), bytes.length).set(bytes);
      let loaded = a.web_load(bytes.length);
      // A score never plays as an ordinary song: it either binds to the
      // interactive runtime or is refused with the reason.
      if (loaded === -2) {
        loaded = a.web_load_interactive(bytes.length);
        if (loaded === -3) throw new Error('This interactive score needs a newer profile or capability than this player supports.');
        if (loaded === -5) throw new Error('Interactive scores need a 44.1, 48 or 96 kHz audio device.');
      }
      if (loaded !== 1) throw new Error('Invalid, unsupported or over-budget ntrk/MOD/XM file');
      return this.snapshot();
    }
    if (type === 'select') {
      if (!isU32(args[0])) throw new Error('Section ID must be an integer from 0 to 4294967295');
      const status = a.web_ia_select(args[0]);
      if (status === -1) throw new Error('Stop playback before choosing the initial section');
      if (status) throw new Error(status === 4 ? 'No such section in this score' : `Section refused: ${STATUS[status]}`);
      return {state: this.state(), feedback: this.feedback()};
    }
    if (type === 'score') return {results: this.scoreCommands(args[0]), feedback: this.feedback()};
    if (type === 'feedback') return {state: this.state(), feedback: this.feedback()};
    if (this.state()[14] && type === 'edit') throw new Error('Interactive scores are read-only in this player');
    if (this.state()[14] && type === 'transport') {
      if (args[0] === 3) throw new Error('Interactive scores move by section requests, not seeking');
      if (!a.web_transport(...args)) throw new Error('Choose an initial section first');
      return this.state();
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
