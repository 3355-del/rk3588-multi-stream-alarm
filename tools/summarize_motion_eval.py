"""Summarize offline decoder/preprocessor/inference timing, not alarm latency."""
import argparse,csv,json,math
from pathlib import Path

def main():
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args()
    with (a.directory/'latency.csv').open() as f:rows=list(csv.DictReader(f))
    if not rows:raise ValueError('No successful frames')
    summary={'frames':len(rows),'scope':'Offline sequential pipeline, excluding screenshot/file output; total includes CPU pixel-copy/hash audit. Not capture-to-alarm latency.'}
    for name in ('capture_ms','preprocess_ms','infer_nms_ms','total_with_audit_ms'):
        values=sorted(float(r[name]) for r in rows)
        if any(not math.isfinite(v) or v<0 for v in values):raise ValueError('Invalid timing')
        summary[name]={'p50':values[math.ceil(.5*len(values))-1],'p95':values[math.ceil(.95*len(values))-1],'max':values[-1]}
    with (a.directory/'latency-summary.json').open('x') as f:json.dump(summary,f,indent=2)
    print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
