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

#include "Metal.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_pipeline.hpp"
#include "wave_native_bundle.hpp"
#include "d3d12_shader_capture.hpp"
#include "d3d12_diagnostic_counters.hpp"
#include "log/log.hpp"

namespace dxmt {

HRESULT
PrepareAccelerationBuildPlan(MTLD3D12Device *device,const D3D12AccelerationBuildInput &input,
    std::shared_ptr<const D3D12AccelerationBuildPlan> &out) {
  out.reset();
  if (!device || (input.kind!=WMTASBuildTriangles && input.kind!=WMTASBuildInstances)) return E_INVALIDARG;
  try {
    auto plan=std::make_shared<D3D12AccelerationBuildPlan>();
    plan->device_identity=static_cast<ID3D12Device *>(device);plan->kind=input.kind;
    auto buffer=[&](const D3D12DescriptorCapture &view,uint64_t relative,uint64_t bytes,
                    obj_handle_t &handle,uint64_t &offset) {
      handle=0;offset=0;
      if (!view.resource || !view.resource->Live() || view.resource->device.ptr()!=plan->device_identity ||
          view.counter || view.texture || view.texel || view.sampler ||
          (view.type!=ShaderVisibleDescriptorType::SRVBuffer && view.type!=ShaderVisibleDescriptorType::UAVBuffer &&
           view.type!=ShaderVisibleDescriptorType::ConstantBuffer)) return false;
      const auto &native=view.resource->native;
      if (!native || !native->buffer_allocation ||
          !scene_build::Rebase(native->logical_length,view.buffer_offset,view.byte_length,relative,bytes,offset)) return false;
      handle=native->buffer_allocation->buffer();
      if (!handle) return false;
      plan->inputs.push_back(native);
      return true;
    };
    uint64_t bytes=0;
    if (input.kind==WMTASBuildTriangles) {
      if (input.geometries.empty() || input.geometries.size()>4096 || !input.children.empty() ||
          input.instances.resource || input.instance_offset || input.instance_count || input.instance_stride) return E_INVALIDARG;
      plan->geometries.reserve(input.geometries.size());
      for (const auto &g:input.geometries) {
        auto wire=g.geometry;
        if (wire.vertex_buffer || wire.index_buffer || !wire.triangle_count || wire.triangle_count>UINT32_MAX/3 ||
            !scene_build::ByteSpan(wire.vertex_count,wire.vertex_stride,12,bytes) ||
            !buffer(g.vertices,wire.vertex_offset,bytes,wire.vertex_buffer,wire.vertex_offset)) return E_INVALIDARG;
        if (wire.index_type==WMTASIndexNone) {
          if (g.indices.resource || wire.index_offset || wire.vertex_count<wire.triangle_count*3) return E_INVALIDARG;
        } else if (wire.index_type==WMTASIndexUInt16 || wire.index_type==WMTASIndexUInt32) {
          const uint64_t width=wire.index_type==WMTASIndexUInt16?2:4;
          bytes=wire.triangle_count*3*width;
          if (!buffer(g.indices,wire.index_offset,bytes,wire.index_buffer,wire.index_offset)) return E_INVALIDARG;
        } else return DXGI_ERROR_UNSUPPORTED;
        plan->geometries.push_back(wire);
      }
    } else {
      if (!input.geometries.empty() || input.children.empty() || input.children.size()>4096 ||
          input.instance_count>UINT32_MAX || input.instance_stride%4 ||
          !scene_build::ByteSpan(input.instance_count,input.instance_stride,sizeof(WMTASUserIDInstance),bytes) ||
          !buffer(input.instances,input.instance_offset,bytes,plan->instance_buffer,plan->instance_offset)) return E_INVALIDARG;
      plan->instance_count=input.instance_count;plan->instance_stride=input.instance_stride;
      for (const auto &child:input.children) {
        if (child.recorded_device!=plan->device_identity || child.acceleration_device.ptr()!=plan->device_identity ||
            child.root_buffer || child.compiler_footprint || bool(child.acceleration)==bool(child.acceleration_producer))
          return E_INVALIDARG;
        if (child.acceleration_producer) {
          if (child.acceleration_producer->device_identity!=plan->device_identity ||
              child.acceleration_producer->kind!=WMTASBuildTriangles) return E_INVALIDARG;
        } else if (child.acceleration->Type()!=D3D12NativeAccelerationCapture::Kind::Primitive) return E_INVALIDARG;
        plan->children.push_back({child.acceleration,child.acceleration_producer});
      }
    }
    // Remaining backend alignment/device/flag checks happen before any GPU encoding.
    // GPU index/instance contents still follow the normalized internal producer contract.
    out=std::move(plan);return S_OK;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}

HRESULT
CaptureBuiltAccelerationStructure(MTLD3D12Device *device, WMTASBuildKind kind,
    WMT::Reference<WMT::AccelerationStructure> resource,
    std::vector<D3D12NativeAccelerationCapture::Ptr> children, D3D12RecordedDescriptorBinding &out) {
  out = {};
  if (!device || !resource || (kind != WMTASBuildTriangles && kind != WMTASBuildInstances) ||
      (kind == WMTASBuildTriangles && !children.empty()) ||
      (kind == WMTASBuildInstances && (children.empty() || children.size() > 4096))) return E_INVALIDARG;
  using Scene = D3D12NativeAccelerationCapture;
  auto validate = [&](obj_handle_t value, bool instance) {
    WMTComputeBinding requirement{};
    requirement.kind = instance ? WMTComputeBindingInstanceAccelerationStructure :
        WMTComputeBindingPrimitiveAccelerationStructure;
    WMTArgumentBinding binding{};
    binding.kind = instance ? WMTArgumentKindInstanceAccelerationStructure :
        WMTArgumentKindPrimitiveAccelerationStructure;
    binding.resource = value;
    return MTLDevice_validateComputeBindings(device->GetMTLDevice(), &requirement, &binding, 1);
  };
  if (validate(resource, kind == WMTASBuildInstances) != WMTArgumentStatusReady) return E_INVALIDARG;
  for (const auto &child : children) {
    if (!child || child->Type() != Scene::Kind::Primitive ||
        child->Native().resource.handle == resource.handle ||
        validate(child->Native().resource, false) != WMTArgumentStatusReady) return E_INVALIDARG;
  }
  try {
    auto allocation = std::make_shared<D3D12NativeAccelerationAllocation>(std::move(resource), device->GetResidencyOwner());
    Scene::Ptr native;
    const auto status = Scene::Create(kind == WMTASBuildInstances ? Scene::Kind::Instance : Scene::Kind::Primitive,
                                     std::move(allocation), std::move(children), native);
    if (status != capture::Status::Ready) return CaptureResult(status);
    out.recorded_device = static_cast<ID3D12Device *>(device);
    out.acceleration_device = static_cast<ID3D12Device *>(device);
    out.acceleration = std::move(native);
    return S_OK;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}


class MTLD3D12ComputePipelineStateImpl : public MTLD3D12Pageable<MTLD3D12ComputePipelineState> {

  MTL_SHADER_REFLECTION ref_cs;

public:
  MTLD3D12ComputePipelineStateImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12ComputePipelineState>(pDevice) {
    IsComputePipelineState = 1;
  }

  HRESULT InitializeNativeWave(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc) {
    char enabled[4] = {};
    const DWORD length = GetEnvironmentVariableA("MACRUNNER_DX12_NATIVE_WAVE", enabled, sizeof(enabled));
    if (length != 1 || enabled[0] != '1') return S_FALSE;
    const auto *entry = native_wave::MatchShader(native_wave::entries, native_wave::entry_count, desc->CS.pShaderBytecode, desc->CS.BytecodeLength);
    if (!entry) return S_FALSE;
    // This diagnostic candidate implements one verified ABI, not arbitrary roots.
    if (!desc->pRootSignature) return E_NOTIMPL;
    const void *root = nullptr;
    const size_t root_size = static_cast<MTLD3D12RootSignature *>(desc->pRootSignature)->GetBlob(&root);
    if (!native_wave::MatchesRoot(*entry, root, root_size)) return E_NOTIMPL;
    auto metal = device_->GetMTLDevice();
    WMT::Reference<WMT::Error> error;
    auto data = WMT::MakeDispatchData(reinterpret_cast<uint64_t>(entry->library), entry->library_size);
    auto library = metal.newLibrary(data, error);
    if (!library) { ERR("Native Wave library rejected"); return E_FAIL; }
    auto function = library.newFunction("cs_main");
    if (!function) { ERR("Native Wave entry missing"); return E_FAIL; }
    WMTComputePipelineInfo info;
    WMT::InitializeComputePipelineInfo(info);
    info.compute_function = function;
    info.support_indirect_command_buffers = true;
    pso = metal.newComputePipelineState(info, error);
    if (!pso) { ERR("Native Wave compute PSO rejected"); return E_FAIL; }
    threadgroup_size = {8, 4, 1};
    return S_OK;
  }

  HRESULT
  Initialize(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc) {

    if (!pDesc) return E_INVALIDARG;
    if (pDesc->pRootSignature && static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->IsLocal)
      return E_INVALIDARG;
    const HRESULT native_wave_result = InitializeNativeWave(pDesc);
    if (native_wave_result != S_FALSE) return native_wave_result;
    std::vector<uint8_t> compiled_cs;
    D3D12_SHADER_BYTECODE cs = pDesc->CS;
    const HRESULT artifact = LoadDiagnosticComputeArtifact(this, pDesc->CS, compiled_cs, cs);
    if (FAILED(artifact)) return artifact;
    // The external artifact carries no root signature; retain the application's
    // original layout and do not infer one from rewritten shader metadata.
    if (artifact == S_OK && !pDesc->pRootSignature) return E_NOTIMPL;

    SM50Shader shader_cs;
    SM50Error sm50_err;

    SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
    rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
    if (pDesc->pRootSignature) {
      rootsig.bytecode_length = static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->GetBlob(&rootsig.bytecode);
    } else {
      rootsig.bytecode = pDesc->CS.pShaderBytecode;
      rootsig.bytecode_length = pDesc->CS.BytecodeLength;
    }
    rootsig.next = nullptr;

    SM50_SHADER_COMMON_DATA common;
    common.flags = {};
    common.type = SM50_SHADER_COMMON;
    common.metal_version = SM50_SHADER_METAL_310;
    common.next = &rootsig;

    if (HRESULT hr = InitializeShader(cs, &shader_cs, &ref_cs); FAILED(hr))
      return hr;

    threadgroup_size = {ref_cs.ThreadgroupSize[0], ref_cs.ThreadgroupSize[1], ref_cs.ThreadgroupSize[2]};

    SM50ShaderBitcode cs_bitcode;

    diagnostic::Add(diagnostic::Counter::CompileCS);
    if (SM50Compile(shader_cs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common, "cs_main", &cs_bitcode, &sm50_err)) {
      ERR("Failed to compile cs shader");
      return E_FAIL;
    }

    SM50_COMPILED_BITCODE cs_bitcode_compiled;

    SM50GetCompiledBitcode(cs_bitcode, &cs_bitcode_compiled);

    auto cs_data = WMT::MakeDispatchData(cs_bitcode_compiled.Data, cs_bitcode_compiled.Size);

    auto metal = device_->GetMTLDevice();

    WMT::Reference<WMT::Error> err;

    auto cs_lib = metal.newLibrary(cs_data, err);

    auto cs_func = cs_lib.newFunction("cs_main");
    function = cs_func;

    // PSO
    {
      WMTComputePipelineInfo info;
      WMT::InitializeComputePipelineInfo(info);
      info.compute_function = cs_func;
      info.support_indirect_command_buffers = true;

      pso = metal.newComputePipelineState(info, err);
      if (!pso) {
        ERR("Failed to create compute PSO: ", err.description().getUTF8String());
        return E_FAIL;
      }
    }

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineState), riid)) {
      WARN("D3D12ComputePipelineState: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetCachedBlob(ID3DBlob **blob) {
    IMPLEMENT_ME
    return E_NOTIMPL;
  }
};

HRESULT
CreateComputePipelineState(
    MTLD3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
) {
  shader_capture::Scope originals("compute", pDevice, pDesc);
  auto pso = Com(new MTLD3D12ComputePipelineStateImpl(pDevice));
  HRESULT hr = pso->Initialize(pDesc);
  if (FAILED(hr)) {
    if (pDesc) TraceRejectedComputeBytecode(pso.ptr(), pDesc->CS, hr);
    return originals.finish(hr);
  }
  return originals.finish(pso->QueryInterface(riid, ppPipelineState));
};


HRESULT
CreateCompiledComputeProgram(MTLD3D12Device *device, const void *bytes, size_t size,
    std::string_view entry, WMTSize threads, std::shared_ptr<const D3D12CompiledComputeProgram> &out) {
  out.reset();
  if (!device || !bytes || size < 16 || size > 64 * 1024 * 1024 ||
      memcmp(bytes, "MTLB", 4) || entry.empty() || entry.size() > 255 ||
      entry.find('\0') != std::string_view::npos || !threads.width || !threads.height || !threads.depth)
    return E_INVALIDARG;
  try {
    auto program = std::shared_ptr<D3D12CompiledComputeProgram>(new D3D12CompiledComputeProgram);
    program->owner_ = static_cast<ID3D12Device *>(device);
    auto metal = device->GetMTLDevice();
    WMT::Error error; // newLibrary's NSError is borrowed, not an owned Reference.
    program->library_ = metal.newLibrary(bytes, size, error);
    if (!program->library_) return E_FAIL;
    const std::string name(entry);
    program->function_ = program->library_.newFunction(name.c_str());
    if (!program->function_) return E_INVALIDARG;
    WMTArgumentStatus status = WMTArgumentStatusFailed;
    program->pipeline_ = WMT::Reference<WMT::ComputePipelineState>(
        MTLDevice_newReflectedComputePipeline(metal, program->function_, &program->layout_, &status));
    if (!program->pipeline_ || status != WMTArgumentStatusReady)
      return status == WMTArgumentStatusUnsupported ? DXGI_ERROR_UNSUPPORTED :
          status == WMTArgumentStatusInvalid ? E_INVALIDARG :
          status == WMTArgumentStatusOutOfMemory ? E_OUTOFMEMORY : E_FAIL;
    if (!metal_argument::Threads(threads, program->layout_.max_threads)) return E_INVALIDARG;
    // The compiler supplies the semantic local size. Reflection proves hardware bounds only.
    program->threads_ = threads;
    out = std::move(program);
    return S_OK;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}

} // namespace dxmt
