# DXMT trace source inventory

DXMT fork source inventory, October 2, 2026. The 78 targets below are the trace-stand changes on top of the `macrunner-base` branch, under DXMT's LGPL-2.1-or-later license. They are committed directly in this fork; no patch application is needed on `macrunner-trace-stand`. No DXMT file is added to HyperBridge.

The patch has 78 target files. Record/codec/object/ownership helpers are under `src/winemetal`; producer range and staging hooks are in `src/d3d11` and `src/dxmt`; the native player, comparators and own tests are under `tools`.

| Target path | Change |
| --- | --- |
| `src/d3d11/d3d11_context_def.cpp` | Recorder/producer integration |
| `src/d3d11/d3d11_context_imm.cpp` | Recorder/producer integration |
| `src/d3d11/d3d11_resource_staging.cpp` | Recorder/producer integration |
| `src/d3d11/d3d11_texture_linear.cpp` | Recorder/producer integration |
| `src/dxmt/dxmt_buffer.hpp` | Recorder/producer integration |
| `src/dxmt/dxmt_context.cpp` | Recorder/producer integration |
| `src/dxmt/dxmt_context.hpp` | Recorder/producer integration |
| `src/dxmt/dxmt_staging.cpp` | Recorder/producer integration |
| `src/dxmt/dxmt_staging.hpp` | Recorder/producer integration |
| `src/dxmt/dxmt_texture.hpp` | Recorder/producer integration |
| `src/winemetal/unix/meson.build` | Recorder/producer integration |
| `src/winemetal/unix/winemetal_unix.c` | Recorder/producer integration |
| `src/winemetal/winemetal.h` | Recorder/producer integration |
| `src/winemetal/winemetal_thunks.c` | Recorder/producer integration |
| `src/winemetal/winemetal_thunks.h` | Recorder/producer integration |
| `src/winemetal/unix/wmt_trace_api_abi_asserts.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_api_codec.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_api_dispatch.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_api_lifetime.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_api_metadata.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_api_native.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_api_payload_validation.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_api_payloads.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_api_preflight.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_api_record.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_cb.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_command_schema.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_commands.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_descriptors.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_device_identity.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_fields.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_file.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_frame.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_input_evidence.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_native_backend.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_native_encoders.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_native_objects.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_ownership.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_pass.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_png.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_record.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_registry.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_relocate.h` | Trace codec/replay helper |
| `src/winemetal/unix/wmt_trace_snapshot.h` | Trace codec/replay helper |
| `tools/wmt_audit_lease_test.m` | Own synthetic test source |
| `tools/wmt_audit_png10_test.m` | Own synthetic test source |
| `tools/wmt_relocations.py` | Inspection/comparison/stand source |
| `tools/wmt_trace_api_codec_test.m` | Own synthetic test source |
| `tools/wmt_trace_cb_test.m` | Own synthetic test source |
| `tools/wmt_trace_command_schema.py` | Inspection/comparison/stand source |
| `tools/wmt_trace_commands_test.m` | Own synthetic test source |
| `tools/wmt_trace_compare_frames.py` | Inspection/comparison/stand source |
| `tools/wmt_trace_coverage.py` | Inspection/comparison/stand source |
| `tools/wmt_trace_failure_evidence.py` | Inspection/comparison/stand source |
| `tools/wmt_trace_fence_test.m` | Own synthetic test source |
| `tools/wmt_trace_fields_test.m` | Own synthetic test source |
| `tools/wmt_trace_file_test.m` | Own synthetic test source |
| `tools/wmt_trace_inventory.py` | Inspection/comparison/stand source |
| `tools/wmt_trace_native_blit_test.m` | Own synthetic test source |
| `tools/wmt_trace_native_cached_test.m` | Own synthetic test source |
| `tools/wmt_trace_native_completion_test.m` | Own synthetic test source |
| `tools/wmt_trace_native_encoder_check.py` | Inspection/comparison/stand source |
| `tools/wmt_trace_native_player.m` | Native player |
| `tools/wmt_trace_native_render_test.m` | Own synthetic test source |
| `tools/wmt_trace_negative_backend.m` | Inspection/comparison/stand source |
| `tools/wmt_trace_nooverwrite_test.m` | Own synthetic test source |
| `tools/wmt_trace_ownership_test.m` | Own synthetic test source |
| `tools/wmt_trace_pipeline_fields_test.m` | Own synthetic test source |
| `tools/wmt_trace_producer_ranges_test.m` | Own synthetic test source |
| `tools/wmt_trace_ranges_test.m` | Own synthetic test source |
| `tools/wmt_trace_record_test.m` | Own synthetic test source |
| `tools/wmt_trace_registry_test.m` | Own synthetic test source |
| `tools/wmt_trace_relocate_test.c` | Own synthetic test source |
| `tools/wmt_trace_snapshot_test.m` | Own synthetic test source |
| `tools/wmt_trace_stand.py` | Inspection/comparison/stand source |
| `tools/wmt_trace_stream_fixture.py` | Inspection/comparison/stand source |
| `tools/trace_support/native_backend_foreign.c` | Own standalone support |
| `tools/trace_support/png_full_depth.py` | Own standalone support |

Export-only changes: `wmt_trace_stand.py` resolves the source checkout from its own location, takes the external corpus root from `WMT_TRACE_CORPUS`, uses included standalone decoder/stub source and an output-volume disk check, and treats the private build scheduler as optional. The native player is the snapshot before a concurrently added GPU-load barrier experiment; that unrelated insertion is deliberately excluded. Patch application was checked against the exact base without running Metal.

Excluded: the unrelated `air_tessellation.metal` change; the contracts JSON containing recorded trace coverage/evidence; its generator requiring private audit inputs; all application traces, frames, payload bytes, runtime builds and logs. Generated source ABI headers remain included. See `DATA_FORMAT.md` for recording your own inputs.
