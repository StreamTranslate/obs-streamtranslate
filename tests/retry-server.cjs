const {WebSocketServer}=require(process.env.WS_MODULE||'ws');
const assert=require('node:assert/strict');
const wss=new WebSocketServer({port:8796,host:'127.0.0.1'});let count=0,frames=0;const captures=[];
wss.on('connection',(ws,req)=>{captures.push(new URL(req.url,'http://localhost').searchParams.get('captureId'));const id=++count;console.log('connection',id);if(id<=3)setTimeout(()=>ws.close([4008,4403,4010][id-1],'test'),100);else if(id===4)setTimeout(()=>ws.close(1012,'Service restart'),700);ws.on('message',(d,b)=>{if(b&&id===5)frames++});});
setTimeout(()=>{assert.equal(count,5);for(const id of captures)assert.match(id,/^[A-Za-z0-9_-]{1,80}$/);assert.equal(new Set(captures.slice(0,3)).size,3);assert.equal(new Set(captures.slice(2)).size,1);assert.ok(frames>50);wss.close();console.log('PASS 5 connections, resumed PCM',frames);},28000);
