'use strict';

var wsConn=null,wsRetry=1000;
var wsDot=document.getElementById('wsDot');

function wsConnect(){
  wsConn=new WebSocket('ws://'+location.host+'/ws');
  wsConn.onopen=()=>{wsRetry=1000;wsDot.className='ws-dot connected';};
  wsConn.onerror=()=>wsConn.close();
  wsConn.onclose=()=>{wsDot.className='ws-dot disconnected';setTimeout(()=>{wsRetry=Math.min(wsRetry*2,16000);wsConnect();},wsRetry);};
  wsConn.onmessage=e=>{var d;try{d=JSON.parse(e.data);}catch(ex){return;}onMsg(d);};
}

function onMsg(d){
  if(d.type==='init'){
    raceRunning=d.raceRunning||false;
    raceStarted=!!(d.raceRunning||d.racePaused||(d.pilots||[]).some(pd=>pd.lapCount>0));
    setBtns(raceRunning);
    timerFrozenMs=d.raceElapsedMs||0;
    if(raceRunning)resumeTimer();
    else if(d.racePaused)timerEl.textContent=fmtTimer(timerFrozenMs);
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
    c5Status=d;updateHeaderStatus();
    if(activeTab==='config')renderC5Status();
    return;
  }
  if(d.type==='rssi_all'){
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
    if(d.name&&d.name!=='---')p.name=d.name;
    // Enter/exit cues — audio is tab-independent during a race or calibration.
    if(raceRunning||activeTab==='calib'){
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
  if(d.type==='gate_start'){
    var s=d.pilot;if(s<0||s>=N)return;
    var p=slots[s];
    if(d.name&&d.name!=='---')p.name=d.name;
    ensureAudio();sfx.enter(s);
    if(lapMode==='holeshot')speak(getSpokenName(p)+(announceMode==='lap_laptime'?'、ホールショット':''));
    updateRaceCard(p);
    return;
  }
  if(d.type==='lap'){
    if(lapQueue){lapQueue.push(d);return;}   // the lap list is being fetched
    applyLap(d,false);
    return;
  }
  if(d.type==='race_stage'){if(!raceRunning)showCountdown();return;}
  if(d.type==='race_clear'){onRaceClear();return;}
  if(d.type==='race_start'){onRaceStart();return;}
  if(d.type==='race_resume'){onRaceResume();return;}
  if(d.type==='race_stop'){onRaceStop();return;}
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
  updateRaceCard(p);addLapRow(p,lapMs,_cum,quiet);
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
  slots.forEach(p=>updateRaceCard(p));
}

// App init
buildRaceCards();
buildCalibCards();
applyAuthState();
wsConnect();
loadAll();
