import {Engine} from './engine.js';
class Tracker extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.engine = new Engine(options.processorOptions.module, sampleRate);
    this.port.onmessage = ({data: {id, type, args}}) => {
      try { this.port.postMessage({id, result: this.engine.command(type, args)}); }
      catch (error) { this.port.postMessage({id, error: error.message}); }
    };
  }
  process(inputs, outputs) {
    this.engine.render(outputs[0][0], outputs[0][1]);
    return true;
  }
}
registerProcessor('ntrk', Tracker);
