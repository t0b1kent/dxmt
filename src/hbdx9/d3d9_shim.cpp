/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HyperBridge DX9 — thin guest d3d9.dll (lane HBDX9, 07.11.2026).  Protocol v2 (milestone b).
 *
 * Runs translated (FEX32 for i386 games), so it only keeps COM objects, shadow state for getters and a packet buffer.
 * Everything with real cost (Metal objects, state tracking for draws, shader translation, PSO caching, encoding,
 * presentation) lives in the native ARM64 core hbdx9.so behind HBDX9_CALL_* (hbdx9_protocol.h).  One unix call per
 * frame (SUBMIT at Present) plus synchronous calls where the API needs an answer: object creation, readback, and
 * large uploads (> 64 KB: the pending packets and the upload go in one SUBMIT that references guest memory).
 *
 * Implemented: device/adapter queries, Clear (target/z/stencil, rects, viewport), Present, back buffer and auto depth,
 * render/sampler/texture-stage states, viewport/scissor/transforms, FVF and vertex declarations, vertex/index buffers
 * (Lock/Unlock), 2D textures (LockRect/UnlockRect, surfaces of levels), render-target/depth surfaces,
 * VS/PS creation and constants (F/I/B), DrawPrimitive[UP], DrawIndexedPrimitive[UP], GetRenderTargetData.
 * Every other method is a generated stub that logs its first use (hbdx9_stubs.hpp). */
#include "hbdx9_common.hpp"
#include "hbdx9_protocol.h"
#include "hbdx9_stubs.hpp"
#include <stdarg.h>
#include <string.h>
#include <new>
#include <vector>
#include <deque>
#include <algorithm>

/* unix calls go through the builtin carrier hbdx9.dll (see hbdx9_carrier.c for why d3d9.dll itself is native) */
extern "C" __declspec(dllimport) NTSTATUS WINAPI HBDX9Call(unsigned int code, void *params);

/* ---------------- logging ---------------- */
/* one WriteFile per line: stderr is shared with the translator's own messages, and a line written in pieces gets them
   spliced into it (07.11: "@ approx macrunner-fex-div-proven-high: ..." in a trace:fps line) */
static void err_line(const char *b, int n) {
  if (n <= 0) return;
  DWORD w; HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, b, (DWORD)n, &w, nullptr);
}
void hbdx9_log(const char *fmt, ...) {
  char b[1024]; int n = snprintf(b, sizeof(b), "hbdx9: ");
  va_list ap; va_start(ap, fmt); int m = vsnprintf(b + n, sizeof(b) - n - 1, fmt, ap); va_end(ap);
  if (m < 0) m = 0; n += std::min<int>(m, (int)sizeof(b) - n - 2); b[n++] = '\n';
  err_line(b, n);
}

/* Match wined3d's fullscreen setup: select the back-buffer mode and cover
   the monitor with the device window. With EmulateModeset, win32u maps that
   window from the emulated monitor to the physical display. Restore the
   display mode and window state on exit. HBDX9_FS_MODESET=0 disables this. */
struct Hbdx9FsState { bool on = false, mode_changed = false; HWND wnd = nullptr; LONG style = 0, exstyle = 0; RECT rect{}; };
static int fs_modeset_on() {
  static int v = -1;
  if (v < 0) { char b[8]; DWORD n = GetEnvironmentVariableA("HBDX9_FS_MODESET", b, sizeof(b)); v = !(n && n < sizeof(b) && b[0] == '0'); }
  return v;
}
static void hbdx9_fs_enter(Hbdx9FsState &fs, HWND wnd, UINT w, UINT h, UINT hz) {
  if (!wnd || !w || !h || !fs_modeset_on()) return;
  DEVMODEW cur{}; cur.dmSize = sizeof(cur);
  bool have = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur);
  if (!have || cur.dmPelsWidth != w || cur.dmPelsHeight != h) {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm); dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmBitsPerPel = 32;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (hz > 1 && hz < 1000) { dm.dmDisplayFrequency = hz; dm.dmFields |= DM_DISPLAYFREQUENCY; }
    LONG r = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL && (dm.dmFields & DM_DISPLAYFREQUENCY)) {   /* the rate is a hint: try the mode alone */
      dm.dmFields &= ~DM_DISPLAYFREQUENCY; dm.dmDisplayFrequency = 0; r = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
    }
    hbdx9_log("fullscreen: mode %lux%lu -> %ux%u: ChangeDisplaySettings = %ld", have ? cur.dmPelsWidth : 0, have ? cur.dmPelsHeight : 0, w, h, (long)r);
    if (r == DISP_CHANGE_SUCCESSFUL) fs.mode_changed = true;
  }
  if (!fs.on) { fs.wnd = wnd; fs.style = GetWindowLongW(wnd, GWL_STYLE); fs.exstyle = GetWindowLongW(wnd, GWL_EXSTYLE); GetWindowRect(wnd, &fs.rect); }
  MONITORINFO mi{}; mi.cbSize = sizeof(mi); POINT o = {0, 0};
  RECT mr = {0, 0, (LONG)w, (LONG)h};
  if (GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi)) mr = mi.rcMonitor;
  LONG st = (fs.style & ~(WS_OVERLAPPEDWINDOW | WS_CHILD)) | WS_POPUP | WS_SYSMENU | WS_VISIBLE;
  LONG ex = (fs.exstyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE)) | WS_EX_TOPMOST;
  SetWindowLongW(wnd, GWL_STYLE, st); SetWindowLongW(wnd, GWL_EXSTYLE, ex);
  SetWindowPos(wnd, HWND_TOPMOST, mr.left, mr.top, (int)w, (int)h, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
  RECT wr; GetWindowRect(wnd, &wr);
  hbdx9_log("fullscreen: window %p -> [%ld,%ld,%ld,%ld] (monitor [%ld,%ld,%ld,%ld], was [%ld,%ld,%ld,%ld])", (void *)wnd, wr.left, wr.top, wr.right, wr.bottom,
            mr.left, mr.top, mr.right, mr.bottom, fs.rect.left, fs.rect.top, fs.rect.right, fs.rect.bottom);
  fs.on = true;
}
static void hbdx9_fs_leave(Hbdx9FsState &fs) {
  if (!fs.on) return;
  fs.on = false;
  if (fs.mode_changed) { LONG r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr); hbdx9_log("fullscreen: mode restored = %ld", (long)r); fs.mode_changed = false; }
  if (IsWindow(fs.wnd)) {
    SetWindowLongW(fs.wnd, GWL_STYLE, fs.style); SetWindowLongW(fs.wnd, GWL_EXSTYLE, fs.exstyle);
    SetWindowPos(fs.wnd, (fs.exstyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST, fs.rect.left, fs.rect.top, fs.rect.right - fs.rect.left, fs.rect.bottom - fs.rect.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
  }
}
/* (CRYSIS 12.11.2026) "format not supported" once per (call, format): Crysis retried one refused format 124 799 times in a
   7-minute run, one stderr line each. */
static void hbdx9_unsupported_format(const char *call, unsigned long fmt) {
  static struct { const char *call; unsigned long fmt; } seen[64]; static LONG count;
  LONG n = count, i;
  for (i = 0; i < n && i < 64; i++) if (seen[i].call == call && seen[i].fmt == fmt) return;
  if (n < 64) { seen[n].call = call; seen[n].fmt = fmt; count = n + 1; }
  hbdx9_log("%s: format %08lx not supported", call, fmt);
}
void hbdx9_unimpl(const char *name) {
  static const char *seen[512]; static LONG count;
  LONG n = count, i;
  for (i = 0; i < n && i < 512; i++) if (seen[i] == name) return;
  if (n < 512) { seen[n] = name; count = n + 1; }
  hbdx9_log("unimpl %s", name);
}

/* ---------------- tape recorder (HBDX9_TAPE, format in hbdx9_tape.h) ----------------
 * Every unix call of the shim passes here.  Off (the default) it is one load and a branch.  On, it records the call with
 * everything the core read from guest memory, so tape/hbtape_play.exe can replay the stream without the game through
 * any d3d9 (HyperBridge DX9 or Wine's wined3d).  HBDX9_TAPE = 1 (next to the exe) | <dir> | <file>.hbtape; a leading '/'
 * is a unix path (Z:).  HBDX9_TAPE_MAX_MB (default 8192) and HBDX9_TAPE_MAX_FRAMES (default 0 = none) stop the
 * recording with a TRUNCATED record; the game itself runs on unchanged. */
#include "hbdx9_tape.h"
#include <map>
#include <tuple>
/* identity of an upload payload for de-duplication (with its size); two independent word-wise hashes */
static void blob_hash(const uint8_t *p, size_t n, uint64_t *a, uint64_t *b) {
  uint64_t h1 = 0x9e3779b97f4a7c15ull ^ n, h2 = 0xc2b2ae3d27d4eb4full + n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w; memcpy(&w, p + i, 8);
    h1 = (h1 ^ w) * 0x100000001b3ull; h2 = (h2 + w) * 0xff51afd7ed558ccdull; h2 ^= h2 >> 29;
  }
  for (; i < n; i++) { h1 = (h1 ^ p[i]) * 0x100000001b3ull; h2 = (h2 + p[i]) * 0xc4ceb9fe1a85ec53ull; }
  *a = h1; *b = h2;
}
struct TapeRecorder {
  CRITICAL_SECTION cs;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::vector<uint8_t> buf;
  LARGE_INTEGER t0{};
  uint64_t written = 0, limit = 8192ull << 20, records = 0;
  uint32_t frames = 0, frame_limit = 0, check_every = 0;
  uint32_t dedup_min = 4096;                                     /* HBDX9_TAPE_DEDUP_MIN; 0 = every upload inline */
  std::map<std::tuple<uint64_t, uint64_t, uint32_t>, uint32_t> blobs;
  uint64_t blob_bytes = 0, ref_bytes = 0;
  /* v3 (08.11): application writes, positioned in the packet stream of their device: device -> (packet offset, app write id);
     device -> offsets of UPDATE packets the shim produced from application writes (tagged in the tape copy) */
  std::map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> marks;
  std::map<uint64_t, std::vector<uint32_t>> tagged;
  uint32_t next_app = 0; uint64_t app_writes = 0, app_bytes = 0;
  volatile LONG on = 0;
  char path[MAX_PATH] = {};

  void put(const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; buf.insert(buf.end(), b, b + n); }
  void pad() { while (buf.size() & 7) buf.push_back(0); }
  void head(uint32_t kind, uint32_t bytes, uint64_t ts, uint64_t tc) {
    hbtape_record r{kind, bytes, ts, tc}; put(&r, sizeof(r)); records++;
  }
  void drain() {
    size_t at = 0;
    while (at < buf.size()) {
      DWORD n = 0, want = (DWORD)std::min<size_t>(buf.size() - at, 64u << 20);
      if (!WriteFile(file, buf.data() + at, want, &n, nullptr) || !n) { hbdx9_log("tape: write failed (%lu) at %llu bytes, recording stopped",
                                                                                   GetLastError(), (unsigned long long)written); on = 0; break; }
      at += n; written += n;
    }
    buf.clear();
  }
  void stop(uint32_t reason, uint64_t ts) {
    hbtape_truncated t{written + buf.size(), frames, reason};
    head(HBTAPE_REC_TRUNCATED, sizeof(t), ts, 0); put(&t, sizeof(t)); pad(); drain();
    hbdx9_log("tape: stopped (%s) at frame %u, %llu bytes, %llu records: %s", reason == 1 ? "size limit" : "frame limit", frames,
              (unsigned long long)written, (unsigned long long)records, path);
    on = 0;
  }
};
static TapeRecorder g_tape;
static INIT_ONCE g_tape_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK tape_init(PINIT_ONCE, PVOID, PVOID *) {
  TapeRecorder &t = g_tape;
  InitializeCriticalSection(&t.cs);
  char v[MAX_PATH]; DWORD n = GetEnvironmentVariableA("HBDX9_TAPE", v, sizeof(v));
  if (!n || n >= sizeof(v) || !strcmp(v, "0")) return TRUE;
  char num[32];
  if (GetEnvironmentVariableA("HBDX9_TAPE_MAX_MB", num, sizeof(num)) - 1u < sizeof(num) - 1) t.limit = (uint64_t)strtoul(num, nullptr, 10) << 20;
  if (GetEnvironmentVariableA("HBDX9_TAPE_MAX_FRAMES", num, sizeof(num)) - 1u < sizeof(num) - 1) t.frame_limit = strtoul(num, nullptr, 10);
  /* every N-th frame: read the back buffer back before Present and put its hash in the tape (MARK "livecheck"), so a
     replay can prove it draws the live game's frame (the READBACK call itself is in the tape at the same place) */
  if (GetEnvironmentVariableA("HBDX9_TAPE_CHECK_EVERY", num, sizeof(num)) - 1u < sizeof(num) - 1) t.check_every = strtoul(num, nullptr, 10);
  if (GetEnvironmentVariableA("HBDX9_TAPE_DEDUP_MIN", num, sizeof(num)) - 1u < sizeof(num) - 1) t.dedup_min = strtoul(num, nullptr, 10);
  char exe[MAX_PATH]; DWORD en = GetModuleFileNameA(nullptr, exe, sizeof(exe));
  const char *base = exe; for (DWORD k = 0; k < en; k++) if (exe[k] == '\\' || exe[k] == '/') base = exe + k + 1;
  char stem[64]; lstrcpynA(stem, base, sizeof(stem)); if (char *dot = strrchr(stem, '.')) *dot = 0;
  char dir[MAX_PATH];
  bool is_file = n > 7 && !_stricmp(v + n - 7, ".hbtape");
  if (!strcmp(v, "1")) { lstrcpynA(dir, exe, (int)(base - exe) + 1); }
  else {
    if (v[0] == '/') { dir[0] = 'Z'; dir[1] = ':'; lstrcpynA(dir + 2, v, sizeof(dir) - 2); } else lstrcpynA(dir, v, sizeof(dir));
    for (char *c = dir; *c; c++) if (*c == '/') *c = '\\';
  }
  if (is_file) lstrcpynA(t.path, dir, sizeof(t.path));
  else {
    size_t dl = strlen(dir);
    snprintf(t.path, sizeof(t.path), "%s%s%s-%lu.hbtape", dir, dl && dir[dl - 1] != '\\' ? "\\" : "", stem, (unsigned long)GetCurrentProcessId());
  }
  t.file = CreateFileA(t.path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (t.file == INVALID_HANDLE_VALUE) { hbdx9_log("tape: cannot create %s (%lu), recording off", t.path, GetLastError()); return TRUE; }
  LARGE_INTEGER f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t.t0);
  hbtape_header h{}; memcpy(h.magic, HBTAPE_MAGIC, 8); h.version = HBTAPE_VERSION; h.protocol = HBDX9_PROTOCOL_VERSION;
  h.header_bytes = sizeof(h); h.pointer_bits = sizeof(void *) * 8; h.qpc_frequency = (uint64_t)f.QuadPart; lstrcpynA(h.exe_name, base, sizeof(h.exe_name));
  t.put(&h, sizeof(h)); t.drain();
  t.on = 1;
  hbdx9_log("tape: recording to %s (limit %llu MB, %u frames, live check every %u, de-dup from %u bytes)", t.path,
            (unsigned long long)(t.limit >> 20), t.frame_limit, t.check_every, t.dedup_min);
  return TRUE;
}

/* record one finished call (under g_tape.cs) */
static void tape_record(unsigned code, const void *params, uint64_t ts, uint64_t tc) {
  TapeRecorder &t = g_tape;
  switch (code) {
  case HBDX9_CALL_INIT: t.head(code, sizeof(hbdx9_init_params), ts, tc); t.put(params, sizeof(hbdx9_init_params)); break;
  case HBDX9_CALL_SWAPCHAIN_CREATE: t.head(code, sizeof(hbdx9_swapchain_params), ts, tc); t.put(params, sizeof(hbdx9_swapchain_params)); break;
  case HBDX9_CALL_READBACK: t.head(code, sizeof(hbdx9_readback_params), ts, tc); t.put(params, sizeof(hbdx9_readback_params)); break;
  case HBDX9_CALL_RELEASE: t.head(code, sizeof(hbdx9_release_params), ts, tc); t.put(params, sizeof(hbdx9_release_params)); break;
  case HBDX9_CALL_CREATE_RESOURCE: t.head(code, sizeof(hbdx9_create_resource_params), ts, tc); t.put(params, sizeof(hbdx9_create_resource_params)); break;
  case HBDX9_CALL_CREATE_SHADER: {
    auto *p = (const hbdx9_create_shader_params *)params;
    uint32_t n = (!p->status && p->token_count <= p->max_dwords) ? p->token_count : 0;
    t.head(code, sizeof(*p) + n * 4, ts, tc); t.put(p, sizeof(*p)); if (n) t.put((const void *)(uintptr_t)p->tokens, n * 4);
    break;
  }
  case HBDX9_CALL_SUBMIT: {
    auto *s = (const hbdx9_submit_params *)params;
    const uint8_t *data = (const uint8_t *)(uintptr_t)s->data;
    /* pass 1: per packet its upload payload (inline or by guest pointer), its blob (de-dup) and its size in the tape */
    struct Up { uint32_t at, ref; };
    std::vector<Up> ups;
    uint64_t total = 0; uint32_t presents = 0;
    std::vector<std::pair<uint32_t, uint32_t>> marks; std::vector<uint32_t> tags;
    { auto mk = t.marks.find(s->device); if (mk != t.marks.end()) { marks.swap(mk->second); t.marks.erase(mk); }
      auto tg = t.tagged.find(s->device); if (tg != t.tagged.end()) { tags.swap(tg->second); t.tagged.erase(tg); std::sort(tags.begin(), tags.end()); } }
    total += marks.size() * sizeof(hbtape_pkt_app_write);
    for (uint32_t at = 0; at + sizeof(hbdx9_packet) <= s->size;) {
      auto *h = (const hbdx9_packet *)(data + at);
      uint32_t step = (h->size + 7) & ~7u; if (!step || h->size < sizeof(hbdx9_packet)) break;
      uint32_t out = step;
      const uint8_t *src = nullptr; uint32_t size = 0, hsz = 0; uint64_t gd = 0;
      if (h->type == HBDX9_PKT_UPDATE_BUFFER) { auto *u = (const hbdx9_pkt_update_buffer *)h; gd = u->guest_data; size = u->size; hsz = sizeof(*u); }
      if (h->type == HBDX9_PKT_UPDATE_TEXTURE) { auto *u = (const hbdx9_pkt_update_texture *)h; gd = u->guest_data; size = u->size; hsz = sizeof(*u); }
      if (hsz) {
        src = gd ? (const uint8_t *)(uintptr_t)gd : (const uint8_t *)h + hsz;
        uint32_t ref = ~0u;
        if (t.dedup_min && size >= t.dedup_min) {
          uint64_t a, b; blob_hash(src, size, &a, &b);
          auto key = std::make_tuple(a, b, size);
          auto it = t.blobs.find(key);
          if (it == t.blobs.end()) {
            ref = (uint32_t)t.blobs.size(); t.blobs.emplace(key, ref);
            hbtape_blob bl{ref, size, a, b};
            t.head(HBTAPE_REC_BLOB, sizeof(bl) + size, ts, 0); t.put(&bl, sizeof(bl)); t.put(src, size); t.pad();
            t.blob_bytes += size;
          } else { ref = it->second; t.ref_bytes += size; }
          out = (hsz + 7) & ~7u;
        } else if (gd) out = (hsz + size + 7) & ~7u;
        ups.push_back({at, ref});
      }
      if (h->type == HBDX9_PKT_PRESENT) presents++;
      total += out; at += step;
    }
    hbdx9_submit_params copy = *s; copy.data = 0; copy.size = (uint32_t)total;
    t.head(code, (uint32_t)(sizeof(copy) + total), ts, tc); t.put(&copy, sizeof(copy));
    /* pass 2 */
    size_t ui = 0, mi = 0;
    auto emit_marks = [&](uint32_t upto) {
      for (; mi < marks.size() && marks[mi].first <= upto; mi++) {
        hbtape_pkt_app_write m{HBTAPE_PKT_APP_WRITE, 0, sizeof(hbtape_pkt_app_write), marks[mi].second, 0}; t.put(&m, sizeof(m));
      }
    };
    for (uint32_t at = 0; at + sizeof(hbdx9_packet) <= s->size;) {
      auto *h = (const hbdx9_packet *)(data + at);
      uint32_t step = (h->size + 7) & ~7u; if (!step || h->size < sizeof(hbdx9_packet)) break;
      emit_marks(at);
      uint16_t tag = std::binary_search(tags.begin(), tags.end(), at) ? HBTAPE_PKT_APP_TAG : 0;
      if (ui < ups.size() && ups[ui].at == at) {
        uint32_t ref = ups[ui++].ref;
        if (h->type == HBDX9_PKT_UPDATE_BUFFER) {
          hbdx9_pkt_update_buffer u = *(const hbdx9_pkt_update_buffer *)h; u.hdr.type |= tag;
          const void *src = u.guest_data ? (const void *)(uintptr_t)u.guest_data : (const void *)((const hbdx9_pkt_update_buffer *)h + 1);
          if (ref != ~0u) { u.guest_data = HBTAPE_BLOB_REF | ref; u.hdr.size = sizeof(u); t.put(&u, sizeof(u)); }
          else { u.guest_data = 0; u.hdr.size = sizeof(u) + u.size; t.put(&u, sizeof(u)); t.put(src, u.size); }
        } else {
          hbdx9_pkt_update_texture u = *(const hbdx9_pkt_update_texture *)h; u.hdr.type |= tag;
          const void *src = u.guest_data ? (const void *)(uintptr_t)u.guest_data : (const void *)((const hbdx9_pkt_update_texture *)h + 1);
          if (ref != ~0u) { u.guest_data = HBTAPE_BLOB_REF | ref; u.hdr.size = sizeof(u); t.put(&u, sizeof(u)); }
          else { u.guest_data = 0; u.hdr.size = sizeof(u) + u.size; t.put(&u, sizeof(u)); t.put(src, u.size); }
        }
        t.pad();
      } else {
        t.put(h, step);
      }
      at += step;
    }
    emit_marks(~0u);
    t.pad();
    if (presents) {
      uint32_t before = t.frames; t.frames += presents;
      if (t.frames / 600 != before / 600) {
        char m[96]; int k = snprintf(m, sizeof(m), "frame %u bytes %llu", t.frames, (unsigned long long)(t.written + t.buf.size()));
        t.head(HBTAPE_REC_MARK, k + 1, ts, 0); t.put(m, k + 1); t.pad();
        hbdx9_log("tape: %s", m);
      }
      t.drain();
      if (t.on && t.frame_limit && t.frames >= t.frame_limit) { t.stop(2, ts); return; }
    }
    break;
  }
  default: return; /* STATS: answers only */
  }
  t.pad();
  if (t.buf.size() > (8u << 20)) t.drain();
  if (t.on && t.written + t.buf.size() > t.limit) t.stop(1, ts);
}

/* v3: one application write (caller holds the device lock; takes the tape lock).  offset = the device's packet stream position */
static void tape_app_write(uint64_t device, uint32_t offset, uint32_t kind, uint32_t flags, uint64_t resource, uint32_t level,
                           uint32_t face, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pitch, const void *data, uint32_t size) {
  TapeRecorder &t = g_tape;
  if (!t.on || !data) return;
  EnterCriticalSection(&t.cs);
  if (t.on) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now); uint64_t ts = (uint64_t)(now.QuadPart - t.t0.QuadPart);
    hbtape_app_write a{}; a.id = t.next_app++; a.kind = kind; a.flags = flags; a.thread = GetCurrentThreadId(); a.resource = resource;
    a.level = level; a.face = face; a.x = x; a.y = y; a.w = w; a.h = h; a.pitch = pitch; a.size = size; a.blob = ~0u;
    if (t.dedup_min && size >= t.dedup_min) {
      uint64_t h1, h2; blob_hash((const uint8_t *)data, size, &h1, &h2);
      auto key = std::make_tuple(h1, h2, size); auto it = t.blobs.find(key);
      if (it == t.blobs.end()) {
        uint32_t ref = (uint32_t)t.blobs.size(); t.blobs.emplace(key, ref); hbtape_blob bl{ref, size, h1, h2};
        t.head(HBTAPE_REC_BLOB, sizeof(bl) + size, ts, 0); t.put(&bl, sizeof(bl)); t.put(data, size); t.pad(); t.blob_bytes += size; a.blob = ref;
      } else { a.blob = it->second; t.ref_bytes += size; }
    }
    t.head(HBTAPE_REC_APP_WRITE, sizeof(a) + (a.blob == ~0u ? size : 0), ts, 0); t.put(&a, sizeof(a));
    if (a.blob == ~0u) t.put(data, size);
    t.pad();
    t.marks[device].push_back({offset, a.id}); t.app_writes++; t.app_bytes += size;
    if (t.buf.size() > (8u << 20)) t.drain();
    if (t.on && t.written + t.buf.size() > t.limit) t.stop(1, ts);
  }
  LeaveCriticalSection(&t.cs);
}
static void tape_tag(uint64_t device, uint32_t offset) {
  if (!g_tape.on) return;
  EnterCriticalSection(&g_tape.cs); if (g_tape.on) g_tape.tagged[device].push_back(offset); LeaveCriticalSection(&g_tape.cs);
}

/* (session 6) time the calling thread spends inside unix calls (the native core), for the per-frame figure in the boundary
   line; QPC ticks, summed over all calls of the process */
static volatile uint64_t g_in_call_ticks = 0;
static NTSTATUS hb_call(unsigned code, void *params) {
  InitOnceExecuteOnce(&g_tape_once, tape_init, nullptr, nullptr);
  if (!g_tape.on) {
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    NTSTATUS st = HBDX9Call(code, params);
    QueryPerformanceCounter(&b);
    g_in_call_ticks += (uint64_t)(b.QuadPart - a.QuadPart);
    return st;
  }
  EnterCriticalSection(&g_tape.cs); /* whole call: the tape order is the core's order, also across devices */
  LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
  NTSTATUS st = HBDX9Call(code, params);
  QueryPerformanceCounter(&b);
  g_in_call_ticks += (uint64_t)(b.QuadPart - a.QuadPart);
  if (g_tape.on) tape_record(code, params, (uint64_t)(a.QuadPart - g_tape.t0.QuadPart), (uint64_t)(b.QuadPart - a.QuadPart));
  LeaveCriticalSection(&g_tape.cs);
  return st;
}
static void tape_mark(const char *text) {
  if (!g_tape.on) return;
  EnterCriticalSection(&g_tape.cs);
  if (g_tape.on) {
    LARGE_INTEGER a; QueryPerformanceCounter(&a);
    uint32_t n = (uint32_t)strlen(text) + 1;
    g_tape.head(HBTAPE_REC_MARK, n, (uint64_t)(a.QuadPart - g_tape.t0.QuadPart), 0); g_tape.put(text, n); g_tape.pad();
  }
  LeaveCriticalSection(&g_tape.cs);
}
static void tape_finish() {
  if (!g_tape.on) return;
  /* process exit: other threads are gone and may have died holding the lock; a tape without END is still valid */
  if (!TryEnterCriticalSection(&g_tape.cs)) return;
  if (g_tape.on) {
    LARGE_INTEGER a; QueryPerformanceCounter(&a);
    g_tape.head(HBTAPE_REC_END, 0, (uint64_t)(a.QuadPart - g_tape.t0.QuadPart), 0); g_tape.drain();
    hbdx9_log("tape: end, frame %u, %llu bytes, %llu records, %u blobs %llu MB, repeats not written %llu MB: %s", g_tape.frames,
              (unsigned long long)g_tape.written, (unsigned long long)g_tape.records, (unsigned)g_tape.blobs.size(),
              (unsigned long long)(g_tape.blob_bytes >> 20), (unsigned long long)(g_tape.ref_bytes >> 20), g_tape.path);
    CloseHandle(g_tape.file); g_tape.file = INVALID_HANDLE_VALUE; g_tape.on = 0;
  }
  LeaveCriticalSection(&g_tape.cs);
}
struct hbdx9_pkt_empty { struct hbdx9_packet hdr; };
/* ---------------- frame-rate traces, same definition and format as wined3d (dlls/wined3d/cs.c, wined3d_cs_exec_present) ----
 * WINEDEBUG +fps: "@ approx N.NNfps" every 1.5 s (GetTickCount window); +frametime: "Frame duration N μs." per Present
 * (QueryPerformanceCounter).  So an A/B of HyperBridge DX9 against wined3d-GL reads both arms with one parser and one
 * definition: Present calls per wall time, counted inside the d3d9 layer.  Off unless WINEDEBUG names the channel. */
static int g_trace_fps = -1, g_trace_frametime = 0;
static void trace_channels_init() {
  if (g_trace_fps >= 0) return;
  char v[1024]; DWORD n = GetEnvironmentVariableA("WINEDEBUG", v, sizeof(v));
  int fps = 0, ft = 0;
  if (n && n < sizeof(v)) {
    for (char *tok = strtok(v, ","); tok; tok = strtok(nullptr, ",")) {
      char *sign = strpbrk(tok, "+-"); if (!sign) continue;
      if (sign != tok && strncmp(tok, "trace", (size_t)(sign - tok))) continue; /* only the trace class (or no class) */
      int on = *sign == '+'; const char *name = sign + 1;
      if (!strcmp(name, "fps") || !strcmp(name, "all")) fps = on;
      if (!strcmp(name, "frametime") || !strcmp(name, "all")) ft = on;
    }
  }
  g_trace_frametime = ft; g_trace_fps = fps;
}
/* One recursive lock per device: D3D9 serialises calls of a MULTITHREADED device; the packet buffer, the shadow state
   and the native core (one unix call at a time per device) rely on it.  Uncontended it costs one interlocked op. */
/* (DX9FULL 13.11.2026, curator / NATIVE32 census: ~92k critical-section pairs a frame in Gothic 3, 73 host instructions a pair)
   the device lock is taken only for a device created with D3DCREATE_MULTITHREADED, as DXVK's LockDevice(); HBDX9_DEVICE_LOCK=1
   takes it always (the s8j behaviour), for a game that calls one device from several threads without the flag */
struct Hbdx9Lock {
  CRITICAL_SECTION *cs;
  explicit Hbdx9Lock(CRITICAL_SECTION *c, bool on = true) : cs(on ? c : nullptr) { if (cs) EnterCriticalSection(cs); }
  ~Hbdx9Lock() { if (cs) LeaveCriticalSection(cs); }
};
#define DLOCK Hbdx9Lock dlock_(&this->cs, this->locking)
static const uint32_t kInlineLimit = 64 * 1024;

/* ---------------- uploads across the boundary (session 5, 07.11) ----------------
 * HBDX9_UPLOADS=old restores the session-4 behaviour on both sides (shim and core) for before/after measurements:
 * every unlock uploads at once (> 64 KB: synchronous SUBMIT), READONLY locks included.  New (default):
 *  - MANAGED 2D/cube textures upload lazily: an unlock marks the level dirty (union rect); the dirty levels go to the
 *    core right before the first draw that samples the texture (or GenerateMipSubLevels / PreLoad).  The Gothic 3 world
 *    tape: 108 224 identical re-uploads of one 512x256 texture during loading, none of them drawn in between.
 *  - NOOVERWRITE unlocks of DYNAMIC buffers put a guest pointer into the batched packet stream (no copy, no unix call);
 *    a later non-NOOVERWRITE lock of that buffer flushes first, so the core reads the shadow before the app rewrites it.
 *    The core compares against its copy and writes only the changed runs, in place (D3D9: NOOVERWRITE = no conflict).
 *  - READONLY unlocks upload nothing (D3D9: the application does not write). */
static int uploads_old() {
  static int v = -1;
  if (v < 0) { char b[8]; DWORD n = GetEnvironmentVariableA("HBDX9_UPLOADS", b, sizeof(b)); v = n == 3 && !_stricmp(b, "old"); }
  return v;
}
/* diagnostic switches for bisecting a pixel difference to one mechanism (default: all on): HBDX9_NO_LAZY=1 (MANAGED textures
   upload at unlock), HBDX9_NO_DEFER=1 (NOOVERWRITE unlocks upload at once, as other locks) */
static int env_on(const char *name) { char b[8]; DWORD n = GetEnvironmentVariableA(name, b, sizeof(b)); return n && n < sizeof(b) && b[0] == '1'; }
static int no_lazy() { static int v = -1; if (v < 0) v = env_on("HBDX9_NO_LAZY"); return v; }
static int no_defer() { static int v = -1; if (v < 0) v = env_on("HBDX9_NO_DEFER"); return v; }
/* (s8, 08.11) live Gothic 3 world black on the session-5 path: one key per remaining part, and an audit.
   HBDX9_READONLY_UPLOAD=1: (s8 diagnosis) a READONLY unlock of a MANAGED texture marks its rect dirty like any other unlock - the
   default since the s8 fix (key kept, no effect); HBDX9_READONLY_SKIP=1 = the session-5/6 skip.  HBDX9_UPLOAD_AUDIT=1: at every draw, a bound MANAGED texture level that was written (locked) but never sent to the
   core is counted and its first 24 textures logged; the counters are logged every 300 frames. */
/* HBDX9_NO_FLAGMASK=1: DISCARD/NOOVERWRITE of non-DYNAMIC buffers reach the core unmasked (session 4) */
static int no_flagmask() { static int v = -1; if (v < 0) v = env_on("HBDX9_NO_FLAGMASK"); return v; }
/* ---------------- INT32G (12.11.2026): the wait of a game thread for query results ----------------
 * Gothic 3 reads its occlusion queries back in the frame that issued them: it polls IDirect3DQuery9::GetData until the answer
 * is there (world: 2 wait points per frame; corpus run: 67.8 thousand polls per frame, 99.9 % of them "not ready").
 * HBDX9_QUERY_LOCKFREE=1  polls that change nothing (answer ready; or not ready and already flushed) run without the device
 *                         lock: they read only the query's own fields and the fence page, which the lock never protected (the
 *                         core's completion handler writes it).  Every other case takes the lock as before.
 * HBDX9_EARLY_HANDOFF=1   hand the recorded stream to the core before the game asks.  Scope (HBDX9_EARLY_SCOPE): 2
 *                         (default) the whole frame, but only when the previous frame had a query wait - menus, loading
 *                         and games without such waits hand nothing over; 1 only up to the stream position of the
 *                         previous frame's first wait; 0 always.  Inside the scope: SUBMIT at a draw once HBDX9_EARLY_KB
 *                         (default 64) KB are unsent; SUBMIT at the begin of an occlusion query once
 *                         HBDX9_EARLY_QUERY_KB (default 4) KB are unsent; a commit (FLUSH packet) right before the first
 *                         draw/clear on another render target 0 - where the core ends its render pass anyway
 *                         (hb_begin_pass) - once HBDX9_EARLY_DRAWS (default 24) draws were recorded since the last
 *                         commit.  0 switches a part off.  A SUBMIT costs the calling thread about 5 us inside the
 *                         unix call (world tape: about 17 more per frame at scope 2, about 4 at scope 1).  The packets and their order do not change.  Nothing goes early while the unsent
 *                         stream holds a deferred NOOVERWRITE range: the core reads that guest memory when it runs the
 *                         SUBMIT, and Gothic writes into it after Unlock - so every byte the core reads is read at the
 *                         same point as before.
 * Both default off: the stream, the SUBMIT boundaries and the locking are then those of shim 8184fce0. */
static unsigned env_num(const char *name, unsigned def) {
  char b[16]; DWORD n = GetEnvironmentVariableA(name, b, sizeof(b));
  return n && n < sizeof(b) ? (unsigned)strtoul(b, nullptr, 10) : def;
}
static int query_lockfree() { static int v = -1; if (v < 0) v = env_on("HBDX9_QUERY_LOCKFREE"); return v; }
static int submit_nocopy() { static int v = -1; if (v < 0) v = env_on("HBDX9_SUBMIT_NOCOPY"); return v; }
static int ring_on() { static int v = -1; if (v < 0) v = env_on("HBDX9_RING"); return v; }
static int early_on() { static int v = -1; if (v < 0) v = env_on("HBDX9_EARLY_HANDOFF"); return v; }
static unsigned early_kb() { static int v = -1; if (v < 0) v = (int)env_num("HBDX9_EARLY_KB", 64); return (unsigned)v; }
static unsigned early_query_kb() { static int v = -1; if (v < 0) v = (int)env_num("HBDX9_EARLY_QUERY_KB", 4); return (unsigned)v; }
static unsigned early_scope() { static int v = -1; if (v < 0) v = (int)env_num("HBDX9_EARLY_SCOPE", 2); return (unsigned)v; }
static unsigned early_draws_min() { static int v = -1; if (v < 0) v = (int)env_num("HBDX9_EARLY_DRAWS", 24); return (unsigned)v; }
/* HBDX9_QUERY_PREDICT (INT32G, 12.11.2026).  Gothic 3 tests bounding-box sets against its depth pre-pass and polls each result in
 * the frame that issued it.  It hands its ~48 query objects out in a different order every frame, so "the previous result of
 * this query object" belongs to another scene object; the previous result of the same BOX SET is what repeats (world tape:
 * the same visible/hidden state in 97 % of consecutive frames, the count within 0.2 % at the median).
 * =1: an unfinished occlusion query whose box set was VISIBLE at its previous finished test answers S_OK with that count at
 *     once.  Hidden or unknown box sets, and queries without a key (no draw, several draws, a non-indexed draw), wait exactly
 *     as before (so do overlapping queries: no key).  The game is never told "hidden" early: it can only draw an object the exact answer would have culled - an
 *     object that fails the depth test everywhere - never skip one.  The same issue keeps its early answer until it is issued
 *     again.  The key: vertex and index buffer objects with their write generations (a writing Lock makes a new key, so
 *     per-frame box buffers never predict), stream 0 offset and stride, the draw's ranges, the world transform.
 * =2: reference for measurements, never for a profile: every unfinished occlusion query answers visible (count 1 if unknown). */
static int query_predict() { static int v = -1; if (v < 0) v = (int)env_num("HBDX9_QUERY_PREDICT", 0); return v; }
/* (s8 fix, 08.11) READONLY no longer skips a MANAGED texture's upload.  Live Gothic 3 (ERC33 DR1-DR4): 113 919 of its 114 827
   texture unlocks are READONLY and its DXT textures are filled ONLY under READONLY locks; the session-5 skip left 121-130 bound
   textures never sent (black world), HBDX9_READONLY_UPLOAD=1 alone made it lit.  D3D9: LockRect adds a dirty region unless
   D3DLOCK_NO_DIRTY_UPDATE - READONLY is no exception.  The unlock marks the rect dirty like any other; the lazy path folds the
   repeats (live: 1 947 uploads for 114 827 unlocks).  HBDX9_READONLY_SKIP=1 restores the session-5/6 skip (diagnostic). */
static int readonly_skip() { static int v = -1; if (v < 0) v = env_on("HBDX9_READONLY_SKIP"); return v; }
static int upload_audit() { static int v = -1; if (v < 0) v = env_on("HBDX9_UPLOAD_AUDIT"); return v; }
/* (s8 audit) fast 64-bit content hash: 8-byte words, FNV-style mixing; tail bytes one by one */
static uint64_t audit_hash(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t *b = (const uint8_t *)p; size_t i = 0;
  for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 1099511628211ull; h ^= h >> 29; }
  for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
/* HBDX9_ADD_DIRTY=1: AddDirtyRect of a MANAGED texture marks it dirty (D3D9); default: counted only (s6 behaviour, no-op) */
static int add_dirty_on() { static int v = -1; if (v < 0) v = env_on("HBDX9_ADD_DIRTY"); return v; }
struct UploadStats {
  uint64_t inline_bytes = 0, inline_n = 0;             /* copied into packets by the shim (translated memcpy) */
  uint64_t pointer_bytes = 0, pointer_n = 0;           /* referenced by guest pointer with an immediate SUBMIT */
  uint64_t deferred_bytes = 0, deferred_n = 0;         /* referenced by guest pointer inside the batched stream */
  uint64_t sync_submits = 0, ref_flushes = 0;          /* SUBMITs forced by large uploads / by a relock of a referenced buffer */
  uint64_t tex_unlocks = 0, tex_marked = 0, tex_lazy_uploads = 0, readonly_skips = 0;
  uint64_t tex_readonly = 0, tex_readonly_marked = 0, tex_add_dirty = 0;   /* (s8) READONLY texture unlocks; AddDirtyRect calls */
  uint64_t audit_draws = 0, audit_unsent_draws = 0, audit_unsent_textures = 0; /* (s8) HBDX9_UPLOAD_AUDIT */
  uint64_t audit_dirty_draws = 0, audit_dirty_textures = 0;   /* (s8) bound + dirty AFTER pre_draw: the dirty_bound count missed it */
  uint64_t tex_unlocks_other_thread = 0;                      /* (s8) texture unlocks on a thread other than the last Present's */
  uint64_t tex_readonly_written = 0;                          /* (s8) READONLY locks whose rect changed between Lock and Unlock */
  uint64_t noov_written_after_unlock = 0, noov_written_bytes = 0, noov_checked = 0; /* (s8) deferred NOOVERWRITE ranges changed
                                                                 between Unlock and the SUBMIT that hands them to the core */
  DWORD present_thread = 0;
  uint64_t seq_waits = 0, seq_wait_ticks = 0;          /* (s6) waits for the core to consume pointer-referenced guest memory */
};

/* ---------------- formats (sizes only; the native side owns the Metal mapping) ---------------- */
/* (DX9FULL 13.11.2026) HBDX9_FMT_EXT (read once; default ON, "0" = the s8j+ATI2 answers): DXVK's CheckDeviceFormat rules, the
   vendor depth textures DF16/DF24, X4R4G4B4, Q16W16V16U16, timestamp queries and the D3DCAPS9 bits DXVK reports that this layer
   implements.  The core reads the same variable (Fetch4, NVDB, ATOC, BOTH*ALPHA blend, border colour). */
/* (DX9FULL v13) HBDX9_SB_JOURNAL=0: state blocks compare everything they hold at every Apply / Capture (v12); HBDX9_SB_VERIFY=1:
   after every Apply / Capture the block is compared with the device and a difference is logged ("sb-verify ... MISMATCH") */
static int sb_journal_on() {
  static int v = -1;
  if (v < 0) { char b[8]; DWORD n = GetEnvironmentVariableA("HBDX9_SB_JOURNAL", b, sizeof(b)); v = !(n && n < sizeof(b) && b[0] == '0'); }
  return v;
}
static int sb_verify() { static int v = -1; if (v < 0) v = env_on("HBDX9_SB_VERIFY"); return v; }
/* (DX9FULL v13) the x87 control word of the CALLING thread, as the real d3d9.dll does it - MEASURED on Windows Server 2022 and 2025
   (GitHub-hosted runners, probe stands/d3d9-fpu of the public hyperbridge repo, artifacts/claude-hb-work/curator/win64-reference-20261113):
     CreateDevice / CreateDeviceEx without D3DCREATE_FPU_PRESERVE, and Reset of such a device: the word becomes 0x007f from every start
     value measured (0x027f, 0x0f7f, 0x083f) - all exceptions masked, 24-bit precision, round to nearest, infinity control set;
     ResetEx, Present, other threads and MXCSR: untouched.
   DXVK and Wine do cw = (cw & 0xF0C0) | 0x003F in creation only: that differs from Windows in two measured ways - from a start word
   with bit 6 clear (0x083f) they leave 0x003f where Windows gives 0x007f, and they do not re-apply it on Reset.  We follow the
   measurement.  HBDX9_FPU_SETUP=0 (or HBDX9_FMT_EXT=0): leave the word alone (the s8j behaviour). */
static int fmt_ext_on();
static int fpu_setup_on() {
  static int v = -1;
  if (v < 0) { char b[8]; DWORD n = GetEnvironmentVariableA("HBDX9_FPU_SETUP", b, sizeof(b)); v = !(n && n < sizeof(b) && b[0] == '0'); }
  return v;
}
/* 32-bit only: Wine's d3d9 tests (device.c test_fpu_setup, run on Windows) expect 0xf60 -> 0x7f on 32-bit and the word left alone on
   64-bit Windows */
static void setup_fpu(DWORD flags, const char *what = "CreateDevice") {
#if defined(__i386__)
  if ((flags & D3DCREATE_FPU_PRESERVE) || !fpu_setup_on() || !fmt_ext_on()) return;
  unsigned short cw, nw;
  __asm__ __volatile__("fnstcw %0" : "=m"(cw));
  nw = (unsigned short)((cw & 0xF000) | 0x007F);   /* the measured Windows result; only the unused top nibble is kept */
  __asm__ __volatile__("fldcw %0" : : "m"(nw));
  hbdx9_log("fpu control word %04x -> %04x (%s, thread %lu; D3DCREATE_FPU_PRESERVE not set)", cw, nw, what, (unsigned long)GetCurrentThreadId());
#else
  (void)flags; (void)what;
#endif
}
static int fmt_ext_on() {
  static int v = -1;
  if (v < 0) { char b[8]; DWORD n = GetEnvironmentVariableA("HBDX9_FMT_EXT", b, sizeof(b)); v = !(n && n < sizeof(b) && b[0] == '0'); }
  return v;
}
#ifndef D3DPRESENT_FORCEIMMEDIATE
#define D3DPRESENT_FORCEIMMEDIATE 0x00000100L   /* D3D9Ex (d3d9.h of the toolchain leaves it out) */
#endif
#define HB_FOURCC_DF16 MAKEFOURCC('D', 'F', '1', '6')
#define HB_FOURCC_DF24 MAKEFOURCC('D', 'F', '2', '4')
/* (DX9FULL 13.11.2026) DXVK's per-application d3d9 options (src/util/config/config.cpp, 2097f56) for what HBDX9_FMT_EXT added:
   d3d9.supportDFFormats = False for Gothic 3 (Gothic3 / Gothic3Final / Gothic III Forsaken Gods), Borderlands 2, Borderlands:
   The Pre-Sequel, Dead Space, GTA IV / EFLC (these games take another, worse render path when DF16 / DF24 exist);
   d3d9.supportX4R4G4B4 = False and d3d9.disableA8RT = True for The Sims 2 family.  Matched on the executable's file name, case
   ignored, as DXVK's regular expressions.  HBDX9_APP_NAME=<file name> replaces the name (probes only). */
struct AppOpt { bool df = true, x4r4g4b4 = true, a8rt = true; char name[64] = {}; };
static AppOpt g_app_opt;
static BOOL WINAPI app_opt_init(PINIT_ONCE, PVOID, PVOID *) {
    AppOpt &o = g_app_opt;
    char path[MAX_PATH] = {}; DWORD n = GetEnvironmentVariableA("HBDX9_APP_NAME", path, sizeof(path));
    if (!n || n >= sizeof(path)) n = GetModuleFileNameA(nullptr, path, sizeof(path));
    const char *b = path; for (const char *c = path; *c; c++) if (*c == '\\' || *c == '/') b = c + 1;
    char lo[64] = {}; for (int i = 0; i < 63 && b[i]; i++) lo[i] = (char)(b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i]);
    memcpy(o.name, lo, sizeof(lo));
    static const char *no_df[] = {"gothic3.exe", "gothic3final.exe", "gothic iii forsaken gods.exe", "borderlands2.exe",
                                  "borderlandspresequel.exe", "dead space.exe", "gtaiv.exe", "eflc.exe"};
    for (auto *x : no_df) if (!strcmp(lo, x)) o.df = false;
    static const char *sims[] = {"ts2bodyshop.exe", "simsls.exe", "simsps.exe", "simscs.exe", "the sims 2 content manager.exe", "ts2homecrafterplus.exe"};
    bool s2 = !strncmp(lo, "sims2", 5) && strlen(lo) >= 9 && !strcmp(lo + strlen(lo) - 4, ".exe");
    for (auto *x : sims) if (!strcmp(lo, x)) s2 = true;
    if (s2) { o.x4r4g4b4 = false; o.a8rt = false; }
    return TRUE;
}
static const AppOpt &app_opt() {
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(&once, app_opt_init, nullptr, nullptr);
  return g_app_opt;
}
static bool df_on() { return fmt_ext_on() && app_opt().df; }
struct FmtSize { UINT block, bytes; };
static FmtSize fmt_size(D3DFORMAT f) {
  if (fmt_ext_on()) switch ((DWORD)f) {
  case HB_FOURCC_DF16: if (!app_opt().df) break; return {1, 2};
  case D3DFMT_X4R4G4B4: if (!app_opt().x4r4g4b4) break; return {1, 2};
  case HB_FOURCC_DF24: if (!app_opt().df) break; return {1, 4};
  case D3DFMT_Q16W16V16U16: return {1, 8};
  /* converted by the core on upload (guest layout here): bump formats, A4L4, packed 4:2:2 */
  case D3DFMT_L6V5U5: case D3DFMT_UYVY: case D3DFMT_YUY2: case D3DFMT_R8G8_B8G8: case D3DFMT_G8R8_G8B8: return {1, 2};
  case D3DFMT_X8L8V8U8: case D3DFMT_A2W10V10U10: return {1, 4};
  case D3DFMT_A4L4: return {1, 1};
  default: break;
  }
  switch ((DWORD)f) {
  case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2B10G10R10:
  case D3DFMT_A2R10G10B10: case D3DFMT_G16R16: case D3DFMT_G16R16F: case D3DFMT_R32F: case D3DFMT_D24S8: case D3DFMT_D24X8:
  case D3DFMT_D32: case D3DFMT_D24FS8: case D3DFMT_D32F_LOCKABLE: return {1, 4};
  case MAKEFOURCC('I', 'N', 'T', 'Z'): case MAKEFOURCC('N', 'U', 'L', 'L'): return {1, 4};
  case D3DFMT_V16U16: case D3DFMT_Q8W8V8U8: return {1, 4};
  case D3DFMT_V8U8:
  case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: case D3DFMT_L16:
  case D3DFMT_R16F: case D3DFMT_D16: return {1, 2};
  case D3DFMT_A8: case D3DFMT_L8: return {1, 1};
  case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: return {1, 8};
  case D3DFMT_A32B32G32R32F: return {1, 16};
  case D3DFMT_DXT1: return {4, 8};
  case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return {4, 16};
  /* (CRYSIS 12.11.2026) 3Dc: ATI1 = one BC4 block (8 bytes), ATI2 = two (16 bytes); the core maps them to BC4 / BC5 with the
     channel order D3D9 applications expect.  Crysis creates its ATI2 normal maps without asking CheckDeviceFormat first. */
  case MAKEFOURCC('A', 'T', 'I', '1'): return {4, 8};
  case MAKEFOURCC('A', 'T', 'I', '2'): return {4, 16};
  default: return {0, 0};
  }
}
/* (DX9FULL) formats the core converts on upload: sampled only, never a render target, a lock does not read the content back */
static bool is_converted(D3DFORMAT f) {
  switch ((DWORD)f) {
  case D3DFMT_L6V5U5: case D3DFMT_X8L8V8U8: case D3DFMT_A2W10V10U10: case D3DFMT_A4L4: case D3DFMT_UYVY: case D3DFMT_YUY2:
  case D3DFMT_R8G8_B8G8: case D3DFMT_G8R8_G8B8: return fmt_ext_on() != 0;
  default: return false;
  }
}
static bool is_depth(D3DFORMAT f) {
  if (df_on() && ((DWORD)f == HB_FOURCC_DF16 || (DWORD)f == HB_FOURCC_DF24)) return true;
  switch ((DWORD)f) { case D3DFMT_D16: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32: case D3DFMT_D24FS8: case D3DFMT_D32F_LOCKABLE:
                     case MAKEFOURCC('I', 'N', 'T', 'Z'): return true; default: return false; }
}
/* (CRYSIS 12.11.2026) HBDX9_FMT_TRACE=1 (read once, default off): one line per distinct (call, format, usage, type, result)
   of CheckDeviceFormat and of the texture / surface creations - which formats a game asks for and what it is told. */
static int fmt_trace_on() { static int v = -1; if (v < 0) v = env_on("HBDX9_FMT_TRACE"); return v; }
static void fmt_trace(const char *call, D3DFORMAT f, DWORD usage, DWORD rtype, HRESULT hr) {
  struct Seen { const char *call; DWORD f, usage, rtype; HRESULT hr; };
  static Seen seen[512]; static LONG count; static SRWLOCK lock = SRWLOCK_INIT;
  AcquireSRWLockExclusive(&lock);
  LONG n = count, i;
  for (i = 0; i < n; i++) if (seen[i].call == call && seen[i].f == (DWORD)f && seen[i].usage == usage && seen[i].rtype == rtype && seen[i].hr == hr) break;
  bool fresh = i == n && n < 512;
  if (fresh) { seen[n] = {call, (DWORD)f, usage, rtype, hr}; count = n + 1; }
  ReleaseSRWLockExclusive(&lock);
  if (!fresh) return;
  DWORD v = (DWORD)f; char cc[5] = "----";
  if (v > 0xffff) for (int k = 0; k < 4; k++) { char c = (char)(v >> (8 * k)); cc[k] = c >= 32 && c < 127 ? c : '?'; }
  hbdx9_log("fmt-trace: %s fmt=%lu/%08lx(%s) usage=%08lx type=%lu -> %08lx", call, (unsigned long)v, (unsigned long)v, cc,
            (unsigned long)usage, (unsigned long)rtype, (unsigned long)hr);
}
#define FMT_TRACE(call, f, usage, rtype, hr) do { if (fmt_trace_on()) fmt_trace(call, f, usage, rtype, hr); } while (0)

class Hbdx9Device;

/* ---------------- surfaces ---------------- */
struct TexCore;
class Hbdx9Surface final : public Hbdx9Stub_IDirect3DSurface9<IDirect3DSurface9> {
public:
  LONG refs = 1;
  IDirect3DDevice9 *device;      /* AddRef'd */
  uint64_t native = 0;           /* native resource (render targets, depth, back buffer, texture of a level) */
  UINT level = 0, face = 0;      /* mip level / cube face inside the native resource */
  TexCore *container = nullptr;  /* level of a texture: not owned, the texture owns its surfaces */
  UINT samples = 1;
  BYTE *memory = nullptr;        /* guest memory (SYSTEMMEM/SCRATCH surfaces) */
  bool owns_native = true;       /* false: back buffer / auto depth (swapchain) and texture levels (texture) */
  UINT pitch = 0;
  D3DSURFACE_DESC desc{};
  bool locked = false, gpu_shadow = false;
  DWORD lock_flags = 0;
  uint32_t ptr_seq = 0;          /* (s6) SUBMIT carrying a pointer to `memory`; see Hbdx9Device::wait_seq */

  Hbdx9Surface(IDirect3DDevice9 *dev, const D3DSURFACE_DESC &d) : device(dev), desc(d) { device->AddRef(); }
  ~Hbdx9Surface();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DResource9) || IsEqualGUID(riid, IID_IDirect3DSurface9)) {
      *out = this; AddRef(); return S_OK;
    }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override { if (!out) return D3DERR_INVALIDCALL; *out = device; device->AddRef(); return D3D_OK; }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *d) override { if (!d) return D3DERR_INVALIDCALL; *d = desc; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **out) override;
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override;
  HRESULT STDMETHODCALLTYPE UnlockRect() override;
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
};

/* ---------------- state block masks ---------------- */
struct SBMask {
  uint8_t rs[256], ss[20][14], tss[8][33], tex[20], stream[16], freq[16];
  uint8_t ib, vs, ps, decl, viewport, scissor, material;
  uint8_t vsf[256], psf[256], vsi[16], psi[16], vsb[16], psb[16];
  uint8_t transform[512], clip[6];
};
class Hbdx9StateBlock;
class Hbdx9SwapChain;
class Hbdx9Device;
/* live devices, for the counters at process exit (no lock needed beyond the loader lock at detach; creation is rare) */
static Hbdx9Device *g_devices[16];
static void register_device(Hbdx9Device *d) { for (auto &x : g_devices) if (!x) { x = d; return; } }
static void unregister_device(Hbdx9Device *d) { for (auto &x : g_devices) if (x == d) x = nullptr; }

/* ---------------- device ---------------- */
class Hbdx9Device final : public Hbdx9Stub_IDirect3DDevice9Ex<IDirect3DDevice9Ex> {
public:
  LONG refs = 1;
  IDirect3D9Ex *parent;
  uint64_t native_device;
  bool ex;
  D3DDEVICE_CREATION_PARAMETERS creation{};
  D3DPRESENT_PARAMETERS pp{};
  uint64_t native_swapchain = 0;
  Hbdx9Surface *backbuffer = nullptr, *auto_depth = nullptr;
  /* (DX9FULL 13.11.2026, curator / NATIVE32 census: 7k vector::insert + memset calls a frame) the packet buffer grows without
     zero-filling (allocator with default-init construct); add<> zeroes the header struct and the 8-byte tail itself */
  template <typename U> struct NoInitAlloc : std::allocator<U> {
    template <typename V> struct rebind { using other = NoInitAlloc<V>; };
    NoInitAlloc() = default;
    template <typename V> NoInitAlloc(const NoInitAlloc<V> &) {}
    template <typename V> void construct(V *p) { ::new ((void *)p) V; }
    template <typename V, typename... A> void construct(V *p, A &&...a) { ::new ((void *)p) V(std::forward<A>(a)...); }
  };
  using PktBuf = std::vector<uint8_t, NoInitAlloc<uint8_t>>;
  PktBuf packets;
  unsigned frames = 0, submits = 0;
  /* (INT32G fix, patch 0006) `submits` is the SUBMIT sequence of the NATIVE device (the number the core publishes when it has
     run a SUBMIT), not a count of this object: it starts at seq_base, the core's count when this device was created */
  uint32_t seq_base = 0;
  /* (INT32G patch 0005) HBDX9_SUBMIT_NOCOPY / HBDX9_RING: the core runs the packets where they are, so handed-over bytes are
     not touched until their SUBMIT sequence is published.  transport: -1 not chosen yet, 0 the core copies (as before), 1 no
     copy (whole buffers rotate), 2 the chunk ring (`sent` bytes of `packets` are published, the stream goes on behind them) */
  struct Flight { PktBuf buf; uint32_t seq; };
  std::deque<Flight> inflight; std::vector<PktBuf> spare;
  size_t sent = 0;
  size_t unsent() const { return packets.size() - sent; }
  uint32_t core_caps = 0, pkt_presents = 0, ring_presents = 0;
  int transport = -1;
  hbdx9_ring *ring = nullptr;
  uint64_t tr_nocopy = 0, tr_ring = 0, tr_relaxed = 0, tr_wakes = 0, tr_full = 0, tr_pacing = 0, tr_reused = 0, tr_fresh = 0, tr_rewind = 0, tr_moved = 0;
  UploadStats up;
  std::vector<uint32_t> frame_call_us; uint64_t last_call_ticks = 0;   /* (s6) per frame: µs inside unix calls */
  /* (s6) sequence of the SUBMIT that last carried a pointer to guest memory: the core publishes the sequence it has run in
     fence_page[1023]; until then that memory must not change (asynchronous core) */
  bool seq_done(uint32_t seq) const { return !seq || !fence_page || (int32_t)(fence_page[1023] - seq) >= 0; }
  void wait_seq(uint32_t seq) {
    if (seq_done(seq)) return;
    if ((int32_t)(seq - submits) > 0) flush();          /* still in the unsent stream: send it */
    if (transport == 2 && ring->sleeping && InterlockedExchange((volatile LONG *)&ring->sleeping, 0)) { tr_wakes++; ring_call(HBDX9_SUBMIT_RING_WAKE, 0); }
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    for (uint32_t spins = 0; !seq_done(seq); spins++) { if (spins < 200) YieldProcessor(); else Sleep(0); }
    QueryPerformanceCounter(&b); up.seq_waits++; up.seq_wait_ticks += (uint64_t)(b.QuadPart - a.QuadPart);
  }
  uint32_t dirty_bound = 0;        /* bound textures with lazy (not yet uploaded) levels: draws check this one number */
  struct DeferAudit { const void *p; uint32_t size; uint64_t hash; uint64_t resource; uint32_t offset, flags; };
  int tape_app_scope = 0;                /* (s8, tape v3) >0 while an application write is being uploaded: its UPDATE packets are tagged */
  std::vector<DeferAudit> defer_audit;   /* (s8) HBDX9_UPLOAD_AUDIT: deferred NOOVERWRITE ranges of the unsent stream */

  /* shadow state (getters only; the native side tracks the state that draws use) */
  DWORD rs[256] = {};
  DWORD ss[20][14] = {};
  DWORD tss[8][33] = {};
  IDirect3DBaseTexture9 *tex[20] = {};
  struct { IDirect3DVertexBuffer9 *vb; UINT offset, stride, freq; } streams[16] = {};
  IDirect3DIndexBuffer9 *ib = nullptr;
  IDirect3DVertexShader9 *vs = nullptr;
  IDirect3DPixelShader9 *ps = nullptr;
  IDirect3DVertexDeclaration9 *decl = nullptr;
  DWORD fvf = 0;
  float vsf[256][4] = {}, psf[256][4] = {};
  int vsi[16][4] = {}, psi[16][4] = {};
  BOOL vsb[16] = {}, psb[16] = {};
  D3DVIEWPORT9 viewport{};
  RECT scissor{};
  D3DMATRIX transforms[512];
  Hbdx9Surface *rt[4] = {};
  Hbdx9Surface *ds = nullptr;
  D3DMATERIAL9 material{};
  float clip_planes[6][4] = {};
  BOOL sw_vp = FALSE;
  Hbdx9FsState fs;   /* (FULLSCREEN) display mode / window placement of a full-screen device */
  DWORD create_flags = 0;   /* (DX9FULL v13) for Reset: the real d3d9 applies its FPU setup again (measured), ResetEx does not */
  /* (DX9FULL v13, curator: Apply of a D3DSBT_ALL block 35.6k host instructions -> ~10k and less) the state-block change journal.
     Every write of a state a block can hold is noted here, at the one place it happens: the journal position of each category's
     last note and a ring of the last SB_JCAP notes (category, index, count).  A block keeps the journal position of the moment it
     last equalled the device (end of its Capture / Apply).  Its next Apply / Capture then touches only what changed since: nothing
     (same position), the noted entries (the ring still holds them and walking them is cheaper), or the categories noted since
     (compared as before).  Notes are taken only while a block that uses them exists (sb_track: D3DSBT_* blocks and large
     recorded ones; a small recorded block is compared whole, as in v12, so a game without large blocks pays one test a Set*).
     HBDX9_SB_JOURNAL=0: no block uses the journal (v12). */
  enum { SB_RS, SB_SS, SB_TSS, SB_TEX, SB_STREAM, SB_FREQ, SB_IB, SB_VS, SB_PS, SB_DECL, SB_VSF, SB_PSF, SB_VSI, SB_PSI, SB_VSB, SB_PSB,
         SB_VIEWPORT, SB_SCISSOR, SB_MATERIAL, SB_CLIP, SB_TR, SB_LIGHT,
         /* last-note groups only (no note of these kinds in the ring): the world matrices 256..511 in 8 groups of 32, so one
            SetTransform(WORLD) does not make a block compare all 266 matrices it holds (the largest category: ~80 host
            instructions a matrix); SB_TR's own group is matrices 0..255 (VIEW, PROJECTION, TEXTURE0..7) */
         SB_TRW0, SB_TRW7 = SB_TRW0 + 7, SB_NCAT };
  static constexpr uint32_t SB_TR_GROUPS = 1u << SB_TR | 0xffu << SB_TRW0;
  static unsigned sb_tr_group(unsigned s) { return s < 256 ? (unsigned)SB_TR : SB_TRW0 + ((s - 256) >> 5); }
  static constexpr uint32_t SB_JCAP = 1024;
  /* a note: count << 16 | category << 10 | index (rs, sampler * 14 + state, stage * 33 + state, register, transform...) */
  uint32_t sb_last[SB_NCAT] = {};   /* position of the category's last note; 0 = never (positions start at 1) */
  uint32_t sb_seq = 1;
  uint32_t sb_track = 0;            /* blocks that use the journal; 0: no notes are taken */
  uint32_t sb_jr[SB_JCAP];
  __attribute__((always_inline)) void sb_note(unsigned c, unsigned i, unsigned n = 1) {
    if (!sb_track) return;
    uint32_t q = sb_seq; sb_last[c] = q; sb_jr[q & (SB_JCAP - 1)] = n << 16 | c << 10 | i; sb_seq = q + 1;
  }
  __attribute__((always_inline)) void sb_note_tr(unsigned s) {
    if (!sb_track) return;
    uint32_t q = sb_seq; sb_last[sb_tr_group(s)] = q; sb_jr[q & (SB_JCAP - 1)] = 1u << 16 | SB_TR << 10 | s; sb_seq = q + 1;
  }
  /* Reset: every category changed and no earlier position can be walked */
  void sb_note_all() { for (auto &g : sb_last) g = sb_seq; sb_seq += 2 * SB_JCAP; }
  uint64_t sb_n[10] = {};   /* apply: skipped, walked, compared; capture: the same; walked notes; verify checks, mismatches; - */
  /* BeginStateBlock..EndStateBlock: Set* calls are recorded, not applied (D3D9 semantics) */
  Hbdx9StateBlock *recording = nullptr;
  SBMask *rec = nullptr;
  /* lights: every SetLight index, the enabled ones (<= 8, by index) go to the core */
  struct LightRec { DWORD index; D3DLIGHT9 light; bool enabled; };
  std::vector<LightRec> lights;
  /* (DX9FULL, HBDX9_FMT_EXT) light indices touched between BeginStateBlock and EndStateBlock (SetLight / LightEnable): state
     blocks hold lights as D3D9 and DXVK do */
  std::vector<DWORD> rec_lt_set, rec_lt_en;
  /* fences / queries: the core writes values into this guest page when GPU work completes */
  volatile uint32_t *fence_page = nullptr;
  uint32_t fence_value = 0;
  std::vector<uint32_t> free_slots;
  std::vector<uint32_t> free_slots2;      /* (INT32G fix) slot pairs of released occlusion queries */
  std::vector<uint32_t> free_slots3;      /* (DX9FULL) slot triples of released timestamp queries */
  uint32_t next_slot = 0;
  Hbdx9SwapChain *swapchain_obj = nullptr;
  UINT max_samples = 1;
  /* (INT32G) HBDX9_EARLY_HANDOFF */
  uint32_t early_draws = 0;                                    /* draws recorded since the last commit (FLUSH packet or PRESENT) */
  bool early_blocked = false;                                  /* the unsent stream holds a deferred NOOVERWRITE range */
  uint64_t early_rt_native = 0; uint32_t early_rt_level = 0;   /* render target 0 of the previous draw/clear */
  uint64_t early_n_size = 0, early_n_query = 0, early_n_commit = 0, early_n_blocked = 0;
  /* (INT32G) HBDX9_QUERY_PREDICT: the last finished test of each box set.  1024 entries, 256 sets of 4, the oldest of a set
     is replaced.  An entry is PENDING while its newest issue (slot, value) is not harvested; known/count describe the finished
     test before that issue. */
  struct QKey { uint64_t vb, ib; uint32_t vb_gen, ib_gen, voff, stride, prim, min_index, nverts, start_index, prims, world; int32_t base; uint32_t pad; };
  struct QPred { QKey key; uint32_t count, slot, value, stamp; uint8_t used, known, pending, early; };
  QPred *qpred = nullptr; uint32_t qstamp = 0;
  bool q_open = false; uint8_t q_key_state = 0; QKey q_key;      /* the query being issued: 0 no draw yet, 1 one indexed draw, 2 no key */
  const void *q_owner = nullptr; bool q_overlap = false;         /* its object; another query began while it was open: no key for either */
  uint32_t q_flush_submits = ~0u;                                 /* `submits` right after the last FLUSH packet went out */
  uint64_t qp_early = 0, qp_hidden = 0, qp_unknown = 0, qp_nokey = 0, qp_over = 0, qp_exact = 0, qp_flush_saved = 0;
  static uint32_t qp_fnv(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
  }
  QPred *qp_entry(const QKey &k, bool insert) {
    if (!qpred) {
      if (!insert) return nullptr;
      qpred = (QPred *)VirtualAlloc(nullptr, 1024 * sizeof(QPred), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (!qpred) return nullptr;
    }
    QPred *set = qpred + (size_t)(qp_fnv(&k, sizeof(k)) & 255) * 4;
    for (int i = 0; i < 4; i++) if (set[i].used && !memcmp(&set[i].key, &k, sizeof(k))) return set + i;
    if (!insert) return nullptr;
    QPred *v = set;
    for (int i = 0; i < 4; i++) {
      if (!set[i].used) { v = set + i; break; }
      if ((int32_t)(set[i].stamp - v->stamp) < 0) v = set + i;
    }
    memset(v, 0, sizeof(*v)); v->key = k; v->used = 1;
    return v;
  }
  /* the exact result of the entry's newest issue: taken if it is there, else nothing is known about this box set */
  void qp_harvest(QPred *e) {
    if (!e || !e->pending) return;
    if (__atomic_load_n((const uint32_t *)&fence_page[e->slot + 1], __ATOMIC_ACQUIRE) == e->value) {
      e->count = fence_page[e->slot]; e->known = 1;
      if (e->early && !e->count) qp_over++;        /* told "visible", was hidden: one object drawn without need */
    } else e->known = 0;
    e->pending = 0; e->early = 0;
  }
  QPred *qp_issue_entry(const QKey &k, uint32_t slot, uint32_t value) {
    QPred *e = qp_entry(k, false);
    return e && e->pending && e->slot == slot && e->value == value ? e : nullptr;
  }
  /* the count to answer at once for an unfinished issue, 0 = wait as before */
  uint32_t qp_predict(const QKey *k, uint32_t slot, uint32_t value) {
    QPred *e = k ? qp_issue_entry(*k, slot, value) : nullptr;
    uint32_t c = e && e->known ? e->count : 0;
    if (query_predict() == 2) { if (e) e->early = 1; qp_early++; return c ? c : 1; }
    if (!k) { qp_nokey++; return 0; }
    if (!e || !e->known) { qp_unknown++; return 0; }
    if (!c) { qp_hidden++; return 0; }
    e->early = 1; qp_early++;
    return c;
  }
  /* the FLUSH of the first unfinished poll, unless nothing was recorded since the last one went out */
  void qp_flush() {
    early_note_wait();
    if (!unsent() && submits == q_flush_submits) { qp_flush_saved++; return; }
    add<hbdx9_pkt_empty>(HBDX9_PKT_FLUSH); flush(); q_flush_submits = submits; early_draws = 0;
  }
  void qp_note_indexed(D3DPRIMITIVETYPE pt, INT base, UINT min_index, UINT nverts, UINT start_index, UINT prims);
  uint32_t early_sent = 0;                                     /* bytes of packets handed to the core in this frame */
  uint32_t early_wait_at = 0, early_wait_prev = 0;             /* stream position of the first query wait: this frame, the previous one */

  CRITICAL_SECTION cs;
  bool locking = true;   /* (DX9FULL) false for a device created without D3DCREATE_MULTITHREADED (set in init), see Hbdx9Lock */
  Hbdx9Device(IDirect3D9Ex *p, uint64_t dev, bool is_ex, volatile uint32_t *page, UINT ms)
      : parent(p), native_device(dev), ex(is_ex), fence_page(page), max_samples(ms) {
    InitializeCriticalSection(&cs); parent->AddRef(); reset_shadow(); register_device(this);
  }
  uint32_t alloc_slot(uint32_t n) {
    if (n == 1 && !free_slots.empty()) { uint32_t v = free_slots.back(); free_slots.pop_back(); return v; }
    if (n == 2 && !free_slots2.empty()) { uint32_t v = free_slots2.back(); free_slots2.pop_back(); return v; }
    if (next_slot + n > 1023) return ~0u;   /* slot 1023: the core's run-SUBMIT sequence (s6) */
    uint32_t v = next_slot; next_slot += n; return v;
  }
  ~Hbdx9Device() {
    if (hw_cursor) { SetCursor(nullptr); DestroyCursor(hw_cursor); hw_cursor = nullptr; }   /* (DX9FULL) */
    flush();
    report();
    unregister_device(this);
    teardown();
  }
  /* counters, once per device: at destruction, or at process exit for a device nobody released (07.11 s5: the world-tape
     replay ended with references left, the destructor never ran and the counters were never printed) */
  bool reported = false;
  void report() {
    if (reported) return;
    reported = true;
    { hbdx9_stats_params st{}; st.device = native_device; hb_call(HBDX9_CALL_STATS, &st);
      hbdx9_log("stats variants_compiled=%u variants_cached=%u cache_writes=%u psos=%u pso_archive_hits=%u pso_archive_misses=%u draws=%u frames=%u submits=%u",
                st.variants_compiled, st.variants_cached, st.cache_rows_written, st.psos_created, st.pad[0], st.pad[1], st.draws, frames, submits - seq_base);
      hbdx9_log("boundary mode=%s submits=%u sync_upload_submits=%llu ref_flushes=%llu inline=%llu/%llu pointer=%llu/%llu deferred=%llu/%llu "
                "tex_unlocks=%llu tex_marked_lazy=%llu tex_lazy_uploads=%llu readonly_skips=%llu tex_readonly=%llu tex_readonly_marked=%llu "
                "tex_add_dirty=%llu audit_draws=%llu audit_unsent_draws=%llu audit_unsent_textures=%llu (n/bytes)",
                uploads_old() ? "old" : "new", submits - seq_base, (unsigned long long)up.sync_submits, (unsigned long long)up.ref_flushes,
                (unsigned long long)up.inline_n, (unsigned long long)up.inline_bytes, (unsigned long long)up.pointer_n,
                (unsigned long long)up.pointer_bytes, (unsigned long long)up.deferred_n, (unsigned long long)up.deferred_bytes,
                (unsigned long long)up.tex_unlocks, (unsigned long long)up.tex_marked, (unsigned long long)up.tex_lazy_uploads,
                (unsigned long long)up.readonly_skips, (unsigned long long)up.tex_readonly, (unsigned long long)up.tex_readonly_marked,
                (unsigned long long)up.tex_add_dirty, (unsigned long long)up.audit_draws, (unsigned long long)up.audit_unsent_draws,
                (unsigned long long)up.audit_unsent_textures);
      if (sb_n[0] + sb_n[1] + sb_n[2] + sb_n[3] + sb_n[4] + sb_n[5])   /* (DX9FULL v13) */
        hbdx9_log("state-blocks journal=%d apply skip/walk/compare=%llu/%llu/%llu capture skip/walk/compare=%llu/%llu/%llu walked_notes=%llu "
                  "verify=%llu mismatches=%llu", sb_journal_on(), (unsigned long long)sb_n[0], (unsigned long long)sb_n[1], (unsigned long long)sb_n[2],
                  (unsigned long long)sb_n[3], (unsigned long long)sb_n[4], (unsigned long long)sb_n[5], (unsigned long long)sb_n[6],
                  (unsigned long long)sb_n[7], (unsigned long long)sb_n[8]);
      if (query_lockfree() || early_on() || query_predict())
        hbdx9_log("int32g query_lockfree=%d early_handoff=%d scope=%u kb=%u query_kb=%u draws=%u early_submits_size=%llu early_submits_query=%llu "
                  "early_commits=%llu early_blocked=%llu", query_lockfree(), early_on(), early_scope(), early_kb(), early_query_kb(), early_draws_min(),
                  (unsigned long long)early_n_size, (unsigned long long)early_n_query, (unsigned long long)early_n_commit,
                  (unsigned long long)early_n_blocked);
      if (query_lockfree() || early_on() || query_predict()) {
        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
        hbdx9_log("int32g submits=%u in_unix_calls_ms=%llu (all unix calls of the process, calling threads)", submits - seq_base,
                  (unsigned long long)(g_in_call_ticks * 1000 / (uint64_t)qf.QuadPart));
      }
      if (transport > 0)
        hbdx9_log("int32g transport=%d nocopy_submits=%llu ring_chunks=%llu (early %llu) ring_wakes=%llu ring_full_waits=%llu ring_pacing_waits=%llu "
                  "buffers: reused=%llu fresh=%llu rewinds=%llu moved=%llu in_flight=%u spare=%u capacity=%u", transport, (unsigned long long)tr_nocopy,
                  (unsigned long long)tr_ring, (unsigned long long)tr_relaxed, (unsigned long long)tr_wakes, (unsigned long long)tr_full,
                  (unsigned long long)tr_pacing, (unsigned long long)tr_reused, (unsigned long long)tr_fresh, (unsigned long long)tr_rewind,
                  (unsigned long long)tr_moved, (unsigned)inflight.size(), (unsigned)spare.size(), (unsigned)packets.capacity());
      if (query_predict())
        hbdx9_log("int32g query_predict=%d answered_early=%llu waited_hidden=%llu waited_unknown=%llu waited_nokey=%llu early_but_hidden=%llu "
                  "exact_reads=%llu flushes_saved=%llu", query_predict(), (unsigned long long)qp_early, (unsigned long long)qp_hidden,
                  (unsigned long long)qp_unknown, (unsigned long long)qp_nokey, (unsigned long long)qp_over, (unsigned long long)qp_exact,
                  (unsigned long long)qp_flush_saved);
      std::vector<uint32_t> v(frame_call_us); std::sort(v.begin(), v.end());
      uint64_t sum = 0; for (auto x : v) sum += x;
      LARGE_INTEGER f; QueryPerformanceFrequency(&f);
      auto q = [&](double p) { return v.empty() ? 0u : v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
      hbdx9_log("in-call per frame (us, calling thread inside unix calls): frames=%u median=%u p90=%u p99=%u mean=%llu total_ms=%llu; seq_waits=%llu wait_ms=%llu",
                (unsigned)v.size(), q(0.5), q(0.9), q(0.99), (unsigned long long)(v.empty() ? 0 : sum / v.size()), (unsigned long long)(sum / 1000),
                (unsigned long long)up.seq_waits, (unsigned long long)(up.seq_wait_ticks * 1000 / (uint64_t)f.QuadPart)); }
  }
  void teardown() {
    hbdx9_fs_leave(fs);   /* (FULLSCREEN) */
    for (auto &t : tex) if (t) t->Release();
    for (auto &s : streams) if (s.vb) s.vb->Release();
    if (ib) ib->Release();
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (decl) decl->Release();
    for (auto &r : rt) if (r) r->Release();
    if (ds) ds->Release();
    if (auto_depth) auto_depth->Release();
    if (backbuffer) backbuffer->Release();
    if (native_swapchain) { hbdx9_release_params r{native_swapchain, 1, 0}; hb_call(HBDX9_CALL_RELEASE, &r); }
    if (transport > 0) {             /* (INT32G patch 0005) the core reads handed-over bytes in place: let it finish before they go */
      flush();
      wait_seq(submits);
      if (transport == 2) { hbdx9_submit_params a{native_device, 0, 0, HBDX9_SUBMIT_RING_ATTACH}; hb_call(HBDX9_CALL_SUBMIT, &a); transport = 0; }
    }
    parent->Release();
    if (qpred) { VirtualFree(qpred, 0, MEM_RELEASE); qpred = nullptr; }
    DeleteCriticalSection(&cs);
  }

  void reset_shadow() {
    memset(rs, 0, sizeof(rs));
    rs[D3DRS_ZENABLE] = D3DZB_TRUE; rs[D3DRS_FILLMODE] = D3DFILL_SOLID; rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    rs[D3DRS_ZWRITEENABLE] = TRUE; rs[D3DRS_SRCBLEND] = D3DBLEND_ONE; rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRS_CULLMODE] = D3DCULL_CCW; rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL; rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS; rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xffffffff; rs[D3DRS_TEXTUREFACTOR] = 0xffffffff;
    rs[D3DRS_CCW_STENCILFAIL] = rs[D3DRS_CCW_STENCILZFAIL] = rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP; rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_LIGHTING] = TRUE; rs[D3DRS_COLORVERTEX] = TRUE; rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD; rs[D3DRS_LOCALVIEWER] = TRUE;
    rs[D3DRS_COLORWRITEENABLE] = rs[D3DRS_COLORWRITEENABLE1] = rs[D3DRS_COLORWRITEENABLE2] = rs[D3DRS_COLORWRITEENABLE3] = 0xf;
    rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE; rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO; rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    rs[D3DRS_BLENDFACTOR] = 0xffffffff; rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1; rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE; rs[D3DRS_MULTISAMPLEMASK] = 0xffffffff; rs[D3DRS_CLIPPING] = TRUE;
    float one = 1.0f, pmax = 64.0f, fogend = 1.0f; memcpy(&rs[D3DRS_POINTSIZE], &one, 4); memcpy(&rs[D3DRS_POINTSIZE_MAX], &pmax, 4);
    memcpy(&rs[D3DRS_ADAPTIVETESS_Z], &one, 4);   /* (DX9FULL) D3D9 default; the core's state starts the same */
    memcpy(&rs[D3DRS_POINTSIZE_MIN], &one, 4);    /* (DX9FULL) D3D9 / DXVK default 1.0 (the core's state starts the same) */
    memcpy(&rs[D3DRS_POINTSCALE_A], &one, 4); memcpy(&rs[D3DRS_FOGEND], &fogend, 4); memcpy(&rs[D3DRS_FOGDENSITY], &one, 4);
    for (auto &s : ss) {
      memset(s, 0, sizeof(s));
      s[D3DSAMP_ADDRESSU] = s[D3DSAMP_ADDRESSV] = s[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
      s[D3DSAMP_MAGFILTER] = s[D3DSAMP_MINFILTER] = D3DTEXF_POINT; s[D3DSAMP_MAXANISOTROPY] = 1;
    }
    for (int i = 0; i < 8; i++) {
      DWORD *t = tss[i]; memset(t, 0, sizeof(tss[i]));
      t[D3DTSS_COLOROP] = i ? D3DTOP_DISABLE : D3DTOP_MODULATE; t[D3DTSS_COLORARG1] = D3DTA_TEXTURE; t[D3DTSS_COLORARG2] = D3DTA_CURRENT;
      t[D3DTSS_ALPHAOP] = i ? D3DTOP_DISABLE : D3DTOP_SELECTARG1; t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
      t[D3DTSS_TEXCOORDINDEX] = i; t[D3DTSS_RESULTARG] = D3DTA_CURRENT;
    }
    static const D3DMATRIX ident = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    for (auto &m : transforms) m = ident;
    for (auto &s : streams) s.freq = 1;
  }

  HRESULT init(HWND focus, D3DPRESENT_PARAMETERS *params);

  /* ---- packets ---- */
  template <typename T> T *add(uint16_t type, size_t extra = 0) {
    size_t at = packets.size(), sz = sizeof(T) + extra, need = (sz + 7) & ~(size_t)7;
    if (sent && at + need > packets.capacity()) { ring_room(need); at = packets.size(); }   /* a published range must not move */
    packets.resize(at + need);
    T *p = reinterpret_cast<T *>(packets.data() + at);
    uint64_t z = 0; memcpy(packets.data() + at + need - 8, &z, 8);   /* the padding (resize no longer zero-fills) */
    *p = T{};
    p->hdr.type = type; p->hdr.size = (uint32_t)sz;
    if (type == HBDX9_PKT_PRESENT) pkt_presents++;
    return p;
  }
  void pkt_state(uint16_t type, UINT index, UINT state, DWORD value) { DLOCK;
    auto *p = add<hbdx9_pkt_state>(type); p->index = index; p->state = state; p->value = value;
  }
  void pkt_object(uint16_t type, UINT index, uint64_t obj, UINT level = 0) { DLOCK;
    auto *p = add<hbdx9_pkt_object>(type); p->index = index; p->level = level; p->object = obj;
  }
  void pkt_viewport() { DLOCK;
    auto *p = add<hbdx9_pkt_viewport>(HBDX9_PKT_VIEWPORT);
    p->x = viewport.X; p->y = viewport.Y; p->width = viewport.Width; p->height = viewport.Height; p->min_z = viewport.MinZ; p->max_z = viewport.MaxZ;
  }
  HRESULT flush(bool early = false) { DLOCK;
    if (!defer_audit.empty()) {
      for (auto &a : defer_audit) {
        up.noov_checked++;
        if (audit_hash(a.p, a.size) != a.hash) {
          up.noov_written_after_unlock++; up.noov_written_bytes += a.size;
          if (upload_audit() && up.noov_written_after_unlock <= 8) hbdx9_log("upload-audit NOOVERWRITE range written after Unlock: %u bytes, frame %u", a.size, frames);
          /* tape v3: the bytes the core will read at this SUBMIT, as a write after Unlock at the end of the stream */
          tape_app_write(native_device, (uint32_t)unsent(), 3, a.flags, a.resource, 0, 0, a.offset, 0, a.size, 0, 0, a.p, a.size);
        }
      }
      defer_audit.clear();
    }
    if (!unsent()) return D3D_OK;
    if (transport < 0) choose_transport();
    uint32_t n = (uint32_t)unsent();
    hbdx9_submit_params s{native_device, (uint64_t)(uintptr_t)(packets.data() + sent), n, 0};
    NTSTATUS st = 0;
    if (transport == 2) ring_publish(packets.data() + sent, n, early);
    else {
      if (transport == 1) { s.status = HBDX9_SUBMIT_NOCOPY | HBDX9_SUBMIT_HAS_PRESENTS | (pkt_presents & 0xff); tr_nocopy++; }
      st = hb_call(HBDX9_CALL_SUBMIT, &s);
    }
    early_sent += n; submits++; early_blocked = false; pkt_presents = 0;
    if (transport == 2) sent = packets.size(); else if (transport == 1) rotate_packets(); else packets.clear();
    if (st || s.status) { hbdx9_log("submit failed st=%08lx status=%u", (unsigned long)st, s.status); return D3DERR_DRIVERINTERNALERROR; }
    return D3D_OK;
  }
  /* (INT32G patch 0005) which way the stream goes to the core: decided once, at the first flush */
  void choose_transport() {
    transport = 0;
    if (ring_on() && (core_caps & HBDX9_CAP_RING) && !g_tape.on) {
      /* never freed: the core's worker may still be looking at it after a detach */
      ring = (hbdx9_ring *)VirtualAlloc(nullptr, sizeof(hbdx9_ring), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (ring) {
        ring->magic = HBDX9_RING_MAGIC; ring->slots = HBDX9_RING_SLOTS;
        hbdx9_submit_params a{native_device, (uint64_t)(uintptr_t)ring, (uint32_t)sizeof(hbdx9_ring), HBDX9_SUBMIT_RING_ATTACH};
        if (!hb_call(HBDX9_CALL_SUBMIT, &a) && !a.status) { transport = 2; packets.reserve(4u << 20); } else ring = nullptr;
      }
    }
    if (!transport && submit_nocopy() && (core_caps & HBDX9_CAP_SUBMIT_NOCOPY)) transport = 1;
    if (ring_on() || submit_nocopy()) hbdx9_log("int32g transport=%d (0 the core copies, 1 no copy, 2 ring) core_caps=%#x", transport, core_caps);
  }
  void recycle_buffers() {
    while (!inflight.empty() && seq_done(inflight.front().seq)) {
      if (spare.size() < 32 && inflight.front().buf.capacity() <= (8u << 20)) { inflight.front().buf.clear(); spare.push_back(std::move(inflight.front().buf)); }
      inflight.pop_front();
    }
  }
  /* no copy: the buffer just handed over stays as it is until its sequence is published; take a finished one (or a new one) */
  void rotate_packets() {
    inflight.push_back(Flight{std::move(packets), submits});
    recycle_buffers();
    if (!spare.empty()) { packets = std::move(spare.back()); spare.pop_back(); tr_reused++; }
    else { packets = PktBuf(); packets.reserve(64u << 10); tr_fresh++; }
  }
  /* ring: `packets` has no room for `need` more bytes and holds published ranges, so it must not be reallocated.  If the core
     has run everything published (the sequence of the last flush), the unsent tail goes to the start of the same buffer;
     else the stream continues in another buffer and this one waits for its sequence. */
  void ring_room(size_t need) {
    size_t tail = unsent();
    if (seq_done(submits)) {
      if (tail) memmove(packets.data(), packets.data() + sent, tail);
      packets.resize(tail); sent = 0; tr_rewind++;
      return;
    }
    PktBuf nb;
    if (!spare.empty()) { nb = std::move(spare.back()); spare.pop_back(); tr_reused++; } else tr_fresh++;
    nb.clear(); nb.reserve(std::max<size_t>(packets.capacity(), tail + need + (64u << 10)));
    nb.insert(nb.end(), packets.begin() + sent, packets.end());
    inflight.push_back(Flight{std::move(packets), submits});
    packets = std::move(nb); sent = 0; tr_moved++;
    recycle_buffers();
  }
  void ring_call(uint32_t flags, uint32_t value) {
    hbdx9_submit_params c{native_device, (uint64_t)(uintptr_t)ring, value, flags};
    hb_call(HBDX9_CALL_SUBMIT, &c);
  }
  /* one chunk into the ring: the slot, then head (this thread is the only writer of head: the device lock) */
  void ring_publish(const uint8_t *data, uint32_t size, bool early) {
    hbdx9_ring *r = ring;
    uint32_t head = r->head;
    if (head - r->tail >= HBDX9_RING_SLOTS) { tr_full++; ring_call(HBDX9_SUBMIT_RING_WAIT | 1, 0); }
    hbdx9_ring_slot *sl = &r->slot[head & (HBDX9_RING_SLOTS - 1)];
    sl->data = (uint64_t)(uintptr_t)data; sl->size = size; sl->presents = pkt_presents;
    if (early) { r->head = head + 1; tr_relaxed++; }                          /* a lost wake-up is made up by the next publish */
    else InterlockedExchange((volatile LONG *)&r->head, (LONG)(head + 1));     /* the full barrier before the look at `sleeping` */
    tr_ring++;
    /* the worker parked: one call wakes it.  The flag is taken (exchanged to 0) so that the publishes of the next moments,
       before the worker is back on a processor, do not call again; the worker sets it anew when it parks again */
    if (r->sleeping && InterlockedExchange((volatile LONG *)&r->sleeping, 0)) { tr_wakes++; ring_call(HBDX9_SUBMIT_RING_WAKE, 0); }
    if (pkt_presents) {
      ring_presents += pkt_presents;
      /* frame pacing as in the core's SUBMIT: at most two presented frames ahead of the worker */
      if ((int32_t)(ring_presents - r->presents_done) > 2) { tr_pacing++; ring_call(HBDX9_SUBMIT_RING_WAIT | 2, ring_presents); }
    }
  }
  HRESULT maybe_flush() { return unsent() > (1u << 20) ? flush() : D3D_OK; }
  /* (INT32G) HBDX9_EARLY_HANDOFF.  early_pass_boundary(): called before a draw or an effective clear is recorded; true when render
     target 0 is not the one of the previous draw/clear - the core ends its render pass at this packet whatever we do - and a
     commit is due.  early_commit(): FLUSH packet (the core commits its command buffer) + SUBMIT. */
  /* inside the scope: 0 always; 2 the previous frame waited for a query; 1 also: this frame has not reached the stream position
     where the previous frame first waited */
  bool early_armed() const {
    unsigned sc = early_scope();
    return !sc || (early_wait_prev && (sc != 1 || (!early_wait_at && early_sent + unsent() < early_wait_prev)));
  }
  void early_note_wait() { if (!early_wait_at) early_wait_at = (early_sent + (uint32_t)unsent()) | 1; }
  bool early_pass_boundary() {
    uint64_t n = rt[0] ? rt[0]->native : 0; uint32_t l = rt[0] ? (rt[0]->level | (rt[0]->face << 16)) : 0;
    bool moved = n != early_rt_native || l != early_rt_level;
    early_rt_native = n; early_rt_level = l;
    if (!moved || !early_draws_min() || early_draws < early_draws_min() || !early_armed()) return false;
    if (early_blocked) { early_n_blocked++; return false; }
    return true;
  }
  void early_commit() { add<hbdx9_pkt_empty>(HBDX9_PKT_FLUSH); flush(true); q_flush_submits = submits; early_draws = 0; early_n_commit++; }
  HRESULT after_draw() {
    if (early_on()) {
      early_draws++;
      if (early_kb() && unsent() >= (size_t)early_kb() * 1024 && early_armed()) {
        if (early_blocked) early_n_blocked++; else { early_n_size++; return flush(true); }
      }
    }
    return maybe_flush();
  }

  /* uploads: small inline, large by guest pointer in an immediate SUBMIT */
  /* returns the sequence of the SUBMIT that carries a pointer to `data` (0: copied inline) */
  uint32_t upload_buffer(uint64_t native, UINT offset, UINT size, DWORD flags, const void *data) { DLOCK;
    bool inl = size <= kInlineLimit;
    if (tape_app_scope) tape_tag(native_device, (uint32_t)unsent());
    auto *p = add<hbdx9_pkt_update_buffer>(HBDX9_PKT_UPDATE_BUFFER, inl ? size : 0);
    p->buffer = native; p->offset = offset; p->size = size; p->flags = flags;
    if (inl) { memcpy(p + 1, data, size); up.inline_n++; up.inline_bytes += size; return 0; }
    p->guest_data = (uint64_t)(uintptr_t)data; up.pointer_n++; up.pointer_bytes += size; up.sync_submits++; flush();
    return submits;
  }
  /* NOOVERWRITE: the shadow range is referenced, not copied, and read by the core at the next SUBMIT of this stream.
     Returns the submit generation the reference lives in (see Hbdx9BufferT::Lock). */
  unsigned upload_buffer_deferred(uint64_t native, UINT offset, UINT size, DWORD flags, const void *data) { DLOCK;
    if (tape_app_scope) tape_tag(native_device, (uint32_t)unsent());
    auto *p = add<hbdx9_pkt_update_buffer>(HBDX9_PKT_UPDATE_BUFFER, 0);
    p->buffer = native; p->offset = offset; p->size = size; p->flags = flags; p->guest_data = (uint64_t)(uintptr_t)data;
    up.deferred_n++; up.deferred_bytes += size; early_blocked = true;
    if (upload_audit() || g_tape.on) defer_audit.push_back({data, size, audit_hash(data, size), native, offset, flags});
    unsigned seq = submits + 1;      /* the SUBMIT that will carry it */
    maybe_flush();
    return seq;
  }
  uint32_t upload_texture(uint64_t native, UINT level, UINT face, UINT x, UINT y, UINT w, UINT h, UINT pitch, UINT size, const void *data) { DLOCK;
    bool inl = size <= kInlineLimit;
    if (tape_app_scope) tape_tag(native_device, (uint32_t)unsent());
    auto *p = add<hbdx9_pkt_update_texture>(HBDX9_PKT_UPDATE_TEXTURE, inl ? size : 0);
    p->texture = native; p->level = level; p->face = face; p->x = x; p->y = y; p->width = w; p->height = h; p->pitch = pitch; p->size = size;
    if (inl) { memcpy(p + 1, data, size); up.inline_n++; up.inline_bytes += size; return 0; }
    p->guest_data = (uint64_t)(uintptr_t)data; up.pointer_n++; up.pointer_bytes += size; up.sync_submits++; flush();
    return submits;
  }
  /* lazy MANAGED textures (defined after TexCore) */
  void upload_dirty(TexCore *c);
  void pre_draw();
  void audit_draw();   /* (s8) HBDX9_UPLOAD_AUDIT */
  void bind_texture(int i, IDirect3DBaseTexture9 *t);
  void release_native(uint64_t obj, UINT kind) { if (obj) pkt_object(HBDX9_PKT_RELEASE_OBJECT, kind, obj); }

  /* IUnknown */
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DDevice9) || (ex && IsEqualGUID(riid, IID_IDirect3DDevice9Ex))) {
      *out = this; AddRef(); return S_OK;
    }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }

  /* ---- device basics ---- */
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return D3D_OK; }
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return 0x7ff00000u; }
  HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **out) override { if (!out) return D3DERR_INVALIDCALL; *out = parent; parent->AddRef(); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *caps) override { return parent->GetDeviceCaps(0, D3DDEVTYPE_HAL, caps); }
  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT sc, D3DDISPLAYMODE *mode) override { (void)sc; return parent->GetAdapterDisplayMode(0, mode); }
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *p) override { if (!p) return D3DERR_INVALIDCALL; *p = creation; return D3D_OK; }
  UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }
  HRESULT STDMETHODCALLTYPE BeginScene() override { DLOCK; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE EndScene() override { DLOCK; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *passes) override { if (passes) *passes = 1; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL v) override { DLOCK; sw_vp = v; return D3D_OK; }
  /* N-patches: deprecated, no D3D9 driver tessellates since 2005; accept and draw flat (Gothic 3 calls it) */
  float npatch_segments = 0.0f;
  HRESULT STDMETHODCALLTYPE SetNPatchMode(float n) override { npatch_segments = n; return D3D_OK; }
  float STDMETHODCALLTYPE GetNPatchMode() override { return npatch_segments; }
  BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return sw_vp; }
  /* (DX9FULL 13.11.2026) hardware cursor as DXVK's D3D9Cursor (Win32 path): the A8R8G8B8 surface becomes a Win32 cursor
     (CreateIconIndirect, alpha from the surface), ShowCursor sets / clears it, SetCursorPosition moves the system cursor only
     when it is elsewhere.  Without HBDX9_FMT_EXT: the s8j stubs (D3D_OK, nothing shown, CursorCaps 0). */
  HCURSOR hw_cursor = nullptr; BOOL cursor_visible = FALSE;
  HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT hx, UINT hy, IDirect3DSurface9 *bmp) override { DLOCK;
    if (!fmt_ext_on()) return D3D_OK;
    if (!bmp) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC d{}; bmp->GetDesc(&d);
    if (d.Format != D3DFMT_A8R8G8B8) return D3DERR_INVALIDCALL;
    UINT w = d.Width, h = d.Height;
    if ((w & (w - 1)) || (h & (h - 1)) || hx >= w || hy >= h) return D3DERR_INVALIDCALL;
    D3DLOCKED_RECT lr;
    if (FAILED(bmp->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return D3DERR_INVALIDCALL;
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (UINT y = 0; y < h; y++) memcpy(&px[(size_t)y * w * 4], (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, w * 4);
    bmp->UnlockRect();
    std::vector<uint8_t> mask((size_t)((w + 15) / 16 * 2) * h, 0xff);
    ICONINFO ii{}; ii.fIcon = FALSE; ii.xHotspot = hx; ii.yHotspot = hy;
    ii.hbmMask = CreateBitmap(w, h, 1, 1, mask.data()); ii.hbmColor = CreateBitmap(w, h, 1, 32, px.data());
    HCURSOR c = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask); DeleteObject(ii.hbmColor);
    if (!c) return D3DERR_INVALIDCALL;
    if (hw_cursor) DestroyCursor(hw_cursor);
    hw_cursor = c;
    if (cursor_visible) SetCursor(hw_cursor);
    return D3D_OK;
  }
  void STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD) override { DLOCK;
    if (!fmt_ext_on() || !hw_cursor) return;
    POINT p{}; if (GetCursorPos(&p) && p.x == x && p.y == y) return;
    SetCursorPos(x, y);
  }
  BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) override {
    if (!fmt_ext_on()) return FALSE;
    Hbdx9Lock l(&cs, locking);
    if (!hw_cursor) return cursor_visible;   /* DXVK: visibility unchanged while no cursor is set */
    SetCursor(show ? hw_cursor : nullptr);
    BOOL prev = cursor_visible; cursor_visible = show; return prev;
  }
  void STDMETHODCALLTYPE SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) override { DLOCK;}
  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT, D3DRASTER_STATUS *s) override {
    if (!s) return D3DERR_INVALIDCALL;
    if (!fmt_ext_on()) { s->InVBlank = FALSE; s->ScanLine = 0; return D3D_OK; }
    /* (DX9FULL 13.11.2026) DXVK D3D9SwapChainEx::GetRasterStatus: a scan line made up from the time and the display mode
       (20 blanking lines), so a game waiting for InVBlank sees it come once per refresh instead of never */
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    UINT height = 1080, hz = 60;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) { if (dm.dmPelsHeight) height = dm.dmPelsHeight; if (dm.dmDisplayFrequency > 1) hz = dm.dmDisplayFrequency; }
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    uint64_t us = (uint64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart), frame_us = 1000000u / hz, lines = height + 20;
    uint64_t line_us = frame_us / lines ? frame_us / lines : 1;
    UINT line = (UINT)((us % frame_us) / line_us);
    s->InVBlank = line >= height; s->ScanLine = s->InVBlank ? 0 : line;
    return D3D_OK;
  }

  /* ---- viewport / scissor / transforms / material ---- */
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *vp) override { DLOCK; if (!vp) return D3DERR_INVALIDCALL; if (rec) rec->viewport = 1; viewport = *vp; sb_note(SB_VIEWPORT, 0); pkt_viewport(); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *vp) override { if (!vp) return D3DERR_INVALIDCALL; *vp = viewport; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *r) override { DLOCK;
    if (!r) return D3DERR_INVALIDCALL; if (rec) rec->scissor = 1; scissor = *r; sb_note(SB_SCISSOR, 0);
    auto *p = add<hbdx9_pkt_scissor>(HBDX9_PKT_SCISSOR); p->rect = {r->left, r->top, r->right, r->bottom}; return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *r) override { if (!r) return D3DERR_INVALIDCALL; *r = scissor; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX *m) override { DLOCK;
    if (!m || (UINT)s >= 512) return D3DERR_INVALIDCALL;
    if (rec) rec->transform[s] = 1;
    transforms[s] = *m; sb_note_tr(s);
    auto *p = add<hbdx9_pkt_transform>(HBDX9_PKT_TRANSFORM); p->state = s; memcpy(p->m, m, 64);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX *m) override { if (!m || (UINT)s >= 512) return D3DERR_INVALIDCALL; *m = transforms[s]; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX *m) override { DLOCK;
    if (!m || (UINT)s >= 512) return D3DERR_INVALIDCALL;
    D3DMATRIX r; const D3DMATRIX &a = transforms[s];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float v = 0; for (int k = 0; k < 4; k++) v += a.m[i][k] * m->m[k][j]; r.m[i][j] = v; }
    return SetTransform(s, &r);
  }
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *m) override { DLOCK;
    if (!m) return D3DERR_INVALIDCALL; if (rec) rec->material = 1; material = *m; sb_note(SB_MATERIAL, 0);
    auto *p = add<hbdx9_pkt_material>(HBDX9_PKT_MATERIAL);
    memcpy(p->diffuse, &m->Diffuse, 16); memcpy(p->ambient, &m->Ambient, 16); memcpy(p->specular, &m->Specular, 16);
    memcpy(p->emissive, &m->Emissive, 16); p->power = m->Power;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *m) override { if (!m) return D3DERR_INVALIDCALL; *m = material; return D3D_OK; }
  LightRec *find_light(DWORD i) { for (auto &l : lights) if (l.index == i) return &l; return nullptr; }
  void send_lights() { DLOCK;
    std::vector<const LightRec *> on;
    for (auto &l : lights) if (l.enabled) on.push_back(&l);
    std::sort(on.begin(), on.end(), [](const LightRec *a, const LightRec *b) { return a->index < b->index; });
    if (on.size() > 8) on.resize(8);
    auto *p = add<hbdx9_pkt_lights>(HBDX9_PKT_LIGHTS, on.size() * sizeof(hbdx9_light));
    p->count = (uint32_t)on.size();
    auto *o = reinterpret_cast<hbdx9_light *>(p + 1);
    for (size_t i = 0; i < on.size(); i++) {
      const D3DLIGHT9 &L = on[i]->light; hbdx9_light &x = o[i];
      memset(&x, 0, sizeof(x));
      x.type = L.Type; memcpy(x.diffuse, &L.Diffuse, 16); memcpy(x.specular, &L.Specular, 16); memcpy(x.ambient, &L.Ambient, 16);
      memcpy(x.position, &L.Position, 12); memcpy(x.direction, &L.Direction, 12);
      x.range = L.Range; x.falloff = L.Falloff; x.attenuation0 = L.Attenuation0; x.attenuation1 = L.Attenuation1;
      x.attenuation2 = L.Attenuation2; x.theta = L.Theta; x.phi = L.Phi;
    }
  }
  HRESULT STDMETHODCALLTYPE SetLight(DWORD i, const D3DLIGHT9 *l) override { DLOCK;
    if (!l || l->Type < D3DLIGHT_POINT || l->Type > D3DLIGHT_DIRECTIONAL) return D3DERR_INVALIDCALL;
    if (rec && fmt_ext_on()) rec_lt_set.push_back(i);
    LightRec *r = find_light(i);
    if (!r) { lights.push_back({i, *l, false}); r = &lights.back(); } else r->light = *l;
    sb_note(SB_LIGHT, 0);
    if (r->enabled) send_lights();
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetLight(DWORD i, D3DLIGHT9 *l) override {
    LightRec *r = find_light(i); if (!l || !r) return D3DERR_INVALIDCALL; *l = r->light; return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD i, BOOL on) override { DLOCK;
    if (rec && fmt_ext_on()) rec_lt_en.push_back(i);
    LightRec *r = find_light(i);
    if (!r) {  /* D3D9: enabling an unset light creates the default (white directional along +z) */
      D3DLIGHT9 d{}; d.Type = D3DLIGHT_DIRECTIONAL; d.Diffuse = {1, 1, 1, 0}; d.Direction = {0, 0, 1};
      lights.push_back({i, d, false}); r = &lights.back(); sb_note(SB_LIGHT, 0);
    }
    bool e = on != FALSE;
    if (r->enabled != e) { r->enabled = e; sb_note(SB_LIGHT, 0); send_lights(); }
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD i, BOOL *on) override {
    LightRec *r = find_light(i); if (!on || !r) return D3DERR_INVALIDCALL; *on = r->enabled ? 128 : 0; return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD i, const float *p) override { DLOCK;
    if (i >= 6 || !p) return D3DERR_INVALIDCALL; if (rec) rec->clip[i] = 1; memcpy(clip_planes[i], p, 16); sb_note(SB_CLIP, i);
    auto *k = add<hbdx9_pkt_clip_plane>(HBDX9_PKT_CLIP_PLANE); k->index = i; memcpy(k->plane, p, 16);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD i, float *p) override { if (i >= 6 || !p) return D3DERR_INVALIDCALL; memcpy(p, clip_planes[i], 16); return D3D_OK; }

  /* ---- states ---- */
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override { DLOCK;
    if ((UINT)s >= 256) return D3DERR_INVALIDCALL;
    if (rec) rec->rs[s] = 1;
    if (rs[s] == v) return D3D_OK;
    rs[s] = v; sb_note(SB_RS, s); pkt_state(HBDX9_PKT_RENDER_STATE, 0, s, v); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE s, DWORD *v) override { if (!v || (UINT)s >= 256) return D3DERR_INVALIDCALL; *v = rs[s]; return D3D_OK; }
  static int sampler_index(DWORD s) { if (s < 16) return (int)s; if (s >= D3DVERTEXTEXTURESAMPLER0 && s <= D3DVERTEXTEXTURESAMPLER3) return 16 + (int)(s - D3DVERTEXTEXTURESAMPLER0); return -1; }
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) override { DLOCK;
    int i = sampler_index(s); if (i < 0 || (UINT)t >= 14) return D3DERR_INVALIDCALL;
    if (rec) rec->ss[i][t] = 1;
    if (ss[i][t] == v) return D3D_OK;
    ss[i][t] = v; sb_note(SB_SS, i * 14 + t); pkt_state(HBDX9_PKT_SAMPLER_STATE, i, t, v); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD *v) override {
    int i = sampler_index(s); if (i < 0 || (UINT)t >= 14 || !v) return D3DERR_INVALIDCALL; *v = ss[i][t]; return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v) override { DLOCK;
    if (st >= 8 || (UINT)t >= 33) return D3DERR_INVALIDCALL;
    if (rec) rec->tss[st][t] = 1;
    if (tss[st][t] == v) return D3D_OK;
    tss[st][t] = v; sb_note(SB_TSS, st * 33 + t); pkt_state(HBDX9_PKT_TEXTURE_STAGE_STATE, st, t, v); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD *v) override {
    if (st >= 8 || (UINT)t >= 33 || !v) return D3DERR_INVALIDCALL; *v = tss[st][t]; return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD s, IDirect3DBaseTexture9 *t) override;
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD s, IDirect3DBaseTexture9 **t) override {
    int i = sampler_index(s); if (i < 0 || !t) return D3DERR_INVALIDCALL;
    *t = tex[i]; if (*t) (*t)->AddRef(); return D3D_OK;
  }

  /* ---- vertex input ---- */
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD f) override;
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *f) override { if (!f) return D3DERR_INVALIDCALL; *f = fvf; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **out) override;
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *d) override;
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **d) override { if (!d) return D3DERR_INVALIDCALL; *d = decl; if (decl) decl->AddRef(); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf_, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *shared) override;
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *shared) override;
  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT s, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) override;
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT s, IDirect3DVertexBuffer9 **vb, UINT *offset, UINT *stride) override {
    if (s >= 16 || !vb) return D3DERR_INVALIDCALL;
    *vb = streams[s].vb; if (*vb) (*vb)->AddRef();
    if (offset) *offset = streams[s].offset;
    if (stride) *stride = streams[s].stride;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT s, UINT f) override { DLOCK;
    if (s >= 16) return D3DERR_INVALIDCALL; if (rec) rec->freq[s] = 1; streams[s].freq = f; sb_note(SB_FREQ, s);
    pkt_state(HBDX9_PKT_STREAM_FREQ, s, 0, f); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT s, UINT *f) override { if (s >= 16 || !f) return D3DERR_INVALIDCALL; *f = streams[s].freq; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *i) override;
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **i) override { if (!i) return D3DERR_INVALIDCALL; *i = ib; if (ib) ib->AddRef(); return D3D_OK; }

  /* ---- shaders and constants ---- */
  HRESULT create_shader(const DWORD *fn, UINT stage, uint64_t *native, UINT *count);
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *fn, IDirect3DVertexShader9 **out) override;
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *fn, IDirect3DPixelShader9 **out) override;
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *s) override;
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *s) override;
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **s) override { if (!s) return D3DERR_INVALIDCALL; *s = vs; if (vs) vs->AddRef(); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **s) override { if (!s) return D3DERR_INVALIDCALL; *s = ps; if (ps) ps->AddRef(); return D3D_OK; }
  __attribute__((always_inline)) HRESULT set_consts(UINT kind, void *bank, UINT elem, UINT limit, UINT start, const void *data, UINT count) {
    if (!data || start + count > limit) return D3DERR_INVALIDCALL;
    if (rec) {
      uint8_t *m = kind == HBDX9_CONST_VS_F ? rec->vsf : kind == HBDX9_CONST_PS_F ? rec->psf : kind == HBDX9_CONST_VS_I ? rec->vsi
                 : kind == HBDX9_CONST_PS_I ? rec->psi : kind == HBDX9_CONST_VS_B ? rec->vsb : rec->psb;
      memset(m + start, 1, count);
    }
    if (!count) return D3D_OK;
    memcpy((BYTE *)bank + start * elem, data, count * elem);
    sb_note(SB_VSF + (kind - HBDX9_CONST_VS_F), start, count);
    auto *p = add<hbdx9_pkt_constants>(HBDX9_PKT_CONSTANTS, count * elem);
    p->kind = kind; p->start = start; p->count = count; memcpy(p + 1, data, count * elem);
    return D3D_OK;
  }
  HRESULT get_consts(const void *bank, UINT elem, UINT limit, UINT start, void *data, UINT count) {
    if (!data || start + count > limit) return D3DERR_INVALIDCALL;
    memcpy(data, (const BYTE *)bank + start * elem, count * elem); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT s, const float *d, UINT n) override { DLOCK; return set_consts(HBDX9_CONST_VS_F, vsf, 16, 256, s, d, n); }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT s, float *d, UINT n) override { return get_consts(vsf, 16, 256, s, d, n); }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT s, const float *d, UINT n) override { DLOCK; return set_consts(HBDX9_CONST_PS_F, psf, 16, 224, s, d, n); }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT s, float *d, UINT n) override { return get_consts(psf, 16, 224, s, d, n); }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT s, const int *d, UINT n) override { DLOCK; return set_consts(HBDX9_CONST_VS_I, vsi, 16, 16, s, d, n); }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT s, int *d, UINT n) override { return get_consts(vsi, 16, 16, s, d, n); }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT s, const int *d, UINT n) override { DLOCK; return set_consts(HBDX9_CONST_PS_I, psi, 16, 16, s, d, n); }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT s, int *d, UINT n) override { return get_consts(psi, 16, 16, s, d, n); }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT s, const BOOL *d, UINT n) override { DLOCK; return set_consts(HBDX9_CONST_VS_B, vsb, 4, 16, s, d, n); }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT s, BOOL *d, UINT n) override { return get_consts(vsb, 4, 16, s, d, n); }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT s, const BOOL *d, UINT n) override { DLOCK; return set_consts(HBDX9_CONST_PS_B, psb, 4, 16, s, d, n); }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT s, BOOL *d, UINT n) override { return get_consts(psb, 4, 16, s, d, n); }

  /* ---- draws ---- */
  static UINT vertex_count(D3DPRIMITIVETYPE pt, UINT prims) {
    switch (pt) {
    case D3DPT_POINTLIST: return prims; case D3DPT_LINELIST: return prims * 2; case D3DPT_LINESTRIP: return prims + 1;
    case D3DPT_TRIANGLELIST: return prims * 3; case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return prims + 2;
    default: return 0;
    }
  }
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE pt, UINT start, UINT prims) override { DLOCK;
    if (!prims) return D3D_OK;
    if (dirty_bound) pre_draw();
    if (upload_audit()) audit_draw();
    if (early_on() && early_pass_boundary()) early_commit();
    if (q_open) q_key_state = 2;
    auto *p = add<hbdx9_pkt_draw>(HBDX9_PKT_DRAW); p->primitive = pt; p->start_vertex = start; p->primitive_count = prims;
    return after_draw();
  }
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE pt, INT base, UINT min_index, UINT nverts, UINT start_index, UINT prims) override { DLOCK;
    if (!prims) return D3D_OK;
    if (dirty_bound) pre_draw();
    if (upload_audit()) audit_draw();
    if (early_on() && early_pass_boundary()) early_commit();
    if (q_open) qp_note_indexed(pt, base, min_index, nverts, start_index, prims);
    auto *p = add<hbdx9_pkt_draw_indexed>(HBDX9_PKT_DRAW_INDEXED);
    p->primitive = pt; p->base_vertex = base; p->min_index = min_index; p->num_vertices = nverts; p->start_index = start_index; p->primitive_count = prims;
    return after_draw();
  }
  void after_up() {
    /* D3D9: DrawPrimitiveUP leaves stream 0 (and the index buffer for the indexed form) unbound */
    if (streams[0].vb) { streams[0].vb->Release(); streams[0].vb = nullptr; }
    streams[0].offset = streams[0].stride = 0; sb_note(SB_STREAM, 0);
    auto *s = add<hbdx9_pkt_stream_source>(HBDX9_PKT_STREAM_SOURCE); s->stream = 0;
  }
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE pt, UINT prims, const void *data, UINT stride) override { DLOCK;
    if (!data || !stride) return D3DERR_INVALIDCALL;
    if (!prims) return D3D_OK;
    UINT bytes = vertex_count(pt, prims) * stride;
    if (dirty_bound) pre_draw();
    if (upload_audit()) audit_draw();
    if (early_on() && early_pass_boundary()) early_commit();
    if (q_open) q_key_state = 2;
    auto *p = add<hbdx9_pkt_draw_up>(HBDX9_PKT_DRAW_UP, bytes);
    p->primitive = pt; p->primitive_count = prims; p->stride = stride; p->vertex_bytes = bytes;
    memcpy(p + 1, data, bytes);
    after_up();
    return after_draw();
  }
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE pt, UINT min_index, UINT nverts, UINT prims, const void *idx, D3DFORMAT fmt,
                                                   const void *data, UINT stride) override { DLOCK;
    if (!data || !idx || !stride) return D3DERR_INVALIDCALL;
    if (!prims) return D3D_OK;
    UINT isz = fmt == D3DFMT_INDEX32 ? 4 : 2;
    UINT ibytes = vertex_count(pt, prims) * isz, ipad = (ibytes + 3) & ~3u;
    UINT vbytes = (min_index + nverts) * stride;
    if (dirty_bound) pre_draw();
    if (upload_audit()) audit_draw();
    if (early_on() && early_pass_boundary()) early_commit();
    if (q_open) q_key_state = 2;
    auto *p = add<hbdx9_pkt_draw_indexed_up>(HBDX9_PKT_DRAW_INDEXED_UP, ipad + vbytes);
    p->primitive = pt; p->min_index = min_index; p->num_vertices = nverts; p->primitive_count = prims;
    p->index_format = fmt; p->stride = stride; p->index_bytes = ibytes; p->vertex_bytes = vbytes;
    memcpy(p + 1, idx, ibytes); memcpy((BYTE *)(p + 1) + ipad, data, vbytes);
    after_up();
    if (ib) { ib->Release(); ib = nullptr; sb_note(SB_IB, 0); pkt_object(HBDX9_PKT_INDICES, 0, 0); }
    return after_draw();
  }

  /* ---- render targets ---- */
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out) override {
    (void)type;
    if (sc || index || !out) return D3DERR_INVALIDCALL;
    *out = backbuffer; backbuffer->AddRef(); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD index, IDirect3DSurface9 **out) override {
    if (!out || index >= 4) return D3DERR_INVALIDCALL;
    if (!rt[index]) { *out = nullptr; return D3DERR_NOTFOUND; }
    *out = rt[index]; rt[index]->AddRef(); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) override { DLOCK;
    if (index >= 4 || (!index && !surface)) return D3DERR_INVALIDCALL;
    Hbdx9Surface *s = static_cast<Hbdx9Surface *>(surface);
    if (s && !s->native) return D3DERR_INVALIDCALL;
    if (s) s->AddRef();
    if (rt[index]) rt[index]->Release();
    rt[index] = s;
    pkt_object(HBDX9_PKT_RENDER_TARGET, index, s ? s->native : 0, s ? (s->level | (s->face << 16)) : 0);
    if (!index) { viewport = {0, 0, s->desc.Width, s->desc.Height, 0.0f, 1.0f}; sb_note(SB_VIEWPORT, 0); sb_note(SB_SCISSOR, 0); pkt_viewport();
      scissor = {0, 0, (LONG)s->desc.Width, (LONG)s->desc.Height}; auto *p = add<hbdx9_pkt_scissor>(HBDX9_PKT_SCISSOR); p->rect = {0, 0, scissor.right, scissor.bottom}; }
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *surface) override { DLOCK;
    Hbdx9Surface *s = static_cast<Hbdx9Surface *>(surface);
    if (s) s->AddRef();
    if (ds) ds->Release();
    ds = s;
    pkt_object(HBDX9_PKT_DEPTH_STENCIL, 0, s ? s->native : 0);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **out) override {
    if (!out) return D3DERR_INVALIDCALL;
    *out = ds; if (!ds) return D3DERR_NOTFOUND;
    ds->AddRef(); return D3D_OK;
  }
  HRESULT create_surface(UINT w, UINT h, D3DFORMAT fmt, bool depth, IDirect3DSurface9 **out, UINT samples = 1,
                         D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE, DWORD quality = 0) {
    if (!out) return D3DERR_INVALIDCALL;
    hbdx9_create_resource_params p{}; p.device = native_device; p.kind = depth ? HBDX9_RES_SURFACE_DS : HBDX9_RES_SURFACE_RT;
    p.format = fmt; p.width = w; p.height = h; p.depth = 1; p.levels = 1; p.usage = depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET; p.pool = D3DPOOL_DEFAULT;
    p.samples = samples;
    if (hb_call(HBDX9_CALL_CREATE_RESOURCE, &p) || p.status) {
      HRESULT hr = p.status == 2 ? E_OUTOFMEMORY : D3DERR_NOTAVAILABLE;
      FMT_TRACE(depth ? "CreateDepthStencilSurface" : "CreateRenderTarget", fmt, p.usage, D3DRTYPE_SURFACE, hr);
      return hr;
    }
    FMT_TRACE(depth ? "CreateDepthStencilSurface" : "CreateRenderTarget", fmt, p.usage, D3DRTYPE_SURFACE, D3D_OK);
    D3DSURFACE_DESC d{}; d.Format = fmt; d.Type = D3DRTYPE_SURFACE; d.Usage = p.usage; d.Pool = D3DPOOL_DEFAULT; d.Width = w; d.Height = h;
    d.MultiSampleType = samples > 1 ? ms : D3DMULTISAMPLE_NONE; d.MultiSampleQuality = samples > 1 ? quality : 0;
    auto *s = new Hbdx9Surface(this, d); s->native = p.resource; s->samples = samples;
    *out = s; return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL, IDirect3DSurface9 **out, HANDLE *shared) override { DLOCK;
    if (shared) return D3DERR_INVALIDCALL; return create_surface(w, h, fmt, false, out, samples_of(ms, q), ms, q);
  }
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL, IDirect3DSurface9 **out, HANDLE *shared) override { DLOCK;
    if (shared) return D3DERR_INVALIDCALL; return create_surface(w, h, fmt, true, out, samples_of(ms, q), ms, q);
  }

  HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override { DLOCK;
    if (!(flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))) return D3DERR_INVALIDCALL;
    if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && !ds) return D3DERR_INVALIDCALL;
    if (count && !rects) return D3DERR_INVALIDCALL;
    LONG vx1 = viewport.X, vy1 = viewport.Y, vx2 = viewport.X + viewport.Width, vy2 = viewport.Y + viewport.Height;
    DWORD n = count ? count : 1;
    auto *pkt = add<hbdx9_pkt_clear>(HBDX9_PKT_CLEAR, n * sizeof(hbdx9_rect));
    pkt->flags = flags; pkt->color = color; pkt->z = z; pkt->stencil = stencil;
    auto *out = reinterpret_cast<hbdx9_rect *>(pkt + 1);
    DWORD kept = 0;
    for (DWORD i = 0; i < n; i++) {
      LONG x1 = count ? rects[i].x1 : vx1, y1 = count ? rects[i].y1 : vy1, x2 = count ? rects[i].x2 : vx2, y2 = count ? rects[i].y2 : vy2;
      if (x1 < vx1) x1 = vx1; if (y1 < vy1) y1 = vy1; if (x2 > vx2) x2 = vx2; if (y2 > vy2) y2 = vy2;
      if (x2 <= x1 || y2 <= y1) continue;
      out[kept++] = {(int32_t)x1, (int32_t)y1, (int32_t)x2, (int32_t)y2};
    }
    pkt->rect_count = kept;
    pkt->hdr.size = (uint32_t)(sizeof(hbdx9_pkt_clear) + kept * sizeof(hbdx9_rect));
    /* (INT32G fix) rects clipped away must not stay in the stream as zero bytes: the core's parser stops at a zero header and
       drops the rest of this SUBMIT.  A shrinking resize keeps `pkt` valid. */
    if (kept != n) packets.resize((size_t)((uint8_t *)pkt - packets.data()) + (((size_t)pkt->hdr.size + 7) & ~(size_t)7));
    if (early_on() && kept && (flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER)) && early_pass_boundary()) {
      /* (INT32G) take the CLEAR packet out, FLUSH + SUBMIT what was recorded before it, put it back unchanged */
      size_t at0 = (size_t)((uint8_t *)pkt - packets.data());
      std::vector<uint8_t> keep(packets.begin() + at0, packets.end());
      packets.resize(at0); early_commit();
      if (sent && packets.size() + keep.size() > packets.capacity()) ring_room(keep.size());
      packets.insert(packets.end(), keep.begin(), keep.end());
    }
    return maybe_flush();
  }

  DWORD fps_prev = 0, fps_frames = 0; LARGE_INTEGER last_present{};
  BYTE *check_buf = nullptr; size_t check_size = 0;
  void live_check(uint32_t f) {
    D3DFORMAT fmt = backbuffer->desc.Format; FmtSize fs = fmt_size(fmt);
    UINT w = backbuffer->desc.Width, h = backbuffer->desc.Height, pitch = w * fs.bytes;
    if (fs.block != 1 || !fs.bytes || backbuffer->samples > 1) return;
    size_t need = (size_t)pitch * h;
    if (check_size < need) {
      if (check_buf) VirtualFree(check_buf, 0, MEM_RELEASE);
      check_buf = (BYTE *)VirtualAlloc(nullptr, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); check_size = check_buf ? need : 0;
    }
    if (!check_buf || FAILED(flush())) return;
    hbdx9_readback_params r{native_device, backbuffer->native, (uint64_t)(uintptr_t)check_buf, pitch, w, h, 0, 0, 0};
    if (hb_call(HBDX9_CALL_READBACK, &r) || r.status) return;
    bool x8 = fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_X8B8G8R8;
    uint64_t hsh = 1469598103934665603ull;
    for (size_t i = 0; i < need; i++) { hsh ^= (x8 && (i & 3) == 3) ? 0 : check_buf[i]; hsh *= 1099511628211ull; }
    char m[160]; snprintf(m, sizeof(m), "livecheck frame=%u hash=%016llx w=%u h=%u fmt=%u", f, (unsigned long long)hsh, w, h, (unsigned)fmt);
    tape_mark(m);
  }
  HRESULT present(DWORD flags = 0) {
    if (g_tape.on && g_tape.check_every && backbuffer && (frames + 1) % g_tape.check_every == 0) live_check(frames + 1);
    auto *pkt = add<hbdx9_pkt_present>(HBDX9_PKT_PRESENT);
    pkt->swapchain = native_swapchain;
    /* (DX9FULL v12) D3DPRESENT_FORCEIMMEDIATE (D3D9Ex): this frame without display sync, as DXVK.  D3DPRESENT_DONOTWAIT: Present never
       reports D3DERR_WASSTILLDRAWING here (as DXVK; the flag allows it, it does not require it) */
    if ((flags & D3DPRESENT_FORCEIMMEDIATE) && fmt_ext_on() && (core_caps & HBDX9_CAP_TEXTURE_LOD)) pkt->hdr.pad = HBDX9_PRESENT_FORCE_IMMEDIATE;
    frames++; early_draws = 0;
    if (frames == 1 || frames % 600 == 0) hbdx9_log("frame %u submits=%u", frames, submits - seq_base);
    up.present_thread = GetCurrentThreadId();
    if (upload_audit() && (frames == 1 || frames % 100 == 0))
      hbdx9_log("upload-audit frame %u tex_unlocks=%llu tex_marked_lazy=%llu tex_lazy_uploads=%llu tex_readonly=%llu tex_readonly_marked=%llu "
                "tex_add_dirty=%llu audit_draws=%llu audit_unsent_draws=%llu audit_unsent_textures=%llu audit_dirty_draws=%llu "
                "audit_dirty_textures=%llu dirty_bound=%u tex_unlocks_other_thread=%llu tex_readonly_written=%llu noov_checked=%llu "
                "noov_written_after_unlock=%llu noov_written_bytes=%llu deferred=%llu pointer=%llu",
                frames, (unsigned long long)up.tex_unlocks, (unsigned long long)up.tex_marked, (unsigned long long)up.tex_lazy_uploads,
                (unsigned long long)up.tex_readonly, (unsigned long long)up.tex_readonly_marked, (unsigned long long)up.tex_add_dirty,
                (unsigned long long)up.audit_draws, (unsigned long long)up.audit_unsent_draws, (unsigned long long)up.audit_unsent_textures,
                (unsigned long long)up.audit_dirty_draws, (unsigned long long)up.audit_dirty_textures, dirty_bound,
                (unsigned long long)up.tex_unlocks_other_thread, (unsigned long long)up.tex_readonly_written,
                (unsigned long long)up.noov_checked, (unsigned long long)up.noov_written_after_unlock, (unsigned long long)up.noov_written_bytes,
                (unsigned long long)up.deferred_n, (unsigned long long)up.pointer_n);
    HRESULT hr = flush();
    early_wait_prev = early_wait_at; early_wait_at = 0; early_sent = 0;
    { uint64_t now = g_in_call_ticks; LARGE_INTEGER f; QueryPerformanceFrequency(&f);
      if (frame_call_us.size() < 100000) frame_call_us.push_back((uint32_t)((now - last_call_ticks) * 1000000 / (uint64_t)f.QuadPart));
      last_call_ticks = now; }
    trace_channels_init();
    if (g_trace_frametime) {
      LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
      if (last_present.QuadPart)
      { char b[128]; err_line(b, snprintf(b, sizeof(b), "%04lx:trace:frametime:hbdx9_present Frame duration %u \xce\xbcs.\n", GetCurrentThreadId(),
                                           (unsigned)((t.QuadPart - last_present.QuadPart) * 1000000 / f.QuadPart))); }
      last_present = t;
    }
    if (g_trace_fps) {
      DWORD t = GetTickCount(); ++fps_frames;
      if (t - fps_prev > 1500) {
        char b[128]; err_line(b, snprintf(b, sizeof(b), "%04lx:trace:fps:hbdx9_present %p @ approx %.2ffps\n", GetCurrentThreadId(), (void *)this,
                                          1000.0 * fps_frames / (t - fps_prev)));
        fps_prev = t; fps_frames = 0;
      }
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE Present(const RECT *, const RECT *, HWND, const RGNDATA *) override { DLOCK; return present(); }
  HRESULT STDMETHODCALLTYPE PresentEx(const RECT *, const RECT *, HWND, const RGNDATA *, DWORD flags) override { DLOCK; return present(flags); }
  HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND) override { return D3D_OK; }

  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT format, D3DPOOL pool, IDirect3DSurface9 **out,
                                                        HANDLE *shared) override { DLOCK;
    if (!out || shared) return D3DERR_INVALIDCALL;
    FmtSize fs = fmt_size(format);
    if (!fs.bytes || fs.block != 1) { hbdx9_unimpl("IDirect3DDevice9::CreateOffscreenPlainSurface(format)"); return D3DERR_INVALIDCALL; }
    if (pool == D3DPOOL_DEFAULT) return create_surface(w, h, format, false, out);
    D3DSURFACE_DESC d{}; d.Format = format; d.Type = D3DRTYPE_SURFACE; d.Pool = pool; d.Width = w; d.Height = h;
    auto *s = new Hbdx9Surface(this, d);
    s->pitch = (w * fs.bytes + 3) & ~3u;
    s->memory = (BYTE *)VirtualAlloc(nullptr, (SIZE_T)s->pitch * h, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s->memory) { s->Release(); return E_OUTOFMEMORY; }
    *out = s; return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *rt_, IDirect3DSurface9 *dst) override;
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared) override;
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *shared) override;
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *shared) override;
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const RECT *dr, D3DTEXTUREFILTERTYPE filter) override;
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) override;
  HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const POINT *dp) override;
  HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) override;
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sc, IDirect3DSurface9 *dst) override;
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *pp) override;
  HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *) override { DLOCK;
    /* (DX9FULL v13, measured on Windows) ResetEx leaves the x87 control word alone where Reset sets it: do Reset's work with the
       setup suppressed */
    const DWORD saved = create_flags; create_flags |= D3DCREATE_FPU_PRESERVE;
    HRESULT hr = Reset(pp); create_flags = saved; return hr;
  }
  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT i, IDirect3DSwapChain9 **out) override;
  UINT samples_of(D3DMULTISAMPLE_TYPE ms, DWORD quality) const {
    UINT n = ms == D3DMULTISAMPLE_NONMASKABLE ? (quality ? 4 : 2) : (UINT)ms;
    if (n < 2) return 1;
    while (n > max_samples) n >>= 1;
    return n < 2 ? 1 : n;
  }
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **out) override;
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **out) override;
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **out) override;
};

/* ---------------- resources: buffers ---------------- */
struct BufInfo { UINT length; DWORD usage, fvf; D3DFORMAT format; D3DPOOL pool; };
template <typename Stub> class Hbdx9BufferT final : public Stub {
public:
  LONG refs = 1;
  Hbdx9Device *device;
  uint64_t native = 0;
  BYTE *shadow = nullptr;
  UINT lock_offset = 0, lock_size = 0; DWORD lock_flags = 0; bool locked = false;
  uint32_t ref_seq = 0;        /* (s6) SUBMIT carrying a pointer to this shadow; until the core has run it, see Lock */
  uint32_t write_gen = 0;      /* (INT32G) writing Locks so far: part of the key of a box set drawn from this buffer */

  Hbdx9BufferT(Hbdx9Device *dev, UINT len) : device(dev) { device->AddRef(); this->bi.length = len; }

  ~Hbdx9BufferT() {
    if (!device->seq_done(ref_seq)) { device->up.ref_flushes++; device->wait_seq(ref_seq); }   /* the core reads the shadow before it is freed */
    device->release_native(native, 1);
    if (shadow) VirtualFree(shadow, 0, MEM_RELEASE);
    device->Release();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DResource9) || IsEqualGUID(riid, Stub::iid())) {
      *out = this; AddRef(); return S_OK;
    }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override { if (!out) return D3DERR_INVALIDCALL; *out = device; device->AddRef(); return D3D_OK; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, void **data, DWORD flags) override {
    if (!data) return D3DERR_INVALIDCALL;
    UINT length = this->bi.length;
    if (offset > length) return D3DERR_INVALIDCALL;
    if (!size) size = length - offset;
    if (offset + size > length) size = length - offset;
    if (!uploads_old()) {
      /* D3D9 (and wined3d): DISCARD/NOOVERWRITE mean something only for DYNAMIC buffers */
      if (!(this->bi.usage & D3DUSAGE_DYNAMIC) && !no_flagmask()) flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
      /* a lock that may rewrite bytes still in use must not run ahead of a pending reference: the core reads the shadow
         at SUBMIT time.  NOOVERWRITE promises not to touch bytes in use, READONLY writes nothing. */
    }
    /* a lock that may rewrite bytes the core has not read yet waits for it (the core reads a pointer-referenced shadow when
       it runs that SUBMIT: at once in the synchronous core, later in the asynchronous one).  NOOVERWRITE promises not to touch
       bytes in use, READONLY writes nothing. */
    if (!(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) && !device->seq_done(ref_seq)) { device->up.ref_flushes++; device->wait_seq(ref_seq); }
    lock_offset = offset; lock_size = size; lock_flags = flags; locked = true;
    if (!(flags & D3DLOCK_READONLY)) write_gen++;
    *data = shadow + offset;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE Unlock() override {
    if (!locked) return D3D_OK;
    locked = false;
    /* tape v3: the locked range as the application left it, before the upload decision; its UPDATE packets get tagged */
    if (g_tape.on && lock_size) {
      Hbdx9Lock lk(&device->cs);
      tape_app_write(device->native_device, (uint32_t)device->unsent(), 2, lock_flags, native, 0, 0, lock_offset, 0, lock_size, 0,
                     0, shadow + lock_offset, lock_size);
      device->tape_app_scope++;
      unlock_upload();
      device->tape_app_scope--;
      return D3D_OK;
    }
    unlock_upload();
    return D3D_OK;
  }
  void unlock_upload() {
    if (!(lock_flags & D3DLOCK_READONLY) && lock_size) {
      if (!uploads_old() && !no_defer() && (lock_flags & D3DLOCK_NOOVERWRITE))
        ref_seq = device->upload_buffer_deferred(native, lock_offset, lock_size, lock_flags, shadow + lock_offset);
      else { uint32_t q = device->upload_buffer(native, lock_offset, lock_size, lock_flags, shadow + lock_offset); if (q) ref_seq = q; }
    } else if (lock_flags & D3DLOCK_READONLY) device->up.readonly_skips++;
  }
};
class Hbdx9VBStub : public Hbdx9Stub_IDirect3DVertexBuffer9<IDirect3DVertexBuffer9> {
public:
  BufInfo bi{};
  static const IID &iid() { return IID_IDirect3DVertexBuffer9; }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *d) override {
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = D3DFMT_VERTEXDATA; d->Type = D3DRTYPE_VERTEXBUFFER; d->Usage = bi.usage; d->Pool = bi.pool; d->Size = bi.length; d->FVF = bi.fvf;
    return D3D_OK;
  }
};
class Hbdx9IBStub : public Hbdx9Stub_IDirect3DIndexBuffer9<IDirect3DIndexBuffer9> {
public:
  BufInfo bi{};
  static const IID &iid() { return IID_IDirect3DIndexBuffer9; }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *d) override {
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = bi.format; d->Type = D3DRTYPE_INDEXBUFFER; d->Usage = bi.usage; d->Pool = bi.pool; d->Size = bi.length;
    return D3D_OK;
  }
};
using Hbdx9VB = Hbdx9BufferT<Hbdx9VBStub>;
using Hbdx9IB = Hbdx9BufferT<Hbdx9IBStub>;
/* (INT32G) HBDX9_QUERY_PREDICT: the one indexed draw inside a query is the box set it tests */
void Hbdx9Device::qp_note_indexed(D3DPRIMITIVETYPE pt, INT base, UINT min_index, UINT nverts, UINT start_index, UINT prims) {
  if (q_key_state) { q_key_state = 2; return; }
  auto *v = static_cast<Hbdx9VB *>(streams[0].vb); auto *x = static_cast<Hbdx9IB *>(ib);
  if (!v || !x) { q_key_state = 2; return; }
  memset(&q_key, 0, sizeof(q_key));
  q_key.vb = (uint64_t)(uintptr_t)v; q_key.ib = (uint64_t)(uintptr_t)x; q_key.vb_gen = v->write_gen; q_key.ib_gen = x->write_gen;
  q_key.voff = streams[0].offset; q_key.stride = streams[0].stride; q_key.prim = (uint32_t)pt; q_key.base = base;
  q_key.min_index = min_index; q_key.nverts = nverts; q_key.start_index = start_index; q_key.prims = prims;
  q_key.world = qp_fnv(&transforms[D3DTS_WORLD], sizeof(D3DMATRIX));
  q_key_state = 1;
}

static HRESULT make_buffer(Hbdx9Device *dev, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, uint64_t *native, BYTE **shadow) {
  /* pool: unused by the core, carried for the tape (a replay on another d3d9 must create the buffer in the same pool) */
  hbdx9_create_resource_params p{}; p.device = dev->native_device; p.kind = HBDX9_RES_BUFFER; p.format = fmt; p.width = len; p.usage = usage; p.pool = pool;
  if (hb_call(HBDX9_CALL_CREATE_RESOURCE, &p) || p.status) return p.status == 2 ? E_OUTOFMEMORY : D3DERR_NOTAVAILABLE;
  *shadow = (BYTE *)VirtualAlloc(nullptr, len ? len : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!*shadow) { dev->release_native(p.resource, 1); return E_OUTOFMEMORY; }
  *native = p.resource;
  return D3D_OK;
}


/* ---------------- textures (2D, cube, volume) ---------------- */
/* Shared storage of every texture kind: per (layer, level) guest shadow memory for Lock, uploads on Unlock.
 * layer = cube face (6) or 1; volumes keep slices in one shadow per level. */
struct TexCore {
  Hbdx9Device *device = nullptr;
  IUnknown *self = nullptr;            /* the COM object (refcount / QueryInterface of its surfaces) */
  uint64_t native = 0;
  UINT width = 1, height = 1, depth = 1, levels = 1, layers = 1, native_levels = 1;
  DWORD usage = 0; D3DFORMAT format = D3DFMT_UNKNOWN; D3DPOOL pool = D3DPOOL_DEFAULT;
  bool volume = false;
  std::vector<Hbdx9Surface *> surfaces;
  std::vector<BYTE *> shadow;
  std::vector<UINT> pitch, slice;
  std::vector<RECT> lock_rect; std::vector<D3DBOX> lock_box;
  std::vector<bool> locked, written;
  DWORD lod = 0;
  /* lazy uploads (MANAGED 2D/cube): dirty level rects not yet sent; bound = samplers this texture is set to */
  std::vector<RECT> dirty_rect; std::vector<bool> dirty; std::vector<DWORD> lock_flags;
  bool dirty_any = false; UINT bound = 0;
  uint32_t ptr_seq = 0;                /* (s6) last SUBMIT carrying a pointer into these shadows; see Hbdx9Device::wait_seq */
  std::vector<bool> sent;              /* (s8) the level has been sent to the core at least once */
  uint32_t locks_n = 0, readonly_n = 0; bool audit_logged = false, audit_dirty_logged = false;
  uint64_t ro_hash = 0;                /* (s8 audit) content of the rect at a READONLY Lock */
  uint64_t rect_hash(UINT i, UINT l, RECT r) const {
    FmtSize fs = fmt_size(format);
    if (!shadow[i] || r.right <= r.left || r.bottom <= r.top) return 0;
    UINT rows = ((UINT)(r.bottom - r.top) + fs.block - 1) / fs.block, row = (((UINT)(r.right - r.left) + fs.block - 1) / fs.block) * fs.bytes;
    uint64_t h = 1469598103934665603ull;
    for (UINT y = 0; y < rows; y++) h = audit_hash(shadow[i] + (r.top / fs.block + y) * pitch[i] + (r.left / fs.block) * fs.bytes, row, h);
    return h;
  }

  void init(UINT nlayers) {
    layers = nlayers; UINT n = layers * levels;
    surfaces.assign(n, nullptr); shadow.assign(n, nullptr); pitch.assign(n, 0); slice.assign(n, 0);
    lock_rect.assign(n, RECT{}); lock_box.assign(n, D3DBOX{}); locked.assign(n, false); written.assign(n, false);
    dirty_rect.assign(n, RECT{}); dirty.assign(n, false); lock_flags.assign(n, 0); sent.assign(n, false);
  }
  bool lazy() const { return pool == D3DPOOL_MANAGED && !volume && !uploads_old() && !no_lazy(); }
  ~TexCore();
  UINT lw(UINT l) const { return width >> l ? width >> l : 1; }
  UINT lh(UINT l) const { return height >> l ? height >> l : 1; }
  UINT ld(UINT l) const { return depth >> l ? depth >> l : 1; }
  UINT idx(UINT layer, UINT level) const { return layer * levels + level; }
  void level_desc(UINT l, D3DSURFACE_DESC *d) const {
    d->Format = format; d->Type = D3DRTYPE_SURFACE; d->Usage = usage; d->Pool = pool; d->MultiSampleType = D3DMULTISAMPLE_NONE;
    d->MultiSampleQuality = 0; d->Width = lw(l); d->Height = lh(l);
  }
  bool ensure_shadow(UINT i, UINT l) {
    if (shadow[i]) return true;
    FmtSize fs = fmt_size(format);
    UINT bw = (lw(l) + fs.block - 1) / fs.block, bh = (lh(l) + fs.block - 1) / fs.block;
    pitch[i] = (bw * fs.bytes + 3) & ~3u; slice[i] = pitch[i] * bh;
    shadow[i] = (BYTE *)VirtualAlloc(nullptr, (SIZE_T)slice[i] * (volume ? ld(l) : 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return shadow[i] != nullptr;
  }
  Hbdx9Surface *surface(UINT layer, UINT l);
  HRESULT lock(UINT layer, UINT l, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) {
    if (layer >= layers || l >= levels || !lr) return D3DERR_INVALIDCALL;
    UINT i = idx(layer, l);
    if (locked[i]) return D3DERR_INVALIDCALL;
    if (!(flags & D3DLOCK_READONLY)) device->wait_seq(ptr_seq);   /* the core may still read these shadows (s6) */
    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && pool == D3DPOOL_DEFAULT) {
      hbdx9_unimpl("IDirect3DTexture9::LockRect(render target)"); return D3DERR_INVALIDCALL;
    }
    FmtSize fs = fmt_size(format);
    if (!ensure_shadow(i, l)) return E_OUTOFMEMORY;
    RECT r = rect ? *rect : RECT{0, 0, (LONG)lw(l), (LONG)lh(l)};
    if (fs.block > 1) { r.left &= ~3; r.top &= ~3; r.right = (r.right + 3) & ~3; r.bottom = (r.bottom + 3) & ~3; }
    lock_rect[i] = r; locked[i] = true; lock_flags[i] = flags;
    if ((flags & D3DLOCK_READONLY) && upload_audit()) ro_hash = rect_hash(i, l, r);
    lr->Pitch = pitch[i];
    lr->pBits = shadow[i] + (r.top / fs.block) * pitch[i] + (r.left / fs.block) * fs.bytes;
    return D3D_OK;
  }
  void upload_rect(UINT layer, UINT l, RECT r);
  HRESULT unlock(UINT layer, UINT l) {
    if (layer >= layers || l >= levels) return D3DERR_INVALIDCALL;
    UINT i = idx(layer, l);
    if (!locked[i]) return D3DERR_INVALIDCALL;
    locked[i] = false;
    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH) { written[i] = true; return D3D_OK; }
    if (g_tape.on) {   /* tape v3: what the application wrote, at its place in the stream, before the shim decides anything */
      Hbdx9Lock lk(&device->cs);
      tape_texture_write(layer, l, i);
      mark_written(layer, l, lock_rect[i], lock_flags[i]);
      return D3D_OK;
    }
    mark_written(layer, l, lock_rect[i], lock_flags[i]);
    return D3D_OK;
  }
  void mark_written(UINT layer, UINT l, RECT r, DWORD flags);
  void tape_texture_write(UINT layer, UINT l, UINT i);
  /* (s8) D3D9 AddDirtyRect: for a MANAGED texture the given rect of the top level, and every lower level whole, are uploaded
     again before the next use (a game may write through a pointer it keeps, or lock with D3DLOCK_NO_DIRTY_UPDATE) */
  void add_dirty(UINT layer, const RECT *r) {
    if (layer >= layers || pool != D3DPOOL_MANAGED) return;
    device_add_dirty_count();
    if (!add_dirty_on()) return;
    for (UINT l = 0; l < levels; l++) {
      UINT i = idx(layer, l);
      if (!shadow[i]) continue;
      RECT full{0, 0, (LONG)lw(l), (LONG)lh(l)};
      mark_written(layer, l, (l == 0 && r) ? *r : full, 0);
    }
  }
  void device_add_dirty_count();
  HRESULT lock_box_(UINT l, D3DLOCKED_BOX *lb, const D3DBOX *box) {
    if (l >= levels || !lb || locked[l]) return D3DERR_INVALIDCALL;
    device->wait_seq(ptr_seq);
    if (!ensure_shadow(l, l)) return E_OUTOFMEMORY;
    D3DBOX b = box ? *box : D3DBOX{0, 0, lw(l), lh(l), 0, ld(l)};
    FmtSize fs = fmt_size(format);
    if (fs.block > 1) { b.Left &= ~3u; b.Top &= ~3u; }
    lock_box[l] = b; locked[l] = true;
    lb->RowPitch = pitch[l]; lb->SlicePitch = slice[l];
    lb->pBits = shadow[l] + b.Front * slice[l] + (b.Top / fs.block) * pitch[l] + (b.Left / fs.block) * fs.bytes;
    return D3D_OK;
  }
  HRESULT unlock_box_(UINT l);
  /* (DX9FULL v12) IDirect3DBaseTexture9::SetLOD: MANAGED textures only (D3D9, wined3d; other pools return 0 and nothing changes);
     the core clamps sampling to this level (as DXVK's sample view with base level = LOD).  Without HBDX9_FMT_EXT, or with a core
     that does not know the packet, the level is only stored (s8j). */
  DWORD set_lod(DWORD l) {
    if (fmt_ext_on() && pool != D3DPOOL_MANAGED) return 0;
    DWORD o = lod; lod = l < levels ? l : levels - 1;
    if (lod != o && native && fmt_ext_on() && (device->core_caps & HBDX9_CAP_TEXTURE_LOD))
      device->pkt_object(HBDX9_PKT_TEXTURE_LOD, lod, native);
    return o;
  }
  void upload_all() {   /* UpdateTexture destination: every level that has data in a source */
    for (UINT layer = 0; layer < layers; layer++) for (UINT l = 0; l < levels; l++) upload_rect(layer, l, RECT{0, 0, (LONG)lw(l), (LONG)lh(l)});
  }
};

class Hbdx9Texture final : public Hbdx9Stub_IDirect3DTexture9<IDirect3DTexture9> {
public:
  LONG refs = 1;
  TexCore c;
  explicit Hbdx9Texture(Hbdx9Device *dev);
  ~Hbdx9Texture();
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DResource9) || IsEqualGUID(riid, IID_IDirect3DBaseTexture9) ||
        IsEqualGUID(riid, IID_IDirect3DTexture9)) { *out = this; AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override;
  DWORD STDMETHODCALLTYPE SetLOD(DWORD l) override { return c.set_lod(l); }
  DWORD STDMETHODCALLTYPE GetLOD() override { return c.lod; }
  DWORD STDMETHODCALLTYPE GetLevelCount() override { return c.levels; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override { return D3D_OK; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_LINEAR; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() override;
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *r) override { c.add_dirty(0, r); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT l, D3DSURFACE_DESC *d) override { if (l >= c.levels || !d) return D3DERR_INVALIDCALL; c.level_desc(l, d); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT l, IDirect3DSurface9 **out) override {
    if (l >= c.levels || !out) return D3DERR_INVALIDCALL;
    *out = c.surface(0, l); (*out)->AddRef(); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LockRect(UINT l, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override { return c.lock(0, l, lr, rect, flags); }
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT l) override { return c.unlock(0, l); }
};

class Hbdx9CubeTexture final : public Hbdx9Stub_IDirect3DCubeTexture9<IDirect3DCubeTexture9> {
public:
  LONG refs = 1;
  TexCore c;
  explicit Hbdx9CubeTexture(Hbdx9Device *dev);
  ~Hbdx9CubeTexture();
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DResource9) || IsEqualGUID(riid, IID_IDirect3DBaseTexture9) ||
        IsEqualGUID(riid, IID_IDirect3DCubeTexture9)) { *out = this; AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_CUBETEXTURE; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override;
  DWORD STDMETHODCALLTYPE SetLOD(DWORD l) override { return c.set_lod(l); }
  DWORD STDMETHODCALLTYPE GetLOD() override { return c.lod; }
  DWORD STDMETHODCALLTYPE GetLevelCount() override { return c.levels; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override { return D3D_OK; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_LINEAR; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() override;
  HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES f, const RECT *r) override { c.add_dirty((UINT)f, r); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT l, D3DSURFACE_DESC *d) override { if (l >= c.levels || !d) return D3DERR_INVALIDCALL; c.level_desc(l, d); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES f, UINT l, IDirect3DSurface9 **out) override {
    if ((UINT)f >= 6 || l >= c.levels || !out) return D3DERR_INVALIDCALL;
    *out = c.surface((UINT)f, l); (*out)->AddRef(); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES f, UINT l, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override { return c.lock((UINT)f, l, lr, rect, flags); }
  HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES f, UINT l) override { return c.unlock((UINT)f, l); }
};

class Hbdx9VolumeTexture;
class Hbdx9Volume final : public Hbdx9Stub_IDirect3DVolume9<IDirect3DVolume9> {
public:
  Hbdx9VolumeTexture *container; UINT level;
  Hbdx9Volume(Hbdx9VolumeTexture *c, UINT l) : container(c), level(l) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DVolume9)) { *out = this; AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override;
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **out) override;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC *d) override;
  HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD flags) override;
  HRESULT STDMETHODCALLTYPE UnlockBox() override;
};

class Hbdx9VolumeTexture final : public Hbdx9Stub_IDirect3DVolumeTexture9<IDirect3DVolumeTexture9> {
public:
  LONG refs = 1;
  TexCore c;
  std::vector<Hbdx9Volume *> volumes;
  explicit Hbdx9VolumeTexture(Hbdx9Device *dev);
  ~Hbdx9VolumeTexture();
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DResource9) || IsEqualGUID(riid, IID_IDirect3DBaseTexture9) ||
        IsEqualGUID(riid, IID_IDirect3DVolumeTexture9)) { *out = this; AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  DWORD STDMETHODCALLTYPE SetLOD(DWORD l) override { return c.set_lod(l); }
  DWORD STDMETHODCALLTYPE GetLOD() override { return c.lod; }
  DWORD STDMETHODCALLTYPE GetLevelCount() override { return c.levels; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override { return D3D_OK; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_LINEAR; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() override {}
  HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX *) override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT l, D3DVOLUME_DESC *d) override {
    if (l >= c.levels || !d) return D3DERR_INVALIDCALL;
    d->Format = c.format; d->Type = D3DRTYPE_VOLUME; d->Usage = c.usage; d->Pool = c.pool; d->Width = c.lw(l); d->Height = c.lh(l); d->Depth = c.ld(l);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT l, IDirect3DVolume9 **out) override {
    if (l >= c.levels || !out) return D3DERR_INVALIDCALL;
    if (!volumes[l]) volumes[l] = new Hbdx9Volume(this, l);
    *out = volumes[l]; (*out)->AddRef(); return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LockBox(UINT l, D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD) override { return c.lock_box_(l, lb, box); }
  HRESULT STDMETHODCALLTYPE UnlockBox(UINT l) override { return c.unlock_box_(l); }
};

static TexCore *tex_core(IDirect3DBaseTexture9 *t) {
  if (!t) return nullptr;
  switch (t->GetType()) {
  case D3DRTYPE_TEXTURE: return &static_cast<Hbdx9Texture *>(t)->c;
  case D3DRTYPE_CUBETEXTURE: return &static_cast<Hbdx9CubeTexture *>(t)->c;
  case D3DRTYPE_VOLUMETEXTURE: return &static_cast<Hbdx9VolumeTexture *>(t)->c;
  default: return nullptr;
  }
}

/* ---------------- textures and surfaces (out of line) ---------------- */
TexCore::~TexCore() {
  if (device) device->wait_seq(ptr_seq);   /* the core may still read the shadows freed below (s6) */
  for (auto *sf : surfaces) if (sf) { sf->container = nullptr; sf->Release(); }
  for (auto *m : shadow) if (m) VirtualFree(m, 0, MEM_RELEASE);
  if (device) { device->release_native(native, 1); device->Release(); }
}
Hbdx9Surface *TexCore::surface(UINT layer, UINT l) {
  UINT i = idx(layer, l);
  if (!surfaces[i]) {
    D3DSURFACE_DESC d; level_desc(l, &d);
    auto *sf = new Hbdx9Surface(device, d);
    sf->native = native; sf->level = l; sf->face = layer; sf->container = this; sf->owns_native = false;
    surfaces[i] = sf;
  }
  return surfaces[i];
}
void TexCore::upload_rect(UINT layer, UINT l, RECT r) {
  UINT i = idx(layer, l);
  if (!shadow[i] || !native) return;
  FmtSize fs = fmt_size(format);
  if (r.right > (LONG)lw(l)) r.right = lw(l);
  if (r.bottom > (LONG)lh(l)) r.bottom = lh(l);
  if (r.right <= r.left || r.bottom <= r.top) return;
  UINT w = (UINT)(r.right - r.left), h = (UINT)(r.bottom - r.top);
  UINT rows = (h + fs.block - 1) / fs.block;
  const BYTE *src = shadow[i] + (r.top / fs.block) * pitch[i] + (r.left / fs.block) * fs.bytes;
  UINT size = rows ? (rows - 1) * pitch[i] + ((w + fs.block - 1) / fs.block) * fs.bytes : 0;
  device->tape_app_scope++;
  { uint32_t q = device->upload_texture(native, l, layer, r.left, r.top, w, h, pitch[i], size, src); if (q) ptr_seq = q; }
  device->tape_app_scope--;
  sent[i] = true;
}
/* tape v3: the locked rect of a texture level as the application left it at Unlock (READONLY locks included) */
void TexCore::tape_texture_write(UINT layer, UINT l, UINT i) {
  if (!shadow[i] || !native) return;
  FmtSize fs = fmt_size(format);
  RECT r = lock_rect[i];
  if (r.right > (LONG)lw(l)) r.right = lw(l);
  if (r.bottom > (LONG)lh(l)) r.bottom = lh(l);
  if (r.right <= r.left || r.bottom <= r.top) return;
  UINT rows = ((UINT)(r.bottom - r.top) + fs.block - 1) / fs.block, row = (((UINT)(r.right - r.left) + fs.block - 1) / fs.block) * fs.bytes;
  const BYTE *src = shadow[i] + (r.top / fs.block) * pitch[i] + (r.left / fs.block) * fs.bytes;
  tape_app_write(device->native_device, (uint32_t)device->unsent(), 1, lock_flags[i], native, l, layer, (uint32_t)r.left,
                 (uint32_t)r.top, (uint32_t)(r.right - r.left), (uint32_t)(r.bottom - r.top), pitch[i], src, (rows - 1) * pitch[i] + row);
}
void TexCore::mark_written(UINT layer, UINT l, RECT r, DWORD flags) {
  UINT i = idx(layer, l);
  written[i] = true;
  if (uploads_old()) { upload_rect(layer, l, r); return; }
  device->up.tex_unlocks++; locks_n++;
  if (device->up.present_thread && GetCurrentThreadId() != device->up.present_thread) device->up.tex_unlocks_other_thread++;
  if (flags & D3DLOCK_READONLY) {
    device->up.tex_readonly++; readonly_n++;
    if (upload_audit() && rect_hash(i, l, r) != ro_hash) {
      device->up.tex_readonly_written++;
      if (device->up.tex_readonly_written <= 16)
        hbdx9_log("upload-audit READONLY lock WRITTEN: texture fmt=%08lx %ux%u levels=%u level=%u rect=%ld,%ld-%ld,%ld",
                  (unsigned long)format, width, height, levels, l, r.left, r.top, r.right, r.bottom);
    }
    if (upload_audit() && device->up.tex_readonly <= 24)
      hbdx9_log("upload-audit READONLY unlock: texture fmt=%08lx %ux%u levels=%u usage=%08lx pool=%u layer=%u level=%u rect=%ld,%ld-%ld,%ld",
                (unsigned long)format, width, height, levels, (unsigned long)usage, (unsigned)pool, layer, l, r.left, r.top, r.right, r.bottom);
    if (readonly_skip()) { device->up.readonly_skips++; return; }
    device->up.tex_readonly_marked++;
  }
  if (!lazy()) { upload_rect(layer, l, r); return; }
  device->up.tex_marked++;
  if (!dirty[i]) { dirty[i] = true; dirty_rect[i] = r; }
  else {
    RECT &d = dirty_rect[i];
    if (r.left < d.left) d.left = r.left;
    if (r.top < d.top) d.top = r.top;
    if (r.right > d.right) d.right = r.right;
    if (r.bottom > d.bottom) d.bottom = r.bottom;
  }
  if (!dirty_any) { dirty_any = true; if (bound) device->dirty_bound++; }
}
void Hbdx9Device::upload_dirty(TexCore *c) { DLOCK;
  if (!c || !c->dirty_any) return;
  c->dirty_any = false;
  if (c->bound) dirty_bound--;
  for (UINT layer = 0; layer < c->layers; layer++)
    for (UINT l = 0; l < c->levels; l++) {
      UINT i = c->idx(layer, l);
      if (!c->dirty[i]) continue;
      c->dirty[i] = false; up.tex_lazy_uploads++;
      c->upload_rect(layer, l, c->dirty_rect[i]);
    }
}
/* before a draw: the lazy levels of every bound texture go first, so the draw samples them (stream order) */
void Hbdx9Device::pre_draw() {
  for (int i = 0; i < 20 && dirty_bound; i++)
    if (tex[i]) { TexCore *c = tex_core(tex[i]); if (c && c->dirty_any) upload_dirty(c); }
}
void TexCore::device_add_dirty_count() { device->up.tex_add_dirty++; }
/* (s8) HBDX9_UPLOAD_AUDIT: a bound MANAGED texture with a level the application wrote (locked) but that never reached the core */
void Hbdx9Device::audit_draw() {
  up.audit_draws++;
  bool any = false;
  bool dirty_seen = false;
  for (int t = 0; t < 20; t++) {
    TexCore *c = tex[t] ? tex_core(tex[t]) : nullptr;
    if (!c || c->pool != D3DPOOL_MANAGED || !c->native) continue;
    if (c->dirty_any) {
      dirty_seen = true;
      if (!c->audit_dirty_logged) {
        c->audit_dirty_logged = true; up.audit_dirty_textures++;
        if (up.audit_dirty_textures <= 24)
          hbdx9_log("upload-audit DIRTY-AT-DRAW: sampler %d texture fmt=%08lx %ux%u levels=%u bound=%u dirty_bound=%u locks=%u frame=%u",
                    t, (unsigned long)c->format, c->width, c->height, c->levels, c->bound, dirty_bound, c->locks_n, frames);
      }
    }
    for (UINT i = 0; i < c->layers * c->levels; i++) {
      if (!c->written[i] || c->sent[i] || c->dirty[i]) continue;
      any = true;
      if (!c->audit_logged) {
        c->audit_logged = true; up.audit_unsent_textures++;
        if (up.audit_unsent_textures <= 24)
          hbdx9_log("upload-audit UNSENT: sampler %d texture fmt=%08lx %ux%u levels=%u layers=%u usage=%08lx index=%u locks=%u readonly_locks=%u frame=%u",
                    t, (unsigned long)c->format, c->width, c->height, c->levels, c->layers, (unsigned long)c->usage, i, c->locks_n, c->readonly_n, frames);
      }
      break;
    }
  }
  if (any) up.audit_unsent_draws++;
  if (dirty_seen) up.audit_dirty_draws++;
}
/* the one place sampler bindings change (SetTexture, Reset): keeps TexCore::bound and dirty_bound exact */
__attribute__((always_inline)) inline void Hbdx9Device::bind_texture(int i, IDirect3DBaseTexture9 *t) {
  if (tex[i] == t) return;
  if (t) {
    t->AddRef();
    if (TexCore *c = tex_core(t)) { if (!c->bound++ && c->dirty_any) dirty_bound++; }
  }
  if (tex[i]) {
    if (TexCore *c = tex_core(tex[i])) { if (!--c->bound && c->dirty_any) dirty_bound--; }
    tex[i]->Release();
  }
  tex[i] = t; sb_note(SB_TEX, i);
}
HRESULT TexCore::unlock_box_(UINT l) {
  if (l >= levels || !locked[l]) return D3DERR_INVALIDCALL;
  locked[l] = false; written[l] = true;
  if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH || !native) return D3D_OK;
  sent[l] = true;
  D3DBOX b = lock_box[l];
  if (b.Right > lw(l)) b.Right = lw(l);
  if (b.Bottom > lh(l)) b.Bottom = lh(l);
  if (b.Back > ld(l)) b.Back = ld(l);
  FmtSize fs = fmt_size(format);
  UINT w = b.Right - b.Left, h = b.Bottom - b.Top, rows = (h + fs.block - 1) / fs.block;
  UINT size = rows ? (rows - 1) * pitch[l] + ((w + fs.block - 1) / fs.block) * fs.bytes : 0;
  for (UINT z = b.Front; z < b.Back; z++)
  { uint32_t q = device->upload_texture(native, l, z, b.Left, b.Top, w, h, pitch[l], size, shadow[l] + z * slice[l] + (b.Top / fs.block) * pitch[l] + (b.Left / fs.block) * fs.bytes); if (q) ptr_seq = q; }
  return D3D_OK;
}
Hbdx9Texture::Hbdx9Texture(Hbdx9Device *dev) { c.device = dev; c.self = this; dev->AddRef(); }
Hbdx9Texture::~Hbdx9Texture() {}
HRESULT STDMETHODCALLTYPE Hbdx9Texture::GetDevice(IDirect3DDevice9 **out) { if (!out) return D3DERR_INVALIDCALL; *out = c.device; c.device->AddRef(); return D3D_OK; }
void STDMETHODCALLTYPE Hbdx9Texture::GenerateMipSubLevels() { if (c.native) { c.device->upload_dirty(&c); c.device->pkt_object(HBDX9_PKT_GENERATE_MIPS, 0, c.native); } }
void STDMETHODCALLTYPE Hbdx9Texture::PreLoad() { c.device->upload_dirty(&c); }
void STDMETHODCALLTYPE Hbdx9CubeTexture::PreLoad() { c.device->upload_dirty(&c); }
Hbdx9CubeTexture::Hbdx9CubeTexture(Hbdx9Device *dev) { c.device = dev; c.self = this; dev->AddRef(); }
Hbdx9CubeTexture::~Hbdx9CubeTexture() {}
HRESULT STDMETHODCALLTYPE Hbdx9CubeTexture::GetDevice(IDirect3DDevice9 **out) { if (!out) return D3DERR_INVALIDCALL; *out = c.device; c.device->AddRef(); return D3D_OK; }
void STDMETHODCALLTYPE Hbdx9CubeTexture::GenerateMipSubLevels() { if (c.native) { c.device->upload_dirty(&c); c.device->pkt_object(HBDX9_PKT_GENERATE_MIPS, 0, c.native); } }
Hbdx9VolumeTexture::Hbdx9VolumeTexture(Hbdx9Device *dev) { c.device = dev; c.self = this; c.volume = true; dev->AddRef(); }
Hbdx9VolumeTexture::~Hbdx9VolumeTexture() { for (auto *v : volumes) delete v; }
HRESULT STDMETHODCALLTYPE Hbdx9VolumeTexture::GetDevice(IDirect3DDevice9 **out) { if (!out) return D3DERR_INVALIDCALL; *out = c.device; c.device->AddRef(); return D3D_OK; }
ULONG STDMETHODCALLTYPE Hbdx9Volume::AddRef() { return container->AddRef(); }
ULONG STDMETHODCALLTYPE Hbdx9Volume::Release() { return container->Release(); }
HRESULT STDMETHODCALLTYPE Hbdx9Volume::GetDevice(IDirect3DDevice9 **out) { return container->GetDevice(out); }
HRESULT STDMETHODCALLTYPE Hbdx9Volume::GetContainer(REFIID riid, void **out) { return container->QueryInterface(riid, out); }
HRESULT STDMETHODCALLTYPE Hbdx9Volume::GetDesc(D3DVOLUME_DESC *d) { return container->GetLevelDesc(level, d); }
HRESULT STDMETHODCALLTYPE Hbdx9Volume::LockBox(D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD) { return container->c.lock_box_(level, lb, box); }
HRESULT STDMETHODCALLTYPE Hbdx9Volume::UnlockBox() { return container->c.unlock_box_(level); }

Hbdx9Surface::~Hbdx9Surface() {
  if (memory && ptr_seq) static_cast<Hbdx9Device *>(device)->wait_seq(ptr_seq);
  if (memory) VirtualFree(memory, 0, MEM_RELEASE);
  if (owns_native && native) static_cast<Hbdx9Device *>(device)->release_native(native, 1);
  device->Release();
}
ULONG STDMETHODCALLTYPE Hbdx9Surface::AddRef() { if (container) container->self->AddRef(); return InterlockedIncrement(&refs); }
ULONG STDMETHODCALLTYPE Hbdx9Surface::Release() {
  TexCore *c = container;
  LONG r = InterlockedDecrement(&refs);
  if (c) { c->self->Release(); return r; }  /* level surfaces live as long as their texture */
  if (!r) delete this;
  return r;
}
HRESULT STDMETHODCALLTYPE Hbdx9Surface::GetContainer(REFIID riid, void **out) {
  if (container) return container->self->QueryInterface(riid, out);
  return device->QueryInterface(riid, out);
}
HRESULT STDMETHODCALLTYPE Hbdx9Surface::LockRect(D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) {
  if (container) return container->lock(face, level, lr, rect, flags);
  if (!lr) return D3DERR_INVALIDCALL;
  if (locked) return D3DERR_INVALIDCALL;
  FmtSize fs = fmt_size(desc.Format);
  if (!memory) {
    /* a GPU surface (lockable render target, back buffer, DEFAULT offscreen plain): synchronous readback, upload on unlock */
    if (!native || samples > 1 || is_depth(desc.Format) || fs.block != 1 || !fs.bytes) { hbdx9_unimpl("IDirect3DSurface9::LockRect(GPU surface)"); return D3DERR_INVALIDCALL; }
    pitch = (desc.Width * fs.bytes + 3) & ~3u;
    memory = (BYTE *)VirtualAlloc(nullptr, (SIZE_T)pitch * desc.Height, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory) return E_OUTOFMEMORY;
    gpu_shadow = true;
  }
  if (gpu_shadow) {
    auto *dev = static_cast<Hbdx9Device *>(device);
    Hbdx9Lock l(&dev->cs, dev->locking);
    if (FAILED(dev->flush())) return D3DERR_DRIVERINTERNALERROR;
    hbdx9_readback_params r{dev->native_device, native, (uint64_t)(uintptr_t)memory, pitch, desc.Width, desc.Height, level, 0, 0};
    if (hb_call(HBDX9_CALL_READBACK, &r) || r.status) return D3DERR_DRIVERINTERNALERROR;
  }
  if (!(flags & D3DLOCK_READONLY) && ptr_seq) static_cast<Hbdx9Device *>(device)->wait_seq(ptr_seq);   /* (s6) */
  BYTE *p = memory;
  if (rect) p += rect->top * pitch + rect->left * fs.bytes;
  lr->pBits = p; lr->Pitch = pitch; locked = true; lock_flags = flags;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Surface::UnlockRect() {
  if (container) return container->unlock(face, level);
  if (!locked) return D3DERR_INVALIDCALL;
  locked = false;
  if (gpu_shadow && !(lock_flags & D3DLOCK_READONLY)) {
    FmtSize fs = fmt_size(desc.Format);
    uint32_t q = static_cast<Hbdx9Device *>(device)->upload_texture(native, level, face, 0, 0, desc.Width, desc.Height, pitch,
                                                                     pitch * (desc.Height - 1) + desc.Width * fs.bytes, memory);
    if (q) ptr_seq = q;
  }
  return D3D_OK;
}

static HRESULT create_texture_native(Hbdx9Device *dev, UINT kind, UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT fmt,
                                     D3DPOOL pool, uint64_t *native, UINT *out_levels) {
  *native = 0;
  UINT full = 1; for (UINT m = w > h ? w : h, dd = depth; (m > dd ? m : dd) > 1; m >>= 1, dd >>= 1) full++;
  *out_levels = (!levels || levels > full) ? full : levels;
  if (usage & D3DUSAGE_AUTOGENMIPMAP) *out_levels = 1;
  if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH) return D3D_OK; /* guest memory only (UpdateTexture sources) */
  hbdx9_create_resource_params p{}; p.device = dev->native_device; p.kind = kind; p.format = fmt; p.width = w; p.height = h;
  p.depth = depth; p.levels = (usage & D3DUSAGE_AUTOGENMIPMAP) ? 0 : *out_levels; p.usage = usage; p.pool = pool;
  if (hb_call(HBDX9_CALL_CREATE_RESOURCE, &p) || p.status) return p.status == 2 ? E_OUTOFMEMORY : D3DERR_INVALIDCALL;
  *native = p.resource;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Hbdx9Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                     IDirect3DTexture9 **out, HANDLE *shared) { DLOCK;
  if (!out || shared || !w || !h) return D3DERR_INVALIDCALL;
  if (!fmt_size(fmt).bytes) { FMT_TRACE("CreateTexture", fmt, usage, D3DRTYPE_TEXTURE, D3DERR_INVALIDCALL); hbdx9_unsupported_format("CreateTexture", fmt); return D3DERR_INVALIDCALL; }
  uint64_t native; UINT nl;
  HRESULT hr = create_texture_native(this, HBDX9_RES_TEXTURE_2D, w, h, 1, levels, usage, fmt, pool, &native, &nl);
  FMT_TRACE("CreateTexture", fmt, usage, D3DRTYPE_TEXTURE, hr);
  if (FAILED(hr)) return hr;
  auto *t = new Hbdx9Texture(this);
  t->c.native = native; t->c.width = w; t->c.height = h; t->c.levels = nl; t->c.usage = usage; t->c.format = fmt; t->c.pool = pool;
  t->c.init(1);
  *out = t; return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                         IDirect3DCubeTexture9 **out, HANDLE *shared) { DLOCK;
  if (!out || shared || !edge) return D3DERR_INVALIDCALL;
  if (!fmt_size(fmt).bytes) { FMT_TRACE("CreateCubeTexture", fmt, usage, D3DRTYPE_CUBETEXTURE, D3DERR_INVALIDCALL); hbdx9_unsupported_format("CreateCubeTexture", fmt); return D3DERR_INVALIDCALL; }
  uint64_t native; UINT nl;
  HRESULT hr = create_texture_native(this, HBDX9_RES_TEXTURE_CUBE, edge, edge, 1, levels, usage, fmt, pool, &native, &nl);
  FMT_TRACE("CreateCubeTexture", fmt, usage, D3DRTYPE_CUBETEXTURE, hr);
  if (FAILED(hr)) return hr;
  auto *t = new Hbdx9CubeTexture(this);
  t->c.native = native; t->c.width = t->c.height = edge; t->c.levels = nl; t->c.usage = usage; t->c.format = fmt; t->c.pool = pool;
  t->c.init(6);
  *out = t; return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::CreateVolumeTexture(UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT fmt,
                                                           D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *shared) { DLOCK;
  if (!out || shared || !w || !h || !depth) return D3DERR_INVALIDCALL;
  if (!fmt_size(fmt).bytes || is_depth(fmt)) { FMT_TRACE("CreateVolumeTexture", fmt, usage, D3DRTYPE_VOLUMETEXTURE, D3DERR_INVALIDCALL); hbdx9_unsupported_format("CreateVolumeTexture", fmt); return D3DERR_INVALIDCALL; }
  uint64_t native; UINT nl;
  HRESULT hr = create_texture_native(this, HBDX9_RES_TEXTURE_3D, w, h, depth, levels, usage & ~D3DUSAGE_AUTOGENMIPMAP, fmt, pool, &native, &nl);
  FMT_TRACE("CreateVolumeTexture", fmt, usage, D3DRTYPE_VOLUMETEXTURE, hr);
  if (FAILED(hr)) return hr;
  auto *t = new Hbdx9VolumeTexture(this);
  t->c.native = native; t->c.width = w; t->c.height = h; t->c.depth = depth; t->c.levels = nl; t->c.usage = usage; t->c.format = fmt; t->c.pool = pool;
  t->c.init(1);
  t->volumes.assign(nl, nullptr);
  *out = t; return D3D_OK;
}

/* ---------------- vertex declarations ---------------- */
class Hbdx9Decl final : public Hbdx9Stub_IDirect3DVertexDeclaration9<IDirect3DVertexDeclaration9> {
public:
  LONG refs = 1;
  IDirect3DDevice9 *device;
  std::vector<D3DVERTEXELEMENT9> elements;  /* including D3DDECL_END */
  explicit Hbdx9Decl(IDirect3DDevice9 *d) : device(d) { device->AddRef(); }
  ~Hbdx9Decl() { device->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DVertexDeclaration9)) { *out = this; AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override { if (!out) return D3DERR_INVALIDCALL; *out = device; device->AddRef(); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *e, UINT *n) override {
    if (!n) return D3DERR_INVALIDCALL;
    if (e) memcpy(e, elements.data(), elements.size() * sizeof(D3DVERTEXELEMENT9));
    *n = (UINT)elements.size(); return D3D_OK;
  }
};

/* ---------------- shaders ---------------- */
template <typename Stub> class Hbdx9ShaderT final : public Stub {
public:
  LONG refs = 1;
  Hbdx9Device *device;
  uint64_t native = 0;
  std::vector<DWORD> tokens;
  explicit Hbdx9ShaderT(Hbdx9Device *d) : device(d) { device->AddRef(); }
  ~Hbdx9ShaderT() { device->release_native(native, 2); device->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, Stub::iid())) { *out = this; AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override { if (!out) return D3DERR_INVALIDCALL; *out = device; device->AddRef(); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetFunction(void *data, UINT *size) override {
    if (!size) return D3DERR_INVALIDCALL;
    UINT bytes = (UINT)(tokens.size() * 4);
    if (data) { if (*size < bytes) return D3DERR_INVALIDCALL; memcpy(data, tokens.data(), bytes); }
    *size = bytes; return D3D_OK;
  }
};
struct Hbdx9VSStub : public Hbdx9Stub_IDirect3DVertexShader9<IDirect3DVertexShader9> { static const IID &iid() { return IID_IDirect3DVertexShader9; } };
struct Hbdx9PSStub : public Hbdx9Stub_IDirect3DPixelShader9<IDirect3DPixelShader9> { static const IID &iid() { return IID_IDirect3DPixelShader9; } };
using Hbdx9VS = Hbdx9ShaderT<Hbdx9VSStub>;
using Hbdx9PS = Hbdx9ShaderT<Hbdx9PSStub>;

/* ---------------- device methods needing the classes above ---------------- */
HRESULT Hbdx9Device::init(HWND focus, D3DPRESENT_PARAMETERS *params) {
  pp = *params;
  HWND window = pp.hDeviceWindow ? pp.hDeviceWindow : focus;
  pp.hDeviceWindow = window;
  if (!pp.BackBufferWidth || !pp.BackBufferHeight) {
    RECT r; GetClientRect(window, &r);
    if (!pp.BackBufferWidth) pp.BackBufferWidth = r.right > 0 ? r.right : 640;
    if (!pp.BackBufferHeight) pp.BackBufferHeight = r.bottom > 0 ? r.bottom : 480;
  }
  if (pp.BackBufferFormat == D3DFMT_UNKNOWN) pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  if (!pp.BackBufferCount) pp.BackBufferCount = 1;
  *params = pp;
  if (!pp.Windowed) hbdx9_fs_enter(fs, window, pp.BackBufferWidth, pp.BackBufferHeight, pp.FullScreen_RefreshRateInHz); else hbdx9_fs_leave(fs);   /* (FULLSCREEN) */
  hbdx9_swapchain_params sc{};
  sc.device = native_device; sc.hwnd = (uint64_t)(uintptr_t)window;
  sc.width = pp.BackBufferWidth; sc.height = pp.BackBufferHeight; sc.format = pp.BackBufferFormat;
  sc.depth_format = pp.EnableAutoDepthStencil ? pp.AutoDepthStencilFormat : 0;
  UINT samples = samples_of(pp.MultiSampleType, pp.MultiSampleQuality);
  sc.samples = samples; sc.interval = pp.PresentationInterval;
  /* (DX9FULL 13.11.2026) presentation pacing as DXVK's d3d9 swap chain: HBDX9_PRESENT_INTERVAL=0..4 replaces the application's
     interval (d3d9.presentInterval; 0 = IMMEDIATE), HBDX9_MAX_FRAME_RATE=n limits to n frames a second (d3d9.maxFrameRate n > 0);
     under HBDX9_FMT_EXT a full-screen device with vsync is held to its FullScreen_RefreshRateInHz / interval (DXVK limits a
     full-screen game with vsync to the display mode's refresh; the Mac display does not switch modes, so the requested mode's rate
     is kept by the limiter, which the core engages only below what display sync already gives) */
  {
    char b[16]; DWORD n = GetEnvironmentVariableA("HBDX9_PRESENT_INTERVAL", b, sizeof(b));
    if (n && n < sizeof(b) && b[0] >= '0' && b[0] <= '4' && !b[1]) {
      static const UINT iv[5] = {D3DPRESENT_INTERVAL_IMMEDIATE, D3DPRESENT_INTERVAL_ONE, D3DPRESENT_INTERVAL_TWO, D3DPRESENT_INTERVAL_THREE, D3DPRESENT_INTERVAL_FOUR};
      sc.interval = iv[b[0] - '0'];
    }
    double fps = 0; n = GetEnvironmentVariableA("HBDX9_MAX_FRAME_RATE", b, sizeof(b));
    if (n && n < sizeof(b)) fps = atof(b);
    if (fps > 0) sc.pace_mhz = 0x80000000u | (uint32_t)std::min(fps * 1000.0, 2.0e9);
    else if (fmt_ext_on() && !pp.Windowed && sc.interval != D3DPRESENT_INTERVAL_IMMEDIATE && pp.FullScreen_RefreshRateInHz) {
      UINT vb = sc.interval == D3DPRESENT_INTERVAL_TWO ? 2 : sc.interval == D3DPRESENT_INTERVAL_THREE ? 3 : sc.interval == D3DPRESENT_INTERVAL_FOUR ? 4 : 1;
      sc.pace_mhz = (uint32_t)(pp.FullScreen_RefreshRateInHz * 1000u / vb);
    }
  }
  NTSTATUS st = hb_call(HBDX9_CALL_SWAPCHAIN_CREATE, &sc);
  if (st || sc.status) { hbdx9_log("swapchain create failed st=%08lx status=%u", (unsigned long)st, sc.status); return D3DERR_NOTAVAILABLE; }
  native_swapchain = sc.swapchain;
  /* (INT32G fix, patch 0006) the call above drained the core: the published sequence is its count of SUBMITs so far (those of
     an earlier device of the same IDirect3D9 object); this device's sequence continues from there */
  if (fence_page) { uint32_t now = fence_page[1023]; seq_base += now - submits; submits = now; }
  D3DSURFACE_DESC d{}; d.Format = pp.BackBufferFormat; d.Type = D3DRTYPE_SURFACE; d.Usage = D3DUSAGE_RENDERTARGET;
  d.Pool = D3DPOOL_DEFAULT; d.Width = pp.BackBufferWidth; d.Height = pp.BackBufferHeight;
  if (samples > 1) { d.MultiSampleType = pp.MultiSampleType; d.MultiSampleQuality = pp.MultiSampleQuality; }
  backbuffer = new Hbdx9Surface(this, d); backbuffer->native = sc.backbuffer; backbuffer->owns_native = false; /* swapchain owns it */
  backbuffer->samples = samples;
  rt[0] = backbuffer; rt[0]->AddRef();
  if (sc.depth) {
    D3DSURFACE_DESC dd = d; dd.Format = pp.AutoDepthStencilFormat; dd.Usage = D3DUSAGE_DEPTHSTENCIL;
    auto_depth = new Hbdx9Surface(this, dd); auto_depth->native = sc.depth; auto_depth->owns_native = false; auto_depth->samples = samples;
    ds = auto_depth; ds->AddRef();
  }
  /* Self-references: surfaces AddRef the device; drop the device's own count for its back buffer and auto depth. */
  Release(); if (auto_depth) Release();
  viewport = {0, 0, pp.BackBufferWidth, pp.BackBufferHeight, 0.0f, 1.0f};
  scissor = {0, 0, (LONG)pp.BackBufferWidth, (LONG)pp.BackBufferHeight};
  hbdx9_log("device ready %ux%u fmt=%u depth=%u samples=%u interval=%lx windowed=%d flags=%lx hwnd=%p", pp.BackBufferWidth,
            pp.BackBufferHeight, (unsigned)pp.BackBufferFormat, (unsigned)sc.depth_format, samples, (unsigned long)pp.PresentationInterval,
            pp.Windowed, (unsigned long)creation.BehaviorFlags, window);
  if (sc.interval != pp.PresentationInterval || sc.pace_mhz)
    hbdx9_log("present pacing interval=%lx (application %lx) pace_mhz=%#x fullscreen_refresh=%u", (unsigned long)sc.interval,
              (unsigned long)pp.PresentationInterval, sc.pace_mhz, pp.FullScreen_RefreshRateInHz);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Hbdx9Device::SetTexture(DWORD s, IDirect3DBaseTexture9 *t) { DLOCK;
  int i = sampler_index(s); if (i < 0) return D3DERR_INVALIDCALL;
  if (rec) rec->tex[i] = 1;
  if (tex[i] == t) return D3D_OK;
  uint64_t native = 0;
  if (t) {
    TexCore *c = tex_core(t);
    if (!c) return D3DERR_INVALIDCALL;
    native = c->native;
  }
  bind_texture(i, t);
  pkt_object(HBDX9_PKT_TEXTURE, i, native);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Hbdx9Device::SetFVF(DWORD f) { DLOCK;
  if (rec) rec->decl = 1;
  fvf = f;
  if (decl) { decl->Release(); decl = nullptr; }
  sb_note(SB_DECL, 0);
  auto *p = add<hbdx9_pkt_fvf>(HBDX9_PKT_FVF); p->fvf = f;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **out) { DLOCK;
  if (!e || !out) return D3DERR_INVALIDCALL;
  auto *d = new Hbdx9Decl(this);
  for (UINT i = 0; i < 65; i++) { d->elements.push_back(e[i]); if (e[i].Stream == 0xFF) break; }
  if (d->elements.back().Stream != 0xFF) { d->Release(); return D3DERR_INVALIDCALL; }
  *out = d; return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *dcl) { DLOCK;
  if (rec) rec->decl = 1;
  if (dcl == decl) return D3D_OK;
  if (dcl) dcl->AddRef();
  if (decl) decl->Release();
  decl = dcl; fvf = 0; sb_note(SB_DECL, 0);
  if (!dcl) return D3D_OK;
  auto *d = static_cast<Hbdx9Decl *>(dcl);
  UINT n = (UINT)d->elements.size() - 1;
  auto *p = add<hbdx9_pkt_vertex_decl>(HBDX9_PKT_VERTEX_DECL, n * sizeof(hbdx9_vertex_element));
  p->count = n;
  auto *out = reinterpret_cast<hbdx9_vertex_element *>(p + 1);
  for (UINT i = 0; i < n; i++) {
    const auto &e = d->elements[i];
    out[i] = {e.Stream, e.Offset, e.Type, e.Method, e.Usage, e.UsageIndex};
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Hbdx9Device::CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf_, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *shared) { DLOCK;
  if (!out || shared || !len) return D3DERR_INVALIDCALL;
  uint64_t native; BYTE *shadow;
  HRESULT hr = make_buffer(this, len, usage, D3DFMT_VERTEXDATA, pool, &native, &shadow);
  if (FAILED(hr)) return hr;
  auto *b = new Hbdx9VB(this, len);
  b->native = native; b->shadow = shadow; b->bi.usage = usage; b->bi.fvf = fvf_; b->bi.format = D3DFMT_VERTEXDATA; b->bi.pool = pool;
  *out = b; return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *shared) { DLOCK;
  if (!out || shared || !len || (fmt != D3DFMT_INDEX16 && fmt != D3DFMT_INDEX32)) return D3DERR_INVALIDCALL;
  uint64_t native; BYTE *shadow;
  HRESULT hr = make_buffer(this, len, usage, fmt, pool, &native, &shadow);
  if (FAILED(hr)) return hr;
  auto *b = new Hbdx9IB(this, len);
  b->native = native; b->shadow = shadow; b->bi.usage = usage; b->bi.fvf = 0; b->bi.format = fmt; b->bi.pool = pool;
  *out = b; return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::SetStreamSource(UINT s, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) { DLOCK;
  if (s >= 16) return D3DERR_INVALIDCALL;
  if (rec) rec->stream[s] = 1;
  if (streams[s].vb == vb && streams[s].offset == offset && streams[s].stride == stride) return D3D_OK;
  if (vb) vb->AddRef();
  if (streams[s].vb) streams[s].vb->Release();
  streams[s].vb = vb; streams[s].offset = offset; streams[s].stride = stride; sb_note(SB_STREAM, s);
  auto *p = add<hbdx9_pkt_stream_source>(HBDX9_PKT_STREAM_SOURCE);
  p->stream = s; p->offset = offset; p->stride = stride; p->buffer = vb ? static_cast<Hbdx9VB *>(vb)->native : 0;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::SetIndices(IDirect3DIndexBuffer9 *i) { DLOCK;
  if (rec) rec->ib = 1;
  if (ib == i) return D3D_OK;
  if (i) i->AddRef();
  if (ib) ib->Release();
  ib = i; sb_note(SB_IB, 0);
  pkt_object(HBDX9_PKT_INDICES, 0, i ? static_cast<Hbdx9IB *>(i)->native : 0);
  return D3D_OK;
}

HRESULT Hbdx9Device::create_shader(const DWORD *fn, UINT stage, uint64_t *native, UINT *count) {
  if (!fn) return D3DERR_INVALIDCALL;
  hbdx9_create_shader_params p{native_device, (uint64_t)(uintptr_t)fn, 1u << 18, stage, 0, 0, 0};
  NTSTATUS st = hb_call(HBDX9_CALL_CREATE_SHADER, &p);
  if (st || p.status) { hbdx9_log("Create%sShader failed (version %08lx)", stage ? "Pixel" : "Vertex", (unsigned long)fn[0]); return D3DERR_INVALIDCALL; }
  *native = p.shader; *count = p.token_count;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::CreateVertexShader(const DWORD *fn, IDirect3DVertexShader9 **out) { DLOCK;
  if (!out) return D3DERR_INVALIDCALL;
  uint64_t native; UINT n; HRESULT hr = create_shader(fn, 0, &native, &n); if (FAILED(hr)) return hr;
  auto *s = new Hbdx9VS(this); s->native = native; s->tokens.assign(fn, fn + n);
  *out = s; return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::CreatePixelShader(const DWORD *fn, IDirect3DPixelShader9 **out) { DLOCK;
  if (!out) return D3DERR_INVALIDCALL;
  uint64_t native; UINT n; HRESULT hr = create_shader(fn, 1, &native, &n); if (FAILED(hr)) return hr;
  auto *s = new Hbdx9PS(this); s->native = native; s->tokens.assign(fn, fn + n);
  *out = s; return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::SetVertexShader(IDirect3DVertexShader9 *s) { DLOCK;
  if (rec) rec->vs = 1;
  if (vs == s) return D3D_OK;
  if (s) s->AddRef();
  if (vs) vs->Release();
  vs = s; sb_note(SB_VS, 0);
  pkt_object(HBDX9_PKT_SHADER, 0, s ? static_cast<Hbdx9VS *>(s)->native : 0);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::SetPixelShader(IDirect3DPixelShader9 *s) { DLOCK;
  if (rec) rec->ps = 1;
  if (ps == s) return D3D_OK;
  if (s) s->AddRef();
  if (ps) ps->Release();
  ps = s; sb_note(SB_PS, 0);
  pkt_object(HBDX9_PKT_SHADER, 1, s ? static_cast<Hbdx9PS *>(s)->native : 0);
  return D3D_OK;
}

/* ---------------- state blocks ---------------- */
class Hbdx9StateBlock final : public Hbdx9Stub_IDirect3DStateBlock9<IDirect3DStateBlock9> {
public:
  LONG refs = 1;
  Hbdx9Device *dev;
  SBMask m{};
  DWORD rs[256] = {}, ss[20][14] = {}, tss[8][33] = {};
  IDirect3DBaseTexture9 *tex[20] = {};
  struct { IDirect3DVertexBuffer9 *vb; UINT offset, stride, freq; } streams[16] = {};
  IDirect3DIndexBuffer9 *ib = nullptr; IDirect3DVertexShader9 *vs = nullptr; IDirect3DPixelShader9 *ps = nullptr;
  IDirect3DVertexDeclaration9 *decl = nullptr; DWORD fvf = 0;
  float vsf[256][4] = {}, psf[256][4] = {}; int vsi[16][4] = {}, psi[16][4] = {}; BOOL vsb[16] = {}, psb[16] = {};
  D3DVIEWPORT9 viewport{}; RECT scissor{}; D3DMATERIAL9 material{}; float clip[6][4] = {};
  D3DMATRIX tr[512];
  /* (DX9FULL, HBDX9_FMT_EXT) lights: lt_all = every light of the device (D3DSBT_VERTEXSTATE / D3DSBT_ALL, as DXVK), else the
     indices recorded by SetLight (lt_set) and LightEnable (lt_en) */
  bool lt_all = false;
  std::vector<DWORD> lt_set, lt_en;
  std::vector<Hbdx9Device::LightRec> lights;
  Hbdx9Device::LightRec *lt_find(DWORD i) { for (auto &l : lights) if (l.index == i) return &l; return nullptr; }
  Hbdx9Device::LightRec &lt_slot(DWORD i) {
    if (auto *l = lt_find(i)) return *l;
    Hbdx9Device::LightRec r{}; r.index = i; lights.push_back(r); return lights.back();
  }
  void lt_apply_one(const Hbdx9Device::LightRec &l) {
    Hbdx9Device::LightRec *c = dev->find_light(l.index);
    if (!c || memcmp(&c->light, &l.light, sizeof(l.light))) dev->SetLight(l.index, &l.light);
  }

  explicit Hbdx9StateBlock(Hbdx9Device *d) : dev(d) { dev->AddRef(); }
  bool tracking = false;   /* (DX9FULL v13) counted in dev->sb_track */
  ~Hbdx9StateBlock() {
    if (tracking) { Hbdx9Lock l(&dev->cs, dev->locking); dev->sb_track--; }
    for (auto &t : tex) if (t) t->Release();
    for (auto &st : streams) if (st.vb) st.vb->Release();
    if (ib) ib->Release(); if (vs) vs->Release(); if (ps) ps->Release(); if (decl) decl->Release();
    dev->Release();
  }
  template <typename T> static void set_ref(T *&dst, T *src) { if (src) src->AddRef(); if (dst) dst->Release(); dst = src; }
  /* equal words, without an early exit (so the compiler keeps it inline instead of calling the guest's memcmp) */
  static bool same_words(const void *a, const void *b, size_t bytes) {
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b; uint32_t diff = 0;
    for (size_t i = 0; i < bytes / 4; i++) diff |= x[i] ^ y[i];
    return diff == 0;
  }
  /* (DX9FULL 13.11.2026, curator / NATIVE32 census: Gothic 3 calls apply 100 and capture 94 times a frame, 33k and 11k host
     instructions a call, because both walked every slot of the mask) the mask is turned once into lists of what the block
     holds (as DXVK's state blocks iterate their captured bit sets); apply / capture walk only those, in the same order as
     before; float constants go in runs (one Set*ConstantF per run instead of one per register). */
  struct Idx {
    bool built = false;
    std::vector<uint16_t> rs, ss, tss, tr;                       /* rs index; sampler * 14 + state; stage * 33 + state; transform */
    std::vector<std::pair<uint16_t, uint16_t>> vsf, psf;         /* (start, count) */
    /* runs of the same masks for capture (contiguous in both the block's and the device's arrays: one memcpy a run) and the
       integer / bool constants for apply (one Set*Constant a run) */
    std::vector<std::pair<uint16_t, uint16_t>> rs_r, ss_r, tss_r, tr_r, vsi_r, psi_r, vsb_r, psb_r;
    /* capture copies a category by runs (memcpy) when its runs are long (4+ entries on average, D3DSBT_* blocks), else entry by
       entry (a recorded block's few scattered states: a memcpy call a state costs more than the copy) */
    bool rs_bulk = false, ss_bulk = false, tss_bulk = false, tr_bulk = false;
    uint32_t tex = 0, stream = 0, freq = 0, vsi = 0, psi = 0, vsb = 0, psb = 0, clip = 0;
    uint32_t cost[Hbdx9Device::SB_NCAT] = {}, ccost[Hbdx9Device::SB_NCAT] = {};   /* host instructions of Apply / Capture per category */
    bool small = false;                                                             /* all of it costs less than one category walk choice */
  } ix;
  void build() {
    ix = Idx{};
    for (int i = 0; i < 256; i++) if (m.rs[i]) ix.rs.push_back((uint16_t)i);
    for (int i = 0; i < 20; i++) { for (int j = 0; j < 14; j++) if (m.ss[i][j]) ix.ss.push_back((uint16_t)(i * 14 + j)); if (m.tex[i]) ix.tex |= 1u << i; }
    for (int i = 0; i < 8; i++) for (int j = 0; j < 33; j++) if (m.tss[i][j]) ix.tss.push_back((uint16_t)(i * 33 + j));
    for (int i = 0; i < 16; i++) {
      if (m.stream[i]) ix.stream |= 1u << i; if (m.freq[i]) ix.freq |= 1u << i;
      if (m.vsi[i]) ix.vsi |= 1u << i; if (m.psi[i]) ix.psi |= 1u << i; if (m.vsb[i]) ix.vsb |= 1u << i; if (m.psb[i]) ix.psb |= 1u << i;
    }
    auto runs = [](const uint8_t *mk, int n, std::vector<std::pair<uint16_t, uint16_t>> &out) {
      for (int i = 0; i < n;) { if (!mk[i]) { i++; continue; } int j = i; while (j < n && mk[j]) j++; out.push_back({(uint16_t)i, (uint16_t)(j - i)}); i = j; }
    };
    runs(m.vsf, 256, ix.vsf); runs(m.psf, 224, ix.psf);
    runs(m.rs, 256, ix.rs_r); runs(&m.ss[0][0], 20 * 14, ix.ss_r); runs(&m.tss[0][0], 8 * 33, ix.tss_r); runs(m.transform, 512, ix.tr_r);
    runs(m.vsi, 16, ix.vsi_r); runs(m.psi, 16, ix.psi_r); runs(m.vsb, 16, ix.vsb_r); runs(m.psb, 16, ix.psb_r);
    for (int i = 0; i < 6; i++) if (m.clip[i]) ix.clip |= 1u << i;
    for (int i = 0; i < 512; i++) if (m.transform[i]) ix.tr.push_back((uint16_t)i);
    ix.rs_bulk = ix.rs.size() >= 4 * ix.rs_r.size(); ix.ss_bulk = ix.ss.size() >= 4 * ix.ss_r.size();
    ix.tss_bulk = ix.tss.size() >= 4 * ix.tss_r.size(); ix.tr_bulk = ix.tr.size() >= 4 * ix.tr_r.size();
    /* (DX9FULL v13) host instructions of comparing (Apply) and copying (Capture) each category, measured on v12 with probe cost13
       (Apply ALL 35.7k: ~7 an rs entry, ~20 a constant register, ~80 a matrix; Capture ALL 10.5k), for the walk-or-compare choice */
    using D = Hbdx9Device;
    auto regs = [](const std::vector<std::pair<uint16_t, uint16_t>> &r) { uint32_t n = 0; for (auto &x : r) n += x.second; return n; };
    auto pc = [](uint32_t b) { return (uint32_t)__builtin_popcount(b); };
    auto bulk = [](bool b, uint32_t n, uint32_t runs) { return b ? n + 20 * runs : 4 * n; };
    uint32_t nrs = (uint32_t)ix.rs.size(), nss = (uint32_t)ix.ss.size(), ntss = (uint32_t)ix.tss.size();
    uint32_t nvsf = regs(ix.vsf), npsf = regs(ix.psf), lt = lt_all || !lt_set.empty() || !lt_en.empty() ? 300 : 0;
    ix.cost[D::SB_RS] = 7 * nrs; ix.cost[D::SB_SS] = 9 * nss + (nss ? 40 : 0); ix.cost[D::SB_TSS] = 9 * ntss;
    ix.cost[D::SB_TEX] = 6 * pc(ix.tex); ix.cost[D::SB_STREAM] = 10 * pc(ix.stream); ix.cost[D::SB_FREQ] = 5 * pc(ix.freq);
    ix.cost[D::SB_IB] = m.ib ? 6 : 0; ix.cost[D::SB_VS] = m.vs ? 6 : 0; ix.cost[D::SB_PS] = m.ps ? 6 : 0; ix.cost[D::SB_DECL] = m.decl ? 10 : 0;
    ix.cost[D::SB_VSF] = 20 * nvsf + 20 * (uint32_t)ix.vsf.size(); ix.cost[D::SB_PSF] = 20 * npsf + 20 * (uint32_t)ix.psf.size();
    ix.cost[D::SB_VSI] = 20 * regs(ix.vsi_r); ix.cost[D::SB_PSI] = 20 * regs(ix.psi_r); ix.cost[D::SB_VSB] = 6 * regs(ix.vsb_r); ix.cost[D::SB_PSB] = 6 * regs(ix.psb_r);
    ix.cost[D::SB_VIEWPORT] = m.viewport ? 20 : 0; ix.cost[D::SB_SCISSOR] = m.scissor ? 12 : 0; ix.cost[D::SB_MATERIAL] = m.material ? 40 : 0;
    ix.cost[D::SB_CLIP] = 20 * pc(ix.clip); ix.cost[D::SB_LIGHT] = lt;
    for (uint16_t i : ix.tr) { ix.cost[D::sb_tr_group(i)] += 80; ix.ccost[D::sb_tr_group(i)] += ix.tr_bulk ? 18 : 20; }
    ix.ccost[D::SB_RS] = bulk(ix.rs_bulk, nrs, (uint32_t)ix.rs_r.size()); ix.ccost[D::SB_SS] = bulk(ix.ss_bulk, nss, (uint32_t)ix.ss_r.size());
    ix.ccost[D::SB_TSS] = bulk(ix.tss_bulk, ntss, (uint32_t)ix.tss_r.size());
    ix.ccost[D::SB_TEX] = 15 * pc(ix.tex); ix.ccost[D::SB_STREAM] = 20 * pc(ix.stream); ix.ccost[D::SB_FREQ] = 4 * pc(ix.freq);
    ix.ccost[D::SB_IB] = m.ib ? 15 : 0; ix.ccost[D::SB_VS] = m.vs ? 15 : 0; ix.ccost[D::SB_PS] = m.ps ? 15 : 0; ix.ccost[D::SB_DECL] = m.decl ? 20 : 0;
    ix.ccost[D::SB_VSF] = 5 * nvsf + 20 * (uint32_t)ix.vsf.size(); ix.ccost[D::SB_PSF] = 5 * npsf + 20 * (uint32_t)ix.psf.size();
    ix.ccost[D::SB_VSI] = 5 * regs(ix.vsi_r); ix.ccost[D::SB_PSI] = 5 * regs(ix.psi_r); ix.ccost[D::SB_VSB] = 2 * regs(ix.vsb_r); ix.ccost[D::SB_PSB] = 2 * regs(ix.psb_r);
    ix.ccost[D::SB_VIEWPORT] = m.viewport ? 8 : 0; ix.ccost[D::SB_SCISSOR] = m.scissor ? 6 : 0; ix.ccost[D::SB_MATERIAL] = m.material ? 20 : 0;
    ix.ccost[D::SB_CLIP] = 8 * pc(ix.clip); ix.ccost[D::SB_LIGHT] = lt;
    uint32_t total = 0; for (uint32_t c : ix.cost) total += c;
    ix.small = total <= SMALL;   /* a recorded block of a few states: compared whole, as in v12 */
    bool use = !ix.small && sb_journal_on();
    if (use != tracking) {
      /* the first user switches the notes on: positions before it were never noted, so every category counts as changed */
      if (use) { if (!dev->sb_track++) dev->sb_note_all(); } else dev->sb_track--;
      tracking = use;
    }
    synced = false;   /* what the block holds changed: compare everything once */
    ix.built = true;
  }
  /* (DX9FULL v13) the journal position of the moment this block last equalled the device on everything it holds */
  bool synced = false;
  uint32_t sync_seq = 0;
  /* SMALL: blocks whose whole compare costs less than this are not walked; WALK_FREE: this many notes are walked without looking at
     the categories; WALK_MAX: notes walked at most (< SB_JCAP); WALK_A / WALK_C: host instructions a walked note costs in Apply / Capture */
  static constexpr uint32_t SMALL = 1500, WALK_FREE = 8, WALK_MAX = 256, WALK_A = 200, WALK_C = 100;
  void resync() { synced = true; sync_seq = dev->sb_seq; }
  /* what to do: 0 nothing changed since the block equalled the device; 3 only categories the block does not hold changed;
     1 walk the journal (it still holds every note and walking is cheaper); 2 compare the categories in *cats */
  int changes(uint32_t *cats, bool cap) {
    Hbdx9Device *d = dev;
    if (!synced || !tracking) { *cats = ~0u; return 2; }
    uint32_t delta = d->sb_seq - sync_seq;
    if (!delta) return 0;
    if (delta <= WALK_FREE) return 1;
    uint32_t c = 0, cost = 0; const uint32_t *ct = cap ? ix.ccost : ix.cost;
    for (int k = 0; k < Hbdx9Device::SB_NCAT; k++) if (d->sb_last[k] - sync_seq < delta) { c |= 1u << k; cost += ct[k]; }
    *cats = c;
    if (!cost) return 3;
    return delta <= WALK_MAX && delta * (cap ? WALK_C : WALK_A) < cost ? 1 : 2;
  }
  static DWORD smp_of(unsigned i) { return i < 16 ? (DWORD)i : D3DVERTEXTEXTURESAMPLER0 + (i - 16); }
  void capture_lights() {
    Hbdx9Device *d = dev;
    if (lt_all) lights = d->lights;
    else {
      for (DWORD i : lt_set) if (auto *l = d->find_light(i)) lt_slot(i).light = l->light;
      for (DWORD i : lt_en) if (auto *l = d->find_light(i)) lt_slot(i).enabled = l->enabled;
    }
  }
  void apply_lights() {
    Hbdx9Device *d = dev;
    if (lt_all) for (auto &l : lights) { lt_apply_one(l); d->LightEnable(l.index, l.enabled); }
    else {
      for (DWORD i : lt_set) if (auto *l = lt_find(i)) lt_apply_one(*l);
      for (DWORD i : lt_en) if (auto *l = lt_find(i)) d->LightEnable(i, l->enabled);
    }
  }
  void apply_decl() {
    Hbdx9Device *d = dev;
    if (decl) { if (d->decl != decl) d->SetVertexDeclaration(decl); }
    else if (!(fvf && !d->decl && d->fvf == fvf)) d->SetFVF(fvf);   /* d->fvf != 0: the last vertex-input call was SetFVF(fvf) */
  }
  /* registers [i, i + n) of a constant bank (a noted range): each run of them the block holds goes whole, in one Set*, when it differs
     from the device (one compare a run: comparing register by register costs more than sending the run) */
  template <typename F> static void put_runs(const uint8_t *mk, const void *dv, const void *bv, unsigned elem, unsigned i, unsigned n, F &&set) {
    const uint8_t *a = (const uint8_t *)dv, *b = (const uint8_t *)bv;
    for (unsigned j = i, e = i + n; j < e;) {
      if (!mk[j]) { j++; continue; }
      unsigned k = j + 1;
      while (k < e && mk[k]) k++;
      if (!same_words(a + j * elem, b + j * elem, (k - j) * elem)) set(j, b + j * elem, k - j);
      j = k;
    }
  }
  void put_vsf(unsigned i, unsigned n) { Hbdx9Device *d = dev; put_runs(m.vsf, d->vsf, vsf, 16, i, n, [d](unsigned j, const void *p, unsigned c) { d->SetVertexShaderConstantF(j, (const float *)p, c); }); }
  void put_psf(unsigned i, unsigned n) { Hbdx9Device *d = dev; put_runs(m.psf, d->psf, psf, 16, i, n, [d](unsigned j, const void *p, unsigned c) { d->SetPixelShaderConstantF(j, (const float *)p, c); }); }
  void put_vsi(unsigned i, unsigned n) { Hbdx9Device *d = dev; put_runs(m.vsi, d->vsi, vsi, 16, i, n, [d](unsigned j, const void *p, unsigned c) { d->SetVertexShaderConstantI(j, (const int *)p, c); }); }
  void put_psi(unsigned i, unsigned n) { Hbdx9Device *d = dev; put_runs(m.psi, d->psi, psi, 16, i, n, [d](unsigned j, const void *p, unsigned c) { d->SetPixelShaderConstantI(j, (const int *)p, c); }); }
  void put_vsb(unsigned i, unsigned n) { Hbdx9Device *d = dev; put_runs(m.vsb, d->vsb, vsb, 4, i, n, [d](unsigned j, const void *p, unsigned c) { d->SetVertexShaderConstantB(j, (const BOOL *)p, c); }); }
  void put_psb(unsigned i, unsigned n) { Hbdx9Device *d = dev; put_runs(m.psb, d->psb, psb, 4, i, n, [d](unsigned j, const void *p, unsigned c) { d->SetPixelShaderConstantB(j, (const BOOL *)p, c); }); }
  /* Capture of the categories in c (all: the v12 capture) */
  __attribute__((always_inline)) void capture_cats(uint32_t c) {
    using D = Hbdx9Device;
    Hbdx9Device *d = dev;
    if (c >> D::SB_RS & 1) {
      if (ix.rs_bulk) for (auto &r : ix.rs_r) memcpy(&rs[r.first], &d->rs[r.first], 4u * r.second);
      else for (uint16_t i : ix.rs) rs[i] = d->rs[i];
    }
    if (c >> D::SB_SS & 1) {
      if (ix.ss_bulk) for (auto &r : ix.ss_r) memcpy(&ss[0][0] + r.first, &d->ss[0][0] + r.first, 4u * r.second);
      else for (uint16_t k : ix.ss) (&ss[0][0])[k] = (&d->ss[0][0])[k];
    }
    if (c >> D::SB_TSS & 1) {
      if (ix.tss_bulk) for (auto &r : ix.tss_r) memcpy(&tss[0][0] + r.first, &d->tss[0][0] + r.first, 4u * r.second);
      else for (uint16_t k : ix.tss) (&tss[0][0])[k] = (&d->tss[0][0])[k];
    }
    if (c >> D::SB_TEX & 1) for (uint32_t b = ix.tex; b; b &= b - 1) { int i = __builtin_ctz(b); set_ref(tex[i], d->tex[i]); }
    if (c & (1u << D::SB_STREAM | 1u << D::SB_FREQ))
      for (uint32_t b = ix.stream | ix.freq; b; b &= b - 1) {
        int i = __builtin_ctz(b);
        if (ix.stream >> i & 1) { set_ref(streams[i].vb, d->streams[i].vb); streams[i].offset = d->streams[i].offset; streams[i].stride = d->streams[i].stride; }
        if (ix.freq >> i & 1) streams[i].freq = d->streams[i].freq;
      }
    if (m.ib && (c >> D::SB_IB & 1)) set_ref(ib, d->ib);
    if (m.vs && (c >> D::SB_VS & 1)) set_ref(vs, d->vs);
    if (m.ps && (c >> D::SB_PS & 1)) set_ref(ps, d->ps);
    if (m.decl && (c >> D::SB_DECL & 1)) { set_ref(decl, d->decl); fvf = d->fvf; }
    if (c >> D::SB_VSF & 1) for (auto &r : ix.vsf) memcpy(vsf[r.first], d->vsf[r.first], 16u * r.second);
    if (c >> D::SB_PSF & 1) for (auto &r : ix.psf) memcpy(psf[r.first], d->psf[r.first], 16u * r.second);
    if (c >> D::SB_VSI & 1) for (auto &r : ix.vsi_r) memcpy(vsi[r.first], d->vsi[r.first], 16u * r.second);
    if (c >> D::SB_PSI & 1) for (auto &r : ix.psi_r) memcpy(psi[r.first], d->psi[r.first], 16u * r.second);
    if (c >> D::SB_VSB & 1) for (auto &r : ix.vsb_r) memcpy(&vsb[r.first], &d->vsb[r.first], 4u * r.second);
    if (c >> D::SB_PSB & 1) for (auto &r : ix.psb_r) memcpy(&psb[r.first], &d->psb[r.first], 4u * r.second);
    if (m.viewport && (c >> D::SB_VIEWPORT & 1)) viewport = d->viewport;
    if (m.scissor && (c >> D::SB_SCISSOR & 1)) scissor = d->scissor;
    if (m.material && (c >> D::SB_MATERIAL & 1)) material = d->material;
    if (c >> D::SB_CLIP & 1) for (uint32_t b = ix.clip; b; b &= b - 1) { int i = __builtin_ctz(b); memcpy(clip[i], d->clip_planes[i], 16); }
    if ((c & D::SB_TR_GROUPS) == D::SB_TR_GROUPS) {
      if (ix.tr_bulk) for (auto &r : ix.tr_r) memcpy(&tr[r.first], &d->transforms[r.first], sizeof(D3DMATRIX) * r.second);
      else for (uint16_t i : ix.tr) tr[i] = d->transforms[i];
    } else if (c & D::SB_TR_GROUPS) for (uint16_t i : ix.tr) if (c >> D::sb_tr_group(i) & 1) tr[i] = d->transforms[i];
    if (c >> D::SB_LIGHT & 1) capture_lights();
  }
  /* Capture of the noted entries only (the journal holds every change since the block last equalled the device) */
  void capture_walk() {
    using D = Hbdx9Device;
    Hbdx9Device *d = dev; bool lights_changed = false; uint32_t n = 0;
    for (uint32_t q = sync_seq, e = d->sb_seq; q != e; q++, n++) {
      uint32_t nt = d->sb_jr[q & (D::SB_JCAP - 1)];
      unsigned i = nt & 1023, cnt = nt >> 16;
      switch (nt >> 10 & 63) {
      case D::SB_RS: if (m.rs[i]) rs[i] = d->rs[i]; break;
      case D::SB_SS: if ((&m.ss[0][0])[i]) (&ss[0][0])[i] = (&d->ss[0][0])[i]; break;
      case D::SB_TSS: if ((&m.tss[0][0])[i]) (&tss[0][0])[i] = (&d->tss[0][0])[i]; break;
      case D::SB_TEX: if (ix.tex >> i & 1) set_ref(tex[i], d->tex[i]); break;
      case D::SB_STREAM: if (ix.stream >> i & 1) { set_ref(streams[i].vb, d->streams[i].vb); streams[i].offset = d->streams[i].offset; streams[i].stride = d->streams[i].stride; } break;
      case D::SB_FREQ: if (ix.freq >> i & 1) streams[i].freq = d->streams[i].freq; break;
      case D::SB_IB: if (m.ib) set_ref(ib, d->ib); break;
      case D::SB_VS: if (m.vs) set_ref(vs, d->vs); break;
      case D::SB_PS: if (m.ps) set_ref(ps, d->ps); break;
      case D::SB_DECL: if (m.decl) { set_ref(decl, d->decl); fvf = d->fvf; } break;
      case D::SB_VSF: for (unsigned j = i; j < i + cnt; j++) if (m.vsf[j]) memcpy(vsf[j], d->vsf[j], 16); break;
      case D::SB_PSF: for (unsigned j = i; j < i + cnt; j++) if (m.psf[j]) memcpy(psf[j], d->psf[j], 16); break;
      case D::SB_VSI: for (unsigned j = i; j < i + cnt; j++) if (m.vsi[j]) memcpy(vsi[j], d->vsi[j], 16); break;
      case D::SB_PSI: for (unsigned j = i; j < i + cnt; j++) if (m.psi[j]) memcpy(psi[j], d->psi[j], 16); break;
      case D::SB_VSB: for (unsigned j = i; j < i + cnt; j++) if (m.vsb[j]) vsb[j] = d->vsb[j]; break;
      case D::SB_PSB: for (unsigned j = i; j < i + cnt; j++) if (m.psb[j]) psb[j] = d->psb[j]; break;
      case D::SB_VIEWPORT: if (m.viewport) viewport = d->viewport; break;
      case D::SB_SCISSOR: if (m.scissor) scissor = d->scissor; break;
      case D::SB_MATERIAL: if (m.material) material = d->material; break;
      case D::SB_CLIP: if (m.clip[i]) memcpy(clip[i], d->clip_planes[i], 16); break;
      case D::SB_TR: if (m.transform[i]) tr[i] = d->transforms[i]; break;
      case D::SB_LIGHT: lights_changed = true; break;
      }
    }
    if (lights_changed) capture_lights();
    d->sb_n[6] += n;
  }
  void capture() {
    if (!ix.built) build();
    /* a block that does not use the journal (small, or HBDX9_SB_JOURNAL=0): the v12 capture, with everything known at compile time */
    if (!tracking) { capture_cats(~0u); dev->sb_n[5]++; if (sb_verify()) verify("capture"); return; }
    uint32_t c; int how = changes(&c, true);
    if (how == 1) { capture_walk(); dev->sb_n[4]++; }
    else if (how == 2) { capture_cats(c); dev->sb_n[5]++; }
    else dev->sb_n[3]++;
    if (how) resync();
    if (sb_verify()) verify("capture");
  }
  /* Apply of the categories in c (all: the v12 apply, constants: only the differing registers go) */
  __attribute__((always_inline)) void apply_cats(uint32_t c) {
    using D = Hbdx9Device;
    Hbdx9Device *d = dev;
    /* rs / ss / tss / texture / stream / indices / shaders: the value test is the one each Set* method makes first (an equal
       value returns there before any work), done here without the call; Apply cannot run while recording, so nothing is lost */
    if (m.decl && (c >> D::SB_DECL & 1)) apply_decl();
    if (m.vs && (c >> D::SB_VS & 1) && d->vs != vs) d->SetVertexShader(vs);
    if (m.ps && (c >> D::SB_PS & 1) && d->ps != ps) d->SetPixelShader(ps);
    if (c >> D::SB_RS & 1) for (uint16_t i : ix.rs) if (d->rs[i] != rs[i]) d->SetRenderState((D3DRENDERSTATETYPE)i, rs[i]);
    if (c & (1u << D::SB_SS | 1u << D::SB_TEX)) {
      bool do_ss = c >> D::SB_SS & 1, do_tex = c >> D::SB_TEX & 1;
      size_t k = 0;
      for (int i = 0; i < 20; i++) {
        DWORD smp = smp_of(i);
        for (; k < ix.ss.size() && ix.ss[k] / 14 == i; k++) {
          int t = ix.ss[k] % 14;
          if (do_ss && d->ss[i][t] != ss[i][t]) d->SetSamplerState(smp, (D3DSAMPLERSTATETYPE)t, ss[i][t]);
        }
        if (do_tex && (ix.tex >> i & 1) && d->tex[i] != tex[i]) d->SetTexture(smp, tex[i]);
      }
    }
    if (c >> D::SB_TSS & 1)
      for (uint16_t s : ix.tss) if (d->tss[s / 33][s % 33] != tss[s / 33][s % 33]) d->SetTextureStageState(s / 33, (D3DTEXTURESTAGESTATETYPE)(s % 33), tss[s / 33][s % 33]);
    if (c & (1u << D::SB_STREAM | 1u << D::SB_FREQ))
      for (uint32_t b = ix.stream | ix.freq; b; b &= b - 1) {
        int i = __builtin_ctz(b);
        if ((ix.stream >> i & 1) && (d->streams[i].vb != streams[i].vb || d->streams[i].offset != streams[i].offset || d->streams[i].stride != streams[i].stride))
          d->SetStreamSource(i, streams[i].vb, streams[i].offset, streams[i].stride);
        if ((ix.freq >> i & 1) && d->streams[i].freq != streams[i].freq) d->SetStreamSourceFreq(i, streams[i].freq);
      }
    if (m.ib && (c >> D::SB_IB & 1) && d->ib != ib) d->SetIndices(ib);
    /* constants, viewport, scissor, material, clip planes, transforms go only when they differ from the device: the device's copy
       is what the core has (each of these Set* methods always sends, and Reset resets both sides to the same defaults) */
    if (c >> D::SB_VSF & 1) for (auto &r : ix.vsf) if (!same_words(d->vsf[r.first], vsf[r.first], 16u * r.second)) d->SetVertexShaderConstantF(r.first, vsf[r.first], r.second);
    if (c >> D::SB_PSF & 1) for (auto &r : ix.psf) if (!same_words(d->psf[r.first], psf[r.first], 16u * r.second)) d->SetPixelShaderConstantF(r.first, psf[r.first], r.second);
    if (c >> D::SB_VSI & 1) for (auto &r : ix.vsi_r) if (!same_words(d->vsi[r.first], vsi[r.first], 16u * r.second)) d->SetVertexShaderConstantI(r.first, vsi[r.first], r.second);
    if (c >> D::SB_PSI & 1) for (auto &r : ix.psi_r) if (!same_words(d->psi[r.first], psi[r.first], 16u * r.second)) d->SetPixelShaderConstantI(r.first, psi[r.first], r.second);
    if (c >> D::SB_VSB & 1) for (auto &r : ix.vsb_r) if (!same_words(&d->vsb[r.first], &vsb[r.first], 4u * r.second)) d->SetVertexShaderConstantB(r.first, &vsb[r.first], r.second);
    if (c >> D::SB_PSB & 1) for (auto &r : ix.psb_r) if (!same_words(&d->psb[r.first], &psb[r.first], 4u * r.second)) d->SetPixelShaderConstantB(r.first, &psb[r.first], r.second);
    if (m.viewport && (c >> D::SB_VIEWPORT & 1) && !same_words(&d->viewport, &viewport, sizeof(viewport))) d->SetViewport(&viewport);
    if (m.scissor && (c >> D::SB_SCISSOR & 1) && !same_words(&d->scissor, &scissor, sizeof(scissor))) d->SetScissorRect(&scissor);
    if (m.material && (c >> D::SB_MATERIAL & 1) && !same_words(&d->material, &material, sizeof(material))) d->SetMaterial(&material);
    if (c >> D::SB_CLIP & 1)
      for (uint32_t b = ix.clip; b; b &= b - 1) { int i = __builtin_ctz(b); if (!same_words(d->clip_planes[i], clip[i], 16)) d->SetClipPlane(i, clip[i]); }
    if (c & D::SB_TR_GROUPS)
      for (uint16_t i : ix.tr) if ((c >> D::sb_tr_group(i) & 1) && !same_words(&d->transforms[i], &tr[i], sizeof(D3DMATRIX))) d->SetTransform((D3DTRANSFORMSTATETYPE)i, &tr[i]);
    if (c >> D::SB_LIGHT & 1) apply_lights();
  }
  /* Apply of the noted entries only.  The notes are copied first: the Set* calls below add notes of their own to the ring. */
  void apply_walk() {
    using D = Hbdx9Device;
    Hbdx9Device *d = dev;
    uint32_t n = d->sb_seq - sync_seq, nb[WALK_MAX];
    for (uint32_t k = 0; k < n; k++) nb[k] = d->sb_jr[(sync_seq + k) & (D::SB_JCAP - 1)];
    bool lights_changed = false;
    for (uint32_t k = 0; k < n; k++) {
      unsigned i = nb[k] & 1023, cnt = nb[k] >> 16;
      switch (nb[k] >> 10 & 63) {
      case D::SB_RS: if (m.rs[i] && d->rs[i] != rs[i]) d->SetRenderState((D3DRENDERSTATETYPE)i, rs[i]); break;
      case D::SB_SS: { unsigned s = i / 14, t = i % 14; if (m.ss[s][t] && d->ss[s][t] != ss[s][t]) d->SetSamplerState(smp_of(s), (D3DSAMPLERSTATETYPE)t, ss[s][t]); break; }
      case D::SB_TSS: { unsigned s = i / 33, t = i % 33; if (m.tss[s][t] && d->tss[s][t] != tss[s][t]) d->SetTextureStageState(s, (D3DTEXTURESTAGESTATETYPE)t, tss[s][t]); break; }
      case D::SB_TEX: if ((ix.tex >> i & 1) && d->tex[i] != tex[i]) d->SetTexture(smp_of(i), tex[i]); break;
      case D::SB_STREAM:
        if ((ix.stream >> i & 1) && (d->streams[i].vb != streams[i].vb || d->streams[i].offset != streams[i].offset || d->streams[i].stride != streams[i].stride))
          d->SetStreamSource(i, streams[i].vb, streams[i].offset, streams[i].stride);
        break;
      case D::SB_FREQ: if ((ix.freq >> i & 1) && d->streams[i].freq != streams[i].freq) d->SetStreamSourceFreq(i, streams[i].freq); break;
      case D::SB_IB: if (m.ib && d->ib != ib) d->SetIndices(ib); break;
      case D::SB_VS: if (m.vs && d->vs != vs) d->SetVertexShader(vs); break;
      case D::SB_PS: if (m.ps && d->ps != ps) d->SetPixelShader(ps); break;
      case D::SB_DECL: if (m.decl) apply_decl(); break;
      case D::SB_VSF: put_vsf(i, cnt); break;
      case D::SB_PSF: put_psf(i, cnt); break;
      case D::SB_VSI: put_vsi(i, cnt); break;
      case D::SB_PSI: put_psi(i, cnt); break;
      case D::SB_VSB: put_vsb(i, cnt); break;
      case D::SB_PSB: put_psb(i, cnt); break;
      case D::SB_VIEWPORT: if (m.viewport && !same_words(&d->viewport, &viewport, sizeof(viewport))) d->SetViewport(&viewport); break;
      case D::SB_SCISSOR: if (m.scissor && !same_words(&d->scissor, &scissor, sizeof(scissor))) d->SetScissorRect(&scissor); break;
      case D::SB_MATERIAL: if (m.material && !same_words(&d->material, &material, sizeof(material))) d->SetMaterial(&material); break;
      case D::SB_CLIP: if (m.clip[i] && !same_words(d->clip_planes[i], clip[i], 16)) d->SetClipPlane(i, clip[i]); break;
      case D::SB_TR: if (m.transform[i] && !same_words(&d->transforms[i], &tr[i], sizeof(D3DMATRIX))) d->SetTransform((D3DTRANSFORMSTATETYPE)i, &tr[i]); break;
      case D::SB_LIGHT: lights_changed = true; break;
      }
    }
    if (lights_changed) apply_lights();
    d->sb_n[6] += n;
  }
  void apply() {
    if (!ix.built) build();
    if (!tracking) { apply_cats(~0u); dev->sb_n[2]++; if (sb_verify()) verify("apply"); return; }
    uint32_t c; int how = changes(&c, false);
    if (how == 1) { apply_walk(); dev->sb_n[1]++; }
    else if (how == 2) { apply_cats(c); dev->sb_n[2]++; }
    else dev->sb_n[0]++;
    if (how) resync();
    if (sb_verify()) verify("apply");
  }
  /* HBDX9_SB_VERIFY=1 (diagnostic): after every Capture / Apply, everything the block holds is compared with the device (the
     v12 compare without the Set* calls); a difference is a change the journal missed */
  void verify(const char *what) {
    Hbdx9Device *d = dev; unsigned bad = 0; const char *first = ""; unsigned fi = 0;
    auto chk = [&](bool ok, const char *nm, unsigned i) { if (!ok && !bad++) { first = nm; fi = i; } };
    for (uint16_t i : ix.rs) chk(d->rs[i] == rs[i], "rs", i);
    for (uint16_t k : ix.ss) chk((&d->ss[0][0])[k] == (&ss[0][0])[k], "ss", k);
    for (uint16_t k : ix.tss) chk((&d->tss[0][0])[k] == (&tss[0][0])[k], "tss", k);
    for (uint32_t b = ix.tex; b; b &= b - 1) { int i = __builtin_ctz(b); chk(d->tex[i] == tex[i], "tex", i); }
    for (uint32_t b = ix.stream; b; b &= b - 1) { int i = __builtin_ctz(b);
      chk(d->streams[i].vb == streams[i].vb && d->streams[i].offset == streams[i].offset && d->streams[i].stride == streams[i].stride, "stream", i); }
    for (uint32_t b = ix.freq; b; b &= b - 1) { int i = __builtin_ctz(b); chk(d->streams[i].freq == streams[i].freq, "freq", i); }
    if (m.ib) chk(d->ib == ib, "ib", 0);
    if (m.vs) chk(d->vs == vs, "vs", 0);
    if (m.ps) chk(d->ps == ps, "ps", 0);
    if (m.decl) chk(decl ? d->decl == decl : !d->decl && d->fvf == fvf, "decl", 0);
    for (int j = 0; j < 256; j++) if (m.vsf[j]) chk(same_words(d->vsf[j], vsf[j], 16), "vsf", j);
    for (int j = 0; j < 224; j++) if (m.psf[j]) chk(same_words(d->psf[j], psf[j], 16), "psf", j);
    for (int j = 0; j < 16; j++) {
      if (m.vsi[j]) chk(same_words(d->vsi[j], vsi[j], 16), "vsi", j);
      if (m.psi[j]) chk(same_words(d->psi[j], psi[j], 16), "psi", j);
      if (m.vsb[j]) chk(d->vsb[j] == vsb[j], "vsb", j);
      if (m.psb[j]) chk(d->psb[j] == psb[j], "psb", j);
    }
    if (m.viewport) chk(same_words(&d->viewport, &viewport, sizeof(viewport)), "viewport", 0);
    if (m.scissor) chk(same_words(&d->scissor, &scissor, sizeof(scissor)), "scissor", 0);
    if (m.material) chk(same_words(&d->material, &material, sizeof(material)), "material", 0);
    for (uint32_t b = ix.clip; b; b &= b - 1) { int i = __builtin_ctz(b); chk(same_words(d->clip_planes[i], clip[i], 16), "clip", i); }
    for (uint16_t i : ix.tr) chk(same_words(&d->transforms[i], &tr[i], sizeof(D3DMATRIX)), "transform", i);
    for (auto &l : lights) {
      auto *x = d->find_light(l.index);
      bool held_l = lt_all || std::find(lt_set.begin(), lt_set.end(), l.index) != lt_set.end();
      bool held_e = lt_all || std::find(lt_en.begin(), lt_en.end(), l.index) != lt_en.end();
      if (!x) continue;   /* gone from the device (Reset): Apply sets it again, Capture of a recorded block keeps it */
      if (held_l) chk(same_words(&x->light, &l.light, sizeof(l.light)), "light", l.index);
      if (held_e) chk(x->enabled == l.enabled, "light-enable", l.index);
    }
    d->sb_n[7]++;
    if (bad && ++d->sb_n[8] <= 16) hbdx9_log("sb-verify %s MISMATCH entries=%u first=%s[%u] seq=%u synced=%d", what, bad, first, fi, d->sb_seq, (int)synced);
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DStateBlock9)) { *out = this; AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override { if (!out) return D3DERR_INVALIDCALL; *out = dev; dev->AddRef(); return D3D_OK; }
  /* the whole Capture / Apply under the device lock (as DXVK), when the device has one */
  HRESULT STDMETHODCALLTYPE Capture() override { Hbdx9Lock l(&dev->cs, dev->locking); if (dev->recording) return D3DERR_INVALIDCALL; capture(); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE Apply() override { Hbdx9Lock l(&dev->cs, dev->locking); if (dev->recording) return D3DERR_INVALIDCALL; apply(); return D3D_OK; }
};

/* (DX9FULL, HBDX9_FMT_EXT) D3DSBT_PIXELSTATE / D3DSBT_VERTEXSTATE hold the render states of these lists, the pixel samplers'
   states but D3DSAMP_DMAPOFFSET, the vertex samplers' D3DSAMP_DMAPOFFSET (DXVK d3d9_stateblock.cpp CapturePixel* / CaptureVertex*,
   = Wine's pixel / vertex state lists); D3DSBT_ALL also holds WORLD1..WORLD255 and the lights.  Without the key every block
   holds all 256 render states and D3DSBT_ALL 11 transforms (s8j+ATI2). */
static const D3DRENDERSTATETYPE k_pixel_rs[] = {
  D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_LASTPIXEL, D3DRS_SRCBLEND,
  D3DRS_DESTBLEND, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_DITHERENABLE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY,
  D3DRS_ALPHABLENDENABLE, D3DRS_DEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS,
  D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR, D3DRS_WRAP0, D3DRS_WRAP1,
  D3DRS_WRAP2, D3DRS_WRAP3, D3DRS_WRAP4, D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_WRAP8, D3DRS_WRAP9, D3DRS_WRAP10, D3DRS_WRAP11,
  D3DRS_WRAP12, D3DRS_WRAP13, D3DRS_WRAP14, D3DRS_WRAP15, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP, D3DRS_SCISSORTESTENABLE,
  D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_ANTIALIASEDLINEENABLE, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL,
  D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3,
  D3DRS_BLENDFACTOR, D3DRS_SRGBWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA};
static const D3DRENDERSTATETYPE k_vertex_rs[] = {
  D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY,
  D3DRS_RANGEFOGENABLE, D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_FOGVERTEXMODE, D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_LOCALVIEWER,
  D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE,
  D3DRS_VERTEXBLEND, D3DRS_CLIPPLANEENABLE, D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN, D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE,
  D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK,
  D3DRS_PATCHEDGESTYLE, D3DRS_POINTSIZE_MAX, D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_TWEENFACTOR, D3DRS_POSITIONDEGREE,
  D3DRS_NORMALDEGREE, D3DRS_MINTESSELLATIONLEVEL, D3DRS_MAXTESSELLATIONLEVEL, D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y,
  D3DRS_ADAPTIVETESS_Z, D3DRS_ADAPTIVETESS_W, D3DRS_ENABLEADAPTIVETESSELLATION, D3DRS_NORMALIZENORMALS, D3DRS_SPECULARENABLE,
  D3DRS_SHADEMODE};
static void mask_all(SBMask &m, bool pixel, bool vertex) {
  memset(&m, 0, sizeof(m));
  if (fmt_ext_on() && pixel != vertex) {
    if (pixel) {
      for (auto r : k_pixel_rs) m.rs[r] = 1;
      for (int i = 0; i < 16; i++) for (int t = D3DSAMP_ADDRESSU; t <= D3DSAMP_ELEMENTINDEX; t++) m.ss[i][t] = 1;
      memset(m.tss, 1, sizeof(m.tss)); m.ps = 1;
      memset(m.psf, 1, 224); memset(m.psi, 1, 16); memset(m.psb, 1, 16);
    } else {
      for (auto r : k_vertex_rs) m.rs[r] = 1;
      for (int i = 16; i < 20; i++) m.ss[i][D3DSAMP_DMAPOFFSET] = 1;
      m.vs = 1; m.decl = 1; memset(m.freq, 1, 16);
      memset(m.vsf, 1, 256); memset(m.vsi, 1, 16); memset(m.vsb, 1, 16);
    }
    return;
  }
  memset(m.rs, 1, sizeof(m.rs));
  if (pixel) {
    memset(m.ss, 1, 16 * 14); memset(m.tss, 1, sizeof(m.tss)); m.ps = 1;
    memset(m.psf, 1, 224); memset(m.psi, 1, 16); memset(m.psb, 1, 16);
  }
  if (vertex) {
    memset(&m.ss[16][0], 1, 4 * 14); m.vs = 1; m.decl = 1; memset(m.freq, 1, 16);
    memset(m.vsf, 1, 256); memset(m.vsi, 1, 16); memset(m.vsb, 1, 16);
  }
  if (pixel && vertex) {
    memset(m.tex, 1, 20); memset(m.stream, 1, 16); m.ib = 1; m.viewport = 1; m.scissor = 1; m.material = 1; memset(m.clip, 1, 6);
    m.transform[D3DTS_VIEW] = m.transform[D3DTS_PROJECTION] = m.transform[256] = 1;
    for (int i = 0; i < 8; i++) m.transform[D3DTS_TEXTURE0 + i] = 1;
    if (fmt_ext_on()) memset(m.transform + 257, 1, 255);
  }
}

HRESULT STDMETHODCALLTYPE Hbdx9Device::CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **out) { DLOCK;
  if (!out || recording) return D3DERR_INVALIDCALL;
  auto *b = new Hbdx9StateBlock(this);
  mask_all(b->m, type == D3DSBT_ALL || type == D3DSBT_PIXELSTATE, type == D3DSBT_ALL || type == D3DSBT_VERTEXSTATE);
  b->lt_all = fmt_ext_on() && (type == D3DSBT_ALL || type == D3DSBT_VERTEXSTATE);
  b->capture();
  *out = b; return D3D_OK;
}
static Hbdx9StateBlock *g_sb_snapshot; /* device state at BeginStateBlock (single device per process in practice) */
HRESULT STDMETHODCALLTYPE Hbdx9Device::BeginStateBlock() { DLOCK;
  if (recording) return D3DERR_INVALIDCALL;
  g_sb_snapshot = new Hbdx9StateBlock(this);
  mask_all(g_sb_snapshot->m, true, true);
  memset(g_sb_snapshot->m.transform, 1, sizeof(g_sb_snapshot->m.transform));
  g_sb_snapshot->lt_all = fmt_ext_on();
  g_sb_snapshot->capture();
  recording = new Hbdx9StateBlock(this);
  rec = &recording->m; rec_lt_set.clear(); rec_lt_en.clear();
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::EndStateBlock(IDirect3DStateBlock9 **out) { DLOCK;
  if (!out || !recording) return D3DERR_INVALIDCALL;
  Hbdx9StateBlock *b = recording;
  recording = nullptr; rec = nullptr;
  auto uniq = [](std::vector<DWORD> &v) { std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); };
  b->lt_set = std::move(rec_lt_set); b->lt_en = std::move(rec_lt_en); uniq(b->lt_set); uniq(b->lt_en);
  rec_lt_set.clear(); rec_lt_en.clear();
  b->capture();                       /* the recorded values = what the calls set */
  Hbdx9StateBlock *s0 = g_sb_snapshot; g_sb_snapshot = nullptr;
  s0->m = b->m; s0->ix.built = false; /* the calls were applied while recording: put those states back */
  s0->lt_all = false; s0->lt_set = b->lt_set; s0->lt_en = b->lt_en;
  s0->apply();
  /* a light first created while recording did not exist before: remove it again */
  for (int pass = 0; pass < 2; pass++)
    for (DWORD i : pass ? b->lt_en : b->lt_set)
      if (!s0->lt_find(i))
        for (size_t k = 0; k < lights.size(); k++)
          if (lights[k].index == i) { bool was = lights[k].enabled; lights.erase(lights.begin() + k); sb_note(SB_LIGHT, 0); if (was) send_lights(); break; }
  s0->Release();
  *out = b; return D3D_OK;
}

/* ---------------- queries (EVENT: GPU fence; OCCLUSION: Metal visibility counts) ---------------- */
class Hbdx9Query final : public Hbdx9Stub_IDirect3DQuery9<IDirect3DQuery9> {
public:
  LONG refs = 1;
  Hbdx9Device *dev; D3DQUERYTYPE type;
  uint32_t slot = ~0u, value = 0;
  bool issued = false, flushed = false, building = false;
  /* (INT32G) HBDX9_QUERY_PREDICT: the box set of the current issue, whether its prediction was looked up, the answer given early */
  Hbdx9Device::QKey key; bool keyed = false, pred_checked = false; uint32_t early_answer = 0;
  Hbdx9Query(Hbdx9Device *d, D3DQUERYTYPE t, uint32_t s) : dev(d), type(t), slot(s) { dev->AddRef(); }
  ~Hbdx9Query() {
    /* (INT32G fix) under the device lock - CreateQuery pops from these lists on another thread - and for occlusion queries
       too (they were never returned: CreateQuery failed after 511 occlusion queries per device).  The slot may be reused at
       once: the core writes a result together with the issue's own value, a later issue of the slot has another value. */
    { Hbdx9Lock l(&dev->cs, dev->locking);
      if (query_predict() && keyed) dev->qp_harvest(dev->qp_issue_entry(key, slot, value));
      if (slot != ~0u && type == D3DQUERYTYPE_TIMESTAMP) dev->free_slots3.push_back(slot);
      else if (slot != ~0u) (type == D3DQUERYTYPE_EVENT ? dev->free_slots : dev->free_slots2).push_back(slot); }
    dev->Release();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DQuery9)) { *out = this; AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override { if (!out) return D3DERR_INVALIDCALL; *out = dev; dev->AddRef(); return D3D_OK; }
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return type; }
  DWORD STDMETHODCALLTYPE GetDataSize() override {
    if (type == D3DQUERYTYPE_TIMESTAMP || type == D3DQUERYTYPE_TIMESTAMPFREQ) return sizeof(UINT64);   /* (DX9FULL) */
    return type == D3DQUERYTYPE_EVENT || type == D3DQUERYTYPE_TIMESTAMPDISJOINT ? sizeof(BOOL) : sizeof(DWORD);
  }
  /* (DX9FULL 13.11.2026) timestamp queries (DXVK d3d9_query.cpp): TIMESTAMP = the GPU end time in ns of the command buffer the
     core ends at the Issue (HBDX9_PKT_TIMESTAMP: one Metal command buffer per timestamp, not a sample inside a pass), FREQ =
     1 GHz, DISJOINT = FALSE (one GPU clock) */
  bool ts_type() const { return type == D3DQUERYTYPE_TIMESTAMP || type == D3DQUERYTYPE_TIMESTAMPFREQ || type == D3DQUERYTYPE_TIMESTAMPDISJOINT; }
  HRESULT ts_issue(DWORD flags) {
    Hbdx9Lock l(&dev->cs, dev->locking);
    if (type == D3DQUERYTYPE_TIMESTAMP) {
      if (!(flags & D3DISSUE_END)) return D3D_OK;
      value = ++dev->fence_value;
      auto *p = dev->add<hbdx9_pkt_fence>(HBDX9_PKT_TIMESTAMP); p->slot = slot; p->value = value;
    } else if (!(flags & D3DISSUE_END)) { building = true; return D3D_OK; }
    building = false; issued = true; flushed = false;
    return D3D_OK;
  }
  HRESULT ts_get(void *data, DWORD size) {
    Hbdx9Lock l(&dev->cs, dev->locking);
    if (building) return S_FALSE;
    if (type == D3DQUERYTYPE_TIMESTAMPFREQ) { if (data && size >= sizeof(UINT64)) *(UINT64 *)data = 1000000000ull; return S_OK; }
    if (type == D3DQUERYTYPE_TIMESTAMPDISJOINT) { if (data && size >= sizeof(BOOL)) *(BOOL *)data = FALSE; return S_OK; }
    if (!issued) return D3DERR_INVALIDCALL;
    if (__atomic_load_n((const uint32_t *)&dev->fence_page[slot + 2], __ATOMIC_ACQUIRE) != value) {
      if (!flushed) { dev->add<hbdx9_pkt_empty>(HBDX9_PKT_FLUSH); dev->flush(); flushed = true; }
      return S_FALSE;
    }
    if (data && size >= sizeof(UINT64)) *(UINT64 *)data = (UINT64)dev->fence_page[slot] | (UINT64)dev->fence_page[slot + 1] << 32;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override {
    if (ts_type()) return ts_issue(flags);
    Hbdx9Lock l(&dev->cs, dev->locking);
    if (type == D3DQUERYTYPE_EVENT) {
      if (!(flags & D3DISSUE_END)) return D3D_OK;
      value = ++dev->fence_value;
      auto *p = dev->add<hbdx9_pkt_fence>(HBDX9_PKT_FENCE); p->slot = slot; p->value = value;
    } else {
      if (flags & D3DISSUE_BEGIN) {
        if (early_on() && early_query_kb() && dev->unsent() >= (size_t)early_query_kb() * 1024 && dev->early_armed()) {
          if (dev->early_blocked) dev->early_n_blocked++; else { dev->early_n_query++; dev->flush(true); }
        }
        if (query_predict()) {
          /* the previous issue of this query object ends here: its result goes to its box set before the slot is used again */
          if (keyed) dev->qp_harvest(dev->qp_issue_entry(key, slot, value));
          keyed = false; pred_checked = false; early_answer = 0;
          dev->q_overlap = dev->q_open; dev->q_open = true; dev->q_owner = this; dev->q_key_state = 0;
        }
        auto *p = dev->add<hbdx9_pkt_query>(HBDX9_PKT_QUERY); p->slot = slot; p->begin = 1; building = true;
        return D3D_OK;
      }
      if (!(flags & D3DISSUE_END)) return D3D_OK;
      value = ++dev->fence_value;
      auto *p = dev->add<hbdx9_pkt_query>(HBDX9_PKT_QUERY); p->slot = slot; p->begin = 0; p->value = value; building = false;
      if (query_predict()) {
        keyed = dev->q_open && dev->q_owner == this && !dev->q_overlap && dev->q_key_state == 1;
        if (dev->q_owner == this) dev->q_open = false;
        pred_checked = false; early_answer = 0;
        if (keyed) {
          key = dev->q_key;
          Hbdx9Device::QPred *e = dev->qp_entry(key, true);
          if (e) { dev->qp_harvest(e); e->slot = slot; e->value = value; e->pending = 1; e->early = 0; e->stamp = ++dev->qstamp; }
          else keyed = false;
        }
      }
    }
    issued = true; flushed = false;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetData(void *data, DWORD size, DWORD flags) override {
    (void)flags;
    if (ts_type()) return ts_get(data, size);
    if (query_lockfree() && __atomic_load_n((const uint8_t *)&issued, __ATOMIC_RELAXED)) {
      /* (INT32G) HBDX9_QUERY_LOCKFREE: the two outcomes that write nothing.  issued/value/flushed are written only under the
         device lock (Issue: value first, then issued, then flushed = false; GetData: flushed = true after its flush); slot,
         type and dev never change; the fence page is written by the core's completion handler, outside the lock in every
         mode.  On one thread the answer is the locked path's answer for the same field values.  With an Issue of this very
         query running on another thread the answer is that of this call running just before it or just after it, or one
         "not ready" without the flush - the next poll takes the lock and flushes. */
      uint32_t ea = __atomic_load_n(&early_answer, __ATOMIC_RELAXED);      /* HBDX9_QUERY_PREDICT: this issue was answered already */
      if (ea) { if (data && size >= sizeof(DWORD)) *(DWORD *)data = ea; return S_OK; }
      uint32_t v = __atomic_load_n(&value, __ATOMIC_RELAXED);
      uint32_t at = type == D3DQUERYTYPE_EVENT ? slot : slot + 1;
      if (__atomic_load_n((const uint32_t *)&dev->fence_page[at], __ATOMIC_ACQUIRE) == v) {
        if (data && size >= sizeof(DWORD)) *(DWORD *)data = type == D3DQUERYTYPE_EVENT ? TRUE : dev->fence_page[slot];
        return S_OK;
      }
      if (__atomic_load_n((const uint8_t *)&flushed, __ATOMIC_RELAXED)) return S_FALSE;
    }
    Hbdx9Lock l(&dev->cs, dev->locking);
    if (early_answer) { if (data && size >= sizeof(DWORD)) *(DWORD *)data = early_answer; return S_OK; }
    if (!issued) {
      if (building) return S_FALSE;
      if (data && size >= sizeof(DWORD)) *(DWORD *)data = type == D3DQUERYTYPE_EVENT ? TRUE : 0;
      return S_OK;
    }
    uint32_t done_at = type == D3DQUERYTYPE_EVENT ? slot : slot + 1;
    if (__atomic_load_n((const uint32_t *)&dev->fence_page[done_at], __ATOMIC_ACQUIRE) != value) {
      /* not finished: hand the recorded work to the GPU once (D3DGETDATA_FLUSH or not: a spin loop must terminate) */
      if (query_predict() && type == D3DQUERYTYPE_OCCLUSION) {
        if (!pred_checked) {
          pred_checked = true;
          uint32_t c = dev->qp_predict(keyed ? &key : nullptr, slot, value);
          if (c) {
            /* visible at its previous finished test: the answer now; the hand-over stays where this poll would have put it */
            if (!flushed) { dev->qp_flush(); flushed = true; }
            early_answer = c;
            if (data && size >= sizeof(DWORD)) *(DWORD *)data = c;
            return S_OK;
          }
        }
        if (!flushed) { dev->qp_flush(); flushed = true; }
        return S_FALSE;
      }
      if (!flushed) { dev->early_note_wait(); dev->add<hbdx9_pkt_empty>(HBDX9_PKT_FLUSH); dev->flush(); flushed = true; dev->early_draws = 0; }
      return S_FALSE;
    }
    if (query_predict() && keyed) { dev->qp_harvest(dev->qp_issue_entry(key, slot, value)); dev->qp_exact++; }
    if (data && size >= sizeof(DWORD)) *(DWORD *)data = type == D3DQUERYTYPE_EVENT ? TRUE : dev->fence_page[slot];
    return S_OK;
  }
};
HRESULT STDMETHODCALLTYPE Hbdx9Device::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **out) { DLOCK;
  if (fmt_ext_on() && (core_caps & HBDX9_CAP_TIMESTAMP) &&
      (type == D3DQUERYTYPE_TIMESTAMP || type == D3DQUERYTYPE_TIMESTAMPFREQ || type == D3DQUERYTYPE_TIMESTAMPDISJOINT)) {
    /* (DX9FULL) */
    if (!out) return D3D_OK;
    if (!fence_page) return D3DERR_NOTAVAILABLE;
    uint32_t slot = ~0u;
    if (type == D3DQUERYTYPE_TIMESTAMP) {
      if (!free_slots3.empty()) { slot = free_slots3.back(); free_slots3.pop_back(); }
      else { if (next_slot + 3 > 1023) return D3DERR_NOTAVAILABLE; slot = next_slot; next_slot += 3; }
      fence_page[slot] = 0; fence_page[slot + 1] = 0; fence_page[slot + 2] = 0;
    }
    *out = new Hbdx9Query(this, type, slot); return D3D_OK;
  }
  if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION) { hbdx9_unimpl("IDirect3DDevice9::CreateQuery(type)"); return D3DERR_NOTAVAILABLE; }
  if (!out) return D3D_OK;
  if (!fence_page) return D3DERR_NOTAVAILABLE;
  uint32_t slot = alloc_slot(type == D3DQUERYTYPE_EVENT ? 1 : 2);
  if (slot == ~0u) return D3DERR_NOTAVAILABLE;
  fence_page[slot] = 0; if (type != D3DQUERYTYPE_EVENT) fence_page[slot + 1] = 0;
  *out = new Hbdx9Query(this, type, slot); return D3D_OK;
}

/* ---------------- implicit swap chain ---------------- */
class Hbdx9SwapChain final : public Hbdx9Stub_IDirect3DSwapChain9Ex<IDirect3DSwapChain9Ex> {
public:
  LONG refs = 1;
  Hbdx9Device *dev;
  explicit Hbdx9SwapChain(Hbdx9Device *d) : dev(d) { dev->AddRef(); }
  ~Hbdx9SwapChain() { dev->swapchain_obj = nullptr; dev->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3DSwapChain9) || (dev->ex && IsEqualGUID(riid, IID_IDirect3DSwapChain9Ex))) {
      *out = this; AddRef(); return S_OK;
    }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE Present(const RECT *, const RECT *, HWND, const RGNDATA *, DWORD flags) override { return dev->PresentEx(nullptr, nullptr, nullptr, nullptr, flags); }
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9 *dst) override { return dev->GetFrontBufferData(0, dst); }
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9 **out) override { return dev->GetBackBuffer(0, i, t, out); }
  HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *st) override { return dev->GetRasterStatus(0, st); }
  HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *m) override { return dev->GetDisplayMode(0, m); }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) override { if (!out) return D3DERR_INVALIDCALL; *out = dev; dev->AddRef(); return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS *pp) override { if (!pp) return D3DERR_INVALIDCALL; *pp = dev->pp; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *n) override { if (!n) return D3DERR_INVALIDCALL; *n = dev->frames; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS *) override { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX *m, D3DDISPLAYROTATION *r) override {
    IDirect3D9Ex *p = dev->parent; return p->GetAdapterDisplayModeEx(0, m, r);
  }
};
HRESULT STDMETHODCALLTYPE Hbdx9Device::GetSwapChain(UINT i, IDirect3DSwapChain9 **out) { DLOCK;
  if (!out || i) return D3DERR_INVALIDCALL;
  if (swapchain_obj) swapchain_obj->AddRef(); else swapchain_obj = new Hbdx9SwapChain(this);
  *out = swapchain_obj; return D3D_OK;
}

/* ---------------- copies between surfaces ---------------- */
static bool surface_target(IDirect3DSurface9 *s, uint64_t *native, UINT *level, UINT *face) {
  auto *x = static_cast<Hbdx9Surface *>(s);
  if (!x || !x->native) return false;
  *native = x->native; *level = x->level; *face = x->face; return true;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::StretchRect(IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const RECT *dr, D3DTEXTUREFILTERTYPE filter) { DLOCK;
  /* D3D9 wants DEFAULT-pool sources; should a MANAGED level come anyway, its lazy data goes first (and before the add) */
  if (src && static_cast<Hbdx9Surface *>(src)->container) upload_dirty(static_cast<Hbdx9Surface *>(src)->container);
  auto *p = add<hbdx9_pkt_blit>(HBDX9_PKT_BLIT);
  if (!surface_target(src, &p->src, &p->src_level, &p->src_face) || !surface_target(dst, &p->dst, &p->dst_level, &p->dst_face)) {
    packets.resize(packets.size() - ((sizeof(hbdx9_pkt_blit) + 7) & ~(size_t)7));
    return D3DERR_INVALIDCALL;
  }
  if (sr) p->src_rect = {sr->left, sr->top, sr->right, sr->bottom};
  if (dr) p->dst_rect = {dr->left, dr->top, dr->right, dr->bottom};
  p->filter = filter;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) { DLOCK;
  uint64_t native; UINT level, face;
  if (!surface_target(surface, &native, &level, &face)) return D3DERR_INVALIDCALL;
  auto *x = static_cast<Hbdx9Surface *>(surface);
  auto *p = add<hbdx9_pkt_color_fill>(HBDX9_PKT_COLOR_FILL);
  p->dst = native; p->level = level; p->face = face; p->color = color;
  p->rect = rect ? hbdx9_rect{rect->left, rect->top, rect->right, rect->bottom} : hbdx9_rect{0, 0, (int32_t)x->desc.Width, (int32_t)x->desc.Height};
  return D3D_OK;
}
/* guest memory and pitch of a SYSTEMMEM surface (offscreen plain or a level of a SYSTEMMEM texture) */
static bool surface_memory(Hbdx9Surface *s, BYTE **mem, UINT *pitch) {
  if (s->memory) { *mem = s->memory; *pitch = s->pitch; return true; }
  if (s->container) {
    UINT i = s->container->idx(s->face, s->level);
    if (!s->container->shadow[i]) return false;
    *mem = s->container->shadow[i]; *pitch = s->container->pitch[i]; return true;
  }
  return false;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::UpdateSurface(IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const POINT *dp) { DLOCK;
  auto *s = static_cast<Hbdx9Surface *>(src), *d = static_cast<Hbdx9Surface *>(dst);
  if (!s || !d || !d->native || s->desc.Format != d->desc.Format) return D3DERR_INVALIDCALL;
  BYTE *mem; UINT pitch;
  if (!surface_memory(s, &mem, &pitch)) return D3DERR_INVALIDCALL;
  FmtSize fs = fmt_size(s->desc.Format);
  RECT r = sr ? *sr : RECT{0, 0, (LONG)s->desc.Width, (LONG)s->desc.Height};
  POINT at = dp ? *dp : POINT{r.left, r.top};
  UINT w = r.right - r.left, h = r.bottom - r.top;
  if (!w || !h || at.x + w > d->desc.Width || at.y + h > d->desc.Height) return D3DERR_INVALIDCALL;
  UINT rows = (h + fs.block - 1) / fs.block;
  UINT size = (rows - 1) * pitch + ((w + fs.block - 1) / fs.block) * fs.bytes;
  if (d->container) upload_dirty(d->container);   /* keep stream order with lazy data of the destination */
  uint32_t q = upload_texture(d->native, d->level, d->face, at.x, at.y, w, h, pitch, size, mem + (r.top / fs.block) * pitch + (r.left / fs.block) * fs.bytes);
  if (q) { if (s->container) s->container->ptr_seq = q; else s->ptr_seq = q; }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) { DLOCK;
  TexCore *s = tex_core(src), *d = tex_core(dst);
  if (!s || !d || src->GetType() != dst->GetType() || !d->native || s->levels < d->levels) return D3DERR_INVALIDCALL;
  UINT skip = s->levels - d->levels;
  if (d->usage & D3DUSAGE_AUTOGENMIPMAP) skip = 0;
  upload_dirty(d);   /* keep stream order with lazy data of the destination */
  for (UINT layer = 0; layer < d->layers; layer++)
    for (UINT l = 0; l < d->levels; l++) {
      UINT si = s->idx(layer, l + skip);
      if (!s->shadow[si]) continue;
      FmtSize fs = fmt_size(d->format);
      UINT w = d->lw(l), h = d->lh(l), rows = (h + fs.block - 1) / fs.block;
      UINT size = (rows - 1) * s->pitch[si] + ((w + fs.block - 1) / fs.block) * fs.bytes;
      if (d->volume) {
        for (UINT z = 0; z < d->ld(l); z++) { uint32_t q = upload_texture(d->native, l, z, 0, 0, w, h, s->pitch[si], size, s->shadow[si] + z * s->slice[si]); if (q) s->ptr_seq = q; }
      } else { uint32_t q = upload_texture(d->native, l, layer, 0, 0, w, h, s->pitch[si], size, s->shadow[si]); if (q) s->ptr_seq = q; }
    }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::GetFrontBufferData(UINT sc, IDirect3DSurface9 *dst) { DLOCK;
  if (sc || !dst || !backbuffer) return D3DERR_INVALIDCALL;
  /* the last presented image is the back buffer content at Present (DISCARD leaves it intact here) */
  return GetRenderTargetData(backbuffer, dst);
}
HRESULT STDMETHODCALLTYPE Hbdx9Device::Reset(D3DPRESENT_PARAMETERS *params) { DLOCK;
  if (!params) return D3DERR_INVALIDCALL;
  flush();
  for (int i = 0; i < 20; i++) bind_texture(i, nullptr);
  for (auto &st : streams) { if (st.vb) st.vb->Release(); st = {}; }
  if (ib) { ib->Release(); ib = nullptr; }
  if (vs) { vs->Release(); vs = nullptr; }
  if (ps) { ps->Release(); ps = nullptr; }
  if (decl) { decl->Release(); decl = nullptr; }
  fvf = 0;
  for (auto &r : rt) if (r) { r->Release(); r = nullptr; }
  if (ds) { ds->Release(); ds = nullptr; }
  /* the swap chain owns its buffers: drop them (each held a device reference we gave back at creation) */
  if (backbuffer) { AddRef(); backbuffer->Release(); backbuffer = nullptr; }
  if (auto_depth) { AddRef(); auto_depth->Release(); auto_depth = nullptr; }
  if (native_swapchain) { hbdx9_release_params r{native_swapchain, 1, 0}; hb_call(HBDX9_CALL_RELEASE, &r); native_swapchain = 0; }
  lights.clear();
  memset(vsf, 0, sizeof(vsf)); memset(psf, 0, sizeof(psf)); memset(vsi, 0, sizeof(vsi)); memset(psi, 0, sizeof(psi));
  memset(vsb, 0, sizeof(vsb)); memset(psb, 0, sizeof(psb)); memset(&material, 0, sizeof(material)); memset(clip_planes, 0, sizeof(clip_planes));
  reset_shadow();
  add<hbdx9_pkt_empty>(HBDX9_PKT_RESET_STATE);
  flush();
  HRESULT hr = init(creation.hFocusWindow, params);
  if (SUCCEEDED(hr)) setup_fpu(create_flags, "Reset");   /* (DX9FULL v13) measured: Windows applies it again in Reset, not in ResetEx */
  sb_note_all();   /* (DX9FULL v13) every state went back to its default */
  hbdx9_log("Reset %ux%u -> %08lx", params->BackBufferWidth, params->BackBufferHeight, (unsigned long)hr);
  return hr;
}

HRESULT STDMETHODCALLTYPE Hbdx9Device::GetRenderTargetData(IDirect3DSurface9 *rt_, IDirect3DSurface9 *dst) { DLOCK;
  if (!rt_ || !dst) return D3DERR_INVALIDCALL;
  auto *src = static_cast<Hbdx9Surface *>(rt_); auto *d = static_cast<Hbdx9Surface *>(dst);
  BYTE *mem = d->memory; UINT pitch = d->pitch;
  if (!mem && d->container && (d->container->pool == D3DPOOL_SYSTEMMEM || d->container->pool == D3DPOOL_SCRATCH)) {
    UINT i = d->container->idx(d->face, d->level);
    if (!d->container->ensure_shadow(i, d->level)) return E_OUTOFMEMORY;
    mem = d->container->shadow[i]; pitch = d->container->pitch[i];
  }
  if (!src->native || !mem || src->samples > 1 || src->desc.Width != d->desc.Width || src->desc.Height != d->desc.Height) return D3DERR_INVALIDCALL;
  HRESULT hr = flush(); if (FAILED(hr)) return hr;
  hbdx9_readback_params r{native_device, src->native, (uint64_t)(uintptr_t)mem, pitch, d->desc.Width, d->desc.Height, src->level, 0, 0};
  NTSTATUS st = hb_call(HBDX9_CALL_READBACK, &r);
  return (st || r.status) ? D3DERR_DRIVERINTERNALERROR : D3D_OK;
}


/* ---------------- IDirect3D9Ex ---------------- */
class Hbdx9Interface final : public Hbdx9Stub_IDirect3D9Ex<IDirect3D9Ex> {
public:
  LONG refs = 1;
  bool ex;
  hbdx9_init_params info{};
  volatile uint32_t *fence_page = nullptr;

  // D3D9 snapshots the adapter's available modes when the interface is created.
  // Returning only the current desktop mode makes a game's saved fullscreen
  // resolution disappear whenever the desktop/Retina mode differs.
  std::vector<D3DDISPLAYMODE> display_modes;
  std::vector<D3DDISPLAYMODEEX> display_modes_ex;

  explicit Hbdx9Interface(bool is_ex) : ex(is_ex) {
    for (DWORD index = 0; ; ++index) {
      DEVMODEW dm{}; dm.dmSize = sizeof(dm);
      if (!EnumDisplaySettingsW(nullptr, index, &dm)) break;
      if (dm.dmBitsPerPel != 32 || !dm.dmPelsWidth || !dm.dmPelsHeight) continue;
      D3DDISPLAYMODEEX mode{}; mode.Size = sizeof(mode);
      mode.Width = dm.dmPelsWidth; mode.Height = dm.dmPelsHeight;
      mode.RefreshRate = dm.dmDisplayFrequency; mode.Format = D3DFMT_X8R8G8B8;
      mode.ScanLineOrdering = (dm.dmDisplayFlags & DM_INTERLACED)
          ? D3DSCANLINEORDERING_INTERLACED : D3DSCANLINEORDERING_PROGRESSIVE;
      display_modes_ex.push_back(mode);
    }
    auto less = [](const D3DDISPLAYMODEEX &a, const D3DDISPLAYMODEEX &b) {
      if (a.Width != b.Width) return a.Width < b.Width;
      if (a.Height != b.Height) return a.Height < b.Height;
      if (a.RefreshRate != b.RefreshRate) return a.RefreshRate < b.RefreshRate;
      return a.ScanLineOrdering < b.ScanLineOrdering;
    };
    std::sort(display_modes_ex.begin(), display_modes_ex.end(), less);
    display_modes_ex.erase(std::unique(display_modes_ex.begin(), display_modes_ex.end(),
        [&less](const D3DDISPLAYMODEEX &a, const D3DDISPLAYMODEEX &b) {
          return !less(a, b) && !less(b, a);
        }), display_modes_ex.end());
    for (const auto &mode : display_modes_ex) {
      if (!display_modes.empty()) {
        const auto &last = display_modes.back();
        if (last.Width == mode.Width && last.Height == mode.Height && last.RefreshRate == mode.RefreshRate) continue;
      }
      display_modes.push_back({mode.Width, mode.Height, mode.RefreshRate, mode.Format});
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirect3D9) || (ex && IsEqualGUID(riid, IID_IDirect3D9Ex))) {
      *out = this; AddRef(); return S_OK;
    }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }

  UINT STDMETHODCALLTYPE GetAdapterCount() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id) override {
    (void)flags;
    if (adapter || !id) return D3DERR_INVALIDCALL;
    memset(id, 0, sizeof(*id));
    lstrcpynA(id->Driver, "hbdx9.dll", sizeof(id->Driver));
    lstrcpynA(id->Description, info.name, sizeof(id->Description));
    lstrcpynA(id->DeviceName, "\\\\.\\DISPLAY1", sizeof(id->DeviceName));
    id->DriverVersion.QuadPart = 0x0006000E00010000ull; /* 6.14.1.0 */
    id->VendorId = info.vendor_id; id->DeviceId = info.device_id; id->WHQLLevel = 1;
    return D3D_OK;
  }
  static bool mode_format_ok(D3DFORMAT f) { return f == D3DFMT_X8R8G8B8; }
  static bool mode_filter_ok(const D3DDISPLAYMODEFILTER *filter) {
    return filter && filter->Size == sizeof(*filter) && mode_format_ok(filter->Format)
        && (filter->ScanLineOrdering == D3DSCANLINEORDERING_UNKNOWN
            || filter->ScanLineOrdering == D3DSCANLINEORDERING_PROGRESSIVE
            || filter->ScanLineOrdering == D3DSCANLINEORDERING_INTERLACED);
  }
  static bool mode_matches(const D3DDISPLAYMODEEX &mode, const D3DDISPLAYMODEFILTER &filter) {
    return filter.ScanLineOrdering == D3DSCANLINEORDERING_UNKNOWN
        || mode.ScanLineOrdering == filter.ScanLineOrdering;
  }
  static HRESULT current_display_mode(DEVMODEW &dm) {
    dm = {}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)
        || !dm.dmPelsWidth || !dm.dmPelsHeight || dm.dmBitsPerPel != 32) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
  }
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT f) override {
    return (!adapter && mode_format_ok(f)) ? (UINT)display_modes.size() : 0;
  }
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *mode) override {
    if (adapter || !mode) return D3DERR_INVALIDCALL;
    DEVMODEW dm{}; HRESULT hr = current_display_mode(dm);
    if (FAILED(hr)) return hr;
    *mode = {dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, D3DFMT_X8R8G8B8};
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT f, UINT index, D3DDISPLAYMODE *mode) override {
    if (adapter || !mode || !mode_format_ok(f) || index >= display_modes.size()) return D3DERR_INVALIDCALL;
    *mode = display_modes[index];
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT bb, BOOL windowed) override {
    (void)windowed;
    if (adapter || type != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    if (display != D3DFMT_X8R8G8B8 && display != D3DFMT_UNKNOWN) return D3DERR_NOTAVAILABLE;
    if (bb != D3DFMT_X8R8G8B8 && bb != D3DFMT_A8R8G8B8 && bb != D3DFMT_UNKNOWN) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE rtype, D3DFORMAT f) override {
    HRESULT hr = check_device_format(adapter, type, adapter_format, usage, rtype, f);
    FMT_TRACE("CheckDeviceFormat", f, usage, (DWORD)rtype, hr);
    return hr;
  }
  /* (DX9FULL 13.11.2026) DXVK D3D9Adapter::CheckDeviceFormat + CheckDeviceVkFormat for a vendor that is neither NVIDIA nor AMD
     (this adapter reports 0x106b).  Differences to the s8j answers: a depth format is a valid sampled TEXTURE without
     DEPTHSTENCIL usage (AUTOGENMIPMAP -> D3DOK_NOAUTOGEN; Crysis asks D24S8/D16 that way), an offscreen plain surface of a
     non-lockable depth format is NOTAVAILABLE, DF16/DF24 are depth textures, ATOC is offered, NVDB is offered only where
     the GPU honours Metal's depth bounds (DXVK: NVIDIA with depthBounds only), the remaining driver hacks are NOTAVAILABLE as
     DXVK answers them. */
  /* NVDB only where the GPU really honours Metal's depth bounds (measured by the core once: an Apple7 GPU ignores the call) */
  LONG nvdb_state = -1;
  bool depth_bounds_ok() {
    if (nvdb_state < 0) {
      LONG v = 0;
      if ((info.caps & HBDX9_CAP_FEATURES) && info.device) {
        hbdx9_features_params fp{}; fp.device = info.device; fp.query = 1;
        if (!hb_call(HBDX9_CALL_FEATURES, &fp) && fp.result == 1) v = 1;
      }
      nvdb_state = v;
      hbdx9_log("NVDB (depth bounds) %s", v ? "offered: the GPU honours Metal depth bounds" : "not offered: Metal depth bounds ignored or unknown on this GPU");
    }
    return nvdb_state == 1;
  }
  HRESULT check_device_format_ext(D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE rtype, D3DFORMAT f) {
    const DWORD v = (DWORD)f;
    if (adapter_format == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;
    if (rtype == D3DRTYPE_VERTEXBUFFER || rtype == D3DRTYPE_INDEXBUFFER) return D3DERR_INVALIDCALL;
    const bool rt = usage & D3DUSAGE_RENDERTARGET, ds = usage & D3DUSAGE_DEPTHSTENCIL;
    const bool surface = rtype == D3DRTYPE_SURFACE, texture = rtype == D3DRTYPE_TEXTURE;
    const bool volume = rtype == D3DRTYPE_VOLUMETEXTURE || rtype == D3DRTYPE_VOLUME, cube = rtype == D3DRTYPE_CUBETEXTURE;
    const bool depth = is_depth(f) || f == D3DFMT_S8_LOCKABLE;
    const bool lockable_depth = f == D3DFMT_D16_LOCKABLE || f == D3DFMT_D32_LOCKABLE || f == D3DFMT_D32F_LOCKABLE || f == D3DFMT_S8_LOCKABLE;
    if (ds && !depth) return D3DERR_NOTAVAILABLE;
    if (surface && !(rt || ds) && depth && !lockable_depth) return D3DERR_NOTAVAILABLE;
    if (rt && volume) return D3DERR_NOTAVAILABLE;
    if (v == MAKEFOURCC('N', 'U', 'L', 'L')) return rt && (surface || texture) ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (surface) {
      if (v == MAKEFOURCC('A', 'T', 'O', 'C')) return D3D_OK;
      if (v == MAKEFOURCC('N', 'V', 'D', 'B')) return depth_bounds_ok() ? D3D_OK : D3DERR_NOTAVAILABLE;
      if (v == MAKEFOURCC('R', 'E', 'S', 'Z') || v == MAKEFOURCC('S', 'S', 'A', 'A') || v == MAKEFOURCC('R', '2', 'V', 'B') ||
          v == MAKEFOURCC('I', 'N', 'S', 'T') || v == MAKEFOURCC('C', 'E', 'N', 'T')) return D3DERR_NOTAVAILABLE;
    }
    if (usage & D3DUSAGE_DMAP) return D3DERR_NOTAVAILABLE;
    FmtSize fs = fmt_size(f);
    if (!fs.bytes) return D3DERR_NOTAVAILABLE;
    if (usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE)) {
      switch (v) {
      case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
      case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: break;
      default: return D3DERR_NOTAVAILABLE;
      }
    }
    if (depth && (cube || volume)) return D3DERR_NOTAVAILABLE;
    if (rt && (depth || fs.block > 1 || is_converted(f))) return D3DERR_NOTAVAILABLE;          /* a colour attachment */
    if (rt && f == D3DFMT_A8 && !app_opt().a8rt) return D3DERR_NOTAVAILABLE;                  /* DXVK d3d9.disableA8RT */
    if ((usage & D3DUSAGE_AUTOGENMIPMAP) && (fs.block > 1 || depth || volume)) return D3DOK_NOAUTOGEN;
    return D3D_OK;
  }
  HRESULT check_device_format(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE rtype, D3DFORMAT f) {
    if (adapter || type != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    if (fmt_ext_on()) return check_device_format_ext(adapter_format, usage, rtype, f);
    FmtSize fs = fmt_size(f);
    if (!fs.bytes) return D3DERR_NOTAVAILABLE;
    bool depth = is_depth(f);
    if ((rtype == D3DRTYPE_VOLUMETEXTURE || rtype == D3DRTYPE_VOLUME) && (depth || (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))))
      return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_DEPTHSTENCIL) && !depth) return D3DERR_NOTAVAILABLE;
    if (depth && rtype == D3DRTYPE_CUBETEXTURE) return D3DERR_NOTAVAILABLE;
    if (depth && rtype == D3DRTYPE_TEXTURE && !(usage & D3DUSAGE_DEPTHSTENCIL)) return D3DERR_NOTAVAILABLE;
    if ((DWORD)f == MAKEFOURCC('N', 'U', 'L', 'L') && !(usage & D3DUSAGE_RENDERTARGET)) return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_RENDERTARGET) && (depth || fs.block > 1)) return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_AUTOGENMIPMAP) && (fs.block > 1 || depth || rtype == D3DRTYPE_VOLUMETEXTURE)) return D3DOK_NOAUTOGEN;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE ms, DWORD *q) override {
    if (adapter) return D3DERR_INVALIDCALL;
    UINT max = info.max_samples ? info.max_samples : 1;
    if (ms == D3DMULTISAMPLE_NONE) { if (q) *q = 1; return D3D_OK; }
    if (ms == D3DMULTISAMPLE_NONMASKABLE) { if (q) *q = max >= 4 ? 2 : max >= 2 ? 1 : 0; return max >= 2 ? D3D_OK : D3DERR_NOTAVAILABLE; }
    if ((UINT)ms > max || ((UINT)ms & ((UINT)ms - 1))) return D3DERR_NOTAVAILABLE;
    if (q) *q = 1;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT rtf, D3DFORMAT dsf) override {
    if (!fmt_ext_on()) return D3D_OK;
    /* (DX9FULL) DXVK: the depth format must be a depth format this layer creates; the RT format NULL or one it creates */
    if (!is_depth(dsf) || !fmt_size(dsf).bytes) return D3DERR_NOTAVAILABLE;
    if ((DWORD)rtf == MAKEFOURCC('N', 'U', 'L', 'L')) return D3D_OK;
    return fmt_size(rtf).bytes && !is_depth(rtf) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT, D3DDEVTYPE, D3DFORMAT src, D3DFORMAT dst) override {
    if (!fmt_ext_on()) return D3D_OK;
    /* (DX9FULL) DXVK: StretchRect format conversion from a back-buffer format or a DXT format into one of its list */
    bool s_ok = false, d_ok = false;
    switch ((DWORD)src) {
    case D3DFMT_A2R10G10B10: case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: case D3DFMT_R5G6B5:
    case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
    case D3DFMT_UYVY: case D3DFMT_YUY2: case D3DFMT_R8G8_B8G8: case D3DFMT_G8R8_G8B8: s_ok = true; break;
    default: break;
    }
    switch ((DWORD)dst) {
    case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_R5G6B5: case D3DFMT_X8R8G8B8: case D3DFMT_A8R8G8B8: case D3DFMT_A2R10G10B10:
    case D3DFMT_A16B16G16R16: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A16B16G16R16F:
    case D3DFMT_A32B32G32R32F: d_ok = true; break;
    default: break;
    }
    return s_ok && d_ok ? D3D_OK : D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *c) override {
    if (adapter || !c) return D3DERR_INVALIDCALL;
    memset(c, 0, sizeof(*c));
    c->DeviceType = type; c->AdapterOrdinal = 0;
    c->Caps2 = D3DCAPS2_CANAUTOGENMIPMAP | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_FULLSCREENGAMMA;
    c->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
    c->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
    c->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                 D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_DRAWPRIMITIVES2 |
                 D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE;
    c->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                           D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_SEPARATEALPHABLEND |
                           D3DPMISCCAPS_INDEPENDENTWRITEMASKS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING;
    c->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS |
                    D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_FOGVERTEX |
                    D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_WFOG |
                    D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;
    c->ZCmpCaps = c->AlphaCmpCaps = 0xff;
    c->SrcBlendCaps = c->DestBlendCaps = 0x1fff;
    c->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_SPECULARGOURAUDRGB;
    c->TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_PERSPECTIVE |
                     D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPCUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPVOLUMEMAP;
    c->CubeTextureFilterCaps = c->VolumeTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                     D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    c->VolumeTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_INDEPENDENTUV;
    c->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    c->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC |
                           D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    c->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER |
                            D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    c->StencilCaps = 0xff | D3DSTENCILCAPS_TWOSIDED;
    c->TextureOpCaps = 0x03ffffff; c->FVFCaps = 8; c->VertexProcessingCaps = D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
    c->MaxTextureWidth = c->MaxTextureHeight = 8192; c->MaxVolumeExtent = 2048;
    c->MaxTextureRepeat = 8192; c->MaxTextureAspectRatio = 8192; c->MaxAnisotropy = 16;
    c->MaxVertexW = 1e10f; c->GuardBandLeft = -32768.0f; c->GuardBandTop = -32768.0f; c->GuardBandRight = 32768.0f; c->GuardBandBottom = 32768.0f;
    c->MaxTextureBlendStages = 8; c->MaxSimultaneousTextures = 8; c->MaxActiveLights = 8; c->MaxUserClipPlanes = 6;
    c->MaxVertexBlendMatrices = 4; c->MaxVertexBlendMatrixIndex = 0; c->MaxPointSize = 256.0f;
    c->MaxPrimitiveCount = 0x00555555; c->MaxVertexIndex = 0x00ffffff; c->MaxStreams = 16; c->MaxStreamStride = 508;
    c->VertexShaderVersion = D3DVS_VERSION(3, 0); c->MaxVertexShaderConst = 256;
    c->PixelShaderVersion = D3DPS_VERSION(3, 0); c->PixelShader1xMaxValue = 65504.0f;
    c->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET; c->DeclTypes = 0x3ff;
    c->NumSimultaneousRTs = 4; c->MaxVShaderInstructionsExecuted = 0xffffffff; c->MaxPShaderInstructionsExecuted = 0xffffffff;
    c->MaxVertexShader30InstructionSlots = 32768; c->MaxPixelShader30InstructionSlots = 32768;
    c->VS20Caps.Caps = D3DVS20CAPS_PREDICATION; c->VS20Caps.DynamicFlowControlDepth = 24; c->VS20Caps.NumTemps = 32; c->VS20Caps.StaticFlowControlDepth = 4;
    c->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS | D3DPS20CAPS_PREDICATION |
                       D3DPS20CAPS_NODEPENDENTREADLIMIT | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    c->PS20Caps.DynamicFlowControlDepth = 24; c->PS20Caps.NumTemps = 32; c->PS20Caps.StaticFlowControlDepth = 4; c->PS20Caps.NumInstructionSlots = 512;
    c->NumberOfAdaptersInGroup = 1; c->MasterAdapterOrdinal = 0; c->AdapterOrdinalInGroup = 0;
    if (fmt_ext_on()) {
      /* (DX9FULL 13.11.2026) bits DXVK reports that this layer implements (matrix reports/DX9FULL-MATRIX.md); NOT added because
         not implemented here: LINEAR_TO_SRGB_PRESENTATION (D3DPRESENT_LINEAR_CONTENT),
         FOGVERTEXCLAMPED (vertex fog is computed per pixel from the interpolated coordinate), TWEENING, ALPHAPALETTE (no palette formats),
         LINECAPS_ANTIALIAS; MaxTextureWidth stays 8192 (DXVK 16384), MaxVolumeExtent 2048 (Metal's 3D limit) */
      c->Caps |= D3DCAPS_READ_SCANLINE;   /* GetRasterStatus as DXVK (time-based scan line) */
      c->CursorCaps = D3DCURSORCAPS_COLOR;   /* SetCursorProperties: Win32 colour cursor */
      c->PresentationIntervals |= D3DPRESENT_INTERVAL_TWO | D3DPRESENT_INTERVAL_THREE | D3DPRESENT_INTERVAL_FOUR;   /* core: afterMinimumDuration */
      c->DevCaps |= D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_CANBLTSYSTONONLOCAL;
      c->PrimitiveMiscCaps |= D3DPMISCCAPS_CLIPTLVERTS | D3DPMISCCAPS_FOGANDSPECULARALPHA | D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS |
                              D3DPMISCCAPS_POSTBLENDSRGBCONVERT | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_PERSTAGECONSTANT;   /* core FFP: RESULTARG TEMP, CONSTANT */
      /* (DX9FULL v13) points are quads whose corners carry their own clip distances (dxso point expansion before the clip
         planes): a scaled point is cut by a user clip plane through it (probe D3D9_X_CLIPPOINT) */
      c->PrimitiveMiscCaps |= D3DPMISCCAPS_CLIPPLANESCALEDPOINTS;
      c->RasterCaps |= D3DPRASTERCAPS_COLORPERSPECTIVE;
      c->SrcBlendCaps = c->DestBlendCaps = 0x3fff;   /* ZERO .. BLENDFACTOR (BOTHSRCALPHA/BOTHINVSRCALPHA now honoured) */
      c->ShadeCaps |= D3DPSHADECAPS_FOGGOURAUD;
      c->TextureCaps |= D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE;
      c->TextureFilterCaps |= D3DPTFILTERCAPS_MAGFANISOTROPIC;
      c->CubeTextureFilterCaps = c->VolumeTextureFilterCaps = c->TextureFilterCaps;
      c->VolumeTextureAddressCaps = c->TextureAddressCaps;
      c->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;
      c->VertexProcessingCaps |= D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_TEXGEN_SPHEREMAP;
      c->FVFCaps |= D3DFVFCAPS_PSIZE;   /* point size per vertex (core: point expansion) */
      c->DevCaps2 |= D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES;
      c->VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    }
    return D3D_OK;
  }
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override {
    if (adapter) return nullptr;
    POINT pt = {0, 0}; return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  }
  HRESULT create(HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9Ex **out) {
    if (!pp || !out) return D3DERR_INVALIDCALL;
    /* (INT32G fix, patch 0006) one native device per live device: the core keeps one state machine and one SUBMIT sequence per
       native device.  If the IDirect3D9 object's native device is in use by a live device, this one gets its own. */
    uint64_t ndev = info.device; volatile uint32_t *page = fence_page; uint32_t caps = info.caps; UINT ms = info.max_samples ? info.max_samples : 1;
    bool taken = false;
    for (auto *o : g_devices) if (o && o->native_device == info.device) taken = true;
    if (taken) {
      hbdx9_init_params i2{};
      i2.version = HBDX9_PROTOCOL_VERSION; memcpy(i2.exe_name, info.exe_name, sizeof(i2.exe_name));
      page = (volatile uint32_t *)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   /* never freed: the core writes to it */
      i2.fence_page = (uint64_t)(uintptr_t)page;
      NTSTATUS st2 = page ? hb_call(HBDX9_CALL_INIT, &i2) : (NTSTATUS)-1;
      if (st2 || i2.status) { hbdx9_log("native init for a second live device failed st=%08lx status=%u", (unsigned long)st2, i2.status); *out = nullptr; return D3DERR_NOTAVAILABLE; }
      ndev = i2.device; caps = i2.caps; ms = i2.max_samples ? i2.max_samples : 1;
      hbdx9_log("second live device on one Direct3D object: its own native device");
    }
    auto *d = new Hbdx9Device(this, ndev, ex, page, ms);
    d->core_caps = caps;
    d->creation = {0, D3DDEVTYPE_HAL, focus, flags};
    d->locking = (flags & D3DCREATE_MULTITHREADED) || env_on("HBDX9_DEVICE_LOCK");   /* (DX9FULL) as DXVK */
    hbdx9_log("device lock %s (D3DCREATE_MULTITHREADED %s)", d->locking ? "on" : "off", (flags & D3DCREATE_MULTITHREADED) ? "set" : "not set");
    if (fmt_ext_on()) hbdx9_log("app options (DXVK config) %s: df=%d x4r4g4b4=%d a8rt=%d", app_opt().name, app_opt().df, app_opt().x4r4g4b4, app_opt().a8rt);
    HRESULT hr = d->init(focus, pp);
    if (FAILED(hr)) { d->Release(); *out = nullptr; return hr; }
    d->create_flags = flags;
    setup_fpu(flags);   /* (DX9FULL v13) after a successful creation; Windows does the same again in Reset, see setup_fpu */
    *out = d; return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                         IDirect3DDevice9 **out) override {
    if (adapter || type != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    IDirect3DDevice9Ex *d = nullptr; HRESULT hr = create(focus, flags, pp, &d);
    if (out) *out = d;
    return hr;
  }
  UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT adapter, const D3DDISPLAYMODEFILTER *filter) override {
    if (adapter || !mode_filter_ok(filter)) return 0;
    return (UINT)std::count_if(display_modes_ex.begin(), display_modes_ex.end(),
        [filter](const D3DDISPLAYMODEEX &mode) { return mode_matches(mode, *filter); });
  }
  HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT adapter, const D3DDISPLAYMODEFILTER *filter, UINT index, D3DDISPLAYMODEEX *m) override {
    if (adapter || !mode_filter_ok(filter) || !m || m->Size != sizeof(*m)) return D3DERR_INVALIDCALL;
    for (const auto &mode : display_modes_ex) {
      if (!mode_matches(mode, *filter)) continue;
      if (index-- == 0) { *m = mode; return D3D_OK; }
    }
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT adapter, D3DDISPLAYMODEEX *m, D3DDISPLAYROTATION *rot) override {
    if (adapter || (m && m->Size != sizeof(*m))) return D3DERR_INVALIDCALL;
    DEVMODEW dm{}; HRESULT hr = current_display_mode(dm);
    if (FAILED(hr)) return hr;
    if (m) {
      *m = {}; m->Size = sizeof(*m);
      m->Width = dm.dmPelsWidth; m->Height = dm.dmPelsHeight;
      m->RefreshRate = dm.dmDisplayFrequency; m->Format = D3DFMT_X8R8G8B8;
      m->ScanLineOrdering = (dm.dmDisplayFlags & DM_INTERLACED)
          ? D3DSCANLINEORDERING_INTERLACED : D3DSCANLINEORDERING_PROGRESSIVE;
    }
    if (rot) {
      switch (dm.dmDisplayOrientation) {
        case DMDO_90: *rot = D3DDISPLAYROTATION_90; break;
        case DMDO_180: *rot = D3DDISPLAYROTATION_180; break;
        case DMDO_270: *rot = D3DDISPLAYROTATION_270; break;
        default: *rot = D3DDISPLAYROTATION_IDENTITY; break;
      }
    }
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                           D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **out) override {
    if (adapter || type != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    return create(focus, flags, pp, out);
  }
  HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT adapter, LUID *luid) override {
    if (adapter || !luid) return D3DERR_INVALIDCALL;
    luid->LowPart = 0x4842; luid->HighPart = 0; return D3D_OK;
  }
};

static Hbdx9Interface *make_interface(bool ex) {
  auto *i = new Hbdx9Interface(ex);
  i->info.version = HBDX9_PROTOCOL_VERSION;
  {
    char path[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, path, sizeof(path));
    const char *name = path;
    for (DWORD k = 0; k < n; k++) if (path[k] == '\\' || path[k] == '/') name = path + k + 1;
    lstrcpynA(i->info.exe_name, n ? name : "", sizeof(i->info.exe_name));
  }
  i->fence_page = (volatile uint32_t *)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  i->info.fence_page = (uint64_t)(uintptr_t)i->fence_page;
  NTSTATUS st = hb_call(HBDX9_CALL_INIT, &i->info);
  if (st || i->info.status) { hbdx9_log("native init failed st=%08lx status=%u", (unsigned long)st, i->info.status); i->Release(); return nullptr; }
  hbdx9_log("Direct3DCreate9%s adapter=\"%s\" vendor=%04x protocol=%u", ex ? "Ex" : "", i->info.name, i->info.vendor_id, HBDX9_PROTOCOL_VERSION);
  return i;
}

extern "C" {
IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk) { (void)sdk; return make_interface(false); }
HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out) {
  (void)sdk; if (!out) return D3DERR_INVALIDCALL;
  *out = make_interface(true); return *out ? D3D_OK : D3DERR_NOTAVAILABLE;
}
int WINAPI D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR) { return 0; }
int WINAPI D3DPERF_EndEvent(void) { return 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR, LPCWSTR) {}
void WINAPI D3DPERF_SetRegion(D3DCOLOR, LPCWSTR) {}
BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { return FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD) {}
DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }
void WINAPI DebugSetMute(void) {}
int WINAPI DebugSetLevel(void) { return 0; }
void *WINAPI Direct3DShaderValidatorCreate9(void) { hbdx9_unimpl("Direct3DShaderValidatorCreate9"); return nullptr; }
void WINAPI Direct3D9EnableMaximizedWindowedModeShim(UINT) {}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
  if (reason == DLL_PROCESS_DETACH) {
    for (auto *d : g_devices) if (d) d->report();   /* devices never released: counters still reach the log */
    tape_finish();
  }
  return TRUE;
}
}
