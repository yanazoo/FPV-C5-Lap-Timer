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
// Timed race callouts on every browser, from the shared race clock: "残り
// 30秒", "残り20秒", then 10, 9 ... 1 each second, and a fanfare at 0. Only
// what is crossed while this browser's clock runs (opening or resuming
// mid-race doesn't replay the earlier ones).
var calloutPrevLeft=null;
function timedCallouts(el){
  if(rankMode!==2){calloutPrevLeft=null;return;}
  var left=timeLimitSec*1000-el,prev=calloutPrevLeft;
  calloutPrevLeft=left;
  if(prev===null||left>=prev)return;
  var crossed=t=>prev>t&&left<=t;
  if(crossed(0)){ensureAudio();sfx.fanfare();return;}
  for(var n=10;n>=1;n--)if(crossed(n*1000)){sayNow(String(n));return;}
  if(crossed(20000)){sayNow('残り20秒');return;}
  if(crossed(30000)&&timeLimitSec>30){sayNow('残り30秒');return;}
}
function runTimer(){
  if(countdownH){clearInterval(countdownH);countdownH=null;}
  timerEl.classList.add('running');
  if(timerH)clearInterval(timerH);
  calloutPrevLeft=null;
  var tick=()=>{
    var el=Math.max(0,serverNow()-raceStartSrv);
    timerEl.textContent=raceClockText(el);
    timedCallouts(el);
  };
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
        +'<button class="lap-btn" id="rcLapBtn'+p.id+'" onclick="manualLap('+p.id+',this)" title="この時点でラップを記録します（管理者・レース中）">+LAP</button>'
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
  setBtns(raceRunning);   // the new cards' lap buttons
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

function lapRowEl(r,isBest,idx){
  var tr=document.createElement('tr');
  if(isBest)tr.className='best-row';
  tr.dataset.idx=idx;
  if(isAuthed)tr.classList.add('deletable');
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
    tbody.appendChild(lapRowEl(r,isBest,i));
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
  tbody.insertBefore(lapRowEl(r,isBest,p.rows.length-1),tbody.firstChild);
  showDelta(p,lapMs);
  if(quiet)return;
  var card=e?e.card:document.getElementById('rc-'+p.id);
  card.classList.remove('flash');void card.offsetWidth;card.classList.add('flash');
  card.addEventListener('animationend',()=>card.classList.remove('flash'),{once:true});
}
// ── Deleting a wrong lap ────────────────────────────────────────────────
// Tap a lap row (logged in): confirm, and the timer deletes that pass. Its
// time joins the next lap (the total stays), the last pass is just dropped;
// every browser reloads the laps, the goggles get the new lap count and best.
var lapDel=null;
function lapLabel(p,i){
  if(lapMode==='immediate')return (i+1)+'周目';
  return i===0?'ホールショット':i+'周目';
}
function openLapDelete(p,i){
  var r=p.rows[i];if(!r)return;
  if(!isAuthed){toast('🔒 ラップの削除にはログインが必要です');return;}
  lapDel={slot:p.id,index:i,lapTime:r.lapMs};
  var nm=p.name==='---'?'Ch'+(p.id+1):p.name;
  document.getElementById('lapDelWhat').textContent=nm+'　'+lapLabel(p,i)+'　'+fmt(r.lapMs);
  var next=p.rows[i+1],how;
  if(!next)how='この周を取り消します。次の周は、ひとつ前の通過から数えます。';
  else if(lapMode!=='immediate'&&i===0)how='この通過を取り消し、次の通過をホールショットにします。';
  else how='この通過をなかったことにして、次の周（'+lapLabel(p,i+1)+' '+fmt(next.lapMs)+'）と合わせて 1 周 '+fmt(r.lapMs+next.lapMs)+' にします。トータルタイムは変わりません。';
  document.getElementById('lapDelHow').textContent=how+' 全ブラウザ・順位表・ゴーグルの表示に反映します。';
  document.getElementById('lapDelModal').style.display='flex';
}
function closeLapDelete(){lapDel=null;document.getElementById('lapDelModal').style.display='none';}
async function confirmLapDelete(){
  var d=lapDel;closeLapDelete();if(!d)return;
  try{
    var r=await fetch('/api/laps/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});
    if(r.ok)toast('🗑 ラップを削除しました');
    else{toast('⚠ ラップ一覧が更新されています。もう一度選んでください');syncLaps();}
  }catch(e){toast('⚠ 接続エラー');}
}
// One listener per card body (rows are redrawn often).
document.addEventListener('click',ev=>{
  var tr=ev.target.closest&&ev.target.closest('tbody tr[data-idx]');if(!tr)return;
  var m=tr.closest('[id^="lapBody"]');
  var s=m?parseInt(m.id.slice(7)):-1;
  if(s<0||!slots[s])return;
  openLapDelete(slots[s],parseInt(tr.dataset.idx));
});

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
// The timer runs the race and tells every browser over the WebSocket:
// race_ready ("レディ"), race_stage (3-2-1), race_start / race_resume,
// race_stop. The buttons only ask the timer, so the browser that pressed
// Start, every other browser and the goggles' OSD all follow one sequence.
// A lost message is put right by the 1 Hz race status (reconcileRace).
var startWatchdog=null;
function clearStartWatchdog(){if(startWatchdog){clearTimeout(startWatchdog);startWatchdog=null;}}

function onRaceStart(ts){
  clearStartWatchdog();
  raceRunning=true;raceStarted=true;setBtns(true);startTimer(ts);
  lapEpoch++;resetLaps();
}
function onRaceResume(ts){
  clearStartWatchdog();
  raceRunning=true;raceStarted=true;setBtns(true);resumeTimer(ts);
}
// A start under way: "レディ" on every browser, then race_stage's countdown.
var readyUntil=0;   // until then the 1 Hz status mustn't undo the start
function onRaceReady(){
  clearStartWatchdog();
  if(countdownH)return;
  readyUntil=performance.now()+2500;
  ['btnStart','btnClear'].forEach(id=>{var b=document.getElementById(id);if(b)b.disabled=true;});
  timerEl.classList.remove('running');
  announceStart();
}
// The stop tone on every browser (Stop pressed elsewhere, or the race
// director); the browser that pressed Stop has already played it.
var stopToneAt=-1e9;
function onRaceStop(elapsed){
  if(raceRunning&&performance.now()-stopToneAt>2000){ensureAudio();sfx.stop();}
  if(countdownH){clearInterval(countdownH);countdownH=null;}   // stopped before the start
  raceRunning=false;setBtns(false);stopTimer(elapsed);
}

function startRace(){
  ensureAudio();warmSpeech();
  ['btnStart','btnClear'].forEach(id=>{var b=document.getElementById(id);if(b)b.disabled=true;});
  var fail=()=>{setBtns(raceRunning);toast('⚠️ スタート 通信エラー');};
  fetch('/api/race/start',{method:'POST'})
    .then(r=>{if(!r.ok)throw 0;
      // Nothing came back (no race_ready / race_resume): give the buttons back.
      clearStartWatchdog();
      startWatchdog=setTimeout(()=>{if(!raceRunning&&!countdownH)setBtns(raceRunning);},3000);
    })
    .catch(fail);
}
// 3-2-1 before the start, on race_stage (every browser).
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
      // No start followed (stopped meanwhile): give the buttons back.
      setTimeout(()=>{if(!raceRunning&&!countdownH)setBtns(raceRunning);},2500);}
  },50);
}
function stopRace(){
  ensureAudio();sfx.stop();stopToneAt=performance.now();
  fetch('/api/race/stop',{method:'POST'}).catch(()=>toast('⚠️ ストップ 通信エラー'));
}
async function clearAllLaps(){
  // Always ask the timer: it clears every browser (race_clear), even when
  // there are no laps yet (a stopped race with its clock still showing).
  var hasData=slots.some(p=>p.lapCount>0);
  try{
    var r=await fetch('/api/race/save',{method:'POST'});
    if(r.ok){
      var j=await r.json();
      if(j.saved)toast('✅ レース結果をSDに保存しました');
      else if(j.reason==='off')toast('ℹ️ SDログOFF — 未保存のままクリアしました');
      else if(j.reason==='empty'){if(hasData)toast('記録なし');}
      else toast('⚠️ SDカードなし — 未保存のままクリアしました',3500);
    }
    else toast('⚠️ SD保存エラー');
  }catch(e){toast('⚠️ 接続エラー — クリアできませんでした');return;}
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
  // Lap buttons on the race cards: admins, while the race runs.
  document.querySelectorAll('.lap-btn').forEach(b=>b.disabled=locked||!running);
}
// Record a lap for this pilot now (a gate pass missed by the receiver).
async function manualLap(slot,btn){
  if(!isAuthed||!raceRunning)return;
  if(btn){btn.classList.add('hit');setTimeout(()=>btn.classList.remove('hit'),300);}
  try{
    var r=await fetch('/api/laps/manual',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slot:slot})});
    if(!r.ok)toast('⚠ レース中だけ記録できます');
  }catch(e){toast('⚠ 接続エラー');}
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
