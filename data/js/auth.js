'use strict';

// ── Soft auth ────────────────────────────────────────────────────────────
// Accidental-clobber protection, not real security. A single shared admin
// password (stored in NVS on the web node) gates the non-race tabs and the
// race-control buttons. The login is a token from the timer, kept in
// localStorage; the timer drops it once no browser has used it for the hold
// time set in settings (closed browser, phone screen off), or on logout.

// Header button: login when logged out, logout when logged in.
function onAuthBtn(){ if(isAuthed)logout(); else openLoginModal(); }

function openLoginModal(){
  var m=document.getElementById('loginModal');if(!m)return;
  var pw=document.getElementById('loginPw');var err=document.getElementById('loginErr');
  if(pw)pw.value='';if(err)err.style.display='none';
  m.style.display='flex';
  setTimeout(()=>{if(pw)pw.focus();},50);
}
function closeLoginModal(){
  var m=document.getElementById('loginModal');if(m)m.style.display='none';
}
function cancelLogin(){pendingTab=null;closeLoginModal();}

async function submitLogin(){
  var pw=document.getElementById('loginPw');var err=document.getElementById('loginErr');
  var v=pw?pw.value:'';
  try{
    var r=await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({password:v})});
    var j={};try{j=await r.json();}catch(e){}
    if(r.status===409){
      if(err){err.style.display='block';
        err.textContent='管理者のログインは最大 '+(j.max||'')+' 台までです。ほかの端末でログアウトしてから、もう一度ログインしてください。';}
      return;
    }
    if(r.ok&&j.token){
      authToken=j.token;isAuthed=true;lsSet('authToken',authToken);
      sendHello();   // this connection becomes an admin's (RSSI, the date)
      if(pw){pw.blur();pw.value='';}   // close the keyboard; leave no password behind
      closeLoginModal();applyAuthState();
      // Go to the tab that asked for the login; from the header button,
      // stay on the tab that is open (e.g. the race tab).
      var t=pendingTab;pendingTab=null;
      if(t)switchTab(t);
      toast('🔓 ログインしました');
      tuckBrowserBars();
    }else{
      if(err){err.style.display='block';err.textContent='パスワードが違います';}
    }
  }catch(e){if(err){err.style.display='block';err.textContent='通信エラー';}}
}

// After the login keyboard closes, mobile Safari (landscape) shows its
// address and tab bars again and only hides them when the page scrolls.
// Nudge the page down a pixel so they collapse as they do before logging in.
function tuckBrowserBars(){
  setTimeout(function(){
    if(window.scrollY<1)window.scrollTo(0,1);
  },350);
}

function logout(){
  var t=authToken;
  fetch('/api/auth/logout',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token:t})}).catch(()=>{});
  dropAuth();
  switchTab('race');   // bounce viewers off admin panes
  toast('🔒 ログアウトしました');
}

// Forget the login on this tab (logout, or the timer no longer knows it).
function dropAuth(){
  authToken='';isAuthed=false;lsSet('authToken','');
  applyAuthState();
}

// Reflect auth state: header button label, race controls, locked-tab marks,
// and the spectator screen for viewers.
function applyAuthState(){
  document.body.classList.toggle('viewer',!isAuthed);
  var ab=document.getElementById('authBtn');
  if(ab)ab.textContent=isAuthed?'🔓 ログアウト':'🔒 ログイン';
  var hint=document.getElementById('authHint');
  if(hint)hint.style.display=isAuthed?'none':'block';
  if(typeof setBtns==='function')setBtns(raceRunning);
  ['btnStart','btnStop','btnClear'].forEach(function(id){
    var b=document.getElementById(id);if(b)b.title=isAuthed?'':'管理者ログインが必要です';
  });
  document.querySelectorAll('.tab-btn').forEach(function(b,i){
    var name=['race','config','calib','sd'][i];
    if(name===undefined||name==='race')return;
    b.classList.toggle('locked',!isAuthed);
  });
}

// Password change: the current password is never sent to the browser (any
// device on the timer's WiFi could read it), so the field starts empty.
function loadPasswordField(){
  var f=document.getElementById('pwField');if(f)f.value='';
}
async function savePassword(){
  var f=document.getElementById('pwField');if(!f)return;
  var v=f.value;
  if(v.length<1||v.length>32){toast('⚠️ パスワードは1〜32文字');return;}
  try{
    var r=await fetch('/api/auth/password',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({password:v})});
    if(r.ok){f.value='';toast('✅ パスワードを保存しました');}
    else toast('⚠️ 保存エラー');
  }catch(e){toast('⚠️ 通信エラー');}
}
