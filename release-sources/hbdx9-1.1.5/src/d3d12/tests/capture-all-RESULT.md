<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
<!-- Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md -->
# Capture-all implementation receipt

Schema v1 and production source frozen for parent build050, 2026-09-16.

Production changes owned by this task:
- `src/d3d12/d3d12_shader_capture.hpp`: new shared process-local direct-to-disk
  original capture, disabled unless BOTH environment variables enable it.
- `src/d3d12/d3d12_pipeline_graphics.cpp`: include, entry Scope before pointer
  initialization/object creation/validation, final HRESULT through finish().
- `src/d3d12/d3d12_pipeline_compute.cpp`: equivalent Scope and both return paths.

`d3d12_pipeline.hpp` was not changed by this task; existing changes preserved.
No runtime build, staging, or Wine launch performed.

Native verification: `node src/d3d12/tests/capture-all-run.cjs`

Result:
- 20 mocked-I/O scenarios PASS with AddressSanitizer and UndefinedBehaviorSanitizer.
- 10,277 JSONL records parse, ordered sequence 1..10277.
- 2055 unique shaders and PSO outcomes; every SHA1 recomputed independently.
- Both enable gates, all six stages, success/failure HRESULT, preserved LastError,
  exact-byte dedup even under forced SHA1 collision, partial writes, zero write,
  create/write/flush/close failures, no per-PSO I/O retries after storage failure,
  dedup read/size errors, per-original quota, unreadable original, unwind result,
  overlapping PSOs, and refusal to overwrite prior inventory tested.
- git diff --check passed for edited tracked integration files.

Production helper SHA256:
`c568bc2ea91df5539a247c84d2c866a412da4b5e1ec33c61964f770c4c069227`

Residual validation: parent owns Windows compilation, real Wine/Flash I/O,
host audit/export and an actual game run. Native mocks are not game proof.
The 16 GiB total and 512 MiB journal quota boundaries were code-reviewed, not
filled in tests. The 64 MiB per-original rejection was exercised. Unsupported
stream/parser/state-object/library routes are explicitly listed in the contract.
