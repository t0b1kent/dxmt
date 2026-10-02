#ifndef WMT_TRACE_API_NATIVE_H
#define WMT_TRACE_API_NATIVE_H
#include "wmt_trace_api_codec.h"
#include "wmt_trace_native_backend.h"
/* Rebased packed commands execute through the selected build's actual
 * unix implementation. The reconstructed bytes live in the caller's arena. */
static uint64_t wmt_trace_native_command_batches[4];
static BOOL wmt_trace_api_native_commands(unsigned kind,id encoder,
    const struct wmtcmd_base *head,NSString **failure) {
  if(kind<1||kind>3||!encoder||!head) { *failure=@"invalid native command packet";return NO; }
  if(!wmt_trace_native_api_table&&!wmt_trace_api_load_native(failure))return NO;
  unsigned ordinal=35+kind; /* public ABI36 blit,37 compute,38 render */
  wmt_trace_api_native_fn function=(wmt_trace_api_native_fn)wmt_trace_native_api_table[ordinal];
  if(!function) { *failure=@"selected backend command slot missing";return NO; }
  struct unixcall_generic_obj_cmd_noret params={0};
  params.encoder=(obj_handle_t)encoder;
  params.cmd_head.ptr=head;
  if(function(&params)) { *failure=@"selected backend command dispatch failed";return NO; }
  ++wmt_trace_native_command_batches[kind];
  return YES;
}
static BOOL wmt_trace_api_native(NSDictionary *packet,id device,
    NSMutableDictionary *objects,NSMutableSet *active_encoders,NSString **failure) {
  NSMutableArray *arena=[NSMutableArray array];
  NSMutableData *params=wmt_trace_api_decode(packet,objects,device,NO,arena,failure);
  if(!params)return NO;
  unsigned ordinal=[packet[@"ordinal"] unsignedIntValue];
  if(!wmt_trace_native_api_table && !wmt_trace_api_load_native(failure))return NO;
  wmt_trace_api_native_fn function=(wmt_trace_api_native_fn)wmt_trace_native_api_table[ordinal];
  if(!function) { *failure=@"native typed dispatch slot missing";return NO; }
  int status=function(params.mutableBytes);
  if(status) { *failure=@"native typed dispatch failed";return NO; }
  for(NSDictionary *out in packet[@"outputs"]) {
    unsigned offset=[out[@"offset"] unsignedIntValue];
    uint64_t identifier=[out[@"object"] unsignedLongLongValue];
    id result=(id)wmt_trace_api_word(params.bytes,offset,8);
    if((identifier!=0)!=(result!=nil)) {
      *failure=@"native typed constructor nil/presence differs";return NO;
    }
    if(!identifier)continue;
    id previous=objects[@(identifier)];
    if(previous && previous!=result) {
      *failure=@"native typed object generation conflict";return NO;
    }
    objects[@(identifier)]=result;
    if([out[@"type"] hasSuffix:@"CommandEncoder"])
      [active_encoders addObject:@(identifier)];
    /* Table holds an independent reference. Owned constructor results are
     * released after insertion; borrowed NSError results are not released. */
    if(ordinal!=129 && ![out[@"type"] isEqual:@"NSError"]) [result release];
  }
  return YES;
}
#endif
