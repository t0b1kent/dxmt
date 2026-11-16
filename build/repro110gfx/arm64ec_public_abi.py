#!/usr/bin/env python3
"""Reject struct-return entry thunks selected for ordinary Windows COM methods.

This is a focused regression gate, not a complete ARM64EC ABI verifier. The
selected methods return an integer or HRESULT and keep their first arguments
in x0..x3; the entry thunk must preserve them and forward scalar x0 to x8.
"""
import json
from pathlib import Path
import re
import struct


def thunk_contract(words):
    called = False
    forwarded = False
    for word in words:
        if word == 0xd63f0120:  # blr x9: invoke the selected ARM64EC function.
            called = True
            continue
        if word & 0xfffffc1f == 0xd61f0000:  # br Xn: dispatch back to x64.
            break
        if word & 0xffe0ffe0 == 0xaa0003e0:  # mov Xd, Xm (ORR alias).
            destination, source = word & 31, (word >> 16) & 31
            if not called and destination < 4 and destination != source:
                raise ValueError('Entry thunk shifts a Windows argument before the native call')
            if called and destination == 8:
                if source != 0:
                    raise ValueError('Entry thunk returns a saved struct pointer instead of the scalar result')
                forwarded = True
    if not called or not forwarded:
        raise ValueError('Ordinary scalar entry/return contract not found')


def image_symbols(data):
    if data[:2] != b'MZ' or len(data) < 64:
        raise ValueError('Not a PE image')
    header = struct.unpack_from('<I', data, 60)[0]
    if data[header:header + 4] != b'PE\0\0':
        raise ValueError('PE signature missing')
    machine, count, _, table, entries, optional, _ = struct.unpack_from('<HHIIIHH', data, header + 4)
    if machine != 0x8664 or not entries or table + entries * 18 + 4 > len(data):
        raise ValueError('ARM64EC PE symbols required for the public COM gate')
    sections = []
    for index in range(count):
        offset = header + 24 + optional + index * 40
        _, virtual_size, rva, size, raw, _, _, _, _, flags = struct.unpack_from('<8sIIIIIIHHI', data, offset)
        if raw + size > len(data):
            raise ValueError('PE section outside the image')
        sections.append((rva, virtual_size, raw, size, flags))

    def read(rva, size):
        for start, _, raw, length, flags in sections:
            if flags & 0x20000000 and start <= rva and size <= length - (rva - start):
                return data[raw + rva - start:raw + rva - start + size]
        raise ValueError('Entry thunk or function prefix outside executable bytes')

    strings = table + entries * 18
    string_size = struct.unpack_from('<I', data, strings)[0]
    if strings + string_size > len(data):
        raise ValueError('PE string table outside the image')
    symbols = {}
    index = 0
    while index < entries:
        name, value, section, _, _, auxiliary = struct.unpack_from('<8sIhHBB', data, table + index * 18)
        if name[:4] == bytes(4):
            offset = struct.unpack_from('<I', name, 4)[0]
            if offset < 4 or offset >= string_size:
                raise ValueError('PE symbol name outside the string table')
            end = data.find(b'\0', strings + offset, strings + string_size)
            if end < 0:
                raise ValueError('Unterminated PE symbol name')
            name = data[strings + offset:end]
        else:
            name = name.rstrip(b'\0')
        if 0 < section <= len(sections):
            symbols[name.decode('utf-8', errors='strict')] = sections[section - 1][0] + value
        index += 1 + auxiliary
    return symbols, read


def check_image(path):
    path = Path(path)
    names = ({'EnumAdapters', 'EnumAdapters1', 'QueryInterface', 'CreateDXGIFactory2'}
             if path.name.lower() == 'dxgi.dll'
             else {'QueryInterface', 'CheckFeatureSupport', 'CreateCommandQueue', 'GetNodeCount'})
    symbols, read = image_symbols(path.read_bytes())
    thunks = {rva for name, rva in symbols.items() if name.startswith('$ientry_thunk$')}
    checks = []
    covered = set()
    for symbol, rva in symbols.items():
        if not symbol.startswith('#') or '$' in symbol:
            continue
        method = next((name for name in sorted(names, key=len, reverse=True) if re.search(r'(?<![A-Za-z])' + name, symbol)
                       or re.search(r'\d' + name + r'(?:[A-Z_a-z0-9])', symbol)), None)
        if method is None:
            continue
        delta = struct.unpack('<i', read(rva - 4, 4))[0] & ~3
        entry = rva + delta
        if entry not in thunks:
            raise ValueError('Public method has no recognized entry thunk: ' + symbol)
        words = struct.unpack('<64I', read(entry, 256))
        try:
            thunk_contract(words)
        except ValueError as error:
            raise ValueError(symbol + ' at ' + hex(rva) + ', thunk ' + hex(entry) + ': ' + str(error)) from error
        covered.add(method)
        checks.append(dict(symbol=symbol, function_rva=rva, entry_thunk_rva=entry))
    required = {'QueryInterface', 'EnumAdapters'} if path.name.lower() == 'dxgi.dll' else {'QueryInterface', 'CreateCommandQueue'}
    if not required <= covered:
        raise ValueError('Required ordinary Windows COM methods not checked')
    return dict(status='ORDINARY_WINDOWS_COM_ENTRY_CONTRACT_PASS', methods=sorted(covered),
                checks=checks, scope='Argument shuffle and scalar return regression only')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    print(json.dumps(check_image(args.image), indent=2))
