'use strict';

// ── Race results on the SD card ─────────────────────────────────────────
// race_NNN.csv as sdSaveRace() writes it: an optional "#FPVC5,key=value,..."
// line (start time and rules; older files have none), the header, then one
// row per pass. The list reads the newest few files; one race opens with
// its results and a lap-time graph; pilot stats read every file, only when
// asked (with an estimate of how long it takes).

var RES_PAGE = 10;           // summaries read automatically (newest first)
var resShown = RES_PAGE;
var resSel = null;           // file name of the race shown
var resView = 'table';       // 'table' | 'graph'
var resHidden = {};          // graph: slots switched off
var resFull = {};            // name -> parsed race (this page only)
var resMsPerFile = 0, resSamples = 0;   // measured time to read one file
var resBusy = false, resScanStop = false;

// race_012.csv, or race_012_20261108_1432.csv (the race's date and time).
var RES_NAME=/^race_(\d{1,4})(_\d{8}_\d{4})?\.csv$/;
function resNum(name){return parseInt(RES_NAME.exec(name)[1],10);}
function resFiles(){
  return sdFileList.filter(f=>RES_NAME.test(f.name)).sort((a,b)=>
    ((b.t||0)-(a.t||0))||(resNum(b.name)-resNum(a.name)));
}
function resNo(name){return '#'+RES_NAME.exec(name)[1];}
function resKey(f){return 'res1:'+f.name+'|'+f.size+'|'+(f.t||0);}
function resGet(f){try{var v=localStorage.getItem(resKey(f));return v?JSON.parse(v):null;}catch(e){return null;}}
function resPut(f,s){try{localStorage.setItem(resKey(f),JSON.stringify(s));}catch(e){}}
// Drop summaries of files that are gone (deleted, rotated, card formatted).
function resPrune(){
  try{
    var keep={};sdFileList.forEach(f=>keep[resKey(f)]=1);
    for(var i=localStorage.length-1;i>=0;i--){var k=localStorage.key(i);if(k&&k.indexOf('res1:')===0&&!keep[k])localStorage.removeItem(k);}
  }catch(e){}
}

// ── Parse one file ──────────────────────────────────────────────────────
function parseRace(text,f){
  var meta={},rows=[];
  text.replace(/^\uFEFF/,'').split(/\r?\n/).forEach(line=>{
    if(!line.trim())return;
    if(line[0]==='#'){
      if(line.indexOf('#FPVC5')===0)line.split(',').slice(1).forEach(kv=>{var i=kv.indexOf('=');if(i>0)meta[kv.slice(0,i)]=kv.slice(i+1);});
      return;
    }
    var c=line.split(',');
    if(c[0]==='Slot'||c.length<5)return;
    rows.push({slot:+c[0],name:c[1],lap:+c[3],ms:+c[4]});
  });
  var holeshot=meta.mode?meta.mode==='holeshot':rows.some(r=>r.lap===0);
  var by={};
  rows.forEach(r=>{
    var p=by[r.slot]||(by[r.slot]={slot:r.slot,name:r.name,hs:0,laps:[]});
    if(holeshot&&r.lap===0)p.hs=r.ms;
    else p.laps.push(r.ms);
  });
  var rank=meta.rank!==undefined&&meta.rank!==''?+meta.rank:rankMode;
  var totalLaps=+(meta.laps||0);
  var pilots=Object.keys(by).map(k=>{
    var p=by[k],n=p.laps.length;
    p.count=n;
    p.best=n?Math.min.apply(null,p.laps):0;
    p.total=p.laps.reduce((a,b)=>a+b,0);
    p.done=totalLaps>0&&n>=totalLaps;
    return p;
  });
  var byLaps=(a,b)=>(b.count-a.count)||((a.count?a.total:0)-(b.count?b.total:0))||(a.slot-b.slot);
  if(rank===1)pilots.sort((a,b)=>((a.best?0:1)-(b.best?0:1))||(a.best-b.best)||byLaps(a,b));
  else pilots.sort(byLaps);
  var when=resWhen(meta.start,f&&f.t);
  return {name:f?f.name:'',start:meta.start||'',when:when.text,sortKey:when.key||(f&&f.t?f.t*1000:0),
    event:(meta.event||'').trim(),holeshot:holeshot,totalLaps:totalLaps,rank:rank,
    limit:+(meta.limit_s||0),lastLap:meta.lastlap!=='0',hasMeta:!!meta.v,pilots:pilots};
}
// "2026-11-08T14:32:05+09:00" -> "11/08 14:32" (as the timer's clock said).
// Older files: the file's time, if the card had a clock then.
function resWhen(start,t){
  var m=/^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d)/.exec(start||'');
  if(m)return {text:m[2]+'/'+m[3]+' '+m[4]+':'+m[5],full:m[1]+'/'+m[2]+'/'+m[3]+' '+m[4]+':'+m[5],key:Date.parse(start)||0};
  if(t){var d=new Date(t*1000);var s=pad(d.getMonth()+1,2)+'/'+pad(d.getDate(),2)+' '+pad(d.getHours(),2)+':'+pad(d.getMinutes(),2);
    return {text:s+' 保存',full:d.getFullYear()+'/'+s+' 保存',key:t*1000};}
  return {text:'日時不明',full:'日時不明',key:0};
}
function resSummary(r){
  return {when:r.when,sortKey:r.sortKey,event:r.event,totalLaps:r.totalLaps,rank:r.rank,limit:r.limit,holeshot:r.holeshot,
    pilots:r.pilots.map(p=>({slot:p.slot,name:p.name,count:p.count,best:p.best,total:p.total}))};
}

async function resFetch(f){
  var t0=performance.now();
  var r=await fetch('/api/sd/file?path='+encodeURIComponent('/'+f.name));
  if(!r.ok)throw new Error('read');
  var text=await r.text();
  var dt=performance.now()-t0;
  resMsPerFile=(resMsPerFile*resSamples+dt)/(resSamples+1);
  resSamples=Math.min(resSamples+1,20);
  var race=parseRace(text,f);
  resFull[f.name]=race;
  resPut(f,resSummary(race));
  return race;
}

// ── List ────────────────────────────────────────────────────────────────
function renderResList(){
  var el=document.getElementById('resList');if(!el)return;
  var files=resFiles();
  var cnt=document.getElementById('resCount');if(cnt)cnt.textContent=files.length?files.length+' 件':'';
  if(!files.length){el.innerHTML='<p class="res-muted">保存されたレースはありません</p>';return;}
  var h='',missing=[];
  files.slice(0,resShown).forEach(f=>{
    var s=resGet(f);
    if(!s)missing.push(f);
    h+='<div class="res-item'+(f.name===resSel?' on':'')+'" onclick="openRace(\''+f.name+'\')">'
      +'<div class="no">'+resNo(f.name)+'</div><div class="mid">';
    if(s){
      var w=s.pilots[0]&&(s.pilots[0].count||s.pilots[0].best)?s.pilots[0]:null;
      var fast=s.pilots.reduce((m,p)=>p.best&&(!m||p.best<m)?p.best:m,0);
      h+='<div class="t">'+esc(s.when)+(w?'　🥇 '+esc(w.name):'')+'</div><div>'
        +(s.event?'<span class="res-chip">'+esc(s.event)+'</span>':'')
        +'<span class="res-chip">'+s.pilots.length+'人</span>'
        +'<span class="res-chip">'+(s.rank===2?'時間内 '+fmtSec(s.limit):s.totalLaps?s.totalLaps+'周':'周回無制限')+'</span>'
        +(fast?'<span class="res-chip">最速 '+(fast/1000).toFixed(2)+'</span>':'')+'</div>';
    }else h+='<div class="t res-muted">'+(raceRunning?'レース中は読み込みません':'読み込み中…')+'</div>';
    h+='</div><div class="res-muted">›</div></div>';
  });
  if(files.length>resShown)h+='<button class="btn-secondary" style="width:100%" onclick="resShown+='+RES_PAGE+';renderResList()">さらに '+Math.min(RES_PAGE,files.length-resShown)+' 件</button>';
  el.innerHTML=h;
  if(missing.length&&!raceRunning)resLoadList(missing);
}
async function resLoadList(files){
  if(resBusy)return;
  resBusy=true;
  try{for(var i=0;i<files.length;i++){if(raceRunning)break;try{await resFetch(files[i]);}catch(e){}}}
  finally{resBusy=false;}
  renderResList();
}

// ── One race ────────────────────────────────────────────────────────────
async function openRace(name){
  var f=sdFileList.find(x=>x.name===name);if(!f)return;
  if(raceRunning&&!resFull[name]){toast('レース中は読み込めません');return;}
  resSel=name;resHidden={};
  renderResList();
  var box=document.getElementById('resDetail');
  box.style.display='';
  if(!resFull[name]){
    box.innerHTML='<p class="res-muted">読み込み中…</p>';
    try{await resFetch(f);}catch(e){box.innerHTML='<p class="res-muted">⚠ 読み込めませんでした</p>';return;}
  }
  renderRace();
  box.scrollIntoView({behavior:'smooth',block:'start'});
}
function closeRace(){resSel=null;document.getElementById('resDetail').style.display='none';renderResList();}
function resSetView(v){resView=v;renderRace();}

function resName(p){return p.name==='---'?'Ch'+(p.slot+1):p.name;}
function resColor(p){return PCOLORS[p.slot%PCOLORS.length];}

function renderRace(){
  var r=resFull[resSel];var box=document.getElementById('resDetail');if(!r||!box)return;
  var rule=(r.rank===2?'制限時間 '+fmtSec(r.limit)+(r.lastLap?'・最終周あり':'・最終周なし'):r.totalLaps?'総周回数 '+r.totalLaps:'周回数 無制限')
    +' ・ '+RANK_RULES[r.rank]+(r.holeshot?' ・ ホールショットあり':'');
  var h='<div class="res-head"><b>'+resNo(r.name)+'　'+esc(resWhen(r.start,(sdFileList.find(x=>x.name===r.name)||{}).t).full)+'</b><span style="flex:1"></span>'
    +'<button class="btn-secondary res-btn" onclick="downloadSdFile(\'/'+r.name+'\')">⬇ CSV</button>'
    +'<button class="btn-secondary res-btn" onclick="closeRace()">✕</button></div>'
    +(r.event?'<div style="font-size:13px;margin-bottom:2px">'+esc(r.event)+'</div>':'')
    +'<div class="res-muted">'+esc(rule)+(r.hasMeta?'':' ・ 古い形式のファイル（日時・設定の記録なし）')+'</div>'
    +'<div class="res-seg"><button class="'+(resView==='table'?'on':'')+'" onclick="resSetView(\'table\')">結果</button>'
    +'<button class="'+(resView==='graph'?'on':'')+'" onclick="resSetView(\'graph\')">グラフ</button></div>';
  h+=resView==='graph'?resGraphHtml(r):resTableHtml(r);
  box.innerHTML=h;
}

function resTableHtml(r){
  var ps=r.pilots;
  if(!ps.length)return '<p class="res-muted">ラップがありません</p>';
  var lead=ps[0],keyBest=r.rank===1;
  var h='<div class="res-wrap"><table class="res-table"><tr><th>順位</th><th class="l">パイロット</th><th>周回</th><th>最速</th><th>トータル</th><th>差</th></tr>';
  ps.forEach((p,i)=>{
    var has=keyBest?p.best>0:p.count>0,gap='';
    if(i>0&&has){
      if(keyBest)gap='+'+((p.best-lead.best)/1000).toFixed(3);
      else if(p.count<lead.count)gap='-'+(lead.count-p.count)+'周';
      else gap='+'+((p.total-lead.total)/1000).toFixed(3);
    }
    h+='<tr class="'+(has&&i<3?'r'+(i+1):'')+'"><td class="pos">'+(has?i+1:'–')+'</td>'
      +'<td class="l"><span class="res-dot" style="background:'+resColor(p)+'"></span>'+esc(resName(p))+(p.done?' 🏁':'')+'</td>'
      +'<td>'+p.count+'</td><td class="'+(keyBest&&i===0&&has?'best':'')+'">'+(p.best?fmt(p.best):'—')+'</td>'
      +'<td>'+(p.count?fmt(p.total):'—')+'</td><td class="res-muted">'+gap+'</td></tr>';
  });
  h+='</table></div><div class="res-head" style="margin:14px 0 6px"><b>ラップ</b><span class="res-muted">緑は各パイロットの最速</span></div>';
  // Laps: one column per pilot, in slot order.
  var cols=ps.slice().sort((a,b)=>a.slot-b.slot);
  var n=Math.max.apply(null,cols.map(p=>p.count));
  h+='<div class="res-wrap"><table class="res-table"><tr><th>周</th>'+cols.map(p=>'<th><span class="res-dot" style="background:'+resColor(p)+'"></span>'+esc(resName(p))+'</th>').join('')+'</tr>';
  if(r.holeshot)h+='<tr><td class="res-muted">HS</td>'+cols.map(p=>'<td class="res-muted">'+(p.hs?(p.hs/1000).toFixed(2):'—')+'</td>').join('')+'</tr>';
  for(var i=0;i<n;i++){
    h+='<tr><td>'+(i+1)+'</td>'+cols.map(p=>{var v=p.laps[i];return '<td class="'+(v&&v===p.best?'best':'')+'">'+(v?(v/1000).toFixed(2):'—')+'</td>';}).join('')+'</tr>';
  }
  return h+'</table></div>';
}

// ── Graph ───────────────────────────────────────────────────────────────
function resToggle(slot){resHidden[slot]=!resHidden[slot];renderRace();}
function resGraphHtml(r){
  var ps=r.pilots.filter(p=>p.count).sort((a,b)=>a.slot-b.slot);
  if(!ps.length)return '<p class="res-muted">ラップがありません</p>';
  var shown=ps.filter(p=>!resHidden[p.slot]);
  var series=shown.map(p=>({color:resColor(p),pts:p.laps.map((v,i)=>[i+1,v])}));
  var best=0,bestAt=null;
  shown.forEach(p=>p.laps.forEach((v,i)=>{if(v&&(!best||v<best)){best=v;bestAt=[i+1,v,resColor(p)];}}));
  var h=resChartSvg(series,{xLabel:x=>x+'周',mark:bestAt,markText:best?'最速 '+(best/1000).toFixed(2):''});
  h+='<div class="res-legend">'+ps.map(p=>'<button class="'+(resHidden[p.slot]?'off':'')+'" onclick="resToggle('+p.slot+')"><span class="res-dot" style="background:'+resColor(p)+'"></span>'+esc(resName(p))+'</button>').join('')+'</div>';
  // Consistency: mean lap and standard deviation (laps from 1).
  var stats=ps.map(p=>{
    var m=p.total/p.count,sd=Math.sqrt(p.laps.reduce((a,v)=>a+(v-m)*(v-m),0)/p.count);
    return {p:p,mean:m,sd:sd};
  });
  var maxSd=Math.max.apply(null,stats.map(s=>s.sd))||1,minSd=Math.min.apply(null,stats.map(s=>s.sd));
  h+='<div class="res-head" style="margin:6px 0"><b>安定度</b><span class="res-muted">ラップのばらつき（小さいほど安定）</span></div>'
    +'<table class="res-table"><tr><th class="l">パイロット</th><th>平均</th><th>ばらつき</th><th style="width:30%"></th></tr>';
  stats.forEach(s=>{
    var w=Math.max(4,Math.round(s.sd/maxSd*100));
    h+='<tr><td class="l"><span class="res-dot" style="background:'+resColor(s.p)+'"></span>'+esc(resName(s.p))+'</td>'
      +'<td>'+(s.mean/1000).toFixed(2)+'</td><td>±'+(s.sd/1000).toFixed(2)+'</td>'
      +'<td><div class="res-bar" style="width:'+w+'%;'+(s.p.count>1&&s.sd===minSd?'background:var(--ok)':'')+'"></div></td></tr>';
  });
  return h+'</table>';
}

// Line chart: series [{color, pts:[[x, ms], ...]}]. A lap far slower than
// the rest (a crash) would squash every other line, so the top of the
// scale stops at 1.6x the median and slower points sit on the top edge.
function resChartSvg(series,o){
  var W=340,H=170,L=38,R=10,T=12,B=26;
  var all=[];series.forEach(s=>s.pts.forEach(p=>all.push(p[1])));
  if(!all.length)return '<p class="res-muted">表示するパイロットを選んでください</p>';
  var sorted=all.slice().sort((a,b)=>a-b),med=sorted[Math.floor(sorted.length/2)];
  var lo=sorted[0],hi=Math.min(sorted[sorted.length-1],med*1.6);
  if(hi-lo<200){hi+=100;lo-=100;}
  var pad_=(hi-lo)*0.08;lo-=pad_;hi+=pad_;
  var xs=[];series.forEach(s=>s.pts.forEach(p=>xs.push(p[0])));
  var x0=Math.min.apply(null,xs),x1=Math.max.apply(null,xs);if(x1===x0)x1=x0+1;
  var X=x=>L+(x-x0)/(x1-x0)*(W-L-R),Y=v=>T+(1-(Math.min(v,hi)-lo)/(hi-lo))*(H-T-B);
  var g='<svg viewBox="0 0 '+W+' '+H+'" width="100%" style="display:block">';
  for(var k=0;k<=2;k++){var v=lo+(hi-lo)*k/2,y=Y(v);
    g+='<line x1="'+L+'" x2="'+(W-R)+'" y1="'+y+'" y2="'+y+'" stroke="var(--bd)" stroke-dasharray="3 3"/>'
      +'<text x="2" y="'+(y+4)+'" fill="var(--muted)" font-size="10">'+(v/1000).toFixed(1)+'</text>';}
  var step=Math.max(1,Math.ceil((x1-x0+1)/8));
  for(var x=x0;x<=x1;x+=step)g+='<text x="'+X(x)+'" y="'+(H-8)+'" fill="var(--muted)" font-size="10" text-anchor="middle">'+o.xLabel(x)+'</text>';
  series.forEach(s=>{
    var pts=s.pts.map(p=>X(p[0]).toFixed(1)+','+Y(p[1]).toFixed(1)).join(' ');
    if(s.pts.length>1)g+='<polyline fill="none" stroke="'+s.color+'" stroke-width="2.5" stroke-linejoin="round" points="'+pts+'"/>';
    s.pts.forEach(p=>{g+='<circle cx="'+X(p[0]).toFixed(1)+'" cy="'+Y(p[1]).toFixed(1)+'" r="'+(p[1]>hi?4:2.5)+'" fill="'+s.color+'"/>';});
  });
  if(o.mark){var mx=X(o.mark[0]),my=Y(o.mark[1]);
    g+='<circle cx="'+mx+'" cy="'+my+'" r="5" fill="'+o.mark[2]+'" stroke="#fff" stroke-width="1.5"/>'
      +'<text x="'+Math.min(mx+7,W-70)+'" y="'+Math.min(my+14,H-B)+'" fill="var(--tx)" font-size="10">'+o.markText+'</text>';}
  return g+'</svg>';
}

// ── Pilot stats (every file) ────────────────────────────────────────────
function resEstimate(n){
  var per=resSamples?resMsPerFile:300;   // before any read: a guess
  var s=Math.max(1,Math.round(n*per/1000));
  return (s<60?'約 '+s+' 秒':'約 '+Math.floor(s/60)+' 分 '+(s%60)+' 秒')+(resSamples?'':'（目安）');
}
function openPilotScan(){
  if(raceRunning){toast('レース中は使えません');return;}
  var files=resFiles();
  if(!files.length){toast('保存されたレースがありません');return;}
  var todo=files.filter(f=>!resGet(f)).length;
  var t='レース結果 <b>'+files.length+' 件</b>を読み込みます。';
  if(todo)t+='<br>新しく読むファイル：'+todo+' 件（読み込み済み '+(files.length-todo)+' 件）<br>かかる時間：<b>'+resEstimate(todo)+'</b>';
  else t+='<br>すべて読み込み済みなので、すぐに表示できます。';
  document.getElementById('pilotScanWhat').innerHTML=t;
  document.getElementById('pilotScanModal').style.display='flex';
}
function closePilotScan(){document.getElementById('pilotScanModal').style.display='none';}
async function startPilotScan(){
  closePilotScan();
  var box=document.getElementById('resPilots');box.style.display='';
  var files=resFiles(),todo=files.filter(f=>!resGet(f));
  if(todo.length){
    if(resBusy){toast('読み込み中です。少し待ってからもう一度押してください');return;}
    resBusy=true;resScanStop=false;
    try{
      for(var i=0;i<todo.length;i++){
        if(resScanStop||raceRunning)break;
        box.innerHTML='<div class="res-head"><b>👤 パイロット別</b><span class="res-muted">読み込み中 '+i+' / '+todo.length
          +'（残り '+resEstimate(todo.length-i)+'）</span><span style="flex:1"></span>'
          +'<button class="btn-secondary res-btn" onclick="resScanStop=true">中止</button></div>';
        try{await resFetch(todo[i]);}catch(e){}
      }
    }finally{resBusy=false;}
    renderResList();
  }
  renderPilots(null);
  box.scrollIntoView({behavior:'smooth',block:'start'});
}
function resPilotStats(){
  var map={};
  resFiles().forEach(f=>{
    var s=resGet(f);if(!s)return;
    var lead=s.pilots[0];
    s.pilots.forEach((p,i)=>{
      if(p.name==='---')return;
      var m=map[p.name]||(map[p.name]={name:p.name,slot:p.slot,races:0,wins:0,best:0,bestRace:'',hist:[]});
      m.races++;
      if(i===0&&(lead.count||lead.best))m.wins++;
      if(p.best&&(!m.best||p.best<m.best)){m.best=p.best;m.bestRace=resNo(f.name);}
      if(p.best)m.hist.push({key:s.sortKey||resNum(f.name),no:resNo(f.name),best:p.best});
    });
  });
  return Object.keys(map).map(k=>map[k]).sort((a,b)=>((a.best?0:1)-(b.best?0:1))||(a.best-b.best));
}
function renderPilots(sel){   // sel: row index of the pilot whose trend is shown
  var box=document.getElementById('resPilots');
  var all=resFiles(),read=all.filter(f=>resGet(f)).length;
  var st=resPilotStats();
  var h='<div class="res-head"><b>👤 パイロット別</b><span class="res-muted">'+read+' / '+all.length+' 件から集計</span><span style="flex:1"></span>'
    +'<button class="btn-secondary res-btn" onclick="document.getElementById(\'resPilots\').style.display=\'none\'">✕</button></div>';
  if(!st.length){box.innerHTML=h+'<p class="res-muted">記録がありません</p>';return;}
  h+='<div class="res-wrap"><table class="res-table"><tr><th class="l">パイロット</th><th>出走</th><th>1位</th><th>自己ベスト</th><th>レース</th></tr>';
  st.forEach((m,i)=>{
    h+='<tr style="cursor:pointer'+(sel===i?';background:var(--sf2)':'')+'" onclick="renderPilots('+i+')">'
      +'<td class="l"><span class="res-dot" style="background:'+PCOLORS[m.slot%PCOLORS.length]+'"></span>'+esc(m.name)+'</td>'
      +'<td>'+m.races+'</td><td>'+m.wins+'</td><td class="'+(i===0&&m.best?'best':'')+'">'+(m.best?fmt(m.best):'—')+'</td><td class="res-muted">'+m.bestRace+'</td></tr>';
  });
  h+='</table></div><p class="res-muted" style="margin-top:6px">パイロットをタップすると、最速ラップの推移を表示します</p>';
  var m=sel!==null?st[sel]:null;
  if(m&&m.hist.length){
    var hist=m.hist.sort((a,b)=>a.key-b.key);
    h+='<div class="res-head" style="margin:12px 0 4px"><b>'+esc(m.name)+'：最速ラップの推移</b></div>'
      +resChartSvg([{color:PCOLORS[m.slot%PCOLORS.length],pts:hist.map((x,i)=>[i+1,x.best])}],
        {xLabel:x=>hist[x-1]?hist[x-1].no:'',mark:null});
    if(hist.length>1){
      var d=(hist[0].best-hist[hist.length-1].best)/1000;
      h+='<p class="res-muted">'+hist.length+' レースで '+(d>=0?Math.abs(d).toFixed(2)+' 秒速くなりました':Math.abs(d).toFixed(2)+' 秒遅くなりました')+'（最初と最新の比較）</p>';
    }
  }
  box.innerHTML=h;
}

// ── Format (delete every file) ──────────────────────────────────────────
function openSdErase(){
  if(!sdPresent){toast('SDカードがありません');return;}
  if(raceRunning){toast('レース中はフォーマットできません');return;}
  var pw=document.getElementById('sdErasePw'),err=document.getElementById('sdEraseErr');
  pw.value='';err.style.display='none';
  document.getElementById('sdEraseModal').style.display='flex';
  setTimeout(()=>pw.focus(),50);
}
function closeSdErase(){
  var pw=document.getElementById('sdErasePw');if(pw){pw.blur();pw.value='';}
  document.getElementById('sdEraseModal').style.display='none';
}
async function confirmSdErase(){
  var pw=document.getElementById('sdErasePw'),err=document.getElementById('sdEraseErr');
  var say=t=>{err.textContent=t;err.style.display='block';};
  try{
    var r=await fetch('/api/sd/erase',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:pw.value})});
    var j={};try{j=await r.json();}catch(e){}
    if(r.status===401){say('パスワードが違います');return;}
    if(r.status===409){say('レース中、またはクリアしていないラップがあります。クリアしてからもう一度行ってください');return;}
    if(!j.ok){say('フォーマットできませんでした（SDカードを確認してください）');return;}
    closeSdErase();
    toast('🧹 SDカードをフォーマットしました（'+j.removed+' 件削除）');
    resFull={};resSel=null;
    document.getElementById('resDetail').style.display='none';
    document.getElementById('resPilots').style.display='none';
    refreshSdFiles();
  }catch(e){say('接続エラー');}
}
