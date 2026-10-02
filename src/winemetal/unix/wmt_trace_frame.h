#ifndef WMT_TRACE_FRAME_H
#define WMT_TRACE_FRAME_H
#include "wmt_trace_png.h"
#include "wmt_trace_file.h"
#include <stdio.h>
static BOOL wmt_trace_frame_timings = YES;

static dispatch_group_t wmt_trace_frame_group(void) {
  static dispatch_group_t group;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ group = dispatch_group_create(); });
  return group;
}

/* Same readback operation for producer truth and native replay. Encoding occurs
 * before commit/present, completion owns staging bytes. No private getBytes. */
static BOOL wmt_trace_frame_schedule(id<MTLCommandBuffer> command, id<MTLTexture> texture,
    NSString *directory, uint64_t frame) {
  NSUInteger width = texture.width, height = texture.height;
  MTLPixelFormat format = texture.pixelFormat;
  BOOL supported = wmt_trace_png_supported(format);
  if (!command || !texture || !directory || !supported || texture.framebufferOnly ||
      texture.sampleCount != 1 || !width || !height || width > 32768 || height > 32768) return NO;
  NSUInteger row = (width * 4 + 255) & ~(NSUInteger)255;
  if (height > (64ull * 1024 * 1024) / row) return NO;
  if (![[NSFileManager defaultManager] createDirectoryAtPath:directory
      withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:NULL]) return NO;
  id<MTLBuffer> staging = [command.device newBufferWithLength:row * height
      options:MTLResourceStorageModeShared];
  if (!staging) return NO;
  id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
  if (!encoder) { [staging release]; return NO; }
  [encoder copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
      sourceSize:MTLSizeMake(width,height,1) toBuffer:staging destinationOffset:0
      destinationBytesPerRow:row destinationBytesPerImage:row * height];
  [encoder endEncoding];
  NSString *path = [directory stringByAppendingPathComponent:
      [NSString stringWithFormat:@"frame-%06llu.png", (unsigned long long)frame]];
  dispatch_group_enter(wmt_trace_frame_group());
  [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    @autoreleasepool {
      BOOL success = completed.status == MTLCommandBufferStatusCompleted && !completed.error &&
          wmt_trace_write_png(path, staging.contents, width, height, row, format);
      if(wmt_trace_frame_timings) fprintf(stderr, "WMT_TRACE FRAME %s frame=%llu width=%lu height=%lu gpu_ms=%.9f start_s=%.9f end_s=%.9f\n",
          success ? "PRESENT" : "FAILED", (unsigned long long)frame, (unsigned long)width,
          (unsigned long)height, (completed.GPUEndTime-completed.GPUStartTime)*1000,
          completed.GPUStartTime,completed.GPUEndTime);
      else fprintf(stderr,"WMT_TRACE FRAME %s frame=%llu width=%lu height=%lu timing=NOT_ENABLED\n",
          success ? "PRESENT" : "FAILED",(unsigned long long)frame,(unsigned long)width,(unsigned long)height);
      if (!success) wmt_trace_write_text(@"FAILED frame completion/readback/PNG\n",
          [directory stringByAppendingPathComponent:@"FAILED.txt"], NULL);
    }
    dispatch_group_leave(wmt_trace_frame_group());
  }];
  [staging release];
  return YES;
}
#endif
