/* SPDX-License-Identifier: Zlib. From DXVK src/dxso, Copyright (c) 2017-2021 Philip Rebohle, Copyright (c) 2019-2021 Joshua Ashton; licence text in LICENSE.dxvk. ALTERED SOURCE VERSION, not the original software: adapted in 2026 for HyperBridge DX9 by Timur Ravilov (edits marked HBDX9). */ #include "dxso_code.h"

namespace dxvk {


  const uint32_t* DxsoCodeIter::ptrAt(uint32_t id) const {
    return m_ptr + id;
  }


  uint32_t DxsoCodeIter::at(uint32_t id) const {
    return m_ptr[id];
  }


  uint32_t DxsoCodeIter::read() {
    return *(m_ptr++);
  }

  DxsoCodeIter DxsoCodeIter::skip(uint32_t n) const {
    return DxsoCodeIter(m_ptr + n);
  }

}