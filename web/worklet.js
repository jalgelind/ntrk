import {Engine} from './engine.js';
class Tracker extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.engine = new Engine(options.processorOptions.module, sampleRate);
    this.epoch=0;this.revision=0;
    this.port.onmessage = ({data: {id, type, args, epoch, revision}}) => {
      try {
        // Deny before dispatch: formatting must never reach the audio owner.
        if(type==='pattern'||type==='formatted')throw new Error('Formatting belongs to the metadata worker');
        if(['load','demo','edit'].includes(type)&&(epoch!==this.epoch||revision!==this.revision))throw new Error('Stale audio mutation');
        const result=this.engine.command(type,args);
        if(type==='load'||type==='demo'){++this.epoch;this.revision=0;}
        if(type==='edit')++this.revision;
        this.port.postMessage({id,result,epoch:this.epoch,revision:this.revision});
      }
      catch (error) { this.port.postMessage({id, error: error.message}); }
    };
  }
  process(inputs, outputs) {
    this.engine.render(outputs[0][0], outputs[0][1]);
    return true;
  }
}
registerProcessor('ntrk', Tracker);
