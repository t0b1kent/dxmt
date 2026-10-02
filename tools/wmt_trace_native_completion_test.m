/* Native GPU correctness only: requires func slot. No Wine or game. */
#include "../src/winemetal/unix/wmt_trace_cb.h"
#include <assert.h>
#include <dispatch/dispatch.h>
int main(void) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); assert(device);
    id<MTLCommandQueue> queue = [device newCommandQueue]; assert(queue);
    id<MTLSharedEvent> gate = [device newSharedEvent]; assert(gate);
    id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    assert(buffer);
    assert(wmt_trace_owner_change(buffer, 0, 4096, WMT_OWNER_BEGIN));
    memset(buffer.contents, 0x55, 4096);
    assert(wmt_trace_owner_change(buffer, 0, 4096, WMT_OWNER_END));
    dispatch_semaphore_t done[2] = {dispatch_semaphore_create(0), dispatch_semaphore_create(0)};
    id<MTLCommandBuffer> commands[2];
    for (unsigned i = 0; i < 2; ++i) {
      commands[i] = [[queue commandBuffer] retain];
      [commands[i] encodeWaitForEvent:gate value:i + 1];
      id<MTLBlitCommandEncoder> blit = [commands[i] blitCommandEncoder];
      [blit fillBuffer:buffer range:NSMakeRange(0, 4096) value:i ? 0x22 : 0x11];
      [blit endEncoding];
      dispatch_semaphore_t signal = done[i];
      assert(wmt_trace_cb_arm_writes(commands[i], @{@1: buffer}, i + 1,
          ^(id<MTLCommandBuffer> completed, NSDictionary *event) {
            assert(completed.status == MTLCommandBufferStatusCompleted && !completed.error);
            assert([event[@"ownership-release"] isEqual:@"PRESENT"]);
            assert([event[@"queued-buffer-count"] unsignedIntValue] == 1);
            assert([event[@"GPUEndTime"] doubleValue] >= [event[@"GPUStartTime"] doubleValue]);
            printf("PASS completion ticket=%u gpu_ms=%.9f release=PRESENT\n", i + 1,
                ([event[@"GPUEndTime"] doubleValue] - [event[@"GPUStartTime"] doubleValue]) * 1000);
            dispatch_semaphore_signal(signal);
          }));
      [commands[i] commit];
    }
    assert(!wmt_trace_owner_change(buffer, 0, 4096, WMT_OWNER_BEGIN));
    gate.signaledValue = 1;
    assert(dispatch_semaphore_wait(done[0], dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
    assert(!wmt_trace_owner_change(buffer, 0, 4096, WMT_OWNER_BEGIN));
    assert(!wmt_trace_owner_gpu_end(buffer, 1));
    gate.signaledValue = 2;
    assert(dispatch_semaphore_wait(done[1], dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
    for (unsigned i = 0; i < 4096; ++i) assert(((uint8_t *)buffer.contents)[i] == 0x22);
    assert(wmt_trace_owner_change(buffer, 0, 4096, WMT_OWNER_BEGIN));
    assert(wmt_trace_owner_change(buffer, 0, 4096, WMT_OWNER_END));
    for (unsigned i = 0; i < 2; ++i) { [commands[i] release]; dispatch_release(done[i]); }
    [buffer release]; [gate release]; [queue release]; [device release];
    puts("PASS two queued GPU writers: CPU blocked until both completions; bytes=4096 exact");
  }
  return 0;
}
