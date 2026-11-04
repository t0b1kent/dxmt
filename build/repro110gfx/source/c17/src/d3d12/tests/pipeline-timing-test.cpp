#include "d3d12_pipeline_timing.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
using namespace dxmt::pipeline_timing;
static Header &head(){return *static_cast<Header*>(mock::mapped);}
static Record &rec(size_t i){return reinterpret_cast<Record*>(static_cast<Header*>(mock::mapped)+1)[i];}
int main(int argc,char **argv){
  assert(argc==2);
  const std::string mode=argv[1];
  if(mode.rfind("depth",0)==0) {
    const auto value=mode.substr(5);
    if(value=="off")unsetenv("MACRUNNER_DX12_LATENCY_DEPTH");
    else setenv("MACRUNNER_DX12_LATENCY_DEPTH",value.c_str(),1);
    const auto expected=value=="2"?2u:value=="3"?3u:0u;
    assert(LatencyOverride()==expected && GetLastError()==123);
    std::printf("PASS pipeline %s (mock; Wine/Metal NOT_ENABLED)\n",mode.c_str());return 0;
  }
  if(mode=="off"){
    unsetenv("MACRUNNER_DX12_PIPELINE_PATH");
    Recorder r; auto n=mock::calls.load();
    for(int i=0;i<10000;i++) assert(!r.Emit(Execute,Point,nullptr));
    assert(mock::calls==n && !mock::mapped && GetLastError()==123);
  } else {
    setenv("MACRUNNER_DX12_PIPELINE_PATH","pipeline.bin",1);
    if(mode=="arm") setenv("MACRUNNER_DX12_PIPELINE_ARM_PATH","world.arm",1);
    if(mode=="failure") mock::fail_create=true;
    {
      Recorder r; assert(GetLastError()==123);
      if(mode=="failure"){assert(!mock::mapped);assert(!r.Emit(Present,Enter,nullptr));}
      else {
        assert(head().capacity==131072 && head().record_size==80);
        assert(mock::bytes==10485888 && head().anchor_span==1);
        if(mode=="arm"){
          assert(!r.Emit(Execute,Enter,nullptr));
          assert(r.Emit(Swapchain,Point,nullptr,0,0,0,0,0,0,true)==1);
          assert(head().window_begin==0);
          mock::arm=true;mock::ticks.fetch_add(250000);
          assert(r.Emit(Execute,Enter,nullptr)==2 && head().window_begin>0);
        } else if(mode=="parallel") {
          std::vector<std::thread> workers;
          for(int t=0;t<4;t++) workers.emplace_back([&]{for(int i=0;i<5000;i++)assert(r.Emit(Execute,Point,nullptr,0,i));});
          for(auto &w:workers)w.join();
          assert(head().count==20000 && !head().overflow && !head().dropped);
          for(unsigned i=0;i<head().count;i++)assert(rec(i).committed==i+1 && rec(i).kind==Execute && rec(i).thread);
        } else if(mode=="overflow") {
          for(unsigned i=0;i<head().capacity;i++)assert(r.Emit(Present,Point,nullptr)==i+1);
          assert(!r.Emit(Present,Point,nullptr));assert(!r.Emit(Present,Point,nullptr));
          assert(head().count==131072 && head().overflow==1 && head().dropped==2);
          assert(rec(0).committed==1 && rec(131071).committed==131072);
        } else if(mode=="boundary") {
          const auto ticket=r.Emit(Execute,Enter,(void*)0x1234,7,8);
          assert(ticket==1);
          mock::ticks.store(head().window_end);
          assert(!r.Emit(Execute,Point,nullptr));
          assert(r.Emit(Execute,Leave,(void*)0x1234,ticket,0,0,0,0,0,true)==2);
          assert(rec(1).id==ticket && rec(1).qpc>=head().window_end);
          mock::fail_clock=true;
          assert(!r.Emit(Execute,Leave,nullptr,0,0,0,0,0,0,true) && head().clock_failed==1);
          mock::fail_clock=false;
        } else if(mode=="scope") {
          // Scope uses the production singleton rather than this local recorder.
          {Scope s(Execute,(void*)0x1234,17,18);s.Result(-7);}
          assert(head().count==2 && rec(0).phase==Enter && rec(1).phase==Leave);
          assert(rec(1).id==1 && rec(1).status==uint32_t(-7));
        } else assert(false);
        assert(GetLastError()==123);
      }
    }
    assert(GetLastError()==123);
  }
  std::printf("PASS pipeline %s (mock; Wine/Metal NOT_ENABLED)\n",mode.c_str());
}
