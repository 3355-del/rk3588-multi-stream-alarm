import importlib.util
import tempfile
import unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('score',Path(__file__).resolve().parents[1]/'tools/score_detections.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

def box(category=0,score=1):return {'class_id':category,'xywh':[0,0,10,10],'score':score}

class Scores(unittest.TestCase):
    def test_duplicates_do_not_count_as_two_true_positives(self):
        r=module.score({0:[box()]},{0:[box(),box(score=.8)]})['total']
        self.assertEqual((r['tp'],r['fp'],r['fn']),(1,1,0));self.assertEqual(r['precision'],.5)
    def test_wrong_class_and_empty_frame(self):
        r=module.score({0:[box()],1:[]},{0:[box(1)],1:[box()]})['total']
        self.assertEqual((r['tp'],r['fp'],r['fn']),(0,2,1))
    def test_missing_frame_is_not_silently_ignored(self):
        with self.assertRaises(ValueError):module.score({0:[box()]},{})
    def test_unreviewed_annotations_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'labels.jsonl';p.write_text('{"frame":0,"boxes":[],"reviewed":false}')
            with self.assertRaises(ValueError):module.load_rows(p,True)
    def test_low_score_becomes_miss(self):
        r=module.score({0:[box()]},{0:[box(score=.1)]})['total']
        self.assertEqual(r['fn'],1);self.assertIsNone(r['precision'])
