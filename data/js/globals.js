'use strict';

const PCOLORS = ['var(--p0)','var(--p1)','var(--p2)','var(--p3)',
                 'var(--p4)','var(--p5)','var(--p6)','var(--p7)'];
const PCLS    = ['p0','p1','p2','p3','p4','p5','p6','p7'];
const N = 8;

// RSSI is the C5MK scale: 0..1023, 16 counts per dB, floor ~116 at gain 30.
const RSSI_MAX = 1023;
const DEF_ENTER = 350, DEF_EXIT = 280;

// 5.8 GHz VTX bands. The C5 receives 5180-5917 MHz (E7/E8 are out of range).
const BANDS = {
  R:{name:'Raceband', f:[5658,5695,5732,5769,5806,5843,5880,5917]},
  F:{name:'Fatshark', f:[5740,5760,5780,5800,5820,5840,5860,5880]},
  A:{name:'Boscam A', f:[5865,5845,5825,5805,5785,5765,5745,5725]},
  B:{name:'Boscam B', f:[5733,5752,5771,5790,5809,5828,5847,5866]},
  E:{name:'Boscam E', f:[5705,5685,5665,5645,5885,5905,5925,5945]},
  L:{name:'LowRace',  f:[5362,5399,5436,5473,5510,5547,5584,5621]}
};
const F_MIN = 5180, F_MAX = 5917;
function freqLabel(mhz){
  if(!mhz)return '未設定';
  for(var b in BANDS){var i=BANDS[b].f.indexOf(mhz);if(i>=0)return b+(i+1)+' '+mhz;}
  return mhz+' MHz';
}

const slots = Array.from({length:N}, (_,i) => ({
  id:i, name:'---', yomi:'', rosterIdx:-1, freq:0, rssi:0, crossing:false,
  lapCount:0, bestLapMs:0, lapTimes:[], cumulative:0,
  enterRssi:DEF_ENTER, exitRssi:DEF_EXIT, rssiSignal:false
}));

var rosterData = [];
var rosterById = {};
function rebuildRosterIndex(){
  rosterById={};
  for(var i=0;i<rosterData.length;i++)rosterById[rosterData[i].id]=rosterData[i];
}
var activeSlotsLocal = Array(N).fill(-1);

var raceRunning=false, raceStartPerf=0, timerH=null, countdownH=null;
var raceStarted=false, timerFrozenMs=0;
var voiceEnabled = localStorage.getItem('voice')!=='0';
var announceMode = localStorage.getItem('announce')||'lap_laptime';
var speechRate   = parseFloat(localStorage.getItem('srate')||'1.1');
var lapMode      = localStorage.getItem('lapMode')||'holeshot';
var cooldownMs   = parseInt(localStorage.getItem('cooldownMs')||'3000');
var sdLogMode    = localStorage.getItem('sdLogMode')||'always';
function sdLogModeInt(m){return m==='off'?2:(m==='rotate'?1:0);}

var c5Status = null;         // last "c5" WebSocket message (receiver + ELRS link)
var activeTab    = 'race';   // gates per-tab work in the hot RSSI path
var editingRosterId = null;

// Soft auth: viewers see the Race tab read-only, admins (single shared
// password, default "admin") get access to Config/Calib/SD + race controls.
// Session-scoped: clears on tab close so a shared device doesn't stay
// authed forever.
var isAuthed = sessionStorage.getItem('authed')==='1';
var pendingTab = null;       // tab the user is trying to reach via login
var sdPresent = false;
var sdFileList = [];
var calibSaveTimers = {};

function toast(msg, dur) {
  dur=dur||2000;
  var t=document.getElementById('statusMsg');
  if(!t)return;
  t.textContent=msg;
  var isErr=msg.includes('⚠')||msg.includes('エラー')||msg.includes('失敗');
  if(isErr)t.classList.add('err'); else t.classList.remove('err');
  t.classList.add('show');
  clearTimeout(t._hideTimer);
  t._hideTimer=setTimeout(()=>t.classList.remove('show'),dur);
}

function pad(n,w){return String(n).padStart(w,'0');}
function fmt(ms){if(!ms)return'--:--.---';var m=Math.floor(ms/60000),s=Math.floor((ms%60000)/1000),f=ms%1000;return m+':'+pad(s,2)+'.'+pad(f,3);}
function fmtTimer(ms){var h=Math.floor(ms/3600000),m=Math.floor((ms%3600000)/60000),s=Math.floor((ms%60000)/1000),cs=Math.floor((ms%1000)/10);if(h>0)return pad(h,2)+':'+pad(m,2)+':'+pad(s,2)+'.'+pad(cs,2);return pad(m,2)+':'+pad(s,2)+'.'+pad(cs,2);}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
// RSSI bar: floor (~100) to a strong close pass (~900).
function rssiPct(v){return Math.max(0,Math.min(100,((v-100)/(900-100))*100));}
function fmtDelta(ms){return(ms>0?'+':'')+(ms/1000).toFixed(3)+'s';}

function switchTab(tab){
  // Soft auth gate — non-race tabs require admin login.
  if(tab!=='race'&&!isAuthed){pendingTab=tab;openLoginModal();return;}
  activeTab=tab;
  document.querySelectorAll('.tab-pane').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(b=>b.classList.remove('active'));
  document.getElementById('pane-'+tab).classList.add('active');
  document.querySelectorAll('.tab-btn')[['race','config','calib','sd'].indexOf(tab)].classList.add('active');
  // Returning to a tab: repaint its live widgets immediately (the hot RSSI
  // path only touches the DOM of the currently-active tab).
  if(tab==='race')slots.forEach(p=>updateRaceCard(p));
  var sdActive=tab==='config'||tab==='sd';
  fetch('/api/sd/poll',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({enable:sdActive})}).catch(()=>{});
  if(tab==='calib'){
    setTimeout(()=>{
      slots.forEach(p=>{
        var c=charts[p.id];if(!c)return;
        var newW=c.cv.offsetWidth||300;
        if(newW!==c.cv.width){c.cv.width=newW;}
        drawChart(p.id);
      });
    },50);
  }
  if(tab==='sd') refreshSdFiles();
  if(tab==='config'){
    if(typeof loadPasswordField==='function')loadPasswordField();
    loadElrsSettings();loadNetSettings();renderC5Status();
  }
}
