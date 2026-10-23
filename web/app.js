import {TrackerCanvas,hex} from './canvas.js';
const canvas=document.getElementById('tracker'),open=document.getElementById('open');
const m={snapshot:null,state:null,order:0,row:0,channel:0,field:0,dirty:false,nibble:'',busy:false,
  octave:2,instrument:1,edit:false,follow:true,help:false,confirm:null,name:'Loading demo',message:'Loading WebAssembly…',error:false};
let ctx,node,seq=0,demo;
const pending=new Map();
const view=new TrackerCanvas(canvas,m,(type,arg)=>run(()=>action(type,arg)));
function status(message,error=false) {m.message=message;m.error=error;view.invalidate();}
function request(type,args=[]) {
  return new Promise((resolve,reject)=>{
    const id=++seq,timer=setTimeout(()=>{pending.delete(id);reject(new Error('Audio engine did not respond'));},10000);
    pending.set(id,{resolve,reject,timer});node.port.postMessage({id,type,args});
  });
}
function index(r=m.row,c=m.channel,f=m.field) {return ((m.snapshot.orders[m.order]*m.state[1]+r)*m.state[0]+c)*4+f;}
function updateState(s) {
  if(m.state&&s.every((value,i)=>value===m.state[i]))return;
  m.state=s;
  if(m.snapshot&&!m.busy&&m.follow&&s[8]) {
    m.order=s[6];
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
function adopt(result,name) {
  const decoder=new TextDecoder();
  result.instruments=result.instruments.map(ins=>({name:decoder.decode(ins.nameBytes),volume:ins.volume}));
  m.snapshot=result;m.state=result.state;m.order=m.row=m.channel=m.field=0;m.nibble='';
  m.instrument=m.state[4]?1:0;m.name=name;m.dirty=false;
  view.sx=view.sy=view.orderScroll=view.insScroll=0;
  document.title=`ntrk · ${name}`;view.invalidate();
}
async function loadDemo() {
  if(demo) {
    const response=await fetch(demo.url);
    if(!response.ok)throw new Error(`Cannot load configured demo: ${demo.url}`);
    adopt(await request('load',[await response.arrayBuffer()]),demo.name);
  } else adopt(await request('demo'),'Night circuit');
}
async function run(fn) {try {await fn();}catch(error){status(error.message,true);}}
async function playPause(command) {
  // Resume synchronously in the originating pointer/key gesture.
  const resume=ctx.resume();await resume;
  updateState(await request('transport',[command,0,0]));
}
function discardThen(fn) {
  if(m.dirty) {m.confirm=()=>run(fn);view.invalidate();}else return fn();
}
async function action(type,arg) {
  if(m.busy||!m.snapshot)return;
  if(type==='open') {open.click();return;}
  if(type==='transport')return playPause(arg);
  if(type==='mute') {updateState(await request('mute',[arg]));return;}
  if(type==='order') {
    m.follow=false;m.order=arg;m.row=0;m.nibble='';view.sy=0;view.invalidate();
    updateState(await request('transport',[3,arg,0]));return;
  }
  if(type==='demo')return discardThen(async()=>{
    m.busy=true;try {await loadDemo();status('Demo · ready to play');}finally{m.busy=false;view.invalidate();}
  });
  if(type==='save') {
    m.busy=true;
    try {
      const bytes=await request('save'),url=URL.createObjectURL(new Blob([bytes],{type:'application/octet-stream'}));
      const a=document.createElement('a');a.href=url;a.download=m.name.replace(/\.ntrk$/i,'')+'.ntrk';a.click();
      setTimeout(()=>URL.revokeObjectURL(url),1000);m.dirty=false;status('Saved .ntrk download');
    } finally {m.busy=false;view.invalidate();}
  }
}
open.onchange=()=>run(async()=>{
  const file=open.files[0];open.value='';canvas.focus({preventScroll:true});
  if(!file||!node||m.busy)return;
  if(file.size>16*1024*1024)throw new Error('File exceeds 16 MiB limit');
  return discardThen(async()=>{
    m.busy=true;
    try {adopt(await request('load',[await file.arrayBuffer()]),file.name);status('Opened locally · no upload');}
    finally {m.busy=false;view.invalidate();}
  });
});
async function write(value) {
  await request('edit',[m.snapshot.orders[m.order],m.row,m.channel,m.field,value]);
  m.snapshot.cells[index()]=value;m.dirty=true;m.row=(m.row+1)%m.state[1];
  view.reveal();status('Cell updated');
}
canvas.addEventListener('keydown',event=>{
  const key=event.key.toLowerCase();
  if(key==='f1'||key==='escape') {
    event.preventDefault();m.confirm=null;m.help=key==='f1'?!m.help:false;view.helpScroll=0;view.invalidate();return;
  }
  if(m.help||m.confirm||!m.snapshot||m.busy||event.altKey)return;
  if(event.ctrlKey||event.metaKey) {
    if(key==='s'||key==='o') {event.preventDefault();run(()=>action(key==='s'?'save':'open'));}return;
  }
  if(event.code==='Space'||key==='f5'||key==='f8') {
    event.preventDefault();if(!event.repeat)run(()=>playPause(key==='f8'?0:key==='f5'?1:m.state[8]?2:1));return;
  }
  if(['arrowup','arrowdown','arrowleft','arrowright','pageup','pagedown','home','end','tab'].includes(key)) {
    event.preventDefault();m.nibble='';
    const rows=m.state[1],channels=m.state[0];
    if(key==='arrowup')m.row=(m.row+rows-1)%rows;
    if(key==='arrowdown')m.row=(m.row+1)%rows;
    if(key==='pageup')m.row=Math.max(0,m.row-16);
    if(key==='pagedown')m.row=Math.min(rows-1,m.row+16);
    if(key==='home')m.row=0;if(key==='end')m.row=rows-1;
    if(key==='tab')m.channel=(m.channel+channels+(event.shiftKey?-1:1))%channels;
    if(key==='arrowleft'||key==='arrowright') {
      const pos=(m.channel*4+m.field+(key==='arrowleft'?channels*4-1:1))%(channels*4);
      m.channel=Math.floor(pos/4);m.field=pos%4;
    }
    view.reveal();return;
  }
  if(!m.edit||event.repeat)return;
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
  event.preventDefault();m.busy=true;
  run(async()=>{
    try {
      if(m.field===0&&value!==0&&value!==97) {
        const instrument=m.instrument;
        await request('edit',[m.snapshot.orders[m.order],m.row,m.channel,1,instrument]);
        m.snapshot.cells[index(m.row,m.channel,1)]=instrument;
      }
      await write(value);
    } finally {m.busy=false;view.invalidate();}
  });
});
window.addEventListener('beforeunload',e=>{if(m.dirty){e.preventDefault();e.returnValue='';}});
run(async()=>{
  if(!window.AudioWorkletNode)throw new Error('AudioWorklet unavailable. Use a modern browser on localhost or HTTPS.');
  const config=await fetch('demo.json');if(!config.ok)throw new Error('Cannot load demo configuration');
  demo=await config.json();
  if(demo!==null&&(!demo||typeof demo.url!=='string'||!demo.url||typeof demo.name!=='string'||!demo.name))throw new Error('Invalid demo configuration');
  const response=await fetch('ntrk.wasm');if(!response.ok)throw new Error('Cannot load ntrk.wasm');
  const module=await WebAssembly.compile(await response.arrayBuffer());ctx=new AudioContext({sampleRate:48000});
  await ctx.audioWorklet.addModule('worklet.js');
  node=new AudioWorkletNode(ctx,'ntrk',{numberOfInputs:0,numberOfOutputs:1,outputChannelCount:[2],processorOptions:{module}});
  node.onprocessorerror=()=>status('Audio engine failed; reload the page to restart',true);
  node.port.onmessage=({data})=>{
    if(data.state){updateState(data.state);return;}
    const p=pending.get(data.id);if(!p)return;clearTimeout(p.timer);pending.delete(data.id);
    if(data.error)p.reject(new Error(data.error));else p.resolve(data.result);
  };
  node.connect(ctx.destination);await loadDemo();status('Ready · press Play to enable audio');canvas.focus({preventScroll:true});
  let polling=false;
  setInterval(()=>{
    if(polling||ctx.state!=='running')return;polling=true;
    run(async()=>{try{updateState(await request('state'));}finally{polling=false;}});
  },40);
});
