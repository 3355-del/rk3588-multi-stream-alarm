#!/usr/bin/env python3
"""Browser polygon editor. Standard library only; never opens a PC camera.

Default: edit a separate offline configuration. Explicit --config edits that file.
Board capture is opt-in, uses configured source only, and requires ffmpeg.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]


def revision(raw):
    return hashlib.sha256(raw).hexdigest()


def cross(a, b, c):
    return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])


def intersects(a, b, c, d):
    def on(a, b, p):
        return abs(cross(a, b, p)) < 1e-7 and all(min(a[k], b[k]) <= p[k] <= max(a[k], b[k]) for k in (0, 1))
    return (cross(a, b, c)*cross(a, b, d) < 0 and cross(c, d, a)*cross(c, d, b) < 0) or any((on(a,b,c),on(a,b,d),on(c,d,a),on(c,d,b)))


def validate_polygon(points, w, h):
    if not isinstance(points, list) or not 3 <= len(points) <= 256:
        raise ValueError('每个区域需要 3～256 个顶点')
    for p in points:
        if not isinstance(p, list) or len(p) != 2 or any(type(v) not in (int,float) or not math.isfinite(v) for v in p):
            raise ValueError('顶点必须是有限数字坐标')
        if not 0 <= p[0] <= w or not 0 <= p[1] <= h:
            raise ValueError('顶点超出检测画面')
    n = len(points)
    if len({tuple(p) for p in points}) != n:
        raise ValueError('区域不能包含重复顶点')
    for i in range(n):
        for j in range(i+1,n):
            if j == i+1 or (i == 0 and j == n-1):
                continue
            if intersects(points[i],points[(i+1)%n],points[j],points[(j+1)%n]):
                raise ValueError('区域边线不能交叉或相互接触')
    if abs(sum(points[i][0]*points[(i+1)%n][1]-points[(i+1)%n][0]*points[i][1] for i in range(n))) < 1e-6:
        raise ValueError('区域面积不能为零')
    for i in range(n):
        a,b,c=points[i-1],points[i],points[(i+1)%n]
        if abs(cross(a,b,c)) < 1e-7 and sum((a[k]-b[k])*(c[k]-b[k]) for k in (0,1)) > 0:
            raise ValueError('相邻边不能折返重叠')


def merge_regions(config, streams):
    out = copy.deepcopy(config)
    if not isinstance(streams,list) or len(streams) != len(out['streams']):
        raise ValueError('视频路数不匹配，请重新加载')
    w,h=out.get('frame_width',1280),out.get('frame_height',720)
    for idx, (target, incoming) in enumerate(zip(out['streams'],streams)):
        if not isinstance(incoming,dict) or incoming.get('id') != target.get('id',idx):
            raise ValueError('视频 ID 不匹配')
        regions=incoming.get('regions')
        if not isinstance(regions,list) or len(regions)>64:
            raise ValueError('每路最多 64 个区域')
        ids=set()
        for r in regions:
            if not isinstance(r,dict) or set(r) != {'id','polygon','classes','cooldown_ms'}:
                raise ValueError('区域字段不正确')
            if type(r['id']) is not int or not 0 <= r['id'] <= 2147483647 or r['id'] in ids:
                raise ValueError('区域 ID 必须唯一且非负')
            ids.add(r['id'])
            if type(r['cooldown_ms']) is not int or not 0 <= r['cooldown_ms'] <= 2147483647:
                raise ValueError('冷却时间必须为非负整数毫秒')
            classes=r['classes']
            if not isinstance(classes,list) or any(type(c) is not int or not 0 <= c < (80 if out.get('hardware') else 5) for c in classes):
                raise ValueError('目标类别 ID 超出模型配置范围')
            validate_polygon(r['polygon'],w,h)
        target['regions']=copy.deepcopy(regions)
    return out


class Store:
    def __init__(self,path):
        self.path=Path(path).resolve()
        self.lock=threading.Lock()

    def read(self):
        raw=self.path.read_bytes()
        return json.loads(raw.decode('utf-8-sig')),revision(raw)

    def save(self, expected, streams):
        with self.lock:
            config, rev=self.read()
            if expected != rev:
                raise ValueError('配置已被其他窗口或程序修改，请重新加载后再编辑')
            updated=merge_regions(config,streams)
            raw=self.path.read_bytes()
            if revision(raw)!=rev:
                raise ValueError('配置已改变，请重新加载')
            backup=self.path.with_name(self.path.name+'.before-region-edit.bak')
            backup.write_bytes(raw)
            fd,name=tempfile.mkstemp(prefix=self.path.name+'.',dir=self.path.parent)
            try:
                with os.fdopen(fd,'w',encoding='utf-8',newline='\n') as f:
                    json.dump(updated,f,ensure_ascii=False,indent=2)
                    f.write('\n'); f.flush(); os.fsync(f.fileno())
                os.replace(name,self.path)
            finally:
                if os.path.exists(name): os.unlink(name)
            return self.read()[1]


def capture_command(config, index):
    source=config['streams'][index].get('source_uri','')
    if not config.get('hardware') or source.startswith('mock:') or not source:
        raise ValueError('模拟源请使用示例画面或导入图片/视频')
    cmd=['ffmpeg','-nostdin','-hide_banner','-loglevel','error']
    if source.startswith('/dev/video'):
        cmd += ['-f','v4l2','-input_format','yuyv422','-video_size',f"{config.get('capture_width',640)}x{config.get('capture_height',480)}"]
    cmd += ['-i',source]
    w,h=config.get('frame_width',1280),config.get('frame_height',720)
    scale=f"scale=w='max(2,trunc(floor(iw*min({w}/iw,{h}/ih)+0.5)/2)*2)':h='max(2,trunc(floor(ih*min({w}/iw,{h}/ih)+0.5)/2)*2)'"
    filters=(["hflip","vflip"] if config.get('camera_flip_180',False) else [])+[scale,f'pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:black']
    return cmd+['-vf',','.join(filters),'-frames:v','1','-threads','1','-f','image2pipe','-c:v','png','pipe:1']


class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args): pass

    def send_data(self,status,body,kind='application/json; charset=utf-8'):
        if not isinstance(body,bytes): body=json.dumps(body,ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type',kind)
        self.send_header('Content-Length',str(len(body)))
        self.send_header('Cache-Control','no-store')
        self.send_header('X-Content-Type-Options','nosniff')
        self.send_header('X-Frame-Options','DENY')
        self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        path=urlsplit(self.path).path
        try:
            if path=='/':
                self.send_data(200,(ROOT/'tools/region_editor/index.html').read_bytes(),'text/html; charset=utf-8')
            elif path in ('/editor.js','/editor.css'):
                self.send_data(200,(ROOT/'tools/region_editor'/path[1:]).read_bytes(),'text/javascript; charset=utf-8' if path.endswith('.js') else 'text/css; charset=utf-8')
            elif path=='/api/config':
                config,rev=self.server.store.read()
                self.send_data(200,{'config':config,'revision':rev,'token':self.server.token,'path':str(self.server.store.path),'capture':self.server.capture})
            else: self.send_data(404,{'error':'不存在'})
        except (ValueError,OSError) as e: self.send_data(400,{'error':str(e)})

    def do_POST(self):
        if self.headers.get('X-Editor-Token') != self.server.token:
            self.send_data(403,{'error':'请从本服务页面操作'}); return
        origin=self.headers.get('Origin')
        if origin and urlsplit(origin).netloc != self.headers.get('Host'):
            self.send_data(403,{'error':'拒绝跨站请求'}); return
        try:
            size=int(self.headers.get('Content-Length','0'))
            if not 0<size<=1024*1024: raise ValueError('请求体大小无效')
            body=json.loads(self.rfile.read(size))
            if not isinstance(body,dict): raise ValueError('请求格式错误')
            if self.path=='/api/save':
                rev=self.server.store.save(body['revision'],body['streams'])
                self.send_data(200,{'revision':rev,'message':'已保存。检测程序下次使用此配置启动时生效；运行中的规则未改变。'})
            elif self.path=='/api/capture':
                if not self.server.capture: raise ValueError('未启用板卡取景；当前只使用离线素材')
                if not self.server.capture_lock.acquire(False): raise ValueError('正在取景，请稍后')
                try:
                    config,_=self.server.store.read()
                    index=body['index']
                    if type(index) is not int or not 0<=index<len(config['streams']): raise ValueError('视频序号无效')
                    result=subprocess.run(capture_command(config,index),cwd=ROOT,capture_output=True,timeout=12)
                    if result.returncode or not result.stdout.startswith(b'\x89PNG'):
                        raise ValueError('取景失败，请检查 FFmpeg、视频源及摄像头是否被占用；不会停止其他程序')
                    self.send_data(200,result.stdout,'image/png')
                finally: self.server.capture_lock.release()
            else: self.send_data(404,{'error':'不存在'})
        except (ValueError,KeyError,TypeError,OSError,subprocess.TimeoutExpired) as e:
            self.send_data(400,{'error':str(e)})


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',type=Path)
    parser.add_argument('--host',default='127.0.0.1')
    parser.add_argument('--port',type=int,default=8092)
    parser.add_argument('--allow-capture',action='store_true',help='仅在需要取景的主机显式启用')
    args=parser.parse_args()
    path=args.config
    if path is None:
        path=ROOT/'config/region_editor_offline.json'
        if not path.exists():
            path.write_bytes((ROOT/'config/rk3588_config.json').read_bytes())
    store=Store(path); store.read()
    server=ThreadingHTTPServer((args.host,args.port),Handler)
    server.store=store; server.token=secrets.token_urlsafe(32)
    server.capture=args.allow_capture; server.capture_lock=threading.Lock()
    print(f'区域设置 http://{args.host}:{args.port}/\n配置：{store.path}\n保存后下次启动检测生效；Ctrl+C 关闭服务',flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close()


if __name__=='__main__': main()
