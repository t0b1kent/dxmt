"""Canonicalize packed tessellation Location/Component interfaces to vec4.

Preserves logical lanes. Partial stores become individual scalar stores,
never vector read/modify/write (HS invocations may write disjoint lanes).
Only homogeneous 32-bit scalar/vector and fixed-size array interfaces are
accepted. Unsupported pointer escapes or qualifiers fail explicitly.
The original logical interface is returned for reflection/adapter work.
"""
from collections import defaultdict
import struct

class Unsupported(ValueError):
    pass

def pack(data):
    if len(data)<20 or len(data)%4: raise Unsupported('SPIRV length')
    words=list(struct.unpack('<%dI'%(len(data)//4),data))
    if words[0]!=0x07230203: raise Unsupported('SPIRV magic')
    instructions=[];i=5
    while i<len(words):
        n,op=words[i]>>16,words[i]&65535
        if n<1 or i+n>len(words):raise Unsupported('instruction bounds')
        instructions.append((op,words[i+1:i+n]));i+=n
    types={};decorations=defaultdict(list);variables={};constants={};value_type={}
    models=[]
    for op,x in instructions:
        if 19<=op<=32:types[x[0]]=(op,x[1:])
        if op==71:decorations[x[0]].append(x[1:])
        if op==59:variables[x[1]]=(x[0],x[2],x[3:])
        if op==43:constants[x[1]]=(x[0],x[2:])
        if op==15:models.append(x[0])
        # Index expressions have a type/result pair; literal-only instructions
        # do not overwrite valid result IDs in a well-formed module.
        if op in (43,45,50,61,65,66,79,80,81,82,83,124,128,130,132,169,245):
            value_type[x[1]]=x[0]
    if not any(m in (1,2) for m in models):return data,[]
    def deco(v,d,default=None):
        vals=[x[1:] for x in decorations[v] if x[0]==d]
        if len(vals)>1:raise Unsupported('duplicate decoration')
        return vals[0] if vals else default
    def shape(tp):
        arrays=[]
        while types[tp][0]==28:
            element,length=types[tp][1]
            if length not in constants:raise Unsupported('specialization array size')
            arrays.append(length);tp=element
        op,args=types[tp]
        scalar,width=(args[0],args[1]) if op==23 else (tp,1)
        if types[scalar][0] not in (21,22) or types[scalar][1][0]!=32:
            raise Unsupported('non-32-bit scalar interface')
        if width not in (1,2,3,4):raise Unsupported('vector width')
        return scalar,width,tuple(arrays)
    groups=defaultdict(list)
    info={}
    for var,(ptr,storage,initializer) in variables.items():
        loc=deco(var,30)
        if storage not in (1,3) or loc is None or deco(var,11) is not None:continue
        if initializer:raise Unsupported('initialized stage interface')
        if types[ptr][0]!=32 or types[ptr][1][0]!=storage:raise Unsupported('pointer storage mismatch')
        pointee=types[ptr][1][1]
        scalar,width,arrays=shape(pointee)
        component=deco(var,31,[0])[0]
        if component+width>4:raise Unsupported('component exceeds location')
        # Arrays on HS input/output and DS input are arrays of vertices, not
        # arrays consuming successive locations. Patch arrays require a
        # separate ABI and are not silently reinterpreted here.
        patch=deco(var,15) is not None
        if arrays and patch:raise Unsupported('patch array interface')
        if arrays and not (models==[1] or (models==[2] and storage==1)):
            raise Unsupported('non-per-vertex array interface')
        key=(storage,patch,loc[0])
        groups[key].append(var)
        info[var]=dict(pointer_type=ptr,pointee=pointee,scalar=scalar,width=width,
                       arrays=arrays,component=component,storage=storage,location=loc[0],patch=patch)
    groups={key:vs for key,vs in groups.items() if any(info[v]['component'] for v in vs)}
    if not groups:return data,[]
    next_id=words[3];added_types=[];added_constants=[];added_variables=[];added_decorations=[]
    def fresh():
        nonlocal next_id
        out=next_id;next_id+=1;return out
    cache={(op,tuple(args)):tp for tp,(op,args) in types.items()}
    def type_id(op,args):
        key=(op,tuple(args))
        if key not in cache:
            tp=fresh();cache[key]=tp;types[tp]=(op,list(args));added_types.append((op,[tp]+list(args)))
        return cache[key]
    uint=type_id(21,[32,0])
    constant_cache={(tp,tuple(lit)):ident for ident,(tp,lit) in constants.items()}
    def const(value,tp=None):
        tp=uint if tp is None else tp;key=(tp,(value,))
        if key not in constant_cache:
            ident=fresh();constant_cache[key]=ident;constants[ident]=(tp,[value]);value_type[ident]=tp
            added_constants.append((43,[tp,ident,value]))
        return constant_cache[key]
    pointers={};mapping=[];removed=set()
    for key,vs in sorted(groups.items()):
        seed=info[vs[0]];occupied=set();qualifiers=None
        for var in vs:
            item=info[var]
            if (item['scalar'],item['arrays'])!=(seed['scalar'],seed['arrays']):
                raise Unsupported('mixed scalar/array packing at location')
            lane_set=set(range(item['component'],item['component']+item['width']))
            if occupied&lane_set:raise Unsupported('overlapping logical components')
            occupied|=lane_set
            quals=sorted(tuple(x) for x in decorations[var] if x[0] not in (30,31))
            if qualifiers is not None and quals!=qualifiers:raise Unsupported('qualifier mismatch')
            qualifiers=quals
        vec=type_id(23,[seed['scalar'],4]);packed_type=vec
        for size in reversed(seed['arrays']):packed_type=type_id(28,[packed_type,size])
        ptr=type_id(32,[seed['storage'],packed_type]);packed_var=fresh()
        added_variables.append((59,[ptr,packed_var,seed['storage']]))
        added_decorations.append((71,[packed_var,30,seed['location']]))
        for qual in qualifiers:added_decorations.append((71,[packed_var]+list(qual)))
        for var in vs:
            item=info[var]
            pointers[var]=dict(var=packed_var,storage=item['storage'],scalar=item['scalar'],
                               pointee=item['pointee'],path=[],component=item['component'],lane=None)
            removed.add(var)
            mapping.append(dict(variable=var,packed_variable=packed_var,**item))
    function_start=next(i for i,(op,x) in enumerate(instructions) if op==54)
    function_ops=[]
    def component_id(pointer,extra,into):
        lane=pointer['lane']
        if lane is None:return const(pointer['component']+extra)
        offset=pointer['component']+extra
        if not offset:return lane
        if lane in constants:
            tp,lits=constants[lane]
            if len(lits)!=1:raise Unsupported('wide vector index')
            return const(lits[0]+offset,tp)
        if lane not in value_type:raise Unsupported('unknown vector index type')
        tp=value_type[lane]
        if types[tp][0]!=21 or types[tp][1][0]!=32:raise Unsupported('non32 vector index')
        ident=fresh();into.append((128,[tp,ident,lane,const(offset,tp)]));return ident
    def scalar_pointer(pointer,component,into):
        ptr=type_id(32,[pointer['storage'],pointer['scalar']]);ident=fresh()
        lane=component_id(pointer,component,into)
        into.append((65,[ptr,ident,pointer['var']]+pointer['path']+[lane]));return ident
    for op,x in instructions[function_start:]:
        if op in (65,66) and x[2] in pointers:
            old=pointers[x[2]];p=dict(old);p['path']=list(old['path']);tp=old['pointee']
            for index in x[3:]:
                type_op,args=types[tp]
                if type_op==28:
                    p['path'].append(index);tp=args[0]
                elif type_op==23:
                    if p['lane'] is not None:raise Unsupported('multiple vector indices')
                    p['lane']=index;tp=args[0]
                else:raise Unsupported('unexpected access chain shape')
            if types[x[0]]!=(32,[p['storage'],tp]):raise Unsupported('access-chain type mismatch')
            p['pointee']=tp;pointers[x[1]]=p;removed.add(x[1]);continue
        if op==61 and x[2] in pointers:
            if len(x)!=3:raise Unsupported('load memory operands')
            p=pointers[x[2]];tp=p['pointee'];kind,args=types[tp]
            if tp!=x[0]:raise Unsupported('load type mismatch')
            if kind in (21,22):
                address=scalar_pointer(p,0,function_ops)
                function_ops.append((61,[tp,x[1],address]))
            elif kind==23:
                components=[]
                for i in range(args[1]):
                    address=scalar_pointer(p,i,function_ops);ident=fresh()
                    function_ops.append((61,[args[0],ident,address]));components.append(ident)
                function_ops.append((80,[tp,x[1]]+components))
            else:raise Unsupported('whole-array interface load')
            continue
        if op==62 and x[0] in pointers:
            if len(x)!=2:raise Unsupported('store memory operands')
            p=pointers[x[0]];kind,args=types[p['pointee']]
            if kind in (21,22):
                address=scalar_pointer(p,0,function_ops);function_ops.append((62,[address,x[1]]))
            elif kind==23:
                for i in range(args[1]):
                    ident=fresh();function_ops.append((81,[args[0],ident,x[1],i]))
                    address=scalar_pointer(p,i,function_ops);function_ops.append((62,[address,ident]))
            else:raise Unsupported('whole-array interface store')
            continue
        # Only genuine ID operands are tested in common pointer escape sites.
        if op in (57,83,169,245,254) and any(v in pointers for v in x[2:] if op!=254):
            raise Unsupported('interface pointer escape opcode '+str(op))
        if op==254 and x[0] in pointers:raise Unsupported('return interface pointer')
        function_ops.append((op,x))
    header_ops=[]
    for op,x in instructions[:function_start]:
        if op in (5,71) and x[0] in removed:continue
        if op==59 and x[1] in removed:continue
        if op in (5632,5633) and x[0] in removed:raise Unsupported('string decoration requires logical reflection')
        if op==15:
            k=2
            while k<len(x) and not any(b==0 for b in struct.pack('<I',x[k])):k+=1
            if k>=len(x):raise Unsupported('entry name bounds')
            iface=[]
            for ident in x[k+1:]:
                mapped=pointers[ident]['var'] if ident in pointers else ident
                if mapped not in iface:iface.append(mapped)
            x=x[:k+1]+iface
        header_ops.append((op,x))
    # Annotations precede types; new types reference original constants/types.
    # Emit new types/constants/variables after original declarations and
    # before the first function. Pointer variables never depend on new constants.
    annotation_end=next(i for i,(op,x) in enumerate(header_ops) if 19<=op<=39)
    header_ops[annotation_end:annotation_end]=added_decorations
    result=header_ops+added_types+added_constants+added_variables+function_ops
    header=words[:5];header[3]=next_id
    flat=header[:]
    for op,x in result:flat += [((len(x)+1)<<16)|op]+x
    return struct.pack('<%dI'%len(flat),*flat),mapping
