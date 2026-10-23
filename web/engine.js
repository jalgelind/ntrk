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
  state() { return Array.from({length: 12}, (_, i) => this.api.web_info(i)); }
  snapshot() {
    const state = this.state();
    const instruments = Array.from({length: state[4]}, (_, i) => {
      const ptr = this.api.web_instrument_name(i + 1);
      const bytes = new Uint8Array(this.api.memory.buffer, ptr);
      const end = bytes.indexOf(0);
      return {nameBytes: bytes.subarray(0, end < 0 ? bytes.length : end).slice(),
        volume: this.api.web_instrument_volume(i + 1)};
    });
    const usedMask = this.api.web_used_channels();
    const channels = Array.from({length: state[0]}, (_, c) => c).filter(c => usedMask & (1 << c));
    // An empty song still needs a place to enter its first note.
    if (!channels.length) channels.push(0);
    return {state, instruments, usedMask, channels,
      orders: new Uint8Array(this.api.memory.buffer, this.api.web_orders(), state[3]).slice(),
      cells: new Uint8Array(this.api.memory.buffer, this.api.web_patterns(),
        state[0] * state[1] * state[2] * 4).slice()};
  }
  command(type, args = []) {
    const a = this.api;
    if (type === 'state') return this.state();
    if (type === 'snapshot') return this.snapshot();
    if (type === 'load') {
      const bytes = new Uint8Array(args[0]);
      if (bytes.length > a.web_capacity()) throw new Error('File exceeds 16 MiB limit');
      new Uint8Array(a.memory.buffer, a.web_input(), bytes.length).set(bytes);
      if (!a.web_load(bytes.length)) throw new Error('Invalid or unsupported ntrk file');
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
