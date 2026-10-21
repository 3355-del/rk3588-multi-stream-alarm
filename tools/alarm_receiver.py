"""SQLite commit precedes length-prefixed ACK. Same event_id is stored once."""
import argparse,json,socketserver,sqlite3
from contextlib import closing
from pathlib import Path

def receive_exact(client,size,*,idle_ok=False):
    data=bytearray()
    while len(data)<size:
        try:
            chunk=client.recv(size-len(data))
        except TimeoutError:
            if idle_ok and not data:return None
            raise
        if not chunk:
            if not data:return None
            raise EOFError('truncated packet')
        data.extend(chunk)
    return bytes(data)

def initialize(database):
    Path(database).parent.mkdir(parents=True,exist_ok=True)
    with closing(sqlite3.connect(database,timeout=10)) as db, db:
        db.execute('PRAGMA journal_mode=WAL')
        db.execute('PRAGMA synchronous=FULL')
        db.execute('CREATE TABLE IF NOT EXISTS alarms(id INTEGER PRIMARY KEY,event_id TEXT UNIQUE,payload TEXT NOT NULL,received_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)')

def store(database,event):
    if not isinstance(event,dict) or event.get('type')!='alarm':raise ValueError('invalid alarm')
    eid=event.get('event_id')
    if eid is not None and (not isinstance(eid,str) or not 1<=len(eid)<=128):raise ValueError('invalid event_id')
    payload=json.dumps(event,ensure_ascii=False,sort_keys=True,separators=(',',':'),allow_nan=False)
    with closing(sqlite3.connect(database,timeout=10)) as db, db:
        db.execute('PRAGMA synchronous=FULL')
        db.execute('BEGIN IMMEDIATE')
        prior=db.execute('SELECT payload FROM alarms WHERE event_id=?',(eid,)).fetchone() if eid else None
        if prior and prior[0]!=payload:raise ValueError('event_id reused for a different payload')
        inserted=db.execute('INSERT OR IGNORE INTO alarms(event_id,payload) VALUES (?,?)',(eid,payload)).rowcount
    return bool(inserted)

class AlarmServer(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(5)
        try:
            while (header:=receive_exact(self.request,4,idle_ok=True)) is not None:
                length=int.from_bytes(header,'big')
                if not 0<length<=1024*1024:raise ValueError('invalid length')
                body=receive_exact(self.request,length)
                if body is None:raise EOFError('missing body')
                event=json.loads(body);inserted=store(self.server.database,event)
                if event.get('event_id'):
                    ack=json.dumps({'type':'ack','event_id':event['event_id']}).encode()
                    self.request.sendall(len(ack).to_bytes(4,'big')+ack)
                if inserted:print(json.dumps(event,ensure_ascii=False),flush=True)
        except (OSError,ValueError,EOFError,sqlite3.Error) as exc:print(f'Connection ended without ACK: {exc}',flush=True)
def main():
    p=argparse.ArgumentParser();p.add_argument('--host',default='127.0.0.1');p.add_argument('--port',type=int,default=9000);p.add_argument('--db',default='data/alarms.sqlite3');a=p.parse_args();initialize(a.db)
    with AlarmServer((a.host,a.port),Handler) as server:
        server.database=str(Path(a.db).resolve());print(f'Listening on {a.host}:{server.server_address[1]}, database={server.database}',flush=True);server.serve_forever(poll_interval=.2)
if __name__=='__main__':
    try:main()
    except KeyboardInterrupt:pass
