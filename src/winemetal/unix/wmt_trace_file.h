#ifndef WMT_TRACE_FILE_H
#define WMT_TRACE_FILE_H
#import <Foundation/Foundation.h>

/* The owned runner permits writes only below its run directory. Foundation's
 * atomic string writer may use an external replacement directory. Trace
 * metadata uses the same direct, exclusive NSData write as stream records.
 * An existing marker remains the first byte-exact evidence of that failure. */
static BOOL wmt_trace_write_text(NSString *text, NSString *path, NSError **error) {
  NSData *bytes = [text dataUsingEncoding:NSUTF8StringEncoding];
  return bytes && [bytes writeToFile:path options:NSDataWritingWithoutOverwriting error:error];
}
#endif
