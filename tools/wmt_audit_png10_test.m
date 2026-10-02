#include "../src/winemetal/unix/wmt_trace_png.h"
#include <assert.h>
int main(int argc,char **argv) { @autoreleasepool {
  assert(argc==2); NSString *out=[NSString stringWithUTF8String:argv[1]];
  uint32_t pixels[4]={1023u|(511u<<10)|(1u<<20)|(2u<<30), 1u|(1023u<<20)|(3u<<30), 0u, UINT32_MAX};
  assert(wmt_trace_write_png([out stringByAppendingPathComponent:@"rgb10.png"],pixels,4,1,16,MTLPixelFormatRGB10A2Unorm));
  assert(wmt_trace_write_png([out stringByAppendingPathComponent:@"bgr10.png"],pixels,4,1,16,MTLPixelFormatBGR10A2Unorm));
  assert(!wmt_trace_write_png([out stringByAppendingPathComponent:@"unsupported.png"],pixels,4,1,16,MTLPixelFormatR32Uint));
  assert(!wmt_trace_write_png([out stringByAppendingPathComponent:@"short-row.png"],pixels,4,1,12,MTLPixelFormatRGB10A2Unorm));
  for (unsigned i=0;i<1024;i++) assert((wmt_trace_unorm10_to16(i)*1023u+32767u)/65535u==i);
  puts("PASS all1024 codes reversible; RGB/BGR packed10 PNG16 fixtures; unsupported/short-row reject; no GPU/Wine");
} return 0; }
