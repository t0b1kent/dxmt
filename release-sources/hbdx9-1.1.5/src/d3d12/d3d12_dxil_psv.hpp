// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
// MacRunner dxil-tess 17.10.2026: объявления ступеней HS/DS/GS из контейнера DXIL на стороне PE без LLVM —
// заголовок программы DXIL (вид ступени) и чанк PSV0 (DxilPipelineStateValidation: счётчики КТ, домен, примитив,
// топология, число вершин GS). Разбиение и max tess factor в PSV0 нет — их даёт отражение airconv (MTL_TESSELLATOR_REFLECTION).
#pragma once
#include "DXBCParser/BlobContainer.h"
#include <cstdint>
#include <cstring>

namespace dxmt::dxil_psv {

enum Kind : uint32_t { Pixel = 0, Vertex = 1, Geometry = 2, Hull = 3, Domain = 4, Compute = 5 };

struct Info {
  uint32_t kind = ~0u; // DXIL::ShaderKind из ProgramVersion заголовка
  bool has_psv = false;
  uint32_t hs_input_control_points = 0, hs_output_control_points = 0;
  uint32_t tessellator_domain = 0, tessellator_output_primitive = 0; // = D3D11_SB_TESSELLATOR_DOMAIN / _OUTPUT_PRIMITIVE
  uint32_t ds_input_control_points = 0;
  uint32_t gs_input_primitive = 0, gs_output_topology = 0, gs_stream_mask = 0, gs_max_vertex_count = 0; // = D3D10_SB_PRIMITIVE / _TOPOLOGY
};

// false — не DXIL-контейнер или заголовок/PSV0 повреждён. PSV0 читается, если есть (has_psv); первые 16 байт
// PSVRuntimeInfo0 — union по ступени (поля стабильны во всех версиях PSV), ShaderStage — байт 24 (с PSVRuntimeInfo1).
inline bool
Read(microsoft::CDXBCParser &parser, Info &info) {
  using namespace microsoft;
  const UINT program = parser.FindNextMatchingBlob(DXBC_DXIL);
  if (program == DXBC_BLOB_NOT_FOUND || parser.GetBlobSize(program) < 24)
    return false;
  uint32_t version;
  memcpy(&version, parser.GetBlob(program), sizeof(version));
  info.kind = (version >> 16) & 0xffffu;
  const UINT psv = parser.FindNextMatchingBlob((DXBCFourCC)((UINT)('P') | ((UINT)('S') << 8) | ((UINT)('V') << 16) | ((UINT)('0') << 24)));
  if (psv == DXBC_BLOB_NOT_FOUND)
    return true;
  const auto size = parser.GetBlobSize(psv);
  if (size < 4)
    return false;
  const uint8_t *bytes = static_cast<const uint8_t *>(parser.GetBlob(psv));
  uint32_t info_size;
  memcpy(&info_size, bytes, sizeof(info_size));
  if (info_size < 24 || 4ull + info_size > size)
    return false;
  const uint8_t *rt = bytes + 4;
  const auto u32 = [&](unsigned at) { uint32_t v; memcpy(&v, rt + at, sizeof(v)); return v; };
  if (info_size >= 36 && rt[24] != info.kind)
    return false; // PSV ShaderStage расходится с заголовком программы
  switch (info.kind) {
  case Hull:
    info.hs_input_control_points = u32(0);
    info.hs_output_control_points = u32(4);
    info.tessellator_domain = u32(8);
    info.tessellator_output_primitive = u32(12);
    break;
  case Domain:
    info.ds_input_control_points = u32(0);
    info.tessellator_domain = u32(8);
    break;
  case Geometry:
    info.gs_input_primitive = u32(0);
    info.gs_output_topology = u32(4);
    info.gs_stream_mask = u32(8);
    if (info_size >= 36) {
      uint16_t max_vertices;
      memcpy(&max_vertices, rt + 26, sizeof(max_vertices));
      info.gs_max_vertex_count = max_vertices;
    }
    break;
  default:
    break;
  }
  info.has_psv = true;
  return true;
}

// Таблица ресурсов PSV0 (PSVResourceBindInfo0: ResType, Space, LowerBound, UpperBound) — для проверки покрытия корнем.
// PSVResourceType: 1 Sampler, 2 CBV, 3 SRV typed, 4 SRV raw, 5 SRV structured, 6 UAV typed, 7 UAV raw, 8 UAV structured,
// 9 UAV structured with counter. Размеров cbuffer в PSV0 нет (их проверяет airconv при компиляции).
struct Resource {
  uint32_t type = 0, space = 0, lower = 0, upper = 0;
};

template <typename Out>
inline bool
ReadResources(microsoft::CDXBCParser &parser, Out &out) {
  using namespace microsoft;
  const UINT psv = parser.FindNextMatchingBlob((DXBCFourCC)((UINT)('P') | ((UINT)('S') << 8) | ((UINT)('V') << 16) | ((UINT)('0') << 24)));
  if (psv == DXBC_BLOB_NOT_FOUND)
    return false;
  const auto size = parser.GetBlobSize(psv);
  const uint8_t *bytes = static_cast<const uint8_t *>(parser.GetBlob(psv));
  const auto u32 = [&](size_t at, uint32_t &v) { if (at + 4 > size) return false; memcpy(&v, bytes + at, 4); return true; };
  uint32_t info_size, count, record;
  if (!u32(0, info_size) || !u32(4ull + info_size, count))
    return false;
  if (!count)
    return true;
  size_t at = 8ull + info_size;
  if (!u32(at, record) || record < 16 || count > 1024)
    return false;
  at += 4;
  for (uint32_t i = 0; i < count; ++i, at += record) {
    Resource r;
    if (!u32(at, r.type) || !u32(at + 4, r.space) || !u32(at + 8, r.lower) || !u32(at + 12, r.upper))
      return false;
    out.push_back(r);
  }
  return true;
}

inline bool
IsDxilContainer(const void *bytecode, size_t length) {
  using namespace microsoft;
  if (!bytecode || !length)
    return false;
  CDXBCParser parser;
  return SUCCEEDED(parser.ReadDXBC(bytecode, (UINT)length)) && parser.FindNextMatchingBlob(DXBC_DXIL) != DXBC_BLOB_NOT_FOUND;
}

} // namespace dxmt::dxil_psv
