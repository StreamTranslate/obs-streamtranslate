const {WebSocketServer}=require(process.env.WS_MODULE||'ws');
const assert=require('node:assert/strict');
const wss=new WebSocketServer({port:8797,host:'127.0.0.1'});let count=0;const frames={}, metadata={};
wss.on('connection',(ws,req)=>{
 const id=++count;const u=new URL(req.url,'http://localhost');frames[id]=0;metadata[id]=0;assert.equal(u.searchParams.get('cfgInput'),u.searchParams.get('channels')==='2'?'receiver':'separate');
 if(id===1){assert.equal(u.searchParams.get('speaker'),'1');ws.close(4409,'Streamer is not live');}
 else if(id===2){assert.equal(u.searchParams.get('speaker'),'1');}
 else if([3,4,7,8].includes(id)){assert.equal(u.searchParams.get('channels'),'2');assert.equal(u.searchParams.get('speaker'),null);}
 else if(id===5||id===6){assert.equal(u.searchParams.get('channels'),null);assert.equal(u.searchParams.get('speaker'),null);}
 else throw Error('unexpected retry');
 ws.on('message',(d,b)=>{
  if(b){
   const channels=Number(u.searchParams.get('channels')||1);
   // The harness emits 480 frames per callback. The stereo route must send both channels.
   assert.equal(d.length,480*channels*2);
   frames[id]++;
   if(id===5&&frames[id]===31)ws.close(4409,'Streamer input changed; reconnect');
   if(id===4&&frames[id]===50)ws.close(4001,'stopped_by_owner');
   if(id===7&&frames[id]===45)ws.close(4009,'Another audio source is active');
  }else {
   const m=JSON.parse(d);
   if(m.type==='obs_source'){assert.equal(m.v,1);assert.ok(m.name);assert.equal(m.canSelect,false);metadata[id]++;}
  }
 });
});
setTimeout(()=>{assert.equal(count,8);for(const id of [2,3,4,5,6,7,8]){assert.ok(frames[id]>20);assert.ok(metadata[id]>0);}wss.close();console.log('PASS missing-primary automatically retries; stereo, reconnect, mono and server-requested recovery all carry PCM with explicit Start after Control Stop and conflict',frames);},32000);
