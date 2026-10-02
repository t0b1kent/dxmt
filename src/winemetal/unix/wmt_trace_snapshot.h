#ifndef WMT_TRACE_SNAPSHOT_H
#define WMT_TRACE_SNAPSHOT_H
#import <Metal/Metal.h>
#include "wmt_trace_fields.h"
#include "wmt_trace_registry.h"
#include "wmt_trace_ownership.h"

/* Diagnostic commit inputs. Only producer-declared ranges are copied; unknown
 * buffers are listed, never read or silently treated as unchanged. The caller
 * still must associate used ranges with the CB and invalidate queued GPU writes
 * before this can be admitted as a replay-complete commit. */
static NSDictionary *wmt_trace_copy_ranged_owned_inputs(uint64_t byte_limit,
    NSSet *selected, NSDictionary *read_ranges, NSString **failure) {
  NSArray *owners = nil;
  NSData *map = wmt_trace_copy_resources(&owners);
  const struct wmt_trace_resource *resources = [map bytes];
  NSUInteger count = [map length] / sizeof(*resources);
  NSMutableArray *snapshots = [NSMutableArray array], *unknown = [NSMutableArray array];
  uint64_t total = 0;
  NSString *error = nil;
  if ([owners count] != count) error = @"registry owner/descriptor mismatch";
  for (NSUInteger i = 0; !error && i < count; ++i) {
    if (resources[i].kind != 1) continue;
    if (selected && ![selected containsObject:@(resources[i].object_id)]) continue;
    id buffer = owners[i];
    if ([buffer storageMode] >= 2) continue;
    @synchronized(buffer) {
      NSDictionary *ledger = wmt_trace_owner_copy(buffer);
      if (!ledger) {
        [unknown addObject:@(resources[i].object_id)];
        continue;
      }
      NSMutableArray *authoritative = [NSMutableArray arrayWithArray:ledger[@"owned"]];
      id requested = read_ranges[@(resources[i].object_id)];
      for (NSDictionary *range in ledger[@"pending"]) {
        NSData *image = range[@"before-image"];
        if (![range[@"promise"] isEqual:@"D3D11-NO_OVERWRITE"] ||
            ![image isKindOfClass:[NSData class]] || image.length !=
            [range[@"end"] unsignedLongLongValue]-[range[@"begin"] unsignedLongLongValue]) {
          BOOL overlaps_read = !requested || requested == [NSNull null];
          if (!overlaps_read)
            for (NSDictionary *span in requested)
              if (wmt_trace_owner_overlap(range,[span[@"begin"] unsignedLongLongValue],
                  [span[@"end"] unsignedLongLongValue])) overlaps_read = YES;
          if (!overlaps_read) continue;
          error = @"pending CPU writer at commit"; break;
        }
        NSMutableDictionary *prior = [NSMutableDictionary dictionaryWithDictionary:range];
        prior[@"before-image-origin"] = range[@"begin"];
        [authoritative addObject:prior];
      }
      NSMutableArray *ranges = [NSMutableArray array];
      for (NSDictionary *range in authoritative) {
        if (!requested || requested == [NSNull null]) { [ranges addObject:range]; continue; }
        for (NSDictionary *span in requested) {
          uint64_t a = MAX([range[@"begin"] unsignedLongLongValue],[span[@"begin"] unsignedLongLongValue]);
          uint64_t b = MIN([range[@"end"] unsignedLongLongValue],[span[@"end"] unsignedLongLongValue]);
          if (a >= b) continue;
          NSMutableDictionary *clipped = [NSMutableDictionary dictionaryWithDictionary:range];
          clipped[@"begin"] = @(a); clipped[@"end"] = @(b); [ranges addObject:clipped];
        }
      }
      uint64_t capacity = [buffer length];
      const void *source = [buffer contents];
      if (!error && (capacity != resources[i].length || !source))
        error = @"invalid readable buffer at commit";
      NSDictionary *fields = wmt_trace_copy_fields(buffer);
      NSArray *offsets = [[fields allKeys] sortedArrayUsingSelector:@selector(compare:)];
      for (NSDictionary *range in ranges) {
        if (error) break;
        uint64_t begin = [range[@"begin"] unsignedLongLongValue];
        uint64_t end = [range[@"end"] unsignedLongLongValue];
        if (begin > end || end > capacity || end - begin > NSUIntegerMax ||
            total > byte_limit || end - begin > byte_limit - total) {
          error = [NSString stringWithFormat:@"invalid or over-budget owned range object=%llu begin=%llu end=%llu capacity=%llu total=%llu limit=%llu",
              (unsigned long long)resources[i].object_id, (unsigned long long)begin,
              (unsigned long long)end, (unsigned long long)capacity,
              (unsigned long long)total, (unsigned long long)byte_limit];
          break;
        }
        NSData *prior = range[@"before-image"];
        const char *input = prior ? (const char *)prior.bytes + begin -
            [range[@"before-image-origin"] unsignedLongLongValue] : (const char *)source + begin;
        NSMutableData *payload = [NSMutableData dataWithBytes:input
            length:(NSUInteger)(end - begin)];
        NSMutableArray *relocations = [NSMutableArray array];
        for (NSNumber *key in offsets) {
          uint64_t offset = [key unsignedLongLongValue];
          if (offset > UINT64_MAX - 8) { error = @"field offset overflow"; break; }
          if (offset >= end || offset + 8 <= begin) continue;
          if (offset < begin || end - offset < 8) {
            error = @"declared field crosses CPU ownership boundary"; break;
          }
          uint64_t value;
          memcpy(&value, (const char *)[payload bytes] + offset - begin, 8);
          uint32_t kind = [fields[key] unsignedIntValue];
          struct wmt_trace_relocation relocation;
          if (!wmt_trace_resolve(offset - begin, kind, value, resources, count, &relocation)) {
            error = @"unresolved declared resource in owned range"; break;
          }
          [relocations addObject:@{@"offset": @(offset - begin), @"kind": @(kind),
              @"object": @(relocation.object_id), @"addend": @(relocation.addend)}];
        }
        if (error) break;
        // Detect some unexpected writes; this check is NOT ownership evidence.
        if (memcmp(input, [payload bytes], (size_t)(end - begin))) {
          error = @"owned bytes changed during commit copy"; break;
        }
        for (NSDictionary *relocation in relocations) {
          uint64_t zero = 0;
          memcpy((char *)[payload mutableBytes] + [relocation[@"offset"] unsignedLongLongValue],
              &zero, 8);
        }
        total += end - begin;
        [snapshots addObject:@{@"object": @(resources[i].object_id), @"offset": @(begin),
            @"length": @(end - begin), @"version": range[@"version"],
            @"origin": prior ? @"immutable-pre-NO_OVERWRITE-Map" : @"CPU-owned-live-range",
            @"bytes": payload, @"relocations": relocations}];
      }
      [fields release];
      [ledger release];
    }
  }
  [map release]; [owners release];
  if (failure) *failure = error;
  return error ? nil : [@{@"status": @"INCOMPLETE", @"scope": selected ?
      (read_ranges ? @"explicit-CB-blit-spans/shader-whole-allocation" :
          @"explicit-CB-buffer-whole-allocation") : @"all-declared-CPU-ranges",
      @"used-range-coverage": selected ? @"PARTIAL" : @"NOT_ENABLED",
      @"gpu-write-invalidation": @"NOT_ENABLED",
      @"raw-bytes": @(total), @"unknown-readable-buffers": unknown,
      @"snapshots": snapshots} retain];
}

static NSDictionary *wmt_trace_copy_selected_owned_inputs(uint64_t byte_limit,
    NSSet *selected, NSString **failure) {
  return wmt_trace_copy_ranged_owned_inputs(byte_limit,selected,nil,failure);
}

static NSDictionary *wmt_trace_copy_owned_inputs(uint64_t byte_limit, NSString **failure) {
  return wmt_trace_copy_selected_owned_inputs(byte_limit, nil, failure);
}

/* CPU-readable state only. The caller must establish CPU/GPU ownership before
 * using this as replay input. Two matching reads detect some concurrent writes,
 * but do not establish ownership. No runtime addresses are serialized. */
static NSArray *wmt_trace_copy_snapshots(uint64_t byte_limit, NSString **failure) {
  NSArray *owners = nil;
  NSData *map = wmt_trace_copy_resources(&owners);
  const struct wmt_trace_resource *resources = [map bytes];
  NSUInteger count = [map length] / sizeof(*resources);
  NSMutableArray *result = [NSMutableArray array];
  uint64_t total = 0;
  NSString *error = nil;
  if ([owners count] != count) {
    error = @"registry owner/descriptor mismatch";
    goto done;
  }
  for (NSUInteger i = 0; i < count; ++i) {
    if (resources[i].kind != 1)
      continue;
    id buffer = owners[i];
    uint64_t length = [buffer length];
    /* MTLStorageModePrivate = 2; memoryless has no CPU buffer contents either. */
    if ([buffer storageMode] >= 2)
      continue;
    const void *source = [buffer contents];
    if (length != resources[i].length || !source || length > NSUIntegerMax ||
        total > byte_limit || length > byte_limit - total) {
      error = @"invalid or over-budget CPU snapshot";
      goto done;
    }
    total += length;
    NSMutableData *payload = [NSMutableData dataWithBytes:source length:(NSUInteger)length];
    NSDictionary *fields = wmt_trace_copy_fields(buffer);
    NSArray *offsets = [[fields allKeys] sortedArrayUsingSelector:@selector(compare:)];
    NSMutableArray *relocations = [NSMutableArray array];
    for (NSNumber *key in offsets) {
      uint64_t offset = [key unsignedLongLongValue], value;
      uint32_t kind = [fields[key] unsignedIntValue];
      struct wmt_trace_relocation relocation;
      if (offset > length || length - offset < 8) {
        error = @"field outside CPU snapshot";
        break;
      }
      memcpy(&value, (const char *)[payload bytes] + offset, 8);
      if (!wmt_trace_resolve(offset, kind, value, resources, count, &relocation)) {
        error = @"unresolved or ambiguous declared resource";
        break;
      }
      [relocations addObject:@{@"offset": @(offset), @"kind": @(kind),
          @"object": @(relocation.object_id), @"addend": @(relocation.addend)}];
    }
    [fields release];
    if (error)
      goto done;
    if (memcmp(source, [payload bytes], (size_t)length)) {
      error = @"CPU snapshot changed during copy";
      goto done;
    }
    for (NSDictionary *relocation in relocations) {
      uint64_t zero = 0;
      memcpy((char *)[payload mutableBytes] + [relocation[@"offset"] unsignedLongLongValue],
             &zero, sizeof(zero));
    }
    [result addObject:@{@"object": @(resources[i].object_id), @"length": @(length),
        @"bytes": payload, @"relocations": relocations}];
  }
done:
  [map release];
  [owners release];
  if (failure)
    *failure = error;
  if (error)
    return nil;
  /* Canonical ordering also makes native contract tests reproducible. */
  NSSortDescriptor *order = [NSSortDescriptor sortDescriptorWithKey:@"object" ascending:YES];
  return [[result sortedArrayUsingDescriptors:@[order]] copy];
}
#endif
