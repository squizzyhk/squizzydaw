// hardware-surface: pure hot-plug logic (no I/O, unit-tested on its own).
//
// A surface wants an input and/or output port, either by explicit port id (the connection id
// passed to loadSurfaceMap) or by a case-insensitive substring of the port name (.device
// <Ports>). After every port-list change the service resolves what each surface should be
// connected to now and compares it with what it has open.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sq::hwsurface {

struct PortRef {
  std::string id;
  std::string name;
  uint32_t flags = 0;  // SQ_MIDI_PORT_INPUT / SQ_MIDI_PORT_OUTPUT
};

// Explicit id: that port if it exists with the direction flag, else "". Otherwise the first port
// of that direction whose name contains `match` (ASCII case-insensitive); "" when match is empty.
inline std::string resolvePort(const std::vector<PortRef>& ports, uint32_t dirFlag, const std::string& explicitId,
                               const std::string& match) {
  auto lower = [](std::string s) {
    for (char& c : s)
      if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
  };
  if (!explicitId.empty()) {
    for (const auto& p : ports)
      if ((p.flags & dirFlag) && p.id == explicitId) return p.id;
    return {};
  }
  if (match.empty()) return {};
  const std::string m = lower(match);
  for (const auto& p : ports)
    if ((p.flags & dirFlag) && lower(p.name).find(m) != std::string::npos) return p.id;
  return {};
}

enum class LinkChange { None, Connect, Disconnect, Reconnect };

// current*: ids of the ports the surface has open ("" = none); wanted*: freshly resolved ids.
inline LinkChange planLink(const std::string& currentIn, const std::string& currentOut, const std::string& wantedIn,
                           const std::string& wantedOut) {
  if (currentIn == wantedIn && currentOut == wantedOut) return LinkChange::None;
  const bool open = !currentIn.empty() || !currentOut.empty();
  const bool want = !wantedIn.empty() || !wantedOut.empty();
  if (!open) return want ? LinkChange::Connect : LinkChange::None;
  return want ? LinkChange::Reconnect : LinkChange::Disconnect;
}

}  // namespace sq::hwsurface
