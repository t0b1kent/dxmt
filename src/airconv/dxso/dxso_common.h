/* SPDX-License-Identifier: Zlib. From DXVK src/dxso, Copyright (c) 2017-2021 Philip Rebohle, Copyright (c) 2019-2021 Joshua Ashton; licence text in LICENSE.dxvk. ALTERED SOURCE VERSION, not the original software: adapted in 2026 for HyperBridge DX9 by Timur Ravilov (edits marked HBDX9). */ /* HBDX9: DXVK dxso_common.h without the Vulkan/SPIR-V accessors. */
#pragma once
#include "dxso_include.h"
namespace dxvk {
  namespace DxsoProgramTypes {
    enum DxsoProgramType : uint16_t { VertexShader = 0, PixelShader = 1, Count = 2 };
  }
  using DxsoProgramType = DxsoProgramTypes::DxsoProgramType;
  class DxsoProgramInfo {
  public:
    DxsoProgramInfo() { }
    DxsoProgramInfo(DxsoProgramType type, uint32_t minorVersion, uint32_t majorVersion)
      : m_type{ type }, m_minorVersion{ minorVersion }, m_majorVersion{ majorVersion } {}
    DxsoProgramType type() const { return m_type; }
    uint32_t minorVersion() const { return m_minorVersion; }
    uint32_t majorVersion() const { return m_majorVersion; }
  private:
    DxsoProgramType m_type = DxsoProgramTypes::VertexShader;
    uint32_t        m_minorVersion = 0;
    uint32_t        m_majorVersion = 0;
  };
}
