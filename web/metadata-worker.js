import {MetadataEngine} from './metadata.js';
let mirror;
self.onmessage=({data})=>{
  const {id,type,args,epoch,revision}=data;
  try {
    let result;
    if(type==='init') {
      if(mirror)throw new Error('Metadata already initialized');
      mirror=new MetadataEngine(args[0],args[1]);
      result={memoryBytes:mirror.engine.api.memory.buffer.byteLength,rate:mirror.engine.rate};
    } else {
      if(!mirror)throw new Error('Metadata not initialized');
      result=mirror.command(data);
    }
    self.postMessage({id,epoch,revision,result});
  } catch(error){self.postMessage({id,epoch,revision,error:error.message});}
};
self.onmessageerror=()=>{self.postMessage({type:'closed'});self.close();};
