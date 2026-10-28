// Squizzy Studio engine/audio - automation lanes (document format, header-only, no engine deps).
//
// Track property "user.automation" (string): one lane per line
//     <target> <ppq>:<value> <ppq>:<value> ...
// points sorted by ppq; linear interpolation between points, constant before the first / after
// the last. Values are PLAIN values of the target:
//     volume                 fader in dB (-inf written as -144)
//     pan                    -1..1
//     mute                   0/1 (stepped)
//     send.<n>               level in dB of the n-th line of "user.sends"
//     insert.<slot>.<param>  plain value of parameter <param> of the insert in line <slot> of "user.inserts"
//     instrument.<param>     plain value of a parameter of the track instrument ("user.instrument")
// Track property "user.sends" (string): one send per line  "<bus guid> <level dB> [pre]".
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace sq::engine::audio {

struct AutomationLane {
  std::string target;
  std::vector<std::pair<double, double>> points;  // (ppq, value), sorted by ppq
  double valueAt(double ppq, double fallback) const {
    if (points.empty()) return fallback;
    if (ppq <= points.front().first) return points.front().second;
    if (ppq >= points.back().first) return points.back().second;
    auto it = std::upper_bound(points.begin(), points.end(), ppq, [](double p, const std::pair<double, double>& pt) { return p < pt.first; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    if (b.first <= a.first) return b.second;
    if (target == "mute") return a.second;
    return a.second + (b.second - a.second) * (ppq - a.first) / (b.first - a.first);
  }
};

inline std::vector<AutomationLane> parseAutomation(const std::string& text) {
  std::vector<AutomationLane> lanes;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? text.size() : nl + 1;
    size_t sp = line.find(' ');
    std::string target = line.substr(0, sp);
    while (!target.empty() && (target.back() == '\r' || target.back() == ' ')) target.pop_back();
    if (target.empty()) continue;
    AutomationLane lane;
    lane.target = target;
    while (sp != std::string::npos && sp < line.size()) {
      size_t start = sp + 1;
      sp = line.find(' ', start);
      std::string tok = line.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
      size_t colon = tok.find(':');
      if (colon == std::string::npos) continue;
      lane.points.emplace_back(std::strtod(tok.substr(0, colon).c_str(), nullptr), std::strtod(tok.substr(colon + 1).c_str(), nullptr));
    }
    std::stable_sort(lane.points.begin(), lane.points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    lanes.push_back(std::move(lane));
  }
  return lanes;
}

inline std::string formatAutomation(const std::vector<AutomationLane>& lanes) {
  std::string out;
  char buf[64];
  for (const auto& l : lanes) {
    if (l.target.empty()) continue;
    out += l.target;
    for (const auto& p : l.points) {
      std::snprintf(buf, sizeof buf, " %.10g:%.10g", p.first, p.second);
      out += buf;
    }
    out += '\n';
  }
  return out;
}

}  // namespace sq::engine::audio
