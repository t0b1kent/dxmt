"""SPIR-V descriptor de-aliasing; adaptation of SHADERS c-bindings.py.

Preserves the original set/binding/variable mapping for the eventual D3D12
adapter. A new physical binding is not a new D3D register. This module does
not claim to recover D3D register class from an ambiguous SPIR-V binding.
"""
from collections import defaultdict
import struct

def remap(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError('invalid SPIR-V length')
    words = list(struct.unpack('<%dI' % (len(data)//4), data))
    if words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V magic')
    i = 5
    sets, bindings, storage = {}, {}, {}
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 65535
        if not count or i + count > len(words):
            raise ValueError('invalid instruction bounds')
        if op == 71 and count == 4:
            target, decoration, value = words[i+1:i+4]
            if decoration == 34:
                if target in sets:
                    raise ValueError('duplicate DescriptorSet')
                sets[target] = value
            if decoration == 33:
                if target in bindings:
                    raise ValueError('duplicate Binding')
                bindings[target] = (value, i+3)
        if op == 59 and count >= 4:
            storage[words[i+2]] = words[i+3]
        i += count
    groups, next_binding = defaultdict(list), defaultdict(int)
    for var, (binding, offset) in bindings.items():
        if var not in storage:
            raise ValueError('binding does not name a variable')
        desc_set = sets.get(var, 0)
        groups[(desc_set, binding)].append(var)
        next_binding[desc_set] = max(next_binding[desc_set], binding+1)
    mapping, edits = [], []
    for (desc_set, binding), ids in sorted(groups.items()):
        for index, var in enumerate(sorted(ids)):
            target = binding
            if index:
                target = next_binding[desc_set]
                if target > 0xffffffff:
                    raise ValueError('binding namespace exhausted')
                next_binding[desc_set] += 1
                offset = bindings[var][1]
                words[offset] = target
                edits.append(offset)
            mapping.append(dict(variable=var, descriptor_set=desc_set,
                                logical_binding=binding, physical_binding=target,
                                storage_class=storage[var]))
    original = struct.unpack('<%dI' % len(words), data)
    assert {i for i,(a,b) in enumerate(zip(original,words)) if a != b} == set(edits)
    assert len({(m['descriptor_set'],m['physical_binding']) for m in mapping}) == len(mapping)
    return struct.pack('<%dI' % len(words), *words), mapping
