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
    if(d.totalLaps!==undefined){var tl=document.getElementById('totalLaps');if(tl&&document.activeElement!==tl)tl.value=d.totalLaps;}
    (d.pilots||[]).forEach(pd=>{
      var p=slots[pd.slot];if(!p)return;
      p.name=pd.name||'---';p.yomi=pd.yomi||'';p.rosterIdx=pd.rosterIdx;p.freq=pd.freq||0;
      if(pd.lapCount)p.lapCount=pd.lapCount;
      if(pd.bestLap)p.bestLapMs=pd.bestLap;
      p.rssi=pd.rssi||0;p.crossing=pd.crossing||false;
      if(pd.enter!==undefined){p.enterRssi=pd.enter;syncCalibSliders(pd.slot,pd.enter,p.exitRssi);}
      if(pd.exit!==undefined){p.exitRssi=pd.exit;syncCalibSliders(pd.slot,p.enterRssi,pd.exit);}
      activeSlotsLocal[pd.slot]=pd.rosterIdx;
      pushChart(p.id,p.rssi,p.crossing);
    });
    buildRaceCards();slots.forEach(p=>updateRaceCard(p));
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
      if(!prevCrossing&&p.crossing){ensureAudio();sfx.enter();}
      if(prevCrossing&&!p.crossing){ensureAudio();sfx.exit();}
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
    ensureAudio();sfx.enter();
    if(lapMode==='holeshot')speak(getSpokenName(p)+(announceMode==='lap_laptime'?'、ホールショット':''));
    updateRaceCard(p);
    return;
  }
  if(d.type==='lap'){
    var s=d.pilot;if(s<0||s>=N)return;
    var p=slots[s];
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
    updateRaceCard(p);addLapRow(p,lapMs,_cum);checkFinished(p);
    // Always give immediate, reliable audio feedback for the crossing —
    // independent of TTS, which can be dropped by the browser. Best lap gets
    // the fanfare; every other lap gets the standard lap tone.
    ensureAudio();
    if(d.newBest)sfx.best();else sfx.lap();
    // Voice announcement is additive on top of the beep.
    speak(buildSpeech(p,p.lapCount,lapMs));
    return;
  }
  if(d.type==='race_stage'){return;}
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
  el.textContent='C5 '+(ok?'●':'○')+(e.enabled?'  OSD '+(e.connected?'●':'○'):'');
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

  try{
    var rg=await fetch('/api/settings');
    if(rg.ok){var st0=await rg.json();var g=document.getElementById('gainInput');if(g)g.value=st0.gain;
      var tl=document.getElementById('totalLaps');if(tl)tl.value=st0.totalLaps;}
  }catch(e){}

  var am=document.getElementById('announceMode');if(am)am.value=announceMode;
  var sr=document.getElementById('speechRate');if(sr)sr.value=speechRate;
  var srN=document.getElementById('speechRateN');if(srN)srN.value=speechRate;
  var lms=document.getElementById('lapModeSelect');if(lms)lms.value=lapMode;
  var sdm=document.getElementById('sdLogModeSelect');if(sdm)sdm.value=sdLogMode;
  var cdEl=document.getElementById('cooldownInput');if(cdEl)cdEl.value=(cooldownMs/1000).toFixed(1);
  refreshVoiceBtns();
  slots.forEach(p=>updateRaceCard(p));

  // Settings sync policy (multi-client safe):
  //  • If a race is in progress, ADOPT the web node's live settings so a
  //    second device joining mid-race can never clobber lapMode/cooldown
  //    (changing cooldown mid-race would alter lap detection).
  //  • If idle, restore THIS device's saved preferences to the web node so
  //    they survive a web-node reboot.
  try{
    var racing=false;
    var rst=await fetch('/api/status');
    if(rst.ok){var stt=await rst.json();racing=!!stt.raceRunning||((stt.lapCount||0)>0);}
    if(racing){
      var rget=await fetch('/api/settings');
      if(rget.ok){
        var st=await rget.json();
        if(st.lapMode!==undefined){lapMode=st.lapMode===1?'immediate':'holeshot';localStorage.setItem('lapMode',lapMode);}
        if(st.cooldownMs!==undefined){cooldownMs=st.cooldownMs;localStorage.setItem('cooldownMs',cooldownMs);}
        if(st.sdLogMode!==undefined){sdLogMode=st.sdLogMode===2?'off':(st.sdLogMode===1?'rotate':'always');localStorage.setItem('sdLogMode',sdLogMode);}
        var lms2=document.getElementById('lapModeSelect');if(lms2)lms2.value=lapMode;
        var sdm2=document.getElementById('sdLogModeSelect');if(sdm2)sdm2.value=sdLogMode;
        var cd2=document.getElementById('cooldownInput');if(cd2)cd2.value=(cooldownMs/1000).toFixed(1);
      }
    }else if(isAuthed){
      // Only the admin should be pushing settings back to the web node.
      await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify({lapMode:lapMode==='immediate'?1:0,cooldownMs:cooldownMs,sdLogMode:sdLogModeInt(sdLogMode)})});
    }
  }catch(e){}
}

// App init
buildRaceCards();
buildCalibCards();
applyAuthState();
wsConnect();
loadAll();
