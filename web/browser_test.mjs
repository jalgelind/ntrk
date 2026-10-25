// Optional browser harness. Existing Playwright only; no installs or production test hooks.
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import {Engine} from './engine.js';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.NTRK_PLAYWRIGHT||'playwright');
const root=process.env.NTRK_WEB_ROOT||fileURLToPath(new URL('../out/web/',import.meta.url));
const mime={'.html':'text/html','.js':'text/javascript','.css':'text/css','.wasm':'application/wasm'};
const server=createServer(async(req,res)=>{
  try {
    const pathname=new URL(req.url,'http://localhost').pathname;
    const name=pathname.endsWith('/')?'index.html':path.basename(pathname);
    const bytes=await readFile(path.join(root,name));
    res.writeHead(200,{'Content-Type':mime[path.extname(name)]||'application/octet-stream'}).end(bytes);
  } catch {res.writeHead(404).end();}
});
let browser;
try {
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  browser=await chromium.launch({headless:true});
  const page=await browser.newPage({acceptDownloads:true,viewport:{width:1280,height:720},deviceScaleFactor:2});
  const errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.addInitScript(()=>{
    const createGain=AudioContext.prototype.createGain;
    AudioContext.prototype.createGain=function(){const gain=createGain.call(this);window.testGain??=gain;return gain;};
    const Original=window.AudioWorkletNode;
    window.AudioWorkletNode=class extends Original {
      constructor(...args){
        super(...args);window.testNode=this;window.testContext=args[0];
        const descriptor=Object.getOwnPropertyDescriptor(MessagePort.prototype,'onmessage');
        window.delayedPatterns=[];window.delayedLoads=[];
        Object.defineProperty(this.port,'onmessage',{set(fn){descriptor.set.call(this,event=>{
          const result=event.data.result;
          if(window.delayPatternReplies&&result?.rows&&result.start===0&&Number.isInteger(result.pattern))
            delayedPatterns.push({pattern:result.pattern,release:()=>fn(event)});
          else if(window.delayLoadReplies&&result?.cells&&result?.orders)
            delayedLoads.push(()=>fn(event));
          else fn(event);
        });}});
      }
    };
    // Record the actual paint calls, not DOM surrogates for a canvas.
    const text=CanvasRenderingContext2D.prototype.fillText;
    window.painted=[];window.drawn=[];
    CanvasRenderingContext2D.prototype.fillText=function(s,...args){painted.push(String(s));drawn.push({text:String(s),x:args[0],y:args[1]});if(drawn.length>12000)drawn.splice(0,6000);if(painted.length>12000)painted.splice(0,6000);return text.call(this,s,...args);};
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/player/`);
  const demo=JSON.parse(await readFile(path.join(root,'demo.json'),'utf8'));
  const demoName=demo?.name||'Night circuit';
  // Observe the renderer through its module prototype in this harness only.
  await page.evaluate(async()=>{
    const {TrackerCanvas}=await import('/player/canvas.js'+new URL(document.querySelector('script').src).search),draw=TrackerCanvas.prototype.draw;
    TrackerCanvas.prototype.draw=function(){window.view=this;return draw.call(this);};
    dispatchEvent(new Event('resize'));
    window.engineCommand=(type,args=[])=>new Promise((resolve,reject)=>{
      const id=1000000+Math.random();
      const listener=({data})=>{if(data.id!==id)return;testNode.port.removeEventListener('message',listener);data.error?reject(new Error(data.error)):resolve(data.result);};
      testNode.port.addEventListener('message',listener);testNode.port.postMessage({id,type,args});
    });
  });
  await page.waitForFunction(()=>window.view?.m.ready&&view.hits.some(h=>h.id==='open'));
  const model=()=>page.evaluate(()=>({name:view.m.name,state:view.m.state,message:view.m.message,dirty:view.m.dirty,follow:view.m.follow}));
  const wait=async fn=>{try{return await page.waitForFunction(fn);}catch(error){console.error('Browser state:',await model(),errors);throw error;}};
  const click=async(x,y)=>{await page.evaluate(()=>new Promise(requestAnimationFrame));await page.mouse.click(x,y);assert(await page.evaluate(()=>document.activeElement===document.querySelector('canvas')),'canvas retains keyboard focus');};
  // Toolbar coordinates are independent expectations of the compact bands.
  const button=async id=>{
    await page.evaluate(()=>new Promise(requestAnimationFrame));
    const box=await page.evaluate(id=>view.hits.find(h=>h.id===id)?.r,id);
    assert(box,`enabled drawn ${id}`);await click(box.x+box.w/2,box.y+box.h/2);
  };
  assert.equal(await page.title(),'ntrk · Module player');
  assert(await page.evaluate(()=>!view.m.snapshot&&view.hits.some(h=>h.id==='open')&&!view.hits.some(h=>h.id==='play')));
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.startup.png'});
  await button('demo');await wait(()=>!!view.m.snapshot&&!view.m.busy);
  const cell=async(r,c,f)=>{
    await page.evaluate(([r,c,f])=>view.reveal(r,c,f),[r,c,f]);await page.evaluate(()=>new Promise(requestAnimationFrame));
    await page.waitForFunction(()=>view.m.snapshot?.commands?.pattern===view.m.snapshot.orders[view.m.order]);
    const p=await page.evaluate(([r,c,f])=>{const b=view.cell(r,c,f);return {x:b.x+b.w/2,y:b.y+b.h/2};},[r,c,f]);
    await click(p.x,p.y);
    assert.deepEqual(await page.evaluate(()=>[view.m.row,view.m.channel,view.m.field]),[r,c,f],'pointer maps to intended field');
  };
  const download=async()=>{const pending=page.waitForEvent('download');await button('save');const file=await pending;assert(file.suggestedFilename().endsWith('.ntrk'));return readFile(await file.path());};
  const snapshot=()=>page.evaluate(()=>engineCommand('snapshot'));
  const value=async(r,c,f)=>page.evaluate(async([r,c,f])=>{const s=await engineCommand('snapshot');return s.cells[((s.orders[view.m.order]*s.state[1]+r)*s.state[0]+c)*4+f];},[r,c,f]);
  const pixels=(region)=>page.evaluate(async region=>{
    await new Promise(requestAnimationFrame);
    const c=document.querySelector('canvas'),g=c.getContext('2d'),d=devicePixelRatio;
    const b=region==='grid'?view.raster:region==='header'?{x:0,y:0,w:innerWidth,h:135}:{x:0,y:0,w:innerWidth,h:innerHeight};
    const bytes=g.getImageData(Math.round(b.x*d),Math.round(b.y*d),Math.round(b.w*d),Math.round(b.h*d)).data;
    let hash=2166136261;const colors=new Set();
    for(let i=0;i<bytes.length;i+=4){hash=Math.imul(hash^bytes[i],16777619);hash=Math.imul(hash^bytes[i+1],16777619);colors.add((bytes[i]<<16)|(bytes[i+1]<<8)|bytes[i+2]);}
    return {hash:hash>>>0,colors:colors.size};
  },region);
  assert.equal((await model()).state[8],0);
  console.log('initial pixels',await pixels());
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.initial.png'});
  assert((await pixels()).colors>100,'nonempty antialiased tracker rendering');
  assert(await page.evaluate(()=>document.querySelector('canvas').width===innerWidth*devicePixelRatio),'DPR backing store');
  assert.equal(await page.locator('canvas').count(),1);
  assert.equal(await page.locator('button,table,details,output').count(),0);
  assert.equal(await page.locator('input:visible').count(),0);
  const initialBytes=await download();
  if(demo){
    const engine=new Engine(await WebAssembly.compile(await readFile(path.join(root,'ntrk.wasm'))),48000);
    engine.command('load',[await readFile(path.join(root,demo.url))]);
    assert.deepEqual(initialBytes,Buffer.from(engine.command('save')),'authored demo plus mandatory IPAN export');
  }
  const initial=await snapshot();assert.equal(initial.state[0],16);assert.equal(initial.state[1],32);
  const names=Object.values(initial.instruments).map(i=>Buffer.from(Object.values(i.nameBytes)).toString()).filter(Boolean);
  assert(names.length>0);assert(await page.evaluate(names=>names.some(n=>painted.some(t=>t.includes(n))),names),'module-owned instrument names are drawn');
  // Every authored command column has real paint, hit and navigation geometry.
  assert.equal(initial.instruments[0].pan,128);
  assert(await page.evaluate(()=>painted.some(t=>t.startsWith('Pan center'))),'instrument-owned pan shown');
  const verifyPaint=async(r,c,f)=>{
    await cell(r,c,f);
    const result=await page.evaluate(async([r,c,f])=>{
      const {commandCell}=await import('/player/canvas.js'+new URL(document.querySelector('script').src).search);
      const expected=commandCell(view.m.snapshot,view.m.snapshot.orders[view.m.order],r,c,f).text;
      drawn=[];view.invalidate();await new Promise(requestAnimationFrame);await new Promise(requestAnimationFrame);
      const b=view.cell(r,c,f);
      return {expected,painted:drawn.some(t=>t.text===expected&&t.x===Math.round(b.x+2)&&t.y===Math.round(b.y+b.h/2))};
    },[r,c,f]);
    assert(result.painted,`formatted cell ${c}/${f}: ${result.expected}`);
  };
  if(demo) {
    assert.equal(initial.state[12],3);assert.equal(initial.state[13],5);
    for(const c of [0,1,2,3])for(let f=4;f<7;f++)await verifyPaint(0,c,f);
    for(let lane=0;lane<5;lane++)await verifyPaint(0,-1,lane);
    assert(await page.evaluate(()=>view.sx>0),'meta columns reveal horizontally');
    if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.meta.png'});
    const before=await download();await button('edit');
    await cell(0,-1,0);await page.keyboard.press('Delete');await page.keyboard.type('ff');
    await wait(()=>!view.m.busy);assert.match((await model()).message,/read only/);
    await cell(0,0,4);await page.keyboard.type('12');await page.keyboard.press('Enter');
    await wait(()=>!view.m.busy);assert.deepEqual(await download(),before,'read-only lanes cannot corrupt primary bytes');
    await button('edit');
  }
  await cell(0,0,0);
  const idlePixels=await pixels('grid');
  await button('play');await wait(()=>view.m.state[8]===1);
  await page.waitForTimeout(100);
  assert.notEqual((await pixels('grid')).hash,idlePixels.hash,'playing row changes raster pixels');
  const peak=await page.evaluate(async()=>{
    const analyser=testContext.createAnalyser();analyser.fftSize=2048;testNode.connect(analyser);
    const data=new Float32Array(2048);let peak=0;
    for(let i=0;i<20;i++) {await new Promise(r=>setTimeout(r,25));analyser.getFloatTimeDomainData(data);for(const x of data){if(!Number.isFinite(x))throw new Error('Non-finite audio');peak=Math.max(peak,Math.abs(x));}}
    testNode.disconnect(analyser);return peak;
  });
  assert(peak>.001&&peak<=1,`actual worklet audio peak ${peak}`);
  await button('pause');await wait(()=>view.m.state[8]===0);
  const paused=(await model()).state.slice(6,8);await page.waitForTimeout(150);assert.deepEqual((await model()).state.slice(6,8),paused);
  await page.evaluate(()=>{view.sx=view.sy=0;view.invalidate();});await page.waitForTimeout(40);
  const header=await page.evaluate(()=>({x:view.raster.x+35,y:view.grid.y-14}));
  await click(header.x,header.y);await wait(()=>!!(view.m.state[11]&1));
  assert((await page.evaluate(()=>engineCommand('state')))[11]&1);
  await button('stop');await wait(()=>view.m.state[6]===0&&view.m.state[7]===0);assert((await model()).state[11]&1);
  const order=await page.evaluate(()=>({x:view.orders.x+40,y:view.orders.y+30}));
  await click(order.x,order.y);await wait(()=>view.m.state[6]===1);assert.equal((await model()).follow,false);
  // Two-sided controls and the real instrument list change the note-entry source.
  await click(104,117);await wait(()=>view.m.octave===3);await click(74,117);await wait(()=>view.m.octave===2);
  const ins=await page.evaluate(()=>({x:view.instruments.x+25,y:view.instruments.y+30}));
  await click(ins.x,ins.y);await wait(()=>view.m.instrument===2);
  await click(194,117);await wait(()=>view.m.instrument===1);
  await button('edit');await wait(()=>view.m.edit);
  await cell(0,0,0);const beforeEdit=(await pixels('grid')).hash;
  await page.keyboard.press('x');await wait(()=>!view.m.busy&&view.m.row===1);
  assert.equal(await value(0,0,0),15);assert.equal(await value(0,0,1),1);
  assert.notEqual((await pixels('grid')).hash,beforeEdit,'edited cell / cursor pixels change');
  await cell(1,0,3);await page.keyboard.type('7f');await wait(()=>!view.m.busy&&view.m.row===2);assert.equal(await value(1,0,3),127);
  await cell(2,0,0);await page.keyboard.press('Enter');await wait(()=>!view.m.busy&&view.m.row===3);assert.equal(await value(2,0,0),97);
  await cell(3,0,3);await page.keyboard.press('Delete');await wait(()=>!view.m.busy&&view.m.row===4);assert.equal(await value(3,0,3),0);
  await cell(4,0,1);await page.keyboard.type('ff');await wait(()=>!view.m.busy);assert.match((await model()).message,/out of range/);
  // Drawn Help is modal, scrollable, and leaves cells intact.
  const headerBefore=await pixels('header');await button('help');await wait(()=>view.m.help);
  assert.notEqual((await pixels('header')).hash,headerBefore.hash);
  assert(await page.evaluate(()=>painted.includes('KEYBOARD & FILE HELP')));
  const cellsBefore=JSON.stringify((await snapshot()).cells);await page.keyboard.press('x');assert.equal(JSON.stringify((await snapshot()).cells),cellsBefore);
  await page.mouse.wheel(0,400);await wait(()=>view.helpScroll>0);await page.keyboard.press('Escape');await wait(()=>!view.m.help);
  assert((await model()).dirty);
  // Unsaved replacement uses a drawn cancel/discard dialog, not native widgets.
  await button('demo');await wait(()=>!!view.m.confirm);
  await page.keyboard.press('Escape');await wait(()=>!view.m.confirm);assert((await model()).dirty);
  const bytes=await download();assert.equal(bytes.subarray(0,4).toString(),'NTRK');
  // Real Open toolbar opens the hidden file chooser.
  const chooser=page.waitForEvent('filechooser');await button('open');
  await(await chooser).setFiles({name:'edited.ntrk',mimeType:'application/octet-stream',buffer:bytes});
  await wait(()=>view.m.name==='edited.ntrk');
  await click(order.x,order.y);await wait(()=>view.m.state[6]===1);
  assert.equal(await value(0,0,0),15);assert.equal(await value(1,0,3),127);assert.equal(await value(2,0,0),97);
  await page.locator('#open').setInputFiles({name:'bad.ntrk',mimeType:'application/octet-stream',buffer:Buffer.from('bad')});
  await wait(()=>view.m.message.includes('Invalid'));assert.equal((await model()).name,'edited.ntrk');assert.equal(await value(0,0,0),15);
  await cell(0,0,0);await page.keyboard.press('x');await wait(()=>!view.m.busy&&view.m.dirty);
  await button('demo');await wait(()=>!!view.m.confirm);await click(367,123);
  await wait(()=>view.m.name===document.title.slice(7)&&view.m.name!=='edited.ntrk');
  assert.equal((await model()).name,demoName);assert.deepEqual(await download(),initialBytes);
  const fixtureRoot=fileURLToPath(new URL('../out/web-fixtures/',import.meta.url));
  const loadBytes=async(name,buffer)=>{
    await page.locator('#open').setInputFiles({name,mimeType:'application/octet-stream',buffer});
    await page.waitForFunction(name=>!view.m.busy&&view.m.name===name,name);
    assert.equal((await model()).name,name);
    await page.evaluate(()=>new Promise(requestAnimationFrame));
  };
  const loadFixture=async name=>loadBytes(name+'.ntrk',await readFile(path.join(fixtureRoot,name+'.ntrk')));
  const visible=()=>page.evaluate(()=>view.m.snapshot.channels);
  assert.deepEqual(await visible(),demo?[0,1,2,3]:[0,1,2]);
  const small=await readFile(path.join(fixtureRoot,'count-03.ntrk'));
  await loadBytes('small-metadata.ntrk',small);
  assert.deepEqual(await download(),small,'small-count META and opaque bytes retained at /player/');
  await loadBytes('small-reopened.ntrk',await download());
  assert.deepEqual(await download(),small);

  assert.deepEqual(await visible(),[0,2]);
  // Large valid files retain compact raw song data and one formatted pattern.
  await loadFixture('large');
  assert.equal((await model()).state[2],198);
  assert(await page.evaluate(()=>view.m.snapshot.commands.rows.length===256&&view.m.snapshot.commands.rows.every(r=>r.length===149)));
  await cell(255,15,3);await page.keyboard.type('7b');await wait(()=>!view.m.busy);
  assert.equal(await value(255,15,3),123);
  const largeSaved=await download();await loadBytes('large-reloaded.ntrk',largeSaved);
  assert.equal(await value(255,15,3),123);assert.deepEqual(await download(),largeSaved);
  await loadFixture('pattern-transition');
  await page.evaluate(()=>{if(!view.m.follow)view.action('entry',{id:'follow',value:true});});
  await button('play');
  await wait(()=>view.m.order===1&&view.m.snapshot.commands?.pattern===1&&view.m.snapshot.commands.rows[0][0]==='PAN');
  await button('pause');await wait(()=>!view.m.busy);
  await page.evaluate(()=>view.action('order',0));await wait(()=>!view.m.busy&&view.m.order===0&&view.m.snapshot.commands?.pattern===0);
  assert.equal(await page.evaluate(()=>view.m.snapshot.commands.rows[0][0]),'SOF');
  // Delay a follow reply, then browse elsewhere. Blank during transition;
  // releasing the older reply after the new one cannot repaint stale commands.
  await page.evaluate(()=>{delayPatternReplies=true;view.action('entry',{id:'follow',value:true});});
  await button('play');await wait(()=>delayedPatterns.some(p=>p.pattern===1));
  assert(await page.evaluate(()=>view.m.snapshot.commands===null));
  assert.equal(await page.evaluate(async()=>{const {commandCell}=await import('/player/canvas.js'+new URL(document.querySelector('script').src).search);return commandCell(view.m.snapshot,view.m.snapshot.orders[view.m.order],0,0,2).text;}),'');
  await button('pause');await wait(()=>!view.m.busy);
  await page.evaluate(()=>view.action('order',0));await wait(()=>delayedPatterns.some(p=>p.pattern===0));
  await page.evaluate(()=>{const newest=delayedPatterns.findLast(p=>p.pattern===0);newest.release();});
  await wait(()=>!view.m.busy&&view.m.snapshot.commands?.pattern===0);
  await page.evaluate(()=>{delayPatternReplies=false;for(const reply of delayedPatterns)reply.release();delayedPatterns=[];});
  assert.equal(await page.evaluate(()=>view.m.snapshot.commands.pattern),0);
  assert.equal(await page.evaluate(()=>view.m.snapshot.commands.rows[0][0]),'SOF');
  // A pending reply from the old song must also be rejected after a replacement.
  await page.evaluate(()=>{delayPatternReplies=true;view.action('entry',{id:'follow',value:true});});
  await button('play');await wait(()=>delayedPatterns.some(p=>p.pattern===1));
  await page.evaluate(()=>{delayPatternReplies=false;});
  await loadFixture('commands');
  await page.evaluate(()=>{for(const reply of delayedPatterns)reply.release();delayedPatterns=[];});
  assert.equal(await page.evaluate(()=>view.m.snapshot.commands.rows[0][0]),'PAN');
  assert(await page.evaluate(()=>view.m.snapshot.commands.pattern===0&&view.m.snapshot.commands.rows.length===32));
  // Simulate an acknowledged engine load whose response takes over ten seconds.
  // The old UI is hidden and the mutation remains pending instead of timing out.
  await page.evaluate(()=>{delayLoadReplies=true;});
  await page.locator('#open').setInputFiles({name:'delayed-load.ntrk',mimeType:'application/octet-stream',buffer:await readFile(path.join(fixtureRoot,'pattern-transition.ntrk'))});
  await wait(()=>delayedLoads.length>0);
  assert.equal(await page.evaluate(()=>view.m.snapshot),null);
  assert.equal((await page.evaluate(()=>engineCommand('state')))[1],2);
  await page.waitForTimeout(10500);
  assert(await page.evaluate(()=>view.m.busy&&view.m.snapshot===null&&view.m.ready));
  await page.evaluate(()=>{delayLoadReplies=false;for(const release of delayedLoads)release();delayedLoads=[];});
  await wait(()=>!view.m.busy&&view.m.name==='delayed-load.ntrk'&&view.m.snapshot.commands?.pattern===0);
  console.log('browser bounded commands: large load/edit/save, order/follow, blank transition, reversed replies, load generation and >10s acknowledged load passed');
  await loadFixture('commands');
  for(const [c,mnemonic] of [[0,'PAN'],[1,'SLC'],[2,'SOF'],[3,'PAN']]) {
    await verifyPaint(0,c,2);
    const label=await page.evaluate(c=>view.m.snapshot.commands.rows[0][c*4],c);
    assert.equal(label,mnemonic);
  }
  await verifyPaint(0,-1,0);assert(await page.evaluate(()=>painted.includes('M01 00')));
  await verifyPaint(0,-1,1);assert(await page.evaluate(()=>painted.includes('... 40')));
  await loadFixture('sparse');
  assert.deepEqual(await visible(),[0,3,11,14]);
  assert.equal((await snapshot()).usedMask,(1<<0)|(1<<3)|(1<<11)|(1<<14));
  assert(await page.evaluate(()=>view.hits.filter(h=>h.id.startsWith('mute')).every(h=>[0,3,11,14].includes(Number(h.id.slice(4))))));
  assert.equal(await value(0,14,0),0,'FXPL-only track has no primary note');
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.sparse.png'});
  for(const c of [0,3,11,14]) await cell(0,c,0);
  assert(await page.evaluate(()=>['CH 01','CH 04','CH 0C','CH 0F'].every(label=>painted.includes(label))),'original IDs drawn with all lanes');
  await cell(0,3,1);await page.keyboard.type('02');await wait(()=>!view.m.busy&&view.m.row===1);
  assert.equal(await value(0,3,1),2);assert.equal(await value(0,1,1),0,'compact slot never edits its numeric slot index');
  await cell(0,3,5);await page.keyboard.press('ArrowRight');await wait(()=>!view.m.busy);
  assert.deepEqual(await page.evaluate(()=>[view.m.channel,view.m.field]),[11,0]);
  await page.keyboard.press('ArrowLeft');await wait(()=>!view.m.busy);assert.deepEqual(await page.evaluate(()=>[view.m.channel,view.m.field]),[3,5]);
  await page.evaluate(()=>view.reveal(0,11,0));await page.evaluate(()=>new Promise(requestAnimationFrame));
  const sparseMute=await page.evaluate(()=>({x:view.cell(0,11,0).x+35,y:view.grid.y-14}));
  await click(sparseMute.x,sparseMute.y);await wait(()=>view.m.state[11]===(1<<11));
  assert.equal((await page.evaluate(()=>engineCommand('state')))[11],1<<11);
  // Clearing a track's only cell must not make the cursor jump or strand it.
  await cell(0,0,0);await page.keyboard.press('Delete');await wait(()=>!view.m.busy&&view.m.row===1);
  assert.deepEqual(await visible(),[0,3,11,14]);assert.equal(await page.evaluate(()=>view.m.channel),0);
  const sparseSaved=await download();await loadBytes('sparse-cleared.ntrk',sparseSaved);
  assert.deepEqual(await visible(),[3,11,14]);assert.equal(await page.evaluate(()=>view.m.channel),3,'new load clamps cursor to first visible original ID');
  assert.equal(await value(0,3,1),2);assert.equal((await model()).state[0],16);
  assert.deepEqual(await download(),sparseSaved,'save keeps original channel count, FXPL and unreferenced patterns');
  await loadFixture('empty');assert.deepEqual(await visible(),[0]);assert.equal((await snapshot()).usedMask,0);
  assert.equal((await model()).state[0],16,'empty module keeps its actual channel count');
  const emptyBlank=await page.evaluate(()=>({x:view.raster.x+view.channelW+8*view.advance+30,y:view.grid.y+10}));
  await click(emptyBlank.x,emptyBlank.y);assert.equal(await page.evaluate(()=>view.m.channel),0,'empty raster space is not an unused channel');
  await cell(0,0,0);await page.keyboard.press('x');await wait(()=>!view.m.busy&&view.m.row===1);assert.equal(await value(0,0,0),15);
  await cell(0,0,0);await page.keyboard.press('Delete');await wait(()=>!view.m.busy&&view.m.row===1);
  await cell(0,0,1);await page.keyboard.press('Delete');await wait(()=>!view.m.busy&&view.m.row===1);
  assert.equal((await snapshot()).usedMask,0);assert.deepEqual(await visible(),[0]);
  await download();await loadFixture('wide');assert.deepEqual(await visible(),[0,1,3,4,6,7,9,11,13,15]);
  // Wheel and pointer scrollbars both address real geometry; scrolling does not edit.
  const scrollStart=await pixels('grid');
  await page.mouse.move(600,330);await page.mouse.wheel(230,240);await wait(()=>view.sx>0&&view.sy>0);
  assert.notEqual((await pixels('grid')).hash,scrollStart.hash);
  const target=await page.evaluate(()=>{const b=view.xbar;return {x:b.r.x+b.r.w-10,y:b.r.y+5};});
  await click(target.x,target.y);await wait(()=>view.sx>view.maxX*.8);
  const c=await page.evaluate(()=>view.m.snapshot.channels[Math.ceil(view.sx/view.channelW)]);
  const r=await page.evaluate(()=>Math.ceil(view.sy/view.pitch));
  await cell(r,c,2);await page.keyboard.press('Home');await page.keyboard.press('ArrowLeft');
  await wait(()=>view.m.row===0&&view.sy===0);
  await button('demo');await wait(()=>view.m.name===document.title.slice(7)&&!view.m.name.endsWith('.ntrk'));
  assert.deepEqual(await visible(),demo?[0,1,2,3]:[0,1,2]);
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.desktop.png'});
  await page.setViewportSize({width:390,height:844});await wait(()=>view.w===390);
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  assert((await pixels()).colors>100);
  await button('help');await wait(()=>view.m.help);await page.mouse.wheel(0,600);await wait(()=>view.helpScroll>0);
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.help.png'});
  // The drawn close control must work at the resized overlay's actual geometry.
  await click(328,31);await wait(()=>!view.m.help);
  const mobileHeader=await page.evaluate(()=>({x:view.raster.x+25,y:view.grid.y-13}));
  await click(mobileHeader.x,mobileHeader.y);await wait(()=>!!(view.m.state[11]&1));
  await loadFixture('wide');
  await page.mouse.move(250,400);await page.mouse.wheel(360,220);await wait(()=>view.sx>0&&view.sy>0);
  const mobileC=await page.evaluate(()=>view.m.snapshot.channels[Math.ceil(view.sx/view.channelW)]),mobileR=await page.evaluate(()=>Math.ceil(view.sy/view.pitch));
  await cell(mobileR,mobileC,0);
  // Touch-style pointer drag pans without changing the selected cell.
  const selected=await page.evaluate(()=>[view.m.row,view.m.channel,view.m.field]);
  await page.mouse.move(260,450);await page.mouse.down();await page.mouse.move(200,370,{steps:8});await page.mouse.up();
  assert.deepEqual(await page.evaluate(()=>[view.m.row,view.m.channel,view.m.field]),selected);
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.mobile-scrolled.png'});
  await button('follow');await wait(()=>view.m.follow);await button('follow');await wait(()=>!view.m.follow);
  await button('demo');await wait(()=>!view.m.name.endsWith('.ntrk'));
  await page.evaluate(()=>{view.sx=view.sy=0;view.invalidate();});await cell(0,0,0);
  assert.deepEqual(await visible(),demo?[0,1,2,3]:[0,1,2]);
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.mobile.png'});
  if(demo){await cell(0,-1,4);if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.mobile-meta.png'});}
  // Generic local-file checks include actual foreign sampled modules.
  for(const name of ['sampled.mod','sampled.xm','sampled.mod.ntrk','sampled.xm.ntrk']) {
    await loadBytes(name.replace(/\.(mod|xm)$/i,'.ntrk'),await readFile(path.join(fixtureRoot,name)));
    assert((await snapshot()).instruments[0]);
    const saved=await download();assert.equal(saved.subarray(0,4).toString(),'NTRK');
  }
  // All keys dispatch synchronously, while worklet edits are still outstanding.
  await page.setViewportSize({width:1280,height:720});await wait(()=>view.w===1280);
  await page.evaluate(()=>{view.sx=view.sy=0;view.invalidate();});
  if(!await page.evaluate(()=>view.m.edit))await button('edit');
  await cell(0,0,0);
  const burstPending=page.waitForEvent('download');
  await page.evaluate(()=>{
    const canvas=document.querySelector('canvas');
    for(const key of ['z','x','c','ArrowRight','0','1','ArrowLeft','v'])
      canvas.dispatchEvent(new KeyboardEvent('keydown',{key,bubbles:true,cancelable:true}));
    canvas.dispatchEvent(new KeyboardEvent('keydown',{key:'s',ctrlKey:true,bubbles:true,cancelable:true}));
  });
  const burstDownload=await burstPending;
  await wait(()=>!view.m.busy);
  assert.equal(await value(0,0,0),13);assert.equal(await value(1,0,0),15);assert.equal(await value(2,0,0),17);
  assert.equal(await value(3,0,1),1);assert.equal(await value(4,0,0),18);
  const burst=await readFile(await burstDownload.path());
  await loadBytes('burst.ntrk',burst);
  assert.equal(await value(4,0,0),18,'saved bytes include the final queued edit');
  await page.evaluate(()=>document.querySelector('canvas').focus());await page.keyboard.press('Tab');
  assert(await page.evaluate(()=>document.activeElement!==document.querySelector('canvas')),'Tab leaves canvas');
  // Local drop uses the browser File/DataTransfer path.
  const dropBytes=await readFile(path.join(fixtureRoot,'sampled.xm'));
  await page.evaluate(bytes=>{
    const canvas=document.querySelector('canvas');
    for(const key of ['z','x'])canvas.dispatchEvent(new KeyboardEvent('keydown',{key,bubbles:true}));
    const data=new DataTransfer();data.items.add(new File([new Uint8Array(bytes)],'pending.xm'));
    canvas.dispatchEvent(new DragEvent('drop',{dataTransfer:data,bubbles:true,cancelable:true}));
  },Array.from(dropBytes));
  await wait(()=>!!view.m.confirm&&!view.m.busy);
  assert.equal((await model()).name,'burst.ntrk','replacement waits for pending edits and their discard decision');
  assert.equal(await value(0,0,0),13);assert.equal(await value(1,0,0),15);
  await page.keyboard.press('Escape');await wait(()=>!view.m.confirm);await download();
  await page.evaluate(bytes=>{
    const data=new DataTransfer();data.items.add(new File([new Uint8Array(bytes)],'dropped.xm'));
    document.querySelector('canvas').dispatchEvent(new DragEvent('drop',{dataTransfer:data,bubbles:true,cancelable:true}));
  },Array.from(dropBytes));
  await wait(()=>view.m.name==='dropped.ntrk'&&!view.m.busy);
  // The output meter and host gain observe actual audio; song bytes remain identical.
  const beforeVolume=await download();await button('play');await wait(()=>view.m.level>.00001);
  for(let i=0;i<10;i++)await button('volume-');await wait(()=>view.m.volume<.0001&&view.m.level<.00001);
  for(let i=0;i<10;i++)await button('volume+');await wait(()=>view.m.volume>.999&&view.m.level>.00001);
  await button('stop');assert.deepEqual(await download(),beforeVolume,'host volume never changes module');
  await page.evaluate(()=>{
    const oscillator=testContext.createOscillator(),inverse=testContext.createGain(),merger=testContext.createChannelMerger(2);
    inverse.gain.value=-1;oscillator.connect(merger,0,0);oscillator.connect(inverse);inverse.connect(merger,0,1);
    merger.connect(testGain);oscillator.start();window.stereoProbe={oscillator,inverse,merger};
  });
  await wait(()=>view.m.level>.5); // Opposite phases must not disappear in a mono meter.
  await page.evaluate(()=>{stereoProbe.oscillator.stop();for(const node of Object.values(stereoProbe))node.disconnect();});
  await wait(()=>view.m.level<.00001);
  // Double-click seeks accurately, both stopped and running.
  const seek=await page.evaluate(()=>{const b=view.cell(4,0,0);return {x:b.x+5,y:b.y+5};});
  await page.mouse.dblclick(seek.x,seek.y);await wait(()=>view.m.state[7]===4&&!view.m.busy);
  await button('play');await page.mouse.dblclick(seek.x,seek.y);await wait(()=>view.m.state[8]===1&&view.m.state[7]===4&&!view.m.busy);await button('stop');
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.generic-desktop.png'});
  // Real touch events start on hit rows in both long lists.
  await loadFixture('long');await page.setViewportSize({width:390,height:844});await wait(()=>view.w===390);
  const cdp=await page.context().newCDPSession(page);
  await cdp.send('Emulation.setTouchEmulationEnabled',{enabled:true});
  for(const list of ['orders','instruments']) {
    const r=await page.evaluate(list=>view[list],list);
    const selected=await page.evaluate(()=>[view.m.order,view.m.instrument]);
    await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:r.x+20,y:r.y+r.h-10}]});
    for(let step=1;step<=8;step++)await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:r.x+20,y:r.y+r.h-10-step*12}]});
    await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
    await wait(()=>view.orderScroll>0||view.insScroll>0);
    assert.deepEqual(await page.evaluate(()=>[view.m.order,view.m.instrument]),selected,'drag does not click list row');
    assert(await page.evaluate(list=>list==='orders'?view.orderScroll>0:view.insScroll>0,list));
    const target=await page.evaluate(list=>{const r=view[list],scroll=list==='orders'?view.orderScroll:view.insScroll;return {x:r.x+20,y:r.y+10,index:Math.floor((scroll+10)/view.pitch)};},list);
    await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:target.x,y:target.y}]});
    await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:target.x+1,y:target.y+1}]});
    await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
    await page.waitForFunction(([list,index])=>list==='orders'?view.m.order===index:view.m.instrument===index+1,[list,target.index]);
  }
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.generic-mobile.png'});
  // Invalid config/asset never prevents generic startup or local Open.
  for(const config of ['bad',JSON.stringify({url:'missing.ntrk',name:'Missing example',sha256:'0'.repeat(64)}),JSON.stringify({url:'ntrk.wasm',name:'Stale example',sha256:'0'.repeat(64)})]) {
    const isolated=await browser.newPage({viewport:{width:390,height:844}});
    await isolated.route('**/demo.json',route=>route.fulfill({body:config,contentType:'application/json'}));
    await isolated.goto(`http://127.0.0.1:${server.address().port}/player/`);
    await isolated.evaluate(async()=>{const {TrackerCanvas}=await import('/player/canvas.js'+new URL(document.querySelector('script').src).search);const draw=TrackerCanvas.prototype.draw;TrackerCanvas.prototype.draw=function(){window.v=this;return draw.call(this);};dispatchEvent(new Event('resize'));});
    await isolated.waitForFunction(()=>window.v?.m.ready);
    assert.equal(await isolated.title(),'ntrk · Module player');
    if(process.env.NTRK_SCREENSHOT)await isolated.screenshot({path:process.env.NTRK_SCREENSHOT+'.startup-mobile.png'});
    await isolated.evaluate(()=>v.action('demo'));await isolated.waitForFunction(()=>v.m.error&&!v.m.busy&&v.hits.some(h=>h.id==='open'));
    assert(await isolated.evaluate(()=>v.hits.some(h=>h.id==='open')));
    await isolated.locator('#open').setInputFiles({name:'arbitrary.ntrk',mimeType:'application/octet-stream',buffer:beforeVolume});
    await isolated.waitForFunction(()=>v.m.name==='arbitrary.ntrk');await isolated.close();
  }
  // Actual AudioWorklet stereo: centered instrument defaults versus explicit
  // 8FF, through browser audio nodes, with all other voices monitor-muted.
  const stereoPeaks=()=>page.evaluate(async()=>{
    const split=testContext.createChannelSplitter(2),analysers=[testContext.createAnalyser(),testContext.createAnalyser()];
    testNode.connect(split);analysers.forEach((a,i)=>{a.fftSize=2048;split.connect(a,i);});
    const peaks=[0,0],buffer=new Float32Array(2048);
    for(let i=0;i<12;++i){await new Promise(r=>setTimeout(r,25));analysers.forEach((a,c)=>{a.getFloatTimeDomainData(buffer);for(const x of buffer)peaks[c]=Math.max(peaks[c],Math.abs(x));});}
    testNode.disconnect(split);split.disconnect();analysers.forEach(a=>a.disconnect());return peaks;
  });
  await page.evaluate(async()=>{
    const snap=await engineCommand('demo');view.m.snapshot=snap;view.m.state=snap.state;view.m.order=view.m.row=view.m.field=view.m.channel=0;
    for(let c=1;c<16;++c)await engineCommand('mute',[c]);
    await testContext.resume();await engineCommand('transport',[1,0,0]);
  });
  const center=await stereoPeaks();assert(center[0]>.001 && center[0]===center[1],`centered worklet ${center}`);
  await page.evaluate(async()=>{
    await engineCommand('transport',[0,0,0]);
    await engineCommand('edit',[0,0,0,2,8]);await engineCommand('edit',[0,0,0,3,255]);
    await engineCommand('transport',[1,0,0]);
  });
  const right=await stereoPeaks();assert(right[1]>.001 && right[0]===0,`explicit hard-right worklet ${right}`);
  console.log('browser worklet stereo:',{center,right});
  const unavailable=await browser.newPage();
  await unavailable.route('**/ntrk.wasm*',route=>route.fulfill({status:404,body:'missing'}));
  await unavailable.goto(`http://127.0.0.1:${server.address().port}/player/`);
  await unavailable.evaluate(async()=>{const {TrackerCanvas}=await import('/player/canvas.js'+new URL(document.querySelector('script').src).search);const draw=TrackerCanvas.prototype.draw;TrackerCanvas.prototype.draw=function(){window.v=this;return draw.call(this);};dispatchEvent(new Event('resize'));});
  await unavailable.waitForFunction(()=>window.v?.m.error&&!v.m.ready);
  assert(await unavailable.evaluate(()=>!v.hits.some(h=>h.id==='open'||h.id==='play')));
  await unavailable.close();
  await page.evaluate(()=>testContext.close());assert.deepEqual(errors,[]);
  console.log(`browser: ${demoName}; authored demo migration; all command lanes/read-only paint/hit geometry; centered/explicit stereo; real worklet peak ${peak.toFixed(3)}; canvas pixels/DPR; pointer fields/mutes/orders/instruments/steppers; keyboard edits/bounds; save/reopen; failed loads; modal help/discard; sparse IDs/FXPL-only/meta-only/empty/stable visibility; wide fixture wheel/scrollbars/pan; desktop/mobile resize/focus; subpath generic startup/bad example recovery; sampled MOD/XM/PCM; burst edits/save/load ordering; actual gain/meter; seek; real touch drag/tap threshold passed`);
} finally {
  if(browser)await browser.close();await new Promise(resolve=>server.close(resolve));
}
