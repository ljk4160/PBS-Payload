// SPDX-License-Identifier: GPL-3.0-or-later
(function(root){
 'use strict';
 const masks=[0x4000,0x2000,0x8000,0x1000,0x400,0x800,0x100,0x200,0x10000,8,2,4,0x10,0x40,0x80,0x20,0x20000,0x100000];
 const bindings={Space:0x4000,KeyX:0x4000,KeyC:0x2000,KeyZ:0x8000,KeyV:0x1000,KeyQ:0x400,KeyE:0x800,KeyR:0x100,KeyF:0x200,Enter:8,Backspace:0x10000,KeyP:0x20000,KeyT:0x100000,ArrowUp:0x10,ArrowDown:0x40,ArrowLeft:0x80,ArrowRight:0x20};
 const axisKeys=['KeyW','KeyA','KeyS','KeyD','KeyI','KeyJ','KeyK','KeyL'];
 const neutral=()=>({buttons:0,lx:128,ly:128,rx:128,ry:128,l2:0,r2:0});
 function axis(value){value=Number.isFinite(value)?Math.max(-1,Math.min(1,value)):0;return Math.abs(value)<0.12?128:Math.round((value+1)*127.5);}
 function trigger(button){const value=button?.value;return Number.isFinite(value)?Math.round(Math.max(0,Math.min(1,value))*255):0;}
 function stateFor(gamepad,keys=new Set()){
  const s=neutral();
  if(gamepad?.connected&&gamepad.mapping==='standard'){
   for(let i=0;i<masks.length;i++)if(gamepad.buttons?.[i]?.pressed)s.buttons|=masks[i];
   [s.lx,s.ly,s.rx,s.ry]=[0,1,2,3].map(i=>axis(gamepad.axes?.[i]));
   s.l2=trigger(gamepad.buttons?.[6]);s.r2=trigger(gamepad.buttons?.[7]);
  }
  for(const key of keys)if(bindings[key])s.buttons|=bindings[key];
  const keyAxis=(negative,positive)=>keys.has(negative)===keys.has(positive)?128:keys.has(negative)?0:255;
  if(keys.has('KeyA')||keys.has('KeyD'))s.lx=keyAxis('KeyA','KeyD');
  if(keys.has('KeyW')||keys.has('KeyS'))s.ly=keyAxis('KeyW','KeyS');
  if(keys.has('KeyJ')||keys.has('KeyL'))s.rx=keyAxis('KeyJ','KeyL');
  if(keys.has('KeyI')||keys.has('KeyK'))s.ry=keyAxis('KeyI','KeyK');
  if(keys.has('KeyR'))s.l2=255;if(keys.has('KeyF'))s.r2=255;
  return s;
 }
 function encode(sequence,s){return [sequence>>>0,s.buttons>>>0,s.lx,s.ly,s.rx,s.ry,s.l2,s.r2].join(',');}
 const virt={buttons:0,lx:128,ly:128,rx:128,ry:128};
 root.PBSController={neutral,stateFor,encode,virtual:virt,kick(){force=true;},get active(){return !!token;}};
 if(!root.document||!root.navigator)return;
 const doc=root.document,surface=doc.getElementById('controller-controls');
 if(!surface)return;
 const connect=doc.getElementById('connect-controller'),disconnect=doc.getElementById('disconnect-controller'),info=doc.getElementById('controller-info');
 let token='',sequence=0,busy=false,connecting=false,last='',lastSent=0,force=false,mouseX=0,mouseY=0;
 const keys=new Set();
 function reset(){keys.clear();mouseX=mouseY=0;force=true;}
 function reflect(){connect.disabled=connecting;disconnect.hidden=true;connect.dataset.on=token?'1':'0';connect.textContent=token?(window.pbsT?window.pbsT('disconnectController'):'Disconnect controller'):(window.pbsT?window.pbsT('connectController'):'Connect controller');
  if(window.pbsVirtualPad)window.pbsVirtualPad.setConnected(!!token);}
 async function command(path,body='',options={}){
  const abort=new AbortController(),timer=setTimeout(()=>abort.abort(),800);
  try{
   const headers={'X-PBS-Input':'1','Content-Type':'application/x-pbs-controller'};
   if(token)headers['X-PBS-Controller']=token;
   const response=await root.fetch(path,{method:'POST',headers,body,cache:'no-store',signal:abort.signal,...options});
   const data=await response.json();
   if(!response.ok)throw new Error(data.message||'Controller request failed');
   return data;
  }finally{clearTimeout(timer);}
 }
 async function end(){
  if(!token)return;
  reset();
  try{await command('/input/disconnect');}catch(e){}
  virt.buttons=0;virt.lx=virt.ly=virt.rx=virt.ry=128;
  token='';last='';reflect();
 }
 connect.addEventListener('click',async()=>{
  if(connecting)return;
  if(token){end();return;}
  connecting=true;reflect();
  try{
   const result=await command('/input/connect');
   if(!/^[a-f0-9]{32}$/.test(result.token))throw new Error('Invalid controller session');
   token=result.token;sequence=0;last='';lastSent=0;reset();reflect();connect.blur();
  }catch(e){const m=doc.getElementById('message');if(m)m.textContent=e.message;connect.blur();}
  finally{connecting=false;reflect();}
 });
 disconnect.addEventListener('click',()=>end());
 async function tick(){
  if(!token||busy)return;
  let s=neutral();
  {
   let pads=[];try{pads=root.navigator.getGamepads?.()||[];}catch(e){}
   const pad=Array.from(pads).find(p=>p?.connected&&p.mapping==='standard');
   s=stateFor(pad,keys);
   if(mouseX||mouseY){s.rx=Math.max(0,Math.min(255,128+mouseX*6));s.ry=Math.max(0,Math.min(255,128+mouseY*6));mouseX=mouseY=0;}
  }
  if(virt.buttons)s.buttons|=virt.buttons;
  if(window.pbsVpPulse){
    if(window.pbsVpPulse.frames-->0)s.buttons|=window.pbsVpPulse.mask;
    else window.pbsVpPulse=null;
  }
  if(virt.lx!==128||virt.ly!==128){s.lx=virt.lx;s.ly=virt.ly;}
  if(virt.rx!==128||virt.ry!==128){s.rx=virt.rx;s.ry=virt.ry;}
  s.rx=Math.round(s.rx);s.ry=Math.round(s.ry);
  const signature=encode(0,s),now=Date.now();
  if(!force&&signature===last&&now-lastSent<100)return;
  const owner=token;busy=true;force=false;last=signature;lastSent=now;
  try{await command('/input/state',encode(sequence++,s));}
  catch(e){if(token===owner)await end('Controller stopped: '+e.message);}
  finally{busy=false;}
 }
 const owned=e=>{
  if(e.ctrlKey||e.metaKey||e.altKey)return false;
  const t=e.target&&e.target.tagName?e.target.tagName:'';
  if(/^(INPUT|TEXTAREA|SELECT)$/.test(t)||(e.target&&e.target.isContentEditable))return false;
  return Boolean(bindings[e.code]||axisKeys.includes(e.code));
 };
 /* PS5 owns these keys in every phase: no player hotkey (play/pause, volume,
  * seek) may react. Swallow at window and document, keydown/keypress/keyup. */
 const swallow=e=>{if(!owned(e))return;e.preventDefault();e.stopImmediatePropagation();};
 const consumeDown=e=>{if(!owned(e))return;e.preventDefault();e.stopImmediatePropagation();if(token)keys.add(e.code);};
 const consumeUp=e=>{const had=keys.delete(e.code);if(!owned(e))return;e.preventDefault();e.stopImmediatePropagation();if(had)force=true;};
 for(const t of [root,doc]){
  t.addEventListener('keydown',consumeDown,true);
  t.addEventListener('keypress',swallow,true);
  t.addEventListener('keyup',consumeUp,true);
 }
 root.addEventListener('blur',reset);
 doc.addEventListener('visibilitychange',()=>{if(doc.hidden)reset();});
 root.addEventListener('gamepaddisconnected',reset);
 const video=doc.getElementById('video');
 video?.addEventListener('contextmenu',e=>{if(token)e.preventDefault();});
 video?.addEventListener('mousemove',e=>{if(token&&(e.buttons&2)){mouseX+=e.movementX||0;mouseY+=e.movementY||0;}});
 root.addEventListener('pagehide',()=>{
  if(token)root.fetch('/input/disconnect',{method:'POST',headers:{'X-PBS-Input':'1','X-PBS-Controller':token},keepalive:true}).catch(()=>{});
  token='';reset();
 });
 doc.getElementById('stop')?.addEventListener('click',()=>end());
 /* Safety net: a stray play/pause hotkey must never freeze a live stream. */
 const vid=doc.getElementById('video');
 vid?.addEventListener('pause',()=>{
  if(stopped||vid.ended)return;
  setTimeout(()=>{if(!stopped&&vid.paused&&!vid.ended)vid.play().catch(()=>{});},60);
 });
 setInterval(tick,16);
 async function status(){
  const owner=token;
  try{
   const response=await root.fetch('/input/status',{cache:'no-store'});const s=await response.json();
   surface.hidden=!s.supported;
   if(token!==owner||connecting)return;
   if(s.cleanup_required){token='';reflect();}
   else if(token&&!s.connected){token='';reset();reflect();}
  }catch(e){}
 }
 reflect();status();setInterval(status,1500);
})(typeof window!=='undefined'?window:globalThis);
