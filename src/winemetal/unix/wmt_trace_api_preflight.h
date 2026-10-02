#ifndef WMT_TRACE_API_PREFLIGHT_H
#define WMT_TRACE_API_PREFLIGHT_H
#include "wmt_trace_api_codec.h"
static BOOL wmt_trace_api_effect_references(id value,NSDictionary *known,NSString **failure) {
  if([value isKindOfClass:[NSArray class]]) {
    for(id child in value)if(!wmt_trace_api_effect_references(child,known,failure))return NO;
  } else if([value isKindOfClass:[NSDictionary class]]) {
    NSSet *keys=[NSSet setWithArray:@[@"queue",@"commandBuffer",@"texture",@"resolveTexture",
        @"buffer",@"library",@"vertex",@"fragment",@"function",@"object",@"sampler",@"fence"]];
    for(NSString *key in value) {
      id child=value[key];
      if([keys containsObject:key] && [child isKindOfClass:[NSNumber class]]) {
        uint64_t identifier=[child unsignedLongLongValue];
        if(identifier&&!known[child]) {
          *failure=[NSString stringWithFormat:@"effect missing %@ generation %llu",key,identifier];return NO;
        }
      } else if(!wmt_trace_api_effect_references(child,known,failure))return NO;
    }
  }
  return YES;
}
/* Metadata has a checked CPU interpreter too. Logical pool scopes have their
 * own high-bit namespace; GPU ownership deltas never guess absolute refcounts. */
static BOOL wmt_trace_api_preflight_metadata(NSDictionary *record,
    NSMutableDictionary *known,NSMutableDictionary *state,NSString **failure) {
  NSString *event=record[@"event"];NSDictionary *fields=record[@"fields"];
  if([event isEqual:@"api-policy"]) {
    NSArray *calls=fields[@"calls"];
    if(state[@"policy"] || ![calls isKindOfClass:[NSArray class]] ||
        calls.count!=sizeof(wmt_trace_api_descriptors)/sizeof(*wmt_trace_api_descriptors)) {
      *failure=@"API policy missing/duplicate/size mismatch";return NO;
    }
    NSMutableSet *seen=[NSMutableSet set];
    for(NSDictionary *call in calls) {
      unsigned ordinal=[call[@"ordinal"] unsignedIntValue];
      const struct wmt_trace_api_desc *desc=wmt_trace_api_desc(ordinal);
      if(!desc || [seen containsObject:@(ordinal)] ||
          ![call[@"name"] isEqual:[NSString stringWithUTF8String:desc->name]] ||
          ![call[@"record"] isEqual:[NSString stringWithUTF8String:desc->record]] ||
          ![call[@"replay"] isEqual:[NSString stringWithUTF8String:desc->replay]]) {
        *failure=@"API policy ABI/implementation drift";return NO;
      }
      [seen addObject:@(ordinal)];
    }
    state[@"policy"]=@YES;return YES;
  }
  if([event isEqual:@"api-external"]) { *failure=@"external import/producer dependency unsupported";return NO; }
  if([event isEqual:@"api-census"]) {
    NSArray *counts=fields[@"counts"],*previous=state[@"counts"];
    if(!state[@"policy"] || ![counts isKindOfClass:[NSArray class]] || counts.count!=135 ||
        [counts[83] unsignedLongLongValue]) { *failure=@"API census slots/policy/reserved mismatch";return NO; }
    for(unsigned i=0;i<135;i++) {
      if(![counts[i] isKindOfClass:[NSNumber class]] ||
          [counts[i] unsignedLongLongValue]>1000000000ull ||
          (previous && [counts[i] unsignedLongLongValue]<[previous[i] unsignedLongLongValue])) {
        *failure=@"API census nonnumeric/cap/nonmonotonic";return NO;
      }
    }
    state[@"counts"]=counts;return YES;
  }
  if([event isEqual:@"api-lifetime"]) {
    NSArray *deltas=fields[@"deltas"];
    if(!state[@"policy"] || ![fields[@"status"] isEqual:@"PRESENT"] ||
        ![deltas isKindOfClass:[NSArray class]] || deltas.count>100000) {
      *failure=@"lifetime metadata policy/shape/cap";return NO;
    }
    if(!state[@"pools"])state[@"pools"]=[NSMutableDictionary dictionary];
    NSMutableDictionary *pools=state[@"pools"];NSMutableSet *seen=[NSMutableSet set];
    for(NSDictionary *delta in deltas) {
      NSNumber *identifier=delta[@"object"];
      uint64_t number=[identifier unsignedLongLongValue],retains=[delta[@"retain"] unsignedLongLongValue],
          releases=[delta[@"release"] unsignedLongLongValue];
      BOOL pool=[delta[@"pool"] boolValue];
      if(![identifier isKindOfClass:[NSNumber class]] || !number ||
          [seen containsObject:identifier] || pool!=(number>>63) ||
          retains>1000000000ull || releases>1000000000ull ||
          ![delta[@"absolute-refcount"] isEqual:@"UNKNOWN"] || (!pool&&!known[identifier])) {
        *failure=@"lifetime generation/consumer/delta validation";return NO;
      }
      [seen addObject:identifier];
      if(pool) {
        uint64_t balance=[pools[identifier] unsignedLongLongValue];
        if(!wmt_trace_api_add(balance,retains,&balance) || releases>balance) {
          *failure=@"logical pool release without observed creation";return NO;
        }
        balance-=releases;
        if(balance)pools[identifier]=@(balance);else[pools removeObjectForKey:identifier];
      }
    }
    return YES;
  }
  return YES;
}
#endif
