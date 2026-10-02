/* Native render/PNG correctness fixture. Requires func slot, not a game trace. */
#include "../src/winemetal/unix/wmt_trace_commands.h"
#include "../src/winemetal/unix/wmt_trace_native_encoders.h"
#include "../src/winemetal/unix/wmt_trace_png.h"
#include "../src/winemetal/unix/wmt_trace_native_objects.h"
#include "../src/winemetal/unix/wmt_trace_pass.h"
#include <assert.h>

static uint64_t encode(obj_handle_t value, void *context) { return value == 0x1234 ? 1 : 0; }
static obj_handle_t decode(uint64_t value, void *context) { return value == 1 ? (obj_handle_t)context : 0; }
static uint64_t identify_target(id object, void *context) { return object == (id)context ? 44 : 0; }
int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc != 2) return 2;
    NSString *directory = [NSString stringWithUTF8String:argv[1]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:directory]) return 2;
    assert([[NSFileManager defaultManager] createDirectoryAtPath:directory
        withIntermediateDirectories:YES attributes:nil error:NULL]);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); assert(device);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
      "vertex float4 vs(uint i [[vertex_id]]) { float2 p[3] = {float2(-.8,-.8),float2(.8,-.8),float2(0,.8)};"
      "return float4(p[i],0,1); }\nconstant bool paint [[function_constant(0)]];\n"
      "fragment float4 ps() { return paint ? float4(.25,.5,.75,1) : float4(0,0,0,1); }\n";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error]; assert(library && !error);
    id<MTLFunction> vs = [library newFunctionWithName:@"vs"];
    uint32_t bool_storage = 0x11223301; /* PE stores bool in uint32_t; Metal reads one byte. */
    assert(wmt_trace_constant_scalar_size(MTLDataTypeBool) == 1);
    assert(wmt_trace_constant_scalar_size(MTLDataTypeUInt) == 4);
    assert(wmt_trace_constant_scalar_size(MTLDataTypeFloat3) == 0); /* composite layout unverified */
    NSMutableDictionary *objects = [NSMutableDictionary dictionaryWithObject:library forKey:@40];
    NSString *constructor_failure = nil;
    BOOL constructed = wmt_trace_native_construct(@{@"event": @"function-specialized", @"object": @42,
        @"fields": @{@"library": @40, @"name": @"ps", @"constants": @[
          @{@"type": @(MTLDataTypeBool), @"index": @0, @"data": [NSData dataWithBytes:&bool_storage length:1]}]}},
        device, objects, &constructor_failure);
    assert(constructed && !constructor_failure);
    id<MTLFunction> ps = [objects[@42] retain];
    objects[@41] = vs;
    MTLRenderPipelineDescriptor *recorded_pso = [[MTLRenderPipelineDescriptor alloc] init];
    recorded_pso.vertexFunction = vs; recorded_pso.fragmentFunction = ps;
    recorded_pso.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    recorded_pso.vertexBuffers[3].mutability = MTLMutabilityImmutable;
    NSDictionary *pipeline = @{@"event": @"render-pipeline", @"object": @43,
        @"fields": wmt_trace_pipeline_fields(recorded_pso, 41, 42)};
    NSData *pipeline_disk = [NSPropertyListSerialization dataWithPropertyList:pipeline
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    NSString *pipeline_path = [directory stringByAppendingPathComponent:@"pipeline.plist"];
    assert([pipeline_disk writeToFile:pipeline_path options:NSDataWritingWithoutOverwriting error:NULL]);
    [recorded_pso release];
    pipeline = [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:pipeline_path]
        options:NSPropertyListImmutable format:NULL error:NULL];
    struct wmtcmd_render_setpso set = {0}; struct wmtcmd_render_draw draw = {0};
    set.type = WMTRenderCommandSetPSO; set.pso = 0x1234; set.next.ptr = &draw;
    draw.type = WMTRenderCommandDraw; draw.primitive_type = (enum WMTPrimitiveType)MTLPrimitiveTypeTriangle;
    draw.vertex_count = 3; draw.instance_count = 1;
    NSString *failure = nil;
    NSArray *records = wmt_trace_pack_commands(3, (void *)&set, 4096, 8, encode, NULL, &failure);
    assert(records && !failure);
    NSData *disk = [NSPropertyListSerialization dataWithPropertyList:records
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    NSString *stream = [directory stringByAppendingPathComponent:@"commands.plist"];
    assert([disk writeToFile:stream options:NSDataWritingWithoutOverwriting error:NULL]);
    [records release];
    records = [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:stream]
        options:NSPropertyListImmutable format:NULL error:NULL];
    NSData *truth = nil;
    for (unsigned run = 0; run < 3; ++run) {
      @autoreleasepool {
        [objects removeObjectForKey:@43];
        assert(wmt_trace_native_construct(pipeline, device, objects, &constructor_failure));
        assert(!constructor_failure);
        id<MTLRenderPipelineState> pso = [objects[@43] retain];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:128 height:128 mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:td];
        id<MTLBuffer> readback = [device newBufferWithLength:128 * 128 * 4 options:MTLResourceStorageModeShared];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        NSDictionary *pass_fields = wmt_trace_pass_fields(pass, identify_target, texture);
        assert(pass_fields);
        NSData *pass_disk = [NSPropertyListSerialization dataWithPropertyList:pass_fields
            format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
        NSString *pass_path = [directory stringByAppendingPathComponent:
            [NSString stringWithFormat:@"pass-%u.plist", run + 1]];
        assert([pass_disk writeToFile:pass_path options:NSDataWritingWithoutOverwriting error:NULL]);
        pass_fields = [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:pass_path]
            options:NSPropertyListImmutable format:NULL error:NULL];
        objects[@44] = texture;
        pass = wmt_trace_pass_restore(pass_fields, objects);
        assert(pass);
        NSMutableArray *arena = [NSMutableArray array];
        struct wmtcmd_base *head = wmt_trace_unpack_commands(3, records, 4096, 8, decode, pso, arena, &failure);
        assert(head && !failure);
        [objects removeObjectForKey:@51]; [objects removeObjectForKey:@52];
        if (!objects[@50]) {
          assert((wmt_trace_native_construct(@{@"event": @"queue", @"object": @50,
              @"fields": @{@"maxCommandBuffers": @3}}, device, objects, &constructor_failure)));
        }
        assert((wmt_trace_native_construct(@{@"event": @"command-buffer", @"object": @51,
            @"fields": @{@"queue": @50}}, device, objects, &constructor_failure)));
        id<MTLCommandBuffer> command = objects[@51];
        assert((wmt_trace_native_construct(@{@"event": @"render-encoder", @"object": @52,
            @"fields": @{@"commandBuffer": @51, @"pass": pass_fields}}, device, objects, &constructor_failure)));
        id<MTLRenderCommandEncoder> render = objects[@52];
        wmt_trace_native_render(render, head); [render endEncoding];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(128, 128, 1) toBuffer:readback destinationOffset:0
            destinationBytesPerRow:512 destinationBytesPerImage:65536];
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
        NSData *pixels = [NSData dataWithBytes:readback.contents length:65536];
        if (run == 0) truth = [pixels copy]; else assert([pixels isEqualToData:truth]);
        const uint8_t *center = (const uint8_t *)pixels.bytes + (64 * 128 + 64) * 4;
        assert(center[0] == 191 && center[1] == 128 && center[2] == 64 && center[3] == 255);
        NSString *png = [directory stringByAppendingPathComponent:[NSString stringWithFormat:@"frame-%u.png", run + 1]];
        assert(wmt_trace_write_png(png, readback.contents, 128, 128, 512, MTLPixelFormatBGRA8Unorm));
        printf("PASS native render replay=%u raw_bytes=65536 exact=YES gpu_ms=%.9f PNG=frame-%u.png\n",
            run + 1, (command.GPUEndTime - command.GPUStartTime) * 1000.0, run + 1);
        [texture release]; [readback release]; [pso release];
      }
    }
    [truth release]; [vs release]; [ps release]; [library release]; [queue release]; [device release];
  }
  return 0;
}
