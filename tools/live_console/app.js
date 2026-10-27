'use strict';
const $ = id => document.getElementById(id);
const canvas = $('canvas'), ctx = canvas.getContext('2d');
let config, revision, token, streamIndex = 0, regionIndex = 0;
let dirty = false, drawing = false, frozen = false, busy = false, dragging = -1;
let picture = null, packet = null, run = null, state = null, lastFrameAt = 0, receivedAge = 0;
let operation = 0, selectedVertex = -1, lastSequence = null;
const colors = ['#45c9ba', '#efb549', '#95c6ef', '#d7a8ce'];
const regions = () => config.streams[streamIndex].regions;
const current = () => regions()[regionIndex];
function message(text, error = false) { $('message').textContent = text; $('message').classList.toggle('error', error); }
function markDirty() { dirty = true; $('dirty').textContent = '有未应用修改 · 检测仍使用已保存规则'; }
async function request(path, body) {
 const response = await fetch(path, body === undefined ? {cache:'no-store'} : {method:'POST', headers:{'Content-Type':'application/json','X-Editor-Token':token}, body:JSON.stringify(body)});
 const data = await response.json();
 if (!response.ok) throw Error(data.error || '请求失败');
 return data;
}
function invalidate() { operation++; picture = packet = null; lastSequence = null; lastFrameAt = 0; draw(); }
function updateFields() {
 const r = current();
 $('classes').value = r ? r.classes.join(',') : '';
 $('cooldown').value = r ? r.cooldown_ms : 5000;
 $('preset').value = ['0','2','1,2,3',''].includes($('classes').value) ? $('classes').value : 'custom';
 for (const id of ['classes','cooldown','preset','delete','finish','undo']) $(id).disabled = !r;
 vertices();
}
function regionList() {
 $('region').replaceChildren();
 regions().forEach((r,i) => $('region').add(new Option(`区域 ${r.id} · ${r.polygon.length} 个顶点`,i)));
 if (regionIndex >= regions().length) regionIndex = 0;
 $('region').value = regionIndex; updateFields(); draw();
}
function vertices() {
 const root = $('vertices'); root.replaceChildren();
 if (!current()) return;
 current().polygon.forEach((point,i) => {
  const row = document.createElement('div'); row.className = 'vertex';
  const label = document.createElement('span'); label.textContent = i + 1; row.append(label);
  point.forEach((value,k) => {
   const input = document.createElement('input'); input.type = 'number'; input.value = value;
   input.min = 0; input.max = k ? canvas.height : canvas.width;
   input.setAttribute('aria-label', `顶点 ${i+1} 的 ${k ? 'y' : 'x'} 坐标`);
   input.onchange = () => { if (!frozen || busy) { message('请先冻结画面再修改坐标',true); input.value = point[k]; return; } point[k] = input.value === '' ? NaN : Number(input.value); markDirty(); draw(); };
   row.append(input);
  }); root.append(row);
 });
}
function adopt(data, preserveStream = false) {
 const oldId = config?.streams[streamIndex]?.id;
 config = data.config; revision = data.revision; token = data.token;
 streamIndex = preserveStream ? Math.max(0,config.streams.findIndex(s => s.id === oldId)) : 0;
 regionIndex = 0; dirty = drawing = false; selectedVertex = -1;
 $('stream').replaceChildren(); config.streams.forEach((s,i) => $('stream').add(new Option(`通道 ${s.id}`,i)));
 $('stream').value = streamIndex;
 canvas.width = config.frame_width || 1280; canvas.height = config.frame_height || 720;
 $('rotation').value = String(!!config.camera_flip_180);
 $('backend').textContent = config.hardware ? '板卡真实采集 · RGA / RKNN' : '模拟验证 · 非真实摄像头与 NPU 检测';
 $('path').textContent = '本控制台配置：' + data.path;
 $('dirty').textContent = '规则已保存';
 regionList();
}
function contains(poly, p) {
 let inside = false;
 for (let i=0,j=poly.length-1;i<poly.length;j=i++) {
  const a=poly[j], b=poly[i], cross=(p[0]-a[0])*(b[1]-a[1])-(p[1]-a[1])*(b[0]-a[0]);
  if (Math.abs(cross)<1e-6 && p[0]>=Math.min(a[0],b[0]) && p[0]<=Math.max(a[0],b[0]) && p[1]>=Math.min(a[1],b[1]) && p[1]<=Math.max(a[1],b[1])) return true;
  if ((a[1]>p[1]) !== (b[1]>p[1]) && p[0]<(b[0]-a[0])*(p[1]-a[1])/(b[1]-a[1])+a[0]) inside=!inside;
 } return inside;
}
function matches(detection) {
 return regions().filter(r => r.polygon.length>=3 && (!r.classes.length || r.classes.includes(detection.class_id)) && contains(r.polygon,[detection.x+detection.w/2,detection.y+detection.h/2]));
}
function draw() {
 if (!config) return;
 const w=canvas.width,h=canvas.height;
 ctx.fillStyle='#101a22';ctx.fillRect(0,0,w,h);
 $('empty').hidden=!!picture;
 if (picture) ctx.drawImage(picture,0,0,w,h);
 regions().forEach((r,i) => {
  if (!r.polygon.length) return;
  const color=colors[i%colors.length];ctx.beginPath();ctx.moveTo(...r.polygon[0]);r.polygon.slice(1).forEach(p=>ctx.lineTo(...p));
  if (!(drawing&&i===regionIndex)) ctx.closePath();
  ctx.fillStyle=color+'22';ctx.fill();ctx.strokeStyle=color;ctx.lineWidth=w/300;ctx.stroke();
  ctx.font=`${Math.max(14,w/40)}px sans-serif`;ctx.fillStyle=color;ctx.fillText('区域 '+r.id,r.polygon[0][0]+8,Math.max(22,r.polygon[0][1]-8));
  if (frozen&&i===regionIndex) r.polygon.forEach((p,j)=>{ctx.beginPath();ctx.arc(...p,w/100,0,Math.PI*2);ctx.fillStyle=j===selectedVertex?'#efb549':'#fff';ctx.fill();ctx.stroke();});
 });
 let hit=0;
 for (const d of packet?.detections || []) {
  const matched=matches(d).length>0; if (matched) hit++;
  ctx.strokeStyle=matched?'#ffd05c':'#76e4d3';ctx.lineWidth=w/240;ctx.strokeRect(d.x,d.y,d.w,d.h);
  const label=`${d.name} ${(d.confidence*100).toFixed(0)}%`;
  ctx.font=`${Math.max(14,w/37)}px sans-serif`;
  const y=Math.max(24,d.y),bw=ctx.measureText(label).width+12;
  ctx.fillStyle='#101a22';ctx.fillRect(d.x,y-24,bw,25);ctx.fillStyle=matched?'#ffd05c':'#76e4d3';ctx.fillText(label,d.x+6,y-5);
 }
 if (packet) {
  const count=packet.detections.length;
  $('match').textContent = state?.mode==='preview' ? '取景模式 · 不执行推理，也不会产生告警。' : `${count} 个检测目标，${hit} 个中心点在当前显示区域内。${dirty?'区域修改未应用，实际告警仍按旧规则判断。':'是否新增告警以数据库记录为准。'}`;
 }
}
function freeze(value) {
 frozen=value;$('freeze').textContent=value?'恢复实时画面':'冻结画面划区';$('freeze').setAttribute('aria-pressed',String(value));draw();
 if(value) message('画面已冻结供编辑；板卡检测若已启动仍继续运行。保存后应用新规则。');
}
function position(e) { const r=canvas.getBoundingClientRect();return [Math.round(Math.max(0,Math.min(canvas.width,(e.clientX-r.left)*canvas.width/r.width))),Math.round(Math.max(0,Math.min(canvas.height,(e.clientY-r.top)*canvas.height/r.height)))]; }
canvas.onpointerdown=e=>{
 if(busy||!config||!frozen||!picture||!current())return;
 const p=position(e),r=current();
 if(drawing){if(r.polygon.length>=256)return message('最多 256 个顶点',true);r.polygon.push(p);markDirty();vertices();draw();}
 else {dragging=r.polygon.findIndex(v=>Math.hypot(v[0]-p[0],v[1]-p[1])<12*canvas.width/canvas.getBoundingClientRect().width);selectedVertex=dragging;if(dragging>=0)canvas.setPointerCapture(e.pointerId);draw();}
};
canvas.onpointermove=e=>{if(dragging<0||busy||!current())return;current().polygon[dragging]=position(e);markDirty();draw();};
for(const event of ['pointerup','pointercancel','lostpointercapture'])canvas.addEventListener(event,()=>{if(dragging>=0)vertices();dragging=-1;});
canvas.onkeydown=e=>{
 if(!frozen||busy||selectedVertex<0||!current())return;
 const delta={ArrowLeft:[-1,0],ArrowRight:[1,0],ArrowUp:[0,-1],ArrowDown:[0,1]}[e.key];if(!delta)return;
 e.preventDefault();const p=current().polygon[selectedVertex],step=e.shiftKey?10:1;p[0]=Math.max(0,Math.min(canvas.width,p[0]+delta[0]*step));p[1]=Math.max(0,Math.min(canvas.height,p[1]+delta[1]*step));markDirty();vertices();draw();
};
async function action(job) {
 if(busy)return;busy=true;$('rules').disabled=true;document.querySelectorAll('.toolbar button,#save,#reload').forEach(b=>b.disabled=true);
 try{await job();}catch(e){message(e.message,true);}
 finally{busy=false;$('rules').disabled=false;document.querySelectorAll('.toolbar button,#save,#reload').forEach(b=>b.disabled=false);updateFields();}
}
for(const mode of ['preview','detect'])$(mode).onclick=()=>action(async()=>{
 if(dirty)throw Error('请先保存应用规则，或放弃修改再启动');
 message(mode==='preview'?'正在启动预览…':'正在加载模型并启动检测…');invalidate();freeze(false);
 state=await request('/api/start',{mode});run=state.run;
 message('已提交启动请求，正在等待首帧。若失败，请查看运行日志。');
});
$('stop').onclick=()=>action(async()=>{message('正在停止并等待告警写盘…');state=await request('/api/stop',{});message('采集与检测已停止，最后画面仅供参考。');});
$('freeze').onclick=()=>{if(drawing)return message('请先完成绘制或删除未完成区域',true);if(!picture)return message('请先启动预览并等待画面',true);freeze(!frozen);};
$('stream').onchange=()=>{if(drawing){$('stream').value=streamIndex;return message('请先完成绘制',true);}streamIndex=Number($('stream').value);regionIndex=0;freeze(false);invalidate();regionList();};
$('region').onchange=()=>{if(drawing){$('region').value=regionIndex;return message('请先完成绘制',true);}regionIndex=Number($('region').value);selectedVertex=-1;updateFields();draw();};
$('add').onclick=()=>{if(!frozen||!picture)return message('先启动预览，再冻结画面划区',true);if(drawing)return message('请先完成当前区域',true);if(regions().length>=64)return message('每路最多 64 个区域',true);regions().push({id:Math.max(-1,...regions().map(r=>r.id))+1,polygon:[],classes:[0],cooldown_ms:5000});regionIndex=regions().length-1;drawing=true;regionList();markDirty();message('依次点击至少 3 个顶点，最后点击完成绘制。');};
$('finish').onclick=()=>{if(!current()||current().polygon.length<3)return message('至少需要 3 个顶点',true);drawing=false;regionList();message('可以继续拖动顶点，或保存应用规则。');};
$('delete').onclick=()=>{if(!frozen)return message('请先冻结画面再删除区域',true);if(!current()||!confirm('删除当前区域？保存应用后才会生效。'))return;regions().splice(regionIndex,1);drawing=false;regionIndex=0;regionList();markDirty();};
$('undo').onclick=()=>{if(!frozen||!current())return;current().polygon.pop();regionList();markDirty();};
$('classes').oninput=()=>{if(!current())return;const text=$('classes').value.trim();current().classes=text?text.split(',').map(v=>v.trim()===''?NaN:Number(v)):[];$('preset').value='custom';markDirty();draw();};
$('preset').onchange=()=>{if(!current()||$('preset').value==='custom')return;$('classes').value=$('preset').value;current().classes=$('preset').value?$('preset').value.split(',').map(Number):[];markDirty();draw();};
$('cooldown').oninput=()=>{if(current()){current().cooldown_ms=$('cooldown').value===''?NaN:Number($('cooldown').value);markDirty();}};
$('rotation').onchange=()=>{markDirty();message('方向将在保存应用后改变；模型、截图和区域将同步转换。');};
$('save').onclick=()=>action(async()=>{
 if(drawing)throw Error('请先完成绘制');
 message('正在校验、保存并应用规则…');
 const data=await request('/api/save',{revision,streams:config.streams.map(s=>({id:s.id,regions:s.regions})),camera_flip_180:$('rotation').value==='true'});
 adopt(data,true);invalidate();freeze(false);message(data.restart_error?'已保存但启动失败：'+data.restart_error:data.message,!!data.restart_error);
});
$('reload').onclick=()=>action(async()=>{if(dirty&&!confirm('放弃未应用修改？'))return;adopt(await request('/api/config'),true);invalidate();freeze(false);message('已重新加载保存的配置。');});
function events(alarms){const list=$('alarms');list.replaceChildren();if(!alarms.length){const li=document.createElement('li');li.textContent='暂无已入库告警';list.append(li);return;}alarms.forEach(a=>{const li=document.createElement('li');li.textContent=`通道 ${a.stream_id ?? a.stream ?? '—'} · 区域 ${a.region_id ?? a.region ?? '—'} · 类别 ${a.class_id ?? a.class ?? '—'} · 置信度 ${typeof a.confidence==='number'?(a.confidence*100).toFixed(1)+'%':'—'}`;const note=document.createElement('span');note.textContent=`事件 ${a.event_id || '—'} · 入库 ${a.received_at} UTC`;li.append(note);if(a.screenshot&&a.event_id){const link=document.createElement('a');link.textContent='查看告警截图';link.href='/api/shot?event='+encodeURIComponent(a.event_id);link.target='_blank';link.rel='noopener';li.append(link);}list.append(li);});}
async function pollStatus(){
 try{
  const next=await request('/api/status');state=next;
  if(run!==next.run){run=next.run;invalidate();}
  const labels={stopped:'已停止',preview:'预览中 · 无推理',detect:'检测与告警运行中',failed:'运行失败'};
  $('state').textContent=next.running&&!next.ready?'正在启动 · 等待首帧':labels[next.mode]||next.mode;$('state').classList.toggle('error',next.mode==='failed');
  $('summary').textContent=next.hardware?'板卡画面、真实检测与警戒区在同一页面。':'当前是模拟验证，不代表真实摄像头或 NPU 效果。';
  $('logs').textContent=next.logs.join('\n')||'尚无进程日志';events(next.alarms);
  if(next.error)message(next.error,true);
  if(!dirty)$('dirty').textContent=next.applied?'规则已应用':'规则已保存 · 启动后应用';
 }catch(e){$('state').textContent='控制台连接中断';$('state').classList.add('error');message('控制台未响应。检查 USB 与板卡窗口 A；画面可能已经过时。',true);state=null;}
 setTimeout(pollStatus,1000);
}
async function pollFrame(){
 try {
 if(config&&state?.running&&!frozen&&!busy){
  const expected=operation,expectedRun=run;
  try{
   const data=await request('/api/frame?stream='+config.streams[streamIndex].id);
   if(expected!==operation||data.run!==expectedRun||frozen)return;
   if(lastSequence!==data.sequence){
    const image=new Image();image.src=data.image;await image.decode();
    if(expected!==operation||data.run!==run||frozen)return;
    picture=image;packet=data;lastSequence=data.sequence;lastFrameAt=performance.now();receivedAge=data.age_ms;draw();
    if($('message').textContent.startsWith('已提交启动请求')||$('message').textContent.startsWith('正在等待同一帧'))message(state?.mode==='preview'?'预览已开启，可以冻结画面编辑区域。':'画面与检测结果已收到，正在持续更新。');
   }
  }catch(e){if(!picture&&!busy)message(e.message,true);}
 }
 } finally { setTimeout(pollFrame,330); }
}
setInterval(()=>{if(!packet)return;const age=receivedAge+performance.now()-lastFrameAt;const stale=age>3000;$('frameInfo').textContent=`帧 ${packet.frame_index} · ${packet.width}×${packet.height} · ${frozen?'已冻结':!state?.running?'已停止，保留最后画面':stale?'画面未更新，请查看日志':'持续更新'} · 距该帧 ${Math.round(age/1000)} 秒`;},500);
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
async function init(){try{adopt(await request('/api/config'));message('当前空闲。点击启动预览或启动检测，由你开启摄像头。');pollStatus();pollFrame();}catch(e){message(e.message,true);}}
init();
