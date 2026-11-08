// SPDX-License-Identifier: GPL-2.0-or-later
// The page that dcmod studio serves.
#pragma once

namespace dcmod {

inline const char* const kStudioHtml = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DriveClub mod studio</title>
<style>
:root{--bg:#14161a;--panel:#1c1f25;--line:#2c313a;--text:#dfe3ea;--dim:#8b94a3;--acc:#ff8a2a;--ok:#4cc38a;--bad:#ef6a6a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,Segoe UI,sans-serif;display:flex;height:100vh}
#side{width:360px;border-right:1px solid var(--line);background:var(--panel);display:flex;flex-direction:column;min-width:0}
#main{flex:1;display:flex;flex-direction:column;min-width:0}
header{padding:10px 12px;border-bottom:1px solid var(--line);display:flex;gap:8px;align-items:center}
header b{color:var(--acc)}
.tabs{display:flex;border-bottom:1px solid var(--line)}.tabs button{flex:1;border:0;background:none;color:var(--dim);padding:9px;cursor:pointer;font:inherit}
.tabs button.on{color:var(--text);border-bottom:2px solid var(--acc)}
input,select,textarea,button{font:inherit;color:var(--text);background:#23272f;border:1px solid var(--line);border-radius:4px;padding:6px 8px}
button{cursor:pointer}button.pri{background:var(--acc);color:#111;border-color:var(--acc)}button:hover{filter:brightness(1.15)}
#list{overflow:auto;flex:1;padding:4px 0}
.row{padding:3px 12px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-family:Consolas,monospace;font-size:12.5px}
.row:hover{background:#262b33}.row.sel{background:#33291f}.dim{color:var(--dim)}
#bar{padding:8px 12px;border-bottom:1px solid var(--line);display:flex;gap:8px;align-items:center;flex-wrap:wrap}
#view{flex:1;overflow:auto;padding:12px;min-height:0}
textarea.ed{width:100%;height:100%;resize:none;font-family:Consolas,monospace;font-size:13px;white-space:pre;tab-size:4}
pre{margin:0;font-family:Consolas,monospace;font-size:12.5px;white-space:pre-wrap}
table{border-collapse:collapse;font-size:12.5px}td,th{border:1px solid var(--line);padding:2px 6px;white-space:nowrap;max-width:260px;overflow:hidden;text-overflow:ellipsis}
th{background:#23272f;position:sticky;top:0}
.msg{padding:6px 12px;font-size:13px}.ok{color:var(--ok)}.bad{color:var(--bad)}
.pane{padding:12px;overflow:auto;flex:1}.mod{display:flex;gap:8px;align-items:center;padding:6px 0;border-bottom:1px solid var(--line)}
h3{margin:14px 0 6px}
</style></head><body>
<div id="side">
 <header><b>mod studio</b><span class="dim" id="count"></span></header>
 <div class="tabs"><button id="t1" class="on">Game files</button><button id="t2">Mods</button><button id="t3">Help</button></div>
 <div id="p1" style="display:flex;flex-direction:column;flex:1;min-height:0">
  <div style="padding:8px 12px"><input id="q" placeholder="search paths, e.g. vehicles.csv or *.xml" style="width:100%"></div>
  <div id="list"></div>
 </div>
 <div id="p2" class="pane" style="display:none">
  <div style="display:flex;gap:6px"><input id="newname" placeholder="new mod name" style="flex:1"><button id="newmod">Create</button></div>
  <div id="mods"></div>
  <div style="margin-top:10px;display:flex;gap:6px;flex-wrap:wrap"><button id="check">Check selected</button><button id="build" class="pri">Build selected</button></div>
 </div>
 <div id="p3" class="pane" style="display:none">
  <h3>How it works</h3>
  <p>Open a game file on the left, change its text, pick a mod and press <b>Save to mod</b>. The studio compares your text with the game's original and stores only the difference as patches in that mod.</p>
  <p>On the <b>Mods</b> tab, tick mods and press <b>Build</b> to get the changed files in the <code>_built</code> folder, or import the mod folder on the launcher's Mods page.</p>
  <p>Packs (<code>.rpk</code>) list their resources; XML and text resources can be edited too.</p>
 </div>
</div>
<div id="main">
 <div id="bar"><span id="title" class="dim">Pick a file on the left.</span></div>
 <div id="view"></div>
</div>
<script>
const T=new URLSearchParams(location.search).get('t');
const api=async(p,o={})=>{const r=await fetch(p,{...o,headers:{'X-Token':T,...(o.headers||{})}});const t=await r.text();let j;try{j=JSON.parse(t)}catch{j={}};if(!r.ok)throw new Error(j.error||r.statusText);return j};
const post=(p,b)=>api(p,{method:'POST',body:JSON.stringify(b)});
const $=id=>document.getElementById(id);
let files=[],mods=[],cur=null;
const esc=s=>s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
function tab(n){for(let i=1;i<=3;i++){$('t'+i).classList.toggle('on',i==n);$('p'+i).style.display=i==n?(i==1?'flex':'block'):'none'}}
for(let i=1;i<=3;i++)$('t'+i).onclick=()=>tab(i);
function wild(p,t){if(!/[*?]/.test(p))return t.includes(p);const re=new RegExp('^'+p.replace(/[.+^${}()|[\]\\]/g,'\\$&').replace(/\*/g,'.*').replace(/\?/g,'.')+'$');return re.test(t)}
function showList(){const q=$('q').value.toLowerCase();const out=[];for(const f of files){if(!q||wild(q,f[0])){out.push(f);if(out.length>=500)break}}
 $('list').innerHTML=out.map(f=>`<div class="row${cur&&cur.path==f[0]?' sel':''}" data-p="${f[0]}">${esc(f[0])} <span class="dim">${f[1]}</span></div>`).join('')}
$('list').onclick=e=>{const r=e.target.closest('.row');if(r)openFile(r.dataset.p)};
$('q').oninput=showList;
function modSelect(){return `<select id="modsel">${mods.map(m=>`<option>${esc(m.dir)}</option>`).join('')}</select>`}
function editorBar(save){return save?`<span class="dim">Add to mod:</span> ${modSelect()} <button class="pri" id="save">Save to mod</button><span id="saved" class="msg"></span>`:''}
async function openFile(path,res){
 try{const j=await api('/api/open?path='+encodeURIComponent(path)+(res?'&res='+res:''));
 cur={path:j.path,res:res||''};showList();
 const bar=$('bar'),v=$('view');bar.innerHTML=`<b>${esc(j.path)}</b><span class="dim">${j.size} bytes</span>`;
 if(j.kind=='text'||j.kind=='csv'){
  bar.innerHTML+=' '+editorBar(true);
  v.innerHTML='<textarea class="ed" id="ed" spellcheck="false"></textarea>';$('ed').value=j.text;
  if(j.kind=='csv'){bar.innerHTML+=' <button id="tbl">Table view</button>';$('tbl').onclick=()=>table(j.text)}
  $('save').onclick=async()=>{try{const r=await post('/api/mod/addedit',{mod:$('modsel').value,path:j.path,res:res||'',text:$('ed').value});$('saved').className='msg ok';$('saved').textContent=r.added?`saved: ${r.added} change(s) in the mod`:'no difference from the game file'}catch(e){$('saved').className='msg bad';$('saved').textContent=e.message}};
 }else if(j.kind=='pack'){
  v.innerHTML='<input id="rf" placeholder="filter resources" style="margin-bottom:8px;width:320px"><div id="rl"></div>';
  const draw=()=>{const q=$('rf').value.toLowerCase();$('rl').innerHTML='<table><tr><th>type</th><th>name</th><th>size</th><th>id</th></tr>'+j.resources.filter(r=>!q||(r.name+r.type).toLowerCase().includes(q)).slice(0,1500).map(r=>`<tr ${r.text?`class="row" data-r="${r.id}"`:''}><td>${r.type}</td><td>${esc(r.name)}${r.text?' (text)':''}</td><td>${r.size}</td><td class="dim">${r.id}</td></tr>`).join('')+'</table>'};
  $('rf').oninput=draw;draw();$('rl').onclick=e=>{const tr=e.target.closest('tr[data-r]');if(tr)openFile(j.path,tr.dataset.r)};
 }else if(j.kind=='lua'){v.innerHTML='<pre>'+esc(j.text)+'</pre>'}
 else if(j.kind=='resource'||j.info){v.innerHTML='<p class="dim">'+esc(j.info||'')+'</p>'}
 else{v.innerHTML='<p class="dim">Binary file. <a href="#" id="dl">Download</a></p>';$('dl').onclick=async e=>{e.preventDefault();const r=await fetch('/api/raw?path='+encodeURIComponent(j.path),{headers:{'X-Token':T}});const b=await r.blob();const a=document.createElement('a');a.href=URL.createObjectURL(b);a.download=j.path.split('/').pop();a.click()}}
 }catch(e){$('view').innerHTML='<p class="bad">'+esc(e.message)+'</p>'}}
function parseCsv(t){const rows=[];let r=[],f='',q=false;for(let i=0;i<t.length;i++){const c=t[i];if(q){if(c=='"'){if(t[i+1]=='"'){f+='"';i++}else q=false}else f+=c}else if(c=='"')q=true;else if(c==','){r.push(f);f=''}else if(c=='\n'){r.push(f);rows.push(r);r=[];f=''}else if(c!='\r')f+=c}if(f||r.length){r.push(f);rows.push(r)}return rows}
function table(t){const rows=parseCsv(t).slice(0,400);$('view').innerHTML='<table>'+rows.map((r,i)=>'<tr>'+r.map(c=>i?`<td>${esc(c)}</td>`:`<th>${esc(c)}</th>`).join('')+'</tr>').join('')+'</table><p class="dim">First 400 rows. Close this view by opening the file again.</p>'}
async function loadState(){const s=await api('/api/state');mods=s.mods;$('count').textContent=s.entries+' files';
 $('mods').innerHTML=mods.map(m=>`<div class="mod"><input type="checkbox" data-d="${esc(m.dir)}" checked><span style="flex:1"><b>${esc(m.name||m.dir)}</b> <span class="dim">${m.error?'<span class=bad>'+esc(m.error)+'</span>':(m.patches+' patches')}</span></span><button data-e="${esc(m.dir)}">Edit</button></div>`).join('')||'<p class="dim">No mods yet. Create one above.</p>'}
$('mods').onclick=async e=>{const d=e.target.dataset.e;if(!d)return;const j=await api('/api/mod?name='+encodeURIComponent(d));
 $('bar').innerHTML=`<b>${esc(d)}/mod.json</b> <button class="pri" id="msave">Save</button><span id="saved" class="msg"></span>`;
 $('view').innerHTML='<textarea class="ed" id="ed" spellcheck="false"></textarea>';$('ed').value=j.text;
 $('msave').onclick=async()=>{try{await post('/api/mod/save',{name:d,text:$('ed').value});$('saved').className='msg ok';$('saved').textContent='saved';loadState()}catch(x){$('saved').className='msg bad';$('saved').textContent=x.message}}};
$('newmod').onclick=async()=>{const n=$('newname').value.trim();if(!n)return;try{await post('/api/mod/new',{name:n});$('newname').value='';loadState()}catch(e){alert(e.message)}};
async function run(build){const sel=[...document.querySelectorAll('#mods input:checked')].map(c=>c.dataset.d);
 $('bar').innerHTML='<b>'+(build?'Build':'Check')+'</b>';$('view').innerHTML='<pre>working...</pre>';
 try{const r=await post('/api/mod/run',{mods:sel,build});$('view').innerHTML='<pre>'+esc(r.log)+'</pre>'}catch(e){$('view').innerHTML='<pre class="bad">'+esc(e.message)+'</pre>'}}
$('check').onclick=()=>run(false);$('build').onclick=()=>run(true);
(async()=>{files=await api('/api/files');await loadState();showList()})().catch(e=>{$('view').innerHTML='<p class="bad">'+esc(e.message)+'</p>'});
</script></body></html>)HTML";

}  // namespace dcmod
