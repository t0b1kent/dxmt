#ifndef WMT_TRACE_DESCRIPTORS_H
#define WMT_TRACE_DESCRIPTORS_H
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

/* Current presenter supplies scalar Bool constants from PE uint32_t storage.
 * Read Metal's scalar width, not producer pointer/sizeof wrapper struct.
 * Composite types need an explicit layout audit; reject instead of guessing. */
static NSUInteger wmt_trace_constant_scalar_size(MTLDataType type) {
  switch (type) {
    case MTLDataTypeFloat: case MTLDataTypeInt: case MTLDataTypeUInt: return 4;
    case MTLDataTypeHalf: case MTLDataTypeShort: case MTLDataTypeUShort: return 2;
    case MTLDataTypeChar: case MTLDataTypeUChar: case MTLDataTypeBool: return 1;
    default: return 0;
  }
}

/* Explicit allowlists: no runtime reflection, addresses, labels or blanket
 * descriptor byte dumps. NSNumber preserves enum, BOOL and float values. */
static NSDictionary *wmt_trace_descriptor_fields(id descriptor, NSArray *keys) {
  NSMutableDictionary *fields = [NSMutableDictionary dictionary];
  for (NSString *key in keys) fields[key] = [descriptor valueForKey:key];
  return fields;
}
static NSDictionary *wmt_trace_texture_fields(MTLTextureDescriptor *desc) {
  return wmt_trace_descriptor_fields(desc, @[@"textureType", @"pixelFormat",
      @"width", @"height", @"depth", @"arrayLength", @"mipmapLevelCount",
      @"sampleCount", @"usage", @"resourceOptions"]);
}
static NSDictionary *wmt_trace_sampler_fields(MTLSamplerDescriptor *desc) {
  return wmt_trace_descriptor_fields(desc, @[@"borderColor", @"rAddressMode",
      @"sAddressMode", @"tAddressMode", @"magFilter", @"minFilter", @"mipFilter",
      @"compareFunction", @"lodMaxClamp", @"lodMinClamp", @"maxAnisotropy",
      @"lodAverage", @"normalizedCoordinates", @"supportArgumentBuffers"]);
}
static NSDictionary *wmt_trace_stencil_fields(MTLStencilDescriptor *desc) {
  return wmt_trace_descriptor_fields(desc, @[@"depthStencilPassOperation",
      @"depthFailureOperation", @"stencilFailureOperation", @"stencilCompareFunction",
      @"writeMask", @"readMask"]);
}
static NSDictionary *wmt_trace_pipeline_scalars(MTLRenderPipelineDescriptor *desc) {
  return wmt_trace_descriptor_fields(desc, @[@"depthAttachmentPixelFormat",
      @"stencilAttachmentPixelFormat", @"alphaToCoverageEnabled", @"alphaToOneEnabled",
      @"rasterizationEnabled", @"rasterSampleCount", @"inputPrimitiveTopology",
      @"tessellationPartitionMode", @"tessellationFactorStepFunction",
      @"tessellationOutputWindingOrder", @"maxTessellationFactor",
      @"tessellationFactorFormat", @"tessellationControlPointIndexType",
      @"tessellationFactorScaleEnabled"]);
}
static NSDictionary *wmt_trace_pipeline_color(MTLRenderPipelineColorAttachmentDescriptor *desc) {
  return wmt_trace_descriptor_fields(desc, @[@"pixelFormat", @"blendingEnabled", @"writeMask",
      @"alphaBlendOperation", @"rgbBlendOperation", @"sourceRGBBlendFactor",
      @"sourceAlphaBlendFactor", @"destinationRGBBlendFactor", @"destinationAlphaBlendFactor"]);
}
/* Function IDs supplied by the generation registry. No function pointer is serialized.
 * Binary archives are compilation caches; replay recompiles the recorded functions.
 * Private logic operations and vertex layouts require their own explicit codec. */
static NSDictionary *wmt_trace_pipeline_fields(MTLRenderPipelineDescriptor *desc,
    uint64_t vertex, uint64_t fragment) {
  BOOL has_vertex_layout = NO;
  for (NSUInteger i = 0; desc.vertexDescriptor && i < 31; ++i)
    has_vertex_layout |= desc.vertexDescriptor.attributes[i].format != MTLVertexFormatInvalid ||
        desc.vertexDescriptor.layouts[i].stride != 0;
  if (has_vertex_layout || (desc.vertexFunction && !vertex) ||
      (desc.fragmentFunction && !fragment)) return nil;
  NSMutableArray *colors = [NSMutableArray array];
  NSMutableArray *vertex_buffers = [NSMutableArray array];
  NSMutableArray *fragment_buffers = [NSMutableArray array];
  for (NSUInteger i = 0; i < 8; ++i)
    [colors addObject:wmt_trace_pipeline_color(desc.colorAttachments[i])];
  for (NSUInteger i = 0; i < 31; ++i) {
    [vertex_buffers addObject:@(desc.vertexBuffers[i].mutability)];
    [fragment_buffers addObject:@(desc.fragmentBuffers[i].mutability)];
  }
  return @{@"scalars": wmt_trace_pipeline_scalars(desc), @"colors": colors,
      @"vertex": @(vertex), @"fragment": @(fragment),
      @"vertexBuffers": vertex_buffers, @"fragmentBuffers": fragment_buffers};
}
static NSDictionary *wmt_trace_depth_fields(MTLDepthStencilDescriptor *desc) {
  return @{@"depthCompareFunction": @(desc.depthCompareFunction),
      @"depthWriteEnabled": @(desc.depthWriteEnabled),
      @"frontFaceStencil": wmt_trace_stencil_fields(desc.frontFaceStencil),
      @"backFaceStencil": wmt_trace_stencil_fields(desc.backFaceStencil)};
}
#endif
