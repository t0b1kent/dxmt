#include "d3d12_gpu_timing.hpp"
#include <cassert>
#include <thread>
#include <set>
#include <iostream>

int main() {
  unsetenv("MACRUNNER_DX12_GPU_TIMING");
  unsetenv("MACRUNNER_DX12_PRESENT_HEARTBEAT");
  mock_error = 42;
  dxmt::GpuTimingTrace off;
  for (int i=0; i<100; ++i) { assert(!off.Begin().ticket); off.Heartbeat(true, 4); }
  assert(mock_error == 42 && mock_clock_calls == 0 && mock_lines.empty());

  setenv("MACRUNNER_DX12_GPU_TIMING", "1", 1);
  setenv("MACRUNNER_DX12_GPU_TIMING_LIMIT", "2", 1);
  setenv("MACRUNNER_DX12_GPU_TIMING_DELAY_MS", "10", 1);
  dxmt::GpuTimingTrace trace;
  assert(mock_error == 42 && mock_lines.size() == 1);
  assert(mock_lines[0].find("state=READY") != std::string::npos);
  assert(!trace.Begin().ticket);
  mock_clock += 10000;
  auto first = trace.Begin(), second = trace.Begin();
  assert(first.ticket == 1 && second.ticket == 2);
  first.commit_end = first.commit_begin+100;
  trace.Complete(first, &trace, 1, true, 4, 1010100, 1010300, 100000000, 102000000);
  trace.Complete(second, &trace, 2, false, 5, 1010400, 1010500, 0, 0);
  assert(mock_lines[1].find("state=PRESENT") != std::string::npos);
  assert(mock_lines[1].find("gpu_begin_ns=100000000 gpu_end_ns=102000000") != std::string::npos);
  assert(mock_lines[2].find("state=INVALID") != std::string::npos);
  for (int i=0; i<100; ++i) assert(!trace.Begin().ticket);
  assert(mock_lines.size() == 4 && mock_lines[3].find("state=DROPPED") != std::string::npos);
  assert(mock_error == 42);

  setenv("MACRUNNER_DX12_GPU_TIMING_LIMIT", "100", 1);
  setenv("MACRUNNER_DX12_GPU_TIMING_DELAY_MS", "0", 1);
  dxmt::GpuTimingTrace concurrent;
  std::mutex mutex;
  std::set<uint64_t> tickets;
  auto reserve = [&] { for (int i=0; i<120; ++i) {
    auto r = concurrent.Begin();
    if (r.ticket) { std::lock_guard<std::mutex> lock(mutex); assert(tickets.insert(r.ticket).second); }
  }};
  std::thread a(reserve), b(reserve); a.join(); b.join();
  assert(tickets.size() == 100 && *tickets.begin() == 1 && *tickets.rbegin() == 100);
  assert(mock_lines.size() == 6); // one READY and one DROPPED for this instance
  std::cout << "PASS disabled clock/log=0; delay; timestamps; invalid GPU; cap; two-queue unique tickets; LastError\n";

  unsetenv("MACRUNNER_DX12_GPU_TIMING");
  setenv("MACRUNNER_DX12_PRESENT_HEARTBEAT", "1", 1);
  mock_lines.clear();
  dxmt::GpuTimingTrace heartbeat;
  assert(mock_lines.size() == 1 && mock_lines.back().find("dx12_present state=READY") == 0);
  heartbeat.Heartbeat(false, 4);
  assert(mock_lines.back().find("completed=0 failed=0 last_completed_qpc=0") != std::string::npos);
  for (int i=0; i<100; ++i) heartbeat.Heartbeat(true, 4);
  assert(mock_lines.size() == 2);
  mock_clock += 1000000;
  heartbeat.Heartbeat(true, 5);
  assert(mock_lines.size() == 3 && mock_lines.back().find("completed=100 failed=1") != std::string::npos);
  mock_clock += 1000000;
  heartbeat.Heartbeat(false, 4);
  assert(mock_lines.size() == 4 && mock_lines.back().find("completed=100 failed=1") != std::string::npos);
  assert(mock_error == 42);

  // The GPU delay and exhausted record budget cannot suppress progress.
  setenv("MACRUNNER_DX12_GPU_TIMING", "1", 1);
  setenv("MACRUNNER_DX12_GPU_TIMING_DELAY_MS", "3600000", 1);
  setenv("MACRUNNER_DX12_GPU_TIMING_LIMIT", "1", 1);
  mock_lines.clear();
  dxmt::GpuTimingTrace independent;
  assert(!independent.Begin().ticket);
  independent.Heartbeat(true, 4);
  assert(mock_lines.back().find("dx12_present state=PROGRESS completed=1") == 0);
  mock_clock += 3600000000LL;
  assert(independent.Begin().ticket == 1);
  assert(!independent.Begin().ticket);
  independent.Heartbeat(true, 4);
  assert(mock_lines.back().find("dx12_present state=PROGRESS completed=2") == 0);

  unsetenv("MACRUNNER_DX12_GPU_TIMING");
  mock_lines.clear();
  dxmt::GpuTimingTrace producers;
  auto complete = [&] {
    mock_error = 42;
    for (int i=0; i<10000; ++i) producers.Heartbeat(true, 4);
    assert(mock_error == 42);
  };
  std::thread c(complete), d(complete); c.join(); d.join();
  assert(mock_lines.size() == 2); // READY plus one reserved 1-second interval
  mock_clock += 1000000;
  producers.Heartbeat(false, 4);
  assert(mock_lines.size() == 3);
  assert(mock_lines.back().find("completed=20000 failed=0") != std::string::npos);
  mock_clock = 0;
  producers.Heartbeat(true, 4);
  producers.Heartbeat(true, 4);
  assert(mock_lines.size() == 4 && mock_lines.back() == "dx12_present state=FAILED reason=clock_read\n");
  assert(mock_error == 42);
  std::cout << "PASS heartbeat off; independent delay/cap; only successful Presents advance; non-present/failed stable; 1Hz; 2x10000 completions; explicit clock failure; LastError\n";
}
