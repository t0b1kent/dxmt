// SPDX-License-Identifier: LGPL-2.1-or-later
// Included inside MTLD3D12CommandQueueImpl. Values describe recorded commands.
void TraceMergePass(CommittingScope &scope, const void *list, EncoderData *pass) {
  if (!merge_trace::SubmitWindow()) return;
  const auto identity = uint64_t(reinterpret_cast<uintptr_t>(pass));
  auto emit_resource = [&](const char *op, unsigned index, const merge_trace::Resource &r,
      uint64_t offset, uint64_t bytes, uint64_t view, unsigned role, unsigned access) {
    merge_trace::Emit(state_.get(), scope.seq, list, identity, op,
        "index=%u object=%llx native=%llx address=%llx resource_bytes=%llu kind=%u offset=%llu bytes=%llu view=%llx role=%u access=%u",
        index, (unsigned long long)r.object, (unsigned long long)r.native,
        (unsigned long long)r.address, (unsigned long long)r.bytes, r.kind,
        (unsigned long long)offset, (unsigned long long)bytes, (unsigned long long)view, role, access);
  };
  merge_trace::Emit(state_.get(), scope.seq, list, identity, "pass.begin", "type=%u id=%llu",
      unsigned(pass->type), (unsigned long long)pass->id);
  if (pass->type == EncoderType::Render) {
    const auto &r = *static_cast<RenderEncoderData *>(pass);
    const auto &record = r.merge_trace;
    merge_trace::Emit(state_.get(), scope.seq, list, identity, "render",
        "draws=%llu unrecorded=%llu unknown=%llu uses=%llu dropped=%llu failed=%llu shader_access=BOUND_ONLY descriptor_time=RECORD alias_coverage=UNKNOWN",
        (unsigned long long)record.draws, (unsigned long long)record.unrecorded, (unsigned long long)record.unknown,
        (unsigned long long)record.uses.rows.size(), (unsigned long long)record.uses.dropped, (unsigned long long)record.uses.failed);
    auto attachment = [&](const TextureViewRef &view, unsigned slot, unsigned role, unsigned access,
                          unsigned load, unsigned store, unsigned plane) {
      if (!view) return;
      merge_trace::Resource resource;
      resource.object = uint64_t(reinterpret_cast<uintptr_t>(view->allocation->descriptor));
      resource.native = view->allocation->texture().handle;
      resource.kind = 2;
      emit_resource("attachment", slot, resource, 0, 0, uint64_t(view->key), role, access);
      merge_trace::Emit(state_.get(), scope.seq, list, identity, "attachment.actions",
          "index=%u role=%u load=%u store=%u depth_plane=%u", slot, role, load, store, plane);
    };
    for (unsigned i = 0; i < r.render_target_count; ++i)
      if (r.colors[i].attachment)
        attachment(r.colors[i].attachment, i, 1, 3, r.colors[i].load_action, r.colors[i].store_action, r.colors[i].depth_plane);
    if (r.depth.attachment) attachment(r.depth.attachment, 8, 2, (r.dsv_readonly_flags & 1) ? 1 : 3,
        r.depth.load_action, r.depth.store_action, r.depth.depth_plane);
    if (r.stencil.attachment) attachment(r.stencil.attachment, 9, 2, (r.dsv_readonly_flags & 2) ? 1 : 3,
        r.stencil.load_action, r.stencil.store_action, r.stencil.depth_plane);
    for (unsigned i = 0; i < record.uses.rows.size(); ++i) {
      const auto &use = record.uses.rows[i];
      emit_resource("use", i, use.resource, use.offset, use.bytes, use.view, use.role, use.access);
    }
  } else if (pass->type == EncoderType::Blit) {
    const auto &blit = *static_cast<BlitEncoderData *>(pass);
    const auto &record = blit.merge_trace;
    merge_trace::Emit(state_.get(), scope.seq, list, identity, "barriers",
        "barrier_only=%u requested=%llu unrecorded=%llu count=%llu dropped=%llu failed=%llu",
        unsigned(blit.barrier_only), (unsigned long long)record.requested, (unsigned long long)record.unrecorded,
        (unsigned long long)record.values.rows.size(), (unsigned long long)record.values.dropped,
        (unsigned long long)record.values.failed);
    for (unsigned i = 0; i < record.values.rows.size(); ++i) {
      const auto &barrier = record.values.rows[i];
      merge_trace::Emit(state_.get(), scope.seq, list, identity, "barrier",
          "index=%u type=%u flags=%u before=%x after=%x subresource=%u",
          i, barrier.type, barrier.flags, barrier.before, barrier.after, barrier.subresource);
      emit_resource("barrier.a", i, barrier.a, 0, 0, 0, 0, 0);
      emit_resource("barrier.b", i, barrier.b, 0, 0, 0, 0, 0);
    }
  }
  merge_trace::Emit(state_.get(), scope.seq, list, identity, "pass.end", "type=%u", unsigned(pass->type));
}
