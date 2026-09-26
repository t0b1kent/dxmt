// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "wave_native_match.hpp"

// Opt-in native-wave compute substitution table (MACRUNNER_DX12_NATIVE_WAVE=1).
//
// The private MacRunner build carried two entries here: compute shaders and
// root signatures extracted from a commercial game's files, plus Metal
// libraries built from them. That is copyrighted, game-derived data and is
// deliberately NOT part of this public tree. The table is empty, so
// MatchShader() never matches and every compute pipeline takes the regular
// DXBC -> AIR conversion path, whether or not the environment variable is set.
namespace dxmt::native_wave {
inline constexpr const Entry *entries = nullptr;
inline constexpr size_t entry_count = 0;
} // namespace dxmt::native_wave
