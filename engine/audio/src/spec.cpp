// Squizzy Studio engine/audio - insert-line spec helpers (builtin.hpp; parseSpec/formatSpec live in builtin/catalog.cpp).
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sq/engine/audio/builtin.hpp"

namespace sq::engine::audio {

std::string specStructure(const std::string& spec) {
  SpecParts p;
  if (!parseSpec(spec, &p)) return spec;
  p.params.clear();
  p.state.clear();
  return formatSpec(p);
}

std::string specSetState(const std::string& spec, const std::string& base64) {
  SpecParts p;
  if (!parseSpec(spec, &p)) return spec;
  p.state = base64;
  return formatSpec(p);
}

std::string specSetParam(const std::string& spec, uint32_t index, double value) {
  SpecParts p;
  if (!parseSpec(spec, &p)) return spec;
  bool found = false;
  for (auto& pv : p.params)
    if (pv.first == index) {
      pv.second = value;
      found = true;
    }
  if (!found) p.params.emplace_back(index, value);
  return formatSpec(p);
}

}  // namespace sq::engine::audio
