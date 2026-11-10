const {WebSocketServer}=require(process.env.WS_MODULE||'ws');
const assert=require('node:assert/strict');
const wss=new WebSocketServer({port:8798,host:'127.0.0.1'}),seen=new Map();
wss.on('connection',(ws,req)=>{
 const u=new URL(req.url,'http://localhost'),speaker=Number(u.searchParams.get('speaker')||0);
 assert.equal(u.searchParams.get('cfgInput'),'separate');assert.equal(u.searchParams.get('channels'),null);assert.ok(!seen.has(speaker));
 const row={frames:0,energy:0,name:null,id:null};seen.set(speaker,row);
 ws.on('message',(data,binary)=>{
  if(binary){row.frames++;for(let i=0;i<data.length;i+=2)row.energy+=Math.abs(data.readInt16LE(i));}
  else{const m=JSON.parse(data);if(m.type==='obs_source'){row.name=m.name;row.id=m.sourceId;assert.equal(m.canSelect,false);}}
 });
});
setTimeout(()=>{assert.equal(seen.size,2);for(const speaker of [0,1]){const row=seen.get(speaker);assert.ok(row.frames>100);assert.ok(row.energy>0);assert.equal(row.name,speaker?'QA MacBook source':'QA DJI source');assert.ok(row.id);}assert.notEqual(seen.get(0).id,seen.get(1).id);wss.close();console.log('PASS independent Mic 1/Mic 2 transport, actual parent names, distinct source IDs and nonzero PCM');},8500);
