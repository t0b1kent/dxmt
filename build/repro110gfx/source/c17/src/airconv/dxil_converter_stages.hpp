// MacRunner dxil-tess 17.10.2026: ступени HS/DS/GS своего DXIL→AIR поверх существующих конвертеров
// dxbc_converter_ts.cpp / dxbc_converter_gs.cpp (object/mesh ABI прежнего пути). Поля SM50ShaderInternal
// заполняются из метаданных DXIL (HSState/DSState/GSState и подписи), тело — dxil::Converter; у HS две функции:
// main (фаза контрольных точек, по потоку на выходную КТ) и patch-constant (один поток на патч).
#pragma once

#include "airconv_public.h"
#include "dxil_metadata.hpp"
#include "stage_linkage.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/Error.h"

#include <string>
#include <vector>

namespace dxmt::dxbc {
class SM50ShaderInternal;
struct context;
} // namespace dxmt::dxbc

namespace dxmt::dxil {

llvm::Error initialize_hull_shader(dxmt::dxbc::SM50ShaderInternal *shader, const Info &info);
llvm::Error initialize_domain_shader(dxmt::dxbc::SM50ShaderInternal *shader, const Info &info);
llvm::Error initialize_geometry_shader(dxmt::dxbc::SM50ShaderInternal *shader, const Info &info);

// Отражение для HS/DS/GS (как SM50Initialize для DXBC: подгонка threads-per-patch и max tess factor по размеру payload).
llvm::Error fill_stage_reflection(dxmt::dxbc::SM50ShaderInternal *shader, MTL_SHADER_REFLECTION *pRefl);

// Тело HS из DXIL внутри object-функции (зона HS в convert_dxbc_vertex_hull_shader): возвращает блок входа.
llvm::Expected<llvm::BasicBlock *>
convert_hull_shader_body(dxmt::dxbc::SM50ShaderInternal *hull, dxmt::dxbc::context &ctx, llvm::BasicBlock *epilogue_bb);

// Строки линковки VS→HS по подписи DXIL (имя+индекс семантики, регистр по объявленной упаковке).
// output = true для выходов VS (usage = «никогда не пишется» = 0), false для входов HS (usage = маска чтения).
std::vector<dxmt::stage_linkage::Row> linkage_rows(const std::vector<SignatureElement> &elements, bool output);

// Наибольший регистр (+1) по подписи DXIL.
uint32_t max_register(const std::vector<SignatureElement> &elements);

} // namespace dxmt::dxil
