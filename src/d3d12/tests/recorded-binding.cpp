// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_recorded_binding.hpp"
#include "../d3d12_raytracing_bindings.hpp"
#include <array>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
using namespace dxmt;
static uint64_t checks = 0;
static void Check(bool value) { ++checks; if (!value) { std::cerr << "FAIL " << checks << '\n'; std::abort(); } }
struct Range {
  uint32_t RangeType, NumDescriptors, BaseShaderRegister, RegisterSpace, OffsetInDescriptorsFromTableStart, Flags;
};
struct Parameter {
  uint32_t ParameterType = 0, ShaderVisibility = 0;
  struct { uint32_t NumDescriptorRanges; const Range *pDescriptorRanges; } DescriptorTable{};
  struct { uint32_t Num32BitValues, ShaderRegister, RegisterSpace; } Constants{};
  struct { uint32_t ShaderRegister, RegisterSpace, Flags; } Descriptor{};
};
struct Sampler {
  uint32_t ShaderVisibility, RegisterSpace, ShaderRegister, Filter, AddressU, AddressV, AddressW;
  float MipLODBias;
  uint32_t MaxAnisotropy, ComparisonFunc, BorderColor;
  float MinLOD, MaxLOD;
};
struct Desc {
  uint32_t Flags = 0, NumParameters = 0;
  const Parameter *pParameters = nullptr;
  uint32_t NumStaticSamplers = 0;
  const Sampler *pStaticSamplers = nullptr;
};
static void Policies() {
  using namespace ray_binding;
  for (uint32_t type = 0; type < 4; ++type) for (uint32_t table = 0; table < 2; ++table)
    for (uint32_t bits = 0; bits < 64; ++bits) {
      const uint32_t flags = (bits & 15) | ((bits & 16) ? 0x10000 : 0) | ((bits & 32) ? 0x20000 : 0);
      DescriptorPolicy p;
      Status expected = Status::Ready;
      const unsigned data_count = unsigned(bool(flags & 2)) + unsigned(bool(flags & 4)) + unsigned(bool(flags & 8));
      if (type == 3 && !table) expected = Status::Invalid;
      else if (flags & 0x20000) expected = Status::Unsupported;
      else if ((!table && (flags & 0x10001)) || data_count > 1 || (type == 3 && (flags & 14)))
        expected = Status::Invalid;
      else if (type == 3 && (flags & 0x10000)) expected = Status::Unsupported;
      else if ((flags & 1) && (flags & 0x10008)) expected = Status::Invalid;
      Check(DecodePolicy(type, flags, table, p) == expected);
      if (expected != Status::Ready) continue;
      Check(p.descriptors_volatile == bool(flags & 1));
      Check(p.keep_buffer_bounds == bool(flags & 0x10000));
      const auto data = type == 3 ? DataPolicy::None : (flags & 2) ? DataPolicy::Volatile :
          (flags & 4) ? DataPolicy::WhileExecute : (flags & 8) ? DataPolicy::Static :
          type == 1 ? DataPolicy::Volatile : DataPolicy::WhileExecute;
      Check(p.data == data);
    }

  for (uint32_t type = 0; type < 4; ++type) {
    Range ranges[2]{{type, 4, 2, 0, 5, type == 3 ? 1u : 3u}, {type, 1, 9, 0, 12, 0}};
    Parameter p; p.DescriptorTable = {2, ranges}; Desc d; d.NumParameters = 1; d.pParameters = &p;
    RootLayout root;
    Check(Build(d, root) == Status::Ready && root.spans.size() == 2);
    Check(root.spans[0].flags == ranges[0].Flags && root.spans[1].flags == 0);
    Binding b; b.source = Source::Table; b.parameter = 0; b.range = 0; b.flags = ranges[0].Flags; b.descriptor_offset = 6;
    const Span *selected = nullptr;
    Check(SelectTable(root, b, selected) == Status::Ready && selected->type == type);
    b.descriptor_offset = 9; Check(SelectTable(root, b, selected) == Status::Invalid && !selected);
    b.descriptor_offset = 12; Check(SelectTable(root, b, selected) == Status::Invalid && !selected);
    b.range = 1; b.flags = 0; Check(SelectTable(root, b, selected) == Status::Ready);
    b.flags = 1; Check(SelectTable(root, b, selected) == Status::Invalid && !selected);
    b.flags = 0; b.local = true; Check(SelectTable(root, b, selected) == Status::Invalid && !selected);
    ranges[0].Flags = 6; Check(Build(d, root) == Status::Invalid && root.spans.empty());
    ranges[0].Flags = 0x80000000; Check(Build(d, root) == Status::Unsupported && root.spans.empty());
  }
  for (uint32_t kind : {2u, 3u, 4u}) {
    Parameter p; p.ParameterType = kind; p.Descriptor.Flags = 8;
    Desc d; d.NumParameters = 1; d.pParameters = &p; RootLayout root;
    Check(Build(d, root) == Status::Ready && root.spans[0].flags == 8);
    p.Descriptor.Flags = 1; Check(Build(d, root) == Status::Invalid && root.spans.empty());
    p.Descriptor.Flags = 12; Check(Build(d, root) == Status::Invalid && root.spans.empty());
  }
  // Same-type physical aliases are distinguished by the original range index.
  Range ranges[2]{{0, 1, 0, 0, 0, 0}, {0, 1, 5, 0, 0, 3}};
  Parameter p; p.DescriptorTable = {2, ranges}; Desc d; d.NumParameters = 1; d.pParameters = &p;
  RootLayout root; Check(Build(d, root) == Status::Ready);
  for (uint32_t i = 0; i < 2; ++i) {
    Binding b; b.parameter = 0; b.range = i; b.flags = ranges[i].Flags;
    const Span *s = nullptr;
    Check(SelectTable(root, b, s) == Status::Ready && s->first == ranges[i].BaseShaderRegister);
  }
}
struct Resource { uint64_t data; };
struct Captured { std::shared_ptr<Resource> owner; uint64_t word = 0; };
struct Heap {
  std::array<Captured, 8> slots{};
  std::array<capture::Status, 8> status{};
  unsigned reads = 0;
};
struct Provider {
  std::shared_ptr<Heap> heap;
  unsigned index = 0;
  bool Valid() const { return heap && index < heap->slots.size(); }
  capture::Status Acquire(Captured &out) const {
    out = {};
    if (!Valid()) return capture::Status::Invalid;
    ++heap->reads;
    if (heap->status[index] != capture::Status::Ready) return heap->status[index];
    if (!heap->slots[index].owner) return capture::Status::Uninitialized;
    out = heap->slots[index]; return capture::Status::Ready;
  }
};
using Recorded = capture::RecordedDescriptor<Provider, Captured>;
static void Replays() {
  using capture::Status;
  std::mt19937_64 rng(0x470095);
  for (unsigned iteration = 0; iteration < 50000; ++iteration) {
    auto heap = std::make_shared<Heap>(), other = std::make_shared<Heap>();
    const unsigned index = unsigned(rng() % 8);
    auto first = std::make_shared<Resource>(Resource{rng()}), second = std::make_shared<Resource>(Resource{rng()});
    const uint64_t old_word = rng(), new_word = rng();
    heap->slots[index] = {first, old_word};
    Provider caller{heap, index};
    Recorded frozen, dynamic;
    Check(frozen.Record(caller, false) == Status::Ready);
    Check(dynamic.Record(caller, true) == Status::Ready && heap->reads == 1);
    caller = {other, unsigned((index + 1) % 8)};
    heap->slots[index] = {second, new_word};
    Captured a, b;
    Check(frozen.Resolve(a) == Status::Ready && a.owner == first && a.word == old_word);
    Check(dynamic.Resolve(b) == Status::Ready && b.owner == second && b.word == new_word);
    // Data flags are promises, not permission to clone or freeze resource bytes.
    first->data = 12345; Check(a.owner->data == 12345);
    for (const auto status : {Status::Invalid, Status::Unsupported, Status::Retired, Status::OutOfMemory}) {
      heap->status[index] = status;
      Check(dynamic.Resolve(b) == status && !b.owner && b.word == 0);
      Check(frozen.Resolve(a) == Status::Ready && a.word == old_word);
    }
    heap->status[index] = Status::Ready; heap->slots[index] = {first, old_word};
    Check(dynamic.Resolve(b) == Status::Ready && b.owner == first && b.word == old_word);
    std::weak_ptr<Resource> weak = first;
    heap.reset(); other.reset(); first.reset(); second.reset(); caller = {}; frozen = {}; dynamic = {};
    Check(!weak.expired()); a = {}; b = {}; Check(weak.expired());
  }
  Recorded r; Captured out{std::make_shared<Resource>(), 99};
  Check(r.Resolve(out) == Status::Uninitialized && !out.owner && out.word == 0);
  Check(r.Record({}, true) == Status::Invalid);
  auto heap = std::make_shared<Heap>();
  Check(r.Record({heap, 0}, false) == Status::Uninitialized);
  Check(r.Record({heap, 0}, true) == Status::Ready);
  heap->slots[0] = {std::make_shared<Resource>(), 71};
  Check(r.Resolve(out) == Status::Ready && out.word == 71);
  auto copy = r;
  Check(r.Record({heap, 8}, true) == Status::Invalid);
  Check(r.Resolve(out) == Status::Uninitialized && !out.owner);
  Check(copy.Resolve(out) == Status::Ready && out.word == 71);
}

struct NativeCounts {
  std::atomic<unsigned> resident{0}, natives_destroyed{0}, public_destroyed{0};
};
struct NativeResidency {
  std::shared_ptr<NativeCounts> counts;
  void Retain(unsigned) { ++counts->resident; }
  void Release(unsigned) { --counts->resident; }
};
struct NativeOwner {
  std::shared_ptr<NativeCounts> counts;
  capture::ResidencyLease<NativeResidency, unsigned> residency;
  NativeOwner(std::shared_ptr<NativeResidency> owner) : counts(owner->counts), residency(std::move(owner), 1) {}
  ~NativeOwner() { ++counts->natives_destroyed; }
};
struct PublicOwner {
  std::shared_ptr<NativeOwner> native;
  std::thread::id recording_thread = std::this_thread::get_id();
  ~PublicOwner() {
    Check(std::this_thread::get_id() == recording_thread);
    ++native->counts->public_destroyed;
  }
};
template <typename Owner>
struct Packet {
  unsigned type = 0;
  std::array<uint64_t, 4> words{};
  uint32_t shape = 0;
  std::shared_ptr<Owner> resource, counter;
  std::shared_ptr<NativeOwner> texture, texel, sampler;
  uint64_t buffer_offset = 0, byte_length = 0, counter_offset = 0;
};
struct PublicPacket : Packet<PublicOwner> {
  std::shared_ptr<PublicOwner> heap;
};
static void NativeRetirement() {
  for (unsigned i = 0; i < 1000; ++i) {
    auto counts = std::make_shared<NativeCounts>();
    auto residency = std::make_shared<NativeResidency>(); residency->counts = counts;
    PublicPacket recorded;
    recorded.resource = std::make_shared<PublicOwner>();
    recorded.counter = std::make_shared<PublicOwner>();
    recorded.resource->native = std::make_shared<NativeOwner>(residency);
    recorded.counter->native = std::make_shared<NativeOwner>(residency);
    recorded.heap = recorded.resource;
    recorded.type = i % 9; recorded.words = {i, 17, 23, 31};
    recorded.buffer_offset = 12; recorded.byte_length = 128; recorded.counter_offset = 4096;
    recorded.texture = recorded.resource->native; recorded.sampler = recorded.counter->native;
    auto first = capture::DetachNative<Packet<NativeOwner>>(recorded);
    Check(first.resource == recorded.resource->native && first.counter == recorded.counter->native);
    Check(first.resource.owner_before(recorded.resource) || recorded.resource.owner_before(first.resource));
    Check(first.words == recorded.words && first.type == recorded.type && first.texture == recorded.texture &&
          first.sampler == recorded.sampler && first.buffer_offset == 12 && first.byte_length == 128 &&
          first.counter_offset == 4096 && !first.texel);
    recorded.words[0] += 100;
    auto second = capture::DetachNative<Packet<NativeOwner>>(recorded);
    Check(first.words[0] == i && second.words[0] == i + 100);
    recorded = {};
    Check(counts->public_destroyed == 2 && counts->natives_destroyed == 0 && counts->resident == 2);
    // Independent simulated completion order; no public-source deleter reaches either worker.
    std::thread a([packet = std::move(first)]() mutable { packet = {}; }); a.join();
    Check(counts->natives_destroyed == 0 && counts->resident == 2);
    std::thread b([packet = std::move(second)]() mutable { packet = {}; }); b.join();
    Check(counts->natives_destroyed == 2 && counts->resident == 0 && counts->public_destroyed == 2);
  }
  PublicPacket empty; const auto native = capture::DetachNative<Packet<NativeOwner>>(empty);
  Check(!native.resource && !native.counter && !native.texture && !native.texel && !native.sampler);
}

int main() {
  Policies(); Replays(); NativeRetirement();
  std::cout << "RECORDED_BINDING_CPU_PASS checks=" << checks << " replay_cases=50000 flag_cases=512 native_retirement_cases=1000 gpu=0\n";
}
