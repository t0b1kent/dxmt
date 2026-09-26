// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_acceleration_capture.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace dxmt::capture;
static unsigned checks,live;
static std::vector<unsigned> released;
#define CHECK(x) do { ++checks; assert(x); } while(0)
struct Allocation {
  unsigned id;
  Allocation(unsigned id):id(id){++live;}
  ~Allocation(){--live;released.push_back(id);}
};
using Scene=AccelerationTree<Allocation>;
int main(){
  Scene::Ptr primitive,instance,out;
  auto make=[](unsigned id){return std::make_shared<Allocation>(id);};
  CHECK(Scene::Create(Scene::Kind::Primitive,make(1),{},primitive)==Status::Ready);
  CHECK(Scene::Create(Scene::Kind::Instance,make(2),{primitive},instance)==Status::Ready);
  CHECK(Scene::Create(Scene::Kind::Instance,make(3),{instance},out)==Status::Invalid && !out);
  CHECK(Scene::Create(Scene::Kind::Primitive,make(4),{primitive},out)==Status::Invalid && !out);
  CHECK(Scene::Create(Scene::Kind::Instance,make(5),{},out)==Status::Invalid && !out);
  CHECK(Scene::Create(Scene::Kind::Instance,make(6),{nullptr},out)==Status::Invalid && !out);
  CHECK(Scene::Create(Scene::Kind::Primitive,{}, {},out)==Status::Invalid && !out);
  CHECK(Scene::Create(static_cast<Scene::Kind>(9),make(7),{},out)==Status::Invalid && !out);
  auto reused=make(8);Scene::Ptr reusedChild;
  CHECK(Scene::Create(Scene::Kind::Primitive,reused,{},reusedChild)==Status::Ready);
  CHECK(Scene::Create(Scene::Kind::Instance,reused,{reusedChild},out)==Status::Invalid);
  reused.reset();reusedChild.reset();
  std::vector<Scene::Ptr> large(4097,primitive);
  CHECK(Scene::Create(Scene::Kind::Instance,make(9),large,out)==Status::Invalid);
  large.pop_back();CHECK(Scene::Create(Scene::Kind::Instance,make(10),large,out)==Status::Ready);
  out.reset();large.clear();released.clear();
  std::weak_ptr<const Scene> child=primitive, parent=instance;
  primitive.reset(); CHECK(!child.expired());
  auto submission=instance; instance.reset(); CHECK(!parent.expired());
  unsigned uses=0; submission->ForEach([&](const auto &){++uses;});CHECK(uses==2);
  submission.reset();CHECK(child.expired()&&parent.expired()&&live==0);
  CHECK((released==std::vector<unsigned>{2,1}));
  for(unsigned i=0;i<1000;++i){
    released.clear();
    CHECK(Scene::Create(Scene::Kind::Primitive,make(1),{},primitive)==Status::Ready);
    CHECK(Scene::Create(Scene::Kind::Instance,make(2),{primitive},instance)==Status::Ready);
    auto repeated=instance;primitive.reset();instance.reset();CHECK(live==2);
    repeated.reset();CHECK(live==0);
    CHECK((released==std::vector<unsigned>{2,1}));
  }
  printf("AS_OWNERSHIP_PASS checks=%u submissions=1000 GPU=0\n",checks);
}
