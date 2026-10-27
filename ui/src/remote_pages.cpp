// Remote pages from skin XML and the ui/ host services. See remote_pages.hpp.
#include "sq/ui/remote_pages.hpp"

#include <cstring>
#include <set>

#include "sq/platform.hpp"

namespace sq::ui {

namespace {

bool fail(std::string* e, std::string msg) {
  if (e) *e = std::move(msg);
  return false;
}

// Copies into a fixed char field; false if it does not fit.
template <size_t N>
bool putField(char (&dst)[N], const std::string& s) {
  std::memset(dst, 0, N);
  if (s.size() >= N) return false;
  std::memcpy(dst, s.data(), s.size());
  return true;
}
// Truncating copy on a UTF-8 boundary.
template <size_t N>
void putLabel(char (&dst)[N], const std::string& s) {
  std::memset(dst, 0, N);
  size_t n = s.size() < N - 1 ? s.size() : N - 1;
  while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  std::memcpy(dst, s.data(), n);
}

int typeFromName(const std::string& t) {
  if (t == "knob") return SQ_REMOTE_KNOB;
  if (t == "encoder") return SQ_REMOTE_ENCODER;
  if (t == "fader") return SQ_REMOTE_FADER;
  if (t == "button") return SQ_REMOTE_BUTTON;
  if (t == "pad") return SQ_REMOTE_PAD;
  if (t == "display") return SQ_REMOTE_DISPLAY;
  if (t == "led") return SQ_REMOTE_LED;
  return 0;
}

int typeFromWidget(const Widget& w) {
  const std::string& t = w.type();
  if (t == "Knob") return SQ_REMOTE_KNOB;
  if (t == "Fader") return SQ_REMOTE_FADER;
  if (t == "Button") return SQ_REMOTE_BUTTON;
  if (t == "Meter") return SQ_REMOTE_LED;
  if (t == "Label" || t == "Image") return SQ_REMOTE_DISPLAY;
  return 0;
}

bool collect(Widget& root, const std::string& context, RemotePageSpec& page, std::string* error) {
  std::set<std::string> ids;
  bool ok = true;
  root.forEach([&](Widget& w) {
    if (!ok || w.remoteId.empty()) return;
    sq_remote_control c{};
    c.struct_size = sizeof c;
    c.type = w.remoteType.empty() ? typeFromWidget(w) : typeFromName(w.remoteType);
    if (c.type == 0) {
      ok = fail(error, "control '" + w.remoteId + "' (<" + w.type() + ">) needs remoteType=");
      return;
    }
    if (!ids.insert(w.remoteId).second) {
      ok = fail(error, "duplicate remote control id '" + w.remoteId + "'");
      return;
    }
    const auto* button = dynamic_cast<const Button*>(&w);
    if (button && button->toggles) c.flags |= SQ_REMOTE_TOGGLE;
    if (w.remoteFeedback || c.type == SQ_REMOTE_DISPLAY || c.type == SQ_REMOTE_LED) c.flags |= SQ_REMOTE_FEEDBACK;
    std::string target = w.target.url, property = w.target.property;
    if (target.empty() && !w.command.empty()) {
      target = "command://" + w.command;
      property = "invoke";
    } else if (!target.empty() && target.find("://") == std::string::npos && !context.empty()) {
      target = context + (context.back() == '/' ? "" : "/") + target;
    }
    if (target.empty()) property.clear();
    if (!putField(c.id, w.remoteId) || !putField(c.target, target) || !putField(c.property, property)) {
      ok = fail(error, "control '" + w.remoteId + "': id, target or property too long");
      return;
    }
    putLabel(c.label, !w.text.empty() ? w.text : !w.name.empty() ? w.name : w.remoteId);
    page.controls.push_back(c);
  });
  return ok;
}

class RemotePage final : public sq::Object<RemotePage, IRemotePage> {
 public:
  explicit RemotePage(RemotePageSpec spec) : spec_(std::move(spec)) {}
  sq_result SQ_CALL getName(char* buf, uint32_t bufSize, uint32_t* needed) override {
    const auto need = static_cast<uint32_t>(spec_.name.size() + 1);
    if (needed) *needed = need;
    if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, spec_.name.c_str(), need);
    return SQ_OK;
  }
  uint32_t SQ_CALL getControlCount() override { return static_cast<uint32_t>(spec_.controls.size()); }
  sq_result SQ_CALL getControl(uint32_t index, sq_remote_control* out) override {
    if (!out) return SQ_E_INVALIDARG;
    if (index >= spec_.controls.size()) return SQ_E_INVALIDARG;
    if (out->struct_size != 0 && out->struct_size < sizeof *out) return SQ_E_INVALIDARG;
    *out = spec_.controls[index];
    return SQ_OK;
  }

 private:
  RemotePageSpec spec_;
};

class RemotePageSet final : public sq::Object<RemotePageSet, IRemotePageSet> {
 public:
  explicit RemotePageSet(std::vector<RemotePageSpec> pages) {
    for (auto& p : pages) pages_.push_back(sq::Ptr<IRemotePage>::adopt(new RemotePage(std::move(p))));
  }
  uint32_t SQ_CALL getPageCount() override { return static_cast<uint32_t>(pages_.size()); }
  sq_result SQ_CALL getPage(uint32_t index, IRemotePage** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (index >= pages_.size()) return SQ_E_INVALIDARG;
    pages_[index]->addRef();
    *out = pages_[index].get();
    return SQ_OK;
  }

 private:
  std::vector<sq::Ptr<IRemotePage>> pages_;
};

}  // namespace

bool buildRemotePages(const Skin& skin, const SkinDefines& extraDefines, const std::string& context, std::vector<RemotePageSpec>& out,
                      std::string* error) {
  out.clear();
  try {
    std::vector<RemotePageDecl> decls = skin.remotePages();
    const bool implicit = decls.empty();
    if (implicit)
      for (const auto& f : skin.formNames()) decls.push_back(RemotePageDecl{f, f, {}});
    for (const auto& d : decls) {
      SkinDefines defs = extraDefines;
      for (const auto& [k, v] : d.defines) defs[k] = Skin::expand(v, extraDefines);
      defs["context"] = context;
      std::string berr;
      auto root = skin.build(d.form, defs, &berr);
      if (!root) {
        if (implicit) continue;  // helper forms that only work when included
        return fail(error, "remote page '" + d.name + "': " + berr);
      }
      RemotePageSpec page;
      page.name = d.name;
      std::string cerr;
      if (!collect(*root, context, page, &cerr)) return fail(error, "remote page '" + d.name + "': " + cerr);
      if (implicit && page.controls.empty()) continue;
      out.push_back(std::move(page));
    }
    if (out.empty()) return fail(error, "the skin defines no remote controls");
    return true;
  } catch (const std::bad_alloc&) {
    return fail(error, "out of memory building remote pages");
  }
}

sq::Ptr<IRemotePageSet> makeRemotePageSet(std::vector<RemotePageSpec> pages) {
  return sq::Ptr<IRemotePageSet>::adopt(new RemotePageSet(std::move(pages)));
}

sq_result RemotePageProvider::load(const char* skinUrl, const char* context, IRemotePageSet** out, std::string* error) {
  if (!skinUrl || !out) return SQ_E_INVALIDARG;
  *out = nullptr;
  try {
    std::string url(skinUrl), xml;
    SkinDefines extra;
    if (url.rfind("theme://", 0) == 0) {
      if (!themes_) { fail(error, "no theme resolver for " + url); return SQ_E_NOTFOUND; }
      auto bytes = themes_->read(url);
      if (!bytes) { fail(error, "no theme resource " + url); return SQ_E_NOTFOUND; }
      xml = std::move(*bytes);
    } else {
      auto bytes = readFile(url);
      if (!bytes) { fail(error, "cannot read " + url); return SQ_E_NOTFOUND; }
      xml = std::move(*bytes);
    }
    if (themes_) extra = themes_->skinDefines();
    std::string perr;
    auto skin = Skin::parse(xml, &perr);
    if (!skin) { fail(error, perr); return SQ_E_INVALIDARG; }
    std::vector<RemotePageSpec> pages;
    if (!buildRemotePages(*skin, extra, context ? context : "", pages, error)) return SQ_E_INVALIDARG;
    *out = makeRemotePageSet(std::move(pages)).detach();
    return SQ_OK;
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL RemotePageProvider::loadRemotePages(const char* skinUrl, const char* context, IRemotePageSet** out) {
  return load(skinUrl, context, out, nullptr);
}

// ---- host services -------------------------------------------------------------------------------

UiServices::UiServices(IHostContext* host, sq::Ptr<ThemeResolver> themes) : host_(host), themes_(std::move(themes)) {
  if (!themes_) themes_ = ThemeResolver::create();
  pages_ = sq::Ptr<IRemotePageProvider>::adopt(new RemotePageProvider(themes_));
  if (!host_) return;
  themesRegistered_ = SQ_SUCCEEDED(host_->registerService(&IThemeResolver::iid, static_cast<IThemeResolver*>(themes_.get())));
  pagesRegistered_ = SQ_SUCCEEDED(host_->registerService(&IRemotePageProvider::iid, pages_.get()));
  if (SQ_SUCCEEDED(host_->queryService(&IObjectRegistry::iid, objects_.putVoid())) && objects_) {
    auto r = themes_->urlResolver();
    if (SQ_SUCCEEDED(objects_->registerResolver("theme://", r.get()))) urlResolver_ = r;
  }
}

UiServices::~UiServices() { unregisterAll(); }

void UiServices::unregisterAll() {
  if (objects_ && urlResolver_) objects_->unregisterResolver("theme://", urlResolver_.get());
  urlResolver_.reset();
  objects_.reset();
  if (host_ && themesRegistered_) host_->unregisterService(&IThemeResolver::iid, static_cast<IThemeResolver*>(themes_.get()));
  if (host_ && pagesRegistered_) host_->unregisterService(&IRemotePageProvider::iid, pages_.get());
  themesRegistered_ = pagesRegistered_ = false;
}

}  // namespace sq::ui
