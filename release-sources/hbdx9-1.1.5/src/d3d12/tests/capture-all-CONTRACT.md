<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
<!-- Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md -->
# Run-owned original shader capture v1

Enable only with `MACRUNNER_DX12_FRAME_CAPTURE=1` AND a nonempty explicit
`MACRUNNER_DX12_SHADER_CAPTURE_DIR` Windows directory, already created by the host.
The ordinary mode is unchanged. No Wine launch/build/staging is owned here.

Each process creates `capture-p<PID>-<creation-time-high><creation-time-low>.jsonl`
using CREATE_NEW (no overwrite), and binary files with the same prefix followed
by `-s<unique-id>.bin`. JSONL is serialized, append-only, flushed per record.
Binary completion is announced only after complete write, flush and close.

Common fields: `v:1`, `pid`, monotonically increasing `seq`, `event`.
Events:
- `session.begin`: limits, covered stages and known unsupported routes.
- `pso.begin`: `pso` (process-local positive integer), `kind` graphics/compute,
  `owner` hex device pointer, `descriptor_present` boolean.
- `shader.begin`: `pso`, `stage` VS/PS/HS/DS/GS/CS, `length` original byte count.
- `shader.end`: same association plus `sha1` lowercase hex of ORIGINAL bytes,
  `file` basename, `deduplicated` boolean, or `empty:true` for zero-length stages.
- `capture.error`: `pso`, `stage`, `reason`, `win32_error`, `incomplete:true`.
- `pso.result`: `pso`, `hr` eight lowercase hex digits, or null if stack unwound
  before a normal HRESULT return. `returned` distinguishes those cases.
- `pso.end`: `pso`, cumulative `psos`, `shader_occurrences`, `unique_shaders`,
  `errors`, `suppressed_psos`, `blob_bytes`, `inventory_bytes`, `incomplete`.
- `session.end`: same cumulative fields, plus `open_psos`.

Dedup is within one process across all stages, by SHA1/length candidate lookup
followed by exact readback comparison; a collision produces another binary.
Every nonempty stage is attempted before any PSO validation/substitution.
Errors never affect the application's HRESULT or LastError. After persistent
storage failure no further per-PSO I/O retries; one final summary attempt at
normal shutdown is allowed. Emergency stderr has its own one-shot marker,
`dx12_shader_capture_incomplete`, independent of existing stderr budgets.

Declared quotas: 64 MiB per original, 16 GiB unique binary bytes, 512 MiB JSONL.
Quota failure is an explicit capture error/incomplete capture, never success.
There is no shader-count or PSO-count limit. A torn/missing record, missing
binary, nonmatching length/hash, unmatched begin/end, capture.error, null PSO
result, or missing session.end MUST NOT be reported as a complete session.
An abruptly terminated session may still contain a valid captured prefix.
The host owns hashing/verification and reports that distinction.

Counter details: `shader_occurrences` includes zero-length stage entries.
`inventory_bytes` is the byte count BEFORE the record containing that value.
`open_psos` may be nonzero in a normal per-PSO summary when another thread is
creating a PSO; that alone does not set `incomplete`. At session shutdown an
open PSO does mark the final summary incomplete. After storage shutdown,
`suppressed_psos` counts further calls whose shader bytes were not inspected.
The independent one-shot stderr incomplete marker must also be checked by the
host, particularly if the inventory itself could not be created/written.

Coverage: public graphics/compute helpers, including valid traditional
CreatePipelineState streams which dispatch into those helpers. Streams rejected
by the device parser BEFORE dispatch, AS/MS (unsupported by this frontend),
raytracing state objects, and pipeline-library loading are NOT covered. These
routes must be listed as unsupported, never silently included in a completeness
claim. The owned file set cannot instrument the device stream parser.
