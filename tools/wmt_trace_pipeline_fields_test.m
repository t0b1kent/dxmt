/* CPU-only descriptor roundtrip; does not create a device or submit GPU work. */
#include "../src/winemetal/unix/wmt_trace_native_objects.h"
#include "../src/winemetal/unix/wmt_trace_pass.h"
#include <assert.h>
static uint64_t empty_id(id object, void *context) { return 0; }
int main(void) {
  @autoreleasepool {
    MTLRenderPipelineDescriptor *a = [[MTLRenderPipelineDescriptor alloc] init];
    a.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    a.colorAttachments[0].blendingEnabled = YES;
    a.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    a.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    a.colorAttachments[7].pixelFormat = MTLPixelFormatRGBA16Float;
    a.vertexBuffers[30].mutability = MTLMutabilityImmutable;
    a.fragmentBuffers[0].mutability = MTLMutabilityMutable;
    a.tessellationPartitionMode = MTLTessellationPartitionModeFractionalEven;
    NSDictionary *fields = wmt_trace_pipeline_fields(a, 0, 0);
    assert(fields && fields.count == 6);
    NSData *disk = [NSPropertyListSerialization dataWithPropertyList:fields
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    NSDictionary *read = [NSPropertyListSerialization propertyListWithData:disk
        options:NSPropertyListImmutable format:NULL error:NULL];
    assert([read isEqualToDictionary:fields]);
    MTLRenderPipelineDescriptor *b = [[MTLRenderPipelineDescriptor alloc] init];
    assert(wmt_trace_apply_fields(b, read[@"scalars"], wmt_trace_pipeline_scalars(b)));
    for (NSUInteger i = 0; i < 8; ++i)
      assert(wmt_trace_apply_fields(b.colorAttachments[i], read[@"colors"][i],
          wmt_trace_pipeline_color(b.colorAttachments[i])));
    assert([wmt_trace_pipeline_scalars(a) isEqualToDictionary:wmt_trace_pipeline_scalars(b)]);
    assert(b.colorAttachments[7].pixelFormat == MTLPixelFormatRGBA16Float);
    assert(b.colorAttachments[0].destinationRGBBlendFactor == MTLBlendFactorOneMinusSourceAlpha);
    NSMutableDictionary *bad = [read[@"scalars"] mutableCopy];
    bad[@"unknown"] = @1;
    assert(!wmt_trace_apply_fields(b, bad, wmt_trace_pipeline_scalars(b)));
    [bad removeObjectForKey:@"unknown"]; bad[@"rasterSampleCount"] = @"bad";
    assert(!wmt_trace_apply_fields(b, bad, wmt_trace_pipeline_scalars(b)));
    a.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    a.vertexDescriptor.attributes[0].format = MTLVertexFormatFloat3;
    assert(!wmt_trace_pipeline_fields(a, 0, 0));
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[7].clearColor = MTLClearColorMake(.1, .2, .3, .4);
    pass.depthAttachment.clearDepth = .75;
    NSDictionary *pass_fields = wmt_trace_pass_fields(pass, empty_id, NULL);
    assert(pass_fields);
    MTLRenderPassDescriptor *restored = wmt_trace_pass_restore(pass_fields, @{});
    assert(restored && restored.depthAttachment.clearDepth == .75);
    assert(restored.colorAttachments[7].clearColor.alpha == .4);
    assert([wmt_trace_pass_fields(restored, empty_id, NULL) isEqualToDictionary:pass_fields]);
    NSMutableDictionary *bad_pass = [pass_fields mutableCopy];
    bad_pass[@"visibility"] = @99;
    assert(!wmt_trace_pass_restore(bad_pass, @{}));
    [bad_pass release];
    printf("PASS CPU pipeline plist scalars=%lu colors=8 buffers=31+31 malformed/reject=3 bytes=%lu GPU=NOT_ENABLED\n",
        (unsigned long)[read[@"scalars"] count], (unsigned long)disk.length);
    [bad release]; [a release]; [b release];
  }
  return 0;
}
