"""Lower isoline HS/DS bodies to compute with an explicit 32-bit record ABI.

One workgroup evaluates one HS patch; each lane evaluates one output control
point and shares the HS output interface. One invocation evaluates one DS
domain point. The caller supplies each invocation's complete stage
inputs, including TessCoord and outer factors. Domain generation/line assembly
is a separate pipeline operation; changing isolines into quad patches is NOT
performed. This pass does not claim rasterization or hardware tessellation.
"""
import struct
from collections import defaultdict

class Unsupported(ValueError):pass

def lower(data, allow_vertex=False, allow_tessellation=False, pack_records=False):
    if len(data)<20 or len(data)%4:raise Unsupported('SPIRV length')
    w=list(struct.unpack('<%dI'%(len(data)//4),data));ins=[];i=5
    if w[0]!=0x07230203:raise Unsupported('SPIRV magic')
    while i<len(w):
        n,op=w[i]>>16,w[i]&65535
        if not n or i+n>len(w):raise Unsupported('instruction bounds')
        ins.append((op,list(w[i+1:i+n])));i+=n
    entries=[x for op,x in ins if op==15]
    if len(entries)!=1:raise Unsupported('single entry required')
    stage,main=entries[0][:2];modes=[x[1:] for op,x in ins if op==16 and x[0]==main]
    if stage==0:
        if not allow_vertex:return data,None
    elif stage not in (1,2) or (not allow_tessellation and [25] not in modes):return data,None
    domains={22:'TRIANGLES',24:'QUADS',25:'ISOLINES'}
    domain=next((domains[x[0]] for x in modes if x[0] in domains),None)
    if stage in (1,2) and domain is None:raise Unsupported('missing tessellation domain')
    output_points=next((x[1] for x in modes if x[0]==26),None) if stage==1 else 1
    if stage==1 and (output_points is None or not 1<=output_points<=32):raise Unsupported('HS output control points must be 1..32')
    has_barrier=any(op in (224,225) for op,x in ins)
    if has_barrier and stage!=1:raise Unsupported('barrier outside HS')
    shared_output=stage==1 and (output_points>1 or has_barrier)
    types={x[0]:(op,x[1:]) for op,x in ins if 19<=op<=33}
    consts={x[1]:(x[0],x[2:]) for op,x in ins if op==43};deco=defaultdict(list)
    for op,x in ins:
        if op==71:deco[x[0]].append(x[1:])
    vars={x[1]:(x[0],x[2],x[3:]) for op,x in ins if op==59}
    io={v:x for v,x in vars.items() if x[1] in (1,3)}
    if not io:raise Unsupported('no stage interface')
    nxt=w[3];newtypes=[];newconst=[];newvars=[];newdeco=[];newnames=[]
    def fresh():
        nonlocal nxt
        r=nxt;nxt+=1;return r
    # Old stage pointer IDs will become Private/Workgroup below. A new compute
    # builtin must keep Input storage, even when its pointee matches an old
    # InvocationId/vertex input. Never intern against those remapped pointers.
    cache={(op,tuple(x)):i for i,(op,x) in types.items()
           if op!=32 or x[0] not in (1,3)}
    def typ(op,x):
        k=(op,tuple(x))
        if k not in cache:
            n=fresh();cache[k]=n;types[n]=(op,x);newtypes.append((op,[n]+x))
        return cache[k]
    uint=typ(21,[32,0]);vec3=typ(23,[uint,3]);void=typ(19,[]);ft=typ(33,[void])
    constants={(t,tuple(x)):i for i,(t,x) in consts.items()}
    def constant(v):
        k=(uint,(v,))
        if k not in constants:
            n=fresh();constants[k]=n;newconst.append((43,[uint,n,v]));consts[n]=(uint,[v])
        return constants[k]
    def name(v,s):
        b=s.encode()+b'\0';b+=b'\0'*((-len(b))%4)
        newnames.append((5,[v]+list(struct.unpack('<%dI'%(len(b)//4),b))))
    all_iotypes=set()
    def leaves(t,path=()):
        all_iotypes.add(t);op,a=types[t]
        if op in (21,22):
            if a[0]!=32:raise Unsupported('only 32-bit interface scalars')
            return [(path,t)]
        if op in (23,24):children=[a[0]]*a[1]
        elif op==28:
            if a[1] not in consts:raise Unsupported('specialization array')
            length=consts[a[1]][1][0]
            if length>128:raise Unsupported('interface array cap128')
            children=[a[0]]*length
        elif op==30:children=a
        else:raise Unsupported('interface type '+str(op))
        out=[]
        for i,c in enumerate(children):out+=leaves(c,path+(i,))
        if len(out)>4096:raise Unsupported('record word cap4096')
        return out
    # Decorated aggregate types are not interchangeable by opcode/operands.
    # The incoming shader may already have uint[] with stride 4, 16, ...;
    # give the record ABI fresh array/struct IDs so no existing layout changes.
    runtime_array=fresh();types[runtime_array]=(29,[uint]);newtypes.append((29,[runtime_array,uint]))
    block=fresh();types[block]=(30,[runtime_array]);newtypes.append((30,[block,runtime_array]))
    blockptr=typ(32,[12,block]);wordptr=typ(32,[12,uint])
    newdeco += [(71,[runtime_array,6,4]),(71,[block,2]),(72,[block,0,35,0])]
    gidptr=typ(32,[1,vec3]);gid=fresh();newvars.append((59,[gidptr,gid,1]));newdeco.append((71,[gid,11,28]));name(gid,'mr_record_id')
    if shared_output:
        # Records are indexed by patch, never by individual control point.
        newdeco[-1]=(71,[gid,11,26]) # WorkgroupId
        lidptr=typ(32,[1,uint]);lid=fresh();newvars.append((59,[lidptr,lid,1]))
        newdeco.append((71,[lid,11,29]));name(lid,'mr_control_point_id')
        # Vulkan tessellation OutputMemory becomes compute WorkgroupMemory.
        for op,x in ins:
            if op not in (224,225):continue
            index=2 if op==224 else 1
            if x[index] not in consts:raise Unsupported('nonconstant barrier semantics')
            t,words=consts[x[index]]
            if t!=uint or len(words)!=1:raise Unsupported('barrier semantics type')
            semantics=words[0]
            x[index]=constant((semantics & ~0x1000) | (0x100 if semantics & 0x1000 else 0))
    bind=max([x[1] for ds in deco.values() for x in ds if x[0]==33]+[-1])+1
    abi=[];iv=None;buffers={}
    for v,(ptr,storage,init) in io.items():
        if init:raise Unsupported('initialized interface')
        pointee=types[ptr][1][1];ls=leaves(pointee)
        builtin=next((x[1] for x in deco[v] if x[0]==11),None)
        if stage==1 and builtin==8:
            if types[pointee][0]!=21:raise Unsupported('InvocationId type')
            iv=(v,pointee);continue
        group=storage if pack_records else v
        if group not in buffers:
            ssbo=fresh();newvars.append((59,[blockptr,ssbo,12]));newdeco += [(71,[ssbo,34,31]),(71,[ssbo,33,bind])]
            name(ssbo,f'mr_{"input" if storage==1 else "output"}_{v}')
            buffers[group]=dict(variable=ssbo,binding=bind,storage=storage,record_words=0)
            bind+=1
        buffer=buffers[group];ssbo=buffer['variable'];offset=buffer['record_words'];buffer['record_words']+=len(ls)
        abi.append(dict(variable=v,pointee=pointee,storage=storage,record_words=len(ls),
                        leaves=[dict(path=list(path),type=t) for path,t in ls],
                        decorations=deco[v],buffer_variable=ssbo,binding=buffer['binding'],descriptor_set=31,
                        record_offset_words=offset))
    sizes={b['variable']:b['record_words'] for b in buffers.values()}
    for a in abi:a['buffer_record_words']=sizes[a['buffer_variable']]
    body=[]
    def value(op,t,args):
        n=fresh();body.append((op,[t,n]+args));return n
    gidvalue=value(61,vec3,[gid]);record=value(81,uint,[gidvalue,0])
    invocation=value(61,uint,[lid]) if shared_output else constant(0)
    if iv:
        index=invocation
        if iv[1]!=uint:index=value(124,iv[1],[index])
        body.append((62,[iv[0],index]))
    def children(t):
        op,a=types[t]
        if op in (23,24):return [a[0]]*a[1]
        if op==28:return [a[0]]*consts[a[1]][1][0]
        if op==30:return a
        return []
    for a in abi:
        if a['storage']!=1:continue
        base=value(132,uint,[record,constant(a['buffer_record_words'])]);scalar_values={}
        for pos,l in enumerate(a['leaves']):
            index=value(128,uint,[base,constant(a['record_offset_words']+pos)]);ptr=value(65,wordptr,[a['buffer_variable'],constant(0),index]);n=value(61,uint,[ptr])
            if l['type']!=uint:n=value(124,l['type'],[n])
            scalar_values[tuple(l['path'])]=n
        def compose(t,path=()):
            cc=children(t)
            return value(80,t,[compose(c,path+(i,)) for i,c in enumerate(cc)]) if cc else scalar_values[path]
        body.append((62,[a['variable'],compose(a['pointee'])]))
    value(57,void,[main])
    if shared_output:
        # Every lane must finish before lane zero copies the complete patch.
        body.append((224,[constant(2),constant(2),constant(0x108)]))
        leader=value(170,typ(20,[]),[invocation,constant(0)])
        copy_label=fresh();merge_label=fresh()
        body += [(247,[merge_label,0]),(250,[leader,copy_label,merge_label]),(248,[copy_label])]
    for a in abi:
        if a['storage']!=3:continue
        composite=value(61,a['pointee'],[a['variable']]);base=value(132,uint,[record,constant(a['buffer_record_words'])])
        for pos,l in enumerate(a['leaves']):
            n=value(81,l['type'],[composite]+l['path']) if l['path'] else composite
            if l['type']!=uint:n=value(124,uint,[n])
            index=value(128,uint,[base,constant(a['record_offset_words']+pos)]);ptr=value(65,wordptr,[a['buffer_variable'],constant(0),index]);body.append((62,[ptr,n]))
    if shared_output:body += [(249,[merge_label]),(248,[merge_label])]
    wrapper=fresh();label=fresh();name(wrapper,'mr_isoline_compute')
    wrapperbody=[(54,[void,wrapper,0,ft]),(248,[label])]+body+[(253,[]),(56,[])]
    b=b'mr_isoline_compute\0';b+=b'\0'*((-len(b))%4);entryname=list(struct.unpack('<%dI'%(len(b)//4),b))
    # SPIR-V 1.4+ lists every entry global; 1.3 lists only Input/Output.
    interfaces=([gid,lid] if shared_output else [gid]) if w[1]<0x10400 else list(vars)+[x[1] for op,x in newvars]
    entry=(15,[5,wrapper]+entryname+interfaces)
    # Tessellation capability implicitly provides Shader; preserve the latter
    # when stripping the stage capability (glslang often declares only Tessellation).
    out=[] if any(op==17 and x==[1] for op,x in ins) else [(17,[1])]
    names_done=deco_done=globals_done=False
    def before_functions():
        return newtypes+newconst+newvars
    for op,x in ins:
        if op==17 and x==[3]:continue
        if op==16 and x[0]==main:continue
        if op==15:
            out += [entry,(16,[wrapper,17,output_points if shared_output else 1,1,1])];continue
        if not names_done and (op in (71,72) or 19<=op<=39 or op in (41,42,43,44,45,46,48,49,50,51,52,59,54)):
            out+=newnames;names_done=True
        if not deco_done and (19<=op<=39 or op in (41,42,43,44,45,46,48,49,50,51,52,59,54)):
            out+=newdeco;deco_done=True
        if op==54 and not globals_done:
            out+=before_functions();globals_done=True
        if op==71 and x[0] in io and x[1] in (11,13,14,15,16,17,18,30,31):continue
        if op==72 and x[0] in all_iotypes and x[2] in (11,13,14,15,16,17,18,30,31):continue
        if op==32 and x[1] in (1,3):x=[x[0],4 if x[1]==3 and shared_output else 6,x[2]]
        if op==59 and x[1] in io:x=[x[0],x[1],4 if x[2]==3 and shared_output else 6]+x[3:]
        out.append((op,x))
    out+=wrapperbody;ww=w[:5];ww[3]=nxt
    # StorageBuffer is core in 1.3; public glslang fixtures may start at 1.0.
    ww[1]=max(ww[1],0x10300)
    for op,x in out:ww += [((len(x)+1)<<16)|op]+x
    return struct.pack('<%dI'%len(ww),*ww),dict(version=2 if pack_records else 1,source_stage={0:'VS',1:'HS',2:'DS'}[stage],
        physical_stage='CS',domain=domain,invocations_per_group=output_points if shared_output else 1,source_modes=modes,
        input_contract='one complete logical stage record per workgroup' if shared_output else 'one complete logical stage record per dispatch invocation',
        output_synchronization='workgroup barrier then leader copy' if shared_output else 'single invocation',
        domain_generation='NOT_INCLUDED',primitive_assembly='NOT_INCLUDED',record_packing='per_storage' if pack_records else 'per_variable',
        buffers=list(buffers.values()),records=abi)
