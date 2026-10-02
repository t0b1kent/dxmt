#ifndef WMT_TRACE_API_CODEC_H
#define WMT_TRACE_API_CODEC_H
#import <Foundation/Foundation.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "../winemetal_thunks.h"
#include "wmt_trace_api_metadata.h"

/* Raw ABI blobs contain defined scalar fields only. Pointer/object slots and
 * padding are zero. Handles and pointed-to bytes travel as separate records.
 * Same checked decoder is used before device creation and during replay. */
static const uint64_t wmt_trace_api_payload_cap = 64ull*1024*1024;
static const struct wmt_trace_api_desc *wmt_trace_api_desc(unsigned ordinal) {
  for (unsigned i=0;i<sizeof(wmt_trace_api_descriptors)/sizeof(*wmt_trace_api_descriptors);i++)
    if (wmt_trace_api_descriptors[i].ordinal==ordinal) return &wmt_trace_api_descriptors[i];
  return NULL;
}
static uint64_t wmt_trace_api_word(const void *bytes,unsigned offset,unsigned size) {
  uint64_t result=0;
  if (size<=sizeof(result)) memcpy(&result,(const char *)bytes+offset,size);
  return result;
}
static BOOL wmt_trace_api_bound(uint64_t offset,uint64_t length,uint64_t total) {
  return offset<=total && length<=total-offset;
}
static BOOL wmt_trace_api_mul(uint64_t a,uint64_t b,uint64_t *result) {
  if (b && a>UINT64_MAX/b) return NO;
  *result=a*b;return YES;
}
static BOOL wmt_trace_api_add(uint64_t a,uint64_t b,uint64_t *result) {
  if (b>UINT64_MAX-a) return NO;
  *result=a+b;return YES;
}
static BOOL wmt_trace_api_pack_failure(NSMutableDictionary *packet,NSString *reason) {
  packet[@"status"]=@"FAILED";packet[@"reason"]=reason;return NO;
}
static BOOL wmt_trace_api_field_in(const struct wmt_trace_api_field *field) {
  return !strcmp(field->direction,"IN") || !strcmp(field->direction,"INOUT");
}
static BOOL wmt_trace_api_field_out(const struct wmt_trace_api_field *field) {
  return !strcmp(field->direction,"OUT") || !strcmp(field->direction,"INOUT");
}

/* Descriptor decoder rejects nonzero pointer/object slots in byte payloads,
 * duplicate fixups, uncreated handles and invalid root slots before Metal. */
static BOOL wmt_trace_api_validate_layout(NSData *bytes,NSArray *relocations,
    NSData *mask,NSSet *expected,NSString *type) {
  if(bytes.length!=mask.length || relocations.count!=expected.count)return NO;
  const uint8_t *b=bytes.bytes,*m=mask.bytes;
  for(NSUInteger i=0;i<bytes.length;i++)if(!m[i] && b[i])return NO;
  NSMutableSet *seen=[NSMutableSet set];
  for(NSDictionary *r in relocations) {
    NSNumber *offset=r[@"offset"];
    if(![expected containsObject:offset] || [seen containsObject:offset] ||
        ![r[@"type"] isEqual:type])return NO;
    [seen addObject:offset];
  }
  return YES;
}
static BOOL wmt_trace_api_validate_payload(unsigned ordinal,unsigned root,
    NSData *bytes,NSArray *relocations,NSData *params) {
  switch(ordinal) {
#include "wmt_trace_api_payload_validation.h"
  case 45:return root==offsetof(struct unixcall_mtltexture_replaceregion,data) &&
      relocations.count==0;
  case 129: {
    uint64_t count=wmt_trace_api_word(params.bytes,
        offsetof(struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers,num_attachments),8),length=0;
    if(root!=offsetof(struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers,attachments) ||
        !wmt_trace_api_mul(count,sizeof(struct WMTSampleBufferAttachmentInfo),&length) ||
        length!=bytes.length || relocations.count!=count)return NO;
    NSMutableData *mask=[NSMutableData dataWithLength:length];
    NSMutableSet *expected=[NSMutableSet set];
    for(uint64_t i=0;i<count;i++) {
      [expected addObject:@(i*sizeof(struct WMTSampleBufferAttachmentInfo))];
      memset((char *)mask.mutableBytes+i*sizeof(struct WMTSampleBufferAttachmentInfo)+8,1,16);
    }
    return wmt_trace_api_validate_layout(bytes,relocations,mask,expected,@"MTLCounterSampleBuffer");
  }
  default:return NO;
  }
}
static NSMutableData *wmt_trace_api_decode(NSDictionary *packet,
    NSMutableDictionary *objects,id device,BOOL inspect,NSMutableArray *arena,
    NSString **failure) {
  unsigned ordinal=[packet[@"ordinal"] unsignedIntValue];
  const struct wmt_trace_api_desc *desc=wmt_trace_api_desc(ordinal);
  NSData *raw=packet[@"params"];
  NSArray *handles=packet[@"handles"], *payloads=packet[@"payloads"], *outputs=packet[@"outputs"];
  if (!desc || strcmp(desc->replay,"NATIVE_TYPED_CALL") ||
      ![raw isKindOfClass:[NSData class]] || raw.length!=desc->bytes ||
      ![handles isKindOfClass:[NSArray class]] || ![payloads isKindOfClass:[NSArray class]] ||
      ![outputs isKindOfClass:[NSArray class]] || payloads.count>256 ||
      ![packet[@"status"] isEqual:@"PRESENT"]) {
    *failure=@"typed API schema/policy/ABI mismatch";return nil;
  }
  NSMutableData *params=[NSMutableData dataWithData:raw];
  [arena addObject:params];
  NSMutableSet *slots=[NSMutableSet set],*roots=[NSMutableSet set];
  for (unsigned i=0;i<desc->fields;i++) {
    const struct wmt_trace_api_field *f=&desc->field[i];
    if (strcmp(f->kind,"VALUE") || !wmt_trace_api_field_in(f)) {
      const unsigned char *p=(const unsigned char *)raw.bytes+f->offset;
      for (unsigned k=0;k<f->size;k++) if (p[k]) {
        *failure=@"typed API raw pointer/output/object slot is not zero";return nil;
      }
    }
  }
  for (NSDictionary *h in handles) {
    unsigned offset=[h[@"offset"] unsignedIntValue];
    const struct wmt_trace_api_field *field=NULL;
    for (unsigned i=0;i<desc->fields;i++)
      if(desc->field[i].offset==offset && !strcmp(desc->field[i].kind,"HANDLE") &&
          wmt_trace_api_field_in(&desc->field[i])) field=&desc->field[i];
    if (!field || field->size!=8 || [slots containsObject:@(offset)] ||
        ![h[@"type"] isEqual:[NSString stringWithUTF8String:field->object_type]]) {
      *failure=@"typed API invalid/duplicate input handle slot";return nil;
    }
    [slots addObject:@(offset)];
    uint64_t identifier=[h[@"object"] unsignedLongLongValue];
    id object=identifier?objects[@(identifier)]:nil;
    if (identifier && !strcmp(field->object_type,"MTLDevice")) object=inspect?@(identifier):device;
    if (identifier && !object) { *failure=@"typed API missing input object";return nil; }
    uint64_t value=(uint64_t)object;memcpy((char *)params.mutableBytes+offset,&value,8);
  }
  for (unsigned i=0;i<desc->fields;i++)
    if (!strcmp(desc->field[i].kind,"HANDLE") && wmt_trace_api_field_in(&desc->field[i]) &&
        ![slots containsObject:@(desc->field[i].offset)]) {
      *failure=@"typed API omitted input handle";return nil;
    }
  uint64_t total=0;
  for (NSDictionary *payload in payloads) {
    NSData *bytes=payload[@"data"];
    NSArray *relocations=payload[@"handles"];
    unsigned offset=[payload[@"root-offset"] unsignedIntValue];
    const struct wmt_trace_api_field *field=NULL;
    for(unsigned i=0;i<desc->fields;i++)
      if(desc->field[i].offset==offset && !strcmp(desc->field[i].kind,"POINTER") &&
          wmt_trace_api_field_in(&desc->field[i])) field=&desc->field[i];
    if (!field || field->size!=8 || [roots containsObject:@(offset)] ||
        ![bytes isKindOfClass:[NSData class]] ||
        ![relocations isKindOfClass:[NSArray class]] ||
        !wmt_trace_api_add(total,bytes.length,&total) || total>wmt_trace_api_payload_cap ||
        !wmt_trace_api_validate_payload(ordinal,offset,bytes,relocations,raw)) {
      *failure=@"typed API payload cap/root/shape rejected";return nil;
    }
    [roots addObject:@(offset)];
    NSMutableData *copy=[NSMutableData dataWithData:bytes];[arena addObject:copy];
    NSMutableSet *seen=[NSMutableSet set];
    for(NSDictionary *r in relocations) {
      uint64_t at=[r[@"offset"] unsignedLongLongValue],identifier=[r[@"object"] unsignedLongLongValue];
      if(!wmt_trace_api_bound(at,8,copy.length) || [seen containsObject:@(at)] ||
          wmt_trace_api_word(copy.bytes,(unsigned)at,8)) {
        *failure=@"typed API payload object slot rejected";return nil;
      }
      [seen addObject:@(at)];
      id object=identifier?objects[@(identifier)]:nil;
      if(identifier&&!object) { *failure=@"typed API missing descriptor object";return nil; }
      uint64_t value=(uint64_t)object;memcpy((char *)copy.mutableBytes+at,&value,8);
    }
    uint64_t value=copy.length?(uint64_t)copy.mutableBytes:0;
    memcpy((char *)params.mutableBytes+offset,&value,8);
  }
  for(unsigned i=0;i<desc->fields;i++)
    if(!strcmp(desc->field[i].kind,"POINTER") && wmt_trace_api_field_in(&desc->field[i]) &&
        ![roots containsObject:@(desc->field[i].offset)]) {
      *failure=@"typed API omitted pointer payload";return nil;
    }
  NSMutableSet *seen=[NSMutableSet set];
  for(NSDictionary *out in outputs) {
    unsigned offset=[out[@"offset"] unsignedIntValue];
    const struct wmt_trace_api_field *field=NULL;
    for(unsigned i=0;i<desc->fields;i++)
      if(desc->field[i].offset==offset && !strcmp(desc->field[i].kind,"HANDLE") &&
          wmt_trace_api_field_out(&desc->field[i])) field=&desc->field[i];
    if(!field || field->size!=8 || [seen containsObject:@(offset)] ||
        ![out[@"type"] isEqual:[NSString stringWithUTF8String:field->object_type]]) {
      *failure=@"typed API output slot mismatch";return nil;
    }
    [seen addObject:@(offset)];
    uint64_t identifier=[out[@"object"] unsignedLongLongValue];
    if(inspect && identifier) objects[@(identifier)]=@(identifier);
  }
  for(unsigned i=0;i<desc->fields;i++)
    if(!strcmp(desc->field[i].kind,"HANDLE") && wmt_trace_api_field_out(&desc->field[i]) &&
        ![seen containsObject:@(desc->field[i].offset)]) {
      *failure=@"typed API missing output declaration";return nil;
    }
  return params;
}
#endif
