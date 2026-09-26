// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_argument_access.hpp"
#include <concepts>
#include "../../util/rc/util_rc_ptr.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace dxmt::root_argument;
static uint64_t checks = 0;
static void Check(bool value) {
  ++checks;
  if (!value) { std::cerr << "FAIL check=" << checks << '\n'; std::abort(); }
}
struct Range { uint32_t RangeType = 0; };
struct Param {
  uint32_t ParameterType = 0;
  struct { uint32_t NumDescriptorRanges = 0; const Range *pDescriptorRanges = nullptr; } DescriptorTable;
  struct { uint32_t Num32BitValues = 0; } Constants;
};
struct Desc { uint32_t NumParameters; const Param *pParameters; uint32_t Flags = 0; };
static bool Same(const State &a, const State &b) { return a.words == b.words && a.initialized == b.initialized; }

static void Edges() {
  Layout layout;
  Check(Build(Desc{0, nullptr}, layout));
  State state;
  Check(!state.WriteValue(layout, 0, Table, 1));
  Check(!Build(Desc{1, nullptr}, layout));
  Range range;
  Param p[64];
  for (auto &v : p) v.DescriptorTable = {1, &range};
  Check(Build(Desc{64, p}, layout) && layout.qwords == 64);
  Check(state.WriteValue(layout, 63, Table, 8));
  const State saved = state;
  Check(!state.WriteValue(layout, 64, Table, 9) && Same(state, saved));
  Check(!state.WriteValue(layout, UINT32_MAX, Table, 9) && Same(state, saved));
  Check(!state.WriteValue(layout, 0, CBV, 9) && Same(state, saved));
  Check(!Build(Desc{65, p}, layout) && layout.count == 0);
  Check(!Build(Desc{1, p, 0x80}, layout));
  p[0].ParameterType = CBV;
  Check(!Build(Desc{64, p}, layout));
  Check(Build(Desc{63, p}, layout) && layout.qwords == 63);
  p[0].ParameterType = Constants; p[0].Constants.Num32BitValues = 64;
  Check(Build(Desc{1, p}, layout) && layout.qwords == 32);
  state = {};
  std::array<uint32_t, 64> data;
  for (uint32_t i = 0; i < 64; ++i) data[i] = i + 7;
  Check(!state.ConstantsReady(layout, 0));
  Check(state.WriteConstants(layout, 0, 1, data.data(), 63));
  Check(!state.ConstantsReady(layout, 0));
  Check(state.WriteConstants(layout, 0, 63, data.data(), 0));
  Check(state.ConstantsReady(layout, 0));
  Check(state.WriteConstants(layout, 0, 64, data.data(), 0));
  Check(std::memcmp(state.words.data(), data.data(), 256) == 0);
  for (auto [count, offset] : std::vector<std::pair<uint32_t,uint32_t>>{{1,64},{64,1},{UINT32_MAX,1},{1,UINT32_MAX}}) {
    const auto before = state;
    Check(!state.WriteConstants(layout, 0, count, data.data(), offset) && Same(state, before));
  }
  Check(!state.WriteConstants(layout, 1, 1, data.data(), 0));
  Check(!state.WriteConstants(layout, 0, 1, nullptr, 0));
  Check(state.WriteConstants(layout, 0, 0, nullptr, 64));
  std::array<unsigned char, 9> unaligned{};
  unaligned[1] = 17;
  Check(state.WriteConstants(layout, 0, 2, unaligned.data()+1, 0));
  Check((state.words[0] & 255) == 17);
  p[0].Constants.Num32BitValues = 65;
  Check(!Build(Desc{1,p}, layout));
  p[0].Constants.Num32BitValues = 0;
  Check(Build(Desc{1,p}, layout) && layout.qwords==0);
  Check(state.WriteConstants(layout,0,0,nullptr,0));
  Check(!state.WriteConstants(layout,0,1,data.data(),0));
  Check(!state.ConstantsReady(layout,0));
  p[0].ParameterType = 99;
  Check(!Build(Desc{1,p}, layout));
  p[0].ParameterType = Table;
  p[0].DescriptorTable = {0,nullptr};
  Check(Build(Desc{1,p},layout) && layout.parameters[0].heap_type==2);
  Check(state.WriteValue(layout,0,Table,0x1000));
  TableLocation empty;
  Check(!ReadTable(layout,state,0,HeapRange{0x1000,4,32,0},0,1,empty));
  Range mixed[] = {{0},{3}};
  p[0].DescriptorTable = {2,mixed};
  Check(!Build(Desc{1,p}, layout));
  p[0].DescriptorTable = {1,&range};
  Check(Build(Desc{1,p}, layout));
  state = {};
  HeapRange heap{0x1000, 4, 32, 0}; TableLocation loc;
  Check(!ReadTable(layout,state,0,heap,0,1,loc));
  Check(state.WriteValue(layout,0,Table,0x1020));
  Check(ReadTable(layout,state,0,heap,2,1,loc) && loc.index==3 && loc.address==0x1060);
  Check(!ReadTable(layout,state,0,heap,2,2,loc) && loc.address==0);
  Check(!ReadTable(layout,state,0,HeapRange{0x1000,4,32,1},0,1,loc));
  Check(!Locate(heap,0x1080,0,1,loc));
  Check(!Locate(heap,0x1001,0,1,loc));
  Check(!Locate(heap,0xfff,0,1,loc));
  Check(!Locate(heap,0x1000,UINT32_MAX,1,loc));
  Check(!Locate(heap,0x1000,0,0,loc));
  Check(!HeapRange{UINT64_MAX-30,1,32,0}.Valid());
  Check(HeapRange{UINT64_MAX-31,1,32,0}.Valid());
  state.InvalidateTables(layout);
  Check(!ReadTable(layout,state,0,heap,0,1,loc));
  uint64_t value=7;
  Check(!state.ReadValue(layout,64,Table,value) && value==0);
}

static void RandomWrites() {
  std::mt19937 rng(0xa661);
  Range ranges[] = {{0},{1},{2},{3}};
  for (unsigned test=0; test<10000; ++test) {
    std::vector<Param> p;
    unsigned cost=0, qwords=0;
    std::vector<uint32_t> offsets;
    while (cost<64) {
      Param next; next.ParameterType=rng()%5;
      next.DescriptorTable={1,&ranges[rng()%4]}; next.Constants.Num32BitValues=1+rng()%16;
      const auto add=next.ParameterType==Constants?next.Constants.Num32BitValues:next.ParameterType==Table?1u:2u;
      if (add>64-cost) break;
      cost+=add; offsets.push_back(qwords);
      qwords+=next.ParameterType==Constants?(next.Constants.Num32BitValues+1)/2:1;
      p.push_back(next);
    }
    Layout layout; State state;
    Check(Build(Desc{uint32_t(p.size()),p.data()},layout));
    Check(layout.qwords==qwords && layout.count==p.size());
    std::array<uint32_t,128> reference{};
    for (unsigned op=0; op<80; ++op) {
      const uint32_t index=rng()%(p.size()+2), count=rng()%19, offset=rng()%19;
      const Kind kind=Kind(rng()%5);
      std::array<uint32_t,20> data; for (auto &word:data) word=rng();
      const uint64_t val=(uint64_t(rng())<<32)|rng();
      const State before=state;
      bool expected=index<p.size() && p[index].ParameterType==kind;
      bool actual;
      if (kind==Constants) {
        expected=expected && offset<=p[index].Constants.Num32BitValues && count<=p[index].Constants.Num32BitValues-offset;
        actual=state.WriteConstants(layout,index,count,data.data(),offset);
        if (expected && count) std::memcpy(reference.data()+offsets[index]*2+offset,data.data(),count*4);
      } else {
        actual=state.WriteValue(layout,index,kind,val);
        if (expected) { reference[offsets[index]*2]=uint32_t(val); reference[offsets[index]*2+1]=uint32_t(val>>32); }
      }
      Check(actual==expected);
      Check(actual || Same(state,before));
      Check(std::memcmp(reference.data(),state.words.data(),sizeof(reference))==0);
    }
    state.InvalidateTables(layout);
    for (uint32_t i=0; i<p.size(); ++i) {
      Check(layout.parameters[i].qword==offsets[i]);
      if (p[i].ParameterType==Table) Check(state.initialized[i]==0 && state.words[offsets[i]]==0);
    }
  }
}

static void RandomRanges() {
  std::mt19937_64 rng(0xb1ff);
  for (unsigned n=0;n<200000;++n) {
    HeapRange h{rng(),uint32_t(rng()%200),uint32_t(1+rng()%64),uint32_t(rng()%3)};
    const uint64_t table=n%3?h.base+(rng()%220)*h.stride:rng();
    const uint32_t offset=rng()%210,count=rng()%210;
    using Wide=__uint128_t;
    bool expected=h.base && h.count && h.stride && h.type<=1 && Wide(h.base)+Wide(h.count)*h.stride<=(Wide(1)<<64);
    expected=expected && count && table>=h.base && (table-h.base)%h.stride==0;
    Wide first=table>=h.base?(table-h.base)/h.stride:UINT64_MAX;
    expected=expected && first<h.count && first+offset+count<=h.count;
    TableLocation loc{7,9}; const bool actual=Locate(h,table,offset,count,loc);
    Check(actual==expected);
    Check(actual ? loc.index==first+offset && loc.address==Wide(h.base)+(first+offset)*h.stride : loc.index==0 && loc.address==0);
  }
}

struct Allocation {
  std::atomic<unsigned> refs{0};
  std::atomic<unsigned> &destroyed;
  explicit Allocation(std::atomic<unsigned> &d):destroyed(d){}
  void incRef(){ ++refs; }
  void decRef(){ if (--refs==0) { ++destroyed; delete this; } }
};
struct Interval { Allocation *allocation; uint64_t logical_length; };
static void Ownership() {
  std::atomic<unsigned> destroyed{0};
  std::map<uint64_t,Interval> registry;
  std::mutex mutex;
  auto retain=[](Allocation *p){return dxmt::Rc<Allocation>(p);};
  dxmt::Rc<Allocation> owner=new Allocation(destroyed);
  registry.emplace(0x1000,Interval{owner.ptr(),33});
  uint64_t offset=7;
  auto held=AcquireByVA(registry,0x1020,1,offset,retain);
  Check(held && offset==32 && owner->refs==2);
  Check(!AcquireByVA(registry,0x1020,2,offset,retain) && offset==0);
  Check(!AcquireByVA(registry,0xfff,1,offset,retain));
  Check(!AcquireByVA(registry,0x1000,UINT64_MAX,offset,retain));
  Check(bool(AcquireByVA(registry,0x1021,0,offset,retain)) && offset==33);
  registry.clear(); owner=nullptr;
  Check(destroyed==0 && held->refs==1);
  held=nullptr; Check(destroyed==1);
  for (unsigned i=0;i<1000;++i) {
    owner=new Allocation(destroyed); registry.emplace(0x1000,Interval{owner.ptr(),32});
    std::atomic<bool> started=false;
    std::unique_lock lock(mutex);
    std::thread unregister([&]{started=true;std::lock_guard guard(mutex);registry.clear();owner=nullptr;});
    while (!started) std::this_thread::yield();
    held=AcquireByVA(registry,0x1004,4,offset,retain);
    Check(held && offset==4);
    lock.unlock(); unregister.join();
    Check(destroyed==i+1 && held->refs==1);
    held=nullptr; Check(destroyed==i+2);
  }
}

int main() {
  Edges(); RandomWrites(); RandomRanges(); Ownership();
  std::cout << "ARGUMENT_ACCESS_CPU_PASS checks=" << checks
            << " layouts=10000 writes=800000 heap_ranges=200000 owning_lookup_races=1000\n";
}
