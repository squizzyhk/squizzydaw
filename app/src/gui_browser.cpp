// Squizzy Studio GUI - browser panel: Files (places + lazily expanded folder tree filtered to
// audio / MIDI / song files, search, drag onto the arrange view, double-click to import/open),
// Plugins (scanned CLAP/VST3 plugins, insert on the selected track, rescan) and Devices (the
// built-in instruments and effects, gui_browser_devices.cpp).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include "gui_overlay.hpp"
#include "gui_panels.hpp"
#include "sq/interfaces/pluginhost.h"
#include "sq/platform.h"
#include "sq/platform.hpp"

namespace sq::app::gui {

// Devices tab rows (gui_browser_devices.cpp): fills `out` with (category, id, name, instrument).
struct DeviceEntry {
  std::string category, id, name;
  bool instrument = false;
};
std::vector<DeviceEntry> browserDeviceEntries();
// Adds a device to the selected track (instrument -> track instrument / new instrument track,
// effect -> insert on the selected track).
void browserUseDevice(GuiContext& ctx, const DeviceEntry& d);

namespace {

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
  return s;
}
std::string extOf(const std::string& name) {
  const size_t d = name.find_last_of('.');
  return d == std::string::npos ? std::string() : lower(name.substr(d + 1));
}
bool isAudio(const std::string& e) { return e == "wav" || e == "flac" || e == "ogg" || e == "mp3" || e == "aif" || e == "aiff" || e == "m4a" || e == "opus"; }
bool isMidi(const std::string& e) { return e == "mid" || e == "midi"; }
bool isSong(const std::string& e) { return e == "song" || e == "dawproject"; }
std::string norm(std::string s) {
  for (char& ch : s)
    if (ch == '\\') ch = '/';
  while (s.size() > 1 && s.back() == '/' && !(s.size() == 3 && s[1] == ':')) s.pop_back();
  return s;
}
std::string join(const std::string& d, const std::string& n) { return (d.empty() || d.back() == '/') ? d + n : d + "/" + n; }

struct Node {
  std::string name, path;
  bool dir = false;
  bool expanded = false;
  bool loaded = false;
  int depth = 0;
  std::vector<std::unique_ptr<Node>> kids;
};

void loadChildren(Node& n) {
  n.loaded = true;
  n.kids.clear();
  struct Ctx {
    std::vector<std::pair<std::string, bool>> list;
  } cx;
  sq_fs_list_dir(
      n.path.c_str(),
      [](void* u, const char* name, sq_bool isDir) -> sq_bool {
        auto* c = static_cast<Ctx*>(u);
        if (name && name[0] != '.') c->list.emplace_back(name, isDir != 0);
        return c->list.size() < 5000 ? SQ_TRUE : SQ_FALSE;
      },
      &cx);
  std::sort(cx.list.begin(), cx.list.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second;
    return lower(a.first) < lower(b.first);
  });
  for (auto& [name, dir] : cx.list) {
    if (!dir) {
      const std::string e = extOf(name);
      if (!isAudio(e) && !isMidi(e) && !isSong(e)) continue;
    }
    auto k = std::make_unique<Node>();
    k->name = name;
    k->path = join(n.path, name);
    k->dir = dir;
    k->depth = n.depth + 1;
    n.kids.push_back(std::move(k));
  }
}

class Browser final : public Widget, public CharInput {
 public:
  explicit Browser(GuiContext& ctx) : Widget("Browser"), ctx_(ctx) { buildPlaces(); }
  bool focusable() const override { return true; }

  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.panel);
    c.pushClip(f);
    // tabs
    static const char* kTabs[] = {"Files", "Devices", "Plugins"};
    float x = f.x + 8;
    for (int i = 0; i < 3; ++i) {
      const float w = (f.w - 16) / 3;
      tabR_[i] = Rect{x, f.y + 6, w - 4, 24};
      const bool on = tab_ == i;
      c.fillRoundRect(tabR_[i], 5, on ? p.panel3 : (hotTab_ == i ? p.panel2 : p.panel));
      c.drawText(kTabs[i], tabR_[i], on ? p.text : p.dim, 12, TextAlign::Center);
      x += w;
    }
    // search
    searchR_ = Rect{f.x + 8, f.y + 36, f.w - 16, 26};
    search_.paint(c, p, searchR_, searchFocus_ && tree() && tree()->focus() == this, 12);
    if (search_.text.empty()) c.drawText(tab_ == 0 ? "Search files..." : tab_ == 1 ? "Search devices..." : "Search plugins...",
                                         Rect{searchR_.x + 8, searchR_.y, searchR_.w - 16, searchR_.h}, p.faint, 12);
    list_ = Rect{f.x, searchR_.bottom() + 6, f.w, f.bottom() - searchR_.bottom() - 6 - 30};
    rows_.clear();
    if (tab_ == 0) paintFiles(c, p);
    else if (tab_ == 1) paintDevices(c, p);
    else paintPlugins(c, p);
    // footer
    footR_ = Rect{f.x, f.bottom() - 30, f.w, 30};
    c.fillRect(Rect{footR_.x, footR_.y, footR_.w, 1}, p.border);
    footBtn_ = Rect{f.x + 8, footR_.y + 4, f.w - 16, 22};
    const char* foot = tab_ == 0 ? "+ Add Folder..." : tab_ == 1 ? "Double-click or drag onto a track" : "Rescan Plugins";
    if (tab_ == 1) c.drawText(foot, footBtn_, p.faint, 11, TextAlign::Center);
    else draw::button(c, p, footBtn_, foot, hotFoot_, false, p.accent);
    c.popClip();
    // drag ghost
    if (dragging_) {
      Rect g{dragPos_.x + 12, dragPos_.y + 8, std::min(260.0f, c.measureText(dragLabel_, 12) + 20), 22};
      c.fillRoundRect(g, 4, p.accent.withAlpha(0.9f));
      c.drawText(dragLabel_, Rect{g.x + 10, g.y, g.w - 14, g.h}, p.dark ? Color::rgb(0x111214) : Color::rgb(0xFFFFFF), 12);
    }
  }

  bool onMouse(const WidgetMouse& e) override {
    switch (e.action) {
      case MouseAction::Hover: {
        int ht = -1;
        for (int i = 0; i < 3; ++i)
          if (tabR_[i].contains(e.x, e.y)) ht = i;
        hotTab_ = ht;
        hotRow_ = rowAt(e.x, e.y);
        hotFoot_ = footBtn_.contains(e.x, e.y);
        ctx_.invalidate();
        return true;
      }
      case MouseAction::Leave:
        hotRow_ = -1;
        hotTab_ = -1;
        hotFoot_ = false;
        return false;
      case MouseAction::Wheel:
        if (list_.contains(e.x, e.y)) {
          scroll_ = std::max(0.0f, scroll_ - e.wheel * 66.0f);
          ctx_.invalidate();
          return true;
        }
        return false;
      case MouseAction::Down: {
        searchFocus_ = searchR_.contains(e.x, e.y);
        for (int i = 0; i < 3; ++i)
          if (tabR_[i].contains(e.x, e.y)) {
            tab_ = i;
            scroll_ = 0;
            selected_ = -1;
            search_.set("");
            search_.selectAll = false;
            return true;
          }
        if (footBtn_.contains(e.x, e.y)) {
          footer();
          return true;
        }
        const int r = rowAt(e.x, e.y);
        if (r >= 0) {
          selected_ = r;
          pressRow_ = r;
          pressPos_ = Point{e.x, e.y};
          if (e.button == MouseButton::Right) rowMenu(r, Point{e.x, e.y});
          else if (tab_ == 0 && rows_[static_cast<size_t>(r)].node && rows_[static_cast<size_t>(r)].node->dir &&
                   e.x < rows_[static_cast<size_t>(r)].r.x + 14.0f * static_cast<float>(rows_[static_cast<size_t>(r)].node->depth) + 26.0f)
            toggle(*rows_[static_cast<size_t>(r)].node);
        }
        return true;
      }
      case MouseAction::Move:
        if (pressRow_ >= 0 && !dragging_ && (std::fabs(e.x - pressPos_.x) + std::fabs(e.y - pressPos_.y)) > 6) startDrag(pressRow_);
        if (dragging_) {
          dragPos_ = Point{e.x, e.y};
          ctx_.invalidate();
        }
        return true;
      case MouseAction::Up:
        if (dragging_) finishDrag(e.x, e.y);
        pressRow_ = -1;
        return true;
      case MouseAction::DoubleClick: {
        const int r = rowAt(e.x, e.y);
        if (r >= 0) activate(r);
        return true;
      }
      default: return false;
    }
  }

  bool onKey(Key k, unsigned mods) override {
    if (searchFocus_) {
      if (k == Key::Escape) {
        searchFocus_ = false;
        return true;
      }
      if (k == Key::Enter) {
        if (tab_ == 0) runSearch();
        return true;
      }
      if (search_.key(k, mods)) return true;
    }
    const int n = static_cast<int>(rowCount_);
    switch (k) {
      case Key::Up:
        if (n) selected_ = std::max(0, selected_ - 1);
        return n > 0;
      case Key::Down:
        if (n) selected_ = std::min(n - 1, selected_ + 1);
        return n > 0;
      case Key::Enter:
        if (selected_ >= 0) activate(selected_);
        return true;
      case Key::Right:
      case Key::Left:
        if (tab_ == 0 && selected_ >= 0 && selected_ < static_cast<int>(rows_.size()) && rows_[static_cast<size_t>(selected_)].node &&
            rows_[static_cast<size_t>(selected_)].node->dir) {
          Node* nd = rows_[static_cast<size_t>(selected_)].node;
          if ((k == Key::Right) != nd->expanded) toggle(*nd);
          return true;
        }
        return false;
      default: return false;
    }
  }

  bool onChar(uint32_t ch, unsigned mods) override {
    if (!searchFocus_) return false;
    if (ch == 13 || ch == 27 || ch == 9) return true;
    search_.chr(ch, mods);
    results_.clear();
    searched_.clear();
    scroll_ = 0;
    return true;
  }

 private:
  struct Row {
    Rect r;
    Node* node = nullptr;  // files
    int index = -1;        // devices / plugins / search results
  };

  void buildPlaces() {
    places_.clear();
    auto add = [&](const std::string& nm, const std::string& path) {
      const std::string p = norm(path);
      if (p.empty() || !sq_fs_is_dir(p.c_str())) return;
      for (const auto& q : places_)
        if (q->path == p) return;
      auto n = std::make_unique<Node>();
      n->name = nm;
      n->path = p;
      n->dir = true;
      places_.push_back(std::move(n));
    };
    const std::string home = norm(sq::specialDir(SQ_DIR_HOME));
    add("Home", home);
    add("Documents", sq::specialDir(SQ_DIR_DOCUMENTS));
    add("Music", join(home, "Music"));
    add("Desktop", join(home, "Desktop"));
    add("Downloads", join(home, "Downloads"));
    std::string extra = ctx_.shell().settings.get("UI.Browser.Places");
    size_t pos = 0;
    while (pos < extra.size()) {
      size_t nl = extra.find('\n', pos);
      std::string line = extra.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = nl == std::string::npos ? extra.size() : nl + 1;
      if (!line.empty()) add(line.substr(line.find_last_of("/\\") + 1), line);
    }
  }

  void toggle(Node& n) {
    if (!n.dir) return;
    n.expanded = !n.expanded;
    if (n.expanded && !n.loaded) loadChildren(n);
    ctx_.invalidate();
  }

  void addRow(Canvas& c, const Palette& p, float& y, Node* node, int index, const std::string& label, const std::string& detail, int depth,
              bool dir, bool expanded, Color iconCol) {
    const float rowH = 22;
    const size_t i = rowCount_++;
    if (y + rowH >= list_.y && y < list_.bottom()) {
      Row row;
      row.r = Rect{list_.x + 4, y, list_.w - 8, rowH};
      row.node = node;
      row.index = index;
      const bool sel = static_cast<int>(i) == selected_;
      if (sel) c.fillRoundRect(row.r, 4, p.accent.withAlpha(0.3f));
      else if (static_cast<int>(rows_.size()) == hotRow_ - static_cast<int>(firstVisible_)) {
      }
      if (static_cast<int>(i) == hotRowAbs_ && !sel) c.fillRoundRect(row.r, 4, p.panel3.withAlpha(0.6f));
      float x = row.r.x + 6 + 14.0f * static_cast<float>(depth);
      if (dir) draw::triangle(c, Rect{x, y + 7, 7, 8}, expanded ? 2 : 1, p.dim);
      x += 12;
      c.fillRoundRect(Rect{x, y + 5, 12, 12}, 3, iconCol);
      x += 18;
      c.drawText(label, Rect{x, y, row.r.right() - x - (detail.empty() ? 4 : 70), rowH}, p.text, 12);
      if (!detail.empty()) c.drawText(detail, Rect{row.r.right() - 70, y, 64, rowH}, p.faint, 10.5f, TextAlign::Right);
      if (rows_.empty()) firstVisible_ = i;
      rows_.push_back(row);
    }
    y += rowH;
  }

  void beginList(Canvas& c) {
    rowCount_ = 0;
    firstVisible_ = 0;
    hotRowAbs_ = hotRow_ >= 0 && hotRow_ < static_cast<int>(prevRows_.size()) ? prevAbs_[static_cast<size_t>(hotRow_)] : -1;
    c.pushClip(list_);
  }
  void endList(Canvas& c, const Palette& p) {
    c.popClip();
    const float total = static_cast<float>(rowCount_) * 22.0f;
    const float maxScroll = std::max(0.0f, total - list_.h + 8);
    if (scroll_ > maxScroll) scroll_ = maxScroll;
    if (total > list_.h) {
      const float th = std::max(24.0f, list_.h * list_.h / total);
      const float ty = list_.y + (list_.h - th) * (maxScroll > 0 ? scroll_ / maxScroll : 0);
      c.fillRoundRect(Rect{list_.right() - 5, ty, 3, th}, 1.5f, p.faint);
    }
    prevRows_ = rows_;
    prevAbs_.clear();
    for (size_t i = 0; i < rows_.size(); ++i) prevAbs_.push_back(static_cast<int>(firstVisible_ + i));
    if (selected_ >= static_cast<int>(rowCount_)) selected_ = -1;
  }

  void paintFiles(Canvas& c, const Palette& p) {
    beginList(c);
    float y = list_.y - scroll_;
    if (!search_.text.empty() && searched_ == search_.text) {
      for (size_t i = 0; i < results_.size(); ++i) {
        const std::string& path = results_[i];
        const std::string nm = path.substr(path.find_last_of('/') + 1);
        addRow(c, p, y, nullptr, static_cast<int>(i), nm, parentName(path), 0, false, false, iconFor(p, nm));
      }
      if (results_.empty()) c.drawText("No matches", Rect{list_.x + 12, list_.y + 4, list_.w - 24, 20}, p.faint, 12);
    } else {
      const std::string filter = lower(search_.text);
      std::function<void(Node&)> walk = [&](Node& n) {
        const bool match = filter.empty() || n.dir || lower(n.name).find(filter) != std::string::npos;
        if (match) addRow(c, p, y, &n, -1, n.name, std::string(), n.depth, n.dir, n.expanded, n.dir ? p.mute.withAlpha(0.85f) : iconFor(p, n.name));
        if (n.dir && n.expanded)
          for (auto& k : n.kids) walk(*k);
      };
      for (auto& pl : places_) walk(*pl);
      if (!search_.text.empty())
        c.drawText("Enter: search inside places", Rect{list_.x + 12, list_.bottom() - 20, list_.w - 24, 18}, p.faint, 10.5f);
    }
    endList(c, p);
  }

  void paintDevices(Canvas& c, const Palette& p) {
    beginList(c);
    float y = list_.y - scroll_;
    devices_ = browserDeviceEntries();
    const std::string filter = lower(search_.text);
    std::string lastCat;
    for (size_t i = 0; i < devices_.size(); ++i) {
      const auto& d = devices_[i];
      if (!filter.empty() && lower(d.name).find(filter) == std::string::npos && lower(d.category).find(filter) == std::string::npos) continue;
      if (d.category != lastCat) {
        lastCat = d.category;
        if (y + 22 >= list_.y && y < list_.bottom()) c.drawTextBold(lastCat, Rect{list_.x + 12, y + 4, list_.w - 24, 18}, p.dim, 11);
        y += 24;
      }
      addRow(c, p, y, nullptr, static_cast<int>(i), d.name, std::string(), 1, false, false, d.instrument ? p.accent2 : p.accent);
    }
    if (devices_.empty()) c.drawText("No built-in devices in this build", Rect{list_.x + 12, list_.y + 4, list_.w - 24, 20}, p.faint, 12);
    endList(c, p);
  }

  void paintPlugins(Canvas& c, const Palette& p) {
    beginList(c);
    float y = list_.y - scroll_;
    plugins_.clear();
    if (auto reg = ctx_.shell().plugins()) {
      for (uint32_t i = 0; i < reg->getPluginCount(); ++i) {
        sq_plugin_desc d{};
        d.struct_size = sizeof d;
        if (reg->getPlugin(i, &d) == SQ_OK && d.status == SQ_PLUGIN_STATUS_OK) plugins_.push_back(d);
      }
      if (reg->isScanning()) c.drawText("Scanning...", Rect{list_.x + 12, list_.bottom() - 20, list_.w - 24, 18}, p.faint, 11);
    }
    std::sort(plugins_.begin(), plugins_.end(), [](const sq_plugin_desc& a, const sq_plugin_desc& b) { return lower(a.name) < lower(b.name); });
    const std::string filter = lower(search_.text);
    for (size_t i = 0; i < plugins_.size(); ++i) {
      const auto& d = plugins_[i];
      if (!filter.empty() && lower(d.name).find(filter) == std::string::npos && lower(d.vendor).find(filter) == std::string::npos) continue;
      const bool inst = std::strstr(d.category, "Instrument") != nullptr;
      addRow(c, p, y, nullptr, static_cast<int>(i), d.name, d.format, 0, false, false, inst ? p.accent2 : p.accent);
    }
    if (plugins_.empty()) c.drawText("No plugins found. Install CLAP/VST3 plugins and rescan.", Rect{list_.x + 12, list_.y + 4, list_.w - 24, 40}, p.faint, 11.5f);
    endList(c, p);
  }

  static Color iconFor(const Palette& p, const std::string& name) {
    const std::string e = extOf(name);
    if (isAudio(e)) return p.accent;
    if (isMidi(e)) return p.accent2;
    if (isSong(e)) return p.solo;
    return p.faint;
  }
  static std::string parentName(const std::string& path) {
    const size_t k = path.find_last_of('/');
    if (k == std::string::npos || k == 0) return {};
    const std::string d = path.substr(0, k);
    return d.substr(d.find_last_of('/') + 1);
  }

  int rowAt(float x, float y) const {
    for (size_t i = 0; i < rows_.size(); ++i)
      if (rows_[i].r.contains(x, y)) return static_cast<int>(i);
    return -1;
  }

  std::string rowPath(int r) const {
    if (r < 0 || r >= static_cast<int>(rows_.size())) return {};
    const Row& row = rows_[static_cast<size_t>(r)];
    if (row.node) return row.node->dir ? std::string() : row.node->path;
    if (tab_ == 0 && row.index >= 0 && row.index < static_cast<int>(results_.size())) return results_[static_cast<size_t>(row.index)];
    return {};
  }

  void activate(int r) {
    if (r < 0 || r >= static_cast<int>(rows_.size())) return;
    const Row& row = rows_[static_cast<size_t>(r)];
    if (tab_ == 0) {
      if (row.node && row.node->dir) {
        toggle(*row.node);
        return;
      }
      openFile(rowPath(r));
    } else if (tab_ == 1) {
      if (row.index >= 0 && row.index < static_cast<int>(devices_.size())) browserUseDevice(ctx_, devices_[static_cast<size_t>(row.index)]);
    } else {
      if (row.index >= 0 && row.index < static_cast<int>(plugins_.size())) usePlugin(plugins_[static_cast<size_t>(row.index)]);
    }
  }

  void openFile(const std::string& path) {
    if (path.empty()) return;
    const std::string e = extOf(path);
    if (isSong(e)) {
      auto a = sq::makeAttributes();
      a->setString("path", path.c_str());
      ctx_.run(e == "dawproject" ? "File.ImportDawProject" : "File.Open", a.get());
      return;
    }
    // Audio / MIDI: drop at the edit cursor on the selected track (the arrange view decides).
    Widget* arr = findArrange();
    if (arr && arrangeDropFile(arr, -1.0f, -1.0f, path)) return;
    ctx_.setStatus("Drag the file onto a track to import it");
  }

  void usePlugin(const sq_plugin_desc& d) {
    const sq_guid t = ctx_.selectedTrack();
    auto a = sq::makeAttributes();
    a->setGuid("track", &t);
    const std::string want = std::string(d.format) + ":" + d.id;
    a->setString("plugin", want.c_str());
    const bool inst = std::strstr(d.category, "Instrument") != nullptr;
    if (inst) {
      std::string spec = std::string(d.format) + "|" + d.id + "|" + d.path;
      auto b = sq::makeAttributes();
      b->setGuid("track", &t);
      b->setString("spec", spec.c_str());
      if (SQ_SUCCEEDED(ctx_.run("Track.SetInstrument", b.get()))) return;
    }
    ctx_.run("Track.InsertPlugin", a.get());
  }

  Widget* findArrange() {
    Widget* root = this;
    while (root->parent()) root = root->parent();
    return root->findByName("arrange");
  }

  void startDrag(int r) {
    if (tab_ == 0) {
      const std::string path = rowPath(r);
      if (path.empty()) return;
      dragging_ = true;
      dragPath_ = path;
      dragLabel_ = path.substr(path.find_last_of('/') + 1);
      ctx_.setDraggedFile(path);
    } else if (tab_ == 1 && r < static_cast<int>(rows_.size())) {
      const int idx = rows_[static_cast<size_t>(r)].index;
      if (idx < 0 || idx >= static_cast<int>(devices_.size())) return;
      dragging_ = true;
      dragDevice_ = idx;
      dragLabel_ = devices_[static_cast<size_t>(idx)].name;
    }
  }

  void finishDrag(float x, float y) {
    dragging_ = false;
    Widget* arr = findArrange();
    const bool overArrange = arr && arr->frame().contains(x, y);
    if (!dragPath_.empty()) {
      if (overArrange) arrangeDropFile(arr, x, y, dragPath_);
      ctx_.setDraggedFile({});
    } else if (dragDevice_ >= 0 && dragDevice_ < static_cast<int>(devices_.size())) {
      if (overArrange) browserUseDevice(ctx_, devices_[static_cast<size_t>(dragDevice_)]);
    }
    dragPath_.clear();
    dragDevice_ = -1;
    ctx_.invalidate();
  }

  void footer() {
    if (tab_ == 0) {
      ctx_.prompt("Add folder to the browser (full path)", sq::specialDir(SQ_DIR_HOME), [this](const std::string& path) {
        const std::string p = norm(path);
        if (!sq_fs_is_dir(p.c_str())) {
          ctx_.setStatus("not a folder: " + p);
          return;
        }
        std::string cur = ctx_.shell().settings.get("UI.Browser.Places");
        if (!cur.empty() && cur.back() != '\n') cur += '\n';
        cur += p;
        ctx_.shell().settings.setString("UI.Browser.Places", cur.c_str());
        buildPlaces();
        ctx_.invalidate();
      });
    } else if (tab_ == 2) {
      ctx_.run("Plugins.Rescan");
    }
  }

  void rowMenu(int r, Point at) {
    std::vector<MenuItem> m;
    if (tab_ == 0) {
      const std::string path = rowPath(r);
      const Row& row = rows_[static_cast<size_t>(r)];
      if (!path.empty()) {
        MenuItem open;
        open.label = isSong(extOf(path)) ? "Open Song" : "Import at Cursor";
        open.action = [this, path] { openFile(path); };
        m.push_back(open);
      }
      if (row.node && row.node->dir) {
        Node* nd = row.node;
        MenuItem ex;
        ex.label = nd->expanded ? "Collapse" : "Expand";
        ex.action = [this, nd] { toggle(*nd); };
        m.push_back(ex);
        MenuItem rf;
        rf.label = "Refresh";
        rf.action = [this, nd] {
          loadChildren(*nd);
          nd->expanded = true;
        };
        m.push_back(rf);
        if (nd->depth == 0) {
          MenuItem rm;
          rm.label = "Remove from Places";
          const std::string p = nd->path;
          rm.action = [this, p] {
            std::string cur = ctx_.shell().settings.get("UI.Browser.Places"), out;
            size_t pos = 0;
            while (pos < cur.size()) {
              size_t nl = cur.find('\n', pos);
              std::string line = cur.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
              pos = nl == std::string::npos ? cur.size() : nl + 1;
              if (!line.empty() && norm(line) != p) out += line + "\n";
            }
            ctx_.shell().settings.setString("UI.Browser.Places", out.c_str());
            buildPlaces();
          };
          m.push_back(rm);
        }
      }
    } else {
      MenuItem use;
      use.label = "Add to Selected Track";
      use.action = [this, r] { activate(r); };
      m.push_back(use);
    }
    if (!m.empty()) ctx_.showMenu(std::move(m), at);
  }

  void runSearch() {
    results_.clear();
    searched_ = search_.text;
    const std::string want = lower(search_.text);
    if (want.empty()) return;
    size_t visited = 0;
    std::function<void(const std::string&, int)> rec = [&](const std::string& dir, int depth) {
      if (depth > 6 || visited > 4000 || results_.size() >= 500) return;
      ++visited;
      struct Ctx {
        std::vector<std::pair<std::string, bool>> list;
      } cx;
      sq_fs_list_dir(
          dir.c_str(),
          [](void* u, const char* nm, sq_bool isDir) -> sq_bool {
            auto* c = static_cast<Ctx*>(u);
            if (nm && nm[0] != '.') c->list.emplace_back(nm, isDir != 0);
            return c->list.size() < 5000 ? SQ_TRUE : SQ_FALSE;
          },
          &cx);
      for (auto& [nm, isDir] : cx.list) {
        if (isDir) continue;
        const std::string e = extOf(nm);
        if ((isAudio(e) || isMidi(e) || isSong(e)) && lower(nm).find(want) != std::string::npos) results_.push_back(join(dir, nm));
      }
      for (auto& [nm, isDir] : cx.list)
        if (isDir) rec(join(dir, nm), depth + 1);
    };
    for (auto& pl : places_) rec(pl->path, 0);
    std::sort(results_.begin(), results_.end());
    results_.erase(std::unique(results_.begin(), results_.end()), results_.end());
    scroll_ = 0;
    selected_ = -1;
    ctx_.setStatus(std::to_string(results_.size()) + " file(s) found");
  }

  GuiContext& ctx_;
  int tab_ = 0, hotTab_ = -1;
  Rect tabR_[3];
  TextEdit search_;
  bool searchFocus_ = false;
  Rect searchR_, list_, footR_, footBtn_;
  bool hotFoot_ = false;
  std::vector<std::unique_ptr<Node>> places_;
  std::vector<Row> rows_, prevRows_;
  std::vector<int> prevAbs_;
  size_t rowCount_ = 0, firstVisible_ = 0;
  int selected_ = -1, hotRow_ = -1, hotRowAbs_ = -1, pressRow_ = -1;
  Point pressPos_;
  float scroll_ = 0;
  std::vector<DeviceEntry> devices_;
  std::vector<sq_plugin_desc> plugins_;
  std::vector<std::string> results_;
  std::string searched_;
  bool dragging_ = false;
  Point dragPos_;
  std::string dragPath_, dragLabel_;
  int dragDevice_ = -1;
};

}  // namespace

std::unique_ptr<Widget> makeBrowser(GuiContext& ctx) { return std::make_unique<Browser>(ctx); }

}  // namespace sq::app::gui
