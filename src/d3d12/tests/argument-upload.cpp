// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_argument_upload.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace dxmt;
using capture::Status;
static size_t checks = 0;
static void Check(bool value) {
  ++checks;
  if (!value) throw std::runtime_error("check " + std::to_string(checks));
}
struct Resource {
  static inline int live = 0;
  Resource() { ++live; }
  ~Resource() { --live; }
};
struct Source {
  std::array<uint64_t, 4> words{};
  std::shared_ptr<Resource> owner;
};
struct Allocation {
  static inline int live = 0;
  uint64_t address;
  explicit Allocation(uint64_t value) : address(value) { ++live; }
  ~Allocation() { --live; }
};
using Buffer = std::shared_ptr<Allocation>;
using Upload = argument_upload::Upload<Source, Buffer>;
using argument_upload::Plan;
static uint64_t Read(const std::vector<uint8_t> &v, size_t offset) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= uint64_t(v.at(offset+i)) << (i*8);
  return value;
}
static Plan Base() {
  Plan p;
  p.blocks = {{29, std::vector<uint8_t>(80, 0xa5)}, {30, std::vector<uint8_t>(48, 0x5a)}};
  p.fields = {{0, 0, 0, 0, 4}, {0, 32, 1, 1, 2}, {1, 0, 0, 2, 1}};
  p.pointers = {{0, 48, 1, 8}, {1, 16, 0, 32}};
  return p;
}
int main() {
  try {
    Plan p = Base();
    std::vector<Source> sources(2);
    sources[0] = {{1, 2, 3, 4}, std::make_shared<Resource>()};
    sources[1] = {{11, 12, 13, 14}, std::make_shared<Resource>()};
    std::vector<uint64_t> addresses{0x1000, 0x2000};
    std::vector<std::vector<uint8_t>> bytes;
    Check(argument_upload::Materialize(p, sources, addresses, bytes) == Status::Ready);
    Check(Read(bytes[0],0) == 1 && Read(bytes[0],24) == 4);
    Check(Read(bytes[0],32) == 12 && Read(bytes[0],40) == 13);
    Check(Read(bytes[0],48) == 0x2008 && Read(bytes[1],16) == 0x1020);
    Check(Read(bytes[1],0) == 3 && bytes[0][79] == 0xa5 && bytes[1][47] == 0x5a);
    auto first = bytes;
    sources[0].words[0] = 99;
    addresses[0] = 0x3000;
    Check(argument_upload::Materialize(p,sources,addresses,bytes) == Status::Ready);
    Check(Read(bytes[0],0) == 99 && Read(bytes[1],16) == 0x3020);
    Check(Read(first[0],0) == 1 && Read(first[1],16) == 0x1020);
    Check(p.blocks[0].initial[0] == 0xa5);
    auto Invalid = [&](Plan q, Status expected=Status::Invalid) {
      bytes = first;
      Check(argument_upload::Materialize(q,sources,addresses,bytes) == expected);
      Check(bytes.empty());
    };
    { auto q=p; q.blocks[0].index=31; Invalid(q); }
    { auto q=p; q.blocks[0].index=30; Invalid(q); }
    { auto q=p; q.blocks[0].initial.clear(); Invalid(q); }
    { auto q=p; q.blocks.resize(32); Invalid(q,Status::Unsupported); }
    { auto q=p; q.blocks[0].initial.resize(16*1024*1024+1); Invalid(q,Status::Unsupported); }
    { auto q=p; q.fields[0].source=2; Invalid(q); }
    { auto q=p; q.fields[0].block=2; Invalid(q); }
    { auto q=p; q.fields[0].word=4; Invalid(q); }
    { auto q=p; q.fields[0].count=0; Invalid(q); }
    { auto q=p; q.fields[0].count=5; Invalid(q); }
    { auto q=p; q.fields[0].offset=1; Invalid(q); }
    { auto q=p; q.fields[0].offset=UINT32_MAX-7; Invalid(q); }
    { auto q=p; q.fields[0].offset=56; Invalid(q); }
    { auto q=p; q.fields.push_back(q.fields[0]); Invalid(q); }
    { auto q=p; q.pointers[0].offset=24; Invalid(q); }
    { auto q=p; q.pointers[0].target=2; Invalid(q); }
    { auto q=p; q.pointers[0].target_offset=48; Invalid(q); }
    { auto q=p; q.pointers[0].target_offset=1; Invalid(q); }
    { auto q=p; q.pointers[0].block=2; Invalid(q); }
    { auto q=p; q.pointers.push_back(q.pointers[0]); Invalid(q); }
    { auto q=p; q.fields.resize(65537); Invalid(q,Status::Unsupported); }
    Check(argument_upload::Validate(p,65537) == Status::Unsupported);
    for (const auto &a : std::vector<std::vector<uint64_t>>{{}, {0x1000}, {0,0x2000}, {0x1001,0x2000},
                                                          {UINT64_MAX-7,0x2000}, {0x1000,0x2000,0x3000}}) {
      bytes = first;
      Check(argument_upload::Materialize(p,sources,a,bytes) == Status::Invalid);
      Check(bytes.empty());
    }
    Check(argument_upload::Materialize(Plan{},std::vector<Source>{},std::vector<uint64_t>{},bytes) == Status::Ready);
    Check(bytes.empty());
    sources = {};
    Check(Resource::live == 0);
    for (unsigned replay=0; replay<10000; ++replay) {
      std::array<unsigned,2> reads{};
      unsigned allocations=0;
      auto owner=std::make_shared<Resource>();
      auto resolve=[&](size_t index,Source &out) {
        ++reads[index];
        out.words={replay+index, replay+index+1, replay+index+2, replay+index+3};
        out.owner=owner;
        return Status::Ready;
      };
      auto allocate=[&](const argument_upload::Block &,Buffer &out,uint64_t &address) {
        address=(uint64_t(replay)+1)*0x10000+(++allocations)*0x1000;
        out=std::make_shared<Allocation>(address);
        return Status::Ready;
      };
      Upload upload;
      Check(argument_upload::Prepare(p,2,resolve,allocate,upload) == Status::Ready);
      Check(reads[0]==1 && reads[1]==1 && allocations==2);
      Check(upload.buffers.size()==2 && upload.sources.size()==2 && upload.bytes.size()==2);
      for(unsigned word=0;word<4;++word) Check(Read(upload.bytes[0],word*8)==replay+word);
      Check(Read(upload.bytes[0],32)==replay+2 && Read(upload.bytes[0],40)==replay+3);
      Check(Read(upload.bytes[0],48)==upload.buffers[1]->address+8);
      Check(Read(upload.bytes[1],16)==upload.buffers[0]->address+32);
      Check(Read(upload.bytes[1],0)==replay+2 && upload.bytes[1][47]==0x5a);
      owner.reset();
      Check(Resource::live==1 && Allocation::live==2);
      auto inflight=std::move(upload);
      upload={};
      Check(Resource::live==1 && Allocation::live==2);
      inflight={};
      Check(Resource::live==0 && Allocation::live==0);
    }
    for(int failure=0;failure<6;++failure) {
      unsigned calls=0,allocations=0;
      Upload upload;
      auto resolve=[&](size_t index,Source &out) {
        ++calls; out.owner=std::make_shared<Resource>();
        if(failure==0 && index==1) return Status::Retired;
        if(failure==1 && index==1) throw std::bad_alloc();
        return Status::Ready;
      };
      auto allocate=[&](const argument_upload::Block &,Buffer &out,uint64_t &address) {
        ++allocations; address=allocations*0x1000;
        out=std::make_shared<Allocation>(address);
        if(failure==2 && allocations==2) return Status::OutOfMemory;
        if(failure==3 && allocations==2) throw std::bad_alloc();
        if(failure==4 && allocations==2) address=UINT64_MAX-7;
        return Status::Ready;
      };
      auto q=p;
      if(failure==5) q.fields[0].source=99;
      Status result=Status::Invalid;
      try { result=argument_upload::Prepare(q,2,resolve,allocate,upload); }
      catch(const std::bad_alloc &) { Check(failure==1 || failure==3); }
      Check(result!=Status::Ready);
      Check(upload.sources.empty() && upload.buffers.empty() && upload.bytes.empty());
      Check(Resource::live==0 && Allocation::live==0);
      Check(failure==5 ? calls==0&&allocations==0 : calls==2);
      if(failure<2) Check(allocations==0);
    }
    // Preparation owns all templates: later recording mutation cannot change a submission.
    Upload u;
    auto resolve=[](size_t,Source &out){out.words={5,6,7,8};return Status::Ready;};
    unsigned a=0;
    auto allocate=[&](const argument_upload::Block &,Buffer &out,uint64_t &address){
      address=(++a)*0x1000;out=std::make_shared<Allocation>(address);return Status::Ready;
    };
    Check(argument_upload::Prepare(p,2,resolve,allocate,u)==Status::Ready);
    p.blocks[0].initial.assign(1,0); p.fields.clear(); p.pointers.clear();
    Check(u.bytes[0].size()==80 && Read(u.bytes[0],0)==5 && Read(u.bytes[0],48)==0x2008);
    u={}; Check(Allocation::live==0);
    std::cout<<"PASS checks="<<checks<<" immutable_submission_replays=10000 failure_paths=6 GPU=0\n";
  } catch (const std::exception &e) { std::cerr<<e.what()<<"\n"; return 1; }
}
