// SPDX-License-Identifier: LGPL-2.1-or-later
// XL-GAPS (23.10.2026): GPU idle gaps between Metal command buffers, per frame. Observation only: no GPU waits,
// encoders, handlers or queries are inserted; no hashing. Default off.
//
//   MACRUNNER_DX12_GPU_GAPS=P   every P presents one window report. Each committed command buffer carries what produced
//                               it (list encoded at Close / run of lists / Signal / Wait / Present / tile mapping), whether
//                               it is the first of an ExecuteCommandLists, how many Metal encoders it holds, and CPU times
//                               (QPC): the application call, the submit thread taking it, the commit, a ring-capacity
//                               stall, a CPU fence wait (async Wait). On retirement the existing waiting thread reads
//                               kernelStart/kernelEnd/GPUStart/GPUEnd (host time). Frames are bounded by the GPU end of the
//                               present command buffers. Per window: buffers, encoders, union of GPU busy time, span, gap
//                               histogram (<50 us, 50-500 us, >500 us) and, for every gap, what stands before it (kind of the
//                               buffer that ended the busy run, kind of the next one, ECL boundary) and why the next one
//                               was late: the application called later than the GPU went idle (app), our layer held it
//                               (layer_fence / layer_ring / layer_fifo / layer_encode), or it was committed in time and the
//                               GPU started it late (gpu). QPC is mapped to host time by the minimum (kernelStart - commit).
// Lines (stderr): dx12_gpu_gaps / dx12_gpu_gaps_cause.
#pragma once
#include "d3d12_c17_stats.hpp"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace dxmt::gpu_gaps {

inline unsigned Period() { static const unsigned v = c17::ReadKey("MACRUNNER_DX12_GPU_GAPS", 100000); return v; }
inline bool Enabled() { return Period() != 0; }

enum Kind : uint8_t { KindOther, KindList, KindRun, KindSignal, KindWait, KindPresent, KindMapping, KindCount };
inline const char *KindName(unsigned k) {
  static const char *const names[KindCount] = {"other", "list", "run", "signal", "wait", "present", "mapping"};
  return k < KindCount ? names[k] : "other";
}

struct Meta {
  uint8_t kind = KindOther, first = 0;
  uint32_t encoders = 0, encoders_at_start = 0;
  uint64_t app = 0, dequeue = 0, commit = 0;   // QPC: application call, submit thread took it, commit
  uint64_t ring = 0, fence = 0;                // QPC end of a ring-capacity stall / CPU fence wait of >= 10 us, else 0
};

// Per thread, set by the caller of the operation that will create the next command buffer.
inline thread_local uint64_t t_app = 0, t_dequeue = 0, t_fence = 0;
inline thread_local bool t_first = false;

inline uint64_t Qpc() {
  const DWORD error = GetLastError();
  LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
  SetLastError(error);
  return uint64_t(q.QuadPart);
}

struct Record {
  uint64_t queue = 0, seq = 0;
  Meta m;
  uint64_t ks = 0, ke = 0, gs = 0, ge = 0;   // host ns
  uint8_t present = 0, status = 0;
};

class Collector {
  std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<Record> records_;
  std::vector<uint64_t> ends_;          // GPU end of each completed present, in completion order
  std::vector<std::pair<std::vector<Record>, std::vector<uint64_t>>> pending_;
  bool started_ = false;
  uint64_t windows_ = 0, frequency_ = 10000000;
  std::map<uint64_t, uint32_t> queues_;

  static void Line(const char *format, ...) {
    char line[1536];
    va_list args; va_start(args, format);
    const int n = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    DWORD written;
    if (n > 0 && size_t(n) < sizeof(line)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
  }

  void Process(std::vector<Record> records, const std::vector<uint64_t> &b, uint64_t window) {
    const size_t frames = b.size() - 1;
    if (!frames) return;
    std::sort(records.begin(), records.end(), [](const Record &x, const Record &y) { return x.gs < y.gs; });
    // QPC (100 ns at 10 MHz) -> host ns: the smallest kernelStart - commit is the clock offset plus commit latency.
    const double qpc_ns = 1e9 / double(frequency_);
    int64_t offset = INT64_MAX;
    for (const auto &r : records)
      if (r.m.commit && r.ks) offset = std::min<int64_t>(offset, int64_t(r.ks) - int64_t(double(r.m.commit) * qpc_ns));
    std::vector<int64_t> spread;
    if (offset != INT64_MAX)
      for (const auto &r : records)
        if (r.m.commit && r.ks) spread.push_back(int64_t(r.ks) - int64_t(double(r.m.commit) * qpc_ns) - offset);
    std::sort(spread.begin(), spread.end());
    const auto host = [&](uint64_t qpc) -> int64_t { return qpc && offset != INT64_MAX ? int64_t(double(qpc) * qpc_ns) + offset : 0; };
    const auto frame_of = [&](uint64_t t) -> int64_t {
      const auto it = std::upper_bound(b.begin(), b.end(), t);
      return int64_t(it - b.begin()) - 1;   // -1 before the window, frames = after it
    };
    uint64_t cbs = 0, empty = 0, encoders = 0, sum_cb = 0, busy = 0;
    uint64_t gap_n[3] = {}, gap_ns[3] = {};
    std::map<std::tuple<unsigned, std::string, unsigned, unsigned, unsigned>, std::pair<uint64_t, uint64_t>> causes;
    std::map<unsigned, uint64_t> kinds;
    std::vector<uint64_t> first_start(frames, UINT64_MAX), last_end(frames, 0);
    const Record *prev = nullptr;
    uint64_t run_start = 0, run_end = 0;
    const auto close_run = [&]() {
      // union segment [run_start, run_end) split over the frames of the window
      for (uint64_t s = std::max(run_start, b.front()); s < std::min(run_end, b.back());) {
        const auto f = frame_of(s);
        const uint64_t e = std::min<uint64_t>(run_end, b[size_t(f) + 1]);
        busy += e - s; s = e;
      }
    };
    for (const auto &r : records) {
      if (r.gs < b.front() || r.gs >= b.back()) continue;
      const auto f = size_t(frame_of(r.gs));
      ++cbs; ++kinds[r.m.kind]; encoders += r.m.encoders;
      if (r.status != 4 || !r.gs || r.ge < r.gs) { ++empty; continue; }
      sum_cb += r.ge - r.gs;
      first_start[f] = std::min(first_start[f], r.gs); last_end[f] = std::max(last_end[f], r.ge);
      if (!prev) { run_start = r.gs; run_end = r.ge; prev = &r; continue; }
      if (r.gs > run_end) {
        const uint64_t a = run_end, gap = r.gs - run_end;
        const unsigned size = gap < 50000 ? 0 : gap < 500000 ? 1 : 2;
        // a gap that starts before the window (run ended there) is not this window's
        if (a >= b.front()) {
          ++gap_n[size]; gap_ns[size] += gap;
          const char *cause = "gpu";
          if (r.m.app && host(r.m.app) > int64_t(a)) cause = "app";
          else if (r.ks > a) {
            if (r.m.fence && host(r.m.fence) > int64_t(a)) cause = "layer_fence";
            else if (r.m.ring && host(r.m.ring) > int64_t(a)) cause = "layer_ring";
            else if (r.m.dequeue && host(r.m.dequeue) > int64_t(a)) cause = "layer_fifo";
            else cause = "layer_encode";
          }
          auto &slot = causes[{size, cause, unsigned(prev->m.kind), unsigned(r.m.kind), unsigned(r.m.first)}];
          ++slot.first; slot.second += gap;
        }
        close_run();
        run_start = r.gs; run_end = r.ge; prev = &r;
      } else if (r.ge > run_end) { run_end = r.ge; prev = &r; }
    }
    if (prev) close_run();
    uint64_t span = 0;
    for (size_t f = 0; f < frames; ++f) if (last_end[f] > first_start[f] && first_start[f] != UINT64_MAX) span += last_end[f] - first_start[f];
    const double F = double(frames), ms = 1e-6;
    const auto pct = [&](double p) { return spread.empty() ? 0.0 : double(spread[std::min(spread.size() - 1, size_t(p * double(spread.size())))]) / 1000.0; };
    Line("dx12_gpu_gaps window=%llu frames=%zu queues=%zu cbs=%llu empty_cbs=%llu encoders=%llu cbs_per_frame=%.2f "
         "encoders_per_frame=%.2f busy_ms_per_frame=%.3f span_ms_per_frame=%.3f sum_cb_ms_per_frame=%.3f frame_ms=%.3f "
         "lt50_per_frame=%.2f lt50_ms_per_frame=%.3f m50_500_per_frame=%.2f m50_500_ms_per_frame=%.3f gt500_per_frame=%.2f "
         "gt500_ms_per_frame=%.3f offset_ns=%lld commit_to_kstart_us_p50=%.1f p90=%.1f p99=%.1f first_end_ns=%llu last_end_ns=%llu "
         "first_end_qpc=%lld kinds=list:%llu,run:%llu,signal:%llu,wait:%llu,present:%llu,mapping:%llu,other:%llu\n",
         (unsigned long long)window, frames, queues_.size(), (unsigned long long)cbs, (unsigned long long)empty,
         (unsigned long long)encoders, double(cbs) / F, double(encoders) / F, double(busy) * ms / F, double(span) * ms / F,
         double(sum_cb) * ms / F, double(b.back() - b.front()) * ms / F,
         double(gap_n[0]) / F, double(gap_ns[0]) * ms / F, double(gap_n[1]) / F, double(gap_ns[1]) * ms / F,
         double(gap_n[2]) / F, double(gap_ns[2]) * ms / F, (long long)(offset == INT64_MAX ? 0 : offset),
         pct(0.5), pct(0.9), pct(0.99), (unsigned long long)b.front(), (unsigned long long)b.back(),
         (long long)(offset == INT64_MAX ? 0 : int64_t(double(int64_t(b.front()) - offset) / qpc_ns)),
         (unsigned long long)kinds[KindList], (unsigned long long)kinds[KindRun], (unsigned long long)kinds[KindSignal],
         (unsigned long long)kinds[KindWait], (unsigned long long)kinds[KindPresent], (unsigned long long)kinds[KindMapping],
         (unsigned long long)kinds[KindOther]);
    std::vector<std::pair<std::tuple<unsigned, std::string, unsigned, unsigned, unsigned>, std::pair<uint64_t, uint64_t>>> rows(causes.begin(), causes.end());
    std::sort(rows.begin(), rows.end(), [](const auto &x, const auto &y) { return x.second.second > y.second.second; });
    static const char *const sizes[3] = {"lt50", "50_500", "gt500"};
    unsigned printed = 0;
    for (const auto &[key, value] : rows) {
      if (++printed > 48) break;
      Line("dx12_gpu_gaps_cause window=%llu size=%s cause=%s before=%s next=%s next_first_of_execute=%u count=%llu "
           "per_frame=%.3f ms_per_frame=%.4f\n", (unsigned long long)window, sizes[std::get<0>(key)], std::get<1>(key).c_str(),
           KindName(std::get<2>(key)), KindName(std::get<3>(key)), std::get<4>(key), (unsigned long long)value.first,
           double(value.first) / F, double(value.second) * ms / F);
    }
  }

  void Work() {
    for (;;) {
      std::pair<std::vector<Record>, std::vector<uint64_t>> batch;
      uint64_t window;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return !pending_.empty(); });
        batch = std::move(pending_.front()); pending_.erase(pending_.begin());
        window = ++windows_;
      }
      const DWORD error = GetLastError();
      try { Process(std::move(batch.first), batch.second, window); } catch (...) {}
      SetLastError(error);
    }
  }

public:
  Collector() {
    LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
    if (f.QuadPart > 0) frequency_ = uint64_t(f.QuadPart);
    Line("dx12_gpu_gaps state=READY version=1 period=%u qpc_frequency=%llu\n", Period(), (unsigned long long)frequency_);
  }
  void Push(const Record &r) {
    std::lock_guard lock(mutex_);
    ++queues_[r.queue];
    if (records_.size() < 4u * 1024 * 1024) records_.push_back(r);
    if (!(r.present && r.status == 4 && r.ge)) return;
    ends_.push_back(r.ge);
    const size_t P = Period();
    // two presents of lag: work of other queues that started before the boundary has retired by then
    if (ends_.size() < P + 3) return;
    std::vector<uint64_t> b(ends_.begin(), ends_.begin() + P + 1);
    std::sort(b.begin(), b.end());
    std::vector<Record> batch, keep;
    for (auto &x : records_) (x.gs >= b.back() ? keep : batch).push_back(x);
    records_ = std::move(keep);
    ends_.erase(ends_.begin(), ends_.begin() + P);
    pending_.emplace_back(std::move(batch), std::move(b));
    if (!started_) {
      // A plain Win32 thread (the layer's other workers are too); it lives until the process exits.
      started_ = CreateThread(nullptr, 0, [](void *self) -> DWORD { static_cast<Collector *>(self)->Work(); return 0; },
                              this, 0, nullptr) != nullptr;
    }
    ready_.notify_one();
  }
};

inline Collector &Get() { static Collector *c = new Collector(); return *c; }

} // namespace dxmt::gpu_gaps
