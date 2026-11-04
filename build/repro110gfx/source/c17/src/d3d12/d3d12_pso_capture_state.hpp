// Copyright 2026 MacRunner. SPDX-License-Identifier: LGPL-2.1-or-later
// Included after TracePipelineCapture; application descriptors, before DXIL substitution.
#pragma once

namespace pso_capture_state {

// No addresses, padding, locale-dependent floats or cached native PSOs enter the key.
// Floating-point fields retain their IEEE754 bits. Semantic names retain their bytes.
class Json {
  std::string text_;
  std::vector<bool> first_;
public:
  void prefix(const char *key) {
    if (!first_.empty()) {
      if (!first_.back()) text_ += ',';
      first_.back() = false;
    }
    if (key) { text_ += '"'; text_ += key; text_ += "\":"; }
    if (text_.size() > 256 * 1024) throw "description_limit";
  }
  void open(const char *key, char type = '{') { prefix(key); text_ += type; first_.push_back(true); }
  void close(char type = '}') { text_ += type; first_.pop_back(); }
  template<typename T> void number(const char *key, T value) { prefix(key); text_ += std::to_string(value); }
  void literal(const char *key, const char *value) { prefix(key); text_ += '"'; text_ += value; text_ += '"'; }
  void hex(const char *key, const void *data, size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    prefix(key); text_ += '"';
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < count; ++i) { text_ += digits[bytes[i] >> 4]; text_ += digits[bytes[i] & 15]; }
    text_ += '"';
    if (text_.size() > 256 * 1024) throw "description_limit";
  }
  void floating(const char *key, float value) {
    uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); number(key, bits);
  }
  const std::string &text() const { return text_; }
};

template<typename T> static std::vector<T>
Read(const T *ptr, size_t count, size_t limit) {
  if (count > limit) throw "array_limit";
  std::vector<T> copy(count);
  if (count) {
    SIZE_T read = 0;
    if (!ptr || !ReadProcessMemory(GetCurrentProcess(), ptr, copy.data(), count * sizeof(T), &read) ||
        read != count * sizeof(T)) throw "unreadable_memory";
  }
  return copy;
}

static void Semantic(Json &out, const char *name) {
  std::array<char, 256> bytes{};
  if (!name) { out.literal("SemanticName_status", "NULL"); return; }
  for (size_t i = 0; i < bytes.size(); ++i) {
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), name + i, &bytes[i], 1, &read) || read != 1)
      throw "unreadable_semantic";
    if (!bytes[i]) { out.hex("SemanticName_hex", bytes.data(), i); return; }
  }
  throw "semantic_limit";
}

static void Shader(Json &out, const char *stage, D3D12_SHADER_BYTECODE code) {
  out.open(nullptr); out.literal("stage", stage); out.number("length", uint64_t(code.BytecodeLength));
  if (!code.BytecodeLength) out.literal("status", "EMPTY");
  else {
    auto bytes = Read(static_cast<const uint8_t *>(code.pShaderBytecode), code.BytecodeLength, 16 * 1024 * 1024);
    const auto hash = Sha1HashState::compute(bytes.data(), bytes.size());
    out.literal("status", "PRESENT"); out.hex("sha1", hash.data, sizeof(hash.data));
  }
  out.close();
}

static void Root(Json &out, ID3D12RootSignature *root) {
  out.open("RootSignature");
  if (!root) {
    // The bytecode identity is still captured, but no explicit root object was supplied.
    out.literal("status", "NOT_PROVIDED"); out.literal("source", "embedded_shader_if_any");
  } else {
    Com<MTLD3D12RootSignature> implementation;
    if (FAILED(root->QueryInterface(kD3D12RootSignatureImplementationUUID, reinterpret_cast<void **>(&implementation))))
      throw "foreign_root_signature";
    const void *blob = nullptr;
    const UINT size = implementation->GetBlob(&blob);
    auto bytes = Read(static_cast<const uint8_t *>(blob), size, 64 * 1024);
    out.literal("status", "PRESENT"); out.literal("format", "D3D12_serialized_root_signature");
    out.number("length", size);
    const auto hash = Sha1HashState::compute(bytes.data(), bytes.size());
    out.hex("sha1", hash.data, sizeof(hash.data));
    // Complete serialized descriptor: version, parameters/ranges/flags and static samplers.
    // Consumers use D3D12CreateVersionedRootSignatureDeserializer on these exact bytes.
    out.hex("blob_hex", bytes.data(), bytes.size());
  }
  out.close();
}

static void Describe(Json &out, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &d) {
  out.open(nullptr); out.literal("schema", "dx12-pso-state-v1"); out.literal("kind", "graphics");
  out.literal("shader_identity_domain", "application_bytecode_before_translation");
  out.open("shaders", '[');
  Shader(out, "VS", d.VS); Shader(out, "PS", d.PS); Shader(out, "HS", d.HS);
  Shader(out, "DS", d.DS); Shader(out, "GS", d.GS); out.close(']');
  Root(out, d.pRootSignature);
  out.open("InputLayout", '[');
  for (const auto &element : Read(d.InputLayout.pInputElementDescs, d.InputLayout.NumElements, 32)) {
    out.open(nullptr); Semantic(out, element.SemanticName);
#define ELEMENT(field) out.number(#field, unsigned(element.field))
    ELEMENT(SemanticIndex); ELEMENT(Format); ELEMENT(InputSlot); ELEMENT(AlignedByteOffset);
    ELEMENT(InputSlotClass); ELEMENT(InstanceDataStepRate);
#undef ELEMENT
    out.close();
  }
  out.close(']');
  out.open("StreamOutput"); out.number("RasterizedStream", d.StreamOutput.RasterizedStream);
  out.open("entries", '[');
  for (const auto &element : Read(d.StreamOutput.pSODeclaration, d.StreamOutput.NumEntries, 256)) {
    out.open(nullptr); Semantic(out, element.SemanticName);
#define ELEMENT(field) out.number(#field, unsigned(element.field))
    ELEMENT(Stream); ELEMENT(SemanticIndex); ELEMENT(StartComponent); ELEMENT(ComponentCount); ELEMENT(OutputSlot);
#undef ELEMENT
    out.close();
  }
  out.close(']'); out.open("strides", '[');
  for (auto stride : Read(d.StreamOutput.pBufferStrides, d.StreamOutput.NumStrides, 4)) out.number(nullptr, stride);
  out.close(']'); out.close();
  out.open("BlendState");
  out.number("AlphaToCoverageEnable", d.BlendState.AlphaToCoverageEnable);
  out.number("IndependentBlendEnable", d.BlendState.IndependentBlendEnable);
  out.open("RenderTarget", '[');
  for (const auto &target : d.BlendState.RenderTarget) {
    out.open(nullptr);
#define TARGET(field) out.number(#field, unsigned(target.field))
    TARGET(BlendEnable); TARGET(LogicOpEnable); TARGET(SrcBlend); TARGET(DestBlend); TARGET(BlendOp);
    TARGET(SrcBlendAlpha); TARGET(DestBlendAlpha); TARGET(BlendOpAlpha); TARGET(LogicOp); TARGET(RenderTargetWriteMask);
#undef TARGET
    out.close();
  }
  out.close(']'); out.close();
  out.open("RasterizerState");
#define RASTER(field) out.number(#field, d.RasterizerState.field)
  RASTER(FillMode); RASTER(CullMode); RASTER(FrontCounterClockwise); RASTER(DepthBias);
  out.floating("DepthBiasClamp_bits", d.RasterizerState.DepthBiasClamp);
  out.floating("SlopeScaledDepthBias_bits", d.RasterizerState.SlopeScaledDepthBias);
  RASTER(DepthClipEnable); RASTER(MultisampleEnable); RASTER(AntialiasedLineEnable);
  RASTER(ForcedSampleCount); RASTER(ConservativeRaster);
#undef RASTER
  out.close(); out.open("DepthStencilState");
#define DEPTH(field) out.number(#field, d.DepthStencilState.field)
  DEPTH(DepthEnable); DEPTH(DepthWriteMask); DEPTH(DepthFunc); DEPTH(StencilEnable);
  DEPTH(StencilReadMask); DEPTH(StencilWriteMask);
#undef DEPTH
  for (bool front : {true, false}) {
    const auto &face = front ? d.DepthStencilState.FrontFace : d.DepthStencilState.BackFace;
    out.open(front ? "FrontFace" : "BackFace");
#define FACE(field) out.number(#field, unsigned(face.field))
    FACE(StencilFailOp); FACE(StencilDepthFailOp); FACE(StencilPassOp); FACE(StencilFunc);
#undef FACE
    out.close();
  }
  out.close();
#define DESC(field) out.number(#field, unsigned(d.field))
  DESC(SampleMask); DESC(IBStripCutValue); DESC(PrimitiveTopologyType); DESC(NumRenderTargets);
  DESC(DSVFormat); DESC(NodeMask); DESC(Flags);
#undef DESC
  out.open("RTVFormats", '['); for (auto format : d.RTVFormats) out.number(nullptr, unsigned(format)); out.close(']');
  out.open("SampleDesc"); out.number("Count", d.SampleDesc.Count); out.number("Quality", d.SampleDesc.Quality); out.close();
  out.literal("CachedPSO", "EXCLUDED_NON_SEMANTIC_CACHE"); out.close();
}

static void Describe(Json &out, const D3D12_COMPUTE_PIPELINE_STATE_DESC &d) {
  out.open(nullptr); out.literal("schema", "dx12-pso-state-v1"); out.literal("kind", "compute");
  out.literal("shader_identity_domain", "application_bytecode_before_translation");
  out.open("shaders", '['); Shader(out, "CS", d.CS); out.close(']');
  Root(out, d.pRootSignature);
  out.number("NodeMask", d.NodeMask); out.number("Flags", unsigned(d.Flags));
  out.literal("CachedPSO", "EXCLUDED_NON_SEMANTIC_CACHE"); out.close();
}

static UINT InputElements(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &d) { return d.InputLayout.NumElements; }
static UINT InputElements(const D3D12_COMPUTE_PIPELINE_STATE_DESC &) { return 0; }

// Both descriptor families share the same byte quota, including concurrent calls.
static std::atomic<size_t> &DescriptionBytes() {
  static std::atomic<size_t> total{0};
  return total;
}

template<typename Descriptor>
static void Trace(uint32_t id, const void *owner, const Descriptor *desc) noexcept {
  if (!id) return;
  const DWORD saved_error = GetLastError();
  try {
    const auto snapshot = Read(desc, 1, 1);
    Json description; Describe(description, snapshot.front());
    const auto &bytes = description.text();
    auto &total = DescriptionBytes();
    size_t used = total.load(std::memory_order_relaxed);
    do {
      if (bytes.size() > 8 * 1024 * 1024 - used) throw "total_description_limit";
    } while (!total.compare_exchange_weak(used, used + bytes.size(), std::memory_order_relaxed));
    const auto hash = Sha1HashState::compute(bytes.data(), bytes.size()).string();
    TracePipelineCapture(id, owner, "state.begin", "schema=1 status=PRESENT bytes=%llu sha1=%s input_elements=%u root=%s",
        uint64_t(bytes.size()), hash.c_str(), InputElements(snapshot.front()),
        snapshot.front().pRootSignature ? "PRESENT" : "NOT_PROVIDED");
    for (size_t offset = 0; offset < bytes.size(); offset += 128) {
      static constexpr char hex[] = "0123456789abcdef";
      char encoded[257] = {};
      const size_t size = std::min<size_t>(128, bytes.size() - offset);
      for (size_t i = 0; i < size; ++i) {
        const auto byte = uint8_t(bytes[offset + i]);
        encoded[2 * i] = hex[byte >> 4]; encoded[2 * i + 1] = hex[byte & 15];
      }
      TracePipelineCapture(id, owner, "state.chunk", "offset=%llu hex=%s", uint64_t(offset), encoded);
    }
    TracePipelineCapture(id, owner, "state.end", "bytes=%llu sha1=%s", uint64_t(bytes.size()), hash.c_str());
  } catch (const char *reason) {
    TracePipelineCapture(id, owner, "state.failed", "status=FAILED reason=%s", reason);
  } catch (...) {
    TracePipelineCapture(id, owner, "state.failed", "status=FAILED reason=capture_exception");
  }
  SetLastError(saved_error);
}

} // namespace pso_capture_state
