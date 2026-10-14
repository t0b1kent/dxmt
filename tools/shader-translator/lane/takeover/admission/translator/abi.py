"""Соглашение прежнего пути слоя D3D12 (resources/ABI.md) для своего перевода DXIL -> MSL.

Здесь только разбор и раскладка, без запуска программ:
  dxbc_chunks      части контейнера DXBC/DXIL
  parse_rts0       сериализованная корневая подпись (часть RTS0, версии 1.0–1.2)
  root_layout      слово (qword) каждого параметра корня — как CalculateRootSignatureQwordOffsets слоя
  map_resources    ресурс шейдера (класс, пространство, регистр) -> место в корне или в таблице описателей,
                   теми же правилами, что airconv (src/airconv/dxbc_binding_rootsig.cpp:429–609)
  parse_signature  подпись входа/выхода стадии (ISG1/ISGN, OSG1/OSGN)
  write_abi_file   описание для spirv-cross --msl-root-abi
"""
import hashlib
import struct

TABLE, CONSTANTS, ROOT_CBV, ROOT_SRV, ROOT_UAV = range(5)          # D3D12_ROOT_PARAMETER_TYPE
SRV, UAV, CBV, SAMPLER = range(4)                                   # D3D12_DESCRIPTOR_RANGE_TYPE = класс ресурса
WHERE_TABLE, WHERE_ROOT, WHERE_CONSTANTS, WHERE_STATIC = range(4)   # spirv-cross --msl-root-abi
VISIBILITY = dict(VS=1, HS=2, DS=3, GS=4, PS=5)                     # D3D12_SHADER_VISIBILITY; 0 = все стадии
UNBOUNDED = 0xffffffff
CLASS_NAME = {SRV: 'srv', UAV: 'uav', CBV: 'cbv', SAMPLER: 'sampler'}


class AbiError(Exception):
    pass


def dxbc_chunks(data):
    if data[:4] != b'DXBC':
        raise AbiError('not a DXBC container')
    count = struct.unpack_from('<I', data, 28)[0]
    out = {}
    for off in struct.unpack_from('<%dI' % count, data, 32):
        size = struct.unpack_from('<I', data, off + 4)[0]
        out[data[off:off + 4].decode('latin1')] = data[off + 8:off + 8 + size]
    return out


def rts0_of(blob):
    """RTS0 из контейнера (шейдер со встроенной подписью или результат D3D12Serialize*RootSignature);
    голая часть RTS0 возвращается как есть."""
    if blob[:4] == b'DXBC':
        chunks = dxbc_chunks(blob)
        if 'RTS0' not in chunks:
            raise AbiError('container has no RTS0 part')
        return chunks['RTS0']
    return blob


def root_key(rts0):
    """Ключ корневой подписи для слоя: SHA-1 части RTS0."""
    return hashlib.sha1(rts0).hexdigest()


def parse_rts0(p):
    if len(p) < 24:
        raise AbiError('RTS0 too short')
    version, nparam, param_off, nstatic, static_off, flags = struct.unpack_from('<6I', p, 0)
    if version not in (1, 2, 3):
        raise AbiError('RTS0 version %d' % version)
    params = []
    for i in range(nparam):
        kind, visibility, off = struct.unpack_from('<3I', p, param_off + 12 * i)
        row = dict(type=kind, visibility=visibility)
        if kind == TABLE:
            nranges, ranges_off = struct.unpack_from('<2I', p, off)
            ranges = []
            for j in range(nranges):
                if version == 1:
                    rtype, num, base, space, offset = struct.unpack_from('<5I', p, ranges_off + 20 * j)
                else:
                    rtype, num, base, space, _flags, offset = struct.unpack_from('<6I', p, ranges_off + 24 * j)
                ranges.append(dict(type=rtype, num=num, base=base, space=space, offset=offset))
            row['ranges'] = ranges
        elif kind == CONSTANTS:
            row['register'], row['space'], row['count'] = struct.unpack_from('<3I', p, off)
        elif kind in (ROOT_CBV, ROOT_SRV, ROOT_UAV):
            row['register'], row['space'] = struct.unpack_from('<2I', p, off)
        else:
            raise AbiError('root parameter type %d' % kind)
        params.append(row)
    statics = []
    stride = 56 if version == 3 else 52
    for i in range(nstatic):
        f = struct.unpack_from('<4If3I2f3I', p, static_off + stride * i)
        statics.append(dict(register=f[10], space=f[11], visibility=f[12]))
    return dict(version=version, flags=flags, parameters=params, static_samplers=statics)


def root_layout(rs):
    """Слова корня: таблица и корневой описатель — одно, константы — (n + 1) // 2."""
    at = 0
    for p in rs['parameters']:
        p['qword'] = at
        at += (p['count'] + 1) // 2 if p['type'] == CONSTANTS else 1
    rs['qwords'] = at
    return rs


def _visible(visibility, stage):
    return visibility == 0 or visibility == VISIBILITY.get(stage)


def _contained(reg, size, rng):
    if reg < rng['base']:
        return False
    if rng['num'] == UNBOUNDED or size == UNBOUNDED:
        return True
    return reg + size <= rng['base'] + rng['num']


def map_resources(rs, stage, resources):
    """resources: [{class, space, register, count, binding}] (карта своего разбора DXIL).
    Возвращает {binding: {where, qword, index}} и список ненайденных в корне.
    Порядок и правила совпадают с airconv: параметры по порядку, позднее совпадение сильнее раннего,
    статические сэмплеры сильнее всех."""
    found = {}
    for p in rs['parameters']:
        if not _visible(p['visibility'], stage):
            continue
        if p['type'] == TABLE:
            running = 0
            for rng in p['ranges']:
                start = running if rng['offset'] == UNBOUNDED else rng['offset']
                running = (start + rng['num']) & 0xffffffff
                for r in resources:
                    if r['class'] == rng['type'] and r['space'] == rng['space'] and \
                            _contained(r['register'], r['count'], rng):
                        found[r['binding']] = dict(where=WHERE_TABLE, qword=p['qword'],
                                                   index=start + r['register'] - rng['base'])
        elif p['type'] == CONSTANTS:
            for r in resources:
                if r['class'] == CBV and r['register'] == p['register'] and r['space'] == p['space']:
                    found[r['binding']] = dict(where=WHERE_CONSTANTS, qword=p['qword'], index=0)
        else:
            cls = {ROOT_CBV: CBV, ROOT_SRV: SRV, ROOT_UAV: UAV}[p['type']]
            for r in resources:
                if r['class'] == cls and r['register'] == p['register'] and r['space'] == p['space']:
                    found[r['binding']] = dict(where=WHERE_ROOT, qword=p['qword'], index=0)
    for i, s in enumerate(rs['static_samplers']):
        for r in resources:
            if r['class'] == SAMPLER and r['space'] == s['space'] and r['register'] == s['register']:
                if not _visible(s['visibility'], stage):
                    break                      # как в airconv: первая невидимая запись прерывает поиск
                found[r['binding']] = dict(where=WHERE_STATIC, qword=0, index=i)
    missing = [r for r in resources if r['binding'] not in found]
    return found, missing


def parse_signature(chunk, extended):
    """ISG1/OSG1 (extended) или ISGN/OSGN: [{semantic, index, system_value, component_type, register, mask}]."""
    count, _offset = struct.unpack_from('<2I', chunk, 0)
    out = []
    size = 32 if extended else 24
    for i in range(count):
        base = 8 + size * i
        if extended:
            _stream, name_off, index, sysval, comp, reg, mask = struct.unpack_from('<6IB', chunk, base)
        else:
            name_off, index, sysval, comp, reg, mask = struct.unpack_from('<5IB', chunk, base)
        end = chunk.index(b'\0', name_off)
        out.append(dict(semantic=chunk[name_off:end].decode('latin1'), index=index, system_value=sysval,
                        component_type=comp, register=reg, mask=mask))
    return out


def input_signature(container):
    chunks = dxbc_chunks(container)
    if 'ISG1' in chunks:
        return parse_signature(chunks['ISG1'], True)
    if 'ISGN' in chunks:
        return parse_signature(chunks['ISGN'], False)
    return []


def output_signature(container):
    chunks = dxbc_chunks(container)
    if 'OSG1' in chunks:
        return parse_signature(chunks['OSG1'], True)
    if 'OSGN' in chunks:
        return parse_signature(chunks['OSGN'], False)
    return []


def pixel_outputs(container):
    """Что пиксельный шейдер объявляет на выходе: (пишет ли сам покрытие, типы восьми целей).
    Тип цели — как у прежнего пути (тип элемента подписи): 'u', 'i', 'f'; '-' — цель не пишется."""
    colors = ['-'] * 8
    coverage = 0
    for e in output_signature(container):
        name = e['semantic'].upper()
        if name == 'SV_COVERAGE':
            coverage = 1
        elif name == 'SV_TARGET' and e['index'] < 8:
            colors[e['index']] = {1: 'u', 2: 'i', 3: 'f'}.get(e['component_type'], 'f')
    return coverage, ''.join(colors)


def write_abi_file(path, rs, mapping, vertex_pull, prelude, vertex_prelude, sample_mask_constant=False):
    lines = ['qwords %d' % (rs['qwords'] if rs else 0), 'vertex_pull %d' % (1 if vertex_pull else 0),
             'prelude %s' % prelude, 'vertex_prelude %s' % vertex_prelude,
             'sample_mask_constant %d' % (1 if sample_mask_constant else 0)]
    for binding in sorted(mapping):
        m = mapping[binding]
        lines.append('res %d %d %d %d %d' % (binding, m['where'], m['qword'], m['index'], m.get('counter', 0)))
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
