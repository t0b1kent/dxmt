#!/usr/bin/env python3
"""One-line Metal source fix for the native DXMT scenarios (REPRO-110).

The pinned native basis (1226f44e plus the recipe's basis-overlay) calls the compiler builtin
__metal_atomic_fetch_add_explicit with four arguments in src/airconv/shaders/air_tessellation.metal. The Metal compiler of
the cloud image (Apple metal version 32023.921, metalfe-32023.921.6, Metal Toolchain 27.1.266.1) requires five and stops the
build there (runs 38504891301, native-r2 and native-pe). The accepted composition was compiled by metalfe-32023.883, which is
the version string embedded in its winemetal.so and d3d11.dll. The public fork already carries the change that fixes it:
upstream commit 06065754af91 (13.06.2026, "fix compile metal shader with toolchain from XCode 27 beta 1"), which replaces the
builtin by the standard atomic_fetch_add_explicit. This module applies exactly that line, to the one pinned file, after
checking its bytes, and records both digests. Checked on 14.10.2026 with metalfe-32023.921.6: the pinned file fails with the
cloud's error, the fixed file compiles (air64-apple-macos14.0, -std=metal3.1).
"""
import hashlib
from pathlib import Path

PATH = 'src/airconv/shaders/air_tessellation.metal'
PRE_SHA256 = 'b83e2465b8936340a3ea7336d70949a1706e15878ffa7eb4588a3e789cc6e52b'
POST_SHA256 = 'aa7b3e264bf91cd885b5981805878cadb3b1f9f587a5131940ae9ee669a0fbca'
OLD = '  return __metal_atomic_fetch_add_explicit(out_count, 1, int(memory_order_relaxed), __METAL_MEMORY_SCOPE_THREADGROUP__);\n'
NEW = '  return atomic_fetch_add_explicit(reinterpret_cast<threadgroup atomic_int *>(out_count), 1, memory_order_relaxed);\n'
UPSTREAM = '06065754af91352caaf8db87d4566fecc4124552'


def apply(source_root):
    """Fix the prepared source tree in place; return a report, or None when the tree has no airconv shader at all."""
    path = Path(source_root) / PATH
    if not path.is_file() or path.is_symlink():
        return None
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != PRE_SHA256:
        raise ValueError('Tessellation shader differs from the pinned bytes')
    text = data.decode('utf-8')
    if text.count(OLD) != 1:
        raise ValueError('Tessellation shader must contain the pinned call exactly once')
    out = text.replace(OLD, NEW).encode('utf-8')
    if hashlib.sha256(out).hexdigest() != POST_SHA256:
        raise ValueError('Fixed tessellation shader differs from the expected bytes')
    path.write_bytes(out)
    return dict(path=PATH, upstream_commit=UPSTREAM, before=dict(bytes=len(data), sha256=PRE_SHA256),
                after=dict(bytes=len(out), sha256=POST_SHA256), changed_lines=1)
