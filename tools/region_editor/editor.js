'use strict';
const $=id=>document.getElementById(id), canvas=$('canvas'), ctx=canvas.getContext('2d');
let cfg, revision, token, streamIndex=0, regionIndex=0, dirty=false, drawing=false, dragging=-1, probe=null, media=null, mediaURL=null, generation=0;

const colors=['#30d4ab','#ffbd59','#8c9eff','#ed93cf'];
function status(text,error=false){$('status').textContent=text;$('status').style.color=error?'#b52c35':'#165c95';}
function changed(){dirty=true;$('dirty').textContent='有未保存修改';}
function regions(){return cfg.streams[streamIndex].regions;}
function current(){return regions()[regionIndex];}
function stopMedia(){if(media instanceof HTMLVideoElement)media.pause();media=null;if(mediaURL)URL.revokeObjectURL(mediaURL);mediaURL=null;}
function sample(){generation++;stopMedia();$('processed').checked=false;$('source').textContent='示例画面 · 仅演示划区，不进行目标识别';}
function fields(){const r=current();$('classes').value=r?r.classes.join(','):'';$('cooldown').value=r?r.cooldown_ms:5000;const v=$('classes').value;$('preset').value=['0','2','1,2,3',''].includes(v)?v:'custom';for(const id of ['classes','cooldown','preset','delete'])$(id).disabled=!r;}
function regionList(){const el=$('region');el.replaceChildren();regions().forEach((r,i)=>el.add(new Option(`区域 ${r.id} · ${r.polygon.length} 个顶点`,i)));el.value=regionIndex;fields();}
function streamChanged(){streamIndex=Number($('stream').value);regionIndex=0;drawing=false;probe=null;$('probeResult').textContent='未测试';sample();regionList();}
async function load(){
 const res=await fetch('/api/config');if(!res.ok)throw Error('配置加载失败');const data=await res.json();cfg=data.config;revision=data.revision;token=data.token;
 cfg.streams.forEach((s,i)=>{s.id??=i;s.regions??=[];s.regions.forEach((r,j)=>{r.id??=j;r.classes??=[];r.cooldown_ms??=5000;});});
 canvas.width=cfg.frame_width||1280;canvas.height=cfg.frame_height||720;
 // CSS size always has the same aspect ratio: pointer mapping does not include letterboxing from CSS.
 canvas.style.aspectRatio=`${canvas.width}/${canvas.height}`;canvas.style.maxWidth=`${65*canvas.width/canvas.height}vh`;canvas.style.maxHeight='none';
 $('stream').replaceChildren();cfg.streams.forEach((s,i)=>$('stream').add(new Option(`通道 ${s.id}`,i)));
 $('capture').disabled=!data.capture;$('path').textContent=`正在编辑：${data.path}`;
 dirty=false;$('dirty').textContent='配置已加载';streamChanged();status('设置区域后点击保存。原始检测配置以底部路径为准。');
}
function coords(event){const rect=canvas.getBoundingClientRect();return [Math.round(Math.max(0,Math.min(canvas.width,(event.clientX-rect.left)*canvas.width/rect.width))),Math.round(Math.max(0,Math.min(canvas.height,(event.clientY-rect.top)*canvas.height/rect.height)))];}
function contains(poly,p){let inside=false;for(let i=0,j=poly.length-1;i<poly.length;j=i++){
 const a=poly[j],b=poly[i],cross=(p[0]-a[0])*(b[1]-a[1])-(p[1]-a[1])*(b[0]-a[0]);
 if(Math.abs(cross)<1e-6&&p[0]>=Math.min(a[0],b[0])&&p[0]<=Math.max(a[0],b[0])&&p[1]>=Math.min(a[1],b[1])&&p[1]<=Math.max(a[1],b[1]))return true;
 if((a[1]>p[1])!==(b[1]>p[1])&&p[0]<(b[0]-a[0])*(p[1]-a[1])/(b[1]-a[1])+a[0])inside=!inside;
 }return inside;}
function fitDimensions(sw,sh,w,h){const s=Math.min(w/sw,h/sh);return [Math.max(2,Math.floor(Math.floor(sw*s+0.5)/2)*2),Math.max(2,Math.floor(Math.floor(sh*s+0.5)/2)*2)];}
function draw(){requestAnimationFrame(draw);if(!cfg)return;const w=canvas.width,h=canvas.height;ctx.fillStyle='#000';ctx.fillRect(0,0,w,h);
 if(media){const sw=media.videoWidth||media.naturalWidth,sh=media.videoHeight||media.naturalHeight;
 if(sw&&sh){if($('processed').checked){ctx.drawImage(media,0,0,w,h);}else{const [fw,fh]=fitDimensions(sw,sh,w,h);const x=Math.floor((w-fw)/2),y=Math.floor((h-fh)/2);ctx.save();if(cfg.camera_flip_180){ctx.translate(x+fw,y+fh);ctx.rotate(Math.PI);ctx.drawImage(media,0,0,fw,fh);}else ctx.drawImage(media,x,y,fw,fh);ctx.restore();}}
 }else{const cw=cfg.capture_width||640,ch=cfg.capture_height||480;const [fw,fh]=fitDimensions(cw,ch,w,h),x=Math.floor((w-fw)/2),y=Math.floor((h-fh)/2);ctx.fillStyle='#263b4d';ctx.fillRect(x,y,fw,fh);ctx.strokeStyle='#3c5367';ctx.lineWidth=1;for(let k=0;k<10;k++){ctx.beginPath();ctx.moveTo(x+fw*k/10,y);ctx.lineTo(x+fw*k/10,y+fh);ctx.stroke();ctx.beginPath();ctx.moveTo(x,y+fh*k/10);ctx.lineTo(x+fw,y+fh*k/10);ctx.stroke();}ctx.fillStyle='#b4c9db';ctx.font=`${w/28}px sans-serif`;ctx.fillText('示例画面 · 可直接练习划区',x+fw*.08,y+fh*.16);ctx.fillStyle='#637d91';ctx.fillRect(x+fw*.65,y+fh*.3,fw*.16,fh*.42);}
 regions().forEach((r,i)=>{const pts=r.polygon;if(!pts.length)return;const color=colors[i%colors.length];ctx.beginPath();ctx.moveTo(...pts[0]);pts.slice(1).forEach(p=>ctx.lineTo(...p));if(!(drawing&&i===regionIndex))ctx.closePath();ctx.fillStyle=color+'28';ctx.fill();ctx.strokeStyle=color;ctx.lineWidth=w/250;ctx.stroke();ctx.font=`${w/36}px sans-serif`;ctx.fillStyle=color;ctx.fillText(`区域 ${r.id}`,pts[0][0]+9,Math.max(20,pts[0][1]-10));if(i===regionIndex)pts.forEach(p=>{ctx.beginPath();ctx.arc(...p,w/95,0,Math.PI*2);ctx.fillStyle='#fff';ctx.fill();ctx.stroke();});});
 if(probe){ctx.beginPath();ctx.arc(...probe,w/90,0,Math.PI*2);ctx.fillStyle='#ff5464';ctx.fill();}
}
canvas.addEventListener('pointerdown',e=>{if(!cfg)return;const p=coords(e);if($('probe').checked){probe=p;const hits=regions().filter((r,i)=>!(drawing&&i===regionIndex)&&r.polygon.length>=3&&contains(r.polygon,p));$('probeResult').textContent=`中心点 (${p})：${hits.length?'在区域 '+hits.map(r=>r.id).join('、')+' 内':'在所有区域外'}`;return;}const r=current();if(!r)return;if(drawing){if(r.polygon.length>=256){status('最多 256 个顶点',true);return;}r.polygon.push(p);changed();}else{const threshold=12*canvas.width/canvas.getBoundingClientRect().width;dragging=r.polygon.findIndex(q=>Math.hypot(q[0]-p[0],q[1]-p[1])<threshold);if(dragging>=0)canvas.setPointerCapture(e.pointerId);}});
canvas.addEventListener('pointermove',e=>{const p=coords(e);$('coords').textContent=`检测画面坐标：x=${p[0]}，y=${p[1]}；尺寸 ${canvas.width} × ${canvas.height}`;if(dragging>=0&&current()){current().polygon[dragging]=p;changed();}});
for(const evt of ['pointerup','pointercancel','lostpointercapture'])canvas.addEventListener(evt,()=>dragging=-1);
$('add').onclick=()=>{if(drawing){status('请先完成或删除正在绘制的区域',true);return;}const id=Math.max(-1,...regions().map(r=>r.id))+1;regions().push({id,polygon:[],classes:[0],cooldown_ms:5000});regionIndex=regions().length-1;drawing=true;$('probe').checked=false;regionList();changed();status('依次点击顶点，最后点击“完成绘制”。');};
$('finish').onclick=()=>{if(!current()||current().polygon.length<3){status('至少点击三个顶点',true);return;}drawing=false;regionList();status('可拖动白色圆点修改边界。保存时校验区域。');};
$('undo').onclick=()=>{if(drawing&&current()){current().polygon.pop();changed();}};
$('delete').onclick=()=>{if(!current()||!confirm('删除当前区域？保存后才会修改配置文件。'))return;regions().splice(regionIndex,1);regionIndex=0;drawing=false;regionList();changed();};
$('region').onchange=()=>{if(drawing){$('region').value=regionIndex;status('请先完成或删除正在绘制的区域',true);return;}regionIndex=Number($('region').value);fields();};
$('stream').onchange=()=>{if(drawing){$('stream').value=streamIndex;status('请先完成或删除正在绘制的区域',true);return;}streamChanged();};
$('classes').oninput=()=>{const text=$('classes').value.trim();current().classes=text?text.split(',').map(s=>s.trim()===''?NaN:Number(s)):[];$('preset').value='custom';changed();};
$('preset').onchange=()=>{if($('preset').value==='custom')return;const v=$('preset').value;$('classes').value=v;current().classes=v?v.split(',').map(Number):[];changed();};
$('cooldown').oninput=()=>{current().cooldown_ms=$('cooldown').value===''?NaN:Number($('cooldown').value);changed();};
$('sample').onclick=sample;
$('processed').onchange=()=>{if(media&&$('processed').checked){const w=media.videoWidth||media.naturalWidth,h=media.videoHeight||media.naturalHeight;if(w!==canvas.width||h!==canvas.height){$('processed').checked=false;status('已处理截图尺寸必须与检测画面完全相同，避免坐标误配',true);}}};
$('media').onchange=async()=>{const file=$('media').files[0];if(!file)return;sample();const gen=generation;mediaURL=URL.createObjectURL(file);const item=file.type.startsWith('video/')?document.createElement('video'):new Image();if(item instanceof HTMLVideoElement){item.muted=true;item.loop=true;item.playsInline=true;}item.src=mediaURL;const ready=item instanceof HTMLVideoElement?'loadeddata':'load';item.addEventListener(ready,()=>{if(gen!==generation)return;media=item;$('source').textContent=`本机素材：${file.name} · 仅在浏览器读取，不上传；需与选定通道视角一致`;$('processed').checked=false;});item.addEventListener('error',()=>status('无法读取此素材，请尝试 PNG/JPEG 或浏览器支持的 MP4',true));$('media').value='';};
$('pause').onclick=()=>{if(media instanceof HTMLVideoElement){if(media.paused)media.play().catch(e=>status(e.message,true));else media.pause();}};
async function post(path,body){const response=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-Editor-Token':token},body:JSON.stringify(body)});if(!response.ok){const d=await response.json();throw Error(d.error||'请求失败');}return response;}
$('capture').onclick=async()=>{if(!confirm('将短暂读取选定视频源。请先手动停止占用它的检测程序；页面不会终止其他进程。继续？'))return;const index=streamIndex;sample();const gen=generation;$('capture').disabled=true;status('正在取景，最长等待约 12 秒…');try{const res=await post('/api/capture',{index});const blob=await res.blob();if(gen!==generation)return;mediaURL=URL.createObjectURL(blob);const item=new Image();item.onload=()=>{if(gen!==generation)return;media=item;$('processed').checked=true;$('source').textContent='服务所在主机的配置视频源 · 单帧取景（非实时流）';status('取景成功，可以划区。');};item.src=mediaURL;}catch(e){status(e.message,true);}finally{$('capture').disabled=false;}};
$('save').onclick=async()=>{
 if(drawing){status('请先完成绘制',true);return;}
 const snapshot=JSON.stringify(cfg.streams.map(s=>({id:s.id,regions:s.regions})));
 $('save').disabled=true;$('reload').disabled=true;
 try{
  const r=await post('/api/save',{revision,streams:JSON.parse(snapshot)});
  const data=await r.json();revision=data.revision;
  dirty=snapshot!==JSON.stringify(cfg.streams.map(s=>({id:s.id,regions:s.regions})));
  $('dirty').textContent=dirty?'有未保存修改':'已保存 · 下次启动生效';
  status(data.message+(dirty?' 保存期间有新修改，需再次保存。':''));
 }catch(e){status(e.message,true);}
 finally{$('save').disabled=false;$('reload').disabled=false;}
};
$('reload').onclick=()=>{if(dirty&&!confirm('放弃未保存修改并重新加载？'))return;load().catch(e=>status(e.message,true));};
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
load().catch(e=>status(e.message,true));draw();
