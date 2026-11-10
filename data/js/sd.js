'use strict';

// The SD card is on the S3 itself now, so files come straight over HTTP
// (no WebSocket line streaming through a gate node).

async function refreshSdFiles(){
  if(!sdPresent){toast('SDカードがありません');return;}
  try{
    var r=await fetch('/api/sd/files');
    if(!r.ok)throw 0;
    sdFileList=await r.json();
    renderSdFileList();
    resPrune();
    renderResList();
  }catch(e){toast('⚠️ ファイル一覧の取得に失敗しました');}
}

function downloadSdFile(path){
  if(raceRunning){toast('レース中はダウンロードできません');return;}
  var a=document.createElement('a');
  // The settings backup and the QR images hold passwords: sent with the login.
  a.href='/api/sd/file?path='+encodeURIComponent(path)+(authToken?'&token='+encodeURIComponent(authToken):'');
  a.download=path.replace(/^\//,'');
  document.body.appendChild(a);a.click();document.body.removeChild(a);
}

async function deleteSdFile(path){
  if(!confirm(path.replace(/^\//,'')+' を削除しますか？'))return;
  try{
    var r=await fetch('/api/sd/files/delete',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({path:path})});
    var j=await r.json();
    if(j.ok){
      toast('🗑 削除完了: '+path);
      var name=path.replace(/^\//,'');
      sdFileList=sdFileList.filter(function(f){return f.name!==name;});
      renderSdFileList();
    }else toast('削除失敗: '+path);
  }catch(e){toast('⚠️ 接続エラー');}
}

function renderSdFileList(){
  showSettingsBackupInfo();
  var wrap=document.getElementById('sdFileListWrap');
  if(!wrap)return;
  if(!sdFileList.length){
    wrap.innerHTML='<p style="color:var(--muted);font-size:12px">ファイルがありません</p>';
    return;
  }
  var html='<div class="lap-table-wrap"><table class="lapTable"><thead><tr>'
    +'<th style="text-align:left">ファイル名</th><th>サイズ</th><th>操作</th>'
    +'</tr></thead><tbody>';
  sdFileList.forEach(function(f){
    var kb=(f.size/1024).toFixed(1);
    var path='/'+f.name;
    var escaped=esc(path).replace(/'/g,"\\'");
    html+='<tr>'
      +'<td style="text-align:left">'+esc(f.name)+'</td>'
      +'<td>'+kb+' KB</td>'
      +'<td style="display:flex;gap:4px;justify-content:center">'
      +'<button onclick="downloadSdFile(\''+escaped+'\')" class="btn-secondary" style="padding:2px 8px;font-size:11px">⬇ DL</button>'
      +'<button onclick="deleteSdFile(\''+escaped+'\')" class="btn-danger" style="padding:2px 8px;font-size:11px">🗑</button>'
      +'</td></tr>';
  });
  html+='</tbody></table></div>';
  wrap.innerHTML=html;
}

// ── Settings backup (everything but the pilots) ──────────────────────────
function showSettingsBackupInfo(){
  var el=document.getElementById('setBkInfo');if(!el)return;
  var f=(sdFileList||[]).find(function(x){return x.name.toLowerCase()==='settings.json';});
  el.textContent=!f?'':f.t?'保存済み '+fmtSdTime(f.t):'保存済み';
}
function fmtSdTime(t){
  var d=new Date(t*1000),p=function(n){return String(n).padStart(2,'0');};
  return d.getFullYear()+'/'+p(d.getMonth()+1)+'/'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes());
}
async function sdSettingsBackup(){
  if(!sdPresent){toast('SDカードがありません');return;}
  try{
    var r=await fetch('/api/sd/settings/backup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token:authToken})});
    if(r.status===403){toast('⚠️ ログインし直してください');return;}
    toast(r.ok?'✅ 設定を SD カードの settings.json に保存しました':'⚠️ 保存できませんでした',3000);
    if(r.ok)refreshSdFiles();
  }catch(e){toast('⚠️ 接続エラー');}
}
var SETTINGS_RESTORE_ERR={nofile:'SD カードに settings.json がありません',badfile:'settings.json が読めません（このタイマーの設定ファイルではありません）',
  race:'レース中は復元できません',login:'ログインし直してください'};
async function sdSettingsRestore(){
  if(!sdPresent){toast('SDカードがありません');return;}
  if(!confirm('SD カードの settings.json から設定を復元します。今の設定（パイロット以外）は上書きされます。よろしいですか？'))return;
  try{
    var r=await fetch('/api/sd/settings/restore',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token:authToken})});
    var j={};try{j=await r.json();}catch(e){}
    if(!r.ok||!j.ok){toast('⚠️ '+(SETTINGS_RESTORE_ERR[j.reason]||'復元できませんでした'),4000);return;}
    // Race settings come back over the WebSocket; reload the other forms.
    if(typeof loadElrsSettings==='function')loadElrsSettings();
    if(typeof loadNetSettings==='function')loadNetSettings();
    if(typeof loadConnLimits==='function')loadConnLimits();
    var when=j.saved?'（'+fmtSdTime(j.saved)+' の保存）':'';
    if(j.net){
      if(confirm('設定を復元しました'+when+'。\nWiFi の設定は、タイマーを再起動すると切り替わります。今すぐ再起動しますか？')){fetch('/api/restart',{method:'POST'}).catch(()=>{});toast('🔄 再起動しています…（20 秒ほどで戻ります）',8000);}
      else toast('✅ 設定を復元しました（WiFi は再起動後に切り替わります）',4000);
    }else toast('✅ 設定を復元しました'+when+'（WiFi の設定は復元できませんでした）',4000);
  }catch(e){toast('⚠️ 接続エラー');}
}

// ── Connection QR codes as one PNG on the card ──────────────────────────
// Drawn here (the QR library is in the browser), then sent to the timer.
async function sdSaveQrImage(){
  if(!sdPresent){toast('SDカードがありません');return;}
  var d,items;
  try{
    var r=await fetch('/api/net');if(!r.ok)throw 0;
    d=await r.json();
    items=await buildQrItems(d);
    await loadQrLib();
  }catch(e){toast('⚠️ QR コードを作れませんでした');return;}
  var name=(d.status&&d.status.ap)?'qr_host.png':'qr_client.png';
  var cv=drawQrCard(items);
  var url=cv.toDataURL('image/png');
  var pv=document.getElementById('qrSavePreview');
  if(pv){pv.innerHTML='<img alt="" src="'+url+'">';pv.style.display='';}
  var blob=await new Promise(function(ok){cv.toBlob(ok,'image/png');});
  try{
    var u='/api/sd/upload?path='+encodeURIComponent('/'+name)+'&token='+encodeURIComponent(authToken||'');
    var res=await fetch(u,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:blob});
    if(res.status===403){toast('⚠️ ログインし直してください');return;}
    toast(res.ok?'✅ '+name+' を SD カードに保存しました':'⚠️ 保存できませんでした',3000);
    if(res.ok)refreshSdFiles();
  }catch(e){toast('⚠️ 接続エラー');}
}
// White card, black text: each QR with its caption, the SSID / URL and the
// WiFi password under it.
function drawQrCard(items){
  var W=900,QR=520,PAD=40,font='"Hiragino Sans","Noto Sans JP","Yu Gothic",Meiryo,sans-serif';
  var blockH=function(it){return 56+QR+20+44+(it.pass?40:0)+36;};
  var H=PAD+70+items.reduce(function(a,it){return a+blockH(it);},0)+PAD;
  var cv=document.createElement('canvas');cv.width=W;cv.height=H;
  var g=cv.getContext('2d');
  g.fillStyle='#fff';g.fillRect(0,0,W,H);
  g.fillStyle='#000';g.textAlign='center';g.textBaseline='alphabetic';
  g.font='bold 40px '+font;g.fillText('FPV C5 Lap Timer への接続',W/2,PAD+42);
  var y=PAD+70;
  items.forEach(function(it){
    g.fillStyle='#000';g.font='bold 34px '+font;g.fillText(it.cap,W/2,y+40);
    var q=qrcode(0,'M');q.addData(it.text);q.make();
    var n=q.getModuleCount(),cell=Math.floor(QR/(n+8)),size=cell*n,x0=Math.round((W-size)/2),y0=y+56+Math.round((QR-size)/2);
    for(var r=0;r<n;r++)for(var c=0;c<n;c++)if(q.isDark(r,c))g.fillRect(x0+c*cell,y0+r*cell,cell,cell);
    var ty=y+56+QR+20+30;
    g.font='28px '+font;g.fillText(it.pass?'WiFi：'+it.sub:it.sub,W/2,ty);
    if(it.pass){ty+=40;g.fillText('パスワード：'+it.pass,W/2,ty);}
    y+=blockH(it);
  });
  return cv;
}
