// GPU-PASS 25.10.2026 TESS_PREPASS: shared pieces of the compute pre-pass variants of the tessellation stages.
// The vertex record holds exactly what the mesh vertex outputs would receive: slot 0 the (sanitized) position, slot 1 + i
// the mesh vertex data i (32-bit lanes, unused lanes 0), then the clip distances four to a slot. The domain kernel and the
// vertex-pulling fallback fill it through the AIRBuilder mesh sinks; the pass-through vertex function and the fallback
// return it as ordinary vertex outputs with the same user names, so the fragment function links unchanged.
#pragma once
#include "air_signature.hpp"
#include "airconv_error.hpp"
#include "nt/air_builder.hpp"
#include "llvm/IR/IRBuilder.h"
#include <vector>

namespace dxmt::dxbc::tess_prepass {

struct RecordLayout {
  std::vector<air::MeshVertexOutput> outputs; // signature order of the domain mesh function
  uint32_t data_count = 0;
  uint32_t clip_count = 0;
  uint32_t clip_slot = 0;
  uint32_t stride_ints = 4;
  bool has_position = false;
};

// clip_count: the domain converter defines the clip distance output itself (after the signature handlers), so it is
// passed in and appended here when the signature does not carry it yet
inline llvm::Expected<RecordLayout>
MakeRecordLayout(const air::FunctionSignatureBuilder &signature, llvm::LLVMContext &context, size_t clip_count) {
  RecordLayout layout;
  layout.outputs = signature.MeshVertexOutputs();
  bool has_clip = false;
  for (auto &output : layout.outputs) has_clip |= std::get_if<air::OutputClipDistance>(&output) != nullptr;
  if (clip_count && !has_clip) layout.outputs.push_back(air::OutputClipDistance{.count = clip_count});
  if (signature.HasMeshPrimitiveOutputs())
    return llvm::make_error<UnsupportedFeature>("tess prepass: per-primitive mesh outputs are not supported");
  uint32_t max_data = 0;
  for (auto &output : layout.outputs) {
    if (auto *data = std::get_if<air::OutputMeshData>(&output)) {
      auto *type = air::get_llvm_type(data->type, context);
      auto *element = type->isVectorTy() ? llvm::cast<llvm::FixedVectorType>(type)->getElementType() : type;
      const uint32_t lanes = type->isVectorTy() ? llvm::cast<llvm::FixedVectorType>(type)->getNumElements() : 1;
      if (element->getPrimitiveSizeInBits() != 32 || lanes > 4)
        return llvm::make_error<UnsupportedFeature>("tess prepass: mesh vertex data wider or narrower than 32-bit lanes");
      max_data = std::max(max_data, data->index + 1);
    } else if (std::get_if<air::OutputPosition>(&output)) {
      layout.has_position = true;
    } else if (auto *clip = std::get_if<air::OutputClipDistance>(&output)) {
      layout.clip_count = uint32_t(clip->count);
    } else {
      return llvm::make_error<UnsupportedFeature>("tess prepass: point size output is not supported");
    }
  }
  if (!layout.has_position)
    return llvm::make_error<UnsupportedFeature>("tess prepass: domain shader without position");
  layout.data_count = max_data;
  layout.clip_slot = 1 + max_data;
  layout.stride_ints = 4 * (layout.clip_slot + (layout.clip_count + 3) / 4);
  return layout;
}

// 32-bit lanes of a value (float / i32 scalar or vector) as i32
inline std::vector<llvm::Value *>
ToLanes(llvm::IRBuilderBase &ir, llvm::Value *value) {
  std::vector<llvm::Value *> lanes;
  auto *type = value->getType();
  if (type->isVectorTy()) {
    const uint32_t n = llvm::cast<llvm::FixedVectorType>(type)->getNumElements();
    for (uint32_t i = 0; i < n; i++) lanes.push_back(ir.CreateBitCast(ir.CreateExtractElement(value, uint64_t(i)), ir.getInt32Ty()));
  } else {
    lanes.push_back(ir.CreateBitCast(value, ir.getInt32Ty()));
  }
  return lanes;
}
inline llvm::Value *
FromLanes(llvm::IRBuilderBase &ir, llvm::Type *type, const std::vector<llvm::Value *> &lanes) {
  if (!type->isVectorTy()) return ir.CreateBitCast(lanes[0], type);
  auto *vector = llvm::cast<llvm::FixedVectorType>(type);
  llvm::Value *result = llvm::UndefValue::get(type);
  for (uint32_t i = 0; i < vector->getNumElements(); i++)
    result = ir.CreateInsertElement(result, ir.CreateBitCast(lanes[i], vector->getElementType()), uint64_t(i));
  return result;
}

// record_of(vertex) returns an i32 pointer (any address space) to the first int of that vertex's record
inline void
InstallSinks(
    llvm::air::AIRBuilder &air, llvm::IRBuilderBase &ir, const RecordLayout &layout,
    std::function<llvm::Value *(llvm::Value *vertex)> record_of
) {
  auto store_lanes = [&ir, record_of](llvm::Value *vertex, llvm::Value *int_offset, const std::vector<llvm::Value *> &lanes) {
    auto *base = record_of(vertex);
    for (uint32_t i = 0; i < lanes.size(); i++)
      ir.CreateStore(lanes[i], ir.CreateGEP(ir.getInt32Ty(), base, {ir.CreateAdd(int_offset, ir.getInt32(i))}));
  };
  air.MeshPositionSink = [&ir, store_lanes](llvm::Value *vertex, llvm::Value *position) {
    store_lanes(vertex, ir.getInt32(0), ToLanes(ir, position));
  };
  air.MeshVertexDataSink = [&ir, store_lanes](llvm::Value *vertex, llvm::Value *index, llvm::Value *value) {
    auto lanes = ToLanes(ir, value);
    while (lanes.size() < 4) lanes.push_back(ir.getInt32(0));
    store_lanes(vertex, ir.CreateMul(ir.CreateAdd(index, ir.getInt32(1)), ir.getInt32(4)), lanes);
  };
  const uint32_t clip_base = layout.clip_slot * 4;
  air.MeshClipDistanceSink = [&ir, store_lanes, clip_base](llvm::Value *vertex, llvm::Value *index, llvm::Value *value) {
    store_lanes(vertex, ir.CreateAdd(index, ir.getInt32(clip_base)), ToLanes(ir, value));
  };
}

// vertex function outputs, defined in the signature order of the mesh outputs; returns the output indices
inline std::vector<uint32_t>
DefineVertexOutputs(air::FunctionSignatureBuilder &signature, const RecordLayout &layout) {
  std::vector<uint32_t> indices;
  for (auto &output : layout.outputs) {
    if (std::get_if<air::OutputPosition>(&output))
      indices.push_back(signature.DefineOutput(air::OutputPosition{.type = air::msl_float4}));
    else if (auto *data = std::get_if<air::OutputMeshData>(&output))
      indices.push_back(signature.DefineOutput(air::OutputVertex{.user = data->user, .type = data->type}));
    else if (auto *clip = std::get_if<air::OutputClipDistance>(&output))
      indices.push_back(signature.DefineOutput(air::OutputClipDistance{.count = clip->count}));
    else
      indices.push_back(~0u);
  }
  return indices;
}

// the vertex function's return value from one record (record: i32 pointer to its first int)
inline llvm::Value *
LoadVertexOutputs(
    llvm::IRBuilderBase &ir, llvm::Function *function, const RecordLayout &layout, const std::vector<uint32_t> &indices,
    llvm::Value *record
) {
  auto *ret_type = function->getReturnType();
  llvm::Value *ret = llvm::UndefValue::get(ret_type);
  auto load = [&](uint32_t offset) {
    return ir.CreateLoad(ir.getInt32Ty(), ir.CreateGEP(ir.getInt32Ty(), record, {ir.getInt32(offset)}));
  };
  for (uint32_t k = 0; k < layout.outputs.size(); k++) {
    auto &output = layout.outputs[k];
    auto *element_type = ret_type->getStructElementType(indices[k]);
    if (std::get_if<air::OutputPosition>(&output)) {
      ret = ir.CreateInsertValue(ret, FromLanes(ir, element_type, {load(0), load(1), load(2), load(3)}), {indices[k]});
    } else if (auto *data = std::get_if<air::OutputMeshData>(&output)) {
      const uint32_t base = 4 * (1 + data->index);
      ret = ir.CreateInsertValue(ret, FromLanes(ir, element_type, {load(base), load(base + 1), load(base + 2), load(base + 3)}), {indices[k]});
    } else if (std::get_if<air::OutputClipDistance>(&output)) {
      llvm::Value *array = llvm::UndefValue::get(element_type);
      for (uint32_t c = 0; c < layout.clip_count; c++)
        array = ir.CreateInsertValue(array, ir.CreateBitCast(load(layout.clip_slot * 4 + c), ir.getFloatTy()), {c});
      ret = ir.CreateInsertValue(ret, array, {indices[k]});
    }
  }
  return ret;
}

} // namespace dxmt::dxbc::tess_prepass
