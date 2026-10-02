#ifndef WMT_TRACE_API_RECORD_H
#define WMT_TRACE_API_RECORD_H
#include <stdatomic.h>
#include "wmt_trace_api_codec.h"
#include "wmt_trace_api_abi_asserts.h"
#include "wmt_trace_api_lifetime.h"

static _Atomic uint64_t wmt_trace_api_counts[135];
static BOOL wmt_trace_api_policy_written;
static void wmt_trace_api_pack_handle(NSMutableArray *handles,NSMutableData *data,
    unsigned offset,id object,NSString *type,BOOL output) {
  uint64_t identifier=wmt_trace_object_id(object);
  [handles addObject:@{@"offset":@(offset),@"object":@(identifier),@"type":type}];
  memset((char *)data.mutableBytes+offset,0,8);
}
static BOOL wmt_trace_api_texture_extent(id<MTLTexture> texture,
    const struct unixcall_mtltexture_replaceregion *p,uint64_t *length) {
  uint64_t bw=1,bh=1,unit=0;
  switch(texture.pixelFormat) {
    case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm:
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: unit=1;break;
    case MTLPixelFormatR16Unorm:case MTLPixelFormatR16Snorm:case MTLPixelFormatR16Uint:
    case MTLPixelFormatR16Sint:case MTLPixelFormatR16Float:case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Snorm:case MTLPixelFormatRG8Uint:case MTLPixelFormatRG8Sint:unit=2;break;
    case MTLPixelFormatR32Uint:case MTLPixelFormatR32Sint:case MTLPixelFormatR32Float:
    case MTLPixelFormatRG16Unorm:case MTLPixelFormatRG16Snorm:case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRG16Sint:case MTLPixelFormatRG16Float:case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:case MTLPixelFormatRGBA8Snorm:case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatRGBA8Sint:case MTLPixelFormatBGRA8Unorm:case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGB10A2Unorm:case MTLPixelFormatRGB10A2Uint:
    case MTLPixelFormatRG11B10Float:case MTLPixelFormatRGB9E5Float:unit=4;break;
    case MTLPixelFormatRG32Uint:case MTLPixelFormatRG32Sint:case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA16Unorm:case MTLPixelFormatRGBA16Snorm:case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatRGBA16Sint:case MTLPixelFormatRGBA16Float:unit=8;break;
    case MTLPixelFormatRGBA32Uint:case MTLPixelFormatRGBA32Sint:case MTLPixelFormatRGBA32Float:unit=16;break;
    case MTLPixelFormatBC1_RGBA:case MTLPixelFormatBC1_RGBA_sRGB:
    case MTLPixelFormatBC4_RUnorm:case MTLPixelFormatBC4_RSnorm:bw=bh=4;unit=8;break;
    case MTLPixelFormatBC2_RGBA:case MTLPixelFormatBC2_RGBA_sRGB:
    case MTLPixelFormatBC3_RGBA:case MTLPixelFormatBC3_RGBA_sRGB:
    case MTLPixelFormatBC5_RGUnorm:case MTLPixelFormatBC5_RGSnorm:
    case MTLPixelFormatBC6H_RGBFloat:case MTLPixelFormatBC6H_RGBUfloat:
    case MTLPixelFormatBC7_RGBAUnorm:case MTLPixelFormatBC7_RGBAUnorm_sRGB:bw=bh=4;unit=16;break;
    default:return NO;
  }
  uint64_t w=p->size.width,h=p->size.height,d=p->size.depth;
  if(!w||!h||!d) { *length=0;return YES; }
  if(w>UINT64_MAX-bw+1 || h>UINT64_MAX-bh+1) return NO;
  uint64_t row,rows=(h+bh-1)/bh,last,images;
  if(!wmt_trace_api_mul((w+bw-1)/bw,unit,&row) ||
      (rows>1 && p->bytes_per_row<row) ||
      !wmt_trace_api_mul(rows-1,p->bytes_per_row,&last) ||
      !wmt_trace_api_add(last,row,&last) ||
      (d>1 && p->bytes_per_image<last) ||
      !wmt_trace_api_mul(d-1,p->bytes_per_image,&images) ||
      !wmt_trace_api_add(images,last,length)) return NO;
  return *length<=wmt_trace_api_payload_cap;
}
static BOOL wmt_trace_api_pack_payloads(unsigned ordinal,void *params,NSMutableDictionary *packet) {
  switch(ordinal) {
#include "wmt_trace_api_payloads.h"
  case 45: {
    struct unixcall_mtltexture_replaceregion *p=params;uint64_t length=0;
    if(!wmt_trace_api_texture_extent((id<MTLTexture>)p->texture,p,&length) || (length&&!p->data.ptr))
      return wmt_trace_api_pack_failure(packet,@"unknown texture layout/invalid extent/cap");
    [packet[@"payloads"] addObject:@{@"root-offset":@(offsetof(struct unixcall_mtltexture_replaceregion,data)),
        @"data":[NSData dataWithBytes:p->data.ptr length:length],@"handles":@[]}];break;
  }
  case 129: {
    struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers *p=params;uint64_t size=0;
    if(!wmt_trace_api_mul(p->num_attachments,sizeof(struct WMTSampleBufferAttachmentInfo),&size) ||
        size>wmt_trace_api_payload_cap || (size&&!p->attachments.ptr))
      return wmt_trace_api_pack_failure(packet,@"sample attachments extent/cap");
    NSMutableData *data=[NSMutableData dataWithLength:size];
    NSMutableArray *handles=[NSMutableArray array];
    const struct WMTSampleBufferAttachmentInfo *attachments=p->attachments.ptr;
    for(uint64_t i=0;i<p->num_attachments;i++) {
      uint64_t at=i*sizeof(*attachments);
      wmt_trace_api_pack_handle(handles,data,(unsigned)(at+offsetof(struct WMTSampleBufferAttachmentInfo,sample_buffer)),
          (id)attachments[i].sample_buffer,@"MTLCounterSampleBuffer",NO);
      memcpy((char *)data.mutableBytes+at+offsetof(struct WMTSampleBufferAttachmentInfo,start_of_encoder_sample_index),
          &attachments[i].start_of_encoder_sample_index,8);
      memcpy((char *)data.mutableBytes+at+offsetof(struct WMTSampleBufferAttachmentInfo,end_of_encoder_sample_index),
          &attachments[i].end_of_encoder_sample_index,8);
    }
    [packet[@"payloads"] addObject:@{@"root-offset":@(offsetof(struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers,attachments)),
        @"data":data,@"handles":handles}];break;
  }
  default:break;
  }
  return YES;
}
static void *wmt_trace_api_begin(unsigned ordinal,void *params) {
  const char *gate=getenv("MACRUNNER_WMT_RECORD");
  if(!gate||!*gate)return NULL;
  const struct wmt_trace_api_desc *desc=wmt_trace_api_desc(ordinal);
  if(!desc)return NULL; /* Generated table/build guard must make this unreachable. */
  atomic_fetch_add_explicit(&wmt_trace_api_counts[ordinal],1,memory_order_relaxed);
  for(unsigned i=0;i<desc->fields;i++) {
    const struct wmt_trace_api_field *field=&desc->field[i];
    if(wmt_trace_api_field_in(field) && !strcmp(field->kind,"HANDLE") &&
        !strcmp(field->object_type,"MTLDevice"))
      wmt_trace_api_device((id<MTLDevice>)wmt_trace_api_word(params,field->offset,field->size));
  }
  if(ordinal==0 || ordinal==1) {
    id object=*(id *)params;
    wmt_trace_api_object_delta(object,ordinal==0?1:-1);
  }
  if(ordinal==12)wmt_trace_api_lifetime_flush();
  @synchronized([NSPropertyListSerialization class]) {
    if(!wmt_trace_api_policy_written) {
      NSMutableArray *calls=[NSMutableArray array];
      for(unsigned i=0;i<sizeof(wmt_trace_api_descriptors)/sizeof(*wmt_trace_api_descriptors);i++) {
        const struct wmt_trace_api_desc *d=&wmt_trace_api_descriptors[i];
        [calls addObject:@{@"ordinal":@(d->ordinal),@"name":[NSString stringWithUTF8String:d->name],
            @"record":[NSString stringWithUTF8String:d->record],@"replay":[NSString stringWithUTF8String:d->replay]}];
      }
      wmt_trace_record_event(@"api-policy",@"WMT_API_POLICY",@{@"status":@"PRESENT",@"calls":calls,
          @"scope":@"GPU effect stream; CPU producer/query results are projected through final consumer artifacts"});
      wmt_trace_api_policy_written=YES;
    }
  }
  if(!strcmp(desc->record,"EXTERNAL_REQUIRED")) {
    wmt_trace_record_event(@"api-external",@"WMT_API_POLICY",@{@"status":@"FAILED",@"ordinal":@(ordinal),
        @"reason":@"Foreign import requires transferred rights/producer capture; this tape cannot recreate it"});
    return NULL;
  }
  if(strcmp(desc->record,"GENERATED_TYPED_CALL"))return NULL;
  NSMutableData *data=[NSMutableData dataWithLength:desc->bytes];
  NSMutableDictionary *packet=[@{@"ordinal":@(ordinal),@"params":data,
      @"handles":[NSMutableArray array],@"outputs":[NSMutableArray array],
      @"payloads":[NSMutableArray array],@"status":@"PRESENT"} mutableCopy];
  for(unsigned i=0;i<desc->fields;i++) {
    const struct wmt_trace_api_field *f=&desc->field[i];
    if(!wmt_trace_api_field_in(f))continue;
    if(!strcmp(f->kind,"HANDLE"))
      wmt_trace_api_pack_handle(packet[@"handles"],data,f->offset,
          (id)wmt_trace_api_word(params,f->offset,f->size),[NSString stringWithUTF8String:f->object_type],NO);
    else if(!strcmp(f->kind,"VALUE"))
      memcpy((char *)data.mutableBytes+f->offset,(char *)params+f->offset,f->size);
  }
  wmt_trace_api_pack_payloads(ordinal,params,packet);
  BOOL has_output=NO;
  for(unsigned i=0;i<desc->fields;i++)
    has_output |= wmt_trace_api_field_out(&desc->field[i]);
  /* Observable effects without returns must be logged before they can wake
   * another thread. Constructor returns stay private until dispatch returns. */
  if(!has_output) {
    wmt_trace_record_event(@"api-call",@"WMT_API_POLICY",packet);
    [packet release];return NULL;
  }
  return packet;
}
static void wmt_trace_api_end(unsigned ordinal,void *params,int status,void *ticket) {
  const char *gate=getenv("MACRUNNER_WMT_RECORD");
  if(!gate||!*gate)return;
  const struct wmt_trace_api_desc *desc=wmt_trace_api_desc(ordinal);
  if(ordinal==10 && status==0)
    wmt_trace_api_pool_created((const void *)((struct unixcall_generic_obj_ret *)params)->ret);
  if(ticket) {
    NSMutableDictionary *packet=ticket;
    if(status)wmt_trace_api_pack_failure(packet,@"non-success native dispatch status");
    if(![packet[@"status"] isEqual:@"FAILED"]) {
      for(unsigned i=0;i<desc->fields;i++) {
        const struct wmt_trace_api_field *f=&desc->field[i];
        if(!wmt_trace_api_field_out(f))continue;
        if(!strcmp(f->kind,"HANDLE")) {
          id object=(id)wmt_trace_api_word(params,f->offset,f->size);
          wmt_trace_api_pack_handle(packet[@"outputs"],packet[@"params"],f->offset,object,
              [NSString stringWithUTF8String:f->object_type],YES);
        } else if(!strcmp(f->kind,"VALUE")) {
          /* CPU result is evidence, not an instruction to the native backend. */
          if(!packet[@"scalar-results"])packet[@"scalar-results"]=[NSMutableDictionary dictionary];
          packet[@"scalar-results"][[NSString stringWithUTF8String:f->name]]=
              [NSData dataWithBytes:(char *)params+f->offset length:f->size];
        }
      }
    }
    wmt_trace_record_event(@"api-call",@"WMT_API_POLICY",packet);[packet release];
  }
  if(ordinal==12) {
    NSMutableArray *counts=[NSMutableArray arrayWithCapacity:135];
    for(unsigned i=0;i<135;i++)[counts addObject:@(atomic_load_explicit(&wmt_trace_api_counts[i],memory_order_relaxed))];
    wmt_trace_record_event(@"api-census",@"WMT_API_POLICY",@{@"status":@"PRESENT",@"counts":counts,
        @"scope":@"cumulative through this commit; final process tail UNKNOWN until terminal snapshot"});
  }
}
#endif
