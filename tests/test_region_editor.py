import copy
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from urllib.request import Request, urlopen
from urllib.error import HTTPError
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from region_editor import Store, validate_polygon, merge_regions, capture_command, Handler, ThreadingHTTPServer


class RegionEditorTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.path=Path(self.temp.name)/'config.json'
        self.cfg={'hardware':True,'frame_width':640,'frame_height':640,'capture_width':640,'capture_height':480,'camera_flip_180':True,'model_path':'keep.rknn','alarm':{'port':9000},'streams':[{'id':7,'source_uri':'/dev/video0','regions':[{'id':3,'polygon':[[10,10],[100,10],[100,100],[10,100]],'classes':[0],'cooldown_ms':5000}]},{'id':8,'source_uri':'video.mp4','regions':[]}]}
        self.path.write_text(json.dumps(self.cfg),encoding='utf-8')
        self.store=Store(self.path)
    def tearDown(self): self.temp.cleanup()
    def test_preserves_other_settings_and_backup(self):
        data,rev=self.store.read(); original=self.path.read_bytes()
        data['streams'][0]['regions'][0]['classes']=[79]
        self.store.save(rev,data['streams'])
        saved,_=self.store.read()
        self.assertEqual(saved['model_path'],'keep.rknn');self.assertEqual(saved['alarm'],self.cfg['alarm'])
        self.assertEqual(saved['streams'][0]['source_uri'],'/dev/video0')
        self.assertEqual(saved['streams'][1],self.cfg['streams'][1])
        self.assertEqual(self.path.with_name('config.json.before-region-edit.bak').read_bytes(),original)
        self.assertEqual(saved['streams'][0]['regions'][0]['classes'],[79])
    def test_stale_revision_does_not_overwrite(self):
        data,rev=self.store.read();self.path.write_text(json.dumps({**self.cfg,'fps':12}))
        before=self.path.read_bytes()
        with self.assertRaises(ValueError):self.store.save(rev,data['streams'])
        self.assertEqual(self.path.read_bytes(),before)
    def test_invalid_polygons(self):
        for points in ([[0,0],[1,1]],[[0,0],[10,10],[0,10],[10,0]],[[0,0],[10,0],[20,0]],[[0,0],[0,0],[10,10]],[[0,0],[641,0],[0,1]],[[0,0],[float('nan'),1],[1,0]],[[0,0],[10,0],[5,0],[5,5]]):
            with self.subTest(points=points),self.assertRaises(ValueError):validate_polygon(points,640,640)
    def test_concave_and_frame_boundary(self):
        validate_polygon([[0,0],[640,0],[640,640],[300,300],[0,640]],640,640)
    def test_bad_region_settings(self):
        for field,value in [('id',-1),('classes',[80]),('classes',[True]),('cooldown_ms',-1),('cooldown_ms',1.5)]:
            data=copy.deepcopy(self.cfg['streams']);data[0]['regions'][0][field]=value
            with self.subTest(field=field,value=value),self.assertRaises(ValueError):merge_regions(self.cfg,data)
        data=copy.deepcopy(self.cfg['streams']);data[0]['regions']*=2
        with self.assertRaises(ValueError):merge_regions(self.cfg,data)
    def test_delete_all_and_mock_domain(self):
        data=copy.deepcopy(self.cfg['streams']);data[0]['regions']=[]
        self.assertEqual(merge_regions(self.cfg,data)['streams'][0]['regions'],[])
        data=copy.deepcopy(self.cfg['streams']);data[0]['regions'][0]['classes']=[5]
        with self.assertRaises(ValueError):merge_regions({**self.cfg,'hardware':False},data)
    def test_capture_is_configured_source_and_transform(self):
        cmd=capture_command(self.cfg,0)
        self.assertEqual(cmd[cmd.index('-i')+1],'/dev/video0')
        self.assertIn('hflip,vflip,scale',cmd[cmd.index('-vf')+1])
        self.assertIn('pad=640:640',cmd[cmd.index('-vf')+1])
        with self.assertRaises(ValueError):capture_command({**self.cfg,'hardware':False},0)
    def test_http_save_auth_and_capture_off(self):
        srv=ThreadingHTTPServer(('127.0.0.1',0),Handler);srv.store=self.store;srv.token='test';srv.capture=False;srv.capture_lock=threading.Lock()
        thread=threading.Thread(target=srv.serve_forever);thread.start()
        base=f'http://127.0.0.1:{srv.server_port}'
        try:
            with urlopen(base+'/api/config') as r:data=json.load(r)
            body=json.dumps({'revision':data['revision'],'streams':data['config']['streams']}).encode()
            with self.assertRaises(HTTPError) as e:urlopen(Request(base+'/api/save',body))
            self.assertEqual(e.exception.code,403)
            req=Request(base+'/api/save',body,{'X-Editor-Token':'test','Content-Type':'application/json'})
            with urlopen(req) as r:self.assertIn('下次',json.load(r)['message'])
            with self.assertRaises(HTTPError) as e:urlopen(Request(base+'/api/capture',b'{"index":0}',{'X-Editor-Token':'test'}))
            self.assertEqual(e.exception.code,400)
            with self.assertRaises(HTTPError) as e:urlopen(Request(base+'/api/save',body,{'X-Editor-Token':'test','Origin':'https://evil.invalid'}))
            self.assertEqual(e.exception.code,403)
        finally:srv.shutdown();thread.join();srv.server_close()

if __name__=='__main__':unittest.main()
