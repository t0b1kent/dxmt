/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md */
/* Private MacRunner integration: function-reflected, per-submit argument storage.
 * Unsupported layouts fail closed. This does not encode AS, constants or nested arrays.
 * The caller must retain every referenced resource and its residency until GPU completion.
 */
// Function reflection preserves the existing macOS baseline; newer binding APIs need PSO reflection.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static NTSTATUS
_MTLFunction_newArgumentBuffer(void *obj) {
  struct unixcall_mtlfunction_newargumentbuffer *p = (struct unixcall_mtlfunction_newargumentbuffer *)obj;
  p->ret = 0;
  p->status = WMTArgumentStatusInvalid;
  struct WMTBufferInfo *info = (struct WMTBufferInfo *)p->info.ptr;
  if (!info) return STATUS_SUCCESS;
  memset(info, 0, sizeof(*info));
  if (!p->function || p->index > 30 || !p->bindings.ptr || !p->count || p->count > 256)
    return STATUS_SUCCESS;
  const struct WMTArgumentBinding *bindings = (const struct WMTArgumentBinding *)p->bindings.ptr;
  id<MTLArgumentEncoder> encoder = nil;
  id<MTLBuffer> buffer = nil;
  @autoreleasepool {
    @try {
      id<MTLFunction> function = (id<MTLFunction>)p->function;
      id<MTLDevice> device = function.device;
      if (!device || function.functionType != MTLFunctionTypeKernel) {
        p->status = WMTArgumentStatusUnsupported;
        return STATUS_SUCCESS;
      }
      MTLArgument *reflection = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      encoder = [function newArgumentEncoderWithBufferIndex:p->index reflection:&reflection];
#pragma clang diagnostic pop
      if (!encoder || !reflection || reflection.type != MTLArgumentTypeBuffer ||
          reflection.index != p->index || reflection.bufferDataType != MTLDataTypeStruct ||
          !reflection.bufferStructType) {
        p->status = WMTArgumentStatusUnsupported;
        return STATUS_SUCCESS;
      }
      const NSUInteger length = encoder.encodedLength, alignment = encoder.alignment;
      if (!length || length > 16 * 1024 * 1024 || !alignment || (alignment & (alignment - 1))) {
        p->status = WMTArgumentStatusUnsupported;
        return STATUS_SUCCESS;
      }
      NSArray<MTLStructMember *> *members = reflection.bufferStructType.members;
      if (members.count != p->count) return STATUS_SUCCESS;
      // Exact bijection: no omitted, duplicate, extra or silently uninitialized member.
      for (uint64_t i = 0; i < p->count; ++i) {
        const struct WMTArgumentBinding *b = &bindings[i];
        if (!b->resource || b->kind > WMTArgumentKindSampler) return STATUS_SUCCESS;
        for (uint64_t j = 0; j < i; ++j)
          if (bindings[j].index == b->index) return STATUS_SUCCESS;
        MTLStructMember *member = nil;
        for (MTLStructMember *m in members) {
          if (m.argumentIndex != b->index) continue;
          if (member) return STATUS_SUCCESS;
          member = m;
        }
        if (!member) return STATUS_SUCCESS;
        if (member.dataType != MTLDataTypePointer && member.dataType != MTLDataTypeTexture &&
            member.dataType != MTLDataTypeSampler) {
          p->status = WMTArgumentStatusUnsupported;
          return STATUS_SUCCESS;
        }
        if (b->kind == WMTArgumentKindBuffer) {
          if (member.dataType != MTLDataTypePointer) return STATUS_SUCCESS;
          MTLPointerType *pointer = member.pointerType;
          if (!pointer || pointer.elementIsArgumentBuffer) {
            p->status = WMTArgumentStatusUnsupported;
            return STATUS_SUCCESS;
          }
          id<MTLBuffer> resource = (id<MTLBuffer>)b->resource;
          if (resource.device != device || !b->length || !pointer.alignment ||
              b->offset % pointer.alignment || b->length < pointer.dataSize ||
              b->offset > resource.length || b->length > resource.length - b->offset)
            return STATUS_SUCCESS;
        } else if (b->kind == WMTArgumentKindTexture) {
          if (member.dataType != MTLDataTypeTexture || b->offset || b->length)
            return STATUS_SUCCESS;
          MTLTextureReferenceType *type = member.textureReferenceType;
          id<MTLTexture> resource = (id<MTLTexture>)b->resource;
          if (!type || resource.device != device || resource.textureType != type.textureType)
            return STATUS_SUCCESS;
          const MTLTextureUsage needed = type.access == MTLBindingAccessReadOnly ? MTLTextureUsageShaderRead :
              type.access == MTLBindingAccessWriteOnly ? MTLTextureUsageShaderWrite :
              MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
          if (resource.usage && (resource.usage & needed) != needed) return STATUS_SUCCESS;
        } else {
          if (member.dataType != MTLDataTypeSampler || b->offset || b->length)
            return STATUS_SUCCESS;
          id<MTLSamplerState> resource = (id<MTLSamplerState>)b->resource;
          if (resource.device != device || !resource.gpuResourceID._impl) return STATUS_SUCCESS;
        }
      }
      buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
      if (!buffer) {
        p->status = WMTArgumentStatusOutOfMemory;
        return STATUS_SUCCESS;
      }
      void *memory = buffer.contents;
      if (!memory || buffer.length < length || !buffer.gpuAddress || buffer.gpuAddress % alignment) {
        p->status = WMTArgumentStatusFailed;
        return STATUS_SUCCESS;
      }
      memset(memory, 0, length);
      [encoder setArgumentBuffer:buffer offset:0];
      for (uint64_t i = 0; i < p->count; ++i) {
        const struct WMTArgumentBinding *b = &bindings[i];
        switch (b->kind) {
        case WMTArgumentKindBuffer:
          [encoder setBuffer:(id<MTLBuffer>)b->resource offset:b->offset atIndex:b->index];
          break;
        case WMTArgumentKindTexture:
          [encoder setTexture:(id<MTLTexture>)b->resource atIndex:b->index];
          break;
        case WMTArgumentKindSampler:
          [encoder setSamplerState:(id<MTLSamplerState>)b->resource atIndex:b->index];
          break;
        case WMTArgumentKindPrimitiveAccelerationStructure:
        case WMTArgumentKindInstanceAccelerationStructure:
          p->status = WMTArgumentStatusUnsupported;
          return STATUS_SUCCESS;
        }
      }
      info->length = length;
      info->options = WMTResourceStorageModeShared;
      // Native pointer is not exposed to 32-bit callers: this buffer is already encoded.
      info->gpu_address = buffer.gpuAddress;
      p->ret = (obj_handle_t)buffer;
      buffer = nil;
      p->status = WMTArgumentStatusReady;
    } @catch (NSException *exception) {
      (void)exception;
      memset(info, 0, sizeof(*info));
      p->ret = 0;
      p->status = WMTArgumentStatusFailed;
    } @finally {
      [buffer release];
      [encoder release];
    }
  }
  return STATUS_SUCCESS;
}
#pragma clang diagnostic pop
