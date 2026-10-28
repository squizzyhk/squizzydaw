// Squizzy Studio GUI - arrange view (timeline) internals, shared by gui_arrange*.cpp.
//
//   gui_arrange.cpp        model cache, layout/geometry, time helpers, edits, menus, keyboard, drops
//   gui_arrange_paint.cpp  drawing
//   gui_arrange_mouse.cpp  mouse interaction (hit testing, drags, gestures)
//
// Layout (DIPs, inside the widget frame):
//   +--------------------------------------------------------------------------+
//   | toolbar                                                                  |
//   +---------------+----------------------------------------------------------+
//   | position      | marker strip                                             |
//   | readout       | ruler (upper half: labels / locate, lower half: loop)    |
//   +---------------+----------------------------------------------------------+-+
//   | track headers | lanes (clips, automation lanes)                          |v|
//   |               |                                                          |s|
//   +---------------+----------------------------------------------------------+-+
//   | zoom info     | horizontal scrollbar                                     |
//   +---------------+----------------------------------------------------------+
// Every document change goes through GuiContext::exec / run; one undo step per gesture (drags are
// previewed locally and committed on mouse up inside an EditScope).
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gui_context.hpp"
#include "gui_panels.hpp"
#include "gui_waveform.hpp"
#include "sq/engine/audio/builtin.hpp"

namespace sq::app::gui::arrange {

enum class Tool { Pointer, Split, Draw };

struct NoteMini {
  float pos = 0, len = 0;  // ppq relative to the clip start
  uint8_t pitch = 60, vel = 100;
};

struct ClipEntry {
  ClipView v;
  std::vector<NoteMini> notes;  // MIDI preview
  int lo = 60, hi = 72;         // pitch range of the preview
  double end() const { return v.info.start + v.info.length; }
};

struct AutoLane {
  std::string target;                             // automation.hpp target ("volume", "pan", ...)
  std::vector<std::pair<double, double>> points;  // (ppq, plain value)
  bool stored = false;                            // exists in the document
  // instrument.<p> / insert.<slot>.<p> of a built-in device: its parameter (static catalog entry)
  // and a display label ("Synth: Cutoff"); null / empty for track targets and unknown devices.
  const engine::audio::ParamInfo* param = nullptr;
  std::string label;
  float y = 0, h = 0;                             // content coordinates
};

struct Lane {
  TrackView t;
  Color color;
  std::vector<ClipEntry> clips;  // sorted by start
  std::vector<AutoLane> autos;   // shown automation lanes (when expanded)
  float y = 0, h = 0;            // track row, content coordinates (0 = top of the lane area)
  float total = 0;               // row + automation lanes
  bool showAuto = false;
  int number = 0;  // 1-based display number
  bool audio() const { return t.info.kind == SQ_TRACK_AUDIO; }
  bool instrument() const { return t.info.kind == SQ_TRACK_INSTRUMENT; }
};

struct MarkerView {
  sq_doc_marker m{};
  float x = 0, w = 0;  // last painted label box
};

// Value range of an automation target (plain values).
struct AutoRange {
  double lo = 0, hi = 1;
  bool stepped = false;
  const char* unit = "";
  bool log = false;  // logarithmic display (frequencies, times)
};
// pi: the device parameter of an instrument./insert. target (AutoLane::param), null otherwise.
AutoRange autoRange(const std::string& target, const std::vector<std::pair<double, double>>& pts,
                    const engine::audio::ParamInfo* pi = nullptr);
std::string autoLabel(const std::string& target);
std::string formatAutoValue(const std::string& target, double v, const engine::audio::ParamInfo* pi = nullptr);

// A copied clip (Edit > Copy): everything needed to recreate it, independent of the document.
struct ClipCopy {
  sq_clip_info info{};
  std::string name, source;
  std::vector<sq_clip_event> events;
  int laneOffset = 0;  // track offset from the topmost copied clip's track
  sq_guid track{};     // source track (fallback when no track is selected)
};

enum class Drag {
  None,
  Locate,      // ruler upper half: scrub the playhead
  LoopNew,     // ruler lower half: draw a new loop range
  LoopStart,
  LoopEnd,
  LoopMove,
  MarkerMove,
  HeaderResize,
  LaneResize,
  Rubber,
  ClipMove,
  TrimLeft,
  TrimRight,
  FadeIn,
  FadeOut,
  AutoPoint,
  VScroll,
  HScroll,
  Volume,
  Pan,
  Pending,  // Alt+press on a clip: split on release, copy-drag on move
};

enum class HitKind {
  None,
  Toolbar,
  Corner,
  Marker,
  MarkerStrip,
  Ruler,
  LoopZone,
  HeaderBorder,
  Header,
  HeaderName,
  HeaderButton,  // button = index (see HeaderButton)
  HeaderVolume,
  HeaderPan,
  LaneBorder,
  Clip,
  ClipEdgeL,
  ClipEdgeR,
  FadeInHandle,
  FadeOutHandle,
  LaneEmpty,
  AutoHeader,
  AutoLane,
  AutoPoint,
  BelowTracks,
  VScrollbar,
  HScrollbar,
};

enum HeaderButton { kBtnMute = 0, kBtnSolo, kBtnArm, kBtnMonitor, kBtnAuto, kBtnCount };

struct Hit {
  HitKind kind = HitKind::None;
  int lane = -1;
  int clip = -1;
  int autoLane = -1;
  int point = -1;
  int button = -1;
  int marker = -1;
  double ppq = 0;
};

struct ToolButton {
  Rect r;
  int id = 0;
};

class ArrangeView final : public Widget, public CharInput, public EditTarget {
 public:
  explicit ArrangeView(GuiContext& ctx);
  ~ArrangeView() override;

  void paint(Canvas& c) override;
  bool onMouse(const WidgetMouse& e) override;
  bool onKey(Key k, unsigned mods) override;
  bool onChar(uint32_t ch, unsigned mods) override;
  bool editCommand(std::string_view cmd) override;
  bool focusable() const override { return true; }

  bool dropFile(float x, float y, const std::string& path);
  void dragOver(float x, float y);

  // ---- constants ----
  static constexpr float kToolbarH = 32, kMarkerH = 18, kRulerH = 26, kScrollW = 11, kAutoH = 64;
  static constexpr float kMinLaneH = 30, kMaxLaneH = 320, kDefaultLaneH = 66;

  // ---- model (gui_arrange.cpp) ----
  GuiContext& ctx_;
  PeakCache peaks_;
  uint64_t serial_ = 0;
  std::vector<Lane> lanes_;
  std::vector<MarkerView> markers_;
  sq::Ptr<ITempoMap> tempo_;
  struct Sig {
    double pos;
    uint32_t num, den;
  };
  std::vector<Sig> sigs_;
  double songEnd_ = 0;  // end of the last clip (ppq)
  float contentH_ = 0;
  EditStateView es_;
  std::map<sq_guid, float> laneHeights_;
  std::map<sq_guid, bool> autoShown_;
  std::map<sq_guid, std::vector<std::string>> autoExtra_;  // UI-only lanes (no points yet)
  std::map<sq_guid, float> meterHold_;
  double punchIn_ = 0, punchOut_ = 0;  // punch range marker (punchOut_ <= punchIn_: none)
  // live recording region: transport position when recording started (GUI-observed)
  bool wasRecording_ = false;
  bool lastPlaying_ = false;
  double lastPos_ = 0, recStart_ = 0;
  void trackRecording();

  void refresh();
  void rebuild();
  void relayout();
  int laneIndex(const sq_guid& track) const;
  const ClipEntry* findClip(const sq_guid& id, int* lane = nullptr) const;
  bool selected(const sq_guid& clip) const;

  // ---- view state ----
  float headerW_ = 210;
  double ppx_ = 18;     // pixels per ppq (quarter note)
  double scroll_ = 0;   // ppq at the left edge of the lane area
  float scrollY_ = 0;   // DIPs
  Tool tool_ = Tool::Pointer;
  bool follow_ = true;
  bool wasPlaying_ = false;

  // ---- geometry ----
  Rect toolbarRect() const;
  Rect cornerRect() const;
  Rect markerRect() const;
  Rect rulerRect() const;
  Rect lanesRect() const;
  Rect headersRect() const;
  Rect vbarRect() const;
  Rect hbarRect() const;
  float xOf(double ppq) const;
  double ppqAt(float x) const;
  float laneTop(const Lane& l) const;  // screen y of the track row
  Rect clipRect(const Lane& l, const ClipEntry& c) const;
  Rect headerButtonRect(const Lane& l, int button) const;
  Rect volumeRect(const Lane& l) const;
  Rect panRect(const Lane& l) const;
  Rect nameRect(const Lane& l) const;
  float fadeInX(const ClipEntry& c) const;
  float fadeOutX(const ClipEntry& c) const;
  double visibleSpan() const;
  double contentEnd() const;  // scrollable timeline length
  void clampScroll();
  void setZoom(double ppx, float anchorX);
  void zoomToFit();
  void ensureVisible(double ppq);

  // ---- time / grid ----
  double barLength(double ppq) const;   // ppq of the bar containing ppq
  double barStart(double ppq) const;
  double beatLength(double ppq) const;  // ppq of one beat (signature denominator)
  double snap(double ppq, unsigned mods) const;
  double snapFloor(double ppq) const;
  bool snapOn() const { return es_.snap != "off"; }
  double secAt(double ppq) const;
  double ppqAtSec(double sec) const;
  // Calls fn(barIndex(0-based), ppq, barLen, beatLen, numerator) for every bar starting in [from, to).
  template <class Fn>
  void forEachBar(double from, double to, Fn&& fn) const;

  // ---- automation ----
  // Resolves an instrument./insert. target of a track to its built-in parameter + label.
  void resolveAutoParam(const TrackView& t, AutoLane* a);
  // The built-in devices of a track: slot -1 = instrument, else insert slot (bypassed lines count).
  struct DeviceSlot {
    int slot = -1;
    const engine::audio::DeviceInfo* info = nullptr;
  };
  std::vector<DeviceSlot> trackDevices(const TrackView& t);
  static std::string deviceShortName(const engine::audio::DeviceInfo& d);
  void setAutomation(const sq_guid& track, const std::string& target, const std::vector<std::pair<double, double>>& pts);
  float autoValueY(const AutoLane& a, const Rect& r, double v) const;
  double autoValueAt(const AutoLane& a, const Rect& r, float y) const;
  Rect autoLaneRect(const Lane& l, const AutoLane& a) const;  // screen rect (lane area part)

  // ---- edits (gui_arrange.cpp) ----
  sq_guid copyClip(const ClipEntry& c, const sq_guid& track, double start);
  ClipCopy snapshot(const ClipEntry& c);
  sq_guid addClipCopy(const ClipCopy& cc, const sq_guid& track, double start);
  bool copySelection();
  bool pasteClipboard();
  std::vector<ClipCopy> clipboard_;  // sorted by start; the first one starts the copied range
  void commitMove(double delta, int laneDelta, bool copy);
  void splitAt(const ClipEntry& c, double at);
  void createMidiClip(const Lane& l, double at);
  void duplicateTrack(const Lane& l);
  void deleteTrack(const Lane& l);
  void renameTrack(const Lane& l);
  void renameClip(const ClipEntry& c);
  void addMarkerAt(double ppq);
  void locate(double ppq);
  void toggleTrackFlag(const Lane& l, int button);
  bool importMidiFile(const std::string& path, int laneIdx, double at);

  // ---- menus ----
  void clipMenu(int lane, int clip, Point at);
  void trackMenu(int lane, Point at);
  void laneMenu(int lane, double ppq, Point at);
  void markerMenu(int marker, Point at);
  void addTrackMenu(Point at);
  void gridMenu(Point at);
  void autoTargetMenu(int lane, int autoLane, Point at);
  std::vector<MenuItem> colorItems(std::function<void(uint32_t)> apply);

  // ---- toolbar ----
  enum ToolId {
    kTPointer = 1, kTSplit, kTDraw, kTSnap, kTGrid, kTZoomOut, kTZoomIn, kTFit, kTFollow, kTAddTrack, kTAuto
  };
  std::vector<ToolButton> toolButtons_;
  int hotTool_ = 0;
  void toolbarClick(int id, Point at);

  // ---- interaction state (gui_arrange_mouse.cpp) ----
  Hit hover_;
  float mouseX_ = -1, mouseY_ = -1;
  bool mouseInside_ = false;
  Drag drag_ = Drag::None;
  float downX_ = 0, downY_ = 0;
  unsigned downMods_ = 0;
  bool moved_ = false;
  Hit downHit_;
  double dragPpq0_ = 0;         // ppq under the mouse at press
  double dragDelta_ = 0;        // previewed time delta / new value
  int dragLaneDelta_ = 0;       // previewed track offset (clip move)
  bool dragCopy_ = false;
  double dragA_ = 0, dragB_ = 0;  // generic previewed values (trim start/len, fades, loop, slider)
  float dragStartVal_ = 0;
  std::vector<std::pair<double, double>> dragPoints_;  // automation lane being edited
  sq_guid dragNode_{};
  Rect rubber_;
  bool selectOnUp_ = false;   // plain click on an already selected clip: select only it on release
  bool toggleOnUp_ = false;   // shift/ctrl click on a selected clip: deselect on release
  bool autoNew_ = false;      // the automation drag started by adding a point
  bool justAddedPoint_ = false;
  // file drag feedback
  float dropX_ = -1, dropY_ = -1;

  Hit hitTest(float x, float y) const;
  void beginDrag(const WidgetMouse& e, const Hit& h);
  void updateDrag(const WidgetMouse& e);
  void endDrag(const WidgetMouse& e);
  void doubleClick(const WidgetMouse& e);
  void rightClick(const WidgetMouse& e);
  void wheel(const WidgetMouse& e);
  void autoScrollDuringDrag(float x, float y);

  // ---- painting (gui_arrange_paint.cpp) ----
  void paintToolbar(Canvas& c);
  void paintCorner(Canvas& c);
  void paintRuler(Canvas& c);
  void paintMarkers(Canvas& c);
  void paintGrid(Canvas& c, const Rect& r);
  void paintLane(Canvas& c, int idx);
  void paintRecordRegion(Canvas& c, const Lane& l);
  // Finest power-of-two subdivision of a beat whose spacing is >= minPx (== beat: none).
  double subdivision(double beat, double minPx) const;
  void paintHeader(Canvas& c, int idx);
  void paintClip(Canvas& c, const Lane& l, const ClipEntry& ce, const Rect& r, bool sel, bool ghost);
  void paintWaveform(Canvas& c, const ClipEntry& ce, const Rect& body, Color col);
  void paintNotes(Canvas& c, const ClipEntry& ce, const Rect& body, Color col);
  void paintAutoLane(Canvas& c, const Lane& l, int ai);
  void paintAutoHeader(Canvas& c, const Lane& l, int ai);
  void paintDragPreview(Canvas& c);
  void paintScrollbars(Canvas& c);
  void paintOverlayLines(Canvas& c);
  bool peaksPending_ = false;
};

template <class Fn>
void ArrangeView::forEachBar(double from, double to, Fn&& fn) const {
  // Signature segments; every segment starts on a bar line.
  int64_t barIdx = 0;
  for (size_t s = 0; s < sigs_.size(); ++s) {
    const double segStart = sigs_[s].pos;
    const double segEnd = s + 1 < sigs_.size() ? sigs_[s + 1].pos : 1e18;
    const double beat = 4.0 / static_cast<double>(sigs_[s].den);
    const double bar = beat * static_cast<double>(sigs_[s].num);
    if (bar <= 0) continue;
    const auto barsInSeg = segEnd >= 1e17 ? int64_t{1} << 40 : static_cast<int64_t>(std::ceil((segEnd - segStart) / bar - 1e-9));
    if (segEnd <= from) {
      barIdx += barsInSeg;
      continue;
    }
    int64_t k = from > segStart ? static_cast<int64_t>(std::floor((from - segStart) / bar)) : 0;
    for (;; ++k) {
      const double p = segStart + static_cast<double>(k) * bar;
      if (p >= segEnd - 1e-9 || p >= to) break;
      fn(barIdx + k, p, bar, beat, sigs_[s].num);
    }
    if (segEnd >= to) return;
    barIdx += barsInSeg;
  }
}

}  // namespace sq::app::gui::arrange
