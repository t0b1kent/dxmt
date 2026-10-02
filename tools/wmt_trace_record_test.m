#include "../src/winemetal/unix/wmt_trace_record.h"
#include <assert.h>

int main(int argc, char **argv) {
  assert(argc == 2);
  @autoreleasepool {
    assert(setenv("MACRUNNER_WMT_RECORD", argv[1], 1) == 0);
    NSObject *encoder = [[NSObject alloc] init];
    NSObject *buffer = [[NSObject alloc] init];
    wmt_trace_record_event(@"buffer", buffer, @{@"length": @1024, @"options": @0});
    assert(!wmt_trace_record_failed && wmt_trace_record_sequence == 1);
    struct wmtcmd_compute_setbuffer command = {0};
    command.type = WMTComputeCommandSetBuffer;
    command.buffer = (obj_handle_t)buffer;
    wmt_trace_record_commands(2, encoder, (void *)&command);
    assert(!wmt_trace_record_failed && wmt_trace_record_sequence == 2);
    NSString *directory = [NSString stringWithUTF8String:argv[1]];
    NSString *constructor_name = [NSString stringWithFormat:@"event-%d-%012llu.plist", getpid(), 0ULL];
    NSDictionary *constructor = [NSDictionary dictionaryWithContentsOfFile:
        [directory stringByAppendingPathComponent:constructor_name]];
    assert([constructor[@"event"] isEqualToString:@"buffer"]);
    assert(([constructor[@"fields"] isEqualToDictionary:@{@"length": @1024, @"options": @0}]));
    assert([constructor[@"object"] unsignedLongLongValue] == wmt_trace_object_id(buffer));
    NSString *name = [NSString stringWithFormat:@"commands-%d-%012llu.plist", getpid(), 1ULL];
    NSDictionary *record = [NSDictionary dictionaryWithContentsOfFile:[directory stringByAppendingPathComponent:name]];
    assert([record[@"encoder-kind"] unsignedIntValue] == 2);
    assert([record[@"commands"] count] == 1);
    assert([record[@"commands"][0][@"objects"][0] unsignedLongLongValue] == wmt_trace_object_id(buffer));
    assert([[NSFileManager defaultManager] fileExistsAtPath:
        [directory stringByAppendingPathComponent:@"COMMANDS_ONLY_NOT_REPLAYABLE.txt"]]);
    unsetenv("MACRUNNER_WMT_RECORD");
    wmt_trace_record_commands(2, encoder, (void *)&command);
    wmt_trace_record_event(@"buffer", buffer, @{@"length": @2048, @"options": @0});
    assert(wmt_trace_record_sequence == 2);
    [encoder release]; [buffer release];
    puts("PASS producer gate, object IDs, constructor/command shared disk sequence, incomplete marker");
  }
}
