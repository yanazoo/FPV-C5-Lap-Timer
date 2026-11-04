'use strict';

var timerEl=document.getElementById('timer');

// The race clock runs on the timer's own clock (millis()), not on when a
// browser happened to receive race_start: every browser shows the same
// time, whatever its network delay. srvOff = timer ms - performance.now(),
// from the timestamps in the 10 Hz telemetry; the largest value seen is the
// one with the least delay. It drifts down slowly (clocks differ by up to
// ~0.05 %) so a better sample can take over.
var srvOff=null,srvOffAt=0,raceStartSrv=0;
function noteServerTime(ts){
  if(!ts)return;
  var now=performance.now(),s=ts-now;
  if(srvOff===null||s>srvOff-(now-srvOffAt)*0.0005)srvOff=s;
  else srvOff-=(now-srvOffAt)*0.0005;
  srvOffAt=now;
}
function serverNow(){return performance.now()+(srvOff||0);}
function raceElapsedMs(){return timerH?serverNow()-raceStartSrv:timerFrozenMs;}
// What the big race clock shows for a race time: in a timed race (laps
// within a time limit) the time left, then the time over the limit as
// -mm:ss.cc in red; otherwise the time so far.
function raceClockText(el){
  if(rankMode!==2){timerEl.classList.remove('timeup');return fmtTimer(el);}
  var left=timeLimitSec*1000-el;
  timerEl.classList.toggle('timeup',left<=0&&el>0);
  if(left>=0)return fmtTimer(Math.ceil(left/10)*10);
  return '-'+fmtTimer(Math.floor(-left/10)*10);
}
function runTimer(){
  if(countdownH){clearInterval(countdownH);countdownH=null;}
  timerEl.classList.add('running');
  if(timerH)clearInterval(timerH);
  var tick=()=>{var el=Math.max(0,serverNow()-raceStartSrv);timerEl.textContent=raceClockText(el);updateRemain(el);};
  timerH=setInterval(tick,50);tick();
}
// startSrv: the race start on the timer's clock (race_start ts).
function startTimer(startSrv){
  timerFrozenMs=0;
  raceStartSrv=startSrv!==undefined&&srvOff!==null?startSrv:serverNow();
  runTimer();
}
// startSrv: race_resume's / init's raceStartMs (already moved past pauses).
function resumeTimer(startSrv){
  raceStartSrv=startSrv!==undefined&&srvOff!==null?startSrv:serverNow()-timerFrozenMs;
  runTimer();
}
// elapsed: race time at the stop, from the timer (race_stop elapsed).
function stopTimer(elapsed){
  if(timerH){timerFrozenMs=elapsed!==undefined?elapsed:serverNow()-raceStartSrv;clearInterval(timerH);timerH=null;}
  else if(elapsed!==undefined)timerFrozenMs=elapsed;
  timerEl.textContent=raceClockText(timerFrozenMs);
  timerEl.classList.remove('running');
}

function applyActiveToSlots(){
  for(var s=0;s<N;s++){
    var ri=activeSlotsLocal[s];
    var r=rosterById[ri];
    slots[s].rosterIdx=ri;
    slots[s].name=r?r.name:'---';
    slots[s].yomi=r?r.yomi:'';
    slots[s].freq=r?r.freq:0;
    slots[s].enterRssi=r?r.enter:DEF_ENTER;
    slots[s].exitRssi =r?r.exit :DEF_EXIT;
  }
}

// A channel is shown on the race tab only when a pilot with a VTX frequency
// is assigned to it.
function slotInUse(p){return p.rosterIdx>=0&&p.freq>0;}

function buildRaceCards(){
  var g=document.getElementById('pilotGrid');g.innerHTML='';
  var shown=0;
  slots.forEach(p=>{
    if(!slotInUse(p)){p.el=null;return;}
    shown++;
    var d=document.createElement('div');
    d.id='rc-'+p.id;d.className='pilot-race-card';
    d.innerHTML=
      '<div class="pilot-card-header '+PCLS[p.id]+'">'
        +'<span class="p-name" id="rcName'+p.id+'">'+esc(p.name==='---'?'Ch'+(p.id+1):p.name)+'</span>'
        +'<span class="p-freq" id="rcFreq'+p.id+'">'+(p.freq?esc(freqLabel(p.freq)):'')+'</span>'
        +'<span class="crossing-badge" id="rcBadge'+p.id+'">CROSSING</span>'
        +'<span class="p-lapcount" id="rcLaps'+p.id+'"></span>'
      +'</div>'
      +'<div class="rssi-mini-bar">'
        +'<span class="rssi-mini-label">RSSI</span>'
        +'<div class="rssi-mini-track"><div class="rssi-mini-fill '+PCLS[p.id]+'-fill" id="rssiBar'+p.id+'"></div></div>'
        +'<span class="rssi-mini-val" id="rssiVal'+p.id+'">---</span>'
      +'</div>'
      +'<div class="pilot-best-row">'
        +'<span class="best-label">BEST</span>'
        +'<span class="pilot-best" id="rcBest'+p.id+'">--:--.---</span>'
        +'<span class="pilot-delta" id="rcDelta'+p.id+'"></span>'
      +'</div>'
      +'<div class="lap-table-wrap">'
        +'<table class="lapTable"><thead><tr><th>周</th><th>タイム</th><th>累計</th></tr></thead>'
        +'<tbody id="lapBody'+p.id+'"></tbody></table>'
      +'</div>';
    g.appendChild(d);
    // Cache DOM refs once so the hot RSSI path avoids repeated getElementById
    p.el={
      card : d,
      name : document.getElementById('rcName'+p.id),
      laps : document.getElementById('rcLaps'+p.id),
      bar  : document.getElementById('rssiBar'+p.id),
      val  : document.getElementById('rssiVal'+p.id),
      best : document.getElementById('rcBest'+p.id),
      badge: document.getElementById('rcBadge'+p.id),
      delta: document.getElementById('rcDelta'+p.id),
      body : document.getElementById('lapBody'+p.id)
    };
  });
  var none=document.getElementById('noPilotsMsg');
  if(none)none.style.display=shown?'none':'block';
  // Rebuilding the cards (another browser assigned pilots) keeps the laps.
  slots.forEach(p=>renderLapRows(p));
  renderRanking();
}

function updateRaceCard(p){
  var e=p.el;if(!e)return;
  var nm=p.name==='---'?'Ch'+(p.id+1):p.name;
  if(e._name!==nm){e.name.textContent=nm;e._name=nm;}
  var _lbl=lapMode==='immediate'?(p.lapCount?p.lapCount+'周':''):(p.lapCount===1?'HS':p.lapCount?(p.lapCount-1)+'周':'');
  if(e._lbl!==_lbl){e.laps.textContent=_lbl;e._lbl=_lbl;}
  var _w=(p.rssiSignal?rssiPct(p.rssi):0);
  if(e._w!==_w){e.bar.style.width=_w+'%';e._w=_w;}
  var _val=p.rssiSignal?String(p.rssi):'---';
  if(e._val!==_val){e.val.textContent=_val;e._val=_val;}
  var _best=fmt(p.bestLapMs);
  if(e._best!==_best){e.best.textContent=_best;e._best=_best;}
  if(e._cross!==p.crossing){
    if(p.crossing){e.badge.style.display='inline-block';e.badge.style.color=PCOLORS[p.id];}
    else{e.badge.style.display='none';}
    e._cross=p.crossing;
  }
}

function lapRowEl(r,isBest){
  var tr=document.createElement('tr');
  if(isBest)tr.className='best-row';
  tr.innerHTML='<td>'+r.label+'</td><td class="lap-time">'+fmt(r.lapMs)+'</td><td>'+fmt(r.cum)+'</td>';
  return tr;
}
function showDelta(p,lapMs){
  var e=p.el,delta=e?e.delta:document.getElementById('rcDelta'+p.id);
  if(!delta)return;
  if(p.bestLapMs&&lapMs){var d=lapMs-p.bestLapMs;if(d===0){delta.textContent='★ BEST';delta.className='pilot-delta faster';}else{delta.textContent=fmtDelta(d);delta.className='pilot-delta '+(d<0?'faster':'slower');}}
  else{delta.textContent='';delta.className='pilot-delta';}
}
// Redraw a card's lap table from the slot's lap history (p.rows).
function renderLapRows(p){
  var e=p.el,tbody=e?e.body:document.getElementById('lapBody'+p.id);
  if(!tbody)return;
  tbody.innerHTML='';
  var bestDone=false;
  for(var i=p.rows.length-1;i>=0;i--){
    var r=p.rows[i];
    var isBest=!bestDone&&r.lapMs>0&&r.lapMs===p.bestLapMs&&r.label!=='HS';
    if(isBest)bestDone=true;
    tbody.appendChild(lapRowEl(r,isBest));
  }
  var last=p.rows[p.rows.length-1];
  showDelta(p,last?last.lapMs:0);
}
function addLapRow(p, lapMs, cumMs, quiet){
  var r={label:lapMode==='immediate'?p.lapCount:(p.lapCount===1?'HS':(p.lapCount-1)),lapMs:lapMs,cum:cumMs};
  p.rows.push(r);
  var e=p.el,tbody=e?e.body:document.getElementById('lapBody'+p.id);
  if(!tbody)return;   // channel not shown on the race tab
  var isBest=(p.bestLapMs===lapMs&&lapMs>0);
  if(isBest)tbody.querySelectorAll('tr.best-row').forEach(r=>r.classList.remove('best-row'));
  tbody.insertBefore(lapRowEl(r,isBest),tbody.firstChild);
  showDelta(p,lapMs);
  if(quiet)return;
  var card=e?e.card:document.getElementById('rc-'+p.id);
  card.classList.remove('flash');void card.offsetWidth;card.classList.add('flash');
  card.addEventListener('animationend',()=>card.classList.remove('flash'),{once:true});
}
// ── Ranking table ───────────────────────────────────────────────────────
// Every assigned pilot with best lap, laps and total time; rows ordered by
// the ranking rule from settings (rankMode), its column highlighted.
function completedLaps(p){return lapMode==='immediate'?p.lapCount:Math.max(0,p.lapCount-1);}
function fmtSec(s){s=Math.max(0,Math.ceil(s));return Math.floor(s/60)+':'+String(s%60).padStart(2,'0');}
var RANK_RULES=['周回数 → トータルタイム順','最速ラップ順','制限時間内の周回数 → 到達の早い順'];
function rankRows(){
  var rows=slots.filter(slotInUse).map(p=>({p:p,laps:completedLaps(p),best:p.bestLapMs||0,total:p.cumulative||0}));
  var byLaps=(a,b)=>(b.laps-a.laps)||((a.laps?a.total:0)-(b.laps?b.total:0))||(a.p.id-b.p.id);
  if(rankMode===1)rows.sort((a,b)=>((a.best?0:1)-(b.best?0:1))||(a.best-b.best)||byLaps(a,b));
  else rows.sort(byLaps);
  return rows;
}
function renderRanking(){
  var t=document.getElementById('rankTable');if(!t)return;
  var rule=document.getElementById('rankRule');
  if(rule)rule.textContent=RANK_RULES[rankMode]+(rankMode===2?'（制限 '+fmtSec(timeLimitSec)+(countLastLap?'・最終周あり':'・最終周なし')+'）':'');
  var rows=rankRows();
  var panel=document.getElementById('rankPanel');
  if(panel)panel.style.display=rows.length?'':'none';
  if(!rows.length){t.innerHTML='';return;}
  var lead=rows[0],key=rankMode===1?'best':'laps';
  var h='<thead><tr><th>順位</th><th class="l">パイロット</th>'
    +'<th class="'+(key==='best'?'key':'')+'">最速ラップ</th>'
    +'<th class="'+(key==='laps'?'key':'')+'">周回</th>'
    +'<th class="'+(key==='laps'?'key':'')+'">トータル</th><th>差</th></tr></thead><tbody>';
  var started=rows.some(r=>r.laps>0||r.best>0);
  rows.forEach((r,i)=>{
    var has=rankMode===1?r.best>0:r.laps>0;
    var gap='';
    if(i>0&&has){
      if(rankMode===1)gap='+'+((r.best-lead.best)/1000).toFixed(3);
      else if(r.laps<lead.laps)gap='-'+(lead.laps-r.laps)+'周';
      else gap='+'+((r.total-lead.total)/1000).toFixed(3);
    }
    var nm=r.p.name==='---'?'Ch'+(r.p.id+1):r.p.name;
    h+='<tr class="'+(started&&has&&i<3?'r'+(i+1):'')+'">'
      +'<td class="pos">'+(started&&has?(i+1):'–')+'</td>'
      +'<td class="name l"><span class="rank-dot" style="background:'+PCOLORS[r.p.id]+'"></span>'+esc(nm)+(r.p.done?' 🏁':'')+'</td>'
      +'<td class="'+(key==='best'?'key':'')+'">'+(r.best?fmt(r.best):'—')+'</td>'
      +'<td class="'+(key==='laps'?'key':'')+'">'+r.laps+'</td>'
      +'<td class="'+(key==='laps'?'key':'')+'">'+(r.laps?fmt(r.total):'—')+'</td>'
      +'<td class="gap">'+gap+'</td></tr>';
  });
  t.innerHTML=h+'</tbody>';
}
// Timed race: time left next to the rule, from the race clock.
function updateRemain(elapsedMs){
  var el=document.getElementById('rankRemain');if(!el)return;
  if(rankMode!==2){el.textContent='';return;}
  var left=timeLimitSec-(elapsedMs||0)/1000;
  var over=timeIsUp||left<=0;
  el.textContent=over?'時間切れ':'残り '+fmtSec(left);
  el.classList.toggle('over',over);
}

// Forget every lap on this browser (a new race, or Clear from any browser).
function resetLaps(){
  slots.forEach(p=>{
    p.lapCount=0;p.bestLapMs=0;p.lapTimes=[];p.cumulative=0;p.rows=[];p.lastTs=0;p.done=false;
    var e=p.el;
    var b=e?e.body:document.getElementById('lapBody'+p.id);if(b)b.innerHTML='';
    var d=e?e.delta:document.getElementById('rcDelta'+p.id);if(d)d.textContent='';
    updateRaceCard(p);
  });
  renderRanking();
}

// ── Race state transitions ──────────────────────────────────────────────
// The web node broadcasts race_start / race_resume / race_stop over the
// WebSocket, which is the normal trigger for these. Each Start/Stop also
// arms a short watchdog: if that WS message is ever lost, the watchdog
// applies the transition locally so the controls can never get stuck.
var raceStartSeq=0, raceStartApplied=-1, startWatchdog=null, stopWatchdog=null;
function clearStartWatchdog(){if(startWatchdog){clearTimeout(startWatchdog);startWatchdog=null;}}
function clearStopWatchdog(){if(stopWatchdog){clearTimeout(stopWatchdog);stopWatchdog=null;}}

function onRaceStart(ts){
  raceStartApplied=raceStartSeq;clearStartWatchdog();
  raceRunning=true;raceStarted=true;setBtns(true);startTimer(ts);
  lapEpoch++;resetLaps();
  timeIsUp=false;updateRemain(0);
}
function onRaceResume(ts){
  clearStartWatchdog();
  raceRunning=true;raceStarted=true;setBtns(true);resumeTimer(ts);
}
function onRaceStop(elapsed){
  clearStopWatchdog();
  raceRunning=false;setBtns(false);stopTimer(elapsed);
}

function startRace(){
  ensureAudio();
  var btnStart=document.getElementById('btnStart');
  var btnClear=document.getElementById('btnClear');
  btnStart.disabled=true;
  if(btnClear)btnClear.disabled=true;
  // Resume a paused race (Stop → Start) without countdown or reset.
  if(raceStarted && !raceRunning){
    fetch('/api/race/resume',{method:'POST'})
      .then(r=>{if(!r.ok)throw 0;
        clearStartWatchdog();
        startWatchdog=setTimeout(()=>{if(!raceRunning)onRaceResume();},1500);
      })
      .catch(e=>{btnStart.disabled=false;if(btnClear)btnClear.disabled=false;toast('⚠️ 再開 通信エラー');});
    return;
  }
  // "レディ" first; the countdown starts once it has been said.
  announceStart(beginCountdown);
}
function beginCountdown(){
  var btnStart=document.getElementById('btnStart');
  var btnClear=document.getElementById('btnClear');
  // Countdown start = RotorHazard "stage": ARM NOW on the goggles. The
  // firmware passes it on to every browser, which show the countdown too.
  showCountdown();
  fetch('/api/race/stage',{method:'POST'}).catch(()=>{});
  setTimeout(async()=>{
    raceStartSeq++;var mySeq=raceStartSeq;   // claim this start cycle
    try{
      var r=await fetch('/api/race/start',{method:'POST'});
      if(!r.ok)throw 0;
      clearStartWatchdog();
      startWatchdog=setTimeout(()=>{if(raceStartApplied!==mySeq)onRaceStart();},1500);
    }
    catch(e){btnStart.disabled=false;if(btnClear)btnClear.disabled=false;toast('⚠️ スタート 通信エラー');}
  },3300);
}
// 3-2-1 before the start; on the browser that pressed Start and, through
// race_stage, on every other one.
function showCountdown(){
  if(countdownH)return;   // already counting down
  ensureAudio();sfx.count();
  ['btnStart','btnClear'].forEach(id=>{var b=document.getElementById(id);if(b)b.disabled=true;});
  var cdStart=performance.now();
  timerEl.classList.remove('running');
  timerEl.textContent='3.00';
  countdownH=setInterval(()=>{
    var rem=Math.max(0,3000-(performance.now()-cdStart));
    timerEl.textContent=(rem/1000).toFixed(2);
    if(rem===0){clearInterval(countdownH);countdownH=null;
      // No start followed (the presser's request failed): give the buttons back.
      setTimeout(()=>{if(!raceRunning&&!countdownH)setBtns(raceRunning);},2500);}
  },50);
}
function stopRace(){
  ensureAudio();sfx.stop();
  fetch('/api/race/stop',{method:'POST'})
    .then(r=>{if(!r.ok)return;
      clearStopWatchdog();
      stopWatchdog=setTimeout(()=>{if(raceRunning)onRaceStop();},1500);
    })
    .catch(()=>{});
}
async function clearAllLaps(){
  var hasData=slots.some(p=>p.lapCount>0);
  if(hasData){
    try{
      var r=await fetch('/api/race/save',{method:'POST'});
      if(r.ok){
        var j=await r.json();
        if(j.saved)toast('✅ レース結果をSDに保存しました');
        else if(j.reason==='off')toast('ℹ️ SDログOFF — 未保存のままクリアしました');
        else if(j.reason==='empty')toast('記録なし');
        else toast('⚠️ SDカードなし — 未保存のままクリアしました',3500);
      }
      else toast('⚠️ SD保存エラー');
    }catch(e){toast('⚠️ SD保存 接続エラー');}
  }
  onRaceClear();
}
// Clear from any browser (race_clear) or this one.
function onRaceClear(){
  stopTimer();
  timerFrozenMs=0;
  timerEl.textContent=raceClockText(0);
  lapEpoch++;resetLaps();
  // Clear ends the race; next Start is a fresh race.
  raceStarted=false;
  setBtns(false);
}

function setBtns(running){
  // Viewers (unauthed) can never operate race controls regardless of state.
  var locked=!isAuthed;
  document.getElementById('btnStart').disabled=locked||running;
  document.getElementById('btnStop').disabled=locked||!running;
  // Clear is only allowed while stopped (not during a running race).
  var btnClear=document.getElementById('btnClear');
  if(btnClear)btnClear.disabled=locked||running;
}
// finished: the firmware's verdict for this lap (it knows the holeshot lap
// doesn't count). Without it, count laps the same way: the holeshot isn't a lap.
function checkFinished(p,finished){
  var tot=parseInt(document.getElementById('totalLaps').value)||0;
  if(finished===undefined){
    var laps=lapMode==='immediate'?p.lapCount:p.lapCount-1;
    finished=raceRunning&&tot>0&&laps===tot;
  }
  if(finished){
    ensureAudio();sfx.finish();
    if(voiceEnabled&&announceMode!=='beep'&&announceMode!=='none'){
      var u=new SpeechSynthesisUtterance(getSpokenName(p)+'、フィニッシュ');u.lang='ja-JP';u.rate=speechRate;speechSynthesis.speak(u);
    }
  }
}
