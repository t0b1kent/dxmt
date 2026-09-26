// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_capture_ownership.hpp"
#include "../d3d12_argument_access.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace dxmt::capture;
static std::atomic<uint64_t> checks{0};
static void Check(bool value) {
  const auto n = ++checks;
  if (!value) { std::cerr << "FAIL check=" << n << '\n'; std::abort(); }
}
struct Owner {
  std::mutex mutex;
  std::map<uint32_t,uint32_t> refs;
  unsigned destroyed = 0;
  void Create(uint32_t id) { std::lock_guard lock(mutex); Check(refs.emplace(id,1).second); }
  void Retain(uint32_t id) { std::lock_guard lock(mutex); Check(refs.contains(id)); ++refs.at(id); }
  void Release(uint32_t id) { std::lock_guard lock(mutex); Check(refs.contains(id) && refs.at(id)); --refs.at(id); }
  void Destroy(uint32_t id) { std::lock_guard lock(mutex); Check(refs.contains(id) && !refs.at(id)); refs.erase(id); ++destroyed; }
  uint32_t Count(uint32_t id) { std::lock_guard lock(mutex); return refs.contains(id)?refs.at(id):0; }
};
struct Native {
  std::shared_ptr<Owner> owner;
  uint32_t id;
  Native(std::shared_ptr<Owner> value,uint32_t key):owner(std::move(value)),id(key){owner->Create(id);}
  ~Native(){owner->Destroy(id);}
};
struct Resource {
  std::atomic<bool> live{true};
  Native native;
  ResidencyLease<Owner,uint32_t> lease;
  Resource(std::shared_ptr<Owner> owner,uint32_t id):native(owner,id),lease(owner,id){}
  bool Live()const{return live.load(std::memory_order_acquire);}
  void Retire(){if(live.exchange(false,std::memory_order_acq_rel))native.owner->Release(native.id);}
  ~Resource(){Retire();}
};
struct Payload { std::array<uint64_t,4> words{}; bool operator==(const Payload &) const = default; };
using Cell=Slot<Resource,Payload>;
using Captured=Snapshot<Resource,Payload>;

static void Lifetimes() {
  auto owner=std::make_shared<Owner>();
  auto resource=std::make_shared<Resource>(owner,1),counter=std::make_shared<Resource>(owner,2);
  Cell cell; Captured out;
  Check(cell.Acquire(out)==Status::Uninitialized && !out.resource);
  cell.payload.words={1,17,2,0};cell.Publish(resource,counter,true);
  Check(resource.use_count()==1 && counter.use_count()==1 && owner->Count(1)==2);
  Check(cell.Acquire(out)==Status::Ready && out.resource->native.id==1 && out.counter->native.id==2);
  Check(out.payload==cell.payload);
  resource->Retire();resource.reset();counter->Retire();counter.reset();
  Check(owner->Count(1)==1 && owner->Count(2)==1);
  Captured rejected=out;
  Check(cell.Acquire(rejected)==Status::Retired && !rejected.resource && !rejected.counter);
  Check(owner->destroyed==0);
  out={};Check(owner->destroyed==2 && owner->refs.empty());
  resource=std::make_shared<Resource>(owner,3);cell.Publish(resource);
  resource.reset();Check(owner->destroyed==3);
  Check(cell.Acquire(out)==Status::Retired);
  resource=std::make_shared<Resource>(owner,4);cell.Publish(resource,{},true);
  Check(cell.Acquire(out)==Status::Unsupported && !out.resource);
  cell.Publish({});Check(cell.Acquire(out)==Status::Unsupported);
  cell.Publish(resource);Check(cell.Acquire(out)==Status::Ready);
  cell.Publish({});Check(cell.Acquire(out)==Status::Unsupported && !out.resource);
  resource.reset();Check(owner->refs.empty());
}

static void CopiesAndBounds() {
  std::mt19937_64 rng(0xcaf246);
  auto owner=std::make_shared<Owner>();std::vector<std::shared_ptr<Resource>> resources;
  std::array<Cell,64> cells;std::array<uint32_t,64> expected;
  for(uint32_t i=0;i<64;++i){
    resources.push_back(std::make_shared<Resource>(owner,i));
    cells[i].payload.words={i,~uint64_t(i),0,0};cells[i].Publish(resources[i]);expected[i]=i;
  }
  for(unsigned op=0;op<50000;++op){
    const uint64_t from=rng()%75,to=rng()%75,count=rng()%75;
    const auto before=expected;
    const bool valid=from<=64 && to<=64 && count<=64-from && count<=64-to;
    bool visited=false;
    Check(CopyRange(64,from,64,to,count,true,[&](uint64_t a,uint64_t b){visited=true;cells[b]=cells[a];})==valid);
    if(valid)for(uint64_t n=0;n<count;++n)expected[to+n]=before[from+n];
    if(!valid || from==to)Check(!visited);
    for(unsigned n=0;n<4;++n){const auto at=rng()%64;Captured out;
      Check(cells[at].Acquire(out)==Status::Ready && out.resource->native.id==expected[at]);
      Check(out.payload.words[0]==expected[at] && out.payload.words[1]==~uint64_t(expected[at]));
    }
  }
  bool copied=false;
  Check(!CopyRange(64,UINT64_MAX,64,0,1,false,[&](auto,auto){copied=true;}) && !copied);
  Check(!CopyRange(64,0,64,0,UINT64_MAX,false,[&](auto,auto){copied=true;}) && !copied);
  Check(CopyRange(64,64,64,64,0,true,[&](auto,auto){copied=true;}) && !copied);
  for(unsigned i=0;i<200000;++i){
    const uint64_t total=rng(),first=rng(),count=rng();
    const bool expected_span=__uint128_t(first)+count<=total;
    Check(Span(total,first,count)==expected_span);
    Payload value{{rng(),rng(),rng(),rng()}};
    ClearDescriptor(value);value.words[0]=17;value.words[1]=32;
    Check(value.words[2]==0 && value.words[3]==0);
  }
  resources.clear();for(auto &cell:cells){Captured out;Check(cell.Acquire(out)==Status::Retired);}
  Check(owner->refs.empty());
}


struct LegacySlice {
  uint32_t byteOffset, byteLength, firstElement, elementCount;
};
static void WideViewBounds() {
  auto sliceCase = [](uint64_t total, uint64_t first, uint64_t count, uint32_t stride) {
    const __uint128_t offset = __uint128_t(first) * stride;
    const __uint128_t length = __uint128_t(count) * stride;
    Status expected = Status::Ready;
    if (!stride || offset + length > total) expected = Status::Invalid;
    else if (first > UINT32_MAX || count > UINT32_MAX ||
             offset > UINT32_MAX || length > UINT32_MAX) expected = Status::Unsupported;
    LegacySlice out{17, 23, 31, 47};
    Check(BufferSlice(total, first, count, stride, out) == expected);
    if (expected == Status::Ready) {
      Check(out.byteOffset == offset && out.byteLength == length &&
            out.firstElement == first && out.elementCount == count);
    } else {
      Check(!out.byteOffset && !out.byteLength && !out.firstElement && !out.elementCount);
    }
  };
  auto counterCase = [](uint64_t total, uint64_t offset, bool present) {
    Status expected = Status::Ready;
    if (!present) {
      if (offset) expected = Status::Invalid;
    } else if (offset % 4096 || __uint128_t(offset) + 4 > total) {
      expected = Status::Invalid;
    } else if (offset > UINT32_MAX) {
      expected = Status::Unsupported;
    }
    uint32_t out = 47;
    Check(CounterOffset(total, offset, present, out) == expected);
    Check(out == (expected == Status::Ready ? offset : 0));
  };
  // These were silently narrowed before their physical byte ranges were checked.
  const uint64_t highFirst = uint64_t{1} << 32, highCount = 0x40000001;
  Check(uint32_t(highFirst) == 0 && uint32_t(highCount) * uint32_t{4} == 4);
  sliceCase(16, highFirst, 1, 4);
  sliceCase(16, 0, highCount, 4);
  sliceCase(UINT64_MAX, highFirst, 1, 4);
  sliceCase(UINT64_MAX, 0, highCount, 4);
  counterCase(16, highFirst, true);
  counterCase(UINT64_MAX, highFirst, true);
  for (uint64_t total : {uint64_t{0}, uint64_t{4}, uint64_t{4096}, uint64_t{4100},
                        uint64_t{UINT32_MAX}, highFirst, UINT64_MAX}) {
    for (uint64_t first : {uint64_t{0}, uint64_t{1}, uint64_t{4096},
                          uint64_t{UINT32_MAX}, highFirst, UINT64_MAX}) {
      counterCase(total, first, false);
      counterCase(total, first, true);
      for (uint64_t count : {uint64_t{0}, uint64_t{1}, highCount, UINT64_MAX})
        for (uint32_t stride : {uint32_t{0}, uint32_t{1}, uint32_t{4}, uint32_t{UINT32_MAX}})
          sliceCase(total, first, count, stride);
    }
  }
  std::mt19937_64 rng(0x6432b0);
  for (unsigned i = 0; i < 200000; ++i) {
    uint64_t total = rng(), first = rng(), count = rng(), offset = rng();
    uint32_t stride = uint32_t(rng());
    switch (i % 5) {
    case 0:
      total = 1 << 24; first %= 1024; count %= 1024; stride %= 256;
      offset = (offset % 1024) * 4096;
      break;
    case 1:
      total = UINT64_MAX; first %= (uint64_t{1} << 34); count %= (uint64_t{1} << 34);
      stride = 4; offset &= ~uint64_t{4095};
      break;
    case 2:
      total %= 4096; first %= 4096; count %= 4096; stride %= 16;
      offset %= 8192;
      break;
    case 3:
      count = 0; first %= 4096; stride %= 16; offset = 0;
      break;
    default: break;
    }
    sliceCase(total, first, count, stride);
    counterCase(total, offset, i % 3 != 0);
  }
}

static void RetirementRaces() {
  auto owner=std::make_shared<Owner>();
  for(unsigned i=0;i<1000;++i){
    auto resource=std::make_shared<Resource>(owner,i);Cell cell;cell.Publish(resource);
    std::atomic<bool> retire{false},finished{false};
    std::thread thread([p=std::move(resource),&retire,&finished]()mutable{
      while(!retire.load(std::memory_order_acquire))std::this_thread::yield();
      p->Retire();p.reset();finished.store(true,std::memory_order_release);
    });
    Captured early;Check(cell.Acquire(early)==Status::Ready);
    retire.store(true,std::memory_order_release);
    while(!finished.load(std::memory_order_acquire))std::this_thread::yield();
    Captured late=early;Check(cell.Acquire(late)==Status::Retired && !late.resource);
    Check(owner->Count(i)==1 && early.resource->native.id==i);
    thread.join();early={};Check(owner->Count(i)==0);
  }
  Check(owner->refs.empty() && owner->destroyed==1000);
}

static void AtomicHeapSnapshots() {
  auto owner=std::make_shared<Owner>();
  auto a=std::make_shared<Resource>(owner,1),b=std::make_shared<Resource>(owner,2);
  std::mutex heap_mutex;Cell cell;cell.payload.words={1,1,1,1};cell.Publish(a);
  std::thread writer([&]{for(unsigned i=0;i<30000;++i){auto p=i%2?a:b;std::lock_guard lock(heap_mutex);
    cell.payload.words.fill(p->native.id);cell.Publish(p);
  }});
  for(unsigned i=0;i<30000;++i){Captured out;{std::lock_guard lock(heap_mutex);Check(cell.Acquire(out)==Status::Ready);}
    for(auto value:out.payload.words)Check(value==out.resource->native.id);
  }
  writer.join();a.reset();b.reset();Check(owner->refs.empty());
}

static void ArgumentToSnapshot() {
  namespace ra=dxmt::root_argument;
  ra::Layout layout;layout.count=1;layout.qwords=1;layout.parameters[0]={ra::Table,0,0,0};
  ra::State arguments;ra::HeapRange heap{4096,8,32,0};ra::TableLocation location;
  auto owner=std::make_shared<Owner>(),owner_unused=std::make_shared<Owner>();
  auto resource=std::make_shared<Resource>(owner,7);std::array<Cell,8> cells;
  cells[5].payload.words={7,32,0,0};cells[5].Publish(resource);
  Check(!ra::ReadTable(layout,arguments,0,heap,3,1,location));
  Check(arguments.WriteValue(layout,0,ra::Table,4096+2*32));
  Check(ra::ReadTable(layout,arguments,0,heap,3,1,location) && location.index==5);
  Captured captured;Check(cells[location.index].Acquire(captured)==Status::Ready && captured.resource->native.id==7);
  arguments.InvalidateTables(layout);Check(!ra::ReadTable(layout,arguments,0,heap,3,1,location));
  resource->Retire();resource.reset();Check(owner->Count(7)==1);
  captured={};Check(owner->refs.empty() && owner_unused->refs.empty());
}

int main(){
  Lifetimes();CopiesAndBounds();WideViewBounds();RetirementRaces();AtomicHeapSnapshots();ArgumentToSnapshot();
  std::cout<<"CAPTURE_OWNERSHIP_CPU_PASS checks="<<checks.load()
           <<" copy_cases=50000 range_cases=200000 wide_slice_cases=200000 wide_counter_cases=200000 retire_races=1000 concurrent_snapshots=30000 gpu=0\n";
}
