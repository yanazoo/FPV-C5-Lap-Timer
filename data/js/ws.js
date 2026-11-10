'use strict';

var wsConn=null,wsRetry=1000,wsLastMsg=0;
var wsDot=document.getElementById('wsDot');

function wsConnect(){
  var c=new WebSocket('ws://'+location.host+'/ws');
  wsConn=c;wsLastMsg=performance.now();
  c.onopen=()=>{wsRetry=1000;wsLastMsg=performance.now();wsDot.className='ws-dot connected';sendHello();};
  c.onerror=()=>c.close();
  c.onclose=ev=>{
    if(wsConn!==c)return;   // replaced already
    wsDot.className='ws-dot disconnected';
    // 4001: the timer closed this viewer (more viewers than allowed). Don't
    // come straight back (that would push another viewer out): ask first.
    if(ev&&ev.code===4001){wsConn=null;showKicked();return;}
    setTimeout(()=>{wsRetry=Math.min(wsRetry*2,4000);wsConnect();},wsRetry);
  };
  c.onmessage=e=>{wsLastMsg=performance.now();var d;try{d=JSON.parse(e.data);}catch(ex){return;}onMsg(d);};
}
// Tell the timer who this browser is: a logged-in admin (token) gets RSSI
// telemetry and keeps its connection; the timer takes its date from admins
// only (it has no clock of its own).
function sendHello(){
  if(!wsConn||wsConn.readyState!==1)return;
  try{wsConn.send(JSON.stringify({type:'hello',token:authToken,epoch:Date.now(),tz:-new Date().getTimezoneOffset()}));}catch(e){}
}
function showKicked(){
  var w=document.getElementById('kickedWhy');
  if(w)w.textContent='観戦の接続台数が上限に達したため、古い接続から切断しました。';
  var m=document.getElementById('kickedModal');if(m)m.style.display='flex';
}
function wsReconnectNow(){
  var m=document.getElementById('kickedModal');if(m)m.style.display='none';
  wsRetry=1000;wsConnect();
}
// The timer sends its status every second: three seconds of silence means
// the connection is stuck (WiFi hiccup, phone woke from sleep). Reconnect
// instead of showing a frozen race.
setInterval(()=>{
  if(wsConn&&wsConn.readyState===1&&performance.now()-wsLastMsg>3000){try{wsConn.close();}catch(e){}}
},1000);
// The timer's answer to hello: a token it doesn't know (it restarted, or the
// login lapsed) leaves this tab a viewer.
function onHelloReply(d){
  if(d.admin||!isAuthed)return;
  dropAuth();
  if(activeTab!=='race')switchTab('race');
  toast('🔒 ログインが切れました。もう一度ログインしてください');
}

// The race as the timer sees it (1 Hz status): put this browser right if it
// missed a start, stop, clear or lap.
var seenEpoch=null;
function reconcileRace(r){
  noteServerTime(r.now);
  // A delayed start under way (joined late, or race_wait lost).
  if(r.wait>0&&!waitH&&!countdownH&&!raceRunning&&performance.now()>=readyUntil){onRaceWait(r.wait/1000,true);return;}
  if(waitH){
    if(!r.wait&&!r.run&&performance.now()>waitEnd+2000)endWait();   // called off elsewhere, race_stop lost
    else return;
  }
  if(countdownH||performance.now()<readyUntil)return;   // a start is under way
  var epochChanged=seenEpoch!==null&&r.epoch!==seenEpoch;
  seenEpoch=r.epoch;
  if(r.run&&!raceRunning){
    raceRunning=true;raceStarted=true;setBtns(true);resumeTimer(r.now-r.el);
  }else if(!r.run&&raceRunning){
    raceRunning=false;setBtns(false);stopTimer(r.el);
  }else if(!r.run&&!r.paused&&(epochChanged||timerFrozenMs||raceStarted)&&r.laps===0){
    onRaceClear();   // cleared elsewhere
  }
  var mine=slots.reduce((n,p)=>n+p.lapTimes.length,0);
  if(epochChanged||mine!==r.laps)syncLaps();
}

function onMsg(d){
  if(d.type==='init'){
    raceRunning=d.raceRunning||false;
    raceStarted=!!(d.raceRunning||d.racePaused||(d.pilots||[]).some(pd=>pd.lapCount>0));
    setBtns(raceRunning);
    srvOff=null;   // a new connection: measure the timer's clock afresh (it may have restarted)
    noteServerTime(d.now);
    timerFrozenMs=d.raceElapsedMs||0;
    if(raceRunning)resumeTimer(d.now!==undefined?d.raceStartMs:undefined);
    else{
      // Not running on the timer: stop a clock this browser was still running.
      if(timerH){clearInterval(timerH);timerH=null;timerEl.classList.remove('running');}
      timerEl.textContent=raceClockText(d.racePaused?timerFrozenMs:0);
      if(!d.racePaused)timerFrozenMs=0;
    }
    if(d.sdPresent!==undefined)updateSdSection(d.sdPresent);
    applySettings(d);
    (d.pilots||[]).forEach(pd=>{
      var p=slots[pd.slot];if(!p)return;
      p.name=pd.name||'---';p.yomi=pd.yomi||'';p.rosterIdx=pd.rosterIdx;p.freq=pd.freq||0;
      p.rssi=pd.rssi||0;p.crossing=pd.crossing||false;
      if(pd.enter!==undefined){p.enterRssi=pd.enter;syncCalibSliders(pd.slot,pd.enter,p.exitRssi);}
      if(pd.exit!==undefined){p.exitRssi=pd.exit;syncCalibSliders(pd.slot,p.enterRssi,pd.exit);}
      activeSlotsLocal[pd.slot]=pd.rosterIdx;
      pushChart(p.id,p.rssi,p.crossing);
    });
    buildRaceCards();slots.forEach(p=>updateRaceCard(p));
    syncLaps();
    return;
  }
  if(d.type==='settings'){applySettings(d);return;}
  if(d.type==='hello'){onHelloReply(d);return;}
  if(d.type==='race_wait'){onRaceWait(d.sec,false);return;}
  if(d.type==='calib'){
    // Thresholds saved on some browser (this one included).
    var rd=rosterById[d.id];if(rd){rd.enter=d.enter;rd.exit=d.exit;}
    slots.forEach(p=>{if(p.rosterIdx===d.id){p.enterRssi=d.enter;p.exitRssi=d.exit;
      if(!editing('pane-calib'))syncCalibSliders(p.id,d.enter,d.exit);}});
    if(!editing('pane-config')&&typeof renderRoster==='function'&&editingRosterId===null)renderRoster();
    return;
  }
  if(d.type==='config_changed'){
    // Another browser saved these settings: reload the forms (not mid-edit).
    if(d.what==='elrs'&&typeof loadElrsSettings==='function'&&!editing('pane-config'))loadElrsSettings();
    if(d.what==='net'&&typeof loadNetSettings==='function'&&!editing('pane-config'))loadNetSettings();
    return;
  }
  if(d.type==='active_update'){
    (d.pilots||[]).forEach(pd=>{
      var p=slots[pd.slot];if(!p)return;
      p.name=pd.name||'---';p.yomi=pd.yomi||'';p.rosterIdx=pd.rosterIdx;p.freq=pd.freq||0;
      if(pd.enter!==undefined){p.enterRssi=pd.enter;syncCalibSliders(pd.slot,pd.enter,p.exitRssi);}
      if(pd.exit!==undefined){p.exitRssi=pd.exit;syncCalibSliders(pd.slot,p.enterRssi,pd.exit);}
      activeSlotsLocal[pd.slot]=pd.rosterIdx;
      updateRaceCard(p);
    });
    buildRaceCards();buildCalibCards();
    loadRoster();
    return;
  }
  if(d.type==='c5'){
    if(d.race)reconcileRace(d.race);
    c5Status=d;updateHeaderStatus();
    if(activeTab==='config')renderC5Status();
    return;
  }
  if(d.type==='rssi_all'){
    noteServerTime(d.ts);
    (d.p||[]).forEach(x=>{x.type='rssi';x.ts=d.ts;onMsg(x);});
    return;
  }
  if(d.type==='rssi'){
    var s=d.pilot;if(s<0||s>=N)return;
    var p=slots[s];
    var prevCrossing=p.crossing;
    p.rssiSignal=d.signal!==false;
    p.rssi=p.rssiSignal?(d.rssi!==undefined?d.rssi:p.rssi):0;
    p.crossing=p.rssiSignal&&(d.crossing!==undefined?d.crossing:p.crossing);
    // Enter/exit cues — audio is tab-independent during a race or calibration.
    // The firmware counts every gate entry and exit (en / ex); a crossing
    // shorter than the 10 Hz telemetry would never show up as a change of
    // the crossing flag, so the tones follow the counters instead.
    var tonesOn=raceRunning||activeTab==='calib';
    if(d.en!==undefined&&d.ex!==undefined){
      var newEnter=p.enCnt!==undefined&&((d.en-p.enCnt)&0xFFFF)>0;
      var newExit=p.exCnt!==undefined&&((d.ex-p.exCnt)&0xFFFF)>0;
      p.enCnt=d.en;p.exCnt=d.ex;
      if(tonesOn&&(newEnter||newExit)){
        ensureAudio();
        if(newEnter)sfx.enter(s);
        // Both in one sample: a short pass. Let the enter tone be heard first.
        if(newExit){if(newEnter)setTimeout(()=>sfx.exit(s),120);else sfx.exit(s);}
      }
    }else if(tonesOn){   // older firmware: only the flag
      if(!prevCrossing&&p.crossing){ensureAudio();sfx.enter(s);}
      if(prevCrossing&&!p.crossing){ensureAudio();sfx.exit(s);}
    }
    // Per-tab DOM work: only touch the widgets of the visible tab. The hot
    // RSSI path (up to 80 msgs/s with 8 slots) skips everything off-screen.
    if(activeTab==='race'){
      updateRaceCard(p);
    } else if(activeTab==='calib'){
      var calR=p.calRssiEl||(p.calRssiEl=document.getElementById('calRssi'+s));
      if(calR){var cv=p.rssiSignal?p.rssi:'---';if(calR._v!==cv){calR.textContent=cv;calR._v=cv;}}
      pushChart(s,p.rssiSignal?p.rssi:0,p.crossing);
    }
    return;
  }
  if(d.type==='lap'){
    if(lapQueue){lapQueue.push(d);return;}   // the lap list is being fetched
    applyLap(d,false);
    return;
  }
  if(d.type==='race_ready'){if(!raceRunning)onRaceReady();return;}
  if(d.type==='race_stage'){if(!raceRunning)showCountdown();return;}
  if(d.type==='race_clear'){onRaceClear();return;}
  if(d.type==='laps_changed'){lapEpoch++;if(lapQueue)setTimeout(syncLaps,400);else syncLaps();return;}   // a lap was deleted
  if(d.type==='time_up'){toast('⏱ 時間切れ');return;}
  if(d.type==='race_start'){onRaceStart(d.ts);return;}
  if(d.type==='race_resume'){onRaceResume(d.ts);return;}
  if(d.type==='race_stop'){onRaceStop(d.elapsed);return;}
  if(d.type==='sd_status'){updateSdSection(d.present);return;}
}

// Header: C5 receiver and ELRS link at a glance.
function updateHeaderStatus(){
  var el=document.getElementById('c5Badge');if(!el||!c5Status)return;
  var c=c5Status,e=c.elrs||{};
  var ok=c.online&&c.scanning;
  // One lamp per link: green when connected, red when not.
  function lamp(name,on){return '<span class="st '+(on?'ok':'ng')+'">'+name+'<i>●</i></span>';}
  el.innerHTML=lamp('C5',ok)+(e.enabled?lamp('OSD',!!e.connected):'');
  el.className='c5-badge '+(ok&&(!e.enabled||e.connected)?'ok':'ng');
  el.title='C5: '+(c.online?c.state:'OFFLINE')+(e.enabled?' / ELRS: '+(e.connected?e.peer:'未接続'):'');
}

async function loadRoster(){
  try{
    var r=await fetch('/api/pilots');if(!r.ok)return;
    rosterData=await r.json();
    rebuildRosterIndex();
    if(editingRosterId===null)renderRoster();
    applyActiveToSlots();
  }catch(e){}
}

async function loadActive(){
  try{
    var ra=await fetch('/api/active');if(!ra.ok)return;
    (await ra.json()).forEach(a=>{
      activeSlotsLocal[a.slot]=a.rosterIdx;
      var p=slots[a.slot];p.rosterIdx=a.rosterIdx;p.name=a.name||'---';p.yomi=a.yomi||'';p.freq=a.freq||0;
      if(a.enter!==undefined)p.enterRssi=a.enter;
      if(a.exit!==undefined)p.exitRssi=a.exit;
    });
  }catch(e){}
}

async function loadAll(){
  try{
    var rs=await fetch('/api/sd/status');
    if(rs.ok){var sd=await rs.json();updateSdSection(sd.present);}
  }catch(e){}

  await loadActive();
  await loadRoster();
  buildRaceCards();buildCalibCards();
  slots.forEach(p=>syncCalibSliders(p.id,p.enterRssi,p.exitRssi));

  // The timer's settings are the same for every browser: take them from it.
  try{
    var rg=await fetch('/api/settings');
    if(rg.ok)applySettings(await rg.json());
  }catch(e){}

  var am=document.getElementById('announceMode');if(am)am.value=announceMode;
  var sr=document.getElementById('speechRate');if(sr)sr.value=speechRate;
  var srN=document.getElementById('speechRateN');if(srN)srN.value=speechRate;
  refreshVoiceBtns();
  slots.forEach(p=>updateRaceCard(p));
}

// ── Keeping every browser the same ──────────────────────────────────────
// The timer is the one source of truth: laps, race state and settings come
// from it (init, lap, race_*, settings), so all browsers show the same
// thing, including one opened or reconnected in the middle of a race.

var lapQueue=null;   // laps that arrive while the lap list is being fetched

// One lap, live (sound and voice) or replayed from the lap list (quiet).
function applyLap(d,quiet){
  var s=d.pilot;if(s<0||s>=N)return;
  var p=slots[s];
  if(d.ts){if(p.lastTs&&d.ts<=p.lastTs)return;p.lastTs=d.ts;}   // already shown
  if(d.name&&d.name!=='---')p.name=d.name;
  if(d.yomi!==undefined)p.yomi=d.yomi;
  p.lapCount=d.lapCount!==undefined?d.lapCount:p.lapCount+1;
  var lapMs=d.lapTime||0;
  // Defense in depth: never let an implausible lap time (corrupted ts /
  // unsigned-wrap from any source) poison the cumulative or best display.
  if(lapMs<0||lapMs>3600000)lapMs=0;
  p.lapTimes.push(lapMs);
  var _isHS=(lapMode==='holeshot'&&p.lapCount===1);
  var _cum;if(_isHS){_cum=lapMs;}else{p.cumulative+=lapMs;_cum=p.cumulative;}
  // Firmware is authoritative for best lap (it knows the HS exclusion).
  if(d.bestLap!==undefined)p.bestLapMs=d.bestLap;
  else if(!_isHS&&lapMs>0&&(!p.bestLapMs||lapMs<p.bestLapMs))p.bestLapMs=lapMs;
  if(d.finished)p.done=true;
  updateRaceCard(p);addLapRow(p,lapMs,_cum,quiet);
  renderRanking();
  if(quiet)return;
  checkFinished(p,d.finished);
  // No lap tone: a lap is recorded as the pilot leaves the gate, so the
  // gate exit tone (RotorHazard style) marks it, and the voice calls it.
  // A best-lap fanfare (the same "piro" notes) is gone too.
  speak(buildSpeech(p,p.lapCount,lapMs));
}

// Take the race's laps from the timer and redraw them (after connecting or
// reconnecting, which may be mid-race or after laps this browser missed).
async function syncLaps(){
  if(lapQueue)return;
  lapQueue=[];
  var epoch=lapEpoch,list=null;
  try{var r=await fetch('/api/laps');if(r.ok)list=await r.json();}catch(e){}
  var q=lapQueue;lapQueue=null;
  if(list&&epoch===lapEpoch){   // no Start / Clear meanwhile
    resetLaps();
    var n={};
    list.forEach(l=>{n[l.slot]=(n[l.slot]||0)+1;
      applyLap({pilot:l.slot,lapTime:l.lapTime,lapCount:n[l.slot],ts:l.ts},true);});
  }
  q.forEach(d=>applyLap(d,false));
  if(!list)setTimeout(syncLaps,2000);
}

function setField(id,v){var e=document.getElementById(id);if(e&&document.activeElement!==e)e.value=v;}
// Is the user typing in this pane? (then don't overwrite the form)
function editing(paneId){
  var a=document.activeElement,p=document.getElementById(paneId);
  return !!(a&&p&&p.contains(a)&&/^(INPUT|SELECT|TEXTAREA)$/.test(a.tagName));
}
// Race settings from the timer (init, /api/settings, or a settings broadcast).
function applySettings(st){
  if(st.lapMode!==undefined){lapMode=st.lapMode===1?'immediate':'holeshot';localStorage.setItem('lapMode',lapMode);}
  if(st.cooldownMs!==undefined){cooldownMs=st.cooldownMs;localStorage.setItem('cooldownMs',cooldownMs);}
  if(st.sdLogMode!==undefined){sdLogMode=st.sdLogMode===2?'off':(st.sdLogMode===1?'rotate':'always');localStorage.setItem('sdLogMode',sdLogMode);}
  setField('lapModeSelect',lapMode);
  setField('sdLogModeSelect',sdLogMode);
  setField('cooldownInput',(cooldownMs/1000).toFixed(1));
  if(st.totalLaps!==undefined)setField('totalLaps',st.totalLaps);
  if(st.gain!==undefined)setField('gainInput',st.gain);
  if(st.rankMode!==undefined){rankMode=st.rankMode;timeLimitSec=st.timeLimitSec||120;countLastLap=st.countLastLap!==false;
    showRaceRules();renderRanking();}
  if(st.startDelayOn!==undefined)startDelayOn=!!st.startDelayOn;
  if(st.startDelaySec!==undefined){startDelaySec=st.startDelaySec||10;showStartDelay();}
  slots.forEach(p=>updateRaceCard(p));
}

// App init
buildRaceCards();
buildCalibCards();
applyAuthState();
wsConnect();
loadAll();
