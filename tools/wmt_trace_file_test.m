#include "../src/winemetal/unix/wmt_trace_file.h"
#include <assert.h>
#include <stdio.h>

/* CPU-only. Run under the same owned-write sandbox as the game launcher. */
int main(int argc, char **argv) {
  assert(argc == 2);
  @autoreleasepool {
    NSString *directory = [NSString stringWithUTF8String:argv[1]];
    assert([[NSFileManager defaultManager] createDirectoryAtPath:directory
        withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:NULL]);
    NSError *error = nil;
    BOOL atomic = [@"atomic baseline\n" writeToFile:
        [directory stringByAppendingPathComponent:@"atomic-baseline.txt"] atomically:YES
        encoding:NSUTF8StringEncoding error:&error];
    printf("ATOMIC baseline success=%d error_domain=%s error_code=%ld\n", atomic,
        error ? error.domain.UTF8String : "EMPTY", (long)error.code);
    assert(!atomic && error);
    for (NSString *name in @[@"status.txt", @"event-FAILED.txt", @"commands-FAILED.txt", @"frame-FAILED.txt"]) {
      NSString *path = [directory stringByAppendingPathComponent:name];
      assert(wmt_trace_write_text(@"first evidence\n", path, &error));
      NSData *before = [NSData dataWithContentsOfFile:path];
      assert([before isEqualToData:[@"first evidence\n" dataUsingEncoding:NSUTF8StringEncoding]]);
      error = nil;
      assert(!wmt_trace_write_text(@"replacement rejected\n", path, &error) && error);
      assert([[NSData dataWithContentsOfFile:path] isEqualToData:before]);
    }
    puts("PASS direct owned writes: status/event/commands/frame, duplicate preserves first evidence; GPU=NOT_ENABLED Wine=NOT_ENABLED");
  }
  return 0;
}
