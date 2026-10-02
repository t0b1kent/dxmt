#ifndef WMT_TRACE_NATIVE_OBJECTS_H
#define WMT_TRACE_NATIVE_OBJECTS_H
#include "wmt_trace_descriptors.h"
#include "wmt_trace_pass.h"
#include "wmt_trace_native_backend.h"
#include "wmt_trace_device_identity.h"
#include <objc/runtime.h>

/* Metal may intern immutable objects, including functions obtained through two
 * byte-identical library handles. Keep constructor descriptions separate from
 * the NSNumber -> Metal object table and require semantic identity on reuse. */
static char wmt_trace_native_definitions_key;
static NSMutableDictionary *wmt_trace_native_definitions(NSMutableDictionary *objects) {
  NSMutableDictionary *definitions = objc_getAssociatedObject(objects, &wmt_trace_native_definitions_key);
  if (!definitions) {
    definitions = [NSMutableDictionary dictionary];
    objc_setAssociatedObject(objects, &wmt_trace_native_definitions_key, definitions,
        OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  return definitions;
}

static NSDictionary *wmt_trace_native_definition(NSDictionary *event, NSMutableDictionary *objects) {
  NSMutableDictionary *fields = [[event[@"fields"] mutableCopy] autorelease];
  NSString *type = event[@"event"];
  if ([type isEqualToString:@"depth-stencil"]) {
    /* Every Metal stencil attachment has an 8-bit stencil component.
     * Descriptor masks are uint32_t, but upper bits cannot affect a stencil
     * comparison/write. Metal interns UINT32_MAX and 0xff descriptions as one
     * state (record12 seq2211/2231, live generation1085). */
    for (NSString *side in @[@"frontFaceStencil",@"backFaceStencil"]) {
      NSDictionary *original=fields[side];
      if(!original)continue;
      NSMutableDictionary *stencil=[[original mutableCopy] autorelease];
      for(NSString *mask in @[@"readMask",@"writeMask"]) {
        if(![stencil[mask] isKindOfClass:[NSNumber class]])return nil;
        stencil[mask]=@([stencil[mask] unsignedIntValue]&0xffu);
      }
      fields[side]=stencil;
    }
  }
  if ([type isEqualToString:@"function"] || [type isEqualToString:@"function-specialized"]) {
    NSDictionary *library = wmt_trace_native_definitions(objects)[fields[@"library"]];
    NSData *data = library[@"fields"][@"metallib"];
    if (![library[@"event"] isEqualToString:@"library"] || ![data isKindOfClass:[NSData class]]) return nil;
    fields[@"library"] = data;
  }
  return @{@"event": type, @"fields": fields};
}

static BOOL wmt_trace_native_cacheable(NSString *type) {
  return [@[@"depth-stencil", @"sampler", @"library", @"function", @"function-specialized",
      @"render-pipeline", @"compute-pipeline"] containsObject:type];
}

static BOOL wmt_trace_apply_fields(id descriptor, NSDictionary *fields, NSDictionary *expected) {
  if (![fields isKindOfClass:[NSDictionary class]] ||
      ![[NSSet setWithArray:fields.allKeys] isEqualToSet:[NSSet setWithArray:expected.allKeys]]) return NO;
  for (NSString *key in expected)
    if (![fields[key] isKindOfClass:[NSNumber class]]) return NO;
  @try { [descriptor setValuesForKeysWithDictionary:fields]; }
  @catch (NSException *exception) { return NO; }
  return YES;
}

/* Constructor subset only: unsupported event types fail explicitly.
 * IDs are trace generations, never native pointers. Map owns the objects. */
static BOOL wmt_trace_native_construct(NSDictionary *event, id<MTLDevice> device,
    NSMutableDictionary *objects, NSString **failure) {
  NSString *type = event[@"event"];
  NSNumber *identifier = event[@"object"];
  NSDictionary *fields = event[@"fields"];
  id object = nil; NSString *error = nil;
  if (![type isKindOfClass:[NSString class]] || ![identifier isKindOfClass:[NSNumber class]] ||
      !identifier.unsignedLongLongValue ||
      ![fields isKindOfClass:[NSDictionary class]]) {
    error = @"invalid/duplicate constructor"; goto done;
  }
  if (objects[identifier]) {
    NSDictionary *definition = wmt_trace_native_definition(event, objects);
    if (wmt_trace_native_cacheable(type) && definition &&
        [wmt_trace_native_definitions(objects)[identifier] isEqual:definition]) {
      if (failure) *failure = nil;
      return YES;
    }
    error = @"conflicting or mutable duplicate constructor"; goto done;
  }
  if ([type isEqualToString:@"device"]) {
    if (!wmt_trace_device_identity(fields,device,&error)) goto done;
    object=[device retain];
  } else if ([type isEqualToString:@"queue"]) {
    if (fields.count != 1 || ![fields[@"maxCommandBuffers"] isKindOfClass:[NSNumber class]] ||
        ![fields[@"maxCommandBuffers"] unsignedLongLongValue] ||
        [fields[@"maxCommandBuffers"] unsignedLongLongValue] > 65536) {
      error = @"invalid queue capacity"; goto done;
    }
    object = [device newCommandQueueWithMaxCommandBufferCount:[fields[@"maxCommandBuffers"] unsignedIntegerValue]];
  } else if ([type isEqualToString:@"command-buffer"]) {
    id<MTLCommandQueue> queue = objects[fields[@"queue"]];
    if (fields.count != 1 || ![fields[@"queue"] isKindOfClass:[NSNumber class]] || !queue) {
      error = @"invalid/unresolved command-buffer queue"; goto done;
    }
    object = [[queue commandBuffer] retain];
  } else if ([type isEqualToString:@"render-encoder"] || [type isEqualToString:@"blit-encoder"] ||
      [type isEqualToString:@"compute-encoder"]) {
    id<MTLCommandBuffer> command = objects[fields[@"commandBuffer"]];
    BOOL render = [type isEqualToString:@"render-encoder"], compute = [type isEqualToString:@"compute-encoder"];
    if (![fields[@"commandBuffer"] isKindOfClass:[NSNumber class]] || !command ||
        fields.count != (render || compute ? 2 : 1)) {
      error = @"invalid/unresolved encoder command-buffer"; goto done;
    }
    if (render) {
      MTLRenderPassDescriptor *pass = wmt_trace_pass_restore(fields[@"pass"], objects);
      if (!pass) { error = @"invalid/unresolved render pass"; goto done; }
      object = [[command renderCommandEncoderWithDescriptor:pass] retain];
    } else if (compute) {
      if (![fields[@"dispatchType"] isKindOfClass:[NSNumber class]] ||
          [fields[@"dispatchType"] unsignedIntegerValue] > MTLDispatchTypeConcurrent) {
        error = @"invalid compute dispatch type"; goto done;
      }
      object = [[command computeCommandEncoderWithDispatchType:[fields[@"dispatchType"] unsignedIntegerValue]] retain];
    } else object = [[command blitCommandEncoder] retain];
  } else if ([type isEqualToString:@"fence"]) {
    if (fields.count) { error = @"invalid fence constructor fields"; goto done; }
    object = [device newFence];
  } else if ([type isEqualToString:@"buffer"]) {
    id authority=fields[@"initial-CPU-authority"];
    BOOL authority_valid=!authority || [@[@"exclusive-native-allocation-before-PE-return",@"NOT_ENABLED"] containsObject:authority];
    if (!authority_valid || fields.count != 2+(authority?1:0) || ![fields[@"length"] isKindOfClass:[NSNumber class]] ||
        ![fields[@"options"] isKindOfClass:[NSNumber class]] ||
        ![fields[@"length"] unsignedLongLongValue] ||
        [fields[@"length"] unsignedLongLongValue] > UINT64_C(256) * 1024 * 1024) {
      error = @"invalid/over-budget buffer"; goto done;
    }
    object = [device newBufferWithLength:[fields[@"length"] unsignedLongLongValue]
        options:[fields[@"options"] unsignedLongLongValue]];
  } else if ([type isEqualToString:@"texture"] || [type isEqualToString:@"buffer-texture"]) {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    BOOL from_buffer = [type isEqualToString:@"buffer-texture"];
    NSDictionary *values = from_buffer ? fields[@"descriptor"] : fields;
    if (!wmt_trace_apply_fields(desc, values, wmt_trace_texture_fields(desc))) {
      [desc release]; error = @"invalid texture descriptor"; goto done;
    }
    if (from_buffer) {
      id<MTLBuffer> buffer = objects[fields[@"buffer"]];
      if (fields.count != 4 || !buffer ||
          ![fields[@"offset"] isKindOfClass:[NSNumber class]] ||
          ![fields[@"bytesPerRow"] isKindOfClass:[NSNumber class]]) {
        [desc release]; error = @"invalid buffer-texture parent/layout"; goto done;
      }
      object = getenv("MACRUNNER_WMT_NATIVE_BACKEND") ?
          wmt_trace_backend_texture(device,buffer,desc,[fields[@"offset"] unsignedLongLongValue],
              [fields[@"bytesPerRow"] unsignedLongLongValue],&error) :
          [buffer newTextureWithDescriptor:desc offset:[fields[@"offset"] unsignedLongLongValue]
              bytesPerRow:[fields[@"bytesPerRow"] unsignedLongLongValue]];
    } else object = getenv("MACRUNNER_WMT_NATIVE_BACKEND") ?
        wmt_trace_backend_texture(device,nil,desc,0,0,&error) : [device newTextureWithDescriptor:desc];
    [desc release];
  } else if ([type isEqualToString:@"texture-view"]) {
    id<MTLTexture> parent = objects[fields[@"texture"]];
    NSArray *swizzle = fields[@"swizzle"];
    BOOL valid = fields.count == 8 && parent && [swizzle isKindOfClass:[NSArray class]] && swizzle.count == 4;
    for (NSString *key in @[@"texture", @"pixelFormat", @"textureType", @"levelStart",
        @"levelCount", @"sliceStart", @"sliceCount"])
      valid = valid && [fields[key] isKindOfClass:[NSNumber class]];
    for (id channel in valid ? swizzle : @[])
      valid = valid && [channel isKindOfClass:[NSNumber class]] && [channel unsignedIntegerValue] <= MTLTextureSwizzleAlpha;
    if (!valid) { error = @"invalid texture-view descriptor"; goto done; }
    object = [parent newTextureViewWithPixelFormat:[fields[@"pixelFormat"] unsignedIntegerValue]
        textureType:[fields[@"textureType"] unsignedIntegerValue]
        levels:NSMakeRange([fields[@"levelStart"] unsignedIntegerValue], [fields[@"levelCount"] unsignedIntegerValue])
        slices:NSMakeRange([fields[@"sliceStart"] unsignedIntegerValue], [fields[@"sliceCount"] unsignedIntegerValue])
        swizzle:MTLTextureSwizzleChannelsMake([swizzle[0] unsignedIntegerValue], [swizzle[1] unsignedIntegerValue],
            [swizzle[2] unsignedIntegerValue], [swizzle[3] unsignedIntegerValue])];
  } else if ([type isEqualToString:@"sampler"]) {
    MTLSamplerDescriptor *desc = [[MTLSamplerDescriptor alloc] init];
    if (!wmt_trace_apply_fields(desc, fields, wmt_trace_sampler_fields(desc))) {
      [desc release]; error = @"invalid sampler descriptor"; goto done;
    }
    object = [device newSamplerStateWithDescriptor:desc]; [desc release];
  } else if ([type isEqualToString:@"depth-stencil"]) {
    MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
    if (fields.count != 4 || ![fields[@"depthCompareFunction"] isKindOfClass:[NSNumber class]] ||
        ![fields[@"depthWriteEnabled"] isKindOfClass:[NSNumber class]] ||
        !wmt_trace_apply_fields(desc.frontFaceStencil, fields[@"frontFaceStencil"],
            wmt_trace_stencil_fields(desc.frontFaceStencil)) ||
        !wmt_trace_apply_fields(desc.backFaceStencil, fields[@"backFaceStencil"],
            wmt_trace_stencil_fields(desc.backFaceStencil))) {
      [desc release]; error = @"invalid depth-stencil descriptor"; goto done;
    }
    desc.depthCompareFunction = [fields[@"depthCompareFunction"] unsignedIntegerValue];
    desc.depthWriteEnabled = [fields[@"depthWriteEnabled"] boolValue];
    object = [device newDepthStencilStateWithDescriptor:desc]; [desc release];
  } else if ([type isEqualToString:@"library"]) {
    NSData *bytes = fields[@"metallib"];
    if (fields.count != 1 || ![bytes isKindOfClass:[NSData class]] ||
        !bytes.length || bytes.length > UINT64_C(64) * 1024 * 1024) {
      error = @"invalid/over-budget metallib"; goto done;
    }
    dispatch_data_t data = dispatch_data_create(bytes.bytes, bytes.length,
        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *metal_error = nil;
    object = [device newLibraryWithData:data error:&metal_error]; dispatch_release(data);
    if (!object) error = metal_error.localizedDescription;
  } else if ([type isEqualToString:@"function"] || [type isEqualToString:@"function-specialized"]) {
    id<MTLLibrary> library = objects[fields[@"library"]];
    BOOL specialized = [type isEqualToString:@"function-specialized"];
    if (fields.count != (specialized ? 3 : 2) || !library || ![fields[@"name"] isKindOfClass:[NSString class]]) {
      error = @"invalid function parent/name"; goto done;
    }
    if (!specialized) object = [library newFunctionWithName:fields[@"name"]];
    else {
      NSArray *constants = fields[@"constants"];
      if (![constants isKindOfClass:[NSArray class]] || constants.count > 4096) {
        error = @"invalid/over-budget function constants"; goto done;
      }
      MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
      for (id value in constants) {
        if (![value isKindOfClass:[NSDictionary class]] || [value count] != 3 ||
            ![value[@"type"] isKindOfClass:[NSNumber class]] ||
            ![value[@"index"] isKindOfClass:[NSNumber class]] ||
            ![value[@"data"] isKindOfClass:[NSData class]]) {
          error = @"invalid function constant fields"; break;
        }
        MTLDataType type = [value[@"type"] unsignedIntegerValue];
        NSUInteger size = wmt_trace_constant_scalar_size(type);
        if (!size || [value[@"data"] length] != size || [value[@"index"] unsignedLongLongValue] > UINT16_MAX) {
          error = @"unsupported type/invalid constant size/index"; break;
        }
        [values setConstantValue:[value[@"data"] bytes] type:type atIndex:[value[@"index"] unsignedIntegerValue]];
      }
      NSError *metal_error = nil;
      if (!error) object = [library newFunctionWithName:fields[@"name"] constantValues:values error:&metal_error];
      if (!object && !error) error = metal_error.localizedDescription;
      [values release];
    }
  } else if ([type isEqualToString:@"render-pipeline"]) {
    MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
    NSArray *colors = fields[@"colors"], *vb = fields[@"vertexBuffers"], *fb = fields[@"fragmentBuffers"];
    BOOL valid = fields.count == 6 && [fields[@"vertex"] isKindOfClass:[NSNumber class]] &&
        [fields[@"fragment"] isKindOfClass:[NSNumber class]] &&
        [colors isKindOfClass:[NSArray class]] && colors.count == 8 &&
        [vb isKindOfClass:[NSArray class]] && vb.count == 31 &&
        [fb isKindOfClass:[NSArray class]] && fb.count == 31 &&
        wmt_trace_apply_fields(desc, fields[@"scalars"], wmt_trace_pipeline_scalars(desc));
    for (NSUInteger i = 0; valid && i < 8; ++i)
      valid = wmt_trace_apply_fields(desc.colorAttachments[i], colors[i],
          wmt_trace_pipeline_color(desc.colorAttachments[i]));
    for (NSUInteger i = 0; valid && i < 31; ++i) {
      valid = [vb[i] isKindOfClass:[NSNumber class]] && [fb[i] isKindOfClass:[NSNumber class]] &&
          [vb[i] unsignedIntegerValue] <= MTLMutabilityImmutable &&
          [fb[i] unsignedIntegerValue] <= MTLMutabilityImmutable;
      if (valid) { desc.vertexBuffers[i].mutability = [vb[i] unsignedIntegerValue];
        desc.fragmentBuffers[i].mutability = [fb[i] unsignedIntegerValue]; }
    }
    if (valid) {
      desc.vertexFunction = objects[fields[@"vertex"]]; desc.fragmentFunction = objects[fields[@"fragment"]];
      valid = (![fields[@"vertex"] unsignedLongLongValue] || desc.vertexFunction) &&
          (![fields[@"fragment"] unsignedLongLongValue] || desc.fragmentFunction);
    }
    NSError *metal_error = nil;
    if (valid) object = getenv("MACRUNNER_WMT_NATIVE_BACKEND") ?
        wmt_trace_backend_render_pipeline(device,desc,&error) :
        [device newRenderPipelineStateWithDescriptor:desc error:&metal_error];
    else error = @"invalid/unresolved render-pipeline descriptor";
    if (!object && !error) error = metal_error.localizedDescription;
    [desc release];
  } else error = @"unsupported constructor event";
done:
  if (!object && !error) error = @"Metal constructor failed";
  if (object) {
    NSDictionary *definition = wmt_trace_native_definition(event, objects);
    if (definition) wmt_trace_native_definitions(objects)[identifier] = definition;
    objects[identifier] = object;
    [object release];
  }
  if (failure) *failure = error;
  return object && !error;
}

/* Rejection must leave owned unsubmitted encoders closed. Never commit a
 * partially decoded command buffer as cleanup. The original failure wins. */
static void wmt_trace_native_close_encoders(NSMutableDictionary *objects, NSMutableSet *active) {
  for (NSNumber *identifier in [active allObjects]) {
    @try { [objects[identifier] endEncoding]; }
    @catch (NSException *exception) {
      fprintf(stderr, "ENCODER_CLOSE object=%llu exception=%s\n",
          identifier.unsignedLongLongValue, exception.name.UTF8String);
    }
  }
  [active removeAllObjects];
}
#endif
