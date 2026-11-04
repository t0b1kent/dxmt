// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "d3d12_dxil_psv.hpp"
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
  if (FAILED(parser.ReadDXBC(aligned.data(), bytes.size()))) return Status::Malformed;
  // MacRunner dxil-tess 17.10: контейнер DXIL — договор из PSV0 (примитив, топология, поток, число вершин); тело
  // проверяет airconv. Число экземпляров PSV0 не несёт; договор держит 1 — на диспетчеризацию оно не влияет (mesh-сетку
  // примитивы × экземпляры ставит object-функция airconv по GSState), airconv проверяет предел 32.
  const bool dxil = parser.FindNextMatchingBlob(DXBC_DXIL) != DXBC_BLOB_NOT_FOUND;
  auto code = dxil ? DXBC_BLOB_NOT_FOUND : parser.FindNextMatchingBlob(DXBC_GenericShaderEx);
  if (!dxil && code == DXBC_BLOB_NOT_FOUND) code = parser.FindNextMatchingBlob(DXBC_GenericShader);
  if (!dxil && (code == DXBC_BLOB_NOT_FOUND || parser.GetBlobSize(code) % 4)) return Status::Malformed;
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
  if (dxil) {
    dxil_psv::Info info;
    if (!dxil_psv::Read(parser, info) || !info.has_psv || info.kind != dxil_psv::Geometry) return Status::Malformed;
    contract = {};
    contract.primitive = info.gs_input_primitive;
    contract.max_vertices = info.gs_max_vertex_count;
    contract.stream_mask = info.gs_stream_mask;
    contract.topology = info.gs_output_topology;
    contract.instances = 1;
    if (!gs_point::input_vertices(contract.primitive) || !contract.max_vertices || contract.max_vertices > 256 ||
        contract.stream_mask != 1 ||
        (contract.topology != D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST && contract.topology != D3D10_SB_PRIMITIVE_TOPOLOGY_LINESTRIP &&
         contract.topology != D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP))
      return Status::Unsupported;
    return Status::Success;
  }
  std::vector<uint32_t> words(parser.GetBlobSize(code) / 4);
  memcpy(words.data(), parser.GetBlob(code), parser.GetBlobSize(code));
  if (gs_point::validate(words, inputs, outputs, contract)) return Status::Success;
  // MacRunner dxil-tess 17.10: outside the strict list contract the declarations alone give the draw path what it needs
  // (input primitive, vertices, instances); the shape limits are those of the full path, the body is the translator's.
  if (!gs_point::read_declarations(words, contract)) return Status::Malformed;
  const char *reason = nullptr;
  return gs_point::shape_within_limits(contract, outputs, reason) ? Status::Success : Status::Unsupported;
}
}
