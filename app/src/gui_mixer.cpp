// Squizzy Studio GUI - mixer: one channel strip per track (buses after the tracks, master pinned
// on the right). Strip: color + name, instrument/input line, insert slots, sends, pan knob, fader
// with dB scale, stereo meter with peak hold, mute / solo / arm / monitor, output routing.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

#include "gui_panels.hpp"

namespace sq::app::gui {
namespace {

constexpr float kStripW = 96;
constexpr double kMinDb = -72;  // fader bottom = -inf

double faderToDb(double t) {
  if (t <= 0.0) return -144.0;
  // Upper 3/4 of the travel: -24..+6 dB linear in dB; lower quarter: -72..-24.
  if (t >= 0.25) return -24.0 + (t - 0.25) / 0.75 * 30.0;
  return kMinDb + t / 0.25 * (-24.0 - kMinDb);
}
double dbToFader(double db) {
  if (db <= kMinDb) return 0.0;
  if (db >= -24.0) return std::min(1.0, 0.25 + (db + 24.0) / 30.0 * 0.75);
  return (db - kMinDb) / (-24.0 - kMinDb) * 0.25;
}

std::vector<std::string> lines(const std::string& s) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t nl = s.find('\n', pos);
    std::string l = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? s.size() : nl + 1;
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (!l.empty()) out.push_back(l);
  }
  return out;
}

struct Strip {
  TrackView t;
  bool master = false;
  bool bus = false;
  std::vector<std::string> inserts;  // spec lines
  std::vector<std::string> sends;    // "<guid> <db> [pre]"
  std::string instrument;
  // hit rects (DIPs), filled by paint
  Rect box, name, fader, faderTrack, pan, mute, solo, arm, mon, out, peak, addInsert, addSend;
  std::vector<Rect> slots, sendKnobs;
};

enum class Part { None, Strip, Name, Fader, Pan, Mute, Solo, Arm, Mon, Out, Peak, Slot, AddInsert, Send, AddSend };

class Mixer final : public Widget {
 public:
  explicit Mixer(GuiContext& ctx) : Widget("Mixer"), ctx_(ctx) {}

  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.bg);
    refresh();
    c.pushClip(f);
    const sq_guid sel = ctx_.editState().track;
    const float masterX = f.right() - kStripW - 8;
    const float viewW = masterX - f.x - 12;
    const float total = static_cast<float>(strips_.size() - (haveMaster_ ? 1 : 0)) * (kStripW + 4);
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, total - viewW));
    float x = f.x + 6 - scroll_;
    c.pushClip(Rect{f.x, f.y, viewW + 8, f.h});
    for (auto& s : strips_) {
      if (s.master) continue;
      paintStrip(c, p, s, Rect{x, f.y + 6, kStripW, f.h - 12}, s.t.id == sel);
      x += kStripW + 4;
    }
    c.popClip();
    if (total > viewW) {  // scrollbar
      const float th = std::max(30.0f, viewW * viewW / total);
      const float tx = f.x + 6 + (viewW - th) * (scroll_ / std::max(1.0f, total - viewW));
      c.fillRoundRect(Rect{tx, f.bottom() - 5, th, 3}, 1.5f, p.faint);
    }
    c.fillRect(Rect{masterX - 6, f.y, 1, f.h}, p.border);
    for (auto& s : strips_)
      if (s.master) paintStrip(c, p, s, Rect{masterX, f.y + 6, kStripW, f.h - 12}, false);
    c.popClip();
  }

  bool onMouse(const WidgetMouse& e) override {
    Strip* s = nullptr;
    int idx = -1;
    const Part part = hit(e.x, e.y, &s, &idx);
    switch (e.action) {
      case MouseAction::Hover:
        hotPart_ = part;
        hotStrip_ = s ? s->t.id : sq_guid{};
        hotIdx_ = idx;
        return true;
      case MouseAction::Leave: hotPart_ = Part::None; return false;
      case MouseAction::Wheel:
        if (s && (part == Part::Fader || part == Part::Pan || part == Part::Send)) {
          if (part == Part::Fader) setVolume(*s, std::clamp(s->t.info.volume_db + (e.wheel > 0 ? 0.5 : -0.5), static_cast<double>(kMinDb), 6.0), true);
          else if (part == Part::Pan) ctx_.setProperty(s->t.id, "pan", std::clamp(s->t.info.pan + e.wheel * 0.05, -1.0, 1.0), true);
          else setSend(*s, idx, std::clamp(sendDb(*s, idx) + e.wheel * 0.5, -72.0, 6.0), true);
          return true;
        }
        scroll_ -= e.wheel * 60.0f;
        ctx_.invalidate();
        return true;
      case MouseAction::Down: {
        if (!s) return false;
        if (!s->master) ctx_.selectTrack(s->t.id);
        drag_ = part;
        dragStrip_ = s->t.id;
        dragIdx_ = idx;
        dragY_ = e.y;
        dragStart_ = part == Part::Pan ? s->t.info.pan : part == Part::Send ? sendDb(*s, idx) : dbToFader(s->t.info.volume_db);
        if (e.button == MouseButton::Right) {
          drag_ = Part::None;
          contextMenu(*s, part, idx, Point{e.x, e.y});
          return true;
        }
        if (part == Part::Fader && !s->faderTrack.contains(e.x, e.y)) {
          // click on the fader track jumps there
        }
        return true;
      }
      case MouseAction::Move: {
        if (drag_ == Part::None) return true;
        Strip* ds = find(dragStrip_);
        if (!ds) return true;
        const double fine = (e.modifiers & kModShift) ? 0.1 : 1.0;
        if (drag_ == Part::Fader) {
          const double t = std::clamp(dragStart_ + static_cast<double>(dragY_ - e.y) / static_cast<double>(std::max(40.0f, ds->faderTrack.h)) * fine, 0.0, 1.0);
          setVolume(*ds, std::max(-144.0, faderToDb(t)), true);
        } else if (drag_ == Part::Pan) {
          ctx_.setProperty(ds->t.id, "pan", std::clamp(dragStart_ + static_cast<double>(dragY_ - e.y) / 120.0 * fine, -1.0, 1.0), true);
        } else if (drag_ == Part::Send) {
          setSend(*ds, dragIdx_, std::clamp(dragStart_ + static_cast<double>(dragY_ - e.y) * 0.3 * fine, -72.0, 6.0), true);
        }
        return true;
      }
      case MouseAction::Up: {
        const Part was = drag_;
        drag_ = Part::None;
        Strip* ds = find(dragStrip_);
        if (!ds || part != was || std::fabs(e.y - dragY_) > 3) return true;
        click(*ds, was, dragIdx_, Point{e.x, e.y});
        return true;
      }
      case MouseAction::DoubleClick:
        if (!s) return false;
        if (part == Part::Fader) setVolume(*s, 0.0, false);
        else if (part == Part::Pan) ctx_.setProperty(s->t.id, "pan", 0.0);
        else if (part == Part::Send) setSend(*s, idx, 0.0, false);
        else if (part == Part::Name && !s->master) rename(*s);
        return true;
      default: return false;
    }
  }

 private:
  void refresh() {
    const uint64_t serial = ctx_.docSerial();
    if (serial == serial_) {
      // values may change without a serial bump (merge drags notify too, but be safe): reread info
      for (auto& s : strips_) {
        s.t.info.struct_size = sizeof s.t.info;
        if (s.t.track) s.t.track->getTrackInfo(&s.t.info);
      }
      return;
    }
    serial_ = serial;
    strips_.clear();
    haveMaster_ = false;
    std::vector<Strip> buses;
    for (auto& t : ctx_.tracks()) {
      Strip s;
      s.t = t;
      if (t.info.kind == SQ_TRACK_FOLDER) continue;
      s.master = t.info.kind == SQ_TRACK_MASTER;
      s.bus = t.info.kind == SQ_TRACK_BUS || t.info.kind == SQ_TRACK_FX;
      auto props = sq::makeAttributes();
      if (t.track->getProperties(props.get()) == SQ_OK) {
        s.inserts = lines(sq::getAttrString(props.get(), "user.inserts"));
        s.sends = lines(sq::getAttrString(props.get(), "user.sends"));
        s.instrument = sq::getAttrString(props.get(), "user.instrument");
      }
      if (s.master) haveMaster_ = true;
      if (s.bus) buses.push_back(std::move(s));
      else strips_.push_back(std::move(s));
    }
    // buses after the tracks, master last
    std::stable_partition(strips_.begin(), strips_.end(), [](const Strip& s) { return !s.master; });
    auto masterIt = std::find_if(strips_.begin(), strips_.end(), [](const Strip& s) { return s.master; });
    strips_.insert(masterIt, std::make_move_iterator(buses.begin()), std::make_move_iterator(buses.end()));
  }

  Strip* find(const sq_guid& id) {
    for (auto& s : strips_)
      if (s.t.id == id) return &s;
    return nullptr;
  }

  static std::string deviceName(const std::string& spec) {
    std::string s = spec;
    if (!s.empty() && s[0] == '!') s = s.substr(1);
    const size_t a = s.find('|');
    if (a == std::string::npos) return s;
    const size_t b = s.find('|', a + 1);
    std::string id = s.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    if (!id.empty()) id[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(id[0])));
    if (s.compare(0, 8, "builtin|") != 0) {
      // plugin: the id is often reverse-dns; keep the last component
      const size_t dot = id.find_last_of(".:/");
      if (dot != std::string::npos && dot + 1 < id.size()) id = id.substr(dot + 1);
    }
    return id;
  }

  std::string busName(const std::string& guidStr) {
    sq_guid g{};
    if (!sq::parseGuid(guidStr, g)) return "?";
    for (auto& s : strips_)
      if (s.t.id == g) return s.t.name;
    return "?";
  }

  static double sendLevel(const std::string& line) {
    const size_t sp = line.find(' ');
    return sp == std::string::npos ? 0.0 : std::atof(line.c_str() + sp + 1);
  }
  double sendDb(const Strip& s, int idx) const {
    if (idx < 0 || idx >= static_cast<int>(s.sends.size())) return 0.0;
    return sendLevel(s.sends[static_cast<size_t>(idx)]);
  }

  void paintStrip(Canvas& c, const Palette& p, Strip& s, const Rect& r, bool selected) {
    s.box = r;
    const Color col = s.master ? p.accent : ctx_.trackColor(s.t);
    c.fillRoundRect(r, 5, selected ? draw::mix(p.panel2, col, 0.12f) : p.panel2);
    if (selected) c.strokeRoundRect(r, 5, col.withAlpha(0.7f), 1);
    c.fillRoundRect(Rect{r.x, r.y, r.w, 4}, 2, col);
    float y = r.y + 8;
    s.name = Rect{r.x + 4, y, r.w - 8, 18};
    c.drawTextBold(s.master ? "Master" : s.t.name, s.name, p.text, 11.5f, TextAlign::Center);
    y += 20;
    // source line
    std::string src;
    if (s.master) src = "Main Out";
    else if (s.bus) src = "Bus";
    else if (s.t.info.kind == SQ_TRACK_INSTRUMENT) src = s.instrument.empty() ? "Synth" : deviceName(s.instrument);
    else src = "Audio In";
    c.drawText(src, Rect{r.x + 4, y, r.w - 8, 14}, s.t.info.kind == SQ_TRACK_INSTRUMENT ? p.accent2 : p.dim, 10, TextAlign::Center);
    y += 18;
    // inserts
    s.slots.clear();
    const float slotH = 17;
    const size_t maxSlots = static_cast<size_t>(std::clamp((r.h - 330.0f) / (slotH + 2), 1.0f, 8.0f));
    for (size_t i = 0; i < s.inserts.size() && i < maxSlots; ++i) {
      Rect sr{r.x + 5, y, r.w - 10, slotH};
      const bool bypass = !s.inserts[i].empty() && s.inserts[i][0] == '!';
      const bool hot = hotPart_ == Part::Slot && hotStrip_ == s.t.id && hotIdx_ == static_cast<int>(i);
      c.fillRoundRect(sr, 3, hot ? p.panel3 : p.bg);
      c.fillRect(Rect{sr.x + 3, sr.y + 5, 6, 6}, bypass ? p.faint : p.meterLow);
      c.drawText(deviceName(s.inserts[i]), Rect{sr.x + 13, sr.y, sr.w - 15, sr.h}, bypass ? p.faint : p.text, 10.5f);
      s.slots.push_back(sr);
      y += slotH + 2;
    }
    s.addInsert = Rect{r.x + 5, y, r.w - 10, slotH};
    const bool hotAdd = hotPart_ == Part::AddInsert && hotStrip_ == s.t.id;
    c.strokeRoundRect(s.addInsert, 3, hotAdd ? p.accent : p.panel3, 1);
    c.drawText("+ FX", s.addInsert, hotAdd ? p.text : p.faint, 10.5f, TextAlign::Center);
    y += slotH + 6;
    // pan (left) + sends (right) in one row
    s.pan = Rect{r.x + 6, y, 28, 28};
    draw::knob(c, p, s.pan, (s.t.info.pan + 1.0) / 2.0, true, col);
    char buf[32];
    if (std::fabs(s.t.info.pan) < 0.005) std::snprintf(buf, sizeof buf, "C");
    else std::snprintf(buf, sizeof buf, "%s%d", s.t.info.pan < 0 ? "L" : "R", static_cast<int>(std::lround(std::fabs(s.t.info.pan) * 100)));
    c.drawText(buf, Rect{s.pan.x - 4, y + 28, 36, 11}, p.faint, 9, TextAlign::Center);
    s.sendKnobs.clear();
    s.addSend = Rect{};
    if (!s.master) {
      float sx = r.x + 40;
      for (size_t i = 0; i < s.sends.size() && i < 2; ++i) {
        Rect kr{sx, y + 3, 22, 22};
        draw::knob(c, p, kr, dbToFader(sendLevel(s.sends[i])), false, p.solo);
        s.sendKnobs.push_back(kr);
        sx += 24;
      }
      if (s.sends.size() < 2) {
        s.addSend = Rect{sx, y + 6, 22, 16};
        const bool hs = hotPart_ == Part::AddSend && hotStrip_ == s.t.id;
        c.strokeRoundRect(s.addSend, 3, hs ? p.accent : p.panel3, 1);
        c.drawText("S+", s.addSend, hs ? p.text : p.faint, 9, TextAlign::Center);
      }
    }
    y += 40;
    // buttons
    const float bw = (r.w - 12 - 6) / 4;
    auto btn = [&](Rect& out, float bx, const char* label, bool on, Color onCol, Part part) {
      out = Rect{bx, r.bottom() - 26, bw, 20};
      const bool hot = hotPart_ == part && hotStrip_ == s.t.id;
      c.fillRoundRect(out, 3, on ? onCol : hot ? p.panel3 : p.bg);
      c.drawText(label, out, on ? Color::rgb(0x111214) : p.dim, 10.5f, TextAlign::Center);
    };
    const uint32_t fl = s.t.info.flags;
    float bx = r.x + 6;
    btn(s.mute, bx, "M", (fl & SQ_TRACK_MUTE) != 0, p.mute, Part::Mute);
    bx += bw + 2;
    if (!s.master) {
      btn(s.solo, bx, "S", (fl & SQ_TRACK_SOLO) != 0, p.solo, Part::Solo);
      bx += bw + 2;
      if (!s.bus) {
        btn(s.arm, bx, "R", (fl & SQ_TRACK_ARMED) != 0, p.rec, Part::Arm);
        bx += bw + 2;
        btn(s.mon, bx, "I", (fl & SQ_TRACK_MONITOR) != 0, p.accent, Part::Mon);
      } else {
        s.arm = s.mon = Rect{};
      }
    } else {
      s.solo = s.arm = s.mon = Rect{};
    }
    // output selector
    if (!s.master) {
      s.out = Rect{r.x + 5, r.bottom() - 48, r.w - 10, 18};
      std::string outName = "Master";
      if (!sq_guid_is_null(&s.t.info.output))
        for (auto& o : strips_)
          if (o.t.id == s.t.info.output) outName = o.t.name;
      const bool ho = hotPart_ == Part::Out && hotStrip_ == s.t.id;
      c.fillRoundRect(s.out, 3, ho ? p.panel3 : p.bg);
      c.drawText("\xE2\x86\x92 " + outName, Rect{s.out.x + 5, s.out.y, s.out.w - 8, s.out.h}, p.dim, 10);
    } else {
      s.out = Rect{};
    }
    // fader + meter
    const float fy = y, fh = (s.master ? r.bottom() - 30 : s.out.y - 4) - fy - 16;
    s.peak = Rect{r.x + 4, fy, r.w - 8, 14};
    float pk[2] = {0, 0};
    const uint32_t n = ctx_.meter(s.master ? sq_guid{} : s.t.id, pk, nullptr, 2);
    if (n == 1) pk[1] = pk[0];
    auto& m = meters_[s.master ? sq_guid{} : s.t.id];
    const auto now = std::chrono::steady_clock::now();
    const float dt = static_cast<float>(std::chrono::duration<double>(now - m.last).count());
    m.last = now;
    for (int ch = 0; ch < 2; ++ch) {
      m.level[ch] = std::max(pk[ch], m.level[ch] * std::pow(0.5f, dt / 0.2f));
      if (pk[ch] > m.hold[ch] || now > m.holdUntil[ch]) {
        m.hold[ch] = pk[ch];
        m.holdUntil[ch] = now + std::chrono::milliseconds(1500);
      }
      m.peakMax = std::max(m.peakMax, pk[ch]);
    }
    if (m.peakMax > 0) std::snprintf(buf, sizeof buf, "%.1f", 20.0f * std::log10(std::max(m.peakMax, 1e-6f)));
    else std::snprintf(buf, sizeof buf, "-inf");
    c.drawText(buf, s.peak, m.peakMax >= 1.0f ? p.meterHigh : p.faint, 10, TextAlign::Center);
    const float top = fy + 16;
    s.fader = Rect{r.x + 4, top, r.w / 2 + 4, fh};
    s.faderTrack = Rect{r.x + 18, top + 6, 4, fh - 12};
    // dB scale
    for (double db : {6.0, 0.0, -6.0, -12.0, -24.0, -48.0}) {
      const float yy = s.faderTrack.y + s.faderTrack.h * (1.0f - static_cast<float>(dbToFader(db)));
      c.fillRect(Rect{s.faderTrack.right() + 12, yy, 3, 1}, p.faint);
      std::snprintf(buf, sizeof buf, "%d", static_cast<int>(db));
      c.drawText(buf, Rect{s.faderTrack.right() + 16, yy - 6, 26, 12}, p.faint, 8.5f, TextAlign::Center);
    }
    c.fillRoundRect(s.faderTrack, 2, p.bg);
    const float ky = s.faderTrack.y + s.faderTrack.h * (1.0f - static_cast<float>(dbToFader(s.t.info.volume_db)));
    c.fillRoundRect(Rect{s.faderTrack.x, ky, s.faderTrack.w, s.faderTrack.bottom() - ky}, 2, col.withAlpha(0.6f));
    const bool hf = (hotPart_ == Part::Fader && hotStrip_ == s.t.id) || (drag_ == Part::Fader && dragStrip_ == s.t.id);
    c.fillRoundRect(Rect{s.faderTrack.x - 9, ky - 7, s.faderTrack.w + 18, 14}, 3, hf ? p.text : draw::lighten(p.panel3, 0.25f));
    c.fillRect(Rect{s.faderTrack.x - 7, ky - 0.5f, s.faderTrack.w + 14, 1}, p.bg);
    // stereo meter
    const Rect mr{r.right() - 22, top + 6, 6, fh - 12};
    draw::meter(c, p, mr, m.level[0], m.hold[0]);
    draw::meter(c, p, Rect{mr.right() + 2, mr.y, 6, mr.h}, m.level[1], m.hold[1]);
    // volume readout
    if (s.t.info.volume_db <= -144.0) std::snprintf(buf, sizeof buf, "-inf");
    else std::snprintf(buf, sizeof buf, "%+.1f", s.t.info.volume_db);
    c.drawText(buf, Rect{r.x, top + fh - 2, r.w, 14}, p.dim, 10, TextAlign::Center);
  }

  Part hit(float x, float y, Strip** out, int* idx) {
    *out = nullptr;
    *idx = -1;
    for (auto& s : strips_) {
      if (!s.box.contains(x, y)) continue;
      *out = &s;
      for (size_t i = 0; i < s.slots.size(); ++i)
        if (s.slots[i].contains(x, y)) {
          *idx = static_cast<int>(i);
          return Part::Slot;
        }
      for (size_t i = 0; i < s.sendKnobs.size(); ++i)
        if (s.sendKnobs[i].contains(x, y)) {
          *idx = static_cast<int>(i);
          return Part::Send;
        }
      if (s.addInsert.contains(x, y)) return Part::AddInsert;
      if (s.addSend.contains(x, y)) return Part::AddSend;
      if (s.pan.contains(x, y)) return Part::Pan;
      if (s.mute.contains(x, y)) return Part::Mute;
      if (s.solo.contains(x, y)) return Part::Solo;
      if (s.arm.contains(x, y)) return Part::Arm;
      if (s.mon.contains(x, y)) return Part::Mon;
      if (s.out.contains(x, y)) return Part::Out;
      if (s.peak.contains(x, y)) return Part::Peak;
      if (s.fader.contains(x, y)) return Part::Fader;
      if (s.name.contains(x, y)) return Part::Name;
      return Part::Strip;
    }
    return Part::None;
  }

  void setVolume(Strip& s, double db, bool merge) {
    ctx_.setProperty(s.t.id, "volumeDb", db <= kMinDb ? -144.0 : std::round(db * 10.0) / 10.0, merge);
  }

  void setSend(Strip& s, int idx, double db, bool merge) {
    if (idx < 0 || idx >= static_cast<int>(s.sends.size())) return;
    auto a = sq::makeAttributes();
    a->setGuid("track", &s.t.id);
    a->setInt("send", idx);
    a->setDouble("level", db);
    if (merge) a->setBool("merge", SQ_TRUE);
    if (SQ_SUCCEEDED(ctx_.shell().execute("Track.SetSendLevel", a.get()))) {
      ctx_.invalidate();
      return;
    }
    // Fallback: rewrite the property line.
    std::string line = s.sends[static_cast<size_t>(idx)];
    const size_t sp = line.find(' ');
    const std::string guid = line.substr(0, sp);
    std::string rest;
    if (sp != std::string::npos) {
      const size_t sp2 = line.find(' ', sp + 1);
      if (sp2 != std::string::npos) rest = line.substr(sp2);
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, " %.2f", db);
    s.sends[static_cast<size_t>(idx)] = guid + buf + rest;
    std::string all;
    for (auto& l : s.sends) all += l + "\n";
    ctx_.setPropertyString(s.t.id, "user.sends", all, merge);
  }

  void rename(Strip& s) {
    const sq_guid id = s.t.id;
    ctx_.prompt("Rename Track", s.t.name, [this, id](const std::string& nm) {
      if (!nm.empty()) ctx_.setPropertyString(id, "name", nm);
    });
  }

  void click(Strip& s, Part part, int idx, Point at) {
    const uint32_t fl = s.t.info.flags;
    switch (part) {
      case Part::Mute: ctx_.setPropertyBool(s.t.id, "mute", !(fl & SQ_TRACK_MUTE)); break;
      case Part::Solo: ctx_.setPropertyBool(s.t.id, "solo", !(fl & SQ_TRACK_SOLO)); break;
      case Part::Arm: ctx_.setPropertyBool(s.t.id, "armed", !(fl & SQ_TRACK_ARMED)); break;
      case Part::Mon: ctx_.setPropertyBool(s.t.id, "monitor", !(fl & SQ_TRACK_MONITOR)); break;
      case Part::Peak: meters_[s.master ? sq_guid{} : s.t.id].peakMax = 0; break;
      case Part::Slot:
        ctx_.selectTrack(s.t.id);
        ctx_.showBottomPanel(BottomPanel::Devices);
        (void)idx;
        break;
      case Part::AddInsert: ctx_.showMenu(buildInsertMenu(ctx_, s.t.id), Point{at.x, at.y}); break;
      case Part::AddSend: sendMenu(s, at); break;
      case Part::Out: outputMenu(s, at); break;
      default: break;
    }
  }

  void outputMenu(Strip& s, Point at) {
    std::vector<MenuItem> m;
    const sq_guid id = s.t.id;
    MenuItem master;
    master.label = "Master";
    master.checked = sq_guid_is_null(&s.t.info.output);
    master.action = [this, id] {
      auto a = sq::makeAttributes();
      sq_guid none{};
      a->setGuid("node", &id);
      a->setString("key", "output");
      a->setGuid("value", &none);
      ctx_.exec(sq::doc::kSetProperty, a.get());
    };
    m.push_back(master);
    for (auto& o : strips_) {
      if (!o.bus || o.t.id == id) continue;
      MenuItem it;
      it.label = o.t.name;
      it.checked = o.t.id == s.t.info.output;
      const sq_guid bus = o.t.id;
      it.action = [this, id, bus] {
        auto a = sq::makeAttributes();
        a->setGuid("node", &id);
        a->setString("key", "output");
        a->setGuid("value", &bus);
        ctx_.exec(sq::doc::kSetProperty, a.get());
      };
      m.push_back(it);
    }
    m.push_back(MenuItem::sep());
    MenuItem nb;
    nb.label = "New Bus...";
    nb.action = [this] { ctx_.run("Track.AddBus"); };
    m.push_back(nb);
    ctx_.showMenu(std::move(m), at);
  }

  void sendMenu(Strip& s, Point at) {
    std::vector<MenuItem> m;
    const sq_guid id = s.t.id;
    for (auto& o : strips_) {
      if (!o.bus || o.t.id == id) continue;
      MenuItem it;
      it.label = "Send to " + o.t.name;
      const sq_guid bus = o.t.id;
      it.action = [this, id, bus] {
        auto a = sq::makeAttributes();
        a->setGuid("track", &id);
        a->setGuid("bus", &bus);
        a->setDouble("level", -6.0);
        if (SQ_FAILED(ctx_.shell().execute("Track.AddSend", a.get()))) {
          std::string cur = ctx_.trackString(id, "user.sends");
          if (!cur.empty() && cur.back() != '\n') cur += '\n';
          cur += sq::toString(bus) + " -6\n";
          ctx_.setPropertyString(id, "user.sends", cur);
        }
        ctx_.invalidate();
      };
      m.push_back(it);
    }
    if (m.empty()) {
      MenuItem nb;
      nb.label = "No buses yet - create one";
      nb.action = [this] { ctx_.run("Track.AddBus"); };
      m.push_back(nb);
    }
    ctx_.showMenu(std::move(m), at);
  }

  void contextMenu(Strip& s, Part part, int idx, Point at) {
    std::vector<MenuItem> m;
    const sq_guid id = s.t.id;
    if (part == Part::Slot && idx >= 0) {
      MenuItem bypass;
      const bool by = idx < static_cast<int>(s.inserts.size()) && !s.inserts[static_cast<size_t>(idx)].empty() &&
                      s.inserts[static_cast<size_t>(idx)][0] == '!';
      bypass.label = by ? "Enable" : "Bypass";
      bypass.action = [this, id, idx, by] {
        auto a = sq::makeAttributes();
        a->setGuid("track", &id);
        a->setInt("slot", idx);
        a->setBool("bypass", by ? SQ_FALSE : SQ_TRUE);
        ctx_.run("Track.BypassInsert", a.get());
      };
      m.push_back(bypass);
      MenuItem rm;
      rm.label = "Remove";
      rm.action = [this, id, idx] {
        auto a = sq::makeAttributes();
        a->setGuid("track", &id);
        a->setInt("slot", idx);
        ctx_.run("Track.RemoveInsert", a.get());
      };
      m.push_back(rm);
      MenuItem ed;
      ed.label = "Show Device";
      ed.action = [this, id] {
        ctx_.selectTrack(id);
        ctx_.showBottomPanel(BottomPanel::Devices);
      };
      m.push_back(ed);
    } else if (part == Part::Send && idx >= 0) {
      MenuItem rm;
      rm.label = "Remove Send";
      rm.action = [this, id, idx] {
        auto a = sq::makeAttributes();
        a->setGuid("track", &id);
        a->setInt("send", idx);
        ctx_.run("Track.RemoveSend", a.get());
      };
      m.push_back(rm);
    } else if (!s.master) {
      MenuItem rn;
      rn.label = "Rename...";
      Strip* sp = &s;
      rn.action = [this, sp] { rename(*sp); };
      m.push_back(rn);
      MenuItem col;
      col.label = "Color";
      for (size_t i = 0; i < ctx_.palette().trackColors.size(); ++i) {
        MenuItem ci;
        static const char* kNames[] = {"Teal", "Violet", "Amber", "Blue", "Coral", "Sage", "Pink", "Olive"};
        ci.label = i < 8 ? kNames[i] : "Color";
        const Color cc = ctx_.palette().trackColors[i];
        ci.action = [this, id, cc] {
          auto a = sq::makeAttributes();
          a->setGuid("node", &id);
          a->setString("key", "color");
          a->setInt("value", static_cast<int64_t>(cc.toRgba()));
          ctx_.exec(sq::doc::kSetProperty, a.get());
        };
        col.submenu.push_back(ci);
      }
      m.push_back(col);
      MenuItem reset;
      reset.label = "Reset Fader";
      reset.action = [this, id] { ctx_.setProperty(id, "volumeDb", 0.0); };
      m.push_back(reset);
      m.push_back(MenuItem::sep());
      MenuItem del;
      del.label = "Delete Track";
      del.action = [this, id] {
        auto a = sq::makeAttributes();
        a->setGuid("node", &id);
        ctx_.exec(sq::doc::kRemoveNode, a.get());
      };
      m.push_back(del);
    }
    if (!m.empty()) ctx_.showMenu(std::move(m), at);
  }

  struct MeterState {
    float level[2] = {0, 0}, hold[2] = {0, 0};
    float peakMax = 0;
    std::chrono::steady_clock::time_point holdUntil[2]{};
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  };

  GuiContext& ctx_;
  std::vector<Strip> strips_;
  bool haveMaster_ = false;
  uint64_t serial_ = 0;
  float scroll_ = 0;
  Part hotPart_ = Part::None, drag_ = Part::None;
  sq_guid hotStrip_{}, dragStrip_{};
  int hotIdx_ = -1, dragIdx_ = -1;
  float dragY_ = 0;
  double dragStart_ = 0;
  std::map<sq_guid, MeterState> meters_;
};

}  // namespace

std::unique_ptr<Widget> makeMixer(GuiContext& ctx) { return std::make_unique<Mixer>(ctx); }

}  // namespace sq::app::gui
