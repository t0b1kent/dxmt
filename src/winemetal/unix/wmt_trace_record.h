#ifndef WMT_TRACE_RECORD_H
#define WMT_TRACE_RECORD_H
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "wmt_trace_commands.h"
#include "wmt_trace_registry.h"
#include "wmt_trace_cb.h"
#include "wmt_trace_file.h"

static uint64_t wmt_trace_record_sequence;
static BOOL wmt_trace_record_failed;
static BOOL wmt_trace_record_initialized;

/* Diagnostic-only provenance: never serialize CPU pointers or buffer contents. */
static NSDictionary *wmt_audit_owner_fields(id buffer, uint64_t offset,
    uint64_t length, unsigned action, unsigned ordinal, BOOL valid) {
  NSMutableDictionary *result = [NSMutableDictionary dictionaryWithDictionary:
      @{@"offset": @(offset), @"length": @(length), @"action": @(action),
        @"ordinal": @(ordinal), @"status": valid ? @"PRESENT" : @"FAILED"}];
  if (!valid) {
    NSDictionary *ledger = buffer ? wmt_trace_owner_copy(buffer) : nil;
    result[@"reason"] = @"ownership transition rejected";
    result[@"call"] = ordinal == 107 ? @"_MTLBuffer_updateContents" : @"_MTLBuffer_traceOwnership";
    result[@"capacity"] = buffer ? @([(id<MTLBuffer>)buffer length]) : @0;
    result[@"buffer-nil"] = @(!buffer);
    result[@"ledger-present"] = @(ledger != nil);
    result[@"ledger-version"] = ledger[@"version"] ?: @0;
    for (NSString *key in @[@"pending", @"owned", @"gpu"]) {
      NSMutableArray *ranges = [NSMutableArray array];
      for (NSDictionary *range in ledger[key]) {
        NSMutableDictionary *item = [NSMutableDictionary dictionary];
        for (NSString *field in @[@"begin", @"end", @"version", @"ticket", @"promise", @"writer-thread"])
          if (range[field]) item[field] = range[field];
        if (range[@"before-image"]) item[@"before-image-bytes"] = @([range[@"before-image"] length]);
        [ranges addObject:item];
        if (ranges.count >= 256) break;
      }
      result[key] = ranges;
      result[[key stringByAppendingString:@"-count"]] = @([ledger[key] count]);
    }
    [ledger release];
  }
  return result;
}

/* Shared sequence with commands. Caller passes plist-safe typed fields only.
 * A constructor stream is still incomplete until ownership/sync/present exist. */
static void wmt_trace_record_event(NSString *type, id object, NSDictionary *fields) {
  const char *gate = getenv("MACRUNNER_WMT_RECORD");
  if (!gate || !*gate) return;
  @synchronized([NSPropertyListSerialization class]) {
    if (wmt_trace_record_failed) return;
    @autoreleasepool {
      NSError *error = nil;
      NSString *directory = [NSString stringWithUTF8String:gate];
      if (!wmt_trace_record_sequence)
        fprintf(stderr, "MACRUNNER_WMT_AUDIT_STAGE7_FIX2_20261001 loaded pid=%d\n", getpid());
      BOOL valid = directory && object && type && fields &&
          [[NSFileManager defaultManager] createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:&error];
      uint64_t identifier = valid ? wmt_trace_object_id(object) : 0;
      uint64_t thread = 0; struct timespec utc = {0}, monotonic = {0};
      pthread_threadid_np(NULL, &thread);
      clock_gettime(CLOCK_REALTIME, &utc); clock_gettime(CLOCK_MONOTONIC_RAW, &monotonic);
      NSDictionary *event = identifier ? @{@"schema": @1, @"event": type,
          @"object": @(identifier), @"fields": fields,
          @"sequence": @(wmt_trace_record_sequence), @"thread": @(thread),
          @"utc-ns": @((uint64_t)utc.tv_sec * 1000000000 + utc.tv_nsec),
          @"monotonic-ns": @((uint64_t)monotonic.tv_sec * 1000000000 + monotonic.tv_nsec)} : nil;
      NSData *bytes = event ? [NSPropertyListSerialization dataWithPropertyList:event
          format:NSPropertyListBinaryFormat_v1_0 options:0 error:&error] : nil;
      NSString *name = [NSString stringWithFormat:@"event-%d-%012llu.plist", getpid(),
          (unsigned long long)wmt_trace_record_sequence++];
      if (!bytes || ![bytes writeToFile:[directory stringByAppendingPathComponent:name]
          options:NSDataWritingWithoutOverwriting error:&error]) {
        wmt_trace_record_failed = YES;
        fprintf(stderr, "WMT_TRACE FAILED event: %s\n", [type UTF8String]);
        NSDictionary *detail = @{@"sequence": @(wmt_trace_record_sequence - 1),
            @"event": type ?: @"NIL", @"object": @(identifier),
            @"fields-nil": @(!fields), @"object-nil": @(!object),
            @"bytes-nil": @(!bytes), @"fields": fields ?: @{},
            @"error-nil": @(!error), @"error-domain": error.domain ?: @"NIL",
            @"error-code": error ? @(error.code) : @0,
            @"error-description": error.localizedDescription ?: @"NIL"};
        NSError *diagnostic_error = nil;
        NSData *diagnostic = [NSPropertyListSerialization dataWithPropertyList:detail
            format:NSPropertyListBinaryFormat_v1_0 options:0 error:&diagnostic_error];
        if (!diagnostic || ![diagnostic writeToFile:[directory stringByAppendingPathComponent:@"FAILURE-DETAIL.plist"]
            options:NSDataWritingWithoutOverwriting error:&diagnostic_error])
          fprintf(stderr, "WMT_AUDIT failure-detail write failed: %s\n", diagnostic_error.localizedDescription.UTF8String);
        wmt_trace_write_text(@"FAILED event serialization/write; trace is not replayable\n",
            [directory stringByAppendingPathComponent:@"FAILED.txt"], NULL);
      } else if ([fields[@"status"] isEqual:@"FAILED"]) {
        wmt_trace_record_failed = YES;
        fprintf(stderr, "WMT_TRACE FAILED %s: %s\n", type.UTF8String,
            [(fields[@"reason"] ?: @"explicit recorded failure") UTF8String]);
        wmt_trace_write_text([NSString stringWithFormat:@"FAILED %@ sequence=%llu reason=%@\n",
            type, (unsigned long long)(wmt_trace_record_sequence-1), fields[@"reason"] ?: @"explicit failure"],
            [directory stringByAppendingPathComponent:@"FAILED.txt"], NULL);
        NSDictionary *detail = @{@"sequence": @(wmt_trace_record_sequence-1),
            @"event": type, @"object": @(identifier), @"fields": fields,
            @"fields-nil": @NO, @"error-nil": @(!error),
            @"error-domain": error.domain ?: @"NIL", @"error-code": error ? @(error.code) : @0};
        NSData *diagnostic = [NSPropertyListSerialization dataWithPropertyList:detail
            format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
        [diagnostic writeToFile:[directory stringByAppendingPathComponent:@"FAILURE-DETAIL.plist"]
            options:NSDataWritingWithoutOverwriting error:NULL];
      }
    }
  }
}

struct wmt_trace_record_context { unsigned kind; id encoder; };
static uint64_t wmt_trace_record_handle(obj_handle_t handle, void *context) {
  struct wmt_trace_record_context *record = context;
  uint64_t identifier = wmt_trace_object_id((id)handle);
  wmt_trace_cb_use(record->encoder, (id)handle, NO);
  if (record->kind != 1) wmt_trace_cb_read(record->encoder, (id)handle, 0, 0, YES);
  return identifier;
}

/* This stream is explicitly incomplete until constructors, commit snapshots,
 * presentation/readback and synchronization are added. Never admit it as a
 * replay trace. File writes occur only behind the explicit recording gate. */
static void wmt_trace_record_commands(unsigned kind, id encoder,
    const struct wmtcmd_base *head) {
  const char *gate = getenv("MACRUNNER_WMT_RECORD");
  if (!gate || !*gate) return;
  @synchronized([NSPropertyListSerialization class]) {
    if (wmt_trace_record_failed) return;
    @autoreleasepool {
      NSString *directory = [NSString stringWithUTF8String:gate];
      NSString *failure = nil;
      NSError *error = nil;
      if (!directory || ![[NSFileManager defaultManager] createDirectoryAtPath:directory
          withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:&error]) {
        failure = @"record directory creation failed";
      }
      if (!failure && !wmt_trace_record_initialized) {
        NSString *status = [directory stringByAppendingPathComponent:@"COMMANDS_ONLY_NOT_REPLAYABLE.txt"];
        if (!wmt_trace_write_text(
            @"INCOMPLETE: constructors, snapshots, sync, present, PNG and GPU replay are NOT_ENABLED\n",
            status, &error))
          failure = @"record status write failed";
        else wmt_trace_record_initialized = YES;
      }
      struct wmt_trace_record_context context = {kind, encoder};
      NSArray *commands = failure ? nil : wmt_trace_pack_commands(kind, head,
          UINT64_C(64) * 1024 * 1024, 65536, wmt_trace_record_handle, &context, &failure);
      if (commands) wmt_trace_cb_command_writes(kind, encoder, head);
      uint64_t encoder_id = failure ? 0 : wmt_trace_object_id(encoder);
      if (!failure && !encoder_id) failure = @"record encoder identity failed";
      if (!failure) {
        uint64_t thread_id = 0;
        struct timespec monotonic = {0}, utc = {0};
        pthread_threadid_np(NULL, &thread_id);
        clock_gettime(CLOCK_MONOTONIC_RAW, &monotonic);
        clock_gettime(CLOCK_REALTIME, &utc);
        NSDictionary *record = @{@"schema": @1, @"encoder-kind": @(kind),
            @"encoder": @(encoder_id), @"sequence": @(wmt_trace_record_sequence),
            @"thread": @(thread_id),
            @"monotonic-ns": @((uint64_t)monotonic.tv_sec * 1000000000 + monotonic.tv_nsec),
            @"utc-ns": @((uint64_t)utc.tv_sec * 1000000000 + utc.tv_nsec),
            @"commands": commands};
        NSData *data = [NSPropertyListSerialization dataWithPropertyList:record
            format:NSPropertyListBinaryFormat_v1_0 options:0 error:&error];
        NSString *name = [NSString stringWithFormat:@"commands-%d-%012llu.plist", getpid(),
            (unsigned long long)wmt_trace_record_sequence++];
        NSString *path = [directory stringByAppendingPathComponent:name];
        /* Do not overwrite evidence from an earlier process/run. */
        if (!data || ![data writeToFile:path options:NSDataWritingWithoutOverwriting error:&error])
          failure = @"record command write failed";
      }
      [commands release];
      if (failure) {
        wmt_trace_record_failed = YES;
        fprintf(stderr, "WMT_TRACE FAILED commands: %s\n", [failure UTF8String]);
        if (directory) {
          NSString *path = [directory stringByAppendingPathComponent:@"FAILED.txt"];
          wmt_trace_write_text(failure, path, NULL);
        }
      }
    }
  }
}
#endif
