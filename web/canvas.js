// Fixed-pitch raster and compact bands follow js-seq's tracker; palette informed
// by no2's TrackerTheme. All geometry below is shared by painting and input.
export const hex = n => n.toString(16).toUpperCase().padStart(2, '0');
const pitches = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-'];
const noteName = n => n === 97 ? '^^^' : n ? pitches[(n - 1) % 12] + (Math.floor((n - 1) / 12) + 1) : '...';
const ink = {bg:'#100f1b', panel:'#191728', line:'#393650', text:'#ebebfa', dim:'#918ba9',
  faint:'#57516e', accent:'#96f0be', warn:'#ff9682', beat:'#222038', bar:'#2e2c4a',
  play:'#285d46', cursor:'#a096e6', select:'#484060', ins:'#b9a9ee', fx:'#edc18b', param:'#93becf'};
const rect = (x,y,w,h) => ({x,y,w:Math.max(0,w),h:Math.max(0,h)});
const contains = (r,x,y) => r && x >= r.x && y >= r.y && x < r.x+r.w && y < r.y+r.h;
const clamp = (v,max) => Math.max(0,Math.min(v,max));
const help = [
  ['TRANSPORT & FILES', 'Space plays / pauses. F5 plays; F8 stops and rewinds. Open reads a local .ntrk, .mod or .xm file (16 MiB maximum). Save downloads the complete song. Example opens the optional configured song or a generated example. Output changes host volume; the bar measures actual output peaks.'],
  ['PATTERN EDITING', 'Click a field; arrows move the cursor. Page Up / Down move 16 rows. Home / End jump to the first / last row. Tab leaves the canvas. Enable Edit to write. Successful entries advance one row.'],
  ['PIANO KEYS', 'Z S X D C V G B H N J M = one octave. Q 2 W 3 E R 5 T 6 Y 7 U = the next. Octave and instrument controls set note entry. Enter writes note-off. Delete / Backspace clears a field. Notes are not auditioned while stopped.'],
  ['HEX FIELDS', 'Instrument, effect and parameter take two hexadecimal digits. Values outside the module range are refused. Only the four primary cell fields are editable here.'],
  ['NAVIGATION', 'Order rows browse a pattern and seek playback, disabling Follow. Double-click a cell to seek its row; selection alone browses. Follow tracks the playing row. Click a channel header to mute monitoring (not saved). Wheel or drag the scrollbars to scroll; Shift + wheel scrolls channels. Touch-drag the pattern to pan. Lists scroll separately. Only tracks with primary cells or channel automation are shown, keeping their original numbers. The track list stays fixed until another file or demo is loaded. An empty song keeps track 01 editable.'],
  ['INSTRUMENTS', 'Select a module instrument in the list to use it for note entry. Names, volume and pan are read from the module. Unnamed slots are labeled explicitly. All secondary effect columns and meta lanes are displayed read-only; parameters and mixer settings are preserved.'],
  ['LIMITATIONS', 'No undo, instrument creation, structural song editing or browser storage. Large file operations can interrupt audio: pause first. A hardware keyboard is needed for note entry on touch screens. F1 or Escape closes this help.'],
];
// Character positions are the sole geometry for draw, hit, reveal and keyboard.
export function fieldLayout(snapshot) {
  if (!snapshot) return [];
  const columns=[]; let offset=0;
  for (const c of snapshot.channels) {
    for (const [f,width] of [4,3,4,3].entries()) {
      columns.push({channel:c,field:f,offset,width,readOnly:false}); offset+=width;
    }
    for (let lane=0;lane<snapshot.state[12];lane++) {
      columns.push({channel:c,field:4+lane,lane,offset,width:7,readOnly:true});offset+=7;
    }
    offset++;
  }
  for (let lane=0;lane<snapshot.state[13];lane++) {
    columns.push({channel:-1,field:lane,lane,offset,width:7,readOnly:true});offset+=7;
  }
  return columns;
}
export function commandCell(snapshot, pattern, row, channel, field) {
  const s=snapshot.state,base=channel<0?s[0]*(1+s[12])+field:channel*(1+s[12])+(field<4?0:field-3);
  return snapshot.commands[pattern*s[1]+row][base];
}
export class TrackerCanvas {
  constructor(canvas, model, action) {
    this.canvas=canvas; this.g=canvas.getContext('2d'); this.m=model; this.action=action;
    this.sx=0; this.sy=0; this.orderScroll=0; this.insScroll=0; this.helpScroll=0;
    this.hits=[]; this.invalid=true; this.hover='';
    canvas.addEventListener('pointerdown', e => this.down(e));
    canvas.addEventListener('pointermove', e => this.move(e));
    canvas.addEventListener('pointerup', e => this.up(e));
    canvas.addEventListener('pointercancel', () => { this.drag=null; });
    canvas.addEventListener('wheel', e => this.wheel(e), {passive:false});
    canvas.addEventListener('focus', () => this.invalidate());
    canvas.addEventListener('blur', () => this.invalidate());
    window.addEventListener('resize', () => this.invalidate());
    const frame = () => {
      const w=canvas.clientWidth,h=canvas.clientHeight,dpr=window.devicePixelRatio || 1;
      if(w!==this.w || h!==this.h || dpr!==this.dpr) {
        this.w=w;this.h=h;this.dpr=dpr;canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);
        this.g.setTransform(dpr,0,0,dpr,0,0);this.invalid=true;
      }
      if(this.invalid) { this.invalid=false;this.draw(); }
      this.raf=requestAnimationFrame(frame);
    };
    frame();
  }
  invalidate() { this.invalid=true; }
  fill(r,c) { this.g.fillStyle=c;this.g.fillRect(r.x,r.y,r.w,r.h); }
  edge(r,c) { this.g.strokeStyle=c;this.g.lineWidth=1;this.g.strokeRect(r.x+.5,r.y+.5,Math.max(0,r.w-1),Math.max(0,r.h-1)); }
  text(s,x,y,c=ink.text,size=12) {
    this.g.font=`${size}px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace`;
    this.g.textBaseline='middle';this.g.fillStyle=c;this.g.fillText(String(s),Math.round(x),Math.round(y));
  }
  clip(r,fn) { this.g.save();this.g.beginPath();this.g.rect(r.x,r.y,r.w,r.h);this.g.clip();fn();this.g.restore(); }
  label(s,r,c=ink.text,size=12) { this.clip(r,()=>this.text(s,r.x+7,r.y+r.h/2,c,size)); }
  hit(id,r,fn,enabled=true,tip='') { if(enabled) this.hits.push({id,r,fn,tip}); }
  button(id,s,r,fn,on=false,enabled=true,tip='') {
    this.fill(r,on?ink.select:ink.panel);this.edge(r,on?ink.accent: this.hover===id?ink.dim:ink.line);
    this.label(s,r,enabled?(on?ink.accent:ink.text):ink.faint);
    this.hit(id,r,fn,enabled,tip);
  }
  // One field geometry, including its trailing gap, for both paint and selection.
  cell(r,c,f) {
    const col=this.columns.find(col=>col.channel===c&&col.field===f);
    return rect(this.grid.x+this.gutter+(col?.offset||0)*this.advance-this.sx,
      this.grid.y+r*this.pitch-this.sy,(col?.width||0)*this.advance,this.pitch);
  }

  setEntry(id,value) {
    this.action('entry',{id,value});
  }
  limits() {
    const s=this.m.state;
    this.maxX=Math.max(0,(this.columns.at(-1)?.offset+this.columns.at(-1)?.width||0)*this.advance-this.raster.w);
    this.maxY=Math.max(0,(s?.[1]||0)*this.pitch-this.grid.h);
    this.sx=clamp(this.sx,this.maxX);this.sy=clamp(this.sy,this.maxY);
  }
  reveal(r=this.m.row,c=this.m.channel,f=this.m.field) {
    if(!this.grid) return;
    const box=this.cell(r,c,f);
    if(box.y<this.grid.y) this.sy+=box.y-this.grid.y;
    if(box.y+box.h>this.grid.y+this.grid.h) this.sy+=box.y+box.h-this.grid.y-this.grid.h;
    if(box.x<this.raster.x) this.sx+=box.x-this.raster.x;
    if(box.x+box.w>this.raster.x+this.raster.w) this.sx+=box.x+box.w-this.raster.x-this.raster.w;
    this.limits();this.invalidate();
  }
  draw() {
    const m=this.m,s=m.state,w=this.w,h=this.h,ready=!!m.snapshot;
    this.hits=[];this.fill(rect(0,0,w,h),ink.bg);
    this.g.font='13px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
    this.columns=fieldLayout(m.snapshot);
    this.advance=this.g.measureText('0').width;this.pitch=20;this.channelW=Math.ceil((15+7*(s?.[12]||0))*this.advance);this.gutter=34;
    const narrow=w<700;
    this.label('ntrk',rect(3,3,55,28),ink.accent,16);
    this.label(`${m.name}${m.dirty?'  • UNSAVED':''}`,rect(62,3,w-65,28),m.dirty?ink.fx:ink.text);
    let x=5,y=35;
    const buttons=[['open','Open',()=>this.action('open')],['save','Save',()=>this.action('save')],
      ['demo','Example',()=>this.action('demo')],['help','? Help',()=>{this.action('entry',{id:'help',toggle:true});this.helpScroll=0;}]];
    for(const [id,label,fn] of buttons) { this.button(id,label,rect(x,y,id==='demo'?75:65,26),fn,id==='help'&&m.help,id==='help'||(id==='open'||id==='demo'?m.ready:ready));x+=id==='demo'?80:70; }
    if(!narrow) this.label(s?`${m.snapshot.channels.length} tracks · ${s[1]} rows · ${s[4]} instruments`:'PATTERN EDITOR',rect(x+5,y,w-x-10,26),ink.dim,11);
    y+=32;x=5;
    for(const [id,label,cmd] of [['play','Play',1],['pause','Pause',2],['stop','Stop',0]]) {
      this.button(id,label,rect(x,y,65,28),()=>this.action('transport',cmd),id==='play'&&!!s?.[8],ready);x+=70;
    }
    this.button('edit',m.edit?'Edit ON':'Edit OFF',rect(x,y,85,28),()=>this.action('entry',{id:'edit',toggle:true}),m.edit,ready,'Enable keyboard writing');x+=90;
    if(narrow) {y+=33;x=5;}
    this.button('follow',m.follow?'Follow ON':'Follow OFF',rect(x,y,99,28),()=>this.action('entry',{id:'follow',toggle:true}),m.follow,ready);x+=104;
    this.label(s?`${s[8]?'PLAY':'IDLE'} ${hex(s[6])}:${hex(s[7])}  ${s[9]} BPM / ${s[10]} TICKS`:(m.ready?'ENGINE READY':'ENGINE LOADING'),rect(x,y,w-x-5,28),ink.accent,11);
    y+=34;x=5;
    for(const [id,label,value,max] of [['octave','Oct',m.octave,8],['instrument','Ins',m.instrument,s?.[4]||0]]) {
      this.label(`${label} ${id==='instrument'?hex(value):value}`,rect(x,y,55,24),ink.dim);
      this.button(id+'-','−',rect(x+56,y,27,24),()=>this.action('step',{id,delta:-1,min:id==='octave'?1:0,max}),false,ready);
      this.button(id+'+','+',rect(x+85,y,27,24),()=>this.action('step',{id,delta:1,min:id==='octave'?1:0,max}),false,ready);x+=120;
    }
    this.label(`P${ready?hex(m.snapshot.orders[m.order]):'--'}  ${hex(m.row)} / ${hex(m.channel+1)}  ${m.channel<0?'META '+(m.field+1)+' · READ ONLY':m.field>=4?'FX '+(m.field-3)+' · READ ONLY':['NOTE','INSTRUMENT','EFFECT','PARAMETER'][m.field]}`,rect(x,y,w-x-5,24),ink.dim,11);
    y+=29;
    this.label(`OUTPUT ${Math.round(m.volume*100)}%`,rect(5,y,98,23),ink.dim,11);
    this.button('volume-','−',rect(105,y,27,23),()=>this.action('volume',m.volume-.1),false,m.ready);
    this.button('volume+','+',rect(137,y,27,23),()=>this.action('volume',m.volume+.1),false,m.ready);
    const meter=rect(174,y+7,Math.min(150,w-184),9);this.fill(meter,ink.line);
    this.fill(rect(meter.x,meter.y,meter.w*Math.min(1,m.level),meter.h),m.level>1?ink.warn:ink.accent);
    this.label('LEVEL',rect(330,y,w-335,23),ink.dim,10);
    y+=29;
    // Narrow windows keep both lists in one rail; the pattern never changes pitch.
    const left=narrow?121:130,right=narrow?0:218,bottom=h-44;
    const bodyH=Math.max(0,bottom-y),orderH=Math.floor(bodyH*(narrow?.43:1));
    this.orders=rect(4,y+25,left-8,Math.max(0,orderH-29));
    const insX=narrow?4:w-right+4,insY=narrow?y+orderH:y,insW=(narrow?left:right)-8;
    this.instruments=rect(insX,insY+25,insW,Math.max(0,bottom-insY-(narrow?103:180)));
    this.fill(rect(0,y,left,bodyH),ink.panel);this.label('ORDER : PAT',rect(0,y,left,23),ink.dim,11);
    this.fill(rect(insX-4,insY,insW+8,bottom-insY),ink.panel);
    this.label(narrow?'INSTRUMENTS':'INSTRUMENT / NAME',rect(insX,insY,insW,23),ink.dim,11);
    this.grid=rect(left,y+28,Math.max(0,w-left-right-12),Math.max(0,bodyH-40));
    this.raster=rect(this.grid.x+this.gutter,this.grid.y,Math.max(0,this.grid.w-this.gutter),this.grid.h);
    this.limits();
    if(ready) {
      this.list(this.orders,m.snapshot.orders,this.orderScroll,(p,i)=>`${hex(i)} : ${hex(p)}`,m.order,'order',i=>this.action('order',i));
      this.list(this.instruments,m.snapshot.instruments,this.insScroll,(ins,i)=>`${hex(i+1)} ${ins.name||'(unnamed)'}`,m.instrument-1,'ins',i=>this.setEntry('instrument',i+1));
      const info=m.snapshot.instruments[m.instrument-1],infoY=this.instruments.y+this.instruments.h+6;
      this.label(`INS ${hex(m.instrument)}`,rect(insX,infoY,insW,22),ink.accent);
      if(info) {
        this.label(info.name||'(unnamed)',rect(insX,infoY+22,insW,22),ink.text,11);
        this.label(`Pan ${hex(info.pan)} · Vol ${info.volume}`,rect(insX,infoY+44,insW,22),ink.dim,narrow?10:12);
      }
      if(!narrow) {
        this.label('Inspection only',rect(insX,infoY+71,insW,22),ink.dim,11);
        this.label('Parameters / extra FX:',rect(insX,infoY+94,insW,20),ink.dim,11);
        this.label('preserved, not editable',rect(insX,infoY+114,insW,20),ink.dim,11);
      }
      this.pattern();
    }
    if(!ready) {
      this.fill(rect(0,y,w,bodyH),ink.bg);
      const launch=rect(20,y+35,w-40,Math.max(0,bottom-y-50));
      this.fill(launch,ink.bg);
      this.label('MODULE PLAYER',rect(launch.x,launch.y,launch.w,40),ink.accent,20);
      this.label(!m.ready&&m.error?'Audio engine unavailable':m.ready?'Open or drop a module':'Loading audio engine…',rect(launch.x,launch.y+48,launch.w,32),ink.text,15);
      this.label('.ntrk · MOD · XM / up to 16 MiB',rect(launch.x,launch.y+88,launch.w,28),ink.dim,11);
      this.label('Local files stay in your browser.',rect(launch.x,launch.y+120,launch.w,28),ink.dim,11);
      this.label('Example is optional. Play enables audio.',rect(launch.x,launch.y+152,launch.w,28),ink.dim,11);
    }
    this.fill(rect(0,h-43,w,43),ink.panel);
    this.label(m.message,rect(0,h-43,w,22),m.error?ink.warn:ink.accent,11);
    this.label((ready?commandCell(m.snapshot,m.snapshot.orders[m.order],m.row,m.channel,m.field)?.description:'') || this.hits.find(hit=>hit.id===this.hover)?.tip || 'Space play/pause · Arrows move · F1 help · Shift+wheel channels',rect(0,h-21,w,21),ink.dim,10);
    if(document.activeElement===this.canvas) this.edge(rect(0,0,w,h),ink.line);
    if(m.help || m.confirm) this.overlay();
  }
  list(r,items,scroll,label,selected,prefix,fn) {
    const max=Math.max(0,items.length*this.pitch-r.h);
    scroll=clamp(scroll,max);
    if(prefix==='order') this.orderScroll=scroll;else this.insScroll=scroll;
    this.clip(r,()=>{
      for(let i=Math.floor(scroll/this.pitch);i<Math.min(items.length,Math.ceil((scroll+r.h)/this.pitch));i++) {
        const box=rect(r.x,r.y+i*this.pitch-scroll,r.w-5,this.pitch);
        if(i===selected) this.fill(box,ink.select);
        this.label(label(items[i],i),box,i===selected?ink.accent:ink.dim,11);
        const hit=rect(box.x,Math.max(r.y,box.y),box.w,Math.min(box.y+box.h,r.y+r.h)-Math.max(r.y,box.y));
        this.hit(prefix+i,hit,()=>fn(i),true,label(items[i],i));
      }
    });
    if(max>0) {
      const thumbH=Math.max(14,r.h*r.h/(max+r.h));
      this.fill(rect(r.x+r.w-3,r.y+(r.h-thumbH)*scroll/max,2,thumbH),ink.faint);
    }
  }
  pattern() {
    const m=this.m,s=m.state,g=this.grid;
    const firstRow=Math.floor(this.sy/this.pitch),lastRow=Math.min(s[1],Math.ceil((this.sy+g.h)/this.pitch));
    const visible=this.columns.filter(col=>col.offset*this.advance+col.width*this.advance>this.sx&&col.offset*this.advance<this.sx+this.raster.w);
    this.clip(g,()=>{
      for(let r=firstRow;r<lastRow;r++) {
        const y=g.y+r*this.pitch-this.sy;
        if(r%4===0) this.fill(rect(g.x,y,g.w,this.pitch),r%16===0?ink.bar:ink.beat);
        if(m.order===s[6]&&r===s[7]&&s[8]) this.fill(rect(g.x,y,g.w,this.pitch),ink.play);
        if(r===m.row) this.fill(rect(g.x,y,g.w,this.pitch),ink.select+'70');
        this.text(hex(r),g.x+6,y+this.pitch/2,r===m.row?ink.text:ink.dim,11);
      }
    });
    this.clip(this.raster,()=>{
      for(const col of visible) {
        const c=col.channel,f=col.field,muted=c>=0&&!!(s[11]&(1<<c));
        for(let r=firstRow;r<lastRow;r++) {
          const box=this.cell(r,c,f),selected=r===m.row&&c===m.channel&&f===m.field;
          if(selected) {this.fill(box,m.edit&&!col.readOnly?ink.cursor:ink.select);this.edge(box,col.readOnly?ink.fx:ink.cursor);}
          const cmd=commandCell(m.snapshot,m.snapshot.orders[m.order],r,c,f);
          let text,color;
          if(col.readOnly) {text=cmd.text;color=text==='... ..'?ink.faint:ink.fx;}
          else {
            const at=((m.snapshot.orders[m.order]*s[1]+r)*s[0]+c)*4;
            const v=m.snapshot.cells[at+f];
            text=f===0?noteName(v):f===2?cmd.text:f===3?(m.snapshot.cells[at+2]||v?hex(v):'..'):v?hex(v):'..';
            color=f===2?(text==='...'?ink.faint:ink.fx):!v?ink.faint:[ink.text,ink.ins,ink.fx,ink.param][f];
          }
          this.text(text,box.x+2,box.y+box.h/2,selected&&m.edit&&!col.readOnly?ink.bg:muted?ink.faint:color,13);
        }
        if(f===0) this.fill(rect(this.cell(0,c,f).x-3,g.y,1,g.h),ink.line);
      }
    });
    const header=rect(this.raster.x,g.y-28,this.raster.w,27);
    this.fill(rect(g.x,g.y-28,g.w,27),ink.panel);this.label('ROW',rect(g.x,g.y-28,this.gutter,27),ink.dim,10);
    this.clip(header,()=>{
      for(const col of visible) {
        const c=col.channel,f=col.field,x=this.cell(0,c,f).x;
        if(c>=0&&f<4&&f!==0)continue;
        const b=rect(x,g.y-27,(c>=0&&f===0?14:col.width)*this.advance-2,25);
        const muted=c>=0&&!!(s[11]&(1<<c));
        this.fill(b,muted?'#3a292b':ink.panel);
        this.label(c<0?`META ${f+1}`:f>=4?`FX ${f-3}`:`CH ${hex(c+1)}${muted?' MUTE':''}`,b,muted?ink.warn:ink.dim,10);
        if(c>=0&&f===0)this.hit('mute'+c,rect(Math.max(x,header.x),b.y,Math.min(x+b.w,header.x+header.w)-Math.max(x,header.x),b.h),()=>this.action('mute',c),true,'Toggle monitoring mute');
      }
    });
    this.fill(rect(this.raster.x-1,g.y,1,g.h),ink.line);
    this.scrollbar('x',rect(this.raster.x,g.y+g.h+1,this.raster.w,10),this.maxX,this.sx,this.raster.w);
    this.scrollbar('y',rect(g.x+g.w+1,g.y,10,g.h),this.maxY,this.sy,g.h);
  }
  scrollbar(axis,r,max,pos,view) {
    this.fill(r,ink.panel);const length=axis==='x'?r.w:r.h;
    const size=max?Math.max(18,length*view/(max+view)):length;
    const offset=max?(length-size)*pos/max:0;
    const thumb=axis==='x'?rect(r.x+offset,r.y,size,r.h):rect(r.x,r.y+offset,r.w,size);
    this.fill(thumb,max?ink.faint:ink.line);
    this[axis+'bar']={r,thumb,max,length,size};
    this.hit(axis+'scroll',r,()=>{},max>0,'Drag to scroll');
  }
  overlay() {
    const m=this.m;this.fill(rect(0,0,this.w,this.h),'#080710dc');this.hits=[];
    const box=rect(10,10,Math.min(660,this.w-20),Math.max(0,this.h-20));box.x=Math.floor((this.w-box.w)/2);
    this.fill(box,ink.panel);this.edge(box,ink.cursor);
    this.label(m.confirm?'DISCARD UNSAVED EDITS?':'KEYBOARD & FILE HELP',rect(box.x+8,box.y+5,box.w-100,32),ink.accent);
    this.button('close','Close',rect(box.x+box.w-78,box.y+8,68,26),()=>{m.help=false;m.confirm=null;this.invalidate();});
    if(m.confirm) {
      this.label('The current song has unsaved changes.',rect(box.x+8,box.y+52,box.w-16,32),ink.warn,11);
      this.button('discard','Discard',rect(box.x+12,box.y+98,90,30),()=>{const fn=m.confirm;m.confirm=null;this.invalidate();fn();});
      this.button('cancel','Cancel',rect(box.x+112,box.y+98,80,30),()=>{m.confirm=null;this.invalidate();});return;
    }
    const content=rect(box.x+12,box.y+48,box.w-24,box.h-63),lines=[];
    this.g.font='12px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
    for(const [title,body] of help) {
      lines.push([title,ink.accent]);let line='';
      for(const word of body.split(' ')) { if(line&&this.g.measureText(line+' '+word).width>content.w-12) {lines.push([line,ink.text]);line='';}line+=(line?' ':'')+word; }
      lines.push([line,ink.text],['',ink.dim]);
    }
    this.helpMax=Math.max(0,lines.length*20-content.h);this.helpScroll=clamp(this.helpScroll,this.helpMax);
    this.clip(content,()=>lines.forEach(([s,c],i)=>this.text(s,content.x,content.y+i*20+10-this.helpScroll,c)));
    if(this.helpMax) this.label('Scroll for more',rect(box.x+8,box.y+box.h-20,box.w-16,18),ink.dim,10);
  }
  at(x,y) { return [...this.hits].reverse().find(h=>contains(h.r,x,y)); }
  point(e) {const b=this.canvas.getBoundingClientRect();return {x:e.clientX-b.left,y:e.clientY-b.top};}
  down(e) {
    if(e.button!==0) return;e.preventDefault();this.canvas.focus({preventScroll:true});
    const p=this.point(e),hit=this.at(p.x,p.y);
    this.drag={...p,startX:p.x,startY:p.y,hit,sx:this.sx,sy:this.sy,orderScroll:this.orderScroll,insScroll:this.insScroll,moved:false};
    if(hit?.id==='xscroll'||hit?.id==='yscroll') {
      this.drag.axis=hit.id[0];this.scrollTo(p,this.drag.axis);
      this.drag.sx=this.sx;this.drag.sy=this.sy;
    }
    this.canvas.setPointerCapture(e.pointerId);
  }
  scrollTo(p,axis) {
    const bar=this[axis+'bar'],coord=axis==='x'?p.x:p.y,start=axis==='x'?bar.r.x:bar.r.y;
    if(!contains(bar.thumb,p.x,p.y)) this['s'+axis]=clamp((coord-start-bar.size/2)/(bar.length-bar.size)*bar.max,bar.max);
    this.invalidate();
  }
  move(e) {
    const p=this.point(e),hit=this.at(p.x,p.y);this.hover=hit?.id||'';this.canvas.style.cursor=hit?'pointer':'default';
    if(this.drag) {
      const d=this.drag,dx=p.x-d.startX,dy=p.y-d.startY;
      if(Math.abs(dx)+Math.abs(dy)>5)d.moved=true;
      if(d.axis) {const b=this[d.axis+'bar'];this['s'+d.axis]=clamp(d['s'+d.axis]+(d.axis==='x'?dx:dy)*b.max/(b.length-b.size),b.max);}
      else if(d.moved) {
        if(!this.m.help&&!this.m.confirm&&contains(this.orders,d.startX,d.startY))this.orderScroll=d.orderScroll-dy;
        else if(!this.m.help&&!this.m.confirm&&contains(this.instruments,d.startX,d.startY))this.insScroll=d.insScroll-dy;
        else if(d.hit)return;
        if(this.m.help)this.helpScroll=clamp(this.helpScroll+p.y*-1+d.y,this.helpMax||0);
        else if(contains(this.grid,d.startX,d.startY)) {this.sx=clamp(d.sx-dx,this.maxX);this.sy=clamp(d.sy-dy,this.maxY);}
        d.y=p.y;
      }
    }
    this.invalidate();
  }
  up(e) {
    const d=this.drag;this.drag=null;if(!d)return;
    if(this.canvas.hasPointerCapture(e.pointerId))this.canvas.releasePointerCapture(e.pointerId);
    if(d.moved||d.axis)return;
    const p=this.point(e),hit=this.at(p.x,p.y);
    if(hit&&hit.id===d.hit?.id)hit.fn();
    else if(!this.m.help&&!this.m.confirm&&this.m.snapshot&&contains(this.raster,p.x,p.y)) {
      const col=this.columns.find(col=>contains(this.cell(0,col.channel,col.field),p.x,this.cell(0,col.channel,col.field).y+1));
      const r=Math.floor((p.y-this.grid.y+this.sy)/this.pitch);
      if(!col||r>=this.m.state[1])return;
      const c=col.channel,f=col.field;
      this.action('select',{row:r,channel:c,field:f,follow:false});
      const now=performance.now();
      if(this.lastCell?.r===r&&this.lastCell?.c===c&&now-this.lastCell.time<350)this.action('seek',r);
      this.lastCell={r,c,time:now};
    }
  }
  wheel(e) {
    e.preventDefault();const p=this.point(e),scale=e.deltaMode===1?20:e.deltaMode===2?this.grid.h:1;
    const dy=e.deltaY*scale,dx=e.deltaX*scale;
    if(this.m.help)this.helpScroll=clamp(this.helpScroll+dy,this.helpMax||0);
    else if(this.m.confirm)return;
    else if(contains(this.orders,p.x,p.y))this.orderScroll+=dy;
    else if(contains(this.instruments,p.x,p.y))this.insScroll+=dy;
    else {this.sx=clamp(this.sx+(e.shiftKey?dy:dx),this.maxX);this.sy=clamp(this.sy+(e.shiftKey?0:dy),this.maxY);}
    this.invalidate();
  }
}
