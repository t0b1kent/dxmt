/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HyperBridge DX9 tape (lane HBDX9, 07.11.2026).  Format version 1.
 *
 * What it is: the exact stream of calls the thin guest d3d9.dll shim made into the native core (hbdx9_protocol.h), written
 * by the shim when HBDX9_TAPE is set.  Every byte the core read from guest memory is in the tape: packet buffers of SUBMIT
 * (uploads that referenced guest memory are rewritten as inline packets), shader token streams, creation parameters with
 * the handles the core returned.  Nothing the core only WROTE (fence page, readback destinations) is recorded.
 *
 * Layout: struct hbtape_header, then records { struct hbtape_record; payload[bytes]; zero padding to 8 }.  A tape cut short
 * (process killed) is valid up to its last complete record; a clean end has HBTAPE_REC_END.
 *
 * Payload by kind (kind = enum hbdx9_call for calls):
 *   HBDX9_CALL_INIT              struct hbdx9_init_params            (after the call: device handle, adapter)
 *   HBDX9_CALL_SWAPCHAIN_CREATE  struct hbdx9_swapchain_params       (after: swapchain, backbuffer, depth handles)
 *   HBDX9_CALL_SUBMIT            struct hbdx9_submit_params; packets (data = 0, size = packet bytes in the tape; every
 *                                packet starts at an 8-byte boundary, as in the shim's buffer; guest_data always 0)
 *   HBDX9_CALL_READBACK          struct hbdx9_readback_params        (dst is a guest address of the recording process)
 *   HBDX9_CALL_RELEASE           struct hbdx9_release_params
 *   HBDX9_CALL_CREATE_RESOURCE   struct hbdx9_create_resource_params (after: resource handle, out_levels; buffers carry pool)
 *   HBDX9_CALL_CREATE_SHADER     struct hbdx9_create_shader_params; uint32 tokens[token_count]
 *   HBTAPE_REC_MARK              text (frame counter / notes), NUL-terminated
 *   HBTAPE_REC_END               empty
 *   HBTAPE_REC_TRUNCATED         struct hbtape_truncated (size or frame limit reached; recording stopped there)
 *   HBTAPE_REC_BLOB  (version 2) struct hbtape_blob; data[size] — an upload payload stored once.  An UPDATE_BUFFER or
 *                                UPDATE_TEXTURE packet whose guest_data has HBTAPE_BLOB_REF set carries no inline data: its
 *                                bytes are blob (guest_data & 0xffffffff), always written earlier in the tape.
 *                                (07.11: the first Gothic 3 tape reached 10 GiB in ~60 s of loading.)
 *   HBTAPE_REC_APP_WRITE (v3)    struct hbtape_app_write; data[size] unless blob != ~0 — what the APPLICATION wrote, at the API:
 *                                a texture rect or buffer range at Unlock with its lock flags (READONLY included), or a deferred
 *                                NOOVERWRITE range found changed after Unlock when its SUBMIT went out.  Independent of the
 *                                shim's upload decisions (08.11: the world tape, recorded below them, could not show a black
 *                                Gothic 3 world that the session-5 upload path produced live).
 *   In SUBMIT packet streams of a v3 tape (tape copy only, never sent to a core):
 *     HBTAPE_PKT_APP_WRITE   {hbdx9_packet; uint32 id; uint32 pad} at the stream position where the application's write happened
 *     type | HBTAPE_PKT_APP_TAG   an UPDATE_BUFFER/UPDATE_TEXTURE the shim produced from an application write (immediate, lazy or
 *                                 deferred); a replay in --api-writes mode skips these and replays the APP_WRITE instead, so the
 *                                 shim under test makes its own upload decisions
 */
#ifndef HBDX9_TAPE_H
#define HBDX9_TAPE_H
#include <stdint.h>

#define HBTAPE_MAGIC "HBDX9TP1"
#define HBTAPE_VERSION 3
#define HBTAPE_BLOB_REF (1ull << 63)

enum hbtape_record_kind {
  HBTAPE_REC_MARK = 100,
  HBTAPE_REC_END = 101,
  HBTAPE_REC_TRUNCATED = 102,
  HBTAPE_REC_BLOB = 103,
  HBTAPE_REC_APP_WRITE = 104,   /* version 3 */
};

struct hbtape_blob {
  uint32_t id;            /* 0, 1, 2 ... in order of appearance */
  uint32_t size;
  uint64_t h1, h2;        /* two independent 64-bit hashes of the bytes (with size: the identity used for de-duplication) */
};

struct hbtape_header {
  char magic[8];          /* HBTAPE_MAGIC */
  uint32_t version;       /* HBTAPE_VERSION */
  uint32_t protocol;      /* HBDX9_PROTOCOL_VERSION of the recording shim */
  uint32_t header_bytes;  /* sizeof(struct hbtape_header) */
  uint32_t pointer_bits;  /* 32 for an i386 guest */
  uint64_t qpc_frequency; /* ticks per second of the time fields */
  char exe_name[32];      /* recording process */
};

struct hbtape_record {
  uint32_t kind;
  uint32_t bytes;         /* payload bytes (without padding) */
  uint64_t t_start;       /* QPC ticks since the tape began, when the call started */
  uint64_t t_call;        /* QPC ticks the unix call took (0 for marks) */
};

struct hbtape_truncated {
  uint64_t bytes_written;
  uint32_t frames;
  uint32_t reason;        /* 1 size limit, 2 frame limit */
};

#define HBTAPE_PKT_APP_WRITE 200
#define HBTAPE_PKT_APP_TAG 0x8000
struct hbtape_app_write {
  uint32_t id;            /* referenced by HBTAPE_PKT_APP_WRITE */
  uint32_t kind;          /* 1 texture rect at Unlock; 2 buffer range at Unlock; 3 buffer range written after Unlock */
  uint32_t flags;         /* D3DLOCK_* of the lock */
  uint32_t thread;        /* recording thread id */
  uint64_t resource;      /* native handle, as in CREATE_RESOURCE */
  uint32_t level, face;   /* texture */
  uint32_t x, y, w, h;    /* texture rect in pixels; buffer: x = offset, w = size */
  uint32_t pitch;         /* texture: bytes between block rows of data */
  uint32_t size;          /* bytes of data */
  uint32_t blob;          /* ~0u: data follows inline; else blob id (written earlier) */
  uint32_t pad;
};
struct hbtape_pkt_app_write { uint16_t type, pad0; uint32_t size; uint32_t id, pad; };

#if defined(__cplusplus)
static_assert(sizeof(struct hbtape_app_write) == 64, "hbtape_app_write");
static_assert(sizeof(struct hbtape_pkt_app_write) == 16, "hbtape_pkt_app_write");
static_assert(sizeof(struct hbtape_header) == 64, "hbtape_header");
static_assert(sizeof(struct hbtape_record) == 24, "hbtape_record");
static_assert(sizeof(struct hbtape_truncated) == 16, "hbtape_truncated");
static_assert(sizeof(struct hbtape_blob) == 24, "hbtape_blob");
#endif

#endif
