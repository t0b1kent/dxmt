#!/usr/bin/env python3
"""Перевод заранее по запросам слоя (веха ресурсов).

Слой под ключом MACRUNNER_DX12_OWN_SHADER_REQUEST_DIR оставляет на каждый ненайденный перевод:
  <шейдер>.<стадия>.dxbc   контейнер, как его передала программа
  <корень>.rts0            часть RTS0 корневой подписи конвейера
  requests.txt             строки  stage=<..> shader=<sha1> root=<sha1|none> [variant=<sm|ds|ds.sm>]
Этот сценарий переводит каждую тройку конвейером pipeline.py в режиме --abi root и кладёт в каталог
для слоя (MACRUNNER_DX12_OWN_SHADER_DIR):
  <шейдер>.<корень>.<стадия>[.<вариант>].metallib/.desc   функция читает аргументы корня
  <шейдер>.<стадия>[.<вариант>].metallib/.desc            не читает
Описание — format=3 (слой: d3d12_shader_source.hpp, ParseDescription).
Рядом, в <каталог>/msl/, остаются текст MSL и описание привязок — для сверки, слой их не читает.
Игра, Wine, видеочип не запускаются.
"""
import sys
sys.dont_write_bytecode = True
import argparse, hashlib, json, os, shutil, subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import abi as rootabi      # noqa: E402
import pipeline            # noqa: E402

STAGE = dict(vs='VS', ps='PS', cs='CS', hs='HS', ds='DS', gs='GS')


def gate():
    if not (pipeline.ROOT / 'scripts/wine-slot.sh').is_file():     # standalone tool: no project build gate
        return
    r = subprocess.run(['bash', str(pipeline.ROOT / 'scripts/wine-slot.sh'), 'builds-ok'], cwd=str(pipeline.ROOT),
                       capture_output=True)
    if r.returncode != 0:
        raise SystemExit('builds-ok отказал: ' + r.stderr.decode('utf-8', 'replace').strip())


def description(row, container, root_key):
    lines = ['format=3', 'entry=main0', 'root=%s' % (root_key or 'none'), 'root_qwords=%d' % row.get('root_qwords', 0),
             'resources=%d' % row.get('resources', 0), 'variant=%s' % (row.get('variant') or 'none')]
    if row['stage'] == 'PS':
        lines += ['coverage_output=%d' % row['coverage_output'], 'colors=%s' % row['colors']]
    inputs = row.get('vertex_inputs') or []
    lines.append('vertex_inputs=%d' % len(inputs))
    if inputs:
        by_register = {e['register']: e for e in rootabi.input_signature(container)}
        for location in inputs:
            e = by_register.get(location)
            if e is None:
                raise SystemExit('нет элемента входной подписи с регистром %d' % location)
            lines.append('vin=%d %d %s' % (location, e['index'], e['semantic']))
    if row['stage'] == 'CS':
        if not row.get('local_size'):
            raise SystemExit('нет размера группы потоков у вычислительной стадии')
        lines.append('threads=%d %d %d' % tuple(row['local_size']))
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('requests', type=Path, help='каталог запросов слоя')
    ap.add_argument('out', type=Path, help='каталог переводов для слоя')
    ap.add_argument('--work', type=Path, required=True, help='рабочий каталог (внутри resources)')
    ap.add_argument('--frontend', help='свой двоичный разбора DXIL вместо двоичного лейна')
    a = ap.parse_args()
    cur = os.getpriority(os.PRIO_PROCESS, 0)
    if cur < 20:
        os.nice(20 - cur)
    gate()
    pipeline.ABI = 'root'
    pipeline.FRONTEND = a.frontend
    tools = pipeline.tools()
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / 'msl').mkdir(exist_ok=True)
    a.work.mkdir(parents=True, exist_ok=True)
    runner = pipeline.Runner(a.work.resolve(), tools, 64 * 1024 * 1024, 20)
    seen, index = set(), []
    for line in (a.requests / 'requests.txt').read_text().splitlines():
        f = dict(x.split('=', 1) for x in line.split())
        key = (f['stage'], f['shader'], f['root'], f.get('variant', ''))
        if key in seen:
            continue
        seen.add(key)
        stage, sha1, root, variant = key
        src = a.requests / ('%s.%s.dxbc' % (sha1, stage))
        container = src.read_bytes()
        assert hashlib.sha1(container).hexdigest() == sha1, 'контейнер запроса не равен своему имени'
        rts0 = None
        if root != 'none':
            rts0 = (a.requests / (root + '.rts0')).read_bytes()
            assert rootabi.root_key(rts0) == root, 'корневая подпись запроса не равна своему имени'
        pipeline.ROOTSIG = rts0
        if 'DXIL' not in rootabi.dxbc_chunks(container):
            index.append(dict(stage=stage, shader=sha1, root=root, variant=variant, accepted=False, fail_step='input',
                              fail_class='container has no DXIL part (SM5.0 shader: legacy path only)'))
            continue
        row = runner._translate(dict(sha1=sha1, stage=STAGE[stage], source=str(src), check=('sha1', sha1),
                                     variant=variant))
        entry = dict(stage=stage, shader=sha1, root=root, variant=variant, accepted=row['accepted'],
                     fail_step=row.get('fail_step'), fail_class=row.get('fail_class'), warnings=row.get('warnings'),
                     resources=row.get('resources'), root_qwords=row.get('root_qwords'),
                     vertex_inputs=row.get('vertex_inputs'), metal_slots=row.get('metal_slots'),
                     local_size=row.get('local_size'), t_ms={k: round(v / 1e6, 1) for k, v in row['t'].items()})
        if row['accepted']:
            uses_root = bool(row.get('resources'))
            name = '%s.%s.%s' % (sha1, root, stage) if uses_root else '%s.%s' % (sha1, stage)
            if variant:
                name += '.' + variant
            work = runner.wdir()
            shutil.copyfile(work / 'shader.metallib', a.out / (name + '.metallib'))
            (a.out / (name + '.desc')).write_text(description(row, container, root if uses_root else None))
            shutil.copyfile(work / 'shader.metal', a.out / 'msl' / (name + '.metal'))
            if (work / 'root-abi.txt').is_file():
                shutil.copyfile(work / 'root-abi.txt', a.out / 'msl' / (name + '.root-abi.txt'))
            entry.update(name=name, library_bytes=(a.out / (name + '.metallib')).stat().st_size,
                         library_sha1=hashlib.sha1((a.out / (name + '.metallib')).read_bytes()).hexdigest(),
                         desc=(a.out / (name + '.desc')).read_text().splitlines())
        index.append(entry)
        print(json.dumps({k: entry.get(k) for k in ('stage', 'shader', 'root', 'variant', 'accepted', 'fail_step', 'fail_class',
                                                     'resources', 'vertex_inputs', 'metal_slots', 'name')},
                         ensure_ascii=False), flush=True)
    (a.out / 'INDEX.json').write_text(json.dumps(dict(pins=pipeline.pins(tools), translations=index),
                                                 ensure_ascii=False, indent=1) + '\n')
    bad = [e for e in index if not e['accepted']]
    print(json.dumps(dict(requests=len(index), accepted=len(index) - len(bad), failed=len(bad))))


if __name__ == '__main__':
    main()
