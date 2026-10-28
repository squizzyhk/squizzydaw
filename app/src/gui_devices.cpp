// Squizzy Studio GUI - device rack: the selected track's instrument and insert chain as cards with
// one control per parameter (knobs for continuous values, buttons for toggles, pop-up menus for
// choices). Edits rewrite the track's spec strings ("user.instrument" / "user.inserts", grammar in
// engine/audio/include/sq/engine/audio/builtin.hpp); each drag gesture is one undo step and
// the engine applies parameter-only changes without rebuilding its graph.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

#include "gui_panels.hpp"
#include "sq/engine/audio/builtin.hpp"

namespace sq::app::gui {
namespace {

namespace ea = sq::engine::audio;

constexpr float kCell = 62;     // knob cell width
constexpr float kRowH = 74;     // knob cell height
constexpr float kHeader = 30;

std::vector<std::string> splitLines(const std::string& s) {
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

double toNorm(const ea::ParamInfo& p, double v) {
  if (p.max <= p.min) return 0.0;
  if (p.logScale && p.min > 0) return std::log(v / p.min) / std::log(static_cast<double>(p.max) / p.min);
  return (v - p.min) / (p.max - p.min);
}
double fromNorm(const ea::ParamInfo& p, double n) {
  n = std::clamp(n, 0.0, 1.0);
  double v = p.logScale && p.min > 0 ? p.min * std::pow(static_cast<double>(p.max) / p.min, n) : p.min + n * (p.max - p.min);
  if (p.kind == ea::ParamKind::Integer || p.kind == ea::ParamKind::Choice || p.kind == ea::ParamKind::Toggle) v = std::round(v);
  return v;
}
std::string formatValue(const ea::ParamInfo& p, double v) {
  char buf[64];
  if (p.kind == ea::ParamKind::Choice) {
    const auto i = static_cast<size_t>(std::clamp(std::lround(v), 0L, static_cast<long>(p.choices.size()) - 1));
    return i < p.choices.size() ? p.choices[i] : std::to_string(static_cast<int>(v));
  }
  if (p.kind == ea::ParamKind::Toggle) return v >= 0.5 ? "On" : "Off";
  if (p.kind == ea::ParamKind::Integer) {
    std::snprintf(buf, sizeof buf, "%d%s%s", static_cast<int>(std::lround(v)), p.unit.empty() ? "" : " ", p.unit.c_str());
    return buf;
  }
  if (p.unit == "Hz" && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000.0);
  else if (p.unit == "ms" && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f s", v / 1000.0);
  else if (std::fabs(v) >= 100) std::snprintf(buf, sizeof buf, "%.0f %s", v, p.unit.c_str());
  else if (std::fabs(v) >= 10) std::snprintf(buf, sizeof buf, "%.1f %s", v, p.unit.c_str());
  else std::snprintf(buf, sizeof buf, "%.2f %s", v, p.unit.c_str());
  std::string s = buf;
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

struct Card {
  bool instrument = false;
  int slot = -1;           // insert index (-1 = instrument)
  bool bypass = false;
  std::string line;        // original spec line (with "!" when bypassed)
  ea::SpecParts spec;
  const ea::DeviceInfo* info = nullptr;
  std::vector<double> values;
  // layout (DIPs)
  Rect box, header, power, remove, left, right, menu;
  std::vector<Rect> controls;
};

class DeviceRack final : public Widget {
 public:
  explicit DeviceRack(GuiContext& ctx) : Widget("DeviceRack"), ctx_(ctx) {}

  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.bg);
    refresh();
    c.pushClip(f);
    if (!haveTrack_) {
      c.drawText("Select a track to see its instrument and effects.", Rect{f.x + 20, f.y + 20, f.w - 40, 20}, p.dim, 13);
      c.popClip();
      return;
    }
    // track label strip on the left
    const Rect label{f.x, f.y, 36, f.h};
    c.fillRect(label, p.panel);
    c.fillRect(Rect{label.x, label.y, 4, label.h}, trackColor_);
    c.drawTextBold(trackName_, Rect{f.x + 44, f.y + 6, 300, 18}, p.text, 12.5f);
    float x = f.x + 44 - scroll_;
    const float top = f.y + 30, h = f.h - 38;
    // cards scroll under the label strip: clip them to the content area right of it
    c.pushClip(contentRect());
    for (auto& card : cards_) {
      x = paintCard(c, p, card, x, top, h) + 10;
      if (&card != &cards_.back() || true) {
        // chain arrow
        draw::triangle(c, Rect{x - 8, top + h / 2 - 4, 5, 8}, 1, p.faint);
      }
    }
    addBtn_ = Rect{x, top, 96, h};
    c.strokeRoundRect(addBtn_, 6, hotAdd_ ? p.accent : p.panel3, 1);
    c.drawText("+ Add Effect", addBtn_, hotAdd_ ? p.text : p.faint, 12, TextAlign::Center);
    contentW_ = x + 96 + scroll_ - (f.x + 44);
    c.popClip();
    const float viewW = f.w - 52;
    if (contentW_ > viewW) {
      const float th = std::max(30.0f, viewW * viewW / contentW_);
      const float tx = f.x + 44 + (viewW - th) * (scroll_ / std::max(1.0f, contentW_ - viewW));
      c.fillRoundRect(Rect{tx, f.bottom() - 5, th, 3}, 1.5f, p.faint);
    }
    // value tooltip while dragging / hovering
    if (Card* cd = cardAt(tipCard_); cd && tipParam_ >= 0 && cd->info && tipParam_ < static_cast<int>(cd->info->params.size())) {
      const auto& pi = cd->info->params[static_cast<size_t>(tipParam_)];
      const std::string t = pi.name + ": " + formatValue(pi, cd->values[static_cast<size_t>(tipParam_)]);
      const Rect cr = cd->controls[static_cast<size_t>(tipParam_)];
      const float w = c.measureText(t, 11.5f) + 16;
      Rect tr{std::min(cr.x, f.right() - w - 4), cr.y - 22, w, 20};
      c.fillRoundRect(tr, 4, p.panel3);
      c.drawText(t, tr, p.text, 11.5f, TextAlign::Center);
    }
    c.popClip();
  }

  bool onMouse(const WidgetMouse& e) override {
    int ci = -1, pi = -1;
    const int part = hit(e.x, e.y, &ci, &pi);
    switch (e.action) {
      case MouseAction::Hover:
        hotAdd_ = addBtn_.contains(e.x, e.y) && contentRect().contains(e.x, e.y);
        tipCard_ = pi >= 0 ? ci : -1;
        tipParam_ = pi;
        hotCard_ = ci;
        hotPart_ = part;
        return true;
      case MouseAction::Leave:
        wheelCard_ = wheelParam_ = -1;
        tipCard_ = tipParam_ = -1;
        hotAdd_ = false;
        return false;
      case MouseAction::Wheel:
        if (pi >= 0 && ci >= 0) {
          Card& cd = cards_[static_cast<size_t>(ci)];
          const auto& info = cd.info->params[static_cast<size_t>(pi)];
          const double step = (info.kind == ea::ParamKind::Continuous) ? 0.02 : 1.0 / std::max(1.0, static_cast<double>(info.max - info.min));
          // consecutive wheel steps on one control are one undo step; anything else starts a new one
          if (wheelCard_ != ci || wheelParam_ != pi) ++gesture_;
          if (setParam(cd, pi, fromNorm(info, toNorm(info, cd.values[static_cast<size_t>(pi)]) + (e.wheel > 0 ? step : -step)), true)) {
            wheelCard_ = ci;
            wheelParam_ = pi;
          }
          return true;
        }
        scroll_ = std::clamp(scroll_ - e.wheel * 60.0f, 0.0f, std::max(0.0f, contentW_ - (frame().w - 52)));
        return true;
      case MouseAction::Down:
        wheelCard_ = wheelParam_ = -1;
        if (addBtn_.contains(e.x, e.y) && contentRect().contains(e.x, e.y)) {
          ctx_.showMenu(buildInsertMenu(ctx_, track_), Point{e.x, e.y});
          return true;
        }
        if (ci < 0) return false;
        if (e.button == MouseButton::Right) {
          cardMenu(ci, Point{e.x, e.y});
          return true;
        }
        dragCard_ = ci;
        dragParam_ = pi;
        dragY_ = e.y;
        dragMoved_ = false;
        ++gesture_;  // every press starts a new undo step
        if (pi >= 0) {
          Card& cd = cards_[static_cast<size_t>(ci)];
          dragNorm_ = toNorm(cd.info->params[static_cast<size_t>(pi)], cd.values[static_cast<size_t>(pi)]);
        }
        return true;
      case MouseAction::Move:
        if (dragCard_ >= 0 && dragParam_ >= 0 && dragCard_ < static_cast<int>(cards_.size())) {
          Card& cd = cards_[static_cast<size_t>(dragCard_)];
          const auto& info = cd.info->params[static_cast<size_t>(dragParam_)];
          if (info.kind != ea::ParamKind::Continuous && info.kind != ea::ParamKind::Integer) return true;
          if (std::fabs(e.y - dragY_) > 2) dragMoved_ = true;
          const double per = (e.modifiers & kModShift) ? 1.0 / 1500.0 : 1.0 / 180.0;
          setParam(cd, dragParam_, fromNorm(info, dragNorm_ + static_cast<double>(dragY_ - e.y) * per), true);
          tipCard_ = dragCard_;
          tipParam_ = dragParam_;
        }
        return true;
      case MouseAction::Up: {
        const int dc = dragCard_, dp = dragParam_;
        dragCard_ = dragParam_ = -1;
        if (dc < 0 || dc >= static_cast<int>(cards_.size()) || dc != ci) return true;
        Card& cd = cards_[static_cast<size_t>(dc)];
        if (dp >= 0 && dp == pi && !dragMoved_) {
          const auto& info = cd.info->params[static_cast<size_t>(dp)];
          if (info.kind == ea::ParamKind::Toggle) setParam(cd, dp, cd.values[static_cast<size_t>(dp)] >= 0.5 ? 0.0 : 1.0, false);
          else if (info.kind == ea::ParamKind::Choice) choiceMenu(dc, dp, Point{e.x, e.y});
        } else if (dp < 0) {
          headerClick(dc, part, Point{e.x, e.y});
        }
        return true;
      }
      case MouseAction::DoubleClick:
        if (ci >= 0 && pi >= 0) {
          Card& cd = cards_[static_cast<size_t>(ci)];
          const auto& info = cd.info->params[static_cast<size_t>(pi)];
          if (info.kind == ea::ParamKind::Continuous || info.kind == ea::ParamKind::Integer) setParam(cd, pi, info.def, false);
        }
        return true;
      default: return false;
    }
  }

 private:
  enum { kNone = 0, kBody, kHeaderPart, kPower, kRemove, kLeft, kRight, kMenu };

  void refresh() {
    const sq_guid sel = ctx_.selectedTrack();
    const uint64_t serial = ctx_.docSerial();
    if (serial == serial_ && sel == track_ && dragCard_ < 0) return;
    if (dragCard_ >= 0 && sel == track_) return;  // keep local values while dragging
    serial_ = serial;
    if (!(sel == track_)) scroll_ = 0;
    track_ = sel;
    cards_.clear();
    TrackView t;
    haveTrack_ = ctx_.findTrack(sel, &t) && t.info.kind != SQ_TRACK_FOLDER;
    if (!haveTrack_) return;
    trackName_ = t.info.kind == SQ_TRACK_MASTER ? std::string("Master") : t.name;
    trackColor_ = ctx_.trackColor(t);
    auto props = sq::makeAttributes();
    t.track->getProperties(props.get());
    if (t.info.kind == SQ_TRACK_INSTRUMENT) {
      std::string spec = sq::getAttrString(props.get(), "user.instrument");
      if (spec.empty()) spec = ea::kDefaultInstrument;
      addCard(spec, true, -1);
    }
    const auto ins = splitLines(sq::getAttrString(props.get(), "user.inserts"));
    for (size_t i = 0; i < ins.size(); ++i) addCard(ins[i], false, static_cast<int>(i));
  }

  void addCard(const std::string& line, bool instrument, int slot) {
    Card cd;
    cd.instrument = instrument;
    cd.slot = slot;
    cd.line = line;
    std::string spec = line;
    if (!spec.empty() && spec[0] == '!') {
      cd.bypass = true;
      spec = spec.substr(1);
    }
    if (!ea::parseSpec(spec, &cd.spec)) {
      cd.spec.format = "?";
      cd.spec.id = spec;
    }
    if (cd.spec.format == "builtin") cd.info = ea::findBuiltin(cd.spec.id);
    if (cd.info) {
      cd.values.resize(cd.info->params.size());
      for (size_t i = 0; i < cd.values.size(); ++i) cd.values[i] = cd.info->params[i].def;
      for (const auto& [idx, v] : cd.spec.params)
        if (idx < cd.values.size()) cd.values[idx] = v;
    }
    cards_.push_back(std::move(cd));
  }

  Card* cardAt(int i) { return i >= 0 && i < static_cast<int>(cards_.size()) ? &cards_[static_cast<size_t>(i)] : nullptr; }

  float paintCard(Canvas& c, const Palette& p, Card& cd, float x, float top, float h) {
    const size_t n = cd.info ? cd.info->params.size() : 0;
    const int rows = std::max(1, static_cast<int>((h - kHeader - 8) / kRowH));
    const int cols = n ? static_cast<int>((n + static_cast<size_t>(rows) - 1) / static_cast<size_t>(rows)) : 2;
    const float w = std::max(170.0f, static_cast<float>(cols) * kCell + 16);
    cd.box = Rect{x, top, w, h};
    const Color accent = cd.instrument ? p.accent2 : p.accent;
    c.fillRoundRect(cd.box, 6, p.panel2);
    c.strokeRoundRect(cd.box, 6, cd.bypass ? p.border : draw::mix(p.panel3, accent, 0.3f), 1);
    cd.header = Rect{x, top, w, kHeader - 4};
    c.fillRoundRect(cd.header, 6, draw::mix(p.panel2, accent, cd.bypass ? 0.05f : 0.22f));
    // power
    cd.power = Rect{x + 8, top + 6, 14, 14};
    c.fillEllipse(cd.power, cd.bypass ? p.faint : accent);
    c.fillEllipse(cd.power.inset(4), p.panel2);
    const std::string title = cd.info ? cd.info->name : (cd.spec.id.empty() ? std::string("Device") : cd.spec.id);
    c.drawTextBold(title, Rect{x + 28, top, w - 110, kHeader - 4}, cd.bypass ? p.faint : p.text, 12);
    cd.menu = Rect{x + w - 24, top + 4, 18, 18};
    c.drawText("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2", cd.menu, p.dim, 10, TextAlign::Center);
    if (!cd.instrument) {
      cd.remove = Rect{x + w - 44, top + 4, 18, 18};
      c.drawLine(Point{cd.remove.x + 5, cd.remove.y + 5}, Point{cd.remove.right() - 5, cd.remove.bottom() - 5}, p.dim, 1.5f);
      c.drawLine(Point{cd.remove.right() - 5, cd.remove.y + 5}, Point{cd.remove.x + 5, cd.remove.bottom() - 5}, p.dim, 1.5f);
      cd.right = Rect{x + w - 64, top + 4, 18, 18};
      cd.left = Rect{x + w - 82, top + 4, 18, 18};
      draw::triangle(c, Rect{cd.left.x + 6, cd.left.y + 5, 6, 8}, 3, p.dim);
      draw::triangle(c, Rect{cd.right.x + 6, cd.right.y + 5, 6, 8}, 1, p.dim);
    } else {
      cd.remove = cd.left = cd.right = Rect{};
    }
    cd.controls.clear();
    if (!cd.info) {
      c.drawText(cd.spec.format == "?" ? "Unknown device" : "Plugin: " + cd.spec.id, Rect{x + 12, top + kHeader + 6, w - 24, 18}, p.dim, 11.5f);
      c.drawText("Parameters are edited in the plugin.", Rect{x + 12, top + kHeader + 26, w - 24, 18}, p.faint, 11);
      return x + w;
    }
    for (size_t i = 0; i < n; ++i) {
      const int col = static_cast<int>(i) / rows, row = static_cast<int>(i) % rows;
      Rect cell{x + 8 + static_cast<float>(col) * kCell, top + kHeader + 4 + static_cast<float>(row) * kRowH, kCell, kRowH};
      cd.controls.push_back(cell);
      const auto& pi = cd.info->params[i];
      const double v = cd.values[i];
      const Color ac = cd.bypass ? p.faint : accent;
      // inset so neighbouring labels keep a gap (long names are ellipsized per cell)
      c.drawText(pi.name, Rect{cell.x + 3, cell.y, cell.w - 6, 13}, p.dim, 9.5f, TextAlign::Center);
      if (pi.kind == ea::ParamKind::Toggle) {
        Rect b{cell.x + 10, cell.y + 22, cell.w - 20, 20};
        c.fillRoundRect(b, 4, v >= 0.5 ? ac : p.bg);
        c.drawText(v >= 0.5 ? "On" : "Off", b, v >= 0.5 ? Color::rgb(0x111214) : p.dim, 10.5f, TextAlign::Center);
      } else if (pi.kind == ea::ParamKind::Choice) {
        Rect b{cell.x + 3, cell.y + 22, cell.w - 6, 20};
        c.fillRoundRect(b, 4, p.bg);
        c.drawText(formatValue(pi, v), Rect{b.x + 4, b.y, b.w - 14, b.h}, p.text, 10, TextAlign::Center);
        draw::triangle(c, Rect{b.right() - 10, b.y + 8, 6, 4}, 2, p.dim);
      } else {
        const bool bip = pi.min < 0 && pi.max > 0 && std::fabs(pi.min + pi.max) < 1e-6f;
        draw::knob(c, p, Rect{cell.x + cell.w / 2 - 18, cell.y + 14, 36, 36}, toNorm(pi, v), bip, ac);
        c.drawText(formatValue(pi, v), Rect{cell.x + 2, cell.y + 50, cell.w - 4, 13}, p.text, 9.5f, TextAlign::Center);
      }
    }
    return x + w;
  }

  // The scrollable card area (right of the track label strip).
  Rect contentRect() const {
    const Rect& f = frame();
    return Rect{f.x + 44, f.y, std::max(0.0f, f.w - 44), f.h};
  }

  int hit(float x, float y, int* ci, int* pi) {
    *ci = *pi = -1;
    if (!contentRect().contains(x, y)) return kNone;
    for (size_t i = 0; i < cards_.size(); ++i) {
      const Card& cd = cards_[i];
      if (!cd.box.contains(x, y)) continue;
      *ci = static_cast<int>(i);
      if (cd.power.inset(-3).contains(x, y)) return kPower;
      if (cd.remove.contains(x, y)) return kRemove;
      if (cd.left.contains(x, y)) return kLeft;
      if (cd.right.contains(x, y)) return kRight;
      if (cd.menu.contains(x, y)) return kMenu;
      if (cd.header.contains(x, y)) return kHeaderPart;
      for (size_t k = 0; k < cd.controls.size(); ++k)
        if (cd.controls[k].contains(x, y)) {
          *pi = static_cast<int>(k);
          return kBody;
        }
      return kBody;
    }
    return kNone;
  }

  std::string specLine(const Card& cd) const {
    ea::SpecParts sp = cd.spec;
    if (cd.info) {
      sp.params.clear();
      for (size_t i = 0; i < cd.values.size(); ++i)
        if (cd.values[i] != static_cast<double>(cd.info->params[i].def)) sp.params.emplace_back(static_cast<uint32_t>(i), cd.values[i]);
    }
    return (cd.bypass ? "!" : "") + ea::formatSpec(sp);
  }

  // merge=true: part of the running gesture (gesture_): its changes are one undo step, and a new
  // gesture never merges into the previous one (merge_tag), even on the same device.
  void writeProp(const char* key, const std::string& value, bool merge) {
    auto a = sq::makeAttributes();
    a->setGuid("node", &track_);
    a->setString("key", key);
    a->setString("value", value.c_str());
    if (merge) {
      a->setBool("merge", SQ_TRUE);
      a->setString("merge_tag", ("rack" + std::to_string(gesture_)).c_str());
    }
    ctx_.exec(sq::doc::kSetProperty, a.get());
  }

  void writeCard(Card& cd, bool merge) {
    if (cd.instrument) {
      writeProp("user.instrument", specLine(cd), merge);
    } else {
      std::string all;
      for (const auto& o : cards_)
        if (!o.instrument) all += (&o == &cd ? specLine(cd) : o.line) + "\n";
      writeProp("user.inserts", all, merge);
    }
    cd.line = specLine(cd);
  }

  // Returns true when the value changed (and was written).
  bool setParam(Card& cd, int idx, double v, bool merge) {
    if (!cd.info || idx < 0 || idx >= static_cast<int>(cd.values.size())) return false;
    const auto& pi = cd.info->params[static_cast<size_t>(idx)];
    v = std::clamp(v, static_cast<double>(pi.min), static_cast<double>(pi.max));
    if (pi.kind != ea::ParamKind::Continuous) v = std::round(v);
    if (v == cd.values[static_cast<size_t>(idx)]) return false;
    cd.values[static_cast<size_t>(idx)] = v;
    writeCard(cd, merge);
    ctx_.invalidate();
    return true;
  }

  // Insert-list edits go through the Track.* commands so automation lanes follow their slots.
  void insertCmd(const char* cmd, const std::function<void(IAttributes*)>& args) {
    auto a = sq::makeAttributes();
    a->setGuid("track", &track_);
    args(a.get());
    if (SQ_FAILED(ctx_.shell().execute(cmd, a.get()))) ctx_.setStatus(std::string(cmd) + " failed");
    ctx_.invalidate();
  }
  std::vector<std::string> insertLines() const {
    std::vector<std::string> v;
    for (const auto& o : cards_)
      if (!o.instrument) v.push_back(o.line);
    return v;
  }

  void headerClick(int ci, int part, Point at) {
    Card& cd = cards_[static_cast<size_t>(ci)];
    if (part == kPower) {
      cd.bypass = !cd.bypass;
      if (cd.instrument) {
        cd.bypass = false;  // an instrument is not bypassable
        return;
      }
      writeCard(cd, false);
    } else if (part == kRemove && !cd.instrument) {
      const int slot = cd.slot;
      insertCmd("Track.RemoveInsert", [&](IAttributes* a) { a->setInt("slot", slot); });
    } else if ((part == kLeft || part == kRight) && !cd.instrument) {
      const int slot = cd.slot;
      const int to = slot + (part == kLeft ? -1 : 1);
      if (to < 0 || to >= static_cast<int>(insertLines().size())) return;
      insertCmd("Track.MoveInsert", [&](IAttributes* a) {
        a->setInt("slot", slot);
        a->setInt("to", to);
      });
    } else if (part == kMenu) {
      cardMenu(ci, at);
    }
  }

  void cardMenu(int ci, Point at) {
    const Card cd = cards_[static_cast<size_t>(ci)];
    std::vector<MenuItem> m;
    if (cd.instrument) {
      MenuItem ch;
      ch.label = "Change Instrument";
      ch.submenu = buildInstrumentMenu(ctx_, track_);
      m.push_back(ch);
    } else {
      MenuItem by;
      by.label = cd.bypass ? "Enable" : "Bypass";
      by.action = [this, ci] { headerClick(ci, kPower, Point{}); };
      m.push_back(by);
      MenuItem rm;
      rm.label = "Remove";
      rm.action = [this, ci] { headerClick(ci, kRemove, Point{}); };
      m.push_back(rm);
      MenuItem dup;
      dup.label = "Duplicate";
      dup.action = [this, cd] {
        insertCmd("Track.AddInsert", [&](IAttributes* a) {
          a->setString("spec", cd.line.c_str());
          a->setInt("index", cd.slot + 1);
        });
      };
      m.push_back(dup);
    }
    if (cd.info) {
      MenuItem reset;
      reset.label = "Reset to Defaults";
      reset.action = [this, ci] {
        Card& c2 = cards_[static_cast<size_t>(ci)];
        for (size_t i = 0; i < c2.values.size(); ++i) c2.values[i] = c2.info->params[i].def;
        writeCard(c2, false);
      };
      m.push_back(reset);
    }
    m.push_back(MenuItem::sep());
    MenuItem add;
    add.label = "Add Effect";
    add.submenu = buildInsertMenu(ctx_, track_);
    m.push_back(add);
    ctx_.showMenu(std::move(m), at);
  }

  void choiceMenu(int ci, int pi, Point at) {
    const Card& cd = cards_[static_cast<size_t>(ci)];
    const auto& info = cd.info->params[static_cast<size_t>(pi)];
    std::vector<MenuItem> m;
    for (size_t k = 0; k < info.choices.size(); ++k) {
      MenuItem it;
      it.label = info.choices[k];
      it.checked = std::lround(cd.values[static_cast<size_t>(pi)]) == static_cast<long>(k);
      it.action = [this, ci, pi, k] {
        if (ci < static_cast<int>(cards_.size())) setParam(cards_[static_cast<size_t>(ci)], pi, static_cast<double>(k), false);
      };
      m.push_back(it);
    }
    ctx_.showMenu(std::move(m), at);
  }

  GuiContext& ctx_;
  sq_guid track_{};
  bool haveTrack_ = false;
  std::string trackName_;
  Color trackColor_;
  std::vector<Card> cards_;
  uint64_t serial_ = 0;
  float scroll_ = 0, contentW_ = 0;
  Rect addBtn_;
  bool hotAdd_ = false;
  int hotCard_ = -1, hotPart_ = 0, tipCard_ = -1, tipParam_ = -1;
  int dragCard_ = -1, dragParam_ = -1;
  float dragY_ = 0;
  double dragNorm_ = 0;
  bool dragMoved_ = false;
  uint64_t gesture_ = 0;  // mouse gesture counter (undo merge tag)
  int wheelCard_ = -1, wheelParam_ = -1;  // control of the running wheel gesture
};

}  // namespace

std::unique_ptr<Widget> makeDeviceRack(GuiContext& ctx) { return std::make_unique<DeviceRack>(ctx); }

}  // namespace sq::app::gui
