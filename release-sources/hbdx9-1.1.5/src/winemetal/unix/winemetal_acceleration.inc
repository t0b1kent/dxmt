/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md */
/* Private MacRunner: normalized AS build bridge; no public DXR feature admission. */
static bool
wmt_as_span(uint64_t count, uint64_t stride, uint64_t element, uint64_t *bytes) {
  if (!count || !stride || stride < element || count - 1 > (UINT64_MAX - element) / stride)
    return false;
  *bytes = (count - 1) * stride + element;
  return true;
}
static bool
wmt_as_buffer(id<MTLDevice> device, obj_handle_t handle, uint64_t offset, uint64_t bytes) {
  if (!handle || !bytes) return false;
  id<MTLBuffer> buffer = (id<MTLBuffer>)handle;
  return buffer.device == device && !buffer.heap && offset <= buffer.length && bytes <= buffer.length - offset;
}
// Autoreleased descriptor and resources are consumed synchronously. No borrowed CPU arrays escape.
static MTLAccelerationStructureDescriptor *
wmt_as_descriptor(id<MTLDevice> device, const struct WMTASBuildDesc *in,
                  NSMutableArray *resources, enum WMTArgumentStatus *status) {
  *status = WMTArgumentStatusInvalid;
  if (!in || !device) return nil;
  if (!device.supportsRaytracing || in->flags) {
    *status = WMTArgumentStatusUnsupported;
    return nil;
  }
  if (in->kind == WMTASBuildTriangles) {
    if (!in->geometries.ptr || !in->geometry_count || in->geometry_count > 4096 ||
        in->instance_buffer || in->instance_offset || in->instance_count || in->instance_stride ||
        in->acceleration_structures.ptr || in->acceleration_structure_count) return nil;
    const struct WMTASTriangleGeometry *geometry = (const struct WMTASTriangleGeometry *)in->geometries.ptr;
    NSMutableArray *list = [NSMutableArray arrayWithCapacity:in->geometry_count];
    if (!list) { *status = WMTArgumentStatusOutOfMemory; return nil; }
    for (uint64_t i = 0; i < in->geometry_count; ++i) {
      const struct WMTASTriangleGeometry *g = &geometry[i];
      uint64_t bytes;
      if (!g->triangle_count || g->triangle_count > UINT32_MAX / 3 || g->vertex_count > UINT32_MAX ||
          g->vertex_stride < 12 || g->vertex_stride % 4 || g->vertex_offset % g->vertex_stride ||
          g->opaque > 1 || g->intersection_function_offset > UINT32_MAX ||
          !wmt_as_span(g->vertex_count, g->vertex_stride, 12, &bytes) ||
          !wmt_as_buffer(device, g->vertex_buffer, g->vertex_offset, bytes)) return nil;
      if (g->index_type > WMTASIndexUInt32) { *status = WMTArgumentStatusUnsupported; return nil; }
      if (g->index_type == WMTASIndexNone) {
        if (g->index_buffer || g->index_offset || g->vertex_count < g->triangle_count * 3) return nil;
      } else {
        const uint64_t width = g->index_type == WMTASIndexUInt16 ? 2 : 4;
        if (g->index_offset % width ||
            !wmt_as_buffer(device, g->index_buffer, g->index_offset, g->triangle_count * 3 * width)) return nil;
        [resources addObject:(id)g->index_buffer];
      }
      MTLAccelerationStructureTriangleGeometryDescriptor *d = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
      if (!d) { *status = WMTArgumentStatusOutOfMemory; return nil; }
      d.vertexBuffer = (id<MTLBuffer>)g->vertex_buffer;
      d.vertexBufferOffset = g->vertex_offset;
      d.vertexStride = g->vertex_stride; // Metal's default vertex format is packed float3.
      d.indexBuffer = (id<MTLBuffer>)g->index_buffer;
      d.indexBufferOffset = g->index_offset;
      d.indexType = g->index_type == WMTASIndexUInt16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
      d.triangleCount = g->triangle_count;
      d.opaque = g->opaque != 0;
      d.intersectionFunctionTableOffset = g->intersection_function_offset;
      d.allowDuplicateIntersectionFunctionInvocation = NO;
      [resources addObject:(id)g->vertex_buffer];
      [list addObject:d];
    }
    MTLPrimitiveAccelerationStructureDescriptor *descriptor = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    if (!descriptor) { *status = WMTArgumentStatusOutOfMemory; return nil; }
    descriptor.geometryDescriptors = list;
    *status = WMTArgumentStatusReady;
    return descriptor;
  }
  if (in->kind == WMTASBuildInstances) {
    uint64_t bytes;
    if (in->geometries.ptr || in->geometry_count || !in->acceleration_structures.ptr ||
        !in->acceleration_structure_count || in->acceleration_structure_count > 4096 ||
        in->instance_count > UINT32_MAX || in->instance_offset % 64 || in->instance_stride % 4 ||
        !wmt_as_span(in->instance_count, in->instance_stride,
            sizeof(MTLAccelerationStructureUserIDInstanceDescriptor), &bytes) ||
        !wmt_as_buffer(device, in->instance_buffer, in->instance_offset, bytes)) return nil;
    NSMutableArray *children = [NSMutableArray arrayWithCapacity:in->acceleration_structure_count];
    if (!children) { *status = WMTArgumentStatusOutOfMemory; return nil; }
    const obj_handle_t *handles = (const obj_handle_t *)in->acceleration_structures.ptr;
    for (uint64_t i = 0; i < in->acceleration_structure_count; ++i) {
      if (!handles[i]) return nil;
      id<MTLAccelerationStructure> child = (id<MTLAccelerationStructure>)handles[i];
      if (child.device != device || child.heap || !child.size) return nil;
      [children addObject:child];
      [resources addObject:child];
    }
    MTLInstanceAccelerationStructureDescriptor *descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
    if (!descriptor) { *status = WMTArgumentStatusOutOfMemory; return nil; }
    descriptor.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
    descriptor.instanceDescriptorBuffer = (id<MTLBuffer>)in->instance_buffer;
    descriptor.instanceDescriptorBufferOffset = in->instance_offset;
    descriptor.instanceDescriptorStride = in->instance_stride;
    descriptor.instanceCount = in->instance_count;
    descriptor.instancedAccelerationStructures = children;
    [resources addObject:(id)in->instance_buffer];
    *status = WMTArgumentStatusReady;
    return descriptor;
  }
  *status = WMTArgumentStatusUnsupported;
  return nil;
}
static NTSTATUS
_MTLDevice_accelerationStructureSizes(void *obj) {
  struct unixcall_mtl_as_sizes *p = (struct unixcall_mtl_as_sizes *)obj;
  p->status = WMTArgumentStatusInvalid;
  struct WMTASSizeInfo *out = (struct WMTASSizeInfo *)p->info.ptr;
  if (!out) return STATUS_SUCCESS;
  memset(out, 0, sizeof(*out));
  @autoreleasepool {
    @try {
      id<MTLDevice> device = (id<MTLDevice>)p->device;
      MTLAccelerationStructureDescriptor *descriptor = wmt_as_descriptor(
          device, (const struct WMTASBuildDesc *)p->desc.ptr, nil, &p->status);
      if (!descriptor) return STATUS_SUCCESS;
      MTLAccelerationStructureSizes sizes = [device accelerationStructureSizesWithDescriptor:descriptor];
      if (!sizes.accelerationStructureSize || !sizes.buildScratchBufferSize) {
        p->status = WMTArgumentStatusFailed;
        return STATUS_SUCCESS;
      }
      out->structure_size = sizes.accelerationStructureSize;
      out->build_scratch_size = sizes.buildScratchBufferSize;
      out->refit_scratch_size = sizes.refitScratchBufferSize;
    } @catch (NSException *e) {
      (void)e;
      memset(out, 0, sizeof(*out));
      p->status = WMTArgumentStatusFailed;
    }
  }
  return STATUS_SUCCESS;
}
static NTSTATUS
_MTLDevice_newAccelerationStructure(void *obj) {
  struct unixcall_mtl_as_new *p = (struct unixcall_mtl_as_new *)obj;
  p->ret = 0;
  p->status = WMTArgumentStatusInvalid;
  if (!p->device || !p->size || p->size > NSUIntegerMax) return STATUS_SUCCESS;
  @autoreleasepool {
    @try {
      id<MTLDevice> device = (id<MTLDevice>)p->device;
      if (!device.supportsRaytracing) p->status = WMTArgumentStatusUnsupported;
      else {
        p->ret = (obj_handle_t)[device newAccelerationStructureWithSize:p->size];
        p->status = p->ret ? WMTArgumentStatusReady : WMTArgumentStatusOutOfMemory;
      }
    } @catch (NSException *e) { (void)e; p->status = WMTArgumentStatusFailed; }
  }
  return STATUS_SUCCESS;
}
static NTSTATUS
_MTLCommandBuffer_buildAccelerationStructure(void *obj) {
  struct unixcall_mtl_as_build *p = (struct unixcall_mtl_as_build *)obj;
  p->status = WMTArgumentStatusInvalid;
  if (!p->command_buffer || !p->target || !p->scratch || p->scratch_offset % 256) return STATUS_SUCCESS;
  @autoreleasepool {
    id<MTLAccelerationStructureCommandEncoder> encoder = nil;
    @try {
      id<MTLCommandBuffer> commands = (id<MTLCommandBuffer>)p->command_buffer;
      id<MTLDevice> device = commands.device;
      if (commands.status != MTLCommandBufferStatusNotEnqueued) return STATUS_SUCCESS;
      id<MTLAccelerationStructure> target = (id<MTLAccelerationStructure>)p->target;
      if (target.device != device || target.heap ||
          (p->fence && [(id<MTLFence>)p->fence device] != device)) return STATUS_SUCCESS;
      NSMutableArray *resources = [NSMutableArray array];
      if (!resources) { p->status = WMTArgumentStatusOutOfMemory; return STATUS_SUCCESS; }
      MTLAccelerationStructureDescriptor *descriptor = wmt_as_descriptor(
          device, (const struct WMTASBuildDesc *)p->desc.ptr, resources, &p->status);
      if (!descriptor) return STATUS_SUCCESS;
      // A build may not alias its target, inputs or scratch object.
      if ([(id)p->target isEqual:(id)p->scratch] ||
          [resources containsObject:(id)p->target] || [resources containsObject:(id)p->scratch]) {
        p->status = WMTArgumentStatusInvalid;
        return STATUS_SUCCESS;
      }
      MTLAccelerationStructureSizes sizes = [device accelerationStructureSizesWithDescriptor:descriptor];
      if (!sizes.accelerationStructureSize || !sizes.buildScratchBufferSize ||
          target.size < sizes.accelerationStructureSize ||
          !wmt_as_buffer(device, p->scratch, p->scratch_offset, sizes.buildScratchBufferSize)) {
        p->status = WMTArgumentStatusInvalid;
        return STATUS_SUCCESS;
      }
      [resources addObject:target];
      [resources addObject:(id)p->scratch];
      [resources addObject:descriptor];
      if (p->fence) [resources addObject:(id)p->fence];
      // The copied completion block owns native objects even for an unretained command buffer.
      [commands addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)done; (void)resources; }];
      encoder = [commands accelerationStructureCommandEncoder];
      if (!encoder) { p->status = WMTArgumentStatusFailed; return STATUS_SUCCESS; }
      if (p->fence) [encoder waitForFence:(id<MTLFence>)p->fence];
      [encoder buildAccelerationStructure:target descriptor:descriptor
          scratchBuffer:(id<MTLBuffer>)p->scratch scratchBufferOffset:p->scratch_offset];
      if (p->fence) [encoder updateFence:(id<MTLFence>)p->fence];
      [encoder endEncoding];
      encoder = nil;
      p->status = WMTArgumentStatusReady;
    } @catch (NSException *e) {
      (void)e;
      if (encoder) { @try { [encoder endEncoding]; } @catch (NSException *ignored) { (void)ignored; } }
      p->status = WMTArgumentStatusFailed;
    }
  }
  return STATUS_SUCCESS;
}
