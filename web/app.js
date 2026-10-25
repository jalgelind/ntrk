import {TrackerCanvas,fieldLayout} from './canvas.js';
import {PATTERN_CHUNK_ROWS} from './engine.js';
const canvas=document.getElementById('tracker'),open=document.getElementById('open');
const m={snapshot:null,state:null,order:0,row:0,channel:0,field:0,dirty:false,nibble:'',busy:false,ready:false,volume:1,level:0,
  octave:2,instrument:1,edit:false,follow:true,help:false,confirm:null,name:'Module player',message:'Loading audio engine…',error:false,
  feedback:null,cues:[],rate:48000};
let ctx,node,gain,analysers,seq=0,demo,configReady,queue=Promise.resolve(),queued=0;
const pending=new Map();
// Score commands: one producer, coalesced by identity (latest value wins), at most
// one batch in flight, each tagged with the bound generation and a strictly
// increasing sequence. A batch for a replaced generation is refused as stale.
const staged=new Map();let scoreSeq=0,scoreInflight=false;
let loadGeneration=0,patternSerial=0,descriptionSerial=0,editSerial=0;
const view=new TrackerCanvas(canvas,m,(type,arg)=>{if(type==='open'){open.click();return;}if(type==='transport')ctx?.resume();enqueue(()=>action(type,arg));});
function status(message,error=false) {m.message=message;m.error=error;view.invalidate();}
function request(type,args=[]) {
  return new Promise((resolve,reject)=>{
    const id=++seq;
    const mutation=['load','demo','edit','transport','mute','select','score'].includes(type);
    const timer=setTimeout(()=>{
      // A committed load must be acknowledged, never abandoned behind an old UI.
      if(mutation){status('Waiting for audio engine…');return;}
      pending.delete(id);reject(new Error('Audio engine did not respond'));
    },10000);
    pending.set(id,{resolve,reject,timer});node.port.postMessage({id,type,args});
  });
}
function selectedDescription() {
  const snapshot=m.snapshot,gen=loadGeneration,serial=++descriptionSerial;
  m.description='';
  if(!snapshot?.commands)return;
  const pattern=snapshot.orders[m.order],row=m.row,c=m.channel,f=m.field;
  const lane=c<0?f:f<4?-1:f-4;
  request('formatted',[pattern,row,c,lane]).then(cell=>{
    if(gen!==loadGeneration||serial!==descriptionSerial||snapshot!==m.snapshot)return;
    m.description=cell.description==='None'?'':cell.description;view.invalidate();
  }).catch(error=>{if(gen===loadGeneration&&serial===descriptionSerial)status(error.message,true);});
}
async function refreshPattern() {
  const snapshot=m.snapshot;
  if(!snapshot)return;
  const pattern=snapshot.orders[m.order],gen=loadGeneration,serial=++patternSerial;
  snapshot.commands=null;m.description='';++descriptionSerial;view.invalidate();
  // Rows arrive in small windows so the audio worklet interleaves rendering
  // between them. An edit landing mid-assembly restarts it, so a window fetched
  // before the edit never survives.
  const stale=()=>gen!==loadGeneration||serial!==patternSerial||snapshot!==m.snapshot||snapshot.orders[m.order]!==pattern;
  let commands;
  for(;;) {
    const edits=editSerial;
    commands={pattern,rows:[]};
    for(let start=0;start<m.state[1];start+=PATTERN_CHUNK_ROWS) {
      const part=await request('pattern',[pattern,start,PATTERN_CHUNK_ROWS]);
      if(stale())return;
      commands.rows.push(...part.rows);
    }
    if(edits===editSerial)break;
  }
  snapshot.commands=commands;selectedDescription();view.invalidate();
}
async function replaceSong(type,args,name) {
  const previous={snapshot:m.snapshot,state:m.state};
  ++loadGeneration;++patternSerial;++descriptionSerial;m.snapshot=null;m.state=null;m.description='';view.invalidate();
  let committed=false;
  try {
    const result=await request(type,args);committed=true;
    await adopt(result,name);
  } catch(error) {
    if(!committed&&m.ready){Object.assign(m,previous);if(m.snapshot)await refreshPattern();}
    throw error;
  }
}
function index(r=m.row,c=m.channel,f=m.field) {return ((m.snapshot.orders[m.order]*m.state[1]+r)*m.state[0]+c)*4+f;}
function updateState(s) {
  if(m.state&&s.every((value,i)=>value===m.state[i])&&!(m.snapshot&&!m.busy&&m.follow&&s[8]&&m.order!==s[6]))return;
  m.state=s;
  if(m.snapshot&&!m.busy&&m.follow&&s[8]) {
    const oldPattern=m.snapshot.orders[m.order];
    m.order=s[6];
    if(m.snapshot.orders[m.order]!==oldPattern)refreshPattern().catch(error=>status(error.message,true));
    const horizontal=view.sx;
    view.reveal(s[7],m.channel,m.field);view.sx=horizontal;
    if(view.orders) {
      const top=m.order*view.pitch;
      if(top<view.orderScroll)view.orderScroll=top;
      if(top+view.pitch>view.orderScroll+view.orders.h)view.orderScroll=top+view.pitch-view.orders.h;
    }
  }
  view.invalidate();
}
function applyFeedback(feedback) {
  if(!m.snapshot?.score)return;
  m.feedback=feedback;
  if(feedback.cues.length)m.cues=[...m.cues,...feedback.cues].slice(-6);
  view.invalidate();
}
function stage(kind,id,value=0,fadeMs=0) {
  const key=kind==='section'?kind:`${kind}:${id}`;
  staged.delete(key);staged.set(key,{kind,id,value,fadeMs});flush();
}
function stagedValue(kind,id) {return staged.get(`${kind}:${id}`)?.value;}
async function flush() {
  if(scoreInflight||!staged.size)return;
  const generation=m.feedback?.bound?m.feedback.generation:0;
  if(!generation){staged.clear();status('Choose an initial section first',true);return;}
  const batch=[...staged.values()].map(c=>({...c,generation,sequence:++scoreSeq}));
  staged.clear();scoreInflight=true;
  try {
    const {results,feedback}=await request('score',[batch]);
    if(feedback.generation===generation)applyFeedback(feedback);
    const refused=results.find(r=>r!=='ok');
    if(refused)status(refused==='stale'?'Score command for a replaced song was ignored':`Score command refused: ${refused}`,refused!=='stale');
  } catch(error){status(error.message,true);}
  finally{scoreInflight=false;flush();}
}
async function refreshFeedback() {
  if(!m.snapshot?.score)return;
  const snapshot=m.snapshot,{state,feedback}=await request('feedback');
  if(snapshot!==m.snapshot)return;
  updateState(state);applyFeedback(feedback);
}
async function adopt(result,name) {
  ++loadGeneration;++patternSerial;++descriptionSerial;m.description="";
  staged.clear();m.feedback=null;m.cues=[];
  const decoder=new TextDecoder();
  result.instruments=result.instruments.map(ins=>({name:decoder.decode(ins.nameBytes),pan:ins.pan,volume:ins.volume}));
  // Keep this load's channel list stable through edits, including clearing its
  // final cell. Reopening recomputes usage without changing the stored channels.
  m.snapshot=result;m.state=result.state;m.order=m.row=m.field=0;m.channel=result.channels[0];m.nibble='';
  m.instrument=m.state[4]?1:0;m.name=name;m.dirty=false;
  view.sx=view.sy=view.orderScroll=view.insScroll=0;
  document.title=`ntrk · ${name}`;view.invalidate();
  if(result.score)await refreshFeedback();
  await refreshPattern();
}
async function loadDemo() {
  await configReady;
  if(demo?.error)throw new Error(demo.error);
  if(demo) {
    const response=await fetch(demo.url,{cache:'no-store',signal:AbortSignal.timeout(10000)});
    if(!response.ok)throw new Error(`Cannot load configured example: ${demo.url}`);
    const bytes=await response.arrayBuffer();
    if(bytes.byteLength>16*1024*1024)throw new Error('Example exceeds 16 MiB limit');
    const hash=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),n=>n.toString(16).padStart(2,'0')).join('');
    if(hash!==demo.sha256)throw new Error('Example integrity mismatch; reload after deployment completes');
    await replaceSong('load',[bytes],demo.name);
  } else await replaceSong('demo',[],'Night circuit');
}
function enqueue(fn) {
  ++queued;m.busy=true;
  queue=queue.then(()=>run(fn)).finally(()=>{m.busy=!!(--queued);view.invalidate();});
  return queue;
}
async function run(fn) {try {await fn();}catch(error){status(error.message,true);}}
async function playPause(command) {
  // Resume synchronously in the originating pointer/key gesture.
  const resume=ctx.resume();await resume;
  updateState(await request('transport',[command,0,0]));
  if(m.snapshot?.score){if(command===1&&!m.feedback?.playing)m.cues=[];await refreshFeedback();}
}
function discardThen(fn) {
  if(m.dirty) {m.confirm=()=>enqueue(fn);view.invalidate();}else return fn();
}
async function action(type,arg) {
  if(!m.ready)return;
  if(type==='select'){Object.assign(m,arg);m.nibble='';selectedDescription();status(m.channel<0||m.field>=4?'Selected command lane · read only':'Primary field selected');view.invalidate();return;}
  if(type==='step'){m[arg.id]=Math.max(arg.min,Math.min(arg.max,m[arg.id]+arg.delta));view.invalidate();return;}
  if(type==='entry'){m[arg.id]=arg.toggle?!m[arg.id]:arg.value;view.invalidate();return;}
  if(type==='volume'){m.volume=Math.max(0,Math.min(1,arg));gain.gain.setTargetAtTime(m.volume,ctx.currentTime,.008);view.invalidate();return;}
  if(type==='seek'){
    if(m.snapshot?.score){status('Interactive scores move by section requests',true);return;}
    m.follow=false;updateState(await request('transport',[3,m.order,arg]));return;
  }
  if(type==='section'&&m.snapshot?.score) {
    const name=m.snapshot.score.sections.find(s=>s.id===arg)?.name||`#${arg}`;
    if(m.feedback?.started){stage('section',arg);status(`Requested section ${name} at its next boundary`);return;}
    const {state,feedback}=await request('select',[arg]);updateState(state);applyFeedback(feedback);
    status(`Initial section ${name} · Play enters it`);return;
  }
  if(type==='control'&&m.feedback?.bound) {
    const held=m.feedback.controls.find(c=>c.id===arg.id)?.held??0;
    stage('control',arg.id,Math.max(0,Math.min(1,(stagedValue('control',arg.id)??held)+arg.delta)));return;
  }
  if(type==='layer'&&m.feedback?.bound) {
    const layer=m.feedback.layers.find(l=>l.id===arg),authored=m.snapshot.score.layers.find(l=>l.id===arg);
    const target=stagedValue('layer',arg)??(layer?.held?layer.heldValue:layer?.target??1);
    stage('layer',arg,target>.5?0:1,authored?.fadeMs||0);return;
  }
  if(type!=='demo'&&type!=='open'&&!m.snapshot)return;
  if(type==='open') {open.click();return;}
  if(type==='transport')return playPause(arg);
  if(type==='mute') {updateState(await request('mute',[arg]));return;}
  if(type==='order') {
    m.follow=false;m.order=arg;m.row=0;m.nibble='';view.sy=0;view.invalidate();
    if(m.snapshot.score){await refreshPattern();return;} // browse only: sections own position
    // Observe both promises so neither failure becomes an unhandled rejection.
    const results=await Promise.allSettled([refreshPattern(),request('transport',[3,arg,0])]);
    if(results[1].status==='fulfilled')updateState(results[1].value);
    const failed=results.find(r=>r.status==='rejected');
    if(failed)throw failed.reason;
    return;
  }
  if(type==='demo')return discardThen(async()=>{
    try {await loadDemo();status('Example · ready to play');}finally{view.invalidate();}
  });
  if(type==='save') {
    try {
      const bytes=await request('save'),url=URL.createObjectURL(new Blob([bytes],{type:'application/octet-stream'}));
      const a=document.createElement('a');a.href=url;a.download=m.name.replace(/\.ntrk$/i,'')+'.ntrk';a.click();
      setTimeout(()=>URL.revokeObjectURL(url),1000);m.dirty=false;status('Saved .ntrk download');
    } finally {view.invalidate();}
  }
}
async function loadFile(file) {
  canvas.focus({preventScroll:true});
  if(!file||!m.ready)return;
  if(file.size>16*1024*1024)throw new Error('File exceeds 16 MiB limit');
  return discardThen(async()=>{
    try {await replaceSong('load',[await file.arrayBuffer()],file.name.replace(/\.(mod|xm)$/i,'.ntrk'));status('Opened locally · no upload');}
    finally {view.invalidate();}
  });
}
open.onchange=()=>{const file=open.files[0];open.value='';enqueue(()=>loadFile(file));};
canvas.addEventListener('dragover',e=>{e.preventDefault();});
canvas.addEventListener('drop',e=>{e.preventDefault();const file=e.dataTransfer.files[0];enqueue(()=>loadFile(file));});
async function write(value) {
  ++editSerial;
  await request('edit',[m.snapshot.orders[m.order],m.row,m.channel,m.field,value]);
  m.snapshot.cells[index()]=value;m.dirty=true;
  const pattern=m.snapshot.orders[m.order];
  const formatted=await request('formatted',[pattern,m.row,m.channel,-1]);
  if(m.snapshot.commands?.pattern===pattern)
    m.snapshot.commands.rows[m.row][m.channel*(1+m.state[12])]=formatted.text;
  m.row=(m.row+1)%m.state[1];
  selectedDescription();view.reveal();status('Cell updated');
}
async function keydown(event) {
  const key=event.key.toLowerCase();
  if(key==='f1'||key==='escape') {
    event.preventDefault();m.confirm=null;m.help=key==='f1'?!m.help:false;view.helpScroll=0;view.invalidate();return;
  }
  if(m.help||m.confirm||event.altKey)return;
  if(event.ctrlKey||event.metaKey) {
    if(key==='s'||key==='o') {event.preventDefault();await action(key==='s'?'save':'open');}return;
  }
  if(!m.snapshot)return;
  if(event.code==='Space'||key==='f5'||key==='f8') {
    event.preventDefault();if(!event.repeat)await playPause(key==='f8'?0:key==='f5'?1:m.state[8]?2:1);return;
  }
  if(['arrowup','arrowdown','arrowleft','arrowright','pageup','pagedown','home','end'].includes(key)) {
    event.preventDefault();m.nibble='';
    const rows=m.state[1];
    if(key==='arrowup')m.row=(m.row+rows-1)%rows;
    if(key==='arrowdown')m.row=(m.row+1)%rows;
    if(key==='pageup')m.row=Math.max(0,m.row-16);
    if(key==='pagedown')m.row=Math.min(rows-1,m.row+16);
    if(key==='home')m.row=0;if(key==='end')m.row=rows-1;
    if(key==='arrowleft'||key==='arrowright') {
      const cols=fieldLayout(m.snapshot),at=cols.findIndex(c=>c.channel===m.channel&&c.field===m.field);
      const col=cols[(at+(key==='arrowleft'?cols.length-1:1))%cols.length];
      m.channel=col.channel;m.field=col.field;
      status(col.readOnly?'Selected command lane · read only':'Primary field selected');
    }
    selectedDescription();view.reveal();return;
  }
  if(!m.edit||event.repeat)return;
  if(m.snapshot.score){status('Interactive scores are read-only in this player');return;}
  if(m.channel<0||m.field>=4){status('Selected command lane · read only');return;}
  let value;
  if(key==='delete'||key==='backspace') {value=0;m.nibble='';}
  else if(m.field===0) {
    if(key==='enter')value=97;
    else {
      const low='zsxdcvgbhnjm',high='q2w3er5t6y7u';
      const offset=low.includes(key)?low.indexOf(key):high.includes(key)?high.indexOf(key)+12:-1;
      if(offset<0)return;
      value=(m.octave-1)*12+offset+1;
      if(value>m.state[5]){status('Note exceeds module range',true);return;}
    }
  } else if(/^[0-9a-f]$/.test(key)) {
    m.nibble+=key;
    if(m.nibble.length<2){status(`Hex entry: ${m.nibble.toUpperCase()}_`);event.preventDefault();return;}
    value=parseInt(m.nibble,16);m.nibble='';
  } else return;
  event.preventDefault();
  try {
    if(m.field===0&&value!==0&&value!==97) {
      const instrument=m.instrument;
      await request('edit',[m.snapshot.orders[m.order],m.row,m.channel,1,instrument]);
      m.snapshot.cells[index(m.row,m.channel,1)]=instrument;
    }
    await write(value);
  } finally {view.invalidate();}
}
canvas.addEventListener('keydown',event=>{
  if(event.key==='Tab'||event.altKey)return;
  const key=event.key.toLowerCase();
  if(['arrowup','arrowdown','arrowleft','arrowright','pageup','pagedown','home','end','f1','f5','f8','escape','delete','backspace','enter',' '].includes(key)||((event.ctrlKey||event.metaKey)&&['s','o'].includes(key))||(!event.ctrlKey&&!event.metaKey&&key.length===1))event.preventDefault();
  if(event.code==='Space'||['f5','f8'].includes(key))ctx?.resume();
  const input={key:event.key,code:event.code,repeat:event.repeat,altKey:event.altKey,ctrlKey:event.ctrlKey,metaKey:event.metaKey,shiftKey:event.shiftKey,preventDefault(){}};
  enqueue(()=>keydown(input));
});
window.addEventListener('beforeunload',e=>{if(m.dirty){e.preventDefault();e.returnValue='';}});
run(async()=>{
  if(!window.AudioWorkletNode)throw new Error('AudioWorklet unavailable. Use a modern browser on localhost or HTTPS.');
  const response=await fetch('ntrk.wasm',{cache:'no-store'});if(!response.ok)throw new Error('Cannot load ntrk.wasm');
  const module=await WebAssembly.compile(await response.arrayBuffer());ctx=new AudioContext({sampleRate:48000});
  await ctx.audioWorklet.addModule('worklet.js');
  m.rate=ctx.sampleRate;
  node=new AudioWorkletNode(ctx,'ntrk',{numberOfInputs:0,numberOfOutputs:1,outputChannelCount:[2],processorOptions:{module}});
  node.onprocessorerror=()=>{
    m.ready=false;m.snapshot=null;m.state=null;++loadGeneration;++patternSerial;++descriptionSerial;
    for(const p of pending.values()){clearTimeout(p.timer);p.reject(new Error('Audio engine failed'));}pending.clear();
    status('Audio engine failed; reload the page to restart',true);
  };
  node.port.onmessage=({data})=>{
    if(data.state){updateState(data.state);return;}
    const p=pending.get(data.id);if(!p)return;clearTimeout(p.timer);pending.delete(data.id);
    if(data.error)p.reject(new Error(data.error));else p.resolve(data.result);
  };
  gain=ctx.createGain();
  const splitter=ctx.createChannelSplitter(2);
  analysers=[ctx.createAnalyser(),ctx.createAnalyser()];
  node.connect(gain);gain.connect(ctx.destination);gain.connect(splitter);
  analysers.forEach((analyser,channel)=>{analyser.fftSize=2048;splitter.connect(analyser,channel);});
  m.ready=true;
  status('Open or drop a .ntrk, .mod or .xm module · files stay local');
  // Example configuration is independent of engine readiness and local opening.
  configReady=fetch('demo.json',{cache:'no-store',signal:AbortSignal.timeout(10000)}).then(async response=>{
    if(!response.ok)throw new Error('Example configuration unavailable');
    const config=await response.json();
    if(config!==null&&(!config||typeof config.url!=='string'||!config.url||typeof config.name!=='string'||!config.name||!/^[0-9a-f]{64}$/.test(config.sha256)))throw new Error('Invalid example configuration');
    demo=config;
  }).catch(error=>{demo={error:error.message};});
  const levels=new Float32Array(2048);
  let polling=false;
  setInterval(()=>{
    // Measure channels separately: a mono downmix could cancel opposite phases.
    let level=0;
    for(const analyser of analysers){
      analyser.getFloatTimeDomainData(levels);
      for(const value of levels)level=Math.max(level,Math.abs(value));
    }
    if(level!==m.level){m.level=level;view.invalidate();}
    if(polling||!m.snapshot||ctx.state!=='running')return;polling=true;
    run(async()=>{try{if(m.snapshot.score)await refreshFeedback();else updateState(await request('state'));}finally{polling=false;}});
  },40);
});
