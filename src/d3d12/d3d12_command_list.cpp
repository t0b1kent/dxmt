/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Modified 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include "d3d12_object_grid.hpp"
#include "indirect-mesh-contract.hpp"
#include "indirect-mesh-sync.hpp"

#include "d3d12_command_allocator.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_frame_trace.hpp"
#include "d3d12_command_failure_trace.hpp"
#include "d3d12_recording_guard.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_format.hpp"
#include <cstdio>

namespace dxmt {

static bool
RecordingTraceEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FRAME_CAPTURE", value, sizeof(value)) == 1 &&
                        value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

enum class DirtyState {
  VertexBuffer,
  GraphicsRootArguments,
  GraphicsRootSignature,
  Viewport,
  ScissorRect,
  ComputeRootArguments,
  ComputeRootSignature,
  BlendFactor,
  StencilRef,
  GraphicsPipelineState,
  ComputePipelineState,
};

enum class DrawCallStatus {
  Invalid,
  Ordinary,
  Tessellation,
  Geometry,
};

inline bool
to_metal_primitive_type(D3D12_PRIMITIVE_TOPOLOGY topo, WMTPrimitiveType &primitive, uint32_t &control_point_num) {
  control_point_num = 0;
  switch (topo) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    primitive = WMTPrimitiveTypeLine;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    primitive = WMTPrimitiveTypeLineStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    primitive = WMTPrimitiveTypeTriangle;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    primitive = WMTPrimitiveTypeTriangleStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    // geometry
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST:
    primitive = WMTPrimitiveTypePoint;
    control_point_num = topo - 32;
    break;
  default:
    return false;
  }
  return true;
}

// `Graphics`CommandList is a really confusing name
class MTLD3D12GraphicsCommandListImpl : public MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList> {

  Com<MTLD3D12CommandAllocatorImpl, false> allocator_;

  /* state */

  Flags<DirtyState> dirty_state_;

  std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex_buffers_;

  uint64_t index_buffer_address;
  WMT::Buffer index_buffer;
  Rc<BufferAllocation> index_allocation_;
  std::array<Com<ID3D12DescriptorHeap>, 2> bound_heaps_;
  std::array<root_argument::HeapRange, 2> bound_heap_ranges_{};
  WMTIndexType index_type;
  uint64_t index_offset;
  uint32_t index_buffer_size = 0;
  uint32_t mesh_index_variant_ = 0;

  UINT num_rtvs;
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;

  D3D12_PRIMITIVE_TOPOLOGY topology_;

  UINT num_viewports;
  D3D12_VIEWPORT
  viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

  UINT num_scissors;
  D3D12_RECT
  scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

  Com<MTLD3D12GraphicsPipelineState, false> pso_graphics_;
  Com<MTLD3D12RootSignature, false> rootsig_graphics_;
  root_argument::State rootarg_graphics_staging_;

  Com<MTLD3D12ComputePipelineState, false> pso_compute_;
  Com<MTLD3D12RootSignature, false> rootsig_compute_;
  root_argument::State rootarg_compute_staging_;

  FLOAT blend_factor_[4];
  UINT8 stencil_ref_;

  struct ActiveQuery {
    MTLD3D12QueryHeap *heap;
    D3D12_QUERY_TYPE type;
    UINT index;
  };
  std::vector<ActiveQuery> active_queries_;
  HRESULT recording_error_ = S_OK;
  CommandRecordingState recording_state_;
  FirstRecordingFailure first_recording_failure_;
  uint64_t recording_invocation_ = 0;
  const uint64_t max_object_threadgroups_;

  void SetRecordingError(HRESULT hr, const char *operation, unsigned line) {
    recording_error_ = hr;
    first_recording_failure_.Capture(hr, operation, line, unsigned(topology_), pso_graphics_.ptr(), recording_invocation_);
  }

  bool RequireOpenRecording(const char *operation) {
    ++recording_invocation_;
    if (recording_state_.Allows(allocator_.ptr() && allocator_->encoder_last)) return true;
    TraceCommandFailure(CommandFailureOperation::RecordAfterClose, "command.record.failure", this,
                        FAILED(close_result) ? close_result : E_FAIL, operation, allocator_.ptr());
    return false;
  }

  const bool recording_trace_enabled_ = RecordingTraceEnabled();
  D3D12RecordingTrace recording_trace_;
  uint64_t recording_operation_ = 0;
  char recording_rtvs_[8][128] = {};
  const char *recording_predraw_reason_ = nullptr;

  template<typename... Args>
  void AppendRecordingTrace(const char *format, Args... args) {
    if (!recording_trace_enabled_)
      return;
    if (recording_trace_.entries_.size() >= 2048) {
      ++recording_trace_.dropped_;
      return;
    }
    const DWORD error = GetLastError();
    char line[512];
    const int length = std::snprintf(line, sizeof(line), format, args...);
    if (length < 0) {
      ++recording_trace_.dropped_;
    } else {
      if (size_t(length) >= sizeof(line)) {
        constexpr char marker[] = " [truncated]";
        memcpy(line + sizeof(line) - sizeof(marker), marker, sizeof(marker));
      }
      try {
        recording_trace_.entries_.emplace_back(line);
      } catch (...) {
        ++recording_trace_.dropped_;
      }
    }
    SetLastError(error);
  }

  template<typename... Args>
  void TraceRecording(const char *operation, const char *outcome, const char *reason,
                      const char *format, Args... args) {
    if (!std::strcmp(outcome, "rejected"))
      first_recording_failure_.Detail(recording_invocation_, operation, reason, format, args...);
    if (!recording_trace_enabled_)
      return;
    const auto id = ++recording_operation_;
    const auto rtv_count = std::min(num_rtvs, 8u);
    // Do not format or resolve anything once the bounded snapshot is full.
    if (recording_trace_.entries_.size() >= 2048) {
      recording_trace_.dropped_ += 1 + rtv_count;
      return;
    }
    const DWORD error = GetLastError();
    char detail[256];
    const int detail_length = std::snprintf(detail, sizeof(detail), format, args...);
    if (detail_length < 0) {
      detail[0] = 0;
    } else if (size_t(detail_length) >= sizeof(detail)) {
      constexpr char marker[] = " [truncated]";
      memcpy(detail + sizeof(detail) - sizeof(marker), marker, sizeof(marker));
    }
    const void *graphics_pso = static_cast<const void *>(pso_graphics_.ptr());
    const void *compute_pso = static_cast<const void *>(pso_compute_.ptr());
    AppendRecordingTrace("epoch=%llu call_id=%llu api=%s outcome=%s reason=%s graphics_pso=%p compute_pso=%p rtvs=%u %s",
                         static_cast<unsigned long long>(recording_trace_.epoch_),
                         static_cast<unsigned long long>(id), operation, outcome, reason, graphics_pso, compute_pso, num_rtvs, detail);
    for (UINT i = 0; i < rtv_count; ++i) {
      AppendRecordingTrace("epoch=%llu call_id=%llu api=%s outcome=metadata reason=render_target_state graphics_pso=%p target_slot=%u descriptor=%llx %s",
                           static_cast<unsigned long long>(recording_trace_.epoch_),
                           static_cast<unsigned long long>(id), operation, graphics_pso, i,
                           static_cast<unsigned long long>(rtvs[i].ptr), recording_rtvs_[i][0] ? recording_rtvs_[i] :
                           (rtvs[i].ptr ? "resolution=deferred_until_PreDraw" : "resolution=unbound"));
    }
    SetLastError(error);
  }

  bool ValidateQuery(MTLD3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index, UINT count, const char *operation) {
    if (encoder_count != std::numeric_limits<size_t>::max()) {
      WARN("Query command ignored on a closed command list");
      return false;
    }
    if (heap && (heap->type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS || heap->type == D3D12_QUERY_HEAP_TYPE_SO_STATISTICS)) {
      static std::atomic_uint trace_count{0};
      if (trace_count.fetch_add(1, std::memory_order_relaxed) < 16)
        fprintf(stderr, "dx12_statistics_storage_use operation=%s type=%u index=%u count=%u hr=80004001\n",
                operation, unsigned(type), index, count);
      SetRecordingError(E_NOTIMPL, __func__, __LINE__);
      return false;
    }
    const bool timestamp = type == D3D12_QUERY_TYPE_TIMESTAMP;
    const bool queue_valid = allocator_->type_ == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        (timestamp && allocator_->type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE);
    if (!heap || heap->owner != device_ || !queue_valid ||
        (timestamp ? heap->type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP :
         (heap->type != D3D12_QUERY_HEAP_TYPE_OCCLUSION ||
          (type != D3D12_QUERY_TYPE_OCCLUSION && type != D3D12_QUERY_TYPE_BINARY_OCCLUSION))) ||
        index > heap->count || count > heap->count - index) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return false;
    }
    return true;
  }

public:
  MTLD3D12GraphicsCommandListImpl(MTLD3D12Device *pDevice) : MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList>(pDevice),
      max_object_threadgroups_(ObjectGridLimit(pDevice->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7))) {}

  ~MTLD3D12GraphicsCommandListImpl() {}

  const std::vector<std::string> &GetRecordingTrace() const override { return recording_trace_.GetRecordingTrace(); }
  uint64_t GetRecordingTraceEpoch() const override { return recording_trace_.GetRecordingTraceEpoch(); }
  uint64_t GetRecordingTraceDropped() const override { return recording_trace_.GetRecordingTraceDropped(); }

  void
  ResetState(ID3D12PipelineState *pInitialPipelineState) {
    pso_graphics_ = nullptr;
    pso_compute_ = nullptr;
    if (auto pso = static_cast<MTLD3D12PipelineState *>(pInitialPipelineState)) {
      if (!pso->IsComputePipelineState)
        pso_graphics_ = static_cast<MTLD3D12GraphicsPipelineState *>(pInitialPipelineState);
      else
        pso_compute_ = static_cast<MTLD3D12ComputePipelineState *>(pInitialPipelineState);
    }

    num_rtvs = {};
    memset(rtvs, 0, sizeof(rtvs));
    dsv = {};

    topology_ = {};

    num_viewports = {};
    memset(viewports, 0, sizeof(viewports));

    num_scissors = {};
    memset(scissors, 0, sizeof(scissors));

    blend_factor_[0] = 1.0f;
    blend_factor_[1] = 1.0f;
    blend_factor_[2] = 1.0f;
    blend_factor_[3] = 1.0f;
    stencil_ref_ = 0;

    rootsig_graphics_ = nullptr;
    rootarg_graphics_staging_ = {};

    rootsig_compute_ = nullptr;
    rootarg_compute_staging_ = {};

    bound_heaps_ = {};
    bound_heap_ranges_ = {};
    memset(vertex_buffers_.data(), 0, sizeof(vertex_buffers_));

    index_buffer_address = 0;
    index_buffer = {};
    index_allocation_ = nullptr;
    index_type = {};
    index_offset = 0;
    index_buffer_size = 0;
    mesh_index_variant_ = 0;

    dirty_state_.clrAll();
    if (recording_trace_enabled_)
      memset(recording_rtvs_, 0, sizeof(recording_rtvs_));
  }

  HRESULT
  Initialize(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialPipelineState) {
    auto allocator = static_cast<MTLD3D12CommandAllocatorImpl *>(pAllocator);

    if (allocator_ != allocator)
      allocator_ = allocator;

    ResetState(pInitialPipelineState);

    active_queries_.clear();
    recording_state_.End();
    first_recording_failure_.Clear();
    recording_error_ = S_OK;
    recording_trace_.entries_.clear();
    ++recording_trace_.epoch_;
    recording_trace_.dropped_ = 0;
    recording_operation_ = 0;

    encoder_count = std::numeric_limits<size_t>::max();
    close_result = E_FAIL;
    const HRESULT hr = allocator_->StartRecord(&entry);
    recording_state_.Begin(SUCCEEDED(hr));
    TraceRecording("Reset", FAILED(hr) ? "rejected" : "accepted", FAILED(hr) ? "StartRecord_failed" : "new_recording",
                   "hr=%08x initial_pso=%p", unsigned(hr), static_cast<const void *>(pInitialPipelineState));
    return TraceCommandFailure(CommandFailureOperation::ListInitialize, "list.initialize.failure", this,
                               hr, "StartRecord", pAllocator);
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList1) || riid == __uuidof(ID3D12GraphicsCommandList2)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12GraphicsCommandList), riid)) {
      WARN("D3D12GraphicsCommandList: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE
  GetType() {
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
  }

  HRESULT STDMETHODCALLTYPE
  Close() {
    ++recording_invocation_;
    if (!recording_state_.open || encoder_count < std::numeric_limits<size_t>::max()) {
      TraceRecording("Close", "rejected", "already_closed", "hr=%08x", unsigned(E_FAIL));
      return TraceCommandFailure(CommandFailureOperation::ListClose, "list.close.failure", this,
                                 E_FAIL, "already_closed", allocator_.ptr(), encoder_count);
    }
    if (!active_queries_.empty())
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
    HRESULT hr = allocator_->EndRecord(&encoder_count);
    recording_state_.End();
    close_result = FAILED(hr) ? hr : recording_error_;
    TraceRecording("Close", FAILED(close_result) ? "rejected" : "accepted",
                   FAILED(hr) ? "EndRecord_failed" : !active_queries_.empty() ? "active_queries" :
                   FAILED(recording_error_) ? "recording_error" : "closed",
                   "hr=%08x end_record_hr=%08x encoders=%llu", unsigned(close_result), unsigned(hr),
                   static_cast<unsigned long long>(encoder_count));
    if (FAILED(close_result)) first_recording_failure_.Trace(this);
    return TraceCommandFailure(CommandFailureOperation::ListClose, "list.close.failure", this,
                               close_result, FAILED(hr) ? "EndRecord" : !active_queries_.empty() ?
                               "active_queries" : "recording_error", allocator_.ptr(), encoder_count);
  };

  HRESULT STDMETHODCALLTYPE
  Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) {
    ++recording_invocation_;
    if (encoder_count == std::numeric_limits<size_t>::max()) {
      TraceRecording("Reset", "rejected", "still_recording", "hr=%08x", unsigned(E_FAIL));
      return TraceCommandFailure(CommandFailureOperation::ListReset, "list.reset.failure", this,
                                 E_FAIL, "still_recording", pAllocator);
    }
    if (FAILED(close_result)) {
      TraceRecording("Reset", "rejected", "previous_Close_failed", "hr=%08x", unsigned(close_result));
      return TraceCommandFailure(CommandFailureOperation::ListReset, "list.reset.failure", this,
                                 close_result, "previous_Close_failed", pAllocator);
    }
    return Initialize(pAllocator, pInitialState);
  };

  void STDMETHODCALLTYPE
  ClearState(ID3D12PipelineState *pPipelineState) {
    if (!RequireOpenRecording("ClearState")) return;
    try {
    allocator_->InvalidateCurrentPass();
    ResetState(pPipelineState);
    TraceRecording("ClearState", "accepted", "state_reset", "initial_pso=%p", static_cast<const void *>(pPipelineState));
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearState", __LINE__);
      TraceRecording("ClearState", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  std::tuple<uint64_t, uint64_t>
  PopulateVertexBufferTable(uint32_t Count) {
    auto slot_mask = pso_graphics_ ? pso_graphics_->slot_mask : 0;
    if (!slot_mask)
      return {0, 0};
    uint32_t max_slot = 32 - __builtin_clz(slot_mask);
    struct VERTEX_BUFFER_ENTRY {
      uint64_t buffer_handle;
      uint32_t stride;
      uint32_t length;
    };
    auto stride = align(sizeof(VERTEX_BUFFER_ENTRY) * max_slot, 16);

    auto [mapped, offset] = allocator_->AllocateGPUHeap(stride * Count, 16);

    for (unsigned i = 0; i < Count; i++) {
      VERTEX_BUFFER_ENTRY *entries = (VERTEX_BUFFER_ENTRY *)(reinterpret_cast<char *>(mapped) + i * stride);
      for (unsigned slot = 0, index = 0; slot < max_slot; slot++) {
        if (!(slot_mask & (1 << slot)))
          continue;
        auto &state = vertex_buffers_[slot];
        entries[index].buffer_handle = state.BufferLocation;
        entries[index].stride = state.StrideInBytes;
        entries[index++].length = state.SizeInBytes;
      };
    }

    return {offset, stride};
  }

  DrawCallStatus
  PreDraw(bool SkipResourceBinding = false, uint32_t MeshIndexVariant = 0) {
    if (recording_trace_enabled_)
      recording_predraw_reason_ = nullptr;
    if (!pso_graphics_) {
      if (recording_trace_enabled_)
        recording_predraw_reason_ = "PreDraw_invalid_missing_graphics_pso";
      return DrawCallStatus::Invalid;
    }
    const bool mesh_pipeline = pso_graphics_->tess_control_points || pso_graphics_->geometry_pipeline;
    if (MeshIndexVariant > 2 || (MeshIndexVariant &&
        (!mesh_pipeline || !pso_graphics_->mesh_indexed_pso[MeshIndexVariant - 1]))) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      if (recording_trace_enabled_)
        recording_predraw_reason_ = MeshIndexVariant > 2 ? "PreDraw_invalid_mesh_index_variant" :
                                   "PreDraw_unavailable_mesh_indexed_pso";
      return DrawCallStatus::Invalid;
    }
    if (mesh_index_variant_ != MeshIndexVariant) {
      mesh_index_variant_ = MeshIndexVariant;
      dirty_state_.set(DirtyState::GraphicsPipelineState);
    }
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render) {

      allocator_->InvalidateCurrentPass();
      auto render = allocator_->AllocatePass<RenderEncoderData>();
      render->type = EncoderType::Render;
      render->cmd_head.type = WMTRenderCommandNop;
      render->cmd_head.next.set(0);
      render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
      render->dsv_planar_flags = 0;
      render->dsv_readonly_flags = 0;
      render->render_target_count = num_rtvs;

      if (!active_queries_.empty()) {
        render->visibility_accumulate = active_queries_.front().heap->accumulate;
        for (const auto &query : active_queries_)
          render->visibility_targets.push_back({query.heap->results, uint64_t(query.index) * sizeof(uint64_t)});
        auto &visibility = allocator_->EncodeRenderCommand<wmtcmd_render_setvisibilitymode>();
        visibility.type = WMTRenderCommandSetVisibilityMode;
        visibility.mode = WMTVisibilityResultModeCounting;
        visibility.offset = 0;
      }

      unsigned render_target_width = 16384, render_target_height = 16384, render_target_array_length = 0;

      unsigned effective_rtvs = 0;
      if (recording_trace_enabled_)
        memset(recording_rtvs_, 0, sizeof(recording_rtvs_));
      for (unsigned i = 0; i < num_rtvs; i++) {
        if (!rtvs[i].ptr)
          continue;
        effective_rtvs++;
        auto [Heap, Index] = GetRenderTargetHeap(device_, rtvs[i]);
        auto AttachmentDesc = Heap->GetRenderTarget(Index);
        if (!AttachmentDesc.Texture) {
          if (recording_trace_enabled_ && i < 8)
            std::snprintf(recording_rtvs_[i], sizeof(recording_rtvs_[i]), "resolution=no_texture");
          continue;
        }
        auto &rt = render->colors[i];
        rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
        rt.depth_plane = AttachmentDesc.DepthPlane;
        rt.load_action = WMTLoadActionLoad;
        rt.store_action = WMTStoreActionStore;
        // Copy only values from the view already resolved for the actual command.
        if (recording_trace_enabled_ && i < 8) {
          const DWORD error = GetLastError();
          std::snprintf(recording_rtvs_[i], sizeof(recording_rtvs_[i]), "resolution=resolved texture=%llx width=%u height=%u",
                        static_cast<unsigned long long>(rt.attachment.texture().handle),
                        unsigned(AttachmentDesc.Width), unsigned(AttachmentDesc.Height));
          SetLastError(error);
        }
        static std::atomic<uint32_t> trace_target{0};
        TraceFrame(trace_target, "render.target", this, "slot=%u texture=%llx width=%u height=%u pso=%p",
                   i, static_cast<unsigned long long>(rt.attachment.texture().handle),
                   unsigned(AttachmentDesc.Width), unsigned(AttachmentDesc.Height),
                   static_cast<const void *>(pso_graphics_.ptr()));
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
      }
      while (dsv.ptr) {
        effective_rtvs++;
        auto [Heap, Index] = GetRenderTargetHeap(device_, dsv);
        auto AttachmentDesc = Heap->GetRenderTarget(Index);
        if (!AttachmentDesc.Texture)
          continue;
        auto dsv_planar_flags = DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View));
        if (dsv_planar_flags & 1) {
          auto &rt = render->depth;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        if (dsv_planar_flags & 2) {
          auto &rt = render->stencil;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        render->dsv_planar_flags = dsv_planar_flags;
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
        break;
      }
      render->render_target_width = render_target_width;
      render->render_target_height = render_target_height;
      render->render_target_array_length = render_target_array_length;
      if (effective_rtvs == 0) {
        render->default_raster_sample_count = std::max(1u, pso_graphics_->forced_sample_count);
      }

      dirty_state_.set(DirtyState::VertexBuffer, DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
      dirty_state_.set(DirtyState::Viewport, DirtyState::ScissorRect);
      dirty_state_.set(DirtyState::BlendFactor, DirtyState::StencilRef);
      dirty_state_.set(DirtyState::GraphicsPipelineState);
    }

    if (!pso_graphics_) {
      if (recording_trace_enabled_)
        recording_predraw_reason_ = "PreDraw_invalid_missing_graphics_pso_after_pass";
      return DrawCallStatus::Invalid;
    }

    if (dirty_state_.test(DirtyState::GraphicsPipelineState)) {
      auto &cmd_setpso = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
      cmd_setpso.type = WMTRenderCommandSetPSO;
      cmd_setpso.pso = MeshIndexVariant ? pso_graphics_->mesh_indexed_pso[MeshIndexVariant - 1] : pso_graphics_->pso;

      auto &cmd_setdsso = allocator_->EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
      cmd_setdsso.type = WMTRenderCommandSetDepthStencilState;
      cmd_setdsso.depth_stencil_state = pso_graphics_->GetDepthStencilState(
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_planar_flags,
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_readonly_flags
      );

      auto &cmd_setrs = allocator_->EncodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      cmd_setrs.type = WMTRenderCommandSetRasterizerState;
      cmd_setrs.cull_mode = pso_graphics_->cull_mode;
      cmd_setrs.depth_clip_mode = pso_graphics_->depth_clip_mode;
      cmd_setrs.fill_mode = pso_graphics_->fill_mode;
      cmd_setrs.depth_bias = pso_graphics_->depth_bias;
      cmd_setrs.depth_bias_clamp = pso_graphics_->depth_bias_clamp;
      cmd_setrs.scole_scale = pso_graphics_->scole_scale;
      cmd_setrs.winding = pso_graphics_->winding;

      dirty_state_.clr(DirtyState::GraphicsPipelineState);
    }
    if (dirty_state_.test(DirtyState::VertexBuffer)) {
      auto [Offset, Stride] = PopulateVertexBufferTable(1);
      if (Stride) {
        auto &cmd_vsvb = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_vsvb.type = mesh_pipeline ? WMTRenderCommandSetObjectBuffer : WMTRenderCommandSetVertexBuffer;
        cmd_vsvb.buffer = allocator_->gpu_heap_buffer_;
        cmd_vsvb.offset = Offset;
        cmd_vsvb.index = SM50_BINDING_INDEX_VERTEX_BUFFER;
      }
      dirty_state_.clr(DirtyState::VertexBuffer);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootArguments) && !SkipResourceBinding) {
      if (rootsig_graphics_) {
        auto Offset = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_.words.data());
        auto &cmd_vsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_vsargbuf.type = mesh_pipeline ? WMTRenderCommandSetObjectBuffer : WMTRenderCommandSetVertexBuffer;
        cmd_vsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_vsargbuf.offset = Offset;
        cmd_vsargbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        if (mesh_pipeline) {
          auto &cmd_dsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          cmd_dsargbuf.type = WMTRenderCommandSetMeshBuffer;
          cmd_dsargbuf.buffer = allocator_->gpu_heap_buffer_;
          cmd_dsargbuf.offset = Offset;
          cmd_dsargbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        }
        auto &cmd_fsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_fsargbuf.type = WMTRenderCommandSetFragmentBuffer;
        cmd_fsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_fsargbuf.offset = Offset;
        cmd_fsargbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
      }
      dirty_state_.clr(DirtyState::GraphicsRootArguments);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootSignature) && !SkipResourceBinding) {
      if (rootsig_graphics_) {
        auto Offset = EncodeStaticSamplers(rootsig_graphics_.ptr());
        auto &cmd_vsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_vsargbuf.type = mesh_pipeline ? WMTRenderCommandSetObjectBuffer : WMTRenderCommandSetVertexBuffer;
        cmd_vsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_vsargbuf.offset = Offset;
        cmd_vsargbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
        if (mesh_pipeline) {
          auto &cmd_dsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          cmd_dsargbuf.type = WMTRenderCommandSetMeshBuffer;
          cmd_dsargbuf.buffer = allocator_->gpu_heap_buffer_;
          cmd_dsargbuf.offset = Offset;
          cmd_dsargbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
        }
        auto &cmd_fsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_fsargbuf.type = WMTRenderCommandSetFragmentBuffer;
        cmd_fsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_fsargbuf.offset = Offset;
        cmd_fsargbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
      }
      dirty_state_.clr(DirtyState::GraphicsRootSignature);
    }

    if (dirty_state_.test(DirtyState::Viewport)) {
      static std::atomic<uint32_t> trace_viewport{0};
      TraceFrame(trace_viewport, "render.viewport", this, "count=%u first=%f,%f,%f,%f depth=%f,%f",
                 num_viewports, double(viewports[0].TopLeftX), double(viewports[0].TopLeftY),
                 double(viewports[0].Width), double(viewports[0].Height),
                 double(viewports[0].MinDepth), double(viewports[0].MaxDepth));
      auto metal_viewport = allocator_->AllocateCommandData<WMTViewport>(num_viewports);
      for (auto i = 0u; i < num_viewports; i++) {
        auto &viewport = viewports[i];
        metal_viewport[i] = {viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                             viewport.Height,   viewport.MinDepth, viewport.MaxDepth};
      }
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setviewports>();
      cmd.type = WMTRenderCommandSetViewports;
      cmd.viewports.set(metal_viewport);
      cmd.viewport_count = num_viewports;
      dirty_state_.clr(DirtyState::Viewport);
    }

    if (dirty_state_.test(DirtyState::ScissorRect)) {
      static std::atomic<uint32_t> trace_scissor{0};
      TraceFrame(trace_scissor, "render.scissor", this, "count=%u first=%d,%d,%d,%d",
                 num_scissors, int(scissors[0].left), int(scissors[0].top), int(scissors[0].right), int(scissors[0].bottom));
      auto metal_scissors = allocator_->AllocateCommandData<WMTScissorRect>(num_viewports /* yes */);
      for (auto i = 0u; i < num_viewports; i++) {
        if (i < num_scissors) {
          auto &d3d_rect = scissors[i];
          LONG left = std::clamp(d3d_rect.left, (LONG)0, (LONG)16384);
          LONG top = std::clamp(d3d_rect.top, (LONG)0, (LONG)16384);
          LONG right = std::clamp(d3d_rect.right, left, (LONG)16384);
          LONG bottom = std::clamp(d3d_rect.bottom, top, (LONG)16384);
          metal_scissors[i] = {uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top)};
        } else {
          metal_scissors[i] = {0, 0, 16384, 16384};
        }
      }
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setscissorrects>();
      cmd.type = WMTRenderCommandSetScissorRects;
      cmd.scissor_rects.set(metal_scissors);
      cmd.rect_count = num_viewports;
      dirty_state_.clr(DirtyState::ScissorRect);
    }

    if (dirty_state_.test(DirtyState::BlendFactor)) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setblendcolor>();
      cmd.type = WMTRenderCommandSetBlendFactor;
      cmd.red = blend_factor_[0];
      cmd.green = blend_factor_[1];
      cmd.blue = blend_factor_[2];
      cmd.alpha = blend_factor_[3];
      dirty_state_.clr(DirtyState::BlendFactor);
    }

    if (dirty_state_.test(DirtyState::StencilRef)) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setstencilref>();
      cmd.type = WMTRenderCommandSetStencilRef;
      cmd.stencil_ref = stencil_ref_;
      dirty_state_.clr(DirtyState::StencilRef);
    }

    if (pso_graphics_->geometry_pipeline)
      return DrawCallStatus::Geometry;
    return pso_graphics_->tess_control_points ? DrawCallStatus::Tessellation : DrawCallStatus::Ordinary;
  }

  void STDMETHODCALLTYPE
  DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) {
    if (!RequireOpenRecording("DrawInstanced")) return;
    try {
    static std::atomic<uint32_t> trace_enter{0}, trace_record{0};
    TraceFrame(trace_enter, "draw.enter", this, "count=%u instances=%u pso=%p topology=%u hr=%08x",
               VertexCountPerInstance, InstanceCount, static_cast<const void *>(pso_graphics_.ptr()),
               unsigned(topology_), unsigned(recording_error_));
    const auto trace_outcome = [&](const char *outcome, const char *reason) {
      TraceRecording("DrawInstanced", outcome, reason,
                     "count=%u instances=%u start_vertex=%u start_instance=%u topology=%u zero_count=%u zero_instances=%u recording_hr=%08x",
                     VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation,
                     unsigned(topology_), unsigned(!VertexCountPerInstance), unsigned(!InstanceCount), unsigned(recording_error_));
    };
    const auto trace_recorded = [&](const char *type) {
      trace_outcome("emitted", type);
      TraceFrame(trace_record, "draw.recorded", this, "type=%s count=%u instances=%u pso=%p hr=%08x", type,
                 VertexCountPerInstance, InstanceCount, static_cast<const void *>(pso_graphics_.ptr()), unsigned(recording_error_));
    };
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "invalid_topology");
      return;
    }
    if (!pso_graphics_) {
      trace_outcome("rejected", "missing_graphics_pso");
      return;
    }
    if (cp_count != pso_graphics_->tess_control_points) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "patch_control_point_mismatch");
      return;
    }
    if (pso_graphics_->geometry_pipeline) {
      const auto expected = pso_graphics_->geometry_input_vertices == 1 ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST :
                            pso_graphics_->geometry_input_vertices == 2 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      if (topology_ != expected) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        trace_outcome("rejected", "geometry_input_topology_mismatch");
        return;
      }
      if (VertexCountPerInstance < pso_graphics_->geometry_input_vertices || !InstanceCount) {
        trace_outcome("rejected", !VertexCountPerInstance ? "zero_vertex_count" : !InstanceCount ? "zero_instance_count" :
                      "geometry_incomplete_primitive");
        return;
      }
      if (VertexCountPerInstance % pso_graphics_->geometry_input_vertices) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "geometry_partial_primitive_unsupported");
        return;
      }
      const uint32_t vertices_per_group = pso_graphics_->geometry_vertices_per_group;
      const auto groups = 1 + (VertexCountPerInstance - 1) / vertices_per_group;
      if (!ObjectGridFits(groups, InstanceCount, max_object_threadgroups_)) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "geometry_object_group_limit");
        return;
      }
      if (PreDraw() != DrawCallStatus::Geometry) {
        trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_expected_geometry");
        return;
      }
      const D3D12_DRAW_ARGUMENTS arguments = {
          VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation};
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(arguments), 32);
      memcpy(mapped, &arguments, sizeof(arguments));
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = allocator_->gpu_heap_buffer_;
      binding.offset = 0;
      binding.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_geometry_draw>();
      draw.type = WMTRenderCommandDXMTGeometryDraw;
      draw.draw_arguments_offset = offset;
      draw.instance_count = InstanceCount;
      draw.warp_count = groups;
      draw.vertex_per_warp = vertices_per_group;
      trace_recorded("geometry");
      return;
    }
    if (cp_count) {
      const auto threads = pso_graphics_->tess_threads_per_patch;
      const auto patches = VertexCountPerInstance / cp_count;
      if (!patches || !InstanceCount) {
        trace_outcome("rejected", !VertexCountPerInstance ? "zero_vertex_count" : !InstanceCount ? "zero_instance_count" :
                      "tessellation_zero_complete_patches");
        return;
      }
      if (!threads || threads > 32 || 32 % threads) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        trace_outcome("rejected", "invalid_tessellation_threads_per_patch");
        return;
      }
      const auto patches_per_group = 32 / threads;
      const auto groups = 1 + (patches - 1) / patches_per_group;
      // Object-grid policy is device-specific; child mesh limits remain in the PSO.
      if (!ObjectGridFits(groups, InstanceCount, max_object_threadgroups_)) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "tessellation_object_group_limit");
        return;
      }
      if (PreDraw() != DrawCallStatus::Tessellation) {
        trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_expected_tessellation");
        return;
      }
      const D3D12_DRAW_ARGUMENTS arguments = {
          VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation};
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(arguments), 32);
      memcpy(mapped, &arguments, sizeof(arguments));
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = allocator_->gpu_heap_buffer_;
      binding.offset = 0;
      binding.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw>();
      draw.type = WMTRenderCommandDXMTTessellationMeshDraw;
      draw.draw_arguments_offset = offset;
      draw.instance_count = InstanceCount;
      draw.threads_per_patch = threads;
      draw.patch_per_group = patches_per_group;
      draw.patch_per_mesh_instance = groups;
      trace_recorded("tessellation");
      return;
    }
    DrawCallStatus status = PreDraw();
    if (status == DrawCallStatus::Invalid) {
      trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_invalid");
      return;
    }

    auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
    cmd_draw.type = WMTRenderCommandDraw;
    cmd_draw.primitive_type = primitive_type;
    cmd_draw.base_instance = StartInstanceLocation;
    cmd_draw.instance_count = InstanceCount;
    cmd_draw.vertex_start = StartVertexLocation;
    cmd_draw.vertex_count = VertexCountPerInstance;
    trace_recorded(!VertexCountPerInstance || !InstanceCount ? "ordinary_zero_work" : "ordinary");
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "DrawInstanced", __LINE__);
      TraceRecording("DrawInstanced", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  bool RetainIndexAllocation() {
    auto *pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    try {
      if (pass->index_allocations.empty() || pass->index_allocations.back() != index_allocation_)
        pass->index_allocations.push_back(index_allocation_);
      return true;
    } catch (const std::bad_alloc &) {
      SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);
      return false;
    }
  }

  void STDMETHODCALLTYPE
  DrawIndexedInstanced(
      UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
      UINT StartInstanceLocation
  ) {
    if (!RequireOpenRecording("DrawIndexedInstanced")) return;
    try {
    static std::atomic<uint32_t> trace_enter{0}, trace_record{0};
    TraceFrame(trace_enter, "draw_indexed.enter", this, "count=%u instances=%u pso=%p topology=%u hr=%08x",
               IndexCountPerInstance, InstanceCount, static_cast<const void *>(pso_graphics_.ptr()),
               unsigned(topology_), unsigned(recording_error_));
    const auto trace_outcome = [&](const char *outcome, const char *reason) {
      TraceRecording("DrawIndexedInstanced", outcome, reason,
                     "count=%u instances=%u start_index=%u base_vertex=%d start_instance=%u topology=%u zero_count=%u zero_instances=%u recording_hr=%08x",
                     IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation,
                     unsigned(topology_), unsigned(!IndexCountPerInstance), unsigned(!InstanceCount), unsigned(recording_error_));
    };
    const auto trace_recorded = [&](const char *type) {
      trace_outcome("emitted", type);
      TraceFrame(trace_record, "draw_indexed.recorded", this, "type=%s count=%u instances=%u pso=%p hr=%08x", type,
                 IndexCountPerInstance, InstanceCount, static_cast<const void *>(pso_graphics_.ptr()), unsigned(recording_error_));
    };
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "invalid_topology");
      return;
    }
    if (!pso_graphics_ || cp_count != pso_graphics_->tess_control_points) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", !pso_graphics_ ? "missing_graphics_pso" : "patch_control_point_mismatch");
      return;
    }
    if (!IndexCountPerInstance || !InstanceCount) {
      trace_outcome("rejected", !IndexCountPerInstance ? "zero_index_count" : "zero_instance_count");
      return;
    }
    const uint32_t index_size = index_type == WMTIndexTypeUInt32 ? 4 : 2;
    if (!index_buffer || uint64_t(StartIndexLocation) + IndexCountPerInstance > index_buffer_size / index_size) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", !index_buffer ? "missing_index_buffer" : "index_buffer_range_exceeded");
      return;
    }
    if (pso_graphics_->geometry_pipeline) {
      const auto vertices = pso_graphics_->geometry_input_vertices;
      const auto expected = vertices == 1 ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST :
                            vertices == 2 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      if (topology_ != expected) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        trace_outcome("rejected", "geometry_input_topology_mismatch");
        return;
      }
      if (IndexCountPerInstance % vertices) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "geometry_partial_primitive_unsupported");
        return;
      }
      const auto vertices_per_group = pso_graphics_->geometry_vertices_per_group;
      const auto groups = 1 + (IndexCountPerInstance - 1) / vertices_per_group;
      if (!ObjectGridFits(groups, InstanceCount, max_object_threadgroups_)) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "geometry_object_group_limit");
        return;
      }
      if (PreDraw(false, index_size == 4 ? 2 : 1) != DrawCallStatus::Geometry) {
        trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_expected_geometry");
        return;
      }
      if (!RetainIndexAllocation()) return;
      const D3D12_DRAW_INDEXED_ARGUMENTS arguments = {
          IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation};
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(arguments), 32);
      memcpy(mapped, &arguments, sizeof(arguments));
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = allocator_->gpu_heap_buffer_;
      binding.offset = 0;
      binding.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_geometry_draw_indexed>();
      draw.type = WMTRenderCommandDXMTGeometryDrawIndexed;
      draw.draw_arguments_offset = offset;
      draw.instance_count = InstanceCount;
      draw.warp_count = groups;
      draw.vertex_per_warp = vertices_per_group;
      draw.index_buffer = index_buffer;
      // The object shader applies StartIndexLocation; bind only the view base.
      draw.index_buffer_offset = index_offset;
      trace_recorded("geometry");
      return;
    }
    if (cp_count) {
      const auto patches = IndexCountPerInstance / cp_count;
      const auto threads = pso_graphics_->tess_threads_per_patch;
      if (!patches) {
        trace_outcome("rejected", "tessellation_zero_complete_patches");
        return;
      }
      if (!threads || threads > 32 || 32 % threads) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        trace_outcome("rejected", "invalid_tessellation_threads_per_patch");
        return;
      }
      const auto patches_per_group = 32 / threads;
      const auto groups = 1 + (patches - 1) / patches_per_group;
      if (!ObjectGridFits(groups, InstanceCount, max_object_threadgroups_)) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "tessellation_object_group_limit");
        return;
      }
      if (PreDraw(false, index_size == 4 ? 2 : 1) != DrawCallStatus::Tessellation) {
        trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_expected_tessellation");
        return;
      }
      const D3D12_DRAW_INDEXED_ARGUMENTS arguments = {
          IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation};
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(arguments), 32);
      memcpy(mapped, &arguments, sizeof(arguments));
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = allocator_->gpu_heap_buffer_;
      binding.offset = 0;
      binding.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      if (!RetainIndexAllocation()) return;
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw_indexed>();
      draw.type = WMTRenderCommandDXMTTessellationMeshDrawIndexed;
      draw.draw_arguments_offset = offset;
      draw.instance_count = InstanceCount;
      draw.threads_per_patch = threads;
      draw.patch_per_group = patches_per_group;
      draw.patch_per_mesh_instance = groups;
      draw.index_buffer = index_buffer;
      // The hull shader applies StartIndex; bind only the index view's base.
      draw.index_buffer_offset = index_offset;
      trace_recorded("tessellation");
      return;
    }
    DrawCallStatus status = PreDraw();
    if (status == DrawCallStatus::Invalid) {
      trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_invalid");
      return;
    }
    if (!RetainIndexAllocation()) return;
    auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indexed>();
    cmd_draw.type = WMTRenderCommandDrawIndexed;
    cmd_draw.primitive_type = primitive_type;
    cmd_draw.index_type = index_type;
    cmd_draw.index_count = IndexCountPerInstance;
    cmd_draw.index_buffer = index_buffer;
    cmd_draw.index_buffer_offset = index_offset + uint64_t(StartIndexLocation) * index_size;
    cmd_draw.instance_count = InstanceCount;
    cmd_draw.base_vertex = BaseVertexLocation;
    cmd_draw.base_instance = StartInstanceLocation;
    trace_recorded("ordinary");
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "DrawIndexedInstanced", __LINE__);
      TraceRecording("DrawIndexedInstanced", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  uint64_t
  EncodeRootArgument(MTLD3D12RootSignature *pRootSig, uint64_t const pStaging[64], UINT Count = 1) {
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(sizeof(uint64_t) * pRootSig->UploadQwords * Count, 64);
    for (unsigned i = 0; i < Count; i++)
      memcpy(
          reinterpret_cast<uint64_t *>(Ptr) + i * pRootSig->UploadQwords, pStaging,
          pRootSig->UploadQwords * sizeof(uint64_t)
      );
    return Offset;
  }

  uint64_t
  EncodeStaticSamplers(MTLD3D12RootSignature *pRootSig) {
    auto static_sampler_encode_size = sizeof(uint64_t) * pRootSig->NumStaticSamplers * 4;
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(static_sampler_encode_size, 64);
    memcpy(Ptr, pRootSig->EncodedStaticSamplers, static_sampler_encode_size);
    return Offset;
  }

  bool
  PreDispatch(bool SkipResourceBinding = false) {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Compute) {
      allocator_->InvalidateCurrentPass();
      auto compute = allocator_->AllocatePass<ComputeEncoderData>();
      compute->type = EncoderType::Compute;
      compute->cmd_head.type = WMTComputeCommandNop;
      compute->cmd_head.next.set(0);
      compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
      dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
      dirty_state_.set(DirtyState::ComputePipelineState);
    }

    if (!pso_compute_)
        return false;

    if (dirty_state_.test(DirtyState::ComputePipelineState)) {
      auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      cmd_setpso.type = WMTComputeCommandSetPSO;
      cmd_setpso.pso = pso_compute_->pso;
      cmd_setpso.threadgroup_size = pso_compute_->threadgroup_size;
      dirty_state_.clr(DirtyState::ComputePipelineState);
    }

    if (dirty_state_.test(DirtyState::ComputeRootArguments) && !SkipResourceBinding) {
      if (rootsig_compute_) {
        auto Offset = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_.words.data());
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        cmd_argbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_argbuf.offset = Offset;
        cmd_argbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
      }
      dirty_state_.clr(DirtyState::ComputeRootArguments);
    }

    if (dirty_state_.test(DirtyState::ComputeRootSignature) && !SkipResourceBinding) {
      if (rootsig_compute_) {
        auto Offset = EncodeStaticSamplers(rootsig_compute_.ptr());
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        cmd_argbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_argbuf.offset = Offset;
        cmd_argbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
      }
      dirty_state_.clr(DirtyState::ComputeRootSignature);
    }

    return true;
  }

  void STDMETHODCALLTYPE
  Dispatch(UINT X, UINT Y, UINT Z) {
    if (!RequireOpenRecording("Dispatch")) return;
    try {
    if (!PreDispatch()) {
      TraceRecording("Dispatch", "rejected", "PreDispatch_false_missing_compute_pso", "x=%u y=%u z=%u zero_work=%u",
                     X, Y, Z, unsigned(!X || !Y || !Z));
      return;
    }

    auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    cmd_dispatch.type = WMTComputeCommandDispatch;
    cmd_dispatch.size = {X, Y, Z};
    TraceRecording("Dispatch", "emitted", !X || !Y || !Z ? "zero_work" : "compute_command",
                   "x=%u y=%u z=%u zero_work=%u", X, Y, Z, unsigned(!X || !Y || !Z));
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "Dispatch", __LINE__);
      TraceRecording("Dispatch", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  bool
  PreBlit() {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Blit) {
      allocator_->InvalidateCurrentPass();
      auto render = allocator_->AllocatePass<BlitEncoderData>();
      render->type = EncoderType::Blit;
      render->cmd_head.type = WMTBlitCommandNop;
      render->cmd_head.next.set(0);
      render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
    }
    return true;
  }

  void STDMETHODCALLTYPE
  CopyBufferRegion(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 ByteCount
  ) {
    if (!RequireOpenRecording("CopyBufferRegion")) return;
    try {
    if (!pDstBuffer || !pSrcBuffer)
      return;
    if (!PreBlit())
      return;

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd_cp.src = static_cast<MTLD3D12Resource *>(pSrcBuffer)->buffer->current()->buffer();
    cmd_cp.dst = static_cast<MTLD3D12Resource *>(pDstBuffer)->buffer->current()->buffer();
    cmd_cp.src_offset = SrcOffset;
    cmd_cp.dst_offset = DstOffset;
    cmd_cp.copy_length = ByteCount;
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "CopyBufferRegion", __LINE__);
      TraceRecording("CopyBufferRegion", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  CopyTextureRegion(
      const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc,
      const D3D12_BOX *pSrcBox
  ) {
    if (!RequireOpenRecording("CopyTextureRegion")) return;
    try {
    if (!pDst || !pSrc)
      return;
    if (!PreBlit())
      return;

    auto src_desc = pSrc->pResource->GetDesc();
    auto dst_desc = pDst->pResource->GetDesc();
    uint32_t src_level = 0, src_slice = 0, src_planar = 0;
    uint32_t dst_level = 0, dst_slice = 0, dst_planar = 0;
    D3D12_BOX src_box, full_src_box;

    if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      full_src_box = {
          0, 0, 0,
          pSrc->PlacedFootprint.Footprint.Width,
          pSrc->PlacedFootprint.Footprint.Height,
          pSrc->PlacedFootprint.Footprint.Depth
      };
    } else {
      DecomposeSubresource(src_desc, pSrc->SubresourceIndex, &src_level, &src_slice, &src_planar);
      full_src_box = GetResourceExtent(src_desc, src_level);
    }
    src_box = pSrcBox ? *pSrcBox : full_src_box;

    // discard invalid & empty box
    if (src_box.left >= src_box.right || src_box.front >= src_box.back || src_box.top >= src_box.bottom)
      return;

    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      auto &dst = static_cast<MTLD3D12Resource *>(pDst->pResource)->texture;
      if (!dst)
        return;

      DecomposeSubresource(dst_desc, pDst->SubresourceIndex, &dst_level, &dst_slice, &dst_planar);

      MTL_DXGI_FORMAT_DESC dst_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), dst_desc.Format, dst_format))) {
        WARN("CopyTextureRegion: unsupported format ", dst_desc.Format);
        return;
      }
      auto dst_planar_count = dst_format.PlanarCount;

      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->buffer;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), pSrc->PlacedFootprint.Footprint.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", pSrc->PlacedFootprint.Footprint.Format);
          return;
        }

        auto block_width = src_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
        auto src_depth_pitch = dst->textureType() == WMTTextureType3D
                                   ? (pSrc->PlacedFootprint.Footprint.Height / block_width) * pSrc->PlacedFootprint.Footprint.RowPitch
                                   : 0;

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
        cmd_cp.src = src->current()->buffer();
        cmd_cp.src_offset = pSrc->PlacedFootprint.Offset + (src_box.left / block_width) * src_format.BytesPerTexel +
                            (src_box.top / block_width) * pSrc->PlacedFootprint.Footprint.RowPitch + src_box.front * src_depth_pitch;
        cmd_cp.bytes_per_row = pSrc->PlacedFootprint.Footprint.RowPitch;
        cmd_cp.bytes_per_image = src_depth_pitch;
        cmd_cp.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->texture();
        cmd_cp.level = dst_level;
        cmd_cp.slice = dst_slice;
        cmd_cp.options = (dst_planar_count > 1)
                             ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
        cmd_cp.origin = {DstX, DstY, DstZ};
      } else {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", src_desc.Format);
          return;
        }
        auto src_planar_count = src_format.PlanarCount;

        // copy between depth-stencil texture is tricky
        if (dst_planar_count > 1 || src_planar_count > 1) {
          // in this path, one/both of dst/src would be depth-stencil texture

          if (dst_planar_count > 1 && src_planar_count > 1 && dst_planar != src_planar) {
            WARN("CopyTextureRegion: unmatched planar"); // just in case
            return;
          }

          auto texel_size = (dst_planar == 1 || src_planar == 1) ? 1 : 4;
          auto width = src_box.right - src_box.left;
          auto height = src_box.bottom - src_box.top;
          auto bytes_per_row = align(width * texel_size, 256);
          auto bytes_per_image = bytes_per_row * height;

          auto [temp_buffer, temp_buffer_offset] = allocator_->AllocateTempBuffer(bytes_per_image, 256);

          auto &cmd_to_tmp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          cmd_to_tmp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          cmd_to_tmp.src = src->current()->texture();
          cmd_to_tmp.level = src_level;
          cmd_to_tmp.slice = src_slice;
          cmd_to_tmp.origin = {src_box.left, src_box.top, src_box.front};
          cmd_to_tmp.size = {width, height, src_box.back - src_box.front};
          cmd_to_tmp.dst = temp_buffer;
          cmd_to_tmp.offset = temp_buffer_offset;
          cmd_to_tmp.bytes_per_image = 0; // DSV cannot be 3D
          cmd_to_tmp.bytes_per_row = bytes_per_row;
          cmd_to_tmp.options = (src_planar_count > 1) ? (src_planar ? WMTBlitOptionStencilFromDepthStencil
                                                                    : WMTBlitOptionDepthFromDepthStencil)
                                                      : WMTBlitOptionNone;

          auto &cmd_to_tex = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
          cmd_to_tex.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          cmd_to_tex.src = temp_buffer;
          cmd_to_tex.src_offset = temp_buffer_offset;
          cmd_to_tex.bytes_per_image = 0; // DSV cannot be 3D
          cmd_to_tex.bytes_per_row = bytes_per_row;
          cmd_to_tex.dst = dst->current()->texture();
          cmd_to_tex.level = dst_level;
          cmd_to_tex.slice = dst_slice;
          cmd_to_tex.origin = {DstX, DstY, DstZ};
          cmd_to_tex.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
          cmd_to_tex.options = (dst_planar_count > 1) ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil
                                                                    : WMTBlitOptionDepthFromDepthStencil)
                                                      : WMTBlitOptionNone;
          return;
        }

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
        cmd_cp.type = WMTBlitCommandCopyFromTextureToTexture;
        cmd_cp.src = src->current()->texture();
        cmd_cp.src_level = src_level;
        cmd_cp.src_slice = src_slice;
        cmd_cp.src_origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.src_size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->texture();
        cmd_cp.dst_level = dst_level;
        cmd_cp.dst_slice = dst_slice;
        cmd_cp.dst_origin = {DstX, DstY, DstZ};
      }
    } else if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      auto &dst = static_cast<MTLD3D12Resource *>(pDst->pResource)->buffer;
      if (!dst)
        return;

      MTL_DXGI_FORMAT_DESC dst_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), pDst->PlacedFootprint.Footprint.Format, dst_format))) {
        WARN("CopyTextureRegion: unsupported format ", pDst->PlacedFootprint.Footprint.Format);
        return;
      }

      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", src_desc.Format);
          return;
        }
        auto src_planar_count = src_format.PlanarCount;

        auto block_width = dst_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
        auto dst_depth_pitch = src->textureType() == WMTTextureType3D
                                   ? (pDst->PlacedFootprint.Footprint.Height / block_width) * pDst->PlacedFootprint.Footprint.RowPitch
                                   : 0;

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
        cmd_cp.src = src->current()->texture();
        cmd_cp.level = src_level;
        cmd_cp.slice = src_slice;
        cmd_cp.origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->buffer();
        cmd_cp.offset = pDst->PlacedFootprint.Offset + (DstX / block_width) * dst_format.BytesPerTexel +
                        (DstY / block_width) * pDst->PlacedFootprint.Footprint.RowPitch + DstZ * dst_depth_pitch;
        cmd_cp.bytes_per_row = pDst->PlacedFootprint.Footprint.RowPitch;
        cmd_cp.bytes_per_image = dst_depth_pitch;
        cmd_cp.options = (src_planar_count > 1)
                             ? (src_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
      } else {
        // so it is buffer to buffer copy?
        IMPLEMENT_ME
      }
    }
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "CopyTextureRegion", __LINE__);
      TraceRecording("CopyTextureRegion", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource) {
    if (!RequireOpenRecording("CopyResource")) return;
    try {
    auto *pDst = static_cast<MTLD3D12Resource *>(pDstResource);
    auto *pSrc = static_cast<MTLD3D12Resource *>(pSrcResource);
    if (!pDst || !pSrc || (pDst == pSrc))
      return;

    auto DstDesc = pDst->GetDesc();
    auto SrcDesc = pSrc->GetDesc();
    if (DstDesc.Dimension != SrcDesc.Dimension)
      return;

    if (!PreBlit())
      return;

    if (DstDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
      cmd_cp.copy_length = SrcDesc.Width;
      cmd_cp.src = pSrc->buffer->current()->buffer();
      cmd_cp.src_offset = 0;
      cmd_cp.dst = pDst->buffer->current()->buffer();
      cmd_cp.dst_offset = 0;
      return;
    }

    // TODO: handle reinterpret copy
    if (pDst->texture->pixelFormat() != pSrc->texture->pixelFormat()) {
      static std::atomic<uint32_t> trace_reinterpret{0};
      TraceFrame(trace_reinterpret, "copy.resource.skip", this, "reason=reinterpret_format src=%llx dst=%llx src_format=%u dst_format=%u",
                 (unsigned long long)pSrc->texture->current()->texture().handle,
                 (unsigned long long)pDst->texture->current()->texture().handle,
                 unsigned(pSrc->texture->pixelFormat()), unsigned(pDst->texture->pixelFormat()));
      WARN("CopyResource: TODO: reinterpret copy");
      return;
    }

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_texture>();
    cmd_cp.type = WMTBlitCommandCopyTexture;
    cmd_cp.src = pSrc->texture->current()->texture();
    cmd_cp.dst = pDst->texture->current()->texture();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "CopyResource", __LINE__);
      TraceRecording("CopyResource", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE CopyTiles(
      ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset,
      D3D12_TILE_COPY_FLAGS flags
  ) {
    if (!RequireOpenRecording("CopyTiles")) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "CopyTiles", __LINE__);
      TraceRecording("CopyTiles", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE ResolveSubresource(
      ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource,
      DXGI_FORMAT Format
  ) {
    if (!RequireOpenRecording("ResolveSubresource")) return;
    try {
    auto *pDst = static_cast<MTLD3D12Resource *>(pDstResource);
    auto *pSrc = static_cast<MTLD3D12Resource *>(pSrcResource);

    if (!pDst->texture || !pSrc->texture)
      return;

    auto DstMips = pDst->texture->miplevelCount();
    auto DstLevel = DstSubresource % DstMips;
    auto DstSlice = DstSubresource / DstMips;

    allocator_->InvalidateCurrentPass();
    auto resolve = allocator_->AllocatePass<ResolveEncoderData>();
    resolve->type = EncoderType::Resolve;

    MTL_DXGI_FORMAT_DESC format_desc;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format_desc))) {
      ERR("ResolveSubresource: invalid format ", Format);
      return;
    }
    {
      auto format = format_desc.PixelFormat;
      TextureViewDescriptor src_desc;
      auto &src = pSrc->texture;
      auto &dst = pDst->texture;
      src_desc.format = format;
      src_desc.type = src->textureType();
      src_desc.arraySize = 1;
      src_desc.firstArraySlice = SrcSubresource; // src must be a MS(Array) texture which has exactly 1 mipmap level
      src_desc.miplevelCount = 1;
      src_desc.firstMiplevel = 0;

      TextureViewDescriptor dst_desc;
      dst_desc.format = format;
      dst_desc.type = WMTTextureType2D;
      dst_desc.arraySize = 1;
      dst_desc.firstArraySlice = DstSlice;
      dst_desc.miplevelCount = 1;
      dst_desc.firstMiplevel = DstLevel;

      auto src_view = src->createView(src_desc);
      auto dst_view = dst->createView(dst_desc);

      resolve->src = src->view(src_view);
      resolve->dst = dst->view(dst_view);
    }
    allocator_->InvalidateCurrentPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ResolveSubresource", __LINE__);
      TraceRecording("ResolveSubresource", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY Topology) {
    if (!RequireOpenRecording("IASetPrimitiveTopology")) return;
    try {
    topology_ = Topology;
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "IASetPrimitiveTopology", __LINE__);
      TraceRecording("IASetPrimitiveTopology", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports) {
    if (!RequireOpenRecording("RSSetViewports")) return;
    try {
    if (NumViewports > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    num_viewports = NumViewports;
    for (auto i = 0u; i < NumViewports; i++) {
      viewports[i] = pViewports[i];
    }
    dirty_state_.set(DirtyState::Viewport);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "RSSetViewports", __LINE__);
      TraceRecording("RSSetViewports", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  RSSetScissorRects(UINT NumRects, const D3D12_RECT *rects) {
    if (!RequireOpenRecording("RSSetScissorRects")) return;
    try {
    if (NumRects > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    num_scissors = NumRects;
    for (auto i = 0u; i < NumRects; i++) {
      scissors[i] = rects[i];
    }
    dirty_state_.set(DirtyState::ScissorRect);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "RSSetScissorRects", __LINE__);
      TraceRecording("RSSetScissorRects", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  OMSetBlendFactor(const FLOAT BlendFactors[4]) {
    if (!RequireOpenRecording("OMSetBlendFactor")) return;
    try {
    if (BlendFactors) {
      memcpy(blend_factor_, BlendFactors, std::size(blend_factor_) * sizeof(blend_factor_[0]));
    } else {
      blend_factor_[0] = 1.0f;
      blend_factor_[1] = 1.0f;
      blend_factor_[2] = 1.0f;
      blend_factor_[3] = 1.0f;
    }
    dirty_state_.set(DirtyState::BlendFactor);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "OMSetBlendFactor", __LINE__);
      TraceRecording("OMSetBlendFactor", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  OMSetStencilRef(UINT StencilRef) {
    if (!RequireOpenRecording("OMSetStencilRef")) return;
    try {
    if (stencil_ref_ == (UINT8)StencilRef)
      return;
    stencil_ref_ = (UINT8)StencilRef;
    dirty_state_.set(DirtyState::StencilRef);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "OMSetStencilRef", __LINE__);
      TraceRecording("OMSetStencilRef", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  SetPipelineState(ID3D12PipelineState *pPSO) {
    if (!RequireOpenRecording("SetPipelineState")) return;
    try {
    if (!pPSO) {
      pso_graphics_ = nullptr;
      pso_compute_ = nullptr;
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
      TraceRecording("SetPipelineState", "accepted", "cleared", "requested_pso=%p", static_cast<const void *>(pPSO));
      return;
    }

    auto pso = static_cast<MTLD3D12PipelineState *>(pPSO);
    if (pso->IsComputePipelineState) {
      auto compute_pso = static_cast<MTLD3D12ComputePipelineState *>(pPSO);
      if (pso_compute_.ptr() == compute_pso) {
        TraceRecording("SetPipelineState", "accepted", "unchanged_compute", "requested_pso=%p", static_cast<const void *>(pPSO));
        return;
      }
      pso_compute_ = compute_pso;
      pso_graphics_ = nullptr;
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
      TraceRecording("SetPipelineState", "accepted", "bound_compute", "requested_pso=%p", static_cast<const void *>(pPSO));
      return;
    }

    auto graphics_pso = static_cast<MTLD3D12GraphicsPipelineState *>(pPSO);
    if (pso_graphics_.ptr() == graphics_pso) {
      TraceRecording("SetPipelineState", "accepted", "unchanged_graphics", "requested_pso=%p", static_cast<const void *>(pPSO));
      return;
    }
    pso_graphics_ = graphics_pso;
    pso_compute_ = nullptr;
    dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState, DirtyState::VertexBuffer);
    dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
    TraceRecording("SetPipelineState", "accepted", "bound_graphics", "requested_pso=%p", static_cast<const void *>(pPSO));
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetPipelineState", __LINE__);
      TraceRecording("SetPipelineState", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE ResourceBarrier(UINT Count, const D3D12_RESOURCE_BARRIER *barriers) {
    if (!RequireOpenRecording("ResourceBarrier")) return;
    try {
    if (!Count || !barriers || encoder_count != std::numeric_limits<size_t>::max())
      return;
    // Join deferred OUT writes only at an explicit dependency, not every pass.
    // Record this even without a local OUT: it may be pending from a prior list.
    allocator_->InvalidateCurrentPass();
    PreBlit();
    auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
    pass->join_immediate_out = true;
    pass->barrier_only = true;
    allocator_->InvalidateCurrentPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ResourceBarrier", __LINE__);
      TraceRecording("ResourceBarrier", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *CommandList) {
    if (!RequireOpenRecording("ExecuteBundle")) return;
    try { IMPLEMENT_ME   } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ExecuteBundle", __LINE__);
      TraceRecording("ExecuteBundle", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };


  HRESULT RecordBoundDescriptor(bool compute, const ray_binding::Binding &binding, const ray_library::Resource &required,
                                D3D12RecordedDescriptorBinding &out) override {
    out = {};
    const auto *root = compute ? rootsig_compute_.ptr() : rootsig_graphics_.ptr();
    const auto &arguments = compute ? rootarg_compute_staging_ : rootarg_graphics_staging_;
    if (!root || root->RayBindingStatus != ray_binding::Status::Ready || binding.local)
      return DXGI_ERROR_UNSUPPORTED;
    const ray_binding::Span *span = nullptr;
    uint64_t selected_offset = 0;
    auto status = ray_binding::SelectResource(root->RayBindings, binding, required, 0, span, selected_offset);
    if (binding.source != ray_binding::Source::Table) return E_INVALIDARG;
    if (status != ray_binding::Status::Ready || binding.descriptor_offset > UINT32_MAX) return E_INVALIDARG;
    D3D12RecordedDescriptorBinding next;
    next.recorded_device = device_;
    status = ray_binding::DecodePolicy(span->type, span->flags, true, next.policy);
    if (status != ray_binding::Status::Ready)
      return status == ray_binding::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
    const UINT type = span->type == 3 ? 1 : 0;
    root_argument::TableLocation location;
    if (!root_argument::ReadTable(root->ArgumentLayout, arguments, binding.parameter,
                                  bound_heap_ranges_[type], UINT(binding.descriptor_offset), 1, location))
      return E_INVALIDARG;
    D3D12DescriptorProvider provider{bound_heaps_[type], location.index, span->type, required};
    const HRESULT hr = CaptureResult(next.replay.Record(std::move(provider), next.policy.descriptors_volatile));
    if (FAILED(hr)) return hr;
    out = std::move(next);
    return S_OK;
  }

  HRESULT RecordRayArgument(const ray_binding::Binding &binding, const ray_library::Resource &required, UINT element,
      MTLD3D12RootSignature *local, const void *arguments, size_t argument_bytes,
      uint64_t buffer_bytes, D3D12RecordedDescriptorBinding &out) override {
    out = {};
    const auto *root = binding.local ? local : rootsig_compute_.ptr();
    if (!root || root->IsLocal != binding.local || root->RayBindingStatus != ray_binding::Status::Ready)
      return DXGI_ERROR_UNSUPPORTED;
    if (binding.local) {
      Com<ID3D12Device> owner;
      if (!root->LocalRootLayout || FAILED(local->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device_)) return E_INVALIDARG;
    }
    const ray_binding::Span *span = nullptr;
    uint64_t descriptor_offset = 0;
    const auto status = ray_binding::SelectResource(root->RayBindings, binding, required, element, span, descriptor_offset);
    if (status != ray_binding::Status::Ready)
      return status == ray_binding::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
    D3D12RecordedDescriptorBinding next;
    next.recorded_device = device_;
    const bool table = binding.source == ray_binding::Source::Table;
    const auto policy = ray_binding::DecodePolicy(span->type, span->flags, table, next.policy);
    if (policy != ray_binding::Status::Ready)
      return policy == ray_binding::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
    const auto kind = table ? root_argument::Table :
        span->type == 2 ? root_argument::CBV :
        span->type == 0 ? root_argument::SRV : root_argument::UAV;
    uint64_t value = 0;
    if (binding.local) {
      const auto &layout = *root->LocalRootLayout;
      // The caller supplies the captured local-argument region, excluding the
      // shader identifier. It must not point into a mutable shader-table upload.
      if (binding.parameter >= layout.parameters.size() || layout.parameters[binding.parameter].kind != kind ||
          !local_root::Read(layout, binding.parameter, arguments, argument_bytes, 0, value)) return E_INVALIDARG;
    } else if (!rootarg_compute_staging_.ReadValue(root->ArgumentLayout, binding.parameter, kind, value)) return E_INVALIDARG;
    if (table) {
      const UINT heap_type = span->type == 3 ? 1 : 0;
      root_argument::TableLocation location;
      if (descriptor_offset > UINT32_MAX ||
          !root_argument::Locate(bound_heap_ranges_[heap_type], value, UINT(descriptor_offset), 1, location)) return E_INVALIDARG;
      D3D12DescriptorProvider provider{bound_heaps_[heap_type], location.index, span->type, required};
      const HRESULT hr = CaptureResult(next.replay.Record(std::move(provider), next.policy.descriptors_volatile));
      if (FAILED(hr)) return hr;
    } else {
      const HRESULT valid = CaptureResult(resource_shape::Root(required, value, buffer_bytes));
      if (FAILED(valid)) return valid;
      uint64_t offset = 0;
      auto owner = device_->LookupCaptureByVA(value, buffer_bytes, offset);
      if (!owner) return DXGI_ERROR_UNSUPPORTED;
      next.root_buffer = true;
      auto &capture = next.fixed_buffer;
      capture.type = span->type == 2 ? ShaderVisibleDescriptorType::ConstantBuffer :
          span->type == 0 ? ShaderVisibleDescriptorType::SRVBuffer : ShaderVisibleDescriptorType::UAVBuffer;
      // Root descriptors have no view stride or bounds. The byte requirement
      // comes from executable shader ABI analysis, never from base RDAT alone.
      capture.shape = {required.kind, 0, 0, 0};
      capture.words = {value, buffer_bytes, 0, 0};
      capture.resource = std::move(owner);
      capture.buffer_offset = offset;
      capture.byte_length = buffer_bytes;
    }
    out = std::move(next);
    return S_OK;
  }

  HRESULT RecordCompilerRayArgument(const ray_binding::Binding &binding, const compiler_resource::Footprint &footprint,
      UINT element, MTLD3D12RootSignature *local, const void *arguments, size_t argument_bytes,
      uint64_t buffer_bytes, D3D12RecordedDescriptorBinding &out) override {
    out = {};
    if (encoder_count != std::numeric_limits<size_t>::max()) return E_INVALIDARG;
    const HRESULT valid = CaptureResult(compiler_resource::Validate(footprint));
    if (FAILED(valid)) return valid;
    // Declared DXIL size is a lower bound, not proof of the lowered backend's
    // physical loads. Direct roots still require a caller-proven byte range.
    try {
      D3D12RecordedDescriptorBinding next;
      const HRESULT hr = RecordRayArgument(binding, footprint.resource, element, local,
          arguments, argument_bytes, buffer_bytes, next);
      if (FAILED(hr)) return hr;
      next.compiler_footprint = footprint;
      // Static arguments can be checked now. Volatile descriptors may not yet
      // be initialized; check their exact compiler requirements at submission.
      if (next.root_buffer || !next.policy.descriptors_volatile) {
        D3D12NativeDescriptorCapture checked;
        const HRESULT match = next.ResolveForSubmission(checked);
        if (FAILED(match)) return match;
      }
      out = std::move(next);
      return S_OK;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

  // Internal transport for a compiler-supplied ABI and current compute PSO.
  // It does not admit a StateObject or implement public DispatchRays.
  HRESULT RecordArgumentDispatch(const argument_upload::Plan &arguments,
      const std::vector<D3D12RecordedDescriptorBinding> &bindings, UINT x, UINT y, UINT z) override {
    if (!allocator_ || encoder_count != std::numeric_limits<size_t>::max() ||
        !pso_compute_ || !pso_compute_->pso) return E_INVALIDARG;
    if (x > 65535 || y > 65535 || z > 65535) return E_INVALIDARG;
    if (allocator_->type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && allocator_->type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return DXGI_ERROR_UNSUPPORTED;
    if (!x || !y || !z) return S_OK;
    try {
      const HRESULT valid = CaptureResult(argument_upload::Validate(arguments, bindings.size()));
      if (FAILED(valid)) return valid;
      for (const auto &binding : bindings)
        if (binding.recorded_device != static_cast<ID3D12Device *>(device_)) return E_INVALIDARG;
      auto record = std::make_shared<D3D12RecordedComputeDispatch>();
      record->device = device_;
      record->arguments = arguments;
      record->bindings = bindings;
      record->pipeline = pso_compute_->pso;
      record->threads = pso_compute_->threadgroup_size;
      record->groups = {x, y, z};
      allocator_->InvalidateCurrentPass();
      auto pass = allocator_->AllocatePass<ArgumentComputeEncoderData>();
      pass->type = EncoderType::ArgumentCompute;
      pass->dispatch = std::move(record);
      allocator_->InvalidateCurrentPass();
      dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature, DirtyState::ComputePipelineState);
      return S_OK;
    } catch (const std::bad_alloc &) {
      SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);
      return E_OUTOFMEMORY;
    }
  }

  // Function identity is retained from the same PSO, not accepted from an unrelated caller.
  HRESULT RecordMetalArgumentDispatch(const metal_argument::Plan &,
      const std::vector<D3D12RecordedDescriptorBinding> &, UINT, UINT, UINT) override {
    // A current ordinary PSO has no complete entry contract; use a reflected compiled program.
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT RecordAccelerationStructureBuild(const D3D12AccelerationBuildInput &input,
      D3D12RecordedDescriptorBinding &out) override {
    out={};
    if (!allocator_ || encoder_count!=std::numeric_limits<size_t>::max()) return E_INVALIDARG;
    if (allocator_->type_!=D3D12_COMMAND_LIST_TYPE_DIRECT && allocator_->type_!=D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return DXGI_ERROR_UNSUPPORTED;
    try {
      std::shared_ptr<const D3D12AccelerationBuildPlan> plan;
      const HRESULT hr=PrepareAccelerationBuildPlan(device_,input,plan);
      if (FAILED(hr)) return hr;
      allocator_->InvalidateCurrentPass();
      auto pass=allocator_->AllocatePass<AccelerationBuildEncoderData>();
      pass->type=EncoderType::AccelerationBuild;pass->build=plan;
      allocator_->InvalidateCurrentPass();
      out.recorded_device=static_cast<ID3D12Device *>(device_);
      out.acceleration_device=static_cast<ID3D12Device *>(device_);
      out.acceleration_producer=std::move(plan);
      return S_OK;
    } catch (const std::bad_alloc &) { SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);return E_OUTOFMEMORY; }
  }

  HRESULT RecordCompiledComputeDispatch(const std::shared_ptr<const D3D12CompiledComputeProgram> &program,
      const metal_argument::Plan &arguments,
      const std::vector<D3D12RecordedDescriptorBinding> &bindings, UINT x, UINT y, UINT z) override {
    if (!allocator_ || encoder_count != std::numeric_limits<size_t>::max() ||
        !program || program->Owner() != static_cast<ID3D12Device *>(device_) ||
        !program->Pipeline() || !program->Function()) return E_INVALIDARG;
    if (x > 65535 || y > 65535 || z > 65535) return E_INVALIDARG;
    if (allocator_->type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && allocator_->type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return DXGI_ERROR_UNSUPPORTED;
    if (!x || !y || !z) return S_OK;
    try {
      const HRESULT valid = CaptureResult(metal_argument::Match(arguments, bindings.size(), program->Layout()));
      if (FAILED(valid)) return valid;
      for (const auto &binding : bindings)
        if (binding.recorded_device != static_cast<ID3D12Device *>(device_)) return E_INVALIDARG;
      auto record = std::make_shared<D3D12RecordedComputeDispatch>();
      record->device = device_;
      record->metal_arguments = arguments;
      record->function = program->Function();
      record->entry_layout = program->Layout();
      record->bindings = bindings;
      record->pipeline = program->Pipeline();
      record->threads = program->Threads();
      record->groups = {x, y, z};
      allocator_->InvalidateCurrentPass();
      auto pass = allocator_->AllocatePass<ArgumentComputeEncoderData>();
      pass->type = EncoderType::ArgumentCompute;
      pass->dispatch = std::move(record);
      allocator_->InvalidateCurrentPass();
      dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature, DirtyState::ComputePipelineState);
      return S_OK;
    } catch (const std::bad_alloc &) {
      SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);
      return E_OUTOFMEMORY;
    }
  }

  HRESULT CaptureBoundDescriptor(bool compute, UINT parameter, UINT offset, D3D12DescriptorCapture &out) override {
    out = {};
    const auto *root = compute ? rootsig_compute_.ptr() : rootsig_graphics_.ptr();
    const auto &arguments = compute ? rootarg_compute_staging_ : rootarg_graphics_staging_;
    if (!root || parameter >= root->ArgumentLayout.count) return E_INVALIDARG;
    const auto &p = root->ArgumentLayout.parameters[parameter];
    if (p.kind != root_argument::Table || p.heap_type > 1 || !bound_heaps_[p.heap_type]) return E_INVALIDARG;
    root_argument::TableLocation location;
    if (!root_argument::ReadTable(root->ArgumentLayout, arguments, parameter,
                                  bound_heap_ranges_[p.heap_type], offset, 1, location)) return E_INVALIDARG;
    if (p.heap_type == 0) {
      Com<MTLD3D12DescriptorHeap> heap;
      if (FAILED(bound_heaps_[0]->QueryInterface(kD3D12ResourceHeapCaptureUUID, reinterpret_cast<void **>(&heap))))
        return DXGI_ERROR_UNSUPPORTED;
      return heap->CaptureDescriptor(location.index, out);
    }
    Com<MTLD3D12SamplerDescriptorHeap> heap;
    if (FAILED(bound_heaps_[1]->QueryInterface(kD3D12SamplerHeapCaptureUUID, reinterpret_cast<void **>(&heap))))
      return DXGI_ERROR_UNSUPPORTED;
    return heap->CaptureDescriptor(location.index, out);
  }

  HRESULT CaptureRootBuffer(bool compute, UINT parameter, uint64_t bytes, D3D12DescriptorCapture &out) override {
    out = {};
    const auto *root = compute ? rootsig_compute_.ptr() : rootsig_graphics_.ptr();
    const auto &arguments = compute ? rootarg_compute_staging_ : rootarg_graphics_staging_;
    if (!root || parameter >= root->ArgumentLayout.count || !bytes) return E_INVALIDARG;
    const auto kind = root->ArgumentLayout.parameters[parameter].kind;
    if (kind != root_argument::CBV && kind != root_argument::SRV && kind != root_argument::UAV)
      return DXGI_ERROR_UNSUPPORTED;
    uint64_t address = 0, offset = 0;
    if (!arguments.ReadValue(root->ArgumentLayout, parameter, kind, address)) return E_INVALIDARG;
    if (kind == root_argument::CBV && ((address & 255) || bytes > 65536)) return E_INVALIDARG;
    auto resource = device_->LookupCaptureByVA(address, bytes, offset);
    if (!resource) return DXGI_ERROR_UNSUPPORTED;
    D3D12DescriptorCapture next;
    next.type = kind == root_argument::CBV ? ShaderVisibleDescriptorType::ConstantBuffer :
                kind == root_argument::SRV ? ShaderVisibleDescriptorType::SRVBuffer : ShaderVisibleDescriptorType::UAVBuffer;
    next.resource = std::move(resource);
    next.buffer_offset = offset;
    next.byte_length = bytes;
    next.words = {address, bytes, 0, 0};
    out = std::move(next);
    return S_OK;
  }

  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT HeapCount, ID3D12DescriptorHeap *const *Heaps) {
    if (!RequireOpenRecording("SetDescriptorHeaps")) return;
    try {
    if (HeapCount > 2 || (HeapCount && !Heaps)) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
    std::array<Com<ID3D12DescriptorHeap>, 2> next;
    std::array<root_argument::HeapRange, 2> ranges{};
    for (UINT i = 0; i < HeapCount; ++i) {
      if (!Heaps[i]) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
      const auto desc = Heaps[i]->GetDesc();
      if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
      }
      const UINT type = UINT(desc.Type);
      Com<ID3D12Device> owner;
      if (next[type] || !(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ||
          FAILED(Heaps[i]->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device_)) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
      ranges[type] = {Heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr, desc.NumDescriptors,
                      device_->GetDescriptorHandleIncrementSize(desc.Type), type};
      if (!ranges[type].Valid()) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
      next[type] = Heaps[i];
    }
    if (next[0].ptr() == bound_heaps_[0].ptr() && next[1].ptr() == bound_heaps_[1].ptr()) return;
    bound_heaps_ = std::move(next);
    bound_heap_ranges_ = ranges;
    if (rootsig_compute_) rootarg_compute_staging_.InvalidateTables(rootsig_compute_->ArgumentLayout);
    if (rootsig_graphics_) rootarg_graphics_staging_.InvalidateTables(rootsig_graphics_->ArgumentLayout);
    dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetDescriptorHeaps", __LINE__);
      TraceRecording("SetDescriptorHeaps", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *root) {
    if (!RequireOpenRecording("SetComputeRootSignature")) return;
    try {
    if (rootsig_compute_.ptr() == root) return;
    Com<MTLD3D12RootSignature> impl;
    if (root) {
      Com<ID3D12Device> owner;
      if (FAILED(root->QueryInterface(kD3D12RootSignatureImplementationUUID, reinterpret_cast<void **>(&impl))) ||
          !impl || impl->IsLocal || impl->UploadQwords > 64 ||
          FAILED(impl->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device_)) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
    }
    rootsig_compute_ = impl.ptr();
    rootarg_compute_staging_ = {};
    dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootSignature", __LINE__);
      TraceRecording("SetComputeRootSignature", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (!RequireOpenRecording("SetComputeRootDescriptorTable")) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteValue(rootsig_compute_->ArgumentLayout, Index, root_argument::Table, BaseDescriptor.ptr)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootDescriptorTable", __LINE__);
      TraceRecording("SetComputeRootDescriptorTable", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (!RequireOpenRecording("SetComputeRootConstantBufferView")) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteValue(rootsig_compute_->ArgumentLayout, Index, root_argument::CBV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootConstantBufferView", __LINE__);
      TraceRecording("SetComputeRootConstantBufferView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (!RequireOpenRecording("SetComputeRootShaderResourceView")) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteValue(rootsig_compute_->ArgumentLayout, Index, root_argument::SRV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootShaderResourceView", __LINE__);
      TraceRecording("SetComputeRootShaderResourceView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (!RequireOpenRecording("SetComputeRootUnorderedAccessView")) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteValue(rootsig_compute_->ArgumentLayout, Index, root_argument::UAV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootUnorderedAccessView", __LINE__);
      TraceRecording("SetComputeRootUnorderedAccessView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) {
    if (!RequireOpenRecording("SetComputeRoot32BitConstant")) return;
    try {
    SetComputeRoot32BitConstants(Index, 1, &Data, DstOffset);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRoot32BitConstant", __LINE__);
      TraceRecording("SetComputeRoot32BitConstant", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT Index, UINT Count, const void *Data, UINT DstOffset) {
    if (!RequireOpenRecording("SetComputeRoot32BitConstants")) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteConstants(rootsig_compute_->ArgumentLayout, Index, Count, Data, DstOffset)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    if (Count) dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRoot32BitConstants", __LINE__);
      TraceRecording("SetComputeRoot32BitConstants", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }
  void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *root) {
    if (!RequireOpenRecording("SetGraphicsRootSignature")) return;
    try {
    if (rootsig_graphics_.ptr() == root) return;
    Com<MTLD3D12RootSignature> impl;
    if (root) {
      Com<ID3D12Device> owner;
      if (FAILED(root->QueryInterface(kD3D12RootSignatureImplementationUUID, reinterpret_cast<void **>(&impl))) ||
          !impl || impl->IsLocal || impl->UploadQwords > 64 ||
          FAILED(impl->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device_)) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
    }
    rootsig_graphics_ = impl.ptr();
    rootarg_graphics_staging_ = {};
    dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootSignature", __LINE__);
      TraceRecording("SetGraphicsRootSignature", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (!RequireOpenRecording("SetGraphicsRootDescriptorTable")) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteValue(rootsig_graphics_->ArgumentLayout, Index, root_argument::Table, BaseDescriptor.ptr)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootDescriptorTable", __LINE__);
      TraceRecording("SetGraphicsRootDescriptorTable", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (!RequireOpenRecording("SetGraphicsRootConstantBufferView")) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteValue(rootsig_graphics_->ArgumentLayout, Index, root_argument::CBV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootConstantBufferView", __LINE__);
      TraceRecording("SetGraphicsRootConstantBufferView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (!RequireOpenRecording("SetGraphicsRootShaderResourceView")) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteValue(rootsig_graphics_->ArgumentLayout, Index, root_argument::SRV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootShaderResourceView", __LINE__);
      TraceRecording("SetGraphicsRootShaderResourceView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (!RequireOpenRecording("SetGraphicsRootUnorderedAccessView")) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteValue(rootsig_graphics_->ArgumentLayout, Index, root_argument::UAV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootUnorderedAccessView", __LINE__);
      TraceRecording("SetGraphicsRootUnorderedAccessView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) {
    if (!RequireOpenRecording("SetGraphicsRoot32BitConstant")) return;
    try {
    SetGraphicsRoot32BitConstants(Index, 1, &Data, DstOffset);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRoot32BitConstant", __LINE__);
      TraceRecording("SetGraphicsRoot32BitConstant", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT Index, UINT Count, const void *Data, UINT DstOffset) {
    if (!RequireOpenRecording("SetGraphicsRoot32BitConstants")) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteConstants(rootsig_graphics_->ArgumentLayout, Index, Count, Data, DstOffset)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    if (Count) dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRoot32BitConstants", __LINE__);
      TraceRecording("SetGraphicsRoot32BitConstants", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) {
    if (!RequireOpenRecording("IASetIndexBuffer")) return;
    try {
    index_buffer_address = 0;
    index_buffer = {};
    index_allocation_ = nullptr;
    index_type = {};
    index_offset = 0;
    index_buffer_size = 0;
    if (!pView || (pView->Format != DXGI_FORMAT_R16_UINT && pView->Format != DXGI_FORMAT_R32_UINT))
      return;
    const uint32_t index_size = pView->Format == DXGI_FORMAT_R32_UINT ? 4 : 2;
    if (pView->BufferLocation % index_size)
      return;
    auto index_buffer_allocation = device_->LookupBufferByVA(pView->BufferLocation, pView->SizeInBytes, &index_offset);
    if (index_buffer_allocation) {
      index_buffer_address = pView->BufferLocation;
      index_buffer = index_buffer_allocation->buffer();
      index_allocation_ = std::move(index_buffer_allocation);
      index_type = pView->Format == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
      index_buffer_size = pView->SizeInBytes;
    } else {
      index_buffer_address = 0;
      index_buffer = {};
      index_type = {};
      index_offset = {};
    }
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "IASetIndexBuffer", __LINE__);
      TraceRecording("IASetIndexBuffer", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  IASetVertexBuffers(UINT StartSlot, UINT Count, const D3D12_VERTEX_BUFFER_VIEW *Views) {
    if (!RequireOpenRecording("IASetVertexBuffers")) return;
    try {
    if (!Views)
      return;
    
    for (unsigned Slot = StartSlot; Slot < StartSlot + Count; Slot++) {
      vertex_buffers_[Slot] = Views[Slot - StartSlot];
    }
    dirty_state_.set(DirtyState::VertexBuffer);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "IASetVertexBuffers", __LINE__);
      TraceRecording("IASetVertexBuffers", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT Count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *Views) {
    if (!RequireOpenRecording("SOSetTargets")) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SOSetTargets", __LINE__);
      TraceRecording("SOSetTargets", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  OMSetRenderTargets(
      UINT NumRTV, const D3D12_CPU_DESCRIPTOR_HANDLE *RTVs, WINBOOL SingleDescriptor,
      const D3D12_CPU_DESCRIPTOR_HANDLE *DSV
  ) {
    if (!RequireOpenRecording("OMSetRenderTargets")) return;
    try {
    allocator_->InvalidateCurrentPass();

    num_rtvs = NumRTV;
    for (unsigned i = 0; i < NumRTV; i++) {
      auto RTV = SingleDescriptor ? D3D12_CPU_DESCRIPTOR_HANDLE{RTVs[0].ptr + i * 32 /* kRTVDSVHeapIncrementalSize */}
                                  : RTVs[i];
      rtvs[i] = RTV;
    }
    dsv = DSV ? *DSV : D3D12_CPU_DESCRIPTOR_HANDLE();
    if (recording_trace_enabled_)
      memset(recording_rtvs_, 0, sizeof(recording_rtvs_));
    TraceRecording("OMSetRenderTargets", "accepted", "bound_descriptors", "single_descriptor=%u dsv_descriptor=%llx",
                   unsigned(SingleDescriptor != 0), static_cast<unsigned long long>(dsv.ptr));
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "OMSetRenderTargets", __LINE__);
      TraceRecording("OMSetRenderTargets", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ClearDepthStencilView(
      D3D12_CPU_DESCRIPTOR_HANDLE DSV, D3D12_CLEAR_FLAGS Flags, FLOAT Depth, UINT8 Stencil, UINT RectCount,
      const D3D12_RECT *Rects
  ) {
    if (!RequireOpenRecording("ClearDepthStencilView")) return;
    try {
    auto [Heap, Index] = GetRenderTargetHeap(device_, DSV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (!AttachmentDesc.Texture)
      return;
    auto CheckedFlags = Flags & DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View));
    if (!CheckedFlags)
      return;
    if (Rects) {
      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, 0, CheckedFlags);
      for (unsigned i = 0; i < RectCount; i++) {
        auto rect = Rects[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        allocator_->clear_rtv_.clear(rect_offset_x, rect_offset_y, rect_width, rect_height, Depth, Stencil);
      }
      allocator_->clear_rtv_.end();
      return;
    }
    allocator_->InvalidateCurrentPass();
    auto encoder_info = allocator_->AllocatePass<ClearEncoderData>();
    encoder_info->type = EncoderType::Clear;
    encoder_info->clear_dsv = CheckedFlags;
    encoder_info->depth_stencil = {Depth, Stencil};
    encoder_info->attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    encoder_info->array_length = AttachmentDesc.RenderTargetArrayLength;
    encoder_info->width = AttachmentDesc.Width;
    encoder_info->height = AttachmentDesc.Height;
    encoder_info->depth_plane = 0;

    allocator_->InvalidateCurrentPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearDepthStencilView", __LINE__);
      TraceRecording("ClearDepthStencilView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ClearRenderTargetView(
      D3D12_CPU_DESCRIPTOR_HANDLE RTV, const FLOAT Color[4], UINT RectCount, const D3D12_RECT *Rects
  ) {
    if (!RequireOpenRecording("ClearRenderTargetView")) return;
    try {
    auto [Heap, Index] = GetRenderTargetHeap(device_, RTV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (!AttachmentDesc.Texture)
      return;
    if (Rects) {
      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, AttachmentDesc.DepthPlane);
      for (unsigned i = 0; i < RectCount; i++) {
        auto rect = Rects[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        allocator_->clear_rtv_.clear(
            rect_offset_x, rect_offset_y, rect_width, rect_height, AttachmentDesc.RenderTargetArrayLength,
            {Color[0], Color[1], Color[2], Color[3]}
        );
      }
      allocator_->clear_rtv_.end();
      return;
    }
    allocator_->InvalidateCurrentPass();
    auto encoder_info = allocator_->AllocatePass<ClearEncoderData>();
    encoder_info->type = EncoderType::Clear;
    encoder_info->clear_dsv = 0;
    encoder_info->color = {Color[0], Color[1], Color[2], Color[3]};
    SanitizeRTVClearColor(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View), encoder_info->color);
    encoder_info->attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    encoder_info->array_length = AttachmentDesc.RenderTargetArrayLength;
    encoder_info->width = AttachmentDesc.Width;
    encoder_info->height = AttachmentDesc.Height;
    encoder_info->depth_plane = AttachmentDesc.DepthPlane;

    allocator_->InvalidateCurrentPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearRenderTargetView", __LINE__);
      TraceRecording("ClearRenderTargetView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewUint(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const UINT Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) {
    if (!RequireOpenRecording("ClearUnorderedAccessViewUint")) return;
    try {
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    auto color = std::array<uint32_t, 4>({Values[0], Values[1], Values[2], Values[3]});
    D3D12_RECT full_rect;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      full_rect = {
          (LONG)Descriptor.UAVBuffer.slice.byteOffset >> 2, 0,
          (LONG)((Descriptor.UAVBuffer.slice.byteOffset + Descriptor.UAVBuffer.slice.byteLength) >> 2), 1
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexture: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexture.texture, Descriptor.UAVTexture.view);
      full_rect = {
          0, 0, (LONG)Descriptor.UAVTexture.texture->width(Descriptor.UAVTexture.view),
          (LONG)Descriptor.UAVTexture.texture->height(Descriptor.UAVTexture.view)
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexelBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      full_rect = {
          (LONG)Descriptor.UAVTexelBuffer.slice.firstElement, 0,
          (LONG)(Descriptor.UAVTexelBuffer.slice.firstElement + Descriptor.UAVTexelBuffer.slice.elementCount), 1
      };
      break;
    }
    default:
      allocator_->clear_uav_.end();
      return;
    }

    const D3D12_RECT *rects = RectCount > 0 ? pRects : &full_rect;
    UINT rect_count = RectCount > 0 ? RectCount : 1;

    for (unsigned i = 0; i < rect_count; i++) {
      auto &rect = rects[i];
      auto width = rect.right - rect.left;
      auto height = rect.bottom - rect.top;
      if (width <= 0 || height <= 0)
        continue;
      allocator_->clear_uav_.clear(rect.left, rect.top, width, height);
    }

    allocator_->clear_uav_.end();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearUnorderedAccessViewUint", __LINE__);
      TraceRecording("ClearUnorderedAccessViewUint", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewFloat(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const float Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) {
    if (!RequireOpenRecording("ClearUnorderedAccessViewFloat")) return;
    try {
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    auto color = std::array<float, 4>({Values[0], Values[1], Values[2], Values[3]});
    D3D12_RECT full_rect;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      full_rect = {
          (LONG)Descriptor.UAVBuffer.slice.byteOffset >> 2, 0,
          (LONG)((Descriptor.UAVBuffer.slice.byteOffset + Descriptor.UAVBuffer.slice.byteLength) >> 2), 1
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexture: {
      allocator_->clear_uav_.begin(color, Descriptor.SRVTexture.texture, Descriptor.SRVTexture.view);
      full_rect = {
          0, 0, (LONG)Descriptor.SRVTexture.texture->width(Descriptor.SRVTexture.view),
          (LONG)Descriptor.SRVTexture.texture->height(Descriptor.SRVTexture.view)
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexelBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      full_rect = {
          (LONG)Descriptor.UAVTexelBuffer.slice.firstElement, 0,
          (LONG)(Descriptor.UAVTexelBuffer.slice.firstElement + Descriptor.UAVTexelBuffer.slice.elementCount), 1
      };
      break;
    }
    default:
      allocator_->clear_uav_.end();
      return;
    }

    const D3D12_RECT *rects = RectCount > 0 ? pRects : &full_rect;
    UINT rect_count = RectCount > 0 ? RectCount : 1;

    for (unsigned i = 0; i < rect_count; i++) {
      auto &rect = rects[i];
      auto width = rect.right - rect.left;
      auto height = rect.bottom - rect.top;
      if (width <= 0 || height <= 0)
        continue;
      allocator_->clear_uav_.clear(rect.left, rect.top, width, height);
    }

    allocator_->clear_uav_.end();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearUnorderedAccessViewFloat", __LINE__);
      TraceRecording("ClearUnorderedAccessViewFloat", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) {
    if (!RequireOpenRecording("DiscardResource")) return;
    try {
    // do nothing for now
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "DiscardResource", __LINE__);
      TraceRecording("DiscardResource", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  BeginQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    if (!RequireOpenRecording("BeginQuery")) return;
    try {
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (!ValidateQuery(heap, Type, Index, 1, "BeginQuery"))
      return;
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    for (const auto &query : active_queries_) {
      if (query.heap == heap && query.index == Index) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        return;
      }
    }
    // Query boundaries split the pass so each pass has a constant active set.
    PreBlit();
    static_cast<BlitEncoderData *>(allocator_->encoder_current)->query_buffers.push_back(heap->results);
    auto &clear = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
    clear.type = WMTBlitCommandFillBuffer;
    clear.buffer = heap->results;
    clear.offset = uint64_t(Index) * sizeof(uint64_t);
    clear.length = sizeof(uint64_t);
    clear.value = 0;
    active_queries_.push_back({heap, Type, Index});
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "BeginQuery", __LINE__);
      TraceRecording("BeginQuery", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  EndQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    if (!RequireOpenRecording("EndQuery")) return;
    try {
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (!ValidateQuery(heap, Type, Index, 1, "EndQuery"))
      return;
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      allocator_->InvalidateCurrentPass();
      PreBlit();
      auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
      pass->timestamp_samples = heap->timestamp_pages[Index / heap->TimestampPageSize];
      pass->timestamp_index = Index % heap->TimestampPageSize;
      pass->query_buffers.push_back(heap->results);
      // Metal does not sample an empty blit pass; the private write gives it work.
      auto &fill = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
      fill.type = WMTBlitCommandFillBuffer;
      fill.buffer = heap->results;
      fill.offset = uint64_t(Index) * sizeof(uint64_t);
      fill.length = sizeof(uint64_t);
      fill.value = 0;
      allocator_->InvalidateCurrentPass();
      return;
    }
    for (auto query = active_queries_.begin(); query != active_queries_.end(); ++query) {
      if (query->heap == heap && query->index == Index && query->type == Type) {
        allocator_->InvalidateCurrentPass();
        active_queries_.erase(query);
        return;
      }
    }
    SetRecordingError(E_INVALIDARG, __func__, __LINE__);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "EndQuery", __LINE__);
      TraceRecording("EndQuery", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ResolveQueryData(
      ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT QueryCount, ID3D12Resource *pDstBuffer,
      UINT64 AlignedDstBufferOffset
  ) {
    if (!RequireOpenRecording("ResolveQueryData")) return;
    try {
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    auto dst = static_cast<MTLD3D12Resource *>(pDstBuffer);
    if (!ValidateQuery(heap, Type, StartIndex, QueryCount, "ResolveQueryData"))
      return;
    if (!dst || !dst->buffer || (AlignedDstBufferOffset & 7)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    Com<ID3D12Device> dst_device;
    if (FAILED(dst->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&dst_device))) ||
        dst_device.ptr() != device_) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    const auto desc = dst->GetDesc();
    const uint64_t bytes = uint64_t(QueryCount) * sizeof(uint64_t);
    if (AlignedDstBufferOffset > desc.Width || bytes > desc.Width - AlignedDstBufferOffset) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    for (const auto &query : active_queries_) {
      if (query.heap == heap && query.index >= StartIndex && query.index - StartIndex < QueryCount) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        return;
      }
    }
    if (!QueryCount)
      return;
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      allocator_->InvalidateCurrentPass();
      while (QueryCount) {
        const UINT local_index = StartIndex % heap->TimestampPageSize;
        const UINT length = std::min(QueryCount, heap->TimestampPageSize - local_index);
        PreBlit();
        auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
        pass->resolve_samples = heap->timestamp_pages[StartIndex / heap->TimestampPageSize];
        pass->resolve_destination = dst->buffer->current()->buffer();
        pass->resolve_start = local_index;
        pass->resolve_count = length;
        pass->resolve_offset = AlignedDstBufferOffset;
        allocator_->InvalidateCurrentPass();
        StartIndex += length;
        QueryCount -= length;
        AlignedDstBufferOffset += uint64_t(length) * sizeof(uint64_t);
      }
      return;
    }
    if (Type == D3D12_QUERY_TYPE_OCCLUSION) {
      PreBlit();
      auto &buffers = static_cast<BlitEncoderData *>(allocator_->encoder_current)->query_buffers;
      buffers.push_back(heap->results);
      buffers.emplace_back(dst->buffer->current()->buffer());
      auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = heap->results;
      copy.src_offset = uint64_t(StartIndex) * sizeof(uint64_t);
      copy.dst = dst->buffer->current()->buffer();
      copy.dst_offset = AlignedDstBufferOffset;
      copy.copy_length = bytes;
      return;
    }

    allocator_->InvalidateCurrentPass();
    auto compute = allocator_->AllocatePass<ComputeEncoderData>();
    compute->type = EncoderType::Compute;
    compute->cmd_head.type = WMTComputeCommandNop;
    compute->cmd_head.next.set(0);
    compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
    compute->query_buffers.push_back(heap->results);
    compute->query_buffers.emplace_back(dst->buffer->current()->buffer());
    compute->query_pipeline = heap->binary_resolve;
    auto &pipeline = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    pipeline.type = WMTComputeCommandSetPSO;
    pipeline.pso = heap->binary_resolve;
    pipeline.threadgroup_size = {1, 1, 1};
    auto &src_buffer = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    src_buffer.type = WMTComputeCommandSetBuffer;
    src_buffer.buffer = heap->results;
    src_buffer.offset = uint64_t(StartIndex) * sizeof(uint64_t);
    src_buffer.index = 0;
    auto &dst_buffer = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    dst_buffer.type = WMTComputeCommandSetBuffer;
    dst_buffer.buffer = dst->buffer->current()->buffer();
    dst_buffer.offset = AlignedDstBufferOffset;
    dst_buffer.index = 1;
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {QueryCount, 1, 1};
    // Internal compute state must never leak into an application's next dispatch.
    allocator_->InvalidateCurrentPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ResolveQueryData", __LINE__);
      TraceRecording("ResolveQueryData", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Op) {
    if (!RequireOpenRecording("SetPredication")) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetPredication", __LINE__);
      TraceRecording("SetPredication", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *data, UINT size) {
    if (!RequireOpenRecording("SetMarker")) return;
    try { IMPLEMENT_ME   } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetMarker", __LINE__);
      TraceRecording("SetMarker", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *data, UINT size) {
    if (!RequireOpenRecording("BeginEvent")) return;
    try { IMPLEMENT_ME   } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "BeginEvent", __LINE__);
      TraceRecording("BeginEvent", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE EndEvent() {
    if (!RequireOpenRecording("EndEvent")) return;
    try { IMPLEMENT_ME   } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "EndEvent", __LINE__);
      TraceRecording("EndEvent", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

#include "encode-mesh-indirect.inc"

  void STDMETHODCALLTYPE ExecuteIndirect(
      ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgBuffer,
      UINT64 ArgBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset
  ) {
    if (!RequireOpenRecording("ExecuteIndirect")) return;
    try {
    const auto trace_outcome = [&](const char *outcome, const char *reason) {
      TraceRecording("ExecuteIndirect", outcome, reason,
                     "max_count=%u zero_count=%u signature=%p arg_buffer=%p arg_offset=%llu count_buffer=%p count_offset=%llu recording_hr=%08x",
                     MaxCommandCount, unsigned(!MaxCommandCount), static_cast<const void *>(pCommandSignature),
                     static_cast<const void *>(pArgBuffer), static_cast<unsigned long long>(ArgBufferOffset),
                     static_cast<const void *>(pCountBuffer), static_cast<unsigned long long>(CountBufferOffset), unsigned(recording_error_));
    };
    auto sig = static_cast<MTLD3D12CommandSignature *>(pCommandSignature);
    if (!sig) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "missing_command_signature");
      return;
    }
    auto arg_buffer = static_cast<MTLD3D12Resource *>(pArgBuffer);
    if (!arg_buffer || !arg_buffer->buffer) {
      trace_outcome("rejected", !arg_buffer ? "missing_argument_resource" : "argument_resource_not_buffer");
      return;
    }
    auto ArgBufferAddress = arg_buffer->buffer->current()->gpuAddress() + ArgBufferOffset;
    uint64_t CountBufferAddress = 0;
    if (auto count_buffer = static_cast<MTLD3D12Resource *>(pCountBuffer)) {
      if (!count_buffer->buffer) {
        trace_outcome("rejected", "count_resource_not_buffer");
        return;
      }
      CountBufferAddress = count_buffer->buffer->current()->gpuAddress() + CountBufferOffset;
    }
    if (sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
      if (!PreDispatch(sig->UpdateRootArguments)) {
        trace_outcome("rejected", "PreDispatch_false_missing_compute_pso");
        return;
      }

      auto cmd = allocator_->EncodeIndirectComputeCommand(sig, pso_compute_.ptr(), MaxCommandCount);
      cmd->max_count_buffer = CountBufferAddress;
      cmd->argument_buffer = ArgBufferAddress;

      if (sig->UpdateRootArguments) {
        cmd->rootsig_qwords = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_.words.data(), MaxCommandCount);
        cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
        cmd->rootsig_qwords_stride = rootsig_compute_->UploadQwords;
        cmd->static_samplers = EncodeStaticSamplers(rootsig_compute_.ptr());
        cmd->static_samplers += allocator_->gpu_heap_buffer_address_;
        // The CPU snapshot cannot claim arguments modified by indirect execution are still initialized.
        rootarg_compute_staging_.initialized.fill(0);
      }

      trace_outcome("emitted", MaxCommandCount ? "indirect_compute_command" : "indirect_compute_zero_max_count");
      return;
    }
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "invalid_topology");
      return;
    }
    if (!pso_graphics_ || cp_count != pso_graphics_->tess_control_points) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", !pso_graphics_ ? "missing_graphics_pso" : "patch_control_point_mismatch");
      return;
    }
    const auto gs_topology = pso_graphics_->geometry_input_vertices == 1 ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST :
                             pso_graphics_->geometry_input_vertices == 2 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (pso_graphics_->geometry_pipeline && topology_ != gs_topology) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "geometry_input_topology_mismatch");
      return;
    }
    if (pso_graphics_->geometry_pipeline || cp_count) {
      if (EncodeMeshIndirect(sig, MaxCommandCount, arg_buffer, ArgBufferOffset,
                             static_cast<MTLD3D12Resource *>(pCountBuffer), CountBufferOffset))
        trace_outcome("emitted", MaxCommandCount ? "indirect_mesh_gpu_marshaled" : "indirect_mesh_zero_max_count");
      return;
    }
    bool encode_binding = sig->UpdateRootArguments || sig->UpdateIndexBuffer || sig->UpdateVertexBuffers;
    DrawCallStatus status = PreDraw(encode_binding);
    if (status == DrawCallStatus::Invalid) {
      trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_invalid");
      return;
    }
    if (status != DrawCallStatus::Ordinary) {
      SetRecordingError(E_NOTIMPL, __func__, __LINE__);
      trace_outcome("rejected", status == DrawCallStatus::Geometry ? "indirect_geometry_unsupported" :
                    "indirect_tessellation_unsupported");
      return;
    }

    auto cmd = allocator_->EncodeIndirectRenderCommand(sig, pso_graphics_.ptr(), MaxCommandCount);
    cmd->max_count_buffer = CountBufferAddress;
    cmd->argument_buffer = ArgBufferAddress;
    cmd->primitive_type = primitive_type;
    cmd->index_buffer = index_buffer_address;
    cmd->index_buffer_format = index_type == WMTIndexTypeUInt32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    if (!encode_binding) {
      trace_outcome("emitted", MaxCommandCount ? "indirect_render_command" : "indirect_render_zero_max_count");
      return;
    }
    cmd->rootsig_qwords = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_.words.data(), MaxCommandCount);
    cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
    cmd->rootsig_qwords_stride = rootsig_graphics_->UploadQwords;
    cmd->static_samplers = EncodeStaticSamplers(rootsig_graphics_.ptr());
    cmd->static_samplers += allocator_->gpu_heap_buffer_address_;
    auto [VBOffset, VBStride] = PopulateVertexBufferTable(MaxCommandCount);
    cmd->vertex_buffer = allocator_->gpu_heap_buffer_address_ + VBOffset;
    cmd->vertex_argbuf_stride = VBStride;
    if (sig->UpdateRootArguments) rootarg_graphics_staging_.initialized.fill(0);
    trace_outcome("emitted", MaxCommandCount ? "indirect_render_with_bindings" : "indirect_render_zero_max_count");
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ExecuteIndirect", __LINE__);
      TraceRecording("ExecuteIndirect", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) {
    if (!RequireOpenRecording("AtomicCopyBufferUINT")) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "AtomicCopyBufferUINT", __LINE__);
      TraceRecording("AtomicCopyBufferUINT", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT64(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) {
    if (!RequireOpenRecording("AtomicCopyBufferUINT64")) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "AtomicCopyBufferUINT64", __LINE__);
      TraceRecording("AtomicCopyBufferUINT64", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  OMSetDepthBounds(FLOAT Min, FLOAT Max) {
    if (!RequireOpenRecording("OMSetDepthBounds")) return;
    try {
    WARN("OMSetDepthBounds: ignoring (", Min, ", ", Max, ")");
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "OMSetDepthBounds", __LINE__);
      TraceRecording("OMSetDepthBounds", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  SetSamplePositions(UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION *pSamplePositions) {
    if (!RequireOpenRecording("SetSamplePositions")) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetSamplePositions", __LINE__);
      TraceRecording("SetSamplePositions", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  ResolveSubresourceRegion(
      ID3D12Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource *pSrcResource,
      UINT SrcSubresource, D3D12_RECT *pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode
  ) {
    if (!RequireOpenRecording("ResolveSubresourceRegion")) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ResolveSubresourceRegion", __LINE__);
      TraceRecording("ResolveSubresourceRegion", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  SetViewInstanceMask(UINT Mask) {
    if (!RequireOpenRecording("SetViewInstanceMask")) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetViewInstanceMask", __LINE__);
      TraceRecording("SetViewInstanceMask", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  WriteBufferImmediate(
      UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes
  ) {
    if (!RequireOpenRecording("WriteBufferImmediate")) return;
    try {
    if (encoder_count != std::numeric_limits<size_t>::max() || !Count)
      return;
    if (!pParams) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    if (allocator_->type_ != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        allocator_->type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        allocator_->type_ != D3D12_COMMAND_LIST_TYPE_COPY) {
      SetRecordingError(E_NOTIMPL, __func__, __LINE__);
      return;
    }
    struct Destination {
      WMT::Reference<WMT::Buffer> buffer;
      uint64_t offset;
    };
    std::vector<Destination> destinations;
    destinations.reserve(Count);
    UINT modes_seen = 0;
    for (UINT i = 0; i < Count; ++i) {
      const auto mode = pModes ? pModes[i] : D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
      uint64_t offset = 0;
      auto allocation = (pParams[i].Dest & 3) ? nullptr :
          device_->LookupBufferByVA(pParams[i].Dest, sizeof(UINT), &offset);
      if (!allocation || (mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT &&
                          mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN &&
                          mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT)) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        return;
      }
      modes_seen |= 1u << mode;
      destinations.push_back({WMT::Reference<WMT::Buffer>(allocation->buffer()), offset});
    }
    static std::atomic_uint trace_count{0};
    if (trace_count.fetch_add(1, std::memory_order_relaxed) < 16)
      fprintf(stderr, "dx12_immediate count=%u modes_mask=%u supported=1\n", Count, modes_seen);
    WMTBufferInfo info{};
    info.length = uint64_t(Count) * sizeof(UINT);
    info.options = WMTResourceStorageModeShared;
    auto values = device_->GetMTLDevice().newBuffer(info);
    if (!values || !info.memory.get()) {
      SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);
      return;
    }
    // Capture caller values at recording time; retain them across list replays.
    for (UINT i = 0; i < Count; ++i)
      static_cast<UINT *>(info.memory.get())[i] = pParams[i].Value;
    for (UINT i = 0; i < Count; ++i) {
      const auto mode = pModes ? pModes[i] : D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
      allocator_->InvalidateCurrentPass();
      if (!PreBlit())
        return;
      auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
      pass->wait_for_prior_work = mode == D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
      pass->immediate_out = mode == D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
      pass->join_immediate_out = true;
      pass->immediate_buffers.push_back(values);
      pass->immediate_buffers.push_back(destinations[i].buffer);
      auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = values;
      copy.src_offset = uint64_t(i) * sizeof(UINT);
      copy.dst = destinations[i].buffer;
      copy.dst_offset = destinations[i].offset;
      copy.copy_length = sizeof(UINT);
      allocator_->InvalidateCurrentPass();
    }
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "WriteBufferImmediate", __LINE__);
      TraceRecording("WriteBufferImmediate", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }
};

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::CreateCommandList(
    UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
    void **ppCommandList
) {
  if (Type != type_)
    return E_INVALIDARG;

  auto cmd_list = Com(new MTLD3D12GraphicsCommandListImpl(device_));
  HRESULT hr = cmd_list->Initialize(this, pInitialPipelineState);
  if (FAILED(hr))
    return hr;
  return cmd_list->QueryInterface(riid, ppCommandList);
}

}; // namespace dxmt
