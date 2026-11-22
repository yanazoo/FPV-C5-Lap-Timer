'use strict';

// ── The race's RSSI record (race tab, "📈" on a pilot's card) ─────────────
// The timer keeps every channel's RSSI for the race (the highest reading of
// every 40 ms, race time: pauses left out). Here it is drawn with the
// pilot's passes, so a pass the receiver missed can be found and added
// afterwards (race stopped, not cleared), or a wrong one deleted.
var rr={slot:-1,data:null,recMs:40,raceMs:0,t0:0,span:30000,cur:-1,drag:null,bound:false};

async function openRssiRec(slot){
  if(!isAuthed){toast('🔒 ログインが必要です');return;}
  var p=slots[slot];if(!p)return;
  rr.slot=slot;rr.cur=-1;rr.data=null;document.getElementById('rrMsg').textContent='';
  var nm=p.name==='---'?'Ch'+(slot+1):p.name;
  var n=document.getElementById('rrName');n.textContent=nm;n.className='cz-name '+PCLS[slot];
  document.getElementById('rrFreq').textContent=p.freq?freqLabel(p.freq):'';
  document.getElementById('rssiRec').style.display='flex';
  rrBind();rrSize();rrInfo('読み込み中…');
  try{
    var r=await fetch('/api/race/rssi?slot='+slot,{cache:'no-store'});
    if(!r.ok)throw 0;
    rr.recMs=parseInt(r.headers.get('X-Rec-Ms'))||40;
    rr.raceMs=parseInt(r.headers.get('X-Race-Ms'))||0;
    rr.data=new Uint16Array(await r.arrayBuffer());
  }catch(e){rr.data=new Uint16Array(0);rr.raceMs=0;}
  if(!rr.raceMs){rrInfo('このレースの RSSI の記録がありません（レース中に記録します。クリアすると消えます）');rrDraw();return;}
  // Start at the first pass (or the start), 30 s wide.
  var pt=rrPasses();
  rr.span=Math.min(30000,rr.raceMs);
  rr.t0=Math.max(0,Math.min(rr.raceMs-rr.span,(pt.length?pt[0]:0)-3000));
  rrSyncPan();rrDraw();rrInfo();
}
function closeRssiRec(){rr.slot=-1;rr.data=null;document.getElementById('rssiRec').style.display='none';}
// syncLaps() calls this: redraw the passes after an edit (any browser).
function rrLapsChanged(){if(rr.slot>=0){rrDraw();rrInfo();}}

// The pilot's passes as race times (sums of the lap times).
function rrPasses(){
  var p=slots[rr.slot],t=0;
  return (p&&p.rows?p.rows:[]).map(r=>(t+=r.lapMs));
}
function rrLabel(i){return lapMode==='immediate'?(i+1)+'周目':(i===0?'HS':i+'周目');}
function rrFmt(ms){
  var m=Math.floor(ms/60000),s=(ms%60000)/1000;
  return m+':'+(s<10?'0':'')+s.toFixed(2);
}
function rrSize(){
  var cv=document.getElementById('rrCanvas');
  cv.width=cv.offsetWidth||window.innerWidth-32;
  cv.height=Math.max(220,Math.min(460,Math.round(window.innerHeight*0.42)));
}
function rrVal(t){var i=Math.floor(t/rr.recMs);return rr.data&&i>=0&&i<rr.data.length?rr.data[i]:0;}

function rrDraw(){
  var cv=document.getElementById('rrCanvas');if(!cv||rr.slot<0)return;
  var ctx=cv.getContext('2d'),w=cv.width,h=cv.height,p=slots[rr.slot];
  ctx.fillStyle='#0d1117';ctx.fillRect(0,0,w,h);
  if(!rr.data||!rr.raceMs)return;
  var t0=rr.t0,t1=t0+rr.span,toX=t=>(t-t0)/rr.span*w;
  // The value range of what is on screen, the thresholds included.
  var lo=1023,hi=0;
  var i0=Math.max(0,Math.floor(t0/rr.recMs)),i1=Math.min(rr.data.length-1,Math.ceil(t1/rr.recMs));
  for(var i=i0;i<=i1;i++){var v=rr.data[i];if(v){if(v<lo)lo=v;if(v>hi)hi=v;}}
  [p.enterRssi,p.exitRssi].forEach(v=>{if(v<lo)lo=v;if(v>hi)hi=v;});
  lo=Math.max(0,lo-30);hi=Math.min(RSSI_MAX,hi+30);if(hi-lo<120){var m=(hi+lo)/2;lo=m-60;hi=m+60;}
  var toY=v=>h-(Math.max(lo,Math.min(hi,v))-lo)/(hi-lo)*h;
  rr.map={toX:toX,lo:lo,hi:hi};
  // Seconds grid.
  var step=rr.span<=15000?1000:rr.span<=60000?5000:rr.span<=300000?30000:60000;
  ctx.strokeStyle='#1e2530';ctx.lineWidth=1;ctx.fillStyle='#666';ctx.font='12px monospace';
  for(var g=Math.ceil(t0/step)*step;g<=t1;g+=step){
    var gx=toX(g);ctx.beginPath();ctx.moveTo(gx,0);ctx.lineTo(gx,h);ctx.stroke();
    ctx.fillText(rrFmt(g).replace(/\.\d+$/,''),gx+2,h-4);
  }
  // Thresholds.
  ctx.setLineDash([6,4]);ctx.lineWidth=2;
  ctx.strokeStyle='#3fb950';ctx.beginPath();ctx.moveTo(0,toY(p.enterRssi));ctx.lineTo(w,toY(p.enterRssi));ctx.stroke();
  ctx.strokeStyle='#f85149';ctx.beginPath();ctx.moveTo(0,toY(p.exitRssi));ctx.lineTo(w,toY(p.exitRssi));ctx.stroke();
  ctx.setLineDash([]);
  // The trace: the highest value per screen pixel.
  var col=getComputedStyle(document.documentElement).getPropertyValue('--p'+rr.slot).trim()||'#58a6ff';
  ctx.strokeStyle=col;ctx.lineWidth=1.5;ctx.beginPath();
  var first=true;
  for(var x=0;x<w;x++){
    var a=Math.floor((t0+x/w*rr.span)/rr.recMs),b=Math.max(a,Math.floor((t0+(x+1)/w*rr.span)/rr.recMs)-1),mx=0;
    for(var k=a;k<=b&&k<rr.data.length;k++)if(k>=0&&rr.data[k]>mx)mx=rr.data[k];
    var y=toY(mx);if(first){ctx.moveTo(x,y);first=false;}else ctx.lineTo(x,y);
  }
  ctx.stroke();
  // The passes counted.
  ctx.font='bold 12px sans-serif';
  rrPasses().forEach(function(t,i){
    if(t<t0||t>t1)return;var x=toX(t);
    ctx.strokeStyle=col;ctx.lineWidth=2;ctx.beginPath();ctx.moveTo(x,14);ctx.lineTo(x,h);ctx.stroke();
    ctx.fillStyle=col;ctx.fillText(rrLabel(i),x+3,12);
  });
  // The end of the race.
  if(rr.raceMs>=t0&&rr.raceMs<=t1){var ex=toX(rr.raceMs);ctx.strokeStyle='#888';ctx.setLineDash([2,3]);ctx.beginPath();ctx.moveTo(ex,0);ctx.lineTo(ex,h);ctx.stroke();ctx.setLineDash([]);}
  // The time picked.
  if(rr.cur>=0&&rr.cur>=t0&&rr.cur<=t1){
    var cx=toX(rr.cur);ctx.strokeStyle='#fff';ctx.lineWidth=2;ctx.setLineDash([4,3]);
    ctx.beginPath();ctx.moveTo(cx,0);ctx.lineTo(cx,h);ctx.stroke();ctx.setLineDash([]);
  }
}

// What is picked, and what can be done with it.
function rrNearPass(){
  var pt=rrPasses(),best=-1,d=1e9;
  pt.forEach((t,i)=>{var e=Math.abs(t-rr.cur);if(e<d){d=e;best=i;}});
  return d<=600?best:-1;
}
function rrInfo(msg){
  var el=document.getElementById('rrInfo'),add=document.getElementById('rrAdd'),del=document.getElementById('rrDel');
  add.disabled=del.disabled=true;
  if(msg){el.textContent=msg;return;}
  if(rr.cur<0){el.textContent='グラフをタップして時刻を選びます（近くの山に合わせます）。「次の候補」で、ラップになっていない山を順に探せます。';return;}
  var pt=rrPasses(),prev=null,next=null;
  pt.forEach(t=>{if(t<rr.cur)prev=t;else if(next===null&&t>rr.cur)next=t;});
  var s='選んだ時刻 '+rrFmt(rr.cur)+'　RSSI '+rrVal(rr.cur);
  if(prev!==null)s+='　前の通過から '+((rr.cur-prev)/1000).toFixed(2)+' 秒';
  if(next!==null)s+='　次まで '+((next-rr.cur)/1000).toFixed(2)+' 秒';
  var near=rrNearPass();
  if(raceRunning)s+='（レースをストップすると編集できます）';
  else if(near>=0){s+='　← '+rrLabel(near)+'の通過';del.disabled=false;}
  else add.disabled=false;
  el.textContent=s;
}
// The highest sample within ±ms of t.
function rrPeakNear(t,ms){
  var a=Math.max(0,Math.floor((t-ms)/rr.recMs)),b=Math.min(rr.data.length-1,Math.floor((t+ms)/rr.recMs)),k=Math.floor(t/rr.recMs);
  for(var i=a;i<=b;i++)if(rr.data[i]>rr.data[k])k=i;
  return k*rr.recMs+rr.recMs/2;
}
function rrPick(t){rr.cur=Math.max(0,Math.min(rr.raceMs-1,t));rrDraw();rrInfo();}
function rrShow(t){   // keep t on screen
  if(t<rr.t0||t>rr.t0+rr.span)rr.t0=Math.max(0,Math.min(rr.raceMs-rr.span,t-rr.span/2));
  rrSyncPan();
}
// The next peak after the picked time (or the screen's start) that is above
// the exit line, isn't a counted pass and is clear of the passes by the
// cooldown: a pass the receiver may have missed.
function rrNextCandidate(dir){
  if(!rr.data||!rr.raceMs)return;
  var p=slots[rr.slot],lvl=p.exitRssi,pt=rrPasses(),gap=Math.max(1000,cooldownMs||0);
  var n=rr.data.length,k=Math.floor((rr.cur>=0?rr.cur:(dir>0?rr.t0:rr.t0+rr.span))/rr.recMs)+dir*Math.ceil(gap/rr.recMs/2);
  for(;k>0&&k<n-1;k+=dir){
    var v=rr.data[k];
    if(v<lvl||v<rr.data[k-1]||v<rr.data[k+1])continue;
    var t=rrPeakNear(k*rr.recMs,400);
    if(pt.some(x=>Math.abs(x-t)<gap))continue;
    rrShow(t);rrPick(t);return;
  }
  rrMsg(dir>0?'この先に候補はありません':'この前に候補はありません');
}
async function rrAddLap(){
  if(rr.cur<0||raceRunning)return;
  var p=slots[rr.slot];
  if(!confirm((p.name==='---'?'Ch'+(rr.slot+1):p.name)+' に '+rrFmt(rr.cur)+' の通過を追加します。その周を 2 つに分け、トータルタイムは変わりません。全ブラウザ・順位表・ゴーグルに反映します。'))return;
  try{
    var r=await fetch('/api/laps/insert',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slot:rr.slot,atMs:Math.round(rr.cur)})});
    if(r.ok)rrMsg('✓ '+rrFmt(rr.cur)+' にラップを追加しました','var(--ok)');
    else rrMsg('⚠ 追加できません（レース中・ほかの通過のすぐ近く・レースの終わりより後）','var(--err)');
  }catch(e){rrMsg('⚠ 接続エラー','var(--err)');}
}
// The header's toast is under this full-screen view: messages go here.
function rrMsg(t,col){var e=document.getElementById('rrMsg');e.textContent=t;e.style.color=col||'';clearTimeout(rrMsg.t);rrMsg.t=setTimeout(()=>{e.textContent='';},6000);}
function rrDeleteLap(){
  var i=rrNearPass();if(i<0)return;
  openLapDelete(slots[rr.slot],i);   // the race tab's confirmation
}
function rrZoom(f){
  var c=rr.cur>=0&&rr.cur>=rr.t0&&rr.cur<=rr.t0+rr.span?rr.cur:rr.t0+rr.span/2;
  rr.span=f==='all'?rr.raceMs:Math.max(5000,Math.min(rr.raceMs,rr.span*f));
  rr.t0=Math.max(0,Math.min(rr.raceMs-rr.span,c-rr.span/2));
  rrSyncPan();rrDraw();
}
function rrSyncPan(){
  var s=document.getElementById('rrPan');
  s.max=Math.max(0,rr.raceMs-rr.span);s.value=rr.t0;s.disabled=rr.raceMs<=rr.span;
  document.getElementById('rrSpan').textContent=(rr.span/1000).toFixed(0)+' 秒';
}
function rrPan(){rr.t0=parseInt(document.getElementById('rrPan').value)||0;rrDraw();}
function rrBind(){
  if(rr.bound)return;rr.bound=true;
  var cv=document.getElementById('rrCanvas');
  cv.addEventListener('pointerdown',function(e){rr.drag={x:e.clientX,t0:rr.t0,moved:false};cv.setPointerCapture(e.pointerId);});
  cv.addEventListener('pointermove',function(e){
    if(!rr.drag)return;var dx=e.clientX-rr.drag.x;
    if(Math.abs(dx)>6)rr.drag.moved=true;
    if(rr.drag.moved){var r=cv.getBoundingClientRect();rr.t0=Math.max(0,Math.min(rr.raceMs-rr.span,rr.drag.t0-dx/r.width*rr.span));rrSyncPan();rrDraw();}
  });
  cv.addEventListener('pointerup',function(e){
    var d=rr.drag;rr.drag=null;if(!d||d.moved||!rr.raceMs)return;
    var r=cv.getBoundingClientRect(),t=rr.t0+(e.clientX-r.left)/r.width*rr.span;
    rrPick(rrPeakNear(t,600));
  });
  cv.addEventListener('pointercancel',function(){rr.drag=null;});
  window.addEventListener('resize',function(){if(rr.slot>=0){rrSize();rrDraw();}});
  document.addEventListener('keydown',function(e){if(e.key==='Escape'&&rr.slot>=0)closeRssiRec();});
}
