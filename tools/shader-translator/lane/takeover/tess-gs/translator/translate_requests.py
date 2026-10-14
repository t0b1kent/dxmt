#!/usr/bin/env python3
"""tess-gs 14.10.2026: перевод запросов слоя — пары стадий (vshs/hsds) здесь, одиночные — сценарием допуска.

Строки requests.txt:
  stage=vshs shader=<vs> second=<hs> root=<rts0|none>      -> <vs>.<hs>.<root>.vshs.metallib/.desc
  stage=hsds shader=<hs> second=<ds> root=<rts0|none>      -> <hs>.<ds>.<root>.hsds.metallib/.desc
  stage=<vs|ps|cs|...> shader=<sha1> root=... [variant=...] -> admission/translator/translate_requests.py
Object и mesh одной тесселяции порождаются ВМЕСТЕ (раскладка payload общая), поэтому пары сшиваются по HS;
контейнеры лежат рядом: <vs>.vs.dxbc, <hs>.hs.dxbc, <ds>.ds.dxbc, <root>.rts0.
  translate_requests.py <каталог запросов> <каталог переводов> [--work <каталог>]
"""
import sys
sys.dont_write_bytecode = True
import argparse, hashlib, json, shutil, subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESS = HERE.parent
ADM = TESS.parent / 'admission'
sys.path.insert(0, str(HERE))
import tess_compose  # noqa: E402
import gs_compose    # noqa: E402


def parse(line):
    return dict(x.split('=', 1) for x in line.split() if '=' in x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('requests', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--work', type=Path)
    a = ap.parse_args()
    work = a.work or (TESS / 'work' / a.out.name)
    work.mkdir(parents=True, exist_ok=True)
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / 'msl').mkdir(exist_ok=True)
    lines = [l.strip() for l in (a.requests / 'requests.txt').read_text().splitlines() if l.strip()]
    singles = [l for l in lines if parse(l).get('stage') not in ('vshs', 'hsds', 'vsgs', 'gsm', 'vsx')]
    pairs = [parse(l) for l in lines if parse(l).get('stage') in ('vshs', 'hsds')]
    gpairs = [parse(l) for l in lines if parse(l).get('stage') in ('vsgs', 'gsm')]
    xpairs = [parse(l) for l in lines if parse(l).get('stage') == 'vsx']
    index = []
    # одиночные стадии — сценарием допуска на отфильтрованной копии каталога запросов
    if singles:
        sub = work / 'singles'
        if sub.exists():
            shutil.rmtree(sub)
        sub.mkdir(parents=True)
        for item in a.requests.iterdir():
            if item.name != 'requests.txt' and item.is_file():
                shutil.copyfile(item, sub / item.name)
        (sub / 'requests.txt').write_text('\n'.join(singles) + '\n')
        r = subprocess.run([sys.executable, str(ADM / 'translator/translate_requests.py'), str(sub), str(a.out),
                            '--work', str(work / 'singles-work')], capture_output=True, text=True)
        print(r.stdout[-2500:], r.stderr[-1200:], flush=True)
        if r.returncode:
            return 2
        try:
            index.extend(json.loads((a.out / 'INDEX.json').read_text()).get('translations', []))
        except Exception:
            pass
    # пары: сшить по HS
    by_hs = {}
    for p in pairs:
        if p['stage'] == 'vshs':
            by_hs.setdefault(p['second'], {})['vs'] = p['shader']; by_hs[p['second']]['root'] = p.get('root', 'none')
        else:
            by_hs.setdefault(p['shader'], {})['ds'] = p['second']; by_hs[p['shader']]['root'] = p.get('root', 'none')
    failed = 0
    for hs, d in sorted(by_hs.items()):
        if 'vs' not in d or 'ds' not in d:
            print(json.dumps(dict(hs=hs, skipped='pair incomplete', have=sorted(d))), flush=True); failed += 1; continue
        vs, ds, root = d['vs'], d['ds'], d['root']
        rts0 = (a.requests / (root + '.rts0')).read_bytes() if root != 'none' and (a.requests / (root + '.rts0')).is_file() else None
        out = work / ('%s-%s-%s' % (vs[:8], hs[:8], ds[:8]))
        entry = dict(stage='tess', vs=vs, hs=hs, ds=ds, root=root)
        try:
            s = tess_compose.compose(str(a.requests / (vs + '.vs.dxbc')), str(a.requests / (hs + '.hs.dxbc')),
                                     str(a.requests / (ds + '.ds.dxbc')), out, rts0)
            b = tess_compose.build(out)
            if any(v[0] for v in b.values()):
                raise tess_compose.ComposeError('; '.join('%s: %s' % (k, v[1].splitlines()[0][:200]) for k, v in b.items() if v[0]))
            rk = s['root_key'] or root
            uses_root = bool(s['resources']['vs'] or s['resources']['hs'] or s['resources']['ds'])
            for kind, lib, desc, first, second in (('vshs', 'object', 'object.desc', vs, hs), ('hsds', 'mesh', 'mesh.desc', hs, ds)):
                name = '%s.%s.%s.%s' % (first, second, rk, kind) if uses_root and rk != 'none' else '%s.%s.%s' % (first, second, kind)
                shutil.copyfile(out / (lib + '.metallib'), a.out / (name + '.metallib'))
                shutil.copyfile(out / desc, a.out / (name + '.desc'))
                shutil.copyfile(out / (lib + '.metal'), a.out / 'msl' / (name + '.metal'))
                entry[kind] = dict(name=name, library_bytes=(a.out / (name + '.metallib')).stat().st_size,
                                   library_sha1=hashlib.sha1((a.out / (name + '.metallib')).read_bytes()).hexdigest())
            entry.update(accepted=True, decl=s['decl'], admission=s['admission'],
                         payload=s['object']['payload_bytes'], threads_per_patch=s['object']['threads_per_patch'])
        except (tess_compose.ComposeError, tess_compose.tess_body.BodyError, OSError) as e:
            entry.update(accepted=False, fail_class=type(e).__name__ + ': ' + str(e)[:400]); failed += 1
        index.append(entry)
        print(json.dumps({k: entry.get(k) for k in ('stage', 'vs', 'hs', 'ds', 'accepted', 'fail_class', 'payload', 'threads_per_patch')}), flush=True)
    # геометрия: пары (vs, gs) — object (vsgs) и mesh (gsm) порождаются вместе
    by_gs = {}
    for p in gpairs:
        by_gs.setdefault((p['shader'], p['second']), {})['root'] = p.get('root', 'none')
    for (vs, gsh), d in sorted(by_gs.items()):
        root = d['root']
        rts0 = (a.requests / (root + '.rts0')).read_bytes() if root != 'none' and (a.requests / (root + '.rts0')).is_file() else None
        out = work / ('gs-%s-%s' % (vs[:8], gsh[:8]))
        entry = dict(stage='geometry', vs=vs, gs=gsh, root=root)
        try:
            s = gs_compose.compose(str(a.requests / (vs + '.vs.dxbc')), str(a.requests / (gsh + '.gs.dxbc')), out, rts0)
            b = tess_compose.build(out)
            if any(v[0] for v in b.values()):
                raise tess_compose.ComposeError('; '.join('%s: %s' % (k, v[1].splitlines()[0][:200]) for k, v in b.items() if v[0]))
            rk = s['root_key'] or root
            uses_root = bool(s['resources']['vs'] or s['resources']['gs'])
            for kind, lib, desc in (('vsgs', 'object', 'object.desc'), ('gsm', 'mesh', 'mesh.desc')):
                name = '%s.%s.%s.%s' % (vs, gsh, rk, kind) if uses_root and rk != 'none' else '%s.%s.%s' % (vs, gsh, kind)
                shutil.copyfile(out / (lib + '.metallib'), a.out / (name + '.metallib'))
                shutil.copyfile(out / desc, a.out / (name + '.desc'))
                shutil.copyfile(out / (lib + '.metal'), a.out / 'msl' / (name + '.metal'))
                entry[kind] = dict(name=name, library_bytes=(a.out / (name + '.metallib')).stat().st_size,
                                   library_sha1=hashlib.sha1((a.out / (name + '.metallib')).read_bytes()).hexdigest())
            entry.update(accepted=True, gs=s['gs'])
        except (tess_compose.ComposeError, tess_compose.tess_body.BodyError, OSError) as e:
            entry.update(accepted=False, fail_class=type(e).__name__ + ': ' + str(e)[:400]); failed += 1
        index.append(entry)
        print(json.dumps({k: entry.get(k) for k in ('stage', 'vs', 'gs', 'accepted', 'fail_class')}), flush=True)
    # уровень 4: vsx (VS+GS одной вершинной функцией)
    for p in xpairs:
        vs, gsh, root = p['shader'], p['second'], p.get('root', 'none')
        rts0 = (a.requests / (root + '.rts0')).read_bytes() if root != 'none' and (a.requests / (root + '.rts0')).is_file() else None
        out = work / ('vsx-%s-%s' % (vs[:8], gsh[:8]))
        entry = dict(stage='vsx', vs=vs, gs=gsh, root=root)
        try:
            info = gs_compose.compose_expanded(str(a.requests / (vs + '.vs.dxbc')), str(a.requests / (gsh + '.gs.dxbc')), out, rts0)
            rc, msg = gs_compose.build_expanded(out)
            if rc:
                raise tess_compose.ComposeError('expanded: ' + msg.splitlines()[0][:200])
            rk = info['root_key'] or root
            uses_root = bool(info.get('qwords')) and rk != 'none'
            name = '%s.%s.%s.vsx' % (vs, gsh, rk) if uses_root else '%s.%s.vsx' % (vs, gsh)
            shutil.copyfile(out / 'expanded.metallib', a.out / (name + '.metallib'))
            shutil.copyfile(out / 'expanded.desc', a.out / (name + '.desc'))
            shutil.copyfile(out / 'expanded.metal', a.out / 'msl' / (name + '.metal'))
            entry.update(accepted=True, name=name, expand=info['expand'])
        except (tess_compose.ComposeError, tess_compose.tess_body.BodyError, OSError) as e:
            entry.update(accepted=False, fail_class=type(e).__name__ + ': ' + str(e)[:400]); failed += 1
        index.append(entry)
        print(json.dumps({k: entry.get(k) for k in ('stage', 'vs', 'gs', 'accepted', 'fail_class', 'expand')}), flush=True)
    (a.out / 'INDEX.json').write_text(json.dumps(dict(translations=index), ensure_ascii=False, indent=1) + '\n')
    print(json.dumps(dict(requests=len(lines), pairs=len(by_hs), geometry_pairs=len(by_gs), failed=failed)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
