#!/usr/bin/env python3
"""Single-capture live console. Idle until an explicit browser start action.

Images and boxes are published together by the C++ pipeline, never a second camera.
The original configuration/binary remain untouched; live rules have their own file.
"""
import argparse
import base64
import copy
from collections import deque
from contextlib import closing
import ipaddress
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import sqlite3
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

from alarm_receiver import AlarmServer, Handler as AlarmHandler, initialize
from region_editor import merge_regions, revision

ROOT = Path(__file__).resolve().parents[1]
ASSETS = Path(__file__).with_suffix('')


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=path.name+'.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as file:
            json.dump(value, file, ensure_ascii=False, indent=2, allow_nan=False)
            file.write('\n'); file.flush(); os.fsync(file.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name): os.unlink(name)


def rotate_rules(config):
    """Keep the same physical regions when rotating the complete detector image."""
    config['camera_flip_180'] = not config.get('camera_flip_180', False)
    w, h = config.get('frame_width', 1280), config.get('frame_height', 720)
    for stream in config['streams']:
        for region in stream.get('regions', []):
            region['polygon'] = [[w-x, h-y] for x, y in region['polygon']]


class Controller:
    def __init__(self, project, source, binary, runtime=None):
        self.root = Path(project).resolve()
        self.binary = Path(binary).resolve()
        self.config_path = self.root/'data/live-console/config.json'
        self.lock = threading.RLock()
        self.token = secrets.token_urlsafe(32)
        self.logs = deque(maxlen=120)
        self.log_lock = threading.Lock()
        self.process = None
        self.receiver = None
        self.receiver_thread = None
        self.mode = 'stopped'
        self.error = ''
        self.active_revision = None
        self.session = secrets.token_hex(8)
        self.runtime_base = Path(runtime) if runtime else (Path('/dev/shm') if Path('/dev/shm').is_dir() else self.root/'data/live-console/tmp')
        self.runtime_base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='msad-live-', dir=self.runtime_base)
        self.frames = Path(self.temp.name)
        self.run_id = 0
        if not self.config_path.exists():
            config = json.loads(Path(source).read_text(encoding='utf-8-sig'))
            for i, stream in enumerate(config['streams']):
                stream.setdefault('id', i); stream.setdefault('regions', [])
                for j, region in enumerate(stream['regions']):
                    region.setdefault('id', j); region.setdefault('classes', [])
                    region.setdefault('cooldown_ms', 5000)
            # The user's camera is upright. Preserve original file and physical regions.
            if config.get('hardware') and config.get('camera_flip_180'):
                rotate_rules(config)
            atomic_json(self.config_path, config)
        self.database = self.root/'data/alarms.sqlite3'

    def read_config(self):
        raw = self.config_path.read_bytes()
        return json.loads(raw), revision(raw)

    def config_response(self):
        with self.lock:
            config, rev = self.read_config()
            return {'config': config, 'revision': rev, 'token': self.token,
                    'path': str(self.config_path), 'hardware': bool(config.get('hardware'))}

    def _receiver(self):
        if self.receiver: return
        # DurableAlarmNotifier binds pending events to their original endpoint.
        # Reuse it across console restarts; never delete or redirect the outbox.
        destination = self.root/'alarm_outbox_live/destination.txt'
        port = 0
        if destination.exists():
            host, separator, value = destination.read_text(encoding='ascii').strip().rpartition(':')
            if host != '127.0.0.1' or separator != ':' or not value.isdecimal() or not 1 <= int(value) <= 65535:
                raise ValueError('补发队列接收地址无效，已保留原文件，请检查 alarm_outbox_live/destination.txt')
            port = int(value)
        initialize(self.database)
        try:
            receiver = AlarmServer(('127.0.0.1', port), AlarmHandler)
        except OSError as error:
            raise ValueError(f'无法绑定告警接收端口 {port}；请释放端口后重试，补发队列已保留：{error}') from error
        receiver.database = str(self.database)
        self.receiver = receiver
        self.receiver_thread = threading.Thread(target=receiver.serve_forever, kwargs={'poll_interval': .2}, daemon=True)
        self.receiver_thread.start()

    def _read_log(self, process, run_id):
        try:
            for line in process.stdout:
                # Bound both line count and individual line size.
                with self.log_lock:
                    if run_id == self.run_id:
                        self.logs.append(line.rstrip()[:2000])
        finally:
            process.stdout.close()

    def _poll(self):
        if self.process and self.process.poll() is not None:
            code = self.process.returncode
            self.error = f'采集/检测进程已退出（代码 {code}），请查看运行日志。'
            self.mode = 'failed'
            self.active_revision = None

    def start(self, mode):
        if mode not in ('preview', 'detect'): raise ValueError('运行模式无效')
        with self.lock:
            self._poll()
            if self.process and self.process.poll() is None:
                if self.mode == mode: return self.status()
                self.stop()
            if not self.binary.is_file(): raise ValueError(f'未找到新程序：{self.binary}，请先构建')
            config, rev = self.read_config()
            if config.get('frame_width',1280)*config.get('frame_height',720)>1280*1280:
                raise ValueError('实时显示支持每路最多 1280×1280 像素，请调整配置后重试')
            self.run_id += 1
            if self.frames != Path(self.temp.name):
                shutil.rmtree(self.frames)
            self.frames = Path(self.temp.name)/str(self.run_id)
            self.frames.mkdir()
            config['run_seconds'] = 0
            config['screenshot_dir'] = str((self.root/config.get('screenshot_dir','screenshots')).resolve())
            if config.get('hardware'):
                config['model_path'] = str((self.root/config['model_path']).resolve())
                for stream in config['streams']:
                    uri = stream.get('source_uri','')
                    if '://' not in uri and not Path(uri).is_absolute():
                        stream['source_uri'] = str((self.root/uri).resolve())
            if mode == 'detect':
                self._receiver()
                config['alarm'] = {'host':'127.0.0.1', 'port':self.receiver.server_address[1],
                                   'reliable':True, 'outbox_dir':str(self.root/'alarm_outbox_live')}
            runtime_config = self.frames/'runtime.json'
            atomic_json(runtime_config, config)
            command = [str(self.binary), str(runtime_config), '--preview-dir',str(self.frames),'--preview-fps','3']
            if mode == 'preview': command.append('--preview-only')
            options = {'start_new_session':True} if os.name != 'nt' else {'creationflags':subprocess.CREATE_NEW_PROCESS_GROUP}
            process = subprocess.Popen(command, cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, encoding='utf-8', errors='replace', **options)
            self.process = process
            self.mode, self.error, self.active_revision = mode, '', rev
            with self.log_lock: self.logs.clear()
            threading.Thread(target=self._read_log,args=(process,self.run_id),daemon=True).start()
            return self.status()

    def stop(self):
        with self.lock:
            process = self.process
            if process and process.poll() is None:
                if os.name == 'nt': process.terminate()
                else: os.killpg(process.pid, signal.SIGINT)
                try: process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    # Do not force-kill a process that might be persisting alarms.
                    self.error = '停止超过 15 秒，尚未强制终止；请查看日志，等待写盘完成后重试。'
                    raise ValueError(self.error)
            self.process = None
            self.mode, self.active_revision = 'stopped', None
            return self.status()

    def save(self, body):
        with self.lock:
            config, rev = self.read_config()
            if body.get('revision') != rev: raise ValueError('配置已改变，请重新加载后编辑')
            updated = merge_regions(config, body.get('streams'))
            desired = body.get('camera_flip_180', config.get('camera_flip_180',False))
            if type(desired) is not bool: raise ValueError('方向参数无效')
            # Browser submits polygons in the OLD image coordinate system.
            if desired != config.get('camera_flip_180',False): rotate_rules(updated)
            self._poll()
            previous_mode = self.mode
            if self.process and self.process.poll() is None: self.stop()
            self.config_path.with_suffix('.json.bak').write_bytes(self.config_path.read_bytes())
            atomic_json(self.config_path, updated)
            restart_error = ''
            try:
                if previous_mode in ('preview','detect'): self.start(previous_mode)
            except (ValueError, OSError) as error:
                self.mode, self.error = 'failed', str(error)
                restart_error = self.error
            return {**self.config_response(), 'message':'规则已保存；运行中的程序会短暂停顿并重新加载。' if previous_mode in ('preview','detect') else '规则已保存，点击启动后生效。',
                    'restart_error': restart_error}

    def status(self):
        with self.lock:
            self._poll()
            config, rev = self.read_config()
            running = bool(self.process and self.process.poll() is None)
            with self.log_lock: logs = list(self.logs)[-25:]
            return {'mode':self.mode, 'running':running, 'error':self.error,
                    'ready':running and any(self.frames.glob('s*.json')),
                    'applied':running and rev == self.active_revision,
                    'hardware':bool(config.get('hardware')), 'run':f'{self.session}-{self.run_id}',
                    'logs':logs, 'alarms':self.alarms()}

    def alarms(self):
        if not self.database.exists(): return []
        try:
            with closing(sqlite3.connect(self.database.as_uri()+'?mode=ro',uri=True,timeout=.2)) as db:
                rows = db.execute('SELECT payload,received_at FROM alarms ORDER BY id DESC LIMIT 6').fetchall()
            return [{**json.loads(payload),'received_at':received} for payload,received in rows]
        except (sqlite3.Error, ValueError): return []

    def shot(self, event_id):
        if not isinstance(event_id,str) or not 1 <= len(event_id) <= 128:
            raise ValueError('告警事件 ID 无效')
        with closing(sqlite3.connect(self.database.as_uri()+'?mode=ro',uri=True,timeout=.2)) as db:
            row=db.execute('SELECT payload FROM alarms WHERE event_id=?',(event_id,)).fetchone()
        if not row: raise ValueError('未找到告警事件')
        name=json.loads(row[0]).get('screenshot','')
        if not name or Path(name).name != name or not name.endswith('.png'):
            raise ValueError('此告警没有可用截图')
        config,_=self.read_config()
        directory=(self.root/config.get('screenshot_dir','screenshots')).resolve()
        path=(directory/name).resolve()
        if path.parent != directory: raise ValueError('截图路径无效')
        return path.read_bytes()

    def frame(self, stream_id):
        with self.lock:
            if not self.process or self.process.poll() is not None: raise ValueError('未运行，画面已停止')
            config,_ = self.read_config()
            if stream_id not in [s['id'] for s in config['streams']]: raise ValueError('通道不存在')
            directory, run = self.frames, f'{self.session}-{self.run_id}'
        for _ in range(3):
            try:
                path = directory/f's{stream_id}.json'
                raw = path.read_bytes()
                data = json.loads(raw)
                name = data['image']
                if Path(name).name != name or not name.endswith('.png'): raise ValueError('预览文件名无效')
                image = (directory/name).read_bytes()
                data['age_ms'] = max(0,int((time.time()-path.stat().st_mtime)*1000))
                data['run'] = run
                data['image'] = 'data:image/png;base64,'+base64.b64encode(image).decode('ascii')
                return data
            except (FileNotFoundError, json.JSONDecodeError): continue
        raise ValueError('正在等待同一帧画面与检测结果；若持续无画面，请查看日志')

    def close(self):
        self.stop()
        if self.receiver:
            self.receiver.shutdown(); self.receiver.server_close()
            self.receiver_thread.join(timeout=3)
        self.temp.cleanup()


class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args): pass

    def send(self, status, data, kind='application/json; charset=utf-8'):
        if not isinstance(data,bytes): data = json.dumps(data,ensure_ascii=False,allow_nan=False).encode()
        self.send_response(status)
        for key,value in [('Content-Type',kind),('Content-Length',str(len(data))),('Cache-Control','no-store'),
                          ('X-Content-Type-Options','nosniff'),('X-Frame-Options','DENY'),
                          ('Content-Security-Policy',"default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; frame-ancestors 'none'; base-uri 'none'")]:
            self.send_header(key,value)
        self.end_headers()
        try: self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError): pass

    def valid_host(self):
        try:
            hostname=urlsplit('http://'+self.headers.get('Host','')).hostname
            if hostname != 'localhost': ipaddress.ip_address(hostname)
            return True
        except (ValueError,TypeError): return False

    def do_GET(self):
        if not self.valid_host(): self.send(403,{'error':'请使用 localhost 或 IP 地址访问'}); return
        target = urlsplit(self.path)
        controller = self.server.controller
        try:
            if target.path in ('/','/app.js','/app.css'):
                name = 'index.html' if target.path == '/' else target.path[1:]
                kind = {'index.html':'text/html; charset=utf-8','app.js':'text/javascript; charset=utf-8','app.css':'text/css; charset=utf-8'}[name]
                self.send(200,(ASSETS/name).read_bytes(),kind)
            elif target.path == '/api/config': self.send(200,controller.config_response())
            elif target.path == '/api/status': self.send(200,controller.status())
            elif target.path == '/api/frame': self.send(200,controller.frame(int(parse_qs(target.query).get('stream',['0'])[0])))
            elif target.path == '/api/shot': self.send(200,controller.shot(parse_qs(target.query).get('event',[''])[0]),'image/png')
            else: self.send(404,{'error':'路径不存在'})
        except (ValueError,OSError,KeyError,sqlite3.Error) as error: self.send(409,{'error':str(error)})

    def do_POST(self):
        controller = self.server.controller
        origin = self.headers.get('Origin')
        if (not self.valid_host() or self.headers.get('X-Editor-Token') != controller.token
                or (origin and urlsplit(origin).netloc != self.headers.get('Host'))):
            self.send(403,{'error':'请在本服务页面操作，拒绝跨站请求'}); return
        try:
            size = int(self.headers.get('Content-Length','0'))
            if not 0 < size <= 1024*1024: raise ValueError('请求体大小无效')
            body = json.loads(self.rfile.read(size))
            if not isinstance(body,dict): raise ValueError('请求格式错误')
            if self.path == '/api/start': result = controller.start(body.get('mode'))
            elif self.path == '/api/stop': result = controller.stop()
            elif self.path == '/api/save': result = controller.save(body)
            else: self.send(404,{'error':'路径不存在'}); return
            self.send(200,result)
        except (ValueError,OSError,KeyError,TypeError) as error: self.send(409,{'error':str(error)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-root',type=Path,default=ROOT)
    parser.add_argument('--config',type=Path,required=True)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--host',choices=['127.0.0.1','0.0.0.0'],default='127.0.0.1')
    parser.add_argument('--port',type=int,default=8081)
    args = parser.parse_args()
    controller = Controller(args.project_root,args.config,args.binary)
    try:
        with ThreadingHTTPServer((args.host,args.port),Handler) as server:
            server.controller = controller
            print(f'统一控制台已启动，当前空闲，摄像头未开启。板卡端口 {args.port}。',flush=True)
            print(f'USB：电脑转发 18081→{args.port} 后，浏览器打开 http://127.0.0.1:18081/ 。',flush=True)
            try: server.serve_forever(poll_interval=.2)
            except KeyboardInterrupt: pass
    finally: controller.close()


if __name__ == '__main__': main()
