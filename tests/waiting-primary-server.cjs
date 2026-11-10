const {WebSocketServer}=require(process.env.WS_MODULE||'ws');
const assert=require('node:assert/strict');const wss=new WebSocketServer({port:8798,host:'127.0.0.1'});let count=0;
wss.on('connection',ws=>{count++;assert.ok(count<=3);ws.close(count<=2?4409:4403,count<=2?'Streamer is not live':'Invalid plugin key');});
setTimeout(()=>{assert.equal(count,3);wss.close();console.log('PASS bounded retries, Stop, and invalid-key pause');},20000);
