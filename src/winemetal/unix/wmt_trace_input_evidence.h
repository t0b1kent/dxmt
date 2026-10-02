/* Optional hashes of raw records at the point the native consumer reads them.
 * This observes recorded inputs; it does not assert GPU memory completeness. */
#ifndef WMT_TRACE_INPUT_EVIDENCE_H
#define WMT_TRACE_INPUT_EVIDENCE_H
#include <CommonCrypto/CommonDigest.h>
#include <time.h>

static NSString *wmt_input_hex(const unsigned char *digest) {
  char hex[CC_SHA256_DIGEST_LENGTH*2+1];
  for(unsigned i=0;i<CC_SHA256_DIGEST_LENGTH;++i) sprintf(hex+i*2,"%02x",digest[i]);
  return [NSString stringWithUTF8String:hex];
}

static BOOL wmt_input_evidence(FILE *file, CC_SHA256_CTX *chain, NSData *raw,
    NSDictionary *record, NSString *path) {
  if(!file) return YES;
  unsigned char digest[CC_SHA256_DIGEST_LENGTH],prefix[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(raw.bytes,(CC_LONG)raw.length,digest);
  uint64_t length=raw.length;
  CC_SHA256_Update(chain,&length,sizeof(length));
  CC_SHA256_Update(chain,raw.bytes,(CC_LONG)raw.length);
  CC_SHA256_CTX copy=*chain;CC_SHA256_Final(prefix,&copy);
  NSMutableDictionary *row=[@{@"sequence":record[@"sequence"],
      @"file":path.lastPathComponent,@"bytes":@(raw.length),
      @"sha256":wmt_input_hex(digest),@"prefix-sha256":wmt_input_hex(prefix)} mutableCopy];
  struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
  row[@"monotonic-seconds"]=@((double)now.tv_sec+(double)now.tv_nsec/1e9);
  for(NSString *key in @[@"event",@"object",@"encoder",@"encoder-kind"])
    if(record[key]) row[key]=record[key];
  NSDictionary *fields=record[@"fields"];
  for(NSString *key in @[@"frame",@"texture",@"status",@"used-range-coverage",@"gpu-write-invalidation"])
    if(fields[key]) row[key]=fields[key];
  if(fields[@"snapshots"]) {
    NSMutableArray *snapshots=[NSMutableArray array];
    for(NSDictionary *snapshot in fields[@"snapshots"]) {
      NSData *bytes=snapshot[@"bytes"];
      if(![bytes isKindOfClass:[NSData class]]) return NO;
      CC_SHA256(bytes.bytes,(CC_LONG)bytes.length,digest);
      NSMutableDictionary *entry=[snapshot mutableCopy];[entry removeObjectForKey:@"bytes"];
      entry[@"bytes-sha256"]=wmt_input_hex(digest);[snapshots addObject:entry];
    }
    row[@"snapshots"]=snapshots;
  }
  NSData *json=[NSJSONSerialization dataWithJSONObject:row options:0 error:NULL];
  if(!json || fwrite(json.bytes,1,json.length,file)!=json.length || fputc('\n',file)==EOF) return NO;
  if([record[@"event"] isEqual:@"command-buffer-commit"] && fflush(file)) return NO;
  return YES;
}
#endif
