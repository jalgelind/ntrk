// Optional browser harness. Existing Playwright only; no installs or production test hooks.
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.NTRK_PLAYWRIGHT||'playwright');
const root=fileURLToPath(new URL('../out/web/',import.meta.url));
const mime={'.html':'text/html','.js':'text/javascript','.css':'text/css','.wasm':'application/wasm'};
const server=createServer(async(req,res)=>{
  try {
    const name=path.basename(new URL(req.url,'http://localhost').pathname)||'index.html';
    res.writeHead(200,{'Content-Type':mime[path.extname(name)]||'application/octet-stream'}).end(await readFile(path.join(root,name)));
  } catch {res.writeHead(404).end();}
});
let browser;
try {
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  browser=await chromium.launch({headless:true});
  const page=await browser.newPage({acceptDownloads:true,viewport:{width:1280,height:720},deviceScaleFactor:2});
  const errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.addInitScript(()=>{
    const Original=window.AudioWorkletNode;
    window.AudioWorkletNode=class extends Original {
      constructor(...args){super(...args);window.testNode=this;window.testContext=args[0];}
    };
    // Record the actual paint calls, not DOM surrogates for a canvas.
    const text=CanvasRenderingContext2D.prototype.fillText;
    window.painted=[];
    CanvasRenderingContext2D.prototype.fillText=function(s,...args){painted.push(String(s));if(painted.length>12000)painted.splice(0,6000);return text.call(this,s,...args);};
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const demo=JSON.parse(await readFile(path.join(root,'demo.json'),'utf8'));
  const demoName=demo?.name||'Night circuit';
  try {await page.waitForFunction(name=>document.title===`ntrk · ${name}`,demoName);}
  catch(error){console.error('startup',errors,await page.evaluate(()=>painted.slice(-20)));throw error;}
  // Observe the renderer through its module prototype in this harness only.
  await page.evaluate(async()=>{
    const {TrackerCanvas}=await import('./canvas.js'),draw=TrackerCanvas.prototype.draw;
    TrackerCanvas.prototype.draw=function(){window.view=this;return draw.call(this);};
    dispatchEvent(new Event('resize'));
    window.engineCommand=(type,args=[])=>new Promise((resolve,reject)=>{
      const id=1000000+Math.random();
      const listener=({data})=>{if(data.id!==id)return;testNode.port.removeEventListener('message',listener);data.error?reject(new Error(data.error)):resolve(data.result);};
      testNode.port.addEventListener('message',listener);testNode.port.postMessage({id,type,args});
    });
  });
  await page.waitForFunction(()=>window.view?.m.snapshot&&painted.includes('Ready · press Play to enable audio'));
  const model=()=>page.evaluate(()=>({name:view.m.name,state:view.m.state,message:view.m.message,dirty:view.m.dirty,follow:view.m.follow}));
  const wait=fn=>page.waitForFunction(fn);
  const click=async(x,y)=>{await page.evaluate(()=>new Promise(requestAnimationFrame));await page.mouse.click(x,y);assert(await page.evaluate(()=>document.activeElement===document.querySelector('canvas')),'canvas retains keyboard focus');};
  // Toolbar coordinates are independent expectations of the compact bands.
  const toolbar={open:[37,48],save:[107,48],demo:[177,48],help:[247,48],play:[37,81],pause:[107,81],stop:[177,81],edit:[257,81],follow:[354,81]};
  const button=id=>click(...toolbar[id]);
  const cell=async(r,c,f)=>{
    const p=await page.evaluate(([r,c,f])=>{const b=view.cell(r,c,f);return {x:b.x+b.w/2,y:b.y+b.h/2};},[r,c,f]);
    await click(p.x,p.y);
    assert.deepEqual(await page.evaluate(()=>[view.m.row,view.m.channel,view.m.field]),[r,c,f],'pointer maps to intended field');
  };
  const download=async()=>{const pending=page.waitForEvent('download');await button('save');return readFile(await(await pending).path());};
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
  if(demo)assert.deepEqual(initialBytes,await readFile(path.join(root,demo.url)),'authored demo identity');
  const initial=await snapshot();assert.equal(initial.state[0],16);assert.equal(initial.state[1],32);
  const names=Object.values(initial.instruments).map(i=>Buffer.from(Object.values(i.nameBytes)).toString()).filter(Boolean);
  assert(names.length>0);assert(await page.evaluate(names=>names.some(n=>painted.some(t=>t.includes(n))),names),'module-owned instrument names are drawn');
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
  await loadFixture('sparse');
  assert.deepEqual(await visible(),[0,3,11,14]);
  assert.equal((await snapshot()).usedMask,(1<<0)|(1<<3)|(1<<11)|(1<<14));
  assert.deepEqual(await page.evaluate(()=>view.hits.filter(h=>h.id.startsWith('mute')).map(h=>h.id)),['mute0','mute3','mute11','mute14']);
  assert.equal(await value(0,14,0),0,'FXPL-only track has no primary note');
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.sparse.png'});
  assert(await page.evaluate(()=>['CH 01','CH 04','CH 0C','CH 0F'].every(label=>painted.includes(label))),'original IDs drawn compactly');
  await cell(0,3,1);await page.keyboard.type('02');await wait(()=>!view.m.busy&&view.m.row===1);
  assert.equal(await value(0,3,1),2);assert.equal(await value(0,1,1),0,'compact slot never edits its numeric slot index');
  await page.keyboard.press('Tab');assert.equal(await page.evaluate(()=>view.m.channel),11);
  await page.keyboard.press('Tab');assert.equal(await page.evaluate(()=>view.m.channel),14);
  await page.keyboard.press('Tab');assert.equal(await page.evaluate(()=>view.m.channel),0);
  await page.keyboard.press('Shift+Tab');assert.equal(await page.evaluate(()=>view.m.channel),14);
  await cell(0,3,3);await page.keyboard.press('ArrowRight');
  assert.deepEqual(await page.evaluate(()=>[view.m.channel,view.m.field]),[11,0]);
  await page.keyboard.press('ArrowLeft');assert.deepEqual(await page.evaluate(()=>[view.m.channel,view.m.field]),[3,3]);
  const sparseMute=await page.evaluate(()=>({x:view.cell(0,11,0).x+35,y:view.grid.y-14}));
  await click(sparseMute.x,sparseMute.y);await wait(()=>view.m.state[11]===(1<<11));
  assert.equal((await page.evaluate(()=>engineCommand('state')))[11],1<<11);
  // Clearing a track's only cell must not make the cursor jump or strand it.
  await cell(0,0,0);await page.keyboard.press('Delete');await wait(()=>!view.m.busy&&view.m.row===1);
  assert.deepEqual(await visible(),[0,3,11,14]);assert.equal(await page.evaluate(()=>view.m.channel),0);
  const sparseSaved=await download();await loadBytes('sparse-cleared.ntrk',sparseSaved);
  assert.deepEqual(await visible(),[3,11,14]);assert.equal(await page.evaluate(()=>view.m.channel),3,'new load clamps cursor to first visible original ID');
  assert.equal(await value(0,3,1),2);assert.equal((await model()).state[0],16);
  await page.keyboard.press('Tab');assert.equal(await page.evaluate(()=>view.m.channel),11);
  await page.keyboard.press('Shift+Tab');assert.equal(await page.evaluate(()=>view.m.channel),3);
  assert.deepEqual(await download(),sparseSaved,'save keeps original channel count, FXPL and unreferenced patterns');
  await loadFixture('empty');assert.deepEqual(await visible(),[0]);assert.equal((await snapshot()).usedMask,0);
  assert.equal((await model()).state[0],16,'empty module keeps its actual channel count');
  const emptyBlank=await page.evaluate(()=>({x:view.raster.x+view.channelW+30,y:view.grid.y+10}));
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
  await click(54,114);await wait(()=>view.m.follow);await click(54,114);await wait(()=>!view.m.follow);
  await button('demo');await wait(()=>!view.m.name.endsWith('.ntrk'));
  await page.evaluate(()=>{view.sx=view.sy=0;view.invalidate();});await cell(0,0,0);
  assert.deepEqual(await visible(),demo?[0,1,2,3]:[0,1,2]);
  if(process.env.NTRK_SCREENSHOT)await page.screenshot({path:process.env.NTRK_SCREENSHOT+'.mobile.png'});
  if(demo) {
    await page.route(`**/${demo.url}`,route=>route.fulfill({status:404,body:'missing'}));
    await button('demo');await wait(()=>view.m.message.includes('Cannot load configured demo'));assert.equal((await model()).name,demoName);
    const failed=await browser.newPage();await failed.route(`**/${demo.url}`,route=>route.fulfill({status:404,body:'missing'}));
    await failed.goto(`http://127.0.0.1:${server.address().port}/`);
    await failed.waitForTimeout(700);
    assert.equal(await failed.title(),'ntrk · Loading demo');
    const result=await failed.evaluate(async()=>{const {TrackerCanvas}=await import('./canvas.js');const original=TrackerCanvas.prototype.draw;TrackerCanvas.prototype.draw=function(){window.failedView=this;return original.call(this);};dispatchEvent(new Event('resize'));});
    await failed.waitForFunction(()=>window.failedView?.m.message.includes('Cannot load configured demo'));
    assert(await failed.evaluate(()=>!failedView.hits.some(h=>h.id==='play')));await failed.close();
  }
  await page.evaluate(()=>testContext.close());assert.deepEqual(errors,[]);
  console.log(`browser: ${demoName}; exact demo bytes; real worklet peak ${peak.toFixed(3)}; canvas pixels/DPR; pointer fields/mutes/orders/instruments/steppers; keyboard edits/bounds; save/reopen; failed loads; modal help/discard; sparse IDs/FXPL-only/meta-only/empty/stable visibility; wide fixture wheel/scrollbars/pan; desktop/mobile resize/focus passed`);
} finally {
  if(browser)await browser.close();await new Promise(resolve=>server.close(resolve));
}
