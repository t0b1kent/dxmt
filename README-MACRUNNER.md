# DXMT with MacRunner D3D12 extensions (unofficial fork)

**This is an unofficial fork of [DXMT](https://github.com/3Shain/dxmt).** It is
not affiliated with, endorsed by, or supported by the DXMT project, its author
3Shain (Feifan He) or CodeWeavers. Please do not report problems with this fork
to the upstream project.

- Upstream base: DXMT `ef116534b2af7037c670ede3297e3ee74b45058d` (2026-09-14,
  "feat(d3d11): introduce `d3d11.maxTessFactor` option").
- Branch: `macrunner-d3d12`. Everything outside the files listed in
  [CHANGES-MACRUNNER.md](CHANGES-MACRUNNER.md) is upstream DXMT, unchanged.
- License: LGPL-2.1-or-later, the same as upstream (see `LICENSE` and
  `COPYING.LIB`). Upstream copyright notices are kept; every file in `src/`
  that this fork modified or added carries a dated change notice.

## Why a fork, and why it is not submitted upstream

### Optional bounded diagnostic counters (2026-10-03)

Root signatures now reject ambiguous shader-register bindings across tables,
root descriptors/constants and static samplers, accounting for resource type,
register space and intersecting shader visibility. Bounded register/descriptor
offset overflow, empty ranges, and APPEND after an unbounded range are rejected.
Physical heap aliasing remains legal; distinct visibility/space stays legal.
Both root-signature versions 1.0 and 1.1 share this validation.

`MACRUNNER_DX12_COUNTERS=1` records process-wide residency mutex acquisitions,
failed `try_lock` observations, QPC acquisition/hold ticks, both CPU GPUVA lookup
paths, explicit root-state reset bytes, actual root-upload bytes, and all ordinary
SM50 compiler entry points. The device destructor prints the cumulative counts.
The default is off. These measurements include observer overhead and are not
production performance comparisons. Native-wave and external-artifact compiler
paths are outside the SM50 counters. Reset bytes exclude initial object construction;
upload bytes exclude heap alignment padding. Multiple devices share the counters.
`MACRUNNER_DX12_FRAME_TRACE=1` retains its bounded sampling and adds thread, QPC,
frequency and UTC anchors to each emitted event.

MacRunner runs x86-64 Windows games on Apple Silicon Macs. It uses DXMT's
experimental, opt-in D3D12 frontend (meson option `enable_d3d12`) and extends
it here, together with the parts of the shader converter (`airconv`) and of
`winemetal` that the frontend depends on.

These changes were developed with AI assistance. The upstream contributing
guide (`CONTRIBUTING.md`, "AI Policy") does not accept contributions made or
co-authored by AI/LLMs, so this work is **not** submitted upstream and must not
be offered to DXMT as a pull request. It is published separately to comply with
the LGPL and so the work is not lost.

## What the fork adds (summary of the diff against the base commit)

D3D12 frontend (`src/d3d12`):

- **Device:** `ID3D12Device5` through `QueryInterface`, with conservative
  Device5 methods: lifetime trackers and `RemoveDevice` are implemented; meta
  commands, raytracing prebuild info and driver matching report "none" or
  "unsupported"; `CreateStateObject` does not publish DXR state objects. A D3D12
  format-support table that reuses D3D11's DXGI-to-Metal mapping with
  D3D12-specific restrictions, `D3D12_OPTIONS5` reporting, a per-adapter device
  cache, reserved (tiled) buffers via `CreateReservedResource` plus
  `UpdateTileMappings`/`CopyTileMappings` on Metal placement-sparse heaps, and
  fences whose native state outlives their COM objects and whose waits are
  released on device removal. The device still reports feature level 11_0
  only: exposing Device5 does not mean feature level 12 conformance.
- **Pipelines:** graphics PSOs with hull/domain shaders (tessellation) and with
  bounded geometry shaders (point expansion, pass-through and bounded list GS)
  are converted to Metal object/mesh pipelines, with validation of signatures,
  stage linkage, tessellation domains/factors and resource contracts;
  pixel-shader input linkage; compute PSO changes; an optional loader for
  externally prepared shader artifacts (see Status).
- **Command lists:** a recording guard and validation on every command
  (invalid arguments and GPU upload-heap exhaustion are reported by `Close`
  instead of crashing or writing out of bounds), draws through the
  mesh-emulated GS and tessellation pipelines, `ExecuteIndirect` including
  indirect draws for those pipelines (a small GPU resolver kernel with
  producer-to-consumer Metal barriers), occlusion queries
  (`BeginQuery`/`EndQuery`, `ResolveQueryData` accumulated on the GPU) and
  paged timestamp queries.
- **Command queues and swap chain:** reworked submission with a native
  retirement worker, residency handling and present-path fixes.
- **Descriptor heaps, root signatures, buffers, textures and heaps:** fixes and
  additions required by the above.
- **Raytracing:** CPU-side planning only (state-object associations, library
  reflection, acceleration-structure build plans). DXR is **not** exposed.
- **Tests:** CPU-only unit tests in `src/d3d12/tests` and
  `src/winemetal/tests`. They are standalone programs and are not wired into
  meson.

Shader converter (`src/airconv`): fixes to geometry-shader and tessellation
conversion, new compilation arguments for tessellation workload splitting,
vertex linkage and pixel-shader input linkage (`airconv_public.h`), a
stage-linkage helper, root-signature binding fixes, tessellator helper changes
in `shaders/air_tessellation.metal`, and CLI additions.

`winemetal` (`src/winemetal`): new calls for placement-sparse buffers and heaps
and a Metal 4 mapping queue (update/copy buffer mappings, event wait/signal),
residency sets, argument buffers built from function reflection, reflected
compute pipelines with binding validation, and acceleration-structure
sizing/creation/build (used internally only). `src/dxmt/dxmt_buffer.*` gains a
placement-sparse allocation flag.

Diagnostics, all off unless the variable is set: `MACRUNNER_DX12_FRAME_TRACE`,
`MACRUNNER_DX12_GAME_BOUNDARY_TRACE`, `MACRUNNER_DX12_FRAME_CAPTURE` with
`MACRUNNER_DX12_SHADER_CAPTURE_DIR`, `MACRUNNER_DX12_FRAME_READBACK`,
`MACRUNNER_DX12_FAULT_CAPTURE`, `MACRUNNER_DX12_PRESENT_TIMING_PATH`,
`MACRUNNER_DX12_STATISTICS_STORAGE_ONLY`, and the artifact loader
`MACRUNNER_DX12_DXIL_ARTIFACT_DIR`.

## Status (as of 2026-09-27)

- Elden Ring (x86-64, running under FEX on Apple Silicon inside MacRunner)
  shows its menus with this D3D12 frontend since 2026-09-16.
- On 2026-09-24 a player reached the Limgrave area. The in-game counter showed
  about 11-13 FPS. That was one bounded five-minute diagnostic session: it is
  neither a stability nor a performance result.
- Known gaps: some pipeline state objects still fail to be created; in an
  earlier session the player character was not drawn; DXR, stream output and
  several optional D3D12 features are unimplemented.
- **Important:** Elden Ring ships DXIL (Shader Model 6) shaders, which DXMT's
  DXBC-based converter does not translate. In the Limgrave session, DXIL
  shaders were replaced at PSO creation by DXBC equivalents prepared offline
  and loaded from `MACRUNNER_DX12_DXIL_ARTIFACT_DIR`. That shader cache is
  derived from the game's files and is not included; neither is the offline
  tooling that produced it. This tree alone does not reproduce that result.

## Removed from the published tree

- `src/d3d12/wave_native_bundle.hpp` in the private build embedded two compute
  shaders extracted from the game's files, their root signatures and Metal
  libraries built from them (opt-in via `MACRUNNER_DX12_NATIVE_WAVE=1`; it was
  not enabled in the Limgrave session). That game-derived data is not
  published: the table is empty, so the switch has no effect and those
  pipelines take the regular conversion path.
- Not included from a separate, older private snapshot: its tessellation
  chunk changes, skipping of NULL command lists in `ExecuteCommandLists`, and
  `CreateCommandList1`.
- No game files, shader caches, captures, logs or run data are included.

## Provenance and building

The published `src/d3d12` sources match the private build used in the Limgrave
session (its `d3d12.dll` was compiled from them). That build carried nine
changed and nine new files that had not been merged back into the fork's
working tree; they are merged here and marked in CHANGES-MACRUNNER.md.
Since then, only comments (change notices) and the emptied native-wave table
with its call site were changed. This exact tree has not been rebuilt yet.

Build as upstream describes in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md),
adding `-Denable_d3d12=true` to `meson setup`. Initialise the
`include/native/directx` submodule first (`git submodule update --init
include/native/directx`).
