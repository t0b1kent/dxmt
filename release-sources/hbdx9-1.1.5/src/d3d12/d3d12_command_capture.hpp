// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

#include "../winemetal/winemetal.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace dxmt {

enum class WMTCommandCaptureFamily { Render, Compute, Blit };

constexpr std::size_t WMTCommandCaptureMaxCommands = 8192;
constexpr std::size_t WMTCommandCaptureMaxPODBytes = 512;

struct WMTCommandCaptureResult {
  std::size_t command_count = 0;
  bool truncated = false;
  const char *stop_reason = "running";
};

namespace wmt_command_capture_detail {

struct Record {
  const char *kind;
  const char *pod_type;
  std::size_t size;
};

inline const char *
family_name(WMTCommandCaptureFamily family) {
  switch (family) {
  case WMTCommandCaptureFamily::Render: return "Render";
  case WMTCommandCaptureFamily::Compute: return "Compute";
  case WMTCommandCaptureFamily::Blit: return "Blit";
  }
  return "invalid";
}

// Generated from winemetal.h enums and checked against winemetal_unix.c casts.
// Shared variants deliberately retain their individual enum names.
#define DXMT_WMT_CAPTURE_CASE(kind, pod)                                                   \
  case kind:                                                                              \
    static_assert(std::is_standard_layout<pod>::value &&                                   \
                  std::is_trivially_copyable<pod>::value, "capture requires a POD record"); \
    static_assert(sizeof(pod) >= sizeof(wmtcmd_base) &&                                     \
                  offsetof(pod, type) == offsetof(wmtcmd_base, type) &&                     \
                  offsetof(pod, next) == offsetof(wmtcmd_base, next),                       \
                  "capture requires the WMT command prefix");                             \
    return {#kind, #pod, sizeof(pod)}

// Missing future enum members must fail compilation instead of silently losing coverage.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wswitch-enum"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch-enum"
#endif
inline Record
record(WMTCommandCaptureFamily family, std::uint16_t type) {
  switch (family) {
  case WMTCommandCaptureFamily::Render:
    switch (static_cast<WMTRenderCommandType>(type)) {
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandNop, wmtcmd_render_nop);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandUseResource, wmtcmd_render_useresource);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetVertexBuffer, wmtcmd_render_setbuffer);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetVertexBufferOffset, wmtcmd_render_setbufferoffset);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetFragmentBuffer, wmtcmd_render_setbuffer);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetFragmentBufferOffset, wmtcmd_render_setbufferoffset);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetMeshBuffer, wmtcmd_render_setbuffer);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetMeshBufferOffset, wmtcmd_render_setbufferoffset);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetObjectBuffer, wmtcmd_render_setbuffer);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetObjectBufferOffset, wmtcmd_render_setbufferoffset);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetFragmentTexture, wmtcmd_render_settexture);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetFragmentBytes, wmtcmd_render_setbytes);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetRasterizerState, wmtcmd_render_setrasterizerstate);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetViewports, wmtcmd_render_setviewports);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetScissorRects, wmtcmd_render_setscissorrects);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetPSO, wmtcmd_render_setpso);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetDSSO, wmtcmd_render_setdsso);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetBlendFactorAndStencilRef, wmtcmd_render_setblendcolor);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetVisibilityMode, wmtcmd_render_setvisibilitymode);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDraw, wmtcmd_render_draw);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDrawIndexed, wmtcmd_render_draw_indexed);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDrawIndirect, wmtcmd_render_draw_indirect);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDrawIndexedIndirect, wmtcmd_render_draw_indexed_indirect);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDrawMeshThreadgroups, wmtcmd_render_draw_meshthreadgroups);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDrawMeshThreadgroupsIndirect, wmtcmd_render_draw_meshthreadgroups_indirect);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandMemoryBarrier, wmtcmd_render_memory_barrier);
    case Unused0: return {"Unused0", "none", 0};
    case Unused1: return {"Unused1", "none", 0};
    case Unused2: return {"Unused2", "none", 0};
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDXMTGeometryDraw, wmtcmd_render_dxmt_geometry_draw);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDXMTGeometryDrawIndexed, wmtcmd_render_dxmt_geometry_draw_indexed);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDXMTGeometryDrawIndirect, wmtcmd_render_dxmt_geometry_draw_indirect);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDXMTGeometryDrawIndexedIndirect, wmtcmd_render_dxmt_geometry_draw_indexed_indirect);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandWaitForFence, wmtcmd_render_fence_op);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandUpdateFence, wmtcmd_render_fence_op);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetViewport, wmtcmd_render_setviewport);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetScissorRect, wmtcmd_render_setscissorrect);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDXMTTessellationMeshDraw, wmtcmd_render_dxmt_tessellation_mesh_draw);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDXMTTessellationMeshDrawIndexed, wmtcmd_render_dxmt_tessellation_mesh_draw_indexed);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDXMTTessellationMeshDrawIndirect, wmtcmd_render_dxmt_tessellation_mesh_draw_indirect);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect, wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandDispatchThreadsPerTile, wmtcmd_render_dispatch_threads_per_tile);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandExecuteCommandsInBuffer, wmtcmd_render_executecommands);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetBlendFactor, wmtcmd_render_setblendcolor);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetStencilRef, wmtcmd_render_setstencilref);
      DXMT_WMT_CAPTURE_CASE(WMTRenderCommandSetDepthStencilState, wmtcmd_render_setdepthstencilstate);
    }
    break;
  case WMTCommandCaptureFamily::Compute:
    switch (static_cast<WMTComputeCommandType>(type)) {
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandNop, wmtcmd_compute_nop);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandDispatch, wmtcmd_compute_dispatch);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandDispatchIndirect, wmtcmd_compute_dispatch_indirect);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandSetPSO, wmtcmd_compute_setpso);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandSetBuffer, wmtcmd_compute_setbuffer);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandSetBufferOffset, wmtcmd_compute_setbufferoffset);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandUseResource, wmtcmd_compute_useresource);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandSetBytes, wmtcmd_compute_setbytes);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandSetTexture, wmtcmd_compute_settexture);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandDispatchThreads, wmtcmd_compute_dispatch);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandWaitForFence, wmtcmd_compute_fence_op);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandUpdateFence, wmtcmd_compute_fence_op);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandMemoryBarrier, wmtcmd_compute_memory_barrier);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandExecuteCommandsInBuffer, wmtcmd_compute_executecommands);
      DXMT_WMT_CAPTURE_CASE(WMTComputeCommandSetAccelerationStructure, wmtcmd_compute_setaccelerationstructure);
    }
    break;
  case WMTCommandCaptureFamily::Blit:
    switch (static_cast<WMTBlitCommandType>(type)) {
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandNop, wmtcmd_blit_nop);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandCopyFromBufferToBuffer, wmtcmd_blit_copy_from_buffer_to_buffer);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandCopyFromBufferToTexture, wmtcmd_blit_copy_from_buffer_to_texture);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandCopyFromTextureToBuffer, wmtcmd_blit_copy_from_texture_to_buffer);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandCopyFromTextureToTexture, wmtcmd_blit_copy_from_texture_to_texture);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandGenerateMipmaps, wmtcmd_blit_generate_mipmaps);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandWaitForFence, wmtcmd_blit_fence_op);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandUpdateFence, wmtcmd_blit_fence_op);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandFillBuffer, wmtcmd_blit_fillbuffer);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandResolveCounters, wmtcmd_blit_resolvecounters);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandCopyFromBufferToTextureWithBlitOption, wmtcmd_blit_copy_from_buffer_to_texture_withblitoption);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandCopyFromTextureToBufferWithBlitOption, wmtcmd_blit_copy_from_texture_to_buffer_withblitoption);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandResetCommandsInBuffer, wmtcmd_blit_resetcommands);
      DXMT_WMT_CAPTURE_CASE(WMTBlitCommandCopyTexture, wmtcmd_blit_copy_texture);
    }
    break;
  }
  return {"unknown", "none", 0};
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef DXMT_WMT_CAPTURE_CASE

} // namespace wmt_command_capture_detail

// Caller owns a valid, immutable native chain for the entire synchronous call.
// emit(const char *detail) must consume/copy the bounded, temporary string now.
// Caller adds submission/list/pass/frame IDs and owns frame gating/global row caps.
// A callback return value is ignored; callback-side drops need caller-side reporting.
//
// This is command-kind coverage, NOT resource contents or API-call replay.
// Hex is the native object representation, including pointers, reserved bytes and
// padding (which may be uninitialized). Only next links are followed; bindings,
// arrays, byte payloads, indirect buffers, resources and pipeline objects are not
// dereferenced. Pass descriptors and calls outside these chains are not captured.
// Limits cannot be raised above 8192 commands/pass or 512 POD bytes/command.
// Each emitted line is at most 2047 bytes excluding its terminating NUL.
template <typename Emit>
inline WMTCommandCaptureResult
CaptureWMTCommands(
    WMTCommandCaptureFamily family, const wmtcmd_base *head, Emit &&emit,
    std::size_t max_commands = WMTCommandCaptureMaxCommands) {
  using namespace wmt_command_capture_detail;
  WMTCommandCaptureResult result;
  const char *family_text = family_name(family);
  const std::size_t limit =
      max_commands < WMTCommandCaptureMaxCommands ? max_commands : WMTCommandCaptureMaxCommands;
  char line[2048];

  // All summary fields are fixed strings or bounded integers, well below 2048.
  auto summary = [&](const char *event) {
    std::snprintf(
        line, sizeof(line),
        "event=%s family=%s index=%zu command_count=%zu truncated=%u stop_reason=%s "
        "command_limit=%zu pod_limit=512 bindings_data=not_captured "
        "pointer_payloads=not_captured resource_contents=not_captured scope=command_kinds_only",
        event, family_text, result.command_count, result.command_count,
        result.truncated ? 1u : 0u, result.stop_reason, limit);
    emit(line);
  };

  if (family != WMTCommandCaptureFamily::Render &&
      family != WMTCommandCaptureFamily::Compute &&
      family != WMTCommandCaptureFamily::Blit) {
    result.truncated = true;
    result.stop_reason = "invalid_family";
    summary("unknown_family");
    return result;
  }

  summary("begin");
  const void *current = head;
  while (current) {
    if (result.command_count == limit) {
      result.truncated = true;
      result.stop_reason = "command_limit";
      break;
    }

    // Read only the common type until the exact struct size is known.
    std::uint16_t type = 0;
    std::memcpy(&type, current, sizeof(type));
    const Record info = record(family, type);
    if (!info.size) {
      result.truncated = true;
      result.stop_reason = "unknown_type";
      std::snprintf(
          line, sizeof(line),
          "event=unknown family=%s index=%zu type=%u kind=%s size=unknown captured=0 "
          "command_count=%zu truncated=1 stop_reason=unknown_type",
          family_text, result.command_count, static_cast<unsigned>(type), info.kind,
          result.command_count);
      emit(line);
      break; // Never read the next link or guess a POD size for an unknown kind.
    }

    const bool oversize = info.size > WMTCommandCaptureMaxPODBytes;
    const std::size_t captured = oversize ? WMTCommandCaptureMaxPODBytes : info.size;
    if (oversize)
      result.truncated = true;
    const int prefix = std::snprintf(
        line, sizeof(line),
        "event=command family=%s index=%zu type=%u kind=%s pod_type=%s size=%zu "
        "captured=%zu truncated=%u oversize=%u pod_hex=",
        family_text, result.command_count, static_cast<unsigned>(type), info.kind,
        info.pod_type, info.size, captured, oversize ? 1u : 0u, oversize ? 1u : 0u);
    if (prefix < 0 || static_cast<std::size_t>(prefix) >= sizeof(line) ||
        captured * 2 >= sizeof(line) - static_cast<std::size_t>(prefix)) {
      result.truncated = true;
      result.stop_reason = "log_truncated";
      break;
    }

    const auto *bytes = static_cast<const unsigned char *>(current);
    static constexpr char hex[] = "0123456789abcdef";
    std::size_t pos = static_cast<std::size_t>(prefix);
    for (std::size_t i = 0; i < captured; ++i) {
      line[pos++] = hex[bytes[i] >> 4];
      line[pos++] = hex[bytes[i] & 15];
    }
    line[pos] = '\0';

    // memcpy avoids aliasing command structs through an unrelated base type.
    wmtcmd_base base;
    std::memcpy(&base, current, sizeof(base));
    emit(line);
    ++result.command_count;

#if defined(__i386__)
    if (base.next.high_part) {
      result.truncated = true;
      result.stop_reason = "inaccessible_next";
      break;
    }
#endif
    current = base.next.ptr;
  }

  if (!current)
    result.stop_reason = result.truncated ? "pod_oversize" : "complete";
  summary("end");
  return result;
}

} // namespace dxmt
