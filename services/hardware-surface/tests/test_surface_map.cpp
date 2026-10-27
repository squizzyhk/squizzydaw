// hardware-surface core without the module: map parsing + validation, MIDI matching (CC, 14-bit
// CC, note, pitch bend, pressure, program, relative encoders), feedback encoding and the pure
// hot-plug reconnect logic.
#include <cmath>
#include <string>

#include "matcher.hpp"
#include "port_diff.hpp"
#include "sq/platform.hpp"
#include "sqtest.hpp"
#include "surface_map.hpp"

using namespace sq::hwsurface;

namespace {
bool hasMessage(const std::vector<std::string>& v, const std::string& needle) {
  for (const auto& s : v)
    if (s.find(needle) != std::string::npos) return true;
  return false;
}

SurfaceMap mustParse(const std::string& controls, const std::string& bindings = {}) {
  std::string xml = "<ControlSurface format=\"1\" name=\"T\"><Controls>" + controls + "</Controls>";
  if (!bindings.empty()) xml += "<Bindings>" + bindings + "</Bindings>";
  xml += "</ControlSurface>";
  SurfaceMap m;
  Diagnostics d;
  sq_result r = parseSurfaceMap(xml, m, d);
  if (r != SQ_OK) std::printf("%s", d.text().c_str());
  validateSurfaceMap(m, d);
  if (!d.ok()) std::printf("%s", d.text().c_str());
  return m;
}

Diagnostics problems(const std::string& controls, const std::string& bindings = {}) {
  std::string xml = "<ControlSurface name=\"T\"><Controls>" + controls + "</Controls><Bindings>" + bindings + "</Bindings></ControlSurface>";
  SurfaceMap m;
  Diagnostics d;
  if (parseSurfaceMap(xml, m, d) == SQ_OK) validateSurfaceMap(m, d);
  return d;
}

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

bool feed(Matcher& m, std::initializer_list<uint8_t> bytes, ControlEvent& ev) {
  uint8_t b[3] = {};
  uint32_t n = 0;
  for (uint8_t x : bytes) b[n++] = x;
  return m.match(b, n, ev);
}
}  // namespace

SQ_TEST("surface map: shipped example .device + .surface.xml load and validate") {
  DeviceInfo dev;
  SurfaceMap map;
  Diagnostics d;
  const std::string path = sq::joinPath(SQ_HWS_MAPS_DIR, "generic8.device");
  sq_result r = loadSurfaceFiles(path, dev, map, d);
  if (r != SQ_OK) std::printf("%s", d.text().c_str());
  REQUIRE_OK(r);
  CHECK(d.warnings.empty());
  CHECK(dev.id == "squizzy.generic8");
  CHECK(dev.inputMatch == "Squizzy Generic 8");
  CHECK(dev.surfaceFile == "generic8.surface.xml");
  CHECK_EQ(map.controls.size(), size_t(27));
  CHECK_EQ(map.bindings.size(), size_t(16));
  const int32_t f1 = map.find("fader1");
  REQUIRE(f1 >= 0);
  const Control& c = map.controls[size_t(f1)];
  CHECK_EQ(c.type, SQ_REMOTE_FADER);
  CHECK(c.midi.kind == MsgKind::PitchBend);
  CHECK(c.midi.fourteenBit);
  CHECK_EQ(c.midi.max, 16383);
  CHECK(c.flags & SQ_REMOTE_FEEDBACK);
  const Control& disp = map.controls[size_t(map.find("display"))];
  CHECK_EQ(disp.textLength, 16u);
  CHECK_EQ(disp.sysexHeader.size(), size_t(5));
  CHECK(map.controls[size_t(map.find("pageNext"))].action == Action::PageNext);

  // A .surface.xml can be loaded directly; the map name becomes the id.
  DeviceInfo dev2;
  SurfaceMap map2;
  Diagnostics d2;
  REQUIRE_OK(loadSurfaceFiles(sq::joinPath(SQ_HWS_MAPS_DIR, "generic8.surface.xml"), dev2, map2, d2));
  CHECK(dev2.id == "Generic 8");
  CHECK(dev2.inputMatch.empty());
  Diagnostics d3;
  CHECK_EQ(loadSurfaceFiles(sq::joinPath(SQ_HWS_MAPS_DIR, "missing.device"), dev2, map2, d3), SQ_E_IO);
}

SQ_TEST("surface map: attribute syntax and structural errors") {
  int64_t v = 0;
  CHECK(parseInt("0xB0", v) && v == 0xB0);
  CHECK(parseInt("176", v) && v == 176);
  CHECK(parseInt("-5", v) && v == -5);
  CHECK(!parseInt("12x", v));
  CHECK(!parseInt("", v));
  std::vector<uint8_t> bytes;
  CHECK(parseHexBytes("F0 7D,0x01  02", bytes));
  CHECK_EQ(bytes.size(), size_t(4));
  CHECK_EQ(bytes[3], 2);
  CHECK(!parseHexBytes("F0 XYZ", bytes));

  SurfaceMap m;
  Diagnostics d;
  CHECK_EQ(parseSurfaceMap("<ControlSurface><Controls>", m, d), SQ_E_PARSE);  // malformed XML
  CHECK(hasMessage(d.errors, "xml"));
  Diagnostics d2;
  CHECK_EQ(parseSurfaceMap("<Surface/>", m, d2), SQ_E_PARSE);
  CHECK(hasMessage(d2.errors, "root element"));
  Diagnostics d3;
  CHECK_EQ(parseSurfaceMap("<ControlSurface format=\"2\"><Controls/></ControlSurface>", m, d3), SQ_E_PARSE);
  CHECK(hasMessage(d3.errors, "newer"));
  Diagnostics d4;
  CHECK_EQ(parseSurfaceMap("<ControlSurface><Controls><Control id=\"a\" type=\"knob\"><MidiMessage status=\"0xF8\"/></Control></Controls></ControlSurface>", m, d4),
           SQ_E_PARSE);
  CHECK(hasMessage(d4.errors, "status"));
  DeviceInfo dev;
  Diagnostics d5;
  CHECK_EQ(parseDevice("<Device id=\"x\"/>", dev, d5), SQ_E_PARSE);
  CHECK(hasMessage(d5.errors, "Surface"));
}

SQ_TEST("surface map: validator catches semantic mistakes") {
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"knob\"><MidiMessage status=\"0xB0\" address=\"1\"/></Control>"
                            "<Control id=\"a\" type=\"knob\"><MidiMessage status=\"0xB0\" address=\"2\"/></Control>")
                       .errors,
                   "duplicate id"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"knob\"><MidiMessage status=\"0xB0\" address=\"1\"/></Control>"
                            "<Control id=\"b\" type=\"knob\"><MidiMessage status=\"0xB0\" address=\"1\"/></Control>")
                       .errors,
                   "already used"));
  // note-on and note-off status both mean "note": same key.
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"button\"><MidiMessage status=\"0x90\" address=\"1\"/></Control>"
                            "<Control id=\"b\" type=\"button\"><MidiMessage status=\"0x80\" address=\"1\"/></Control>")
                       .errors,
                   "already used"));
  // Different channel: fine.
  CHECK(problems("<Control id=\"a\" type=\"knob\"><MidiMessage status=\"0xB0\" address=\"1\"/></Control>"
                 "<Control id=\"b\" type=\"knob\"><MidiMessage status=\"0xB1\" address=\"1\"/></Control>")
            .ok());
  // 14-bit LSB (address + 32) collides with a plain controller.
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"fader\"><MidiMessage status=\"0xB0\" address=\"7\" resolution=\"14\"/></Control>"
                            "<Control id=\"b\" type=\"knob\"><MidiMessage status=\"0xB0\" address=\"39\"/></Control>")
                       .errors,
                   "LSB"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"fader\"><MidiMessage status=\"0xB0\" address=\"100\" resolution=\"14\"/></Control>").errors,
                   "explicit lsb"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"fader\"><MidiMessage status=\"0x90\" address=\"1\" resolution=\"14\"/></Control>").errors,
                   "14-bit"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"fader\" toggle=\"true\"><MidiMessage status=\"0xB0\" address=\"1\"/></Control>").errors,
                   "toggle"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"fader\" mode=\"offset-64\"><MidiMessage status=\"0xB0\" address=\"1\"/></Control>").errors,
                   "relative"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"encoder\"><MidiMessage status=\"0xE0\"/></Control>").errors, "7-bit control change"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"knob\"/>").errors, "MidiMessage"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"slider\"><MidiMessage status=\"0xB0\"/></Control>").errors, "unknown type"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"display\" length=\"8\"/>").errors, "Sysex"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"knob\"><MidiMessage status=\"0xB0\" min=\"100\" max=\"10\"/></Control>").errors, "min"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"knob\"><MidiMessage status=\"0xC0\" address=\"3\"/></Control>").errors, "program change"));
  CHECK(hasMessage(problems("<Control id=\"a\" type=\"button\" action=\"page.sideways\"><MidiMessage status=\"0x90\"/></Control>").errors,
                   "action"));
  const std::string knob = "<Control id=\"k\" type=\"knob\"><MidiMessage status=\"0xB0\" address=\"1\"/></Control>";
  CHECK(hasMessage(problems(knob, "<Bind control=\"nope\" target=\"object://doc/tracks/0\"/>").errors, "no such control"));
  CHECK(hasMessage(problems(knob, "<Bind control=\"k\" target=\"http://x\"/>").errors, "object://"));
  CHECK(hasMessage(problems(knob, "<Bind control=\"k\" target=\"object://a\" min=\"1\" max=\"1\"/>").errors, "min equals max"));
  CHECK(hasMessage(problems(knob, "<Bind control=\"k\" target=\"object://a\"/><Bind control=\"k\" target=\"object://b\"/>").warnings,
                   "more than once"));
  // An LED may share its message with the button that drives it.
  CHECK(problems("<Control id=\"b\" type=\"button\"><MidiMessage status=\"0x90\" address=\"5\"/></Control>"
                 "<Control id=\"l\" type=\"led\"><MidiMessage status=\"0x90\" address=\"5\"/></Control>")
            .ok());
}

SQ_TEST("surface matcher: 7-bit CC with a raw range, channel filtering, unknown messages") {
  SurfaceMap m = mustParse("<Control id=\"k\" type=\"knob\"><MidiMessage status=\"0xB2\" address=\"10\" min=\"27\" max=\"127\"/></Control>");
  Matcher mt(m);
  ControlEvent ev;
  REQUIRE(feed(mt, {0xB2, 10, 77}, ev));
  CHECK_EQ(ev.control, 0u);
  CHECK(!ev.relative);
  CHECK(near(ev.value, 0.5));
  REQUIRE(feed(mt, {0xB2, 10, 5}, ev));  // below min clamps
  CHECK(near(ev.value, 0.0));
  CHECK(!feed(mt, {0xB0, 10, 77}, ev));  // other channel
  CHECK(!feed(mt, {0xB2, 11, 77}, ev));  // other controller
  CHECK(!feed(mt, {0xF8}, ev));          // realtime
  CHECK(!feed(mt, {0xB2, 10}, ev));      // truncated
}

SQ_TEST("surface matcher: notes as momentary buttons, toggles and velocity pads") {
  SurfaceMap m = mustParse(
      "<Control id=\"mom\" type=\"button\"><MidiMessage status=\"0x90\" address=\"60\"/></Control>"
      "<Control id=\"tog\" type=\"button\" toggle=\"true\"><MidiMessage status=\"0x90\" address=\"61\"/></Control>"
      "<Control id=\"pad\" type=\"pad\"><MidiMessage status=\"0x99\" address=\"36\"/></Control>");
  Matcher mt(m);
  ControlEvent ev;
  REQUIRE(feed(mt, {0x90, 60, 100}, ev));
  CHECK_EQ(ev.control, 0u);
  CHECK(near(ev.value, 1.0));
  REQUIRE(feed(mt, {0x80, 60, 64}, ev));  // note-off with release velocity
  CHECK(near(ev.value, 0.0));
  REQUIRE(feed(mt, {0x90, 60, 127}, ev));
  REQUIRE(feed(mt, {0x90, 60, 0}, ev));  // note-on velocity 0 = release
  CHECK(near(ev.value, 0.0));

  REQUIRE(feed(mt, {0x90, 61, 127}, ev));
  CHECK_EQ(ev.control, 1u);
  CHECK(near(ev.value, 1.0));
  CHECK(mt.toggleState(1));
  CHECK(!feed(mt, {0x80, 61, 0}, ev));  // release ignored
  REQUIRE(feed(mt, {0x90, 61, 127}, ev));
  CHECK(near(ev.value, 0.0));
  mt.setToggleState(1, true);  // e.g. the target was switched on elsewhere
  REQUIRE(feed(mt, {0x90, 61, 127}, ev));
  CHECK(near(ev.value, 0.0));

  REQUIRE(feed(mt, {0x99, 36, 127}, ev));
  CHECK(near(ev.value, 1.0));
  REQUIRE(feed(mt, {0x99, 36, 64}, ev));
  CHECK(near(ev.value, 64.0 / 127.0));
  REQUIRE(feed(mt, {0x89, 36, 0}, ev));
  CHECK(near(ev.value, 0.0));
}

SQ_TEST("surface matcher: pitch bend is 14-bit") {
  SurfaceMap m = mustParse("<Control id=\"f\" type=\"fader\"><MidiMessage status=\"0xE3\"/></Control>");
  Matcher mt(m);
  ControlEvent ev;
  REQUIRE(feed(mt, {0xE3, 0, 0}, ev));
  CHECK(near(ev.value, 0.0));
  REQUIRE(feed(mt, {0xE3, 0x7F, 0x7F}, ev));
  CHECK(near(ev.value, 1.0));
  REQUIRE(feed(mt, {0xE3, 0x00, 0x40}, ev));  // 8192
  CHECK(near(ev.value, 8192.0 / 16383.0));
  REQUIRE(feed(mt, {0xE3, 0x01, 0x00}, ev));  // the LSB matters
  CHECK(near(ev.value, 1.0 / 16383.0));
  CHECK(!feed(mt, {0xE0, 0, 0x40}, ev));
}

SQ_TEST("surface matcher: 14-bit CC pairs MSB and LSB") {
  SurfaceMap m = mustParse(
      "<Control id=\"a\" type=\"fader\"><MidiMessage status=\"0xB0\" address=\"1\" resolution=\"14\"/></Control>"
      "<Control id=\"b\" type=\"fader\"><MidiMessage status=\"0xB0\" address=\"2\" resolution=\"14\" lsb=\"90\"/></Control>");
  Matcher mt(m);
  ControlEvent ev;
  // Before any LSB was seen, the MSB alone produces a (coarse) event.
  REQUIRE(feed(mt, {0xB0, 1, 64}, ev));
  CHECK_EQ(ev.control, 0u);
  CHECK(near(ev.value, (64 << 7) / 16383.0));
  // The LSB completes the value.
  REQUIRE(feed(mt, {0xB0, 33, 5}, ev));
  CHECK(near(ev.value, ((64 << 7) | 5) / 16383.0));
  // From now on the MSB is held until its LSB arrives: one event per fine move.
  CHECK(!feed(mt, {0xB0, 1, 100}, ev));
  REQUIRE(feed(mt, {0xB0, 33, 0x7F}, ev));
  CHECK(near(ev.value, ((100 << 7) | 0x7F) / 16383.0));
  // Custom LSB controller number.
  REQUIRE(feed(mt, {0xB0, 2, 0x7F}, ev));
  CHECK_EQ(ev.control, 1u);
  REQUIRE(feed(mt, {0xB0, 90, 0x7F}, ev));
  CHECK_EQ(ev.control, 1u);
  CHECK(near(ev.value, 1.0));
  CHECK(!feed(mt, {0xB0, 34, 1}, ev));  // b's default LSB slot is not mapped
}

SQ_TEST("surface matcher: relative encoders, pressure, program change") {
  SurfaceMap m = mustParse(
      "<Control id=\"e2\" type=\"encoder\" step=\"0.01\"><MidiMessage status=\"0xB0\" address=\"20\"/></Control>"
      "<Control id=\"es\" type=\"encoder\" mode=\"sign-magnitude\" step=\"0.01\"><MidiMessage status=\"0xB0\" address=\"21\"/></Control>"
      "<Control id=\"eo\" type=\"encoder\" mode=\"offset-64\" step=\"0.01\"><MidiMessage status=\"0xB0\" address=\"22\"/></Control>"
      "<Control id=\"ea\" type=\"encoder\" mode=\"absolute\"><MidiMessage status=\"0xB0\" address=\"23\"/></Control>"
      "<Control id=\"cp\" type=\"knob\"><MidiMessage status=\"0xD0\"/></Control>"
      "<Control id=\"pa\" type=\"knob\"><MidiMessage status=\"0xA0\" address=\"60\"/></Control>"
      "<Control id=\"pc\" type=\"button\"><MidiMessage status=\"0xC0\" address=\"5\"/></Control>");
  Matcher mt(m);
  ControlEvent ev;
  REQUIRE(feed(mt, {0xB0, 20, 3}, ev));
  CHECK(ev.relative);
  CHECK(near(ev.value, 0.03));
  REQUIRE(feed(mt, {0xB0, 20, 127}, ev));  // -1
  CHECK(near(ev.value, -0.01));
  REQUIRE(feed(mt, {0xB0, 21, 0x42}, ev));  // sign bit + 2
  CHECK(near(ev.value, -0.02));
  REQUIRE(feed(mt, {0xB0, 21, 0x05}, ev));
  CHECK(near(ev.value, 0.05));
  REQUIRE(feed(mt, {0xB0, 22, 60}, ev));
  CHECK(near(ev.value, -0.04));
  CHECK(!feed(mt, {0xB0, 22, 64}, ev));  // zero ticks: nothing
  REQUIRE(feed(mt, {0xB0, 23, 127}, ev));
  CHECK(!ev.relative);
  CHECK(near(ev.value, 1.0));
  REQUIRE(feed(mt, {0xD0, 127}, ev));
  CHECK_EQ(ev.control, 4u);
  CHECK(near(ev.value, 1.0));
  REQUIRE(feed(mt, {0xA0, 60, 0}, ev));
  CHECK_EQ(ev.control, 5u);
  REQUIRE(feed(mt, {0xC0, 5}, ev));
  CHECK_EQ(ev.control, 6u);
  CHECK(near(ev.value, 1.0));
  CHECK(!feed(mt, {0xC0, 6}, ev));
}

SQ_TEST("surface feedback: value -> MIDI for each message kind, display sysex") {
  SurfaceMap m = mustParse(
      "<Control id=\"pb\" type=\"fader\" feedback=\"true\"><MidiMessage status=\"0xE1\"/></Control>"
      "<Control id=\"cc14\" type=\"fader\" feedback=\"true\"><MidiMessage status=\"0xB0\" address=\"7\" resolution=\"14\"/></Control>"
      "<Control id=\"cc\" type=\"encoder\" feedback=\"true\"><MidiMessage status=\"0xB0\" address=\"16\"/></Control>"
      "<Control id=\"btn\" type=\"button\" feedback=\"true\" on=\"65\" off=\"1\"><MidiMessage status=\"0x80\" address=\"40\"/></Control>"
      "<Control id=\"pc\" type=\"button\"><MidiMessage status=\"0xC0\" address=\"1\"/></Control>"
      "<Control id=\"lcd\" type=\"display\" length=\"6\"><Sysex header=\"F0 7D 01\"/></Control>");
  sq_midi_timed_event ev[2];
  REQUIRE_EQ(encodeFeedback(m.controls[0], 0.5, ev), 1u);
  CHECK_EQ(ev[0].bytes[0], 0xE1);
  CHECK_EQ(ev[0].size, 3u);
  CHECK_EQ(int(ev[0].bytes[1]) | (int(ev[0].bytes[2]) << 7), 8192);  // lround(0.5 * 16383)
  CHECK_EQ(ev[0].time_ns, 0u);

  REQUIRE_EQ(encodeFeedback(m.controls[1], 1.0, ev), 2u);
  CHECK_EQ(ev[0].bytes[1], 7);
  CHECK_EQ(ev[0].bytes[2], 0x7F);
  CHECK_EQ(ev[1].bytes[0], 0xB0);
  CHECK_EQ(ev[1].bytes[1], 39);
  CHECK_EQ(ev[1].bytes[2], 0x7F);

  REQUIRE_EQ(encodeFeedback(m.controls[2], 0.25, ev), 1u);
  CHECK_EQ(ev[0].bytes[2], 32);  // lround(31.75)

  REQUIRE_EQ(encodeFeedback(m.controls[3], 1.0, ev), 1u);
  CHECK_EQ(ev[0].bytes[0], 0x90);  // LEDs are driven with note-on
  CHECK_EQ(ev[0].bytes[2], 65);
  REQUIRE_EQ(encodeFeedback(m.controls[3], 0.2, ev), 1u);
  CHECK_EQ(ev[0].bytes[2], 1);

  CHECK_EQ(encodeFeedback(m.controls[4], 1.0, ev), 0u);  // program change has no feedback
  CHECK_EQ(encodeFeedback(m.controls[5], 1.0, ev), 0u);  // displays use encodeDisplay

  auto sx = encodeDisplay(m.controls[5], "Vol\xC3\xA9 long text");
  REQUIRE_EQ(sx.size(), size_t(3 + 6 + 1));
  CHECK_EQ(sx[0], 0xF0);
  CHECK_EQ(sx[3], 'V');
  CHECK_EQ(sx[6], '?');  // non-ASCII
  CHECK_EQ(sx[7], ' ');
  CHECK_EQ(sx[8], 'l');
  CHECK_EQ(sx[9], 0xF7);
  auto pad = encodeDisplay(m.controls[5], "ab");
  CHECK_EQ(pad[5], ' ');
  CHECK_EQ(pad[8], ' ');
}

SQ_TEST("surface hot-plug: port resolution and reconnect planning") {
  std::vector<PortRef> ports = {
      {"winmm:in:Desk Ctl#1", "Desk Ctl", SQ_MIDI_PORT_INPUT},
      {"winmm:out:Desk Ctl#1", "Desk Ctl", SQ_MIDI_PORT_OUTPUT},
      {"loopback:in:0", "Loopback 1 In", SQ_MIDI_PORT_INPUT},
  };
  CHECK(resolvePort(ports, SQ_MIDI_PORT_INPUT, "loopback:in:0", "") == "loopback:in:0");
  CHECK(resolvePort(ports, SQ_MIDI_PORT_OUTPUT, "loopback:in:0", "").empty());  // wrong direction
  CHECK(resolvePort(ports, SQ_MIDI_PORT_INPUT, "gone", "desk").empty());        // explicit id wins
  CHECK(resolvePort(ports, SQ_MIDI_PORT_INPUT, "", "desk ctl") == "winmm:in:Desk Ctl#1");
  CHECK(resolvePort(ports, SQ_MIDI_PORT_OUTPUT, "", "DESK") == "winmm:out:Desk Ctl#1");
  CHECK(resolvePort(ports, SQ_MIDI_PORT_INPUT, "", "").empty());

  CHECK(planLink("", "", "", "") == LinkChange::None);
  CHECK(planLink("", "", "in", "") == LinkChange::Connect);
  CHECK(planLink("", "", "", "out") == LinkChange::Connect);
  CHECK(planLink("in", "out", "in", "out") == LinkChange::None);
  CHECK(planLink("in", "out", "", "") == LinkChange::Disconnect);
  CHECK(planLink("in", "out", "in", "") == LinkChange::Reconnect);  // output unplugged
  CHECK(planLink("in", "", "in2", "") == LinkChange::Reconnect);
}

// Verifier regressions: inputs the validator used to accept although they could not work.
SQ_TEST("surface map: validator rejects dead buttons, unbounded names and overflowing ranges") {
  // A pitch-bend button never reads as pressed: reject instead of loading a dead control.
  CHECK(hasMessage(problems("<Control id=\"b\" type=\"button\"><MidiMessage status=\"0xE0\"/></Control>").errors, "pitch bend"));
  // The surface id comes from the map name and must fit sq_surface_info.id (else it is truncated
  // there and setControlValue() with that id can never find the surface).
  {
    std::string xml = "<ControlSurface name=\"" + std::string(300, 'n') +
                      "\"><Controls><Control id=\"k\" type=\"knob\"><MidiMessage status=\"0xB0\"/></Control></Controls></ControlSurface>";
    SurfaceMap m;
    Diagnostics d;
    REQUIRE_OK(parseSurfaceMap(xml, m, d));
    validateSurfaceMap(m, d);
    CHECK(hasMessage(d.errors, "name"));
  }
  // min/max finite but max - min overflows to infinity: target writes would be inf/NaN.
  const std::string knob = "<Control id=\"k\" type=\"knob\"><MidiMessage status=\"0xB0\" address=\"1\"/></Control>";
  CHECK(hasMessage(problems(knob, "<Bind control=\"k\" target=\"object://a\" min=\"-1e308\" max=\"1e308\"/>").errors, "range"));
}

// Malformed input never crashes: random short messages through a real map, random XML mutations.
SQ_TEST("surface robustness: random MIDI bytes and mutated XML") {
  DeviceInfo dev;
  SurfaceMap map;
  Diagnostics d;
  REQUIRE_OK(loadSurfaceFiles(sq::joinPath(SQ_HWS_MAPS_DIR, "generic8.device"), dev, map, d));
  Matcher mt(map);
  uint32_t seed = 12345;
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
  };
  ControlEvent ev;
  uint32_t hits = 0;
  for (int i = 0; i < 200000; ++i) {
    uint8_t b[3] = {uint8_t(rnd()), uint8_t(rnd()), uint8_t(rnd())};
    if (mt.match(b, rnd() % 4, ev)) {
      ++hits;
      REQUIRE(ev.control < map.controls.size());
      REQUIRE(std::isfinite(ev.value));
      REQUIRE(ev.value >= -1.0 && ev.value <= 1.0);
    }
  }
  CHECK(hits > 0);
  CHECK(!mt.match(nullptr, 3, ev));

  auto text = sq::readFile(sq::joinPath(SQ_HWS_MAPS_DIR, "generic8.surface.xml"));
  REQUIRE(text);
  const std::string base = *text;
  const char junk[] = "<>\"'&=/x0 \x80\xff";
  for (int i = 0; i < 3000; ++i) {
    std::string s = base;
    const int edits = 1 + int(rnd() % 8);
    for (int e = 0; e < edits && !s.empty(); ++e) {
      const size_t pos = rnd() % s.size();
      switch (rnd() % 3) {
        case 0: s[pos] = junk[rnd() % (sizeof junk - 1)]; break;
        case 1: s.erase(pos, 1 + rnd() % 16); break;
        default: s.insert(pos, 1, junk[rnd() % (sizeof junk - 1)]); break;
      }
    }
    SurfaceMap m;
    Diagnostics dd;
    if (parseSurfaceMap(s, m, dd) == SQ_OK) {
      validateSurfaceMap(m, dd);
      if (dd.ok()) {
        Matcher x(m);
        uint8_t b[3] = {0xB0, 16, 1};
        x.match(b, 3, ev);
      }
    }
  }
  CHECK(true);
}
