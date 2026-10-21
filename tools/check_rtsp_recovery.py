"""Board-only RTSP restart and stop test; synthetic stream, existing NPU model."""
import json,subprocess,time,signal
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

def main():
    out=ROOT/'evidence'/time.strftime('rtsp-recovery-%Y%m%d-%H%M%S');out.mkdir()
    result={'completed':False};server=demo=None
    def stop(p):
        if p is not None and p.poll() is None:
            p.terminate()
            try:p.wait(5)
            except subprocess.TimeoutExpired:p.kill();p.wait(5)
    def wait_text(path,text,seconds):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if text in path.read_text(errors='replace'):return
            if demo is not None and demo.poll() is not None:raise RuntimeError('pipeline exited early')
            time.sleep(.1)
        raise TimeoutError(text)
    def launch(log):
        return subprocess.Popen(['/usr/bin/python3','-u','tools/rtsp_fixture.py'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
    print(out,flush=True)
    try:
        cfg=json.loads((ROOT/'evidence/rtsp_loopback/config.json').read_text())
        cfg['run_seconds']=120;cfg['screenshot_dir']=str(out/'shots')
        cfg['alarm']['outbox_dir']=str(out/'outbox')
        config=out/'config.json';config.write_text(json.dumps(cfg))
        with (out/'fixture.log').open('w') as fixture, (out/'run.log').open('w') as log:
            server=launch(fixture);wait_text(out/'fixture.log','READY',10)
            demo=subprocess.Popen([str(ROOT/'build/multi_stream_alarm_demo'),str(config)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
            wait_text(out/'run.log','pipeline started',20);time.sleep(3)
            stop(server);time.sleep(2)
            server=launch(fixture)
            wait_text(out/'run.log','reconnected; frames resumed',35)
            result['resumed_after_restart']=True
            time.sleep(2);stop(server)
            begin=time.monotonic();demo.send_signal(signal.SIGTERM);code=demo.wait(timeout=5)
            result.update(exit_code=code,stop_seconds=time.monotonic()-begin)
            assert code==0 and result['stop_seconds']<5
            result['completed']=True
    finally:
        stop(demo);stop(server)
        (out/'result.json').write_text(json.dumps(result,indent=2))
        print(json.dumps(result),flush=True)

if __name__=='__main__':main()
