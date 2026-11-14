import {Engine} from './engine.js';
export const sameVersion = (a,b) => a.epoch===b.epoch && a.revision===b.revision;
const counter = n => Number.isSafeInteger(n) && n>=0;
// Only authored state enters this instance. No transport, render or score control.
export class MetadataEngine {
  constructor(module,rate) { this.engine=new Engine(module,rate);this.epoch=0;this.revision=0; }
  command({type,args=[],epoch,revision}) {
    if(!counter(epoch)||!counter(revision))throw new Error('Invalid metadata version');
    if(type==='load'||type==='demo') {
      if(epoch<=this.epoch||revision!==0)throw new Error('Stale metadata load');
      this.engine.command(type,args);this.epoch=epoch;this.revision=0;
      return null;
    }
    if(epoch!==this.epoch||!epoch)throw new Error('Stale metadata song');
    if(type==='edit') {
      if(revision!==this.revision+1)throw new Error('Stale metadata edit');
      this.engine.command(type,args);this.revision=revision;return null;
    }
    if(revision!==this.revision)throw new Error('Stale metadata revision');
    if(type!=='pattern'&&type!=='formatted')throw new Error('Metadata command refused');
    return this.engine.command(type,args);
  }
}
// At most one active and one latest waiting query per kind; one mutation.
// Cancelling settles callers immediately but retains the active slot until its
// reply, so a stalled worker cannot accumulate an unbounded message backlog.
export class MetadataClient {
  constructor(worker,onfailure,timeout=10000) {
    this.worker=worker;this.onfailure=onfailure;this.timeout=timeout;this.serial=0;this.failed=null;
    this.slots=new Map();
    worker.onmessage=({data})=>{
      if(!data)return;
      if(data.type==='closed'){this.fail(new Error('Metadata worker closed'));return;}
      if(!Number.isSafeInteger(data.id)||data.id<1)return;
      const slot=[...this.slots.values()].find(s=>s.active?.id===data.id);
      if(!slot)return;
      const p=slot.active;clearTimeout(p.timer);slot.active=null;
      if(!sameVersion(p.message,data)) {this.fail(new Error('Metadata reply version mismatch'));p.reject(this.failed);return;}
      if(data.error){this.fail(new Error(data.error));p.reject(this.failed);return;}
      p.resolve(data.result);
      if(slot.next){const next=slot.next;slot.next=null;this.send(slot,next);}
    };
    worker.onerror=event=>{event.preventDefault?.();this.fail(new Error(event.message||'Metadata worker failed'));};
    worker.onmessageerror=()=>this.fail(new Error('Metadata message could not be decoded'));
  }
  fail(error) {
    if(this.failed)return;this.failed=error;
    for(const slot of this.slots.values())for(const p of [slot.active,slot.next])if(p){clearTimeout(p.timer);p.reject(error);}
    this.slots.clear();this.worker.terminate();this.onfailure(error);
  }
  close() { this.fail(new Error('Metadata worker closed')); }
  cancelQueries() {
    for(const kind of ['pattern','formatted']){
      const slot=this.slots.get(kind);if(!slot)continue;
      slot.active?.resolve(null);slot.next?.resolve(null);slot.next=null;
    }
  }
  request(type,args,version) {
    if(this.failed)return Promise.reject(this.failed);
    const kind=type==='pattern'||type==='formatted'?type:'mutation';
    let slot=this.slots.get(kind);if(!slot){slot={active:null,next:null};this.slots.set(kind,slot);}
    return new Promise((resolve,reject)=>{
      const p={id:++this.serial,message:{type,args,...version},resolve,reject};
      if(!slot.active)this.send(slot,p);
      else if(kind==='mutation')reject(new Error('Metadata mutation already pending'));
      else {slot.active.resolve(null);slot.next?.resolve(null);slot.next=p;}
    });
  }
  send(slot,p) {
    slot.active=p;p.timer=setTimeout(()=>this.fail(new Error('Metadata worker timed out')),this.timeout);
    try{this.worker.postMessage({id:p.id,...p.message});}catch(error){this.fail(error);}
  }
}
