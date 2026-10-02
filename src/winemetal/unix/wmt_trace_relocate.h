#ifndef WMT_TRACE_RELOCATE_H
#define WMT_TRACE_RELOCATE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Stable IDs are recording-local generations, never native pointers. Values
 * below are runtime map entries, not the on-disk encoding of these structs. */
struct wmt_trace_resource {
  uint64_t object_id;
  uint64_t address;
  uint64_t length;
  uint64_t resource_id;
  uint32_t kind; /* 1 buffer, 2 texture/view, 3 sampler */
};
struct wmt_trace_relocation {
  uint64_t offset;
  uint64_t object_id;
  uint64_t addend;
  uint32_t kind;
};

/* Convert one producer-declared value into a generation + addend. An ambiguous
 * address/ID is rejected, never resolved by scanning arbitrary snapshot words. */
static int
wmt_trace_resolve(uint64_t offset, uint32_t kind, uint64_t value,
                  const struct wmt_trace_resource *objects, size_t count,
                  struct wmt_trace_relocation *out) {
  if (!out || kind < 1 || kind > 3 || (count && !objects))
    return 0;
  struct wmt_trace_relocation result = {offset, 0, 0, kind};
  if (value) {
    size_t matches = 0;
    for (size_t i = 0; i < count; ++i) {
      const struct wmt_trace_resource *object = &objects[i];
      if (object->kind != kind || !object->object_id)
        continue;
      int matched = kind == 1 ?
          value >= object->address && value - object->address < object->length :
          value == object->resource_id;
      if (matched) {
        if (++matches != 1)
          return 0;
        result.object_id = object->object_id;
        result.addend = kind == 1 ? value - object->address : 0;
      }
    }
    if (!matches)
      return 0;
  }
  *out = result;
  return 1;
}

/* Atomic validation: destination is unchanged on every rejection, including
 * missing generations, duplicate fields, overflow and kind mismatch. */
static int
wmt_trace_relocate(void *destination, size_t length,
                   const struct wmt_trace_relocation *fields, size_t field_count,
                   const struct wmt_trace_resource *objects, size_t object_count) {
  if ((!destination && length) || (field_count && !fields) ||
      (object_count && !objects) || field_count > SIZE_MAX / sizeof(uint64_t))
    return 0;
  uint64_t *values = field_count ? malloc(field_count * sizeof(uint64_t)) : NULL;
  if (field_count && !values)
    return 0;
  for (size_t i = 0; i < field_count; ++i) {
    const struct wmt_trace_relocation *field = &fields[i];
    if ((field->offset & 7) || field->offset > length ||
        length - field->offset < 8 || field->kind < 1 || field->kind > 3)
      goto reject;
    for (size_t j = 0; j < i; ++j)
      if (fields[j].offset == field->offset)
        goto reject;
    values[i] = 0;
    if (!field->object_id) {
      if (field->addend)
        goto reject;
      continue;
    }
    const struct wmt_trace_resource *object = NULL;
    for (size_t j = 0; j < object_count; ++j) {
      if (objects[j].object_id == field->object_id) {
        if (object)
          goto reject;
        object = &objects[j];
      }
    }
    if (!object || object->kind != field->kind)
      goto reject;
    if (field->kind == 1) {
      if (field->addend >= object->length || !object->address ||
          field->addend > UINT64_MAX - object->address)
        goto reject;
      values[i] = object->address + field->addend;
    } else {
      if (field->addend || !object->resource_id)
        goto reject;
      values[i] = object->resource_id;
    }
  }
  for (size_t i = 0; i < field_count; ++i)
    memcpy((unsigned char *)destination + fields[i].offset, &values[i], 8);
  free(values);
  return 1;
reject:
  free(values);
  return 0;
}
#endif
