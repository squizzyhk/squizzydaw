// hardware-surface: .device / .surface.xml parsing and validation (see surface_map.hpp).
#include "surface_map.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>

#include "sq/platform.hpp"

namespace sq::hwsurface {
namespace {

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return r;
}

bool parseBool(const std::string& s, bool def) {
  if (s.empty()) return def;
  std::string l = lower(s);
  if (l == "true" || l == "1" || l == "yes" || l == "on") return true;
  if (l == "false" || l == "0" || l == "no" || l == "off") return false;
  return def;
}

bool parseDouble(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || !end || *end != '\0' || !std::isfinite(v)) return false;
  out = v;
  return true;
}

std::string where(const Control& c) { return "control '" + c.id + "'"; }

// Reads an integer attribute; records an error when present but malformed or out of range.
bool intAttr(const XmlNode& n, const char* name, int64_t lo, int64_t hi, int64_t& out, const std::string& ctx, Diagnostics& d) {
  if (!n.hasAttr(name)) return false;
  int64_t v = 0;
  std::string s = n.attr(name);
  if (!parseInt(s, v)) {
    d.errors.push_back(ctx + ": attribute " + name + "=\"" + s + "\" is not a number");
    return false;
  }
  if (v < lo || v > hi) {
    d.errors.push_back(ctx + ": attribute " + name + "=" + s + " out of range " + std::to_string(lo) + ".." + std::to_string(hi));
    return false;
  }
  out = v;
  return true;
}

void parseMidi(const XmlNode& m, Control& c, Diagnostics& d) {
  const std::string ctx = where(c);
  int64_t status = 0;
  if (!m.hasAttr("status")) {
    d.errors.push_back(ctx + ": <MidiMessage> needs a status attribute");
    return;
  }
  if (!intAttr(m, "status", 0x80, 0xEF, status, ctx, d)) return;
  MidiSpec& s = c.midi;
  s.channel = uint8_t(status & 0x0F);
  switch (status & 0xF0) {
    case 0x80:
    case 0x90: s.kind = MsgKind::Note; break;
    case 0xA0: s.kind = MsgKind::PolyPressure; break;
    case 0xB0: s.kind = MsgKind::ControlChange; break;
    case 0xC0: s.kind = MsgKind::Program; break;
    case 0xD0: s.kind = MsgKind::ChannelPressure; break;
    default: s.kind = MsgKind::PitchBend; break;
  }
  int64_t v = 0;
  if (intAttr(m, "address", 0, 127, v, ctx, d)) s.address = uint8_t(v);
  if (s.kind == MsgKind::ChannelPressure || s.kind == MsgKind::PitchBend) {
    if (m.hasAttr("address")) d.warnings.push_back(ctx + ": address is ignored for this status");
    s.address = 0;
  }
  int64_t res = s.kind == MsgKind::PitchBend ? 14 : 7;
  if (m.hasAttr("resolution")) {
    if (!intAttr(m, "resolution", 7, 14, res, ctx, d)) return;
    if (res != 7 && res != 14) {
      d.errors.push_back(ctx + ": resolution must be 7 or 14");
      return;
    }
    if (res == 14 && s.kind != MsgKind::ControlChange && s.kind != MsgKind::PitchBend) {
      d.errors.push_back(ctx + ": 14-bit resolution needs a control change (0xB_) or pitch bend (0xE_) status");
      return;
    }
    if (res == 7 && s.kind == MsgKind::PitchBend) d.warnings.push_back(ctx + ": pitch bend is always 14-bit");
  }
  s.fourteenBit = s.kind == MsgKind::PitchBend || res == 14;
  if (s.fourteenBit && s.kind == MsgKind::ControlChange) {
    int64_t lsb = int64_t(s.address) + 32;
    if (m.hasAttr("lsb")) intAttr(m, "lsb", 0, 127, lsb, ctx, d);
    if (lsb > 127) {
      d.errors.push_back(ctx + ": 14-bit controller " + std::to_string(s.address) + " needs an explicit lsb (address + 32 > 127)");
      lsb = 0;
    }
    s.lsbAddress = uint8_t(lsb);
  } else if (m.hasAttr("lsb")) {
    d.warnings.push_back(ctx + ": lsb is only used by 14-bit control change");
  }
  const int64_t top = s.fourteenBit ? 16383 : 127;
  s.min = 0;
  s.max = int32_t(top);
  if (intAttr(m, "min", 0, top, v, ctx, d)) s.min = int32_t(v);
  if (intAttr(m, "max", 0, top, v, ctx, d)) s.max = int32_t(v);
  c.hasMidi = true;
}

void parseControl(const XmlNode& n, Control& c, Diagnostics& d) {
  c.id = n.attr("id");
  c.label = n.attr("label", c.id);
  const std::string typeName = n.attr("type");
  c.type = remoteTypeFromName(typeName);
  const std::string ctx = where(c);
  if (!c.type) d.errors.push_back(ctx + ": unknown type \"" + typeName + "\"");
  if (parseBool(n.attr("feedback"), false) || c.type == SQ_REMOTE_DISPLAY || c.type == SQ_REMOTE_LED) c.flags |= SQ_REMOTE_FEEDBACK;
  if (parseBool(n.attr("toggle"), false)) c.flags |= SQ_REMOTE_TOGGLE;

  c.mode = c.type == SQ_REMOTE_ENCODER ? EncoderMode::TwosComplement : EncoderMode::Absolute;
  if (n.hasAttr("mode")) {
    std::string m = lower(n.attr("mode"));
    if (m == "twos-complement") c.mode = EncoderMode::TwosComplement;
    else if (m == "sign-magnitude") c.mode = EncoderMode::SignMagnitude;
    else if (m == "offset-64") c.mode = EncoderMode::Offset64;
    else if (m == "absolute") c.mode = EncoderMode::Absolute;
    else d.errors.push_back(ctx + ": unknown encoder mode \"" + n.attr("mode") + "\"");
  }
  if (n.hasAttr("step") && !parseDouble(n.attr("step"), c.step)) d.errors.push_back(ctx + ": step is not a number");
  int64_t v = 0;
  if (intAttr(n, "on", 0, 16383, v, ctx, d)) c.onValue = int32_t(v);
  if (intAttr(n, "off", 0, 16383, v, ctx, d)) c.offValue = int32_t(v);
  if (n.hasAttr("action")) {
    std::string a = lower(n.attr("action"));
    if (a == "page.next") {
      c.action = Action::PageNext;
    } else if (a == "page.prev") {
      c.action = Action::PagePrev;
    } else if (a.rfind("page.", 0) == 0 && parseInt(std::string_view(a).substr(5), v) && v >= 0 && v < 4096) {
      c.action = Action::PageSelect;
      c.actionPage = uint32_t(v);
    } else {
      d.errors.push_back(ctx + ": unknown action \"" + n.attr("action") + "\"");
    }
  }
  if (intAttr(n, "length", 1, 128, v, ctx, d)) c.textLength = uint32_t(v);

  for (XmlNode child : n.children()) {
    if (child.name() == "MidiMessage") {
      if (c.hasMidi) d.errors.push_back(ctx + ": more than one <MidiMessage>");
      else parseMidi(child, c, d);
    } else if (child.name() == "Sysex") {
      if (!parseHexBytes(child.attr("header"), c.sysexHeader)) d.errors.push_back(ctx + ": <Sysex header> is not a list of hex bytes");
    } else {
      d.warnings.push_back(ctx + ": unknown element <" + std::string(child.name()) + "> ignored");
    }
  }
}

}  // namespace

bool parseInt(std::string_view s, int64_t& out) {
  bool neg = false;
  if (!s.empty() && s[0] == '-') {
    neg = true;
    s.remove_prefix(1);
  }
  int base = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s.remove_prefix(2);
  }
  if (s.empty() || s.size() > 12) return false;
  int64_t v = 0;
  for (char ch : s) {
    int dgt;
    if (ch >= '0' && ch <= '9') dgt = ch - '0';
    else if (base == 16 && ch >= 'a' && ch <= 'f') dgt = ch - 'a' + 10;
    else if (base == 16 && ch >= 'A' && ch <= 'F') dgt = ch - 'A' + 10;
    else return false;
    v = v * base + dgt;
  }
  out = neg ? -v : v;
  return true;
}

bool parseHexBytes(std::string_view s, std::vector<uint8_t>& out) {
  out.clear();
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\t')) ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != ',' && s[j] != '\t') ++j;
    std::string tok(s.substr(i, j - i));
    if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) tok = tok.substr(2);
    int64_t v = 0;
    if (tok.empty() || tok.size() > 2 || !parseInt("0x" + tok, v)) return false;
    out.push_back(uint8_t(v));
    i = j;
  }
  return !out.empty();
}

int32_t remoteTypeFromName(std::string_view s) {
  std::string l = lower(s);
  if (l == "knob") return SQ_REMOTE_KNOB;
  if (l == "encoder") return SQ_REMOTE_ENCODER;
  if (l == "fader") return SQ_REMOTE_FADER;
  if (l == "button") return SQ_REMOTE_BUTTON;
  if (l == "pad") return SQ_REMOTE_PAD;
  if (l == "display") return SQ_REMOTE_DISPLAY;
  if (l == "led") return SQ_REMOTE_LED;
  return 0;
}

const char* remoteTypeName(int32_t type) {
  switch (type) {
    case SQ_REMOTE_KNOB: return "knob";
    case SQ_REMOTE_ENCODER: return "encoder";
    case SQ_REMOTE_FADER: return "fader";
    case SQ_REMOTE_BUTTON: return "button";
    case SQ_REMOTE_PAD: return "pad";
    case SQ_REMOTE_DISPLAY: return "display";
    case SQ_REMOTE_LED: return "led";
    default: return "?";
  }
}

int32_t SurfaceMap::find(std::string_view id) const {
  for (size_t i = 0; i < controls.size(); ++i)
    if (controls[i].id == id) return int32_t(i);
  return -1;
}

std::string Diagnostics::text() const {
  std::string s;
  for (const auto& e : errors) s += "error: " + e + "\n";
  for (const auto& w : warnings) s += "warning: " + w + "\n";
  return s;
}

sq_result parseSurfaceMap(std::string_view xml, SurfaceMap& out, Diagnostics& d) {
  out = SurfaceMap{};
  std::string err;
  XmlDocPtr doc = xmlParse(xml, &err);
  if (!doc) {
    d.errors.push_back("xml: " + err);
    return SQ_E_PARSE;
  }
  XmlNode root = xmlRoot(doc);
  if (root.name() != "ControlSurface") {
    d.errors.push_back("root element must be <ControlSurface>, found <" + std::string(root.name()) + ">");
    return SQ_E_PARSE;
  }
  int64_t v = kFormatVersion;
  if (root.hasAttr("format")) intAttr(root, "format", 1, 1000, v, "ControlSurface", d);
  out.format = int32_t(v);
  if (out.format > kFormatVersion) {
    d.errors.push_back("format " + std::to_string(out.format) + " is newer than supported (" + std::to_string(kFormatVersion) + ")");
    return SQ_E_PARSE;
  }
  out.name = root.attr("name");
  XmlNode controls = root.child("Controls");
  if (!controls) d.errors.push_back("missing <Controls>");
  for (XmlNode n : controls.children()) {
    if (n.name() != "Control") {
      d.warnings.push_back("unknown element <" + std::string(n.name()) + "> in <Controls> ignored");
      continue;
    }
    if (out.controls.size() >= kMaxControls) {
      d.errors.push_back("more than " + std::to_string(kMaxControls) + " controls");
      break;
    }
    Control c;
    parseControl(n, c, d);
    out.controls.push_back(std::move(c));
  }
  for (XmlNode b : root.child("Bindings").children("Bind")) {
    StaticBinding sb;
    sb.control = b.attr("control");
    sb.target = b.attr("target");
    sb.property = b.attr("property", "value");
    const std::string ctx = "binding of '" + sb.control + "'";
    if (b.hasAttr("min") && !parseDouble(b.attr("min"), sb.min)) d.errors.push_back(ctx + ": min is not a number");
    if (b.hasAttr("max") && !parseDouble(b.attr("max"), sb.max)) d.errors.push_back(ctx + ": max is not a number");
    out.bindings.push_back(std::move(sb));
  }
  return d.ok() ? SQ_OK : SQ_E_PARSE;
}

void validateSurfaceMap(const SurfaceMap& map, Diagnostics& d) {
  // The map name becomes the surface id when no .device names one (sq_surface_info.id holds 127).
  if (map.name.size() > 100) d.errors.push_back("<ControlSurface> name longer than 100 bytes");
  std::set<std::string> ids;
  // (kind, channel, address) of every incoming message a control listens to.
  // Value: who owns it, for the message ("'a'" or "'a' (14-bit LSB)").
  std::map<std::tuple<int, int, int>, std::string> inputs;
  for (const Control& c : map.controls) {
    const std::string ctx = where(c);
    if (c.id.empty()) d.errors.push_back("a control has no id");
    else if (c.id.size() > 63) d.errors.push_back(ctx + ": id longer than 63 bytes");
    else if (!ids.insert(c.id).second) d.errors.push_back(ctx + ": duplicate id");
    if (c.label.size() > 63) d.warnings.push_back(ctx + ": label longer than 63 bytes is truncated");
    if (!c.type) continue;  // already reported

    const bool isButton = c.type == SQ_REMOTE_BUTTON || c.type == SQ_REMOTE_PAD;
    if (c.type == SQ_REMOTE_DISPLAY) {
      if (c.sysexHeader.empty() || c.sysexHeader[0] != 0xF0) d.errors.push_back(ctx + ": display needs <Sysex header=\"F0 ...\">");
      for (size_t i = 1; i < c.sysexHeader.size(); ++i)
        if (c.sysexHeader[i] >= 0x80) d.errors.push_back(ctx + ": sysex header byte " + std::to_string(i) + " is not a data byte");
      if (c.textLength == 0) d.errors.push_back(ctx + ": display needs length=\"1..128\"");
      continue;
    }
    if (!c.hasMidi) {
      d.errors.push_back(ctx + ": needs a <MidiMessage>");
      continue;
    }
    const MidiSpec& m = c.midi;
    if (m.min >= m.max) d.errors.push_back(ctx + ": min must be below max");
    if ((c.flags & SQ_REMOTE_TOGGLE) && !isButton) d.errors.push_back(ctx + ": toggle is only valid for buttons and pads");
    if (c.action != Action::None && !isButton) d.errors.push_back(ctx + ": action is only valid for buttons and pads");
    if (m.kind == MsgKind::Program && !isButton) d.errors.push_back(ctx + ": program change can only drive a button or pad");
    if (m.kind == MsgKind::PitchBend && isButton) d.errors.push_back(ctx + ": pitch bend cannot drive a button or pad (it has no press)");
    if (m.kind == MsgKind::Program && (c.flags & SQ_REMOTE_FEEDBACK)) d.warnings.push_back(ctx + ": program change cannot show feedback");
    if (c.mode != EncoderMode::Absolute) {
      if (c.type != SQ_REMOTE_ENCODER) d.errors.push_back(ctx + ": relative modes are only valid for encoders");
      else if (m.kind != MsgKind::ControlChange || m.fourteenBit) d.errors.push_back(ctx + ": relative encoders need a 7-bit control change");
    }
    if (!(c.step > 0.0 && c.step <= 1.0)) d.errors.push_back(ctx + ": step must be in (0, 1]");
    const int32_t top = m.fourteenBit ? 16383 : 127;
    if (c.onValue > top || c.offValue > top) d.errors.push_back(ctx + ": on/off exceed the message range");
    if (m.fourteenBit && m.kind == MsgKind::ControlChange) {
      if (m.address > 31) d.warnings.push_back(ctx + ": 14-bit MSB above controller 31 is unusual");
      if (m.lsbAddress == m.address) d.errors.push_back(ctx + ": lsb equals the MSB controller");
    }
    if (c.type == SQ_REMOTE_LED) continue;  // output only: may share a message with its button
    auto key = std::make_tuple(int(m.kind), int(m.channel), int(m.address));
    auto [it, fresh] = inputs.emplace(key, "'" + c.id + "'");
    if (!fresh) d.errors.push_back(ctx + ": MIDI message already used by control " + it->second);
    if (m.fourteenBit && m.kind == MsgKind::ControlChange) {
      auto lsbKey = std::make_tuple(int(m.kind), int(m.channel), int(m.lsbAddress));
      auto [lit, lfresh] = inputs.emplace(lsbKey, "'" + c.id + "' (14-bit LSB)");
      if (!lfresh) d.errors.push_back(ctx + ": 14-bit LSB controller already used by control " + lit->second);
    }
  }
  std::set<std::string> bound;
  for (const StaticBinding& b : map.bindings) {
    const std::string ctx = "binding of '" + b.control + "'";
    if (map.find(b.control) < 0) d.errors.push_back(ctx + ": no such control");
    if (b.target.rfind("object://", 0) != 0) d.errors.push_back(ctx + ": target must be an object:// URL");
    if (b.target.size() > 255) d.errors.push_back(ctx + ": target longer than 255 bytes");
    if (b.property.empty() || b.property.size() > 63) d.errors.push_back(ctx + ": property must be 1..63 bytes");
    if (b.min == b.max) d.errors.push_back(ctx + ": min equals max");
    else if (!std::isfinite(b.max - b.min)) d.errors.push_back(ctx + ": min..max range is too large");
    if (!bound.insert(b.control).second) d.warnings.push_back(ctx + ": bound more than once (the last binding wins)");
  }
}

sq_result parseDevice(std::string_view xml, DeviceInfo& out, Diagnostics& d) {
  out = DeviceInfo{};
  std::string err;
  XmlDocPtr doc = xmlParse(xml, &err);
  if (!doc) {
    d.errors.push_back("xml: " + err);
    return SQ_E_PARSE;
  }
  XmlNode root = xmlRoot(doc);
  if (root.name() != "Device") {
    d.errors.push_back("root element must be <Device>, found <" + std::string(root.name()) + ">");
    return SQ_E_PARSE;
  }
  int64_t v = kFormatVersion;
  if (root.hasAttr("format")) intAttr(root, "format", 1, 1000, v, "Device", d);
  out.format = int32_t(v);
  if (out.format > kFormatVersion) d.errors.push_back("format " + std::to_string(out.format) + " is newer than supported");
  out.id = root.attr("id");
  out.name = root.attr("name", out.id);
  out.vendor = root.attr("vendor");
  out.model = root.attr("model");
  if (out.id.empty()) d.errors.push_back("<Device> needs an id");
  if (out.id.size() > 100) d.errors.push_back("<Device> id longer than 100 bytes");
  XmlNode ports = root.child("Ports");
  out.inputMatch = ports.attr("input");
  out.outputMatch = ports.attr("output");
  out.surfaceFile = root.child("Surface").attr("file");
  if (out.surfaceFile.empty()) d.errors.push_back("<Device> needs <Surface file=\"...\">");
  return d.ok() ? SQ_OK : SQ_E_PARSE;
}

sq_result loadSurfaceFiles(const std::string& path, DeviceInfo& dev, SurfaceMap& map, Diagnostics& d) {
  auto text = readFile(path);
  if (!text) {
    d.errors.push_back("cannot read " + path);
    return SQ_E_IO;
  }
  std::string surfacePath = path;
  const std::string l = lower(path);
  if (l.size() > 7 && l.compare(l.size() - 7, 7, ".device") == 0) {
    sq_result r = parseDevice(*text, dev, d);
    if (SQ_FAILED(r)) return r;
    const size_t slash = path.find_last_of("/\\");
    const bool absolute = dev.surfaceFile.size() > 1 && (dev.surfaceFile[0] == '/' || dev.surfaceFile[1] == ':');
    surfacePath = (absolute || slash == std::string::npos) ? dev.surfaceFile : joinPath(path.substr(0, slash), dev.surfaceFile);
    text = readFile(surfacePath);
    if (!text) {
      d.errors.push_back("cannot read " + surfacePath);
      return SQ_E_IO;
    }
  }
  sq_result r = parseSurfaceMap(*text, map, d);
  if (SQ_FAILED(r)) return r;
  validateSurfaceMap(map, d);
  if (!d.ok()) return SQ_E_INVALIDARG;
  if (dev.id.empty()) {
    dev.id = map.name.empty() ? std::string("surface") : map.name;
    dev.name = dev.id;
  }
  return SQ_OK;
}

}  // namespace sq::hwsurface
