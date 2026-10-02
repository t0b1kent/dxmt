"""Decode raw non-interlaced PNG8/16 samples with bounded CRC-checked parsing.

No color management, premultiplication or rounding. Unsupported forms fail closed.
The tiny C helper only reconstructs scanlines (filters 0..4).
"""
import ctypes
import functools
import hashlib
import struct
import zlib
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
MAX_BYTES = 512 * 1024 * 1024

@functools.lru_cache(maxsize=1)
def unfilter():
    lib = ctypes.CDLL(str(HERE / 'png_unfilter.dylib'))
    fn = lib.png_unfilter
    fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                   ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t]
    fn.restype = ctypes.c_int
    return fn

def decode(path):
    if path.stat().st_size > MAX_BYTES:
        raise ValueError('PNG file exceeds bounded decoder limit')
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('invalid PNG signature')
    pos, header, payload, ended, transparent = 8, None, [], False, None
    seen_idat = False
    while pos < len(data):
        if pos + 12 > len(data): raise ValueError('truncated PNG chunk')
        length, kind = struct.unpack_from('>I4s', data, pos)
        end = pos + 12 + length
        if end > len(data): raise ValueError('PNG chunk exceeds file')
        chunk = data[pos+8:pos+8+length]
        crc = struct.unpack_from('>I', data, pos+8+length)[0]
        if zlib.crc32(kind+chunk) & 0xffffffff != crc: raise ValueError('PNG CRC mismatch')
        if header is None and kind != b'IHDR': raise ValueError('IHDR must be first')
        if kind == b'IHDR':
            if header is not None or length != 13: raise ValueError('invalid IHDR')
            header = struct.unpack('>IIBBBBB', chunk)
        elif kind == b'IDAT':
            seen_idat = True
            payload.append(chunk)
        elif kind == b'IEND':
            if length or end != len(data): raise ValueError('invalid IEND/trailing bytes')
            ended = True
            break
        elif kind == b'tRNS':
            if seen_idat or transparent is not None: raise ValueError('invalid tRNS')
            transparent = chunk
        elif kind in (b'acTL', b'fcTL', b'fdAT'):
            raise ValueError('animated PNG is not a single frame')
        elif kind[0] & 32 == 0 and kind != b'PLTE':
            raise ValueError('unknown critical PNG chunk')
        pos = end
    if not ended or not seen_idat: raise ValueError('incomplete PNG')
    width, height, depth, color, compression, filtering, interlace = header
    if depth not in (8, 16) or color not in (0, 2, 4, 6):
        raise ValueError('full-depth supports PNG8/16 gray, gray-alpha, RGB, RGBA only')
    if compression or filtering or interlace:
        raise ValueError('unsupported compression/filter/interlaced PNG')
    channels = {0:1, 2:3, 4:2, 6:4}[color]
    bpp = channels * (depth//8)
    stride = width*bpp
    expected = height*(stride+1)
    if not width or not height or expected > MAX_BYTES: raise ValueError('PNG dimensions exceed bound')
    inflater = zlib.decompressobj()
    raw = inflater.decompress(b''.join(payload), expected+1)
    if len(raw) != expected or not inflater.eof or inflater.unused_data or inflater.unconsumed_tail:
        raise ValueError('PNG decompressed extent mismatch')
    decoded = np.empty(height*stride, dtype=np.uint8)
    if unfilter()(raw, len(raw), decoded.ctypes.data, stride, height, bpp):
        raise ValueError('invalid PNG scanline filter/extent')
    samples = decoded.view('>u2' if depth == 16 else np.uint8).reshape(height,width,channels)
    maximum = (1 << depth)-1
    rgba = np.empty((height,width,4), dtype=np.uint16 if depth == 16 else np.uint8)
    rgba[:,:,:3] = samples[:,:,:1] if color in (0,4) else samples[:,:,:3]
    rgba[:,:,3] = samples[:,:,-1] if color in (4,6) else maximum
    if transparent is not None:
        if color not in (0,2) or len(transparent) != (2 if color == 0 else 6):
            raise ValueError('invalid transparency extent/color')
        key = np.array(struct.unpack('>'+'H'*(1 if color == 0 else 3), transparent))
        rgba[:,:,3][np.all(samples == key,axis=2)] = 0
    return rgba

def provenance():
    return {name:hashlib.sha256((HERE/name).read_bytes()).hexdigest()
            for name in ['png_full_depth.py','png_unfilter.c','png_unfilter.dylib']}
