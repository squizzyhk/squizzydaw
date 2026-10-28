// Squizzy Studio GUI - modal overlays drawn above the widget tree: popup menus (with submenus),
// text prompts, confirmations and the portable file chooser. The main GUI routes input to the top
// overlay first; anything outside a menu closes it.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gui_context.hpp"

namespace sq::app::gui {

// Single-line text editing state (UTF-8, byte cursor at code point boundaries).
struct TextEdit {
  std::string text;
  size_t cursor = 0;
  bool selectAll = false;  // first typed character replaces everything
  void set(std::string s) {
    text = std::move(s);
    cursor = text.size();
    selectAll = true;
  }
  // Returns true if the key was consumed.
  bool key(Key k, unsigned mods);
  bool chr(uint32_t ch, unsigned mods);
  void paint(Canvas& c, const Palette& p, const Rect& r, bool focused, float size = 13);
};

class Overlay {
 public:
  virtual ~Overlay() = default;
  // viewport = the whole window in DIPs.
  virtual void paint(Canvas& c, const Palette& p, const Rect& viewport) = 0;
  virtual bool onMouse(const WidgetMouse& e) = 0;   // true = consumed (always for modal ones)
  virtual bool onKey(Key k, unsigned mods) = 0;
  virtual bool onChar(uint32_t ch, unsigned mods) { (void)ch, (void)mods; return true; }
  bool closed = false;
  // Runs after the overlay is removed (menus run their action here, so it may open a new overlay).
  std::function<void()> after;
};

class OverlayStack {
 public:
  void push(std::unique_ptr<Overlay> o) { stack_.push_back(std::move(o)); }
  bool empty() const { return stack_.empty(); }
  void clear() { stack_.clear(); }
  void paint(Canvas& c, const Palette& p, const Rect& viewport);
  bool mouse(const WidgetMouse& e);
  bool key(Key k, unsigned mods);
  bool chr(uint32_t ch, unsigned mods);

 private:
  void reap();
  std::vector<std::unique_ptr<Overlay>> stack_;
};

std::unique_ptr<Overlay> makeMenuOverlay(std::vector<MenuItem> items, Point at);
// A menu opened from the menu bar: knows the bar's title rects so clicking / hovering another title
// or pressing Left / Right at the top level switches menus (openTitle(index) runs after it closed).
struct MenuBarLink {
  std::vector<Rect> titles;
  int current = -1;
  // how: kMenuByClick / kMenuByHover / kMenuByKey (keyboard: the first item starts highlighted).
  std::function<void(int index, int how)> openTitle;
  int how = 0;
};
std::unique_ptr<Overlay> makeMenuBarOverlay(std::vector<MenuItem> items, Point at, MenuBarLink link);
// Message with several buttons (left to right); Enter picks the first, Escape the last.
std::unique_ptr<Overlay> makeChoiceOverlay(const std::string& message, std::vector<std::string> buttons, std::function<void(int)> onPick);
// Folder chooser: returns the current folder, the selected subfolder or the typed (possibly new) folder.
std::unique_ptr<Overlay> makeFolderOverlay(const std::string& title, const std::string& initial, std::function<void(const std::string&)> onOk);
std::unique_ptr<Overlay> makePromptOverlay(const std::string& title, const std::string& initial, std::function<void(const std::string&)> onOk);
std::unique_ptr<Overlay> makeConfirmOverlay(const std::string& message, std::function<void()> onYes);
std::unique_ptr<Overlay> makeFileOverlay(bool save, const std::string& title, const std::vector<std::string>& exts, const std::string& initial,
                                         std::function<void(const std::string&)> onOk);

}  // namespace sq::app::gui
