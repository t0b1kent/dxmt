/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md */
/* Private MacRunner: reject incomplete or unsupported entry-point binding before dispatch. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static NTSTATUS
_MTLDevice_newReflectedComputePipeline(void *obj) {
  struct unixcall_mtl_reflected_compute *p = (struct unixcall_mtl_reflected_compute *)obj;
  p->ret = 0;
  p->status = WMTArgumentStatusInvalid;
  struct WMTComputeBindingLayout *out = (struct WMTComputeBindingLayout *)p->layout.ptr;
  if (!out) return STATUS_SUCCESS;
  memset(out, 0, sizeof(*out));
  if (!p->device || !p->function) return STATUS_SUCCESS;
  id<MTLComputePipelineState> pipeline = nil;
  MTLComputePipelineDescriptor *descriptor = nil;
  @autoreleasepool {
    @try {
      id<MTLDevice> device = (id<MTLDevice>)p->device;
      id<MTLFunction> function = (id<MTLFunction>)p->function;
      if (function.device != device || function.functionType != MTLFunctionTypeKernel)
        return STATUS_SUCCESS;
      descriptor = [[MTLComputePipelineDescriptor alloc] init];
      descriptor.computeFunction = function;
      MTLComputePipelineReflection *reflection = nil;
      NSError *error = nil;
      pipeline = [device newComputePipelineStateWithDescriptor:descriptor
          options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
          reflection:&reflection error:&error];
      if (!pipeline || !reflection) {
        p->status = WMTArgumentStatusFailed;
        return STATUS_SUCCESS;
      }
      struct WMTComputeBindingLayout layout;
      memset(&layout, 0, sizeof(layout));
      layout.max_threads = pipeline.maxTotalThreadsPerThreadgroup;
      if (!layout.max_threads || !reflection.arguments) {
        p->status = WMTArgumentStatusUnsupported;
        return STATUS_SUCCESS;
      }
      for (MTLArgument *argument in reflection.arguments) {
        if (!argument.active) continue;
        if (layout.count == WMTMaxComputeBindings || argument.arrayLength > 1) {
          p->status = WMTArgumentStatusUnsupported;
          return STATUS_SUCCESS;
        }
        struct WMTComputeBinding binding;
        memset(&binding, 0, sizeof(binding));
        binding.index = argument.index;
        binding.access = argument.access;
        if (binding.access > MTLBindingAccessWriteOnly) {
          p->status = WMTArgumentStatusUnsupported;
          return STATUS_SUCCESS;
        }
        if (argument.type == MTLArgumentTypeBuffer) {
          if (binding.index > 30 || !argument.bufferAlignment) return STATUS_SUCCESS;
          binding.kind = argument.bufferPointerType.elementIsArgumentBuffer ?
              WMTComputeBindingArgumentBuffer : WMTComputeBindingBuffer;
          binding.alignment = argument.bufferAlignment;
          binding.minimum_size = argument.bufferDataSize;
        } else if (argument.type == MTLArgumentTypeTexture) {
          if (binding.index > 127) return STATUS_SUCCESS;
          binding.kind = WMTComputeBindingTexture;
          binding.texture_type = argument.textureType;
          binding.texture_data_type = argument.textureDataType;
          binding.depth = argument.isDepthTexture;
        } else if (argument.type == MTLArgumentTypePrimitiveAccelerationStructure ||
                   argument.type == MTLArgumentTypeInstanceAccelerationStructure) {
          if (binding.index > 30 || binding.access != MTLBindingAccessReadOnly) return STATUS_SUCCESS;
          binding.kind = argument.type == MTLArgumentTypePrimitiveAccelerationStructure ?
              WMTComputeBindingPrimitiveAccelerationStructure : WMTComputeBindingInstanceAccelerationStructure;
        } else {
          // Dynamic threadgroup memory, function tables and direct samplers need dedicated transport.
          p->status = WMTArgumentStatusUnsupported;
          return STATUS_SUCCESS;
        }
        for (uint64_t i = 0; i < layout.count; ++i)
          if ((layout.bindings[i].kind == WMTComputeBindingTexture) ==
              (binding.kind == WMTComputeBindingTexture) && layout.bindings[i].index == binding.index)
            return STATUS_SUCCESS;
        layout.bindings[layout.count++] = binding;
      }
      *out = layout;
      p->ret = (obj_handle_t)pipeline;
      pipeline = nil;
      p->status = WMTArgumentStatusReady;
    } @catch (NSException *exception) {
      (void)exception;
      memset(out, 0, sizeof(*out));
      p->status = WMTArgumentStatusFailed;
    } @finally {
      [pipeline release];
      [descriptor release];
    }
  }
  return STATUS_SUCCESS;
}
static unsigned
wmt_texture_numeric_domain(MTLPixelFormat format, bool *depth) {
  *depth = false;
  switch (format) {
  case MTLPixelFormatR8Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRGBA8Uint:
  case MTLPixelFormatR16Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRGBA16Uint:
  case MTLPixelFormatR32Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRGBA32Uint:
    return 1;
  case MTLPixelFormatR8Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRGBA8Sint:
  case MTLPixelFormatR16Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA16Sint:
  case MTLPixelFormatR32Sint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA32Sint:
    return 2;
  case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float:
  case MTLPixelFormatDepth24Unorm_Stencil8: case MTLPixelFormatDepth32Float_Stencil8:
    *depth = true;
    return 3;
  case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm: case MTLPixelFormatRG8Unorm:
  case MTLPixelFormatRG8Snorm: case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
  case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
  case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Float:
  case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Float:
  case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Float:
  case MTLPixelFormatR32Float: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA32Float:
  case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float:
  case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC1_RGBA_sRGB:
  case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC2_RGBA_sRGB:
  case MTLPixelFormatBC3_RGBA: case MTLPixelFormatBC3_RGBA_sRGB:
  case MTLPixelFormatBC4_RUnorm: case MTLPixelFormatBC4_RSnorm:
  case MTLPixelFormatBC5_RGUnorm: case MTLPixelFormatBC5_RGSnorm:
  case MTLPixelFormatBC6H_RGBFloat: case MTLPixelFormatBC6H_RGBUfloat:
  case MTLPixelFormatBC7_RGBAUnorm: case MTLPixelFormatBC7_RGBAUnorm_sRGB:
    return 3;
  default: return 0;
  }
}

#pragma clang diagnostic pop

static NTSTATUS
_MTLDevice_validateComputeBindings(void *obj) {
  struct unixcall_mtl_validate_compute *p = (struct unixcall_mtl_validate_compute *)obj;
  p->status = WMTArgumentStatusInvalid;
  if (!p->device || p->count > WMTMaxComputeBindings ||
      (p->count && (!p->requirements.ptr || !p->bindings.ptr))) return STATUS_SUCCESS;
  const struct WMTComputeBinding *requirements = (const struct WMTComputeBinding *)p->requirements.ptr;
  const struct WMTArgumentBinding *bindings = (const struct WMTArgumentBinding *)p->bindings.ptr;
  @autoreleasepool {
    @try {
      id<MTLDevice> device = (id<MTLDevice>)p->device;
      for (uint64_t i = 0; i < p->count; ++i) {
        const struct WMTComputeBinding *r = &requirements[i];
        const struct WMTArgumentBinding *b = &bindings[i];
        if (!b->resource || r->index != b->index || r->access > MTLBindingAccessWriteOnly)
          return STATUS_SUCCESS;
        if (r->kind == WMTComputeBindingBuffer) {
          if (r->index > 30 || b->kind != WMTArgumentKindBuffer) return STATUS_SUCCESS;
          id<MTLBuffer> resource = (id<MTLBuffer>)b->resource;
          if (resource.device != device || !r->alignment || b->offset % r->alignment ||
              !b->length || b->length < r->minimum_size ||
              b->offset > resource.length || b->length > resource.length - b->offset)
            return STATUS_SUCCESS;
        } else if (r->kind == WMTComputeBindingTexture) {
          if (r->index > 127 || b->kind != WMTArgumentKindTexture || b->offset || b->length)
            return STATUS_SUCCESS;
          id<MTLTexture> resource = (id<MTLTexture>)b->resource;
          if (resource.device != device || resource.textureType != r->texture_type)
            return STATUS_SUCCESS;
          bool depth = false;
          const unsigned domain = wmt_texture_numeric_domain(resource.pixelFormat, &depth);
          const unsigned expected = r->texture_data_type == MTLDataTypeUInt ? 1 :
              r->texture_data_type == MTLDataTypeInt ? 2 :
              r->texture_data_type == MTLDataTypeFloat || r->texture_data_type == MTLDataTypeHalf ? 3 : 0;
          if (!domain || !expected) {
            p->status = WMTArgumentStatusUnsupported;
            return STATUS_SUCCESS;
          }
          if (domain != expected || depth != (r->depth != 0)) return STATUS_SUCCESS;
          MTLTextureUsage needed = r->access == MTLBindingAccessReadOnly ? MTLTextureUsageShaderRead :
              r->access == MTLBindingAccessWriteOnly ? MTLTextureUsageShaderWrite :
              MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
          if (resource.usage && (resource.usage & needed) != needed) return STATUS_SUCCESS;
        } else if (r->kind == WMTComputeBindingPrimitiveAccelerationStructure ||
                   r->kind == WMTComputeBindingInstanceAccelerationStructure) {
          // The typed producer tracks BLAS/TLAS kind; Metal exposes no descriptor-type query.
          if (r->index > 30 || (uint64_t)b->kind != (uint64_t)r->kind || r->access ||
              r->alignment || r->minimum_size || r->texture_type || r->texture_data_type || r->depth ||
              b->offset || b->length) return STATUS_SUCCESS;
          id<MTLAccelerationStructure> resource = (id<MTLAccelerationStructure>)b->resource;
          if (resource.device != device || resource.heap || !resource.size) return STATUS_SUCCESS;
        } else {
          p->status = WMTArgumentStatusUnsupported;
          return STATUS_SUCCESS;
        }
      }
      p->status = WMTArgumentStatusReady;
    } @catch (NSException *exception) {
      (void)exception;
      p->status = WMTArgumentStatusFailed;
    }
  }
  return STATUS_SUCCESS;
}
