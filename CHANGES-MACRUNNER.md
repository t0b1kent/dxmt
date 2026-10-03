# Changes in the MacRunner fork

Relative to upstream DXMT `ef116534b2af7037c670ede3297e3ee74b45058d`. This list is
the change notice required by LGPL-2.1 section 2a; every file below in `src/`
also carries a one-line dated notice in its header comment. See
[README-MACRUNNER.md](README-MACRUNNER.md) for what the changes do.

Files marked "from the Limgrave build copy" are the versions compiled into the
`d3d12.dll` used on 2026-09-24; they had not been merged back into the fork's
working tree before this publication.

## Modified upstream files (45)

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
