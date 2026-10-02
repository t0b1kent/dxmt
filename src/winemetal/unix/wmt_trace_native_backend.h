#ifndef WMT_TRACE_NATIVE_BACKEND_H
#define WMT_TRACE_NATIVE_BACKEND_H
#import <Metal/Metal.h>
#include "../winemetal_thunks.h"
#include <dlfcn.h>
typedef int (*wmt_trace_api_native_fn)(void *);
static const void **wmt_trace_native_api_table;
static void *wmt_trace_native_backend;

/* Called only after CPU validation. Backend is the ARM64 unix implementation
 * relinked against native frameworks; it has no Wine runtime dependencies. */
static BOOL wmt_trace_api_load_native(NSString **failure) {
  const char *path=getenv("MACRUNNER_WMT_NATIVE_BACKEND");
  if(!path||!*path) { *failure=@"missing private native backend path";return NO; }
  if(getenv("MACRUNNER_WMT_RECORD")) {
    *failure=@"record gate must be absent in the native player";return NO;
  }
  wmt_trace_native_backend=dlopen(path,RTLD_NOW|RTLD_LOCAL);
  if(!wmt_trace_native_backend) {
    *failure=[NSString stringWithFormat:@"native backend load: %s",dlerror()];return NO;
  }
  wmt_trace_native_api_table=dlsym(wmt_trace_native_backend,"__wine_unix_call_funcs");
  if(!wmt_trace_native_api_table) { *failure=@"native dispatch table missing";return NO; }
  return YES;
}

static uint64_t wmt_trace_native_factory_calls[135];
static BOOL wmt_trace_backend_call(unsigned ordinal,void *params,NSString **failure) {
  if(!wmt_trace_native_api_table&&!wmt_trace_api_load_native(failure))return NO;
  wmt_trace_api_native_fn fn=(wmt_trace_api_native_fn)wmt_trace_native_api_table[ordinal];
  if(!fn||fn(params)) { *failure=@"selected backend factory dispatch failed";return NO; }
  ++wmt_trace_native_factory_calls[ordinal];return YES;
}
static id wmt_trace_backend_texture(id device,id buffer,MTLTextureDescriptor *desc,
    uint64_t offset,uint64_t row,NSString **failure) {
  if(desc.width>UINT32_MAX||desc.height>UINT32_MAX||desc.depth>UINT32_MAX||
      desc.arrayLength>UINT32_MAX||desc.mipmapLevelCount>255||desc.sampleCount>255||
      desc.textureType>255||desc.usage>255||desc.resourceOptions>UINT32_MAX) {
    *failure=@"texture exceeds selected WMT ABI fields";return nil;
  }
  struct WMTTextureInfo info={0};
  info.pixel_format=(enum WMTPixelFormat)desc.pixelFormat;
  info.width=desc.width;info.height=desc.height;info.depth=desc.depth;
  info.array_length=desc.arrayLength;info.mipmap_level_count=desc.mipmapLevelCount;
  info.sample_count=desc.sampleCount;info.type=(enum WMTTextureType)desc.textureType;
  info.usage=(enum WMTTextureUsage)desc.usage;info.options=(enum WMTResourceOptions)desc.resourceOptions;
  if(buffer) {
    struct unixcall_mtlbuffer_newtexture p={0};p.buffer=(obj_handle_t)buffer;
    p.info.ptr=&info;p.offset=offset;p.bytes_per_row=row;
    if(!wmt_trace_backend_call(22,&p,failure))return nil;return (id)p.ret;
  }
  struct unixcall_mtldevice_newtexture p={0};p.device=(obj_handle_t)device;p.info.ptr=&info;
  if(!wmt_trace_backend_call(21,&p,failure))return nil;return (id)p.ret;
}
static id wmt_trace_backend_render_pipeline(id device,MTLRenderPipelineDescriptor *d,NSString **failure) {
  MTLRenderPipelineDescriptor *defaults=[[MTLRenderPipelineDescriptor alloc] init];
  BOOL supported=d.alphaToOneEnabled==defaults.alphaToOneEnabled&&
      d.tessellationFactorFormat==defaults.tessellationFactorFormat&&
      d.tessellationControlPointIndexType==defaults.tessellationControlPointIndexType&&
      d.tessellationFactorScaleEnabled==defaults.tessellationFactorScaleEnabled&&
      d.rasterSampleCount<=255&&d.maxTessellationFactor<=255;
  [defaults release];
  if(!supported) {*failure=@"render descriptor cannot be represented by selected WMT ABI";return nil;}
  struct WMTRenderPipelineInfo info={0};
  for(unsigned i=0;i<8;++i) {
    MTLRenderPipelineColorAttachmentDescriptor *c=d.colorAttachments[i];
    info.colors[i].pixel_format=(enum WMTPixelFormat)c.pixelFormat;
    info.colors[i].blending_enabled=c.blendingEnabled;info.colors[i].write_mask=c.writeMask;
    info.colors[i].rgb_blend_operation=(enum WMTBlendOperation)c.rgbBlendOperation;
    info.colors[i].alpha_blend_operation=(enum WMTBlendOperation)c.alphaBlendOperation;
    info.colors[i].src_rgb_blend_factor=(enum WMTBlendFactor)c.sourceRGBBlendFactor;
    info.colors[i].dst_rgb_blend_factor=(enum WMTBlendFactor)c.destinationRGBBlendFactor;
    info.colors[i].src_alpha_blend_factor=(enum WMTBlendFactor)c.sourceAlphaBlendFactor;
    info.colors[i].dst_alpha_blend_factor=(enum WMTBlendFactor)c.destinationAlphaBlendFactor;
  }
  for(unsigned i=0;i<31;++i) {
    if(d.vertexBuffers[i].mutability==MTLMutabilityImmutable)info.immutable_vertex_buffers|=1u<<i;
    else if(d.vertexBuffers[i].mutability!=MTLMutabilityDefault) {*failure=@"non-default mutable vertex buffer";return nil;}
    if(d.fragmentBuffers[i].mutability==MTLMutabilityImmutable)info.immutable_fragment_buffers|=1u<<i;
    else if(d.fragmentBuffers[i].mutability!=MTLMutabilityDefault) {*failure=@"non-default mutable fragment buffer";return nil;}
  }
  info.vertex_function=(obj_handle_t)d.vertexFunction;info.fragment_function=(obj_handle_t)d.fragmentFunction;
  info.alpha_to_coverage_enabled=d.alphaToCoverageEnabled;info.rasterization_enabled=d.rasterizationEnabled;
  info.raster_sample_count=d.rasterSampleCount;info.depth_pixel_format=(enum WMTPixelFormat)d.depthAttachmentPixelFormat;
  info.stencil_pixel_format=(enum WMTPixelFormat)d.stencilAttachmentPixelFormat;
  info.input_primitive_topology=(enum WMTPrimitiveTopologyClass)d.inputPrimitiveTopology;
  info.tessellation_partition_mode=(enum WMTTessellationPartitionMode)d.tessellationPartitionMode;
  info.tessellation_factor_step=(enum WMTTessellationFactorStepFunction)d.tessellationFactorStepFunction;
  info.tessellation_output_winding_order=(enum WMTWinding)d.tessellationOutputWindingOrder;
  info.max_tessellation_factor=d.maxTessellationFactor;
  struct unixcall_mtldevice_newrenderpso p={0};p.device=(obj_handle_t)device;p.info.ptr=&info;
  if(!wmt_trace_backend_call(34,&p,failure))return nil;
  if(!p.ret_pso) {*failure=p.ret_error ? [(NSError *)(id)p.ret_error localizedDescription] : @"selected render PSO nil";return nil;}
  return (id)p.ret_pso;
}
#endif
