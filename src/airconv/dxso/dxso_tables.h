/* SPDX-License-Identifier: Zlib. From DXVK src/dxso, Copyright (c) 2017-2021 Philip Rebohle, Copyright (c) 2019-2021 Joshua Ashton; licence text in LICENSE.dxvk. ALTERED SOURCE VERSION, not the original software: adapted in 2026 for HyperBridge DX9 by Timur Ravilov (edits marked HBDX9). */ #pragma once

#include "dxso_enums.h"

namespace dxvk {

  constexpr uint32_t InvalidOpcodeLength = UINT32_MAX;

  uint32_t DxsoGetDefaultOpcodeLength(DxsoOpcode opcode);

}