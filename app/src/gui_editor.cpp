// Squizzy Studio GUI - clip editor panel: the piano roll for MIDI clips, an inspector for audio
// clips, and an empty state with "Create MIDI Clip" when nothing is open.
//
// Piano roll layout (all regions computed from the frame in layoutRegions()):
//   header   clip/track, tools, grid/snap, note length, velocity, quantize, legato, transpose, fold,
//            follow, zoom
//   corner | ruler (bars.beats, clip end marker, loop range, playhead)
//   keys   | note grid                                                               | v-scroll
//   ------ divider (drag to resize the velocity lane) -------------------------------------------
//   label  | velocity lane
//          | h-scroll
//
// Editing model: the clip's events are cached per document serial; the notes are a working copy
// (gui_editor_notes.hpp). A gesture edits the working copy live (preview) and commits ONE
// kSetClipEvents with the full event list on mouse up, so every gesture is one undo step.
// Selection is local to the editor and survives document reloads by (position, pitch) matching.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gui_editor.hpp"
#include "gui_editor_notes.hpp"
#include "gui_panels.hpp"
#include "sq/engine/doc/actions.hpp"

namespace sq::app::gui {
namespace {

using pr::Note;
using Clock = std::chrono::steady_clock;

constexpr float kHeaderH = 34, kRulerH = 22, kKeysW = 62, kDrumKeysW = 92, kSbW = 11, kDivH = 6;
constexpr int kGridDiv[] = {0, 2, 4, 8, 16, 32, 64};  // 0 = bar
constexpr const char* kGridNames[] = {"Bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"};
constexpr int kGridCount = 7;
// Note length choices: Grid, Last, then fixed.
constexpr int kLenDiv[] = {-1, -2, 64, 32, 16, 8, 4, 2, 0};
constexpr const char* kLenNames[] = {"Grid", "Last", "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "Bar"};
constexpr int kLenCount = 9;

enum class Tool { Select, Draw, Erase };
enum class Drag {
  None, Move, ResizeL, ResizeR, Draw, Rubber, Erase, Velocity, VelPaint, VelSplit, ScrollH, ScrollV, Keys, ClipEnd, HeaderVel
};
enum Hdr : int {
  kHToolSelect = 1, kHToolDraw, kHToolErase, kHGrid, kHTriplet, kHSnap, kHLength, kHVel, kHQuantize, kHQStrength, kHLegato,
  kHTransDown, kHTransUp, kHFold, kHFollow, kHZoomOut, kHZoomIn, kHZoomFit, kHCreateClip, kHAddTrack, kHMore
};
// Header control groups, left to right. Groups that do not fit the panel go into the "..." menu.
enum HGroup : int { kGTools, kGGrid, kGLength, kGVel, kGQuantize, kGTranspose, kGView, kGCount };
enum Zone { kZoneBody = 0, kZoneLeft = 1, kZoneRight = 2 };

struct HitBox {
  Rect r;
  int id;
};

Color contrastText(const Palette& p, Color bgc) {
  const float lum = 0.299f * bgc.r + 0.587f * bgc.g + 0.114f * bgc.b;
  const Color darkC = p.dark ? p.bg : p.text;
  const Color lightC = p.dark ? p.text : p.bg;
  return lum > 0.55f ? darkC : lightC;
}

class ClipEditor final : public Widget, public CharInput, public EditTarget {
 public:
  explicit ClipEditor(GuiContext& ctx) : Widget("ClipEditor"), ctx_(ctx), audio_(makeAudioInspector(ctx)) {
    hitTestable = true;
    rowOf_.fill(-1);
  }

  bool focusable() const override { return true; }

  // ---- paint ------------------------------------------------------------------------------------
  void paint(Canvas& c) override {
    sync();
    flushPreview(false);
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.bg);
    if (f.w < 40 || f.h < 30) return;
    c.pushClip(f);
    hits_.clear();
    if (!haveClip_) paintEmpty(c);
    else if (clip_.info.kind == SQ_CLIP_AUDIO) {
      paintHeader(c, false);
      audio_->paint(c, Rect{f.x, f.y + kHeaderH, f.w, f.h - kHeaderH}, clip_, trackCol_);
    } else {
      layoutRegions();
      if (needFit_ && grid_.w > 20 && grid_.h > 20) fitView(true);
      clampScroll();
      followPlayhead();
      paintHeader(c, true);
      paintRuler(c);
      paintGrid(c);
      paintKeys(c);
      paintVelocity(c);
      paintScrollbars(c);
    }
    c.popClip();
  }

  // ---- mouse ------------------------------------------------------------------------------------
  bool onMouse(const WidgetMouse& e) override {
    const Rect& f = frame();
    if (e.action == MouseAction::Hover) {
      if (!f.contains(e.x, e.y)) return false;
      hover(e);
      return true;
    }
    if (!haveClip_) return emptyMouse(e);
    if (clip_.info.kind == SQ_CLIP_AUDIO) {
      if (e.action == MouseAction::Down && headerHit(e.x, e.y) >= 0) return true;
      const bool r = audio_->mouse(e, clip_);
      if (r) ctx_.invalidate();
      return r || f.contains(e.x, e.y);
    }
    layoutRegions();
    switch (e.action) {
      case MouseAction::Wheel: return wheel(e);
      case MouseAction::Down: return down(e);
      case MouseAction::Move: return move(e);
      case MouseAction::Up: return up(e);
      case MouseAction::DoubleClick: return doubleClick(e);
      default: return false;
    }
  }

  // ---- keyboard ---------------------------------------------------------------------------------
  bool onKey(Key k, unsigned mods) override {
    if (!haveClip_ || clip_.info.kind != SQ_CLIP_MIDI) return false;
    const bool shift = (mods & kModShift) != 0, ctrl = (mods & kModCtrl) != 0;
    switch (k) {
      case Key::Delete:
      case Key::Backspace:
        if (anySelected()) deleteSelected();  // never falls through to Edit.Delete (would delete the clip)
        return true;
      case Key::Escape:
        if (!anySelected()) return false;
        for (Note& n : notes_) n.sel = false;
        ctx_.invalidate();
        return true;
      case Key::Up:
      case Key::Down: {
        if (!anySelected()) return false;
        const int dir = k == Key::Up ? 1 : -1;
        if (ctrl) changeVelocity(dir * (shift ? 1 : 8));
        else transpose(dir * (shift ? 12 : 1), true);
        return true;
      }
      case Key::Left:
      case Key::Right: {
        if (!anySelected()) return false;
        const double step = shift ? gridStep() / 4 : gridStep();
        nudge(k == Key::Right ? step : -step);
        return true;
      }
      default: return false;
    }
  }

  bool onChar(uint32_t ch, unsigned mods) override {
    if (!haveClip_ || clip_.info.kind != SQ_CLIP_MIDI) return false;
    if (mods & kModCtrl) {
      switch (ch) {
        case 'A' - '@': selectAll(); return true;
        case 'D' - '@':
          if (!anySelected()) return true;
          duplicateSelection();
          return true;
        case 'C' - '@':
          if (!anySelected()) return true;
          copySelection();
          return true;
        case 'X' - '@':
          if (!anySelected()) return true;
          copySelection();
          deleteSelected();
          return true;
        case 'V' - '@':
          if (clipboard_.empty()) return false;
          paste();
          return true;
        default: return false;
      }
    }
    switch (ch) {
      case 'q': quantizeNotes(); return true;
      case '1': tool_ = Tool::Select; ctx_.invalidate(); return true;
      case '2': tool_ = Tool::Draw; ctx_.invalidate(); return true;
      case '3': tool_ = Tool::Erase; ctx_.invalidate(); return true;
      default: return false;
    }
  }

  // Edit menu commands while the piano roll has focus: they act on the notes, never on the clip.
  bool editCommand(std::string_view cmd) override {
    if (!haveClip_ || clip_.info.kind != SQ_CLIP_MIDI) return false;
    const bool any = anySelected();
    if (cmd == "Copy" || cmd == "Cut") {
      if (!any) {
        ctx_.setStatus("No notes selected");
        return true;
      }
      copySelection();
      if (cmd == "Cut") deleteSelected();
      return true;
    }
    if (cmd == "Paste") {
      if (clipboard_.empty()) return false;  // nothing copied here: the arrangement clipboard may apply
      paste();
      return true;
    }
    if (cmd == "Delete") {
      if (any) deleteSelected();
      else ctx_.setStatus("No notes selected");
      return true;
    }
    if (cmd == "Duplicate") {
      if (any) duplicateSelection();
      else ctx_.setStatus("No notes selected");
      return true;
    }
    if (cmd == "SelectAll") {
      selectAll();
      return true;
    }
    if (cmd == "SelectNone") {
      for (Note& n : notes_) n.sel = false;
      ctx_.invalidate();
      return true;
    }
    if (cmd == "Quantize") {
      quantizeNotes();
      return true;
    }
    return false;
  }

 private:
  // ==== document cache ===========================================================================
  void sync() {
    const sq_guid want = ctx_.editorClip();
    const bool clipChanged = !(want == clipId_);
    if (!clipChanged && loadedSerial_ == ctx_.docSerial()) return;
    if (!clipChanged && drag_ != Drag::None && drag_ != Drag::HeaderVel) return;  // keep the live preview
    loadedSerial_ = ctx_.docSerial();
    // Remember the selection by (position, pitch) to restore it after the reload.
    std::vector<std::pair<double, int>> keys;
    if (!clipChanged)
      for (const Note& n : notes_)
        if (n.sel) keys.emplace_back(n.pos, n.pitch);
    if (clipChanged) {
      clipId_ = want;
      needFit_ = true;
      drag_ = Drag::None;
      audio_->reset();
    }
    haveClip_ = ctx_.findClip(clipId_, &clip_);
    events_.clear();
    notes_.clear();
    if (!haveClip_) return;
    TrackView t;
    trackName_.clear();
    trackCol_ = ctx_.palette().accent2;
    const bool wasDrums = drums_;
    drums_ = false;
    if (ctx_.findTrack(clip_.track, &t)) {
      trackName_ = t.name;
      trackCol_ = ctx_.trackColor(t);
      drums_ = t.info.kind == SQ_TRACK_INSTRUMENT && ctx_.trackString(clip_.track, "user.instrument").rfind("builtin|drums", 0) == 0;
    }
    if (drums_ != wasDrums) needFit_ = true;
    if (clip_.info.color != 0 && (clip_.info.color & 0xFF) != 0) trackCol_ = Color::rgba(clip_.info.color);
    readSignature();
    if (clip_.info.kind != SQ_CLIP_MIDI) return;
    events_ = ctx_.clipEvents(clip_.clip.get());
    notes_ = pr::extractNotes(events_);
    for (Note& n : notes_)
      for (const auto& k : keys)
        if (k.second == n.pitch && std::fabs(k.first - n.pos) < 1e-6) n.sel = true;
    buildRows();
  }

  void readSignature() {
    num_ = 4;
    den_ = 4;
    auto d = ctx_.doc();
    sq::Ptr<ITempoMap> tm;
    if (d && d->getTempoMap(tm.put()) == SQ_OK && tm && tm->getSignatureCount() > 0) {
      sq_time_signature s{};
      s.struct_size = sizeof s;
      if (tm->getSignature(0, &s) == SQ_OK && s.numerator > 0 && s.denominator > 0) {
        num_ = static_cast<int>(s.numerator);
        den_ = static_cast<int>(s.denominator);
      }
    }
  }
  double beatLen() const { return 4.0 / den_; }
  double barLen() const { return beatLen() * num_; }
  double clipStart() const { return clip_.info.start; }
  double clipLen() const { return drag_ == Drag::ClipEnd ? dragClipLen_ : clip_.info.length; }

  void buildRows() {
    rows_.clear();
    rowOf_.fill(-1);
    if (fold_) {
      std::array<bool, 128> used{};
      for (const Note& n : notes_) used[static_cast<size_t>(std::clamp(n.pitch, 0, 127))] = true;
      if (drums_)  // a drum lane per kit piece, used or not
        for (int pch = pr::kDrumLo; pch <= pr::kDrumHi; ++pch)
          if (pr::drumName(pch)) used[static_cast<size_t>(pch)] = true;
      for (int pch = 127; pch >= 0; --pch)
        if (used[static_cast<size_t>(pch)]) rows_.push_back(pch);
    }
    if (rows_.empty())
      for (int pch = 127; pch >= 0; --pch) rows_.push_back(pch);
    for (size_t r = 0; r < rows_.size(); ++r) rowOf_[static_cast<size_t>(rows_[r])] = static_cast<int>(r);
  }
  bool folded() const { return rows_.size() < 128; }
  // Flat rows (no piano keyboard): folded view, or a drum kit (rows are named pieces).
  bool flatKeys() const { return folded() || drums_; }
  float keysW() const { return drums_ ? kDrumKeysW : kKeysW; }
  std::string pitchName(int pch) const {
    if (drums_)
      if (const char* d = pr::drumName(pch)) return d;
    return pr::noteName(pch);
  }

  // ==== geometry ===================================================================================
  void layoutRegions() {
    const Rect& f = frame();
    header_ = Rect{f.x, f.y, f.w, kHeaderH};
    const float top = f.y + kHeaderH;
    const float kw = keysW();
    const float gw = std::max(10.0f, f.w - kw - kSbW);
    const float avail = f.bottom() - top - kRulerH - kSbW - kDivH;
    velH_ = std::clamp(velH_, 24.0f, std::max(24.0f, avail - 40.0f));
    float vh = velH_;
    if (avail - vh < 40) vh = std::max(0.0f, avail - 40);
    ruler_ = Rect{f.x + kw, top, gw, kRulerH};
    corner_ = Rect{f.x, top, kw, kRulerH};
    hbar_ = Rect{f.x + kw, f.bottom() - kSbW, gw, kSbW};
    vel_ = Rect{f.x + kw, hbar_.y - vh, gw, vh};
    velLabel_ = Rect{f.x, vel_.y, kw, vh};
    div_ = Rect{f.x, vel_.y - kDivH, f.w, kDivH};
    grid_ = Rect{f.x + kw, ruler_.bottom(), gw, std::max(0.0f, div_.y - ruler_.bottom())};
    keys_ = Rect{f.x, grid_.y, kw, grid_.h};
    vbar_ = Rect{grid_.right(), grid_.y, kSbW, grid_.h};
  }

  float xOf(double rel) const { return grid_.x + static_cast<float>((rel - scrollX_) * ppx_); }
  double ppqAt(float x) const { return scrollX_ + static_cast<double>(x - grid_.x) / ppx_; }
  float yOfRow(int r) const { return grid_.y + static_cast<float>(r) * rowH_ - scrollY_; }
  int rowAt(float y) const { return static_cast<int>(std::floor((y - grid_.y + scrollY_) / rowH_)); }
  int pitchAtRow(int r) const {
    if (rows_.empty()) return -1;
    return rows_[static_cast<size_t>(std::clamp(r, 0, static_cast<int>(rows_.size()) - 1))];
  }
  int rowOfPitch(int pch) const { return pch < 0 || pch > 127 ? -1 : rowOf_[static_cast<size_t>(pch)]; }
  double visiblePpq() const { return grid_.w / ppx_; }

  double gridStep() const {
    double g = kGridDiv[gridIdx_] == 0 ? barLen() : 4.0 / kGridDiv[gridIdx_];
    if (triplet_ && kGridDiv[gridIdx_] != 0) g = g * 2.0 / 3.0;
    return g;
  }
  bool snapping(unsigned mods) const { return snap_ != ((mods & kModAlt) != 0); }
  double snapRound(double rel) const {
    const double g = gridStep(), o = clipStart();
    return std::round((o + rel) / g) * g - o;
  }
  double snapFloor(double rel) const {
    const double g = gridStep(), o = clipStart();
    return std::floor((o + rel) / g + 1e-9) * g - o;
  }
  double defaultLen() const {
    const int d = kLenDiv[lenIdx_];
    if (d == -1) return gridStep();
    if (d == -2) return lastLen_ > 0 ? lastLen_ : gridStep();
    if (d == 0) return barLen();
    return 4.0 / d;
  }
  double minLen(unsigned mods) const { return snapping(mods) ? std::min(gridStep(), 0.25) : 1.0 / 32; }

  void xRange(double& lo, double& hi) const {
    lo = -beatLen();
    hi = std::max(clipLen(), 1.0) + barLen() * 2;
    for (const Note& n : notes_) {
      lo = std::min(lo, n.pos - beatLen());
      hi = std::max(hi, n.end() + barLen());
    }
  }
  void clampScroll() {
    ppx_ = std::clamp(ppx_, 1.0, 800.0);
    rowH_ = std::clamp(rowH_, 6.0f, 40.0f);
    double lo = 0, hi = 0;
    xRange(lo, hi);
    scrollX_ = std::clamp(scrollX_, lo, std::max(lo, hi - visiblePpq()));
    const float total = static_cast<float>(rows_.size()) * rowH_;
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, total - grid_.h));
  }
  void fitView(bool vertical) {
    needFit_ = false;
    const double len = std::max(clipLen(), beatLen());
    ppx_ = std::clamp(static_cast<double>(grid_.w - 16) / len, 1.0, 400.0);
    scrollX_ = -8.0 / ppx_;
    if (!vertical) return;
    int lo = 128, hi = -1;
    for (const Note& n : notes_) {
      const int r = rowOfPitch(n.pitch);
      if (r < 0) continue;
      lo = std::min(lo, r);
      hi = std::max(hi, r);
    }
    if (hi < 0 && drums_) {  // empty drum clip: the kit range, kick at the bottom when it does not all fit
      lo = std::max(0, rowOfPitch(pr::kDrumHi));
      hi = std::max(0, rowOfPitch(pr::kDrumLo));
    } else if (hi < 0) {
      lo = hi = std::max(0, rowOfPitch(66));
    }
    const float centerY = (static_cast<float>(lo + hi) / 2.0f + 0.5f) * rowH_;
    scrollY_ = centerY - grid_.h / 2;
    if (static_cast<float>(hi - lo + 1) * rowH_ > grid_.h) scrollY_ = static_cast<float>(hi + 1) * rowH_ - grid_.h;
    clampScroll();
  }
  void zoomX(double factor, float aroundX) {
    const double at = ppqAt(aroundX);
    ppx_ = std::clamp(ppx_ * factor, 1.0, 800.0);
    scrollX_ = at - static_cast<double>(aroundX - grid_.x) / ppx_;
    clampScroll();
    ctx_.invalidate();
  }
  void zoomY(float factor, float aroundY) {
    const float at = (aroundY - grid_.y + scrollY_) / rowH_;
    rowH_ = std::clamp(rowH_ * factor, 6.0f, 40.0f);
    scrollY_ = at * rowH_ - (aroundY - grid_.y);
    clampScroll();
    ctx_.invalidate();
  }
  void followPlayhead() {
    if (!follow_ || drag_ != Drag::None) return;
    const auto ts = ctx_.transport();
    if (!(ts.flags & SQ_TRANSPORT_PLAYING)) return;
    const double rel = ts.position - clipStart();
    if (rel < 0 || rel > clipLen()) return;
    const float x = xOf(rel);
    if (x < grid_.x || x > grid_.right() - 24) {
      scrollX_ = rel - visiblePpq() * 0.05;
      clampScroll();
    }
  }

  // ==== note helpers ===============================================================================
  Rect noteRect(const Note& n) const {
    const int r = rowOfPitch(n.pitch);
    const float x0 = xOf(n.pos), x1 = xOf(n.end());
    return Rect{x0, yOfRow(r) + 1, std::max(3.0f, x1 - x0 - 1), rowH_ - 1};
  }
  int hitNote(float x, float y, int* zone) const {
    if (!grid_.contains(x, y)) return -1;
    for (size_t i = notes_.size(); i-- > 0;) {
      const Note& n = notes_[i];
      if (rowOfPitch(n.pitch) < 0) continue;
      const Rect r = noteRect(n);
      if (!r.contains(x, y)) continue;
      if (zone) {
        const float edge = std::min(6.0f, r.w / 4);
        *zone = x >= r.right() - edge ? kZoneRight : (x <= r.x + edge && r.w > 12) ? kZoneLeft : kZoneBody;
      }
      return static_cast<int>(i);
    }
    return -1;
  }
  bool anySelected() const {
    for (const Note& n : notes_)
      if (n.sel) return true;
    return false;
  }
  void selectOnly(int idx) {
    for (size_t i = 0; i < notes_.size(); ++i) notes_[i].sel = static_cast<int>(i) == idx;
  }
  void selectAll() {
    for (Note& n : notes_) n.sel = true;
    ctx_.invalidate();
  }
  void sanitize() {
    for (Note& n : notes_) {
      n.pitch = std::clamp(n.pitch, 0, 127);
      n.vel = std::clamp(n.vel, 1, 127);
      if (!(n.len > 1e-4)) n.len = 1e-4;
    }
  }
  // Writes the working notes back as one undoable step.
  void commit(const char* description, bool extendClip = false) {
    if (!haveClip_) return;
    sanitize();
    const auto ev = pr::buildEvents(events_, notes_);
    auto p = sq::makeAttributes();
    p->setGuid("clip", &clipId_);
    p->setBlob("events", ev.empty() ? nullptr : ev.data(), static_cast<uint32_t>(ev.size() * sizeof(sq_clip_event)));
    EditScope scope(ctx_, description);
    if (extendClip) {
      double end = clip_.info.length;
      for (const Note& n : notes_) end = std::max(end, n.end());
      if (end > clip_.info.length + 1e-9) {
        const double bar = barLen();
        end = std::ceil((clipStart() + end) / bar - 1e-9) * bar - clipStart();
        auto r = sq::makeAttributes();
        r->setGuid("node", &clipId_);
        r->setDouble("start", clipStart());
        r->setDouble("length", end);
        ctx_.exec(sq::doc::kResizeClip, r.get());
      }
    }
    if (SQ_FAILED(ctx_.exec(sq::doc::kSetClipEvents, p.get()))) scope.cancel();
  }

  std::vector<Note*> targets(bool allIfNone) {
    std::vector<Note*> v;
    const bool any = anySelected();
    for (Note& n : notes_)
      if (n.sel || (!any && allIfNone)) v.push_back(&n);
    return v;
  }
  void deleteSelected() {
    const size_t before = notes_.size();
    notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [](const Note& n) { return n.sel; }), notes_.end());
    if (notes_.size() != before) commit("Delete Notes");
  }
  void transpose(int semis, bool onlySelected) {
    auto t = targets(!onlySelected);
    if (t.empty()) return;
    int lo = 127, hi = 0;
    for (Note* n : t) {
      lo = std::min(lo, n->pitch);
      hi = std::max(hi, n->pitch);
    }
    semis = std::clamp(semis, -lo, 127 - hi);
    if (semis == 0) return;
    for (Note* n : t) n->pitch += semis;
    if (fold_) {
      fold_ = false;  // the new pitches may not have rows yet
      buildRows();
      fold_ = true;
    }
    commit("Transpose Notes");
    if (t.size() <= 4) preview(t.front()->pitch, t.front()->vel, t.front()->chan);
  }
  void changeVelocity(int delta) {
    auto t = targets(false);
    for (Note* n : t) n->vel = std::clamp(n->vel + delta, 1, 127);
    if (!t.empty()) {
      commit("Change Velocity");
    }
  }
  void setVelocity(int v) {
    auto t = targets(true);
    for (Note* n : t) n->vel = std::clamp(v, 1, 127);
    if (!t.empty()) commit("Change Velocity");
  }
  void nudge(double d) {
    auto t = targets(false);
    if (t.empty()) return;
    double lo = 1e300, hi = -1e300;
    for (Note* n : t) {
      lo = std::min(lo, n->pos);
      hi = std::max(hi, n->pos);
    }
    d = std::clamp(d, -lo, std::max(0.0, clipLen() - 1e-3 - hi));
    if (std::fabs(d) < 1e-9) return;
    for (Note* n : t) n->pos += d;
    commit("Move Notes");
  }
  void quantizeNotes() {
    if (notes_.empty()) return;
    pr::quantize(notes_, gridStep(), clipStart(), qStrength_, anySelected());
    commit("Quantize Notes");
  }
  void legatoNotes() {
    if (notes_.empty()) return;
    pr::legato(notes_, anySelected());
    commit("Legato");
  }
  void duplicateSelection() {
    auto t = targets(false);
    if (t.empty()) return;
    double lo = 1e300, hi = -1e300;
    for (Note* n : t) {
      lo = std::min(lo, n->pos);
      hi = std::max(hi, n->end());
    }
    const double g = gridStep();
    const double shift = std::max(g, std::ceil((hi - lo) / g - 1e-9) * g);
    std::vector<Note> copies;
    for (Note* n : t) {
      Note c = *n;
      c.pos += shift;
      c.src = -1;
      copies.push_back(c);
      n->sel = false;
    }
    notes_.insert(notes_.end(), copies.begin(), copies.end());
    commit("Duplicate Notes", true);
  }
  void copySelection() {
    clipboard_.clear();
    double lo = 1e300;
    for (const Note& n : notes_)
      if (n.sel) lo = std::min(lo, n.pos);
    for (const Note& n : notes_)
      if (n.sel) {
        Note c = n;
        c.pos -= lo;
        c.src = -1;
        clipboard_.push_back(c);
      }
    double span = 0;
    for (const Note& n : clipboard_) span = std::max(span, n.end());
    clipboardSpan_ = std::max(gridStep(), std::ceil(span / gridStep() - 1e-9) * gridStep());
    ctx_.setStatus(std::to_string(clipboard_.size()) + " note(s) copied");
  }
  void paste() {
    double at = ctx_.editState().cursor - clipStart();
    if (at < 0 || at >= clipLen()) at = std::max(0.0, snapFloor(ppqAt(grid_.x + 1)));
    if (lastPasteAt_ >= 0 && std::fabs(lastPasteAt_ - at) < 1e-9) at = lastPasteAt_ + clipboardSpan_;
    lastPasteAt_ = at;
    for (Note& n : notes_) n.sel = false;
    for (Note c : clipboard_) {
      c.pos += at;
      c.sel = true;
      notes_.push_back(c);
    }
    commit("Paste Notes", true);
  }

  // ==== note preview (Midi.Inject) =================================================================
  bool injectAvailable() {
    if (injectState_ < 0) {
      injectState_ = 0;
      for (const auto& ci : ctx_.shell().commands())
        if (ci.category == "Midi" && ci.name == "Inject") injectState_ = 1;
    }
    return injectState_ == 1;
  }
  void sendMidi(int status, int d1, int d2) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%X,%d,%d", status & 0xFF, d1 & 0x7F, d2 & 0x7F);
    auto a = sq::makeAttributes();
    a->setString("bytes", buf);
    (void)ctx_.shell().execute("Midi.Inject", a.get());  // failures are silent (no engine, no target)
  }
  void preview(int pitch, int vel, int chan) {
    if (pitch < 0 || pitch > 127 || !injectAvailable()) return;
    for (auto it = pending_.begin(); it != pending_.end();)
      if (it->pitch == pitch && it->chan == chan) {
        sendMidi(0x80 | chan, pitch, 0);
        it = pending_.erase(it);
      } else ++it;
    sendMidi(0x90 | (chan & 0x0F), pitch, std::clamp(vel, 1, 127));
    pending_.push_back(Pending{Clock::now() + std::chrono::milliseconds(200), pitch, chan & 0x0F});
    previewPitch_ = pitch;
  }
  void flushPreview(bool all) {
    if (pending_.empty()) return;
    const auto now = Clock::now();
    for (auto it = pending_.begin(); it != pending_.end();)
      if (all || now >= it->at) {
        sendMidi(0x80 | it->chan, it->pitch, 0);
        if (it->pitch == previewPitch_) previewPitch_ = -1;
        it = pending_.erase(it);
      } else ++it;
    if (!pending_.empty()) ctx_.invalidate();  // keep frames coming until the note-offs went out
  }

  // ==== painting ===================================================================================
  void addHit(const Rect& r, int id) { hits_.push_back(HitBox{r, id}); }
  int headerHit(float x, float y) const {
    for (const auto& h : hits_)
      if (h.r.contains(x, y)) return h.id;
    return -1;
  }
  // Header widgets. In a dry pass (dry_) they only measure: nothing is drawn or hit-tested.
  float button(Canvas& c, float x, const char* lbl, int id, bool on, float minW = 0) {
    const Palette& p = ctx_.palette();
    const float w = std::max(minW, c.measureText(lbl, 12) + (compact_ ? 12.0f : 18.0f));
    if (dry_) return x + w;
    const Rect r{x, header_.y + 5, w, header_.h - 10};
    addHit(r, id);
    const bool hot = hotHdr_ == id;
    if (on) c.fillRoundRect(r, 4, p.accent.withAlpha(p.dark ? 0.85f : 0.9f));
    else c.fillRoundRect(r, 4, hot ? p.panel3 : p.panel2);
    if (!on) c.strokeRoundRect(r, 4, p.border, 1);
    c.drawText(lbl, r, on ? contrastText(p, p.accent) : p.text, 12, TextAlign::Center);
    return x + w;
  }
  float dropdown(Canvas& c, float x, const std::string& lbl, int id, float minW) {
    const Palette& p = ctx_.palette();
    const float w = std::max(minW, c.measureText(lbl, 12) + 30);
    if (dry_) return x + w;
    const Rect r{x, header_.y + 5, w, header_.h - 10};
    addHit(r, id);
    c.fillRoundRect(r, 4, hotHdr_ == id ? p.panel3 : p.panel2);
    c.strokeRoundRect(r, 4, p.border, 1);
    c.drawText(lbl, Rect{r.x + 9, r.y, r.w - 24, r.h}, p.text, 12);
    draw::triangle(c, Rect{r.right() - 15, r.y + r.h / 2 - 2, 8, 5}, 2, p.dim);
    return x + w;
  }
  float caption(Canvas& c, float x, const char* str) {
    if (compact_) return x;  // compact header: no captions
    const float w = c.measureText(str, 11) + 6;
    if (!dry_) c.drawText(str, Rect{x, header_.y, w, header_.h}, ctx_.palette().dim, 11);
    return x + w;
  }
  float separator(Canvas& c, float x) {
    const float w = compact_ ? 9.0f : 13.0f;
    if (!dry_) c.fillRect(Rect{x + (w - 1) / 2, header_.y + 9, 1, header_.h - 18}, ctx_.palette().border);
    return x + w;
  }

  // One header group (starting with its separator); returns the x after it.
  float headerGroup(Canvas& c, int g, float x) {
    const Palette& p = ctx_.palette();
    x = separator(c, x);
    switch (g) {
      case kGTools:
        x = button(c, x, "Select", kHToolSelect, tool_ == Tool::Select) + 2;
        x = button(c, x, "Draw", kHToolDraw, tool_ == Tool::Draw) + 2;
        return button(c, x, "Erase", kHToolErase, tool_ == Tool::Erase);
      case kGGrid:
        x = caption(c, x, "Grid");
        x = dropdown(c, x, kGridNames[gridIdx_], kHGrid, 58) + 2;
        x = button(c, x, "3", kHTriplet, triplet_, 24) + 2;
        return button(c, x, "Snap", kHSnap, snap_);
      case kGLength:
        x = caption(c, x, "Length");
        return dropdown(c, x, kLenNames[lenIdx_], kHLength, 62);
      case kGVel: {
        x = caption(c, x, "Vel");
        const Rect r{x, header_.y + 5, 40, header_.h - 10};
        if (dry_) return x + r.w;
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", newVel_);
        addHit(r, kHVel);
        c.fillRoundRect(r, 4, (hotHdr_ == kHVel || drag_ == Drag::HeaderVel) ? p.panel3 : p.bg);
        c.strokeRoundRect(r, 4, p.border, 1);
        const float frac = static_cast<float>(newVel_) / 127.0f;
        c.fillRect(Rect{r.x + 3, r.bottom() - 4, (r.w - 6) * frac, 2}, velColor(newVel_));
        c.drawText(buf, Rect{r.x, r.y - 1, r.w, r.h}, p.text, 12, TextAlign::Center);
        return x + r.w;
      }
      case kGQuantize: {
        x = button(c, x, compact_ ? "Quant" : "Quantize", kHQuantize, false);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(qStrength_ * 100)));
        x = dropdown(c, x + 2, buf, kHQStrength, 52) + 2;
        return button(c, x, "Legato", kHLegato, false);
      }
      case kGTranspose:
        x = button(c, x, "-", kHTransDown, false, 24);
        x = compact_ ? x + 2 : caption(c, x + 3, "Transpose");
        return button(c, x, "+", kHTransUp, false, 24);
      case kGView:
        x = button(c, x, "Fold", kHFold, fold_) + 2;
        return button(c, x, "Follow", kHFollow, follow_);
      default: return x;
    }
  }

  void paintHeader(Canvas& c, bool midi) {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    header_ = Rect{f.x, f.y, f.w, kHeaderH};
    c.fillRect(header_, p.panel);
    c.fillRect(Rect{f.x, header_.bottom() - 1, f.w, 1}, p.border);
    if (tree() && tree()->focus() == this) c.fillRect(Rect{f.x, header_.bottom() - 2, f.w, 2}, p.accent.withAlpha(0.6f));
    const std::string clipName = clip_.name.empty() ? std::string(midi ? "MIDI Clip" : "Audio Clip") : clip_.name;
    const std::string trackName = trackName_.empty() ? std::string("-") : trackName_;
    const float nameFull = std::max(c.measureText(clipName, 12) + 4, c.measureText(trackName, 10.5f)) + 6;
    // Layout: the zoom group is pinned right; the groups fill the space left of it, first at full
    // size, then compact (no captions, a shorter name), then the ones that do not fit go into "...".
    const float zw = 24 + 2 + 24 + 2 + 34;
    const float limit = f.right() - zw - 10 - 8;
    std::array<float, kGCount> gw{};
    auto measure = [&]() {
      float sum = 0;
      dry_ = true;
      for (int g = 0; g < kGCount; ++g) {
        gw[static_cast<size_t>(g)] = headerGroup(c, g, 0);
        sum += gw[static_cast<size_t>(g)];
      }
      dry_ = false;
      return sum;
    };
    compact_ = false;
    float nameW = std::clamp(nameFull, 70.0f, 200.0f);
    if (midi && f.x + 20 + nameW + 4 + measure() > limit) {
      compact_ = true;
      nameW = std::clamp(nameFull, 60.0f, 110.0f);
      measure();
    }
    // clip + track
    float x = f.x + 10;
    c.fillRoundRect(Rect{x, header_.y + 8, 4, header_.h - 16}, 2, trackCol_);
    x += 10;
    c.drawTextBold(clipName, Rect{x, header_.y + 3, nameW, 16}, p.text, 12);
    c.drawText(trackName, Rect{x, header_.y + 17, nameW, 14}, p.dim, 10.5f);
    x += nameW + 4;
    if (!midi) return;
    float need = 0;
    for (float w : gw) need += w;
    shownGroups_ = 0;
    if (x + need <= limit) {
      shownGroups_ = kGCount;
    } else {
      const float moreW = 13 + 30;
      float xx = x;
      while (shownGroups_ < kGCount && xx + gw[static_cast<size_t>(shownGroups_)] <= limit - moreW) xx += gw[static_cast<size_t>(shownGroups_++)];
    }
    for (int g = 0; g < shownGroups_; ++g) x = headerGroup(c, g, x);
    if (shownGroups_ < kGCount) {
      x = separator(c, x);
      x = button(c, x, "...", kHMore, false, 30);
    }
    // zoom: right aligned and always reachable (only a panel narrower than the clip name pushes it)
    float zx = std::max(x + 13, f.right() - zw - 10);
    zx = button(c, zx, "-", kHZoomOut, false, 24) + 2;
    zx = button(c, zx, "+", kHZoomIn, false, 24) + 2;
    button(c, zx, "Fit", kHZoomFit, false, 34);
  }

  // ---- header menus (dropdowns and the "..." overflow menu) ----
  std::vector<MenuItem> gridMenu() {
    std::vector<MenuItem> items;
    for (int i = 0; i < kGridCount; ++i) {
      MenuItem m;
      m.label = kGridNames[i];
      m.checked = i == gridIdx_;
      m.action = [this, i] {
        gridIdx_ = i;
        ctx_.invalidate();
      };
      items.push_back(std::move(m));
    }
    items.push_back(MenuItem::sep());
    MenuItem t;
    t.label = "Triplets";
    t.checked = triplet_;
    t.action = [this] { triplet_ = !triplet_; };
    items.push_back(std::move(t));
    return items;
  }
  std::vector<MenuItem> lengthMenu() {
    std::vector<MenuItem> items;
    for (int i = 0; i < kLenCount; ++i) {
      MenuItem m;
      m.label = kLenNames[i];
      if (i == 0) m.label = "Grid (follows the grid)";
      if (i == 1) m.label = "Last (length of the last edited note)";
      m.checked = i == lenIdx_;
      m.action = [this, i] { lenIdx_ = i; };
      items.push_back(std::move(m));
      if (i == 1) items.push_back(MenuItem::sep());
    }
    return items;
  }
  std::vector<MenuItem> strengthMenu() {
    std::vector<MenuItem> items;
    for (int s : {100, 75, 50, 25}) {
      MenuItem m;
      m.label = "Strength " + std::to_string(s) + "%";
      m.checked = std::lround(qStrength_ * 100) == s;
      m.action = [this, s] { qStrength_ = s / 100.0; };
      items.push_back(std::move(m));
    }
    return items;
  }
  // The controls of the header groups that did not fit.
  std::vector<MenuItem> overflowMenu() {
    std::vector<MenuItem> m;
    auto add = [&m](const std::string& lbl, std::function<void()> fn, bool on = false, std::vector<MenuItem> sub = {}) {
      MenuItem it;
      it.label = lbl;
      it.action = std::move(fn);
      it.checked = on;
      it.submenu = std::move(sub);
      m.push_back(std::move(it));
    };
    for (int g = shownGroups_; g < kGCount; ++g) {
      if (!m.empty()) m.push_back(MenuItem::sep());
      switch (g) {
        case kGTools:
          add("Select Tool", [this] { tool_ = Tool::Select; }, tool_ == Tool::Select);
          add("Draw Tool", [this] { tool_ = Tool::Draw; }, tool_ == Tool::Draw);
          add("Erase Tool", [this] { tool_ = Tool::Erase; }, tool_ == Tool::Erase);
          break;
        case kGGrid:
          add(std::string("Grid: ") + kGridNames[gridIdx_], {}, false, gridMenu());
          add("Snap to Grid", [this] { snap_ = !snap_; }, snap_);
          break;
        case kGLength: add(std::string("Note Length: ") + kLenNames[lenIdx_], {}, false, lengthMenu()); break;
        case kGVel:
          add("Velocity for New Notes (" + std::to_string(newVel_) + ")...", [this] {
            ctx_.prompt("Velocity for new notes (1-127)", std::to_string(newVel_), [this](const std::string& v) {
              const int n = std::atoi(v.c_str());
              if (n >= 1 && n <= 127) newVel_ = n;
              ctx_.invalidate();
            });
          });
          break;
        case kGQuantize:
          add("Quantize", [this] { quantizeNotes(); });
          add("Quantize Strength", {}, false, strengthMenu());
          add("Legato", [this] { legatoNotes(); });
          break;
        case kGTranspose:
          add("Transpose +1", [this] { transpose(1, false); });
          add("Transpose -1", [this] { transpose(-1, false); });
          add("Transpose +12", [this] { transpose(12, false); });
          add("Transpose -12", [this] { transpose(-12, false); });
          break;
        case kGView:
          add("Fold", [this] {
            fold_ = !fold_;
            buildRows();
            fitView(true);
          }, fold_);
          add("Follow Playhead", [this] { follow_ = !follow_; }, follow_);
          break;
        default: break;
      }
    }
    return m;
  }

  Color velColor(int vel) const {
    const Palette& p = ctx_.palette();
    const float t = std::clamp(static_cast<float>(vel) / 127.0f, 0.0f, 1.0f);
    const Color lo = draw::mix(p.panel3, trackCol_, 0.5f);
    const Color hi = p.dark ? draw::lighten(trackCol_, 0.28f) : draw::mix(trackCol_, p.text, 0.1f);
    return draw::mix(lo, hi, t);
  }

  void paintRuler(Canvas& c) {
    const Palette& p = ctx_.palette();
    c.fillRect(corner_, p.panel);
    c.fillRect(ruler_, p.panel);
    c.fillRect(Rect{corner_.x, ruler_.bottom() - 1, frame().w, 1}, p.border);
    // hovered pitch in the corner
    if (hoverPitch_ >= 0) c.drawTextBold(pitchName(hoverPitch_), Rect{corner_.x + 6, corner_.y, corner_.w - 10, corner_.h}, p.dim, 11, TextAlign::Left);
    c.pushClip(ruler_);
    const double cs = clipStart(), len = clipLen();
    // clip range strip
    {
      const float x0 = std::max(ruler_.x, xOf(0)), x1 = std::min(ruler_.right(), xOf(len));
      if (x1 > x0) c.fillRect(Rect{x0, ruler_.bottom() - 4, x1 - x0, 3}, trackCol_.withAlpha(0.85f));
    }
    // loop range
    const auto ts = ctx_.transport();
    if ((ts.flags & SQ_TRANSPORT_LOOPING) && ts.loop_end > ts.loop_start) {
      const float x0 = std::max(ruler_.x, xOf(ts.loop_start - cs)), x1 = std::min(ruler_.right(), xOf(ts.loop_end - cs));
      if (x1 > x0) c.fillRect(Rect{x0, ruler_.y + 2, x1 - x0, ruler_.h - 7}, p.accent.withAlpha(0.22f));
    }
    // bar / beat labels
    const double bar = barLen(), beat = beatLen();
    const double barPx = bar * ppx_;
    int barEvery = 1;
    while (barPx * barEvery < 34 && barEvery < 4096) barEvery *= 2;
    const bool beatLabels = beat * ppx_ >= 34;
    const double a0 = cs + ppqAt(ruler_.x), a1 = cs + ppqAt(ruler_.right());
    const double step = beatLabels ? beat : bar;
    for (double k = std::floor(a0 / step); k * step <= a1; k += 1) {
      const double abs = k * step;
      if (abs < 0) continue;
      const float x = xOf(abs - cs);
      const double barIdxD = std::floor(abs / bar + 1e-9);
      const auto barIdx = static_cast<long long>(barIdxD);
      const bool isBar = std::fabs(abs - barIdxD * bar) < 1e-6;
      if (isBar && (barIdx % barEvery) != 0) continue;
      char buf[48];
      if (isBar) std::snprintf(buf, sizeof buf, "%lld", barIdx + 1);
      else std::snprintf(buf, sizeof buf, "%lld.%d", barIdx + 1, static_cast<int>(std::lround((abs - barIdxD * bar) / beat)) + 1);
      c.fillRect(Rect{x, ruler_.y + (isBar ? 4.0f : 12.0f), 1, isBar ? ruler_.h - 8 : ruler_.h - 16}, isBar ? p.dim : p.faint);
      c.drawText(buf, Rect{x + 3, ruler_.y + 1, 44, ruler_.h - 6}, isBar ? p.text : p.dim, isBar ? 11.0f : 10.0f);
    }
    // clip end marker
    {
      const float x = xOf(len);
      const bool hot = hotRulerEnd_ || drag_ == Drag::ClipEnd;
      const Color mc = hot ? draw::lighten(trackCol_, 0.3f) : trackCol_;
      draw::triangle(c, Rect{x - 6, ruler_.y + 3, 12, 8}, 2, mc);
      c.fillRect(Rect{x - 1, ruler_.y + 3, 2, ruler_.h - 6}, mc);
    }
    // edit cursor + playhead
    const float cx = xOf(ctx_.editState().cursor - cs);
    if (cx >= ruler_.x && cx <= ruler_.right()) draw::triangle(c, Rect{cx - 4, ruler_.bottom() - 7, 8, 6}, 2, p.cursor);
    const float px = xOf(ts.position - cs);
    if (px >= ruler_.x && px <= ruler_.right()) draw::triangle(c, Rect{px - 5, ruler_.y, 10, 7}, 2, p.playhead);
    c.popClip();
  }

  void paintGrid(Canvas& c) {
    const Palette& p = ctx_.palette();
    c.pushClip(grid_);
    c.fillRect(grid_, p.bg);
    const Color white = p.trackBg;
    const Color black = p.dark ? draw::mix(p.trackBg, p.border, 0.5f) : draw::mix(p.trackBg, p.dim, 0.13f);
    const int r0 = std::max(0, rowAt(grid_.y)), r1 = std::min(static_cast<int>(rows_.size()) - 1, rowAt(grid_.bottom()));
    for (int r = r0; r <= r1; ++r) {
      const int pch = rows_[static_cast<size_t>(r)];
      const float y = yOfRow(r);
      Color rc = (drums_ ? !pr::drumName(pch) : pr::isBlackKey(pch)) ? black : white;
      if (pch == hoverPitch_ && drag_ == Drag::None) rc = draw::mix(rc, p.accent, 0.07f);
      c.fillRect(Rect{grid_.x, y, grid_.w, rowH_}, rc);
      const bool octave = !flatKeys() && (pch % 12) == 0;
      c.fillRect(Rect{grid_.x, y + rowH_ - 1, grid_.w, 1}, octave ? p.gridBar : p.grid.withAlpha(0.7f));
    }
    // vertical lines
    const double cs = clipStart(), bar = barLen(), beat = beatLen();
    double sub = gridStep();
    while (sub * ppx_ < 7) sub *= 2;
    const double a0 = cs + ppqAt(grid_.x), a1 = cs + ppqAt(grid_.right());
    for (double k = std::floor(a0 / sub); k * sub <= a1; k += 1) {
      const double abs = k * sub;
      const float x = std::round(xOf(abs - cs));
      const bool isBar = std::fabs(abs / bar - std::round(abs / bar)) < 1e-6;
      const bool isBeat = std::fabs(abs / beat - std::round(abs / beat)) < 1e-6;
      const Color lc = isBar ? draw::mix(p.gridBar, p.dim, 0.25f) : isBeat ? draw::mix(p.grid, p.gridBar, 0.7f) : p.grid;
      c.fillRect(Rect{x, grid_.y, 1, grid_.h}, lc);
    }
    // loop range
    const auto ts = ctx_.transport();
    if ((ts.flags & SQ_TRANSPORT_LOOPING) && ts.loop_end > ts.loop_start) {
      const float x0 = std::max(grid_.x, xOf(ts.loop_start - cs)), x1 = std::min(grid_.right(), xOf(ts.loop_end - cs));
      if (x1 > x0) c.fillRect(Rect{x0, grid_.y, x1 - x0, grid_.h}, p.accent.withAlpha(0.05f));
    }
    // notes
    const bool anySel = anySelected();
    for (size_t i = 0; i < notes_.size(); ++i) {
      const Note& n = notes_[i];
      if (rowOfPitch(n.pitch) < 0) continue;
      const Rect r = noteRect(n);
      if (r.right() < grid_.x || r.x > grid_.right() || r.bottom() < grid_.y || r.y > grid_.bottom()) continue;
      Color col = velColor(n.vel);
      if (anySel && !n.sel) col = draw::mix(col, p.trackBg, 0.25f);
      const float rad = std::min(3.0f, std::min(r.w, r.h) / 3);
      c.fillRoundRect(r, rad, col);
      if (n.sel) c.strokeRoundRect(r, rad, p.text, 1.5f);
      else c.strokeRoundRect(r, rad, draw::mix(col, p.bg, 0.45f), 1);
      // velocity tick on the left edge
      if (r.w > 8 && r.h > 6) c.fillRect(Rect{r.x + 1.5f, r.y + 2, 2, r.h - 4}, draw::mix(col, contrastText(p, col), 0.35f));
      if (static_cast<int>(i) == hotNote_ && drag_ == Drag::None && hotZone_ != kZoneBody) {
        const float ex = hotZone_ == kZoneRight ? r.right() - 3 : r.x;
        c.fillRect(Rect{ex, r.y, 3, r.h}, contrastText(p, col).withAlpha(0.7f));
      }
      if (r.w > 30 && rowH_ >= 10) {
        const float fs = std::min(11.0f, rowH_ - 3);
        c.drawText(pitchName(n.pitch), Rect{r.x + 6, r.y, r.w - 8, r.h}, contrastText(p, col), fs);
      }
    }
    // draw-tool ghost
    if (tool_ == Tool::Draw && drag_ == Drag::None && hotNote_ < 0 && ghostRow_ >= 0) {
      const double pos = snap_ ? snapFloor(ghostPos_) : ghostPos_;
      if (pos >= 0 && pos < clipLen()) {
        const float x0 = xOf(pos), x1 = xOf(std::min(clipLen(), pos + defaultLen()));
        const Rect r{x0, yOfRow(ghostRow_) + 1, std::max(3.0f, x1 - x0 - 1), rowH_ - 1};
        c.strokeRoundRect(r, 3, velColor(newVel_).withAlpha(0.8f), 1);
        c.fillRoundRect(r, 3, velColor(newVel_).withAlpha(0.25f));
      }
    }
    // outside the clip
    const float xs = xOf(0), xe = xOf(clipLen());
    const Color dim = p.dark ? p.border.withAlpha(0.62f) : p.dim.withAlpha(0.2f);
    if (xs > grid_.x) c.fillRect(Rect{grid_.x, grid_.y, xs - grid_.x, grid_.h}, dim);
    if (xe < grid_.right()) c.fillRect(Rect{xe, grid_.y, grid_.right() - xe, grid_.h}, dim);
    c.fillRect(Rect{xe - 1, grid_.y, 2, grid_.h}, trackCol_.withAlpha(0.8f));
    // rubber band
    if (drag_ == Drag::Rubber) {
      const Rect rb{std::min(dx0_, curX_), std::min(dy0_, curY_), std::fabs(curX_ - dx0_), std::fabs(curY_ - dy0_)};
      c.fillRect(rb, p.accent.withAlpha(0.12f));
      c.strokeRect(rb, p.accent, 1);
    }
    // cursor + playhead
    const float cx = xOf(ctx_.editState().cursor - cs);
    if (cx >= grid_.x && cx <= grid_.right()) c.fillRect(Rect{cx, grid_.y, 1, grid_.h}, p.cursor.withAlpha(0.7f));
    const float px = xOf(ts.position - cs);
    if (px >= grid_.x && px <= grid_.right()) c.fillRect(Rect{px - 0.5f, grid_.y, 1.5f, grid_.h}, (ts.flags & SQ_TRANSPORT_RECORDING) ? p.rec : p.playhead);
    // drag feedback
    if (drag_ != Drag::None && !dragInfo_.empty()) {
      const float w = c.measureText(dragInfo_, 11) + 14;
      const Rect r{std::min(curX_ + 14, grid_.right() - w - 4), std::max(grid_.y + 4, curY_ - 26), w, 18};
      c.fillRoundRect(r, 4, p.panel2.withAlpha(0.95f));
      c.strokeRoundRect(r, 4, p.border, 1);
      c.drawText(dragInfo_, r, p.text, 11, TextAlign::Center);
    }
    if (notes_.empty() && drag_ == Drag::None) {
      const char* hint = tool_ == Tool::Draw ? "Click to draw notes" : "Double-click (or use the Draw tool) to add notes";
      c.drawText(hint, Rect{grid_.x, grid_.y + grid_.h / 2 - 10, std::min(grid_.w, std::max(0.0f, xe - grid_.x)), 20}, p.dim, 12, TextAlign::Center);
    }
    c.popClip();
  }

  void paintKeys(Canvas& c) {
    const Palette& p = ctx_.palette();
    c.pushClip(keys_);
    const Color whiteK = p.dark ? draw::mix(p.text, p.panel2, 0.18f) : draw::lighten(p.panel, 0.7f);
    const Color blackK = p.dark ? p.bg : draw::mix(p.text, p.panel, 0.2f);
    const Color line = p.dark ? draw::mix(whiteK, p.bg, 0.35f) : draw::mix(whiteK, p.text, 0.25f);
    std::array<bool, 128> selPitch{};
    for (const Note& n : notes_)
      if (n.sel) selPitch[static_cast<size_t>(std::clamp(n.pitch, 0, 127))] = true;
    const int r0 = std::max(0, rowAt(keys_.y)), r1 = std::min(static_cast<int>(rows_.size()) - 1, rowAt(keys_.bottom()));
    c.fillRect(keys_, p.panel);
    if (r1 >= r0) c.fillRect(Rect{keys_.x, yOfRow(r0), keys_.w, yOfRow(r1 + 1) - yOfRow(r0)}, whiteK);
    const float bw = keys_.w * 0.6f;
    for (int r = r0; r <= r1; ++r) {
      const int pch = rows_[static_cast<size_t>(r)];
      const float y = yOfRow(r);
      const bool flat = flatKeys();
      const char* drum = drums_ ? pr::drumName(pch) : nullptr;
      const bool blk = drums_ ? drum == nullptr : pr::isBlackKey(pch);
      Color kc = blk ? blackK : whiteK;
      Color hl{0, 0, 0, 0};
      if (pch == previewPitch_) hl = p.accent;
      else if (pch == hoverPitch_) hl = p.accent.withAlpha(0.55f);
      else if (selPitch[static_cast<size_t>(pch)]) hl = p.accent.withAlpha(0.32f);
      if (blk && !flat) {
        c.fillRect(Rect{keys_.x, y, bw, rowH_}, hl.a > 0 ? draw::mix(kc, hl.withAlpha(1), hl.a) : kc);
        if (hl.a > 0) c.fillRect(Rect{keys_.x + bw, y, keys_.w - bw, rowH_}, hl.withAlpha(hl.a * 0.4f));
      } else {
        if (flat) c.fillRect(Rect{keys_.x, y, keys_.w, rowH_}, kc);
        if (hl.a > 0) c.fillRect(Rect{keys_.x, y, keys_.w, rowH_}, hl);
        if (flat || pch % 12 == 0 || pch % 12 == 5) c.fillRect(Rect{keys_.x, y + rowH_ - 1, keys_.w, 1}, line);
      }
      if (blk && !flat) c.fillRect(Rect{keys_.x + bw, y + rowH_ / 2, keys_.w - bw, 1}, line);
      const bool showLabel = flat ? rowH_ >= 9 : (pch % 12 == 0 && rowH_ >= 7);
      if (showLabel) {
        const float fs = std::clamp(rowH_ - 3, 8.0f, 11.0f);
        const Color tc = flat && blk ? contrastText(p, blackK) : contrastText(p, whiteK);
        const bool strong = drums_ ? drum != nullptr : pch % 12 == 0;
        c.drawText(drum ? std::string(drum) : pr::noteName(pch), Rect{keys_.x + 3, y, keys_.w - 7, rowH_}, strong ? tc : tc.withAlpha(0.6f), fs,
                   drums_ ? TextAlign::Left : TextAlign::Right);
      }
    }
    c.fillRect(Rect{keys_.right() - 1, keys_.y, 1, keys_.h}, p.border);
    c.popClip();
  }

  void paintVelocity(Canvas& c) {
    const Palette& p = ctx_.palette();
    // divider
    c.fillRect(div_, p.panel);
    c.fillRect(Rect{div_.x, div_.y, div_.w, 1}, p.border);
    c.fillRect(Rect{div_.x, div_.bottom() - 1, div_.w, 1}, p.border);
    const float gx = div_.x + div_.w / 2;
    for (int i = -1; i <= 1; ++i) c.fillRect(Rect{gx + static_cast<float>(i) * 5 - 1, div_.y + 2, 2, 2}, hotDiv_ || drag_ == Drag::VelSplit ? p.accent : p.faint);
    if (vel_.h < 8) return;
    c.fillRect(velLabel_, p.panel);
    if (vel_.h > 34) c.drawText("Velocity", Rect{velLabel_.x + 6, velLabel_.bottom() - 16, velLabel_.w - 8, 14}, p.dim, 10.5f);
    const Color bgc = draw::mix(p.trackBg, p.bg, 0.5f);
    c.fillRect(vel_, bgc);
    const float top = vel_.y + 4, h = vel_.h - 8;
    for (int q = 1; q <= 3; ++q) c.fillRect(Rect{vel_.x, std::round(top + h * static_cast<float>(q) / 4), vel_.w, 1}, p.grid.withAlpha(0.8f));
    c.drawText("127", Rect{velLabel_.x, top - 6, velLabel_.w - 6, 12}, p.faint, 9.5f, TextAlign::Right);
    if (vel_.h > 44) c.drawText("64", Rect{velLabel_.x, top + h / 2 - 6, velLabel_.w - 6, 12}, p.faint, 9.5f, TextAlign::Right);
    c.pushClip(vel_);
    // bar lines for orientation
    const double cs = clipStart(), bar = barLen();
    const double a0 = cs + ppqAt(vel_.x), a1 = cs + ppqAt(vel_.right());
    double bstep = bar;
    while (bstep * ppx_ < 30) bstep *= 2;
    for (double k = std::floor(a0 / bstep); k * bstep <= a1; k += 1) c.fillRect(Rect{std::round(xOf(k * bstep - cs)), vel_.y, 1, vel_.h}, p.grid);
    const bool anySel = anySelected();
    for (int pass = 0; pass < 2; ++pass)  // selected notes on top
      for (const Note& n : notes_) {
        if (n.sel != (pass == 1)) continue;
        const float x = xOf(n.pos);
        if (x < vel_.x - 10 || x > vel_.right() + 10) continue;
        const float vh = h * static_cast<float>(n.vel) / 127.0f;
        Color col = velColor(n.vel);
        if (anySel && !n.sel) col = draw::mix(col, bgc, 0.45f);
        const float w = std::clamp(static_cast<float>(n.len * ppx_) - 2, 2.0f, 5.0f);
        c.fillRect(Rect{x, top + h - vh, w, vh}, col.withAlpha(0.8f));
        const Rect head{x - 1, top + h - vh - 2, w + 2, 4};
        c.fillRoundRect(head, 1.5f, n.sel ? p.text : col);
      }
    if (drag_ == Drag::VelPaint) {
      c.fillRect(Rect{curX_ - 0.5f, vel_.y, 1, vel_.h}, p.accent.withAlpha(0.5f));
    }
    c.popClip();
  }

  void paintScrollbars(Canvas& c) {
    const Palette& p = ctx_.palette();
    c.fillRect(Rect{frame().x, hbar_.y, keysW(), kSbW}, p.panel);
    c.fillRect(hbar_, p.panel);
    c.fillRect(vbar_, p.panel);
    c.fillRect(Rect{vbar_.x, vel_.y - kDivH, kSbW, vel_.h + kDivH + kSbW}, p.panel);
    const Rect ht = hThumb(), vt = vThumb();
    c.fillRoundRect(ht.inset(2), 3, drag_ == Drag::ScrollH ? p.dim : p.dim.withAlpha(0.4f));
    c.fillRoundRect(vt.inset(2), 3, drag_ == Drag::ScrollV ? p.dim : p.dim.withAlpha(0.4f));
  }
  Rect hThumb() const {
    double lo = 0, hi = 0;
    xRange(lo, hi);
    const double range = std::max(hi - lo, visiblePpq());
    const float w = std::max(24.0f, static_cast<float>(hbar_.w * visiblePpq() / range));
    const float x = hbar_.x + static_cast<float>((scrollX_ - lo) / range) * hbar_.w;
    return Rect{std::min(x, hbar_.right() - w), hbar_.y, w, hbar_.h};
  }
  Rect vThumb() const {
    const float total = std::max(grid_.h, static_cast<float>(rows_.size()) * rowH_);
    const float h = std::max(24.0f, vbar_.h * grid_.h / total);
    const float y = vbar_.y + scrollY_ / total * vbar_.h;
    return Rect{vbar_.x, std::min(y, vbar_.bottom() - h), vbar_.w, h};
  }

  // ==== empty state ================================================================================
  // Track the new clip goes to: the selected track when it is an instrument track, else the first
  // instrument track.
  bool createTarget(TrackView* out) {
    const sq_guid sel = ctx_.selectedTrack();
    TrackView first;
    bool haveFirst = false;
    for (auto& t : ctx_.tracks()) {
      if (t.info.kind != SQ_TRACK_INSTRUMENT) continue;
      if (t.id == sel) {
        *out = t;
        return true;
      }
      if (!haveFirst) {
        first = t;
        haveFirst = true;
      }
    }
    if (haveFirst) *out = first;
    return haveFirst;
  }
  void paintEmpty(Canvas& c) {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    TrackView t;
    const bool can = createTarget(&t);
    const float cy = f.y + f.h / 2;
    c.drawTextBold("No clip open", Rect{f.x, cy - 58, f.w, 22}, p.text, 14, TextAlign::Center);
    c.drawText("Double-click a MIDI clip in the arrangement to edit its notes, or create a new one here.", Rect{f.x + 16, cy - 34, f.w - 32, 18},
               p.dim, 12, TextAlign::Center);
    const char* lbl = can ? "Create MIDI Clip" : "Add Instrument Track";
    const float w = c.measureText(lbl, 12.5f) + 40;
    const Rect r{f.x + (f.w - w) / 2, cy - 6, w, 30};
    addHit(r, can ? kHCreateClip : kHAddTrack);
    const bool hot = hotHdr_ == kHCreateClip || hotHdr_ == kHAddTrack;
    c.fillRoundRect(r, 6, hot ? draw::lighten(p.accent, 0.12f) : p.accent);
    c.drawTextBold(lbl, r, contrastText(p, p.accent), 12.5f, TextAlign::Center);
    std::string sub;
    if (can) sub = "1 bar on \"" + t.name + "\" at " + ctx_.formatBars(ctx_.editState().cursor);
    else sub = "MIDI clips live on instrument tracks";
    c.drawText(sub, Rect{f.x + 16, r.bottom() + 8, f.w - 32, 16}, p.faint, 11, TextAlign::Center);
  }
  void createClip() {
    TrackView t;
    if (!createTarget(&t)) return;
    const double at = std::max(0.0, ctx_.editState().cursor);  // exactly at the edit cursor (the hint says so)
    readSignature();
    const double bar = barLen();
    auto a = sq::makeAttributes();
    a->setGuid("track", &t.id);
    a->setInt("kind", SQ_CLIP_MIDI);
    a->setDouble("start", at);
    a->setDouble("length", bar);
    auto res = sq::makeAttributes();
    sq_result r;
    {
      EditScope scope(ctx_, "Create MIDI Clip");
      r = ctx_.exec(sq::doc::kAddClip, a.get(), res.get());
      if (SQ_FAILED(r)) scope.cancel();
    }
    sq_guid id{};
    if (SQ_SUCCEEDED(r) && res->getGuid("node", &id) == SQ_OK) {
      ctx_.selectTrack(t.id);
      ctx_.setSelection({id});
      ctx_.openClipEditor(id);
      ctx_.setStatus("Created a MIDI clip on " + t.name);
    }
  }
  bool emptyMouse(const WidgetMouse& e) {
    if (e.action != MouseAction::Down || e.button != MouseButton::Left) return frame().contains(e.x, e.y);
    const int h = headerHit(e.x, e.y);
    if (h == kHCreateClip) createClip();
    else if (h == kHAddTrack) ctx_.run("Track.AddInstrument");
    ctx_.invalidate();
    return true;
  }

  // ==== input ======================================================================================
  void hover(const WidgetMouse& e) {
    int hh = headerHit(e.x, e.y);
    int hp = -1, hn = -1, hz = 0, gr = -1;
    bool hre = false, hd = false;
    if (haveClip_ && clip_.info.kind == SQ_CLIP_MIDI) {
      layoutRegions();
      if (grid_.contains(e.x, e.y) || keys_.contains(e.x, e.y)) {
        const int r = rowAt(e.y);
        if (r >= 0 && r < static_cast<int>(rows_.size())) hp = rows_[static_cast<size_t>(r)];
        if (grid_.contains(e.x, e.y)) {
          hn = hitNote(e.x, e.y, &hz);
          gr = r < static_cast<int>(rows_.size()) ? r : -1;
          ghostPos_ = ppqAt(e.x);
        }
      }
      hre = ruler_.contains(e.x, e.y) && std::fabs(e.x - xOf(clipLen())) <= 7;
      hd = div_.contains(e.x, e.y);
    } else if (haveClip_) {
      if (audio_->mouse(e, clip_)) ctx_.invalidate();
    }
    if (hh != hotHdr_ || hp != hoverPitch_ || hn != hotNote_ || hz != hotZone_ || hre != hotRulerEnd_ || hd != hotDiv_ || gr != ghostRow_ ||
        tool_ == Tool::Draw) {
      hotHdr_ = hh;
      hoverPitch_ = hp;
      hotNote_ = hn;
      hotZone_ = hz;
      hotRulerEnd_ = hre;
      hotDiv_ = hd;
      ghostRow_ = gr;
      ctx_.invalidate();
    }
  }

  bool wheel(const WidgetMouse& e) {
    if (!frame().contains(e.x, e.y)) return false;
    const unsigned m = e.modifiers;
    const bool ctrl = (m & kModCtrl) != 0, shift = (m & kModShift) != 0, alt = (m & kModAlt) != 0;
    const float notches = e.wheel;
    if (alt || (ctrl && shift)) zoomY(std::pow(1.15f, notches), std::clamp(e.y, grid_.y, grid_.bottom()));
    else if (ctrl) zoomX(std::pow(1.2, static_cast<double>(notches)), std::clamp(e.x, grid_.x, grid_.right()));
    else if (shift || vel_.contains(e.x, e.y) || ruler_.contains(e.x, e.y) || hbar_.contains(e.x, e.y)) {
      scrollX_ -= static_cast<double>(notches) * visiblePpq() * 0.12;
      clampScroll();
    } else {
      scrollY_ -= notches * rowH_ * 3;
      clampScroll();
    }
    ctx_.invalidate();
    return true;
  }

  void headerClick(int id, const WidgetMouse& e) {
    const bool shift = (e.modifiers & kModShift) != 0;
    switch (id) {
      case kHToolSelect: tool_ = Tool::Select; break;
      case kHToolDraw: tool_ = Tool::Draw; break;
      case kHToolErase: tool_ = Tool::Erase; break;
      case kHTriplet: triplet_ = !triplet_; break;
      case kHSnap: snap_ = !snap_; break;
      case kHFold:
        fold_ = !fold_;
        buildRows();
        fitView(true);
        break;
      case kHFollow: follow_ = !follow_; break;
      case kHQuantize: quantizeNotes(); break;
      case kHLegato: legatoNotes(); break;
      case kHTransDown: transpose(shift ? -12 : -1, false); break;
      case kHTransUp: transpose(shift ? 12 : 1, false); break;
      case kHZoomIn: zoomX(1.5, grid_.x + grid_.w / 2); break;
      case kHZoomOut: zoomX(1 / 1.5, grid_.x + grid_.w / 2); break;
      case kHZoomFit: fitView(true); break;
      case kHGrid: ctx_.showMenu(gridMenu(), Point{hitRect(id).x, header_.bottom()}); break;
      case kHLength: ctx_.showMenu(lengthMenu(), Point{hitRect(id).x, header_.bottom()}); break;
      case kHQStrength: ctx_.showMenu(strengthMenu(), Point{hitRect(id).x, header_.bottom()}); break;
      case kHMore: ctx_.showMenu(overflowMenu(), Point{hitRect(id).x, header_.bottom()}); break;
      default: break;
    }
    ctx_.invalidate();
  }
  Rect hitRect(int id) const {
    for (const auto& h : hits_)
      if (h.id == id) return h.r;
    return Rect{};
  }

  void beginDrag(Drag d, const WidgetMouse& e) {
    drag_ = d;
    dx0_ = curX_ = e.x;
    dy0_ = curY_ = e.y;
    moved_ = false;
    mods0_ = e.modifiers;
    base_ = notes_;
    dragInfo_.clear();
  }

  bool down(const WidgetMouse& e) {
    const Rect& f = frame();
    if (!f.contains(e.x, e.y)) return false;
    flushPreview(true);
    const int h = headerHit(e.x, e.y);
    if (h >= 0) {
      if (e.button != MouseButton::Left) return true;
      if (h == kHVel) {
        beginDrag(Drag::HeaderVel, e);
        velStart_ = newVel_;
        return true;
      }
      headerClick(h, e);
      return true;
    }
    if (header_.contains(e.x, e.y)) return true;
    if (div_.contains(e.x, e.y)) {
      beginDrag(Drag::VelSplit, e);
      velStartH_ = velH_;
      return true;
    }
    if (hbar_.contains(e.x, e.y)) {
      const Rect t = hThumb();
      if (!t.contains(e.x, e.y)) {
        scrollX_ += (e.x < t.x ? -1.0 : 1.0) * visiblePpq() * 0.9;
        clampScroll();
      }
      beginDrag(Drag::ScrollH, e);
      scrollStartX_ = scrollX_;
      ctx_.invalidate();
      return true;
    }
    if (vbar_.contains(e.x, e.y)) {
      const Rect t = vThumb();
      if (!t.contains(e.x, e.y)) {
        scrollY_ += (e.y < t.y ? -1.0f : 1.0f) * grid_.h * 0.9f;
        clampScroll();
      }
      beginDrag(Drag::ScrollV, e);
      scrollStartY_ = scrollY_;
      ctx_.invalidate();
      return true;
    }
    if (ruler_.contains(e.x, e.y)) {
      if (e.button != MouseButton::Left) return true;
      if (std::fabs(e.x - xOf(clipLen())) <= 7) {
        beginDrag(Drag::ClipEnd, e);
        dragClipLen_ = clip_.info.length;
        return true;
      }
      double rel = ppqAt(e.x);
      if (snapping(e.modifiers)) rel = snapRound(rel);
      ctx_.setCursor(clipStart() + rel);
      return true;
    }
    if (keys_.contains(e.x, e.y)) {
      const int pch = pitchAtRow(rowAt(e.y));
      beginDrag(Drag::Keys, e);
      keyPitch_ = pch;
      preview(pch, newVel_, 0);
      ctx_.invalidate();
      return true;
    }
    if (vel_.contains(e.x, e.y)) return velDown(e);
    if (grid_.contains(e.x, e.y)) return gridDown(e);
    return true;
  }

  bool velDown(const WidgetMouse& e) {
    if (e.button != MouseButton::Left) return true;
    // the nearest stem (selected notes first)
    int best = -1;
    float bestD = 6;
    for (int pass = 1; pass >= 0 && best < 0; --pass)
      for (size_t i = 0; i < notes_.size(); ++i) {
        if (notes_[i].sel != (pass == 1)) continue;
        const float d = std::fabs(e.x - (xOf(notes_[i].pos) + 1.5f));
        if (d < bestD) {
          bestD = d;
          best = static_cast<int>(i);
        }
      }
    if (best >= 0) {
      beginDrag(Drag::Velocity, e);
      anchor_ = best;
      velStart_ = notes_[static_cast<size_t>(best)].vel;
      dragInfo_ = "Velocity " + std::to_string(velStart_);
      return true;
    }
    beginDrag(Drag::VelPaint, e);
    velPaint(e.x, e.x, e.y);
    return true;
  }
  int velAtY(float y) const {
    const float top = vel_.y + 4, h = vel_.h - 8;
    return std::clamp(static_cast<int>(std::lround((top + h - y) / h * 127)), 1, 127);
  }
  void velPaint(float xa, float xb, float y) {
    const float lo = std::min(xa, xb) - 2, hi = std::max(xa, xb) + 2;
    const bool any = anySelected();
    const int v = velAtY(y);
    for (Note& n : notes_) {
      if (any && !n.sel) continue;
      const float x = xOf(n.pos);
      if (x >= lo && x <= hi) {
        n.vel = v;
        moved_ = true;
      }
    }
    dragInfo_ = "Velocity " + std::to_string(v);
  }

  bool gridDown(const WidgetMouse& e) {
    int zone = kZoneBody;
    const int hit = hitNote(e.x, e.y, &zone);
    const bool shift = (e.modifiers & kModShift) != 0, ctrl = (e.modifiers & kModCtrl) != 0;
    if (e.button == MouseButton::Right) {
      if (hit >= 0 && !notes_[static_cast<size_t>(hit)].sel) selectOnly(hit);
      contextMenu(hit, Point{e.x, e.y});
      ctx_.invalidate();
      return true;
    }
    if (e.button != MouseButton::Left) return true;
    if (tool_ == Tool::Erase) {
      beginDrag(Drag::Erase, e);
      eraseAt(e.x, e.y);
      return true;
    }
    if (hit >= 0) {
      Note& n = notes_[static_cast<size_t>(hit)];
      toggleOnUp_ = false;
      selectOnlyOnUp_ = false;
      if (shift) {
        n.sel = !n.sel;
        if (!n.sel) {
          ctx_.invalidate();
          return true;
        }
      } else if (!n.sel) {
        if (!ctrl) selectOnly(hit);
        n.sel = true;
      } else if (ctrl) {
        toggleOnUp_ = true;
      } else {
        selectOnlyOnUp_ = true;
      }
      beginDrag(zone == kZoneRight ? Drag::ResizeR : zone == kZoneLeft ? Drag::ResizeL : Drag::Move, e);
      anchor_ = hit;
      lastPitch_ = n.pitch;
      lastLen_ = n.len;
      preview(n.pitch, n.vel, n.chan);
      ctx_.invalidate();
      return true;
    }
    if (tool_ == Tool::Draw) return drawDown(e);
    // rubber band
    if (!(shift || ctrl))
      for (Note& n : notes_) n.sel = false;
    beginDrag(Drag::Rubber, e);
    ctx_.invalidate();
    return true;
  }

  // Adds a note at the mouse (draw tool / double-click). Returns false outside the clip.
  bool addNoteAt(float x, float y, unsigned mods) {
    double pos = ppqAt(x);
    if (snapping(mods)) pos = snapFloor(pos);
    const int r = rowAt(y);
    if (r < 0 || r >= static_cast<int>(rows_.size())) return false;
    if (pos < -1e-9 || pos >= clipLen() - 1e-9) {
      ctx_.setStatus("Notes go inside the clip - drag the clip end marker in the ruler to make it longer");
      return false;
    }
    pos = std::max(0.0, pos);
    Note n;
    n.pos = pos;
    n.len = std::min(defaultLen(), clipLen() - pos);
    n.pitch = rows_[static_cast<size_t>(r)];
    n.vel = newVel_;
    n.sel = true;
    for (Note& o : notes_) o.sel = false;
    notes_.push_back(n);
    preview(n.pitch, n.vel, n.chan);
    return true;
  }
  bool drawDown(const WidgetMouse& e) {
    if (!addNoteAt(e.x, e.y, e.modifiers)) return true;
    beginDrag(Drag::Draw, e);
    anchor_ = static_cast<int>(notes_.size()) - 1;
    lastPitch_ = notes_.back().pitch;
    ctx_.invalidate();
    return true;
  }

  void eraseAt(float x, float y) {
    const int hit = hitNote(x, y, nullptr);
    if (hit < 0) return;
    notes_.erase(notes_.begin() + hit);
    moved_ = true;
    ctx_.invalidate();
  }

  bool move(const WidgetMouse& e) {
    if (drag_ == Drag::None) return false;
    const float prevX = curX_;
    curX_ = e.x;
    curY_ = e.y;
    if (std::fabs(e.x - dx0_) > 3 || std::fabs(e.y - dy0_) > 3) moved_ = true;
    const unsigned mods = e.modifiers;
    switch (drag_) {
      case Drag::HeaderVel: {
        newVel_ = std::clamp(velStart_ + static_cast<int>(std::lround((dy0_ - e.y) * 0.75f)), 1, 127);
        break;
      }
      case Drag::VelSplit: velH_ = velStartH_ + (dy0_ - e.y); break;
      case Drag::ScrollH: {
        double lo = 0, hi = 0;
        xRange(lo, hi);
        const double range = std::max(hi - lo, visiblePpq());
        scrollX_ = scrollStartX_ + static_cast<double>(e.x - dx0_) / hbar_.w * range;
        clampScroll();
        break;
      }
      case Drag::ScrollV: {
        const float total = std::max(grid_.h, static_cast<float>(rows_.size()) * rowH_);
        scrollY_ = scrollStartY_ + (e.y - dy0_) / vbar_.h * total;
        clampScroll();
        break;
      }
      case Drag::Keys: {
        const int pch = pitchAtRow(rowAt(e.y));
        if (pch != keyPitch_) {
          keyPitch_ = pch;
          preview(pch, newVel_, 0);
        }
        break;
      }
      case Drag::ClipEnd: {
        double len = ppqAt(e.x);
        if (snapping(mods)) len = snapRound(len);
        dragClipLen_ = std::max(snapping(mods) ? gridStep() : 1.0 / 16, len);
        dragInfo_ = "Clip end " + ctx_.formatBars(clipStart() + dragClipLen_);
        break;
      }
      case Drag::Rubber: rubber(mods); break;
      case Drag::Erase: eraseAt(e.x, e.y); break;
      case Drag::Velocity: {
        const float h = vel_.h - 8;
        const int v = std::clamp(velStart_ + static_cast<int>(std::lround((dy0_ - e.y) / h * 127)), 1, 127);
        const int delta = v - velStart_;
        const bool group = base_[static_cast<size_t>(anchor_)].sel;
        for (size_t i = 0; i < notes_.size(); ++i)
          if ((group && base_[i].sel) || static_cast<int>(i) == anchor_) notes_[i].vel = std::clamp(base_[i].vel + delta, 1, 127);
        dragInfo_ = "Velocity " + std::to_string(v);
        break;
      }
      case Drag::VelPaint: velPaint(prevX, e.x, e.y); break;
      case Drag::Move: dragMove(e); break;
      case Drag::ResizeR: dragResizeR(e); break;
      case Drag::ResizeL: dragResizeL(e); break;
      case Drag::Draw: dragDraw(e); break;
      case Drag::None: break;
    }
    ctx_.invalidate();
    return true;
  }

  void rubber(unsigned mods) {
    const bool additive = (mods0_ & (kModShift | kModCtrl)) != 0;
    const float x0 = std::min(dx0_, curX_), x1 = std::max(dx0_, curX_), y0 = std::min(dy0_, curY_), y1 = std::max(dy0_, curY_);
    (void)mods;
    for (size_t i = 0; i < notes_.size(); ++i) {
      const Note& n = notes_[i];
      bool in = false;
      if (rowOfPitch(n.pitch) >= 0) {
        const Rect r = noteRect(n);
        in = r.x <= x1 && r.right() >= x0 && r.y <= y1 && r.bottom() >= y0;
      }
      notes_[i].sel = additive ? (base_[i].sel != in) : in;
    }
  }

  void dragMove(const WidgetMouse& e) {
    if (!moved_) return;
    const Note& a = base_[static_cast<size_t>(anchor_)];
    double np = a.pos + (ppqAt(e.x) - ppqAt(dx0_));
    if (snapping(e.modifiers)) np = snapRound(np);
    double delta = np - a.pos;
    // clamp so every moved note stays inside the clip
    double lo = 1e300, hi = -1e300;
    for (const Note& n : base_)
      if (n.sel) {
        lo = std::min(lo, n.pos);
        hi = std::max(hi, n.pos);
      }
    const double maxStart = std::max(0.0, clipLen() - std::min(gridStep(), 0.25));
    delta = std::clamp(delta, -lo, std::max(-lo, maxStart - hi));
    // rows
    int dRow = rowAt(e.y) - rowAt(dy0_);
    int rlo = 1 << 30, rhi = -1;
    for (const Note& n : base_)
      if (n.sel) {
        const int r = rowOfPitch(n.pitch);
        if (r < 0) continue;
        rlo = std::min(rlo, r);
        rhi = std::max(rhi, r);
      }
    if (rhi >= 0) dRow = std::clamp(dRow, -rlo, static_cast<int>(rows_.size()) - 1 - rhi);
    else dRow = 0;
    const bool copy = (mods0_ & kModCtrl) != 0;
    notes_ = base_;
    std::vector<Note> copies;
    for (Note& n : notes_) {
      if (!n.sel) continue;
      Note m = n;
      m.pos += delta;
      const int r = rowOfPitch(m.pitch);
      if (r >= 0) m.pitch = rows_[static_cast<size_t>(r + dRow)];
      if (copy) {
        n.sel = false;
        m.src = -1;
        copies.push_back(m);
      } else {
        n = m;
      }
    }
    notes_.insert(notes_.end(), copies.begin(), copies.end());
    const int ap = a.pitch;
    const int r = rowOfPitch(ap);
    const int newPitch = r >= 0 ? rows_[static_cast<size_t>(r + dRow)] : ap;
    if (newPitch != lastPitch_) {
      lastPitch_ = newPitch;
      preview(newPitch, a.vel, a.chan);
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s%s  %s", copy ? "Copy  " : "", ctx_.formatBars(clipStart() + a.pos + delta).c_str(), pitchName(newPitch).c_str());
    dragInfo_ = buf;
  }

  void dragResizeR(const WidgetMouse& e) {
    const Note& a = base_[static_cast<size_t>(anchor_)];
    double ne = a.end() + (ppqAt(e.x) - ppqAt(dx0_));
    if (snapping(e.modifiers)) ne = snapRound(ne);
    const double d = ne - a.end();
    const double ml = minLen(e.modifiers);
    notes_ = base_;
    for (Note& n : notes_) {
      if (!n.sel) continue;
      n.len = std::max(ml, n.len + d);
      if (n.end() > clipLen()) n.len = std::max(ml, clipLen() - n.pos);
    }
    lastLen_ = notes_[static_cast<size_t>(anchor_)].len;
    dragInfo_ = "Length " + lengthText(lastLen_);
  }

  void dragResizeL(const WidgetMouse& e) {
    const Note& a = base_[static_cast<size_t>(anchor_)];
    double ns = a.pos + (ppqAt(e.x) - ppqAt(dx0_));
    if (snapping(e.modifiers)) ns = snapRound(ns);
    const double d = ns - a.pos;
    const double ml = minLen(e.modifiers);
    notes_ = base_;
    for (Note& n : notes_) {
      if (!n.sel) continue;
      const double end = n.end();
      const double np = std::clamp(n.pos + d, 0.0, std::max(0.0, end - ml));
      n.pos = np;
      n.len = end - np;
    }
    lastLen_ = notes_[static_cast<size_t>(anchor_)].len;
    dragInfo_ = "Start " + ctx_.formatBars(clipStart() + notes_[static_cast<size_t>(anchor_)].pos);
  }

  void dragDraw(const WidgetMouse& e) {
    Note& n = notes_[static_cast<size_t>(anchor_)];
    // pitch follows the mouse
    const int r = rowAt(e.y);
    const int pch = pitchAtRow(r);
    if (pch >= 0 && pch != n.pitch) {
      n.pitch = pch;
      preview(pch, n.vel, n.chan);
    }
    if (std::fabs(e.x - dx0_) > 4) {
      double end = ppqAt(e.x);
      const double ml = minLen(e.modifiers);
      if (snapping(e.modifiers)) end = std::ceil((clipStart() + end) / gridStep() - 1e-9) * gridStep() - clipStart();
      n.len = std::clamp(end - n.pos, ml, std::max(ml, clipLen() - n.pos));
      lastLen_ = n.len;
    }
    dragInfo_ = pitchName(n.pitch) + "  " + lengthText(n.len);
  }

  std::string lengthText(double ppq) const {
    // as a fraction of a whole note when it is a simple one
    static const std::pair<double, const char*> kNames[] = {{4, "1/1"}, {2, "1/2"}, {1, "1/4"}, {0.5, "1/8"}, {0.25, "1/16"}, {0.125, "1/32"},
                                                           {0.0625, "1/64"}, {3, "3/4"}, {1.5, "3/8"}, {0.75, "3/16"}};
    for (const auto& [v, n] : kNames)
      if (std::fabs(ppq - v) < 1e-6) return n;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f beats", ppq / beatLen());
    return buf;
  }

  bool up(const WidgetMouse& e) {
    const Drag d = drag_;
    if (d == Drag::None) return frame().contains(e.x, e.y);
    drag_ = Drag::None;
    dragInfo_.clear();
    switch (d) {
      case Drag::HeaderVel:
        if (!moved_) {
          ctx_.prompt("Velocity for new notes (1-127)", std::to_string(newVel_), [this](const std::string& v) {
            const int n = std::atoi(v.c_str());
            if (n >= 1 && n <= 127) newVel_ = n;
            ctx_.invalidate();
          });
        }
        break;
      case Drag::ClipEnd:
        if (moved_ && std::fabs(dragClipLen_ - clip_.info.length) > 1e-9) {
          auto a = sq::makeAttributes();
          a->setGuid("node", &clipId_);
          a->setDouble("start", clipStart());
          a->setDouble("length", dragClipLen_);
          EditScope scope(ctx_, "Resize Clip");
          if (SQ_FAILED(ctx_.exec(sq::doc::kResizeClip, a.get()))) scope.cancel();
        }
        break;
      case Drag::Erase:
        if (moved_) commit("Delete Notes");
        break;
      case Drag::Velocity:
      case Drag::VelPaint:
        if (moved_) commit("Change Velocity");
        else notes_ = base_;
        break;
      case Drag::Draw: commit("Add Note"); break;
      case Drag::Move:
        if (moved_) {
          commit((mods0_ & kModCtrl) ? "Copy Notes" : "Move Notes");
        } else {
          notes_ = base_;
          if (toggleOnUp_) notes_[static_cast<size_t>(anchor_)].sel = false;
          else if (selectOnlyOnUp_) selectOnly(anchor_);
        }
        break;
      case Drag::ResizeL:
      case Drag::ResizeR:
        if (moved_) commit("Resize Notes");
        else {
          notes_ = base_;
          if (toggleOnUp_) notes_[static_cast<size_t>(anchor_)].sel = false;
          else if (selectOnlyOnUp_) selectOnly(anchor_);
        }
        break;
      default: break;
    }
    ctx_.invalidate();
    return true;
  }

  bool doubleClick(const WidgetMouse& e) {
    if (e.button != MouseButton::Left) return false;
    if (keys_.contains(e.x, e.y)) {  // select every note of that pitch
      const int pch = pitchAtRow(rowAt(e.y));
      const bool add = (e.modifiers & (kModShift | kModCtrl)) != 0;
      for (Note& n : notes_) n.sel = (add && n.sel) || n.pitch == pch;
      ctx_.invalidate();
      return true;
    }
    if (!grid_.contains(e.x, e.y) || tool_ != Tool::Select) return grid_.contains(e.x, e.y);
    drag_ = Drag::None;
    const int hit = hitNote(e.x, e.y, nullptr);
    if (hit >= 0) {
      notes_.erase(notes_.begin() + hit);
      commit("Delete Note");
    } else if (addNoteAt(e.x, e.y, e.modifiers)) {
      lastLen_ = notes_.back().len;
      commit("Add Note");
    }
    ctx_.invalidate();
    return true;
  }

  void contextMenu(int hit, Point at) {
    const bool any = anySelected();
    std::vector<MenuItem> m;
    auto add = [&m](const std::string& lbl, std::function<void()> fn, const std::string& sc = {}, bool on = true) {
      MenuItem it;
      it.label = lbl;
      it.action = std::move(fn);
      it.shortcut = sc;
      it.enabled = on;
      m.push_back(std::move(it));
    };
    add("Delete", [this] { deleteSelected(); }, "Del", any);
    add("Duplicate", [this] { duplicateSelection(); }, "Ctrl+D", any);
    add("Copy", [this] { copySelection(); }, "Ctrl+C", any);
    add("Paste", [this] { paste(); }, "Ctrl+V", !clipboard_.empty());
    m.push_back(MenuItem::sep());
    add(any ? "Quantize Selection" : "Quantize All", [this] { quantizeNotes(); }, "Q", !notes_.empty());
    add("Legato", [this] { legatoNotes(); }, {}, !notes_.empty());
    add("Transpose +12", [this] { transpose(12, false); }, "Shift+Up", !notes_.empty());
    add("Transpose -12", [this] { transpose(-12, false); }, "Shift+Down", !notes_.empty());
    add("Velocity...", [this] {
      int v = newVel_;
      for (const Note& n : notes_)
        if (n.sel) {
          v = n.vel;
          break;
        }
      ctx_.prompt("Velocity (1-127)", std::to_string(v), [this](const std::string& s) {
        const int n = std::atoi(s.c_str());
        if (n >= 1 && n <= 127) setVelocity(n);
      });
    }, {}, !notes_.empty());
    m.push_back(MenuItem::sep());
    add("Select All", [this] { selectAll(); }, "Ctrl+A", !notes_.empty());
    if (hit >= 0) {
      const int pch = notes_[static_cast<size_t>(hit)].pitch;
      add("Select All " + pitchName(pch), [this, pch] {
        for (Note& n : notes_) n.sel = n.pitch == pch;
        ctx_.invalidate();
      });
    }
    ctx_.showMenu(std::move(m), at);
  }

  struct Pending {
    Clock::time_point at;
    int pitch, chan;
  };

  GuiContext& ctx_;
  std::unique_ptr<AudioInspector> audio_;
  // cache
  uint64_t loadedSerial_ = 0;
  sq_guid clipId_{};
  bool haveClip_ = false;
  ClipView clip_;
  std::string trackName_;
  Color trackCol_;
  int num_ = 4, den_ = 4;
  std::vector<sq_clip_event> events_;
  std::vector<Note> notes_, base_, clipboard_;
  double clipboardSpan_ = 0, lastPasteAt_ = -1;
  std::vector<int> rows_;
  std::array<int, 128> rowOf_{};
  // view
  double scrollX_ = 0, ppx_ = 40;
  float scrollY_ = 0, rowH_ = 14, velH_ = 64;
  bool needFit_ = true;
  // settings
  Tool tool_ = Tool::Select;
  int gridIdx_ = 4, lenIdx_ = 0, newVel_ = 100;
  bool triplet_ = false, snap_ = true, fold_ = false, follow_ = true, drums_ = false;
  bool dry_ = false, compact_ = false;  // header layout passes
  int shownGroups_ = kGCount;
  double qStrength_ = 1.0, lastLen_ = 0;
  // regions
  Rect header_, ruler_, corner_, grid_, keys_, vel_, velLabel_, div_, hbar_, vbar_;
  std::vector<HitBox> hits_;
  // hover
  int hotHdr_ = -1, hoverPitch_ = -1, hotNote_ = -1, hotZone_ = 0, ghostRow_ = -1;
  double ghostPos_ = 0;
  bool hotRulerEnd_ = false, hotDiv_ = false;
  // drag
  Drag drag_ = Drag::None;
  float dx0_ = 0, dy0_ = 0, curX_ = 0, curY_ = 0;
  bool moved_ = false, toggleOnUp_ = false, selectOnlyOnUp_ = false;
  unsigned mods0_ = 0;
  int anchor_ = -1, lastPitch_ = -1, keyPitch_ = -1, velStart_ = 0;
  float velStartH_ = 0, scrollStartY_ = 0;
  double scrollStartX_ = 0, dragClipLen_ = 0;
  std::string dragInfo_;
  // preview
  std::vector<Pending> pending_;
  int injectState_ = -1, previewPitch_ = -1;
};

}  // namespace

std::unique_ptr<Widget> makeClipEditor(GuiContext& ctx) { return std::make_unique<ClipEditor>(ctx); }

}  // namespace sq::app::gui
