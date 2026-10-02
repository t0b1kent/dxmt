/* Native arm64 Metal stream consumer. --inspect performs no GPU work.
 * --diagnostic permits explicitly INCOMPLETE producer streams, never claims
 * game replay completeness. Unknown operations and recorded FAILED stop replay. */
#include "../src/winemetal/unix/wmt_trace_native_objects.h"
#include "../src/winemetal/unix/wmt_trace_commands.h"
#include "../src/winemetal/unix/wmt_trace_native_encoders.h"
#include "../src/winemetal/unix/wmt_trace_relocate.h"
#include "../src/winemetal/unix/wmt_trace_frame.h"
#include <stdio.h>
#include "../src/winemetal/unix/wmt_trace_api_native.h"
#include "../src/winemetal/unix/wmt_trace_api_preflight.h"
#include "../src/winemetal/unix/wmt_trace_input_evidence.h"

static obj_handle_t handle(uint64_t identifier, void *context) {
  return (obj_handle_t)((NSDictionary *)context)[@(identifier)];
}

static BOOL restore_inputs(NSDictionary *fields, NSDictionary *objects, NSString **error) {
  NSArray *snapshots = fields[@"snapshots"];
  if (![snapshots isKindOfClass:[NSArray class]] || [fields[@"unknown-readable-buffers"] count]) {
    *error = @"missing snapshots or UNKNOWN readable buffers"; return NO;
  }
  for (NSDictionary *snapshot in snapshots) {
    id<MTLBuffer> buffer = objects[snapshot[@"object"]];
    NSData *bytes = snapshot[@"bytes"];
    uint64_t offset = [snapshot[@"offset"] unsignedLongLongValue];
    if (!buffer || ![bytes isKindOfClass:[NSData class]] || !buffer.contents ||
        offset > buffer.length || bytes.length > buffer.length - offset ||
        bytes.length != [snapshot[@"length"] unsignedLongLongValue]) {
      *error = @"invalid snapshot allocation/range"; return NO;
    }
    NSMutableData *payload = [NSMutableData dataWithData:bytes];
    NSArray *relocations = snapshot[@"relocations"];
    if (![relocations isKindOfClass:[NSArray class]]) { *error = @"missing relocations"; return NO; }
    NSMutableData *map = [NSMutableData data], *fixups = [NSMutableData data];
    NSMutableSet *seen = [NSMutableSet set];
    for (NSDictionary *fixup in relocations) {
      uint64_t identifier = [fixup[@"object"] unsignedLongLongValue];
      uint32_t kind = [fixup[@"kind"] unsignedIntValue];
      struct wmt_trace_relocation field = {[fixup[@"offset"] unsignedLongLongValue],
          identifier, [fixup[@"addend"] unsignedLongLongValue], kind};
      [fixups appendBytes:&field length:sizeof(field)];
      if (!identifier || [seen containsObject:@(identifier)]) continue;
      [seen addObject:@(identifier)];
      id resource = objects[@(identifier)];
      struct wmt_trace_resource entry = {identifier, 0, 0, 0, kind};
      if (!resource) { *error = @"unresolved snapshot generation"; return NO; }
      if (kind == 1) { entry.address = [(id<MTLBuffer>)resource gpuAddress]; entry.length = [resource length]; }
      else if (kind == 2) entry.resource_id = [(id<MTLTexture>)resource gpuResourceID]._impl;
      else if (kind == 3) entry.resource_id = [(id<MTLSamplerState>)resource gpuResourceID]._impl;
      else { *error = @"unknown relocation kind"; return NO; }
      [map appendBytes:&entry length:sizeof(entry)];
    }
    if (!wmt_trace_relocate(payload.mutableBytes, payload.length, fixups.bytes,
        fixups.length / sizeof(struct wmt_trace_relocation), map.bytes,
        map.length / sizeof(struct wmt_trace_resource))) {
      *error = @"snapshot relocation rejected"; return NO;
    }
    memcpy((char *)buffer.contents + offset, payload.bytes, payload.length);
    if (buffer.storageMode == MTLStorageModeManaged)
      [buffer didModifyRange:NSMakeRange(offset, payload.length)];
  }
  return YES;
}

int main(int argc, char **argv) {
  @autoreleasepool {
    if ((argc != 3 && argc != 4) || (strcmp(argv[1], "--inspect") && strcmp(argv[1], "--diagnostic") && strcmp(argv[1],"--check"))) {
      fprintf(stderr, "usage: wmt_trace_native_player --inspect|--diagnostic TRACE_DIRECTORY [NEW_FRAME_DIRECTORY]\n"); return 2;
    }
    BOOL inspect = !strcmp(argv[1], "--inspect");
    BOOL timing=strcmp(argv[1],"--check")!=0;
    wmt_trace_frame_timings=timing;
    NSString *directory = [NSString stringWithUTF8String:argv[2]], *failure = nil;
    NSString *frame_directory = argc == 4 ? [NSString stringWithUTF8String:argv[3]] :
        [directory stringByAppendingPathComponent:@"native-frames"];
    NSString *manifest_path=[directory stringByAppendingPathComponent:@"PREFIX.json"];
    NSData *manifest_data=[NSData dataWithContentsOfFile:manifest_path];
    NSDictionary *manifest=manifest_data ? [NSJSONSerialization JSONObjectWithData:manifest_data options:0 error:NULL] : nil;
    NSArray *names=manifest ? [manifest[@"files"] valueForKey:@"name"] :
        [[NSFileManager defaultManager] contentsOfDirectoryAtPath:directory error:NULL];
    if(manifest && (![names isKindOfClass:[NSArray class]] || !names.count)) {
      fprintf(stderr,"FAILED invalid immutable prefix manifest\n");return 1;
    }
    fprintf(stderr,"PHASE LOAD_BEGIN directory_entries=%lu\n",(unsigned long)names.count);
    NSMutableArray *records = [NSMutableArray array];
    uint64_t disk_bytes = 0;
    for (NSString *name in names) {
      @autoreleasepool {
      if (![name hasSuffix:@".plist"] || (![name hasPrefix:@"event-"] && ![name hasPrefix:@"commands-"])) continue;
      NSString *path = [directory stringByAppendingPathComponent:name];
      NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:NULL];
      uint64_t size = [attrs[NSFileSize] unsignedLongLongValue];
      if (!size || size > 128ull * 1024 * 1024 || records.count >= 1000000) {
        failure = @"diagnostic input cap 128MiB/file, 1000000 records"; break;
      }
      disk_bytes += size;
      id record = [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:path]
          options:NSPropertyListImmutable format:NULL error:NULL];
      if (![record isKindOfClass:[NSDictionary class]] || [record[@"schema"] intValue] != 1 ||
          ![record[@"sequence"] isKindOfClass:[NSNumber class]]) { failure = @"invalid stream record"; break; }
      if ([record[@"fields"][@"status"] isEqual:@"FAILED"]) {
        failure = @"recorded FAILED operation"; break;
      }
      /* Retain paths only: payloads are read again one at a time during replay. */
      [records addObject:@{@"sequence": record[@"sequence"], @"path": path}];
      if(records.count%10000==0)fprintf(stderr,"PHASE LOAD records=%lu\n",(unsigned long)records.count);
      }
    }
    [records sortUsingComparator:^NSComparisonResult(id a, id b) { return [a[@"sequence"] compare:b[@"sequence"]]; }];
    if (!records.count) failure = @"EMPTY stream";
    for (NSUInteger i = 0; !failure && i < records.count; ++i) {
      NSDictionary *record = records[i];
      if ([record[@"sequence"] unsignedLongLongValue] != i) failure = @"missing/duplicate sequence or mixed processes";
    }
    /* Validate object dependencies and the same command decoder without Metal.
     * NSNumber stand-ins are opaque handles; no encoder is invoked here. */
    NSMutableDictionary *known = [NSMutableDictionary dictionary];
    NSMutableDictionary *api_state=[NSMutableDictionary dictionary];
    NSSet *constructors = [NSSet setWithArray:@[@"device", @"queue", @"command-buffer", @"render-encoder",
        @"blit-encoder", @"compute-encoder", @"buffer", @"texture", @"buffer-texture",
        @"texture-view", @"library", @"function", @"function-specialized", @"sampler",
        @"depth-stencil", @"render-pipeline", @"compute-pipeline", @"fence"]];
    for (NSDictionary *entry in records) {
      if (failure) break;
      @autoreleasepool {
        if([entry[@"sequence"] unsignedLongLongValue]%10000==0)
          fprintf(stderr,"PHASE PREFLIGHT sequence=%llu\n",[entry[@"sequence"] unsignedLongLongValue]);
        NSDictionary *record = [NSPropertyListSerialization propertyListWithData:
            [NSData dataWithContentsOfFile:entry[@"path"]] options:NSPropertyListImmutable format:NULL error:NULL];
        if ([constructors containsObject:record[@"event"]]) {
          if (![record[@"object"] isKindOfClass:[NSNumber class]] ||
              ![record[@"object"] unsignedLongLongValue] ||
              ![record[@"fields"] isKindOfClass:[NSDictionary class]]) {
            failure = @"CPU preflight invalid constructor";
            break;
          }
          known[record[@"object"]] = record[@"object"];
        }
        if (!wmt_trace_api_preflight_metadata(record,known,api_state,&failure)) {
          fprintf(stderr,"PREFLIGHT metadata sequence=%llu\n",
              [record[@"sequence"] unsignedLongLongValue]);break;
        }
        if (![record[@"event"] hasPrefix:@"api-"] && record[@"fields"] &&
            !wmt_trace_api_effect_references(record[@"fields"],known,&failure)) {
          fprintf(stderr,"PREFLIGHT effect sequence=%llu event=%s\n",
              [record[@"sequence"] unsignedLongLongValue],[record[@"event"] UTF8String]);break;
        }
        if ([record[@"event"] isEqual:@"api-call"]) {
          NSMutableArray *arena=[NSMutableArray array];
          if (!wmt_trace_api_decode(record[@"fields"],known,nil,YES,arena,&failure)) {
            fprintf(stderr,"PREFLIGHT sequence=%llu api=%u\n",
                [record[@"sequence"] unsignedLongLongValue],[record[@"fields"][@"ordinal"] unsignedIntValue]);
            break;
          }
        }
        if (record[@"commands"]) {
          unsigned kind = [record[@"encoder-kind"] unsignedIntValue];
          NSMutableArray *arena = [NSMutableArray array];
          if (!known[record[@"encoder"]] || kind < 1 || kind > 3 ||
              !wmt_trace_unpack_commands(kind, record[@"commands"], 64ull*1024*1024,
                  65536, handle, known, arena, &failure)) {
            failure = failure ?: @"CPU preflight unresolved encoder";
            fprintf(stderr, "PREFLIGHT sequence=%llu encoder=%llu\n",
                [record[@"sequence"] unsignedLongLongValue], [record[@"encoder"] unsignedLongLongValue]);
          }
        }
      }
    }
    if ([[NSFileManager defaultManager] fileExistsAtPath:[directory stringByAppendingPathComponent:@"FAILED.txt"]])
      failure = @"producer FAILED.txt present";
    if (failure) { fprintf(stderr, "FAILED %s\n", failure.UTF8String); return 1; }
    if (inspect) {
      printf("INSPECT records=%lu bytes=%llu GPU=NOT_ENABLED Wine=NOT_ENABLED completeness=UNVERIFIED\n",
          (unsigned long)records.count, (unsigned long long)disk_bytes); return 0;
    }
    fprintf(stderr,"PHASE CPU_PREFLIGHT_PASS records=%lu\n",(unsigned long)records.count);
    FILE *input_evidence=NULL;
    CC_SHA256_CTX input_chain;
    const char *input_path=getenv("MACRUNNER_WMT_INPUT_EVIDENCE");
    if(input_path && *input_path) {
      input_evidence=fopen(input_path,"wx");
      if(!input_evidence) { fprintf(stderr,"FAILED input evidence file open\n");return 1; }
      CC_SHA256_Init(&input_chain);
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    fprintf(stderr,"PHASE DEVICE_CREATED\n");
    NSMutableDictionary *objects = [NSMutableDictionary dictionary];
    NSMutableArray *submitted = [NSMutableArray array];
    NSMutableArray *submitted_ids = [NSMutableArray array];
    NSMutableSet *active_encoders = [NSMutableSet set];
    NSUInteger commits = 0;
    uint64_t current_sequence=0;
    for (NSDictionary *entry in records) {
      @autoreleasepool {
        if([entry[@"sequence"] unsignedLongLongValue]%5000==0)
          fprintf(stderr,"PHASE REPLAY sequence=%llu\n",[entry[@"sequence"] unsignedLongLongValue]);
        current_sequence=[entry[@"sequence"] unsignedLongLongValue];
        NSData *raw=[NSData dataWithContentsOfFile:entry[@"path"]];
        NSDictionary *record = [NSPropertyListSerialization propertyListWithData:
             raw options:NSPropertyListImmutable format:NULL error:NULL];
        if (![record isKindOfClass:[NSDictionary class]] ||
            ![record[@"sequence"] isEqual:entry[@"sequence"]]) {
          failure = @"record changed/missing during replay"; break;
        }
        if(!wmt_input_evidence(input_evidence,&input_chain,raw,record,entry[@"path"])) {
          failure=@"input evidence write failed";break;
        }
        NSString *event = record[@"event"];
        NSDictionary *fields = record[@"fields"];
        id object = objects[record[@"object"]];
        if (record[@"commands"]) {
          unsigned kind = [record[@"encoder-kind"] unsignedIntValue];
          id encoder = objects[record[@"encoder"]];
          NSMutableArray *arena = [NSMutableArray array];
          struct wmtcmd_base *head = wmt_trace_unpack_commands(kind, record[@"commands"],
              64ull * 1024 * 1024, 65536, handle, objects, arena, &failure);
          if (!encoder || !head || failure || kind < 1 || kind > 3) {
            failure = failure ?: @"invalid commands or unresolved encoder"; break;
          }
          if (!wmt_trace_api_native_commands(kind,encoder,head,&failure)) break;
        } else if ([event isEqual:@"api-call"]) {
          if (!wmt_trace_api_native(fields,device,objects,active_encoders,&failure)) {
            fprintf(stderr,"API sequence=%llu ordinal=%u\n",
                [record[@"sequence"] unsignedLongLongValue],[fields[@"ordinal"] unsignedIntValue]);
            break;
          }
        } else if ([event isEqual:@"api-lifetime"]) {
          /* Logical ownership deltas are evidence independent of the registry
           * retain. Registry entries survive until all submitted GPU work
           * completes. Absolute source retainCount is intentionally UNKNOWN. */
          if (![fields[@"status"] isEqual:@"PRESENT"] ||
              ![fields[@"deltas"] isKindOfClass:[NSArray class]]) {
            failure=@"invalid logical lifetime packet";break;
          }
        } else if ([event isEqual:@"api-policy"] || [event isEqual:@"api-census"]) {
          /* Fixed finite-call policy and cumulative observation, no GPU effect. */
        } else if ([event isEqual:@"api-external"]) {
          failure=@"external API has no portable producer/rights capture";break;
        } else if ([event isEqual:@"encoder-end"]) {
          if (!object) { failure = @"unresolved encoder-end"; break; }
          [object endEncoding];
          [active_encoders removeObject:record[@"object"]];
        } else if ([event isEqual:@"commit-inputs"]) {
          if (!restore_inputs(fields, objects, &failure)) break;
        } else if ([event isEqual:@"command-buffer-commit"]) {
          if (!object) { failure = @"unresolved commit"; break; }
          [object commit]; [submitted addObject:object]; [submitted_ids addObject:record[@"object"]]; ++commits;
        } else if ([event isEqual:@"command-buffer-wait"]) {
          if (!object) { failure = @"unresolved wait"; break; }
          [object waitUntilCompleted];
        } else if ([event isEqual:@"frame-readback"]) {
          id<MTLTexture> texture = objects[fields[@"texture"]];
          if (!object || !texture || !wmt_trace_frame_schedule(object, texture,
              frame_directory,
              [fields[@"frame"] unsignedLongLongValue])) {
            failure = @"frame readback rejected"; break;
          }
        } else if ([event isEqual:@"diagnostic-buffer-check"]) {
          NSData *expected = fields[@"bytes"];
          id<MTLBuffer> buffer = object;
          uint64_t offset = [fields[@"offset"] unsignedLongLongValue];
          for (id<MTLCommandBuffer> cb in submitted) [cb waitUntilCompleted];
          if (![expected isKindOfClass:[NSData class]] || !buffer || !buffer.contents ||
              offset > buffer.length || expected.length > buffer.length - offset ||
              memcmp((char *)buffer.contents + offset, expected.bytes, expected.length)) {
            failure = @"diagnostic buffer bytes mismatch"; break;
          }
          printf("CHECK buffer=%llu bytes=%lu exact=YES\n", [record[@"object"] unsignedLongLongValue],
              (unsigned long)expected.length);
        } else if ([event isEqual:@"buffer-ownership"] || [event isEqual:@"commit-resources"] ||
            [event isEqual:@"command-buffer-completed"]) {
          /* Evidence-only metadata; never an operation in the native process. */
        } else if (!wmt_trace_native_construct(record, device, objects, &failure)) {
          fprintf(stderr, "CONSTRUCTOR sequence=%llu event=%s object=%llu\n",
              [record[@"sequence"] unsignedLongLongValue], event.UTF8String,
              [record[@"object"] unsignedLongLongValue]);
          break;
        }
        if ([event hasSuffix:@"-encoder"] && objects[record[@"object"]])
          [active_encoders addObject:record[@"object"]];
      }
    }
    if (failure) {
      fprintf(stderr,"FIRST_FAILURE sequence=%llu reason=%s\n",(unsigned long long)current_sequence,failure.UTF8String);
      wmt_trace_native_close_encoders(objects, active_encoders);
    }
    for (NSUInteger index=0;index<submitted.count;++index) {
      id<MTLCommandBuffer> cb=submitted[index];
      [cb waitUntilCompleted];
      if (cb.status != MTLCommandBufferStatusCompleted || cb.error) {
        fprintf(stderr,"GPU_FAILURE id=%llu status=%lu error=%s\n",[submitted_ids[index] unsignedLongLongValue],
            (unsigned long)cb.status,cb.error.localizedDescription.UTF8String ?: "nil");
        failure=failure ?: @"native GPU command-buffer failure";
      }
      /* One stderr record: frame callbacks use the same FILE lock, so redirected
       * stdout/stderr buffers cannot split a GPU timing row. */
      if(timing) fprintf(stderr,"GPU command_buffer_ms=%.9f start_s=%.9f end_s=%.9f status=%lu id=%llu\n",
          (cb.GPUEndTime-cb.GPUStartTime)*1000,cb.GPUStartTime,cb.GPUEndTime,
          (unsigned long)cb.status,[submitted_ids[index] unsignedLongLongValue]);
      else fprintf(stderr,"GPU completion status=%lu id=%llu timing=NOT_ENABLED\n",
          (unsigned long)cb.status,[submitted_ids[index] unsignedLongLongValue]);
    }
    if (dispatch_group_wait(wmt_trace_frame_group(), dispatch_time(DISPATCH_TIME_NOW, 30ull * NSEC_PER_SEC))) {
      fprintf(stderr,"SECONDARY frame PNG completion timeout\n");
      failure=failure ?: @"frame PNG completion timeout";
    }
    if ([[NSFileManager defaultManager] fileExistsAtPath:[frame_directory stringByAppendingPathComponent:@"FAILED.txt"]])
      failure = failure ?: @"frame PNG completion failed";
    if(input_evidence && fclose(input_evidence)) failure=failure ?: @"input evidence close failed";
    [device release];
    uint64_t (*foreign_count)(void)=wmt_trace_native_backend ?
        dlsym(wmt_trace_native_backend,"wmt_native_foreign_callback_count") : NULL;
    if(!foreign_count||foreign_count()!=0)failure=failure ?: @"native foreign callback coverage missing/nonzero";
    fprintf(stderr,"NATIVE command_batches blit=%llu compute=%llu render=%llu foreign_callbacks=%llu\n",
        (unsigned long long)wmt_trace_native_command_batches[1],
        (unsigned long long)wmt_trace_native_command_batches[2],
        (unsigned long long)wmt_trace_native_command_batches[3],
        (unsigned long long)(foreign_count?foreign_count():UINT64_MAX));
    fprintf(stderr,"NATIVE factories texture=%llu buffer_texture=%llu render_pipeline=%llu\n",
        (unsigned long long)wmt_trace_native_factory_calls[21],
        (unsigned long long)wmt_trace_native_factory_calls[22],
        (unsigned long long)wmt_trace_native_factory_calls[34]);
    if (failure) { fprintf(stderr, "FAILED %s\n", failure.UTF8String); return 1; }
    printf("DIAGNOSTIC replay commits=%lu records=%lu Wine=NOT_ENABLED game_frames=NOT_ENABLED completeness=INCOMPLETE\n",
        (unsigned long)commits, (unsigned long)records.count);
  }
  return 0;
}
