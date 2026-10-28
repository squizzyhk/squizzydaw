// Squizzy Studio GUI - arrange view: model cache, geometry, time grid, document edits, context
// menus, keyboard and file drops. Painting lives in gui_arrange_paint.cpp, mouse interaction in
// gui_arrange_mouse.cpp (see gui_arrange.hpp for the layout).
#include "gui_arrange.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "sq/cpp/settings.hpp"
#include "sq/engine/audio/automation.hpp"
#include "sq/engine/doc/actions.hpp"
#include "sq/engine/doc/document.hpp"
#include "sq/interfaces/midi.h"

namespace sq::app::gui {
namespace arrange {

namespace {

std::string stemOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = s.find_last_of('.');
  if (dot != std::string::npos && dot > 0) s.resize(dot);
  return s;
}

uint32_t packColor(Color c) {
  auto b = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  return (b(c.r) << 24) | (b(c.g) << 16) | (b(c.b) << 8) | 0xFFu;
}

// Copies attribute `key` of src into dst under `dstKey` (any scalar/blob type).
void copyAttr(IAttributes* src, const char* key, IAttributes* dst, const char* dstKey) {
  switch (src->getType(key)) {
    case SQ_VALUE_INT: dst->setInt(dstKey, src->getInt(key, 0)); break;
    case SQ_VALUE_DOUBLE: dst->setDouble(dstKey, src->getDouble(key, 0)); break;
    case SQ_VALUE_BOOL: dst->setBool(dstKey, src->getBool(key, SQ_FALSE)); break;
    case SQ_VALUE_STRING: dst->setString(dstKey, sq::getAttrString(src, key).c_str()); break;
    case SQ_VALUE_GUID: {
      sq_guid g{};
      src->getGuid(key, &g);
      dst->setGuid(dstKey, &g);
      break;
    }
    case SQ_VALUE_BLOB: {
      uint32_t need = 0;
      src->getBlob(key, nullptr, 0, &need);
      std::vector<uint8_t> buf(need);
      if (need) src->getBlob(key, buf.data(), need, &need);
      dst->setBlob(dstKey, buf.data(), need);
      break;
    }
    default: break;
  }
}

// Automation lanes of a track: the "automation" track property (automation.hpp text format) when
// the document exposes it, else straight from the native store.
std::vector<engine::audio::AutomationLane> readAutomation(GuiContext& ctx, const TrackView& t) {
  if (t.track) {
    auto props = sq::makeAttributes();
    if (t.track->getProperties(props.get()) == SQ_OK && props->getType("automation") == SQ_VALUE_STRING)
      return engine::audio::parseAutomation(sq::getAttrString(props.get(), "automation"));
  }
  std::vector<engine::audio::AutomationLane> out;
  auto d = ctx.doc();
  auto* impl = d ? edoc::DocumentImpl::from(d.get()) : nullptr;
  if (!impl) return out;
  if (const auto* rec = impl->store().track(t.id)) {
    for (const auto& [param, pts] : rec->automation) {
      engine::audio::AutomationLane l;
      l.target = param;
      for (const auto& p : pts) l.points.emplace_back(p.pos, p.value);
      out.push_back(std::move(l));
    }
  }
  return out;
}

const char* kColorNames[] = {"Teal", "Violet", "Amber", "Blue", "Coral", "Sage", "Magenta", "Olive", "Red", "Gray"};

}  // namespace

// ---- automation targets ---------------------------------------------------------------------------
AutoRange autoRange(const std::string& tgt, const std::vector<std::pair<double, double>>& pts) {
  if (tgt == "volume" || tgt.compare(0, 5, "send.") == 0) return AutoRange{-60.0, 6.0, false, "dB"};
  if (tgt == "pan") return AutoRange{-1.0, 1.0, false, ""};
  if (tgt == "mute") return AutoRange{0.0, 1.0, true, ""};
  AutoRange r{0.0, 1.0, false, ""};
  for (const auto& p : pts) {
    r.lo = std::min(r.lo, p.second);
    r.hi = std::max(r.hi, p.second);
  }
  return r;
}

std::string autoLabel(const std::string& tgt) {
  if (tgt == "volume") return "Volume";
  if (tgt == "pan") return "Pan";
  if (tgt == "mute") return "Mute";
  if (tgt.compare(0, 5, "send.") == 0) return "Send " + std::to_string(std::atoi(tgt.c_str() + 5) + 1);
  if (tgt.compare(0, 7, "insert.") == 0) {
    const size_t dot = tgt.find('.', 7);
    if (dot != std::string::npos) return "FX " + std::to_string(std::atoi(tgt.c_str() + 7) + 1) + ": " + tgt.substr(dot + 1);
  }
  if (tgt.compare(0, 11, "instrument.") == 0) return "Instr: " + tgt.substr(11);
  return tgt;
}

std::string formatAutoValue(const std::string& tgt, double v) {
  char buf[48];
  if (tgt == "volume" || tgt.compare(0, 5, "send.") == 0) {
    if (v <= -59.95) return "-inf dB";
    std::snprintf(buf, sizeof buf, "%+.1f dB", v);
  } else if (tgt == "pan") {
    const int p = static_cast<int>(std::lround(v * 100.0));
    if (p == 0) return "C";
    std::snprintf(buf, sizeof buf, "%c%d", p < 0 ? 'L' : 'R', std::abs(p));
  } else if (tgt == "mute") {
    return v >= 0.5 ? "Muted" : "On";
  } else {
    std::snprintf(buf, sizeof buf, "%.3g", v);
  }
  return buf;
}

// ---- construction / model -------------------------------------------------------------------------
ArrangeView::ArrangeView(GuiContext& ctx) : Widget("Arrange"), ctx_(ctx), peaks_(&ctx.shell().registry) {
  hitTestable = true;
  text = "Arrange";
}
ArrangeView::~ArrangeView() = default;

void ArrangeView::refresh() {
  if (serial_ != ctx_.docSerial()) rebuild();
  es_ = ctx_.editState();
}

void ArrangeView::rebuild() {
  serial_ = ctx_.docSerial();
  lanes_.clear();
  markers_.clear();
  sigs_.clear();
  tempo_ = nullptr;
  songEnd_ = 0;
  auto d = ctx_.doc();
  if (d) {
    d->getTempoMap(tempo_.put());
    const uint32_t nm = d->getMarkerCount();
    for (uint32_t i = 0; i < nm; ++i) {
      MarkerView mv;
      mv.m.struct_size = sizeof mv.m;
      if (d->getMarker(i, &mv.m) != SQ_OK) continue;
      if (mv.m.flags & (SQ_DOCMARKER_LOOP | SQ_DOCMARKER_PUNCH)) continue;
      markers_.push_back(mv);
    }
  }
  if (tempo_) {
    const uint32_t n = tempo_->getSignatureCount();
    for (uint32_t i = 0; i < n; ++i) {
      sq_time_signature s{};
      s.struct_size = sizeof s;
      if (tempo_->getSignature(i, &s) == SQ_OK && s.numerator > 0 && s.denominator > 0) sigs_.push_back(Sig{s.position, s.numerator, s.denominator});
    }
  }
  if (sigs_.empty() || sigs_.front().pos > 1e-9) sigs_.insert(sigs_.begin(), Sig{0.0, 4, 4});

  int number = 0;
  for (auto& t : ctx_.tracks()) {
    if (t.info.kind == SQ_TRACK_MASTER || (t.info.flags & SQ_TRACK_HIDDEN)) continue;
    Lane l;
    l.color = ctx_.trackColor(t);
    l.number = ++number;
    for (auto& cv : ctx_.clips(t)) {
      ClipEntry ce;
      ce.v = std::move(cv);
      if (ce.v.info.kind == SQ_CLIP_MIDI && ce.v.info.event_count > 0) {
        int lo = 127, hi = 0;
        for (const auto& ev : ctx_.clipEvents(ce.v.clip.get())) {
          if ((ev.bytes[0] & 0xF0) != 0x90 || ev.bytes[2] == 0) continue;
          ce.notes.push_back(NoteMini{static_cast<float>(ev.position), static_cast<float>(ev.length), ev.bytes[1], ev.bytes[2]});
          lo = std::min(lo, static_cast<int>(ev.bytes[1]));
          hi = std::max(hi, static_cast<int>(ev.bytes[1]));
        }
        if (!ce.notes.empty()) {
          // at least an octave so a single note does not fill the whole clip
          const int mid = (lo + hi) / 2;
          ce.lo = std::min(lo, mid - 6);
          ce.hi = std::max(hi, mid + 6);
        }
      }
      songEnd_ = std::max(songEnd_, ce.end());
      l.clips.push_back(std::move(ce));
    }
    std::sort(l.clips.begin(), l.clips.end(), [](const ClipEntry& a, const ClipEntry& b) { return a.v.info.start < b.v.info.start; });
    l.showAuto = autoShown_.count(t.id) && autoShown_[t.id];
    if (l.showAuto) {
      std::vector<std::string> have;
      for (auto& al : readAutomation(ctx_, t)) {
        AutoLane a;
        a.target = al.target;
        a.points = std::move(al.points);
        a.stored = true;
        have.push_back(a.target);
        l.autos.push_back(std::move(a));
      }
      auto& extra = autoExtra_[t.id];
      for (const auto& tg : extra)
        if (std::find(have.begin(), have.end(), tg) == have.end()) {
          AutoLane a;
          a.target = tg;
          l.autos.push_back(std::move(a));
          have.push_back(tg);
        }
      if (l.autos.empty()) {
        AutoLane a;
        a.target = "volume";
        l.autos.push_back(std::move(a));
        extra.push_back("volume");
      }
    }
    l.t = std::move(t);
    lanes_.push_back(std::move(l));
  }
  relayout();
}

void ArrangeView::relayout() {
  float y = 0;
  for (auto& l : lanes_) {
    auto it = laneHeights_.find(l.t.id);
    l.h = it != laneHeights_.end() ? it->second : kDefaultLaneH;
    l.y = y;
    y += l.h;
    if (l.showAuto)
      for (auto& a : l.autos) {
        a.y = y;
        a.h = kAutoH;
        y += kAutoH;
      }
    l.total = y - l.y;
  }
  contentH_ = y;
  clampScroll();
}

int ArrangeView::laneIndex(const sq_guid& track) const {
  for (size_t i = 0; i < lanes_.size(); ++i)
    if (lanes_[i].t.id == track) return static_cast<int>(i);
  return -1;
}

const ClipEntry* ArrangeView::findClip(const sq_guid& id, int* lane) const {
  for (size_t i = 0; i < lanes_.size(); ++i)
    for (const auto& c : lanes_[i].clips)
      if (c.v.id == id) {
        if (lane) *lane = static_cast<int>(i);
        return &c;
      }
  return nullptr;
}

bool ArrangeView::selected(const sq_guid& clip) const {
  return std::find(es_.selection.begin(), es_.selection.end(), clip) != es_.selection.end();
}

// ---- geometry ---------------------------------------------------------------------------------------
Rect ArrangeView::toolbarRect() const {
  const Rect& f = frame();
  return Rect{f.x, f.y, f.w, kToolbarH};
}
Rect ArrangeView::cornerRect() const {
  const Rect& f = frame();
  return Rect{f.x, f.y + kToolbarH, headerW_, kMarkerH + kRulerH};
}
Rect ArrangeView::markerRect() const {
  const Rect& f = frame();
  return Rect{f.x + headerW_, f.y + kToolbarH, std::max(0.0f, f.w - headerW_ - kScrollW), kMarkerH};
}
Rect ArrangeView::rulerRect() const {
  const Rect& f = frame();
  return Rect{f.x + headerW_, f.y + kToolbarH + kMarkerH, std::max(0.0f, f.w - headerW_ - kScrollW), kRulerH};
}
Rect ArrangeView::lanesRect() const {
  const Rect& f = frame();
  const float y0 = f.y + kToolbarH + kMarkerH + kRulerH;
  return Rect{f.x + headerW_, y0, std::max(0.0f, f.w - headerW_ - kScrollW), std::max(0.0f, f.bottom() - kScrollW - y0)};
}
Rect ArrangeView::headersRect() const {
  const Rect l = lanesRect();
  return Rect{frame().x, l.y, headerW_, l.h};
}
Rect ArrangeView::vbarRect() const {
  const Rect l = lanesRect();
  return Rect{l.right(), l.y, kScrollW, l.h};
}
Rect ArrangeView::hbarRect() const {
  const Rect l = lanesRect();
  return Rect{l.x, l.bottom(), l.w, kScrollW};
}
float ArrangeView::xOf(double ppq) const { return lanesRect().x + static_cast<float>((ppq - scroll_) * ppx_); }
double ArrangeView::ppqAt(float x) const { return scroll_ + static_cast<double>(x - lanesRect().x) / ppx_; }
float ArrangeView::laneTop(const Lane& l) const { return lanesRect().y + l.y - scrollY_; }

Rect ArrangeView::clipRect(const Lane& l, const ClipEntry& c) const {
  const float x0 = xOf(c.v.info.start), x1 = xOf(c.end());
  return Rect{x0, laneTop(l) + 2, std::max(1.0f, x1 - x0), l.h - 5};
}

Rect ArrangeView::headerButtonRect(const Lane& l, int b) const {
  const float top = laneTop(l);
  const float x = frame().x;
  if (l.h < 50) {
    const float bx = x + headerW_ - 16 - static_cast<float>(kBtnCount - b) * 21.0f;
    return Rect{bx, top + (l.h - 16) / 2, 19, 16};
  }
  return Rect{x + 14 + static_cast<float>(b) * 23.0f, top + 27, 20, 17};
}

Rect ArrangeView::nameRect(const Lane& l) const {
  const float top = laneTop(l);
  const float x = frame().x + 46;
  const float right = l.h < 50 ? headerButtonRect(l, 0).x - 6 : frame().x + headerW_ - 18;
  return Rect{x, l.h < 50 ? top + (l.h - 18) / 2 : top + 5, std::max(10.0f, right - x), 18};
}

Rect ArrangeView::volumeRect(const Lane& l) const {
  if (l.h < 64) return Rect{};
  const float avail = headerW_ - 14 - 20;
  return Rect{frame().x + 14, laneTop(l) + 51, avail * 0.62f, 9};
}
Rect ArrangeView::panRect(const Lane& l) const {
  const Rect v = volumeRect(l);
  if (v.w <= 0) return Rect{};
  const float avail = headerW_ - 14 - 20;
  return Rect{v.right() + 8, v.y, avail - v.w - 8, v.h};
}

float ArrangeView::fadeInX(const ClipEntry& c) const {
  return xOf(ppqAtSec(secAt(c.v.info.start) + c.v.info.fade_in_seconds));
}
float ArrangeView::fadeOutX(const ClipEntry& c) const {
  return xOf(ppqAtSec(secAt(c.end()) - c.v.info.fade_out_seconds));
}

double ArrangeView::visibleSpan() const { return static_cast<double>(lanesRect().w) / ppx_; }

double ArrangeView::contentEnd() const {
  const auto ts = const_cast<ArrangeView*>(this)->ctx_.transport();
  double e = std::max({songEnd_ + 4.0 * barLength(songEnd_), 64.0, ts.loop_end + 16.0});
  return std::max(e, scroll_ + visibleSpan());
}

void ArrangeView::clampScroll() {
  scroll_ = std::max(0.0, scroll_);
  const float maxY = std::max(0.0f, contentH_ + 80.0f - lanesRect().h);
  scrollY_ = std::clamp(scrollY_, 0.0f, maxY);
}

void ArrangeView::setZoom(double ppx, float anchorX) {
  const Rect L = lanesRect();
  anchorX = std::clamp(anchorX, L.x, L.right());
  const double at = ppqAt(anchorX);
  ppx_ = std::clamp(ppx, 0.15, 1500.0);
  scroll_ = at - static_cast<double>(anchorX - L.x) / ppx_;
  clampScroll();
  ctx_.invalidate();
}

void ArrangeView::zoomToFit() {
  const Rect L = lanesRect();
  double end = std::max(songEnd_, 4.0 * barLength(0));
  if (L.w > 20) ppx_ = std::clamp(static_cast<double>(L.w) / (end * 1.03), 0.15, 1500.0);
  scroll_ = 0;
  scrollY_ = 0;
  clampScroll();
  ctx_.invalidate();
}

void ArrangeView::ensureVisible(double ppq) {
  const double span = visibleSpan();
  if (ppq < scroll_ || ppq > scroll_ + span) {
    scroll_ = std::max(0.0, ppq - span * 0.1);
    ctx_.invalidate();
  }
}

// ---- time / grid ------------------------------------------------------------------------------------
double ArrangeView::barLength(double ppq) const {
  const Sig* s = sigs_.empty() ? nullptr : &sigs_.front();
  for (const auto& g : sigs_)
    if (g.pos <= ppq + 1e-9) s = &g;
  if (!s) return 4.0;
  return 4.0 / static_cast<double>(s->den) * static_cast<double>(s->num);
}
double ArrangeView::beatLength(double ppq) const {
  const Sig* s = sigs_.empty() ? nullptr : &sigs_.front();
  for (const auto& g : sigs_)
    if (g.pos <= ppq + 1e-9) s = &g;
  return s ? 4.0 / static_cast<double>(s->den) : 1.0;
}
double ArrangeView::barStart(double ppq) const {
  const Sig* s = sigs_.empty() ? nullptr : &sigs_.front();
  for (const auto& g : sigs_)
    if (g.pos <= ppq + 1e-9) s = &g;
  if (!s) return std::floor(ppq / 4.0) * 4.0;
  const double bar = 4.0 / static_cast<double>(s->den) * static_cast<double>(s->num);
  return s->pos + std::floor((ppq - s->pos) / bar + 1e-9) * bar;
}

double ArrangeView::snap(double ppq, unsigned mods) const {
  ppq = std::max(0.0, ppq);
  if (!snapOn() || (mods & kModShift)) return ppq;
  if (es_.snap == "bar") {
    const double bs = barStart(ppq), bl = barLength(ppq);
    return ppq - bs < bl / 2 ? bs : bs + bl;
  }
  const double g = es_.grid > 0 ? es_.grid : 1.0;
  const double bs = barStart(ppq);  // grid relative to the bar (odd signatures)
  return std::max(0.0, bs + std::round((ppq - bs) / g) * g);
}

double ArrangeView::snapFloor(double ppq) const {
  ppq = std::max(0.0, ppq);
  if (!snapOn()) return ppq;
  const double g = es_.snap == "bar" ? barLength(ppq) : (es_.grid > 0 ? es_.grid : 1.0);
  const double bs = barStart(ppq);
  return bs + std::floor((ppq - bs) / g + 1e-9) * g;
}

double ArrangeView::secAt(double ppq) const { return tempo_ ? tempo_->ppqToSeconds(ppq) : ppq * 0.5; }
double ArrangeView::ppqAtSec(double sec) const { return tempo_ ? tempo_->secondsToPpq(sec) : sec * 2.0; }

// ---- automation -------------------------------------------------------------------------------------
void ArrangeView::setAutomation(const sq_guid& track, const std::string& tgt, const std::vector<std::pair<double, double>>& pts) {
  std::vector<double> raw;
  raw.reserve(pts.size() * 2);
  for (const auto& p : pts) {
    raw.push_back(std::max(0.0, p.first));
    raw.push_back(p.second);
  }
  auto a = sq::makeAttributes();
  a->setGuid("track", &track);
  a->setString("param", tgt.c_str());
  a->setBlob("points", raw.data(), static_cast<uint32_t>(raw.size() * sizeof(double)));
  auto& extra = autoExtra_[track];
  if (std::find(extra.begin(), extra.end(), tgt) == extra.end()) extra.push_back(tgt);  // keep the lane visible when emptied
  ctx_.exec(edoc::kSetAutomation, a.get());
}

Rect ArrangeView::autoLaneRect(const Lane& l, const AutoLane& a) const {
  (void)l;
  const Rect L = lanesRect();
  return Rect{L.x, L.y + a.y - scrollY_, L.w, a.h};
}

float ArrangeView::autoValueY(const AutoLane& a, const Rect& r, double v) const {
  const AutoRange rg = autoRange(a.target, a.points);
  const double t = rg.hi > rg.lo ? std::clamp((v - rg.lo) / (rg.hi - rg.lo), 0.0, 1.0) : 0.5;
  return r.bottom() - 6 - static_cast<float>(t) * (r.h - 12);
}

double ArrangeView::autoValueAt(const AutoLane& a, const Rect& r, float y) const {
  const AutoRange rg = autoRange(a.target, a.points);
  const double t = std::clamp(static_cast<double>(r.bottom() - 6 - y) / std::max(1.0, static_cast<double>(r.h - 12)), 0.0, 1.0);
  double v = rg.lo + t * (rg.hi - rg.lo);
  if (rg.stepped) v = std::round(v);
  return v;
}

// ---- edits ------------------------------------------------------------------------------------------
sq_guid ArrangeView::copyClip(const ClipEntry& c, const sq_guid& track, double start) {
  const auto& ci = c.v.info;
  auto p = sq::makeAttributes();
  p->setGuid("track", &track);
  p->setInt("kind", ci.kind);
  p->setDouble("start", std::max(0.0, start));
  p->setDouble("length", ci.length);
  p->setString("name", c.v.name.c_str());
  if (ci.kind == SQ_CLIP_AUDIO) {
    p->setString("source", c.v.source.c_str());
    p->setDouble("source_offset", ci.source_offset_seconds);
  }
  auto res = sq::makeAttributes();
  if (SQ_FAILED(ctx_.exec(sq::doc::kAddClip, p.get(), res.get()))) return {};
  sq_guid id{};
  res->getGuid("node", &id);
  if (sq_guid_is_null(&id)) return id;
  if (ci.gain_db != 0) ctx_.setProperty(id, "gainDb", ci.gain_db);
  if (ci.fade_in_seconds > 0) ctx_.setProperty(id, "fadeIn", ci.fade_in_seconds);
  if (ci.fade_out_seconds > 0) ctx_.setProperty(id, "fadeOut", ci.fade_out_seconds);
  if (ci.flags & SQ_CLIP_MUTED) ctx_.setPropertyBool(id, "muted", true);
  if (ci.color != 0) {
    auto cp = sq::makeAttributes();
    cp->setGuid("node", &id);
    cp->setString("key", "color");
    cp->setInt("value", ci.color);
    ctx_.exec(sq::doc::kSetProperty, cp.get());
  }
  if (ci.kind == SQ_CLIP_MIDI && ci.event_count > 0) {
    auto evs = ctx_.clipEvents(c.v.clip.get());
    auto e = sq::makeAttributes();
    e->setGuid("clip", &id);
    e->setBlob("events", evs.data(), static_cast<uint32_t>(evs.size() * sizeof(sq_clip_event)));
    ctx_.exec(sq::doc::kSetClipEvents, e.get());
  }
  return id;
}

void ArrangeView::commitMove(double delta, int laneDelta, bool copy) {
  struct Item {
    const ClipEntry* c;
    int lane;
  };
  std::vector<Item> items;
  for (size_t i = 0; i < lanes_.size(); ++i)
    for (const auto& c : lanes_[i].clips)
      if (selected(c.v.id)) items.push_back(Item{&c, static_cast<int>(i)});
  if (items.empty() || (std::fabs(delta) < 1e-9 && laneDelta == 0 && !copy)) return;
  std::vector<sq_guid> newSel;
  {
    EditScope scope(ctx_, copy ? (items.size() > 1 ? "Copy Clips" : "Copy Clip") : (items.size() > 1 ? "Move Clips" : "Move Clip"));
    for (const auto& it : items) {
      const ClipEntry& c = *it.c;
      int tgt = std::clamp(it.lane + laneDelta, 0, static_cast<int>(lanes_.size()) - 1);
      const Lane& tl = lanes_[static_cast<size_t>(tgt)];
      const bool fits = (c.v.info.kind == SQ_CLIP_AUDIO && tl.audio()) || (c.v.info.kind == SQ_CLIP_MIDI && tl.instrument());
      if (!fits) tgt = it.lane;
      const sq_guid track = lanes_[static_cast<size_t>(tgt)].t.id;
      const double start = std::max(0.0, c.v.info.start + delta);
      if (copy) {
        const sq_guid id = copyClip(c, track, start);
        if (!sq_guid_is_null(&id)) newSel.push_back(id);
      } else {
        auto p = sq::makeAttributes();
        p->setGuid("node", &c.v.id);
        p->setDouble("start", start);
        if (tgt != it.lane) p->setGuid("track", &track);
        ctx_.exec(sq::doc::kMoveClip, p.get());
      }
    }
  }
  if (copy && !newSel.empty()) ctx_.setSelection(newSel);
}

void ArrangeView::splitAt(const ClipEntry& c, double at) {
  if (at <= c.v.info.start + 1e-6 || at >= c.end() - 1e-6) return;
  auto p = sq::makeAttributes();
  p->setGuid("node", &c.v.id);
  p->setDouble("at", at);
  ctx_.exec(sq::doc::kSplitClip, p.get());
}

void ArrangeView::createMidiClip(const Lane& l, double at) {
  if (!l.instrument()) {
    ctx_.setStatus("MIDI clips go on instrument tracks");
    return;
  }
  const double start = snapFloor(at);
  auto p = sq::makeAttributes();
  p->setGuid("track", &l.t.id);
  p->setInt("kind", SQ_CLIP_MIDI);
  p->setDouble("start", start);
  p->setDouble("length", barLength(start));
  auto res = sq::makeAttributes();
  if (SQ_FAILED(ctx_.exec(sq::doc::kAddClip, p.get(), res.get()))) return;
  sq_guid id{};
  res->getGuid("node", &id);
  ctx_.selectTrack(l.t.id);
  ctx_.setSelection({id});
  ctx_.openClipEditor(id);
}

void ArrangeView::duplicateTrack(const Lane& l) {
  EditScope scope(ctx_, "Duplicate Track");
  auto p = sq::makeAttributes();
  p->setInt("kind", l.t.info.kind);
  p->setString("name", (l.t.name + " copy").c_str());
  p->setInt("index", static_cast<int64_t>(l.t.index) + 1);
  auto res = sq::makeAttributes();
  if (SQ_FAILED(ctx_.exec(sq::doc::kAddTrack, p.get(), res.get()))) {
    scope.cancel();
    return;
  }
  sq_guid id{};
  res->getGuid("node", &id);
  auto props = sq::makeAttributes();
  if (l.t.track && l.t.track->getProperties(props.get()) == SQ_OK) {
    static const char* kCopy[] = {"volumeDb", "pan", "color", "mute", "solo", "monitor", "output", "timebase"};
    std::vector<std::string> keys;
    for (const char* k : kCopy) keys.emplace_back(k);
    const uint32_t n = props->getCount();
    for (uint32_t i = 0; i < n; ++i) {
      char buf[256];
      uint32_t need = 0;
      if (props->getKey(i, buf, sizeof buf, &need) == SQ_OK && std::strncmp(buf, "user.", 5) == 0) keys.emplace_back(buf);
    }
    for (const auto& k : keys) {
      if (props->getType(k.c_str()) == SQ_VALUE_NONE) continue;
      auto sp = sq::makeAttributes();
      sp->setGuid("node", &id);
      sp->setString("key", k.c_str());
      copyAttr(props.get(), k.c_str(), sp.get(), "value");
      auto d = ctx_.doc();
      sq::Ptr<IEditor> ed;
      if (d && d->getEditor(ed.put()) == SQ_OK && ed) ed->execute(&sq::doc::kSetProperty, sp.get(), nullptr);  // quiet: optional keys
    }
  }
  for (const auto& al : readAutomation(ctx_, l.t)) {
    std::vector<std::pair<double, double>> pts(al.points.begin(), al.points.end());
    setAutomation(id, al.target, pts);
  }
  for (const auto& c : l.clips) copyClip(c, id, c.v.info.start);
  ctx_.selectTrack(id);
}

void ArrangeView::deleteTrack(const Lane& l) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &l.t.id);
  ctx_.exec(sq::doc::kRemoveNode, p.get());
}

void ArrangeView::renameTrack(const Lane& l) {
  const sq_guid id = l.t.id;
  GuiContext& ctx = ctx_;
  ctx_.prompt("Rename Track", l.t.name, [&ctx, id](const std::string& nm) {
    if (!nm.empty()) ctx.setPropertyString(id, "name", nm);
  });
}

void ArrangeView::renameClip(const ClipEntry& c) {
  const sq_guid id = c.v.id;
  GuiContext& ctx = ctx_;
  ctx_.prompt("Rename Clip", c.v.name, [&ctx, id](const std::string& nm) { ctx.setPropertyString(id, "name", nm); });
}

void ArrangeView::addMarkerAt(double ppq) {
  auto p = sq::makeAttributes();
  p->setDouble("position", std::max(0.0, ppq));
  ctx_.exec(sq::doc::kAddMarker, p.get());
}

void ArrangeView::locate(double ppq) {
  ppq = std::max(0.0, ppq);
  if (ITransport* t = ctx_.transportIf()) t->locate(ppq);
  ctx_.setCursor(ppq);
}

void ArrangeView::toggleTrackFlag(const Lane& l, int button) {
  const uint32_t f = l.t.info.flags;
  switch (button) {
    case kBtnMute: ctx_.setPropertyBool(l.t.id, "mute", !(f & SQ_TRACK_MUTE)); break;
    case kBtnSolo: ctx_.setPropertyBool(l.t.id, "solo", !(f & SQ_TRACK_SOLO)); break;
    case kBtnArm: ctx_.setPropertyBool(l.t.id, "armed", !(f & SQ_TRACK_ARMED)); break;
    case kBtnMonitor: ctx_.setPropertyBool(l.t.id, "monitor", !(f & SQ_TRACK_MONITOR)); break;
    case kBtnAuto:
      autoShown_[l.t.id] = !l.showAuto;
      rebuild();
      ctx_.invalidate();
      break;
    default: break;
  }
}

bool ArrangeView::importMidiFile(const std::string& path, int laneIdx, double at) {
  auto rd = sq::createInstance<IMidiFileReader>(&ctx_.shell().registry, sq::midi::kMidiFileReaderClassId);
  if (!rd) {
    ctx_.setStatus("MIDI import needs the midi-file service");
    return false;
  }
  if (SQ_FAILED(rd->open(path.c_str()))) {
    ctx_.setStatus("cannot read " + path);
    return false;
  }
  sq_midi_file_info fi{};
  fi.struct_size = sizeof fi;
  rd->getInfo(&fi);
  const double tpq = fi.ticks_per_quarter > 0 ? static_cast<double>(fi.ticks_per_quarter) : 480.0;
  std::vector<sq_clip_event> evs;
  double end = 0;
  for (uint32_t tr = 0; tr < fi.track_count; ++tr) {
    struct Open {
      uint64_t tick;
      uint8_t vel;
      bool on = false;
    };
    Open open[16][128]{};
    const uint32_t n = rd->getEventCount(tr);
    for (uint32_t i = 0; i < n; ++i) {
      sq_midi_file_event e{};
      e.struct_size = sizeof e;
      if (rd->getEvent(tr, i, &e) != SQ_OK) continue;
      const uint8_t type = e.status & 0xF0, ch = e.status & 0x0F;
      if (e.status >= 0xF0) continue;
      const uint8_t key = e.data1 & 0x7F;
      const bool on = type == 0x90 && e.data2 > 0;
      const bool off = type == 0x80 || (type == 0x90 && e.data2 == 0);
      if ((on || off) && open[ch][key].on) {
        sq_clip_event ce{};
        ce.struct_size = sizeof ce;
        ce.position = static_cast<double>(open[ch][key].tick) / tpq;
        ce.length = std::max(1.0 / 64.0, static_cast<double>(e.tick - open[ch][key].tick) / tpq);
        ce.bytes[0] = static_cast<uint8_t>(0x90 | ch);
        ce.bytes[1] = key;
        ce.bytes[2] = open[ch][key].vel;
        ce.size = 3;
        evs.push_back(ce);
        end = std::max(end, ce.position + ce.length);
        open[ch][key].on = false;
      }
      if (on) open[ch][key] = Open{e.tick, e.data2, true};
    }
  }
  if (evs.empty()) {
    ctx_.setStatus("no notes in " + path);
    return false;
  }
  std::sort(evs.begin(), evs.end(), [](const sq_clip_event& a, const sq_clip_event& b) { return a.position < b.position; });
  const double bar = barLength(at);
  const double len = std::max(bar, std::ceil(end / bar - 1e-9) * bar);
  EditScope scope(ctx_, "Import MIDI File");
  sq_guid track{};
  if (laneIdx >= 0 && lanes_[static_cast<size_t>(laneIdx)].instrument()) {
    track = lanes_[static_cast<size_t>(laneIdx)].t.id;
  } else {
    auto p = sq::makeAttributes();
    p->setInt("kind", SQ_TRACK_INSTRUMENT);
    p->setString("name", stemOf(path).c_str());
    auto res = sq::makeAttributes();
    if (SQ_FAILED(ctx_.exec(sq::doc::kAddTrack, p.get(), res.get()))) {
      scope.cancel();
      return false;
    }
    res->getGuid("node", &track);
  }
  auto p = sq::makeAttributes();
  p->setGuid("track", &track);
  p->setInt("kind", SQ_CLIP_MIDI);
  p->setDouble("start", at);
  p->setDouble("length", len);
  p->setString("name", stemOf(path).c_str());
  auto res = sq::makeAttributes();
  if (SQ_FAILED(ctx_.exec(sq::doc::kAddClip, p.get(), res.get()))) {
    scope.cancel();
    return false;
  }
  sq_guid id{};
  res->getGuid("node", &id);
  auto e = sq::makeAttributes();
  e->setGuid("clip", &id);
  e->setBlob("events", evs.data(), static_cast<uint32_t>(evs.size() * sizeof(sq_clip_event)));
  ctx_.exec(sq::doc::kSetClipEvents, e.get());
  ctx_.selectTrack(track);
  ctx_.setSelection({id});
  return true;
}

// ---- file drops -----------------------------------------------------------------------------------
bool ArrangeView::dropFile(float x, float y, const std::string& path) {
  dropX_ = dropY_ = -1;
  refresh();
  const std::string ext = fileExtension(path);
  const Rect L = lanesRect();
  const double at = snap(ppqAt(std::max(x, L.x)), 0);
  const Hit h = hitTest(x, y);
  int li = h.lane;
  if (y > L.bottom() || y < L.y) li = -1;
  if (isMidiFileExtension(ext)) return importMidiFile(path, li, at);
  if (!isAudioFileExtension(ext)) {
    ctx_.setStatus("cannot import " + path + " (not an audio or MIDI file)");
    return false;
  }
  AudioFileInfo info;
  if (!probeAudioFile(&ctx_.shell().registry, path, &info) || info.frames <= 0) {
    ctx_.setStatus("cannot read audio file " + path);
    return false;
  }
  peaks_.request(path);  // start decoding right away
  sq_guid track{};
  sq_guid clip{};
  {
    EditScope scope(ctx_, "Import Audio File");
    if (li >= 0 && lanes_[static_cast<size_t>(li)].audio()) {
      track = lanes_[static_cast<size_t>(li)].t.id;
    } else {
      auto p = sq::makeAttributes();
      p->setInt("kind", SQ_TRACK_AUDIO);
      p->setString("name", stemOf(path).c_str());
      if (info.channels == 1) p->setInt("channels", 1);
      auto res = sq::makeAttributes();
      if (SQ_FAILED(ctx_.exec(sq::doc::kAddTrack, p.get(), res.get()))) {
        scope.cancel();
        return false;
      }
      res->getGuid("node", &track);
    }
    const double len = std::max(1e-3, ppqAtSec(secAt(at) + info.seconds()) - at);
    auto p = sq::makeAttributes();
    p->setGuid("track", &track);
    p->setInt("kind", SQ_CLIP_AUDIO);
    p->setDouble("start", at);
    p->setDouble("length", len);
    p->setString("source", path.c_str());
    auto res = sq::makeAttributes();
    if (SQ_FAILED(ctx_.exec(sq::doc::kAddClip, p.get(), res.get()))) {
      scope.cancel();
      return false;
    }
    res->getGuid("node", &clip);
  }
  ctx_.selectTrack(track);
  if (!sq_guid_is_null(&clip)) ctx_.setSelection({clip});
  ctx_.setStatus("imported " + stemOf(path));
  return true;
}

void ArrangeView::dragOver(float x, float y) {
  dropX_ = x;
  dropY_ = y;
  ctx_.invalidate();
}

// ---- menus ------------------------------------------------------------------------------------------
std::vector<MenuItem> ArrangeView::colorItems(std::function<void(uint32_t)> apply) {
  std::vector<MenuItem> items;
  MenuItem def;
  def.label = "Default";
  def.action = [apply] { apply(0); };
  items.push_back(def);
  items.push_back(MenuItem::sep());
  const auto& cols = ctx_.palette().trackColors;
  for (size_t i = 0; i < cols.size(); ++i) {
    MenuItem m;
    m.label = i < std::size(kColorNames) ? kColorNames[i] : "Color " + std::to_string(i + 1);
    const uint32_t v = packColor(cols[i]);
    m.action = [apply, v] { apply(v); };
    items.push_back(m);
  }
  return items;
}

void ArrangeView::clipMenu(int lane, int clip, Point at) {
  const ClipEntry& c = lanes_[static_cast<size_t>(lane)].clips[static_cast<size_t>(clip)];
  if (!selected(c.v.id)) {
    ctx_.setSelection({c.v.id});
    es_ = ctx_.editState();
  }
  GuiContext& ctx = ctx_;
  const sq_guid id = c.v.id;
  const bool midi = c.v.info.kind == SQ_CLIP_MIDI;
  const bool muted = (c.v.info.flags & SQ_CLIP_MUTED) != 0;
  std::vector<sq_guid> sel = es_.selection;
  auto cmd = [&ctx](const char* label, const char* cmdName, const char* shortcut = "") {
    MenuItem m;
    m.label = label;
    m.shortcut = shortcut;
    std::string cm = cmdName;
    m.action = [&ctx, cm] { ctx.run(cm); };
    return m;
  };
  std::vector<MenuItem> m;
  MenuItem open;
  open.label = "Open in Editor";
  open.shortcut = "Dbl-click";
  open.action = [&ctx, id] { ctx.openClipEditor(id); };
  m.push_back(open);
  MenuItem ren;
  ren.label = "Rename...";
  ren.action = [this, id] {
    if (const ClipEntry* ce = findClip(id)) renameClip(*ce);
  };
  m.push_back(ren);
  MenuItem col;
  col.label = "Color";
  col.submenu = colorItems([&ctx, sel](uint32_t v) {
    EditScope scope(ctx, "Clip Color");
    for (const auto& g : sel) {
      auto p = sq::makeAttributes();
      p->setGuid("node", &g);
      p->setString("key", "color");
      p->setInt("value", v);
      ctx.exec(sq::doc::kSetProperty, p.get());
    }
  });
  m.push_back(col);
  MenuItem mute;
  mute.label = muted ? "Unmute" : "Mute";
  mute.action = [&ctx, sel, muted] {
    EditScope scope(ctx, muted ? "Unmute Clips" : "Mute Clips");
    for (const auto& g : sel) ctx.setPropertyBool(g, "muted", !muted);
  };
  m.push_back(mute);
  m.push_back(MenuItem::sep());
  m.push_back(cmd("Duplicate", "Edit.Duplicate", "Ctrl+D"));
  m.push_back(cmd("Split at Cursor", "Edit.Split", "S"));
  m.push_back(cmd("Delete", "Edit.Delete", "Del"));
  m.push_back(MenuItem::sep());
  if (midi) {
    m.push_back(cmd("Quantize", "Edit.Quantize", "Q"));
    for (int semis : {12, -12}) {
      MenuItem t;
      t.label = semis > 0 ? "Transpose +12" : "Transpose -12";
      t.action = [&ctx, sel, semis] {
        EditScope scope(ctx, "Transpose");
        for (const auto& g : sel) {
          auto p = sq::makeAttributes();
          p->setGuid("clip", &g);
          p->setInt("semitones", semis);
          ctx.exec(edoc::kTransposeClip, p.get());
        }
      };
      m.push_back(t);
    }
  } else {
    m.push_back(cmd("Normalize", "Edit.Normalize"));
    MenuItem gain;
    gain.label = "Gain";
    gain.submenu = {cmd("Gain +1 dB", "Edit.GainUp"), cmd("Gain -1 dB", "Edit.GainDown")};
    MenuItem reset;
    reset.label = "Reset Gain";
    reset.action = [&ctx, sel] {
      EditScope scope(ctx, "Reset Gain");
      for (const auto& g : sel) ctx.setProperty(g, "gainDb", 0.0);
    };
    gain.submenu.push_back(reset);
    m.push_back(gain);
    MenuItem fades;
    fades.label = "Fades";
    fades.submenu = {cmd("Fade In to Cursor", "Edit.FadeInToCursor"), cmd("Fade Out from Cursor", "Edit.FadeOutFromCursor"),
                     cmd("Crossfade", "Edit.Crossfade")};
    MenuItem nf;
    nf.label = "Remove Fades";
    nf.action = [&ctx, sel] {
      EditScope scope(ctx, "Remove Fades");
      for (const auto& g : sel) {
        ctx.setProperty(g, "fadeIn", 0.0);
        ctx.setProperty(g, "fadeOut", 0.0);
      }
    };
    fades.submenu.push_back(nf);
    m.push_back(fades);
    m.push_back(cmd("Strip Silence", "Edit.StripSilence"));
  }
  ctx_.showMenu(std::move(m), at);
}

void ArrangeView::trackMenu(int lane, Point at) {
  const Lane& l = lanes_[static_cast<size_t>(lane)];
  ctx_.selectTrack(l.t.id);
  GuiContext& ctx = ctx_;
  const sq_guid id = l.t.id;
  std::vector<MenuItem> m;
  MenuItem ren;
  ren.label = "Rename...";
  ren.shortcut = "Dbl-click";
  ren.action = [this, id] {
    const int i = laneIndex(id);
    if (i >= 0) renameTrack(lanes_[static_cast<size_t>(i)]);
  };
  m.push_back(ren);
  MenuItem col;
  col.label = "Color";
  col.submenu = colorItems([&ctx, id](uint32_t v) {
    auto p = sq::makeAttributes();
    p->setGuid("node", &id);
    p->setString("key", "color");
    p->setInt("value", v);
    ctx.exec(sq::doc::kSetProperty, p.get());
  });
  m.push_back(col);
  MenuItem dup;
  dup.label = "Duplicate Track";
  dup.action = [this, id] {
    const int i = laneIndex(id);
    if (i >= 0) duplicateTrack(lanes_[static_cast<size_t>(i)]);
  };
  m.push_back(dup);
  MenuItem del;
  del.label = "Delete Track";
  del.action = [this, id] {
    const int i = laneIndex(id);
    if (i >= 0) deleteTrack(lanes_[static_cast<size_t>(i)]);
  };
  m.push_back(del);
  m.push_back(MenuItem::sep());
  MenuItem autoItem;
  autoItem.label = "Show Automation";
  autoItem.checked = l.showAuto;
  autoItem.action = [this, id] {
    const int i = laneIndex(id);
    if (i >= 0) toggleTrackFlag(lanes_[static_cast<size_t>(i)], kBtnAuto);
  };
  m.push_back(autoItem);
  MenuItem h;
  h.label = "Track Height";
  for (auto [nm, px] : {std::pair<const char*, float>{"Small", 34.0f}, {"Medium", kDefaultLaneH}, {"Large", 110.0f}, {"Huge", 180.0f}}) {
    MenuItem it;
    it.label = nm;
    it.checked = std::fabs(l.h - px) < 0.5f;
    const float v = px;
    it.action = [this, id, v] {
      laneHeights_[id] = v;
      relayout();
      ctx_.invalidate();
    };
    h.submenu.push_back(it);
  }
  m.push_back(h);
  m.push_back(MenuItem::sep());
  MenuItem aa;
  aa.label = "Add Audio Track";
  aa.action = [&ctx] { ctx.run("Track.AddAudio"); };
  m.push_back(aa);
  MenuItem ai;
  ai.label = "Add Instrument Track";
  ai.action = [&ctx] { ctx.run("Track.AddInstrument"); };
  m.push_back(ai);
  ctx_.showMenu(std::move(m), at);
}

void ArrangeView::laneMenu(int lane, double ppq, Point at) {
  GuiContext& ctx = ctx_;
  std::vector<MenuItem> m;
  const double pos = snap(ppq, 0);
  if (lane >= 0) {
    const Lane& l = lanes_[static_cast<size_t>(lane)];
    ctx_.selectTrack(l.t.id);
    if (l.instrument()) {
      const sq_guid id = l.t.id;
      MenuItem mc;
      mc.label = "Create MIDI Clip";
      mc.shortcut = "Dbl-click";
      mc.action = [this, id, ppq] {
        const int i = laneIndex(id);
        if (i >= 0) createMidiClip(lanes_[static_cast<size_t>(i)], ppq);
      };
      m.push_back(mc);
    }
  }
  MenuItem cur;
  cur.label = "Set Cursor Here";
  cur.action = [this, pos] { locate(pos); };
  m.push_back(cur);
  MenuItem mk;
  mk.label = "Insert Marker";
  mk.action = [this, pos] { addMarkerAt(pos); };
  m.push_back(mk);
  MenuItem all;
  all.label = "Select All";
  all.shortcut = "Ctrl+A";
  all.action = [&ctx] { ctx.run("Navigate.SelectAll"); };
  m.push_back(all);
  m.push_back(MenuItem::sep());
  MenuItem aa;
  aa.label = "Add Audio Track";
  aa.action = [&ctx] { ctx.run("Track.AddAudio"); };
  m.push_back(aa);
  MenuItem ai;
  ai.label = "Add Instrument Track";
  ai.action = [&ctx] { ctx.run("Track.AddInstrument"); };
  m.push_back(ai);
  ctx_.showMenu(std::move(m), at);
}

void ArrangeView::markerMenu(int marker, Point at) {
  const sq_doc_marker mk = markers_[static_cast<size_t>(marker)].m;
  GuiContext& ctx = ctx_;
  const sq_guid id = mk.id;
  const std::string nm = mk.name;
  const double pos = mk.position;
  std::vector<MenuItem> m;
  MenuItem go;
  go.label = "Go to Marker";
  go.action = [this, pos] { locate(pos); };
  m.push_back(go);
  MenuItem ren;
  ren.label = "Rename...";
  ren.action = [&ctx, id, nm] {
    ctx.prompt("Rename Marker", nm, [&ctx, id](const std::string& n) { ctx.setPropertyString(id, "name", n); });
  };
  m.push_back(ren);
  MenuItem del;
  del.label = "Delete Marker";
  del.action = [&ctx, id] {
    auto p = sq::makeAttributes();
    p->setGuid("node", &id);
    ctx.exec(sq::doc::kRemoveNode, p.get());
  };
  m.push_back(del);
  ctx_.showMenu(std::move(m), at);
}

void ArrangeView::addTrackMenu(Point at) {
  GuiContext& ctx = ctx_;
  std::vector<MenuItem> m;
  for (auto [label, cmdName] : {std::pair<const char*, const char*>{"Audio Track", "Track.AddAudio"},
                                {"Instrument Track", "Track.AddInstrument"},
                                {"Drum Track", "Track.AddDrums"},
                                {"Bus / FX Track", "Track.AddBus"}}) {
    MenuItem it;
    it.label = label;
    std::string c = cmdName;
    it.action = [this, &ctx, c] {
      ctx.run(c);
      refresh();
      scrollY_ = contentH_;  // reveal the new track
      clampScroll();
    };
    m.push_back(it);
  }
  ctx_.showMenu(std::move(m), at);
}

void ArrangeView::gridMenu(Point at) {
  struct G {
    const char* label;
    double ppq;
  };
  const double bar = barLength(0);
  const G grids[] = {{"Bar", bar},   {"1/2", 2.0},         {"1/4", 1.0},         {"1/8", 0.5},          {"1/16", 0.25},
                     {"1/32", 0.125}, {"1/4 T", 2.0 / 3.0}, {"1/8 T", 1.0 / 3.0}, {"1/16 T", 1.0 / 6.0}};
  std::vector<MenuItem> m;
  for (size_t i = 0; i < std::size(grids); ++i) {
    if (i == 6) m.push_back(MenuItem::sep());
    MenuItem it;
    it.label = grids[i].label;
    it.checked = std::fabs(es_.grid - grids[i].ppq) < 1e-6;
    const double v = grids[i].ppq;
    it.action = [this, v] { ctx_.setGrid(v); };
    m.push_back(it);
  }
  ctx_.showMenu(std::move(m), at);
}

void ArrangeView::autoTargetMenu(int lane, int autoLane, Point at) {
  const Lane& l = lanes_[static_cast<size_t>(lane)];
  const sq_guid id = l.t.id;
  const std::string cur = l.autos[static_cast<size_t>(autoLane)].target;
  const bool stored = l.autos[static_cast<size_t>(autoLane)].stored;
  std::vector<MenuItem> m;
  std::vector<std::string> targets = {"volume", "pan", "mute"};
  // sends of the track
  {
    const std::string sends = ctx_.trackString(id, "user.sends");
    int n = 0;
    for (char ch : sends)
      if (ch == '\n') ++n;
    if (!sends.empty() && sends.back() != '\n') ++n;
    for (int i = 0; i < n; ++i) targets.push_back("send." + std::to_string(i));
  }
  for (const auto& tg : targets) {
    bool shown = false;
    for (const auto& a : l.autos) shown = shown || a.target == tg;
    MenuItem it;
    it.label = "Show " + autoLabel(tg);
    it.checked = shown;
    it.enabled = !shown;
    it.action = [this, id, tg] {
      auto& extra = autoExtra_[id];
      if (std::find(extra.begin(), extra.end(), tg) == extra.end()) extra.push_back(tg);
      rebuild();
      ctx_.invalidate();
    };
    m.push_back(it);
  }
  m.push_back(MenuItem::sep());
  MenuItem clear;
  clear.label = "Clear Points";
  clear.enabled = stored;
  clear.action = [this, id, cur] { setAutomation(id, cur, {}); };
  m.push_back(clear);
  MenuItem hide;
  hide.label = "Hide Lane";
  hide.action = [this, id, cur] {
    auto& extra = autoExtra_[id];
    extra.erase(std::remove(extra.begin(), extra.end(), cur), extra.end());
    rebuild();
    ctx_.invalidate();
  };
  hide.enabled = !stored;
  m.push_back(hide);
  ctx_.showMenu(std::move(m), at);
}

// ---- toolbar ----------------------------------------------------------------------------------------
void ArrangeView::toolbarClick(int id, Point at) {
  switch (id) {
    case kTPointer: tool_ = Tool::Pointer; break;
    case kTSplit: tool_ = Tool::Split; break;
    case kTDraw: tool_ = Tool::Draw; break;
    case kTSnap: {
      auto a = sq::makeAttributes();
      a->setString("snap", snapOn() ? "off" : "grid");
      ctx_.shell().execute("Navigate", "SetEditState", a.get());
      break;
    }
    case kTGrid: gridMenu(at); break;
    case kTZoomOut: setZoom(ppx_ / 1.5, lanesRect().x + lanesRect().w / 2); break;
    case kTZoomIn: setZoom(ppx_ * 1.5, lanesRect().x + lanesRect().w / 2); break;
    case kTFit: zoomToFit(); break;
    case kTFollow: follow_ = !follow_; break;
    case kTAddTrack: addTrackMenu(at); break;
    default: break;
  }
  ctx_.invalidate();
}

// ---- keyboard ---------------------------------------------------------------------------------------
bool ArrangeView::onKey(Key k, unsigned mods) {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0;
  refresh();
  switch (k) {
    case Key::Escape:
      if (drag_ != Drag::None) {
        drag_ = Drag::None;
        ctx_.invalidate();
        return true;
      }
      if (!es_.selection.empty()) {
        ctx_.setSelection({});
        return true;
      }
      return false;
    case Key::Delete:
    case Key::Backspace: ctx_.run("Edit.Delete"); return true;
    case Key::Left:
    case Key::Right: {
      const bool right = k == Key::Right;
      if (ctrl) ctx_.run(right ? "Edit.NudgeRight" : "Edit.NudgeLeft");
      else ctx_.run(right ? "Navigate.CursorNextGrid" : "Navigate.CursorPreviousGrid");
      es_ = ctx_.editState();
      if (!ctrl) {
        if (ITransport* t = ctx_.transportIf()) {
          if (!(ctx_.transport().flags & SQ_TRANSPORT_PLAYING)) t->locate(es_.cursor);
        }
        ensureVisible(es_.cursor);
      }
      return true;
    }
    case Key::Up:
    case Key::Down:
      if (shift && !es_.selection.empty()) ctx_.run(k == Key::Up ? "Edit.MoveToTrackAbove" : "Edit.MoveToTrackBelow");
      else ctx_.run(k == Key::Up ? "Navigate.PreviousTrack" : "Navigate.NextTrack");
      return true;
    case Key::Plus: setZoom(ppx_ * 1.5, xOf(es_.cursor)); return true;
    case Key::Minus: setZoom(ppx_ / 1.5, xOf(es_.cursor)); return true;
    case Key::PageUp: scrollY_ -= lanesRect().h * 0.8f; clampScroll(); ctx_.invalidate(); return true;
    case Key::PageDown: scrollY_ += lanesRect().h * 0.8f; clampScroll(); ctx_.invalidate(); return true;
    case Key::End: ensureVisible(songEnd_); locate(songEnd_); return true;
    default: return false;
  }
}

bool ArrangeView::onChar(uint32_t ch, unsigned mods) {
  if (mods & kModCtrl) {
    if (ch == 'D' - '@') {
      ctx_.run("Edit.Duplicate");
      return true;
    }
    return false;
  }
  switch (ch) {
    case 'f':
    case 'F': zoomToFit(); return true;
    case '+':
    case '=': setZoom(ppx_ * 1.5, xOf(es_.cursor)); return true;
    case '-':
    case '_': setZoom(ppx_ / 1.5, xOf(es_.cursor)); return true;
    case '1': tool_ = Tool::Pointer; ctx_.invalidate(); return true;
    case '2': tool_ = Tool::Split; ctx_.invalidate(); return true;
    case '3': tool_ = Tool::Draw; ctx_.invalidate(); return true;
    default: return false;
  }
}

}  // namespace arrange

// ---- public entry points ------------------------------------------------------------------------------
std::unique_ptr<Widget> makeArrange(GuiContext& ctx) { return std::make_unique<arrange::ArrangeView>(ctx); }

bool arrangeDropFile(Widget* w, float x, float y, const std::string& path) {
  auto* v = dynamic_cast<arrange::ArrangeView*>(w);
  if (!v || path.empty()) return false;
  return v->dropFile(x, y, path);
}

void arrangeDragOver(Widget* w, float x, float y) {
  if (auto* v = dynamic_cast<arrange::ArrangeView*>(w)) v->dragOver(x, y);
}



}  // namespace sq::app::gui
