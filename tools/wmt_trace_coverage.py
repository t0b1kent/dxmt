#!/usr/bin/env python3
"""Conservative source matrix. A hook is not proof of semantic coverage."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from wmt_trace_inventory import inventory, structures

def body_at(text, start):
    depth, i, state = 1, start, 'code'
    while i < len(text) and depth:
        c, n = text[i], text[i:i+2]
        if state == 'line':
            if c == '\n': state = 'code'
        elif state == 'comment':
            if n == '*/': state = 'code'; i += 1
        elif state in ('string', 'char'):
            if c == '\\': i += 1
            elif c == ('"' if state == 'string' else "'"): state = 'code'
        elif n == '//': state = 'line'; i += 1
        elif n == '/*': state = 'comment'; i += 1
        elif c == '"': state = 'string'
        elif c == "'": state = 'char'
        elif c == '{': depth += 1
        elif c == '}': depth -= 1
        i += 1
    if depth: raise ValueError('unbalanced function body')
    return text[start:i-1]

def matrix(root):
    info = inventory(root)
    source_path = root/'src/winemetal/unix/winemetal_unix.c'
    source = source_path.read_text()
    thunks = (root/'src/winemetal/winemetal_thunks.h').read_text()
    abi = structures(thunks)
    player = (root/'tools/wmt_trace_native_player.m').read_text()
    native = (root/'src/winemetal/unix/wmt_trace_native_objects.h').read_text()
    replay = set(re.findall(r'\[(?:event|type) isEqual(?:ToString)?:@"([^"]+)"\]', player+native))
    definitions = {}
    for p in (root/'src/winemetal/unix').iterdir():
        if p.suffix not in ('.c','.m','.h'): continue
        text=p.read_text(errors='replace')
        for match in re.finditer(r'\b(_\w+)\s*\(\s*void\s*\*\s*\w+\s*\)\s*\{',text):
            definitions.setdefault(match[1],[]).append((p,match.start(),body_at(text,match.end())))
    rows=[]
    for call in info['calls']:
        if not call['entry']: continue
        name=call['entry']; defs=definitions.get(name,[])
        body='\n'.join(d[2] for d in defs)
        events=set(re.findall(r'wmt_trace_record_event\s*\(\s*@"([^"]+)"',body))
        helper=[]
        if 'wmt_trace_present(' in body:
            events |= {'texture','frame-readback'};helper.append('wmt_trace_present')
        packed='wmt_trace_record_commands' in body
        if packed: helper.append('wmt_trace_record_commands')
        if 'wmt_trace_cpu_begin' in body or 'wmt_trace_cpu_end' in body:
            helper.append('CPU ownership -> commit-inputs')
        has_hook=bool(events or packed)
        native_complete=bool(has_hook and events<=replay)
        if packed: native_complete=True
        creation=bool(re.search(r'new[A-Z]|alloc\]|alloc_init|MTLCreate|copyAllDevices|CreateMetalView',body+name))
        destruction=bool(re.search(r'release\]|destroy|free',body+name,re.I))
        fields=call['pointer_fields']
        param=call['params']
        declaration=abi.get(param,'')
        formulas=sorted(set(re.findall(r'params->\w*(?:length|Length|size|Size|count|Count|bytes_per\w*|byte\w*)',body)))
        rows.append(dict(ordinal=call['ordinal'],call=name,params=param,
            source=[dict(path=str(p.relative_to(root)),line=p.read_text(errors='replace')[:pos].count('\n')+1,
                body_sha256=hashlib.sha256(b.encode()).hexdigest()) for p,pos,b in defs],
            definition='PRESENT' if defs else 'MACRO_OR_EXTERNAL_REQUIRES_REVIEW',
            record_hook='PRESENT' if has_hook else 'NOT_ENABLED',record_events=sorted(events),helpers=helper,
            replay_branch='PRESENT' if native_complete else 'NOT_ENABLED',
            missing_replay_events=sorted(events-replay),
            creates_object=creation,destroys_object=destruction,
            object_handles=[l.strip() for l in declaration.splitlines() if 'obj_handle_t' in l],
            pe_pointer_fields=fields,pe_byte_formula_candidates=formulas,
            pe_bytes_status='NOT_APPLICABLE' if not fields else 'REQUIRES_SEMANTIC_REVIEW',
            gap=not(has_hook and native_complete),
            certainty='STATIC_SYNTAX_ONLY; semantic completeness not inferred'))
    return dict(schema=1,status='CONSERVATIVE_STATIC_MATRIX_NOT_RUNTIME_COVERAGE',root=str(root),
        source_sha256=hashlib.sha256(source_path.read_bytes()).hexdigest(),
        counts=dict(dispatch_slots=info['dispatch_count'],reserved=info['reserved_ordinals'],
            active_calls=len(rows),source_literal_functions=len(definitions),
            record_hooks=sum(x['record_hook']=='PRESENT' for x in rows),
            replay_branches=sum(x['replay_branch']=='PRESENT' for x in rows),
            gaps=sum(x['gap'] for x in rows),pe_pointer_calls=sum(bool(x['pe_pointer_fields']) for x in rows)),
        calls=rows)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();data=matrix(args.root.resolve());args.out.mkdir(parents=True,exist_ok=False)
    (args.out/'COVERAGE.json').write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n')
    lines=['Консервативная статическая матрица; наличие хука не доказывает полноту семантики.',
        '',json.dumps(data['counts'],ensure_ascii=False),'',
        '| Ord | Call | Record | Replay | Object create/destroy | PE pointer/formula candidates |',
        '|---:|---|---|---|---|---|']
    for row in data['calls']:
        memory='; '.join(row['pe_pointer_fields']+row['pe_byte_formula_candidates']).replace('|','\\|')
        lines.append(f"| {row['ordinal']} | {row['call']} | {row['record_hook']} | {row['replay_branch']} | {row['creates_object']}/{row['destroys_object']} | {memory or 'none'} |")
    (args.out/'COVERAGE.md').write_text('\n'.join(lines)+'\n')
    print(json.dumps(data['counts']))

if __name__=='__main__':main()
