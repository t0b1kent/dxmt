#include "../src/winemetal/unix/wmt_trace_relocate.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  const struct wmt_trace_resource original[] = {
      {1, 0x100000, 4096, 0, 1}, {2, 0x200000, 4, 0, 1},
      {3, 0, 0, 0x777, 2}, {4, 0, 0, 0x777, 3}, {5, 0, 0, 0x888, 3}};
  const struct wmt_trace_resource replay[] = {
      {1, 0x900000, 4096, 0, 1}, {2, 0xa00000, 4, 0, 1},
      {3, 0, 0, 0x999, 2}, {4, 0, 0, 0xaaa, 3}, {5, 0, 0, 0xbbb, 3}};
  struct wmt_trace_relocation fields[5];
  assert(wmt_trace_resolve(0, 1, 0x100040, original, 5, &fields[0]));
  assert(wmt_trace_resolve(16, 2, 0x777, original, 5, &fields[1]));
  assert(wmt_trace_resolve(24, 3, 0x777, original, 5, &fields[2]));
  assert(wmt_trace_resolve(32, 3, 0x888, original, 5, &fields[3]));
  assert(wmt_trace_resolve(48, 1, 0x200000, original, 5, &fields[4]));
  uint64_t bytes[] = {0x100040, 0x100000, 0x777, 0x777, 0x888, 0x100000, 0x200000};
  assert(wmt_trace_relocate(bytes, sizeof(bytes), fields, 5, replay, 5));
  const uint64_t expected[] = {0x900040, 0x100000, 0x999, 0xaaa, 0xbbb, 0x100000, 0xa00000};
  assert(!memcmp(bytes, expected, sizeof(bytes)));
  /* Rejected batches never change any destination byte. */
  struct wmt_trace_relocation bad[2] = {{0, 1, 0, 1}, {16, 999, 0, 2}};
  assert(!wmt_trace_relocate(bytes, sizeof(bytes), bad, 2, replay, 5));
  assert(!memcmp(bytes, expected, sizeof(bytes)));
  bad[1] = (struct wmt_trace_relocation){0, 1, 0, 1};
  assert(!wmt_trace_relocate(bytes, sizeof(bytes), bad, 2, replay, 5));
  bad[1] = (struct wmt_trace_relocation){16, 1, 4096, 1};
  assert(!wmt_trace_relocate(bytes, sizeof(bytes), bad, 2, replay, 5));
  bad[1] = (struct wmt_trace_relocation){16, 0, 1, 1};
  assert(!wmt_trace_relocate(bytes, sizeof(bytes), bad, 2, replay, 5));
  bad[1] = (struct wmt_trace_relocation){16, 3, 0, 3};
  assert(!wmt_trace_relocate(bytes, sizeof(bytes), bad, 2, replay, 5));
  bad[1] = (struct wmt_trace_relocation){17, 3, 0, 2};
  assert(!wmt_trace_relocate(bytes, sizeof(bytes), bad, 2, replay, 5));
  assert(!memcmp(bytes, expected, sizeof(bytes)));
  struct wmt_trace_resource overflow[] = {{1, UINT64_MAX - 4, 16, 0, 1}};
  bad[0] = (struct wmt_trace_relocation){0, 1, 8, 1};
  assert(!wmt_trace_relocate(bytes, sizeof(bytes), bad, 1, overflow, 1));
  assert(!wmt_trace_resolve(0, 1, 0x101000, original, 5, &bad[0]));
  struct wmt_trace_resource ambiguous[] = {original[0], original[0]};
  ambiguous[1].object_id = 6;
  assert(!wmt_trace_resolve(0, 1, 0x100040, ambiguous, 2, &bad[0]));
  assert(wmt_trace_resolve(0, 1, 0, original, 5, &bad[0]));
  assert(wmt_trace_relocate(bytes, sizeof(bytes), bad, 1, replay, 5));
  assert(bytes[0] == 0);
  puts("PASS native relocation: generation/addend, typed ID namespaces, scalar preservation, atomic rejection, null");
  return 0;
}
