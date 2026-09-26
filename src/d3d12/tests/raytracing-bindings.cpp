// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_raytracing_bindings.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>

using namespace dxmt;
using namespace dxmt::ray_binding;
static uint64_t checks = 0;
static void Check(bool value) {
  ++checks;
  if (!value) { std::cerr << "FAIL check=" << checks << '\n'; std::abort(); }
}
struct Range { uint32_t RangeType, NumDescriptors, BaseShaderRegister, RegisterSpace, OffsetInDescriptorsFromTableStart; uint32_t Flags = 0; };
struct Parameter {
  uint32_t ParameterType = 0, ShaderVisibility = 0;
  struct { uint32_t NumDescriptorRanges = 0; const Range *pDescriptorRanges = nullptr; } DescriptorTable;
  struct { uint32_t ShaderRegister = 0, RegisterSpace = 0, Num32BitValues = 0; } Constants;
  struct { uint32_t ShaderRegister = 0, RegisterSpace = 0, Flags = 0; } Descriptor;
};
struct StaticSampler {
  uint32_t Filter = 0, AddressU = 1, AddressV = 1, AddressW = 1;
  float MipLODBias = 0;
  uint32_t MaxAnisotropy = 1, ComparisonFunc = 1, BorderColor = 0;
  float MinLOD = 0, MaxLOD = 16;
  uint32_t ShaderRegister = 0, RegisterSpace = 0, ShaderVisibility = 0;
};
struct Desc {
  uint32_t NumParameters; const Parameter *pParameters;
  uint32_t NumStaticSamplers; const StaticSampler *pStaticSamplers;
  uint32_t Flags;
};
struct Fixture {
  uint32_t flags = 0;
  std::vector<Parameter> p;
  std::vector<std::vector<Range>> ranges;
  std::vector<StaticSampler> samplers;
  void Table(std::vector<Range> r, uint32_t visibility = 0) {
    p.push_back({}); p.back().ShaderVisibility = visibility;
    ranges.resize(p.size()); ranges.back() = std::move(r);
  }
  void Direct(uint32_t kind, uint32_t reg, uint32_t space = 0, uint32_t visibility = 0) {
    p.push_back({}); p.back().ParameterType = kind; p.back().ShaderVisibility = visibility;
    p.back().Descriptor = {reg, space}; ranges.resize(p.size());
  }
  void Constants(uint32_t reg, uint32_t count) {
    p.push_back({}); p.back().ParameterType = 1;
    p.back().Constants = {reg, 0, count}; ranges.resize(p.size());
  }
  void Sampler(uint32_t reg, uint32_t space = 0) {
    samplers.push_back({}); samplers.back().ShaderRegister = reg; samplers.back().RegisterSpace = space;
  }
  Desc View() {
    for (size_t i = 0; i < p.size(); ++i)
      if (p[i].ParameterType == 0) p[i].DescriptorTable = {uint32_t(ranges[i].size()), ranges[i].data()};
    return {uint32_t(p.size()), p.data(), uint32_t(samplers.size()), samplers.data(), flags};
  }
  RootLayout Ready() { RootLayout r; Check(Build(View(), r) == Status::Ready); return r; }
};
static ray_library::Resource Resource(uint32_t type, uint32_t kind, uint32_t first, uint32_t last, uint32_t space = 0) {
  return {type, kind, 0, space, first, last, 0, "resource"};
}
struct Library { ray_library::Prepared prepared; };
static state_plan::Plan Plan(uint32_t shaders = 1) {
  state_plan::Plan p;
  p.items = {{state_plan::GlobalRoot, 0}, {state_plan::LocalRoot, 1}, {state_plan::LocalRoot, 2}};
  for (uint32_t i = 0; i < shaders; ++i) {
    p.shaders.push_back({"shader", "", 7, 0, 0, 0, 0, false});
    p.shader_bindings.push_back({0, i + 1, state_plan::None, state_plan::None});
  }
  return p;
}
static std::vector<Library> Libraries() {
  std::vector<Library> l(1); l[0].prepared.reflection.functions.push_back({}); return l;
}
static void Unit() {
  Fixture f; f.Table({{0, 4, 10, 3, 7}, {0, 3, 20, 3, UINT32_MAX}});
  auto g = f.Ready(); Binding b;
  Check(Find(&g, nullptr, Resource(0, 2, 12, 13, 3), b) == Status::Ready);
  Check(b.source == Source::Table && b.parameter == 0 && b.range == 0 && b.descriptor_offset == 9);
  Check(Find(&g, nullptr, Resource(0, 2, 20, 22, 3), b) == Status::Ready && b.descriptor_offset == 11);
  Check(Find(&g, nullptr, Resource(0, 2, 12, 14, 3), b) == Status::Invalid && b.parameter == None);
  Check(Find(&g, nullptr, Resource(0, 2, 12, 12, 4), b) == Status::Invalid);
  f.ranges[0][1].BaseShaderRegister = 12; RootLayout invalid;
  Check(Build(f.View(), invalid) == Status::Invalid && invalid.spans.empty());
  f.ranges[0][1] = {1, 3, 20, 3, 8};
  Check(Build(f.View(), invalid) == Status::Invalid); // Physical alias with other type.
  f.ranges[0][1].RangeType = 0; g = f.Ready(); // Same-type physical alias is legal.
  Fixture local; local.flags = 0x80; local.Direct(3, 12, 3); auto l = local.Ready();
  Check(Pair(&g, &l) == Status::Invalid);
  local.p[0].Descriptor.RegisterSpace = 4; l = local.Ready(); Check(Pair(&g, &l) == Status::Ready);
  Check(Pair(&l, &g) == Status::Invalid);
  Check(Find(&g, &l, Resource(0, 11, 12, 12, 4), b) == Status::Ready && b.local);
  Check(Find(nullptr, &l, Resource(0, 2, 12, 12, 4), b) == Status::Invalid);
  Check(Find(nullptr, &l, Resource(0, 10, 12, 12, 4), b) == Status::Invalid);
  Check(Find(nullptr, &l, Resource(0, 16, 12, 12, 4), b) == Status::Ready);
  Check(Find(nullptr, &l, Resource(0, 12, 12, 12, 4), b) == Status::Ready);
  Fixture direct; direct.Direct(4, 0); auto d = direct.Ready(); auto u = Resource(1, 12, 0, 0);
  u.flags = 2; Check(Find(&d, nullptr, u, b) == Status::Invalid);
  u.flags = 8; Check(Find(&d, nullptr, u, b) == Status::Unsupported);
  u.flags = 0; Check(Find(&d, nullptr, u, b) == Status::Ready);
  u.kind = 16; Check(Find(&d, nullptr, u, b) == Status::Invalid);
  Fixture constants; constants.Constants(0, 4); auto c = constants.Ready();
  Check(Find(&c, nullptr, Resource(2, 13, 0, 0), b) == Status::Unsupported);
  Fixture split; split.Table({{0, 2, 0, 0, 0}, {0, 2, 2, 0, 2}}); auto s = split.Ready();
  Check(Find(&s, nullptr, Resource(0, 2, 0, 3), b) == Status::Unsupported);
  Check(Find(&s, nullptr, Resource(0, 2, 0, 4), b) == Status::Invalid);
  Fixture unbounded; unbounded.Table({{0, UINT32_MAX, 7, 0, 5}}); auto ub = unbounded.Ready();
  Check(Find(&ub, nullptr, Resource(0, 2, 8, UINT32_MAX), b) == Status::Ready && b.descriptor_offset == 6);
  unbounded.ranges[0].push_back({0, 1, 0, 0, UINT32_MAX});
  Check(Build(unbounded.View(), invalid) == Status::Invalid);
  unbounded.ranges[0].back().OffsetInDescriptorsFromTableStart = 0; unbounded.Ready();
  Fixture overflow; overflow.Table({{0, 2, UINT32_MAX, 0, 0}});
  Check(Build(overflow.View(), invalid) == Status::Invalid);
  overflow.ranges[0][0] = {0, 2, 0, 0, UINT32_MAX - 1}; overflow.Ready();
  overflow.ranges[0][0].NumDescriptors = 3; Check(Build(overflow.View(), invalid) == Status::Invalid);
  overflow.ranges[0][0].NumDescriptors = 0; Check(Build(overflow.View(), invalid) == Status::Invalid);
  Fixture visibility; visibility.Direct(3, 1, 0, 1); auto v = visibility.Ready();
  Check(v.spans.empty()); Check(Find(&v, nullptr, Resource(0, 11, 1, 1), b) == Status::Invalid);
  visibility.flags = 0x80; Check(Build(visibility.View(), invalid) == Status::Invalid);
  visibility.flags = 0x400; Check(Build(visibility.View(), invalid) == Status::Unsupported);
  Fixture limits; limits.Constants(0, 64); limits.Ready(); limits.p[0].Constants.Num32BitValues = 65;
  Check(Build(limits.View(), invalid) == Status::Invalid);
  limits.flags = 0x80; limits.p[0].Constants.Num32BitValues = 1016; limits.Ready();
  limits.Direct(3, 5); Check(Build(limits.View(), invalid) == Status::Invalid);
  Desc null{1, nullptr, 0, nullptr, 0}; Check(Build(null, invalid) == Status::Invalid);
  Fixture stat; stat.Sampler(5); auto st = stat.Ready();
  Check(Find(&st, nullptr, Resource(3, 14, 5, 5), b) == Status::Ready && b.source == Source::StaticSampler);
  stat.Table({{3, 1, 5, 0, 0}}); Check(Build(stat.View(), invalid) == Status::Invalid);
  Fixture a, z, empty; a.flags = z.flags = 0x80; a.Sampler(5); z.Sampler(5);
  std::array<RootLayout, 3> roots{empty.Ready(), a.Ready(), z.Ready()};
  auto plan = Plan(2); auto libs = Libraries(); std::vector<std::vector<Binding>> mapped;
  const auto lookup = [&](uint32_t key) { return key < roots.size() ? &roots[key] : nullptr; };
  Check(Resolve(plan, libs, lookup, mapped) == Status::Ready);
  z.samplers[0].MipLODBias = -0.0f; roots[2] = z.Ready();
  Check(Resolve(plan, libs, lookup, mapped) == Status::Ready);
  z.samplers[0].AddressU = 2; roots[2] = z.Ready();
  Check(Resolve(plan, libs, lookup, mapped) == Status::Invalid && mapped.empty());
  plan = Plan(); plan.shaders[0].library = 1; Check(Resolve(plan, libs, lookup, mapped) == Status::Invalid);
  plan = Plan(); plan.shaders[0].function = 1; Check(Resolve(plan, libs, lookup, mapped) == Status::Invalid);
  plan = Plan(); plan.items[0].key = 8; Check(Resolve(plan, libs, lookup, mapped) == Status::Invalid);
  plan = Plan(); libs[0].prepared.reflection.functions[0].resources.push_back(0);
  Check(Resolve(plan, libs, lookup, mapped) == Status::Invalid);
  // A normalized snapshot must not borrow descriptor arrays or sampler input.
  auto owned = a.Ready(); a.p.clear(); a.samplers.clear();
  Check(Find(nullptr, &owned, Resource(3, 14, 5, 5), b) == Status::Ready);
  Fixture last; last.Direct(2, UINT32_MAX); auto last_root = last.Ready();
  Check(Find(&last_root, nullptr, Resource(2, 13, UINT32_MAX, UINT32_MAX), b) == Status::Invalid);
  auto malformed = Resource(2, 13, 0, 0); malformed.flags = 2;
  Check(Find(&c, nullptr, malformed, b) == Status::Invalid);
  auto overlap_root = g; overlap_root.local = true;
  Check(Find(&g, &overlap_root, Resource(0, 2, 12, 13, 3), b) == Status::Invalid);
}

static void Random() {
  std::mt19937 gen(0x4281);
  for (uint32_t trial = 0; trial < 10000; ++trial) {
    Fixture a, z; z.flags = 0x80;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Binding> oracle;
    for (uint32_t i = 0; i < 12; ++i) {
      auto &f = i & 1 ? z : a;
      const uint32_t type = gen() % 4, space = gen() % 3, base = i * 8, count = 1 + gen() % 5, offset = gen() % 16;
      const uint32_t parameter = f.p.size(); f.Table({{type, count, base, space, offset}});
      for (uint32_t j = 0; j < count; ++j)
        oracle.emplace(std::tuple{type, space, base + j}, Binding{None, bool(i & 1), Source::Table, parameter, 0, offset + j});
    }
    auto g = a.Ready(), l = z.Ready(); Check(Pair(&g, &l) == Status::Ready);
    for (uint32_t q = 0; q < 48; ++q) {
      uint32_t type = gen() % 4, space = gen() % 3, reg = gen() % 100;
      if (q < 12) { const auto it = std::next(oracle.begin(), gen() % oracle.size()); std::tie(type, space, reg) = it->first; }
      Binding actual; const auto found = Find(&g, &l, Resource(type, type == 2 ? 13 : type == 3 ? 14 : 2, reg, reg, space), actual);
      const auto expected = oracle.find({type, space, reg});
      Check((found == Status::Ready) == (expected != oracle.end()));
      if (expected != oracle.end()) {
        const auto &e = expected->second;
        Check(actual.local == e.local && actual.parameter == e.parameter && actual.descriptor_offset == e.descriptor_offset);
      } else Check(actual.parameter == None);
    }
    auto conflicting = g; conflicting.local = true;
    Check(Pair(&g, &conflicting) == Status::Invalid);
  }
}

static void Corpus(const char *list) {
  std::ifstream names(list); Check(bool(names)); uint32_t libraries = 0; uint64_t resources = 0;
  std::string path;
  while (std::getline(names, path)) {
    std::ifstream file(path, std::ios::binary); Check(bool(file));
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    std::vector<Library> libs(1);
    Check(ray_library::Prepare(bytes.data(), bytes.size(), {}, libs[0].prepared) == ray_library::Result::Ready);
    auto &refl = libs[0].prepared.reflection; Check(refl.functions.size() == 1);
    std::array<Fixture, 2> fixtures; fixtures[1].flags = 0x80;
    std::vector<Binding> expected;
    for (uint32_t i = 0; i < refl.resources.size(); ++i) {
      const auto &r = refl.resources[i]; auto &f = fixtures[i % 2]; Binding b;
      b.local = bool(i % 2); b.resource = i; b.parameter = f.p.size(); b.range = 0;
      const bool scalar = r.lower == r.upper;
      if (i % 3 == 0 && scalar && (r.type == 2 || (r.type == 0 && (r.kind == 11 || r.kind == 12 || r.kind == 16)))) {
        b.source = Source::Descriptor; f.Direct(r.type == 2 ? 2 : 3, r.lower, r.space);
      } else if (i % 3 == 0 && scalar && r.type == 3) {
        b.source = Source::StaticSampler; b.parameter = f.samplers.size(); f.Sampler(r.lower, r.space);
      } else {
        b.source = Source::Table; b.descriptor_offset = 13 + i;
        const uint32_t count = r.upper == UINT32_MAX ? UINT32_MAX : r.upper - r.lower + 1;
        f.Table({{r.type, count, r.lower, r.space, uint32_t(b.descriptor_offset)}});
      }
      expected.push_back(b);
    }
    std::array<RootLayout, 2> roots{fixtures[0].Ready(), fixtures[1].Ready()};
    std::vector<std::vector<Binding>> actual;
    auto plan = Plan();
    const auto lookup = [&](uint32_t key) { return key < roots.size() ? &roots[key] : nullptr; };
    Check(Resolve(plan, libs, lookup, actual) == Status::Ready);
    Check(actual.size() == 1 && actual[0].size() == refl.functions[0].resources.size());
    for (const auto &b : actual[0]) {
      const auto &e = expected[b.resource];
      Check(b.local == e.local && b.source == e.source && b.parameter == e.parameter &&
            b.range == e.range && b.descriptor_offset == e.descriptor_offset);
      ++resources;
    }
    if (!actual[0].empty()) {
      const auto id = actual[0][0].resource; auto &spans = roots[expected[id].local].spans;
      const auto &r = refl.resources[id];
      auto it = std::find_if(spans.begin(), spans.end(), [&](const Span &s) { return s.type == r.type && s.space == r.space && s.first == r.lower; });
      Check(it != spans.end()); spans.erase(it);
      Check(Resolve(plan, libs, lookup, actual) == Status::Invalid && actual.empty());
    }
    ++libraries;
  }
  Check(libraries == 4281 && resources == 31485);
  std::cout << "BINDINGS_CORPUS_PASS libraries=" << libraries << " resources=" << resources
            << " checks=" << checks << " syntheticRoots=1 actualGameRoots=0 gpu=0\n";
}
int main(int argc, char **argv) {
  if (argc == 2) { Corpus(argv[1]); return 0; }
  Unit(); Random();
  std::cout << "BINDINGS_CPU_PASS checks=" << checks << " randomLayouts=10000 queries=480000 gpu=0\n";
}
