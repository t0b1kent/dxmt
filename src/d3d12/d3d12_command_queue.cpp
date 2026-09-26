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

#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_compute_commands.hpp"
#include "d3d12_frame_trace.hpp"
#include "d3d12_command_capture.hpp"
#include "d3d12_submission_capture.hpp"
#include "d3d12_pageable.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace dxmt {

constexpr auto kCommandQueueSize = 32u;

const GUID kD3D12CommandQueueDownlevelUUID = {
    0x38a8c5ef, 0x7ccb, 0x4e81, {0x91, 0x4f, 0xa6, 0xe9, 0xd0, 0x72, 0xc4, 0x94}
};

static bool
FrameReadbackEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FRAME_READBACK", value, sizeof(value)) == 1 &&
                        value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

static bool
FullFrameCaptureEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FRAME_CAPTURE", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

// Explicit diagnostics only. This clock includes work from all queues between observed Present calls.
static std::atomic<uint64_t> capture_next_present{1};
static std::atomic<uint32_t> capture_lines{0};

static bool
FaultCaptureEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FAULT_CAPTURE", value, sizeof(value)) == 1 &&
                        value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

// First observed GPU failure across queues, independent of sampled frame trace windows.
static std::atomic<bool> fault_capture_dumped{false};

struct D3D12FrameReadback {
  static constexpr uint8_t sentinel = 0xa5;
  uint64_t present_index = 0, width = 0, height = 0;
  uint64_t render_index = 0;
  WMTPixelFormat format{};
  uint32_t pitch = 0;
  bool bgra = false, copied = false;
  const uint8_t *data = nullptr;
  WMT::Reference<WMT::Buffer> buffer;
  WMT::Reference<WMT::Texture> source_allocation;
  WMT::Reference<WMT::Texture> source_texture;
  WMT::Reference<WMT::ResidencySet> residency;

  void Log(const void *queue, uint64_t seq, const char *stage, const char *detail) const {
    const DWORD error = GetLastError();
    char line[640];
    const int length = std::snprintf(
        line, sizeof(line),
        "dx12_readback source=%s stage=%s queue=%p present=%llu render=%llu seq=%llu texture=%llx format=%u width=%llu height=%llu %s\n",
        render_index ? "render_target" : "actual_backbuffer", stage, queue,
        static_cast<unsigned long long>(present_index), static_cast<unsigned long long>(render_index),
        static_cast<unsigned long long>(seq), static_cast<unsigned long long>(source_texture.handle),
        unsigned(format), static_cast<unsigned long long>(width), static_cast<unsigned long long>(height), detail);
    DWORD written;
    if (length > 0 && size_t(length) < sizeof(line))
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
    SetLastError(error);
  }

  void Complete(const void *queue, uint64_t seq, WMTCommandBufferStatus status) {
    if (!present_index && !render_index)
      return;
    char detail[320];
    std::snprintf(detail, sizeof(detail), "status=%u copied=%u", unsigned(status), unsigned(copied));
    // This stream is indexed by presents, not by all command buffer completions.
    Log(queue, seq, render_index ? "render.complete" : "present.complete", detail);
    if (copied && status == WMTCommandBufferStatusCompleted) {
      uint64_t nonzero = 0, hash = 14695981039346656037ull;
      bool unchanged_sentinel = true;
      unsigned rgb_min[3] = {255, 255, 255}, rgb_max[3] = {};
      for (uint64_t y = 0; y < height; ++y) {
        const auto *row = data + y * pitch;
        for (uint64_t x = 0; x < width; ++x) {
          const auto *pixel = row + x * 4;
          unchanged_sentinel &= pixel[0] == sentinel && pixel[1] == sentinel && pixel[2] == sentinel;
          nonzero += (pixel[0] | pixel[1] | pixel[2]) != 0;
          for (unsigned c = 0; c < 3; ++c) {
            const unsigned value = pixel[bgra ? 2 - c : c];
            rgb_min[c] = std::min(rgb_min[c], value);
            rgb_max[c] = std::max(rgb_max[c], value);
            hash = (hash ^ value) * 1099511628211ull;
          }
        }
      }
      // Hash canonical RGB bytes only: alpha and row padding never affect the black test or hash.
      if (unchanged_sentinel) {
        // A real image could match the sentinel too; neither case establishes a valid capture.
        Log(queue, seq, "backbuffer.ambiguous", "reason=unchanged_sentinel_rgb sentinel_byte=0xa5 black=unknown");
      } else {
        const bool constant = rgb_min[0] == rgb_max[0] && rgb_min[1] == rgb_max[1] && rgb_min[2] == rgb_max[2];
        // Nonzero RGB (including a constant clear) is not evidence of a rendered menu.
        std::snprintf(detail, sizeof(detail),
                    "status=%u rgb_nonzero_pixels=%llu rgb_min=%u,%u,%u rgb_max=%u,%u,%u "
                    "rgb_fnv1a64=%016llx black=%u rgb_constant=%u",
                    unsigned(status), static_cast<unsigned long long>(nonzero),
                    rgb_min[0], rgb_min[1], rgb_min[2], rgb_max[0], rgb_max[1], rgb_max[2],
                    static_cast<unsigned long long>(hash), unsigned(nonzero == 0), unsigned(constant));
        Log(queue, seq, "backbuffer.rgb", detail);
      }
    }
    if (residency) {
      residency.removeAllAllocations();
      residency.commit();
    }
  }
};

// The retirement worker must not own or access any public COM object.
struct D3D12CommandQueueState {
  WMT::Reference<WMT::Device> device_;
  WMT::Reference<WMT::ResidencySet> residency_set_;
  WMT::Reference<WMT::CommandQueue> queue_;
  WMT::Reference<WMT::Fence> fence_;
  WMT::Reference<WMT::Fence> immediate_out_fence_;
  bool immediate_out_pending_ = false;
  WMT::Reference<WMT::Buffer> visibility_scratch_;
  WMT::Reference<WMT::SharedEvent> timestamp_event_;
  uint64_t timestamp_serial_ = 0;
  WMT::Reference<WMT::MappingCommandQueue> mapping_queue_;
  WMT::Reference<WMT::SharedEvent> mapping_event_;
  uint64_t mapping_serial_ = 0;
  uint64_t readback_present_index_ = 0; // Accessed only under mutex_commit_; saturates after the last sample.
  uint64_t readback_render_index_ = 0;
  unsigned transfer_trace_lines_ = 0;
  // A native enqueue failure may leave work in another queue. Quarantine rather
  // than releasing that work's allocations based on an unrelated CB completion.
  std::shared_ptr<D3D12CommandQueueState> failed_mapping_keepalive_;
  std::vector<std::shared_ptr<D3D12SparseBufferState>> failed_mapping_resources_;

  std::atomic_uint64_t inflight_cmdbuf_seq_ = 1;
  std::atomic_uint64_t inflight_cmdbuf_count_ = 0;
  std::atomic_uint64_t inflight_cmdbuf_stop_ = 0;

  struct InflightCommandBuffer {
    WMT::Reference<WMT::CommandBuffer> cmdbuf{};
    std::shared_ptr<D3D12PresentState> present;
    WMT::Reference<WMT::Texture> present_texture;
    std::shared_ptr<D3D12RemovableFence> retained_fence;
    std::vector<std::shared_ptr<D3D12SparseBufferState>> sparse_resources;
    std::vector<WMT::Reference<WMT::Buffer>> timestamp_scratch;
    D3D12FrameReadback readback;
    std::vector<D3D12FrameReadback> render_readbacks;
    std::vector<D3D12NativeComputeDispatch> argument_dispatches;
    std::vector<D3D12NativeAccelerationBuild> acceleration_builds;
    D3D12SubmissionCapture *fault_capture = nullptr;
  };

  std::array<InflightCommandBuffer, kCommandQueueSize> inflight_cmdbuf_pool_;
  std::array<std::unique_ptr<D3D12SubmissionCapture>, kCommandQueueSize> fault_capture_pool_;
  dxmt::mutex mutex_commit_;

  void
  CommandBufferWaitingThread() {
    env::setThreadName("dxmt-cmdbuf-waiting-thread");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    uint64_t internal_seq = 1;
    for (;;) {
      inflight_cmdbuf_seq_.wait(internal_seq, std::memory_order_acquire);
      if (inflight_cmdbuf_stop_.load() == internal_seq)
        break;
      auto pool = WMT::MakeAutoreleasePool();
      auto &inflight = inflight_cmdbuf_pool_[internal_seq % kCommandQueueSize];

      if (inflight.cmdbuf.status() <= WMTCommandBufferStatusScheduled)
        inflight.cmdbuf.waitUntilCompleted();
      const auto status = inflight.cmdbuf.status();
      static std::atomic<uint32_t> trace_complete{0}, trace_error{0};
      TraceFrame(trace_complete, "cmdbuf.complete", this, "seq=%llu status=%u present=%u",
                 static_cast<unsigned long long>(internal_seq), unsigned(status), unsigned(bool(inflight.present)));
      if (status == WMTCommandBufferStatusError) {
        TraceFrame(trace_error, "cmdbuf.error", this, "seq=%llu status=%u present=%u",
                   static_cast<unsigned long long>(internal_seq), unsigned(status), unsigned(bool(inflight.present)));
        ERR("Device error: ", inflight.cmdbuf.error().description().getUTF8String());
        if (FaultCaptureEnabled() && !fault_capture_dumped.exchange(true, std::memory_order_relaxed)) {
          const DWORD error = GetLastError();
          auto emit = [&](const char *detail) {
            char line[2304];
            const int n = std::snprintf(line, sizeof(line), "dx12_fault queue=%p seq=%llu status=%u %s\n",
                this, (unsigned long long)internal_seq, unsigned(status), detail);
            DWORD written;
            if (n > 0 && std::size_t(n) < sizeof(line))
              WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
          };
          if (inflight.fault_capture) inflight.fault_capture->Dump(emit);
          else emit("event=unavailable reason=capture_allocation_failed");
          SetLastError(error);
        }
      }

      inflight.readback.Complete(this, internal_seq, status);
      for (auto &readback : inflight.render_readbacks)
        readback.Complete(this, internal_seq, status);

      if (inflight.present && inflight.present->semaphore)
        ReleaseSemaphore(inflight.present->semaphore, 1, nullptr);

      inflight = {};

      inflight_cmdbuf_count_.fetch_sub(1, std::memory_order_release);
      inflight_cmdbuf_count_.notify_one();

      internal_seq++;
    }
  };
};

struct D3D12QueueRetirement {
  std::shared_ptr<D3D12CommandQueueState> state;
  HMODULE module;
};

static DWORD WINAPI
RetireD3D12Queue(void *context) {
  HMODULE module;
  {
    std::unique_ptr<D3D12QueueRetirement> retirement(static_cast<D3D12QueueRetirement *>(context));
    module = retirement->module;
    retirement->state->CommandBufferWaitingThread();
  }
  // No C++ objects or return address may remain in the module being released.
  FreeLibraryAndExitThread(module, 0);
}

class MTLD3D12CommandQueueImpl : public MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory> {
  D3D12_COMMAND_QUEUE_DESC desc_;
  std::shared_ptr<D3D12CommandQueueState> state_ = std::make_shared<D3D12CommandQueueState>();
  HANDLE inflight_cmdbuf_wait_thread_ = nullptr;

  struct CommittingScope {
    D3D12CommandQueueState *queue;
    std::lock_guard<dxmt::mutex> lock;
    uint64_t seq;
    D3D12CommandQueueState::InflightCommandBuffer &inflight;
    WMT::Reference<WMT::Object> pool;
    bool discard = false;

    CommittingScope(D3D12CommandQueueState *queue) :
        queue(queue),
        lock(queue->mutex_commit_),
        seq(queue->inflight_cmdbuf_seq_.load(std::memory_order_relaxed)),
        inflight(queue->inflight_cmdbuf_pool_[seq % kCommandQueueSize]),
        pool(WMT::MakeAutoreleasePool()) {
      // Serialize the capacity check with all other producers of this ring.
      queue->inflight_cmdbuf_count_.wait(kCommandQueueSize, std::memory_order_acquire);
      inflight.cmdbuf = queue->queue_.commandBuffer();
      if (FaultCaptureEnabled() && !fault_capture_dumped.load(std::memory_order_relaxed)) {
        const DWORD error = GetLastError();
        auto &capture = queue->fault_capture_pool_[seq % kCommandQueueSize];
        if (!capture) capture.reset(new (std::nothrow) D3D12SubmissionCapture);
        if (capture) {
          capture->Reset();
          inflight.fault_capture = capture.get();
        }
        SetLastError(error);
      }
    };

    ~CommittingScope() {
      if (discard) {
        inflight = {};
        return;
      }
      static std::atomic<uint32_t> trace_enter{0}, trace_return{0};
      TraceFrame(trace_enter, "cmdbuf.commit.enter", queue, "seq=%llu present=%u",
                 static_cast<unsigned long long>(seq), unsigned(bool(inflight.present)));
      inflight.cmdbuf.commit();
      TraceFrame(trace_return, "cmdbuf.commit.return", queue, "seq=%llu present=%u",
                 static_cast<unsigned long long>(seq), unsigned(bool(inflight.present)));
      queue->inflight_cmdbuf_count_.fetch_add(1, std::memory_order_relaxed);
      queue->inflight_cmdbuf_seq_.fetch_add(1, std::memory_order_release);
      queue->inflight_cmdbuf_seq_.notify_one();
    }
  };

  CommittingScope
  StartCommitting() {
    return CommittingScope(state_.get());
  }


  static HRESULT NativeMetalBinding(const D3D12NativeDescriptorCapture &capture,
                                    const metal_argument::Member &member, WMTArgumentBinding &binding) {
    binding = {};
    if (capture.counter) return DXGI_ERROR_UNSUPPORTED;
    binding.index = member.index;
    binding.kind = member.kind;
    if (metal_argument::Acceleration(member.kind)) {
      if (!capture.acceleration) return E_INVALIDARG;
      const auto kind = capture.acceleration->Type() == D3D12NativeAccelerationCapture::Kind::Primitive ?
          WMTArgumentKindPrimitiveAccelerationStructure : WMTArgumentKindInstanceAccelerationStructure;
      if (member.kind != kind) return E_INVALIDARG;
      binding.resource = capture.acceleration->Native().resource;
    } else if (capture.acceleration) {
      return E_INVALIDARG;
    } else if (member.kind == WMTArgumentKindBuffer) {
      if (capture.type != ShaderVisibleDescriptorType::ConstantBuffer &&
          capture.type != ShaderVisibleDescriptorType::SRVBuffer &&
          capture.type != ShaderVisibleDescriptorType::UAVBuffer) return E_INVALIDARG;
      if (!capture.resource || !capture.resource->buffer_allocation || !capture.byte_length)
        return E_INVALIDARG;
      binding.resource = capture.resource->buffer_allocation->buffer();
      binding.offset = capture.buffer_offset;
      binding.length = capture.byte_length;
    } else if (member.kind == WMTArgumentKindTexture) {
      if (capture.type == ShaderVisibleDescriptorType::SRVTexture ||
          capture.type == ShaderVisibleDescriptorType::UAVTexture)
        binding.resource = capture.texture.texture();
      else if (capture.type == ShaderVisibleDescriptorType::SRVTexelBuffer ||
               capture.type == ShaderVisibleDescriptorType::UAVTexelBuffer)
        binding.resource = capture.texel;
      else return E_INVALIDARG;
    } else {
      if (capture.type != ShaderVisibleDescriptorType::Sampler || !capture.sampler ||
          capture.sampler->lod_bias != 0) return DXGI_ERROR_UNSUPPORTED;
      binding.resource = capture.sampler->sampler_state;
    }
    return binding.resource ? S_OK : E_INVALIDARG;
  }

  static void PrepareResourceUses(D3D12NativeComputeDispatch &packet) {
    auto add = [&](obj_handle_t handle, WMTResourceUsage usage) {
      if (!handle) return;
      wmtcmd_compute_useresource command{};
      command.type = WMTComputeCommandUseResource;
      command.resource = handle;
      command.usage = usage;
      packet.resource_uses.push_back(command);
    };
    for (const auto &r : packet.resources) {
      if (r.acceleration) r.acceleration->ForEach([&](const auto &allocation) {
        add(allocation.resource, WMTResourceUsageRead);
      });
      const bool writable = r.type == ShaderVisibleDescriptorType::UAVBuffer ||
          r.type == ShaderVisibleDescriptorType::UAVTexelBuffer || r.type == ShaderVisibleDescriptorType::UAVTexture;
      const auto usage = writable ? WMTResourceUsage(WMTResourceUsageRead | WMTResourceUsageWrite) : WMTResourceUsageRead;
      if (r.resource) {
        if (r.resource->buffer_allocation) add(r.resource->buffer_allocation->buffer(), usage);
        if (r.resource->texture_allocation) add(r.resource->texture_allocation->texture(), usage);
      }
      if (r.texture) add(r.texture.texture(), usage);
      if (r.texel) add(r.texel, usage);
      if (r.counter && r.counter->buffer_allocation)
        add(r.counter->buffer_allocation->buffer(), WMTResourceUsage(WMTResourceUsageRead | WMTResourceUsageWrite));
    }
  }

  HRESULT PrepareAccelerationBuild(const std::shared_ptr<const D3D12AccelerationBuildPlan> &plan,
      D3D12SubmissionScenes &scenes,uint64_t &bytes,D3D12NativeAccelerationBuild &out) {
    out={};
    if (!plan || plan->device_identity!=static_cast<ID3D12Device *>(device_) ||
        (desc_.Type!=D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type!=D3D12_COMMAND_LIST_TYPE_COMPUTE)) return E_INVALIDARG;
    D3D12NativeAccelerationBuild next;next.plan=plan;
    std::vector<D3D12NativeAccelerationCapture::Ptr> children;
    for (const auto &input:plan->children) {
      auto child=input.producer?scenes.Find(input.producer.get()):input.existing;
      if (!child || child->Type()!=D3D12NativeAccelerationCapture::Kind::Primitive) return E_INVALIDARG;
      next.child_handles.push_back(child->Native().resource);
      children.push_back(std::move(child));
    }
    WMTASSizeInfo sizes{};
    if (state_->device_.accelerationStructureSizes(next.Descriptor(),sizes)!=WMTArgumentStatusReady)
      return E_INVALIDARG;
    if (!scene_build::Charge(sizes.structure_size,sizes.build_scratch_size,bytes)) return DXGI_ERROR_UNSUPPORTED;
    WMTArgumentStatus status=WMTArgumentStatusFailed;
    auto target=state_->device_.newAccelerationStructure(sizes.structure_size,status);
    if (!target || status!=WMTArgumentStatusReady) return status==WMTArgumentStatusOutOfMemory?E_OUTOFMEMORY:E_FAIL;
    WMTBufferInfo info{};info.length=sizes.build_scratch_size;info.options=WMTResourceStorageModePrivate;
    auto scratch=state_->device_.newBuffer(info);
    if (!scratch) return E_OUTOFMEMORY;
    auto owner=device_->GetResidencyOwner();
    next.scratch=std::make_shared<D3D12NativeArgumentBuffer>(std::move(scratch),info.gpu_address,owner);
    auto allocation=std::make_shared<D3D12NativeAccelerationAllocation>(std::move(target),owner);
    const auto result=D3D12NativeAccelerationCapture::Create(
        plan->kind==WMTASBuildTriangles?D3D12NativeAccelerationCapture::Kind::Primitive:D3D12NativeAccelerationCapture::Kind::Instance,
        std::move(allocation),std::move(children),next.result);
    if (result!=capture::Status::Ready) return CaptureResult(result);
    scenes.Publish(plan.get(),next.result);
    out=std::move(next);return S_OK;
  }

  HRESULT PrepareMetalArgumentDispatch(const D3D12RecordedComputeDispatch &record,
                                       const D3D12SubmissionScenes &scenes,D3D12NativeComputeDispatch &out) {
    const auto &plan = *record.metal_arguments;
    auto valid = CaptureResult(metal_argument::Match(plan, record.bindings.size(), record.entry_layout));
    if (FAILED(valid)) return valid;
    if (!record.function || !record.pipeline) return E_INVALIDARG;
    D3D12NativeComputeDispatch next;
    next.pipeline = record.pipeline;
    next.threads = record.threads;
    next.groups = record.groups;
    next.resources.resize(record.bindings.size());
    for (size_t i = 0; i < record.bindings.size(); ++i) {
      const auto &binding=record.bindings[i];
      if (binding.acceleration_producer) {
        if (binding.acceleration || binding.root_buffer || binding.compiler_footprint ||
            binding.recorded_device!=record.device || binding.acceleration_device.ptr()!=record.device) return E_INVALIDARG;
        next.resources[i].acceleration=scenes.Find(binding.acceleration_producer.get());
        if (!next.resources[i].acceleration) return E_INVALIDARG;
      } else {
        const auto hr=binding.ResolveForSubmission(next.resources[i]);
        if (FAILED(hr)) return hr;
      }
    }

    std::vector<WMTComputeBinding> requirements;
    for (const auto &member : plan.direct) {
      const WMTComputeBinding *requirement = nullptr;
      for (size_t i = 0; i < record.entry_layout.count; ++i) {
        const auto &r = record.entry_layout.bindings[i];
        if (r.kind != WMTComputeBindingArgumentBuffer && r.index == member.index &&
            uint64_t(r.kind) == uint64_t(member.kind)) requirement = &r;
      }
      if (!requirement) return E_INVALIDARG;
      const auto &capture = next.resources[member.source];
      if (requirement->access != 0 && capture.type != ShaderVisibleDescriptorType::UAVBuffer &&
          capture.type != ShaderVisibleDescriptorType::UAVTexture &&
          capture.type != ShaderVisibleDescriptorType::UAVTexelBuffer) return E_INVALIDARG;
      WMTArgumentBinding binding{};
      const auto hr = NativeMetalBinding(capture, member, binding);
      if (FAILED(hr)) return hr;
      requirements.push_back(*requirement);
      next.direct_bindings.push_back(binding);
    }
    const auto direct_status = MTLDevice_validateComputeBindings(state_->device_,
        requirements.data(), next.direct_bindings.data(), requirements.size());
    if (direct_status != WMTArgumentStatusReady)
      return direct_status == WMTArgumentStatusInvalid ? E_INVALIDARG :
          direct_status == WMTArgumentStatusUnsupported ? DXGI_ERROR_UNSUPPORTED : E_FAIL;
    uint64_t total = 0;
    for (const auto &block : plan.blocks) {
      std::vector<WMTArgumentBinding> bindings;
      bindings.reserve(block.members.size());
      for (const auto &member : block.members) {
        WMTArgumentBinding binding{};
        const auto hr = NativeMetalBinding(next.resources[member.source], member, binding);
        if (FAILED(hr)) return hr;
        bindings.push_back(binding);
      }
      WMTBufferInfo info{};
      WMTArgumentStatus status = WMTArgumentStatusFailed;
      auto buffer = record.function.newArgumentBuffer(block.index, bindings.data(), bindings.size(), info, status);
      if (!buffer || status != WMTArgumentStatusReady)
        return status == WMTArgumentStatusOutOfMemory ? E_OUTOFMEMORY :
               status == WMTArgumentStatusInvalid ? E_INVALIDARG :
               status == WMTArgumentStatusUnsupported ? DXGI_ERROR_UNSUPPORTED : E_FAIL;
      if (!info.length || info.length > 16 * 1024 * 1024 || total > 64 * 1024 * 1024 - info.length)
        return DXGI_ERROR_UNSUPPORTED;
      total += info.length;
      next.buffers.push_back(std::make_shared<D3D12NativeArgumentBuffer>(
          std::move(buffer), info.gpu_address, device_->GetResidencyOwner()));
      next.indices.push_back(block.index);
    }
    PrepareResourceUses(next);
    out = std::move(next);
    return S_OK;
  }

  HRESULT PrepareArgumentDispatch(const D3D12RecordedComputeDispatch &record,
                                  const D3D12SubmissionScenes &scenes,D3D12NativeComputeDispatch &out) {
    out = {};
    if (record.device != static_cast<ID3D12Device *>(device_) ||
        (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE))
      return E_INVALIDARG;
    if (record.metal_arguments) return PrepareMetalArgumentDispatch(record, scenes, out);
    for (const auto &binding : record.bindings)
      if (binding.acceleration || binding.acceleration_producer) return DXGI_ERROR_UNSUPPORTED;
    argument_upload::Upload<D3D12NativeDescriptorCapture, std::shared_ptr<D3D12NativeArgumentBuffer>> upload;
    std::vector<void *> mapped;
    mapped.reserve(record.arguments.blocks.size());
    const auto status = argument_upload::Prepare(record.arguments, record.bindings.size(),
      [&](size_t index, D3D12NativeDescriptorCapture &capture) {
        // Resolve public heap state only here, on the caller's Wine thread.
        const HRESULT hr = record.bindings[index].ResolveForSubmission(capture);
        if (SUCCEEDED(hr)) return capture::Status::Ready;
        return hr == E_OUTOFMEMORY ? capture::Status::OutOfMemory :
               hr == E_INVALIDARG ? capture::Status::Invalid : capture::Status::Unsupported;
      },
      [&](const argument_upload::Block &block, std::shared_ptr<D3D12NativeArgumentBuffer> &buffer,
          uint64_t &address) {
        WMTBufferInfo info{};
        info.length = block.initial.size();
        info.options = WMTResourceStorageModeShared;
        auto native = state_->device_.newBuffer(info);
        auto *memory = info.memory.get_accessible_or_null();
        if (!native || !memory) return capture::Status::OutOfMemory;
        address = info.gpu_address;
        buffer = std::make_shared<D3D12NativeArgumentBuffer>(std::move(native), address, device_->GetResidencyOwner());
        mapped.push_back(memory);
        return capture::Status::Ready;
      }, upload);
    if (status != capture::Status::Ready) return CaptureResult(status);
    D3D12NativeComputeDispatch next;
    next.pipeline = record.pipeline;
    next.threads = record.threads;
    next.groups = record.groups;
    next.buffers = std::move(upload.buffers);
    next.resources = std::move(upload.sources);
    next.indices.reserve(record.arguments.blocks.size());
    for (size_t i = 0; i < upload.bytes.size(); ++i) {
      memcpy(mapped[i], upload.bytes[i].data(), upload.bytes[i].size());
      next.indices.push_back(record.arguments.blocks[i].index);
    }
    PrepareResourceUses(next);
    out = std::move(next);
    return S_OK;
  }

  bool TransferWindowEnabled() const {
    if (FullFrameCaptureEnabled()) {
      const auto n = capture_next_present.load(std::memory_order_relaxed);
      return FrameReadbackEnabled() && FrameTraceEnabled() && capture_lines.load(std::memory_order_relaxed) <= 40960 &&
             (n <= 3 || (n >= 512 && n <= 514) || (n >= 4096 && n <= 4098));
    }
    const auto n = state_->readback_present_index_;
    return FrameReadbackEnabled() && FrameTraceEnabled() && state_->transfer_trace_lines_ < 1024 &&
           ((n >= 511 && n <= 513) || (n >= 4095 && n <= 4097));
  }

  template<typename... Args>
  void TraceTransferAt(CommittingScope &scope, const void *list, const EncoderData *pass, uint64_t frame,
                     const char *op, const char *format, Args... args) {
    const DWORD error = GetLastError();
    const bool full = FullFrameCaptureEnabled();
    const unsigned row = full ? capture_lines.fetch_add(1, std::memory_order_relaxed) + 1 : ++state_->transfer_trace_lines_;
    if (full && row > 40961) { SetLastError(error); return; }
    char detail[2304], line[3072];
    const int detail_size = std::snprintf(detail, sizeof(detail), format, args...);
    if (detail_size < 0 || size_t(detail_size) >= sizeof(detail)) {
      op = "capture.format_limit";
      std::snprintf(detail, sizeof(detail), "truncated=1 reason=detail_size");
    }
    if (full && row == 40961) { op = "capture.limit"; std::snprintf(detail, sizeof(detail), "truncated=1 row_limit=40960"); }
    const int n = std::snprintf(line, sizeof(line),
        "dx12_transfer line=%u queue=%p before_present=%llu seq=%llu list=%p pass=%p id=%llu op=%s %s\n",
        row, state_.get(),
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(scope.seq), list, static_cast<const void *>(pass),
        static_cast<unsigned long long>(pass ? pass->id : 0), op, detail);
    DWORD written;
    if (n > 0 && size_t(n) < sizeof(line))
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
    SetLastError(error);
  }

  template<typename... Args>
  void TraceTransfer(CommittingScope &scope, const void *list, const EncoderData *pass,
                     const char *op, const char *format, Args... args) {
    if (!TransferWindowEnabled()) return;
    const auto frame = FullFrameCaptureEnabled() ? capture_next_present.load(std::memory_order_relaxed) : state_->readback_present_index_ + 1;
    TraceTransferAt(scope, list, pass, frame, op, format, args...);
  }

  static D3D12SubmissionCapture::Context FaultContext(const void *list, const EncoderData *pass) {
    return {reinterpret_cast<std::uintptr_t>(list), reinterpret_cast<std::uintptr_t>(pass),
            pass ? pass->id : 0, pass ? unsigned(pass->type) : 0};
  }

  static void CaptureRenderPass(CommittingScope &scope, const void *list, EncoderData *pass,
                                const WMTRenderPassInfo &info) {
    if (scope.inflight.fault_capture)
      scope.inflight.fault_capture->RenderPass(FaultContext(list, pass), info);
  }

  static void CaptureFaultCommands(CommittingScope &scope, const void *list, EncoderData *pass,
                                   WMTCommandCaptureFamily family, const wmtcmd_base *cmd) {
    if (scope.inflight.fault_capture)
      scope.inflight.fault_capture->Commands(FaultContext(list, pass), family, cmd);
  }

  void TraceTransferCommands(CommittingScope &scope, const void *list, EncoderData *pass, wmtcmd_base *cmd) {
    const auto family = pass->type == EncoderType::Render ? WMTCommandCaptureFamily::Render :
                        pass->type == EncoderType::Compute ? WMTCommandCaptureFamily::Compute :
                        WMTCommandCaptureFamily::Blit;
    CaptureFaultCommands(scope, list, pass, family, cmd);
    if (!TransferWindowEnabled())
      return;
    if (FullFrameCaptureEnabled()) {
      const auto frame = capture_next_present.load(std::memory_order_relaxed);
      CaptureWMTCommands(family, cmd, [&](const char *detail) {
        // Complete an admitted pass even if another queue advances the capture window meanwhile.
        TraceTransferAt(scope, list, pass, frame, "command", "%s", detail);
      });
      return;
    }
    unsigned count = 0;
    // Walk only the already-owned command chain, with a hard bound independent of the log cap.
    for (; cmd && count < 256; cmd = static_cast<wmtcmd_base *>(cmd->next.get()), ++count) {
      if (pass->type == EncoderType::Blit) {
        if (cmd->type == WMTBlitCommandCopyFromTextureToTexture) {
          auto c = reinterpret_cast<wmtcmd_blit_copy_from_texture_to_texture *>(cmd);
          TraceTransfer(scope, list, pass, "copy.region", "src=%llx dst=%llx src_level=%u src_slice=%u dst_level=%u dst_slice=%u size=%llu,%llu,%llu origin=%llu,%llu,%llu",
                        (unsigned long long)c->src, (unsigned long long)c->dst, c->src_level, c->src_slice,
                        c->dst_level, c->dst_slice, (unsigned long long)c->src_size.width,
                        (unsigned long long)c->src_size.height, (unsigned long long)c->src_size.depth,
                        (unsigned long long)c->dst_origin.x, (unsigned long long)c->dst_origin.y,
                        (unsigned long long)c->dst_origin.z);
        } else if (cmd->type == WMTBlitCommandCopyTexture) {
          auto c = reinterpret_cast<wmtcmd_blit_copy_texture *>(cmd);
          TraceTransfer(scope, list, pass, "copy.texture", "src=%llx dst=%llx",
                        (unsigned long long)c->src, (unsigned long long)c->dst);
        }
      } else if (pass->type == EncoderType::Render) {
        if (cmd->type == WMTRenderCommandSetPSO) {
          auto c = reinterpret_cast<wmtcmd_render_setpso *>(cmd);
          TraceTransfer(scope, list, pass, "render.pso", "pso=%llx", (unsigned long long)c->pso);
        } else if (cmd->type == WMTRenderCommandSetFragmentTexture) {
          auto c = reinterpret_cast<wmtcmd_render_settexture *>(cmd);
          TraceTransfer(scope, list, pass, "render.fragment_texture", "texture=%llx slot=%u",
                        (unsigned long long)c->texture, unsigned(c->index));
        } else if (cmd->type == WMTRenderCommandDraw || cmd->type == WMTRenderCommandDrawIndexed) {
          TraceTransfer(scope, list, pass, "render.draw", "type=%u", unsigned(cmd->type));
        }
      }
    }
    TraceTransfer(scope, list, pass, "commands.end", "scanned=%u truncated=%u", count, unsigned(cmd != nullptr));
  }

  void EncodeFrameReadback(CommittingScope &scope, MTLD3D12Resource *resource, TextureView &view) {
    if (!FrameReadbackEnabled() || state_->readback_present_index_ > 8192)
      return;
    const uint64_t frame = ++state_->readback_present_index_;
    if (frame != 1 && frame != 64 && frame != 512 && frame != 4096 && frame != 8192)
      return;
    auto &readback = scope.inflight.readback;
    readback.present_index = frame;
    EncodeTextureReadback(scope, view, readback, resource->texture->textureType(view.key),
                          resource->texture->sampleCount());
  }

  void EncodeRenderReadback(CommittingScope &scope, const RenderEncoderData &render) {
    if (!FrameReadbackEnabled() || state_->readback_render_index_ > 8192)
      return;
    const uint64_t index = ++state_->readback_render_index_;
    if (index != 1 && index != 64 && index != 512 && index != 4096 && index != 8192)
      return;
    auto &readback = scope.inflight.render_readbacks.emplace_back();
    readback.render_index = index;
    const auto &color = render.colors[0];
    // Sample the stored color attachment immediately after its render pass, before later passes can overwrite it.
    if (!color.attachment || color.level || color.slice || color.depth_plane ||
        color.store_action != WMTStoreActionStore) {
      readback.Log(state_.get(), scope.seq, "backbuffer.skip", "reason=unsupported_render_attachment");
      return;
    }
    auto &view = *color.attachment.ptr();
    auto *texture = view.allocation->descriptor;
    EncodeTextureReadback(scope, view, readback, texture->textureType(view.key), texture->sampleCount());
  }

  void EncodeTextureReadback(CommittingScope &scope, TextureView &view, D3D12FrameReadback &readback,
                            WMTTextureType type, unsigned samples) {
    readback.source_texture = view.texture;
    readback.format = view.texture.pixelFormat();
    readback.width = view.texture.width();
    readback.height = view.texture.height();
    const auto skip = [&](const char *reason) { readback.Log(state_.get(), scope.seq, "backbuffer.skip", reason); };
    switch (readback.format) {
    case WMTPixelFormatRGBA8Unorm:
    case WMTPixelFormatRGBA8Unorm_sRGB:
      break;
    case WMTPixelFormatBGRA8Unorm:
    case WMTPixelFormatBGRA8Unorm_sRGB:
      readback.bgra = true;
      break;
    default: {
      char reason[96];
      std::snprintf(reason, sizeof(reason), "reason=unsupported_format metal_pixel_format=0x%08x", unsigned(readback.format));
      skip(reason);
      return;
    }
    }
    if (type != WMTTextureType2D || samples != 1 ||
        view.texture.depth() != 1 || view.texture.arrayLength() != 1 ||
        view.allocation->flags().test(TextureAllocationFlag::AllocatedOnHeap)) {
      skip("reason=unsupported_texture_or_heap_allocation");
      return;
    }
    constexpr uint64_t max_dimension = 16384, max_bytes = 64ull * 1024 * 1024;
    if (!readback.width || !readback.height || readback.width > max_dimension || readback.height > max_dimension) {
      skip("reason=dimensions");
      return;
    }
    const uint64_t pitch = (readback.width * 4 + 255) & ~uint64_t(255);
    if (pitch > UINT32_MAX || readback.height > max_bytes / pitch) {
      skip("reason=readback_size");
      return;
    }
    readback.pitch = uint32_t(pitch);
    WMTBufferInfo info{};
    info.length = pitch * readback.height;
    info.options = WMTResourceStorageModeShared;
    readback.buffer = state_->device_.newBuffer(info);
    readback.data = static_cast<const uint8_t *>(info.memory.get_accessible_or_null());
    if (!readback.buffer || !readback.data) {
      skip("reason=buffer_allocation_or_mapping");
      return;
    }
    std::memset(info.memory.get_accessible_or_null(), D3D12FrameReadback::sentinel, size_t(info.length));
    WMT::Reference<WMT::Error> error;
    readback.residency = state_->device_.newResidencySet(2, error);
    if (!readback.residency) {
      skip("reason=residency_allocation");
      return;
    }
    readback.source_allocation = view.allocation->texture();
    WMT::Allocation allocations[] = {readback.source_allocation, readback.buffer};
    readback.residency.addAllocations(allocations, 2);
    readback.residency.commit();
    // Queue attachment provides execution-time residency; retirement empties the set.
    // At most ten empty sets remain attached for this queue's lifetime (five presents and five render passes).
    state_->queue_.addResidencySet(readback.residency);
    auto encoder = scope.inflight.cmdbuf.blitCommandEncoder();
    if (!encoder) {
      skip("reason=blit_encoder_allocation");
      return;
    }
    encoder.waitForFence(state_->fence_);
    wmtcmd_blit_copy_from_texture_to_buffer copy{};
    copy.type = WMTBlitCommandCopyFromTextureToBuffer;
    copy.src = view.texture;
    copy.size = {uint32_t(readback.width), uint32_t(readback.height), 1};
    copy.dst = readback.buffer;
    copy.bytes_per_row = readback.pitch;
    MTLBlitCommandEncoder_encodeCommands(encoder, reinterpret_cast<const wmtcmd_base *>(&copy));
    encoder.updateFence(state_->fence_);
    encoder.endEncoding();
    readback.copied = true;
  }

  bool SameDevice(ID3D12DeviceChild *object) {
    if (!object) return false;
    Com<ID3D12Device> owner;
    return SUCCEEDED(object->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) &&
           owner.ptr() == static_cast<ID3D12Device *>(device_);
  }

  void RejectMapping() {
    ERR("Tile mappings: unsupported or invalid buffer-only mapping request");
    device_->RemoveDevice();
  }

  template<typename Operation>
  void SubmitMapping(CommittingScope &scope, Operation operation) {
    if (!state_->mapping_queue_) {
      state_->mapping_queue_ = state_->device_.newMappingCommandQueue();
      state_->mapping_event_ = state_->device_.newSharedEvent();
      if (state_->mapping_queue_ && !state_->mapping_queue_.addResidencySet(state_->residency_set_)) {
        state_->mapping_queue_ = {};
        RejectMapping();
        return;
      }
    }
    bool ok = state_->mapping_queue_ && state_->mapping_event_;
    // One attached global set includes reference-counted sparse backing leases.
    // requestResidency alone is not a substitute for execution-time residency.
    if (!ok) { RejectMapping(); return; }
    uint64_t ready = ++state_->mapping_serial_, done = ++state_->mapping_serial_;
    scope.inflight.cmdbuf.encodeSignalEvent(state_->mapping_event_, ready);
    ok = state_->mapping_queue_.waitForEvent(state_->mapping_event_, ready) && operation() &&
         state_->mapping_queue_.signalEvent(state_->mapping_event_, done);
    if (!ok) {
      state_->failed_mapping_resources_ = scope.inflight.sparse_resources;
      state_->failed_mapping_keepalive_ = state_;
      RejectMapping();
      return;
    }
    scope.inflight.cmdbuf.encodeWaitForEvent(state_->mapping_event_, done);
  }

  static bool LinearRegion(const D3D12_TILED_RESOURCE_COORDINATE *start, const D3D12_TILE_REGION_SIZE *size,
                           uint32_t capacity, uint64_t &offset, uint64_t &count) {
    offset = start ? start->X : 0;
    if (start && (start->Y || start->Z || start->Subresource)) return false;
    count = size ? (size->UseBox ? size->Width : size->NumTiles) : capacity;
    if (size && size->UseBox && (size->Height != 1 || size->Depth != 1)) return false;
    return count && offset <= capacity && count <= capacity - offset;
  }

  static void RetainBacking(D3D12SparseBufferState &resource, WMT::Heap heap) {
    for (const auto &old : resource.heaps) if (old.handle == heap.handle) return;
    resource.heaps.emplace_back(heap);
    resource.residency->Retain(heap);
  }

public:
  MTLD3D12CommandQueueImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory>(pDevice) {}

  ~MTLD3D12CommandQueueImpl() {
    if (!inflight_cmdbuf_wait_thread_)
      return;
    std::lock_guard<dxmt::mutex> lock(state_->mutex_commit_);
    state_->inflight_cmdbuf_stop_.store(state_->inflight_cmdbuf_seq_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    state_->inflight_cmdbuf_seq_.fetch_add(1, std::memory_order_release);
    state_->inflight_cmdbuf_seq_.notify_one();
    // A user GPU wait may never signal. The worker drains native state only;
    // final COM release must not join it or keep the public device alive.
    CloseHandle(inflight_cmdbuf_wait_thread_);
  }

  HRESULT
  Initialize(const D3D12_COMMAND_QUEUE_DESC *pDesc) {
    // TODO: validate and normalize
    desc_ = *pDesc;
    desc_.NodeMask = 1; // typically 1 GPU only

    auto metal_device = device_->GetMTLDevice();
    state_->device_ = metal_device;
    state_->residency_set_ = device_->GetGlobalResidencySet();
    state_->queue_ = metal_device.newCommandQueue(kCommandQueueSize);
    if (!state_->queue_)
      return E_FAIL;
    state_->queue_.addResidencySet(state_->residency_set_);

    state_->fence_ = metal_device.newFence();
    state_->immediate_out_fence_ = metal_device.newFence();
    if (!state_->fence_ || !state_->immediate_out_fence_)
      return E_OUTOFMEMORY;
    state_->timestamp_event_ = metal_device.newSharedEvent();
    if (!state_->timestamp_event_)
      return E_OUTOFMEMORY;
    WMTBufferInfo visibility_info{};
    visibility_info.length = sizeof(uint64_t);
    visibility_info.options = WMTResourceStorageModePrivate;
    state_->visibility_scratch_ = metal_device.newBuffer(visibility_info);
    if (!state_->visibility_scratch_)
      return E_OUTOFMEMORY;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&RetireD3D12Queue), &module))
      return HRESULT_FROM_WIN32(GetLastError());
    auto retirement = std::make_unique<D3D12QueueRetirement>(D3D12QueueRetirement{state_, module});
    inflight_cmdbuf_wait_thread_ = CreateThread(nullptr, 0x100000, RetireD3D12Queue, retirement.get(),
                                              STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!inflight_cmdbuf_wait_thread_) {
      const DWORD error = GetLastError();
      FreeLibrary(module);
      return HRESULT_FROM_WIN32(error);
    }
    retirement.release();

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandQueue)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IMTLSwapChainFactory)) {
      *ppvObject = ref_and_cast<IMTLSwapChainFactory>(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) || riid == __uuidof(ID3D11Device2) ||
        riid == __uuidof(ID3D11Device3) || riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5))
      return E_NOINTERFACE;

    if (riid == kD3D12CommandQueueDownlevelUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid)) {
      WARN("D3D12CommandQueue: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE UpdateTileMappings(
      ID3D12Resource *resource, UINT region_count, const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
      const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap, UINT range_count,
      const D3D12_TILE_RANGE_FLAGS *range_flags, const UINT *heap_range_offsets, const UINT *range_tile_counts,
      D3D12_TILE_MAPPING_FLAGS flags
  ) {
    if (FAILED(device_->GetDeviceRemovedReason())) return;
    if (!SameDevice(resource) || !SameDevice(heap) || region_count != 1 || !range_count ||
        !region_start_coordinates || !region_sizes || !heap_range_offsets || !range_tile_counts ||
        flags != D3D12_TILE_MAPPING_FLAG_NONE) { RejectMapping(); return; }
    auto sparse = static_cast<MTLD3D12Resource *>(resource)->sparse;
    auto backing = static_cast<MTLD3D12Heap *>(heap);
    if (!sparse || !backing->placement_sparse_compatible) { RejectMapping(); return; }
    uint64_t offset, count;
    if (!LinearRegion(region_start_coordinates, region_sizes, sparse->tiles, offset, count) || range_count > count) {
      RejectMapping(); return;
    }
    const uint64_t heap_tiles = backing->GetDesc().SizeInBytes / 65536;
    std::vector<WMT4UpdateSparseBufferMappingOperation> operations;
    uint64_t consumed = 0;
    for (UINT i = 0; i < range_count; ++i) {
      const auto mode = range_flags ? range_flags[i] : D3D12_TILE_RANGE_FLAG_NONE;
      const uint64_t n = range_tile_counts ? range_tile_counts[i] : count;
      const uint64_t h = heap_range_offsets ? heap_range_offsets[i] : 0;
      if (!n || consumed > count || n > count - consumed ||
          (mode != D3D12_TILE_RANGE_FLAG_NONE && mode != D3D12_TILE_RANGE_FLAG_SKIP &&
           mode != D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE)) { RejectMapping(); return; }
      if (mode != D3D12_TILE_RANGE_FLAG_SKIP) {
        const uint64_t needed = mode == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE ? 1 : n;
        if (h > heap_tiles || needed > heap_tiles - h) { RejectMapping(); return; }
        if (mode == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) {
          for (uint64_t j = 0; j < n; ++j)
            operations.push_back({WMTSparseTextureMappingModeMap, {offset + consumed + j, 1}, h});
        } else operations.push_back({WMTSparseTextureMappingModeMap, {offset + consumed, n}, h});
      }
      consumed += n;
    }
    if (consumed != count) { RejectMapping(); return; }
    if (operations.empty()) return;
    auto scope = StartCommitting();
    std::lock_guard lock(sparse->mutex);
    RetainBacking(*sparse, backing->heap);
    scope.inflight.sparse_resources.push_back(sparse);
    SubmitMapping(scope, [&] {
      return state_->mapping_queue_.updateBufferMappings(sparse->buffer, backing->heap, operations.data(), operations.size());
    });
  };

  void STDMETHODCALLTYPE CopyTileMappings(
      ID3D12Resource *dst_resource, const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
      ID3D12Resource *src_resource, const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags
  ) {
    if (FAILED(device_->GetDeviceRemovedReason())) return;
    if (!SameDevice(dst_resource) || !SameDevice(src_resource) || !dst_region_start_coordinate ||
        !src_region_start_coordinate || !region_size || flags != D3D12_TILE_MAPPING_FLAG_NONE) {
      RejectMapping(); return;
    }
    auto dst = static_cast<MTLD3D12Resource *>(dst_resource)->sparse;
    auto src = static_cast<MTLD3D12Resource *>(src_resource)->sparse;
    uint64_t dst_offset, src_offset, dst_count, src_count;
    if (!dst || !src || dst == src ||
        !LinearRegion(dst_region_start_coordinate, region_size, dst->tiles, dst_offset, dst_count) ||
        !LinearRegion(src_region_start_coordinate, region_size, src->tiles, src_offset, src_count) || dst_count != src_count) {
      RejectMapping(); return;
    }
    auto scope = StartCommitting();
    std::scoped_lock lock(dst->mutex, src->mutex);
    for (auto &heap : src->heaps) RetainBacking(*dst, heap);
    scope.inflight.sparse_resources = {src, dst};
    WMT4CopySparseBufferMappingOperation operation{{src_offset, src_count}, dst_offset};
    SubmitMapping(scope, [&] {
      return state_->mapping_queue_.copyBufferMappings(src->buffer, dst->buffer, &operation, 1);
    });
  };

  void STDMETHODCALLTYPE
  ExecuteCommandLists(UINT Count, ID3D12CommandList *const *ppCommandLists) {
    static std::atomic<uint32_t> trace_enter{0}, trace_accepted{0}, trace_rejected{0};
    TraceFrame(trace_enter, "execute.enter", this, "type=%u count=%u state=%p", unsigned(desc_.Type), Count,
               static_cast<const void *>(state_.get()));
    for (UINT i = 0; i < Count; ++i) {
      auto list = ppCommandLists ? static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]) : nullptr;
      if (!list || FAILED(list->close_result) || list->encoder_count == std::numeric_limits<size_t>::max()) {
        ERR("ExecuteCommandLists: invalid or unclosed command list");
        TraceFrame(trace_rejected, "execute.rejected", this,
                   "type=%u count=%u index=%u array=%p list=%p reason=%s close_hr=%08x encoders=%llu tid=%lu",
                   unsigned(desc_.Type), Count, i, static_cast<const void *>(ppCommandLists),
                   static_cast<const void *>(list), !ppCommandLists ? "null_array" : !list ? "null_entry" :
                   FAILED(list->close_result) ? "Close_failed" : "not_closed",
                   unsigned(list ? list->close_result : E_POINTER),
                   static_cast<unsigned long long>(list ? list->encoder_count : 0),
                   static_cast<unsigned long>(GetCurrentThreadId()));
        device_->RemoveDevice();
        return;
      }
    }
    TraceFrame(trace_accepted, "execute.accepted", this, "type=%u count=%u state=%p", unsigned(desc_.Type), Count,
               static_cast<const void *>(state_.get()));
    auto scope = StartCommitting();
    D3D12SubmissionScenes scenes;
    uint64_t acceleration_bytes=0;
    // Prepare every occurrence separately, including the same list submitted twice.
    // No COM-bearing recording survives into the native retirement packet.
    try {
      for (UINT i = 0; i < Count; ++i) {
        auto list = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
        for (auto pass = list->entry; pass; pass = pass->next) {
          if (pass->type==EncoderType::AccelerationBuild) {
            D3D12NativeAccelerationBuild native;
            const auto hr=PrepareAccelerationBuild(static_cast<AccelerationBuildEncoderData *>(pass)->build,
                                                  scenes,acceleration_bytes,native);
            if (FAILED(hr)) {
              scope.discard=true;
              ERR("ExecuteCommandLists: scene build preparation failed, hr=",str::format(hr));
              device_->RemoveDevice();return;
            }
            scope.inflight.acceleration_builds.push_back(std::move(native));
            continue;
          }
          if (pass->type != EncoderType::ArgumentCompute) continue;
          const auto &record = static_cast<ArgumentComputeEncoderData *>(pass)->dispatch;
          D3D12NativeComputeDispatch native;
          const HRESULT hr = record ? PrepareArgumentDispatch(*record, scenes, native) : E_INVALIDARG;
          if (FAILED(hr)) {
            scope.discard = true;
            ERR("ExecuteCommandLists: argument preparation failed, hr=", str::format(hr));
            device_->RemoveDevice();
            return;
          }
          scope.inflight.argument_dispatches.push_back(std::move(native));
        }
      }
    } catch (const std::bad_alloc &) {
      scope.discard = true;
      ERR("ExecuteCommandLists: argument preparation exhausted memory");
      device_->RemoveDevice();
      return;
    }
    size_t argument_index = 0, acceleration_index=0;
    auto &cmdbuf = scope.inflight.cmdbuf;
    for (unsigned i = 0; i < Count; i++) {
      auto pCommandList = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
      if (FullFrameCaptureEnabled() && TransferWindowEnabled()) {
        const auto frame = capture_next_present.load(std::memory_order_relaxed);
        const auto &recording = pCommandList->GetRecordingTrace();
        TraceTransferAt(scope, pCommandList, nullptr, frame, "recording.begin", "epoch=%llu count=%llu dropped=%llu",
                        (unsigned long long)pCommandList->GetRecordingTraceEpoch(), (unsigned long long)recording.size(),
                        (unsigned long long)pCommandList->GetRecordingTraceDropped());
        for (const auto &detail : recording)
          TraceTransferAt(scope, pCommandList, nullptr, frame, "recording.call", "%s", detail.c_str());
        TraceTransferAt(scope, pCommandList, nullptr, frame, "recording.end", "epoch=%llu count=%llu truncated=%u",
                        (unsigned long long)pCommandList->GetRecordingTraceEpoch(), (unsigned long long)recording.size(),
                        unsigned(pCommandList->GetRecordingTraceDropped() != 0));
      }
      EncoderData *current = pCommandList->entry;
      while (current) {
        if (scope.inflight.fault_capture)
          scope.inflight.fault_capture->Add(FaultContext(pCommandList, current), "pass.begin", nullptr, 0);
        TraceTransfer(scope, pCommandList, current, "pass.dispatch", "type=%u", unsigned(current->type));
        switch (current->type) {
        case EncoderType::Null:
          break;
        case EncoderType::Clear: {
          auto data = static_cast<ClearEncoderData *>(current);
          TraceTransfer(scope, pCommandList, current, "clear.target", "texture=%llx dsv=%u size=%u,%u",
                        (unsigned long long)data->attachment.texture().handle, data->clear_dsv, data->width, data->height);
          {
            WMTRenderPassInfo info;
            WMT::InitializeRenderPassInfo(info);
            if (data->clear_dsv) {
              if (data->clear_dsv & 1) {
                info.depth.clear_depth = data->depth_stencil.first;
                info.depth.texture = data->attachment.texture();
                info.depth.load_action = WMTLoadActionClear;
                info.depth.store_action = WMTStoreActionStore;
                info.depth.depth_plane = data->depth_plane;
              }
              if (data->clear_dsv & 2) {
                info.stencil.clear_stencil = data->depth_stencil.second;
                info.stencil.texture = data->attachment.texture();
                info.stencil.load_action = WMTLoadActionClear;
                info.stencil.store_action = WMTStoreActionStore;
                info.stencil.depth_plane = data->depth_plane;
              }
              info.render_target_width = data->width;
              info.render_target_height = data->height;
            } else {
              info.colors[0].clear_color = data->color;
              info.colors[0].texture = data->attachment.texture();
              info.colors[0].load_action = WMTLoadActionClear;
              info.colors[0].store_action = WMTStoreActionStore;
              info.colors[0].depth_plane = data->depth_plane;
            }
            info.render_target_array_length = data->array_length;
            CaptureRenderPass(scope, pCommandList, current, info);
            auto encoder = cmdbuf.renderCommandEncoder(info);
            encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
            encoder.waitForFence(state_->fence_, WMTRenderStageFragment);
            encoder.updateFence(state_->fence_, WMTRenderStageFragment);
            encoder.endEncoding();
          }
          break;
        }
        case EncoderType::Render: {
          auto data = static_cast<RenderEncoderData *>(current);
          WMTRenderPassInfo render_pass_info;
          WMT::InitializeRenderPassInfo(render_pass_info);
          if (!data->visibility_targets.empty()) {
            wmtcmd_blit_nop head{};
            wmtcmd_blit_fillbuffer clear{};
            head.type = WMTBlitCommandNop;
            head.next.set(&clear);
            clear.type = WMTBlitCommandFillBuffer;
            clear.buffer = state_->visibility_scratch_;
            clear.length = sizeof(uint64_t);
            auto encoder = cmdbuf.blitCommandEncoder();
            encoder.waitForFence(state_->fence_);
            CaptureFaultCommands(scope, pCommandList, current, WMTCommandCaptureFamily::Blit,
                                 reinterpret_cast<wmtcmd_base *>(&head));
            encoder.encodeCommands(&head);
            encoder.updateFence(state_->fence_);
            encoder.endEncoding();
            render_pass_info.visibility_buffer = state_->visibility_scratch_;
          }
          {
            for (unsigned i = 0; i < std::size(render_pass_info.colors); i++) {
              auto &color_data = data->colors[i];
              if (!color_data.attachment)
                continue;
              auto &color_info = render_pass_info.colors[i];
              color_info.texture = color_data.attachment.texture();
              color_info.load_action = color_data.load_action;
              color_info.store_action = color_data.store_action;
              color_info.level = color_data.level;
              color_info.slice = color_data.slice;
              color_info.depth_plane = color_data.depth_plane;
              color_info.clear_color = color_data.clear_color;
              color_info.resolve_texture = color_data.resolve_attachment.texture();
              color_info.resolve_level = color_data.resolve_level;
              color_info.resolve_slice = color_data.resolve_slice;
              color_info.resolve_depth_plane = color_data.resolve_depth_plane;
              TraceTransfer(scope, pCommandList, current, "render.target", "slot=%u texture=%llx size=%u,%u load=%u store=%u",
                            i, (unsigned long long)color_data.attachment.texture().handle,
                            data->render_target_width, data->render_target_height,
                            unsigned(color_data.load_action), unsigned(color_data.store_action));
            }
            if (data->depth.attachment) {
              auto &depth_info = render_pass_info.depth;
              auto &depth_data = data->depth;
              depth_info.texture = depth_data.attachment.texture();
              depth_info.load_action = depth_data.load_action;
              depth_info.store_action = depth_data.store_action;
              depth_info.level = depth_data.level;
              depth_info.slice = depth_data.slice;
              depth_info.depth_plane = depth_data.depth_plane;
              depth_info.clear_depth = depth_data.clear_depth;
            }
            if (data->stencil.attachment) {
              auto &stencil_info = render_pass_info.stencil;
              auto &stencil_data = data->stencil;
              stencil_info.texture = stencil_data.attachment.texture();
              stencil_info.load_action = stencil_data.load_action;
              stencil_info.store_action = stencil_data.store_action;
              stencil_info.level = stencil_data.level;
              stencil_info.slice = stencil_data.slice;
              stencil_info.depth_plane = stencil_data.depth_plane;
              stencil_info.clear_stencil = stencil_data.clear_stencil;
            }
            render_pass_info.default_raster_sample_count = data->default_raster_sample_count;
            render_pass_info.render_target_array_length = data->render_target_array_length;
            render_pass_info.render_target_width = data->render_target_width;
            render_pass_info.render_target_height = data->render_target_height;
          }
          CaptureRenderPass(scope, pCommandList, current, render_pass_info);
          auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
          encoder.waitForFence(state_->fence_, WMTRenderStageVertex);
          TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(state_->fence_, WMTRenderStageFragment);
          encoder.endEncoding();
          EncodeRenderReadback(scope, *data);
          if (!data->visibility_targets.empty()) {
            // All samples in this pass belong to the same active query set.
            // The scratch is queue-local and cannot be reused until reduction finishes.
            auto reduction = cmdbuf.computeCommandEncoder(false);
            reduction.waitForFence(state_->fence_);
            for (const auto &target : data->visibility_targets) {
              wmtcmd_compute_nop head{};
              wmtcmd_compute_setpso pipeline{};
              wmtcmd_compute_setbuffer src{}, dst{};
              wmtcmd_compute_dispatch dispatch{};
              head.type = WMTComputeCommandNop;
              head.next.set(&pipeline);
              pipeline.type = WMTComputeCommandSetPSO;
              pipeline.next.set(&src);
              pipeline.pso = data->visibility_accumulate;
              pipeline.threadgroup_size = {1, 1, 1};
              src.type = WMTComputeCommandSetBuffer;
              src.next.set(&dst);
              src.buffer = state_->visibility_scratch_;
              src.index = 0;
              dst.type = WMTComputeCommandSetBuffer;
              dst.next.set(&dispatch);
              dst.buffer = target.results;
              dst.offset = target.offset;
              dst.index = 1;
              dispatch.type = WMTComputeCommandDispatch;
              dispatch.size = {1, 1, 1};
              CaptureFaultCommands(scope, pCommandList, current, WMTCommandCaptureFamily::Compute,
                                   reinterpret_cast<wmtcmd_base *>(&head));
              reduction.encodeCommands(&head);
            }
            reduction.updateFence(state_->fence_);
            reduction.endEncoding();
          }
          break;
        }
        case EncoderType::Blit: {
          auto data = static_cast<BlitEncoderData *>(current);
          if (data->barrier_only && !state_->immediate_out_pending_) {
            TraceTransfer(scope, pCommandList, current, "blit.skip", "barrier_only=%u", 1u);
            break;
          }
          // Counter samples use queue-wide ordering. Completing prior work
          // also satisfies the stronger-than-required MARKER_IN barrier.
          const auto timestamp_barrier = [&] {
            const auto value = ++state_->timestamp_serial_;
            cmdbuf.encodeSignalEvent(state_->timestamp_event_, value);
            cmdbuf.encodeWaitForEvent(state_->timestamp_event_, value);
          };
          if (data->timestamp_samples || data->wait_for_prior_work)
            timestamp_barrier();
          WMTSampleBufferAttachmentInfo sample{};
          sample.sample_buffer = data->timestamp_samples;
          sample.start_of_encoder_sample_index = ~0ull;
          sample.end_of_encoder_sample_index = data->timestamp_index;
          auto encoder = data->timestamp_samples ? cmdbuf.blitCommandEncoderWithSampleBuffers(&sample, 1)
                                                : cmdbuf.blitCommandEncoder();
          encoder.waitForFence(state_->fence_);
          if (data->join_immediate_out && state_->immediate_out_pending_)
            encoder.waitForFence(state_->immediate_out_fence_);
          TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
          encoder.encodeCommands(&data->cmd_head);
          if (data->resolve_samples) {
            // Metal resolve offsets have stricter alignment than D3D12's 8 bytes.
            // Allocate per execution so concurrent replays never share scratch.
            WMTBufferInfo info{};
            info.length = uint64_t(data->resolve_count) * sizeof(uint64_t);
            info.options = WMTResourceStorageModePrivate;
            auto scratch = state_->device_.newBuffer(info);
            if (!scratch) {
              encoder.endEncoding();
              device_->RemoveDevice();
              return;
            }
            scope.inflight.timestamp_scratch.push_back(scratch);
            encoder.resolveCounters(data->resolve_samples, data->resolve_start, data->resolve_count, scratch, 0);
            wmtcmd_blit_copy_from_buffer_to_buffer copy{};
            copy.type = WMTBlitCommandCopyFromBufferToBuffer;
            copy.next.set(nullptr);
            copy.src = scratch;
            copy.dst = data->resolve_destination;
            copy.dst_offset = data->resolve_offset;
            copy.copy_length = info.length;
            CaptureFaultCommands(scope, pCommandList, current, WMTCommandCaptureFamily::Blit,
                                 reinterpret_cast<wmtcmd_base *>(&copy));
            MTLBlitCommandEncoder_encodeCommands(encoder, reinterpret_cast<wmtcmd_base *>(&copy));
          }
          if (data->immediate_out) {
            // Do not put OUT on the common fence: independent later passes
            // must not acquire a dependency on this deferred write.
            encoder.updateFence(state_->immediate_out_fence_);
            state_->immediate_out_pending_ = true;
          } else {
            encoder.updateFence(state_->fence_);
            if (data->join_immediate_out)
              state_->immediate_out_pending_ = false;
          }
          encoder.endEncoding();
          if (data->timestamp_samples)
            timestamp_barrier();
          break;
        }
        case EncoderType::Compute: {
          auto data = static_cast<ComputeEncoderData *>(current);
          auto encoder = cmdbuf.computeCommandEncoder(false);
          encoder.waitForFence(state_->fence_);
          TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(state_->fence_);
          encoder.endEncoding();
          break;
        }
        case EncoderType::AccelerationBuild: {
          auto &build=scope.inflight.acceleration_builds[acceleration_index++];
          const auto status=cmdbuf.buildAccelerationStructure(build.Descriptor(),build.result->Native().resource,
              build.scratch->buffer,0,state_->fence_);
          if (status!=WMTArgumentStatusReady) {
            scope.discard=true;
            ERR("ExecuteCommandLists: scene build encoding failed");
            device_->RemoveDevice();return;
          }
          break;
        }
        case EncoderType::ArgumentCompute: {
          auto &dispatch = scope.inflight.argument_dispatches[argument_index++];
          wmtcmd_compute_nop head{};
          head.type = WMTComputeCommandNop;
          wmtcmd_compute_setpso pipeline{};
          head.next.set(&pipeline);
          pipeline.type = WMTComputeCommandSetPSO;
          pipeline.pso = dispatch.pipeline;
          pipeline.threadgroup_size = dispatch.threads;
          std::array<wmtcmd_compute_setbuffer, 31> buffers{};
          std::array<wmtcmd_compute_settexture, 128> textures{};
          std::array<wmtcmd_compute_setaccelerationstructure, 31> scenes{};
          size_t buffer_count = 0, texture_count = 0, scene_count = 0;
          for (size_t i = 0; i < dispatch.buffers.size(); ++i) {
            auto &b = buffers[buffer_count++];
            b.type = WMTComputeCommandSetBuffer;
            b.buffer = dispatch.buffers[i]->buffer;
            b.index = uint8_t(dispatch.indices[i]);
          }
          for (const auto &binding : dispatch.direct_bindings) {
            if (binding.kind == WMTArgumentKindBuffer) {
              auto &b = buffers[buffer_count++];
              b.type = WMTComputeCommandSetBuffer;
              b.buffer = binding.resource;
              b.offset = binding.offset;
              b.index = uint8_t(binding.index);
            } else if (metal_argument::Acceleration(binding.kind)) {
              auto &a = scenes[scene_count++];
              a.type = WMTComputeCommandSetAccelerationStructure;
              a.acceleration_structure = binding.resource;
              a.index = uint8_t(binding.index);
            } else {
              auto &t = textures[texture_count++];
              t.type = WMTComputeCommandSetTexture;
              t.texture = binding.resource;
              t.index = uint8_t(binding.index);
            }
          }
          wmtcmd_compute_dispatch launch{};
          launch.type = WMTComputeCommandDispatch;
          launch.size = dispatch.groups;
          compute_commands::Link(pipeline.next, dispatch.resource_uses,
              std::span(buffers.data(), buffer_count), std::span(textures.data(), texture_count), launch,
              std::span(scenes.data(), scene_count));
          auto encoder = cmdbuf.computeCommandEncoder(false);
          encoder.waitForFence(state_->fence_);
          CaptureFaultCommands(scope, pCommandList, current, WMTCommandCaptureFamily::Compute,
                               reinterpret_cast<wmtcmd_base *>(&head));
          encoder.encodeCommands(&head);
          encoder.updateFence(state_->fence_);
          encoder.endEncoding();
          break;
        }
        case EncoderType::Resolve: {
          auto data = static_cast<ResolveEncoderData *>(current);
          TraceTransfer(scope, pCommandList, current, "resolve", "src=%llx dst=%llx",
                        (unsigned long long)data->src.texture().handle, (unsigned long long)data->dst.texture().handle);

          WMTRenderPassInfo info;
          WMT::InitializeRenderPassInfo(info);
          info.colors[0].texture = data->src.texture();
          info.colors[0].load_action = WMTLoadActionLoad;
          info.colors[0].store_action = WMTStoreActionStoreAndMultisampleResolve;
          info.colors[0].resolve_texture = data->dst.texture();

          CaptureRenderPass(scope, pCommandList, current, info);
          auto encoder = cmdbuf.renderCommandEncoder(info);
          encoder.waitForFence(state_->fence_, WMTRenderStageFragment);
          encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
          encoder.updateFence(state_->fence_, WMTRenderStageFragment);
          encoder.endEncoding();

          break;
        }
        }
        current = current->next;
      }
    }
    if (state_->immediate_out_pending_) {
      // Separate ExecuteCommandLists calls are ordered even when the next
      // submission relies on buffer decay/promotion instead of a barrier.
      auto encoder = cmdbuf.blitCommandEncoder();
      encoder.waitForFence(state_->fence_);
      encoder.waitForFence(state_->immediate_out_fence_);
      encoder.updateFence(state_->fence_);
      encoder.endEncoding();
      state_->immediate_out_pending_ = false;
    }
  };

  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE EndEvent() {};

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D12Fence *pFence, UINT64 Value) {
    if (!pFence)
      return E_INVALIDARG;
    const HRESULT hr = device_->GetDeviceRemovedReason();
    if (FAILED(hr))
      return hr;
    auto scope = StartCommitting();
    auto &cmdbuf = scope.inflight.cmdbuf;
    scope.inflight.retained_fence = static_cast<MTLD3D12Fence *>(pFence)->GetRemovalState();
    return static_cast<MTLD3D12Fence *>(pFence)->SignalQueue(cmdbuf, Value);
  };

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D12Fence *pFence, UINT64 Value) {
    if (!pFence)
      return E_INVALIDARG;
    const HRESULT hr = device_->GetDeviceRemovedReason();
    if (FAILED(hr))
      return hr;
    auto scope = StartCommitting();
    auto &cmdbuf = scope.inflight.cmdbuf;
    scope.inflight.retained_fence = static_cast<MTLD3D12Fence *>(pFence)->GetRemovalState();
    return static_cast<MTLD3D12Fence *>(pFence)->WaitQueue(cmdbuf, Value);
  };

  HRESULT STDMETHODCALLTYPE
  GetTimestampFrequency(UINT64 *pFrequency) {
    if (!pFrequency)
      return E_INVALIDARG;
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return E_FAIL;
    // Apple GPU timestamp scale; validated by the native clock/readback probe.
    *pFrequency = 1000000000ull;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) {
    return E_NOTIMPL;
  };

  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain
  ) {
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain);
  }

  HRESULT
  Present(const std::shared_ptr<D3D12PresentState> &present, ID3D12Resource *backbuffer, double after) override {
    static std::atomic<uint32_t> trace_enter{0}, trace_encoded{0};
    TraceFrame(trace_enter, "queue_present.enter", this, "type=%u state=%p backbuffer=%p after=%.6f",
               unsigned(desc_.Type), static_cast<const void *>(state_.get()), static_cast<const void *>(backbuffer), after);
    auto scope = StartCommitting();
    auto &cmdbuf = scope.inflight.cmdbuf;

    auto g = reinterpret_cast<MTLD3D12Resource *>(backbuffer);
    auto &view = g->texture->view(g->texture->fullView);
    static std::atomic<uint32_t> trace_texture{0};
    TraceFrame(trace_texture, "present.texture", this, "texture=%llx resource=%p",
               static_cast<unsigned long long>(view.texture.handle), static_cast<const void *>(backbuffer));
    scope.inflight.present = present;
    scope.inflight.present_texture = view.texture;
    TraceTransfer(scope, nullptr, nullptr, "present", "texture=%llx size=%llu,%llu",
                  (unsigned long long)view.texture.handle, (unsigned long long)view.texture.width(),
                  (unsigned long long)view.texture.height());
    if (FullFrameCaptureEnabled()) capture_next_present.fetch_add(1, std::memory_order_relaxed);
    EncodeFrameReadback(scope, g, view);
    auto presenter = present->presenter.ptr();

    auto state = presenter->synchronizeLayerProperties();
    auto drawable = presenter->encodeCommands(
        cmdbuf, view.texture, state.metadata,
        [&](auto encoder) { encoder.waitForFence(state_->fence_, WMTRenderStageFragment); },
        [&](auto encoder) { encoder.updateFence(state_->fence_, WMTRenderStageFragment); }
    );

    if (after > 0)
      cmdbuf.presentDrawableAfterMinimumDuration(drawable, after);
    else
      cmdbuf.presentDrawable(drawable);
    // Commit occurs in scope's destructor; Present1 logs the result after that return.
    TraceFrame(trace_encoded, "queue_present.encoded", this, "seq=%llu hr=%08x",
               static_cast<unsigned long long>(scope.seq), unsigned(S_OK));
    return S_OK;
  }
};

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
  auto command_queue = Com(new MTLD3D12CommandQueueImpl(pDevice));
  HRESULT hr = command_queue->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  return command_queue->QueryInterface(riid, ppCommandQueue);
};

} // namespace dxmt
