// Squizzy Studio GUI - GuiContext document/engine helpers and shared drawing helpers.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gui_context.hpp"
#include "sq/engine/doc/document.hpp"
#include "sq/engine/edit/edit.hpp"
#include "sq/platform.hpp"

namespace sq::app::gui {
namespace {

std::string readName(INode* n) {
  char buf[512];
  uint32_t need = 0;
  if (!n) return {};
  sq_result r = n->getName(buf, sizeof buf, &need);
  if (r == SQ_OK) return buf;
  if (r == SQ_E_BUFFER_TOO_SMALL && need > 0) {
    std::string big(need, '\0');
    if (n->getName(big.data(), need, &need) == SQ_OK) {
      big.resize(std::strlen(big.c_str()));
      return big;
    }
  }
  return {};
}
std::string readSource(IClip* c) {
  char buf[1024];
  uint32_t need = 0;
  if (!c) return {};
  sq_result r = c->getSourcePath(buf, sizeof buf, &need);
  if (r == SQ_OK) return buf;
  if (r == SQ_E_BUFFER_TOO_SMALL && need > 0) {
    std::string big(need, '\0');
    if (c->getSourcePath(big.data(), need, &need) == SQ_OK) {
      big.resize(std::strlen(big.c_str()));
      return big;
    }
  }
  return {};
}

}  // namespace

sq::Ptr<IDocument> GuiContext::doc() { return shell().activeDocument(); }

std::vector<TrackView> GuiContext::tracks() {
  std::vector<TrackView> out;
  auto d = doc();
  if (!d) return out;
  sq_document_info di{};
  di.struct_size = sizeof di;
  d->getDocumentInfo(&di);
  out.reserve(di.track_count);
  for (uint32_t i = 0; i < di.track_count; ++i) {
    TrackView t;
    if (d->getTrack(i, t.track.put()) != SQ_OK || !t.track) continue;
    t.track->getId(&t.id);
    t.name = readName(t.track.get());
    t.info.struct_size = sizeof t.info;
    t.track->getTrackInfo(&t.info);
    t.index = i;
    out.push_back(std::move(t));
  }
  return out;
}

bool GuiContext::findTrack(const sq_guid& id, TrackView* out) {
  for (auto& t : tracks())
    if (t.id == id) {
      if (out) *out = std::move(t);
      return true;
    }
  return false;
}

std::vector<ClipView> GuiContext::clips(const TrackView& t) {
  std::vector<ClipView> out;
  if (!t.track) return out;
  for (uint32_t k = 0; k < t.info.clip_count; ++k) {
    ClipView c;
    if (t.track->getClip(k, c.clip.put()) != SQ_OK || !c.clip) continue;
    c.clip->getId(&c.id);
    c.name = readName(c.clip.get());
    c.info.struct_size = sizeof c.info;
    c.clip->getClipInfo(&c.info);
    c.track = t.id;
    if (c.info.kind == SQ_CLIP_AUDIO) c.source = readSource(c.clip.get());
    out.push_back(std::move(c));
  }
  return out;
}

bool GuiContext::findClip(const sq_guid& id, ClipView* out) {
  auto d = doc();
  if (!d || sq_guid_is_null(&id)) return false;
  sq::Ptr<INode> n;
  if (d->findNode(&id, n.put()) != SQ_OK || !n || n->getKind() != SQ_NODE_CLIP) return false;
  sq::Ptr<IClip> c;
  if (n->queryInterface(&IClip::iid, c.putVoid()) != SQ_OK || !c) return false;
  if (!out) return true;
  out->clip = c;
  out->id = id;
  out->name = readName(c.get());
  out->info = sq_clip_info{};
  out->info.struct_size = sizeof out->info;
  c->getClipInfo(&out->info);
  out->source = out->info.kind == SQ_CLIP_AUDIO ? readSource(c.get()) : std::string();
  sq::Ptr<INode> parent;
  out->track = sq_guid{};
  if (c->getParent(parent.put()) == SQ_OK && parent) parent->getId(&out->track);
  return true;
}

std::vector<sq_clip_event> GuiContext::clipEvents(IClip* clip) {
  std::vector<sq_clip_event> out;
  if (!clip) return out;
  sq_clip_info ci{};
  ci.struct_size = sizeof ci;
  clip->getClipInfo(&ci);
  out.reserve(ci.event_count);
  for (uint32_t i = 0; i < ci.event_count; ++i) {
    sq_clip_event e{};
    e.struct_size = sizeof e;
    if (clip->getEvent(i, &e) == SQ_OK) out.push_back(e);
  }
  return out;
}

std::string GuiContext::trackString(const sq_guid& track, const char* key) {
  TrackView t;
  if (!findTrack(track, &t)) return {};
  auto props = sq::makeAttributes();
  if (t.track->getProperties(props.get()) != SQ_OK) return {};
  return sq::getAttrString(props.get(), key);
}

Color GuiContext::trackColor(const TrackView& t) const {
  if (t.info.color != 0 && (t.info.color & 0xFF) != 0) return Color::rgba(t.info.color);
  const auto& cols = palette().trackColors;
  if (cols.empty()) return palette().accent;
  return cols[t.index % cols.size()];
}

EditStateView GuiContext::editState() {
  EditStateView v;
  auto d = doc();
  if (!d) return v;
  auto* impl = edoc::DocumentImpl::from(d.get());
  if (!impl) return v;
  const auto st = sq::edit::readEditState(*impl);
  v.selection = st.selection;
  v.cursor = st.cursor;
  v.grid = st.grid;
  v.snap = st.snap;
  v.track = st.track;
  v.hasRange = st.hasRange;
  v.rangeFrom = st.rangeFrom;
  v.rangeTo = st.rangeTo;
  return v;
}

sq_guid GuiContext::selectedTrack() {
  sq_guid t = editState().track;
  if (sq_guid_is_null(&t)) {
    auto ts = tracks();
    if (!ts.empty()) t = ts.front().id;
  }
  return t;
}

void GuiContext::selectTrack(const sq_guid& track) {
  auto a = sq::makeAttributes();
  a->setGuid("track", &track);
  shell().execute("Navigate", "SetEditState", a.get());
  invalidate();
}

void GuiContext::setSelection(const std::vector<sq_guid>& clips) {
  auto a = sq::makeAttributes();
  std::string s;
  for (const auto& g : clips) {
    if (!s.empty()) s += ',';
    s += sq::toString(g);
  }
  a->setString("nodes", s.c_str());
  shell().execute("Navigate", "SetEditState", a.get());
  invalidate();
}

void GuiContext::setCursor(double ppq) {
  auto a = sq::makeAttributes();
  a->setDouble("cursor", std::max(0.0, ppq));
  shell().execute("Navigate", "SetEditState", a.get());
  invalidate();
}

void GuiContext::setGrid(double ppq) {
  if (!(ppq > 0)) return;
  auto a = sq::makeAttributes();
  a->setDouble("grid", ppq);
  shell().execute("Navigate", "SetEditState", a.get());
  invalidate();
}

sq_result GuiContext::exec(const sq_guid& actionClass, IAttributes* params, IAttributes* result) {
  auto d = doc();
  if (!d) return SQ_E_NOTFOUND;
  sq::Ptr<IEditor> ed;
  if (d->getEditor(ed.put()) != SQ_OK || !ed) return SQ_E_UNSUPPORTED;
  sq_result r = ed->execute(&actionClass, params, result);
  if (SQ_FAILED(r)) setStatus(std::string("edit failed: ") + sq_result_string(r));
  invalidate();
  return r;
}

sq_result GuiContext::setProperty(const sq_guid& node, const char* key, double value, bool merge) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setDouble("value", value);
  if (merge) p->setBool("merge", SQ_TRUE);
  return exec(sq::doc::kSetProperty, p.get());
}
sq_result GuiContext::setPropertyBool(const sq_guid& node, const char* key, bool value) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setBool("value", value ? SQ_TRUE : SQ_FALSE);
  return exec(sq::doc::kSetProperty, p.get());
}
sq_result GuiContext::setPropertyString(const sq_guid& node, const char* key, const std::string& value, bool merge) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setString("value", value.c_str());
  if (merge) p->setBool("merge", SQ_TRUE);
  return exec(sq::doc::kSetProperty, p.get());
}

bool GuiContext::beginEdit(const char* description) {
  auto d = doc();
  if (!d) return false;
  sq::Ptr<IEditor> ed;
  if (d->getEditor(ed.put()) != SQ_OK || !ed) return false;
  return SQ_SUCCEEDED(ed->beginTransaction(description));
}
void GuiContext::endEdit(bool commit) {
  auto d = doc();
  if (!d) return;
  sq::Ptr<IEditor> ed;
  if (d->getEditor(ed.put()) != SQ_OK || !ed) return;
  if (commit) ed->commitTransaction();
  else ed->cancelTransaction();
  invalidate();
}

ITransport* GuiContext::transportIf() {
  IAudioEngine* e = shell().engine();
  if (!e) return nullptr;
  sq::Ptr<ITransport> tr;
  e->queryInterface(&ITransport::iid, tr.putVoid());
  return tr.get();  // the engine keeps its own reference for its lifetime
}

sq_transport_state GuiContext::transport() {
  sq_transport_state ts{};
  ts.struct_size = sizeof ts;
  ts.bpm = 120;
  ts.numerator = 4;
  ts.denominator = 4;
  if (ITransport* t = transportIf()) t->getTransportState(&ts);
  return ts;
}

uint32_t GuiContext::meter(const sq_guid& track, float* peaks, float* rms, uint32_t capacity) {
  IAudioEngine* e = shell().engine();
  if (!e || capacity == 0) return 0;
  sq_meter_value mv[8]{};
  uint32_t n = 0;
  const sq_guid* id = sq_guid_is_null(&track) ? nullptr : &track;
  if (e->getMeter(id, mv, std::min<uint32_t>(capacity, 8), &n) != SQ_OK) return 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (peaks) peaks[i] = mv[i].peak;
    if (rms) rms[i] = mv[i].rms;
  }
  return n;
}

double GuiContext::ppqToSeconds(double ppq) {
  auto d = doc();
  sq::Ptr<ITempoMap> tm;
  if (d && d->getTempoMap(tm.put()) == SQ_OK && tm) return tm->ppqToSeconds(ppq);
  return ppq * 0.5;
}
double GuiContext::secondsToPpq(double seconds) {
  auto d = doc();
  sq::Ptr<ITempoMap> tm;
  if (d && d->getTempoMap(tm.put()) == SQ_OK && tm) return tm->secondsToPpq(seconds);
  return seconds * 2.0;
}

std::string GuiContext::formatBars(double ppq) {
  uint32_t num = 4, den = 4;
  auto d = doc();
  sq::Ptr<ITempoMap> tm;
  if (d && d->getTempoMap(tm.put()) == SQ_OK && tm && tm->getSignatureCount() > 0) {
    sq_time_signature s{};
    s.struct_size = sizeof s;
    if (tm->getSignature(0, &s) == SQ_OK && s.numerator && s.denominator) {
      num = s.numerator;
      den = s.denominator;
    }
  }
  const double beatLen = 4.0 / den;  // ppq per beat
  if (ppq < 0) ppq = 0;
  const double beatsTotal = ppq / beatLen;
  const auto beatIdx = static_cast<int64_t>(std::floor(beatsTotal + 1e-9));
  const int64_t bar = beatIdx / num + 1;
  const int64_t beat = beatIdx % num + 1;
  const int tick = std::clamp(static_cast<int>((beatsTotal - static_cast<double>(beatIdx)) * 960.0), 0, 959);
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld.%lld.%03d", static_cast<long long>(bar), static_cast<long long>(beat), tick);
  return buf;
}

std::string GuiContext::formatTime(double seconds) {
  if (seconds < 0) seconds = 0;
  const auto ms = static_cast<int64_t>(std::llround(seconds * 1000.0));
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld:%02lld.%03lld", static_cast<long long>(ms / 60000), static_cast<long long>((ms / 1000) % 60),
                static_cast<long long>(ms % 1000));
  return buf;
}

// ---- drawing helpers ----------------------------------------------------------------------------
namespace draw {

Color mix(Color a, Color b, float t) { return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; }
Color lighten(Color a, float amount) { return mix(a, Color{1, 1, 1, a.a}, amount); }

void button(Canvas& c, const Palette& p, const Rect& r, std::string_view label, bool hot, bool on, Color onColor) {
  Color bg = on ? onColor : (hot ? p.panel3 : p.panel2);
  c.fillRoundRect(r, 4, bg);
  if (!on) c.strokeRoundRect(r, 4, p.border, 1);
  const Color fg = on ? (p.dark ? Color::rgb(0x111214) : Color::rgb(0xFFFFFF)) : p.text;
  c.drawText(label, r, fg, 12, TextAlign::Center);
}

void knob(Canvas& c, const Palette& p, const Rect& r, double normalized, bool bipolar, Color accent) {
  const float d = std::min(r.w, r.h);
  const Point ctr{r.x + r.w / 2, r.y + r.h / 2};
  const float rad = d / 2 - 3;
  if (rad <= 2) return;
  const float start = -135.0f, sweep = 270.0f;
  const float v = static_cast<float>(std::clamp(normalized, 0.0, 1.0));
  c.fillEllipse(Rect{ctr.x - rad + 3, ctr.y - rad + 3, 2 * rad - 6, 2 * rad - 6}, p.panel3);
  c.drawArc(ctr, rad, start, sweep, p.border, 3);
  if (bipolar) c.drawArc(ctr, rad, 0.0f, (v - 0.5f) * sweep, accent, 3);
  else c.drawArc(ctr, rad, start, v * sweep, accent, 3);
  const float a = (start + v * sweep) * 3.14159265f / 180.0f;
  c.drawLine(Point{ctr.x + std::sin(a) * rad * 0.2f, ctr.y - std::cos(a) * rad * 0.2f},
             Point{ctr.x + std::sin(a) * (rad - 5), ctr.y - std::cos(a) * (rad - 5)}, p.text, 2);
}

void meter(Canvas& c, const Palette& p, const Rect& r, float peakLinear, float holdLinear) {
  c.fillRect(r, p.border);
  auto norm = [](float lin) {
    if (lin <= 0.0f) return 0.0f;
    const float db = 20.0f * std::log10(lin);
    return std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);  // -60..+6 dB
  };
  const float v = norm(peakLinear);
  const float h = r.h * v;
  const float yZero = r.y + r.h * (1.0f - norm(1.0f));
  Rect bar{r.x, r.y + r.h - h, r.w, h};
  c.fillRect(bar, p.meterLow);
  const float yWarn = r.y + r.h * (1.0f - norm(0.5f));  // -6 dB
  if (bar.y < yWarn) c.fillRect(Rect{r.x, bar.y, r.w, yWarn - bar.y}, p.meterMid);
  if (bar.y < yZero) c.fillRect(Rect{r.x, bar.y, r.w, yZero - bar.y}, p.meterHigh);
  if (holdLinear > 0) {
    const float hy = r.y + r.h * (1.0f - norm(holdLinear));
    c.fillRect(Rect{r.x, hy, r.w, 1.5f}, holdLinear >= 1.0f ? p.meterHigh : p.text);
  }
}

void triangle(Canvas& c, const Rect& r, int direction, Color col) {
  std::vector<Point> p;
  switch (direction & 3) {
    case 0: p = {Point{r.x, r.bottom()}, Point{r.x + r.w / 2, r.y}, Point{r.right(), r.bottom()}}; break;
    case 1: p = {Point{r.x, r.y}, Point{r.right(), r.y + r.h / 2}, Point{r.x, r.bottom()}}; break;
    case 2: p = {Point{r.x, r.y}, Point{r.right(), r.y}, Point{r.x + r.w / 2, r.bottom()}}; break;
    default: p = {Point{r.right(), r.y}, Point{r.right(), r.bottom()}, Point{r.x, r.y + r.h / 2}}; break;
  }
  c.fillPolygon(p, col);
}

void check(Canvas& c, const Rect& r, Color col) {
  c.strokePolyline({Point{r.x, r.y + r.h * 0.55f}, Point{r.x + r.w * 0.38f, r.bottom()}, Point{r.right(), r.y}}, col, 1.8f);
}

void textLeft(Canvas& c, const Rect& r, std::string_view s, Color col, float size) { c.drawText(s, r, col, size, TextAlign::Left); }
void textCenter(Canvas& c, const Rect& r, std::string_view s, Color col, float size) { c.drawText(s, r, col, size, TextAlign::Center); }

}  // namespace draw
}  // namespace sq::app::gui
