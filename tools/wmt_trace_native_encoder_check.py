#!/usr/bin/env python3
"""Verify native blit/compute handlers remain byte-derived from Wine handlers."""
import pathlib
import re
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'src/winemetal/unix/winemetal_unix.c').read_text()
native = (root / 'src/winemetal/unix/wmt_trace_native_encoders.h').read_text()
for kind in ['Blit', 'Compute']:
    start = source.index('_MTL' + kind + 'CommandEncoder_encodeCommands(void')
    end = source.index('\nstatic NTSTATUS', start)
    body = source[start:end]
    body = body.replace('_MTL' + kind + 'CommandEncoder_encodeCommands(void *obj) {',
        'static void wmt_trace_native_' + kind.lower() + '(id<MTL' + kind +
        'CommandEncoder> encoder,\n    const struct wmtcmd_base *head) {')
    body = re.sub(r'  struct unixcall_generic_obj_cmd_noret \*params = obj;\n', '', body)
    body = re.sub(r'  wmt_trace_record_commands[^\n]*\n', '', body)
    body = body.replace('  const struct wmtcmd_base *next = params->cmd_head.ptr;',
                        '  const struct wmtcmd_base *next = head;')
    body = re.sub(r'  id<MTL' + kind + r'CommandEncoder> encoder = [^\n]*\n', '', body)
    body = body.replace('  return STATUS_SUCCESS;', '  return;')
    if body not in native:
        raise SystemExit('FAIL native selector drift: ' + kind)
    print('PASS exact selector bodies:', kind)
baseline = subprocess.run(['git', 'show', '1226f44:src/winemetal/unix/winemetal_unix.c'],
    cwd=root, check=True, capture_output=True, text=True).stdout
start = baseline.index('_MTLRenderCommandEncoder_encodeCommands(void')
end = baseline.index('\nstatic NTSTATUS', start)
body = baseline[start:end]
body = body.replace('_MTLRenderCommandEncoder_encodeCommands(void *obj) {',
    'static void wmt_trace_native_render(id<MTLRenderCommandEncoder> encoder,\n'
    '    const struct wmtcmd_base *head) {')
body = body.replace('  struct unixcall_generic_obj_cmd_noret *params = obj;\n', '')
body = body.replace('  const struct wmtcmd_base *next = params->cmd_head.ptr;',
                    '  const struct wmtcmd_base *next = head;')
body = body.replace('  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)params->encoder;\n', '')
body = body.replace('  return STATUS_SUCCESS;', '  return;')
metadata = '    case WMTRenderCommandTraceBufferRead: break; /* recorder metadata only */\n'
if metadata not in native:
    raise SystemExit('FAIL native render metadata must be an exact no-op')
if body not in native.replace(metadata, ''):
    raise SystemExit('FAIL native render selector drift from 1226f44')
current_start = source.index('_MTLRenderCommandEncoder_encodeCommands(void')
current_end = source.index('\nstatic NTSTATUS', current_start)
current_cases = set(re.findall(r'case (WMTRenderCommand\w+):', source[current_start:current_end]))
native_start = native.index('static void wmt_trace_native_render(')
native_cases = set(re.findall(r'case (WMTRenderCommand\w+):', native[native_start:]))
if current_cases != native_cases:
    raise SystemExit('FAIL product render case mismatch: ' + str(current_cases ^ native_cases))
print('PASS Render baseline selector bodies; product cases:', len(native_cases))
if re.search(r'\b(NTSTATUS|STATUS_SUCCESS|unixcall|winemac|ntdll)\b', native):
    raise SystemExit('FAIL Wine dependency in native executor')
print('PASS Wine-free source boundary (link dependency proof still required)')
