// MacRunner shader-prewarm 17.10.2026: select с дорогой стороной → ветви (проход над IR DXIL до перевода в AIR).
// Мотив: dxc сворачивает `if` над выборками в select, и обе стороны исполняются (b73fe303: 2 br/20 select, ×3 медленнее нашего
// MSL-пути, где SPIRV-Cross печатал тернарник с выборками внутри и фронтенд Metal ставил ветвь). Правило (куратор 17.10):
// переносить только то, что безопасно в неоднородной ветви — явный LOD, grad, загрузки, gather; неявный LOD (sample, sampleBias,
// sampleCmp, calculateLOD) — только при условии, однородном в кваде (константы/cbuffer).
#pragma once
#include "llvm/IR/Function.h"
#include "../dxil_metadata.hpp"

namespace dxmt::dxil {

struct SelectToBranchStats {
  unsigned candidates = 0, converted = 0, skipped_implicit_lod = 0, skipped_cheap = 0, skipped_unmovable = 0, skipped_order = 0, moved_instructions = 0,
           grad_rewrites = 0, converted_via_grad = 0;
};

bool SelectToBranchEnabled();                       // MACRUNNER_AIRCONV_SELECT_TO_BRANCH=0 выключает (для A/B на стенде)
// Entry (может быть nullptr) нужен для подъёма производных: вид текстуры по createHandle → число компонент градиента.
SelectToBranchStats SelectToBranch(llvm::Function &F, const EntryPoint *Entry = nullptr);

} // namespace dxmt::dxil
