#ifndef WMT_TRACE_DEVICE_IDENTITY_H
#define WMT_TRACE_DEVICE_IDENTITY_H
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

/* registryID is an IOKit boot-session identifier, not a replay requirement.
 * Keep it as provenance. Only behavioral properties participate in admission.
 * Numeric family values are the public MTLGPUFamily values in MTLDevice.h;
 * querying the complete fixed list also records unsupported families. */
static NSDictionary *wmt_trace_device_capabilities(id<MTLDevice> device) {
  const NSInteger families[] = {1001,1002,1003,1004,1005,1006,1007,1008,
      1009,1010,1011,2001,2002,3001,3002,3003,4001,4002,5001,5002};
  NSMutableDictionary *support = [NSMutableDictionary dictionary];
  for (NSUInteger i=0;i<sizeof(families)/sizeof(families[0]);++i)
    support[[NSString stringWithFormat:@"%ld",(long)families[i]]] =
        @([device supportsFamily:(MTLGPUFamily)families[i]]);
  return @{@"schema":@1,@"family-support":support,
      @"bc-texture-compression":@(device.supportsBCTextureCompression),
      @"raster-order-groups":@(device.rasterOrderGroupsSupported),
      @"read-write-texture-tier":@(device.readWriteTextureSupport),
      @"argument-buffer-tier":@(device.argumentBuffersSupport),
      @"max-buffer-length":@(device.maxBufferLength)};
}

static BOOL wmt_trace_device_identity(NSDictionary *fields,id<MTLDevice> device,
    NSString **failure) {
  NSDictionary *current = wmt_trace_device_capabilities(device);
  NSDictionary *recorded = fields[@"capabilities"];
  NSDictionary *receipt = @{@"recorded-registryID":fields[@"registryID"]?:[NSNull null],
      @"current-registryID":@(device.registryID),@"registryID-policy":@"PROVENANCE_ONLY",
      @"recorded-name":fields[@"name"]?:[NSNull null],@"current-name":device.name?:@"",
      @"recorded-unified-memory":fields[@"unified-memory"]?:[NSNull null],
      @"current-unified-memory":@(device.hasUnifiedMemory),
      @"recorded-capabilities":recorded?:[NSNull null],@"current-capabilities":current,
      @"capability-coverage":recorded?@"RECORDED_AND_COMPARED":@"LEGACY_NOT_RECORDED"};
  NSData *json = [NSJSONSerialization dataWithJSONObject:receipt options:NSJSONWritingSortedKeys error:NULL];
  fprintf(stderr,"DEVICE_IDENTITY %s\n",[[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] autorelease].UTF8String);
  if (![fields isKindOfClass:[NSDictionary class]] || (fields.count!=3 && fields.count!=4) ||
      ![fields[@"registryID"] isKindOfClass:[NSNumber class]] ||
      ![fields[@"name"] isKindOfClass:[NSString class]] ||
      ![fields[@"unified-memory"] isKindOfClass:[NSNumber class]] ||
      ![fields[@"name"] isEqual:device.name] ||
      [fields[@"unified-memory"] boolValue]!=device.hasUnifiedMemory ||
      (fields.count==4 && (![recorded isKindOfClass:[NSDictionary class]] || ![recorded isEqual:current]))) {
    if (failure) *failure=@"recorded native device identity differs";
    return NO;
  }
  return YES;
}
#endif
