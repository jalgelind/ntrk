import assert from 'node:assert/strict';
import fs from 'node:fs';
import {Engine} from './engine.js';
import {MetadataEngine,MetadataClient} from './metadata.js';
const root=process.argv[2]||'out/web',fixtures=process.argv[3]||'out/web-fixtures';
const module=await WebAssembly.compile(fs.readFileSync(`${root}/ntrk.wasm`));
const audio=new Engine(module,44100),mirror=new MetadataEngine(module,44100);
assert.equal(mirror.engine.rate,44100);
assert.equal(audio.api.memory.buffer.byteLength,128*1024*1024);
assert.equal(mirror.engine.api.memory.buffer.byteLength,128*1024*1024);
let epoch=0,revision=0;
const query=(type,args=[])=>mirror.command({type,args,epoch,revision});
for(const name of ['large','commands','pattern-transition','interactive']) {
  const bytes=fs.readFileSync(`${fixtures}/${name}.ntrk`);
  audio.command('load',[bytes]);++epoch;revision=0;query('load',[bytes]);
  const s=audio.state();
  for(let row=0;row<s[1];row+=8)assert.deepEqual(query('pattern',[0,row,8]),audio.pattern(0,row,8));
  for(let c=0;c<s[0];c++)for(let lane=-1;lane<s[12];lane++)
    assert.deepEqual(query('formatted',[0,0,c,lane]),audio.formatted(0,0,c,lane));
  for(let lane=0;lane<s[13];lane++)assert.deepEqual(query('formatted',[0,0,-1,lane]),audio.formatted(0,0,-1,lane));
  assert.throws(()=>query('transport',[1]),/refused/);
  assert.throws(()=>query('render'),/refused/);
  assert.throws(()=>query('select',[3]),/refused/);
  assert.throws(()=>query('score',[[]]),/refused/);
  if(!s[14]) {
    const edit=[0,0,0,3,123];audio.command('edit',edit);++revision;query('edit',edit);
    assert.deepEqual(query('pattern',[0,0,8]),audio.pattern(0,0,8));
    assert.throws(()=>query('edit',edit),/Stale/);
    assert.throws(()=>mirror.command({type:'pattern',args:[0,0,8],epoch,revision:revision-1}),/Stale/);
  }
  assert.throws(()=>mirror.command({type:'demo',args:[],epoch,revision:0}),/Stale/);
  assert.throws(()=>mirror.command({type:'pattern',args:[0,0,8],epoch:epoch-1,revision}),/Stale/);
  assert.throws(()=>audio.command('load',[new Uint8Array(20)]));
  assert.deepEqual(query('pattern',[0,0,8]),audio.pattern(0,0,8),'failed audio load preserves mirror');
}
++epoch;revision=0;audio.command('demo');query('demo');
assert.equal(query('pattern',[0,0,8]).rows[0].length,16);
assert.deepEqual(query('formatted',[0,0,0,-1]),audio.formatted(0,0,0,-1));
console.log('Metadata C++ equality, acknowledged edits, stale epochs/revisions, no-FX, actual 44.1 kHz; WASM reservations: audio 128 MiB + mirror 128 MiB');

class FakeWorker {
  sent=[];terminated=false;
  postMessage(m){this.sent.push(m);}
  terminate(){this.terminated=true;}
  reply(m,result=null){this.onmessage({data:{id:m.id,epoch:m.epoch,revision:m.revision,result}});}
}
const version={epoch:1,revision:0};
{
  const w=new FakeWorker(),c=new MetadataClient(w,()=>{});
  w.onmessage({data:{}});
  const first=c.request('pattern',[0,0,8],version);
  const second=c.request('pattern',[1,0,8],version);
  const third=c.request('pattern',[2,0,8],version);
  assert.equal(await first,null);assert.equal(await second,null);assert.equal(w.sent.length,1);
  w.reply(w.sent[0]);assert.equal(w.sent.length,2);assert.equal(w.sent[1].args[0],2);
  w.reply(w.sent[0],{old:true}); // duplicate/old reply cannot settle the new request
  w.reply(w.sent[1],{fresh:true});assert.deepEqual(await third,{fresh:true});
  w.onmessage({data:{}});w.onmessage({data:{id:999}});
  const desc=c.request('formatted',[0,0,0,-1],version);c.cancelQueries();assert.equal(await desc,null);
  const mutation=c.request('edit',[0,0,0,3,123],{epoch:1,revision:1});
  w.reply(w.sent.at(-1));await mutation;
  w.reply(w.sent[2]);c.close();
}
for(const failure of ['error','messageerror','close','timeout','version','reply']) {
  const w=new FakeWorker();let called=0;
  const c=new MetadataClient(w,()=>++called,10);
  const promises=[c.request('pattern',[0,0,8],version),c.request('formatted',[0,0,0,-1],version),c.request('load',[],{epoch:2,revision:0})];
  const all=Promise.allSettled(promises);
  if(failure==='error')w.onerror({message:'crash'});
  if(failure==='messageerror')w.onmessageerror();
  if(failure==='close')w.onmessage({data:{type:'closed'}});
  if(failure==='reply')w.onmessage({data:{...w.sent[0],error:'formatter failed'}});
  if(failure==='version')w.onmessage({data:{id:w.sent[0].id,epoch:99,revision:0}});
  assert((await all).every(p=>p.status==='rejected'),failure);assert(w.terminated);assert.equal(called,1);assert.equal(c.slots.size,0);
  await assert.rejects(c.request('pattern',[],version));
}
// Import the real worklet handler with a counted engine. The negative control
// removes only the deny line and must reach both formatter entry points.
const source=fs.readFileSync(new URL('./worklet.js',import.meta.url),'utf8');
let Type,calls=0;
globalThis.sampleRate=48000;
globalThis.AudioWorkletProcessor=class {constructor(){this.port={postMessage:m=>this.replies.push(m)};this.replies=[];}};
globalThis.registerProcessor=(_,type)=>{Type=type;};
globalThis.TestEngine=class {command(type){if(['pattern','formatted'].includes(type))++calls;return [];}render(){}};
async function probe(text) {
  calls=0;
  await import('data:text/javascript,'+encodeURIComponent(text.replace("import {Engine} from './engine.js';",'const Engine=globalThis.TestEngine;')));
  const worklet=new Type({processorOptions:{module:null}});
  for(const type of ['pattern','formatted'])worklet.port.onmessage({data:{id:1,type,args:[]}});
  return {calls,replies:worklet.replies};
}
const guarded=await probe(source);assert.equal(guarded.calls,0);assert(guarded.replies.every(r=>r.error.includes('metadata')));
const unguarded=await probe(source.replace("if(type==='pattern'||type==='formatted')throw new Error('Formatting belongs to the metadata worker');",''));
assert.equal(unguarded.calls,2,'negative control proves formatter invocation detector');
// Audio epochs/revisions are committed only after successful mutations. Older
// loads and edits must be refused before dispatch, independent of request IDs.
const owner=new Type({processorOptions:{module:null}});
owner.port.onmessage({data:{id:10,type:'demo',args:[],epoch:0,revision:0}});
assert.equal(owner.replies.at(-1).epoch,1);assert.equal(owner.replies.at(-1).revision,0);
owner.port.onmessage({data:{id:11,type:'edit',args:[],epoch:1,revision:0}});
assert.equal(owner.replies.at(-1).revision,1);
for(const type of ['load','demo','edit']){
  owner.port.onmessage({data:{id:12,type,args:[],epoch:0,revision:0}});
  assert.match(owner.replies.at(-1).error,/Stale/);
}
assert.equal(owner.epoch,1);assert.equal(owner.revision,1);
console.log('Metadata bounded queues, cancellation, duplicate replies, all failure lifecycles, worklet deny + negative control passed');
