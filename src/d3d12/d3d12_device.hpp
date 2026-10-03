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

#pragma once
#include "d3d12_acceleration_capture.hpp"
#include "d3d12_acceleration_build.hpp"
#include "d3d12.h"
#include "d3d12_diagnostic_counters.hpp"
#include "d3d12_command_encoder.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_local_root_layout.hpp"
#include "d3d12_argument_access.hpp"
#include "d3d12_argument_upload.hpp"
#include "d3d12_metal_argument.hpp"
#include "d3d12_compiler_resource.hpp"
#include "d3d12_capture_ownership.hpp"
#include "d3d12_recorded_binding.hpp"
#include "dxmt_sampler.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_raytracing_bindings.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "dxgi1_2.h"
#include "dxgi_interfaces.h"
#include "airconv_public.h"
#include "dxmt_buffer.hpp"
#include "dxmt_command.hpp"
#include "dxmt_fence.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_texture.hpp"
#include "log/log.hpp"
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

#define IMPLEMENT_ME                                                                                                   \
  do {                                                                                                                 \
    Logger::err(str::format(__FILE__, ":", __FUNCTION__, "(", __LINE__, ") is not implemented."));                     \
    abort();                                                                                                           \
    __builtin_unreachable();                                                                                           \
  } while (0);

namespace dxmt {

class MTLD3D12Device;
class D3D12CompiledComputeProgram;

inline constexpr D3D_FEATURE_LEVEL kD3D12ExperimentalFeatureLevel = D3D_FEATURE_LEVEL_11_0;

struct D3D12DescriptorCapture;
struct D3D12RecordedDescriptorBinding;
struct D3D12AccelerationBuildInput;
struct D3D12AccelerationBuildPlan;
class MTLD3D12RootSignature;

class MTLD3D12GraphicsCommandList : public ID3D12GraphicsCommandList2 {
public:
  EncoderData *entry;
  size_t encoder_count;
  HRESULT close_result = E_FAIL;
  virtual const std::vector<std::string> &GetRecordingTrace() const = 0;
  virtual uint64_t GetRecordingTraceEpoch() const = 0;
  virtual uint64_t GetRecordingTraceDropped() const = 0;
  virtual HRESULT RecordBoundDescriptor(bool compute, const ray_binding::Binding &binding, const ray_library::Resource &required, D3D12RecordedDescriptorBinding &out) = 0;
  virtual HRESULT RecordRayArgument(const ray_binding::Binding &binding, const ray_library::Resource &required, UINT element,
      MTLD3D12RootSignature *local, const void *arguments, size_t argument_bytes, uint64_t buffer_bytes, D3D12RecordedDescriptorBinding &out) = 0;
  virtual HRESULT RecordCompilerRayArgument(const ray_binding::Binding &binding, const compiler_resource::Footprint &footprint,
      UINT element, MTLD3D12RootSignature *local, const void *arguments, size_t argument_bytes,
      uint64_t buffer_bytes, D3D12RecordedDescriptorBinding &out) = 0;
  virtual HRESULT RecordAccelerationStructureBuild(const D3D12AccelerationBuildInput &input,
      D3D12RecordedDescriptorBinding &out) = 0;
  virtual HRESULT RecordCompiledComputeDispatch(const std::shared_ptr<const D3D12CompiledComputeProgram> &program,
      const metal_argument::Plan &arguments, const std::vector<D3D12RecordedDescriptorBinding> &bindings,
      UINT x, UINT y, UINT z) = 0;
  virtual HRESULT RecordMetalArgumentDispatch(const metal_argument::Plan &arguments,
      const std::vector<D3D12RecordedDescriptorBinding> &bindings, UINT x, UINT y, UINT z) = 0;
  virtual HRESULT RecordArgumentDispatch(const argument_upload::Plan &arguments,
      const std::vector<D3D12RecordedDescriptorBinding> &bindings, UINT x, UINT y, UINT z) = 0;
  virtual HRESULT CaptureBoundDescriptor(bool compute, UINT parameter, UINT offset, D3D12DescriptorCapture &out) = 0;
  virtual HRESULT CaptureRootBuffer(bool compute, UINT parameter, uint64_t bytes, D3D12DescriptorCapture &out) = 0;
};

class MTLD3D12CommandAllocator : public ID3D12CommandAllocator {
public:
  virtual HRESULT STDMETHODCALLTYPE CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
      void **ppCommandList
  ) = 0;
};

// Native-only ownership shared by the swap chain and its in-flight presents.
struct D3D12PresentState {
  WMT::Reference<WMT::Device> device;
  InternalCommandLibrary library;
  WMT::Object native_view;
  WMT::MetalLayer layer;
  Rc<Presenter> presenter;
  HANDLE semaphore = nullptr;

  D3D12PresentState(WMT::Device device, HWND window, float scale_factor, uint8_t sample_count, UINT frame_latency);
  ~D3D12PresentState();
  D3D12PresentState(const D3D12PresentState &) = delete;
  D3D12PresentState &operator=(const D3D12PresentState &) = delete;
};

class MTLD3D12CommandQueue : public ID3D12CommandQueue {
public:
  virtual HRESULT Present(const std::shared_ptr<D3D12PresentState> &present, ID3D12Resource *backbuffer, double after) = 0;
};

struct D3D12ResidencyState {
  WMT::Reference<WMT::ResidencySet> set;
  dxmt::mutex mutex;
  std::unordered_map<obj_handle_t, uint32_t> references;
  void Retain(WMT::Allocation allocation) {
    std::lock_guard lock(mutex);
    diagnostic::Add(diagnostic::Counter::ResidencyRetain);
    if (++references[allocation.handle] == 1) {
      set.addAllocations(&allocation, 1);
      diagnostic::Add(diagnostic::Counter::ResidencyAdd);
      set.commit();
      diagnostic::Add(diagnostic::Counter::ResidencyCommit);
    }
  }
  void Release(WMT::Allocation allocation) {
    std::lock_guard lock(mutex);
    diagnostic::Add(diagnostic::Counter::ResidencyRelease);
    auto entry = references.find(allocation.handle);
    if (entry != references.end() && !--entry->second) {
      set.removeAllocations(&allocation, 1);
      diagnostic::Add(diagnostic::Counter::ResidencyRemove);
      set.commit();
      diagnostic::Add(diagnostic::Counter::ResidencyCommit);
      references.erase(entry);
    }
  }
};

// Native-only allocation ownership can outlive all public Wine/COM objects.
struct D3D12NativeResourceCapture {
  Rc<Buffer> buffer;
  Rc<Texture> texture;
  Rc<BufferAllocation> buffer_allocation;
  Rc<TextureAllocation> texture_allocation;
  uint64_t logical_length = 0;
  capture::ResidencyLease<D3D12ResidencyState, WMT::Allocation> residency;

  D3D12NativeResourceCapture(Rc<Buffer> value, uint64_t length, std::shared_ptr<D3D12ResidencyState> owner) :
      buffer(std::move(value)), buffer_allocation(buffer->current()), logical_length(length),
      residency(std::move(owner), buffer_allocation->buffer()) {}
  D3D12NativeResourceCapture(Rc<Texture> value, std::shared_ptr<D3D12ResidencyState> owner) :
      texture(std::move(value)), texture_allocation(texture->current()),
      residency(std::move(owner), texture_allocation->texture()) {}
  const void *Identity() const { return buffer ? static_cast<const void *>(buffer.ptr()) : texture.ptr(); }
  bool BufferRange(uint64_t address, uint64_t bytes, uint64_t &offset) const {
    offset = 0;
    if (!buffer || !buffer_allocation || !buffer_allocation->buffer() || !buffer_allocation->gpuAddress() || buffer->current() != buffer_allocation.ptr() ||
        !bytes || address < buffer_allocation->gpuAddress()) return false;
    const uint64_t delta = address - buffer_allocation->gpuAddress();
    if (!capture::Span(logical_length, delta, bytes)) return false;
    offset = delta;
    return true;
  }
};

// Admission and public device ownership stay on the recording side.
struct D3D12ResourceCaptureSource {
  std::atomic<bool> live{true};
  Com<ID3D12Device> device;
  std::shared_ptr<D3D12NativeResourceCapture> native;
  D3D12ResourceCaptureSource(ID3D12Device *device, Rc<Buffer> value, uint64_t length, std::shared_ptr<D3D12ResidencyState> owner) :
      device(device), native(std::make_shared<D3D12NativeResourceCapture>(std::move(value), length, std::move(owner))) {}
  D3D12ResourceCaptureSource(ID3D12Device *device, Rc<Texture> value, std::shared_ptr<D3D12ResidencyState> owner) :
      device(device), native(std::make_shared<D3D12NativeResourceCapture>(std::move(value), std::move(owner))) {}
  bool Live() const { return live.load(std::memory_order_acquire); }
  const void *Identity() const { return native->Identity(); }
  bool BufferRange(uint64_t address, uint64_t bytes, uint64_t &offset) const {
    return native->BufferRange(address, bytes, offset);
  }
};

struct D3D12NativeAccelerationAllocation {
  WMT::Reference<WMT::AccelerationStructure> resource;
  capture::ResidencyLease<D3D12ResidencyState, WMT::Allocation> residency;
  D3D12NativeAccelerationAllocation(WMT::Reference<WMT::AccelerationStructure> value,
                                   std::shared_ptr<D3D12ResidencyState> owner)
      : resource(std::move(value)), residency(std::move(owner), resource) {}
};
using D3D12NativeAccelerationCapture = capture::AccelerationTree<D3D12NativeAccelerationAllocation>;

// Contains no public COM heap/device/source. A consumer must upload words into
// its own per-submit argument storage; it must not reuse the mutable heap pointer.
struct D3D12NativeDescriptorCapture {
  D3D12NativeAccelerationCapture::Ptr acceleration;
  ShaderVisibleDescriptorType type = ShaderVisibleDescriptorType::Null;
  resource_shape::View shape;
  std::array<uint64_t, 4> words{};
  std::shared_ptr<D3D12NativeResourceCapture> resource, counter;
  TextureViewRef texture;
  WMT::Reference<WMT::Texture> texel;
  Rc<Sampler> sampler;
  uint64_t buffer_offset = 0, byte_length = 0, counter_offset = 0;
};

struct D3D12DescriptorCapture {
  Com<ID3D12DescriptorHeap> heap;
  ShaderVisibleDescriptorType type = ShaderVisibleDescriptorType::Null;
  resource_shape::View shape;
  std::array<uint64_t, 4> words{};
  std::shared_ptr<D3D12ResourceCaptureSource> resource, counter;
  TextureViewRef texture;
  WMT::Reference<WMT::Texture> texel;
  Rc<Sampler> sampler;
  uint64_t buffer_offset = 0, byte_length = 0, counter_offset = 0;
};


inline HRESULT CaptureResult(capture::Status status) {
  switch (status) {
  case capture::Status::Ready: return S_OK;
  case capture::Status::OutOfMemory: return E_OUTOFMEMORY;
  case capture::Status::Unsupported:
  case capture::Status::Retired: return DXGI_ERROR_UNSUPPORTED;
  default: return E_INVALIDARG;
  }
}

struct D3D12DescriptorProvider {
  Com<ID3D12DescriptorHeap> heap;
  UINT index = 0, type = 0;
  ray_library::Resource required{};
  bool Valid() const {
    if (!heap || type > 3) return false;
    const auto desc = heap->GetDesc();
    return index < desc.NumDescriptors && UINT(desc.Type) == (type == 3 ? 1u : 0u) &&
           (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
  }
  capture::Status Acquire(D3D12DescriptorCapture &out) const {
    out = {};
    if (!Valid()) return capture::Status::Invalid;
    D3D12DescriptorCapture next;
    HRESULT hr;
    if (type == 3) {
      Com<MTLD3D12SamplerDescriptorHeap> implementation;
      if (FAILED(heap->QueryInterface(kD3D12SamplerHeapCaptureUUID, reinterpret_cast<void **>(&implementation))) || !implementation)
        return capture::Status::Unsupported;
      hr = implementation->CaptureDescriptor(index, next);
    } else {
      Com<MTLD3D12DescriptorHeap> implementation;
      if (FAILED(heap->QueryInterface(kD3D12ResourceHeapCaptureUUID, reinterpret_cast<void **>(&implementation))) || !implementation)
        return capture::Status::Unsupported;
      hr = implementation->CaptureDescriptor(index, next);
    }
    if (FAILED(hr)) return hr == E_OUTOFMEMORY ? capture::Status::OutOfMemory :
                          hr == E_INVALIDARG ? capture::Status::Invalid : capture::Status::Unsupported;
    UINT actual = UINT32_MAX;
    switch (next.type) {
    case ShaderVisibleDescriptorType::SRVTexture:
    case ShaderVisibleDescriptorType::SRVTexelBuffer:
    case ShaderVisibleDescriptorType::SRVBuffer: actual = 0; break;
    case ShaderVisibleDescriptorType::UAVTexture:
    case ShaderVisibleDescriptorType::UAVTexelBuffer:
    case ShaderVisibleDescriptorType::UAVBuffer: actual = 1; break;
    case ShaderVisibleDescriptorType::ConstantBuffer: actual = 2; break;
    case ShaderVisibleDescriptorType::Sampler: actual = 3; break;
    default: break;
    }
    if (actual != type) return capture::Status::Invalid;
    const auto match = resource_shape::Match(required, next.shape, bool(next.counter));
    if (match != capture::Status::Ready) return match;
    out = std::move(next);
    return capture::Status::Ready;
  }
};

// Recording-thread ownership only: never put this COM-bearing object on the native retirement worker.
struct D3D12RecordedDescriptorBinding {
  ID3D12Device *recorded_device = nullptr; // Identity only; the recording captures own the device.
  ray_binding::DescriptorPolicy policy;
  Com<ID3D12Device> acceleration_device;
  std::shared_ptr<const D3D12AccelerationBuildPlan> acceleration_producer;
  D3D12NativeAccelerationCapture::Ptr acceleration;
  bool root_buffer = false;
  D3D12DescriptorCapture fixed_buffer;
  std::optional<compiler_resource::Footprint> compiler_footprint;
  capture::RecordedDescriptor<D3D12DescriptorProvider, D3D12DescriptorCapture> replay;
  HRESULT ResolveForSubmission(D3D12NativeDescriptorCapture &out) const {
    out = {};
    if (acceleration_producer) return DXGI_ERROR_UNSUPPORTED; // Queue resolves this occurrence only after its build.
    if (acceleration) {
      if (root_buffer || compiler_footprint || !acceleration_device ||
          acceleration_device.ptr() != recorded_device) return E_INVALIDARG;
      out.acceleration = acceleration;
      return S_OK;
    }
    D3D12DescriptorCapture resolved;
    const HRESULT hr = root_buffer ? (fixed_buffer.resource ? S_OK : E_INVALIDARG) : CaptureResult(replay.Resolve(resolved));
    if (root_buffer) resolved = fixed_buffer;
    if (FAILED(hr)) return hr;
    if (compiler_footprint) {
      const HRESULT match = CaptureResult(compiler_resource::Match(*compiler_footprint, resolved.shape,
          resolved.byte_length, bool(resolved.counter), root_buffer));
      if (FAILED(match)) return match;
    }
    out = capture::DetachNative<D3D12NativeDescriptorCapture>(resolved);
    return S_OK;
  }
};

// Private normalized Metal build result, not a D3D12 VA/SRV. Caller supplies the actual
// build kind and all BLAS children, and orders successful build completion before use.
// No build, wait, commit or public DXR capability change is performed by this capture.
HRESULT CaptureBuiltAccelerationStructure(MTLD3D12Device *device, WMTASBuildKind kind,
    WMT::Reference<WMT::AccelerationStructure> resource,
    std::vector<D3D12NativeAccelerationCapture::Ptr> children, D3D12RecordedDescriptorBinding &out);

// Compiler-generated metallib entry, not a public DXR StateObject or ordinary SM50 PSO.
class D3D12CompiledComputeProgram {
  friend HRESULT CreateCompiledComputeProgram(MTLD3D12Device *, const void *, size_t,
      std::string_view, WMTSize, std::shared_ptr<const D3D12CompiledComputeProgram> &);
  Com<ID3D12Device> owner_;
  WMT::Reference<WMT::Library> library_;
  WMT::Reference<WMT::Function> function_;
  WMT::Reference<WMT::ComputePipelineState> pipeline_;
  WMTComputeBindingLayout layout_{};
  WMTSize threads_{};
  D3D12CompiledComputeProgram() = default;
public:
  ID3D12Device *Owner() const { return owner_.ptr(); }
  const WMT::Reference<WMT::Function> &Function() const { return function_; }
  const WMT::Reference<WMT::ComputePipelineState> &Pipeline() const { return pipeline_; }
  const WMTComputeBindingLayout &Layout() const { return layout_; }
  WMTSize Threads() const { return threads_; }
};
HRESULT CreateCompiledComputeProgram(MTLD3D12Device *device, const void *bytes, size_t size,
    std::string_view entry, WMTSize threads, std::shared_ptr<const D3D12CompiledComputeProgram> &out);

// COM-bearing bindings remain on the recording/ExecuteCommandLists thread.
struct D3D12RecordedComputeDispatch {
  ID3D12Device *device = nullptr; // Recording identity, never retained by native retirement.
  argument_upload::Plan arguments;
  std::optional<metal_argument::Plan> metal_arguments;
  WMTComputeBindingLayout entry_layout{};
  WMT::Reference<WMT::Function> function;
  std::vector<D3D12RecordedDescriptorBinding> bindings;
  WMT::Reference<WMT::ComputePipelineState> pipeline;
  WMTSize threads{}, groups{};
};

struct D3D12NativeArgumentBuffer {
  WMT::Reference<WMT::Buffer> buffer;
  uint64_t address = 0;
  capture::ResidencyLease<D3D12ResidencyState, WMT::Allocation> residency;
  D3D12NativeArgumentBuffer(WMT::Reference<WMT::Buffer> value, uint64_t address,
                           std::shared_ptr<D3D12ResidencyState> owner) :
      buffer(std::move(value)), address(address), residency(std::move(owner), buffer) {}
};
struct D3D12NativeComputeDispatch {
  WMT::Reference<WMT::ComputePipelineState> pipeline;
  WMTSize threads{}, groups{};
  std::vector<std::shared_ptr<D3D12NativeArgumentBuffer>> buffers;
  std::vector<uint32_t> indices;
  std::vector<D3D12NativeDescriptorCapture> resources;
  std::vector<WMTArgumentBinding> direct_bindings;
  std::vector<wmtcmd_compute_useresource> resource_uses;
};

// Recording inputs use normalized Metal geometry/instance layout, not D3D12 DXR structs.
// Geometry offsets are relative to the captured view; handles in geometry must be zero.
struct D3D12AccelerationGeometryInput {
  WMTASTriangleGeometry geometry{};
  D3D12DescriptorCapture vertices, indices;
};
struct D3D12AccelerationBuildInput {
  WMTASBuildKind kind=WMTASBuildTriangles;
  std::vector<D3D12AccelerationGeometryInput> geometries;
  D3D12DescriptorCapture instances;
  uint64_t instance_offset=0, instance_count=0, instance_stride=0;
  std::vector<D3D12RecordedDescriptorBinding> children;
};
struct D3D12AccelerationBuildChild {
  D3D12NativeAccelerationCapture::Ptr existing;
  std::shared_ptr<const D3D12AccelerationBuildPlan> producer;
};
// Created once by recording, never mutated after publication. No COM objects.
struct D3D12AccelerationBuildPlan {
  const void *device_identity=nullptr;
  WMTASBuildKind kind=WMTASBuildTriangles;
  std::vector<WMTASTriangleGeometry> geometries;
  std::vector<std::shared_ptr<D3D12NativeResourceCapture>> inputs;
  obj_handle_t instance_buffer=0;
  uint64_t instance_offset=0,instance_count=0,instance_stride=0;
  std::vector<D3D12AccelerationBuildChild> children;
};
HRESULT PrepareAccelerationBuildPlan(MTLD3D12Device *device,
    const D3D12AccelerationBuildInput &input,std::shared_ptr<const D3D12AccelerationBuildPlan> &out);

// One fresh allocation set for one occurrence of a recorded build.
struct D3D12NativeAccelerationBuild {
  std::shared_ptr<const D3D12AccelerationBuildPlan> plan;
  std::vector<obj_handle_t> child_handles;
  D3D12NativeAccelerationCapture::Ptr result;
  std::shared_ptr<D3D12NativeArgumentBuffer> scratch;
  WMTASBuildDesc Descriptor() const {
    WMTASBuildDesc d{};
    d.kind=plan->kind;
    if (d.kind==WMTASBuildTriangles) {
      d.geometries.set(plan->geometries.data());d.geometry_count=plan->geometries.size();
    } else {
      d.instance_buffer=plan->instance_buffer;d.instance_offset=plan->instance_offset;
      d.instance_count=plan->instance_count;d.instance_stride=plan->instance_stride;
      d.acceleration_structures.set(child_handles.data());d.acceleration_structure_count=child_handles.size();
    }
    return d;
  }
};
using D3D12SubmissionScenes=scene_build::SubmissionScenes<D3D12AccelerationBuildPlan,D3D12NativeAccelerationCapture>;

// Mapping ownership is native-only, including residency after the public heap is released.
// Old backing heaps are conservatively retained until the reserved resource retires.
struct D3D12SparseBufferState {
  dxmt::mutex mutex;
  std::shared_ptr<D3D12ResidencyState> residency;
  WMT::Reference<WMT::Buffer> buffer;
  std::vector<WMT::Reference<WMT::Heap>> heaps;
  uint32_t tiles = 0;
  ~D3D12SparseBufferState() {
    if (!residency) return;
    for (auto &heap : heaps) residency->Release(heap);
    residency->Release(buffer);
  }
};

inline constexpr GUID kD3D12ResourceCaptureImplementationUUID = {0x14d3c352, 0x7fd7, 0x4a90, {0xaf, 0x1a, 0x34, 0x15, 0xa2, 0xbe, 0x71, 0x46}};

class MTLD3D12Resource : public ID3D12Resource {
public:
  Rc<Texture> texture;
  Rc<Buffer> buffer;
  std::shared_ptr<D3D12SparseBufferState> sparse;
  std::shared_ptr<D3D12ResourceCaptureSource> capture_source;

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) = 0;

  virtual HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(
      ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) = 0;

  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) = 0;

  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) = 0;

  virtual void STDMETHODCALLTYPE GetResourceTiling(
      UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
      UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) = 0;
};

class MTLD3D12Heap : public ID3D12Heap {
public:
  WMT::Reference<WMT::Heap> heap;
  bool placement_sparse_compatible = false;
};

class D3D12RemovableFence;

class MTLD3D12Fence : public ID3D12Fence1 {
public:
  virtual std::shared_ptr<D3D12RemovableFence> GetRemovalState() = 0;
  virtual HRESULT SignalQueue(WMT::CommandBuffer cmdbuf, UINT64 value) = 0;
  virtual HRESULT WaitQueue(WMT::CommandBuffer cmdbuf, UINT64 value) = 0;
};

class D3D12RemovableFence {
public:
  virtual ~D3D12RemovableFence() = default;
  virtual void Remove() = 0;
  virtual bool HasPendingEvents() = 0;
};

inline constexpr GUID kD3D12RootSignatureImplementationUUID = {0x0d15dd3f, 0x8881, 0x4c7e, {0xa2, 0x7a, 0x0a, 0x18, 0x62, 0xa1, 0xc0, 0x14}};

class MTLD3D12RootSignature : public ID3D12RootSignature {
public:
  bool IsLocal = false;
  root_argument::Layout ArgumentLayout;
  const local_root::Layout *LocalRootLayout = nullptr;
  ray_binding::Status RayBindingStatus = ray_binding::Status::Unsupported;
  ray_binding::RootLayout RayBindings;

  virtual UINT GetBlob(const void **ppBlob) = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;

  uint32_t UploadQwords = 0;
  uint32_t ParameterSlots = 0;
  uint32_t const *SlotQwordOffsets = nullptr;

  size_t NumStaticSamplers = 0;
  uint64_t const *EncodedStaticSamplers = nullptr;
};

class MTLD3D12CommandSignature : public ID3D12CommandSignature {
public:
  D3D12_INDIRECT_ARGUMENT_TYPE CommandType;
  UINT UpdateRootArguments : 1;
  UINT UpdateVertexBuffers : 1;
  UINT UpdateIndexBuffer   : 1;

  WMT::Reference<WMT::RenderPipelineState> render_resolver;
  WMT::Reference<WMT::ComputePipelineState> compute_resolver;
  WMT::Reference<WMT::RenderPipelineState> mesh_resolver;
  uint32_t MeshDrawStride = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12QueryHeap : public ID3D12QueryHeap {
public:
  ID3D12Device *owner = nullptr;
  UINT count = 0;
  D3D12_QUERY_HEAP_TYPE type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  static constexpr UINT TimestampPageSize = 4096;
  std::vector<WMT::Reference<WMT::CounterSampleBuffer>> timestamp_pages;
  WMT::Reference<WMT::Buffer> results;
  WMT::Reference<WMT::ComputePipelineState> accumulate;
  WMT::Reference<WMT::ComputePipelineState> binary_resolve;
};

class MTLD3D12PipelineState : public ID3D12PipelineState {
public:
  UINT IsComputePipelineState;

  static HRESULT
  InitializeShader(D3D12_SHADER_BYTECODE Bytecode, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl);
};

class MTLD3D12GraphicsPipelineState : public MTLD3D12PipelineState {
public:
  WMT::Reference<WMT::RenderPipelineState> pso;
  WMT::Reference<WMT::RenderPipelineState> mesh_indexed_pso[2];
  uint32_t slot_mask = 0;
  uint32_t tess_control_points = 0;
  uint32_t tess_threads_per_patch = 0;
  bool geometry_pipeline = false;
  uint32_t geometry_input_vertices = 3;
  uint32_t geometry_vertices_per_group = 30;
  enum WMTTriangleFillMode fill_mode;
  enum WMTCullMode cull_mode;
  enum WMTDepthClipMode depth_clip_mode;
  enum WMTWinding winding;
  float depth_bias;
  float scole_scale;
  float depth_bias_clamp;
  uint32_t forced_sample_count;

  virtual WMT::DepthStencilState GetDepthStencilState(UINT DSVPlanar, UINT DSVReadonlyFlags) = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12ComputePipelineState : public MTLD3D12PipelineState {
public:
  WMT::Reference<WMT::Function> function;
  WMT::Reference<WMT::ComputePipelineState> pso;
  WMTSize threadgroup_size;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12Device : public ID3D12Device5 {
public:
  virtual WMT::Device GetMTLDevice() = 0;

  virtual D3D_FEATURE_LEVEL GetFeatureLevel() = 0;

  virtual HRESULT GetAdapter(REFIID riid, void **ppAdapter) = 0;

  virtual WMT::ResidencySet GetGlobalResidencySet() = 0;
  virtual std::shared_ptr<D3D12ResidencyState> GetResidencyOwner() = 0;

  virtual HRESULT RegisterResidency(WMT::Allocation allocation) = 0;

  virtual HRESULT UnregisterResidency(WMT::Allocation allocation) = 0;

  virtual HRESULT RegisterResidencyAndVA(BufferAllocation *allocation, uint64_t logical_length) = 0;

  virtual HRESULT UnregisterResidencyAndVA(BufferAllocation *allocation) = 0;

  virtual void RegisterCaptureSource(const std::shared_ptr<D3D12ResourceCaptureSource> &source) = 0;
  virtual void UnregisterCaptureSource(const D3D12ResourceCaptureSource &source) = 0;
  virtual std::shared_ptr<D3D12ResourceCaptureSource> LookupCaptureSource(const void *identity) = 0;
  virtual std::shared_ptr<D3D12ResourceCaptureSource> LookupCaptureByVA(uint64_t address, uint64_t bytes, uint64_t &offset) = 0;
  virtual Rc<BufferAllocation> LookupBufferByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t length, uint64_t *pOffset) = 0;

  virtual InternalCommandLibrary& GetLib() = 0;

  virtual FormatCapability GetMTLPixelFormatCapability(WMTPixelFormat Format) = 0;

  virtual HRESULT RegisterFence(std::shared_ptr<D3D12RemovableFence> fence) = 0;
  virtual void UnregisterFence(D3D12RemovableFence *fence) = 0;

  EventListener event_listener;

  WMT::Reference<WMT::DepthStencilState> default_depth_stencil_state;
};

HRESULT CreateD3D12Device(IMTLDXGIAdapter *adapter, REFIID riid, void **ppDevice);

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue);

HRESULT
CreateCommandAllocator(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator);

HRESULT
CreateDescriptorHeap(
    MTLD3D12Device *pDevice, const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid, void **ppDescriptorHeap
);

HRESULT
CreateQueryHeap(MTLD3D12Device *pDevice, const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppQueryHeap);

HRESULT CreateCommittedTexture(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource
);

HRESULT
CreatePlacedTexture(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
);

HRESULT CreateCommittedBuffer(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource
);

HRESULT
CreatePlacedBuffer(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
);

HRESULT CreateReservedBuffer(
    MTLD3D12Device *pDevice, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
);

HRESULT
CreateHeap(MTLD3D12Device *pDevice, const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap);

HRESULT
CreateRootSignature(
    MTLD3D12Device *pDevice, UINT NodeMask, const void *pBytecode, SIZE_T BytecodeLength, REFIID riid,
    void **ppRootSignature
);

HRESULT
CreateCommandSignature(
    MTLD3D12Device *pDevice, const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature,
    REFIID riid, void **ppCommandSignature
);

HRESULT
CreateGraphicsPipelineState(
    MTLD3D12Device *pDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
);

HRESULT
CreateComputePipelineState(
    MTLD3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
);

HRESULT
CreateSwapChain(
    IDXGIFactory1 *pFactory, MTLD3D12Device *pDevice, MTLD3D12CommandQueue *pQueue, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGISwapChain1 **ppSwapChain
);

HRESULT
CreateFence(MTLD3D12Device *pDevice, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence);

void PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_STATIC_SAMPLER_DESC const &Desc);

void PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_SAMPLER_DESC const &Desc);

inline std::tuple<MTLD3D12RenderTargetDescriptorHeap *, UINT>
GetRenderTargetHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12RenderTargetDescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetRenderTargetDescriptor(MTLD3D12RenderTargetDescriptorHeap *pHeap, UINT Index) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline std::tuple<MTLD3D12DescriptorHeap *, UINT>
GetShaderVisibleDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12DescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetShaderVisibleDescriptor(MTLD3D12DescriptorHeap *pHeap, UINT Index) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
  //
}

inline std::tuple<MTLD3D12SamplerDescriptorHeap *, UINT>
GetSamplerDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12SamplerDescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetSamplerDescriptor(MTLD3D12SamplerDescriptorHeap *pHeap, UINT Index) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
}

template <typename VIEW_DESC>
HRESULT ExtractEntireResourceViewDescription(const D3D12_RESOURCE_DESC &ResourceDesc, VIEW_DESC *pViewDescOut);

constexpr auto kDefaultShader4Component = 0b1'011'010'001'000;

HRESULT ValidateResourceStates(D3D12_RESOURCE_STATES State, const D3D12_HEAP_PROPERTIES *pHeapProps);

HRESULT ValidateResourceDescs(const D3D12_RESOURCE_DESC *pDesc, const D3D12_HEAP_PROPERTIES *pHeapProps);

HRESULT ValidateHeapProperties(const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS Flags, bool AdapterIsNUMA);

D3D12_BOX GetResourceExtent(const D3D12_RESOURCE_DESC &Desc, UINT MipSlice);

UINT DecomposeSubresource(
    const D3D12_RESOURCE_DESC &Desc, UINT Subresource = 0, UINT *pMipSlice = NULL, UINT *pArraySlice = NULL,
    UINT *pPlaneSlice = NULL
);

bool IsCpuVisibleHeap(const D3D12_HEAP_PROPERTIES *pHeapProps);

bool IsD3D12BoxInBounds(D3D12_BOX &box, D3D12_BOX &bounds);

} // namespace dxmt
