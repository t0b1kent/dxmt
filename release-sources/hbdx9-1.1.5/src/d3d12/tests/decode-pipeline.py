#!/usr/bin/env python3
"""MRPL001: preserve unknown waits; never label Reset..Close elapsed as CPU time."""
import argparse
import collections
import csv
import hashlib
import io
import json
import pathlib
import struct

HEADER = struct.Struct('<8s4I6Q4I5Q')
RECORD = struct.Struct('<7Q6I')
KINDS = dict(enumerate(('Swapchain LatencySet LatencyHandle Present QueuePresent Execute '
    'ExecuteList Commit Retire SemaphoreRelease QueueSignal QueueWait FenceCreate FenceDestroy '
    'FenceSignal FenceCompleted FenceCompletion FenceBranch ListReset ListClose ListDestroy '
    'QueueCapacity QueueIdentity GpuTimes NativeFence AllocatorReset').split(), 1))


def union_ns(intervals):
    total = 0
    start = end = None
    for a, b in sorted(intervals):
        if b < a:
            raise ValueError('reversed interval')
        if start is None:
            start, end = a, b
        elif a > end:
            total += end-start
            start, end = a, b
        else:
            end = max(end, b)
    return total + (0 if start is None else end-start)


def decode(data):
    if len(data) < HEADER.size:
        raise ValueError('short header')
    h = HEADER.unpack_from(data)
    (magic, version, size, capacity, pid, freq, anchor, wall, span, begin, end,
        count, overflow, dropped, clock_failed, *reserved) = h
    if magic != b'MRPL001\0' or version != 1 or size != RECORD.size:
        raise ValueError('unsupported MRPL001 header')
    if not 0 < capacity <= 131072 or not 0 < freq <= 1000000000 or count > capacity:
        raise ValueError('invalid capacity/frequency/count')
    if len(data) != HEADER.size + capacity * size or overflow not in (0, 1) or any(reserved):
        raise ValueError('invalid size/overflow/reserved')
    if bool(begin) != bool(end) or (begin and end < begin):
        raise ValueError('invalid window')
    records, incomplete = [], []
    for index in range(count):
        qpc, obj, ident, a, b, c, d, tid, kind, phase, status, committed, padding = RECORD.unpack_from(data, HEADER.size+index*size)
        if not committed:
            incomplete.append(index+1)
            continue
        if committed != index+1 or padding or kind not in KINDS or phase not in (0, 1, 2) or not qpc:
            raise ValueError(f'invalid committed slot {index+1}')
        records.append(dict(ticket=index+1, qpc=qpc, object=obj, id=ident, a=a, b=b, c=c, d=d,
                            tid=tid, kind=KINDS[kind], phase=phase, status=status))
    by_ticket = {r['ticket']: r for r in records}
    pairs, used, orphan = [], set(), []
    for r in records:
        if r['phase'] != 2:
            continue
        p = by_ticket.get(r['id'])
        if not p or p['phase'] != 1:
            orphan.append(r['ticket'])
            continue
        if p['ticket'] in used or any(p[k] != r[k] for k in ('object', 'tid', 'kind')) or r['qpc'] < p['qpc']:
            raise ValueError(f'invalid pair {r["ticket"]}')
        used.add(p['ticket'])
        pairs.append(dict(kind=p['kind'], object=p['object'], tid=p['tid'],
                          begin=p['qpc'], end=r['qpc'], status=r['status'],
                          enter_ticket=p['ticket'], leave_ticket=r['ticket'],
                          id=p['id'], a=p['a'], b=p['b'], ms=(r['qpc']-p['qpc'])*1000/freq))
    open_pairs = [r['ticket'] for r in records if r['phase'] == 1 and r['ticket'] not in used]
    kinds = collections.Counter(r['kind'] for r in records)
    summary = {}
    for kind in sorted({p['kind'] for p in pairs}):
        rows = [p for p in pairs if p['kind'] == kind]
        summary[kind] = dict(pairs=len(rows), total_ms=sum(p['ms'] for p in rows),
                             max_ms=max(p['ms'] for p in rows), unknown_results=sum(p['status']==0xffffffff for p in rows))
    commits = {(p['object'], p['id']): p for p in pairs if p['kind']=='Commit'}
    retires = {(p['object'], p['id']): p for p in pairs if p['kind']=='Retire'}
    releases = {(p['object'], p['id']): p for p in pairs if p['kind']=='SemaphoreRelease'}
    gpu, rejected_gpu, lo, hi = [], [], None, None
    for r in records:
        if r['kind'] != 'GpuTimes':
            continue
        key = r['object'], r['id']
        commit, retire = commits.get(key), retires.get(key)
        if (r['status'] != 4 or not r['a'] or r['b'] < r['a'] or not commit or not retire
                or commit['a'] != r['c'] or retire['a'] != r['c']):
            rejected_gpu.append(r['ticket'])
            continue
        # Causal bracket, not a measured clock calibration. Assumes stable offset
        # during the short capture and same command-buffer identity.
        low = commit['begin']*1000000000//freq-r['a']-1
        high = retire['end']*1000000000//freq-r['b']+1
        lo = low if lo is None else max(lo, low)
        hi = high if hi is None else min(hi, high)
        release = releases.get(key)
        gpu.append(dict(queue=r['object'], seq=r['id'], cmdbuf=r['c'], present_state=r['d'],
                        gpu_begin_ns=r['a'], gpu_end_ns=r['b'], gpu_ms=(r['b']-r['a'])/1000000,
                        commit_begin=commit['begin'], retire_qpc=retire['end'],
                        retire_to_release_ms=None if not release else (release['begin']-retire['end'])*1000/freq,
                        release_status=None if not release else release['status']))
    bridge = dict(status='UNKNOWN', kind='causal bounds, NOT clock calibration', lower_ns=lo, upper_ns=hi,
                  assumptions='same buffer, GPU after commit entry, GPU before retirement, stable clock offset/no sleep')
    if lo is not None:
        bridge['status'] = 'BOUNDED' if lo <= hi else 'FAILED'
        bridge['width_ns'] = hi-lo
    frames = []
    presents = collections.defaultdict(list)
    for p in pairs:
        if p['kind']=='Present' and p['status']==0 and not p['b'] & 1:
            presents[p['object']].append(p)
    for obj, rows in presents.items():
        rows.sort(key=lambda p:p['begin'])
        for previous, current in zip(rows, rows[1:]):
            a, b = previous['begin'], current['begin']
            if not begin <= a < b <= end:
                continue
            # P_N/S_N/F_(N+1) are observable phase markers. Their correlation is
            # not a measurement of a game's NtWait or CPU execution outside D3D12.
            qp = [p for p in pairs if p['kind']=='QueuePresent' and p['id']==previous['a']
                  and previous['begin'] <= p['begin'] <= p['end'] <= previous['end']]
            pc = [p for p in commits.values() if p['b']==previous['a']
                  and any(p['object']==q['object'] and q['begin']<=p['begin']<=p['end']<=q['end'] for q in qp)]
            release = releases.get((pc[0]['object'],pc[0]['id'])) if len(pc)==1 else None
            next_calls = [r for r in records if r['kind'] in ('AllocatorReset','ListReset','Execute')
                          and r['phase']==1 and previous['end'] <= r['qpc'] < current['begin']]
            first = min(next_calls,key=lambda r:r['qpc']) if next_calls else None
            gpu_n = [g for g in gpu if len(pc)==1 and (g['queue'],g['seq'])==(pc[0]['object'],pc[0]['id'])]
            f_vs_gpu = 'UNKNOWN'
            if first and len(gpu_n)==1 and bridge['status']=='BOUNDED':
                f_ns=first['qpc']*1000000000//freq
                early=gpu_n[0]['gpu_end_ns']+lo
                late=gpu_n[0]['gpu_end_ns']+hi
                f_vs_gpu='BEFORE_GPU_END' if f_ns < early else 'AFTER_GPU_END' if f_ns > late else 'CLOCK_UNCERTAIN'
            frames.append(dict(swapchain=hex(obj), present_index=current['id'], tid=current['tid'],
                begin_qpc=a, end_qpc=b, frame_ms=(b-a)*1000/freq, present_ms=previous['ms'],
                p_qpc=previous['end'], s_qpc=None if not release else release['begin'],
                f_qpc=None if not first else first['qpc'], f_kind=None if not first else first['kind'],
                f_tid=None if not first else first['tid'],
                p_to_f_ms=None if not first else (first['qpc']-previous['end'])*1000/freq,
                s_to_f_ms=None if not first or not release else (first['qpc']-release['begin'])*1000/freq,
                p_to_s_ms=None if not release else (release['begin']-previous['end'])*1000/freq,
                f_vs_gpu_n_end=f_vs_gpu,
                cpu_ms='UNKNOWN', game_latency_wait_ms='NOT_ENABLED', game_fence_wait_ms='NOT_ENABLED',
                other_game_wait_ms='NOT_ENABLED', gpu_recording_overlap='UNKNOWN'))
    return dict(schema='MRPL001-review-1', sha256=hashlib.sha256(data).hexdigest(), pid=pid,
        frequency=freq, anchor_qpc=anchor, anchor_filetime=wall, anchor_span=span,
        window_begin=begin, window_end=end, reserved_count=count, committed=len(records),
        incomplete=incomplete, overflow=bool(overflow), dropped=dropped, clock_failed=clock_failed,
        status='NOT_ENABLED' if not begin else ('DROPPED' if overflow or dropped else ('PRESENT' if records else 'EMPTY')),
        kinds=dict(kinds), pair_summary=summary, orphan_leaves=orphan, open_pairs=open_pairs,
        clock_bridge=bridge, invalid_or_unpaired_gpu=rejected_gpu, gpu=gpu, frames=frames,
        metadata=[r for r in records if r['kind'] in ('Swapchain','LatencySet','LatencyHandle','FenceCreate','FenceDestroy')],
        pairs=pairs, records=records,
        limitations=['retire is worker return, NOT Metal callback', 'Reset..Close elapsed is NOT CPU work',
                     'external game NtWait, native callback and HANDLE close/duplicate are NOT_ENABLED',
                     'late/early window scopes may be incomplete; do not infer zero from missing rows'])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=pathlib.Path)
    p.add_argument('--out', type=pathlib.Path)
    p.add_argument('--csv', type=pathlib.Path)
    args = p.parse_args()
    result = decode(args.input.read_bytes())
    if args.out:
        args.out.write_text(json.dumps(result, indent=2, sort_keys=True)+'\n')
    if args.csv:
        fields = ('swapchain present_index tid begin_qpc end_qpc frame_ms present_ms '
                  'p_qpc s_qpc f_qpc f_kind f_tid p_to_f_ms s_to_f_ms p_to_s_ms f_vs_gpu_n_end cpu_ms '
                  'game_latency_wait_ms game_fence_wait_ms other_game_wait_ms gpu_recording_overlap').split()
        with args.csv.open('w', newline='') as f:
            writer = csv.DictWriter(f, fields); writer.writeheader(); writer.writerows(result['frames'])
    compact = {k:v for k,v in result.items() if k not in ('records','pairs','gpu','metadata','frames')}
    compact['frames'] = len(result['frames']); compact['gpu'] = len(result['gpu'])
    print(json.dumps(compact, sort_keys=True))


if __name__ == '__main__':
    main()
