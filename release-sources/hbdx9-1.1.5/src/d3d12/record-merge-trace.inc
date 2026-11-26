// SPDX-License-Identifier: LGPL-2.1-or-later
// Included inside MTLD3D12GraphicsCommandListImpl. No rendering decisions here.
static merge_trace::Resource MergeResource(Buffer *buffer, Texture *texture) {
  merge_trace::Resource out;
  if (buffer) {
    const auto allocation = buffer->current();
    out = {uint64_t(reinterpret_cast<uintptr_t>(buffer)), allocation->buffer().handle,
           allocation->gpuAddress(), buffer->length(), 1};
  } else if (texture) {
    out = {uint64_t(reinterpret_cast<uintptr_t>(texture)), texture->current()->texture().handle, 0, 0, 2};
  }
  return out;
}
static merge_trace::Resource MergeResource(ID3D12Resource *resource) {
  if (!resource) return {};
  auto r = static_cast<MTLD3D12Resource *>(resource);
  return MergeResource(r->buffer.ptr(), r->texture.ptr());
}
void RecordMergeBarriers(BlitEncoderData *pass, UINT count, const D3D12_RESOURCE_BARRIER *barriers) noexcept {
  if (!merge_trace::Target()) return;
  auto &record = pass->merge_trace;
  record.requested += count;
  if (!merge_trace::RecordWindow()) { record.unrecorded += count; return; }
  const DWORD error = GetLastError();
  for (UINT i = 0; i < count; ++i) {
    if (record.values.rows.size() >= 256) { record.values.dropped += count - i; break; }
    const auto &b = barriers[i];
    merge_trace::Barrier row;
    row.type = uint32_t(b.Type); row.flags = uint32_t(b.Flags);
    if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
      row.a = MergeResource(b.Transition.pResource);
      row.before = uint32_t(b.Transition.StateBefore); row.after = uint32_t(b.Transition.StateAfter);
      row.subresource = b.Transition.Subresource;
    } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
      row.a = MergeResource(b.UAV.pResource);
    } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
      row.a = MergeResource(b.Aliasing.pResourceBefore); row.b = MergeResource(b.Aliasing.pResourceAfter);
    }
    record.values.Add(row);
  }
  SetLastError(error);
}
void RecordMergeDraw(bool indirect) noexcept {
  if (!merge_trace::Target()) return;
  auto &record = static_cast<RenderEncoderData *>(allocator_->encoder_current)->merge_trace;
  ++record.draws;
  if (!merge_trace::RecordWindow()) { ++record.unrecorded; return; }
  const DWORD error = GetLastError();
  try {
    // Only descriptors bound at recording are observed. GPU-dynamic shader use,
    // alias placement, and descriptor mutation after recording remain UNKNOWN.
    if (indirect) ++record.unknown;
    auto buffer = [&](uint64_t address, uint64_t bytes, uint32_t role, uint32_t access) {
      if (!address || !bytes) return;
      uint64_t offset = 0;
      auto source = device_->LookupCaptureByVA(address, bytes, offset);
      if (!source) { ++record.unknown; return; }
      merge_trace::Use row;
      row.resource = MergeResource(source->native->buffer.ptr(), nullptr);
      row.offset = offset; row.bytes = bytes; row.role = role; row.access = access;
      record.Add(row);
    };
    for (UINT slot = 0; slot < 32; ++slot)
      if (pso_graphics_->slot_mask & (uint32_t(1) << slot)) {
        const auto &view = vertex_buffers_[slot];
        buffer(view.BufferLocation, view.SizeInBytes, 6, 1);
      }
    // Conservative: a bound IB is reported even for a nonindexed draw.
    buffer(index_buffer_address, index_buffer_size, 7, 1);
    const auto *root = rootsig_graphics_.ptr();
    if (!root || root->RayBindingStatus != ray_binding::Status::Ready) { ++record.unknown; }
    else {
      uint32_t visited = 0;
      for (const auto &span : root->RayBindings.spans) {
        if (span.type == 3 || span.source == ray_binding::Source::Constants) continue;
        const uint32_t role = span.type == 0 ? 3 : span.type == 1 ? 4 : 5;
        const uint32_t access = span.type == 1 ? 3 : 1;
        if (span.source != ray_binding::Source::Table) {
          const auto kind = span.type == 0 ? root_argument::SRV : span.type == 1 ? root_argument::UAV : root_argument::CBV;
          uint64_t address = 0, offset = 0;
          if (!rootarg_graphics_staging_.ReadValue(root->ArgumentLayout, span.parameter, kind, address)) {
            ++record.unknown; continue;
          }
          auto source = address ? device_->LookupCaptureByVA(address, 1, offset) : nullptr;
          if (!source || !source->native->buffer || offset >= source->native->logical_length) { ++record.unknown; continue; }
          // Root descriptors have no bounds: retain the remaining allocation,
          // explicitly as a potential use, not a shader footprint.
          buffer(address, source->native->logical_length - offset, role, access);
          continue;
        }
        const uint64_t count = span.end - span.first;
        if (count > 256 || visited + count > 256) { ++record.unknown; continue; }
        for (uint32_t i = 0; i < count; ++i) {
          ++visited;
          D3D12DescriptorCapture descriptor;
          if (FAILED(CaptureBoundDescriptor(false, span.parameter, span.offset + i, descriptor))) {
            ++record.unknown; continue;
          }
          if (!descriptor.resource) {
            if (descriptor.type != ShaderVisibleDescriptorType::Null) ++record.unknown;
            continue;
          }
          const auto &native = *descriptor.resource->native;
          merge_trace::Use row;
          row.resource = MergeResource(native.buffer.ptr(), native.texture.ptr());
          row.offset = descriptor.buffer_offset; row.bytes = descriptor.byte_length;
          row.view = descriptor.texture ? uint64_t(descriptor.texture->key) : 0;
          row.role = role; row.access = access;
          record.Add(row);
          if (descriptor.counter) {
            merge_trace::Use counter;
            counter.resource = MergeResource(descriptor.counter->native->buffer.ptr(), nullptr);
            counter.offset = descriptor.counter_offset; counter.bytes = 4; counter.role = 8; counter.access = 3;
            record.Add(counter);
          }
        }
      }
    }
  } catch (...) { ++record.uses.failed; }
  SetLastError(error);
}
