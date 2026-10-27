import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from live_console import Controller, Handler, ThreadingHTTPServer, rotate_rules


class LiveConsoleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='live-test-')
        self.root = Path(self.temp.name)
        self.config = {'frame_width':320,'frame_height':240,'fps':12,'run_seconds':2,
                       'hardware':False,'screenshot_dir':'shots','queue_capacity':2,
                       'alarm_queue_capacity':4,'alarm':{'reliable':True,'port':9000},
                       'streams':[{'id':7,'source_uri':'mock://test','regions':[{
                           'id':2,'polygon':[[0,0],[320,0],[320,240],[0,240]],
                           'classes':[],'cooldown_ms':1000}]}]}
        self.source = self.root/'original.json'
        self.source.write_text(json.dumps(self.config))
        self.original = self.source.read_bytes()
        self.controller = Controller(self.root,self.source,os.environ.get('MSAD_LIVE_BINARY',self.root/'missing'))

    def tearDown(self):
        self.controller.close(); self.temp.cleanup()

    def test_idle_does_not_start_capture_or_receiver(self):
        self.assertIsNone(self.controller.process)
        self.assertIsNone(self.controller.receiver)
        self.assertFalse(self.controller.status()['running'])
        self.assertEqual(self.source.read_bytes(),self.original)

    def test_rotation_preserves_physical_regions_and_original(self):
        config = copy.deepcopy(self.config); config.update(hardware=True,camera_flip_180=True)
        a = copy.deepcopy(config); rotate_rules(a); rotate_rules(a)
        self.assertEqual(a,config)
        response=self.controller.config_response()
        data=self.controller.save({'revision':response['revision'],'streams':response['config']['streams'],'camera_flip_180':True})
        self.assertEqual(data['config']['streams'][0]['regions'][0]['polygon'][0],[320,240])
        self.assertEqual(self.source.read_bytes(),self.original)

    def test_reject_stale_revision_and_invalid_polygon(self):
        response=self.controller.config_response()
        with self.assertRaises(ValueError):self.controller.save({'revision':'old','streams':response['config']['streams']})
        response['config']['streams'][0]['regions'][0]['polygon']=[[0,0],[100,100],[0,100],[100,0]]
        with self.assertRaises(ValueError):self.controller.save({'revision':response['revision'],'streams':response['config']['streams']})
        self.assertEqual(self.source.read_bytes(),self.original)

    def test_missing_binary_is_explicit_and_no_process(self):
        self.controller.binary=self.root/'nonexistent'
        with self.assertRaisesRegex(ValueError,'未找到新程序'):self.controller.start('preview')
        self.assertFalse(self.controller.status()['running'])

    def test_receiver_rejects_invalid_saved_endpoint_without_deleting_outbox(self):
        destination=self.root/'alarm_outbox_live/destination.txt'
        destination.parent.mkdir()
        destination.write_text('remote.example:9000')
        with self.assertRaisesRegex(ValueError,'补发队列接收地址无效'):self.controller._receiver()
        self.assertEqual(destination.read_text(),'remote.example:9000')
        self.assertIsNone(self.controller.receiver)

    def test_http_rejects_unauthorized_cross_origin_and_path_escape(self):
        with ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
            server.controller=self.controller
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            url=f'http://127.0.0.1:{server.server_port}'
            try:
                for headers in ({},{'X-Editor-Token':self.controller.token,'Origin':'http://wrong.example'}):
                    with self.assertRaises(HTTPError) as error:urlopen(Request(url+'/api/start',data=b'{"mode":"preview"}',headers=headers))
                    self.assertEqual(error.exception.code,403)
                with self.assertRaises(HTTPError) as error:urlopen(url+'/../original.json')
                self.assertEqual(error.exception.code,404)
                self.assertFalse(self.controller.status()['running'])
            finally:server.shutdown();thread.join()

    @unittest.skipUnless(os.environ.get('MSAD_LIVE_BINARY'),'set MSAD_LIVE_BINARY to run real mock pipeline')
    def test_pipeline_preview_detect_alarm_apply_and_bounded_cache(self):
        def await_frame(boxes=False):
            end=time.monotonic()+12
            while time.monotonic()<end:
                try:
                    frame=self.controller.frame(7)
                    if not boxes or frame['detections']:return frame
                except ValueError:pass
                time.sleep(.08)
            self.fail('preview did not publish a frame: '+str(self.controller.status()))
        self.controller.start('preview')
        preview=await_frame()
        self.assertTrue(preview['image'].startswith('data:image/png;base64,iVBOR'))
        self.assertEqual(preview['detections'],[])
        self.assertIsNone(self.controller.receiver)
        self.controller.start('detect')
        detected=await_frame(True)
        self.assertNotEqual(preview['run'],detected['run'])
        self.assertEqual((detected['width'],detected['height']),(320,240))
        end=time.monotonic()+8
        while not self.controller.alarms() and time.monotonic()<end:time.sleep(.1)
        self.assertTrue(self.controller.alarms(),'actual TCP/SQLite alarm missing')
        self.assertTrue(list((self.root/'shots').glob('*.png')),'alarm PNG missing')
        self.assertTrue(self.controller.shot(self.controller.alarms()[0]['event_id']).startswith(b'\x89PNG'))
        with self.assertRaises(ValueError):self.controller.shot('../missing.png')
        response=self.controller.config_response()
        response['config']['streams'][0]['regions']=[]
        saved=self.controller.save({'revision':response['revision'],'streams':response['config']['streams']})
        self.assertFalse(saved['restart_error'])
        self.assertTrue(self.controller.status()['applied'])
        self.assertNotEqual(detected['run'],await_frame()['run'])
        self.assertEqual(len(list(Path(self.controller.temp.name).iterdir())),1)
        self.assertLessEqual(len(list(self.controller.frames.glob('*.png'))),4)
        port=self.controller.receiver.server_address[1]
        binary=self.controller.binary
        self.controller.close()
        self.controller=Controller(self.root,self.source,binary)
        self.controller.start('detect')
        self.assertEqual(self.controller.receiver.server_address[1],port)
        await_frame(True)
        self.assertTrue(self.controller.status()['ready'])
        self.controller.stop()
        self.assertFalse(self.controller.status()['running'])
        with self.assertRaises(ValueError):self.controller.frame(7)
        self.assertEqual(self.source.read_bytes(),self.original)


if __name__=='__main__':unittest.main()
