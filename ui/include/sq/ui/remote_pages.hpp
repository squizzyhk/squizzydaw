// Squizzy Studio UI - hardware remote pages generated from skin XML (plan 2.5: the same skin
// language drives on-screen and hardware UIs), plus the module-facing UI services.
//
// A page is one instantiated form; every widget with remote="id" becomes one sq_remote_control:
//   type     remoteType= if given, else by element: Knob -> KNOB, Fader -> FADER,
//            Button -> BUTTON, Meter -> LED, Label/Image -> DISPLAY (View/custom: remoteType required)
//   flags    TOGGLE for toggles="1" buttons; FEEDBACK for feedback="1" and every DISPLAY/LED
//   label    text=, else name=, else the id
//   target   target= ; relative targets (no "://") are joined to the context URL when one is given;
//            a button with only command="X.Y" gets target "command://X.Y" and property "invoke"
//   property property= (default "value")
// Pages: the skin's <RemotePages><Page name= form= .../></RemotePages> in order (extra attributes
// become defines), or, without that list, every form that yields at least one control, by name.
// Each page build sees: skin defines < extra defines (e.g. theme) < page defines < context=<url>.
// Control ids must be unique within a page; id/target/property must fit their fixed fields
// (63/255/63 bytes) - labels are truncated on a UTF-8 boundary.
#pragma once

#include <string>
#include <vector>

#include "sq/cpp/object.hpp"
#include "sq/cpp/ptr.hpp"
#include "sq/interfaces/ui.h"
#include "sq/module.h"
#include "sq/ui/skin.hpp"
#include "sq/ui/theme.hpp"

namespace sq::ui {

struct RemotePageSpec {
  std::string name;
  std::vector<sq_remote_control> controls;
};

bool buildRemotePages(const Skin& skin, const SkinDefines& extraDefines, const std::string& context, std::vector<RemotePageSpec>& out,
                      std::string* error = nullptr);

// Immutable COM snapshot of `pages`.
sq::Ptr<IRemotePageSet> makeRemotePageSet(std::vector<RemotePageSpec> pages);

// IRemotePageProvider: skinUrl is a theme:// URL (needs `themes`) or a file path. The active
// theme's skinDefines() are visible to the skin when `themes` is set.
class RemotePageProvider final : public sq::Object<RemotePageProvider, IRemotePageProvider> {
 public:
  explicit RemotePageProvider(sq::Ptr<ThemeResolver> themes = {}) : themes_(std::move(themes)) {}
  sq_result SQ_CALL loadRemotePages(const char* skinUrl, const char* context, IRemotePageSet** out) override;
  // Same, with the error text.
  sq_result load(const char* skinUrl, const char* context, IRemotePageSet** out, std::string* error);

 private:
  sq::Ptr<ThemeResolver> themes_;
};

// Registers the ui/ services with a host (ui.h): IThemeResolver and IRemotePageProvider through
// registerService, and "theme://" with IObjectRegistry when scripting is loaded. Everything is
// unregistered by the destructor (or unregisterAll). Main thread.
class UiServices {
 public:
  UiServices(IHostContext* host, sq::Ptr<ThemeResolver> themes);
  ~UiServices();
  UiServices(const UiServices&) = delete;
  UiServices& operator=(const UiServices&) = delete;
  void unregisterAll();
  ThemeResolver* themes() const { return themes_.get(); }
  IRemotePageProvider* remotePages() const { return pages_.get(); }
  bool themeUrlsRegistered() const { return static_cast<bool>(urlResolver_); }

 private:
  IHostContext* host_;
  sq::Ptr<ThemeResolver> themes_;
  sq::Ptr<IRemotePageProvider> pages_;
  sq::Ptr<IObjectRegistry> objects_;
  sq::Ptr<IUrlResolver> urlResolver_;
  bool themesRegistered_ = false, pagesRegistered_ = false;
};

}  // namespace sq::ui
