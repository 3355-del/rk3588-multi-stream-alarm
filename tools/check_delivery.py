"""Black-box tests: persistence across restart, ACK loss, fragmentation, dedup, ownership."""
import importlib.util,json,socket,sqlite3,subprocess,sys,tempfile,threading,time
from pathlib import Path
from contextlib import closing
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('receiver',ROOT/'tools/alarm_receiver.py');rx=importlib.util.module_from_spec(spec);spec.loader.exec_module(rx)
def waitfor(test,seconds=6):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if test():return
        time.sleep(.05)
    raise AssertionError('condition timed out')
def run(driver):
    with tempfile.TemporaryDirectory(prefix='delivery-') as temp:
        p=Path(temp);db=p/'alarms.sqlite3';rx.initialize(db)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0));port=listener.getsockname()[1]
            # Bound but not listening: sender must persist even without a receiver.
            first=subprocess.run([str(driver),str(p/'outbox'),str(port),'2','100'],capture_output=True,timeout=5)
            assert first.returncode==0,first.stderr
            assert len(list((p/'outbox').glob('*.json')))==2
            listener.listen();listener.settimeout(8)
            errors=[];seen=[]
            def serve():
                try:
                    while len(seen)<3:
                        client,_=listener.accept()
                        with client:
                            client.settimeout(3)
                            while len(seen)<3:
                                header=rx.receive_exact(client,4)
                                if header is None:break
                                event=json.loads(rx.receive_exact(client,int.from_bytes(header,'big')))
                                rx.store(db,event);seen.append(event['event_id'])
                                if len(seen)==1:break # Commit succeeded, ACK lost, force replay.
                                ack=json.dumps({'type':'ack','event_id':event['event_id']}).encode();packet=len(ack).to_bytes(4,'big')+ack
                                for b in packet:client.sendall(bytes([b])) # fragmented ACK
                except Exception as exc:errors.append(exc)
            thread=threading.Thread(target=serve,daemon=True);thread.start()
            proc=subprocess.Popen([str(driver),str(p/'outbox'),str(port),'0','5000'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                # No new alarms; startup alone must drain old records.
                waitfor(lambda:len(list((p/'outbox').glob('*.json')))==0)
                assert seen[0]==seen[1] and len(set(seen))==2,seen
                with closing(sqlite3.connect(db)) as con:assert con.execute('SELECT COUNT(*) FROM alarms').fetchone()[0]==2
                duplicate=subprocess.run([str(driver),str(p/'outbox'),str(port),'0','1'],capture_output=True,timeout=3)
                assert duplicate.returncode!=0 and b'outbox' in duplicate.stderr
                assert proc.wait(timeout=6)==0
            finally:
                if proc.poll() is None:proc.terminate();proc.wait(timeout=3)
                proc.stdout.close(); proc.stderr.close()
                thread.join(timeout=4)
            thread.join(timeout=2);assert not errors,errors
        # Malformed/colliding IDs must never overwrite an existing received event.
        event={'type':'alarm','event_id':'same','stream_id':1};assert rx.store(db,event);assert not rx.store(db,event)
        try:rx.store(db,dict(event,stream_id=2))
        except ValueError:pass
        else:raise AssertionError('ID collision accepted')
    print('PASS: restart replay without new frames, lost ACK retry, fragmented ACK, SQLite dedup, exclusive owner, collision rejection')
if __name__=='__main__':run(Path(sys.argv[1]).resolve())
