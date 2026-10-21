"""Pipeline -> durable outbox -> actual receiver -> project-two read-only tool."""
import sys,json,socket,subprocess,tempfile,threading,time
from pathlib import Path
from check_delivery import rx,waitfor
ROOT=Path(__file__).resolve().parents[1]
def main(build):
    with tempfile.TemporaryDirectory(prefix='projects-e2e-') as tmp:
        p=Path(tmp);outbox=p/'outbox';db=p/'alarms.sqlite3'
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1',0));port=reserved.getsockname()[1]
            c={'frame_width':32,'frame_height':24,'fps':10,'run_seconds':1,'screenshot_dir':str(p/'shots'),'alarm':{'host':'127.0.0.1','port':port,'reliable':True,'outbox_dir':str(outbox)},'streams':[{'id':8,'regions':[{'id':4,'cooldown_ms':500,'polygon':[[0,0],[32,0],[32,24],[0,24]]}]}]}
            conf=p/'pipeline.json';conf.write_text(json.dumps(c))
            exe=build/('multi_stream_alarm_demo.exe' if sys.platform=='win32' else 'multi_stream_alarm_demo')
            result=subprocess.run([str(exe),str(conf)],cwd=p,capture_output=True,timeout=10)
            assert result.returncode==0,result.stderr
            pending=list(outbox.glob('*.json'));assert pending,'no persisted alarms'
            for file in pending:
                event=json.loads(file.read_text());assert (p/'shots'/event['screenshot']).is_file()
        rx.initialize(db)
        with rx.AlarmServer(('127.0.0.1',port),rx.Handler) as server:
            server.database=str(db);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                driver=build/('delivery_driver.exe' if sys.platform=='win32' else 'delivery_driver')
                result=subprocess.run([str(driver),str(outbox),str(port),'0','1500'],capture_output=True,timeout=6)
                assert result.returncode==0,result.stderr
                waitfor(lambda:not list(outbox.glob('*.json')))
                project2=ROOT.parent/'项目-基于 RK3588-RKLLM 的离线端侧多模态智能体'
                if project2.is_dir():
                    config=json.loads((project2/'config/default.json').read_text(encoding='utf8'));config['alarm_db']=str(db);config['input_dir']=str(p);config['asr_model']=str(p/'models');cf=p/'agent.json';cf.write_text(json.dumps(config),encoding='utf8')
                    answer=subprocess.run([sys.executable,'-X','utf8','-m','edge_agent','--config',str(cf),'alarms','--limit','20'],cwd=project2,capture_output=True,timeout=5)
                    assert answer.returncode==0,answer.stderr
                    events=json.loads(answer.stdout);assert len(events['alarms'])==len(pending);assert all(e['event']['stream_id']==8 for e in events['alarms'])
                    print('PASS: project-two reads actual project-one committed alarms')
                else:
                    print('SKIP: project-two checkout missing; cross-project read was not tested')
            finally:server.shutdown();thread.join()
        print('PASS: reliable pipeline stores screenshots, drains normal shutdown and replays via actual ACK receiver')
if __name__=='__main__':main(Path(sys.argv[1]).resolve())
