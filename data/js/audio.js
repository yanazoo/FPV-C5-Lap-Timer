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
// silently stall (well-known iOS Safari TTS bug). iOS only: Chrome on a PC
// restarts the utterance on resume(), and "レディ" (which the queue's
// lastSpeechStart didn't cover) was then said twice.
var IS_IOS=/iPhone|iPad|iPod/.test(navigator.userAgent)||(navigator.platform==='MacIntel'&&navigator.maxTouchPoints>1);
setInterval(()=>{
  if(typeof speechSynthesis==='undefined'||!IS_IOS)return;
  if(speechSynthesis.speaking&&Date.now()-lastSpeechStart>8000){
    try{speechSynthesis.pause();speechSynthesis.resume();}catch(e){}
  }
},5000);
var cachedJaVoice=null;

// The Japanese voice to speak with. A voice that is not on the device (an
// online voice such as Chrome's "Google 日本語", or an iPhone voice that was
// never downloaded) says nothing at a field with no internet, so the voice
// picked in Settings is used if set, else a voice built into the device:
// the usual system voices by name first (Kyoko on iPhone / Mac, Haruka /
// Ayumi on Windows, ...), then the device's default, then any local one.
// With none of those, the online Japanese voice (the same one every time).
var BUILTIN_JA=['Kyoko','Otoya','Haruka','Ayumi','Ichiro','Sayaka','Nanami','Keita'];
function jaVoices(){
  if(typeof speechSynthesis==='undefined')return [];
  return speechSynthesis.getVoices().filter(v=>v.lang&&v.lang.replace('_','-').toLowerCase().startsWith('ja'));
}
function pickJaVoice(){
  var vs=jaVoices();if(!vs.length)return null;
  var want='';try{want=localStorage.getItem('voiceName')||'';}catch(e){}
  if(want){var c=vs.find(v=>v.name===want);if(c)return c;}
  var local=vs.filter(v=>v.localService!==false);
  for(var i=0;i<BUILTIN_JA.length;i++){
    var b=local.find(v=>v.name.indexOf(BUILTIN_JA[i])>=0&&!/enhanced|premium|拡張/i.test(v.name));
    if(b)return b;
  }
  // No built-in Japanese voice: still always the same one (an online voice,
  // which needs internet), rather than letting the browser choose per call.
  return local.find(v=>v.default)||local[0]||vs.find(v=>v.default)||vs[0];
}
// Every utterance gets the same voice. Without one, iPhone guesses the
// language of each sentence, and a call that starts with a pilot's name in
// letters ("Yana、1周…") was read by an English voice: the voice changed
// from pilot to pilot. The voice list can still be empty when the page
// opens (and iPhone may never fire voiceschanged), so a miss is not
// remembered: the next call looks again. The voice is kept by name and
// looked up in the current list each time (an old voice object can stop
// working after the list is reloaded).
function getJaVoice(){
  var vs=jaVoices();
  if(cachedJaVoice){
    var keep=vs.find(v=>v.name===cachedJaVoice);
    if(keep)return keep;
  }
  var v=pickJaVoice();
  cachedJaVoice=v?v.name:null;
  return v;
}
if(typeof speechSynthesis!=='undefined'&&'onvoiceschanged' in speechSynthesis){
  speechSynthesis.onvoiceschanged=function(){cachedJaVoice=null;renderVoiceInfo();};
}
// Settings: the voice list and which one speaks.
function renderVoiceInfo(){
  var sel=document.getElementById('voiceSelect'),info=document.getElementById('voiceInfo');
  if(!sel||!info)return;
  if(typeof speechSynthesis==='undefined'){
    sel.innerHTML='<option>（このブラウザは読み上げに対応していません）</option>';sel.disabled=true;
    info.textContent=inCaptiveSheet()?'⚠ 簡易ブラウザで開いています。Safari で http://'+location.host+' を開いてください':'⚠ このブラウザでは読み上げできません（通過音などの効果音は鳴ります）';
    return;
  }
  var vs=jaVoices(),want='';try{want=localStorage.getItem('voiceName')||'';}catch(e){}
  var h='<option value="">自動（端末内蔵の声を優先）</option>';
  vs.forEach(v=>{h+='<option value="'+esc(v.name)+'"'+(v.name===want?' selected':'')+'>'+esc(v.name)+(v.localService===false?'（ネット必要）':'（端末内蔵）')+'</option>';});
  sel.innerHTML=h;sel.disabled=false;
  var v=getJaVoice();
  if(!vs.length)info.textContent='日本語の声が見つかりません。端末の言語・読み上げの設定で日本語の音声を追加してください';
  else if(!v)info.textContent='⚠ 端末内蔵の日本語の声がありません。インターネットがない場所では読み上げられない場合があります';
  else info.textContent='使用中の声：'+v.name+(v.localService===false?'（⚠ インターネットが必要。ネットのない場所では読み上げられません）':'（端末内蔵・ネット不要）');
}
function selectVoice(name){
  try{if(name)localStorage.setItem('voiceName',name);else localStorage.removeItem('voiceName');}catch(e){}
  cachedJaVoice=null;renderVoiceInfo();testVoice();
}
// iPhone opens the timer's page in its captive-portal sheet when it joins
// the timer's WiFi. That sheet can't speak: open it in Safari instead.
function inCaptiveSheet(){
  var ua=navigator.userAgent;
  return /iPhone|iPad|iPod/.test(ua)&&!/Safari\//.test(ua);
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
  try{
    // A real (silent) word: iOS doesn't count an empty utterance.
    var u=new SpeechSynthesisUtterance('あ');u.lang='ja-JP';u.volume=0;u.rate=2;
    var v=getJaVoice();if(v)u.voice=v;
    lastSpeechStart=Date.now();speechSynthesis.speak(u);speechWarmedUp=true;
  }catch(e){}
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
    lastSpeechStart=Date.now();speechSynthesis.speak(u);
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
    lastSpeechStart=Date.now();speechSynthesis.speak(u);
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
    var jaVoice=getJaVoice();
    if(jaVoice)u.voice=jaVoice;
    lastSpeechStart=Date.now();speechSynthesis.speak(u);
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
      lastSpeechStart=Date.now();speechSynthesis.speak(u);
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
// Speech too (iOS speaks only after the page has spoken inside a tap).
['pointerdown','keydown'].forEach(ev=>document.addEventListener(ev,function un(){
  ensureAudio();warmSpeech();document.removeEventListener(ev,un,true);
},true));
// Opened in the iPhone's captive-portal sheet: say how to get speech.
if(inCaptiveSheet())setTimeout(()=>{if(typeof toast==='function')toast('⚠ 簡易ブラウザでは読み上げできません。Safari で http://'+location.host+' を開いてください',8000);},1500);
