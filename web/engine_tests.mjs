// Stress the real WASM bridge without requiring an audio device or test hooks.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {Engine,PATTERN_CHUNK_ROWS,ScoreCommands} from './engine.js';
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
// Wall time is reported, not asserted (it flakes under load); one 128-frame
// quantum at 48 kHz is 2.67 ms. The bounds below are deterministic.
assert.equal(windows,256/PATTERN_CHUNK_ROWS);
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

// UI command producer: steps and toggles made while a batch is unacknowledged
// build on the latest intent; acknowledgement, refusal, failure and a new
// generation each release intents. Pure logic, no WASM.
{
  const c=new ScoreCommands(),held=1,layerTarget=1;
  const step=(id,delta)=>c.stage('control',id,Math.max(0,Math.min(1,(c.intended('control',id)??held)+delta)));
  const toggle=id=>c.stage('layer',id,(c.intended('layer',id)??layerTarget)>.5?0:1,40);
  step(1,-.125);assert.equal(c.take(),null,'unbound producer sends nothing');
  c.bind(5);step(1,-.125);toggle(2);
  const first=c.take();assert.deepEqual(first.map(x=>[x.kind,x.id,x.value,x.sequence,x.generation]),[['control',1,.875,1,5],['layer',2,0,2,5]]);
  // Unacknowledged: further actions compound on intent, and stay queued.
  step(1,-.125);toggle(2);assert.equal(c.take(),null,'one batch in flight');
  assert.equal(c.intended('control',1),.75);assert.equal(c.intended('layer',2),1);
  c.settle(first);
  assert.equal(c.intended('control',1),.75,'newer intent survives the older acknowledgement');
  const second=c.take();assert.deepEqual(second.map(x=>[x.id,x.value,x.sequence]),[[1,.75,3],[2,1,4]]);
  c.settle(second);assert.equal(c.intended('control',1),undefined,'acknowledged: feedback is the source again');
  // A refused or failed batch leaves no phantom intent.
  step(1,.125);const failed=c.take();c.settle(failed);assert.equal(c.intended('control',1),undefined);
  assert.equal(c.inflight,null);
  // A new generation drops staged and intended values; the old batch settles harmlessly.
  step(1,-.5);const old=c.take();step(1,-.125);c.bind(6);
  assert.equal(c.intended('control',1),undefined);assert.equal(c.staged.size,0);c.settle(old);
  step(1,-.125);assert.deepEqual(c.take().map(x=>[x.value,x.generation,x.sequence]),[[.875,6,7]]);
  // Section requests coalesce to the latest and carry no intent.
  const s=new ScoreCommands();s.bind(1);s.stage('section',3);s.stage('section',4);
  assert.deepEqual(s.take().map(x=>[x.kind,x.id]),[['section',4]]);assert.equal(s.intended('section',4),undefined);
  console.log('UI score producer: in-flight steps/toggles compound on intent, acknowledgement/refusal/failure/generation release it, one batch in flight passed');
}

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

// Unsigned IDs and boundary validation: refused values never reach WASM, keep
// runtime state and the accepted sequence, and cannot alias another ID.
{
  const read=name=>fs.readFileSync(path.join(fixtures,name));
  const ids=engine.command('load',[read('interactive-ids.ntrk')]).score;
  assert.deepEqual(ids.sections.map(s=>s.id),[0x80000003,0xfffffffe]);
  assert.deepEqual([ids.controls[0].id,ids.layers[0].id],[0x7fffffff,0x80000000]);
  assert(ids.cues.every(c=>c.section===0x80000003||c.section===0xfffffffe));
  for(const bad of [-2147483645,0x100000000,3.5,NaN,'3',null])assert.throws(()=>engine.command('select',[bad]),/Section ID/);
  assert.equal(engine.command('feedback').feedback.bound,false,'refused selections bind nothing');
  assert.throws(()=>engine.command('select',[0xffffffff]),/No such section/);
  const generation=engine.command('select',[0x80000003]).feedback.generation;
  engine.command('transport',[1,0,0]);
  const one=c=>engine.command('score',[[{generation,...c}]]).results[0];
  const accepted=()=>engine.command('feedback').feedback.accepted;
  assert.equal(one({kind:'control',sequence:1,id:0x7fffffff,value:.25}),'ok');
  const refusals=[
    {kind:'control',sequence:2,id:1.5,value:.5},{kind:'control',sequence:2,id:4294967297,value:.5},
    {kind:'control',sequence:2,id:0x7fffffff+0x100000000,value:.5},{kind:'control',sequence:2,id:-1,value:.5},
    {kind:'control',sequence:2,id:0x7fffffff,value:NaN},{kind:'control',sequence:2,id:0x7fffffff,value:1.0000001},
    {kind:'control',sequence:2,id:0x7fffffff,value:'0.5'},{kind:'control',sequence:2,id:0x7fffffff,value:.5,fadeMs:10},
    {kind:'layer',sequence:2,id:0x80000000,value:1,fadeMs:NaN},{kind:'layer',sequence:2,id:0x80000000,value:1,fadeMs:Infinity},
    {kind:'layer',sequence:2,id:0x80000000,value:1,fadeMs:.9},{kind:'layer',sequence:2,id:0x80000000,value:1,fadeMs:60001},
    {kind:'section',sequence:2,id:0x80000003+0.5},{kind:'section',sequence:2,id:0},{kind:'section',sequence:1.5,id:0xfffffffe},
    {kind:'section',sequence:2**53,id:0xfffffffe},{kind:'bogus',sequence:2,id:1},null,7];
  const before=engine.command('feedback').feedback;
  const results=engine.command('score',[refusals]).results;
  assert(results.every(r=>r==='invalid'),JSON.stringify(results));
  const after=engine.command('feedback').feedback;
  assert.equal(after.accepted,1);assert.equal(after.pending,0);
  assert.deepEqual(after.controls.map(c=>c.held),before.controls.map(c=>c.held));
  assert.deepEqual(after.layers.map(l=>[l.target,l.remaining]),before.layers.map(l=>[l.target,l.remaining]));
  // Only the three own kind names map to a command; inherited or coerced names do not.
  const kinds=['toString','constructor','__proto__','hasOwnProperty','valueOf','',Symbol('control'),'0','1','2',0,1,'Control',' control',['control']];
  const kindResults=engine.command('score',[kinds.map(kind=>({kind,generation,sequence:2,id:0x7fffffff,value:.5}))]).results;
  assert(kindResults.every(r=>r==='invalid'),JSON.stringify(kindResults));
  const afterKinds=engine.command('feedback').feedback;
  assert.equal(afterKinds.accepted,1);assert.deepEqual(afterKinds.controls.map(c=>c.held),before.controls.map(c=>c.held));
  // The very next valid sequence is still 2: refusals consumed none.
  assert.equal(one({kind:'layer',sequence:2,id:0x80000000,value:0,fadeMs:0}),'ok');
  assert.equal(one({kind:'section',sequence:3,id:0xfffffffe}),'ok');assert.equal(accepted(),3);
  assert.equal(engine.command('feedback').feedback.pending,0xfffffffe);
  assert.throws(()=>engine.command('score',[Array(50).fill(refusals[0])]),/out of bounds/);
  assert.throws(()=>engine.command('score',[{length:1,0:refusals[0]}]),/out of bounds/);
  const l=new Float32Array(128),r=new Float32Array(128);
  for(let q=0;q<60;q++)engine.render(l,r);
  const fb=engine.command('feedback').feedback;
  assert.equal(fb.current,0xfffffffe);assert.equal(fb.layers[0].current,0);
  assert(fb.cues.some(c=>c.section===0xfffffffe));
  engine.command('transport',[0,0,0]);
  console.log('WASM interactive ids: u32 IDs 0x7fffffff..0xfffffffe round-trip; i32-aliased/fractional/out-of-range IDs, NaN/Infinity/fractional/over-range values and fades refused before WASM with state and sequence preserved passed');
}
// Handler timing for the largest-shaped procedural score. These handlers run in
// the worklet between quanta: the measured time is an audio interruption.
{
  const large=fs.readFileSync(path.join(fixtures,'interactive-large.ntrk'));
  const time=fn=>{const t=performance.now();fn();return performance.now()-t;};
  const runs=[];
  for(let i=0;i<3;i++)runs.push({load:time(()=>engine.command('load',[large])),
    snapshot:time(()=>engine.command('snapshot')),select:time(()=>engine.command('select',[18]))});
  const worst=k=>Math.max(...runs.map(r=>r[k])).toFixed(1),best=k=>Math.min(...runs.map(r=>r[k])).toFixed(1);
  console.log(`WASM interactive large handler (node, ${large.length} bytes, 337 records, 3 runs min/max): load ${best('load')}/${worst('load')} ms, snapshot (score table) ${best('snapshot')}/${worst('snapshot')} ms, select ${best('select')}/${worst('select')} ms`);
  engine.command('demo');
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
