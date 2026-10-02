#ifndef WMT_TRACE_PASS_H
#define WMT_TRACE_PASS_H
#include "wmt_trace_descriptors.h"
typedef uint64_t (*wmt_trace_pass_id)(id, void *);
static NSArray *wmt_trace_pass_attachment_keys(void) {
  return @[@"level", @"slice", @"depthPlane", @"loadAction", @"storeAction",
      @"resolveLevel", @"resolveSlice", @"resolveDepthPlane", @"storeActionOptions"];
}
static NSDictionary *wmt_trace_pass_attachment(MTLRenderPassAttachmentDescriptor *a,
    wmt_trace_pass_id identify, void *context) {
  uint64_t texture = a.texture ? identify(a.texture, context) : 0;
  uint64_t resolve = a.resolveTexture ? identify(a.resolveTexture, context) : 0;
  if ((a.texture && !texture) || (a.resolveTexture && !resolve)) return nil;
  return @{@"scalars": wmt_trace_descriptor_fields(a, wmt_trace_pass_attachment_keys()),
      @"texture": @(texture), @"resolveTexture": @(resolve)};
}
/* All eight colors and depth/stencil recorded, including empty attachments.
 * Tile/imageblock/sample-position state is deliberately rejected until audited. */
static NSDictionary *wmt_trace_pass_fields(MTLRenderPassDescriptor *pass,
    wmt_trace_pass_id identify, void *context) {
  if (!identify || pass.tileWidth || pass.tileHeight || pass.imageblockSampleLength ||
      pass.threadgroupMemoryLength || [pass getSamplePositions:NULL count:0]) return nil;
  NSMutableArray *colors = [NSMutableArray array];
  for (NSUInteger i = 0; i < 8; ++i) {
    MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[i];
    NSDictionary *attachment = wmt_trace_pass_attachment(a, identify, context);
    if (!attachment) return nil;
    MTLClearColor c = a.clearColor;
    [colors addObject:@{@"attachment": attachment, @"clear": @[@(c.red), @(c.green), @(c.blue), @(c.alpha)]}];
  }
  NSDictionary *depth = wmt_trace_pass_attachment(pass.depthAttachment, identify, context);
  NSDictionary *stencil = wmt_trace_pass_attachment(pass.stencilAttachment, identify, context);
  uint64_t visibility = pass.visibilityResultBuffer ? identify(pass.visibilityResultBuffer, context) : 0;
  if (!depth || !stencil || (pass.visibilityResultBuffer && !visibility)) return nil;
  return @{@"colors": colors, @"depth": depth, @"stencil": stencil,
      @"clearDepth": @(pass.depthAttachment.clearDepth),
      @"depthResolveFilter": @(pass.depthAttachment.depthResolveFilter),
      @"clearStencil": @(pass.stencilAttachment.clearStencil),
      @"stencilResolveFilter": @(pass.stencilAttachment.stencilResolveFilter),
      @"visibility": @(visibility),
      @"scalars": wmt_trace_descriptor_fields(pass, @[@"defaultRasterSampleCount",
        @"renderTargetArrayLength", @"renderTargetHeight", @"renderTargetWidth"])};
}
static BOOL wmt_trace_pass_restore_attachment(MTLRenderPassAttachmentDescriptor *a,
    NSDictionary *fields, NSDictionary *objects) {
  if (![fields isKindOfClass:[NSDictionary class]] || fields.count != 3 ||
      ![fields[@"texture"] isKindOfClass:[NSNumber class]] ||
      ![fields[@"resolveTexture"] isKindOfClass:[NSNumber class]]) return NO;
  NSDictionary *scalars = fields[@"scalars"];
  if (![scalars isKindOfClass:[NSDictionary class]] ||
      ![[NSSet setWithArray:scalars.allKeys] isEqualToSet:[NSSet setWithArray:wmt_trace_pass_attachment_keys()]]) return NO;
  for (id value in scalars.allValues) if (![value isKindOfClass:[NSNumber class]]) return NO;
  a.texture = objects[fields[@"texture"]]; a.resolveTexture = objects[fields[@"resolveTexture"]];
  if (([fields[@"texture"] unsignedLongLongValue] && !a.texture) ||
      ([fields[@"resolveTexture"] unsignedLongLongValue] && !a.resolveTexture)) return NO;
  @try { [a setValuesForKeysWithDictionary:scalars]; }
  @catch (NSException *exception) { return NO; }
  return YES;
}
static MTLRenderPassDescriptor *wmt_trace_pass_restore(NSDictionary *fields,
    NSDictionary *objects) {
  if (![fields isKindOfClass:[NSDictionary class]] || fields.count != 9 ||
      ![fields[@"colors"] isKindOfClass:[NSArray class]] || [fields[@"colors"] count] != 8) return nil;
  for (NSString *key in @[@"clearDepth", @"clearStencil", @"depthResolveFilter", @"stencilResolveFilter", @"visibility"])
    if (![fields[key] isKindOfClass:[NSNumber class]]) return nil;
  MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
  for (NSUInteger i = 0; i < 8; ++i) {
    NSDictionary *c = fields[@"colors"][i];
    if (![c isKindOfClass:[NSDictionary class]] || c.count != 2 ||
        ![c[@"clear"] isKindOfClass:[NSArray class]] || [c[@"clear"] count] != 4 ||
        !wmt_trace_pass_restore_attachment(pass.colorAttachments[i], c[@"attachment"], objects)) return nil;
    for (id value in c[@"clear"]) if (![value isKindOfClass:[NSNumber class]]) return nil;
    NSArray *v = c[@"clear"];
    pass.colorAttachments[i].clearColor = MTLClearColorMake([v[0] doubleValue], [v[1] doubleValue],
        [v[2] doubleValue], [v[3] doubleValue]);
  }
  if (!wmt_trace_pass_restore_attachment(pass.depthAttachment, fields[@"depth"], objects) ||
      !wmt_trace_pass_restore_attachment(pass.stencilAttachment, fields[@"stencil"], objects)) return nil;
  pass.depthAttachment.clearDepth = [fields[@"clearDepth"] doubleValue];
  pass.depthAttachment.depthResolveFilter = [fields[@"depthResolveFilter"] unsignedIntegerValue];
  pass.stencilAttachment.clearStencil = [fields[@"clearStencil"] unsignedIntValue];
  pass.stencilAttachment.stencilResolveFilter = [fields[@"stencilResolveFilter"] unsignedIntegerValue];
  pass.visibilityResultBuffer = objects[fields[@"visibility"]];
  if ([fields[@"visibility"] unsignedLongLongValue] && !pass.visibilityResultBuffer) return nil;
  NSDictionary *scalars = fields[@"scalars"];
  NSArray *keys = @[@"defaultRasterSampleCount", @"renderTargetArrayLength", @"renderTargetHeight", @"renderTargetWidth"];
  if (![scalars isKindOfClass:[NSDictionary class]] ||
      ![[NSSet setWithArray:scalars.allKeys] isEqualToSet:[NSSet setWithArray:keys]]) return nil;
  for (id value in scalars.allValues) if (![value isKindOfClass:[NSNumber class]]) return nil;
  @try { [pass setValuesForKeysWithDictionary:scalars]; }
  @catch (NSException *exception) { return nil; }
  return pass;
}
#endif
