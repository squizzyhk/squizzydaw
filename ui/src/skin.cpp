// Skin XML -> widget tree. See skin.hpp for the language.
#include "sq/ui/skin.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "sq/platform.hpp"

namespace sq::ui {

struct Skin::Impl {
  XmlDocPtr doc;
  std::map<std::string, sq_xml_node*, std::less<>> forms;
  std::map<std::string, WidgetFactory, std::less<>> custom;
};

Skin::Skin() : impl_(std::make_unique<Impl>()) {}
Skin::~Skin() = default;

namespace {

// Bare $name references: letters, digits, '_' and '.' (use $(name) for anything else).
bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }

bool parseFloat(std::string_view s, float& out) {
  std::string t(s);
  if (t.empty()) return false;
  char* end = nullptr;
  double v = std::strtod(t.c_str(), &end);
  if (end == t.c_str() || *end != '\0' || !std::isfinite(v)) return false;
  out = static_cast<float>(v);
  return true;
}

bool parseDim(std::string_view s, Dim& out) {
  if (s == "auto") {
    out = Dim::none();
    return true;
  }
  float v = 0;
  if (!s.empty() && s.back() == '%') {
    if (!parseFloat(s.substr(0, s.size() - 1), v)) return false;
    out = Dim::pct(v);
    return true;
  }
  if (!parseFloat(s, v)) return false;
  out = Dim::pt(v);
  return true;
}

bool parseEdges(std::string_view s, Edges& out) {
  std::vector<float> v;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ') ++j;
    if (j > i) {
      float f = 0;
      if (!parseFloat(s.substr(i, j - i), f)) return false;
      v.push_back(f);
    }
    i = j;
  }
  if (v.size() == 1) out = Edges::all(v[0]);
  else if (v.size() == 2) out = Edges{v[1], v[0], v[1], v[0]};
  else if (v.size() == 4) out = Edges{v[3], v[0], v[1], v[2]};
  else return false;
  return true;
}

bool truthy(std::string_view s) { return s == "1" || s == "true" || s == "yes" || s == "on"; }

// Expansion with an output cap: returns false (out truncated) once `limit` bytes are exceeded.
bool expandLimited(std::string_view s, const SkinDefines& scope, size_t limit, std::string& out);

// Skins are third-party data (and drive hardware remote pages): building one must stay bounded
// whatever the XML does. Nested <Include>s multiply (a form including the next one N times, L
// levels deep = N^L widgets) and chained <define>s double their text ("$a$a"), so besides the
// include depth the builder budgets the total widgets, visited nodes and expanded text.
constexpr size_t kMaxSkinWidgets = 100000;
constexpr size_t kMaxSkinNodes = 1000000;
constexpr size_t kMaxSkinText = 32u * 1024 * 1024;   // all expanded attribute/define text
constexpr size_t kMaxSkinValue = 1024u * 1024;       // one expanded value
// Widget nesting of one build, across includes (each XML document is already limited to 256
// levels, but 16 includes of 256 levels each would recurse ~4000 deep and overflow the stack).
constexpr int kMaxSkinNesting = 256;

struct Builder {
  const Skin::Impl& impl;
  std::string* error;
  int depth = 0;
  int nesting = 0;
  size_t widgets = 0, nodes = 0, text = 0;
  bool exhausted = false;

  bool fail(const std::string& msg) {
    if (error && error->empty()) *error = msg;
    return false;
  }

  // Skin::expand under the text budget. On overflow the build fails (ok = false).
  std::string ex(std::string_view s, const SkinDefines& scope, bool& ok) {
    std::string out;
    if (exhausted) {
      ok = false;
      return out;
    }
    if (!expandLimited(s, scope, kMaxSkinValue, out)) {
      exhausted = true;
      ok = false;
      fail("skin value expands beyond " + std::to_string(kMaxSkinValue) + " bytes");
      return out;
    }
    text += out.size();
    if (text > kMaxSkinText) {
      exhausted = true;
      ok = false;
      fail("skin expands beyond " + std::to_string(kMaxSkinText) + " bytes of text");
    }
    return out;
  }
  std::string ex(std::string_view s, const SkinDefines& scope) {
    bool ok = true;
    return ex(s, scope, ok);
  }
  bool visit() {
    if (exhausted) return false;
    if (++nodes > kMaxSkinNodes) {
      exhausted = true;
      return fail("skin build visits more than " + std::to_string(kMaxSkinNodes) + " nodes");
    }
    return true;
  }

  std::unique_ptr<Widget> create(std::string_view element) {
    if (exhausted) return nullptr;
    if (++widgets > kMaxSkinWidgets) {
      exhausted = true;
      fail("skin builds more than " + std::to_string(kMaxSkinWidgets) + " widgets");
      return nullptr;
    }
    if (auto it = impl.custom.find(element); it != impl.custom.end()) return it->second();
    if (element == "View") return std::make_unique<View>();
    if (element == "Label") return std::make_unique<Label>();
    if (element == "Knob") return std::make_unique<Knob>();
    if (element == "Fader") return std::make_unique<Fader>();
    if (element == "Button") return std::make_unique<Button>();
    if (element == "Meter") return std::make_unique<Meter>();
    if (element == "Image") return std::make_unique<ImageView>();
    return nullptr;
  }

  bool applyAttr(Widget& w, std::string_view k, const std::string& v) {
    auto& st = w.style;
    auto bad = [&] { return fail("bad value '" + v + "' for attribute '" + std::string(k) + "' on <" + w.type() + ">"); };
    float f = 0;
    Color c;
    if (k == "name") w.name = v;
    else if (k == "text") w.text = v;
    else if (k == "tooltip") w.tooltip = v;
    else if (k == "remote") w.remoteId = v;
    else if (k == "remoteType") {
      if (v != "knob" && v != "encoder" && v != "fader" && v != "button" && v != "pad" && v != "display" && v != "led") return bad();
      w.remoteType = v;
    } else if (k == "feedback") w.remoteFeedback = truthy(v);
    else if (k == "command") w.command = v;
    else if (k == "target") w.target.url = v;
    else if (k == "property") w.target.property = v;
    else if (k == "width" || k == "height" || k == "minWidth" || k == "minHeight" || k == "maxWidth" || k == "maxHeight" || k == "basis" ||
             k == "left" || k == "top" || k == "right" || k == "bottom") {
      Dim d;
      if (!parseDim(v, d)) return bad();
      Dim* tgt = k == "width" ? &st.width : k == "height" ? &st.height : k == "minWidth" ? &st.minWidth : k == "minHeight" ? &st.minHeight
               : k == "maxWidth" ? &st.maxWidth : k == "maxHeight" ? &st.maxHeight : k == "basis" ? &st.basis : k == "left" ? &st.left
               : k == "top" ? &st.top : k == "right" ? &st.right : &st.bottom;
      *tgt = d;
    } else if (k == "grow" || k == "shrink" || k == "gap" || k == "borderWidth" || k == "radius" || k == "fontSize" || k == "opacity") {
      if (!parseFloat(v, f)) return bad();
      if (k == "grow") st.grow = f;
      else if (k == "shrink") st.shrink = f;
      else if (k == "gap") st.gap = f;
      else if (k == "borderWidth") w.visual.borderWidth = f;
      else if (k == "radius") w.visual.radius = f;
      else if (k == "fontSize") w.visual.fontSize = f;
      else w.visual.opacity = f;
    } else if (k == "min" || k == "max" || k == "default" || k == "value") {
      if (!parseFloat(v, f)) return bad();
      if (k == "min") w.minValue = f;
      else if (k == "max") w.maxValue = f;
      else if (k == "default") w.defaultValue = f;
      else w.setValue(f);
    } else if (k == "direction") {
      if (v == "row") st.direction = FlexDirection::Row;
      else if (v == "column") st.direction = FlexDirection::Column;
      else if (v == "row-reverse") st.direction = FlexDirection::RowReverse;
      else if (v == "column-reverse") st.direction = FlexDirection::ColumnReverse;
      else return bad();
    } else if (k == "justify") {
      if (v == "start") st.justify = Justify::Start;
      else if (v == "end") st.justify = Justify::End;
      else if (v == "center") st.justify = Justify::Center;
      else if (v == "between") st.justify = Justify::SpaceBetween;
      else if (v == "around") st.justify = Justify::SpaceAround;
      else if (v == "evenly") st.justify = Justify::SpaceEvenly;
      else return bad();
    } else if (k == "align" || k == "alignSelf") {
      Align a;
      if (v == "start") a = Align::Start;
      else if (v == "end") a = Align::End;
      else if (v == "center") a = Align::Center;
      else if (v == "stretch") a = Align::Stretch;
      else if (v == "auto") a = Align::Auto;
      else return bad();
      (k == "align" ? st.alignItems : st.alignSelf) = a;
    } else if (k == "wrap") st.wrap = truthy(v) || v == "wrap" ? FlexWrap::Wrap : FlexWrap::NoWrap;
    else if (k == "position") st.position = v == "absolute" ? PositionType::Absolute : PositionType::Relative;
    else if (k == "display") st.display = v != "none";
    else if (k == "padding" || k == "margin") {
      Edges e;
      if (!parseEdges(v, e)) return bad();
      (k == "padding" ? st.padding : st.margin) = e;
    } else if (k == "background" || k == "border" || k == "color" || k == "accent" || k == "trackColor" || k == "thumbColor") {
      if (!Skin::parseColor(v, c)) return bad();
      if (k == "background") w.visual.background = c;
      else if (k == "border") w.visual.border = c;
      else if (k == "color") w.visual.foreground = c;
      else if (k == "accent") w.visual.accent = c;
      else if (k == "trackColor") w.visual.track = c;
      else w.visual.thumb = c;
    } else if (k == "textAlign") {
      w.visual.textAlign = v == "center" ? TextAlign::Center : v == "right" ? TextAlign::Right : TextAlign::Left;
    } else if (k == "image") w.visual.image = v;
    else if (k == "enabled") w.enabled = truthy(v);
    else if (k == "toggles") {
      if (auto* b = dynamic_cast<Button*>(&w)) b->toggles = truthy(v);
    } else if (k == "horizontal") {
      if (auto* fd = dynamic_cast<Fader*>(&w)) fd->horizontal = truthy(v);
      else if (auto* m = dynamic_cast<Meter*>(&w)) m->horizontal = truthy(v);
    } else if (k == "bipolar") {
      if (auto* kn = dynamic_cast<Knob*>(&w)) kn->bipolar = truthy(v);
    } else return fail("unknown attribute '" + std::string(k) + "' on <" + w.type() + ">");
    return true;
  }

  bool evalIf(XmlNode n, SkinDefines& scope) {
    if (n.hasAttr("defined")) return scope.count(ex(n.attr("defined"), scope)) != 0;
    if (n.hasAttr("undefined")) return scope.count(ex(n.attr("undefined"), scope)) == 0;
    if (n.hasAttr("equals")) return ex(n.attr("equals"), scope) == ex(n.attr("value"), scope);
    if (n.hasAttr("notequals")) return ex(n.attr("notequals"), scope) != ex(n.attr("value"), scope);
    return false;
  }

  // Appends the widgets produced by the children of `n` to `parent`. <define> adds to the scope
  // of the remaining siblings (and their subtrees).
  bool children(XmlNode n, Widget& parent, SkinDefines scope) {
    for (XmlNode c : n.children())
      if (!one(c, parent, scope)) return false;
    return true;
  }

  bool one(XmlNode c, Widget& parent, SkinDefines& scope) {
    if (!visit()) return false;
    std::string_view el = c.name();
    if (el == "define") {
      bool ok = true;
      std::string v = ex(c.attr("value"), scope, ok);
      if (!ok) return false;
      scope[c.attr("name")] = std::move(v);
      return true;
    }
    if (el == "if" || el == "switch") {
      const bool r = directive(c, parent, scope);
      return r && !exhausted;
    }
    if (el == "Include") {
      auto w = include(c, scope);
      if (!w) return false;
      parent.addChild(std::move(w));
      return true;
    }
    auto w = element(c, scope);
    if (!w) return false;
    parent.addChild(std::move(w));
    return true;
  }
  // <if>/<switch>: the chosen branch's children go straight into `parent`.
  bool directive(XmlNode c, Widget& parent, SkinDefines& scope) {
    std::string_view el = c.name();
    if (el == "if") {
      if (evalIf(c, scope)) {
        for (XmlNode k : c.children()) {
          if (k.name() == "else") continue;
          if (!one(k, parent, scope)) return false;
        }
        return true;
      }
      XmlNode e = c.child("else");
      return !e || children(e, parent, scope);
    }
    std::string v = ex(c.attr("value"), scope);
    XmlNode chosen;
    for (XmlNode k : c.children("case"))
      if (ex(k.attr("value"), scope) == v) {
        chosen = k;
        break;
      }
    if (!chosen) chosen = c.child("default");
    return !chosen || children(chosen, parent, scope);
  }

  std::unique_ptr<Widget> include(XmlNode c, const SkinDefines& scope) {
    bool ok = true;
    std::string form = ex(c.attr("form"), scope, ok);
    SkinDefines inner = scope;
    for (uint32_t i = 0; ok && i < sq_xml_attr_count(c.raw()); ++i) {
      std::string k = sq_xml_attr_name(c.raw(), i);
      if (k != "form") inner[k] = ex(sq_xml_attr_value(c.raw(), i), scope, ok);
    }
    if (!ok) return nullptr;
    return form_(form, inner);
  }

  std::unique_ptr<Widget> form_(std::string_view form, const SkinDefines& scope) {
    auto it = impl.forms.find(form);
    if (it == impl.forms.end()) {
      fail("unknown form '" + std::string(form) + "'");
      return nullptr;
    }
    if (depth + 1 > 16) {
      fail("form includes nest deeper than 16 levels");
      return nullptr;
    }
    ++depth;
    auto w = element(XmlNode(it->second), scope, "View");
    --depth;
    return w;
  }

  std::unique_ptr<Widget> element(XmlNode n, const SkinDefines& scopeIn, std::string_view as = {}) {
    if (nesting + 1 > kMaxSkinNesting) {
      exhausted = true;
      fail("skin widgets nest deeper than " + std::to_string(kMaxSkinNesting) + " levels");
      return nullptr;
    }
    struct Nest {
      int& n;
      explicit Nest(int& v) : n(++v) {}
      ~Nest() { --n; }
      Nest(const Nest&) = delete;
      Nest& operator=(const Nest&) = delete;
    } guard(nesting);
    std::string_view el = as.empty() ? n.name() : as;
    auto w = create(el);
    if (!w) {
      fail("unknown skin element <" + std::string(n.name()) + ">");
      return nullptr;
    }
    SkinDefines scope = scopeIn;
    for (uint32_t i = 0; i < sq_xml_attr_count(n.raw()); ++i) {
      std::string k = sq_xml_attr_name(n.raw(), i);
      if (!as.empty() && k == "name" && n.name() == "Form") {
        w->name = sq_xml_attr_value(n.raw(), i);
        continue;
      }
      bool ok = true;
      std::string v = ex(sq_xml_attr_value(n.raw(), i), scope, ok);
      if (!ok || !applyAttr(*w, k, v)) return nullptr;
    }
    if (!children(n, *w, scope)) return nullptr;
    return w;
  }
};

}  // namespace

std::string Skin::expand(std::string_view s, const SkinDefines& scope) {
  std::string out;
  expandLimited(s, scope, std::string::npos, out);
  return out;
}

namespace {
bool expandLimited(std::string_view s, const SkinDefines& scope, size_t limit, std::string& out) {
  out.clear();
  out.reserve(std::min(s.size(), limit));
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '$' || i + 1 >= s.size()) {
      out.push_back(s[i]);
      continue;
    }
    std::string_view key;
    size_t end;
    if (s[i + 1] == '(') {
      end = s.find(')', i + 2);
      if (end == std::string_view::npos) {
        out.append(s.substr(i));
        break;
      }
      key = s.substr(i + 2, end - i - 2);
    } else if (s[i + 1] == '$') {
      out.push_back('$');
      ++i;
      continue;
    } else {
      end = i + 1;
      while (end < s.size() && isIdent(s[end])) ++end;
      if (end == i + 1) {
        out.push_back('$');
        continue;
      }
      key = s.substr(i + 1, end - i - 1);
      --end;
    }
    if (auto it = scope.find(key); it != scope.end()) {
      if (limit != std::string::npos && it->second.size() > limit - std::min(limit, out.size())) return false;
      out += it->second;
    }
    i = end;
    if (out.size() > limit) return false;
  }
  return out.size() <= limit;
}
}  // namespace

bool Skin::parseColor(std::string_view s, Color& out) {
  if (s.size() < 2 || s[0] != '#') return false;
  std::string hex(s.substr(1));
  for (char ch : hex)
    if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
  unsigned long v = std::strtoul(hex.c_str(), nullptr, 16);
  if (hex.size() == 3) {
    unsigned r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
    out = Color::rgb((r * 17) << 16 | (g * 17) << 8 | (b * 17));
  } else if (hex.size() == 6) out = Color::rgb(static_cast<unsigned>(v));
  else if (hex.size() == 8) out = Color::rgba(static_cast<uint32_t>(v));
  else return false;
  return true;
}

std::unique_ptr<Skin> Skin::parse(std::string_view xml, std::string* error) {
  std::string err;
  auto doc = xmlParse(xml, &err);
  if (!doc) {
    if (error) *error = "skin XML: " + err;
    return nullptr;
  }
  XmlNode root = xmlRoot(doc);
  if (root.name() != "Skin") {
    if (error) *error = "skin root element must be <Skin>";
    return nullptr;
  }
  std::unique_ptr<Skin> s(new Skin());
  s->name_ = root.attr("name");
  for (XmlNode d : root.children("Defines"))
    for (XmlNode e : d.children("define")) s->defines_[e.attr("name")] = e.attr("value");
  for (XmlNode fs : root.children("Forms"))
    for (XmlNode f : fs.children("Form")) {
      std::string n = f.attr("name");
      if (n.empty() || s->impl_->forms.count(n)) {
        if (error) *error = n.empty() ? "a <Form> has no name" : "duplicate form '" + n + "'";
        return nullptr;
      }
      s->impl_->forms[n] = f.raw();
    }
  for (XmlNode rp : root.children("RemotePages"))
    for (XmlNode pg : rp.children("Page")) {
      RemotePageDecl d;
      for (uint32_t i = 0; i < sq_xml_attr_count(pg.raw()); ++i) {
        std::string k = sq_xml_attr_name(pg.raw(), i);
        std::string v = sq_xml_attr_value(pg.raw(), i);
        if (k == "name") d.name = std::move(v);
        else if (k == "form") d.form = std::move(v);
        else d.defines[k] = std::move(v);
      }
      if (d.form.empty()) {
        if (error) *error = "a remote <Page> has no form";
        return nullptr;
      }
      if (d.name.empty()) d.name = d.form;
      s->remotePages_.push_back(std::move(d));
    }
  s->impl_->doc = std::move(doc);
  return s;
}

std::vector<std::string> Skin::formNames() const {
  std::vector<std::string> v;
  for (const auto& [k, _] : impl_->forms) v.push_back(k);
  return v;
}

bool Skin::hasForm(std::string_view form) const { return impl_->forms.find(form) != impl_->forms.end(); }

void Skin::registerWidget(const std::string& element, WidgetFactory factory) { impl_->custom[element] = std::move(factory); }

std::unique_ptr<Widget> Skin::build(std::string_view form, const SkinDefines& defines, std::string* error) const {
  SkinDefines scope = defines_;
  for (const auto& [k, v] : defines) scope[k] = v;
  std::string err;
  std::unique_ptr<Widget> w;
  try {
    Builder b{*impl_, &err};
    w = b.form_(form, scope);
  } catch (const std::bad_alloc&) {
    w.reset();
    err = "skin build ran out of memory";
  }
  if (!w && error) *error = err.empty() ? "skin build failed" : err;
  return w;
}

// ---- static validation ------------------------------------------------------------------------

namespace {

struct Validator {
  std::vector<SkinIssue>& issues;
  const SkinValidateOptions& opt;
  std::set<std::string, std::less<>> forms;
  std::set<std::string> assets;
  // form -> (included form, guarded by a conditional)
  std::map<std::string, std::vector<std::pair<std::string, bool>>, std::less<>> edges;
  Skin::Impl scratch;
  size_t nodes = 0;
  bool truncated = false;

  void issue(bool err, const std::string& where, std::string msg) { issues.push_back(SkinIssue{err, where, std::move(msg)}); }

  static bool literal(std::string_view v) { return v.find('$') == std::string_view::npos; }

  static std::string label(XmlNode n) {
    std::string l = "<" + std::string(n.name());
    if (n.hasAttr("name")) l += " name=\"" + n.attr("name") + "\"";
    return l + ">";
  }

  bool custom(std::string_view el) const {
    return std::find(opt.customElements.begin(), opt.customElements.end(), el) != opt.customElements.end();
  }
  static bool builtin(std::string_view el) {
    return el == "View" || el == "Label" || el == "Knob" || el == "Fader" || el == "Button" || el == "Meter" || el == "Image";
  }

  bool budget() {
    if (++nodes <= kMaxSkinNodes) return true;
    if (!truncated) issue(true, "skin", "skin has more than " + std::to_string(kMaxSkinNodes) + " nodes; validation stopped");
    truncated = true;
    return false;
  }

  void checkAttrs(XmlNode n, std::string_view asElement, const std::string& where, bool isForm) {
    std::string err;
    Builder b{scratch, &err};
    std::unique_ptr<Widget> w = b.create(asElement);
    if (!w) return;
    for (uint32_t i = 0; i < sq_xml_attr_count(n.raw()); ++i) {
      std::string k = sq_xml_attr_name(n.raw(), i);
      std::string v = sq_xml_attr_value(n.raw(), i);
      if (isForm && k == "name") continue;
      err.clear();
      if (literal(v)) {
        if (!b.applyAttr(*w, k, v)) issue(true, where, err);
      } else if (!b.applyAttr(*w, k, std::string()) && err.rfind("unknown attribute", 0) == 0) {
        issue(true, where, err);
      }
      if (k == "image" && literal(v) && !v.empty()) {
        if (v.rfind("theme://", 0) == 0) {
          assets.insert(v);
          if (opt.assetExists && !opt.assetExists(v)) issue(true, where, "missing theme asset '" + v + "'");
        } else {
          issue(false, where, "image '" + v + "' is not a theme:// URL");
        }
      }
      if (k == "remote" && literal(v) && (v.empty() || v.size() >= 64)) issue(true, where, "remote id must be 1..63 bytes");
    }
  }

  // The walk is iterative (explicit work list): skins are untrusted and a recursive walk 256 XML
  // levels deep overflows a 1 MB stack in debug builds.
  struct Work {
    XmlNode node;
    std::string where;  // the parent's location
    int cond;           // number of enclosing conditionals
  };

  void children(XmlNode n, const std::string& form, const std::string& where, int cond) {
    std::vector<Work> todo, next;
    for (XmlNode c : n.children()) next.push_back(Work{c, where, cond});
    todo.insert(todo.end(), std::make_move_iterator(next.rbegin()), std::make_move_iterator(next.rend()));
    while (!todo.empty() && !truncated) {
      Work w = std::move(todo.back());
      todo.pop_back();
      next.clear();
      one(w.node, form, w.where, w.cond, next);
      todo.insert(todo.end(), std::make_move_iterator(next.rbegin()), std::make_move_iterator(next.rend()));
    }
  }

  // Checks one node; the nodes to visit below it are appended to `next` in document order.
  void one(XmlNode c, const std::string& form, const std::string& parentWhere, int cond, std::vector<Work>& next) {
    if (!budget()) return;
    std::string_view el = c.name();
    std::string where = parentWhere + " > " + label(c);
    auto pushChildren = [&](XmlNode k, const std::string& at, int cnd) {
      for (XmlNode g : k.children()) next.push_back(Work{g, at, cnd});
    };
    if (el == "define") {
      if (c.attr("name").empty()) issue(true, where, "<define> needs a name");
      for (uint32_t i = 0; i < sq_xml_attr_count(c.raw()); ++i) {
        std::string k = sq_xml_attr_name(c.raw(), i);
        if (k != "name" && k != "value") issue(false, where, "<define> ignores attribute '" + k + "'");
      }
      if (c.child()) issue(false, where, "<define> children are ignored");
      return;
    }
    if (el == "if") {
      int conds = (c.hasAttr("defined") ? 1 : 0) + (c.hasAttr("undefined") ? 1 : 0) + (c.hasAttr("equals") ? 1 : 0) +
                  (c.hasAttr("notequals") ? 1 : 0);
      if (conds != 1) issue(true, where, "<if> needs exactly one of defined= undefined= equals= notequals=");
      if ((c.hasAttr("equals") || c.hasAttr("notequals")) && !c.hasAttr("value")) issue(true, where, "<if equals/notequals> needs value=");
      int elses = 0;
      for (XmlNode k : c.children()) {
        if (truncated) return;
        if (k.name() == "else") {
          if (++elses > 1) issue(true, where, "<if> has more than one <else>");
          if (!budget()) return;
          pushChildren(k, where + " > <else>", cond + 1);
        } else {
          next.push_back(Work{k, where, cond + 1});
        }
      }
      return;
    }
    if (el == "switch") {
      if (!c.hasAttr("value")) issue(true, where, "<switch> needs value=");
      int defaults = 0;
      std::set<std::string> seen;
      for (XmlNode k : c.children()) {
        if (truncated || !budget()) return;
        std::string kw = where + " > " + label(k);
        if (k.name() == "case") {
          if (!k.hasAttr("value")) issue(true, kw, "<case> needs value=");
          else if (literal(k.attr("value")) && !seen.insert(k.attr("value")).second)
            issue(false, kw, "duplicate <case value=\"" + k.attr("value") + "\"> is never chosen");
          pushChildren(k, kw, cond + 1);
        } else if (k.name() == "default") {
          if (++defaults > 1) issue(true, kw, "<switch> has more than one <default>");
          pushChildren(k, kw, cond + 1);
        } else {
          issue(true, kw, "<switch> may only contain <case> and <default>");
        }
      }
      return;
    }
    if (el == "else" || el == "case" || el == "default") {
      issue(true, where, "<" + std::string(el) + "> outside its <" + (el == "else" ? "if" : "switch") + ">");
      return;
    }
    if (el == "Include") {
      std::string target = c.attr("form");
      if (target.empty()) {
        issue(true, where, "<Include> needs form=");
      } else if (literal(target)) {
        if (!forms.count(target)) issue(true, where, "<Include> of unknown form '" + target + "'");
        else edges[form].emplace_back(target, cond > 0);
      }
      if (c.child()) issue(false, where, "<Include> children are ignored");
      return;
    }
    if (builtin(el)) {
      checkAttrs(c, el, where, false);
    } else if (!custom(el)) {
      issue(true, where, "unknown skin element <" + std::string(el) + ">");
      return;
    }
    pushChildren(c, where, cond);
  }

  // Cycles in the include graph; withConditional=false only follows unconditional includes.
  // Iterative DFS: a skin may hold tens of thousands of forms in one include chain. Also records,
  // for the unconditional graph, the longest include chain below every form (cycle edges count 0).
  void cycles(bool withConditional, std::set<std::string>& reported, std::map<std::string, size_t, std::less<>>* chainLen = nullptr) {
    std::map<std::string, int, std::less<>> state;  // 0 new, 1 on stack, 2 done
    std::vector<std::string> stack;                 // the DFS path (for cycle text)
    struct Frame {
      std::string form;
      size_t next = 0;
      size_t longest = 1;  // forms in the longest chain starting here
    };
    std::vector<Frame> frames;
    static const std::vector<std::pair<std::string, bool>> kNone;
    auto out = [&](const std::string& f) -> const std::vector<std::pair<std::string, bool>>& {
      auto it = edges.find(f);
      return it == edges.end() ? kNone : it->second;
    };
    for (const auto& root : forms) {
      if (state[root] != 0) continue;
      state[root] = 1;
      stack.push_back(root);
      frames.push_back(Frame{root});
      while (!frames.empty()) {
        Frame& fr = frames.back();
        const auto& es = out(fr.form);
        if (fr.next < es.size()) {
          const auto& [to, conditional] = es[fr.next++];
          if (conditional && !withConditional) continue;
          int st = state[to];
          if (st == 1) {
            auto at = std::find(stack.begin(), stack.end(), to);
            std::string chain;
            for (auto i = at; i != stack.end(); ++i) chain += *i + " -> ";
            chain += to;
            std::vector<std::string> members(at, stack.end());
            std::sort(members.begin(), members.end());
            std::string key;
            for (const auto& m : members) key += m + "|";
            if (reported.insert(key).second)
              issue(!withConditional, "form '" + to + "'",
                    withConditional ? "include cycle guarded by a conditional: " + chain + " (fails unless the guard stops it)"
                                    : "unconditional include cycle: " + chain);
          } else if (st == 2) {
            if (chainLen) fr.longest = std::max(fr.longest, (*chainLen)[to] + 1);
          } else {
            state[to] = 1;
            stack.push_back(to);
            frames.push_back(Frame{to});
          }
          continue;
        }
        const std::string done = fr.form;
        const size_t len = fr.longest;
        if (chainLen) (*chainLen)[done] = len;
        state[done] = 2;
        stack.pop_back();
        frames.pop_back();
        if (!frames.empty()) frames.back().longest = std::max(frames.back().longest, len + 1);
      }
    }
  }
};

}  // namespace

bool Skin::validate(std::string_view xml, std::vector<SkinIssue>& issues, const SkinValidateOptions& options,
                    std::vector<std::string>* assets) {
  const size_t before = issues.size();
  try {
    std::string err;
    auto skin = Skin::parse(xml, &err);
    if (!skin) {
      issues.push_back(SkinIssue{true, "skin", err});
      return false;
    }
    Validator v{issues, options, {}, {}, {}, {}, 0, false};
    for (const auto& f : skin->formNames()) v.forms.insert(f);
    if (v.forms.empty()) issues.push_back(SkinIssue{false, "skin", "skin has no forms"});
    XmlNode root = xmlRoot(skin->impl_->doc);
    for (XmlNode c : root.children()) {
      std::string_view el = c.name();
      if (el == "Defines") {
        for (XmlNode d : c.children()) {
          if (d.name() != "define") issues.push_back(SkinIssue{true, "<Defines>", "<Defines> may only contain <define>"});
          else if (d.attr("name").empty()) issues.push_back(SkinIssue{true, "<Defines>", "<define> needs a name"});
        }
      } else if (el == "Forms") {
        for (XmlNode f : c.children()) {
          if (v.truncated) break;
          if (f.name() != "Form") {
            issues.push_back(SkinIssue{true, "<Forms>", "<Forms> may only contain <Form>, found <" + std::string(f.name()) + ">"});
            continue;
          }
          std::string name = f.attr("name");
          std::string where = "form '" + name + "'";
          v.checkAttrs(f, "View", where, true);
          v.children(f, name, where, 0);
        }
      } else if (el == "RemotePages") {
        std::set<std::string> names;
        for (XmlNode p : c.children()) {
          std::string where = "<RemotePages> > " + Validator::label(p);
          if (p.name() != "Page") {
            issues.push_back(SkinIssue{true, where, "<RemotePages> may only contain <Page>"});
            continue;
          }
          std::string form = p.attr("form");
          if (Validator::literal(form) && !v.forms.count(form))
            issues.push_back(SkinIssue{true, where, "remote page uses unknown form '" + form + "'"});
          std::string name = p.attr("name", form);
          if (!names.insert(name).second) issues.push_back(SkinIssue{true, where, "duplicate remote page name '" + name + "'"});
        }
      } else {
        issues.push_back(SkinIssue{false, "skin", "unknown top-level element <" + std::string(el) + "> is ignored"});
      }
    }
    std::set<std::string> reported;
    std::map<std::string, size_t, std::less<>> chainLen;
    v.cycles(false, reported, &chainLen);
    v.cycles(true, reported);
    // Skin::build refuses includes nested deeper than 16 forms; say so for the deepest chain.
    const std::string* deepest = nullptr;
    size_t deepestLen = 0;
    for (const auto& [f, len] : chainLen)
      if (len > deepestLen) {
        deepest = &f;
        deepestLen = len;
      }
    if (deepest && deepestLen > 16)
      issues.push_back(SkinIssue{false, "form '" + *deepest + "'",
                                 "unconditional include chain is " + std::to_string(deepestLen) +
                                     " forms deep; building it fails (includes nest no deeper than 16)"});
    if (assets) assets->assign(v.assets.begin(), v.assets.end());
  } catch (const std::bad_alloc&) {
    issues.push_back(SkinIssue{true, "skin", "validation ran out of memory"});
  }
  for (size_t i = before; i < issues.size(); ++i)
    if (issues[i].error) return false;
  return true;
}

}  // namespace sq::ui
