#!/usr/bin/env python3
"""MacRunner (c16, 17.10.2026): build-time guard that the PE and unix halves of winemetal agree on ONE
unix-call table. Run by src/winemetal/meson.build at configure time; any mismatch is a configure error.

Why: 1.0.9 shipped a 133-entry table, the D3D12 AIR tree a 164-entry one; both started identical at
0..131 and diverged at 132, which a mixed install would have called silently wrong. Checks:
  1. winemetal_thunks.c: every function F with UNIX_CALL(n) -> table[n] is `_F` (explicit aliases below);
  2. airconv_thunks.h enum airconv_unixcalls -> table entries thunk_SM50*;
  3. __wine_unix_call_funcs and __wine_unix_call_wow64_funcs list the same names in the same order (thunk32_ = thunk_);
  4. no index is used by two different PE functions.
usage: check_unix_table.py <src/winemetal dir>
"""
import re,sys
from pathlib import Path
ALIASES={'WMTCopyAllDevices':'_MTLCopyAllDevices'}
AIRCONV={'sm50_initialize':'thunk_SM50Initialize','sm50_destroy':'thunk_SM50Destroy','sm50_compile':'thunk_SM50Compile',
         'sm50_get_compiled_bitcode':'thunk_SM50GetCompiledBitcode','sm50_destroy_bitcode':'thunk_SM50DestroyBitcode',
         'sm50_get_error_message':'thunk_SM50GetErrorMessage','sm50_free_error':'thunk_SM50FreeError',
         'sm50_compile_geometry_pipeline_vertex':'thunk_SM50CompileGeometryPipelineVertex',
         'sm50_compile_geometry_pipeline_geometry':'thunk_SM50CompileGeometryPipelineGeometry',
         'sm50_compile_tessellation_vertex':'NULL','sm50_compile_tessellation_hull':'thunk_SM50CompileTessellationPipelineHull',
         'sm50_compile_tessellation_domain':'thunk_SM50CompileTessellationPipelineDomain','sm50_get_arguments_info':'thunk_SM50GetArgumentsInfo',
         'sm50_get_dxil_resource_bindings':'thunk_SM50GetDxilResourceBindings'}
def table(src,name):
    i=src.index(name+'[]');i=src.index('{',i);j=src.index('};',i)
    body=re.sub(r'//[^\n]*|/\*.*?\*/','',src[i+1:j],flags=re.S)
    return [x.strip().lstrip('&') for x in body.split(',') if x.strip()]
def main(d):
    d=Path(d);errors=[]
    unix=(d/'unix/winemetal_unix.c').read_text(errors='replace')
    funcs=table(unix,'__wine_unix_call_funcs')
    # wow64 entries of the airconv thunks are their 32-bit marshalling twins (thunk32_*); names otherwise identical.
    wow=[x.replace('thunk32_','thunk_',1) for x in table(unix,'__wine_unix_call_wow64_funcs')]
    if funcs!=wow:
        k=next((i for i,(a,b) in enumerate(zip(funcs,wow)) if a!=b),min(len(funcs),len(wow)))
        errors.append(f'unix and wow64 tables differ at index {k} (lengths {len(funcs)}/{len(wow)})')
    thunks=(d/'winemetal_thunks.c').read_text(errors='replace');users={}
    for m in re.finditer(r'\n([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{(.*?)\n\}',thunks,re.S):
        for n in sorted({int(x) for x in re.findall(r'UNIX_CALL\(\s*(\d+)\s*,',m.group(2))}):
            users.setdefault(n,set()).add(m.group(1))
            want=ALIASES.get(m.group(1),'_'+m.group(1))
            if n>=len(funcs):errors.append(f'{m.group(1)} calls index {n} beyond table size {len(funcs)}')
            elif funcs[n]!=want:errors.append(f'{m.group(1)} calls index {n}, table has {funcs[n]}')
    errors+=[f'index {n} used by {sorted(v)}' for n,v in users.items() if len(v)>1]
    enum=(d/'airconv_thunks.h').read_text(errors='replace')
    body=enum[enum.index('enum airconv_unixcalls'):];body=body[body.index('{')+1:body.index('}')]
    value=-1
    for item in [x.strip() for x in body.split(',') if x.strip()]:
        name,_,v=item.partition('=');name=name.strip().removeprefix('unix_')
        value=int(v.strip(),0) if v.strip() else value+1
        want=AIRCONV.get(name)
        if want is None:errors.append(f'airconv enum {name} has no expected table name')
        elif value>=len(funcs) or funcs[value]!=want:errors.append(f'airconv {name}={value}, table has {funcs[value] if value<len(funcs) else None}')
    if errors:
        print('winemetal unix-call table MISMATCH:',*errors,sep='\n  ');return 1
    print(f'winemetal unix-call table OK: {len(funcs)} entries, {sum(len(v) for v in users.values())} PE thunks, wow64 identical')
    return 0
if __name__=='__main__':sys.exit(main(sys.argv[1]))
