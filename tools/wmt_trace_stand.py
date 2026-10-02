#!/usr/bin/env python3
"""Bounded native Metal pixel gate for a compatible Meson DXMT build."""
from pathlib import Path
import argparse,datetime,hashlib,importlib.util,json,math,os,plistlib,shlex,shutil,struct,subprocess,sys,time,zlib
import numpy as np
from PIL import Image
from wmt_trace_failure_evidence import LoadSampler,preserve_failure
ROOT=Path(__file__).resolve().parents[1]
LANE=Path(os.environ.get('WMT_TRACE_CORPUS', str(ROOT/'trace-corpus'))).resolve()
SOURCE=ROOT
DECODER=ROOT/'tools/trace_support/png_full_depth.py'
_decoder_spec=importlib.util.spec_from_file_location('wmt_stand_png_full_depth',DECODER)
_decoder=importlib.util.module_from_spec(_decoder_spec)
_decoder_spec.loader.exec_module(_decoder)
def sha(p):
    h=hashlib.sha256()
    with Path(p).open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()
def host_identity():
    def query(argv):
        return subprocess.check_output(argv,text=True,timeout=5).strip()
    return dict(product_version=query(['/usr/bin/sw_vers','-productVersion']),
        build_version=query(['/usr/bin/sw_vers','-buildVersion']),
        boot_session_uuid=query(['/usr/sbin/sysctl','-n','kern.bootsessionuuid']),
        boot_session_policy='PROVENANCE_ONLY')
def identity_receipts(log):
    return [json.loads(line.split(' ',1)[1]) for line in log.read_text().splitlines()
            if line.startswith('DEVICE_IDENTITY ')]
def identity_copy(trace,destination,mutate):
    """Read-only hardlinks, except a separately written device record/manifest."""
    manifest=json.loads((trace/'PREFIX.json').read_text())
    devices=[]
    for entry in manifest['files']:
        if entry['name'].startswith('event-') and entry['bytes']<4096:
            record=plistlib.loads((trace/entry['name']).read_bytes())
            if record.get('event')=='device':devices.append((entry,record))
    if len(devices)!=1:raise RuntimeError('identity control requires exactly one device record')
    entry,record=devices[0];original=dict(record['fields']);mutate(record['fields'])
    destination.mkdir()
    for item in manifest['files']:
        if item['name']!=entry['name']:os.link(trace/item['name'],destination/item['name'])
    device=destination/entry['name']
    device.write_bytes(plistlib.dumps(record,fmt=plistlib.FMT_BINARY,sort_keys=False))
    entry['bytes']=device.stat().st_size;entry['sha256']=sha(device)
    manifest['identity_control']=dict(source=str(trace),source_prefix_sha256=sha(trace/'PREFIX.json'),
        original_device=original,altered_device=record['fields'],
        storage='all other bytes immutable hardlinks; mutated record is a new inode')
    (destination/'PREFIX.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest['identity_control']
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    modes=parser.add_mutually_exclusive_group(required=True)
    modes.add_argument('--quick',action='store_true');modes.add_argument('--full',action='store_true')
    parser.add_argument('--dxmt',type=Path,required=True)
    parser.add_argument('--trace',type=Path,help='immutable completed-frame prefix; default is the HK qualification tape')
    parser.add_argument('--out',type=Path)
    parser.add_argument('--replays',type=int,choices=[1,3],help='full defaults to1; use3 before release/nightly')
    parser.add_argument('--prepare-only',action='store_true',help='relink native backend/player only; no pixel qualification')
    args=parser.parse_args()
    if args.quick and args.replays not in (None,1):parser.error('--quick permits one replay only')
    replays=args.replays or 1
    started=time.monotonic();budget=300 if args.quick else (1200 if replays==1 else 2400);deadline=started+budget
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    out=args.out.resolve() if args.out else LANE/'stand-runs'/stamp
    out.mkdir(parents=True,exist_ok=False)
    report=dict(schema=4,status='RUNNING',mode='quick' if args.quick else 'full',replays_per_title=replays,
        Wine='NOT_ENABLED',GPU_timing='NOT_ENABLED',alpha_included=True,
        threshold_psnr_db=60.0,exact_samples_required=True,full_depth=True,
        decoder_provenance=_decoder.provenance(),
        scope='GPU effects/selected unix backend; frozen frontend artifacts',
        source_sha256=sha(Path(__file__)),wall_budget_seconds=budget,out=str(out),steps={})
    def save():
        report['seconds']=time.monotonic()-started
        (out/'RESULT.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    def command(argv,name,cwd=None,env=None):
        remaining=math.floor(deadline-time.monotonic())
        if remaining<1:raise RuntimeError('bounded gate deadline')
        sampler=None
        if env and 'MACRUNNER_WMT_NATIVE_BACKEND' in env:
            env=dict(env,MACRUNNER_WMT_INPUT_EVIDENCE=str(out/(name+'-inputs.jsonl')))
            sampler=LoadSampler(out/(name+'-load.jsonl'));sampler.start()
        with (out/(name+'.log')).open('xb') as log:
            p=subprocess.Popen(['perl','-e',f'alarm {remaining}; exec @ARGV',*map(str,argv)],
                cwd=cwd,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,
                start_new_session=True)
            (out/(name+'-OWNED.json')).write_text(json.dumps(dict(pid=p.pid,argv=list(map(str,argv))))+'\n')
            try:rc=p.wait()
            finally:
                load_receipt=sampler.stop() if sampler else None
        report['steps'][name]=dict(rc=rc,log_sha256=sha(out/(name+'.log')))
        if sampler:
            inputs=out/(name+'-inputs.jsonl')
            report['steps'][name].update(load=load_receipt,inputs=dict(path=str(inputs),
                status='PRESENT' if inputs.exists() else 'NOT_ENABLED',sha256=sha(inputs) if inputs.exists() else None))
        save();return rc
    def build_command(argv,name,cwd=None):
        if (ROOT/'scripts/wine-slot.sh').exists() and subprocess.run([str(ROOT/'scripts/wine-slot.sh'),'builds-ok'],capture_output=True).returncode:
            raise RuntimeError('builds-ok BUSY; no compilation performed')
        daytime=datetime.time(7)<=datetime.datetime.now().time()<datetime.time(23,30)
        priority=['nice','-n','5'] if daytime else []
        if command(priority+list(argv),name,cwd):raise RuntimeError(name+' failed')
    def compare(reference,candidate,names=None):
        ref={p.name:p for p in reference.glob('frame-*.png')}
        got={p.name:p for p in candidate.glob('frame-*.png')}
        if names is not None:ref={n:ref[n] for n in names};got={n:got[n] for n in names}
        rows=[];missing=sorted(set(ref)-set(got));extra=sorted(set(got)-set(ref))
        for name in sorted(set(ref)&set(got)):
            a=_decoder.decode(ref[name]);b=_decoder.decode(got[name])
            if a.shape!=b.shape or a.dtype!=b.dtype:
                rows.append(dict(frame=name,status='FAIL',reason='dimensions or sample depth'));continue
            peak=int(np.iinfo(a.dtype).max)
            d=a.astype(np.float64)-b.astype(np.float64);mse=float(np.mean(d*d))
            psnr=None if mse==0 else 10*math.log10(peak**2/mse)
            changed=np.any(d!=0,axis=2)
            rows.append(dict(frame=name,status='PASS' if mse==0 else 'FAIL',sample_depth=a.dtype.itemsize*8,
                rgba_mse=mse,psnr_db=psnr,psnr_infinite=mse==0,
                max_delta=int(np.abs(d).max()),different_pixels=int(changed.sum()),
                different_pixel_percent=float(changed.mean()*100),
                alpha_different_pixels=int(np.sum(d[:,:,3]!=0)),
                reference_png_sha256=sha(ref[name]),candidate_png_sha256=sha(got[name]),
                candidate_rgba_sha256=hashlib.sha256(b.tobytes()).hexdigest()))
        return dict(status='PASS' if rows and not missing and not extra and all(r['status']=='PASS' for r in rows) else 'FAIL',
            missing=missing,extra=extra,frames=rows,alpha_included=True,
            metric='raw PNG8/16 RGBA samples; own full-scale peak; exact required; no normalization')
    def png16(path,red):
        def chunk(kind,data):
            return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
        pixel=struct.pack('>HHHH',red,0,0,65535)
        raw=b''.join(b'\0'+pixel*16 for _ in range(16))
        path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',16,16,16,6,0,0,0))+
            chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
    try:
        report['host']=host_identity()
        report['truth_os']=dict(product_version='27.0',
            provenance='existing accepted macOS27.0 recording; immutable originals retained',
            update_policy='never replace truth automatically; report full-depth deltas against the recorded OS')
        report['native_source_sha256']={str(p.relative_to(SOURCE)):sha(p)
            for p in sorted((SOURCE/'src/winemetal/unix').glob('wmt_trace*.h'))}
        report['native_source_sha256']['tools/wmt_trace_native_player.m']=sha(SOURCE/'tools/wmt_trace_native_player.m')
        report['native_source_sha256']['tools/wmt_trace_negative_backend.m']=sha(SOURCE/'tools/wmt_trace_negative_backend.m')
        report['native_source_sha256']['tools/wmt_trace_failure_evidence.py']=sha(SOURCE/'tools/wmt_trace_failure_evidence.py')
        if shutil.disk_usage(out).free<30*1024**3:raise RuntimeError('output volume below30GiB')
        build=args.dxmt.resolve()
        if not (build/'build.ninja').is_file():raise RuntimeError('--dxmt requires compatible Meson build directory with build.ninja')
        binary=build/'src/winemetal/unix/winemetal.so'
        if b'MACRUNNER_WMT_TRACE_ABI135_' not in binary.read_bytes():
            raise RuntimeError('selected build has unverified recorder ABI; require135 marker')
        q=subprocess.run(['ninja','-C',str(build),'-t','commands','src/winemetal/unix/winemetal.so'],
            capture_output=True,text=True,check=True)
        links=[x for x in q.stdout.splitlines() if ' -o src/winemetal/unix/winemetal.so ' in x]
        if len(links)!=1:raise RuntimeError('ambiguous selected backend link command')
        link=shlex.split(links[0])
        if Path(link[0]).name=='ccache':link=link[1:]
        backend=out/'libwmt-stand-backend.dylib'
        link[link.index('-o')+1]=str(backend)
        removed=[x for x in link if x.endswith(('/ntdll.so','/winemac.so'))]
        if len(removed)!=2:raise RuntimeError('unexpected Wine dependencies')
        link=[x.replace('@rpath/winemetal.so','@rpath/'+backend.name) for x in link if x not in removed]
        inputs={}
        for arg in link:
            p=Path(arg) if arg.startswith('/') else build/arg
            if arg.endswith(('.o','.a')) and p.is_file():inputs[str(p.resolve())]=sha(p)
        foreign=ROOT/'tools/trace_support/native_backend_foreign.c'
        build_command(['/usr/bin/xcrun','clang','-arch','arm64','-c',foreign,'-o',out/'foreign.o'],'foreign-compile')
        link.append(str(out/'foreign.o'));build_command(link,'backend-link',build)
        deps=subprocess.run(['/usr/bin/otool','-L',str(backend)],capture_output=True,text=True,check=True).stdout
        (out/'dependencies.txt').write_text(deps)
        if 'ntdll.so' in deps or 'winemac.so' in deps:raise RuntimeError('native backend retains Wine dependency')
        if any(sha(p)!=h for p,h in inputs.items()):raise RuntimeError('selected object input drift')
        report['backend']=dict(path=str(backend),sha256=sha(backend),selected_winemetal_sha256=sha(binary),
            inputs=inputs,removed_dependencies=removed,foreign_stub_sha256=sha(foreign))
        player=out/'wmt-trace-player'
        build_command(['/usr/bin/xcrun','clang','-arch','arm64','-fblocks',
            '-framework','Foundation','-framework','Metal','-framework','QuartzCore',
            '-framework','CoreGraphics','-framework','ImageIO',SOURCE/'tools/wmt_trace_native_player.m','-o',player],'player-compile')
        mutant=out/'libwmt-negative.dylib'
        build_command(['/usr/bin/xcrun','clang','-arch','arm64','-fblocks','-dynamiclib',
            '-framework','Foundation','-framework','Metal','-I'+str(SOURCE/'src/winemetal'),
            SOURCE/'tools/wmt_trace_negative_backend.m','-o',mutant],'negative-compile')
        report['player']=dict(path=str(player),sha256=sha(player))
        if args.prepare_only:
            report['status']='PREPARED';report['reason']='native inputs prepared; GPU and pixels NOT_ENABLED';save()
            print('DXMT-STAND: PREPARED seconds='+format(report['seconds'],'.3f')+' report='+str(out/'RESULT.json'))
            return 0
        tapes=[('selected',args.trace.resolve())] if args.trace else (
            [('hk',LANE/'hk-record12-quick32'),('divinity',LANE/'divinity-stage7-quick16'),
             ('abzu',LANE/'abzu-stage7-quick16')] if args.quick else
            [('hk',LANE/'hk-record12-first300'),('divinity',LANE/'peer-call-audit/stage8/divinity-prefix'),
             ('abzu',LANE/'peer-call-audit/stage8/abzu-prefix')])
        env={'PATH':os.environ['PATH'],'HOME':os.environ['HOME'],'LANG':'en_US.UTF-8',
            'MACRUNNER_WMT_NATIVE_BACKEND':str(backend)}
        report['traces']={};report['positive']={};report['pixel_failures']=[]
        repeat_exact={}
        for title,trace in tapes:
            manifest=json.loads((trace/'PREFIX.json').read_text());expected=len(manifest['frames'])
            report['traces'][title]=dict(path=str(trace),prefix_sha256=sha(trace/'PREFIX.json'),frames=expected)
            positives=[];report['positive'][title]=positives
            for i in range(1,replays+1):
                frames=out/f'{title}-positive{i}-frames'
                rc=command([player,'--check',trace,frames],f'{title}-positive{i}',env=env)
                c=compare(trace/'truth-frames',frames);c['rc']=rc
                c['device_identity']=identity_receipts(out/f'{title}-positive{i}.log')
                positives.append(c);save()
                if rc or len(c['frames'])!=expected or c['missing'] or c['extra']:
                    raise RuntimeError(title+' native replay incomplete or failed')
                if c['status']!='PASS':
                    failed=[x for x in c['frames'] if x['status']!='PASS']
                    trap=preserve_failure(trace,trace/'truth-frames',frames,failed,
                        out/f'{title}-positive{i}-failure',out/f'{title}-positive{i}-inputs.jsonl',
                        out/f'{title}-positive{i}-load.jsonl',_decoder)
                    report['pixel_failures'].append(dict(title=title,replay=i,
                        mismatched_frames=failed,evidence=trap))
                    save()
                if i<replays:time.sleep(5)
            if replays>1:
                hs=[[row['candidate_rgba_sha256'] for row in c['frames']] for c in positives]
                repeat_exact[title]=all(h==hs[0] for h in hs)
        report['repeat_exact_by_title']=repeat_exact if replays>1 else 'NOT_ENABLED'
        report['repeat_exact']=all(repeat_exact.values()) if replays>1 else 'NOT_ENABLED'
        report['identity_controls']={}
        for title,trace in [('hk',LANE/'hk-record12-quick32'),
                ('divinity',LANE/'divinity-stage7-quick16'),('abzu',LANE/'abzu-stage7-quick16')]:
            for mode in ['registryID','name']:
                name=f'identity-{title}-{mode}'
                copied=out/(name+'-trace')
                def mutate(fields):
                    if mode=='registryID':fields['registryID']^=0x8000000000000000
                    else:fields['name']+=' CORRUPTED_DEVICE_NAME'
                provenance=identity_copy(trace,copied,mutate)
                frames=out/(name+'-frames');rc=command([player,'--check',copied,frames],name,env=env)
                receipts=identity_receipts(out/(name+'.log'))
                c=compare(trace/'truth-frames',frames);c.update(rc=rc,device_identity=receipts,mutation=provenance)
                if mode=='registryID':
                    passed=rc==0 and c['status']=='PASS' and bool(receipts) and all(
                        x['recorded-registryID']!=x['current-registryID'] for x in receipts)
                else:
                    passed=rc!=0 and bool(receipts) and 'recorded native device identity differs' in (out/(name+'.log')).read_text()
                c['expectation_met']=passed;report['identity_controls'][name]=c;save()
                if not passed:raise RuntimeError(name+' expectation failed')
        # Old tapes cannot acquire historical feature data retroactively. A
        # clearly labelled synthetic current-device profile tests the new schema.
        current=report['positive'][tapes[0][0]][0]['device_identity'][0]['current-capabilities']
        for mode in ['profile-positive','unified-memory','family','capability']:
            name='identity-'+mode;copied=out/(name+'-trace')
            def mutate(fields):
                fields['capabilities']=json.loads(json.dumps(current))
                if mode=='unified-memory':fields['unified-memory']=not fields['unified-memory']
                if mode=='family':fields['capabilities']['family-support']['1007']=not current['family-support']['1007']
                if mode=='capability':fields['capabilities']['bc-texture-compression']=not current['bc-texture-compression']
            provenance=identity_copy(LANE/'hk-record12-quick32',copied,mutate)
            frames=out/(name+'-frames');rc=command([player,'--check',copied,frames],name,env=env)
            c=compare(LANE/'hk-record12-quick32/truth-frames',frames)
            c.update(rc=rc,device_identity=identity_receipts(out/(name+'.log')),mutation=provenance,
                profile_provenance='SYNTHETIC_CURRENT_DEVICE; historical capabilities NOT_RECORDED')
            passed=(rc==0 and c['status']=='PASS') if mode=='profile-positive' else (
                rc!=0 and 'recorded native device identity differs' in (out/(name+'.log')).read_text())
            c['expectation_met']=passed;report['identity_controls'][name]=c;save()
            if not passed:raise RuntimeError(name+' expectation failed')
        controls={}
        control_trace=LANE/'hk-record12-quick32'
        for mode in ['skip','format','blend']:
            altered=dict(env,MACRUNNER_WMT_NATIVE_BACKEND=str(mutant),
                MACRUNNER_WMT_STAND_BASE_BACKEND=str(backend),MACRUNNER_WMT_STAND_MUTATION=mode)
            frames=out/(mode+'-frames')
            rc=command([player,'--check',control_trace,frames],mode,env=altered)
            log=(out/(mode+'.log')).read_text(errors='replace')
            c=compare(control_trace/'truth-frames',frames);c['rc']=rc
            c['mutation_executed']='NEGATIVE_CONTROL '+('skip_SetPSO' if mode=='skip' else mode)+' ACTIVE' in log
            controls[mode]=c;report['negative_controls']=controls;save()
            if not c['mutation_executed'] or (rc==0 and c['status']=='PASS'):
                raise RuntimeError('negative '+mode+' did not fail')
        alpha=out/'alpha-frames';alpha.mkdir()
        first_title,first_trace=tapes[0]
        first=report['positive'][first_title][0]['frames'][0]['frame']
        with Image.open(out/f'{first_title}-positive1-frames'/first) as im:
            data=np.array(im.convert('RGBA'));data[:,:,3]^=np.uint8(1)
            Image.fromarray(data).save(alpha/first)
        c=compare(first_trace/'truth-frames',alpha,[first])
        controls['alpha']=c
        if c['status']!='FAIL' or c['frames'][0]['alpha_different_pixels']==0:
            raise RuntimeError('negative alpha did not fail')
        low_ref=out/'png16-control-reference';low_ref.mkdir()
        low_got=out/'png16-control-candidate';low_got.mkdir()
        png16(low_ref/'frame-000000.png',0);png16(low_got/'frame-000000.png',192)
        with Image.open(low_ref/'frame-000000.png') as im:a8=np.asarray(im.convert('RGBA'))
        with Image.open(low_got/'frame-000000.png') as im:b8=np.asarray(im.convert('RGBA'))
        c=compare(low_ref,low_got);c['rgba8_equal']=bool(np.array_equal(a8,b8))
        controls['png16-lowbits']=c
        if c['status']!='FAIL' or not c['rgba8_equal'] or c['frames'][0]['max_delta']!=192:
            raise RuntimeError('PNG16 lowbits control did not expose RGBA8 blind spot')
        one_lsb=out/'png16-one-lsb-candidate';one_lsb.mkdir()
        png16(one_lsb/'frame-000000.png',1)
        c=compare(low_ref,one_lsb);controls['png16-one-lsb']=c
        if c['status']!='FAIL' or c['frames'][0]['max_delta']!=1:
            raise RuntimeError('PNG16 one-LSB exact control did not fail')
        fingerprint=dict(schema=4,mode=report['mode'],replays_per_title=replays,selected_winemetal_sha256=sha(binary),
            backend_inputs=report['backend']['inputs'],foreign_stub_sha256=sha(foreign),
            decoder=report['decoder_provenance'],source_sha256=report['source_sha256'],
            native_source_sha256=report['native_source_sha256'],
            host={k:report['host'][k] for k in ['product_version','build_version']},
            device_semantics={t:[{k:v for k,v in c['device_identity'][0].items()
                if 'registryID' not in k} for c in rows] for t,rows in report['positive'].items()},
            identity_controls={k:dict(expectation_met=c['expectation_met'],rc=c['rc'],
                frames=[dict(frame=x['frame'],depth=x['sample_depth'],sha256=x['candidate_rgba_sha256'],
                    max_delta=x['max_delta']) for x in c['frames']]) for k,c in report['identity_controls'].items()},
            tapes={t:dict(prefix_sha256=report['traces'][t]['prefix_sha256'],
                frames=[[dict(frame=x['frame'],depth=x['sample_depth'],sha256=x['candidate_rgba_sha256'])
                         for x in c['frames']] for c in rows]) for t,rows in report['positive'].items()},
            negative_controls={k:dict(status=c['status'],frames=[dict(frame=x['frame'],depth=x['sample_depth'],
                sha256=x['candidate_rgba_sha256'],max_delta=x['max_delta']) for x in c['frames']])
                for k,c in controls.items()})
        encoded=json.dumps(fingerprint,sort_keys=True,separators=(',',':')).encode()
        (out/'FINGERPRINT.json').write_bytes(encoded+b'\n')
        report['fingerprint_sha256']=hashlib.sha256(encoded).hexdigest()
        report['fingerprint_policy']='stable selected build inputs and exact raw pixel/control results; relink SHA pinned separately, excluded from result identity'
        report['game_frames']={t:v['frames'] for t,v in report['traces'].items()}
        if any(sha(SOURCE/p)!=h for p,h in report['native_source_sha256'].items()):
            raise RuntimeError('native source drift during gate')
        if report['pixel_failures'] or (replays>1 and not report['repeat_exact']):
            report['status']='FAIL'
            report['reason']='complete coverage; full-depth pixel mismatch or repeat drift; see pixel_failures and repeat_exact_by_title'
        else:
            report['status']='PASS';report['reason']='all selected tapes exact; six negative controls fail; ten boot/device controls meet expectations'
    except Exception as error:
        report['status']='FAIL';report['reason']=str(error)
    save()
    print('DXMT-STAND: '+report['status']+' '+report['mode']+' seconds='+format(report['seconds'],'.3f')+' report='+str(out/'RESULT.json'))
    return 0 if report['status']=='PASS' else 1
if __name__=='__main__':sys.exit(main())
