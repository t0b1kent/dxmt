// MacRunner, 21.10.2026: opt-in asynchronous PSO construction and creation/use tracing.
#pragma once
#include "d3d12_device.hpp"
#include <functional>
#include <memory>

namespace dxmt::pso_work {
struct TraceRecord;
using Trace = std::shared_ptr<TraceRecord>;
enum class Phase { Translation, Backend };
class PhaseScope {
  Trace record_;
  Phase phase_;
  uint64_t start_ = 0;
public:
  explicit PhaseScope(Phase phase);
  ~PhaseScope();
};
template<typename F> auto Timed(Phase phase, F &&fn) {
  PhaseScope scope(phase);
  return fn();
}
class CreateScope {
  Trace record_, previous_;
public:
  explicit CreateScope(bool compute);
  ~CreateScope();
  HRESULT Finish(HRESULT hr, void **output);
  Trace Record() const { return record_; }
};
void Present();
MTLD3D12PipelineState *Resolve(MTLD3D12PipelineState *pso);
void Skipped(const char *operation);
bool AsyncEnabled();

// Destruction requests stop; workers own the state and may outlive the device after their final job.
// In particular the last PSO release on a worker must never join that worker.
class Compiler {
  struct State;
  std::shared_ptr<State> state_;
public:
  Compiler();
  ~Compiler();
  HRESULT Enqueue(std::function<void()> job);
};
HRESULT CreateAsync(MTLD3D12Device *device, Compiler &compiler,
                    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID iid, void **output, Trace trace);
HRESULT CreateAsync(MTLD3D12Device *device, Compiler &compiler,
                    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID iid, void **output, Trace trace);
} // namespace dxmt::pso_work

#define PSO_XLAT(...) dxmt::pso_work::Timed(dxmt::pso_work::Phase::Translation, [&] { return (__VA_ARGS__); })
#define PSO_BACKEND(...) dxmt::pso_work::Timed(dxmt::pso_work::Phase::Backend, [&] { return (__VA_ARGS__); })
