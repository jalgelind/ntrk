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

// Interactive scores: the WASM bridge replays the native script and must equal the
// standalone runtime's audio and cues, written by the native bridge test.
{
  const read=name=>fs.readFileSync(path.join(fixtures,name));
  const score=read('interactive.ntrk'),demoBytes=Buffer.from(engine.command('save'));
  let t=performance.now();
  const loaded=engine.command('load',[score]);const loadMs=performance.now()-t;
  assert.equal(loaded.state[14],1);assert.deepEqual(Buffer.from(engine.command('save')),score);
  assert.deepEqual(loaded.score.sections.map(s=>[s.id,s.name,s.loops]),[[3,'test',true],[4,'test',true]]);
  assert.equal(loaded.score.controls[0].id,1);assert.equal(loaded.score.layers[0].mask,3);
  assert.deepEqual(loaded.score.cues.map(c=>c.id),[9,7,8,10],'score records in file order');
  assert.equal(engine.command('feedback').feedback.bound,false,'sections require an explicit initial choice');
  assert.throws(()=>engine.command('transport',[1,0,0]),/initial section/);
  assert.throws(()=>engine.command('select',[99]),/No such section/);
  assert.throws(()=>engine.command('edit',[0,0,0,0,25]),/read-only/);
  assert.throws(()=>engine.command('transport',[3,0,0]),/section requests/);
  t=performance.now();const selected=engine.command('select',[3]);const selectMs=performance.now()-t;
  const generation=selected.feedback.generation;assert(selected.feedback.bound&&generation>0&&selected.feedback.initial===3);
  const steps=read('interactive-script.txt').toString().trim().split('\n').map(line=>line.split(' '));
  const raw=read('interactive.f32'),expected=new Float32Array(raw.buffer,raw.byteOffset,raw.length/4);
  const cueRaw=read('interactive-cues.bin'),expectedCues=new BigUint64Array(cueRaw.buffer.slice(cueRaw.byteOffset,cueRaw.byteOffset+cueRaw.length));
  const cues=[];let sequence=0;
  const send=(kind,id,value=0,fadeMs=0)=>assert.deepEqual(engine.command('score',[[{kind,generation,sequence:++sequence,id,value,fadeMs}]]).results,['ok']);
  for(let q=0;q<expected.length/256;q++) {
    for(const [at,op,a,b,c] of steps) {
      if(Number(at)!==q)continue;
      if(op==='play'||op==='pause'||op==='stop')engine.command('transport',[{play:1,pause:2,stop:0}[op],0,0]);
      else send(op,Number(a),Number(b??0),Number(c??0));
    }
    const l=new Float32Array(128),r=new Float32Array(128);engine.render(l,r);
    for(let i=0;i<128;i++){assert.equal(l[i],expected[q*256+i*2]);assert.equal(r[i],expected[q*256+i*2+1]);}
    for(const c of engine.command('feedback').feedback.cues)cues.push(c.clock,c.id,c.section,c.traversal,c.sequence,c.offset,c.type,c.strength);
  }
  assert.deepEqual(cues.map(BigInt),Array.from(expectedCues));assert(cues.length>=48);
  // Coalesced batch bound, sequencing, normalization and stale reload.
  assert.throws(()=>engine.command('score',[Array(50).fill({kind:'section',generation,sequence:1,id:3})]),/out of bounds/);
  const one=(gen,seq,kind='control',value=.5,id=1)=>engine.command('score',[[{kind,generation:gen,sequence:seq,id,value}]]).results[0];
  assert.equal(one(generation,++sequence),'ok');assert.equal(one(generation,sequence),'invalid-sequence');
  assert.equal(one(generation,++sequence,'control',NaN),'invalid');assert.equal(one(generation,sequence,'layer',1.5),'invalid');
  assert.equal(one(generation,sequence,'bogus'),'invalid');
  engine.command('load',[score]);const fresh=engine.command('select',[3]).feedback.generation;
  assert(fresh>generation);assert.equal(one(generation,++sequence),'stale');assert.equal(one(fresh,++sequence),'ok');
  assert.equal(engine.command('feedback').feedback.stale,1);
  // Unsupported required profile: distinct refusal, current score untouched.
  assert.throws(()=>engine.command('load',[read('interactive-future.ntrk')]),/newer profile/);
  assert.deepEqual(Buffer.from(engine.command('save')),score);assert(engine.command('feedback').feedback.bound);
  // Ordinary adoption retires the runtime; its commands are then stale.
  engine.command('load',[demoBytes]);assert.equal(engine.state()[14],0);assert.equal(engine.command('snapshot').score,null);
  assert.equal(one(fresh,++sequence),'stale');assert.equal(engine.command('feedback').feedback.bound,false);
  console.log(`WASM interactive: standalone-runtime audio (${expected.length/256} quanta) and ${cues.length/8} cues exact; explicit section, read-only/seek refusals, batch bound, sequence/NaN/range, stale reload, future profile, ordinary retirement passed (handler: load ${loadMs.toFixed(2)} ms, select ${selectMs.toFixed(2)} ms)`);
}

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
