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
  }catch(e){toast('⚠️ ファイル一覧の取得に失敗しました');}
}

function downloadSdFile(path){
  if(raceRunning){toast('レース中はダウンロードできません');return;}
  var a=document.createElement('a');
  a.href='/api/sd/file?path='+encodeURIComponent(path);
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
