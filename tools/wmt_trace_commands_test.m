#include "../src/winemetal/unix/wmt_trace_commands.h"
#include <assert.h>

static uint64_t encode(obj_handle_t handle, void *context) {
  return handle == 0x11110000 ? 7 : 0;
}
static obj_handle_t decode(uint64_t identifier, void *context) {
  return identifier == 7 ? *(obj_handle_t *)context : 0;
}

int main(void) {
  @autoreleasepool {
    unsigned schemas = 0; size_t minimum = SIZE_MAX, maximum = 0;
    for (unsigned encoder = 1; encoder <= 3; ++encoder) {
      for (unsigned type = 0; type <= UINT16_MAX; ++type) {
        struct wmt_trace_command_schema schema;
        if (!wmt_trace_command_schema(encoder, type, &schema)) continue;
        NSMutableData *body = [NSMutableData dataWithLength:schema.size];
        ((struct wmtcmd_base *)[body mutableBytes])->type = type;
        NSString *error = nil;
        NSArray *records = wmt_trace_pack_commands(encoder, [body bytes], 4096, 8, encode, NULL, &error);
        assert(records && !error);
        obj_handle_t handle = 0x22220000;
        NSMutableArray *arena = [NSMutableArray array];
        assert(wmt_trace_unpack_commands(encoder, records, 4096, 8, decode, &handle, arena, &error) && !error);
        [records release];
        if (schema.size < minimum) minimum = schema.size;
        if (schema.size > maximum) maximum = schema.size;
        ++schemas;
      }
    }
    assert(schemas == 63);
    printf("ABI schemas=%u command-struct-min=%zu max=%zu bytes (payload excluded)\n", schemas, minimum, maximum);
    uint8_t bytes[] = {1, 2, 3, 255};
    struct wmtcmd_render_setbytes first = {0};
    struct wmtcmd_render_draw_indexed second = {0};
    first.type = WMTRenderCommandSetFragmentBytes;
    first.bytes.ptr = bytes; first.length = sizeof(bytes); first.index = 4;
    first.next.ptr = &second;
    second.type = WMTRenderCommandDrawIndexed;
    second.index_buffer = 0x11110000;
    NSString *failure = nil;
    NSArray *records = wmt_trace_pack_commands(3, (void *)&first, 4096, 8, encode, NULL, &failure);
    assert(records && !failure && [records count] == 2);
    NSData *disk = [NSPropertyListSerialization dataWithPropertyList:records
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    assert(disk);
    NSArray *roundtrip = [NSPropertyListSerialization propertyListWithData:disk
        options:NSPropertyListImmutable format:NULL error:NULL];
    memset(bytes, 0, sizeof(bytes)); /* producer storage is no longer valid */
    for (unsigned run = 0; run < 3; ++run) {
      obj_handle_t new_handle = 0x22220000 + run * 0x10000;
      NSMutableArray *arena = [NSMutableArray array];
      struct wmtcmd_base *head = wmt_trace_unpack_commands(3, roundtrip, 4096, 8,
          decode, &new_handle, arena, &failure);
      assert(head && !failure);
      struct wmtcmd_render_setbytes *a = (void *)head;
      struct wmtcmd_render_draw_indexed *b = a->next.ptr;
      assert(a->length == 4 && a->index == 4 && ((uint8_t *)a->bytes.ptr)[3] == 255);
      assert(b && b->index_buffer == new_handle && !b->next.ptr);
    }
    assert(!wmt_trace_pack_commands(3, (void *)&first, 1, 8, encode, NULL, &failure) && failure);
    second.next.ptr = &first;
    assert(!wmt_trace_pack_commands(3, (void *)&first, 4096, 8, encode, NULL, &failure) && failure);
    second.next.ptr = NULL;
    first.length = UINT64_MAX;
    assert(!wmt_trace_pack_commands(3, (void *)&first, 4096, 8, encode, NULL, &failure) && failure);
    first.length = 0; first.bytes.ptr = NULL;
    NSArray *zero = wmt_trace_pack_commands(3, (void *)&first, 4096, 8, encode, NULL, &failure);
    assert(zero && !failure); [zero release];
    struct wmtcmd_render_setviewports view = {0};
    struct WMTViewport viewport = {0};
    view.type = WMTRenderCommandSetViewports; view.viewport_count = 1; view.viewports.ptr = &viewport;
    NSArray *views = wmt_trace_pack_commands(3, (void *)&view, 4096, 8, encode, NULL, &failure);
    assert(views && [views[0][@"payload"] length] == sizeof(viewport));
    [views release];
    obj_handle_t new_handle = 0x22220000;
    NSMutableArray *arena = [NSMutableArray array];
    NSMutableArray *invalid = [roundtrip mutableCopy];
    NSMutableDictionary *bad = [invalid[0] mutableCopy];
    bad[@"payload"] = [NSData data]; invalid[0] = bad;
    assert(!wmt_trace_unpack_commands(3, invalid, 4096, 8, decode, &new_handle, arena, &failure) && failure);
    [bad release]; [invalid release]; [records release];
    puts("PASS command pack/disk/3 relocations, ephemeral payload, viewport, caps/cycle/overflow/malformed");
  }
}
