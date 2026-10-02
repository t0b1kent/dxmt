/* Generated from encoder switches; regenerate after ABI changes. */
#ifndef WMT_TRACE_COMMAND_SCHEMA_H
#define WMT_TRACE_COMMAND_SCHEMA_H
#include <stddef.h>
struct wmt_trace_command_schema {
  size_t size; unsigned handle_count; size_t handles[8];
  size_t pointer_offset, count_offset, count_size, element_size;
};
static int wmt_trace_command_schema(unsigned encoder, unsigned type,
    struct wmt_trace_command_schema *out) {
  *out = (struct wmt_trace_command_schema){0};
  switch (encoder) {
  case 1: switch (type) {
  case 0: out->size = sizeof(struct wmtcmd_base); return 1;
  case WMTBlitCommandCopyFromBufferToBuffer:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_copy_from_buffer_to_buffer), 2, {offsetof(struct wmtcmd_blit_copy_from_buffer_to_buffer, src), offsetof(struct wmtcmd_blit_copy_from_buffer_to_buffer, dst)}, 0, 0, 0, 0}; return 1;
  case WMTBlitCommandCopyFromBufferToTexture:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_copy_from_buffer_to_texture), 2, {offsetof(struct wmtcmd_blit_copy_from_buffer_to_texture, src), offsetof(struct wmtcmd_blit_copy_from_buffer_to_texture, dst)}, 0, 0, 0, 0}; return 1;
  case WMTBlitCommandCopyFromBufferToTextureWithBlitOption:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption), 2, {offsetof(struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption, src), offsetof(struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption, dst)}, 0, 0, 0, 0}; return 1;
  case WMTBlitCommandCopyFromTextureToBuffer:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_copy_from_texture_to_buffer), 2, {offsetof(struct wmtcmd_blit_copy_from_texture_to_buffer, src), offsetof(struct wmtcmd_blit_copy_from_texture_to_buffer, dst)}, 0, 0, 0, 0}; return 1;
  case WMTBlitCommandCopyFromTextureToTexture:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_copy_from_texture_to_texture), 2, {offsetof(struct wmtcmd_blit_copy_from_texture_to_texture, src), offsetof(struct wmtcmd_blit_copy_from_texture_to_texture, dst)}, 0, 0, 0, 0}; return 1;
  case WMTBlitCommandGenerateMipmaps:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_generate_mipmaps), 1, {offsetof(struct wmtcmd_blit_generate_mipmaps, texture)}, 0, 0, 0, 0}; return 1;
  case WMTBlitCommandUpdateFence:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_fence_op), 1, {offsetof(struct wmtcmd_blit_fence_op, fence)}, 0, 0, 0, 0}; return 1;
  case WMTBlitCommandWaitForFence:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_fence_op), 1, {offsetof(struct wmtcmd_blit_fence_op, fence)}, 0, 0, 0, 0}; return 1;
  case WMTBlitCommandFillBuffer:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_fillbuffer), 1, {offsetof(struct wmtcmd_blit_fillbuffer, buffer)}, 0, 0, 0, 0}; return 1;
  case WMTBlitCommandResolveCounters:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_blit_resolvecounters), 2, {offsetof(struct wmtcmd_blit_resolvecounters, sample_buffer), offsetof(struct wmtcmd_blit_resolvecounters, dst_buffer)}, 0, 0, 0, 0}; return 1;
  default: return 0;
  }
  case 2: switch (type) {
  case 0: out->size = sizeof(struct wmtcmd_base); return 1;
  case WMTComputeCommandDispatch:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_dispatch), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandDispatchThreads:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_dispatch), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandDispatchIndirect:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_dispatch_indirect), 1, {offsetof(struct wmtcmd_compute_dispatch_indirect, indirect_args_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandSetPSO:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_setpso), 1, {offsetof(struct wmtcmd_compute_setpso, pso)}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandSetBuffer:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_setbuffer), 1, {offsetof(struct wmtcmd_compute_setbuffer, buffer)}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandTraceBufferRead:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_trace_buffer_read), 1, {offsetof(struct wmtcmd_trace_buffer_read, buffer)}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandSetBufferOffset:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_setbufferoffset), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandUseResource:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_useresource), 1, {offsetof(struct wmtcmd_compute_useresource, resource)}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandSetBytes:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_setbytes), 0, {0}, offsetof(struct wmtcmd_compute_setbytes, bytes), offsetof(struct wmtcmd_compute_setbytes, length), sizeof(uint64_t), 1}; return 1;
  case WMTComputeCommandSetTexture:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_settexture), 1, {offsetof(struct wmtcmd_compute_settexture, texture)}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandUpdateFence:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_fence_op), 1, {offsetof(struct wmtcmd_compute_fence_op, fence)}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandWaitForFence:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_fence_op), 1, {offsetof(struct wmtcmd_compute_fence_op, fence)}, 0, 0, 0, 0}; return 1;
  case WMTComputeCommandMemoryBarrier:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_compute_memory_barrier), 0, {0}, 0, 0, 0, 0}; return 1;
  default: return 0;
  }
  case 3: switch (type) {
  case 0: out->size = sizeof(struct wmtcmd_base); return 1;
  case WMTRenderCommandUseResource:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_useresource), 1, {offsetof(struct wmtcmd_render_useresource, resource)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetVertexBuffer:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setbuffer), 1, {offsetof(struct wmtcmd_render_setbuffer, buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetVertexBufferOffset:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setbufferoffset), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetFragmentBuffer:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setbuffer), 1, {offsetof(struct wmtcmd_render_setbuffer, buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandTraceBufferRead:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_trace_buffer_read), 1, {offsetof(struct wmtcmd_trace_buffer_read, buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetFragmentBufferOffset:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setbufferoffset), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetMeshBuffer:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setbuffer), 1, {offsetof(struct wmtcmd_render_setbuffer, buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetMeshBufferOffset:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setbufferoffset), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetObjectBuffer:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setbuffer), 1, {offsetof(struct wmtcmd_render_setbuffer, buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetObjectBufferOffset:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setbufferoffset), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetFragmentBytes:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setbytes), 0, {0}, offsetof(struct wmtcmd_render_setbytes, bytes), offsetof(struct wmtcmd_render_setbytes, length), sizeof(uint64_t), 1}; return 1;
  case WMTRenderCommandSetFragmentTexture:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_settexture), 1, {offsetof(struct wmtcmd_render_settexture, texture)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetRasterizerState:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setrasterizerstate), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetViewports:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setviewports), 0, {0}, offsetof(struct wmtcmd_render_setviewports, viewports), offsetof(struct wmtcmd_render_setviewports, viewport_count), sizeof(uint8_t), sizeof(struct WMTViewport)}; return 1;
  case WMTRenderCommandSetScissorRects:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setscissorrects), 0, {0}, offsetof(struct wmtcmd_render_setscissorrects, scissor_rects), offsetof(struct wmtcmd_render_setscissorrects, rect_count), sizeof(uint8_t), sizeof(struct WMTScissorRect)}; return 1;
  case WMTRenderCommandSetPSO:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setpso), 1, {offsetof(struct wmtcmd_render_setpso, pso)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetDSSO:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setdsso), 1, {offsetof(struct wmtcmd_render_setdsso, dsso)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetBlendFactorAndStencilRef:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setblendcolor), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetVisibilityMode:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setvisibilitymode), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDraw:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_draw), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDrawIndexed:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_draw_indexed), 1, {offsetof(struct wmtcmd_render_draw_indexed, index_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDrawIndirect:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_draw_indirect), 1, {offsetof(struct wmtcmd_render_draw_indirect, indirect_args_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDrawIndexedIndirect:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_draw_indexed_indirect), 2, {offsetof(struct wmtcmd_render_draw_indexed_indirect, index_buffer), offsetof(struct wmtcmd_render_draw_indexed_indirect, indirect_args_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDrawMeshThreadgroups:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_draw_meshthreadgroups), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDrawMeshThreadgroupsIndirect:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_draw_meshthreadgroups_indirect), 1, {offsetof(struct wmtcmd_render_draw_meshthreadgroups_indirect, indirect_args_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandMemoryBarrier:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_memory_barrier), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDXMTGeometryDraw:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_dxmt_geometry_draw), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDXMTGeometryDrawIndexed:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_dxmt_geometry_draw_indexed), 1, {offsetof(struct wmtcmd_render_dxmt_geometry_draw_indexed, index_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDXMTGeometryDrawIndirect:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_dxmt_geometry_draw_indirect), 3, {offsetof(struct wmtcmd_render_dxmt_geometry_draw_indirect, imm_draw_arguments), offsetof(struct wmtcmd_render_dxmt_geometry_draw_indirect, indirect_args_buffer), offsetof(struct wmtcmd_render_dxmt_geometry_draw_indirect, dispatch_args_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDXMTGeometryDrawIndexedIndirect:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect), 4, {offsetof(struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect, index_buffer), offsetof(struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect, imm_draw_arguments), offsetof(struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect, indirect_args_buffer), offsetof(struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect, dispatch_args_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDXMTTessellationMeshDraw:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_dxmt_tessellation_mesh_draw), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed), 1, {offsetof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed, index_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect), 3, {offsetof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect, imm_draw_arguments), offsetof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect, indirect_args_buffer), offsetof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect, dispatch_args_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect), 4, {offsetof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect, imm_draw_arguments), offsetof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect, indirect_args_buffer), offsetof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect, dispatch_args_buffer), offsetof(struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect, index_buffer)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandUpdateFence:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_fence_op), 1, {offsetof(struct wmtcmd_render_fence_op, fence)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandWaitForFence:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_fence_op), 1, {offsetof(struct wmtcmd_render_fence_op, fence)}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetViewport:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setviewport), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandSetScissorRect:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_setscissorrect), 0, {0}, 0, 0, 0, 0}; return 1;
  case WMTRenderCommandDispatchThreadsPerTile:
    *out = (struct wmt_trace_command_schema){sizeof(struct wmtcmd_render_dispatch_threads_per_tile), 0, {0}, 0, 0, 0, 0}; return 1;
  default: return 0;
  }
  default: return 0;
  }
}
#endif
/* 60 non-NOP opcode cases. */
