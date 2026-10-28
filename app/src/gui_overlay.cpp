// Squizzy Studio GUI - overlays (menus, prompts, confirmations, file chooser). See gui_overlay.hpp.
#include "gui_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sq/platform.h"
#include "sq/platform.hpp"

namespace sq::app::gui {
namespace {

size_t prevCp(const std::string& s, size_t i) {
  if (i == 0) return 0;
  --i;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
  return i;
}
size_t nextCp(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  ++i;
  while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
  return i;
}
void appendUtf8(std::string& s, size_t at, uint32_t cp) {
  std::string u;
  if (cp < 0x80) u.push_back(static_cast<char>(cp));
  else if (cp < 0x800) {
    u.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    u.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    u.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    u.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    u.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    u.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  s.insert(at, u);
}

Rect centered(const Rect& vp, float w, float h) {
  w = std::min(w, vp.w - 20);
  h = std::min(h, vp.h - 20);
  return Rect{vp.x + (vp.w - w) / 2, vp.y + (vp.h - h) / 2.6f, w, h};
}

void dialogFrame(Canvas& c, const Palette& p, const Rect& vp, const Rect& box, const std::string& title) {
  c.fillRect(vp, Color{0, 0, 0, 0.45f});
  c.fillRoundRect(Rect{box.x + 2, box.y + 4, box.w, box.h}, 8, Color{0, 0, 0, 0.35f});
  c.fillRoundRect(box, 8, p.panel);
  c.strokeRoundRect(box, 8, p.panel3, 1);
  c.drawTextBold(title, Rect{box.x + 16, box.y + 10, box.w - 32, 22}, p.text, 14);
}

}  // namespace

// ---- TextEdit ----------------------------------------------------------------------------------
bool TextEdit::key(Key k, unsigned mods) {
  (void)mods;
  switch (k) {
    case Key::Left:
      selectAll = false;
      cursor = prevCp(text, cursor);
      return true;
    case Key::Right:
      selectAll = false;
      cursor = nextCp(text, cursor);
      return true;
    case Key::Home:
      selectAll = false;
      cursor = 0;
      return true;
    case Key::End:
      selectAll = false;
      cursor = text.size();
      return true;
    case Key::Delete:
      if (selectAll) {
        text.clear();
        cursor = 0;
        selectAll = false;
      } else if (cursor < text.size()) {
        text.erase(cursor, nextCp(text, cursor) - cursor);
      }
      return true;
    case Key::Backspace:  // handled in chr (WM_CHAR 8) to avoid double deletes
      return true;
    default: return false;
  }
}

bool TextEdit::chr(uint32_t ch, unsigned mods) {
  if (ch == 1 && (mods & sq::ui::kModCtrl)) {  // Ctrl+A
    selectAll = true;
    cursor = text.size();
    return true;
  }
  if (ch == 8) {  // backspace
    if (selectAll) {
      text.clear();
      cursor = 0;
      selectAll = false;
    } else if (cursor > 0) {
      size_t p = prevCp(text, cursor);
      text.erase(p, cursor - p);
      cursor = p;
    }
    return true;
  }
  if (ch < 32 || ch == 127) return false;
  if (selectAll) {
    text.clear();
    cursor = 0;
    selectAll = false;
  }
  appendUtf8(text, cursor, ch);
  cursor = nextCp(text, cursor);
  return true;
}

void TextEdit::paint(Canvas& c, const Palette& p, const Rect& r, bool focused, float size) {
  c.fillRoundRect(r, 4, p.bg);
  c.strokeRoundRect(r, 4, focused ? p.accent : p.panel3, 1);
  const Rect inner{r.x + 8, r.y, r.w - 16, r.h};
  c.pushClip(inner);
  const float before = c.measureText(std::string_view(text).substr(0, cursor), size);
  const float shift = std::max(0.0f, before - (inner.w - 4));
  if (selectAll && !text.empty()) {
    const float w = c.measureText(text, size);
    c.fillRect(Rect{inner.x - shift, r.y + 4, std::min(w, inner.w + shift), r.h - 8}, p.accent.withAlpha(0.35f));
  }
  c.drawText(text, Rect{inner.x - shift, r.y, inner.w + shift + 2000, r.h}, p.text, size);
  if (focused) c.fillRect(Rect{inner.x - shift + before, r.y + 5, 1.5f, r.h - 10}, p.text);
  c.popClip();
}

// ---- OverlayStack -----------------------------------------------------------------------------
void OverlayStack::reap() {
  while (!stack_.empty() && stack_.back()->closed) {
    std::unique_ptr<Overlay> o = std::move(stack_.back());
    stack_.pop_back();
    if (o->after) o->after();
  }
  // closed overlays below the top (rare) go too
  for (size_t i = 0; i < stack_.size();) {
    if (stack_[i]->closed) {
      std::unique_ptr<Overlay> o = std::move(stack_[i]);
      stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(i));
      if (o->after) o->after();
    } else {
      ++i;
    }
  }
}
void OverlayStack::paint(Canvas& c, const Palette& p, const Rect& viewport) {
  for (auto& o : stack_) o->paint(c, p, viewport);
}
bool OverlayStack::mouse(const WidgetMouse& e) {
  if (stack_.empty()) return false;
  stack_.back()->onMouse(e);
  reap();
  return true;
}
bool OverlayStack::key(Key k, unsigned mods) {
  if (stack_.empty()) return false;
  stack_.back()->onKey(k, mods);
  reap();
  return true;
}
bool OverlayStack::chr(uint32_t ch, unsigned mods) {
  if (stack_.empty()) return false;
  stack_.back()->onChar(ch, mods);
  reap();
  return true;
}

// ---- Menu --------------------------------------------------------------------------------------
namespace {

class MenuOverlay final : public Overlay {
 public:
  MenuOverlay(std::vector<MenuItem> items, Point at, MenuBarLink link = {}) : bar_(std::move(link)) {
    Level l;
    l.items = std::move(items);
    l.origin = at;
    if (bar_.how == kMenuByKey) l.hot = firstEnabled(l.items, 0, 1);
    levels_.push_back(std::move(l));
  }

  void paint(Canvas& c, const Palette& p, const Rect& vp) override {
    vp_ = vp;
    for (size_t li = 0; li < levels_.size(); ++li) {
      Level& l = levels_[li];
      float w = 160;
      for (const auto& it : l.items) {
        if (it.separator) continue;
        float tw = c.measureText(it.label, 12.5f) + 60 + (it.shortcut.empty() ? 0 : c.measureText(it.shortcut, 11.5f) + 24);
        w = std::max(w, tw);
      }
      float h = 6;
      for (const auto& it : l.items) h += it.separator ? 9 : kRowH;
      h += 6;
      float x = l.origin.x, y = l.origin.y;
      if (x + w > vp.right() - 4) x = std::max(vp.x + 4, (li > 0 ? levels_[li - 1].box.x - w + 4 : vp.right() - w - 4));
      if (y + h > vp.bottom() - 4) y = std::max(vp.y + 4, vp.bottom() - h - 4);
      l.box = Rect{x, y, w, h};
      c.fillRoundRect(Rect{x + 2, y + 3, w, h}, 6, Color{0, 0, 0, 0.35f});
      c.fillRoundRect(l.box, 6, p.panel2);
      c.strokeRoundRect(l.box, 6, p.panel3, 1);
      l.rows.clear();
      float yy = y + 6;
      for (size_t i = 0; i < l.items.size(); ++i) {
        const MenuItem& it = l.items[i];
        if (it.separator) {
          c.fillRect(Rect{x + 8, yy + 4, w - 16, 1}, p.panel3);
          l.rows.push_back(Rect{x, yy, w, 9});
          yy += 9;
          continue;
        }
        Rect row{x + 4, yy, w - 8, kRowH};
        l.rows.push_back(row);
        const bool hot = static_cast<int>(i) == l.hot && it.enabled;
        if (hot) c.fillRoundRect(row, 4, p.accent.withAlpha(0.85f));
        const Color fg = !it.enabled ? p.faint : hot ? (p.dark ? Color::rgb(0x101114) : Color::rgb(0xFFFFFF)) : p.text;
        if (it.checked) draw::check(c, Rect{row.x + 7, row.y + 6, 11, row.h - 12}, fg);
        c.drawText(it.label, Rect{row.x + 24, row.y, row.w - 40, row.h}, fg, 12.5f);
        if (!it.shortcut.empty()) c.drawText(it.shortcut, Rect{row.x, row.y, row.w - 10, row.h}, hot ? fg : p.dim, 11.5f, TextAlign::Right);
        if (!it.submenu.empty()) draw::triangle(c, Rect{row.right() - 16, row.y + 8, 6, row.h - 16}, 1, fg);
        yy += kRowH;
      }
    }
  }

  bool onMouse(const WidgetMouse& e) override {
    int li = -1, ii = -1;
    hit(e.x, e.y, li, ii);
    const int title = li < 0 ? barTitleAt(e.x, e.y) : -1;
    if (e.action == MouseAction::Move || e.action == MouseAction::Hover) {
      if (title >= 0 && title != bar_.current) {  // hover another menu-bar title: switch menus
        switchTo(title, kMenuByHover);
        return true;
      }
      if (li >= 0) {
        Level& l = levels_[static_cast<size_t>(li)];
        l.hot = ii;
        levels_.resize(static_cast<size_t>(li) + 1);
        if (ii >= 0 && !l.items[static_cast<size_t>(ii)].submenu.empty() && l.items[static_cast<size_t>(ii)].enabled) openSub(static_cast<size_t>(li), static_cast<size_t>(ii));
      }
      return true;
    }
    if (e.action == MouseAction::Down) {
      if (li < 0) {
        if (title >= 0 && title != bar_.current) switchTo(title, kMenuByClick);  // another menu-bar title
        else if (title >= 0 && bar_.how == kMenuByHover) bar_.how = kMenuByClick;  // opened by hovering: the click keeps it
        else closed = true;  // click outside (or a second click on this menu's title) closes
        return true;
      }
      return true;
    }
    if (e.action == MouseAction::Up) {
      if (li >= 0 && ii >= 0) activate(static_cast<size_t>(li), static_cast<size_t>(ii));
      return true;
    }
    return true;
  }

  bool onKey(Key k, unsigned) override {
    Level& l = levels_.back();
    auto step = [&](int dir) {
      const int n = static_cast<int>(l.items.size());
      if (n == 0) return;
      // Nothing highlighted yet: Down starts at the first item, Up at the last one.
      const int from = l.hot < 0 ? (dir > 0 ? n - 1 : 0) : l.hot;
      const int next = firstEnabled(l.items, (from + dir + n) % n, dir);
      if (next >= 0) l.hot = next;
    };
    switch (k) {
      case Key::Escape:
        if (levels_.size() > 1) levels_.pop_back();
        else closed = true;
        return true;
      case Key::Down: step(1); return true;
      case Key::Up: step(-1); return true;
      case Key::Right:
        if (l.hot >= 0 && !l.items[static_cast<size_t>(l.hot)].submenu.empty() && l.items[static_cast<size_t>(l.hot)].enabled) {
          openSub(levels_.size() - 1, static_cast<size_t>(l.hot));
          levels_.back().hot = firstEnabled(levels_.back().items, 0, 1);
        } else if (hasBar()) {
          switchTo((bar_.current + 1) % static_cast<int>(bar_.titles.size()), kMenuByKey);
        }
        return true;
      case Key::Left:
        if (levels_.size() > 1) levels_.pop_back();
        else if (hasBar()) switchTo((bar_.current + static_cast<int>(bar_.titles.size()) - 1) % static_cast<int>(bar_.titles.size()), kMenuByKey);
        return true;
      case Key::Enter:
      case Key::Space:
        if (l.hot >= 0) activate(levels_.size() - 1, static_cast<size_t>(l.hot));
        return true;
      default: return true;
    }
  }

 private:
  static constexpr float kRowH = 24;
  struct Level {
    std::vector<MenuItem> items;
    Point origin;
    Rect box;
    std::vector<Rect> rows;
    int hot = -1;
  };
  static int firstEnabled(const std::vector<MenuItem>& items, int start, int dir) {
    const int n = static_cast<int>(items.size());
    for (int t = 0, i = start; t < n; ++t, i = (i + dir + n) % n) {
      const MenuItem& it = items[static_cast<size_t>(i)];
      if (!it.separator && it.enabled) return i;
    }
    return -1;
  }
  bool hasBar() const { return bar_.openTitle && !bar_.titles.empty() && bar_.current >= 0; }
  int barTitleAt(float x, float y) const {
    if (!hasBar()) return -1;
    for (size_t i = 0; i < bar_.titles.size(); ++i)
      if (bar_.titles[i].contains(x, y)) return static_cast<int>(i);
    return -1;
  }
  void switchTo(int index, int how) {
    auto open = bar_.openTitle;
    after = [open, index, how] { open(index, how); };
    closed = true;
  }
  void hit(float x, float y, int& li, int& ii) {
    li = ii = -1;
    for (size_t l = levels_.size(); l-- > 0;) {
      if (!levels_[l].box.contains(x, y)) continue;
      li = static_cast<int>(l);
      for (size_t i = 0; i < levels_[l].rows.size(); ++i)
        if (levels_[l].rows[i].contains(x, y) && !levels_[l].items[i].separator) ii = static_cast<int>(i);
      return;
    }
  }
  void openSub(size_t li, size_t ii) {
    if (levels_.size() > li + 1) return;
    const Level& l = levels_[li];
    Level s;
    s.items = l.items[ii].submenu;
    const Rect row = ii < l.rows.size() ? l.rows[ii] : Rect{l.box.x, l.box.y, l.box.w, kRowH};
    s.origin = Point{l.box.right() - 4, row.y - 6};
    levels_.push_back(std::move(s));
  }
  void activate(size_t li, size_t ii) {
    const MenuItem& it = levels_[li].items[ii];
    if (!it.enabled || it.separator) return;
    if (!it.submenu.empty()) {
      openSub(li, ii);
      return;
    }
    after = it.action;
    closed = true;
  }
  std::vector<Level> levels_;
  Rect vp_;
  MenuBarLink bar_;
};

// ---- Prompt / Confirm ---------------------------------------------------------------------------
class PromptOverlay final : public Overlay {
 public:
  PromptOverlay(std::string title, const std::string& initial, std::function<void(const std::string&)> ok) : title_(std::move(title)), ok_(std::move(ok)) {
    edit_.set(initial);
  }
  void paint(Canvas& c, const Palette& p, const Rect& vp) override {
    box_ = centered(vp, 440, 136);
    dialogFrame(c, p, vp, box_, title_);
    field_ = Rect{box_.x + 16, box_.y + 44, box_.w - 32, 30};
    edit_.paint(c, p, field_, true);
    okR_ = Rect{box_.right() - 16 - 90, box_.bottom() - 44, 90, 28};
    cancelR_ = Rect{okR_.x - 100, okR_.y, 90, 28};
    draw::button(c, p, cancelR_, "Cancel", hot_ == 2, false, p.accent);
    draw::button(c, p, okR_, "OK", hot_ == 1, true, p.accent);
  }
  bool onMouse(const WidgetMouse& e) override {
    hot_ = okR_.contains(e.x, e.y) ? 1 : cancelR_.contains(e.x, e.y) ? 2 : 0;
    if (e.action == MouseAction::Up) {
      if (hot_ == 1) accept();
      else if (hot_ == 2) closed = true;
    }
    return true;
  }
  bool onKey(Key k, unsigned mods) override {
    if (k == Key::Enter) accept();
    else if (k == Key::Escape) closed = true;
    else edit_.key(k, mods);
    return true;
  }
  bool onChar(uint32_t ch, unsigned mods) override {
    if (ch == 13 || ch == 27 || ch == 9) return true;
    edit_.chr(ch, mods);
    return true;
  }

 private:
  void accept() {
    std::string v = edit_.text;
    auto cb = ok_;
    after = [cb, v] {
      if (cb) cb(v);
    };
    closed = true;
  }
  std::string title_;
  std::function<void(const std::string&)> ok_;
  TextEdit edit_;
  Rect box_, field_, okR_, cancelR_;
  int hot_ = 0;
};

class ConfirmOverlay final : public Overlay {
 public:
  ConfirmOverlay(std::string msg, std::function<void()> yes) : msg_(std::move(msg)), yes_(std::move(yes)) {}
  void paint(Canvas& c, const Palette& p, const Rect& vp) override {
    box_ = centered(vp, 440, 150);
    dialogFrame(c, p, vp, box_, "Squizzy Studio");
    c.drawText(msg_, Rect{box_.x + 16, box_.y + 42, box_.w - 32, 54}, p.text, 13);
    yesR_ = Rect{box_.right() - 16 - 90, box_.bottom() - 44, 90, 28};
    noR_ = Rect{yesR_.x - 100, yesR_.y, 90, 28};
    draw::button(c, p, noR_, "Cancel", hot_ == 2, false, p.accent);
    draw::button(c, p, yesR_, "OK", hot_ == 1, true, p.accent);
  }
  bool onMouse(const WidgetMouse& e) override {
    hot_ = yesR_.contains(e.x, e.y) ? 1 : noR_.contains(e.x, e.y) ? 2 : 0;
    if (e.action == MouseAction::Up) {
      if (hot_ == 1) {
        after = yes_;
        closed = true;
      } else if (hot_ == 2) {
        closed = true;
      }
    }
    return true;
  }
  bool onKey(Key k, unsigned) override {
    if (k == Key::Enter) {
      after = yes_;
      closed = true;
    } else if (k == Key::Escape) {
      closed = true;
    }
    return true;
  }

 private:
  std::string msg_;
  std::function<void()> yes_;
  Rect box_, yesR_, noR_;
  int hot_ = 0;
};

class ChoiceOverlay final : public Overlay {
 public:
  ChoiceOverlay(std::string msg, std::vector<std::string> buttons, std::function<void(int)> pick)
      : msg_(std::move(msg)), buttons_(std::move(buttons)), pick_(std::move(pick)) {
    if (buttons_.empty()) buttons_.push_back("OK");
  }
  void paint(Canvas& c, const Palette& p, const Rect& vp) override {
    float bw = 0;
    for (const auto& b : buttons_) bw = std::max(bw, c.measureText(b, 12) + 28);
    bw = std::max(bw, 90.0f);
    const float need = static_cast<float>(buttons_.size()) * (bw + 10) + 22;
    box_ = centered(vp, std::max(460.0f, std::max(need, c.measureText(msg_, 13) + 40)), 150);
    dialogFrame(c, p, vp, box_, "Squizzy Studio");
    c.drawText(msg_, Rect{box_.x + 16, box_.y + 42, box_.w - 32, 54}, p.text, 13);
    rects_.assign(buttons_.size(), Rect{});
    float x = box_.right() - 16 - bw;
    for (size_t i = buttons_.size(); i-- > 0;) {  // the last button (Cancel) at the right
      rects_[i] = Rect{x, box_.bottom() - 44, bw, 28};
      x -= bw + 10;
    }
    for (size_t i = 0; i < buttons_.size(); ++i) draw::button(c, p, rects_[i], buttons_[i], hot_ == static_cast<int>(i), static_cast<int>(i) == focus_, p.accent);
  }
  bool onMouse(const WidgetMouse& e) override {
    hot_ = -1;
    for (size_t i = 0; i < rects_.size(); ++i)
      if (rects_[i].contains(e.x, e.y)) hot_ = static_cast<int>(i);
    if (e.action == MouseAction::Up && hot_ >= 0) choose(hot_);
    return true;
  }
  bool onKey(Key k, unsigned mods) override {
    const int n = static_cast<int>(buttons_.size());
    if (k == Key::Enter || k == Key::Space) choose(focus_);
    else if (k == Key::Right || (k == Key::Tab && !(mods & sq::ui::kModShift))) focus_ = (focus_ + 1) % n;
    else if (k == Key::Left || k == Key::Tab) focus_ = (focus_ + n - 1) % n;
    else if (k == Key::Escape) choose(static_cast<int>(buttons_.size()) - 1);
    return true;
  }

 private:
  void choose(int i) {
    auto cb = pick_;
    after = [cb, i] {
      if (cb) cb(i);
    };
    closed = true;
  }
  std::string msg_;
  std::vector<std::string> buttons_;
  std::function<void(int)> pick_;
  Rect box_;
  std::vector<Rect> rects_;
  int hot_ = -1, focus_ = 0;  // focus_: keyboard choice (Enter), starts on the first button
};

// ---- File chooser -------------------------------------------------------------------------------
std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
  return s;
}
std::string normSlashes(std::string s) {
  for (char& ch : s)
    if (ch == '\\') ch = '/';
  return s;
}
std::string parentDir(const std::string& d) {
  std::string s = d;
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  size_t k = s.find_last_of('/');
  if (k == std::string::npos) return s;
  if (k == 0) return "/";
  if (k == 2 && s[1] == ':') return s.substr(0, 3);  // "C:/"
  return s.substr(0, k);
}

class FileOverlay final : public Overlay {
 public:
  FileOverlay(bool save, std::string title, std::vector<std::string> exts, const std::string& initial, std::function<void(const std::string&)> ok,
              bool folder = false)
      : save_(save || folder), folder_(folder), title_(std::move(title)), exts_(std::move(exts)), ok_(std::move(ok)) {
    std::string init = normSlashes(initial);
    std::string dir;
    if (!init.empty() && sq_fs_is_dir(init.c_str())) {
      dir = init;
    } else if (!init.empty() && folder_) {  // a folder that does not exist yet: start in its parent
      dir = parentDir(init);
      if (!sq_fs_is_dir(dir.c_str())) dir.clear();
    } else if (!init.empty()) {
      dir = parentDir(init);
      size_t k = init.find_last_of('/');
      name_.set(k == std::string::npos ? init : init.substr(k + 1));
      if (!sq_fs_is_dir(dir.c_str())) dir.clear();
    }
    if (dir.empty()) dir = normSlashes(sq::specialDir(SQ_DIR_DOCUMENTS));
    if (dir.empty() || !sq_fs_is_dir(dir.c_str())) dir = normSlashes(sq::specialDir(SQ_DIR_HOME));
    load(dir);
  }

  void paint(Canvas& c, const Palette& p, const Rect& vp) override {
    box_ = centered(vp, 760, 520);
    dialogFrame(c, p, vp, box_, title_);
    upR_ = Rect{box_.x + 16, box_.y + 42, 60, 26};
    draw::button(c, p, upR_, "Up", hot_ == kUp, false, p.accent);
    homeR_ = Rect{upR_.right() + 6, upR_.y, 64, 26};
    draw::button(c, p, homeR_, "Home", hot_ == kHome, false, p.accent);
    c.drawText(dir_, Rect{homeR_.right() + 10, upR_.y, box_.right() - homeR_.right() - 26, 26}, p.dim, 12);
    list_ = Rect{box_.x + 16, upR_.bottom() + 8, box_.w - 32, box_.h - 160};
    c.fillRoundRect(list_, 4, p.bg);
    c.pushClip(list_);
    const float rowH = 22;
    const int visible = static_cast<int>(list_.h / rowH);
    scroll_ = std::clamp(scroll_, 0, std::max(0, static_cast<int>(entries_.size()) - visible));
    rows_.clear();
    for (int i = 0; i < visible + 1 && scroll_ + i < static_cast<int>(entries_.size()); ++i) {
      const auto& en = entries_[static_cast<size_t>(scroll_ + i)];
      Rect row{list_.x, list_.y + static_cast<float>(i) * rowH, list_.w, rowH};
      rows_.push_back(row);
      const bool sel = scroll_ + i == selected_;
      if (sel) c.fillRect(row, p.accent.withAlpha(0.35f));
      else if (scroll_ + i == hotRow_) c.fillRect(row, p.panel3.withAlpha(0.5f));
      c.drawText(en.dir ? "\xF0\x9F\x93\x81" : "", Rect{row.x + 6, row.y, 20, rowH}, p.dim, 12);
      c.fillRoundRect(Rect{row.x + 8, row.y + 6, 12, 10}, 2, en.dir ? p.mute.withAlpha(0.8f) : p.faint);
      c.drawText(en.name + (en.dir ? "/" : ""), Rect{row.x + 28, row.y, row.w - 36, rowH}, en.dir ? p.text : p.text, 12.5f);
    }
    if (entries_.empty()) c.drawText(error_.empty() ? "(empty)" : error_, Rect{list_.x + 10, list_.y + 6, list_.w - 20, 20}, p.faint, 12);
    c.popClip();
    const float fy = list_.bottom() + 12;
    c.drawText(folder_ ? "New folder" : save_ ? "File name" : "File", Rect{box_.x + 16, fy, 80, 28}, p.dim, 12);
    field_ = Rect{box_.x + 96, fy, box_.w - 112, 28};
    name_.paint(c, p, field_, confirm_.empty());
    if (folder_) {
      c.drawText("Select Folder uses the folder shown above (or the selected / typed one).", Rect{box_.x + 16, box_.bottom() - 42, box_.w - 260, 26},
                 p.faint, 11.5f);
    } else if (!exts_.empty()) {
      std::string f = "Types: ";
      for (size_t i = 0; i < exts_.size(); ++i) f += (i ? ", ." : ".") + exts_[i];
      c.drawText(f, Rect{box_.x + 16, box_.bottom() - 42, box_.w / 2, 26}, p.faint, 11.5f);
    }
    okR_ = Rect{box_.right() - 16 - 110, box_.bottom() - 44, 110, 28};
    cancelR_ = Rect{okR_.x - 110, okR_.y, 100, 28};
    draw::button(c, p, cancelR_, "Cancel", hot_ == kCancel, false, p.accent);
    draw::button(c, p, okR_, folder_ ? "Select Folder" : save_ ? "Save" : "Open", hot_ == kOk, true, p.accent);
    if (!confirm_.empty()) {  // "already exists - replace?" on top of the chooser
      c.fillRoundRect(box_, 8, Color{0, 0, 0, 0.35f});
      const Rect q = centered(box_, 460, 130);
      c.fillRoundRect(Rect{q.x + 2, q.y + 4, q.w, q.h}, 8, Color{0, 0, 0, 0.35f});
      c.fillRoundRect(q, 8, p.panel2);
      c.strokeRoundRect(q, 8, p.panel3, 1);
      c.drawTextBold("Replace file?", Rect{q.x + 16, q.y + 10, q.w - 32, 22}, p.text, 14);
      const size_t k = confirm_.find_last_of('/');
      c.drawText((k == std::string::npos ? confirm_ : confirm_.substr(k + 1)) + " already exists. Replace it?", Rect{q.x + 16, q.y + 40, q.w - 32, 30},
                 p.text, 13);
      replaceR_ = Rect{q.right() - 16 - 100, q.bottom() - 42, 100, 28};
      keepR_ = Rect{replaceR_.x - 110, replaceR_.y, 100, 28};
      draw::button(c, p, keepR_, "Cancel", hot_ == kKeep, false, p.accent);
      draw::button(c, p, replaceR_, "Replace", hot_ == kReplace, true, p.accent);
    }
  }

  bool onMouse(const WidgetMouse& e) override {
    if (!confirm_.empty()) {
      hot_ = replaceR_.contains(e.x, e.y) ? kReplace : keepR_.contains(e.x, e.y) ? kKeep : 0;
      if (e.action == MouseAction::Up) {
        if (hot_ == kReplace) finish(confirm_);
        else if (hot_ == kKeep) confirm_.clear();
      }
      return true;
    }
    hot_ = upR_.contains(e.x, e.y) ? kUp : homeR_.contains(e.x, e.y) ? kHome : okR_.contains(e.x, e.y) ? kOk : cancelR_.contains(e.x, e.y) ? kCancel : 0;
    hotRow_ = -1;
    for (size_t i = 0; i < rows_.size(); ++i)
      if (rows_[i].contains(e.x, e.y)) hotRow_ = scroll_ + static_cast<int>(i);
    switch (e.action) {
      case MouseAction::Wheel:
        scroll_ -= static_cast<int>(e.wheel * 3);
        return true;
      case MouseAction::Down:
        if (hotRow_ >= 0 && hotRow_ < static_cast<int>(entries_.size())) {
          selected_ = hotRow_;
          const auto& en = entries_[static_cast<size_t>(selected_)];
          if (!en.dir) name_.set(en.name);
        }
        return true;
      case MouseAction::DoubleClick:
        if (hotRow_ >= 0 && hotRow_ < static_cast<int>(entries_.size())) {
          const auto en = entries_[static_cast<size_t>(hotRow_)];
          if (en.dir) load(join(dir_, en.name));
          else {
            name_.set(en.name);
            accept();
          }
        }
        return true;
      case MouseAction::Up:
        if (hot_ == kUp) load(parentDir(dir_));
        else if (hot_ == kHome) load(normSlashes(sq::specialDir(SQ_DIR_HOME)));
        else if (hot_ == kOk) accept();
        else if (hot_ == kCancel) closed = true;
        return true;
      default: return true;
    }
  }
  bool onKey(Key k, unsigned mods) override {
    if (!confirm_.empty()) {
      if (k == Key::Enter) finish(confirm_);
      else if (k == Key::Escape) confirm_.clear();
      return true;
    }
    switch (k) {
      case Key::Escape: closed = true; return true;
      case Key::Enter: accept(); return true;
      case Key::Up:
        if (!entries_.empty()) {
          selected_ = std::max(0, selected_ - 1);
          syncSel();
        }
        return true;
      case Key::Down:
        if (!entries_.empty()) {
          selected_ = std::min(static_cast<int>(entries_.size()) - 1, selected_ + 1);
          syncSel();
        }
        return true;
      default: name_.key(k, mods); return true;
    }
  }
  bool onChar(uint32_t ch, unsigned mods) override {
    if (ch == 13 || ch == 27 || ch == 9 || !confirm_.empty()) return true;
    name_.chr(ch, mods);
    return true;
  }

 private:
  enum { kUp = 1, kHome, kOk, kCancel, kReplace, kKeep };
  struct Entry {
    std::string name;
    bool dir;
  };
  static std::string join(const std::string& d, const std::string& n) { return (d.empty() || d.back() == '/') ? d + n : d + "/" + n; }
  void syncSel() {
    if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) return;
    if (!entries_[static_cast<size_t>(selected_)].dir) name_.set(entries_[static_cast<size_t>(selected_)].name);
    const int visible = std::max(1, static_cast<int>(list_.h / 22));
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + visible) scroll_ = selected_ - visible + 1;
  }
  bool accepts(const std::string& name) const {
    if (exts_.empty()) return true;
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string e = lower(name.substr(dot + 1));
    for (const auto& x : exts_)
      if (lower(x) == e) return true;
    return false;
  }
  void load(const std::string& dir) {
    std::vector<Entry> list;
    struct Ctx {
      std::vector<Entry>* list;
    } cx{&list};
    sq_result r = sq_fs_list_dir(
        dir.c_str(),
        [](void* u, const char* name, sq_bool isDir) -> sq_bool {
          auto* c = static_cast<Ctx*>(u);
          if (name && name[0] != '.') c->list->push_back(Entry{name, isDir != 0});
          return SQ_TRUE;
        },
        &cx);
    if (r != SQ_OK) {
      error_ = std::string("cannot list ") + dir + ": " + sq_result_string(r);
      return;
    }
    error_.clear();
    dir_ = dir;
    entries_.clear();
    for (auto& en : list)
      if (en.dir || (!folder_ && accepts(en.name))) entries_.push_back(en);
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
      if (a.dir != b.dir) return a.dir;
      return lower(a.name) < lower(b.name);
    });
    selected_ = -1;
    scroll_ = 0;
  }
  void accept() {
    std::string n = normSlashes(name_.text);
    if (folder_) {  // the typed folder (may be new), else the selected subfolder, else the current one
      const bool sel = selected_ >= 0 && selected_ < static_cast<int>(entries_.size());
      std::string full = n.empty() ? (sel ? join(dir_, entries_[static_cast<size_t>(selected_)].name) : dir_)
                                   : ((n[0] == '/' || (n.size() > 2 && n[1] == ':')) ? n : join(dir_, n));
      if (sq_fs_exists(full.c_str()) && !sq_fs_is_dir(full.c_str())) {
        error_ = "not a folder: " + full;
        return;
      }
      finish(full);
      return;
    }
    if (n.empty()) {
      if (selected_ >= 0 && selected_ < static_cast<int>(entries_.size()) && entries_[static_cast<size_t>(selected_)].dir)
        load(join(dir_, entries_[static_cast<size_t>(selected_)].name));
      return;
    }
    const bool absolute = n[0] == '/' || (n.size() > 2 && n[1] == ':');
    std::string full = absolute ? n : join(dir_, n);
    if (sq_fs_is_dir(full.c_str())) {
      name_.set("");
      load(full);
      return;
    }
    if (save_ && !exts_.empty() && !accepts(full)) full += "." + exts_.front();
    if (!save_ && !sq_fs_is_file(full.c_str())) {
      error_ = "not found: " + full;
      return;
    }
    if (save_ && sq_fs_is_file(full.c_str())) {  // ask before overwriting
      confirm_ = full;
      hot_ = 0;
      return;
    }
    finish(full);
  }
  void finish(const std::string& full) {
    auto cb = ok_;
    after = [cb, full] {
      if (cb) cb(full);
    };
    confirm_.clear();
    closed = true;
  }

  bool save_, folder_;
  std::string confirm_;  // existing file waiting for "Replace?"
  Rect replaceR_, keepR_;
  std::string title_;
  std::vector<std::string> exts_;
  std::function<void(const std::string&)> ok_;
  std::string dir_, error_;
  std::vector<Entry> entries_;
  TextEdit name_;
  Rect box_, upR_, homeR_, list_, field_, okR_, cancelR_;
  std::vector<Rect> rows_;
  int scroll_ = 0, selected_ = -1, hotRow_ = -1, hot_ = 0;
};

}  // namespace

std::unique_ptr<Overlay> makeMenuOverlay(std::vector<MenuItem> items, Point at) { return std::make_unique<MenuOverlay>(std::move(items), at); }
std::unique_ptr<Overlay> makeMenuBarOverlay(std::vector<MenuItem> items, Point at, MenuBarLink link) {
  return std::make_unique<MenuOverlay>(std::move(items), at, std::move(link));
}
std::unique_ptr<Overlay> makeChoiceOverlay(const std::string& message, std::vector<std::string> buttons, std::function<void(int)> onPick) {
  return std::make_unique<ChoiceOverlay>(message, std::move(buttons), std::move(onPick));
}
std::unique_ptr<Overlay> makeFolderOverlay(const std::string& title, const std::string& initial, std::function<void(const std::string&)> onOk) {
  return std::make_unique<FileOverlay>(true, title, std::vector<std::string>{}, initial, std::move(onOk), true);
}
std::unique_ptr<Overlay> makePromptOverlay(const std::string& title, const std::string& initial, std::function<void(const std::string&)> onOk) {
  return std::make_unique<PromptOverlay>(title, initial, std::move(onOk));
}
std::unique_ptr<Overlay> makeConfirmOverlay(const std::string& message, std::function<void()> onYes) {
  return std::make_unique<ConfirmOverlay>(message, std::move(onYes));
}
std::unique_ptr<Overlay> makeFileOverlay(bool save, const std::string& title, const std::vector<std::string>& exts, const std::string& initial,
                                         std::function<void(const std::string&)> onOk) {
  return std::make_unique<FileOverlay>(save, title, exts, initial, std::move(onOk));
}

}  // namespace sq::app::gui
