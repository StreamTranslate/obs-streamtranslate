const {WebSocketServer}=require(process.env.WS_MODULE||'ws');
const assert=require('node:assert/strict');
const wss=new WebSocketServer({port:8796,host:'127.0.0.1'});let count=0,frames=0;
wss.on('connection',ws=>{const id=++count;console.log('connection',id);if(id<=3)setTimeout(()=>ws.close([4008,4403,4010][id-1],'test'),100);else if(id===4)setTimeout(()=>ws.close(1012,'Service restart'),700);ws.on('message',(d,b)=>{if(b&&id===5)frames++});});
setTimeout(()=>{assert.equal(count,5);assert.ok(frames>50);wss.close();console.log('PASS 5 connections, resumed PCM',frames);},28000);
