/* SPDX-License-Identifier: Zlib. From DXVK src/dxso, Copyright (c) 2017-2021 Philip Rebohle, Copyright (c) 2019-2021 Joshua Ashton; licence text in LICENSE.dxvk. ALTERED SOURCE VERSION, not the original software: adapted in 2026 for HyperBridge DX9 by Timur Ravilov (edits marked HBDX9). */ #pragma once

#include "dxso_include.h"

#include <vector>
#include <cstdint>

namespace dxvk {

  /**
   * \brief DXBC code iterator
   * 
   * Convenient pointer wrapper that allows
   * reading the code token stream.
   */
  class DxsoCodeIter {
    
  public:
    
    DxsoCodeIter(
      const uint32_t* ptr)
    : m_ptr(ptr) { }
    
    const uint32_t* ptrAt(uint32_t id) const;
    
    uint32_t at(uint32_t id) const;
    uint32_t read();
    
    DxsoCodeIter skip(uint32_t n) const;
    
  private:
    
    const uint32_t* m_ptr = nullptr;
    
  };


}