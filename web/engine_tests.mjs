// Stress the real WASM bridge without requiring an audio device or test hooks.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {Engine,PATTERN_CHUNK_ROWS} from './engine.js';
const root=process.argv[2]||'out/web',fixtures=process.argv[3]||'out/web-fixtures';
const module=await WebAssembly.compile(fs.readFileSync(path.join(root,'ntrk.wasm')));
const engine=new Engine(module,48000);
let calls=0;
const string=engine.string;
engine.string=function(ptr){calls++;return string.call(this,ptr);};
const original=fs.readFileSync(path.join(fixtures,'large.ntrk'));
const snapshot=engine.command('load',[original]);
assert(original.length>16000000&&original.length<16*1024*1024);
assert(original.length+256*(16*4+(16*8+5)*2)>16*1024*1024,
  "another pattern at maximal rows/lanes exceeds the host file cap");
assert.equal(calls,0,'load/snapshot formats no cells');
assert.equal(snapshot.commands,undefined);
assert.equal(snapshot.state[1],256);assert.equal(snapshot.state[2],198);
assert.equal(snapshot.state[12],8);assert.equal(snapshot.state[13],5);
assert(snapshot.fx.byteLength>13000000);
const NativeMap=globalThis.Map;let peakCache=0;
globalThis.Map=class extends NativeMap{set(k,v){super.set(k,v);peakCache=Math.max(peakCache,this.size);return this;}};
// The worklet handles one window per message, so the longest uninterrupted
// audio-thread hold is the slowest single window, not the whole pattern.
let commands={pattern:197,rows:[]},worstMs=0,windows=0;
assert.throws(()=>engine.command('pattern',[197,0,PATTERN_CHUNK_ROWS+1]),/window/);
try{
  for(let start=0;start<snapshot.state[1];start+=PATTERN_CHUNK_ROWS){
    const t=performance.now(),part=engine.command('pattern',[197,start,PATTERN_CHUNK_ROWS]);
    worstMs=Math.max(worstMs,performance.now()-t);windows++;
    assert.equal(part.start,start);assert(part.rows.length<=PATTERN_CHUNK_ROWS);
    commands.rows.push(...part.rows);
  }
}finally{globalThis.Map=NativeMap;}
// One 128-frame quantum at 48 kHz is 2.67 ms; a window must stay near that.
assert(worstMs<8,`slowest pattern window ${worstMs.toFixed(2)} ms`);
assert.equal(commands.rows.length,256);
assert(commands.rows.every(row=>row.length===149&&row.every(s=>typeof s==='string')));
assert(calls<=256*149);assert(peakCache<=512,'format cache bounded independently of song size');
const payload=Buffer.byteLength(JSON.stringify(commands));assert(payload<512*1024);
assert.equal(commands.rows[255][15*9],engine.command('formatted',[197,255,15,-1]).text);
assert.deepEqual(Buffer.from(engine.command('save')),original);
calls=0;engine.command('edit',[197,255,15,3,123]);
engine.command('formatted',[197,255,15,-1]);assert.equal(calls,2,'one edit formats one cell with lazy description');
const expected=Buffer.from(original);
const at=32+198+snapshot.state[4]*32+((197*256+255)*16+15)*4+3;expected[at]=123;
const saved=Buffer.from(engine.command('save'));assert.deepEqual(saved,expected);
calls=0;const reloaded=engine.command('load',[saved]);assert.equal(calls,0);
assert.equal(reloaded.cells[((197*256+255)*16+15)*4+3],123);
assert.deepEqual(Buffer.from(engine.command('save')),saved);
engine.command('load',[fs.readFileSync(path.join(fixtures,'pattern-transition.ntrk'))]);
assert.equal(engine.command('pattern',[0,0,1]).rows[0][0],'SOF');
assert.equal(engine.command('pattern',[1,0,1]).rows[0][0],'PAN');
engine.command('demo');assert.equal(engine.state()[12],0);
assert.equal(engine.command('pattern',[0,0,1]).rows[0].length,16,'no FXPL means no fake FX lane');
console.log(`WASM large geometry passed: ${original.length} bytes, ${256*149} formatted cells maximum, cache peak ${peakCache}, ${windows} windows of ${PATTERN_CHUNK_ROWS} rows (slowest ${worstMs.toFixed(2)} ms), payload ${payload} bytes, exact load/edit/save`);

// Native bridge fixtures are independent audio references for every actual channel count.
for(let channels=1;channels<=16;++channels) {
  const name=`count-${String(channels).padStart(2,'0')}`;
  const original=fs.readFileSync(path.join(fixtures,name+'.ntrk'));
  const snapshot=engine.command('load',[original]); assert.equal(snapshot.state[0],channels);
  assert.deepEqual(Buffer.from(engine.command('save')),original);
  engine.command('transport',[1,0,0]);
  const bytes=fs.readFileSync(path.join(fixtures,name+'.f32'));
  const expected=new Float32Array(bytes.buffer,bytes.byteOffset,bytes.length/4);
  let energy=0;
  for(let q=0;q<100;++q) {
    const l=new Float32Array(128),r=new Float32Array(128); engine.render(l,r);
    for(let i=0;i<128;++i) {
      assert.equal(l[i],expected[q*256+i*2]); assert.equal(r[i],expected[q*256+i*2+1]);
      energy+=l[i]*l[i]+r[i]*r[i];
    }
  }
  assert(energy>0); engine.command('transport',[0,0,0]);
  engine.command('edit',[0,1,channels-1,3,27]);
  const expectedSave=Buffer.from(original); expectedSave[32+snapshot.orders.length+snapshot.state[4]*32+(channels+channels-1)*4+3]=27;
  const saved=Buffer.from(engine.command('save')); assert.deepEqual(saved,expectedSave);
  engine.command('load',[saved]); assert.deepEqual(Buffer.from(engine.command('save')),saved);
}
console.log('WASM counts 1..16: exact native audio, META/opaque bytes and inactive masks retained through load/edit/save');
