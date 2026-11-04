// SPDX-License-Identifier: LGPL-2.1-or-later
// CPU-LATE (23.10.2026): see d3d12_layer_time.hpp. Slow paths of the layer-time instrument: outermost leave, queue
// events, the reporter thread.
#include "d3d12_layer_time.hpp"
#include <tlhelp32.h>

namespace dxmt::layer_time {

namespace {

uint64_t g_freq = 0, g_b[kBuckets - 1] = {};
uint64_t g_first = 0;              // first registration (ticks), start of window 1
thread_local const char *t_tag = nullptr;

void Init() {
  if (g_freq) return;
  uint64_t f = Frequency();
  if (!f) f = 24000000;
  const uint64_t limits_us[kBuckets - 1] = {100, 500, 2000, 5000, 10000};
  for (unsigned i = 0; i < kBuckets - 1; ++i) g_b[i] = f * limits_us[i] / 1000000;
  g_freq = f;
}

unsigned BucketOf(uint64_t interval) {
  unsigned b = 0;
  while (b < kBuckets - 1 && interval >= g_b[b]) ++b;
  return b;
}

struct Out {
  std::string text;
  void Line(const char *format, ...) {
    char line[2048];
    va_list args; va_start(args, format);
    const int n = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n > 0 && size_t(n) < sizeof(line)) text.append(line, size_t(n));
    if (text.size() > 12000) Flush();
  }
  void Flush() {
    DWORD written;
    if (!text.empty()) WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.data(), DWORD(text.size()), &written, nullptr);
    text.clear();
  }
};

std::string ThreadName(DWORD tid) {
  using Fn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
  static Fn fn = [] {
    HMODULE m = GetModuleHandleW(L"kernelbase.dll");
    if (!m) m = GetModuleHandleW(L"kernel32.dll");
    return m ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(m, "GetThreadDescription"))) : nullptr;
  }();
  std::string name;
  if (!fn) return name;
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (!h) return name;
  PWSTR w = nullptr;
  if (SUCCEEDED(fn(h, &w)) && w) {
    for (const wchar_t *p = w; *p && name.size() < 48; ++p)
      name.push_back(*p > 32 && *p < 127 && *p != '=' ? char(*p) : '_');
    LocalFree(w);
  }
  CloseHandle(h);
  return name;
}

uint64_t g_window = 0, g_prev_now = 0, g_prev_presents = 0;

// PE modules of the process (base, size, path): to attribute profiler samples by address. Read-only, once per dump.
void DumpModules(Out &out, uint64_t window) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
  if (s == INVALID_HANDLE_VALUE) {
    out.Line("dx12_layer_time_module window=%llu error=%lu\n", (unsigned long long)window, GetLastError());
    return;
  }
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  unsigned n = 0;
  for (BOOL ok = Module32FirstW(s, &me); ok && n < 512; ok = Module32NextW(s, &me), ++n) {
    char path[600]; size_t k = 0;
    for (const wchar_t *c = me.szExePath; *c && k + 1 < sizeof(path); ++c) path[k++] = *c > 32 && *c < 127 ? char(*c) : '_';
    path[k] = 0;
    out.Line("dx12_layer_time_module window=%llu base=0x%llx size=0x%lx path=%s\n", (unsigned long long)window,
             (unsigned long long)(uintptr_t)me.modBaseAddr, (unsigned long)me.modBaseSize, path);
  }
  CloseHandle(s);
}

void Report() {
  const uint64_t r0 = Ticks();
  LARGE_INTEGER q{}, qf{};
  QueryPerformanceCounter(&q);
  QueryPerformanceFrequency(&qf);
  const uint64_t r1 = Ticks();
  const uint64_t now = (r0 + r1) / 2;
  const uint64_t presents = G().presents.load(std::memory_order_relaxed);
  const uint64_t window = ++g_window;
  const uint64_t begin = g_prev_now ? g_prev_now : g_first;
  const uint64_t frames = presents - g_prev_presents;
  g_prev_now = now; g_prev_presents = presents;
  if (!frames || now <= begin) return;
  const double F = double(frames), ms = 1000.0 / double(g_freq);
  const double qpc_per_tick = qf.QuadPart > 0 ? double(qf.QuadPart) / double(g_freq) : 0.0;
  std::vector<Block *> blocks;
  { std::lock_guard lock(G().mutex); blocks = G().blocks; }
  Out out;
  out.Line("dx12_layer_time window=%llu frames=%llu wall_ms_per_frame=%.3f threads=%zu freq=%llu qpc=%lld qpc_freq=%lld "
           "cntvct=%llu methods=%u period=%u version=1\n", (unsigned long long)window, (unsigned long long)frames,
           double(now - begin) * ms / F, blocks.size(), (unsigned long long)g_freq, (long long)q.QuadPart,
           (long long)qf.QuadPart, (unsigned long long)now, kMethods, g_period);
  for (Block *b : blocks) {
    uint64_t dt[kMethods], dc[kMethods], tt = 0, tc = 0, cls[ClsCount] = {};
    for (unsigned m = 0; m < kMethods; ++m) {
      const uint64_t t = b->ticks[m].load(std::memory_order_relaxed), c = b->calls[m].load(std::memory_order_relaxed);
      dt[m] = t - b->prev_ticks[m]; dc[m] = c - b->prev_calls[m];
      b->prev_ticks[m] = t; b->prev_calls[m] = c;
      tt += dt[m]; tc += dc[m]; cls[kTable[m].cls] += dt[m];
    }
    uint64_t da[EvCount][kBuckets][kAggFields];
    uint64_t events = 0;
    for (unsigned e = 0; e < EvCount; ++e)
      for (unsigned k = 0; k < kBuckets; ++k)
        for (unsigned f = 0; f < kAggFields; ++f) {
          const uint64_t v = b->agg[e][k][f].load(std::memory_order_relaxed);
          da[e][k][f] = v - b->prev_agg[e][k][f]; b->prev_agg[e][k][f] = v;
          if (f == 0) events += da[e][k][f];
        }
    if (!tc) continue;
    const std::string name = ThreadName(b->tid);
    char classes[1024]; size_t used = 0; classes[0] = 0;
    unsigned order[ClsCount];
    for (unsigned c = 0; c < ClsCount; ++c) order[c] = c;
    std::sort(order, order + ClsCount, [&](unsigned x, unsigned y) { return cls[x] > cls[y]; });
    for (unsigned i = 0; i < ClsCount && cls[order[i]]; ++i) {
      const int n = std::snprintf(classes + used, sizeof(classes) - used, "%s%s:%.3f", used ? "," : "", ClassName(order[i]),
                                  double(cls[order[i]]) * ms / F);
      if (n <= 0 || size_t(n) >= sizeof(classes) - used) break;
      used += size_t(n);
    }
    out.Line("dx12_layer_time_thread window=%llu tid=%u name=%s tag=%s calls_per_frame=%.1f layer_ms_per_frame=%.3f "
             "events_per_frame=%.2f classes=%s\n", (unsigned long long)window, b->tid, name.empty() ? "-" : name.c_str(),
             b->tag ? b->tag : "-", double(tc) / F, double(tt) * ms / F, double(events) / F, used ? classes : "-");
    unsigned idx[kMethods];
    for (unsigned m = 0; m < kMethods; ++m) idx[m] = m;
    std::sort(idx, idx + kMethods, [&](unsigned x, unsigned y) { return dt[x] > dt[y]; });
    for (unsigned i = 0; i < 24 && i < kMethods && dc[idx[i]]; ++i) {
      const unsigned m = idx[i];
      out.Line("dx12_layer_time_method window=%llu tid=%u method=%s class=%s calls_per_frame=%.2f ms_per_frame=%.4f "
               "us_per_call=%.3f\n", (unsigned long long)window, b->tid, kTable[m].name, ClassName(kTable[m].cls),
               double(dc[m]) / F, double(dt[m]) * ms / F, double(dt[m]) * ms * 1000.0 / double(dc[m]));
    }
    for (unsigned e = 0; e < EvCount; ++e)
      for (unsigned k = 0; k < kBuckets; ++k) {
        const uint64_t *a = da[e][k];
        if (!a[0]) continue;
        char top[512]; size_t u = 0; top[0] = 0;
        unsigned o2[ClsCount];
        for (unsigned c = 0; c < ClsCount; ++c) o2[c] = c;
        std::sort(o2, o2 + ClsCount, [&](unsigned x, unsigned y) { return a[4 + x] > a[4 + y]; });
        for (unsigned i = 0; i < 6 && a[4 + o2[i]]; ++i) {
          const int n = std::snprintf(top + u, sizeof(top) - u, "%s%s:%.3f", u ? "," : "", ClassName(o2[i]), double(a[4 + o2[i]]) * ms / F);
          if (n <= 0 || size_t(n) >= sizeof(top) - u) break;
          u += size_t(n);
        }
        out.Line("dx12_layer_time_event window=%llu tid=%u event=%s since_prev=%s count_per_frame=%.3f interval_ms_per_frame=%.3f "
                 "inlayer_ms_per_frame=%.3f outside_ms_per_frame=%.3f event_ms_per_frame=%.3f mean_interval_us=%.1f inlayer=%s\n",
                 (unsigned long long)window, b->tid, EventName(e), BucketName(k), double(a[0]) / F, double(a[1]) * ms / F,
                 double(a[3]) * ms / F, double(a[1] > a[3] ? a[1] - a[3] : 0) * ms / F, double(a[2]) * ms / F,
                 double(a[1]) * ms * 1000.0 / double(a[0]), u ? top : "-");
      }
    if (window == g_detail && b->ring) {
      const uint64_t head = b->head.load(std::memory_order_acquire);
      const uint64_t first = head > kRing - 64 ? head - (kRing - 64) : 0;
      uint64_t prev_t1 = 0;
      for (uint64_t i = first; i < head; ++i) {
        const Ev &v = b->ring[i % kRing];
        char top[512]; size_t u = 0; top[0] = 0;
        for (unsigned c = 0; c < ClsCount; ++c)
          if (v.cls[c]) {
            const int n = std::snprintf(top + u, sizeof(top) - u, "%s%s:%.1f", u ? "," : "", ClassName(c), double(v.cls[c]) * ms * 1000.0);
            if (n <= 0 || size_t(n) >= sizeof(top) - u) break;
            u += size_t(n);
          }
        const long long qpc0 = (long long)q.QuadPart - (long long)(double(int64_t(now) - int64_t(v.t0)) * qpc_per_tick);
        out.Line("dx12_layer_time_ev window=%llu tid=%u n=%llu ev=%s method=%s aux=0x%x presents=%llu t0=%llu qpc0=%lld "
                 "dur_us=%.1f since_prev_us=%.1f inlayer_us=%.1f calls=%u inlayer=%s\n", (unsigned long long)window, b->tid,
                 (unsigned long long)i, EventName(v.kind), v.id < kMethods ? kTable[v.id].name : "?", v.aux,
                 (unsigned long long)v.presents, (unsigned long long)v.t0, qpc0, double(v.t1 - v.t0) * ms * 1000.0,
                 prev_t1 && v.t0 > prev_t1 ? double(v.t0 - prev_t1) * ms * 1000.0 : -1.0, double(v.inlayer) * ms * 1000.0,
                 v.calls, u ? top : "-");
        prev_t1 = v.t1;
      }
    }
  }
  if (window == 1 || window == g_detail) DumpModules(out, window);
  if (const uint64_t ops = g_race_ops.load(std::memory_order_relaxed))
    out.Line("dx12_desc_race window=%llu ops=%llu write_write=%llu write_read=%llu\n", (unsigned long long)window,
             (unsigned long long)ops, (unsigned long long)g_race_write_write.load(std::memory_order_relaxed),
             (unsigned long long)g_race_write_read.load(std::memory_order_relaxed));
  out.Line("dx12_layer_time_end window=%llu report_us=%.1f\n", (unsigned long long)window, double(Ticks() - r0) * ms * 1000.0);
  out.Flush();
}

DWORD WINAPI ReporterMain(void *) {
  Tag("layer_time_reporter");
  for (;;) {
    WaitForSingleObject(G().wake, INFINITE);
    const DWORD error = GetLastError();
    try { Report(); } catch (...) {}
    SetLastError(error);
  }
  return 0;
}

void Wake() {
  const DWORD error = GetLastError();
  {
    std::lock_guard lock(G().mutex);
    if (!G().started) {
      G().started = true;
      G().wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      HANDLE t = G().wake ? CreateThread(nullptr, 0, ReporterMain, nullptr, 0, nullptr) : nullptr;
      if (t) CloseHandle(t);
    }
  }
  if (G().wake) SetEvent(G().wake);
  SetLastError(error);
}

} // namespace

// DESC_RACE totals once more at process exit (probes without a swapchain never reach a report window).
struct RaceAtExit {
  ~RaceAtExit() {
    const uint64_t ops = g_race_ops.load(std::memory_order_relaxed);
    if (!ops) return;
    char line[200];
    const int n = std::snprintf(line, sizeof(line), "dx12_desc_race window=exit ops=%llu write_write=%llu write_read=%llu\n",
                                (unsigned long long)ops, (unsigned long long)g_race_write_write.load(std::memory_order_relaxed),
                                (unsigned long long)g_race_write_read.load(std::memory_order_relaxed));
    DWORD written;
    if (n > 0 && size_t(n) < sizeof(line)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
  }
} g_race_at_exit;

Block *Register() {
  const DWORD error = GetLastError();
  Init();
  Block *b = new (std::nothrow) Block();
  if (b) {
    b->tid = GetCurrentThreadId();
    b->tag = t_tag;
    std::lock_guard lock(G().mutex);
    if (!g_first) g_first = Ticks();
    G().blocks.push_back(b);
  }
  t_block = b;
  SetLastError(error);
  return b;
}

void Tag(const char *tag) {
  t_tag = tag;
  if (Block *b = t_block) b->tag = tag;
}

void Leave(Block *x) {
  if (--x->depth) return;
  const uint64_t t = Ticks(), d = t - x->start;
  const unsigned id = x->cur < kMethods ? x->cur : kMethods - 1, cls = kTable[id].cls;
  Bump(x->ticks[id], d);
  Bump(x->calls[id], 1);
  uint8_t ev = EventOf(cls);
  if (ev == EvNone) {
    x->since_cls[cls] += d;
    ++x->since_calls;
    return;
  }
  const uint64_t presents = G().presents.load(std::memory_order_relaxed);
  if (ev == EvExecute) {
    if (presents != x->last_presents) ev = EvExecuteFirst;
    x->last_presents = presents;
  }
  uint64_t inlayer = 0;
  for (unsigned c = 0; c < ClsCount; ++c) inlayer += x->since_cls[c];
  if (x->last_event_end && x->start >= x->last_event_end) {
    const uint64_t interval = x->start - x->last_event_end;
    Counter *a = x->agg[ev][BucketOf(interval)];
    Bump(a[0], 1); Bump(a[1], interval); Bump(a[2], d); Bump(a[3], inlayer);
    for (unsigned c = 0; c < ClsCount; ++c)
      if (x->since_cls[c]) Bump(a[4 + c], x->since_cls[c]);
  }
  if (!x->ring) {
    const DWORD error = GetLastError();
    x->ring = new (std::nothrow) Ev[kRing]();
    SetLastError(error);
  }
  if (x->ring) {
    const uint64_t h = x->head.load(std::memory_order_relaxed);
    Ev &v = x->ring[h % kRing];
    v.t0 = x->start; v.t1 = t; v.presents = presents; v.inlayer = inlayer;
    v.id = id; v.aux = x->aux; v.calls = uint32_t(x->since_calls); v.kind = ev;
    for (unsigned c = 0; c < ClsCount; ++c) v.cls[c] = x->since_cls[c];
    x->head.store(h + 1, std::memory_order_release);
  }
  for (unsigned c = 0; c < ClsCount; ++c) x->since_cls[c] = 0;
  x->since_calls = 0;
  x->last_event_end = t;
  if (cls == Cls_present) {
    const uint64_t n = G().presents.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_period && n % g_period == 0) Wake();
  }
}

} // namespace dxmt::layer_time
