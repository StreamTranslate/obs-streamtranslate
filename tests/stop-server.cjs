'use strict';
const root=process.env.ST_SERVER_ROOT;if(!root)throw Error('Set ST_SERVER_ROOT to the StreamTranslate server checkout');
const {WebSocketServer}=require(require('node:path').join(root,'node_modules/ws')),assert=require('node:assert/strict');
const {attachPluginCaptureControl}=require(root+'/lib/plugin-capture-control');
const {createSessionDefender}=require(root+'/lib/session-defender');
const {classifyAudioClose}=require(root+'/lib/operational-utils');
const wss=new WebSocketServer({port:8796,host:'127.0.0.1'}),connections=[];let repairs=0;
wss.on('listening',()=>console.log('READY'));
wss.on('connection',(ws,req)=>{
 const captureId=new URL(req.url,'http://localhost').searchParams.get('captureId');assert.match(captureId,/^[A-Za-z0-9_-]{1,80}$/);
 const c={captureId,frames:0,stops:0,code:null,evidence:null};connections.push(c);
 const d=createSessionDefender({snapshot:()=>({connected:true,live:true,fingerprint:'fixture',connectionId:'c',voiceRecent:true,audioRecent:true,providerReady:false}),recover:()=>{repairs++;return true}});d.assign('r','fixture');
 attachPluginCaptureControl(ws,{isCurrentOwner:()=>true,onStop:e=>{c.stops++;c.evidence=e;d.terminated('r',{classification:'owner_stop_requested'})}});
 ws.on('message',(data,binary)=>{if(binary){assert.equal(c.stops,0,'PCM after explicit Stop');c.frames++}});
 ws.on('close',code=>{c.code=code;c.classification=classifyAudioClose(code,{owner_stop_requested:ws._stOwnerStopRequested});d.tick();console.log(JSON.stringify(c));});
});
setTimeout(()=>{
 try{assert.equal(connections.length,2);assert.notEqual(connections[0].captureId,connections[1].captureId);for(const c of connections){assert.ok(c.frames>30);assert.equal(c.stops,1);assert.equal(c.code,1000);assert.equal(c.classification,'owner_stop_requested');assert.equal(c.evidence.client_reported,true)}assert.equal(repairs,0);console.log('PASS actual native client + server control + Defender: 2 explicit Stops, no unintended repair')}
 catch(e){console.error(e);process.exitCode=1}
 finally{for(const ws of wss.clients)ws.terminate();wss.close()}
},15500);
