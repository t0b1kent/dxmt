# Changes in the MacRunner fork

Relative to upstream DXMT `ef116534b2af7037c670ede3297e3ee74b45058d`. This list is
the change notice required by LGPL-2.1 section 2a; every file below in `src/`
also carries a one-line dated notice in its header comment. See
[README-MACRUNNER.md](README-MACRUNNER.md) for what the changes do.

Files marked "from the Limgrave build copy" are the versions compiled into the
`d3d12.dll` used on 2026-09-24; they had not been merged back into the fork's
working tree before this publication.

## Modified upstream files (45)

2026-10-14: the D3D12 layer sources (`src/d3d12`, and the three `src/airconv` /
`src/dxmt` files below) are aligned with the exact source state that built the working
`d3d12.dll` `0bec3f8d4fd985c0...` (Elden Ring, build v6). `build/repro110layer/ER-V6-SOURCE.json`
lists the sha256 of every file of that state; the recipe `build/repro110layer` rebuilds the
DLL from it. Changed against the earlier public text: `d3d12_pipeline_timing.hpp`,
`d3d12_fine_locks.hpp` and `d3d12_gpu_timing.hpp` are added (default-off diagnostics and
striped locks used by that binary); `d3d12_diagnostic_counters.hpp`,
`d3d12_root_validation.hpp` and `src/dxmt/dxmt_shader_metal.hpp` are removed again because
that binary does not contain them (the entries dated 2026-10-03 above describe the earlier
text and stay in the history of this branch); `src/airconv/dxbc_binding_rootsig.cpp`,
`src/airconv/nt/dxbc_binding_map.hpp`, `src/airconv/nt/dxbc_converter_base.{cpp,hpp}` and
`src/dxmt/dxmt_device.cpp` take the versions compiled into that binary.

2026-10-03: `src/d3d12/d3d12_device.hpp` additionally counts existing residency
retain/release and membership add/remove/commit paths when diagnostics are enabled.
The policy and ownership transitions are unchanged; observed add/remove balance
is reported separately from native memory retirement.

2026-10-03: added `src/d3d12/d3d12_root_validation.hpp` and call it during root
creation before sampler allocation. Covers register namespaces, visibility,
spaces, bounded/unbounded ranges, APPEND, constants and static samplers.

2026-10-03: added `src/d3d12/d3d12_diagnostic_counters.hpp` and optional hooks in
device, command-list and both pipeline implementations. FrameTrace now includes
QPC/UTC/thread anchors. The counter gate defaults off; no game or test-suite
implementation code was imported.

- `AGENTS.md`
- `src/airconv/airconv_cli.cpp`
- `src/airconv/airconv_public.h`
- `src/airconv/dxbc_binding_rootsig.cpp`
- `src/airconv/dxbc_binding_sm50.cpp`
- `src/airconv/dxbc_converter.cpp`
- `src/airconv/dxbc_converter.hpp`
- `src/airconv/dxbc_converter_gs.cpp`
- `src/airconv/dxbc_converter_ts.cpp`
- `src/airconv/dxbc_instructions.cpp`
- `src/airconv/dxbc_instructions.hpp`
- `src/airconv/dxbc_signature.cpp`
- `src/airconv/nt/dxbc_binding_map.hpp`
- `src/airconv/nt/dxbc_converter_base.cpp`
- `src/airconv/nt/dxbc_converter_base.hpp`
- `src/airconv/shaders/air_tessellation.metal`
- `src/d3d12/d3d12.cpp`
- `src/d3d12/d3d12_buffer.cpp`
- `src/d3d12/d3d12_command_allocator.cpp` - from the Limgrave build copy
- `src/d3d12/d3d12_command_allocator.hpp` - from the Limgrave build copy
- `src/d3d12/d3d12_command_encoder.hpp` - from the Limgrave build copy
- `src/d3d12/d3d12_command_list.cpp` - from the Limgrave build copy
- `src/d3d12/d3d12_command_queue.cpp` - from the Limgrave build copy
- `src/d3d12/d3d12_command_signature.cpp` - from the Limgrave build copy
- `src/d3d12/d3d12_descriptor_heap.cpp`
- `src/d3d12/d3d12_descriptor_heap.hpp`
- `src/d3d12/d3d12_device.cpp` - from the Limgrave build copy
- `src/d3d12/d3d12_device.hpp` - from the Limgrave build copy
- `src/d3d12/d3d12_fence.cpp`
- `src/d3d12/d3d12_heap.cpp`
- `src/d3d12/d3d12_pipeline.hpp`
- `src/d3d12/d3d12_pipeline_compute.cpp` - native-wave call uses the table size instead of a fixed 2
- `src/d3d12/d3d12_pipeline_graphics.cpp` - from the Limgrave build copy
- `src/d3d12/d3d12_query_heap.cpp`
- `src/d3d12/d3d12_root_signature.cpp`
- `src/d3d12/d3d12_swapchain.cpp`
- `src/d3d12/d3d12_texture.cpp`
- `src/dxmt/dxmt_buffer.cpp`
- `src/dxmt/dxmt_buffer.hpp`
- `src/winemetal/Metal.hpp`
- `src/winemetal/unix/meson.build`
- `src/winemetal/unix/winemetal_unix.c`
- `src/winemetal/winemetal.h`
- `src/winemetal/winemetal_thunks.c`
- `src/winemetal/winemetal_thunks.h`

## Added files (68)

- `CHANGES-MACRUNNER.md`
- `README-MACRUNNER.md`
- `src/airconv/stage_linkage.hpp`
- `src/d3d12/compiler/dxil-resource-footprint.cpp`
- `src/d3d12/d3d12_acceleration_build.hpp`
- `src/d3d12/d3d12_acceleration_capture.hpp`
- `src/d3d12/d3d12_argument_access.hpp`
- `src/d3d12/d3d12_argument_upload.hpp`
- `src/d3d12/d3d12_capture_ownership.hpp`
- `src/d3d12/d3d12_command_capture.hpp`
- `src/d3d12/d3d12_command_failure_trace.hpp` - from the Limgrave build copy
- `src/d3d12/d3d12_command_list.hpp`
- `src/d3d12/d3d12_compiler_resource.hpp`
- `src/d3d12/d3d12_compute_commands.hpp`
- `src/d3d12/d3d12_dxil_artifact.hpp`
- `src/d3d12/d3d12_frame_trace.hpp`
- `src/d3d12/d3d12_gs_contract_reader.hpp` - from the Limgrave build copy
- `src/d3d12/d3d12_gs_passthrough.hpp`
- `src/d3d12/d3d12_gs_point.hpp`
- `src/d3d12/d3d12_lifetime.hpp`
- `src/d3d12/d3d12_local_root_layout.hpp`
- `src/d3d12/d3d12_metal_argument.hpp`
- `src/d3d12/d3d12_object_grid.hpp` - from the Limgrave build copy
- `src/d3d12/d3d12_present_timing.hpp`
- `src/d3d12/d3d12_raytracing_bindings.hpp`
- `src/d3d12/d3d12_raytracing_library.hpp`
- `src/d3d12/d3d12_raytracing_state_plan.hpp`
- `src/d3d12/d3d12_recorded_binding.hpp`
- `src/d3d12/d3d12_recording_guard.hpp` - from the Limgrave build copy
- `src/d3d12/d3d12_resource_shape.hpp`
- `src/d3d12/d3d12_shader_capture.hpp`
- `src/d3d12/d3d12_state_object_associations.hpp`
- `src/d3d12/d3d12_state_object_libraries.hpp`
- `src/d3d12/d3d12_submission_capture.hpp`
- `src/d3d12/encode-mesh-indirect.inc` - from the Limgrave build copy
- `src/d3d12/gpu-heap-bounds.hpp` - from the Limgrave build copy
- `src/d3d12/indirect-mesh-contract.hpp` - from the Limgrave build copy
- `src/d3d12/indirect-mesh-resolver.hpp` - from the Limgrave build copy
- `src/d3d12/indirect-mesh-sync.hpp` - from the Limgrave build copy
- `src/d3d12/tests/acceleration-build.cpp`
- `src/d3d12/tests/acceleration-capture.cpp`
- `src/d3d12/tests/argument-access.cpp`
- `src/d3d12/tests/argument-upload.cpp`
- `src/d3d12/tests/capture-all-CONTRACT.md`
- `src/d3d12/tests/capture-all-RESULT.md`
- `src/d3d12/tests/capture-all-mock.cpp`
- `src/d3d12/tests/capture-all-run.cjs`
- `src/d3d12/tests/capture-ownership.cpp`
- `src/d3d12/tests/compiler-resource.cpp`
- `src/d3d12/tests/compute-entry.cpp`
- `src/d3d12/tests/local-root-layout.cpp`
- `src/d3d12/tests/metal-argument.cpp`
- `src/d3d12/tests/raytracing-bindings.cpp`
- `src/d3d12/tests/raytracing-library.cpp`
- `src/d3d12/tests/raytracing-state-plan.cpp`
- `src/d3d12/tests/recorded-binding.cpp`
- `src/d3d12/tests/shaped-argument.cpp`
- `src/d3d12/wave_native_bundle.hpp` - empty table; game-derived shader data removed
- `src/d3d12/wave_native_match.hpp`
- `src/shared/point_root_contract.hpp`
- `src/winemetal/tests/acceleration-layout.cpp`
- `src/winemetal/tests/acceleration.mm`
- `src/winemetal/tests/argument-buffer.mm`
- `src/winemetal/tests/argument-layout.cpp`
- `src/winemetal/tests/compute-entry.mm`
- `src/winemetal/unix/winemetal_acceleration.inc`
- `src/winemetal/unix/winemetal_arguments.inc`
- `src/winemetal/unix/winemetal_compute_entry.inc`

## Not changed / not included

- `include/native/directx` stays the upstream submodule, pinned at the
  upstream commit `9df86f2341616ef1888ae59919feaa6d4fad693d` (the fork's working
  tree used a local link to a checkout of that same commit).
- The private fork note `MACRUNNER-FORK.md` is replaced by `README-MACRUNNER.md`.
- No game files, shader caches, captures, logs or run data.

### 2026-10-14: shader-source seam, provider `own`

`MACRUNNER_DX12_SHADER_PATH=<provider>` selects where a graphics pipeline takes
ready Metal code for its stages; unset, nothing changes. The seam is
`src/d3d12/d3d12_shader_source.hpp`: one request (shader container, serialized
root signature, stage), one response (Metal library, entry name, resource and
vertex-input counts), a provider table. The only provider is `own`: libraries
of our DXIL -> Metal translator, prepared offline and read from
`MACRUNNER_DX12_OWN_SHADER_DIR` as `<container SHA-1>.<stage>.metallib` with
`<container SHA-1>.<stage>.desc`. No translation happens inside the layer and
nothing is cached.

The choice is per pipeline and reported on stderr as `dx12_shader_source ...`:
a pipeline built from provider libraries prints `source=<provider>` with the
SHA-1 of each library; a pipeline left to the regular path prints
`source=legacy requested=<provider> reason=...` naming the stage and shader
that had no translation. A translation that is present but rejected fails the
pipeline (`failed=...`) and is not replaced by the regular path.

DIAGNOSTIC_ONLY. Admitted by the first milestone: VS+PS, triangles, one RGBA8
target, no input layout, depth, MSAA, sample mask, alpha-to-coverage or
dual-source blending, and only functions that bind no resources and read no
vertex attributes; everything else, and every compute pipeline, took the
regular path.

### 2026-10-14: provider `own` serves resources, vertex inputs and compute

A translated function now follows the binding convention of the regular path,
so the draw side does not change and does not know which path built a
pipeline: root arguments in buffer slot 0, static samplers in slot 1, the
vertex buffer table in slot 2, 32-byte descriptors in the tables. Description
format 2 (`format=2` in `.desc`) names the root signature the function was
translated for (SHA-1 of the RTS0 part), its vertex inputs by semantic and the
compute thread group size; a function that reads root arguments is stored as
`<container SHA-1>.<root SHA-1>.<stage>.metallib`. A translation made for
another root signature fails the pipeline (`failed=root_signature_mismatch`).

Vertex functions pull their inputs from the vertex buffer table themselves.
Slot entry, byte offset, format and step of every input come from the
pipeline's input layout as four Metal function constants per input location
(`newFunctionWithConstants`); the pipeline's `slot_mask` is set from the same
layout. Compute pipelines are served by the same provider.

`MACRUNNER_DX12_OWN_SHADER_REQUEST_DIR=<dir>` (optional): every missing
translation leaves `<container SHA-1>.<stage>.dxbc`, `<root SHA-1>.rts0` and a
line in `requests.txt` there, which is the input of the offline translator.

DIAGNOSTIC_ONLY. Admitted: VS+PS with any input layout, triangles, 1..8 colour
targets, no depth target, MSAA, sample mask, alpha-to-coverage or dual-source
blending; compute. Everything else takes the regular path with a reason line.

### 2026-10-14: provider `own` is admitted like the regular path

A pipeline built from provider libraries is now described by the same code as
one built by the regular path (`Initialize` in
`src/d3d12/d3d12_pipeline_graphics.cpp`): target formats, blending, depth and
stencil, topology class, sample count, rasterizer state. Only two things
differ: where the stage functions come from (`SelectTranslated`,
`TranslatedVertexFunction`, `TranslatedPixelFunction`) and how pipeline state
reaches them. What the regular path compiles into its pixel function
(`SM50_SHADER_PSO_PIXEL_SHADER`) is covered as follows:

- sample mask other than all ones: pixel variant `sm`, the mask is Metal
  function constant 128;
- dual-source blending: pixel variant `ds`;
- target formats: a ready function keeps the declared type of every output; a
  target whose format class (float, signed, unsigned) differs from it fails
  the pipeline (`failed=target_type_mismatch`);
- alpha-to-coverage is dropped when the description says the shader writes
  coverage itself (`coverage_output=1`).

A variant is stored as `<container SHA-1>[.<root SHA-1>].ps.<variant>.metallib`
and requested with `variant=<..>` in `requests.txt`. Description format 3
(`format=3`) adds `variant`, `coverage_output` and `colors`; format 2 is still
read. The line of a pipeline built from provider libraries also names the
variant and the admitted state (`topology`, `targets`, `dsv`, `samples`,
`sample_mask`, `alpha_to_coverage`).

Still left to the regular path, with a reason line: pipelines with a geometry,
hull or domain stage (`stage_not_served`) and pipelines where the provider has
a translation for only one of the two stages. `d3d12_command_list.cpp` and the
macOS side are unchanged. Compute pipelines accept description formats 2 and 3.

DIAGNOSTIC_ONLY. Built for x64 PE. Checked by small D3D12 probes under Wine
only (depth target, MSAA 4x, sample mask, lines and points, vertex-only
pipeline, two targets, dual-source blending, 43 vertex input formats, typed
buffer views, UAV counters, unbounded resource arrays): pixels and words equal
to the regular path. No application has been run on it.

### 2026-10-14: provider failures can fall back to the regular path; counters

Two opt-in keys for running an application on provider `own`
(`src/d3d12/d3d12_shader_source_stats.hpp`). Both are read once per process;
with both unset nothing changes, except that the same translation request is
now left once per process instead of once per pipeline creation.

- `MACRUNNER_DX12_OWN_FALLBACK=1`: a pipeline the provider took and could not
  build (unusable translation, root signature mismatch, target type mismatch,
  input layout not feeding an input, Metal refusing a library, a function or
  the pipeline state) is rebuilt by the regular path instead of failing. A
  graphics pipeline is rebuilt on a fresh object; a compute pipeline continues
  in the same object (nothing of the failed attempt is kept). Every pipeline
  that ends on the regular path while a provider is selected leaves one line:
  `dx12_shader_source_fallback pipeline=.. owner=.. cause=<declined|failed>
  reason=<name> own_hr=.. legacy_hr=..`. With this key an SM5.0 container is
  declined as `not_dxil` without leaving a request.
- `MACRUNNER_DX12_OWN_STATS=<seconds>`: cumulative counters
  `dx12_shader_source_stats ... graphics=<own>/<declined>/<fallback>/<failed>/
  <legacy_failed> compute=... requests=<n> reasons=<name>:<graphics>/<compute>`
  at most once per period (checked at pipeline creation and at Present, no
  thread of its own), at device destruction and at module unload.

Touched: `d3d12_shader_source.hpp`, `d3d12_shader_source_stats.hpp` (new),
`d3d12_pipeline_graphics.cpp`, `d3d12_pipeline_compute.cpp`,
`d3d12_command_queue.cpp` (one include, one line at Present),
`d3d12_device.cpp` (one include, one call in the destructor).
`d3d12_command_list.cpp` and the macOS side are unchanged.

DIAGNOSTIC_ONLY. Checked by small D3D12 probes under Wine with injected
provider failures (key off: the pipeline fails as before; key on: the regular
path builds it from ready SM5.0 bytecode and the pixels equal the regular
path). No application has been run on it.

## Shader-source seam: the answer as MSL text (`MACRUNNER_DX12_OWN_SHADER_KIND`)

The provider `own` can answer a stage with the MSL text the translator wrote, and the layer builds the Metal library
itself. One new key, read once per process; unset, `library` or any other value: nothing changes (ready libraries,
`.metallib`, as before).

- `MACRUNNER_DX12_OWN_SHADER_KIND=source`: for the stage the layer reads
  `<shader>.<root>.<ps|vs|gs|hs|ds|cs>[.<variant>].msl` (or the name without root), next to the same `.desc`, whose two new
  fields say what it is: `kind=source` and `language=<major>.<minor>` (3.1: the translator's text is written for it). A
  description without them is an unusable translation. The library is built with `MTLCompileOptions.languageVersion` set
  to that version and every other option at its default (the system default language moves with the OS: 4.1 on macOS 27,
  about 3.5 times slower on this corpus); once per process for the same text and language, later pipelines take it from the
  process. A build error fails the stage (reason `vs_library` / `ps_library` / `cs_library`) and leaves one stderr line
  `dx12_shader_source_compile_failed key=<sha1 of the text> language=<hex> bytes=.. error=<Metal text>`.
  `MACRUNNER_DX12_OWN_STATS` adds `source_libs=<built>/<found in process>/<ms spent building>` to its line when used.
- winemetal: Unix call 163 `MTLDevice_newLibraryWithSourceOptions(device, source, length, language_version, flags, &error)`
  (language_version: MTLLanguageVersion value, 0 = system default; flags: reserved, must be 0). The renumbering rule if a tree
  already uses 163: the call goes to the next free table position and the thunk number follows.
  `MACRUNNER_WMT_SOURCE_TRACE=1` prints once per process `wmt_source_identity pid= page_size= process= bundle_id= bundle_path=
  executable=` when the call is first used (the identity of the process that talks to Metal).

Touched: `d3d12_shader_source.hpp`, `d3d12_shader_source_stats.hpp`, `d3d12_pipeline_graphics.cpp` (two library calls),
`d3d12_pipeline_compute.cpp` (one), `src/winemetal/{Metal.hpp,winemetal.h,winemetal_thunks.[ch],unix/winemetal_unix.c}`.
Needs a rebuilt `winemetal.so` AND `winemetal.dll` next to the layer (the layer imports the new name).

DIAGNOSTIC_ONLY. No application has been run on it.
