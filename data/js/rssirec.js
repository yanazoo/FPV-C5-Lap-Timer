'use strict';

// ── The race's RSSI record (race tab, "RSSI" on a pilot's card) ─────────────
// The timer keeps every channel's RSSI for the race (the highest reading of
// every 40 ms, race time: pauses left out). Here it is drawn for the whole
// race with the pilot's passes. To get a pass the receiver missed, move the
// Enter / Exit lines until that peak counts (the passes these values would
// give are shown as you go), then have the timer count the pilot's laps
// again with them (race stopped, not cleared); the values are kept for the
// pilot, so the next race counts with them.
var rr={slot:-1,data:null,recMs:40,raceMs:0,t0:0,span:0,cur:-1,drag:null,bound:false,en:0,ex:0,lineDrag:null};

async function openRssiRec(slot){
  if(!isAuthed){toast('🔒 ログインが必要です');return;}
  var p=slots[slot];if(!p)return;
  rr.slot=slot;rr.cur=-1;rr.data=null;rr.en=p.enterRssi;rr.ex=p.exitRssi;   // data=null: fetch it all
  document.getElementById('rrMsg').textContent='';
  var nm=p.name==='---'?'Ch'+(slot+1):p.name;
  var n=document.getElementById('rrName');n.textContent=nm;n.className='cz-name '+PCLS[slot];
  document.getElementById('rrFreq').textContent=p.freq?freqLabel(p.freq):'';
  document.getElementById('rssiRec').style.display='flex';
  rrBind();rrSize();rrShowValues();rrInfo('読み込み中…');
  await rrFetch();
  if(!rr.raceMs){rrInfo('このレースの RSSI の記録がありません（レース中に記録します。クリアすると消えます）');rrDraw();}
  else{rrWhole();rrInfo();}
  // Racing: the record grows; fetch it again every 2 s (the view keeps
  // its place; the whole-race view keeps showing the whole race).
  clearInterval(rr.poll);
  rr.poll=setInterval(async function(){
    if(rr.slot<0||!raceRunning||rr.lineDrag)return;
    var whole=rr.span>=rr.raceMs;
    await rrFetch();
    if(whole){rr.span=rr.raceMs;rr.t0=0;}
    rrSyncPan();rrDraw();rrInfo();
  },2000);
}
// The record (first time whole, then only what was added: 4 h is 720 KB).
// The last sample may still grow, so it is fetched again each time.
async function rrFetch(){
  var have=rr.data&&rr.data.length?rr.data.length-1:0;
  try{
    var r=await fetch('/api/race/rssi?slot='+rr.slot+(have?'&from='+have:''),{cache:'no-store'});
    if(!r.ok)throw 0;
    rr.recMs=parseInt(r.headers.get('X-Rec-Ms'))||40;
    rr.raceMs=parseInt(r.headers.get('X-Race-Ms'))||0;
    var from=parseInt(r.headers.get('X-From'))||0,add=new Uint16Array(await r.arrayBuffer());
    if(!from||!rr.data){rr.data=add;return;}
    var d=new Uint16Array(from+add.length);d.set(rr.data.subarray(0,from));d.set(add,from);rr.data=d;
  }catch(e){if(!rr.data){rr.data=new Uint16Array(0);rr.raceMs=0;}}
}
function closeRssiRec(){rr.slot=-1;rr.data=null;clearInterval(rr.poll);document.getElementById('rssiRec').style.display='none';}
// syncLaps() calls this: redraw the passes after an edit (any browser).
function rrLapsChanged(){if(rr.slot>=0){rrDraw();rrInfo();}}

// The pilot's passes counted, as race times (sums of the lap times).
function rrPasses(){
  var p=slots[rr.slot],t=0;
  return (p&&p.rows?p.rows:[]).map(r=>(t+=r.lapMs));
}
// The passes the Enter / Exit being tried would give: the timer's rule
// (gate::findPasses on the record, then the cooldown and the race's end).
function rrPreview(){
  // A pass counted already keeps its own time (as the timer does).
  var out=[],inside=false,peak=0,at=0,last=-1e9,d=rr.data,n=d?d.length:0,cd=cooldownMs||0,had=rrPasses();
  for(var i=0;i<n;i++){
    var x=d[i];
    if(!inside){if(x>=rr.en){inside=true;peak=x;at=i;}continue;}
    if(x>peak){peak=x;at=i;}
    if(x>rr.ex)continue;
    inside=false;
    var t=at*rr.recMs+rr.recMs/2,h=had.find(x=>Math.abs(x-t)<=400);
    if(h!==undefined)t=h;
    if(t>=rr.raceMs||(out.length&&t-last<cd))continue;
    out.push(t);last=t;
  }
  return out;
}
function rrLabel(i){return lapMode==='immediate'?(i+1)+'周目':(i===0?'HS':i+'周目');}
function rrLaps(nPasses){return lapMode==='immediate'?nPasses:Math.max(0,nPasses-1);}
function rrFmt(ms){
  var m=Math.floor(ms/60000),s=(ms%60000)/1000;
  return m+':'+(s<10?'0':'')+s.toFixed(2);
}
function rrSize(){
  var cv=document.getElementById('rrCanvas');
  cv.width=cv.offsetWidth||window.innerWidth-32;
  cv.height=Math.max(220,Math.min(420,Math.round(window.innerHeight*0.38)));
}
function rrVal(t){var i=Math.floor(t/rr.recMs);return rr.data&&i>=0&&i<rr.data.length?rr.data[i]:0;}

function rrDraw(){
  var cv=document.getElementById('rrCanvas');if(!cv||rr.slot<0)return;
  var ctx=cv.getContext('2d'),w=cv.width,h=cv.height;
  ctx.fillStyle='#0d1117';ctx.fillRect(0,0,w,h);
  if(!rr.data||!rr.raceMs)return;
  var t0=rr.t0,t1=t0+rr.span,toX=t=>(t-t0)/rr.span*w;
  // The value range on screen, the lines included; held while a line is dragged.
  var lo,hi;
  if(rr.lineDrag&&rr.map){lo=rr.map.lo;hi=rr.map.hi;}
  else{
    lo=1023;hi=0;
    var i0=Math.max(0,Math.floor(t0/rr.recMs)),i1=Math.min(rr.data.length-1,Math.ceil(t1/rr.recMs));
    for(var i=i0;i<=i1;i++){var v=rr.data[i];if(v){if(v<lo)lo=v;if(v>hi)hi=v;}}
    [rr.en,rr.ex].forEach(v=>{if(v<lo)lo=v;if(v>hi)hi=v;});
    lo=Math.max(0,lo-30);hi=Math.min(RSSI_MAX,hi+30);if(hi-lo<120){var m=(hi+lo)/2;lo=m-60;hi=m+60;}
  }
  var toY=v=>h-(Math.max(lo,Math.min(hi,v))-lo)/(hi-lo)*h;
  rr.map={toX:toX,toY:toY,lo:lo,hi:hi};
  // Time grid.
  var step=rr.span<=15000?1000:rr.span<=60000?5000:rr.span<=300000?30000:60000;
  ctx.strokeStyle='#1e2530';ctx.lineWidth=1;ctx.fillStyle='#666';ctx.font='12px monospace';
  for(var g=Math.ceil(t0/step)*step;g<=t1;g+=step){
    var gx=toX(g);ctx.beginPath();ctx.moveTo(gx,0);ctx.lineTo(gx,h);ctx.stroke();
    ctx.fillText(rrFmt(g).replace(/\.\d+$/,''),gx+2,h-4);
  }
  // The trace: the highest value per screen pixel.
  var col=getComputedStyle(document.documentElement).getPropertyValue('--p'+rr.slot).trim()||'#58a6ff';
  ctx.strokeStyle=col;ctx.lineWidth=1.5;ctx.beginPath();
  for(var x=0;x<w;x++){
    var a=Math.floor((t0+x/w*rr.span)/rr.recMs),b=Math.max(a,Math.floor((t0+(x+1)/w*rr.span)/rr.recMs)-1),mx=0;
    for(var k=a;k<=b&&k<rr.data.length;k++)if(k>=0&&rr.data[k]>mx)mx=rr.data[k];
    var y=toY(mx);if(x===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
  }
  ctx.stroke();
  // The lines being tried, with their values.
  ctx.setLineDash([6,4]);ctx.lineWidth=2;ctx.font='bold 13px monospace';
  ctx.strokeStyle='#3fb950';ctx.beginPath();ctx.moveTo(0,toY(rr.en));ctx.lineTo(w,toY(rr.en));ctx.stroke();
  ctx.strokeStyle='#f85149';ctx.beginPath();ctx.moveTo(0,toY(rr.ex));ctx.lineTo(w,toY(rr.ex));ctx.stroke();
  ctx.setLineDash([]);
  ctx.fillStyle='#3fb950';ctx.fillText('入 '+rr.en,w-78,toY(rr.en)-5);
  ctx.fillStyle='#f85149';ctx.fillText('出 '+rr.ex,w-78,toY(rr.ex)+15);
  // The passes counted now (lines) and those the values tried would give:
  // ▼ yellow = a pass that would be added, ✕ red = one that would go.
  var now=rrPasses(),pre=rrPreview(),near=(t,list)=>list.some(x=>Math.abs(x-t)<=400);
  ctx.font='bold 12px sans-serif';
  now.forEach(function(t,i){
    if(t<t0||t>t1)return;var x=toX(t),gone=!near(t,pre);
    ctx.strokeStyle=gone?'#f85149':col;ctx.lineWidth=2;ctx.beginPath();ctx.moveTo(x,16);ctx.lineTo(x,h);ctx.stroke();
    ctx.fillStyle=gone?'#f85149':col;ctx.fillText(gone?'✕':(rr.span<=120000?rrLabel(i):''),x+3,13);
  });
  pre.forEach(function(t){
    if(t<t0||t>t1||near(t,now))return;var x=toX(t);
    ctx.strokeStyle='#e3b341';ctx.lineWidth=2;ctx.setLineDash([3,3]);ctx.beginPath();ctx.moveTo(x,16);ctx.lineTo(x,h);ctx.stroke();ctx.setLineDash([]);
    ctx.fillStyle='#e3b341';ctx.beginPath();ctx.moveTo(x-6,2);ctx.lineTo(x+6,2);ctx.lineTo(x,13);ctx.closePath();ctx.fill();
  });
  // The end of the race.
  if(rr.raceMs>=t0&&rr.raceMs<=t1){var ex=toX(rr.raceMs);ctx.strokeStyle='#888';ctx.setLineDash([2,3]);ctx.beginPath();ctx.moveTo(ex,0);ctx.lineTo(ex,h);ctx.stroke();ctx.setLineDash([]);}
  // The time picked.
  if(rr.cur>=0&&rr.cur>=t0&&rr.cur<=t1){
    var cx=toX(rr.cur);ctx.strokeStyle='#fff';ctx.lineWidth=1.5;ctx.setLineDash([4,3]);
    ctx.beginPath();ctx.moveTo(cx,0);ctx.lineTo(cx,h);ctx.stroke();ctx.setLineDash([]);
  }
}

// The counts: now, and with the values tried.
function rrInfo(msg){
  var el=document.getElementById('rrInfo'),del=document.getElementById('rrDel'),go=document.getElementById('rrRecount');
  del.disabled=true;go.disabled=true;
  if(msg){el.textContent=msg;return;}
  var p=slots[rr.slot],now=rrPasses(),pre=rrPreview(),near=(t,list)=>list.some(x=>Math.abs(x-t)<=400);
  var add=pre.filter(t=>!near(t,now)).length,gone=now.filter(t=>!near(t,pre)).length;
  var s='今の計測 '+rrLaps(now.length)+' 周（入 '+p.enterRssi+' / 出 '+p.exitRssi+'）→ この値（入 '+rr.en+' / 出 '+rr.ex+'）だと '+rrLaps(pre.length)+' 周';
  if(add||gone)s+='（▼ 増える通過 '+add+'・✕ 消える通過 '+gone+'）';
  var same=rr.en===p.enterRssi&&rr.ex===p.exitRssi&&!add&&!gone;
  if(rr.en<=rr.ex)s+='　⚠ 入は出より大きくしてください';
  else if(!same)go.disabled=false;
  if(raceRunning)s+='\nレース中：記録を 2 秒ごとに読み直しています。数え直しは今の時点までの記録で行い、ゲートを通過中の分はそのままレースが数えます';
  if(rr.cur>=0){
    var i=rrNearPass();
    s+='\n選んだ時刻 '+rrFmt(rr.cur)+'　RSSI '+rrVal(rr.cur)+(i>=0?'　← '+rrLabel(i)+'の通過':'');
    if(i>=0)del.disabled=false;
  }
  el.textContent=s;
  rrList(now,pre);
}
// The laps these values give, under the chart: lap, lap time, total;
// ▼ = a lap that comes in with them.
function rrList(now,pre){
  var box=document.getElementById('rrList');if(!box)return;
  if(!pre.length){box.innerHTML='<span class="res-muted">この値では通過がありません</span>';return;}
  var near=t=>now.some(x=>Math.abs(x-t)<=400),h='',prev=0;
  // 累計 as on the race card: from the holeshot in holeshot mode.
  var hs=lapMode==='immediate'?0:pre[0];
  var rows=pre.map(function(t,i){var r={i:i,t:(i===0||!hs)?t:t-hs,lap:t-prev,isNew:!near(t)};prev=t;return r;});
  for(var k=rows.length-1;k>=0;k--){   // newest first, as the race card
    var r=rows[k];
    h+='<tr'+(r.isNew?' class="rr-new"':'')+'><td>'+(r.isNew?'▼ ':'')+rrLabel(r.i)+'</td><td>'+fmt(r.lap)+'</td><td>'+fmt(r.t)+'</td></tr>';
  }
  box.innerHTML='<table class="lapTable"><thead><tr><th>周</th><th>タイム</th><th>累計</th></tr></thead><tbody>'+h+'</tbody></table>';
}
function rrNearPass(){
  var pt=rrPasses(),best=-1,d=1e9;
  pt.forEach((t,i)=>{var e=Math.abs(t-rr.cur);if(e<d){d=e;best=i;}});
  return d<=600?best:-1;
}
// The header's toast is under this full-screen view: messages go here.
function rrMsg(t,col){var e=document.getElementById('rrMsg');e.textContent=t;e.style.color=col||'';clearTimeout(rrMsg.t);rrMsg.t=setTimeout(()=>{e.textContent='';},6000);}

// Enter / Exit being tried.
function rrShowValues(){
  ['en','ex'].forEach(k=>{var n=document.getElementById('rr'+(k==='en'?'Enter':'Exit'));if(n&&document.activeElement!==n)n.value=rr[k];});
}
function rrSet(k,v){
  rr[k]=Math.max(0,Math.min(RSSI_MAX,Math.round(v)));
  rrShowValues();rrDraw();rrInfo();
}
function rrStep(k,d){rrSet(k,rr[k]+d);}
function rrNum(k){var v=parseInt(document.getElementById('rr'+(k==='en'?'Enter':'Exit')).value,10);if(!isNaN(v))rrSet(k,v);else rrShowValues();}
function rrReset(){var p=slots[rr.slot];if(p){rr.en=p.enterRssi;rr.ex=p.exitRssi;rrShowValues();rrDraw();rrInfo();}}
async function rrRecount(){
  var p=slots[rr.slot];if(!p||rr.en<=rr.ex)return;
  var nm=p.name==='---'?'Ch'+(rr.slot+1):p.name;
  if(!confirm(nm+' のラップを、入 '+rr.en+' / 出 '+rr.ex+' で数え直します（今の '+rrLaps(rrPasses().length)+' 周 → '+rrLaps(rrPreview().length)+' 周）。この値は '+nm+' のパイロット情報にも保存し、次のレースからこの値で計測します。全ブラウザ・順位表・ゴーグルに反映します。'))return;
  try{
    var r=await fetch('/api/laps/recount',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slot:rr.slot,enter:rr.en,exit:rr.ex})});
    var j={};try{j=await r.json();}catch(e){}
    if(r.ok){p.enterRssi=rr.en;p.exitRssi=rr.ex;rrMsg('✓ 数え直しました（'+rrLaps(j.passes||0)+' 周）。入・出をパイロット情報に保存しました','var(--ok)');}
    else rrMsg('⚠ 数え直せません（記録がありません）','var(--err)');
  }catch(e){rrMsg('⚠ 接続エラー','var(--err)');}
}
function rrDeleteLap(){
  var i=rrNearPass();if(i<0)return;
  openLapDelete(slots[rr.slot],i);   // the race tab's confirmation
}

// Whole race, or a close look (20 s around the time picked).
function rrWhole(){rr.span=rr.raceMs;rr.t0=0;rrSyncPan();rrDraw();}
function rrDetail(){
  if(!rr.raceMs)return;
  var c=rr.cur>=0?rr.cur:rr.t0+rr.span/2;
  rr.span=Math.min(20000,rr.raceMs);
  rr.t0=Math.max(0,Math.min(rr.raceMs-rr.span,c-rr.span/2));
  rrSyncPan();rrDraw();
}
function rrSyncPan(){
  var s=document.getElementById('rrPan'),zoomed=rr.span<rr.raceMs;
  s.max=Math.max(0,rr.raceMs-rr.span);s.value=rr.t0;s.style.display=zoomed?'':'none';
  document.getElementById('rrWhole').disabled=!zoomed;
}
function rrPan(){rr.t0=parseInt(document.getElementById('rrPan').value)||0;rrDraw();}
// The highest sample within ±ms of t.
function rrPeakNear(t,ms){
  var a=Math.max(0,Math.floor((t-ms)/rr.recMs)),b=Math.min(rr.data.length-1,Math.floor((t+ms)/rr.recMs)),k=Math.max(0,Math.min(rr.data.length-1,Math.floor(t/rr.recMs)));
  for(var i=a;i<=b;i++)if(rr.data[i]>rr.data[k])k=i;
  return k*rr.recMs+rr.recMs/2;
}
function rrBind(){
  if(rr.bound)return;rr.bound=true;
  var cv=document.getElementById('rrCanvas');
  var yOf=e=>{var r=cv.getBoundingClientRect();return (e.clientY-r.top)*cv.height/r.height;};
  cv.addEventListener('pointerdown',function(e){
    // On a line (within 24 px): drag it. Elsewhere: pick a time, or pan when zoomed in.
    if(rr.map&&rr.raceMs){
      var y=yOf(e),dEn=Math.abs(rr.map.toY(rr.en)-y),dEx=Math.abs(rr.map.toY(rr.ex)-y);
      if(Math.min(dEn,dEx)<=24){rr.lineDrag=dEn<=dEx?'en':'ex';cv.setPointerCapture(e.pointerId);e.preventDefault();return;}
    }
    rr.drag={x:e.clientX,t0:rr.t0,moved:false};cv.setPointerCapture(e.pointerId);
  });
  cv.addEventListener('pointermove',function(e){
    if(rr.lineDrag){
      var m=rr.map,v=m.lo+(cv.height-yOf(e))/cv.height*(m.hi-m.lo);
      rr[rr.lineDrag]=Math.max(0,Math.min(RSSI_MAX,Math.round(v)));rrShowValues();rrDraw();rrInfo();e.preventDefault();return;
    }
    if(!rr.drag)return;var dx=e.clientX-rr.drag.x;
    if(Math.abs(dx)>6)rr.drag.moved=true;
    if(rr.drag.moved&&rr.span<rr.raceMs){var r=cv.getBoundingClientRect();rr.t0=Math.max(0,Math.min(rr.raceMs-rr.span,rr.drag.t0-dx/r.width*rr.span));rrSyncPan();rrDraw();}
  });
  cv.addEventListener('pointerup',function(e){
    if(rr.lineDrag){rr.lineDrag=null;rrDraw();return;}
    var d=rr.drag;rr.drag=null;if(!d||d.moved||!rr.raceMs)return;
    var r=cv.getBoundingClientRect(),t=rr.t0+(e.clientX-r.left)/r.width*rr.span;
    rr.cur=rrPeakNear(t,rr.span>60000?Math.max(600,rr.span/r.width*6):600);rrDraw();rrInfo();
  });
  cv.addEventListener('pointercancel',function(){rr.drag=null;rr.lineDrag=null;});
  window.addEventListener('resize',function(){if(rr.slot>=0){rrSize();rrDraw();}});
  document.addEventListener('keydown',function(e){if(e.key==='Escape'&&rr.slot>=0)closeRssiRec();});
}
