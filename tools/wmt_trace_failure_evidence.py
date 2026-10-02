"""Exact mismatch trap: preserve pixels, consumed inputs and aggregate host load."""
from pathlib import Path
import ctypes,datetime,hashlib,json,os,plistlib,shutil,subprocess,threading,time

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()

class LoadSampler:
    """System totals only; no unrelated process inspection or global tracing."""
    def __init__(self,path):
        self.path=Path(path);self.finished=threading.Event();self.samples=0;self.errors=0
        self.lib=ctypes.CDLL('/usr/lib/libSystem.B.dylib');self.lib.mach_host_self.restype=ctypes.c_uint
        self.previous=self.ticks();self.thread=threading.Thread(target=self.run,daemon=True)
    def ticks(self):
        ticks=(ctypes.c_uint*4)();count=ctypes.c_uint(4)
        rc=self.lib.host_statistics(self.lib.mach_host_self(),3,ticks,ctypes.byref(count))
        if rc:raise RuntimeError('host_statistics rc='+str(rc))
        return list(ticks)
    def run(self):
        with self.path.open('x') as f:
            while not self.finished.is_set():
                row=dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    monotonic=time.monotonic(),loadavg=os.getloadavg())
                try:
                    ticks=self.ticks();delta=[(b-a)&0xffffffff for a,b in zip(self.previous,ticks)];self.previous=ticks
                    row['cpu_busy_percent']=100*(1-delta[2]/sum(delta)) if sum(delta) else None
                    p=subprocess.run(['/usr/sbin/ioreg','-r','-c','AGXAccelerator','-d','1','-a'],
                        capture_output=True,timeout=4,check=True)
                    stats=plistlib.loads(p.stdout)[0]['PerformanceStatistics']
                    row['gpu']={k:v for k,v in stats.items() if 'Utilization' in k or k in ['recoveryCount','lastRecoveryTime']}
                    row['status']='PRESENT'
                except Exception as e:row.update(status='FAILED',reason=str(e));self.errors+=1
                f.write(json.dumps(row)+'\n');f.flush();self.samples+=1
                self.finished.wait(1)
    def start(self):self.thread.start()
    def stop(self):
        self.finished.set();self.thread.join(timeout=6)
        return dict(path=str(self.path),sha256=sha(self.path),samples=self.samples,errors=self.errors,
            status='FAILED' if self.thread.is_alive() or self.errors else 'PRESENT')

def preserve_failure(trace,reference,candidate,rows,destination,input_log,load_log,decoder):
    """Conservative all-history closure; never asserts missing GPU bytes are known."""
    destination=Path(destination);destination.mkdir()
    trace=Path(trace);manifest=json.loads((trace/'PREFIX.json').read_text())
    shutil.copy2(trace/'PREFIX.json',destination/'PREFIX.json')
    copied=[];coordinates=[]
    for row in rows:
        name=row['frame']
        for label,root in [('truth',reference),('candidate',candidate)]:
            target=destination/(label+'-'+name);shutil.copy2(Path(root)/name,target)
            copied.append(dict(kind=label,frame=name,path=str(target),sha256=sha(target)))
        import numpy as np
        a=decoder.decode(Path(reference)/name);b=decoder.decode(Path(candidate)/name)
        if a.shape==b.shape and a.dtype==b.dtype:
            indices=np.argwhere(a!=b)
            coordinates.append(dict(frame=name,different_samples=len(indices),samples=[
                dict(y=int(y),x=int(x),channel='RGBA'[c],truth=int(a[y,x,c]),candidate=int(b[y,x,c]),
                    delta=int(b[y,x,c])-int(a[y,x,c])) for y,x,c in indices[:4096]],
                dropped=max(0,len(indices)-4096)))
    evidence=dict(schema=1,pixels=copied,coordinates=coordinates,
        trace=str(trace),prefix_sha256=sha(trace/'PREFIX.json'),exact_samples_required=True,
        GPU_memory_completeness='UNKNOWN; original INCOMPLETE metadata preserved',
        dependency_policy='all consumed prior records, not only current-frame uploads; persistent resources included',
        input_hash_policy='SHA256 raw records as consumed; length-prefixed cumulative chain; snapshots before relocation')
    for label,path in [('input-consumption',input_log),('load',load_log)]:
        path=Path(path)
        if path.exists():
            target=destination/(label+'.jsonl');shutil.copy2(path,target)
            evidence[label]=dict(status='PRESENT',path=str(target),sha256=sha(target),bytes=target.stat().st_size)
        else:evidence[label]=dict(status='NOT_ENABLED')
    if Path(input_log).exists():
        wanted={int(x['frame'].split('-')[1].split('.')[0]) for x in rows}
        targets={};commits={};last=None;consumed=0;incomplete=0
        expected={x['sequence']:x for x in manifest['files']}
        mismatched_records=[]
        for line in Path(input_log).open():
            entry=json.loads(line);last=entry;consumed+=1
            if entry.get('status')=='INCOMPLETE':incomplete+=1
            selected=expected.get(entry['sequence'])
            if not selected or selected['sha256']!=entry['sha256'] or selected['bytes']!=entry['bytes']:
                mismatched_records.append(entry['sequence'])
            if entry.get('event')=='frame-readback' and entry.get('frame') in wanted:
                targets[entry['object']]=dict(frame=entry['frame'],texture=entry.get('texture'),
                    schedule_sequence=entry['sequence'],schedule_prefix_sha256=entry['prefix-sha256'])
            if entry.get('event')=='command-buffer-commit' and entry.get('object') in targets:
                commits[str(entry['object'])]=dict(**targets[entry['object']],commit_sequence=entry['sequence'],
                    all_prior_inputs_sha256=entry['prefix-sha256'],monotonic_seconds=entry['monotonic-seconds'])
        evidence.update(consumed_records=consumed,consumed_records_differ_from_manifest=mismatched_records,
            incomplete_records=incomplete,frame_inputs=commits,
            terminal_input_sha256=last['prefix-sha256'] if last else None,
            coverage='PRESENT' if consumed==len(expected) and not mismatched_records and len(commits)==len(wanted) else 'FAILED')
    (destination/'EVIDENCE.json').write_text(json.dumps(evidence,indent=2)+'\n')
    return dict(path=str(destination),sha256=sha(destination/'EVIDENCE.json'),coverage=evidence.get('coverage','NOT_ENABLED'))
