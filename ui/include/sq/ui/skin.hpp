// Squizzy Studio UI - declarative skin XML -> widget trees (backend-neutral, UI thread only).
//
//   <Skin name="default" format="1">
//     <Defines> <define name="accent" value="#4FB3A9"/> </Defines>
//     <Forms>
//       <Form name="Main" direction="column"> ...widgets... </Form>
//       <Form name="Strip" width="84"> <Label text="$(trackName)"/> ... </Form>
//     </Forms>
//   </Skin>
//
// Widgets: View, Label, Knob, Fader, Button, Meter, Image, plus host-registered custom types
// (Skin::registerWidget). Common attributes: name text tooltip remote command target property
// width height minWidth minHeight maxWidth maxHeight (number = DIP, "N%" = percent) grow shrink
// basis direction(row|column|row-reverse|column-reverse) justify(start|end|center|between|around|
// evenly) align alignSelf(start|end|center|stretch) wrap padding margin gap position(absolute)
// left top right bottom display(none) background border borderWidth radius color accent trackColor
// thumbColor fontSize textAlign image opacity enabled min max default value toggles horizontal bipolar.
// Colors: #RGB, #RRGGBB, #RRGGBBAA. Edges (padding/margin): "a" or "v h" or "t r b l".
//
// Preprocessor (evaluated at instantiation, so the same form serves many tracks/pages):
//   $(name) / $name     substituted from the define scope (unknown names expand to "")
//   <define name= value=/>                         adds to the current scope (any level)
//   <if defined="x"> / <if undefined="x"> / <if equals="$(a)" value="b"> / <if notequals=.. value=..>
//      ... <else> ... </else> </if>
//   <switch value="$(kind)"> <case value="audio">..</case> <default>..</default> </switch>
//   <Include form="Strip" trackName="Bass"/>       instantiates another form; extra attrs = defines
// target="object://..." + property="volume" binds a widget's value to an IController property
// (the host does the binding). command="Transport.Play" names the command a button fires.
//
// Hardware remote pages use the same documents (remote_pages.hpp): widgets with remote="id" become
// surface controls (remoteType= knob|encoder|fader|button|pad|display|led overrides the type the
// widget implies, feedback="1" marks controls that can show the value). Optional page list:
//   <RemotePages> <Page name="Mixer" form="Mixer" withSolo="1"/> </RemotePages>   extra attrs = defines
// Theme values are plain defines when the host passes ThemeResolver::skinDefines():
//   $(theme.color.Window.Background), $(theme.metric.Track.Height), $(theme.name)
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sq/ui/widgets.hpp"

namespace sq::ui {

using SkinDefines = std::map<std::string, std::string, std::less<>>;

// One finding of Skin::validate.
struct SkinIssue {
  bool error = true;     // false = warning
  std::string where;     // "form 'Strip' > View > Knob[name=pan]" style path
  std::string message;
};

struct SkinValidateOptions {
  // Extra element names the host registers with Skin::registerWidget (accepted with any attribute).
  std::vector<std::string> customElements;
  // Called for every literal (no $) theme:// URL in an image= attribute; false = missing asset.
  std::function<bool(const std::string& url)> assetExists;
};

// A <RemotePages><Page .../></RemotePages> entry.
struct RemotePageDecl {
  std::string name;
  std::string form;
  SkinDefines defines;  // the Page's other attributes
};

class Skin {
 public:
  using WidgetFactory = std::function<std::unique_ptr<Widget>()>;

  ~Skin();
  // Parses a skin document. nullptr + error on malformed XML or schema errors.
  static std::unique_ptr<Skin> parse(std::string_view xml, std::string* error = nullptr);

  const std::string& name() const { return name_; }
  std::vector<std::string> formNames() const;
  bool hasForm(std::string_view form) const;
  const SkinDefines& defines() const { return defines_; }
  const std::vector<RemotePageDecl>& remotePages() const { return remotePages_; }

  // Custom element types (e.g. the host's arrange view). The created widget then receives the
  // common attributes like any built-in.
  void registerWidget(const std::string& element, WidgetFactory factory);

  // Instantiates `form` with the skin defines overlaid by `defines`. nullptr + error if the form
  // is unknown, an element is unknown, or includes recurse deeper than 16 levels.
  std::unique_ptr<Widget> build(std::string_view form, const SkinDefines& defines = {}, std::string* error = nullptr) const;

  // Substitutes $(name)/$name in `s` from `scope`.
  static std::string expand(std::string_view s, const SkinDefines& scope);
  // "#RRGGBB" etc. Returns false on malformed input.
  static bool parseColor(std::string_view s, Color& out);

  // Static check of a whole skin document, independent of define values: every branch of every
  // <if>/<switch> is visited. Checks elements, attribute names, literal attribute values,
  // <if>/<switch>/<define>/<Include> syntax, unknown include targets, include cycles (error when
  // unconditional, warning when guarded by a conditional), remote ids, <RemotePages> and literal
  // theme:// image URLs (through options.assetExists). Returns true when there is no error.
  // `assets` (optional) receives every literal theme:// URL referenced, sorted, unique.
  static bool validate(std::string_view xml, std::vector<SkinIssue>& issues, const SkinValidateOptions& options = {},
                       std::vector<std::string>* assets = nullptr);

  struct Impl;  // internal

 private:
  Skin();
  std::unique_ptr<Impl> impl_;
  std::string name_;
  SkinDefines defines_;
  std::vector<RemotePageDecl> remotePages_;
};

}  // namespace sq::ui
