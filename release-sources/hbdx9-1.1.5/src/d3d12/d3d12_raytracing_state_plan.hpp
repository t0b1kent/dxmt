// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dxmt::state_plan {
constexpr uint32_t None = UINT32_MAX;
enum class Status { Ready, Invalid, Unsupported };
enum Kind : uint32_t { GlobalRoot, LocalRoot, ShaderConfig, PipelineConfig, Other };
struct Item {
  uint32_t kind = Other, key = None, first = 0, second = 0;
};
struct Shader {
  std::string name, alias;
  uint32_t kind = 0, payload = 0, attributes = 0, library = 0, function = 0;
  bool external_dependencies = false;
};
struct Hit {
  std::string name;
  uint32_t type = 0;
  std::array<std::string, 3> imports; // AnyHit, ClosestHit, Intersection.
};
struct Association {
  uint32_t target;
  std::vector<std::string> exports; // Empty means an explicit default.
};
struct Input {
  std::vector<Item> items; // Original D3D12 subobject indices, including Other.
  std::vector<Shader> shaders;
  std::vector<Hit> hits;
  std::vector<Association> associations;
};
using Bindings = std::array<uint32_t, 4>;
struct Plan {
  std::vector<Item> items;
  std::vector<Shader> shaders;
  std::vector<Hit> hits;
  std::vector<Bindings> shader_bindings, hit_bindings;
  std::vector<std::array<uint32_t, 3>> hit_imports;
};

// Root keys designate the same object or byte-identical serialized signatures.
// Different keys may still be semantically equivalent. Defer such comparisons
// rather than falsely reject or silently select one layout.
inline Status Merge(const std::vector<Item> &items, uint32_t &selected, uint32_t incoming) {
  if (incoming == None) return Status::Ready;
  if (selected == None) { selected = incoming; return Status::Ready; }
  const auto &a = items[selected], &b = items[incoming];
  if (a.kind != b.kind) return Status::Invalid;
  if (a.kind <= LocalRoot) return a.key == b.key ? Status::Ready : Status::Unsupported;
  return a.first == b.first && a.second == b.second ? Status::Ready : Status::Invalid;
}

// Bounded single-scope association preparation only. No collection imports,
// embedded DXIL subobjects, external call-graph link or resource compatibility
// proof is implied. No shader identifier or executable pipeline is fabricated.
inline Status Resolve(const Input &input, Plan &out) {
  out = {};
  const uint64_t total = uint64_t(input.shaders.size()) + input.hits.size();
  if (total >= None || input.items.size() >= None) return Status::Unsupported;
  const uint32_t shader_count = uint32_t(input.shaders.size());
  std::map<std::string, uint32_t> names;
  std::map<std::string, std::vector<uint32_t>> aliases;
  for (uint32_t i = 0; i < shader_count; ++i) {
    const auto &s = input.shaders[i];
    if (s.name.empty() || !names.emplace(s.name, i).second) return Status::Invalid;
    if (s.kind < 7 || s.kind > 12 || s.external_dependencies) return Status::Unsupported;
    if (!s.alias.empty()) aliases[s.alias].push_back(i);
  }
  for (uint32_t i = 0; i < input.hits.size(); ++i)
    if (input.hits[i].name.empty() || !names.emplace(input.hits[i].name, shader_count + i).second)
      return Status::Invalid;
  const auto lookup = [&](const std::string &name, uint32_t &index) {
    if (auto it = names.find(name); it != names.end()) { index = it->second; return Status::Ready; }
    auto it = aliases.find(name);
    if (it == aliases.end()) return Status::Invalid;
    if (it->second.size() != 1) return Status::Unsupported;
    index = it->second[0]; return Status::Ready;
  };
  Plan result;
  for (const auto &hit : input.hits) {
    if (hit.type > 1 || (hit.type == 0 && !hit.imports[2].empty()) ||
        (hit.type == 1 && hit.imports[2].empty())) return Status::Invalid;
    std::array<uint32_t, 3> resolved{None, None, None};
    constexpr uint32_t kinds[] = {9, 10, 8};
    for (uint32_t j = 0; j < 3; ++j) {
      if (hit.imports[j].empty()) continue;
      const auto status = lookup(hit.imports[j], resolved[j]);
      if (status != Status::Ready) return status;
      if (resolved[j] >= shader_count || input.shaders[resolved[j]].kind != kinds[j]) return Status::Invalid;
    }
    result.hit_imports.push_back(resolved);
  }
  for (const auto &item : input.items) {
    if (item.kind > Other) return Status::Invalid;
    if (item.kind <= LocalRoot && item.key == None) return Status::Invalid;
    if (item.kind == ShaderConfig && item.second > 32) return Status::Invalid;
    if (item.kind == PipelineConfig) {
      if (item.first > 31) return Status::Invalid;
      if (item.second & ~uint32_t(0x300)) return Status::Unsupported;
    }
  }
  const Bindings empty{None, None, None, None};
  std::vector<Bindings> bindings(size_t(total), empty);
  std::vector<bool> explicit_use(input.items.size(), false), explicit_default(input.items.size(), false);
  const auto assign = [&](uint32_t node, uint32_t target) {
    return Merge(input.items, bindings[node][input.items[target].kind], target);
  };
  for (const auto &a : input.associations) {
    if (a.target >= input.items.size() || input.items[a.target].kind == Other) return Status::Invalid;
    if (a.exports.empty()) { explicit_default[a.target] = true; continue; }
    explicit_use[a.target] = true;
    for (const auto &name : a.exports) {
      uint32_t node = None;
      auto status = lookup(name, node);
      if (status != Status::Ready) return status;
      status = assign(node, a.target);
      if (status != Status::Ready) return status;
      // A hit-group association applies to every imported component. Shared
      // components with competing root keys defer instead of order-dependent binding.
      if (node >= shader_count)
        for (auto component : result.hit_imports[node - shader_count]) {
          if (component == None) continue;
          status = assign(component, a.target);
          if (status != Status::Ready) return status;
        }
    }
  }
  Bindings defaults = empty;
  std::array<bool, 4> default_computed{};
  for (auto &node : bindings) {
    for (uint32_t kind = 0; kind < 4; ++kind) {
      if (node[kind] != None) continue;
      if (!default_computed[kind]) {
        for (uint32_t i = 0; i < input.items.size(); ++i)
          if (input.items[i].kind == kind && (!explicit_use[i] || explicit_default[i])) {
            auto status = Merge(input.items, defaults[kind], i);
            if (status != Status::Ready) return status;
          }
        default_computed[kind] = true;
      }
      node[kind] = defaults[kind];
    }
  }
  for (uint32_t h = 0; h < result.hit_imports.size(); ++h) {
    auto &group = bindings[shader_count + h];
    for (uint32_t kind = 0; kind < 4; ++kind) {
      uint32_t components = None;
      bool missing = false, present = false;
      for (auto component : result.hit_imports[h]) {
        if (component == None) continue;
        const uint32_t value = bindings[component][kind];
        missing |= value == None; present |= value != None;
        const auto status = Merge(input.items, components, value);
        if (status != Status::Ready) return status;
      }
      if (missing && present) return Status::Unsupported;
      const auto status = Merge(input.items, group[kind], components);
      if (status != Status::Ready) return status;
      if (missing && group[kind] != None) return Status::Unsupported;
    }
  }
  Bindings configs = empty;
  for (const auto &node : bindings)
    for (uint32_t kind = ShaderConfig; kind <= PipelineConfig; ++kind) {
      if (node[kind] == None) return Status::Invalid;
      auto status = Merge(input.items, configs[kind], node[kind]);
      if (status != Status::Ready) return status;
    }
  for (uint32_t i = 0; i < shader_count; ++i) {
    const auto &s = input.shaders[i];
    const auto &config = input.items[bindings[i][ShaderConfig]];
    if (s.kind >= 9 && s.kind <= 11 && s.payload > config.first) return Status::Invalid;
    if ((s.kind == 9 || s.kind == 10) && s.attributes > config.second) return Status::Invalid;
  }
  result.items = input.items; result.shaders = input.shaders; result.hits = input.hits;
  result.shader_bindings.assign(bindings.begin(), bindings.begin() + shader_count);
  result.hit_bindings.assign(bindings.begin() + shader_count, bindings.end());
  out = std::move(result);
  return Status::Ready;
}
} // namespace dxmt::state_plan
