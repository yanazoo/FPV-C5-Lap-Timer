'use strict';

var actx=null,audioUnlocked=false;
function ensureAudio(){
  try{
    if(!actx)actx=new(window.AudioContext||window.webkitAudioContext)();
    if(actx.state==='suspended')actx.resume();
    // Android Chrome only fully unlocks WebAudio when an actual node has
    // produced output inside a user gesture; resume() alone is not enough.
    if(!audioUnlocked){
      var o=actx.createOscillator(),g=actx.createGain();
      g.gain.value=0.00001;o.connect(g);g.connect(actx.destination);
      var t=actx.currentTime;o.start(t);o.stop(t+0.03);
      audioUnlocked=true;
    }
  }catch(e){}
}

// Unlock audio + speech on the very first user interaction anywhere on the
// page. Race crossing/lap sounds and TTS fire from WebSocket events (no
// gesture), so without this Android stays muted until a specific button is
// pressed. Also recover the context after backgrounding (screen lock).
(function(){
  function unlock(){ensureAudio();}
  ['pointerdown','touchstart','mousedown','keydown','click'].forEach(function(ev){
    document.addEventListener(ev,unlock,{capture:true,passive:true});
  });
  document.addEventListener('visibilitychange',function(){
    if(!document.hidden&&actx&&actx.state==='suspended')actx.resume();
  });
})();
// RotorHazard's synthetic tones (its play_beep): a raw oscillator at full
// level for `ms`, then an exponential fade to silence over `fade` seconds
// (RotorHazard's default 1 s); that fade is the ringing tail.
// Volumes keep RotorHazard's balance: race tones (tone_volume 1.0) are
// twice the indicator beeps (indicator_beep_volume 0.5).
var TONE_VOL=0.6, BEEP_VOL=0.3;
function rhBeep(ms,freq,vol,wave,fade){
  if(!actx)return;
  if(actx.state==='suspended'){try{actx.resume();}catch(e){}}
  if(fade==null)fade=1;
  var t=actx.currentTime,end=t+ms/1000;
  var o=actx.createOscillator(),g=actx.createGain();
  o.type=wave;o.frequency.value=freq;
  g.gain.setValueAtTime(vol,t);g.gain.setValueAtTime(vol,end);
  g.gain.exponentialRampToValueAtTime(0.00001,end+fade);
  o.connect(g);g.connect(actx.destination);
  o.start(t);o.stop(end+fade+0.02);
}
function rhBeepAt(at,ms,freq,vol,wave,fade){setTimeout(()=>rhBeep(ms,freq,vol,wave,fade),at);}
// Gate crossing pitch per channel: Ch1 440 Hz (A4) up in semitones to Ch8 659 Hz (E5).
var CH_TONES=[440,466.2,493.9,523.3,554.4,587.3,622.3,659.3];
function chTone(slot){return CH_TONES[slot]||CH_TONES[0];}

var sfx={
  // All as RotorHazard's synthetic tones.
  // Countdown: staging tone (triangle 440 Hz, 100 ms) each second, then the
  // start tone (triangle 880 Hz, 700 ms, 0.25 s fade) at go.
  count: ()=>{
    [0,1000,2000].forEach(at=>rhBeepAt(at,100,440,TONE_VOL,'triangle'));
    rhBeepAt(3000,700,880,TONE_VOL,'triangle',0.25);
  },
  // Stop: the countdown's 440 Hz falling an octave, A4 -> E4 -> A3
  // (triangle, 90 ms each, 150 ms apart; the last one rings out).
  stop: ()=>{
    rhBeepAt(0,90,440,TONE_VOL,'triangle',0.06);
    rhBeepAt(150,90,330,TONE_VOL,'triangle',0.06);
    rhBeepAt(300,90,220,TONE_VOL,'triangle',1);
  },
  // A pilot finishes: RotorHazard's winner tone (square 1200 / 1800 Hz x3).
  finish: ()=>{
    [[0,50,1200],[50,75,1800],[125,50,1200],[175,75,1800],[250,50,1200],[300,100,1800]]
      .forEach(n=>rhBeepAt(n[0],n[1],n[2],BEEP_VOL,'square'));
  },
  // Time-limited race over: a short fanfare (G4 C5 E5, then a long G5 and
  // the C6 above it), triangle like the other race tones.
  fanfare: ()=>{
    [[0,110,392],[130,110,523.3],[260,110,659.3],[390,500,784]]
      .forEach(n=>rhBeepAt(n[0],n[1],n[2],TONE_VOL,'triangle',0.15));
    rhBeepAt(390,500,1046.5,TONE_VOL*0.6,'triangle',0.6);
  },
  // Countdown second without voice: a short 880 Hz tick.
  tick: ()=>rhBeep(60,880,TONE_VOL,'triangle',0.05),
  // Gate crossing, pitch per channel:
  // enter: square 25 ms, 1 s tail; exit: sawtooth 35 ms twice, 70 ms apart.
  enter: (slot)=>rhBeep(25,chTone(slot),BEEP_VOL,'square'),
  exit:  (slot)=>{rhBeep(35,chTone(slot),BEEP_VOL,'sawtooth',0.02);rhBeepAt(70,35,chTone(slot),BEEP_VOL,'sawtooth');}
};

var speechQ=[],speechBusy=false,speechWarmedUp=false,lastSpeechStart=0;

// iOS Safari often fails to fire onend / reports speaking inconsistently
// and rapid speak() calls can drop the second utterance. Poll frequently
// and advance the queue whenever the engine is no longer speaking.
setInterval(()=>{
  if(typeof speechSynthesis==='undefined')return;
  if(speechSynthesis.paused)speechSynthesis.resume();
  if(speechBusy&&!speechSynthesis.speaking&&Date.now()-lastSpeechStart>800){
    speechBusy=false;nextSpeech();
  }
},250);
// iOS keep-alive: nudge a long-running utterance so the engine doesn't
// silently stall (well-known iOS Safari TTS bug).
setInterval(()=>{
  if(typeof speechSynthesis==='undefined')return;
  if(speechSynthesis.speaking&&Date.now()-lastSpeechStart>8000){
    try{speechSynthesis.pause();speechSynthesis.resume();}catch(e){}
  }
},5000);
var cachedJaVoice=null;

function getJaVoice(){
  if(cachedJaVoice)return cachedJaVoice;
  var voices=speechSynthesis.getVoices();
  cachedJaVoice=voices.find(v=>v.lang&&v.lang.startsWith('ja'))||null;
  return cachedJaVoice;
}
if(typeof speechSynthesis!=='undefined'&&'onvoiceschanged' in speechSynthesis){
  speechSynthesis.onvoiceschanged=function(){cachedJaVoice=null;};
}

// Say "レディ" when Start is pressed, then call `then` (the countdown) once
// it has been said. iOS Safari only lets speech start from a user gesture;
// the lap calls come later from WebSocket messages, so this first utterance
// (inside the button press) also unlocks speech for the race.
// With voice off (or beeps only) `then` runs at once. iOS often never fires
// onend, so the end is also taken from speechSynthesis.speaking going false,
// and after 2.5 s at the latest the countdown starts anyway.
// iOS Safari speaks only after the page has spoken inside a tap: the Start
// tap does that (silently), so "レディ" from race_ready is heard.
function warmSpeech(){
  if(speechWarmedUp||typeof speechSynthesis==='undefined')return;
  try{var u=new SpeechSynthesisUtterance(' ');u.volume=0;speechSynthesis.speak(u);speechWarmedUp=true;}catch(e){}
}
function announceStart(then){
  var go=then||function(){};
  if(typeof speechSynthesis==='undefined'||!voiceEnabled||announceMode==='none'||announceMode==='beep'){go();return;}
  speechWarmedUp=true;
  var done=false,started=performance.now(),poll=null,cap=null;
  function finish(){if(done)return;done=true;clearInterval(poll);clearTimeout(cap);go();}
  try{
    var u=new SpeechSynthesisUtterance('レディ');
    u.lang='ja-JP';u.rate=speechRate;
    var jaVoice=getJaVoice();if(jaVoice)u.voice=jaVoice;
    u.onend=finish;u.onerror=finish;
    if(speechSynthesis.paused)speechSynthesis.resume();
    speechSynthesis.speak(u);
  }catch(e){finish();return;}
  poll=setInterval(()=>{if(performance.now()-started>400&&!speechSynthesis.speaking)finish();},100);
  cap=setTimeout(finish,2500);
}

// Voice-only. Callers play the lap/crossing beep themselves, so an audio
// cue always fires even when voice is off or the browser drops the utterance.
// Time-critical callout (the timed race's countdown): said now, ahead of
// queued lap calls, and cutting off one still being said. Without voice,
// a tick instead.
function sayNow(text){
  if(!voiceEnabled||announceMode==='none'||announceMode==='beep'||typeof speechSynthesis==='undefined'){
    ensureAudio();sfx.tick();return;
  }
  speechQ=[];
  try{speechSynthesis.cancel();}catch(e){}
  speechBusy=false;
  speechQ.push(text);
  nextSpeech();
}
function speak(text){
  if(!voiceEnabled||announceMode==='none'||announceMode==='beep')return;
  if(speechQ.length<8)speechQ.push(text);
  if(!speechBusy)nextSpeech();
}
function nextSpeech(){
  if(!speechQ.length){speechBusy=false;return;}
  speechBusy=true;
  var text=speechQ.shift();
  var u=new SpeechSynthesisUtterance(text);
  u.lang='ja-JP';u.rate=speechRate;
  var jaVoice=getJaVoice();
  if(jaVoice)u.voice=jaVoice;
  // Soft timeout = expected duration + grace. On iOS, onend often never
  // fires, so we advance the queue once the engine reports it's no longer
  // speaking. Hard timeout = forced cancel + advance as a last resort.
  var rate=Math.max(0.5,speechRate);
  var estMs=Math.max(900,text.length*200/rate);
  var hardMs=Math.max(5000,estMs*2);
  var done=false;
  function advance(cancel){
    if(done)return;done=true;
    clearTimeout(softT);clearTimeout(hardT);
    if(cancel){try{speechSynthesis.cancel();}catch(e){}}
    speechBusy=false;
    setTimeout(nextSpeech,60);
  }
  var softT=setTimeout(()=>{if(!speechSynthesis.speaking)advance(false);},estMs+400);
  var hardT=setTimeout(()=>advance(true),hardMs);
  u.onend=()=>advance(false);
  u.onerror=()=>advance(false);
  lastSpeechStart=Date.now();
  try{
    if(speechSynthesis.paused)speechSynthesis.resume();
    speechSynthesis.speak(u);
  }catch(e){advance(false);}
}
function getSpokenName(p){return (p.yomi&&p.yomi!=='')?p.yomi:p.name;}
function buildSpeech(p,lapCount,lapMs){
  var spokenName=getSpokenName(p);
  var s=Math.floor(lapMs/1000),ms=Math.floor((lapMs%1000)/100);
  var m=Math.floor(s/60);s=s%60;
  var tStr=m>0?m+'分'+s+'秒'+ms:s+'秒'+ms;
  if(announceMode==='lap_laptime'){var lapLabel=lapMode==='immediate'?lapCount+'周':(lapCount===1?'ホールショット':(lapCount-1)+'周');return spokenName+'、'+lapLabel+'、'+tStr;}
  return spokenName+'、'+tStr;
}
// 音声テスト (race tab) / テスト (settings): the gate tone, then a sample
// call when voice is on. Also unlocks sound for this browser.
function testVoice(){
  ensureAudio();sfx.exit(0);
  if(typeof speechSynthesis==='undefined')return;
  if(!voiceEnabled||announceMode==='none'){toast('🔇 音声はOFFです（設定タブで切替）');return;}
  if(announceMode!=='beep'){
    speechWarmedUp=true;
    var u=new SpeechSynthesisUtterance('テスト、1分23秒4');u.lang='ja-JP';u.rate=speechRate;
    var voices=speechSynthesis.getVoices();
    var jaVoice=voices.find(v=>v.lang&&v.lang.startsWith('ja'));
    if(jaVoice)u.voice=jaVoice;
    speechSynthesis.speak(u);
  }
}
function toggleVoice(){
  ensureAudio();
  voiceEnabled=!voiceEnabled;
  localStorage.setItem('voice',voiceEnabled?'1':'0');
  refreshVoiceBtns();
  // Audible confirmation on enable doubles as the iOS Safari TTS unlock
  // (matches the proven PhobosLT_4ch pattern). Cancel anything pending
  // when disabling.
  if(typeof speechSynthesis==='undefined')return;
  if(voiceEnabled){
    try{
      var u=new SpeechSynthesisUtterance('音声オン');
      u.lang='ja-JP';u.rate=speechRate;u.volume=0.5;
      var jaVoice=getJaVoice();if(jaVoice)u.voice=jaVoice;
      speechSynthesis.speak(u);
      speechWarmedUp=true;
    }catch(e){}
  }else{
    try{speechSynthesis.cancel();}catch(e){}
    speechQ=[];speechBusy=false;
  }
}
function refreshVoiceBtns(){
  ['voiceToggle2'].forEach(id=>{
    var el=document.getElementById(id);if(!el)return;
    el.checked=voiceEnabled;
  });
}

// Browsers only allow sound after a tap or key press on the page. Unlock it
// on the first one, so a start or stop from the race director or another
// browser is heard even when this one never pressed a race button.
['pointerdown','keydown'].forEach(ev=>document.addEventListener(ev,function un(){
  ensureAudio();document.removeEventListener(ev,un,true);
},true));
