"use strict";
const assert=require("node:assert/strict"),fs=require("node:fs"),cp=require("node:child_process");
const {WebSocketServer}=require("ws");
const [binary,output,mode="active"]=process.argv.slice(2);
assert(binary&&output);assert(["active","pending","disconnected"].includes(mode));
const options=process.env.ST_TEST_TLS_CERT?{key:fs.readFileSync(process.env.ST_TEST_TLS_KEY),cert:fs.readFileSync(process.env.ST_TEST_TLS_CERT)}:null;
const server=options?require("node:https").createServer(options):require("node:http").createServer();
const wss=new WebSocketServer({noServer:true});
const report={mode,tls:!!options,connections:0,frames:0,captureStops:0,closed:0,errors:[],providerTested:false};
const pending=new Set();
server.on("connection",socket=>{pending.add(socket);socket.on("close",()=>pending.delete(socket));});
server.on("upgrade",(req,socket,head)=>{if(mode==="pending")return;wss.handleUpgrade(req,socket,head,ws=>wss.emit("connection",ws,req));});
wss.on("connection",socket=>{report.connections++;socket.on("error",e=>report.errors.push(e.message));socket.on("close",()=>report.closed++);socket.on("message",(data,binary)=>{if(binary)report.frames++;else if(JSON.parse(data).type==="capture_stop")report.captureStops++;});if(mode==="disconnected")socket.close(4403,"local_test_reject");});
server.listen(Number(process.env.ST_LOCAL_TEST_PORT||8796),"127.0.0.1",()=>{
 const child=cp.spawn(binary,[mode],{stdio:["ignore","pipe","pipe"]});let log="";
 child.stdout.on("data",b=>log+=b);child.stderr.on("data",b=>log+=b);
 child.on("error",e=>report.errors.push(e.message));
 child.on("close",(code,signal)=>{
  report.nativeExitCode=code;report.nativeSignal=signal;
  const elapsed=log.match(/destroy_elapsed_ns=(\d+)/);report.destroyElapsedNs=elapsed?Number(elapsed[1]):null;
  setTimeout(()=>{
   try{assert.equal(code,0);assert.equal(signal,null);assert.ok(Number.isSafeInteger(report.destroyElapsedNs)&&report.destroyElapsedNs<1e9);assert.equal(report.errors.length,0);assert.equal(report.captureStops,0);
    if(mode==="pending")assert.equal(report.connections,0);else {assert.equal(report.connections,1);assert.equal(report.closed,1);}
    if(mode==="active")assert.ok(report.frames>30);
    else if(mode==="pending")assert.equal(report.frames,0);
    else {assert.match(log,/terminal_sent_count=\d+; no_later_pcm=true/);report.terminalSendFenceVerified=true;}report.passed=true;
   }catch(e){report.errors.push(e.message);report.passed=false;}
   fs.writeFileSync(output,JSON.stringify(report,null,2));fs.writeFileSync(output.replace(/\.json$/,".log"),log);
   for(const socket of pending)socket.destroy();for(const ws of wss.clients)ws.terminate();wss.close();server.close();
   console.log(JSON.stringify(report));process.exitCode=report.passed?0:1;
  },50);
 });
});
