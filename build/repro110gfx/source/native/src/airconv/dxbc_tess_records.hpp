// Bounded mode-3 localization of the observed ER color mismatch. Raw records stay on this GPU.
// The mesh observer stores declared HS outputs/PC, domain coordinates, and declared DS output lanes.
// Pull compares the same workload/local vertex; missing or differently ordered records are explicit counters.
#pragma once

namespace dxmt::dxbc {

struct TessRecordScalar { uint32_t reg, component, counter; };
inline std::vector<TessRecordScalar> TessRecordSignature(SM50ShaderInternal *shader, bool output) {
  std::vector<TessRecordScalar> values;
  if (shader->dxil) {
    const auto &elements=output ? shader->dxil_entry.outputs : shader->dxil_entry.inputs;
    for (const auto &e:elements) {
      if (e.start_row<0 || e.start_col<0) continue;
      const unsigned counter=e.semantic_kind==dxil::SemanticKind::Position ? 14 :
          e.semantic_kind==dxil::SemanticKind::ClipDistance ? 16 : 15;
      for (uint32_t row=0;row<std::max(1u,e.rows);++row)
        for (uint32_t c=0;c<e.cols;++c) if (uint32_t(e.start_col)+c<4)
          values.push_back({uint32_t(e.start_row)+row,uint32_t(e.start_col)+c,counter});
    }
  } else {
    const auto &elements=output ? shader->output_signature : shader->input_signature;
    for (const auto &s:elements) {
      const unsigned counter=s.systemValue()==microsoft::D3D10_SB_NAME_POSITION ? 14 :
          s.systemValue()==microsoft::D3D10_SB_NAME_CLIP_DISTANCE ? 16 : 15;
      for (uint32_t c=0;c<4;++c) if (s.mask()&(1u<<c)) values.push_back({s.reg(),c,counter});
    }
  }
  return values;
}

inline uint32_t TessRecordScalars(SM50ShaderInternal *hs, SM50ShaderInternal *ds) {
  uint32_t n = 8 + uint32_t(hs->patch_constant_scalars.size());
  for (const auto &s : TessRecordSignature(ds,false))
    if (s.reg < hs->max_output_register) n += hs->output_control_point_count;
  for (const auto &s : TessRecordSignature(ds,true))
    if (s.reg < ds->max_output_register) ++n;
  return n;
}

inline void EmitTessRecords(
    llvm::IRBuilder<> &ir, llvm::Function *fn, io_binding_map &resources, air::AirType &types,
    SM50ShaderInternal *hs, SM50ShaderInternal *ds, llvm::Value *buffer, llvm::Value *group,
    llvm::Value *workload, llvm::Value *vertex, bool mesh) {
  auto &context = fn->getContext();
  auto block = [&](const char *name) { return llvm::BasicBlock::Create(context, name, fn); };
  auto *bounds = block("record_bounds"), *body = block("record_body"), *over = block("record_over"), *end = block("record_end");
  auto ptr = [&](llvm::Value *base, uint32_t i) { return ir.CreateGEP(types._int, base, {ir.getInt32(i)}); };
  auto head = [&](uint32_t i) { return ir.CreateLoad(types._int, ptr(buffer, i)); };
  auto add = [&](uint32_t index, llvm::Value *value) {
    ir.CreateAtomicRMW(llvm::AtomicRMWInst::Add, ptr(buffer, index), value, llvm::MaybeAlign(4), llvm::AtomicOrdering::Monotonic);
  };
  ir.CreateCondBr(ir.CreateICmpNE(head(0), ir.getInt32(0)), bounds, end);
  ir.SetInsertPoint(bounds);
  auto *nw = head(2), *nv = head(3), *stride = head(4);
  auto *valid = ir.CreateAnd(ir.CreateICmpULT(group, head(1)),
      ir.CreateAnd(ir.CreateICmpULT(workload, nw), ir.CreateICmpULT(vertex, nv)));
  ir.CreateCondBr(valid, body, over);
  ir.SetInsertPoint(over); add(mesh ? 17 : 18, ir.getInt32(1)); ir.CreateBr(end);
  ir.SetInsertPoint(body);
  auto *index = ir.CreateAdd(ir.CreateMul(ir.CreateAdd(ir.CreateMul(group, nw), workload), nv), vertex);
  auto *record = ir.CreateGEP(types._int, buffer, {ir.CreateAdd(ir.getInt32(32), ir.CreateMul(index, stride))});
  add(mesh ? 8 : 9, ir.getInt32(1));
  const auto compare = [&](uint32_t offset, llvm::Value *value, uint32_t counter) {
    if (mesh) ir.CreateStore(counter >= 14 ? ir.CreateXor(value, head(6)) : value, ptr(record, offset));
    else {
      auto *expected = ir.CreateLoad(types._int, ptr(record, offset));
      auto *different = ir.CreateICmpNE(expected, value);
      add(counter, ir.CreateZExt(different, types._int));
      if (counter >= 14) {
        // Only aggregate FP32 interpretations leave the GPU. These buckets distinguish small arithmetic
        // differences from changed resources/values; they do not assert that every signature lane is FP32.
        auto *finite = ir.CreateAnd(
            ir.CreateICmpNE(ir.CreateAnd(expected, ir.getInt32(0x7f800000)), ir.getInt32(0x7f800000)),
            ir.CreateICmpNE(ir.CreateAnd(value, ir.getInt32(0x7f800000)), ir.getInt32(0x7f800000)));
        const auto ordered = [&](llvm::Value *v) {
          return ir.CreateXor(v, ir.CreateSelect(ir.CreateICmpSLT(v, ir.getInt32(0)),
              ir.getInt32(0xffffffff), ir.getInt32(0x80000000)));
        };
        auto *a = ordered(expected), *b = ordered(value);
        auto *distance = ir.CreateSub(ir.CreateSelect(ir.CreateICmpUGT(a,b),a,b),
            ir.CreateSelect(ir.CreateICmpUGT(a,b),b,a));
        auto *valid_diff = ir.CreateAnd(different,finite);
        const unsigned kind = counter == 14 ? 0 : 1;
        ir.CreateAtomicRMW(llvm::AtomicRMWInst::UMax, ptr(buffer,22+kind),
            ir.CreateSelect(valid_diff,distance,ir.getInt32(0)),llvm::MaybeAlign(4),llvm::AtomicOrdering::Monotonic);
        add(24+kind,ir.CreateZExt(ir.CreateAnd(valid_diff,ir.CreateICmpULE(distance,ir.getInt32(4))),types._int));
        add(26+kind,ir.CreateZExt(ir.CreateAnd(valid_diff,ir.CreateICmpUGT(distance,ir.getInt32(4))),types._int));
        add(28,ir.CreateZExt(ir.CreateAnd(different,ir.CreateNot(finite)),types._int));
        auto *delta = ir.CreateFSub(ir.CreateBitCast(expected,types._float),ir.CreateBitCast(value,types._float));
        auto *magnitude = ir.CreateBitCast(ir.CreateAnd(ir.CreateBitCast(delta,types._int),ir.getInt32(0x7fffffff)),types._float);
        add(30+kind,ir.CreateZExt(ir.CreateAnd(valid_diff,
            ir.CreateFCmpOLE(magnitude,llvm::ConstantFP::get(types._float,1e-5))),types._int));
      }
    }
  };
  if (!mesh) add(10, ir.CreateZExt(ir.CreateICmpNE(ir.CreateLoad(types._int, record), ir.getInt32(0x54524431)), types._int));
  compare(1, resources.patch_id, 11);
  for (unsigned c = 0; c < 3; ++c)
    compare(2 + c, ir.CreateBitCast(ir.CreateExtractElement(resources.domain, uint64_t(c)), types._int), 11);
  uint32_t offset = 8;
  const auto cp_type = resources.input.ptr_int4->getType()->getNonOpaquePointerElementType();
  for (unsigned cp = 0; cp < hs->output_control_point_count; ++cp)
    for (const auto &s : TessRecordSignature(ds,false)) {
      if (s.reg >= hs->max_output_register) continue;
        auto *src = ir.CreateGEP(cp_type, resources.input.ptr_int4, {ir.getInt32(0), ir.getInt32(cp), ir.getInt32(s.reg), ir.getInt32(s.component)});
        compare(offset++, ir.CreateLoad(types._int, src), 12);
        if (!mesh) add(21, ir.getInt32(1));
    }
  // Only declared patch constants: unused register padding is not evidence.
  auto *pc_type = resources.patch_constant_output.ptr_int4->getType()->getNonOpaquePointerElementType();
  for (const auto &s : hs->patch_constant_scalars) {
    if (s.reg >= ds->max_input_register) continue;
    auto *src = ir.CreateGEP(pc_type, resources.patch_constant_output.ptr_int4,
        {ir.getInt32(0), ir.getInt32(s.reg), ir.getInt32(s.component)});
    compare(offset++, ir.CreateLoad(types._int, src), 13);
    if (!mesh) add(21, ir.getInt32(1));
  }
  auto *out_type = resources.output.ptr_int4->getType()->getNonOpaquePointerElementType();
  for (const auto &s : TessRecordSignature(ds,true)) {
    if (s.reg >= ds->max_output_register) continue;
      auto *src = ir.CreateGEP(out_type, resources.output.ptr_int4,
          {ir.getInt32(0), ir.getInt32(s.reg), ir.getInt32(s.component)});
      compare(offset++, ir.CreateLoad(types._int, src), s.counter);
      if (!mesh) add(20, ir.getInt32(1));
  }
  if (mesh) ir.CreateStore(ir.getInt32(0x54524431), record);
  ir.CreateBr(end); ir.SetInsertPoint(end);
}

} // namespace dxmt::dxbc
