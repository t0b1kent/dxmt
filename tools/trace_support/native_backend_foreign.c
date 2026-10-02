#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>

/* The standalone graph has no PE event consumer. Its declarative policy must
 * reject/project this frontend callback before invocation. This implementation
 * reports unsupported access; it provides no Wine event behavior. */
static _Atomic uint64_t foreign_callback_count;
int NtSetEvent(void *handle, void *previous_state) {
  (void)handle; (void)previous_state;
  atomic_fetch_add(&foreign_callback_count, 1);
  fputs("WMT_NATIVE_EXTERNAL_UNSUPPORTED NtSetEvent: no PE consumer\n", stderr);
  return (int)UINT32_C(0xc0000002);
}
uint64_t wmt_native_foreign_callback_count(void) {
  return atomic_load(&foreign_callback_count);
}
