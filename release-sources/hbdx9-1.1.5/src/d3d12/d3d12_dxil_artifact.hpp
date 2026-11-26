// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

// Diagnostic transport for externally compiled shaders, not a DXIL compiler.
// The original bytes travel with the result so lookup hashes are not identities.
static bool
AlignDXBCForInspection(D3D12_SHADER_BYTECODE input, std::vector<uint8_t> &aligned, bool require_aligned = false) {
  if (!input.pShaderBytecode || input.BytecodeLength < 32 || input.BytecodeLength > 16 * 1024 * 1024) return false;
  const auto *bytes = static_cast<const uint8_t *>(input.pShaderBytecode);
  if (memcmp(bytes, "DXBC", 4)) return false;
  const auto u32 = [&](size_t offset) { uint32_t value; memcpy(&value, bytes + offset, 4); return value; };
  const uint32_t count = u32(28);
  if (u32(24) != input.BytecodeLength || count > 64 || size_t(32) + count * 4 > input.BytecodeLength) return false;
  const size_t table_end = 32 + count * 4;
  aligned.assign(bytes, bytes + table_end);
  std::vector<std::pair<size_t, size_t>> ranges;
  for (unsigned i = 0; i < count; ++i) {
    const size_t offset = u32(32 + i * 4);
    if (offset < table_end || offset > input.BytecodeLength - 8) return false;
    if (require_aligned && offset % 4) return false;
    const size_t size = size_t(u32(offset + 4)) + 8;
    if (size > input.BytecodeLength - offset) return false;
    for (const auto &range : ranges)
      if (offset < range.second && range.first < offset + size) return false;
    ranges.emplace_back(offset, offset + size);
    aligned.resize((aligned.size() + 3) & ~size_t(3), 0);
    const uint32_t new_offset = uint32_t(aligned.size());
    memcpy(aligned.data() + 32 + i * 4, &new_offset, 4);
    aligned.insert(aligned.end(), bytes + offset, bytes + offset + size);
    const uint32_t padded_payload_size = uint32_t((size - 8 + 3) & ~size_t(3));
    aligned.resize(size_t(new_offset) + 8 + padded_payload_size, 0);
    memcpy(aligned.data() + new_offset + 4, &padded_payload_size, 4);
  }
  const uint32_t new_size = uint32_t(aligned.size());
  memcpy(aligned.data() + 24, &new_size, 4);
  // Inspection only: payload bytes are retained with zero tail padding because
  // DXBCParser requires contiguous chunks. The checksum is not regenerated;
  // this copy is never compiled or cached.
  return true;
}

static HRESULT
LoadDiagnosticDXILArtifact(D3D12_SHADER_BYTECODE original, unsigned stage, std::vector<uint8_t> &storage,
                           D3D12_SHADER_BYTECODE &result) {
  using namespace microsoft;
  struct PreserveLastError {
    DWORD value = GetLastError();
    ~PreserveLastError() { SetLastError(value); }
  } preserve_last_error;
  result = original;
  char directory[4096];
  DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_DXIL_ARTIFACT_DIR", directory, sizeof(directory));
  if (!n) return S_FALSE;
  if (n >= sizeof(directory)) return E_INVALIDARG;
  CDXBCParser source;
  if (!original.pShaderBytecode || !original.BytecodeLength) return S_FALSE;
  std::vector<uint8_t> source_inspection;
  if (!AlignDXBCForInspection(original, source_inspection) ||
      FAILED(source.ReadDXBC(source_inspection.data(), source_inspection.size()))) return E_INVALIDARG;
  if (source.FindNextMatchingBlob(DXBC_DXIL) == DXBC_BLOB_NOT_FOUND) return S_FALSE;
  if (original.BytecodeLength > 256 * 1024) return E_NOTIMPL;
  const auto key = Sha1HashState::compute(original.pShaderBytecode, original.BytecodeLength);
  char hex[41];
  constexpr char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 20; ++i) { hex[2*i] = digits[key.data[i] >> 4]; hex[2*i+1] = digits[key.data[i] & 15]; }
  hex[40] = 0;
  std::string path = std::string(directory) + "\\" + hex + ".dxil-dxbc";
  HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return E_NOTIMPL;
  LARGE_INTEGER length;
  // Whole-corpus FXC outputs include 26 valid CS containers above 256 KiB
  // (largest 1,469,676 bytes). Keep an explicit bounded transport limit.
  constexpr uint32_t max_target_bytes = 2 * 1024 * 1024;
  if (!GetFileSizeEx(file, &length) || length.QuadPart < 40 ||
      length.QuadPart > 40 + 256 * 1024 + max_target_bytes) {
    CloseHandle(file);
    return E_INVALIDARG;
  }
  storage.resize(size_t(length.QuadPart));
  DWORD read = 0;
  const bool complete = ReadFile(file, storage.data(), DWORD(storage.size()), &read, nullptr) && read == storage.size();
  CloseHandle(file);
  if (!complete || memcmp(storage.data(), "DXILBC01", 8)) return E_INVALIDARG;
  uint32_t version, source_size, target_size;
  memcpy(&version, storage.data() + 8, 4);
  memcpy(&source_size, storage.data() + 12, 4);
  memcpy(&target_size, storage.data() + 16, 4);
  if (version != 1 || source_size != original.BytecodeLength || !target_size || target_size > max_target_bytes ||
      size_t(40) + source_size + target_size != storage.size() ||
      memcmp(storage.data() + 40, original.pShaderBytecode, source_size)) return E_INVALIDARG;
  const uint8_t *target = storage.data() + 40 + source_size;
  const auto digest = Sha1HashState::compute(target, target_size);
  if (memcmp(digest.data, storage.data() + 20, 20)) return E_INVALIDARG;
  memmove(storage.data(), target, target_size);
  storage.resize(target_size);
  target = storage.data();
  std::vector<uint8_t> target_inspection;
  if (!AlignDXBCForInspection({target, target_size}, target_inspection, true)) return E_INVALIDARG;
  CDXBCParser compiled;
  if (FAILED(compiled.ReadDXBC(target, target_size)) ||
      compiled.FindNextMatchingBlob(DXBC_DXIL) != DXBC_BLOB_NOT_FOUND) return E_INVALIDARG;
  auto code = compiled.FindNextMatchingBlob(DXBC_GenericShaderEx);
  if (code == DXBC_BLOB_NOT_FOUND) code = compiled.FindNextMatchingBlob(DXBC_GenericShader);
  if (code == DXBC_BLOB_NOT_FOUND || compiled.GetBlobSize(code) < 8 || compiled.GetBlobSize(code) % 4)
    return E_INVALIDARG;
  std::vector<uint32_t> words(compiled.GetBlobSize(code) / 4);
  memcpy(words.data(), compiled.GetBlob(code), compiled.GetBlobSize(code));
  const uint32_t token = words[0];
  if ((token >> 16) != stage || (token & 255) != 0x50) return E_NOTIMPL;
  if (words[1] != words.size()) return E_INVALIDARG;
  for (size_t at = 2; at < words.size();) {
    const uint32_t op = words[at] & D3D10_SB_OPCODE_TYPE_MASK;
    if (op >= D3D10_SB_NUM_OPCODES || op == D3D10_SB_OPCODE_RESERVED0 ||
        op == D3D10_1_SB_OPCODE_RESERVED1 || op == D3D11_SB_OPCODE_RESERVED0 ||
        op == D3D11_1_SB_OPCODE_RESERVED0 || op == D3DWDDM1_3_SB_OPCODE_RESERVED0)
      return E_INVALIDARG;
    size_t count = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(words[at]);
    if (op == D3D10_SB_OPCODE_CUSTOMDATA) {
      if (words.size() - at < 2 || words[at + 1] < 2) return E_INVALIDARG;
      count = words[at + 1];
    }
    if (!count || count > words.size() - at) return E_INVALIDARG;
    if (op != D3D10_SB_OPCODE_CUSTOMDATA) {
      size_t extended = at;
      while (words[extended] & 0x80000000u)
        if (++extended >= at + count) return E_INVALIDARG;
    }
    at += count;
  }

  CSignatureParser source_input, target_input;
  CSignatureParser source_output, target_output;
  CSignatureParser5 source_geometry_output, target_geometry_output;
  const CSignatureParser *source_output_signature = &source_output;
  const CSignatureParser *target_output_signature = &target_output;
  if (FAILED(DXBCGetInputSignature(source_inspection.data(), &source_input)) ||
      FAILED(DXBCGetInputSignature(target, &target_input))) return E_INVALIDARG;
  if (stage == D3D10_SB_GEOMETRY_SHADER) {
    // GS SM5 output may use OSG5. Keep the stream-aware owners alive through comparison.
    if (FAILED(DXBCGetOutputSignature(source_inspection.data(), &source_geometry_output)) ||
        FAILED(DXBCGetOutputSignature(target, &target_geometry_output)) ||
        source_geometry_output.NumStreams() != 1 || target_geometry_output.NumStreams() != 1)
      return E_INVALIDARG;
    source_output_signature = source_geometry_output.Signature(0);
    target_output_signature = target_geometry_output.Signature(0);
    const auto stream_zero_only = [](const CSignatureParser &signature, UINT total) {
      const D3D11_SIGNATURE_PARAMETER *parameters;
      const UINT count = signature.GetParameters(&parameters);
      if (!count || count != total) return false;
      for (UINT i = 0; i < count; ++i)
        if (parameters[i].Stream != 0) return false;
      return true;
    };
    if (!stream_zero_only(*source_output_signature, source_geometry_output.GetTotalParameters()) ||
        !stream_zero_only(*target_output_signature, target_geometry_output.GetTotalParameters()))
      return E_INVALIDARG;
  } else if (FAILED(DXBCGetOutputSignature(source_inspection.data(), &source_output)) ||
             FAILED(DXBCGetOutputSignature(target, &target_output))) return E_INVALIDARG;
  const auto compatible = [stage](const CSignatureParser &a, const CSignatureParser &b, bool output) {
    const D3D11_SIGNATURE_PARAMETER *sa, *sb;
    const auto na = a.GetParameters(&sa), nb = b.GetParameters(&sb);
    if (na > 32 || nb > 32) return false;
    // FXC may repack a semantic between components (for example .w -> .x).
    // Compare lanes relative to that semantic; retain holes and required lanes.
    const auto relative = [](unsigned mask, unsigned declared) {
      if (!declared) return mask;
      while (!(declared & 1)) { declared >>= 1; mask >>= 1; }
      return mask;
    };
    for (unsigned j = 0; j < nb; ++j) {
      unsigned matches = 0;
      for (unsigned i = 0; i < na; ++i)
        if (!strcasecmp(sa[i].SemanticName, sb[j].SemanticName) && sa[i].SemanticIndex == sb[j].SemanticIndex &&
            sa[i].SystemValue == sb[j].SystemValue && sa[i].ComponentType == sb[j].ComponentType &&
            sa[i].MinPrecision == sb[j].MinPrecision && sa[i].Stream == sb[j].Stream &&
            !(relative(sb[j].Mask, sb[j].Mask) & ~relative(sa[i].Mask, sa[i].Mask))) ++matches;
      if (matches != 1) return false;
    }
    for (unsigned i = 0; i < na; ++i) {
      // Only unused MRT exports may disappear. Other output contracts remain strict.
      const bool unused_mrt = output && stage == D3D10_SB_PIXEL_SHADER &&
        !strcasecmp(sa[i].SemanticName, "SV_Target");
      const unsigned needed = output ? (unused_mrt ? sa[i].Mask & ~sa[i].NeverWrites_Mask : sa[i].Mask)
                                     : sa[i].AlwaysReads_Mask;
      if (!needed) continue;
      unsigned matches = 0;
      for (unsigned j = 0; j < nb; ++j)
        if (!strcasecmp(sa[i].SemanticName, sb[j].SemanticName) && sa[i].SemanticIndex == sb[j].SemanticIndex &&
            !(relative(needed, sa[i].Mask) & ~relative(sb[j].Mask, sb[j].Mask))) ++matches;
      if (matches != 1) return false;
    }
    return true;
  };
  if (!compatible(source_input, target_input, false) ||
      !compatible(*source_output_signature, *target_output_signature, true)) return E_INVALIDARG;
  result = {storage.data(), target_size};
  return S_OK;
}
