// A bounded qualification fixture, not a frame-time benchmark.
// Real swapchain Present, then GPU clears without Present, then Present again.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(HRESULT hr, const char *what) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "fixture FAILED %s hr=%08lx\n", what, (unsigned long)hr);
    std::exit(2);
  }
}
#define CK(x) check((x), #x)
static DWORD WINAPI signal_control(void *p) {
  Sleep(25);
  return FAILED(static_cast<ID3D12Fence *>(p)->Signal(1)) ? 1 : 0;
}
static void mark(const char *phase, unsigned presents, unsigned submissions) {
  LARGE_INTEGER qpc, frequency;
  QueryPerformanceCounter(&qpc); QueryPerformanceFrequency(&frequency);
  std::fprintf(stderr, "fixture phase=%s presents=%u submissions=%u qpc=%lld frequency=%lld\n",
               phase, presents, submissions, qpc.QuadPart, frequency.QuadPart);
  std::fflush(stderr);
}
int main() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"D3D12HeartbeatQualification";
  if (!RegisterClassW(&wc)) return 3;
  HWND window = CreateWindowW(wc.lpszClassName, L"MacRunner D3D12 Present qualification",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 360, 240,
      nullptr, nullptr, wc.hInstance, nullptr);
  if (!window) return 4;
  ID3D12Device *device = nullptr;
  CK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  wchar_t loaded[MAX_PATH]{};
  if (!GetModuleFileNameW(GetModuleHandleW(L"d3d12.dll"), loaded, MAX_PATH)) return 5;
  std::fprintf(stderr, "fixture loaded_d3d12=%ls\n", loaded);
  ID3D12CommandQueue *queue = nullptr;
  D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  CK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  IDXGIFactory4 *factory = nullptr;
  CK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = 320; sd.Height = 180; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  IDXGISwapChain1 *swap1 = nullptr; IDXGISwapChain3 *swap = nullptr;
  CK(factory->CreateSwapChainForHwnd(queue, window, &sd, nullptr, nullptr, &swap1));
  CK(swap1->QueryInterface(IID_PPV_ARGS(&swap))); swap1->Release();
  UINT latency = 0;
  CK(swap->GetMaximumFrameLatency(&latency));
  const char *depth = std::getenv("MACRUNNER_DX12_LATENCY_DEPTH");
  const UINT expected = depth && !std::strcmp(depth, "2") ? 2 : depth && !std::strcmp(depth, "3") ? 3 : 1;
  if (latency != expected) return 8;
  if (swap->SetMaximumFrameLatency(0) != E_INVALIDARG) return 9;
  CK(swap->SetMaximumFrameLatency(1));
  CK(swap->GetMaximumFrameLatency(&latency));
  if (latency != expected) return 10;
  HANDLE latency_event = swap->GetFrameLatencyWaitableObject();
  if (!latency_event) return 11;
  unsigned prefetched = 0;
  for (; prefetched < expected; ++prefetched)
    if (WaitForSingleObject(latency_event, 0) != WAIT_OBJECT_0) return 12;
  if (WaitForSingleObject(latency_event, 0) != WAIT_TIMEOUT) return 13;
  std::fprintf(stderr, "fixture latency=%u initial_credits=%u\n", latency, prefetched);
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
  ID3D12DescriptorHeap *heap = nullptr;
  CK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
  UINT stride = device->GetDescriptorHandleIncrementSize(hd.Type);
  ID3D12Resource *buffers[2]{};
  for (unsigned i = 0; i != 2; ++i) {
    CK(swap->GetBuffer(i, IID_PPV_ARGS(&buffers[i])));
    auto handle = heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += i * stride;
    device->CreateRenderTargetView(buffers[i], nullptr, handle);
  }
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  CK(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&allocator)));
  CK(device->CreateCommandList(0, qd.Type, allocator, nullptr, IID_PPV_ARGS(&list)));
  CK(list->Close());
  ID3D12Fence *fence = nullptr;
  CK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) return 6;
  CK(fence->SetEventOnCompletion(0, event));
  if (WaitForSingleObject(event, 0) != WAIT_OBJECT_0) return 15;
  ID3D12Fence *control = nullptr;
  CK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&control)));
  HANDLE control_thread = CreateThread(nullptr, 0, signal_control, control, 0, nullptr);
  if (!control_thread) return 16;
  CK(control->SetEventOnCompletion(1, nullptr));
  if (WaitForSingleObject(control_thread, 5000) != WAIT_OBJECT_0) return 17;
  DWORD control_exit = 1;
  if (!GetExitCodeThread(control_thread, &control_exit) || control_exit) return 18;
  CK(control->Signal(0)); // Rewind is identified by the changed native event.
  CloseHandle(control_thread); control->Release();
  unsigned presents = 0, submissions = 0;
  mark("start", presents, submissions);
  for (unsigned phase = 0; phase != 3; ++phase) {
    bool presenting = phase != 1;
    mark(phase == 0 ? "present_begin" : phase == 1 ? "nonpresent_begin" : "resume_begin", presents, submissions);
    for (unsigned frame = 0; frame != 120; ++frame) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
      }
      if (presenting) {
        if (prefetched) --prefetched;
        else if (WaitForSingleObject(latency_event, 5000) != WAIT_OBJECT_0) return 14;
      }
      CK(allocator->Reset()); CK(list->Reset(allocator, nullptr));
      UINT index = swap->GetCurrentBackBufferIndex();
      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = buffers[index];
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      list->ResourceBarrier(1, &barrier);
      auto handle = heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += index * stride;
      const float color[4] = {presenting ? 0.0f : 0.5f, phase == 2 ? 0.5f : 1.0f, 0.1f, 1.0f};
      list->ClearRenderTargetView(handle, color, 0, nullptr);
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
      list->ResourceBarrier(1, &barrier); CK(list->Close());
      ID3D12CommandList *lists[] = {list}; queue->ExecuteCommandLists(1, lists);
      ++submissions;
      if (presenting) { CK(swap->Present(0, 0)); ++presents; }
      CK(queue->Signal(fence, submissions));
      CK(fence->SetEventOnCompletion(submissions, event));
      if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) return 7;
      if (fence->GetCompletedValue() < submissions) return 19;
      Sleep(25);
    }
    mark(phase == 0 ? "present_end" : phase == 1 ? "nonpresent_end" : "resume_end", presents, submissions);
  }
  // A final ordinary GPU CB must not manufacture a new completed Present.
  CK(queue->Wait(fence, submissions));
  Sleep(1100);
  mark("done", presents, submissions);
  CloseHandle(latency_event);
  CloseHandle(event); fence->Release(); list->Release(); allocator->Release();
  for (auto *buffer : buffers) buffer->Release();
  heap->Release(); swap->Release(); factory->Release(); queue->Release(); device->Release();
  DestroyWindow(window);
  std::fprintf(stderr, "fixture PASS presents=240 submissions=360\n");
  return 0;
}
