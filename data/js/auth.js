'use strict';

// ── Soft auth ────────────────────────────────────────────────────────────
// Accidental-clobber protection, not real security. A single shared admin
// password (stored in NVS on the web node) gates the non-race tabs and the
// race-control buttons. Session-scoped (sessionStorage) so a shared device
// doesn't stay authed across a browser-close.

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
    if(r.ok){
      isAuthed=true;sessionStorage.setItem('authed','1');
      closeLoginModal();applyAuthState();
      var t=pendingTab;pendingTab=null;
      switchTab(t||'config');
      toast('🔓 ログインしました');
    }else{
      if(err){err.style.display='block';err.textContent='パスワードが違います';}
    }
  }catch(e){if(err){err.style.display='block';err.textContent='通信エラー';}}
}

function logout(){
  isAuthed=false;sessionStorage.removeItem('authed');
  applyAuthState();
  switchTab('race');   // bounce viewers off admin panes
  toast('🔒 ログアウトしました');
}

// Reflect auth state: header button label, race controls, locked-tab marks.
function applyAuthState(){
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

// Primitive password management: the field shows the current password and
// whatever it contains becomes the password on save. Load on first Config view.
async function loadPasswordField(){
  var f=document.getElementById('pwField');if(!f)return;
  try{
    var r=await fetch('/api/auth/password');
    if(r.ok){var j=await r.json();f.value=j.password||'';}
  }catch(e){}
}
async function savePassword(){
  var f=document.getElementById('pwField');if(!f)return;
  var v=f.value;
  if(v.length<1||v.length>32){toast('⚠️ パスワードは1〜32文字');return;}
  try{
    var r=await fetch('/api/auth/password',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({password:v})});
    if(r.ok)toast('✅ パスワードを保存しました');
    else toast('⚠️ 保存エラー');
  }catch(e){toast('⚠️ 通信エラー');}
}
