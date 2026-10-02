#!/usr/bin/env python3
"""Generate ABI-native command metadata from actual encoder switches.

Print to stdout; caller installs with its native edit API. Unknown pointer
fields or untyped cases fail rather than producing an incomplete schema.
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
header = (ROOT / 'src/winemetal/winemetal.h').read_text()
source = (ROOT / 'src/winemetal/unix/winemetal_unix.c').read_text()
structs = dict(re.findall(r'struct (wmtcmd_\w+) \{(.*?)\n\};', header, re.S))
payloads = {
    'wmtcmd_compute_setbytes': ('bytes', 'length', '1'),
    'wmtcmd_render_setbytes': ('bytes', 'length', '1'),
    'wmtcmd_render_setviewports': ('viewports', 'viewport_count', 'sizeof(struct WMTViewport)'),
    'wmtcmd_render_setscissorrects': ('scissor_rects', 'rect_count', 'sizeof(struct WMTScissorRect)'),
}


def generate():
    lines = ['/* Generated from encoder switches; regenerate after ABI changes. */',
             '#ifndef WMT_TRACE_COMMAND_SCHEMA_H', '#define WMT_TRACE_COMMAND_SCHEMA_H',
             '#include <stddef.h>',
             'struct wmt_trace_command_schema {',
             '  size_t size; unsigned handle_count; size_t handles[8];',
             '  size_t pointer_offset, count_offset, count_size, element_size;',
             '};',
             'static int wmt_trace_command_schema(unsigned encoder, unsigned type,',
             '    struct wmt_trace_command_schema *out) {',
             '  *out = (struct wmt_trace_command_schema){0};',
             '  switch (encoder) {']
    total = 0
    for encoder, kind in enumerate(['Blit', 'Compute', 'Render'], 1):
        start = source.index('_MTL' + kind + 'CommandEncoder_encodeCommands')
        end = source.find('\nstatic NTSTATUS', start + 10)
        body = source[start:end]
        cases = list(re.finditer(r'case (WMT\w+):', body))
        lines += [f'  case {encoder}: switch (type) {{',
                  '  case 0: out->size = sizeof(struct wmtcmd_base); return 1;']
        for index, case in enumerate(cases):
            if case[1] == 'WMTRenderCommandNop':
                continue
            block = body[case.end():cases[index + 1].start() if index + 1 < len(cases) else len(body)]
            match = re.search(r'struct (wmtcmd_\w+) \*body', block)
            if not match:
                raise ValueError('untyped command: ' + case[1])
            name = match[1]
            fields = structs[name]
            handles = re.findall(r'obj_handle_t\s+(\w+)\s*;', fields)
            pointers = re.findall(r'(?:WMTMemoryPointer|WMTConstMemoryPointer)\s+(\w+)\s*;', fields)
            pointers = [p for p in pointers if p != 'next']
            if len(handles) > 8 or '*' in fields:
                raise ValueError('unsupported layout: ' + name)
            expected = [payloads[name][0]] if name in payloads else []
            if pointers != expected:
                raise ValueError('unreviewed pointer: ' + name)
            offsets = ', '.join(f'offsetof(struct {name}, {field})' for field in handles) or '0'
            payload = '0, 0, 0, 0'
            if expected:
                pointer, count, size = payloads[name]
                count_type = re.search(r'(uint(?:8|16|32|64)_t)\s+' + count + r'\s*;', fields)
                if not count_type:
                    raise ValueError('unreviewed count type: ' + name)
                payload = f'offsetof(struct {name}, {pointer}), offsetof(struct {name}, {count}), sizeof({count_type[1]}), {size}'
            lines += [f'  case {case[1]}:',
                      '    *out = (struct wmt_trace_command_schema){sizeof(struct ' + name + '), ' + str(len(handles)) + ', {' + offsets + '}, ' + payload + '}; return 1;']
            total += 1
        lines += ['  default: return 0;', '  }']
    lines += ['  default: return 0;', '  }', '}', '#endif', f'/* {total} non-NOP opcode cases. */']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    print(generate(), end='')
