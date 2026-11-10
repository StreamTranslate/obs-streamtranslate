const {WebSocketServer}=require(process.env.WS_MODULE||'ws');
const assert=require('node:assert/strict');
const wss=new WebSocketServer({port:8797,host:'127.0.0.1'});let count=0;const frames={}, metadata={};
wss.on('connection',(ws,req)=>{
 const id=++count;const u=new URL(req.url,'http://localhost');frames[id]=0;metadata[id]=0;assert.equal(u.searchParams.get('cfgInput'),u.searchParams.get('channels')==='2'?'receiver':'separate');
 if(id===1){assert.equal(u.searchParams.get('speaker'),'1');ws.close(4409,'Streamer is not live');}
 else if(id===2){assert.equal(u.searchParams.get('speaker'),'1');}
 else if(id===3||id===4){assert.equal(u.searchParams.get('channels'),'2');assert.equal(u.searchParams.get('speaker'),null);}
 else if(id===5||id===6){assert.equal(u.searchParams.get('channels'),null);assert.equal(u.searchParams.get('speaker'),null);}
 else throw Error('unexpected retry');
 ws.on('message',(d,b)=>{if(b){frames[id]++;if(id===5&&frames[id]===31)ws.close(4409,'Streamer input changed; reconnect');}else {const m=JSON.parse(d);if(m.type==='obs_source'){assert.equal(m.v,1);assert.ok(m.name);assert.equal(m.canSelect,false);metadata[id]++;}}});
});
setTimeout(()=>{assert.equal(count,6);for(const id of [2,3,4,5,6]){assert.ok(frames[id]>20);assert.ok(metadata[id]>0);}wss.close();console.log('PASS missing-primary automatically retries; stereo, reconnect, mono and server-requested recovery all carry PCM',frames);},20000);
