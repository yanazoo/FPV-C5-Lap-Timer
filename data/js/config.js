'use strict';

// ── VTX frequency picker ──────────────────────────────────────────────────
function freqSelectHtml(id, value){
  var h='<select id="'+id+'" class="freq-select">';
  h+='<option value="0"'+(value?'':' selected')+'>未設定</option>';
  var known=false;
  for(var b in BANDS){
    h+='<optgroup label="'+BANDS[b].name+'">';
    BANDS[b].f.forEach(function(f,i){
      var out=f<F_MIN||f>F_MAX;
      var sel=(f===value&&!known);
      if(sel)known=true;
      h+='<option value="'+f+'"'+(sel?' selected':'')+(out?' disabled':'')+'>'+b+(i+1)+' '+f+(out?'（範囲外）':'')+'</option>';
    });
    h+='</optgroup>';
  }
  if(value&&!known)h+='<option value="'+value+'" selected>'+value+' MHz</option>';
  return h+'</select>';
}

function dupFreqSlots(){
  // Active slots sharing a frequency: both would read the same VTX.
  var seen={},dup={};
  for(var s=0;s<N;s++){
    var r=rosterById[activeSlotsLocal[s]];if(!r||!r.freq)continue;
    if(seen[r.freq]!==undefined){dup[s]=true;dup[seen[r.freq]]=true;}else seen[r.freq]=s;
  }
  return dup;
}

// ── Roster ────────────────────────────────────────────────────────────────
function renderRoster(){
  var filter=(document.getElementById('searchInput').value||'').toLowerCase().trim();
  var list=document.getElementById('rosterList');
  document.getElementById('rosterCountBadge').textContent='('+rosterData.length+'/20)';

  var filtered=rosterData.filter(function(r){
    if(!filter)return true;
    return r.name.toLowerCase().includes(filter)||freqLabel(r.freq).toLowerCase().includes(filter);
  });
  filtered.sort(function(a,b){
    var aCh=a.activeSlot>=0?a.activeSlot:99, bCh=b.activeSlot>=0?b.activeSlot:99;
    return aCh!==bCh?aCh-bCh:a.id-b.id;
  });
  if(!filtered.length){
    list.innerHTML='<div style="padding:20px;text-align:center;color:var(--muted);font-size:13px">'+(rosterData.length?'該当なし':'パイロット未登録')+'</div>';
    return;
  }
  var dup=dupFreqSlots();
  list.innerHTML='';
  filtered.forEach(function(r){
    var item=document.createElement('div');item.className='roster-item';item.id='ri-'+r.id;
    if(editingRosterId===r.id){
      item.innerHTML=pilotFormHtml('edit',r)
        +'<div class="roster-actions">'
          +'<button onclick="submitEdit('+r.id+')" class="btn-success" style="font-size:11px;padding:3px 9px">保存</button>'
          +'<button onclick="cancelEdit()" class="btn-secondary" style="font-size:11px;padding:3px 9px">×</button>'
        +'</div>';
    } else {
      var chOpts='<option value="-1"'+(r.activeSlot<0?' selected':'')+'>未割当</option>';
      for(var ch=0;ch<N;ch++)chOpts+='<option value="'+ch+'"'+(r.activeSlot===ch?' selected':'')+'>Ch'+(ch+1)+'</option>';
      var activeBadge=r.activeSlot>=0?'<span class="active-badge '+PCLS[r.activeSlot]+'">Ch'+(r.activeSlot+1)+'</span>':'';
      var warn=(r.activeSlot>=0&&dup[r.activeSlot])?'<span class="warn-badge" title="同じ周波数のパイロットがいます">⚠ 周波数重複</span>':'';
      var osd=r.osd?'<span class="osd-badge" title="ELRS OSD">🥽 OSD</span>':'';
      item.innerHTML=
        '<div style="flex:1;min-width:80px"><span class="roster-name">'+esc(r.name)
          +(r.yomi?'<span class="roster-yomi">'+esc(r.yomi)+'</span>':'')+'</span></div>'
        +'<span class="roster-mac">'+(r.freq?esc(freqLabel(r.freq)):'<span style="color:var(--err)">VTX未設定</span>')+'</span>'
        +osd+activeBadge+warn
        +'<div class="roster-actions">'
          +'<select class="ch-select" data-rid="'+r.id+'" onchange="onChSelectChange(this)">'+chOpts+'</select>'
          +'<button onclick="startEdit('+r.id+')" class="btn-secondary" style="font-size:11px;padding:3px 8px">編集</button>'
          +'<button onclick="deleteRosterPilot('+r.id+')" class="btn-danger" style="font-size:11px;padding:3px 8px">削除</button>'
        +'</div>';
    }
    list.appendChild(item);
  });
}

// Shared by the add form and inline edit. prefix: 'add' | 'edit'.
function pilotFormHtml(prefix,r){
  r=r||{name:'',yomi:'',freq:0,bind:'',osd:true,enter:DEF_ENTER,exit:DEF_EXIT};
  var inp='background:var(--bg);border:1px solid var(--bd);color:var(--tx);border-radius:5px;padding:4px 7px;font-size:12px';
  return '<div class="pilot-form">'
    +'<input type="text" id="'+prefix+'Name" value="'+esc(r.name)+'" maxlength="20" placeholder="パイロット名（OSD送信先の既定バインドフレーズ）" autocomplete="off" style="'+inp+'">'
    +'<input type="text" id="'+prefix+'Yomi" value="'+esc(r.yomi||'')+'" maxlength="20" placeholder="よみかた（TTS読み上げ用）" autocomplete="off" style="'+inp+'">'
    +'<label class="pf-row"><span>VTX</span>'+freqSelectHtml(prefix+'Freq',r.freq||0)+'</label>'
    +'<label class="pf-row"><span>ELRS</span><input type="text" id="'+prefix+'Bind" value="'+esc(r.bind||'')+'" maxlength="32" placeholder="バインドフレーズ（空欄=パイロット名）" autocomplete="off" style="flex:1;'+inp+'"></label>'
    +'<label class="pf-row"><span>OSD</span><input type="checkbox" id="'+prefix+'Osd"'+(r.osd!==false?' checked':'')+'> ゴーグルにラップを表示</label>'
    +'<div class="pf-row"><span>入/出</span>'
      +'<input type="number" id="'+prefix+'Enter" value="'+r.enter+'" min="0" max="1023" style="width:64px;'+inp+'">'
      +'<input type="number" id="'+prefix+'Exit" value="'+r.exit+'" min="0" max="1023" style="width:64px;'+inp+'">'
      +'<span style="color:var(--muted);font-size:10px">0〜1023（キャリブで調整）</span></div>'
    +'</div>';
}

function readPilotForm(prefix){
  var v=function(k){return document.getElementById(prefix+k);};
  return {
    name:v('Name').value.trim(), yomi:v('Yomi').value.trim(),
    freq:parseInt(v('Freq').value)||0, bind:v('Bind').value.trim(), osd:v('Osd').checked,
    enter:Math.max(0,Math.min(RSSI_MAX,parseInt(v('Enter').value)||DEF_ENTER)),
    exit:Math.max(0,Math.min(RSSI_MAX,parseInt(v('Exit').value)||DEF_EXIT))
  };
}

function validPilot(p){
  if(!p.name){toast('⚠️ 名前を入力してください');return false;}
  if(p.enter<=p.exit){toast('⚠️ 入 > 出 にしてください');return false;}
  return true;
}

async function onChSelectChange(sel){
  var rid=parseInt(sel.dataset.rid), newCh=parseInt(sel.value);
  var newSlots=activeSlotsLocal.slice();
  for(var i=0;i<N;i++)if(newSlots[i]===rid)newSlots[i]=-1;
  if(newCh>=0)newSlots[newCh]=rid;
  await postSlots(newSlots);
}

async function postSlots(newSlots,msg){
  try{
    var r=await fetch('/api/active',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slots:newSlots})});
    if(!r.ok){toast('⚠️ 保存エラー');return;}
    activeSlotsLocal=newSlots;await loadRoster();buildRaceCards();buildCalibCards();
    if(msg)toast(msg);
  }catch(e){toast('⚠️ 接続エラー');}
}

function showAddForm(){
  var f=document.getElementById('addForm');
  document.getElementById('addFormBody').innerHTML=pilotFormHtml('add');
  f.style.display='block';document.getElementById('addName').focus();
}
function hideAddForm(){document.getElementById('addForm').style.display='none';}

async function submitAdd(){
  var p=readPilotForm('add');if(!validPilot(p))return;
  try{
    var r=await fetch('/api/pilots',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});
    if(!r.ok){toast('⚠️ 登録エラー');return;}
    var body=await r.json();
    // Put a new pilot straight into the first free channel.
    var free=activeSlotsLocal.indexOf(-1);
    if(free>=0){var ns=activeSlotsLocal.slice();ns[free]=body.id;await postSlots(ns);}
    else await loadRoster();
    hideAddForm();toast('✓ '+p.name+' を登録しました');
  }catch(e){toast('⚠️ 接続エラー');}
}

function startEdit(id){editingRosterId=id;renderRoster();setTimeout(()=>{var el=document.getElementById('editName');if(el)el.focus();},0);}
function cancelEdit(){editingRosterId=null;renderRoster();}

async function submitEdit(id){
  var p=readPilotForm('edit');if(!validPilot(p))return;
  p.id=id;
  try{
    var r=await fetch('/api/pilots',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});
    if(!r.ok){toast('⚠️ 更新エラー');return;}
    editingRosterId=null;await loadRoster();applyActiveToSlots();buildRaceCards();buildCalibCards();toast('✓ 更新しました');
  }catch(e){toast('⚠️ 接続エラー');}
}

async function deleteRosterPilot(id){
  var r=rosterById[id];
  if(!confirm((r?r.name:'')+' を削除しますか？'))return;
  if(editingRosterId===id)editingRosterId=null;
  try{
    var res=await fetch('/api/pilots/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})});
    await loadActive();await loadRoster();applyActiveToSlots();buildRaceCards();buildCalibCards();
    toast(res.ok?'削除しました':'⚠️ 削除エラー');
  }catch(e){toast('⚠️ 接続エラー');}
}

async function clearChannelAssignments(){
  await postSlots(Array(N).fill(-1),'✅ チャンネル割当を全解除しました');
}

// ── C5 receiver status ────────────────────────────────────────────────────
function renderC5Status(){
  var el=document.getElementById('c5StatusBody');if(!el)return;
  var c=c5Status;
  if(!c){el.innerHTML='<span style="color:var(--muted)">受信待ち…</span>';return;}
  var ok=c.online&&c.scanning;
  var h='<div class="kv"><span>C5 受信機</span><b style="color:'+(ok?'var(--ok)':'var(--err)')+'">'
    +(c.online?esc(c.state):'オフライン')+'</b></div>'
    +'<div class="kv"><span>ファームウェア / ゲイン</span><b>'+(c.fw||'-')+' / '+(c.gain>=0?c.gain:'-')+'</b></div>'
    +'<div class="kv"><span>レコード / 欠落 / 不良</span><b>'+c.records+' / '+c.gaps+' / '+c.bad+'</b></div>'
    +'<div class="rate-row">';
  for(var s=0;s<N;s++){
    var r=rosterById[activeSlotsLocal[s]];
    h+='<span class="rate-cell '+PCLS[s]+'">Ch'+(s+1)+'<br><small>'+(r&&r.freq?r.freq:'—')+'</small><br>'+(c.rate?c.rate[s]:0)+'/s</span>';
  }
  h+='</div>';
  var e=c.elrs||{};
  h+='<div class="kv" style="margin-top:6px"><span>ELRS netpack</span><b style="color:'+(!e.enabled?'var(--muted)':(e.connected?'var(--ok)':'var(--err)'))+'">'
    +(!e.enabled?'無効':(e.connected?'接続中 '+esc(e.peer)+(e.version?' (v'+esc(e.version)+')':''):'未接続'))+'</b></div>';
  el.innerHTML=h;
}

// ── Global settings ───────────────────────────────────────────────────────
function saveGlobalConfig(){
  announceMode=document.getElementById('announceMode').value;
  speechRate=parseFloat(document.getElementById('speechRateN').value)||1.1;
  lapMode=document.getElementById('lapModeSelect').value;
  var sdEl=document.getElementById('sdLogModeSelect');
  if(sdEl)sdLogMode=sdEl.value;
  var cdSec=parseFloat(document.getElementById('cooldownInput').value)||3.0;
  cooldownMs=Math.max(500,Math.min(30000,Math.round(cdSec*1000)));
  document.getElementById('cooldownInput').value=(cooldownMs/1000).toFixed(1);
  localStorage.setItem('announce',announceMode);
  localStorage.setItem('srate',String(speechRate));
  localStorage.setItem('lapMode',lapMode);
  localStorage.setItem('cooldownMs',String(cooldownMs));
  localStorage.setItem('sdLogMode',sdLogMode);
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({lapMode:lapMode==='immediate'?1:0,cooldownMs:cooldownMs,sdLogMode:sdLogModeInt(sdLogMode)})}).catch(()=>{});
}

function saveGain(){
  var g=Math.max(0,Math.min(89,parseInt(document.getElementById('gainInput').value)||30));
  document.getElementById('gainInput').value=g;
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({gain:g})})
    .then(r=>toast(r.ok?'✓ ゲインを '+g+' に設定しました（閾値の再調整を推奨）':'⚠️ 保存エラー',3000)).catch(()=>toast('⚠️ 接続エラー'));
}

// Total laps lives on the race tab but the firmware needs it for FINISHED! on the OSD.
function saveTotalLaps(){
  var t=Math.max(0,Math.min(99,parseInt(document.getElementById('totalLaps').value)||0));
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({totalLaps:t})}).catch(()=>{});
}

// ── ELRS OSD (vrxc_elrs settings) ─────────────────────────────────────────
const ELRS_BOOLS=['enabled','showRaceClock','showBestLap','positionMode','showTotalTime','resultsMode'];
const ELRS_TEXTS=['host','rdBind','eventName','stageMsg','startMsg','pilotDoneMsg','raceStopMsg'];
const ELRS_NUMS=['port','startUptime','finishUptime','resultsUptime','announceUptime'];
const ELRS_POS=['eventName','announcement','status','currentLap','lapResults','bestLap','totalTime','results','raceClock'];

async function loadElrsSettings(){
  try{
    var r=await fetch('/api/elrs');if(!r.ok)return;
    var d=await r.json();
    ELRS_BOOLS.forEach(k=>{var e=document.getElementById('elrs_'+k);if(e)e.checked=!!d[k];});
    ELRS_TEXTS.concat(ELRS_NUMS).forEach(k=>{var e=document.getElementById('elrs_'+k);if(e)e.value=d[k];});
    var t=document.getElementById('elrs_transport');if(t)t.value=String(d.transport);
    ELRS_POS.forEach(k=>{var e=document.getElementById('elrsPos_'+k);if(e&&d.pos)e.value=d.pos[k];});
    onElrsTransport();
  }catch(e){}
}

function onElrsTransport(){
  var tcp=document.getElementById('elrs_transport').value==='0';
  document.querySelectorAll('.elrs-tcp').forEach(e=>e.style.display=tcp?'':'none');
}

async function saveElrsSettings(){
  var d={transport:parseInt(document.getElementById('elrs_transport').value)||0,pos:{}};
  ELRS_BOOLS.forEach(k=>{d[k]=document.getElementById('elrs_'+k).checked;});
  ELRS_TEXTS.forEach(k=>{d[k]=document.getElementById('elrs_'+k).value.trim();});
  ELRS_NUMS.forEach(k=>{d[k]=parseInt(document.getElementById('elrs_'+k).value)||0;});
  ELRS_POS.forEach(k=>{d.pos[k]=document.getElementById('elrsPos_'+k).value.trim();});
  try{
    var r=await fetch('/api/elrs',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});
    toast(r.ok?'✓ ELRS OSD 設定を保存しました':'⚠️ 保存エラー');
  }catch(e){toast('⚠️ 接続エラー');}
}

function elrsTest(){
  fetch('/api/elrs/test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slot:-1})})
    .then(()=>toast('🥽 OSDテストを送信しました（5秒間表示）',3000)).catch(()=>toast('⚠️ 接続エラー'));
}

function elrsAnnounce(){
  var t=document.getElementById('elrsAnnounceText').value.trim();if(!t)return;
  fetch('/api/elrs/announce',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text:t})})
    .then(()=>toast('📣 アナウンスを送信しました')).catch(()=>toast('⚠️ 接続エラー'));
}

// ── WiFi (host / client) ──────────────────────────────────────────────────
function onNetMode(){
  var sta=document.getElementById('netMode').value==='1';
  document.querySelectorAll('.net-sta').forEach(e=>e.style.display=sta?'':'none');
}
async function loadNetSettings(){
  try{
    var r=await fetch('/api/net');if(!r.ok)return;
    var d=await r.json();
    document.getElementById('netMode').value=String(d.mode);
    document.getElementById('netSsid').value=d.apSsid;document.getElementById('netPass').value=d.apPass;
    document.getElementById('netStaSsid').value=d.staSsid||'';
    var sp=document.getElementById('netStaPass');sp.value='';
    sp.placeholder=d.staPassSet?'設定済み（変更しない場合は空欄）':'パスワード';
    var st=d.status||{},txt;
    if(st.connected)txt='クライアント接続中 '+esc(st.ip)+'（'+st.rssi+' dBm）';
    else if(st.fallback)txt='⚠ クライアント接続失敗 → ホストで起動中 '+esc(st.ip);
    else txt='ホスト（アクセスポイント） '+esc(st.ip);
    document.getElementById('netStatus').innerHTML='<span>現在</span><b>'+txt+'</b>';
    onNetMode();
  }catch(e){}
}
async function saveNetSettings(){
  var d={mode:parseInt(document.getElementById('netMode').value)||0,
    apSsid:document.getElementById('netSsid').value.trim(),apPass:document.getElementById('netPass').value,
    staSsid:document.getElementById('netStaSsid').value.trim(),staPass:document.getElementById('netStaPass').value};
  if(d.apPass.length<8){toast('⚠️ アクセスポイントのパスワードは8文字以上');return;}
  if(d.mode===1&&!d.staSsid){toast('⚠️ 接続先 SSID を入力してください');return;}
  if(d.staPass&&d.staPass.length<8){toast('⚠️ 接続先パスワードは8文字以上');return;}
  var msg=d.mode===1
    ?'クライアントとして「'+d.staSsid+'」に接続します。再起動後は http://fpv-c5-lt.local（またはルーターが割り当てた IP）で開いてください。60秒接続できない場合は「'+d.apSsid+'」のアクセスポイントで起動します。よろしいですか？'
    :'ホスト（アクセスポイント「'+d.apSsid+'」）で再起動します。よろしいですか？';
  if(!confirm(msg))return;
  try{
    var r=await fetch('/api/net',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});
    if(!r.ok){toast('⚠️ '+((await r.json()).error||'保存エラー'));return;}
    await fetch('/api/reboot',{method:'POST'});
    toast('🔄 再起動します…',4000);
  }catch(e){toast('⚠️ 接続エラー');}
}

// ── SD backup ─────────────────────────────────────────────────────────────
async function sdBackup(){
  try{
    var r=await fetch('/api/sd/pilots/backup',{method:'POST'});
    toast(r.ok?'✅ SDカードにバックアップしました':'⚠️ バックアップエラー');
  }catch(e){toast('⚠️ 接続エラー');}
}
async function sdRestore(){
  if(!confirm('SDカードからパイロット情報を復元します。現在のデータは上書きされます。よろしいですか？'))return;
  try{
    var r=await fetch('/api/sd/pilots/restore',{method:'POST'});
    if(!r.ok){toast('⚠️ 復元エラー（/pilots.csv がありません）');return;}
    var j=await r.json();
    await loadActive();await loadRoster();applyActiveToSlots();buildRaceCards();buildCalibCards();
    toast('✅ SDから'+j.pilots+'人分を復元しました',3000);
  }catch(e){toast('⚠️ 接続エラー');}
}

function updateSdSection(present){
  sdPresent=present;
  var sec=document.getElementById('sdSection');
  if(sec)sec.style.display=present?'block':'none';
  var st=document.getElementById('sdTabStatus');
  if(st)st.innerHTML=present
    ?'<p style="color:var(--ok);font-size:12px">✅ SDカード検出済み — レース中はダウンロードしないでください</p>'
    :'<p style="color:var(--err);font-size:12px">⚠ SDカードが見つかりません</p>';
  if(!present){
    var wrap=document.getElementById('sdFileListWrap');
    if(wrap)wrap.innerHTML='<p style="color:var(--muted);font-size:12px">SDカードが挿入されていません</p>';
  }
}
