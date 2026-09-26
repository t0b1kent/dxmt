// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "d3d12_gs_point.hpp"
#include <array>
#include <cstring>
#include <span>
#include <vector>

namespace dxmt::gs_contract_reader {
enum class Status { Success, Malformed, Unsupported };
inline Status Read(std::span<const uint8_t> bytes, gs_point::Contract &contract) {
  using namespace microsoft;
  if (bytes.size() < 44 || bytes.size() > 2 * 1024 * 1024) return Status::Malformed;
  // Inspect aligned owned storage; native admission must never accept DXIL as DXBC.
  std::vector<uint32_t> aligned((bytes.size() + 3) / 4);
  memcpy(aligned.data(), bytes.data(), bytes.size());
  CDXBCParser parser;
  if (FAILED(parser.ReadDXBC(aligned.data(), bytes.size())) ||
      parser.FindNextMatchingBlob(DXBC_DXIL) != DXBC_BLOB_NOT_FOUND) return Status::Malformed;
  auto code = parser.FindNextMatchingBlob(DXBC_GenericShaderEx);
  if (code == DXBC_BLOB_NOT_FOUND) code = parser.FindNextMatchingBlob(DXBC_GenericShader);
  if (code == DXBC_BLOB_NOT_FOUND || parser.GetBlobSize(code) % 4) return Status::Malformed;
  CSignatureParser input; CSignatureParser5 output;
  if (FAILED(DXBCGetInputSignature(aligned.data(), &input)) ||
      FAILED(DXBCGetOutputSignature(aligned.data(), &output)) || output.NumStreams() != 1 ||
      output.RasterizedStream() != 0 || !output.Signature(0)) return Status::Malformed;
  std::array<uint8_t, 32> inputs{}, outputs{};
  for (unsigned out = 0; out < 2; ++out) {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    const auto count = out ? output.Signature(0)->GetParameters(&parameters) : input.GetParameters(&parameters);
    auto &masks = out ? outputs : inputs;
    for (UINT i = 0; i < count; ++i) {
      const auto &p = parameters[i];
      if (p.Register >= 32 || !p.Mask || (p.Mask & ~15u) || (masks[p.Register] & p.Mask) || p.Stream)
        return Status::Malformed;
      masks[p.Register] |= p.Mask;
    }
  }
  std::vector<uint32_t> words(parser.GetBlobSize(code) / 4);
  memcpy(words.data(), parser.GetBlob(code), parser.GetBlobSize(code));
  return gs_point::validate(words, inputs, outputs, contract) ? Status::Success : Status::Unsupported;
}
}
