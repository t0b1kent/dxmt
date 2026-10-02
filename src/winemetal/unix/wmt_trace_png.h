#ifndef WMT_TRACE_PNG_H
#define WMT_TRACE_PNG_H
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <string.h>

static BOOL wmt_trace_png_supported(MTLPixelFormat format) {
  return format == MTLPixelFormatBGRA8Unorm || format == MTLPixelFormatBGRA8Unorm_sRGB ||
      format == MTLPixelFormatRGBA8Unorm || format == MTLPixelFormatRGBA8Unorm_sRGB ||
      format == MTLPixelFormatRGB10A2Unorm || format == MTLPixelFormatBGR10A2Unorm;
}

/* PNG16 preserves every normalized 10-bit color and 2-bit alpha value. */
static uint16_t wmt_trace_unorm10_to16(uint32_t value) {
  return (uint16_t)((value * 65535u + 511u) / 1023u);
}

/* Bytes are GPU readback, not MTLTexture.getBytes on a private texture.
 * For drawable truth encode the blit into the presenting command buffer;
 * call this only after its completion. Always preserve alpha and raw channels. */
static BOOL wmt_trace_write_png(NSString *path, const void *source,
    NSUInteger width, NSUInteger height, NSUInteger row, MTLPixelFormat format) {
  BOOL bgra = format == MTLPixelFormatBGRA8Unorm || format == MTLPixelFormatBGRA8Unorm_sRGB;
  BOOL rgba = format == MTLPixelFormatRGBA8Unorm || format == MTLPixelFormatRGBA8Unorm_sRGB;
  BOOL packed10 = format == MTLPixelFormatRGB10A2Unorm || format == MTLPixelFormatBGR10A2Unorm;
  BOOL bgr10 = format == MTLPixelFormatBGR10A2Unorm;
  NSUInteger output_bytes = packed10 ? 8 : 4;
  if (!path || !source || (!bgra && !rgba && !packed10) || !width || !height ||
      width > UINT64_C(128) * 1024 * 1024 / 4 || row < width * 4 ||
      height > UINT64_C(128) * 1024 * 1024 / row ||
      width > UINT64_C(128) * 1024 * 1024 / output_bytes ||
      height > UINT64_C(128) * 1024 * 1024 / (width * output_bytes) ||
      [[NSFileManager defaultManager] fileExistsAtPath:path]) return NO;
  NSMutableData *pixels = [NSMutableData dataWithLength:width * height * output_bytes];
  uint8_t *dst = pixels.mutableBytes;
  for (NSUInteger y = 0; y < height; ++y) {
    const uint8_t *src = (const uint8_t *)source + y * row;
    for (NSUInteger x = 0; x < width; ++x) {
      if (packed10) {
        uint32_t value; memcpy(&value, src + x * 4, 4);
        uint16_t components[4] = {
            wmt_trace_unorm10_to16((value >> (bgr10 ? 20 : 0)) & 1023),
            wmt_trace_unorm10_to16((value >> 10) & 1023),
            wmt_trace_unorm10_to16((value >> (bgr10 ? 0 : 20)) & 1023),
            (uint16_t)((value >> 30) * 21845u)};
        for (unsigned c = 0; c < 4; ++c) {
          NSUInteger at = (y * width + x) * 8 + c * 2;
          dst[at] = components[c] >> 8; dst[at + 1] = components[c] & 255;
        }
        continue;
      }
      dst[(y * width + x) * 4 + 0] = src[x * 4 + (bgra ? 2 : 0)];
      dst[(y * width + x) * 4 + 1] = src[x * 4 + 1];
      dst[(y * width + x) * 4 + 2] = src[x * 4 + (bgra ? 0 : 2)];
      dst[(y * width + x) * 4 + 3] = src[x * 4 + 3];
    }
  }
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGDataProviderRef provider = CGDataProviderCreateWithCFData((CFDataRef)pixels);
  CGImageRef image = CGImageCreate(width, height, packed10 ? 16 : 8,
      packed10 ? 64 : 32, width * output_bytes, space,
      kCGImageAlphaLast | (packed10 ? kCGBitmapByteOrder16Big : kCGBitmapByteOrder32Big),
      provider, NULL, false, kCGRenderingIntentDefault);
  CGImageDestinationRef destination = image ? CGImageDestinationCreateWithURL(
      (CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL) : NULL;
  BOOL success = NO;
  if (destination) {
    CGImageDestinationAddImage(destination, image, NULL);
    success = CGImageDestinationFinalize(destination);
    CFRelease(destination);
  }
  if (image) CGImageRelease(image);
  CGDataProviderRelease(provider); CGColorSpaceRelease(space);
  return success;
}
#endif
