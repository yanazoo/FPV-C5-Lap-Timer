'use strict';

var charts={};
var chartDirty=Array(N).fill(false);

// Runs at 60fps via rAF; draws only when calib tab is active and data changed.
// The hot RSSI path only pushes chart data while the calib tab is open, so
// off-tab this loop has nothing dirty to do.
(function chartLoop(){
  requestAnimationFrame(chartLoop);
  if(activeTab!=='calib')return;
  for(var i=0;i<N;i++){
    if(chartDirty[i]&&charts[i]){drawChart(i);chartDirty[i]=false;}
  }
})();

function buildCalibCards(){
  var g=document.getElementById('calibGrid');g.innerHTML='';
  slots.forEach(function(p){
    var d=document.createElement('div');d.className='calib-card';
    d.innerHTML=
      '<div class="calib-header '+PCLS[p.id]+'">'
        +'<span id="calName'+p.id+'">'+esc(p.name==='---'?'Ch'+(p.id+1):p.name)+'</span>'
        +'<span class="rssi-live-badge">'+(p.freq?esc(freqLabel(p.freq))+' ｜ ':'')+'RSSI: <strong id="calRssi'+p.id+'">---</strong></span>'
      +'</div>'
      +'<canvas id="calCanvas'+p.id+'"></canvas>'
      +'<div class="calib-sliders">'
        +'<div class="config-row"><label>Enter（入）</label>'
          +'<div class="slider-num">'
            +'<input type="range" id="calEnter'+p.id+'" min="0" max="1023" step="1" value="'+p.enterRssi+'" oninput="syncCalib('+p.id+',\'enter\')">'
            +'<input type="number" id="calEnterN'+p.id+'" min="0" max="1023" step="1" value="'+p.enterRssi+'" onchange="syncCalibN('+p.id+',\'enter\')">'
            +'<span class="unit"></span>'
          +'</div></div>'
        +'<div class="config-row"><label>Exit（出）</label>'
          +'<div class="slider-num">'
            +'<input type="range" id="calExit'+p.id+'" min="0" max="1023" step="1" value="'+p.exitRssi+'" oninput="syncCalib('+p.id+',\'exit\')">'
            +'<input type="number" id="calExitN'+p.id+'" min="0" max="1023" step="1" value="'+p.exitRssi+'" onchange="syncCalibN('+p.id+',\'exit\')">'
            +'<span class="unit"></span>'
          +'</div></div>'
      +'</div>';
    g.appendChild(d);
    p.calRssiEl=null;
    setTimeout(()=>initChart(p.id),0);
  });
}

function initChart(id){
  var cv=document.getElementById('calCanvas'+id);if(!cv)return;
  cv.width = cv.offsetWidth || cv.parentElement.offsetWidth || 300;
  cv.height = 130;
  var col=getComputedStyle(document.documentElement).getPropertyValue('--p'+id).trim();
  charts[id]={cv,ctx:cv.getContext('2d'),data:new Float32Array(200).fill(0),cross:new Uint8Array(200),col:col};
  drawChart(id);
}

function pushChart(id,rssi,crossing){
  var c=charts[id];if(!c)return;
  c.data.copyWithin(0,1);c.data[199]=rssi;c.cross.copyWithin(0,1);c.cross[199]=crossing?1:0;
  chartDirty[id]=true;
}

function calcChartRange(id){
  var c=charts[id];var p=slots[id];
  var hasData=false;var dMin=0;var dMax=0;
  for(var i=0;i<200;i++){
    if(c.data[i]>0){
      if(!hasData||c.data[i]<dMin)dMin=c.data[i];
      if(!hasData||c.data[i]>dMax)dMax=c.data[i];
      hasData=true;
    }
  }
  var allVals=hasData?[dMin,dMax,p.enterRssi,p.exitRssi]:[p.enterRssi,p.exitRssi];
  var yMin=Math.max(0,Math.min.apply(null,allVals)-40);
  var yMax=Math.min(RSSI_MAX,Math.max.apply(null,allVals)+40);
  if(yMax-yMin<160){var mid=(yMax+yMin)/2;yMin=mid-80;yMax=mid+80;}
  return {yMin,yMax};
}

function drawChart(id){
  var c=charts[id];if(!c)return;
  var cv=c.cv,ctx=c.ctx,w=cv.width,h=cv.height,p=slots[id];
  var range=calcChartRange(id);
  var yMin=range.yMin,yMax=range.yMax,rng=yMax-yMin;
  var toY=function(v){return h-((Math.max(yMin,Math.min(yMax,v))-yMin)/rng)*h;};
  var dx=w/200;
  var col=c.col||(c.col=getComputedStyle(document.documentElement).getPropertyValue('--p'+id).trim());
  ctx.fillStyle='#0d1117';ctx.fillRect(0,0,w,h);
  ctx.fillStyle=col+'22';
  for(var i=0;i<200;i++)if(c.cross[i])ctx.fillRect(i*dx,0,dx+1,h);
  ctx.strokeStyle='#1e2530';ctx.lineWidth=1;ctx.setLineDash([]);
  var tickStart=Math.ceil(yMin/100)*100;
  for(var db=tickStart;db<=yMax;db+=100){
    var ty=toY(db);
    ctx.beginPath();ctx.moveTo(0,ty);ctx.lineTo(w,ty);ctx.stroke();
    ctx.fillStyle='#444';ctx.font='9px monospace';ctx.fillText(String(db),2,ty-2);
  }
  ctx.setLineDash([4,4]);ctx.lineWidth=1;
  ctx.strokeStyle='#3fb95088';ctx.beginPath();ctx.moveTo(0,toY(p.enterRssi));ctx.lineTo(w,toY(p.enterRssi));ctx.stroke();
  ctx.strokeStyle='#f8514988';ctx.beginPath();ctx.moveTo(0,toY(p.exitRssi));ctx.lineTo(w,toY(p.exitRssi));ctx.stroke();
  ctx.setLineDash([]);
  ctx.beginPath();
  for(var j=0;j<200;j++){var x=j*dx,y=toY(c.data[j]);j===0?ctx.moveTo(x,y):ctx.lineTo(x,y);}
  ctx.lineTo(w,h);ctx.lineTo(0,h);ctx.closePath();
  ctx.fillStyle=col+'28';ctx.fill();
  ctx.strokeStyle=col;ctx.lineWidth=1.5;ctx.beginPath();
  for(var j=0;j<200;j++){var x=j*dx,y=toY(c.data[j]);j===0?ctx.moveTo(x,y):ctx.lineTo(x,y);}
  ctx.stroke();
}

function cap(s){return s.charAt(0).toUpperCase()+s.slice(1);}

function syncCalib(id,which){
  var v=parseInt(document.getElementById('cal'+cap(which)+id).value);
  document.getElementById('cal'+cap(which)+'N'+id).value=v;
  slots[id][which+'Rssi']=v;drawChart(id);
  scheduleCalibSave(id);
}
function syncCalibN(id,which){
  var el=document.getElementById('cal'+cap(which)+'N'+id);
  var v=Math.max(0,Math.min(RSSI_MAX,parseInt(el.value)||DEF_ENTER));
  el.value=v;document.getElementById('cal'+cap(which)+id).value=v;
  slots[id][which+'Rssi']=v;drawChart(id);
  scheduleCalibSave(id);
}

function scheduleCalibSave(slotId){
  if(calibSaveTimers[slotId])clearTimeout(calibSaveTimers[slotId]);
  calibSaveTimers[slotId]=setTimeout(()=>saveCalibConfig(slotId),800);
}

async function saveCalibConfig(slotId){
  var enter=parseInt(document.getElementById('calEnterN'+slotId).value);
  var exit_=parseInt(document.getElementById('calExitN'+slotId).value);
  if(enter<=exit_){toast('⚠️ Enter > Exit にしてください');return;}
  var ri=activeSlotsLocal[slotId];
  if(ri<0){return;}
  slots[slotId].enterRssi=enter;slots[slotId].exitRssi=exit_;
  try{
    var r=await fetch('/api/calib',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:ri,enter,exit:exit_})});
    if(r.ok){
      var rd=rosterById[ri];if(rd){rd.enter=enter;rd.exit=exit_;}
      // Unconditionally reflect the new thresholds in the pilot-info UI.
      renderRoster();
    }else toast('⚠️ 保存エラー');
  }catch(e){toast('⚠️ 接続エラー');}
}

function syncCalibSliders(id,enter,exit_){
  var eEl=document.getElementById('calEnter'+id),eN=document.getElementById('calEnterN'+id);
  var xEl=document.getElementById('calExit'+id),xN=document.getElementById('calExitN'+id);
  if(eEl)eEl.value=enter;if(eN)eN.value=enter;if(xEl)xEl.value=exit_;if(xN)xN.value=exit_;
}

// ── Microphone (Sense board): level graph and the two thresholds ─────────
// Same look as a pilot's chart: the level (0-100, 10 per second from the
// telemetry) over the last 20 s, the start threshold green, the false
// start one red.
var micChart=null,micDirty=false,micCfgLocal={start:false,falseStart:false,startCount:2,clapClear:false,startLvl:80,fsLvl:80,startHoldMs:100,fsHoldMs:100};
function initMicChart(){
  var cv=document.getElementById('micCanvas');if(!cv)return;
  cv.width=cv.offsetWidth||300;cv.height=150;
  micChart={cv:cv,ctx:cv.getContext('2d'),data:new Float32Array(200).fill(0)};
  drawMic();
}
function pushMic(lvl){
  if(!micChart)return;
  micChart.data.copyWithin(0,1);micChart.data[199]=lvl;micDirty=true;
  if(activeTab==='calib'){var n=document.getElementById('micNow');if(n)n.textContent=lvl;}
}
(function micLoop(){
  requestAnimationFrame(micLoop);
  if(activeTab!=='calib'||!micDirty)return;
  micDirty=false;drawMic();
})();
function drawMic(){
  var c=micChart;if(!c)return;
  var cv=c.cv;if(cv.offsetWidth&&cv.offsetWidth!==cv.width)cv.width=cv.offsetWidth;
  var ctx=c.ctx,w=cv.width,h=cv.height,toY=v=>h-(Math.max(0,Math.min(100,v))/100)*h,dx=w/200;
  ctx.fillStyle='#0d1117';ctx.fillRect(0,0,w,h);
  ctx.strokeStyle='#1e2530';ctx.lineWidth=1;ctx.setLineDash([]);
  for(var t=20;t<100;t+=20){var ty=toY(t);ctx.beginPath();ctx.moveTo(0,ty);ctx.lineTo(w,ty);ctx.stroke();
    ctx.fillStyle='#444';ctx.font='9px monospace';ctx.fillText(String(t),2,ty-2);}
  ctx.setLineDash([4,4]);
  ctx.strokeStyle=micCfgLocal.start?'#3fb950':'#3fb95055';ctx.beginPath();ctx.moveTo(0,toY(micCfgLocal.startLvl));ctx.lineTo(w,toY(micCfgLocal.startLvl));ctx.stroke();
  ctx.strokeStyle=micCfgLocal.falseStart?'#f85149':'#f8514955';ctx.beginPath();ctx.moveTo(0,toY(micCfgLocal.fsLvl));ctx.lineTo(w,toY(micCfgLocal.fsLvl));ctx.stroke();
  ctx.setLineDash([]);
  var col='#58a6ff';
  ctx.beginPath();for(var j=0;j<200;j++){var x=j*dx,y=toY(c.data[j]);j===0?ctx.moveTo(x,y):ctx.lineTo(x,y);}
  ctx.lineTo(w,h);ctx.lineTo(0,h);ctx.closePath();ctx.fillStyle=col+'28';ctx.fill();
  ctx.strokeStyle=col;ctx.lineWidth=1.5;ctx.beginPath();
  for(var k=0;k<200;k++){var x2=k*dx,y2=toY(c.data[k]);k===0?ctx.moveTo(x2,y2):ctx.lineTo(x2,y2);}
  ctx.stroke();
}
function showMicCfg(){
  var c=micCfgLocal,set=(id,v)=>{var e=document.getElementById(id);if(e)e.value=v;};
  var st=document.getElementById('micStart');if(st)st.checked=!!c.start;
  var fs=document.getElementById('micFs');if(fs)fs.checked=!!c.falseStart;
  var a=document.getElementById('micStartState');if(a)a.textContent=c.start?'ON':'OFF';
  var b=document.getElementById('micFsState');if(b)b.textContent=c.falseStart?'ON':'OFF';
  var cc=document.getElementById('micClapClear');if(cc)cc.checked=!!c.clapClear;
  var ccs=document.getElementById('micClapClearState');if(ccs)ccs.textContent=c.clapClear?'ON':'OFF';
  set('micStartCount',c.startCount);
  set('micStartLvl',c.startLvl);set('micStartLvlN',c.startLvl);
  set('micFsLvl',c.fsLvl);set('micFsLvlN',c.fsLvl);
  set('micStartHold',String(c.startHoldMs));set('micFsHold',String(c.fsHoldMs));
  drawMic();
}
async function loadMic(){
  try{
    var r=await fetch('/api/mic');if(!r.ok)return;
    var d=await r.json();
    micCfgLocal={start:!!d.start,falseStart:!!d.falseStart,startCount:d.startCount||2,clapClear:!!d.clapClear,startLvl:d.startLvl,fsLvl:d.fsLvl,startHoldMs:d.startHoldMs,fsHoldMs:d.fsHoldMs};
    // Only with the Sense board's microphone: otherwise the section stays hidden.
    var sec=document.getElementById('micSection');if(sec)sec.style.display=d.ok?'':'none';
    if(!d.ok)return;
    if(!micChart)initMicChart();
    showMicCfg();
  }catch(e){}
}
function syncMic(w){
  var v=parseInt(document.getElementById('mic'+w+'Lvl').value,10);
  document.getElementById('mic'+w+'LvlN').value=v;
  micCfgLocal[w==='Start'?'startLvl':'fsLvl']=v;drawMic();saveMicSoon(800);
}
function syncMicN(w){
  var el=document.getElementById('mic'+w+'LvlN'),v=Math.max(0,Math.min(100,parseInt(el.value,10)||0));
  el.value=v;document.getElementById('mic'+w+'Lvl').value=v;
  micCfgLocal[w==='Start'?'startLvl':'fsLvl']=v;drawMic();saveMicSoon(0);
}
var micSaveT=null;
function saveMicSoon(ms){
  micCfgLocal.start=document.getElementById('micStart').checked;
  micCfgLocal.falseStart=document.getElementById('micFs').checked;
  micCfgLocal.clapClear=document.getElementById('micClapClear').checked;
  var cn=document.getElementById('micStartCount');
  micCfgLocal.startCount=Math.max(1,Math.min(9,parseInt(cn.value,10)||2));cn.value=micCfgLocal.startCount;
  micCfgLocal.startHoldMs=parseInt(document.getElementById('micStartHold').value,10)||100;
  micCfgLocal.fsHoldMs=parseInt(document.getElementById('micFsHold').value,10)||100;
  showMicCfg();
  clearTimeout(micSaveT);
  micSaveT=setTimeout(async()=>{
    try{
      var r=await fetch('/api/mic',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(micCfgLocal)});
      toast(r.ok?'✓ マイクの設定を保存しました':'⚠️ 保存エラー');
    }catch(e){toast('⚠️ 接続エラー');}
  },ms);
}
