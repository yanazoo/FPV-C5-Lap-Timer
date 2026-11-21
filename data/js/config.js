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
    var item=document.createElement('div');item.className='roster-item'+(editingRosterId===r.id?'':' rv');item.id='ri-'+r.id;
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
        '<div class="rv-name"><span class="roster-name">'+esc(r.name)
          +(r.yomi?'<span class="roster-yomi">'+esc(r.yomi)+'</span>':'')+'</span></div>'
        +'<span class="roster-mac">'+(r.freq?esc(freqLabel(r.freq)):'<span style="color:var(--err)">VTX未設定</span>')+'</span>'
        +'<div class="rv-badges">'+activeBadge+osd
          +'<span class="link-badge" id="lk-'+r.id+'"></span>'+warn
          +'<span class="warn-badge vtx-fix" id="vx-'+r.id+'" style="display:none"></span></div>'
        +'<div class="roster-actions">'
          +'<select class="ch-select" data-rid="'+r.id+'" onchange="onChSelectChange(this)">'+chOpts+'</select>'
          +'<button onclick="startEdit('+r.id+')" class="btn-gray-soft" style="font-size:11px;padding:3px 8px">編集</button>'
          +'<button onclick="deleteRosterPilot('+r.id+')" class="btn-danger-soft" style="font-size:11px;padding:3px 8px">削除</button>'
        +'</div>';
    }
    list.appendChild(item);
  });
  updateLinkBadges();
}

// ── Channel assignment: manual or from the pilots' ELRS links ─────────────
var assignMode=0;
var LINK_WAIT={1:'VTX 未設定',2:'周波数が重なる',3:'空きチャンネルなし',4:'ほかのパイロットとバインドフレーズが同じ'};
function showAssignMode(){
  var sel=document.getElementById('assignModeSelect');if(sel&&document.activeElement!==sel)sel.value=String(assignMode);
  var n=document.getElementById('assignNote');if(n)n.style.display=assignMode?'':'none';
  updateLinkBadges();
}
var vtxAuto=true;
function showVtxAuto(){var t=document.getElementById('vtxModeSelect');if(t&&document.activeElement!==t)t.value=vtxAuto?'1':'0';}
function saveVtxAuto(){
  var v=document.getElementById('vtxModeSelect').value==='1';
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({vtxAuto:v})})
    .then(r=>{if(!r.ok)throw 0;vtxAuto=v;toast(v?'✓ VTX の違いを自動で反映します':'✓ VTX の違いはタップで反映します');})
    .catch(()=>{toast('⚠️ 保存エラー');showVtxAuto();});
}
function onVtxAuto(d){
  var r=rosterById[d.id];
  toast('📡 '+(r?r.name:'#'+d.id)+' の VTX を送信機の設定 '+freqLabel(d.freq)+' に変えました',4000);
}
function saveAssignMode(){
  var v=parseInt(document.getElementById('assignModeSelect').value,10);if(v!==1&&v!==2)v=0;
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({assignMode:v})})
    .then(r=>{if(!r.ok)throw 0;assignMode=v;showAssignMode();
      toast(['✓ チャンネルの割り当てを「手動」にしました','✓ チャンネルの割り当てを「自動（レースの合間だけ）」にしました','✓ チャンネルの割り当てを「自動（レース中も）」にしました'][v]);})
    .catch(()=>{toast('⚠️ 保存エラー');showAssignMode();});
}
// Per-pilot link badges and the status next to the mode (from the 1 Hz c5 status).
function updateLinkBadges(){
  var c=c5Status||{},el=c.elrs||{},links={};
  (c.links||[]).forEach(l=>{links[l.id]=l;});
  rosterData.forEach(function(r){
    var b=document.getElementById('lk-'+r.id);if(!b)return;
    var l=links[r.id];
    // The radio's VTX Administrator says another frequency than registered.
    var vx=document.getElementById('vx-'+r.id);
    if(vx){
      var diff=l&&l.vtx&&l.vtx!==r.freq;
      vx.style.display=diff?'':'none';
      if(diff){vx.textContent='⚠ 機体の VTX は '+freqLabel(l.vtx)+' ▶ 合わせる';vx.title='送信機の VTX Administrator の設定です。タップすると登録の VTX をこれに変えます';vx.onclick=function(){fixVtx(r.id,l.vtx);};}
    }
    if(!l){b.textContent='';b.className='link-badge';b.title='';return;}
    var w=l.wait&&assignMode?' · '+(LINK_WAIT[l.wait]||'待機中'):'';
    b.textContent=(l.on?'📶 LQ '+l.lq:'📴 リンクなし')+w;
    b.className='link-badge'+(l.on?' on':'');
    b.title=l.on?'送信機と機体がリンクしています':'送信機は見えていますが、機体とリンクしていません';
  });
  // ESP-NOW reception: without it these functions are off (manual).
  var avail=!!(c5Status&&el.watch);
  ['assignModeSelect','vtxModeSelect'].forEach(function(id){var s=document.getElementById(id);if(s)s.disabled=!avail;});
  var st=document.getElementById('assignStatus');if(!st)return;
  var why=!el.enabled?'ELRS OSD が OFF です':!el.connected?'ブリッジにつながっていません':'ブリッジから応答がありません（電源・配線、またはブリッジのファームが v2.11 より古い）';
  if(!avail){
    st.textContent=c5Status?'⚠ ESP-NOW を受信できません：'+why+'。自動割り当てと VTX の反映は無効で、手動で動きます。':'';
    st.style.color='var(--warn)';return;
  }
  if(!assignMode){st.textContent='';return;}
  var msg,col='var(--muted)',racing=c.race&&(c.race.run||c.race.paused||c.race.laps>0||c.race.wait>0);
  if(racing&&assignMode===1)msg='⏸ レースをクリアしたら反映します';
  else if(racing)msg='🏁 レース中も反映しています（リンクが切れたパイロットはチャンネルから外れ、ラップは残ります）',col='var(--ok)';
  else if(el.linkAge>=0&&el.linkAge<5)msg='📶 リンクを受信中',col='var(--ok)';
  else msg='送信機の電波を待っています';
  st.textContent=msg;st.style.color=col;
}
// Unregistered radios (c5 "cand"), redrawn only when something changed.
var candHtml='';
function renderCandidates(){
  var box=document.getElementById('candBox');if(!box)return;
  var c=(c5Status&&c5Status.cand)||[];
  var h=c.map(function(x){
    return '<div class="cand-row"><div class="cand-info"><span>送信機 <span class="cand-id">'+esc(x.tag)+'</span></span>'
      +'<span class="link-badge'+(x.on?' on':'')+'">'+(x.on?'📶 LQ '+x.lq:'📴 リンクなし')+'</span>'
      +(x.vtx?'<span class="osd-badge">VTX '+esc(freqLabel(x.vtx))+'</span>':'<span class="res-muted" style="font-size:10px">VTX 不明</span>')
      +'</div><button class="btn-success" style="font-size:11px;padding:3px 10px" onclick="registerCand(\''+esc(x.tag)+'\')">＋ 登録</button></div>';
  }).join('');
  box.style.display=c.length?'':'none';
  if(h!==candHtml){candHtml=h;document.getElementById('candList').innerHTML=h;}
}
function registerCand(tag){
  var x=((c5Status&&c5Status.cand)||[]).find(function(c){return c.tag===tag;});
  if(x)showAddForm({tag:x.tag,vtx:x.vtx||0});
}
async function fixVtx(id,mhz){
  var r=rosterById[id];if(!r)return;
  if(!confirm(r.name+' の VTX を '+freqLabel(r.freq)+' から '+freqLabel(mhz)+' に変えますか？'))return;
  try{
    var res=await fetch('/api/pilots',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({id:id,name:r.name,yomi:r.yomi||'',freq:mhz,bind:r.bind||'',osd:r.osd!==false})});
    if(!res.ok)throw 0;
    toast('✓ '+r.name+' の VTX を '+freqLabel(mhz)+' にしました');loadRoster();
  }catch(e){toast('⚠️ 保存エラー');}
}
function onAutoAssign(d){
  var nm=id=>(rosterById[id]&&rosterById[id].name)||('#'+id);
  (d.joined||[]).forEach(id=>toast('📶 '+nm(id)+' がリンクしたのでチャンネルに入れました'));
  (d.left||[]).forEach(id=>toast('📴 '+nm(id)+' のリンクが切れたのでチャンネルから外しました'+(d.race?'（ラップは残ります）':''),d.race?4000:undefined));
}

// Shared by the add form and inline edit. prefix: 'add' | 'edit'.
function pilotFormHtml(prefix,r){
  r=r||{name:'',yomi:'',freq:0,bind:'',osd:true,enter:DEF_ENTER,exit:DEF_EXIT};
  var inp='background:var(--bg);border:1px solid var(--bd);color:var(--tx);border-radius:5px;padding:4px 7px;font-size:12px';
  return '<div class="pilot-form">'
    +'<input type="text" id="'+prefix+'Name" value="'+esc(r.name)+'" maxlength="20" placeholder="パイロット名（OSD送信先の既定バインドフレーズ）" autocomplete="off" style="'+inp+'">'
    +'<input type="text" id="'+prefix+'Yomi" value="'+esc(r.yomi||'')+'" maxlength="20" placeholder="よみかた（TTS読み上げ用）" autocomplete="off" style="'+inp+'">'
    +'<label class="pf-row"><span>VTX</span>'+freqSelectHtml(prefix+'Freq',r.freq||0)+'</label>'
    +'<div class="pf-row"><span>ELRS</span><input type="text" id="'+prefix+'Bind" value="'+esc(r.uid||r.bind||'')+'" maxlength="40" placeholder="'+bindPlaceholder(prefix,!!r.uid)+'" autocomplete="off" autocapitalize="off" spellcheck="false" style="flex:1;min-width:0;'+inp+'">'
      +'<label style="white-space:nowrap;cursor:pointer;font-size:12px" title="バインドフレーズの代わりに、送信機の UID（6 つの数字）を入れます"><input type="checkbox" id="'+prefix+'IsUid"'+(r.uid?' checked':'')+' onchange="onBindKind(\''+prefix+'\')"> UID</label></div>'
    +'<label class="pf-row"><span>OSD</span><input type="checkbox" id="'+prefix+'Osd"'+(r.osd!==false?' checked':'')+'> ゴーグルにラップを表示</label>'
    +'<div class="pf-row"><span>入/出</span>'
      +'<input type="number" id="'+prefix+'Enter" value="'+r.enter+'" min="0" max="1023" style="width:64px;'+inp+'">'
      +'<input type="number" id="'+prefix+'Exit" value="'+r.exit+'" min="0" max="1023" style="width:64px;'+inp+'">'
      +'<span style="color:var(--muted);font-size:10px">0〜1023（キャリブで調整）</span></div>'
    +'</div>';
}

// The ELRS field holds a bind phrase, or with "UID" ticked the radio's UID
// (six numbers as the ELRS configurator shows them, or 12 hex digits).
function bindPlaceholder(prefix,uid){
  if(!uid)return 'バインドフレーズ（空欄=パイロット名）';
  return prefix==='add'&&addCand?'UID（空欄なら送信機 '+addCand.tag+' の UID）':'UID（例 210,141,145,12,34,56）';
}
function onBindKind(prefix){
  var u=document.getElementById(prefix+'IsUid').checked,b=document.getElementById(prefix+'Bind');
  b.placeholder=bindPlaceholder(prefix,u);
  if(prefix==='add')checkAddUid();
}
var UID_DEC=/^\s*\d{1,3}([\s,.]+\d{1,3}){5}\s*$/, UID_HEX=/^\s*([0-9a-fA-F]{2}[:\-\s]?){5}[0-9a-fA-F]{2}\s*$/;
function validUid(s){
  if(UID_HEX.test(s)&&!UID_DEC.test(s))return true;
  return UID_DEC.test(s)&&s.trim().split(/[\s,.]+/).every(x=>parseInt(x,10)<=255);
}
function readPilotForm(prefix){
  var v=function(k){return document.getElementById(prefix+k);};
  var p={
    name:v('Name').value.trim(), yomi:v('Yomi').value.trim(),
    freq:parseInt(v('Freq').value)||0, bind:v('Bind').value.trim(), osd:v('Osd').checked,
    enter:Math.max(0,Math.min(RSSI_MAX,parseInt(v('Enter').value)||DEF_ENTER)),
    exit:Math.max(0,Math.min(RSSI_MAX,parseInt(v('Exit').value)||DEF_EXIT))
  };
  if(v('IsUid').checked){p.uid=p.bind;p.bind='';}
  else p.uidClear=true;   // a phrase (or the name): not the UID any more
  return p;
}

// The timer keeps a name in 31 bytes: 31 letters, or 10 Japanese characters.
function utf8Bytes(s){try{return new TextEncoder().encode(s).length;}catch(e){return unescape(encodeURIComponent(s)).length;}}
function validPilot(p){
  if(!p.name){toast('⚠️ 名前を入力してください');return false;}
  if(utf8Bytes(p.name)>31){toast('⚠️ 名前が長すぎます（日本語は 10 文字、英数字は 31 文字まで）',5000);return false;}
  if(utf8Bytes(p.yomi)>31){toast('⚠️ よみかたが長すぎます（ひらがなは 10 文字まで）',5000);return false;}
  if(utf8Bytes(p.bind)>32){toast('⚠️ バインドフレーズが長すぎます（英数字 32 文字まで）',5000);return false;}
  if(p.uid!==undefined&&p.uid!==''&&!validUid(p.uid)){toast('⚠️ UID は 0〜255 の数字 6 つで入れてください（例 210,141,145,12,34,56）',5000);return false;}
  if(p.uid===''){toast('⚠️ UID を入れてください');return false;}
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

// cand: a radio from the unregistered list (its VTX goes in, and the bind
// phrase typed is checked against it).
var addCand=null, uidCheckT=null, uidCheckSeq=0;
function showAddForm(cand){
  addCand=cand||null;
  var f=document.getElementById('addForm');
  document.getElementById('addFormBody').innerHTML=pilotFormHtml('add',cand?{name:'',yomi:'',freq:cand.vtx||0,bind:'',osd:true,enter:DEF_ENTER,exit:DEF_EXIT}:null);
  document.getElementById('addUidCheck').textContent='';
  f.style.display='block';document.getElementById('addName').focus();
  if(addCand){f.scrollIntoView({block:'nearest'});checkAddUid();}
}
function hideAddForm(){document.getElementById('addForm').style.display='none';addCand=null;}
// The phrase (or the name, with the phrase empty) against the radio picked.
function checkAddUid(){
  clearTimeout(uidCheckT);
  var out=document.getElementById('addUidCheck');
  if(!addCand){out.textContent='';return;}
  if((document.getElementById('addIsUid')||{}).checked){
    var typed=((document.getElementById('addBind')||{}).value||'').trim();
    if(!typed){addCand.match=true;out.textContent='🔑 送信機 '+addCand.tag+' の UID で登録します（バインドフレーズは使いません）';out.style.color='var(--ok)';return;}
    addCand.match=true;out.textContent='';return;
  }
  var bind=(document.getElementById('addBind')||{}).value||'',name=(document.getElementById('addName')||{}).value||'';
  var phrase=bind.trim()||name.trim();
  if(!phrase){out.textContent='送信機 '+addCand.tag+' のバインドフレーズ（空欄なら名前）を入れてください';out.style.color='var(--muted)';addCand.match=false;return;}
  var seq=++uidCheckSeq;
  uidCheckT=setTimeout(function(){
    fetch('/api/bind_uid?p='+encodeURIComponent(phrase)).then(r=>r.json()).then(function(d){
      if(seq!==uidCheckSeq||!addCand)return;
      addCand.match=d.tag===addCand.tag;
      out.textContent=addCand.match?'✓ 送信機 '+addCand.tag+' と一致しました':'✗ 送信機 '+addCand.tag+' と一致しません（'+(bind.trim()?'バインドフレーズ':'名前（フレーズ空欄）')+'を確かめてください）';
      out.style.color=addCand.match?'var(--ok)':'var(--err)';
    }).catch(function(){});
  },250);
}
document.addEventListener('input',function(e){if(addCand&&e.target&&(e.target.id==='addName'||e.target.id==='addBind'))checkAddUid();});

async function submitAdd(){
  var p=readPilotForm('add');
  // A radio from the list, "UID" ticked and nothing typed: its own UID.
  var byUid=addCand&&p.uid==='';
  if(byUid){delete p.uid;p.uidTag=addCand.tag;}
  if(!validPilot(p))return;
  else if(addCand&&!addCand.match&&!confirm('バインドフレーズが送信機 '+addCand.tag+' と一致しません。このまま登録しますか？'))return;
  try{
    var r=await fetch('/api/pilots',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});
    if(r.status===409&&byUid){toast('⚠️ 送信機 '+addCand.tag+' の電波が消えました。送信機の電源を入れて、もう一度試してください',5000);return;}
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
    if(res.status===409){toast('⚠️ レースをクリアしてから削除してください（ラップが残っています）',5000);return;}
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
  var g=Math.max(0,Math.min(89,parseInt(document.getElementById('gainInput').value)||15));
  document.getElementById('gainInput').value=g;
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({gain:g})})
    .then(r=>toast(r.ok?'✓ ゲインを '+g+' に設定しました（閾値の再調整を推奨）':'⚠️ 保存エラー',3000)).catch(()=>toast('⚠️ 接続エラー'));
}

// ── Ranking rule / timed race ───────────────────────────────────────────
function showRaceRules(){
  setField('rankModeSelect',String(rankMode));
  setField('timeLimitMin',Math.floor(timeLimitSec/60));
  setField('timeLimitSec',timeLimitSec%60);
  setField('countLastLapSelect',countLastLap?'1':'0');
  document.querySelectorAll('.rank-timed').forEach(e=>e.style.display=rankMode===2?'':'none');
  // Not racing: show the new start value (the time limit, or 00:00).
  if(!timerH&&!countdownH)timerEl.textContent=raceClockText(timerFrozenMs);
}
function saveRaceRules(){
  var m=parseInt(document.getElementById('rankModeSelect').value)||0;
  var mi=parseInt(document.getElementById('timeLimitMin').value)||0;
  var se=parseInt(document.getElementById('timeLimitSec').value)||0;
  var t=Math.max(30,Math.min(1800,mi*60+se));
  var c=document.getElementById('countLastLapSelect').value==='1';
  rankMode=m;timeLimitSec=t;countLastLap=c;
  showRaceRules();renderRanking();
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({rankMode:m,timeLimitSec:t,countLastLap:c})})
    .then(r=>toast(r.ok?'✓ レース内容を保存しました'+(raceRunning?'（制限時間は次のスタートから）':''):'⚠️ 保存エラー'))
    .catch(()=>toast('⚠️ 接続エラー'));
}

// Total laps (settings → レース内容); the firmware needs it for FINISHED! on the OSD.
function saveTotalLaps(){
  var t=Math.max(0,Math.min(99,parseInt(document.getElementById('totalLaps').value)||0));
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({totalLaps:t})})
    .then(r=>toast(r.ok?'✓ 総周回数を保存しました':'⚠️ 保存エラー')).catch(()=>toast('⚠️ 接続エラー'));
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
    var rs=document.getElementById('elrs_rdSwitch');if(rs)rs.value=String(d.rdSwitch||0);
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
  var rs=document.getElementById('elrs_rdSwitch');if(rs)d.rdSwitch=parseInt(rs.value)||0;
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
  onNetIpMode();
}
var netNets=[];   // [{ssid, passSet}] from the timer
function renderNetStaList(sel){
  var auto=document.getElementById('netStaAuto').value==='1',h='';
  for(var i=0;i<5;i++){
    var n=netNets[i]||{};
    h+='<div class="config-row" style="gap:6px">'
      +'<input type="radio" name="netStaSel" value="'+i+'"'+(i===sel?' checked':'')+(auto?' disabled':'')+' title="この接続先を使う" style="width:18px;height:18px;flex:none">'
      +'<input type="text" id="netStaSsid'+i+'" maxlength="32" autocomplete="off" placeholder="SSID '+(i+1)+'" value="'+esc(n.ssid||'')+'" style="flex:1;min-width:0;background:var(--sf2);border:1px solid var(--bd);color:var(--tx);border-radius:6px;padding:6px 8px;font-size:13px">'
      +'<input type="text" class="pw-mask" id="netStaPass'+i+'" maxlength="63" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false" data-lpignore="true" data-1p-ignore placeholder="'+(n.passSet?'設定済み':'パスワード')+'" style="flex:1;min-width:0;background:var(--sf2);border:1px solid var(--bd);color:var(--tx);border-radius:6px;padding:6px 8px;font-size:13px">'
      +'</div>';
  }
  document.getElementById('netStaList').innerHTML=h;
}
function netStaSel(){var r=document.querySelector('input[name=netStaSel]:checked');return r?parseInt(r.value):0;}
function onNetStaAuto(){
  var auto=document.getElementById('netStaAuto').value==='1';
  document.querySelectorAll('input[name=netStaSel]').forEach(e=>e.disabled=auto);
}
function onNetIpMode(){
  var fixed=document.getElementById('netIpMode').value==='1';
  document.querySelectorAll('.net-ip-fixed').forEach(e=>e.style.display=fixed?'':'none');
}
var IPV4=/^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/;
// Why the timer could not join the client network (WiFi disconnect reason).
function netReasonText(r){
  if(!r)return '';
  if(r===201)return '接続先の WiFi が見つかりません（SSID の綴り、ルーターの電源、2.4GHz が有効か、距離を確認）';
  if(r===210||r===211)return '接続先は見つかりましたが、暗号化の方式に対応していません（WPA2 / WPA3 にしてください）';
  if(r===202||r===15||r===204||r===14)return 'パスワードが違う可能性があります';
  if(r===200)return '電波が弱く、接続が切れました';
  if(r===5)return 'ルーターの接続台数がいっぱいです';
  return 'エラー '+r;
}
async function loadNetSettings(){
  try{
    var r=await fetch('/api/net');if(!r.ok)return;
    var d=await r.json();
    document.getElementById('netMode').value=String(d.mode);
    document.getElementById('netSsid').value=d.apSsid;document.getElementById('netPass').value=d.apPass;
    netNets=d.nets||[{ssid:d.staSsid||'',passSet:!!d.staPassSet}];
    document.getElementById('netStaAuto').value=d.staAuto?'1':'0';
    renderNetStaList(d.staSel||0);
    document.getElementById('netApCh').value=String(d.apChannel||6);
    document.getElementById('netFallback').value=String(d.fallbackSec||30);
    document.getElementById('netRaceHold').value=String(d.raceHoldSec!==undefined?d.raceHoldSec:60);
    document.getElementById('netIpMode').value=d.ipFixed?'1':'0';
    document.getElementById('netIp').value=d.ip||'';
    document.getElementById('netMask').value=d.mask||'255.255.255.0';
    document.getElementById('netGw').value=d.gw||'';
    document.getElementById('netDns').value=d.dns||'';
    var st=d.status||{},txt;
    if(st.connected)txt='クライアント接続中「'+esc(d.staSsid||'')+'」 '+esc(st.ip)+'（'+st.rssi+' dBm）';
    else if(st.fallback){
      txt='⚠ クライアント接続失敗 → ホストで起動中 '+esc(st.ip);
      var why=netReasonText(st.reason);
      if(why)txt+='<br><span style="font-weight:normal">理由：'+why+'</span>';
      txt+='<br><span style="font-weight:normal;color:var(--muted)">'+(st.retrying
        ?'接続先が見つかり次第、自動で再起動して接続します（このアクセスポイントに誰もつないでいない間・レース中以外）'
        :'自動では戻りません。設定を確認してから再起動してください')+'</span>';
    }
    else txt='ホスト（アクセスポイント） '+esc(st.ip);
    document.getElementById('netStatus').innerHTML='<span>現在</span><b>'+txt+'</b>';
    onNetMode();
    renderConnQr(d);
  }catch(e){}
}
async function saveNetSettings(){
  var d={mode:parseInt(document.getElementById('netMode').value)||0,
    apSsid:document.getElementById('netSsid').value.trim(),apPass:document.getElementById('netPass').value,
    nets:[0,1,2,3,4].map(i=>({ssid:document.getElementById('netStaSsid'+i).value.trim(),pass:document.getElementById('netStaPass'+i).value})),
    staSel:netStaSel(),staAuto:document.getElementById('netStaAuto').value==='1',
    apChannel:parseInt(document.getElementById('netApCh').value)||6,
    fallbackSec:parseInt(document.getElementById('netFallback').value)||30,
    raceHoldSec:(v=>isNaN(v)?60:v)(parseInt(document.getElementById('netRaceHold').value)),
    ipFixed:document.getElementById('netIpMode').value==='1',
    ip:document.getElementById('netIp').value.trim(),mask:document.getElementById('netMask').value.trim(),
    gw:document.getElementById('netGw').value.trim(),dns:document.getElementById('netDns').value.trim()};
  if(d.mode===1&&d.ipFixed){
    if(!IPV4.test(d.ip)){toast('⚠️ IP アドレスを正しく入力してください');return;}
    if(!IPV4.test(d.mask)){toast('⚠️ サブネットマスクを正しく入力してください');return;}
    if(!IPV4.test(d.gw)){toast('⚠️ ゲートウェイを正しく入力してください');return;}
    if(d.dns&&!IPV4.test(d.dns)){toast('⚠️ DNS を正しく入力してください');return;}
  }
  if(d.apPass.length<8){toast('⚠️ アクセスポイントのパスワードは8文字以上');return;}
  var named=d.nets.filter(n=>n.ssid);
  if(d.mode===1&&(d.staAuto?!named.length:!d.nets[d.staSel].ssid)){toast(d.staAuto?'⚠️ 接続先 SSID を入力してください':'⚠️ ● を付けた接続先に SSID を入力してください');return;}
  if(d.nets.some(n=>n.pass&&n.pass.length<8)){toast('⚠️ 接続先パスワードは8文字以上');return;}
  var target=d.staAuto?'登録した接続先のうち電波が一番強いもの（'+named.map(n=>'「'+n.ssid+'」').join('')+'）':'「'+d.nets[d.staSel].ssid+'」';
  var msg=d.mode===1
    ?'クライアントとして'+target+'に接続します。再起動後は '+(d.ipFixed?'http://'+d.ip+'/':'http://fpv-c5-lt.local（またはルーターが割り当てた IP）')+' で開いてください。'+d.fallbackSec+'秒接続できない場合は「'+d.apSsid+'」のアクセスポイントで起動します。よろしいですか？'
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
    var j={};try{j=await r.json();}catch(e){}
    toast(r.ok?'✅ パイロット情報を '+(j.file||'SD カード')+' に保存しました':'⚠️ 保存できませんでした',3000);
  }catch(e){toast('⚠️ 接続エラー');}
}
async function sdRestore(){
  var path=await pickBackup('pilots');if(!path)return;
  try{
    var r=await fetch('/api/sd/pilots/restore',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path})});
    if(r.status===409){toast('⚠️ レースをクリアしてから復元してください（ラップが残っています）',5000);return;}
    if(!r.ok){toast('⚠️ 復元できませんでした');return;}
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
    ?'<p style="color:var(--ok);font-size:12px">✅ SDカード検出済み — レース中は操作できません。</p>'
    :'<p style="color:var(--err);font-size:12px">⚠ SDカードが見つかりません</p>';
  if(!present){
    var wrap=document.getElementById('sdFileListWrap');
    if(wrap)wrap.innerHTML='<p style="color:var(--muted);font-size:12px">SDカードが挿入されていません</p>';
  }
}

// ── 接続台数 ────────────────────────────────────────────────────────────
// Limits per role and WiFi mode; the timer sends the allowed maxima.
function fillConnSelect(id,min,max,val,unit){
  var s=document.getElementById(id);if(!s)return;
  var h='';
  for(var i=min;i<=max;i++)h+='<option value="'+i+'"'+(i===val?' selected':'')+'>'+i+' 台'+'</option>';
  s.innerHTML=h;
}
async function loadConnLimits(){
  try{
    var r=await fetch('/api/conn');if(!r.ok)throw 0;
    var c=await r.json();
    connInfo=c;
    fillConnSelect('connAdmin',1,c.adminLimit,c.adminMax);
    // At least one viewer place: with none, a browser not logged in yet is
    // closed within seconds and could never log in.
    fillViewHost(c.adminMax,c.viewHost);
    fillConnSelect('connViewClient',1,c.viewClientLimit,Math.max(1,c.viewClient));
    var h=document.getElementById('connHold');if(h&&c.holdSec)h.value=String(c.holdSec);
    var now=document.getElementById('connNow');
    if(now)now.innerHTML='<span>現在（'+(c.ap?'ホスト':'クライアント')+'モード）</span><b>管理者 '+c.logins+' / '+c.adminMax
      +' ・ 観戦 '+c.viewers+' / '+c.viewerLimit+'</b>';
  }catch(e){}
}
// Host mode: admins + viewers fit our access point (hostTotal = 10 places
// less one for the netpack), so the viewer choices follow the admin one.
var connInfo=null;
function viewHostMax(admins){
  var c=connInfo||{},total=c.hostTotal||9;
  return Math.max(1,Math.min(c.viewHostLimit||8,total-admins));
}
function fillViewHost(admins,val){
  var max=viewHostMax(admins);
  fillConnSelect('connViewHost',1,max,Math.min(max,Math.max(1,val)));
  var n=document.getElementById('connHostNote');
  if(n)n.textContent='ホストモードでは、管理者と観戦の合計が '+((connInfo&&connInfo.hostTotal)||9)+' 台まで（タイマーの WiFi は 10 台まで、うち 1 台は netpack 用）。管理者 '+admins+' 台なら観戦は '+max+' 台までです。';
}
function onConnAdmin(){
  var v=id=>parseInt(document.getElementById(id).value,10);
  fillViewHost(v('connAdmin'),v('connViewHost'));
  saveConnLimits();
}
async function saveConnLimits(){
  var v=id=>parseInt(document.getElementById(id).value,10);
  try{
    var r=await fetch('/api/conn',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({adminMax:v('connAdmin'),viewHost:v('connViewHost'),viewClient:v('connViewClient'),holdSec:v('connHold')})});
    if(!r.ok)throw 0;
    toast('✅ 接続台数を保存しました');
    loadConnLimits();
  }catch(e){toast('⚠️ 保存エラー');}
}

// ── 再起動 ──────────────────────────────────────────────────────────────
async function restartAll(){
  if(raceRunning){toast('レース中は再起動できません');return;}
  if(!confirm('タイマーを再起動します。よろしいですか？\n（再起動後はログインし直してください）'))return;
  try{
    var r=await fetch('/api/restart',{method:'POST'});
    if(!r.ok)throw 0;
    waitForRestart();
  }catch(e){toast('⚠️ 接続エラー');}
}
// Every open page (admins and viewers) reloads, this one too.
async function reloadAllPages(){
  if(!confirm('つながっているすべての画面（観戦者も含む）を再読み込みします。よろしいですか？'))return;
  try{
    var r=await fetch('/api/reload-all',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token:authToken})});
    if(r.status===403){toast('⚠️ ログインし直してください');return;}
    if(!r.ok)throw 0;
  }catch(e){toast('⚠️ 接続エラー');}
}
// After a restart: wait until the timer answers again, then reload the page.
function waitForRestart(){
  toast('🔄 再起動しています…（戻ったら自動で開き直します）',8000);
  var t0=Date.now(),seenDown=false;
  var poll=async()=>{
    try{
      var c=new AbortController();setTimeout(()=>c.abort(),1500);
      var r=await fetch('/api/sys',{cache:'no-store',signal:c.signal});
      if(r.ok){var j=await r.json();if(seenDown||j.uptime<20){location.reload();return;}}
    }catch(e){seenDown=true;}
    if(Date.now()-t0<90000)setTimeout(poll,2000);
    else toast('⚠️ タイマーが戻りません。電源を入れ直してください',10000);
  };
  setTimeout(poll,3000);
}

// ── 状態（起動からの時間・前回の起動・空きメモリー） ──────────────────────
var SYS_REASON={poweron:'電源オン',restart:'再起動ボタン',sw:'再起動',panic:'エラー（パニック）',watchdog:'エラー（ウォッチドッグ）',
  brownout:'電源の電圧低下',reset:'リセットボタン',usb:'USB',other:'その他'};
async function loadSysInfo(){
  var el=document.getElementById('sysInfo');if(!el)return;
  try{
    var r=await fetch('/api/sys');if(!r.ok)throw 0;
    var s=await r.json(),up=s.uptime,kb=v=>Math.round(v/1024)+' KB';
    var upTxt=up<60?up+' 秒':up<3600?Math.floor(up/60)+' 分':Math.floor(up/3600)+' 時間'+Math.floor(up%3600/60)+' 分';
    var h='起動から '+upTxt+'・今回の起動：'+(SYS_REASON[s.reason]||s.reason)
      +'<br>空きメモリー '+kb(s.heap)+'（起動後の最小 '+kb(s.heapMin)+'）';
    // Which build runs: the firmware (upload) and the web UI (uploadfs).
    var wb=window.WEBUI_BUILD||'-',fb=s.build||'-';
    h+='<br>ビルド：ファーム '+esc(fb)+'・Web 画面 '+esc(wb);
    if(fb!=='-'&&wb!=='-'&&fb.replace('+','')!==wb.replace('+',''))
      h+='<br><span style="color:var(--warn)">⚠ ファームと Web 画面のビルドが違います。upload と uploadfs の両方を、同じ版で書き込んでください</span>';
    if(s.crashes)h+='<br><span style="color:var(--warn)">⚠ エラーによる再起動：これまで '+s.crashes+' 回（最後は'
      +(s.lastCrashAgo?' '+s.lastCrashAgo+' 回前の起動':'今回の起動')+'・'+(SYS_REASON[s.lastCrash]||s.lastCrash)+'）</span>';
    el.innerHTML=h;
  }catch(e){el.textContent='';}
}

// ── 接続用 QR コード ────────────────────────────────────────────────────
// Host mode: ① join the timer's WiFi, ② open the page. Client mode: open
// the page by IP address (the phone on the same router). The QR library
// (qrcode-generator, MIT) is loaded only when this card is shown.
var qrLib=null,qrItems=[];
function loadQrLib(){
  if(qrLib)return qrLib;
  qrLib=new Promise((ok,ng)=>{
    var s=document.createElement('script');s.src='/js/qrcode.js';
    s.onload=()=>{qrcode.stringToBytes=qrcode.stringToBytesFuncs['UTF-8'];ok();};
    s.onerror=()=>{qrLib=null;ng();};
    document.head.appendChild(s);
  });
  return qrLib;
}
// WIFI: fields escape \ ; , : "
function wifiEsc(s){return String(s).replace(/([\\;,:"])/g,'\\$1');}
// Join a network: WPA with a password, or an open one.
function wifiQr(ssid,pass){
  return pass?'WIFI:T:WPA;S:'+wifiEsc(ssid)+';P:'+wifiEsc(pass)+';;':'WIFI:T:nopass;S:'+wifiEsc(ssid)+';;';
}
function qrSvg(text){
  var q=qrcode(0,'M');q.addData(text);q.make();
  return q.createSvgTag({cellSize:4,margin:2,scalable:true});
}
async function renderConnQr(d){
  var wrap=document.getElementById('qrWrap');if(!wrap)return;
  qrItems=await buildQrItems(d);
  try{await loadQrLib();}catch(e){wrap.innerHTML='<span class="res-muted">QR コードを作れませんでした（再読み込みしてください）</span>';return;}
  wrap.innerHTML=qrHtml();
}
// The connection QR codes for /api/net's answer d: [{cap,sub,text,pass}].
async function buildQrItems(d){
  var st=(d&&d.status)||{},ip=st.ip||location.hostname,url='http://'+ip+'/';
  var qrItems=[];
  if(st.ap){
    qrItems.push({cap:'① タイマーの WiFi に接続',sub:d.apSsid,pass:d.apPass,text:wifiQr(d.apSsid,d.apPass)});
    qrItems.push({cap:'② 画面を開く',sub:url,text:url});
  }else{
    // Client mode: the router's network (the password only comes to a
    // logged-in admin), then the page.
    var w=null;
    try{
      var r=await fetch('/api/net/wifi-qr',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token:authToken})});
      if(r.ok)w=await r.json();
    }catch(e){}
    if(w&&w.ssid){
      qrItems.push({cap:'① ルーターの WiFi に接続',sub:w.ssid,pass:w.pass,text:wifiQr(w.ssid,w.pass)});
      qrItems.push({cap:'② 画面を開く',sub:url,text:url});
    }else{
      qrItems.push({cap:'画面を開く（同じ WiFi に接続してから）',sub:url,text:url});
    }
  }
  return qrItems;
}
function qrHtml(){
  return qrItems.map(i=>'<div class="qr-item"><div class="qr">'+qrSvg(i.text)+'</div><div class="cap">'+esc(i.cap)+'</div><div class="sub">'+esc(i.sub)+'</div></div>').join('');
}
function showQrBig(){
  if(!qrItems.length||typeof qrcode==='undefined')return;
  document.getElementById('qrBig').innerHTML=qrHtml();
  document.getElementById('qrModal').style.display='flex';
}

// ── Start delay (settings → レース内容) ─────────────────────────────────
var START_DELAY_PRESETS=[10,15,30,60];
function showStartDelay(){
  var tg=document.getElementById('startDelayToggle');if(tg)tg.checked=startDelayOn;
  var stl=document.getElementById('startDelayState');if(stl)stl.textContent=startDelayOn?'ON':'OFF';
  var sr=document.getElementById('startDelaySelRow');if(sr)sr.style.display=startDelayOn?'':'none';
  var sel=document.getElementById('startDelaySelect');if(!sel)return;
  var preset=START_DELAY_PRESETS.indexOf(startDelaySec)>=0;
  sel.value=preset?String(startDelaySec):'custom';
  if(!preset)setField('startDelayCustom',startDelaySec);
  var row=document.getElementById('startDelayCustomRow');if(row)row.style.display=preset||!startDelayOn?'none':'';
}
function toggleStartDelay(){
  startDelayOn=document.getElementById('startDelayToggle').checked;
  showStartDelay();
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({startDelayOn:startDelayOn})})
    .then(r=>toast(r.ok?(startDelayOn?'✓ スタートの待ち時間を ON（'+startDelaySec+' 秒）にしました':'✓ スタートの待ち時間を OFF にしました'):'⚠️ 保存エラー'))
    .catch(()=>toast('⚠️ 接続エラー'));
}
function onStartDelaySelect(){
  var v=document.getElementById('startDelaySelect').value;
  var row=document.getElementById('startDelayCustomRow');
  if(v==='custom'){if(row)row.style.display='';saveStartDelay();return;}
  if(row)row.style.display='none';
  postStartDelay(parseInt(v,10)||10);
}
function saveStartDelay(){
  var n=Math.max(1,Math.min(600,parseInt(document.getElementById('startDelayCustom').value,10)||1));
  setField('startDelayCustom',n);
  postStartDelay(n);
}
function postStartDelay(n){
  startDelaySec=n;
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({startDelaySec:n})})
    .then(r=>toast(r.ok?'✓ スタートの待ち時間を '+n+' 秒にしました':'⚠️ 保存エラー'))
    .catch(()=>toast('⚠️ 接続エラー'));
}

// ── ファーム更新 ──────────────────────────────────────────────────────────
var fwPoll=null;
var FW_REASON={login:'ログインし直してください',race:'レース中・ラップが残っている間は更新できません（クリアしてください）',
  busy:'ほかの更新が動いています',offline:'インターネットにつながっていません（クライアントモードでルーターに接続してください）',
  nocheck:'先に「最新版を確認」を押してください',part:'ファイルの種類が違います',empty:'ファイルが空です',
  notfirmware:'firmware.bin ではないようです',size:'ファイルが大きすぎます',write:'書き込みに失敗しました',verify:'書き込んだ内容の確認に失敗しました'};
function fwSay(t,col){var e=document.getElementById('fwStatus');if(e){e.textContent=t;e.style.color=col||'';}}
async function loadFwInfo(){
  try{
    var s=await (await fetch('/api/update')).json();
    var v=document.getElementById('fwVer');
    if(v)v.textContent='このタイマー：'+s.version+'（ビルド '+s.build+'）'+(s.online?'':'・インターネット：つながっていません');
    var b=document.getElementById('fwCheckBtn');if(b)b.disabled=!s.online;
    return s;
  }catch(e){return null;}
}
async function fwPost(url){
  var r=await fetch(url+(url.indexOf('?')<0?'?':'&')+'token='+encodeURIComponent(authToken||''),{method:'POST'});
  var j={};try{j=await r.json();}catch(e){}
  if(!r.ok||j.ok===false)throw (FW_REASON[j.reason]||j.reason||('HTTP '+r.status));
  return j;
}
async function fwCheck(){
  clearInterval(fwPoll);
  document.getElementById('fwOnlineBtn').style.display='none';
  try{
    await fwPost('/api/update/check');fwSay('最新版を確認しています…');
    fwPoll=setInterval(async function(){
      var s=await loadFwInfo();if(!s||s.state==='checking')return;
      clearInterval(fwPoll);
      if(s.state==='failed'){fwSay('⚠ '+s.error,'var(--warn)');return;}
      if(s.latest===s.version){fwSay('✓ 最新版です（'+s.latest+'）','var(--ok)');return;}
      fwSay('新しい版があります：'+s.latest+'（このタイマーは '+s.version+'）','var(--accent)');
      var b=document.getElementById('fwOnlineBtn');b.style.display='';b.textContent='⬇️ '+s.latest+' に更新';
    },1000);
  }catch(e){fwSay('⚠ '+e,'var(--warn)');}
}
async function fwOnline(){
  clearInterval(fwPoll);
  var s=await loadFwInfo();if(!s)return;
  if(!confirm(s.latest+' に更新します。書き込みに 1〜2 分かかり、終わると再起動します。途中で電源を切らないでください。'))return;
  try{
    await fwPost('/api/update/online');
    fwPoll=setInterval(async function(){
      var t;try{t=await (await fetch('/api/update')).json();}catch(e){clearInterval(fwPoll);fwSay('再起動しています…（しばらくすると画面が開き直します）');return;}
      if(t.state==='failed'){clearInterval(fwPoll);fwSay('⚠ '+t.error,'var(--warn)');return;}
      if(t.state==='done'){clearInterval(fwPoll);fwSay('✓ 書き込みました。再起動しています…（しばらくすると画面が開き直します）','var(--ok)');return;}
      fwSay((t.phase===1?'ファーム':'Web 画面')+'を書き込んでいます… '+t.pct+'%');
    },1000);
  }catch(e){fwSay('⚠ '+e,'var(--warn)');}
}
async function fwUpload(){
  var fw=document.getElementById('fwFile').files[0],fs=document.getElementById('fsFile').files[0];
  if(!fw&&!fs){fwSay('ファイルを選んでください','var(--warn)');return;}
  if(!confirm('選んだファイルを書き込みます。終わると再起動します。途中で電源を切らないでください。'))return;
  clearInterval(fwPoll);
  try{
    // The web UI first (the page stays on screen), the firmware last; restart after the last one.
    var list=[];if(fs)list.push(['fs',fs,'Web 画面']);if(fw)list.push(['fw',fw,'ファーム']);
    for(var i=0;i<list.length;i++){
      fwSay(list[i][2]+'を書き込んでいます…（'+Math.round(list[i][1].size/1024)+' KB）');
      var r=await fetch('/api/update/upload?part='+list[i][0]+'&restart='+(i===list.length-1?1:0)+'&token='+encodeURIComponent(authToken||''),{method:'POST',body:list[i][1]});
      var j={};try{j=await r.json();}catch(e){}
      if(!r.ok||!j.ok)throw (FW_REASON[j.reason]||j.reason||('HTTP '+r.status));
    }
    fwSay('✓ 書き込みました。再起動しています…（しばらくすると画面が開き直します）','var(--ok)');
  }catch(e){fwSay('⚠ '+e,'var(--warn)');}
}
