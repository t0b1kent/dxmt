"""Проходы SPIR-V для вариантов пиксельной функции (веха допуска).

dual_source: двухисточниковое смешивание, как у прежнего пути (airconv/dxbc_signature.cpp):
SV_Target0 -> color(0), index(0); SV_Target1 -> color(0), index(1); выходы с номером >= 2 прежний
путь выбрасывает — здесь они остаются со своим номером (у конвейера с двухисточниковым смешиванием
цель одна, слой отказывает на второй: InitializePSO).
"""
import struct

OP_DECORATE, OP_VARIABLE, OP_FUNCTION = 71, 59, 54
DEC_LOCATION, DEC_INDEX = 30, 32
STORAGE_OUTPUT = 3


def dual_source(data):
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    outputs = set()
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xffff
        if not count:
            raise ValueError('SPIR-V bounds')
        if op == OP_VARIABLE and words[i + 3] == STORAGE_OUTPUT:
            outputs.add(words[i + 2])
        if op == OP_FUNCTION:
            break
        i += count
    out = words[:5]
    i = 5
    changed = 0
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xffff
        inst = words[i:i + count]
        if op == OP_DECORATE and count == 4 and inst[2] == DEC_LOCATION and inst[1] in outputs and inst[3] < 2:
            source = inst[3]
            out += [inst[0], inst[1], DEC_LOCATION, 0, (4 << 16) | OP_DECORATE, inst[1], DEC_INDEX, source]
            changed += 1
        elif op == OP_DECORATE and count == 4 and inst[2] == DEC_INDEX and inst[1] in outputs:
            raise ValueError('output already has an Index decoration')
        else:
            out += inst
        i += count
    return struct.pack('<%dI' % len(out), *out), changed
