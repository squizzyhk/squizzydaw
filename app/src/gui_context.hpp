// Squizzy Studio GUI - shared context for the panels (menu bar, transport, browser, arrange, clip
// editor, mixer, device rack). Every panel is a custom sq::ui::Widget created by a factory in
// gui_panels.hpp and placed by the main skin (app/resources/default.skin.xml); each one paints and
// handles input itself and talks to the application ONLY through this context: commands
// (Shell::execute, the same command system the CLI and scripts use) and document actions
// (IEditor::execute), so every edit is undoable and shows up everywhere.
//
// Coordinates are DIPs (tree coordinates). Everything runs on the UI (main) thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sq/app/shell.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/audioengine.h"
#include "sq/interfaces/document.h"
#include "sq/sdk.hpp"
#include "sq/ui/widgets.hpp"

namespace sq::app::gui {

using sq::ui::Canvas;
using sq::ui::Color;
using sq::ui::Key;
using sq::ui::MouseAction;
using sq::ui::MouseButton;
using sq::ui::Point;
using sq::ui::Rect;
using sq::ui::TextAlign;
using sq::ui::Widget;
using sq::ui::WidgetMouse;
using sq::ui::kModAlt;
using sq::ui::kModCtrl;
using sq::ui::kModShift;

// Colors of the active theme (theme.color.* with built-in fallbacks). Panels never hard-code
// colors; they take them from here so the light theme works too.
struct Palette {
  Color bg = Color::rgb(0x1C1D21);         // window background
  Color panel = Color::rgb(0x26282E);      // panels / bars
  Color panel2 = Color::rgb(0x2F323A);     // raised elements (track headers, strips)
  Color panel3 = Color::rgb(0x383C45);     // hovered / pressed controls
  Color border = Color::rgb(0x15161A);     // separators
  Color text = Color::rgb(0xE4E6EB);
  Color dim = Color::rgb(0x9AA0A8);
  Color faint = Color::rgb(0x5C626C);      // disabled text, minor grid
  Color accent = Color::rgb(0x4FB3A9);     // selection, active toggles
  Color accent2 = Color::rgb(0x8E7CC3);    // instruments / MIDI
  Color rec = Color::rgb(0xE0604A);
  Color mute = Color::rgb(0xD9A441);
  Color solo = Color::rgb(0x5FA8E8);
  Color playhead = Color::rgb(0xFFFFFF);
  Color cursor = Color::rgb(0xD9A441);     // edit cursor
  Color grid = Color::rgb(0x2A2D33);       // beat lines
  Color gridBar = Color::rgb(0x3A3E46);    // bar lines
  Color trackBg = Color::rgb(0x202227);    // arrange lane background
  Color trackSel = Color::rgb(0x2A3A3A);   // selected lane
  Color meterLow = Color::rgb(0x4FB36A), meterMid = Color::rgb(0xD9C441), meterHigh = Color::rgb(0xE0604A);
  bool dark = true;
  // Default clip/track colors (cycled by track index when a track has no color).
  std::vector<Color> trackColors;
};

struct MenuItem {
  std::string label;
  std::string shortcut;            // display only ("Ctrl+S")
  std::function<void()> action;    // runs after the menu closed
  bool enabled = true;
  bool checked = false;
  bool separator = false;
  std::vector<MenuItem> submenu;
  static MenuItem sep() {
    MenuItem m;
    m.separator = true;
    return m;
  }
};

enum class BottomPanel { Mixer, Editor, Devices };

// Snapshots of document nodes (valid until the next document change).
struct TrackView {
  sq::Ptr<ITrack> track;
  sq_guid id{};
  std::string name;
  sq_track_info info{};
  uint32_t index = 0;  // display order
};
struct ClipView {
  sq::Ptr<IClip> clip;
  sq_guid id{};
  std::string name;
  sq_clip_info info{};
  sq_guid track{};
  std::string source;  // audio: media path
};
struct EditStateView {
  std::vector<sq_guid> selection;  // selected clips
  double cursor = 0;               // edit cursor (ppq)
  double grid = 1;                 // grid (ppq)
  std::string snap = "grid";       // off | grid | relative | bar | events
  sq_guid track{};                 // focused track
  bool hasRange = false;
  double rangeFrom = 0, rangeTo = 0;
};

// Optional interface of panels that want character input while focused (text fields etc.).
struct CharInput {
  virtual ~CharInput() = default;
  virtual bool onChar(uint32_t ch, unsigned mods) = 0;
};

class GuiContext {
 public:
  virtual ~GuiContext() = default;

  virtual Shell& shell() = 0;
  virtual const Palette& palette() const = 0;
  // Bumps on every committed document change, undo/redo and document switch: panels compare it to
  // rebuild caches.
  virtual uint64_t docSerial() const = 0;
  // Runs a command ("Category.Name"); the shell message (or the error) goes to the status line.
  virtual sq_result run(std::string_view dotted, IAttributes* args = nullptr) = 0;
  virtual void setStatus(const std::string& text) = 0;
  virtual void invalidate() = 0;

  // ---- overlays (modal, drawn above everything) ----
  virtual void showMenu(std::vector<MenuItem> items, Point at) = 0;
  // One-line text prompt; onOk gets the text (not called on cancel).
  virtual void prompt(const std::string& title, const std::string& initial, std::function<void(const std::string&)> onOk) = 0;
  virtual void confirm(const std::string& message, std::function<void()> onYes) = 0;
  // File chooser. exts = accepted extensions without dot ({"wav","flac"}), empty = all files.
  virtual void chooseFile(bool save, const std::string& title, const std::vector<std::string>& exts, const std::string& initial,
                          std::function<void(const std::string&)> onOk) = 0;

  // ---- panels ----
  virtual void showBottomPanel(BottomPanel p) = 0;
  virtual BottomPanel bottomPanel() const = 0;
  virtual bool bottomPanelVisible() const = 0;
  virtual void setBottomPanelVisible(bool visible) = 0;
  virtual bool browserVisible() const = 0;
  virtual void setBrowserVisible(bool visible) = 0;
  // Opens a clip in the editor panel (MIDI: piano roll) and shows that panel.
  virtual void openClipEditor(const sq_guid& clip) = 0;
  virtual sq_guid editorClip() const = 0;
  // A file dragged out of the browser: the arrange view asks for it on mouse up.
  virtual std::string draggedFile() const = 0;
  virtual void setDraggedFile(const std::string& path) = 0;

  // ---- document helpers (gui_context.cpp) ----
  sq::Ptr<IDocument> doc();
  std::vector<TrackView> tracks();
  bool findTrack(const sq_guid& id, TrackView* out);
  std::vector<ClipView> clips(const TrackView& t);
  bool findClip(const sq_guid& id, ClipView* out);
  std::vector<sq_clip_event> clipEvents(IClip* clip);
  // Track property ("user.inserts", "user.instrument", "user.automation", "user.sends", ...).
  std::string trackString(const sq_guid& track, const char* key);
  Color trackColor(const TrackView& t) const;

  EditStateView editState();
  sq_guid selectedTrack();
  // Selection / cursor (Navigate.SetEditState; journal only, no undo step).
  void selectTrack(const sq_guid& track);
  void setSelection(const std::vector<sq_guid>& clips);
  void setCursor(double ppq);
  void setGrid(double ppq);

  // One document action (class GUIDs in document.h: sq::doc::kSetProperty, kMoveClip, ...).
  sq_result exec(const sq_guid& actionClass, IAttributes* params, IAttributes* result = nullptr);
  // SetProperty helpers. merge=true coalesces into the previous step (continuous drags).
  sq_result setProperty(const sq_guid& node, const char* key, double value, bool merge = false);
  sq_result setPropertyBool(const sq_guid& node, const char* key, bool value);
  sq_result setPropertyString(const sq_guid& node, const char* key, const std::string& value, bool merge = false);
  // Groups several actions into one undo step.
  bool beginEdit(const char* description);
  void endEdit(bool commit = true);

  // ---- engine helpers ----
  ITransport* transportIf();
  sq_transport_state transport();
  // Peak (linear) of a track since the last read; null id = master. Returns channel count.
  uint32_t meter(const sq_guid& track, float* peaks, float* rms, uint32_t capacity);
  double ppqToSeconds(double ppq);
  double secondsToPpq(double seconds);
  // "bar.beat.tick" (1-based) using the first time signature.
  std::string formatBars(double ppq);
  static std::string formatTime(double seconds);  // m:ss.mmm
};

// RAII undo group.
class EditScope {
 public:
  EditScope(GuiContext& ctx, const char* description) : ctx_(ctx), open_(ctx.beginEdit(description)) {}
  ~EditScope() {
    if (open_) ctx_.endEdit(!cancel_);
  }
  void cancel() { cancel_ = true; }
  EditScope(const EditScope&) = delete;
  EditScope& operator=(const EditScope&) = delete;

 private:
  GuiContext& ctx_;
  bool open_;
  bool cancel_ = false;
};

// ---- shared drawing helpers (gui_draw.cpp) -----------------------------------------------------
namespace draw {
// A flat button: rounded box + centered label. hot = hovered, on = latched/pressed.
void button(Canvas& c, const Palette& p, const Rect& r, std::string_view label, bool hot, bool on, Color onColor);
// Rotary knob value 0..1 (bipolar draws from the center).
void knob(Canvas& c, const Palette& p, const Rect& r, double normalized, bool bipolar, Color accent);
// Vertical meter (linear peak, dB scale).
void meter(Canvas& c, const Palette& p, const Rect& r, float peakLinear, float holdLinear);
// Small text helpers.
void textLeft(Canvas& c, const Rect& r, std::string_view s, Color col, float size = 12);
void textCenter(Canvas& c, const Rect& r, std::string_view s, Color col, float size = 12);
// Filled triangle inside r pointing up (0), right (1), down (2) or left (3).
void triangle(Canvas& c, const Rect& r, int direction, Color col);
// Check mark inside r.
void check(Canvas& c, const Rect& r, Color col);
Color mix(Color a, Color b, float t);
Color lighten(Color a, float amount);
}  // namespace draw

}  // namespace sq::app::gui
