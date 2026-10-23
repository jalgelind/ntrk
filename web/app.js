const $ = id => document.getElementById(id);
const hex = n => n.toString(16).toUpperCase().padStart(2, '0');
const pitches = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-'];
const noteName = n => n === 97 ? '^^^' : n ? pitches[(n - 1) % 12] + (Math.floor((n - 1) / 12) + 1) : '...';
let ctx, node, seq = 0, snapshot, state, order = 0, row = 0, channel = 0, field = 0;
let dirty = false, nibble = '', busy = false;
const pending = new Map();
function status(text, error = false) { $('status').textContent = text; $('status').classList.toggle('error', error); }
function request(type, args = []) {
  return new Promise((resolve, reject) => {
    const id = ++seq;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error('Audio engine did not respond')); }, 10000);
    pending.set(id, {resolve, reject, timer});
    node.port.postMessage({id, type, args});
  });
}
function changed(value) { dirty = value; $('dirty').textContent = value ? '● UNSAVED' : ''; }
function pattern() { return snapshot.orders[order]; }
function index(r = row, c = channel, f = field) { return ((pattern() * state[1] + r) * state[0] + c) * 4 + f; }
function updateState(s) {
  const previous = state;
  state = s;
  $('position').textContent = `${state[8] ? 'PLAY' : 'IDLE'} · ORDER ${hex(state[6])} / ROW ${hex(state[7])}`;
  $('tempo').textContent = `${state[9]} BPM · ${state[10]} TICKS`;
  if (!snapshot) return;
  if (!busy && $('follow').checked && state[8] && order !== state[6]) { order = state[6]; draw(); }
  else if (!previous || previous[11] !== state[11]) draw();
  document.querySelectorAll('tr.playing').forEach(el => el.classList.remove('playing'));
  if (order === state[6] && state[8]) {
    const el = $(`row-${state[7]}`);
    el?.classList.add('playing');
    if ($('follow').checked && el) {
      const box = $('viewport');
      if (el.offsetTop - box.scrollTop > box.clientHeight - 70 || el.offsetTop < box.scrollTop + 70)
        box.scrollTop = el.offsetTop - box.clientHeight / 2;
    }
  }
}
function select() {
  document.querySelector('.selected')?.classList.remove('selected');
  const el = document.querySelector(`[data-row="${row}"][data-channel="${channel}"][data-field="${field}"]`);
  el?.classList.add('selected');
  el?.scrollIntoView({block: 'nearest', inline: 'nearest'});
  $('selection').textContent = `ROW ${hex(row)} · CH ${hex(channel + 1)} · ${['NOTE', 'INSTRUMENT', 'EFFECT', 'PARAMETER'][field]}`;
}
function draw() {
  $('orders').replaceChildren(...Array.from(snapshot.orders, (p, i) => {
    const b = document.createElement('button'); b.textContent = `${hex(i)} : ${hex(p)}`;
    b.setAttribute('aria-pressed', i === order);
    b.onclick = () => run(async () => {
      if (busy) return;
      $('follow').checked = false; order = i; row = 0; nibble = ''; draw();
      updateState(await request('transport', [3, i, 0]));
    }); return b;
  }));
  const head = document.createElement('thead'), tr = document.createElement('tr');
  const corner = document.createElement('th'); corner.textContent = 'ROW'; tr.append(corner);
  for (let c = 0; c < state[0]; ++c) {
    const th = document.createElement('th'), b = document.createElement('button');
    b.textContent = `CH ${hex(c + 1)}${state[11] & (1 << c) ? ' · MUTE' : ''}`;
    b.setAttribute('aria-pressed', !!(state[11] & (1 << c)));
    b.title = 'Toggle channel mute';
    b.onclick = () => run(async () => updateState(await request('mute', [c])));
    th.append(b); tr.append(th);
  }
  head.append(tr);
  const body = document.createElement('tbody');
  for (let r = 0; r < state[1]; ++r) {
    const tr = document.createElement('tr'); tr.id = `row-${r}`;
    if (r % 4 === 0) tr.classList.add('beat');
    const label = document.createElement('td'); label.className = 'rownum'; label.textContent = hex(r); tr.append(label);
    for (let c = 0; c < state[0]; ++c) {
      const td = document.createElement('td'); td.className = 'channel';
      for (let f = 0; f < 4; ++f) {
        const span = document.createElement('span'), value = snapshot.cells[index(r, c, f)];
        span.textContent = f === 0 ? noteName(value) : hex(value);
        Object.assign(span.dataset, {row: r, channel: c, field: f}); td.append(span);
      }
      tr.append(td);
    }
    body.append(tr);
  }
  $('grid').replaceChildren(head, body); select();
  $('instrument').max = state[4];
}
function adopt(result, name) {
  snapshot = result; state = result.state; order = row = channel = field = 0; nibble = '';
  $('instrument').value = state[4] ? 1 : 0;
  $('filename').textContent = name; changed(false); draw(); updateState(state);
}
async function run(fn) {
  try { await fn(); } catch (error) { status(error.message, true); }
}
async function playPause(pauseOnly = false) {
  if (!node) return;
  // Called directly from a gesture, before any asynchronous continuation.
  const resume = ctx.resume();
  await resume;
  updateState(await request('transport', [pauseOnly || state[8] ? 2 : 1, 0, 0]));
}
$('play').onclick = () => run(() => playPause());
$('pause').onclick = () => run(() => playPause(true));
$('stop').onclick = () => run(async () => updateState(await request('transport', [0, 0, 0])));
$('demo').onclick = () => run(async () => {
  if (busy) return;
  if (dirty && !confirm('Discard unsaved edits and load the demo?')) return;
  busy = true;
  try { adopt(await request('demo'), 'Night circuit'); status('Generated demo · ready to play'); }
  finally { busy = false; }
});
$('open').onchange = () => run(async () => {
  const file = $('open').files[0]; $('open').value = '';
  if (!file || !node || busy) return;
  if (file.size > 16 * 1024 * 1024) throw new Error('File exceeds 16 MiB limit');
  if (dirty && !confirm('Discard unsaved edits and open this file?')) return;
  busy = true;
  try {
    const bytes = await file.arrayBuffer();
    adopt(await request('load', [bytes]), file.name); status('Opened locally · no upload');
  } finally { busy = false; }
});
$('save').onclick = () => run(async () => {
  if (busy) return;
  busy = true;
  try {
    const bytes = await request('save');
    const url = URL.createObjectURL(new Blob([bytes], {type: 'application/octet-stream'}));
    const a = document.createElement('a'); a.href = url;
    a.download = $('filename').textContent.replace(/\.ntrk$/i, '') + '.ntrk'; a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000); changed(false); status('Saved .ntrk download');
  } finally { busy = false; }
});
$('grid').onclick = event => {
  if (busy) return;
  const d = event.target.dataset;
  if (d.row === undefined) return;
  row = +d.row; channel = +d.channel; field = +d.field; nibble = ''; select(); $('grid').focus();
};
async function write(value, advance = true) {
  await request('edit', [pattern(), row, channel, field, value]);
  snapshot.cells[index()] = value; changed(true);
  if (advance) row = (row + 1) % state[1];
  draw(); status('Cell updated');
}
document.addEventListener('keydown', event => {
  if (!snapshot || event.ctrlKey || event.metaKey || event.altKey ||
      /INPUT|BUTTON|SUMMARY/.test(event.target.tagName)) return;
  if (event.code === 'Space') { event.preventDefault(); if (!event.repeat) run(() => playPause()); return; }
  const key = event.key.toLowerCase();
  if (busy) return;
  if (['arrowup', 'arrowdown', 'arrowleft', 'arrowright'].includes(key)) {
    event.preventDefault(); nibble = '';
    if (key === 'arrowup') row = (row + state[1] - 1) % state[1];
    if (key === 'arrowdown') row = (row + 1) % state[1];
    if (key === 'arrowleft' || key === 'arrowright') {
      const pos = (channel * 4 + field + (key === 'arrowleft' ? state[0] * 4 - 1 : 1)) % (state[0] * 4);
      channel = Math.floor(pos / 4); field = pos % 4;
    }
    select(); return;
  }
  if (!$('edit').checked || busy || event.repeat) return;
  let value, advance = true;
  if (key === 'delete' || key === 'backspace') { value = 0; nibble = ''; }
  else if (field === 0) {
    if (key === 'enter') value = 97;
    else {
      const low = 'zsxdcvgbhnjm', high = 'q2w3er5t6y7u';
      const offset = low.includes(key) ? low.indexOf(key) : high.includes(key) ? high.indexOf(key) + 12 : -1;
      if (offset < 0) return;
      const octave = Number($('octave').value);
      value = (octave - 1) * 12 + offset + 1;
      if (!Number.isInteger(value) || value < 1 || value > state[5]) { status('Note exceeds module range', true); return; }
    }
  } else if (/^[0-9a-f]$/.test(key)) {
    nibble += key;
    if (nibble.length < 2) { status(`Hex entry: ${nibble.toUpperCase()}_`); event.preventDefault(); return; }
    value = parseInt(nibble, 16); nibble = '';
  } else return;
  event.preventDefault(); busy = true;
  run(async () => {
    try {
      if (field === 0 && value !== 0 && value !== 97) {
        const instrument = Number($('instrument').value);
        await request('edit', [pattern(), row, channel, 1, instrument]);
        snapshot.cells[index(row, channel, 1)] = instrument;
      }
      await write(value, advance);
    } finally { busy = false; }
  });
});
window.addEventListener('beforeunload', e => { if (dirty) { e.preventDefault(); e.returnValue = ''; } });
run(async () => {
  if (!window.AudioWorkletNode) throw new Error('AudioWorklet unavailable. Use a modern browser on localhost or HTTPS.');
  const response = await fetch('ntrk.wasm');
  if (!response.ok) throw new Error('Cannot load ntrk.wasm');
  const module = await WebAssembly.compile(await response.arrayBuffer());
  ctx = new AudioContext({sampleRate: 48000});
  await ctx.audioWorklet.addModule('worklet.js');
  node = new AudioWorkletNode(ctx, 'ntrk', {numberOfInputs: 0, numberOfOutputs: 1,
    outputChannelCount: [2], processorOptions: {module}});
  node.onprocessorerror = () => status('Audio engine failed; reload the page to restart', true);
  node.port.onmessage = ({data}) => {
    if (data.state) { updateState(data.state); return; }
    const p = pending.get(data.id); if (!p) return;
    clearTimeout(p.timer); pending.delete(data.id);
    if (data.error) p.reject(new Error(data.error)); else p.resolve(data.result);
  };
  node.connect(ctx.destination);
  adopt(await request('snapshot'), 'Night circuit');
  for (const id of ['play', 'pause', 'stop', 'save', 'demo']) $(id).disabled = false;
  $('open').disabled = false; document.querySelector('main').setAttribute('aria-busy', 'false');
  status('Ready · press Play to enable audio');
  let polling = false;
  setInterval(() => {
    if (polling || ctx.state !== 'running') return;
    polling = true;
    run(async () => {
      try { updateState(await request('state')); } finally { polling = false; }
    });
  }, 40);
});
