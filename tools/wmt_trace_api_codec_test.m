/* CPU-only generated ABI codec regression; never creates a Metal device. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <objc/runtime.h>
#include <pthread.h>
#include "../src/winemetal/unix/wmt_trace_relocate.h"
static char wmt_trace_resource_key;
static NSMutableArray *events;
static uint64_t next_id;
static char id_key;
static uint64_t wmt_trace_object_id(id object) {
  if(!object)return 0;
  NSNumber *value=objc_getAssociatedObject(object,&id_key);
  if(!value) { value=@(++next_id);objc_setAssociatedObject(object,&id_key,value,OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
  return value.unsignedLongLongValue;
}
static void wmt_trace_record_event(NSString *type,id object,NSDictionary *fields) {
  [events addObject:@{@"event":type,@"fields":[[fields copy] autorelease]}];
}
static void wmt_trace_api_device(id<MTLDevice> device) { (void)device; }
#include "../src/winemetal/unix/wmt_trace_api_record.h"
@interface CpuTexture : NSObject
@property MTLPixelFormat pixelFormat;
@end
@implementation CpuTexture
@end
static NSDictionary *last_packet(void) {
  assert([events.lastObject[@"event"] isEqual:@"api-call"]);return events.lastObject[@"fields"];
}
static BOOL decode(NSDictionary *packet,NSMutableDictionary *objects) {
  NSMutableArray *arena=[NSMutableArray array];NSString *error=nil;
  BOOL ok=wmt_trace_api_decode(packet,objects,nil,YES,arena,&error)!=nil;
  assert(ok?error==nil:error!=nil);return ok;
}
int main(void) {
 @autoreleasepool {
  events=[NSMutableArray array];setenv("MACRUNNER_WMT_RECORD","cpu-fixture",1);
  NSObject *device=[NSObject new],*event=[NSObject new],*command=[NSObject new],*function=[NSObject new],*pso=[NSObject new];
  struct unixcall_generic_obj_obj_ret create={(obj_handle_t)device,0};
  void *ticket=wmt_trace_api_begin(15,&create);create.ret=(obj_handle_t)event;
  wmt_trace_api_end(15,&create,0,ticket);
  NSDictionary *shared=last_packet();
  NSMutableDictionary *known=[NSMutableDictionary dictionary];
  assert(decode(shared,known));
  struct unixcall_generic_obj_obj_uint64_noret signal={(obj_handle_t)command,(obj_handle_t)event,42};
  ticket=wmt_trace_api_begin(17,&signal);wmt_trace_api_end(17,&signal,0,ticket);
  NSDictionary *s=last_packet();known[@(wmt_trace_object_id(command))]=@1;
  assert(decode(s,known));
  NSMutableDictionary *broken=[[s mutableCopy] autorelease];
  broken[@"handles"]=@[s[@"handles"][0]];assert(!decode(broken,known));
  broken=[[s mutableCopy] autorelease];NSMutableData *bad=[NSMutableData dataWithData:s[@"params"]];
  ((uint64_t *)bad.mutableBytes)[0]=0x12345678;broken[@"params"]=bad;assert(!decode(broken,known));
  broken=[[s mutableCopy] autorelease];broken[@"ordinal"]=@83;assert(!decode(broken,known));

  struct WMTComputePipelineInfo info;memset(&info,0xA5,sizeof(info));
  info.compute_function=(obj_handle_t)function;info.binary_archives_for_lookup.ptr=NULL;
  info.binary_archive_for_serialization=0;info.num_binary_archives_for_lookup=0;
  info.fail_on_binary_archive_miss=false;info.tgsize_is_multiple_of_sgwidth=true;info.immutable_buffers=7;
  struct unixcall_mtldevice_newcomputepso compute={0};
  compute.device=(obj_handle_t)device;compute.info.ptr=&info;
  ticket=wmt_trace_api_begin(29,&compute);compute.ret_pso=(obj_handle_t)pso;
  wmt_trace_api_end(29,&compute,0,ticket);NSDictionary *c=last_packet();
  known[@(wmt_trace_object_id(function))]=@2;assert(decode(c,known));
  NSData *descriptor=c[@"payloads"][0][@"data"];
  assert(((const struct WMTComputePipelineInfo *)descriptor.bytes)->padding==0);
  broken=[[c mutableCopy] autorelease];
  NSMutableDictionary *payload=[[c[@"payloads"][0] mutableCopy] autorelease];
  bad=[NSMutableData dataWithData:payload[@"data"]];[bad increaseLengthBy:1];
  payload[@"data"]=bad;broken[@"payloads"]=@[payload];assert(!decode(broken,known));
  broken=[[c mutableCopy] autorelease];broken[@"outputs"]=@[];assert(!decode(broken,known));
  NSMutableDictionary *missing=[NSMutableDictionary dictionary];assert(!decode(c,missing));
  info.num_binary_archives_for_lookup=1;
  ticket=wmt_trace_api_begin(29,&compute);wmt_trace_api_end(29,&compute,0,ticket);
  assert([last_packet()[@"status"] isEqual:@"FAILED"]);

  CpuTexture *texture=[CpuTexture new];
  struct unixcall_mtltexture_replaceregion replace={0};uint64_t length;
  texture.pixelFormat=MTLPixelFormatRGBA8Unorm;
  replace.size.width=3;replace.size.height=2;replace.size.depth=2;
  replace.bytes_per_row=16;replace.bytes_per_image=64;
  assert(wmt_trace_api_texture_extent((id<MTLTexture>)texture,&replace,&length) && length==92);
  texture.pixelFormat=MTLPixelFormatBC1_RGBA;replace.size.width=5;replace.size.height=5;
  replace.size.depth=1;replace.bytes_per_row=24;
  assert(wmt_trace_api_texture_extent((id<MTLTexture>)texture,&replace,&length) && length==40);
  replace.size.width=0;assert(wmt_trace_api_texture_extent((id<MTLTexture>)texture,&replace,&length)&&length==0);
  replace.size.width=UINT64_MAX;assert(!wmt_trace_api_texture_extent((id<MTLTexture>)texture,&replace,&length));
  texture.pixelFormat=MTLPixelFormatInvalid;assert(!wmt_trace_api_texture_extent((id<MTLTexture>)texture,&replace,&length));
  /* Sweep every additional effect handler and sibling descriptor shape. */
  unsigned swept=0;
  for(unsigned di=0;di<sizeof(wmt_trace_api_descriptors)/sizeof(*wmt_trace_api_descriptors);di++) {
    const struct wmt_trace_api_desc *desc=&wmt_trace_api_descriptors[di];
    if(strcmp(desc->record,"GENERATED_TYPED_CALL"))continue;
    NSMutableData *abi=[NSMutableData dataWithLength:desc->bytes];
    uint8_t scratch[4096]={0};
    for(unsigned fi=0;fi<desc->fields;fi++) {
      const struct wmt_trace_api_field *field=&desc->field[fi];
      if(!strcmp(field->kind,"HANDLE") && wmt_trace_api_field_in(field)) {
        id obj=desc->ordinal==45?(id)texture:(id)command;
        uint64_t value=(uint64_t)obj;
        memcpy((char *)abi.mutableBytes+field->offset,&value,8);
        known[@(wmt_trace_object_id(obj))]=@1;
      }
      if(!strcmp(field->kind,"POINTER")) {
        uint64_t pointer=(uint64_t)scratch;
        memcpy((char *)abi.mutableBytes+field->offset,&pointer,8);
      }
    }
    if(desc->ordinal==45) {
      texture.pixelFormat=MTLPixelFormatRGBA8Unorm;
      struct unixcall_mtltexture_replaceregion *p=abi.mutableBytes;
      p->size.width=2;p->size.height=1;p->size.depth=1;p->bytes_per_row=8;p->bytes_per_image=8;
    }
    if(desc->ordinal==129) {
      struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers *p=abi.mutableBytes;
      p->num_attachments=1;
      ((struct WMTSampleBufferAttachmentInfo *)scratch)->sample_buffer=(obj_handle_t)command;
    }
    ticket=wmt_trace_api_begin(desc->ordinal,abi.mutableBytes);
    for(unsigned fi=0;fi<desc->fields;fi++) {
      const struct wmt_trace_api_field *field=&desc->field[fi];
      if(!strcmp(field->kind,"HANDLE") && wmt_trace_api_field_out(field)) {
        uint64_t output=!strcmp(field->object_type,"NSError")?0:(uint64_t)pso;
        memcpy((char *)abi.mutableBytes+field->offset,&output,8);
      }
    }
    wmt_trace_api_end(desc->ordinal,abi.mutableBytes,0,ticket);
    assert(decode(last_packet(),known));++swept;
  }
  assert(swept==17);
  /* Pool IDs never enter ObjC weak maps or the native Metal object registry. */
  struct unixcall_generic_obj_ret pool_result={(obj_handle_t)command};
  wmt_trace_api_end(10,&pool_result,0,NULL);
  obj_handle_t raw_pool=(obj_handle_t)command;
  wmt_trace_api_begin(1,&raw_pool);wmt_trace_api_lifetime_flush();
  assert([events.lastObject[@"event"] isEqual:@"api-lifetime"]);
  NSDictionary *pool_delta=events.lastObject[@"fields"][@"deltas"][0];
  assert([pool_delta[@"pool"] boolValue] && [pool_delta[@"retain"] intValue]==1 &&
      [pool_delta[@"release"] intValue]==1 && [pool_delta[@"object"] unsignedLongLongValue]>>63);
  wmt_trace_api_begin(120,&create);assert([events.lastObject[@"fields"][@"status"] isEqual:@"FAILED"]);
  assert([events[0][@"fields"][@"calls"] count]==134);
  unsetenv("MACRUNNER_WMT_RECORD");NSUInteger count=events.count;
  assert(wmt_trace_api_begin(15,&create)==NULL);wmt_trace_api_end(15,&create,0,NULL);
  assert(events.count==count);
  [texture release];[pso release];[function release];[command release];[event release];[device release];
  puts("API_CODEC PASS: policy134; all17 typed effects; shared-event+compute/mesh/tile/FX siblings; pools; raw-pointer/missing-handle/output/shape/reserved rejection; archive rejection; RGBA/BC/zero/overflow extents; gate-off; GPU=NOT_ENABLED Wine=NOT_ENABLED");
 }return 0;
}
