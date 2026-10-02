#ifndef WMT_TRACE_COMMANDS_H
#define WMT_TRACE_COMMANDS_H
#import <Foundation/Foundation.h>
#include <string.h>
#include "../winemetal.h"
#include "wmt_trace_command_schema.h"

typedef uint64_t (*wmt_trace_encode_handle)(obj_handle_t, void *);
typedef obj_handle_t (*wmt_trace_decode_handle)(uint64_t, void *);

static uint64_t wmt_trace_command_payload_length(const void *body,
    const struct wmt_trace_command_schema *schema, uint64_t limit, int *valid) {
  uint64_t count = 0;
  *valid = 1;
  if (!schema->element_size) return 0;
  memcpy(&count, (const char *)body + schema->count_offset, schema->count_size);
  if (count > limit / schema->element_size) { *valid = 0; return 0; }
  return count * schema->element_size;
}

/* Copy before unixcall returns: PE command lists and SetBytes are ephemeral.
 * All pointer slots are zero on disk; only explicitly typed object IDs survive.
 * Caps bound traversal, payload and cycles (a cycle exhausts the command cap). */
static NSArray *wmt_trace_pack_commands(unsigned encoder,
    const struct wmtcmd_base *head, uint64_t byte_limit, unsigned command_limit,
    wmt_trace_encode_handle encode, void *context, NSString **failure) {
  NSMutableArray *result = [NSMutableArray array];
  uint64_t total = 0;
  NSString *error = nil;
  while (head) {
    struct wmt_trace_command_schema schema;
    if (!command_limit-- || !encode ||
        !wmt_trace_command_schema(encoder, head->type, &schema)) {
      error = @"unknown command or command cap"; break;
    }
    if (total > byte_limit || schema.size > byte_limit - total) {
      error = @"command byte cap"; break;
    }
    total += schema.size;
    int valid;
    uint64_t length = wmt_trace_command_payload_length(head, &schema,
        byte_limit - total, &valid);
    if (!valid) { error = @"payload byte cap"; break; }
    total += length;
    NSMutableData *body = [NSMutableData dataWithBytes:head length:schema.size];
    memset((char *)[body mutableBytes] + offsetof(struct wmtcmd_base, next),
           0, sizeof(head->next));
    NSMutableArray *objects = [NSMutableArray array];
    for (unsigned i = 0; i < schema.handle_count; ++i) {
      obj_handle_t handle;
      memcpy(&handle, (const char *)head + schema.handles[i], sizeof(handle));
      uint64_t identifier = handle ? encode(handle, context) : 0;
      if (handle && !identifier) { error = @"unresolved command object"; break; }
      [objects addObject:@(identifier)];
      memset((char *)[body mutableBytes] + schema.handles[i], 0, sizeof(handle));
    }
    if (error) break;
    NSData *payload = [NSData data];
    if (schema.element_size) {
      struct WMTMemoryPointer pointer;
      memcpy(&pointer, (const char *)head + schema.pointer_offset, sizeof(pointer));
      if (length && !pointer.ptr) { error = @"NULL command payload"; break; }
      if (length) payload = [NSData dataWithBytes:pointer.ptr length:(NSUInteger)length];
      memset((char *)[body mutableBytes] + schema.pointer_offset, 0, sizeof(pointer));
    }
    [result addObject:@{@"body": body, @"objects": objects, @"payload": payload}];
    head = head->next.ptr;
  }
  if (failure) *failure = error;
  return error ? nil : [result copy];
}

/* Returned buffers/payloads are retained by arena; caller keeps it alive until
 * native encoding returns. Object lifetime belongs to the replay object table. */
static struct wmtcmd_base *wmt_trace_unpack_commands(unsigned encoder,
    NSArray *records, uint64_t byte_limit, unsigned command_limit,
    wmt_trace_decode_handle decode, void *context, NSMutableArray *arena,
    NSString **failure) {
  NSString *error = nil;
  struct wmtcmd_base *head = NULL, *previous = NULL;
  uint64_t total = 0;
  if (![records isKindOfClass:[NSArray class]] || [records count] > command_limit || !decode || !arena)
    error = @"invalid command stream";
  for (id record in error ? @[] : records) {
    if (![record isKindOfClass:[NSDictionary class]]) { error = @"invalid command record"; break; }
    NSData *body = record[@"body"], *payload = record[@"payload"];
    NSArray *objects = record[@"objects"];
    if (![body isKindOfClass:[NSData class]] || [body length] < sizeof(struct wmtcmd_base) ||
        ![payload isKindOfClass:[NSData class]] || ![objects isKindOfClass:[NSArray class]]) {
      error = @"invalid command fields"; break;
    }
    uint16_t type;
    memcpy(&type, [body bytes], sizeof(type));
    struct wmt_trace_command_schema schema;
    if (!wmt_trace_command_schema(encoder, type, &schema) || [body length] != schema.size ||
        [objects count] != schema.handle_count || total > byte_limit || schema.size > byte_limit - total) {
      error = @"command schema mismatch or byte cap"; break;
    }
    total += schema.size;
    int valid;
    uint64_t length = wmt_trace_command_payload_length([body bytes], &schema, byte_limit - total, &valid);
    if (!valid || length != [payload length]) { error = @"payload size mismatch"; break; }
    total += length;
    NSMutableData *copy = [NSMutableData dataWithData:body];
    struct wmtcmd_base *node = [copy mutableBytes];
    if (node->next.ptr) { error = @"serialized next pointer"; break; }
    for (unsigned i = 0; i < schema.handle_count; ++i) {
      uint64_t serialized;
      memcpy(&serialized, (char *)node + schema.handles[i], 8);
      if (serialized || ![objects[i] isKindOfClass:[NSNumber class]]) {
        error = @"invalid serialized object"; break;
      }
      uint64_t identifier = [objects[i] unsignedLongLongValue];
      obj_handle_t handle = identifier ? decode(identifier, context) : 0;
      if (identifier && !handle) { error = @"missing replay object"; break; }
      memcpy((char *)node + schema.handles[i], &handle, 8);
    }
    if (error) break;
    if (schema.element_size) {
      struct WMTMemoryPointer pointer;
      memcpy(&pointer, (char *)node + schema.pointer_offset, sizeof(pointer));
      if (pointer.ptr) { error = @"serialized payload pointer"; break; }
      NSData *payload_copy = [NSData dataWithData:payload];
      [arena addObject:payload_copy];
      pointer.ptr = length ? (void *)[payload_copy bytes] : NULL;
      memcpy((char *)node + schema.pointer_offset, &pointer, sizeof(pointer));
    }
    [arena addObject:copy];
    if (previous) previous->next.ptr = node; else head = node;
    previous = node;
  }
  if (failure) *failure = error;
  return error ? NULL : head;
}
#endif
