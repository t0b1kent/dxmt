/* Native GPU correctness probe. Run only while holding a Wine func slot.
 * It exercises codec -> disk -> new Metal objects -> actual encoder handlers.
 * It does not claim frame replay, constructors or render coverage. */
#include "../src/winemetal/unix/wmt_trace_commands.h"
#include "../src/winemetal/unix/wmt_trace_native_encoders.h"
#include "../src/winemetal/unix/wmt_trace_native_objects.h"
#include <assert.h>

static uint64_t encode(obj_handle_t object, void *context) {
  return object == 0x1111 ? 1 : object == 0x2222 ? 2 : 0;
}
static obj_handle_t decode(uint64_t identifier, void *context) {
  NSDictionary *objects = (NSDictionary *)context;
  return (obj_handle_t)objects[@(identifier)];
}
int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: native-blit-test NEW.plist\n"); return 2; }
    NSString *path = [NSString stringWithUTF8String:argv[1]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:path]) return 2;
    struct wmtcmd_blit_fillbuffer fill = {0};
    struct wmtcmd_blit_copy_from_buffer_to_buffer copy = {0};
    fill.type = WMTBlitCommandFillBuffer;
    fill.buffer = 0x1111; fill.offset = 128; fill.length = 512; fill.value = 0xA7;
    fill.next.ptr = &copy;
    copy.type = WMTBlitCommandCopyFromBufferToBuffer;
    copy.src = 0x1111; copy.dst = 0x2222;
    copy.src_offset = 128; copy.dst_offset = 256; copy.copy_length = 512;
    NSString *failure = nil;
    NSArray *records = wmt_trace_pack_commands(1, (void *)&fill, 4096, 8, encode, NULL, &failure);
    assert(records && !failure);
    NSData *disk = [NSPropertyListSerialization dataWithPropertyList:records
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    assert([disk writeToFile:path options:NSDataWritingWithoutOverwriting error:NULL]);
    [records release];
    NSArray *roundtrip = [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:path]
        options:NSPropertyListImmutable format:NULL error:NULL];
    assert(roundtrip);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr, "FAILED no Metal device\n"); return 3; }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    for (unsigned run = 0; run < 3; ++run) {
      NSMutableDictionary *objects = [NSMutableDictionary dictionary];
      for (unsigned identifier = 1; identifier <= 2; ++identifier)
        assert(wmt_trace_native_construct(@{@"event": @"buffer", @"object": @(identifier),
            @"fields": @{@"length": @1024, @"options": @(MTLResourceStorageModeShared)}},
            device, objects, &failure) && !failure);
      assert(!wmt_trace_native_construct(@{@"event": @"buffer", @"object": @1,
          @"fields": @{@"length": @1024, @"options": @0}}, device, objects, &failure) && failure);
      failure = nil;
      id<MTLBuffer> src = objects[@1];
      id<MTLBuffer> dst = objects[@2];
      assert(src && dst); memset(src.contents, 0, 1024); memset(dst.contents, 0, 1024);
      NSMutableArray *arena = [NSMutableArray array];
      struct wmtcmd_base *head = wmt_trace_unpack_commands(1, roundtrip, 4096, 8,
          decode, objects, arena, &failure);
      assert(head && !failure);
      id<MTLCommandBuffer> command = [queue commandBuffer];
      id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
      wmt_trace_native_blit(encoder, head);
      [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
      assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
      for (unsigned i = 0; i < 1024; ++i) {
        assert(((uint8_t *)src.contents)[i] == (i >= 128 && i < 640 ? 0xA7 : 0));
        assert(((uint8_t *)dst.contents)[i] == (i >= 256 && i < 768 ? 0xA7 : 0));
      }
      printf("PASS native GPU blit replay=%u bytes=1024 gpu_ms=%.9f\n", run + 1,
          (command.GPUEndTime - command.GPUStartTime) * 1000.0);
    }
    [queue release]; [device release];
  }
  return 0;
}
