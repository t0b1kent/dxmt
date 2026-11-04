#include "d3d12_pso_work.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_pso_cache.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

namespace dxmt::pso_work {
static bool Enabled(const char *name) { return env::getEnvVar(name) == "1"; }
static bool TraceEnabled() { static const bool enabled = Enabled("MACRUNNER_DX12_PSO_CREATE_TRACE"); return enabled; }
static uint64_t Clock() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static uint64_t Frequency() { LARGE_INTEGER t; QueryPerformanceFrequency(&t); return t.QuadPart; }
static std::mutex output_mutex, present_mutex;
static uint64_t present_time = 0;
static DWORD present_tid = 0;
static std::atomic<uint64_t> sequence{0}, submitted{0}, ready{0}, failed{0}, skipped{0}, waited{0};
static thread_local bool building = false;
static thread_local Trace active_trace;
static thread_local unsigned phase_depth[2] = {};

static void Line(const char *line) {
  std::lock_guard lock(output_mutex);
  DWORD written;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(strlen(line)), &written, nullptr);
}

struct TraceRecord {
  uint64_t id = ++sequence, start = Clock(), api_end = 0, ready_at = 0, last_present = 0;
  DWORD tid = GetCurrentThreadId(), last_present_tid = 0;
  bool compute = false, asynchronous = false;
  HRESULT api_hr = E_PENDING, backend_hr = E_PENDING;
  std::atomic<uint64_t> xlat{0}, backend{0}, first_use{0}, use_end{0};
  std::atomic<bool> emitted{false};
  ~TraceRecord() { Emit(); }
  void Emit() {
    if (emitted.exchange(true)) return;
    char line[1000];
    const auto use = first_use.load();
    snprintf(line, sizeof(line), "dx12_pso_create id=%llu kind=%s async=%u tid=%lu t0=%llu t1=%llu ready=%llu "
        "qpc_hz=%llu dt_xlat=%llu dt_be=%llu present_qpc=%llu present_tid=%lu since_present=%llu "
        "first_use=%llu create_to_use=%llu first_use_wait=%llu hr=%08x backend_hr=%08x\n",
        (unsigned long long)id, compute ? "compute" : "graphics", unsigned(asynchronous), (unsigned long)tid,
        (unsigned long long)start, (unsigned long long)api_end, (unsigned long long)ready_at,
        (unsigned long long)Frequency(), (unsigned long long)xlat.load(), (unsigned long long)backend.load(),
        (unsigned long long)last_present, (unsigned long)last_present_tid,
        (unsigned long long)(last_present ? start - last_present : 0), (unsigned long long)use,
        (unsigned long long)(use ? use - start : 0), (unsigned long long)(use ? use_end.load() - use : 0),
        unsigned(api_hr), unsigned(backend_hr));
    Line(line);
  }
};

PhaseScope::PhaseScope(Phase phase) : phase_(phase) {
  if (active_trace && phase_depth[unsigned(phase_)]++ == 0) { record_ = active_trace; start_ = Clock(); }
}
PhaseScope::~PhaseScope() {
  if (active_trace) --phase_depth[unsigned(phase_)];
  if (record_) (phase_ == Phase::Translation ? record_->xlat : record_->backend).fetch_add(Clock() - start_);
}
CreateScope::CreateScope(bool compute) {
  if (TraceEnabled() && !building && !pso_cache::PipelineCache::Replaying()) {
    record_ = std::make_shared<TraceRecord>(); record_->compute = compute;
    {
      std::lock_guard lock(present_mutex);
      record_->start = Clock(); // The snapshot cannot accidentally refer to a Present newer than this Create.
      record_->last_present = present_time; record_->last_present_tid = present_tid;
    }
    previous_ = std::move(active_trace); active_trace = record_;
  }
}
CreateScope::~CreateScope() { if (record_) active_trace = std::move(previous_); }
HRESULT CreateScope::Finish(HRESULT hr, void **output) {
  if (!record_) return hr;
  record_->api_end = Clock(); record_->api_hr = hr;
  if (!record_->asynchronous) { record_->ready_at = record_->api_end; record_->backend_hr = hr; }
  if (SUCCEEDED(hr) && output && *output)
    static_cast<MTLD3D12PipelineState *>(*output)->creation_trace = record_;
  else record_->Emit();
  return hr;
}
void Present() {
  if (!TraceEnabled()) return;
  char line[180];
  std::lock_guard lock(present_mutex);
  present_time = Clock(); present_tid = GetCurrentThreadId();
  snprintf(line, sizeof(line), "dx12_pso_present qpc=%llu tid=%lu qpc_hz=%llu\n",
      (unsigned long long)present_time, (unsigned long)present_tid, (unsigned long long)Frequency());
  Line(line);
}
MTLD3D12PipelineState *Resolve(MTLD3D12PipelineState *pso) {
  if (!pso) return nullptr;
  auto trace = pso->creation_trace;
  uint64_t expected = 0;
  const bool first = trace && trace->first_use.compare_exchange_strong(expected, Clock());
  auto result = pso->ResolveForUse();
  if (first) { trace->use_end = Clock(); trace->Emit(); }
  return result;
}
static void Stats(const char *event, HRESULT hr = S_OK) {
  char line[360];
  snprintf(line, sizeof(line), "dx12_pso_async event=%s submitted=%llu ready=%llu failed=%llu waited=%llu skipped=%llu hr=%08x\n",
      event, (unsigned long long)submitted.load(), (unsigned long long)ready.load(), (unsigned long long)failed.load(),
      (unsigned long long)waited.load(), (unsigned long long)skipped.load(), unsigned(hr));
  Line(line);
}
void Skipped(const char *operation) { const auto n = ++skipped; if (n <= 16 || (n & (n - 1)) == 0) Stats(operation); }
bool AsyncEnabled() {
  static const bool enabled = Enabled("MACRUNNER_DX12_PSO_ASYNC_CREATE");
  return enabled && !building && !pso_cache::PipelineCache::Replaying();
}

struct Compiler::State {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool stop = false;
  unsigned workers = 0;
  struct Start { std::shared_ptr<State> state; HMODULE module; };
  static DWORD WINAPI Main(void *arg) {
    auto start = std::unique_ptr<Start>(static_cast<Start *>(arg));
    auto state = std::move(start->state); HMODULE module = start->module; start.reset();
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] { return state->stop || !state->jobs.empty(); });
        if (state->jobs.empty()) break;
        job = std::move(state->jobs.front()); state->jobs.pop_front();
      }
      job();
      // Releasing the job may destruct the device and Compiler. Only shared State is accessed after this point.
      job = {};
    }
    state.reset();
    FreeLibraryAndExitThread(module, 0);
    return 0;
  }
};
Compiler::Compiler() : state_(std::make_shared<State>()) {
  auto text = env::getEnvVar("MACRUNNER_DX12_PSO_ASYNC_THREADS");
  const unsigned count = text.empty() ? 2 : std::clamp(std::atoi(text.c_str()), 1, 4);
  for (unsigned i = 0; i < count; ++i) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&State::Main), &module)) break;
    auto start = std::make_unique<State::Start>(State::Start{state_, module});
    HANDLE thread = CreateThread(nullptr, 0, &State::Main, start.get(), CREATE_SUSPENDED, nullptr);
    if (!thread) { FreeLibrary(module); break; }
    start.release(); ++state_->workers;
    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL); ResumeThread(thread); CloseHandle(thread);
  }
}
Compiler::~Compiler() {
  { std::lock_guard lock(state_->mutex); state_->stop = true; }
  state_->cv.notify_all(); Stats("compiler_stop");
}
HRESULT Compiler::Enqueue(std::function<void()> job) {
  { std::lock_guard lock(state_->mutex);
    if (!state_->workers || state_->stop) return E_OUTOFMEMORY;
    state_->jobs.push_back(std::move(job)); ++submitted;
  }
  state_->cv.notify_one(); return S_OK;
}

class Pending final : public MTLD3D12Pageable<MTLD3D12PipelineState> {
  std::mutex mutex_;
  std::condition_variable cv_;
  bool ready_ = false;
  HRESULT result_ = E_PENDING;
  Com<MTLD3D12PipelineState> resolved_;
public:
  Pending(MTLD3D12Device *device, bool compute) : MTLD3D12Pageable(device) { IsComputePipelineState = compute; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **output) override {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Object) && iid != __uuidof(ID3D12DeviceChild) &&
        iid != __uuidof(ID3D12Pageable) && iid != __uuidof(ID3D12PipelineState)) return E_NOINTERFACE;
    *output = ref(this); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob **blob) override {
    if (!blob) return E_POINTER;
    *blob = nullptr;
    // No D3D12-compatible cached blob is implemented by this backend. Do not wait for work we cannot expose.
    return E_NOTIMPL;
  }
  MTLD3D12PipelineState *ResolveForUse() override {
    std::unique_lock lock(mutex_);
    if (!ready_) { ++waited; cv_.wait(lock, [&] { return ready_; }); }
    return SUCCEEDED(result_) ? resolved_.ptr() : nullptr;
  }
  void Finish(HRESULT hr, ID3D12PipelineState *pso, const Trace &trace) {
    {
      std::lock_guard lock(mutex_);
      resolved_ = static_cast<MTLD3D12PipelineState *>(pso); result_ = hr;
      if (trace) { trace->ready_at = Clock(); trace->backend_hr = hr; }
      ready_ = true; ++ready;
      if (FAILED(hr)) { ++failed; Stats("backend_failure", hr); }
    }
    cv_.notify_all();
  }
};

struct Description {
  bool compute;
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs{};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC gs{};
  Com<ID3D12RootSignature> root;
  std::vector<uint8_t> code[5], blob;
  std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
  std::vector<D3D12_SO_DECLARATION_ENTRY> streams;
  std::vector<std::string> names;
  std::vector<UINT> strides;
  static bool Bytes(const void *source, size_t size, std::vector<uint8_t> &target) {
    if (size && !source) return false;
    if (size) target.assign(static_cast<const uint8_t *>(source), static_cast<const uint8_t *>(source) + size);
    return true;
  }
  bool Shader(D3D12_SHADER_BYTECODE &shader, unsigned i) {
    if (!Bytes(shader.pShaderBytecode, shader.BytecodeLength, code[i])) return false;
    shader.pShaderBytecode = code[i].empty() ? nullptr : code[i].data(); return true;
  }
  bool Cached(D3D12_CACHED_PIPELINE_STATE &cached) {
    if (!Bytes(cached.pCachedBlob, cached.CachedBlobSizeInBytes, blob)) return false;
    cached.pCachedBlob = blob.empty() ? nullptr : blob.data(); return true;
  }
  bool Copy(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc) {
    compute = true; cs = desc; root = cs.pRootSignature;
    return Shader(cs.CS, 0) && Cached(cs.CachedPSO);
  }
  bool Copy(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
    compute = false; gs = desc; root = gs.pRootSignature;
    if (!Shader(gs.VS, 0) || !Shader(gs.PS, 1) || !Shader(gs.DS, 2) || !Shader(gs.HS, 3) || !Shader(gs.GS, 4) ||
        !Cached(gs.CachedPSO) || gs.InputLayout.NumElements > 32 ||
        gs.StreamOutput.NumStrides > 4 || (gs.InputLayout.NumElements && !gs.InputLayout.pInputElementDescs) ||
        (gs.StreamOutput.NumEntries && !gs.StreamOutput.pSODeclaration) ||
        (gs.StreamOutput.NumStrides && !gs.StreamOutput.pBufferStrides)) return false;
    if (gs.InputLayout.NumElements) inputs.assign(gs.InputLayout.pInputElementDescs, gs.InputLayout.pInputElementDescs + gs.InputLayout.NumElements);
    if (gs.StreamOutput.NumEntries) streams.assign(gs.StreamOutput.pSODeclaration, gs.StreamOutput.pSODeclaration + gs.StreamOutput.NumEntries);
    if (gs.StreamOutput.NumStrides) strides.assign(gs.StreamOutput.pBufferStrides, gs.StreamOutput.pBufferStrides + gs.StreamOutput.NumStrides);
    names.reserve(inputs.size() + streams.size());
    for (auto &input : inputs) {
      if (!input.SemanticName) return false;
      names.emplace_back(input.SemanticName); input.SemanticName = names.back().c_str();
    }
    for (auto &stream : streams) if (stream.SemanticName) {
      names.emplace_back(stream.SemanticName); stream.SemanticName = names.back().c_str();
    }
    gs.InputLayout.pInputElementDescs = inputs.data(); gs.StreamOutput.pSODeclaration = streams.data();
    gs.StreamOutput.pBufferStrides = strides.data(); return true;
  }
};

template<typename Desc> static HRESULT Create(MTLD3D12Device *device, Compiler &compiler,
    const Desc *desc, REFIID iid, void **output, Trace trace) {
  if (!output) return E_POINTER;
  *output = nullptr;
  if (!desc) return E_INVALIDARG;
  try {
    auto owned = std::make_shared<Description>();
    if (!owned->Copy(*desc)) return E_INVALIDARG;
    auto pending = Com(new Pending(device, owned->compute));
    void *validated = nullptr;
    HRESULT hr = pending->QueryInterface(iid, &validated);
    if (FAILED(hr)) return hr;
    // Hold the successful QI until the job is queued; a failure must still return a null output.
    Com<Pending> returned;
    *&returned = static_cast<Pending *>(validated);
    Com<Pending, false> worker_ref(pending.ptr());
    if (trace) trace->asynchronous = true;
    hr = compiler.Enqueue([device, owned, pending = std::move(worker_ref), trace] {
      const bool previous_building = building; building = true;
      auto previous_trace = std::move(active_trace); active_trace = trace;
      Com<ID3D12PipelineState> actual;
      HRESULT result = E_FAIL;
      try {
        result = owned->compute
            ? device->CreateComputePipelineState(&owned->cs, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&actual))
            : device->CreateGraphicsPipelineState(&owned->gs, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&actual));
      } catch (const std::bad_alloc &) { result = E_OUTOFMEMORY; }
        catch (...) { result = E_FAIL; }
      pending->Finish(result, actual.ptr(), trace);
      active_trace = std::move(previous_trace); building = previous_building;
    });
    if (FAILED(hr)) { if (trace) trace->asynchronous = false; return hr; }
    *output = returned.ptr(); *&returned = nullptr; return S_OK;
  } catch (const std::bad_alloc &) {
    if (trace) trace->asynchronous = false;
    return E_OUTOFMEMORY;
  }
}
HRESULT CreateAsync(MTLD3D12Device *device, Compiler &compiler, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                    REFIID iid, void **output, Trace trace) { return Create(device, compiler, desc, iid, output, trace); }
HRESULT CreateAsync(MTLD3D12Device *device, Compiler &compiler, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                    REFIID iid, void **output, Trace trace) { return Create(device, compiler, desc, iid, output, trace); }
} // namespace dxmt::pso_work
