"""Score independently reviewed annotations against full-frame predictions.

JSONL rows: frame, reviewed (labels only), boxes [{class_id, xywh, score?}].
Coordinates must use the same 640x640 letterboxed space as motion_eval.
This reports precision/recall at a fixed confidence and IoU, not mAP.
"""
import argparse
import json
import math
from pathlib import Path


def load_rows(path, labels=False):
    frames={}
    for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
        if not line.strip():continue
        row=json.loads(line);frame=row['frame']
        if type(frame) is not int or frame<0 or frame in frames:raise ValueError('Invalid or duplicate frame')
        if labels and row.get('reviewed') is not True:raise ValueError('Unreviewed labels cannot be scored')
        boxes=row['boxes']
        if not isinstance(boxes,list):raise ValueError('boxes must be a list, including empty frames')
        for box in boxes:
            if type(box['class_id']) is not int or box['class_id']<0:raise ValueError('Invalid class_id')
            xywh=box['xywh']
            if len(xywh)!=4 or any(type(x) not in (int,float) or not math.isfinite(x) for x in xywh) or min(xywh[2:])<=0:
                raise ValueError('Invalid xywh box')
            if not labels:
                score=box['score']
                if type(score) not in (int,float) or not math.isfinite(score) or not 0<=score<=1:raise ValueError('Invalid score')
        frames[frame]=boxes
    if not frames:raise ValueError('Empty dataset')
    return frames


def iou(a,b):
    w=max(0,min(a[0]+a[2],b[0]+b[2])-max(a[0],b[0]))
    h=max(0,min(a[1]+a[3],b[1]+b[3])-max(a[1],b[1]))
    overlap=w*h
    return overlap/(a[2]*a[3]+b[2]*b[3]-overlap)


def score(labels,predictions,confidence=.25,threshold=.5):
    if labels.keys()!=predictions.keys():raise ValueError('Frame sets differ; explicitly supply empty prediction rows')
    counts={}
    for frame,gold in labels.items():
        used=set()
        for box in gold:counts.setdefault(box['class_id'],{'tp':0,'fp':0,'fn':0})
        for pred in sorted(predictions[frame],key=lambda b:b['score'],reverse=True):
            if pred['score']<confidence:continue
            category=pred['class_id'];c=counts.setdefault(category,{'tp':0,'fp':0,'fn':0})
            candidates=[(iou(pred['xywh'],b['xywh']),i) for i,b in enumerate(gold) if i not in used and b['class_id']==category]
            best,index=max(candidates,default=(0,-1))
            if index>=0 and best>=threshold:c['tp']+=1;used.add(index)
            else:c['fp']+=1
        for i,box in enumerate(gold):
            if i not in used:counts[box['class_id']]['fn']+=1
    def metrics(c):
        return dict(c,precision=c['tp']/(c['tp']+c['fp']) if c['tp']+c['fp'] else None,
                    recall=c['tp']/(c['tp']+c['fn']) if c['tp']+c['fn'] else None)
    total={k:sum(v[k] for v in counts.values()) for k in ('tp','fp','fn')}
    return {'frames':len(labels),'confidence':confidence,'iou_threshold':threshold,'total':metrics(total),
            'classes':{str(k):metrics(v) for k,v in counts.items()},'note':'Fixed-threshold precision/recall, not mAP.'}


def main():
    p=argparse.ArgumentParser();p.add_argument('--labels',required=True);p.add_argument('--predictions',required=True)
    p.add_argument('--output',required=True);p.add_argument('--confidence',type=float,default=.25);p.add_argument('--iou',type=float,default=.5)
    a=p.parse_args()
    if not 0<=a.confidence<=1 or not 0<a.iou<=1:p.error('thresholds out of range')
    result=score(load_rows(a.labels,True),load_rows(a.predictions),a.confidence,a.iou)
    with Path(a.output).open('x',encoding='utf8') as f:json.dump(result,f,ensure_ascii=False,indent=2)
    print(json.dumps(result,ensure_ascii=False,indent=2))

if __name__=='__main__':main()
