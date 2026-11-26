#!/usr/bin/env python3
import importlib.util
import pathlib
import struct
import unittest
spec=importlib.util.spec_from_file_location('pipeline', pathlib.Path(__file__).with_name('decode-pipeline.py'))
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)

def capture(rows, *, count=None, overflow=0, dropped=0, begin=1, end=10000000):
    n=len(rows)
    header=p.HEADER.pack(b'MRPL001\0',1,80,max(1,n),42,1000000,1,0,1,begin,end,
                         n if count is None else count,overflow,dropped,0,*([0]*5))
    return header+b''.join(p.RECORD.pack(*r) for r in rows)+bytes(80 if not n else 0)

def row(ticket,qpc,kind,phase=0,ident=0,*,obj=100,tid=7,a=0,b=0,c=0,d=0,status=0):
    return [qpc,obj,ident,a,b,c,d,tid,kind,phase,status,ticket,0]

class DecodeTests(unittest.TestCase):
    def test_pair(self):
        data=capture([row(1,100,6,1,17),row(2,150,6,2,1)])
        result=p.decode(data)
        self.assertEqual(result['pair_summary']['Execute']['total_ms'],0.05)
        self.assertEqual(result['open_pairs'],[])
        self.assertEqual(result['orphan_leaves'],[])
        self.assertIn('NOT_ENABLED',' '.join(result['limitations']))
    def test_commit_retire_release_clock(self):
        rows=[row(1,100,8,1,9,a=456),row(2,101,8,2,1),
              row(3,110,9,1,9,a=456),row(4,300,9,2,3,status=4),
              row(5,301,24,ident=9,a=120000,b=280000,c=456,status=4),
              row(6,310,10,1,9,a=888),row(7,312,10,2,6)]
        result=p.decode(capture(rows))
        self.assertEqual(result['gpu'][0]['gpu_ms'],0.16)
        self.assertEqual(result['gpu'][0]['retire_to_release_ms'],0.01)
        self.assertEqual(result['clock_bridge']['status'],'BOUNDED')
        self.assertLess(result['clock_bridge']['lower_ns'],result['clock_bridge']['upper_ns'])
    def test_torn_and_dropped(self):
        r=row(0,200,4)
        result=p.decode(capture([r],overflow=1,dropped=9))
        self.assertEqual(result['incomplete'],[1]);self.assertEqual(result['status'],'DROPPED')
    def test_unarmed_empty(self):
        self.assertEqual(p.decode(capture([],begin=0,end=0))['status'],'NOT_ENABLED')
        self.assertEqual(p.decode(capture([]))['status'],'EMPTY')
    def test_reject_header(self):
        good=capture([])
        for data in [good[1:],b'BADMAGIC'+good[8:],good+b'x',capture([],count=2)]:
            with self.assertRaises(ValueError):p.decode(data)
    def test_reject_pairs(self):
        for end in [row(2,90,6,2,1),row(2,110,6,2,1,tid=9),row(2,110,8,2,1)]:
            with self.assertRaises(ValueError):p.decode(capture([row(1,100,6,1),end]))
    def test_reject_duplicate_end(self):
        with self.assertRaises(ValueError):p.decode(capture([row(1,100,6,1),row(2,110,6,2,1),row(3,120,6,2,1)]))
    def test_frames_keep_unknown(self):
        r=p.decode(capture([row(1,100,4,1,5),row(2,110,4,2,1),row(3,200,4,1,6),row(4,210,4,2,3)]))
        self.assertEqual(r['frames'][0]['frame_ms'],0.1)
        self.assertEqual(r['frames'][0]['cpu_ms'],'UNKNOWN')
        self.assertEqual(r['frames'][0]['game_latency_wait_ms'],'NOT_ENABLED')
    def test_union(self):
        self.assertEqual(p.union_ns([(1,8),(3,10),(20,25)]),14)
    def test_phase_markers_do_not_invent_waits(self):
        rows=[row(1,100,4,1,5,a=700), row(2,110,5,1,700,obj=500),
              row(3,120,8,1,1,obj=500,a=456,b=700),row(4,130,8,2,3,obj=500),
              row(5,140,5,2,2,obj=500),row(6,150,4,2,1),
              row(7,160,9,1,1,obj=500,a=456),row(8,180,26,1,obj=800),
              row(9,185,26,2,8,obj=800),row(10,300,9,2,7,obj=500,status=4),
              row(11,301,24,ident=1,obj=500,a=150000,b=290000,c=456,d=700,status=4),
              row(12,310,10,1,1,obj=500,a=888),row(13,311,10,2,12,obj=500),
              row(14,400,4,1,6,a=700),row(15,410,4,2,14)]
        result=p.decode(capture(rows))
        f=result['frames'][0]
        self.assertEqual((f['p_qpc'],f['s_qpc'],f['f_qpc']),(150,310,180))
        self.assertEqual(f['s_to_f_ms'],-0.13)
        self.assertEqual(f['f_vs_gpu_n_end'],'BEFORE_GPU_END')
        self.assertEqual(f['game_latency_wait_ms'],'NOT_ENABLED')
        rows[10][5]=999 # GPU record now claims a different command buffer.
        self.assertEqual(p.decode(capture(rows))['gpu'],[])

if __name__=='__main__':unittest.main()
