// Squizzy Studio UI - themes: theme:// resolution, colors/metrics, @2x asset selection and the
// binary theme pack the skin compiler (tools/skin-compiler, sq-skinc) produces.
//
// A theme is a directory (or a pack prefix) "<name>/" holding theme.xml plus any assets:
//   <Theme name="dark" format="1" inherits="base" title="Squizzy Dark">
//     <Colors>  <Color name="Track.Selected" value="#3A3F4A"/> <Color name="Cursor" value="@Accent"/> </Colors>
//     <Metrics> <Metric name="Track.Height" value="46"/> </Metrics>
//   </Theme>
// Colors are #RGB/#RRGGBB/#RRGGBBAA or "@OtherName" (resolved against the ACTIVE theme chain, so a
// child overriding "Accent" also changes every alias of it in the parent). Lookups (files, colors,
// metrics) walk the inherits chain, child first.
//
// URLs: theme://<theme>/<path> or theme://current/<path>, optional "?scale=N" (IUrlResolver only).
// Image variants are files named <stem>@<N>x.<ext> next to <stem>.<ext>; <path> never names the
// suffix. For a requested scale the resolver picks the smallest density >= scale, else the largest
// available (so 1.25 -> @2x, 4 -> @3x, 1 -> plain).
//
// Thread safety: ThemeResolver is thread-safe (shared lock for reads). setActiveTheme notifies
// ISubject observers ("theme.changed", args theme=<new> previous=<old>) on the calling thread,
// outside the lock; call it on the main thread (ui.h).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "sq/cpp/object.hpp"
#include "sq/cpp/ptr.hpp"
#include "sq/interfaces/scripting.h"
#include "sq/interfaces/ui.h"
#include "sq/ui/image.hpp"
#include "sq/ui/skin.hpp"

namespace sq::ui {

// ---- theme packs ----------------------------------------------------------------------------------
// "SQTHMPK1" container, little-endian:
//   0   char[8] magic "SQTHMPK1"
//   8   u32 version (1)          12  u32 entry count
//   16  u64 data offset          24  u64 total size (== blob size)
//   32  entry[count], 32 bytes:  u64 data offset (relative to the data offset), u64 size,
//                                u32 path offset (absolute), u32 path length, u32 CRC-32 of the data,
//                                u32 kind (SQ_THEME_RES_*)
//   string table: the paths, each followed by NUL; zero padding to a multiple of 8
//   data: every entry 8-aligned, zero padded
// Entries are sorted bytewise by path ("<theme>/<relative path>", '/' separators, no "..").
// The writer is deterministic: the same set of entries yields the same bytes in any input order.
inline constexpr char kThemePackMagic[8] = {'S', 'Q', 'T', 'H', 'M', 'P', 'K', '1'};
inline constexpr const char* kThemePackResourceName = "SQTHEMES";

struct ThemePackEntry {
  std::string path;
  std::string data;
};
bool writeThemePack(std::vector<ThemePackEntry> entries, std::string& out, std::string* error = nullptr);

class ThemePack {
 public:
  // Validates the whole blob (bounds, order, CRCs). copy=false references `data`, which must
  // outlive the pack (resource sections). nullptr + error on any defect.
  static std::shared_ptr<const ThemePack> fromMemory(const void* data, size_t size, bool copy, std::string* error = nullptr);
  static std::shared_ptr<const ThemePack> fromFile(const std::string& path, std::string* error = nullptr);
  // Windows: RCDATA resource `name` of `module` (HMODULE; nullptr = the running EXE).
  // Elsewhere: nullptr + error.
  static std::shared_ptr<const ThemePack> fromResource(const char* name = kThemePackResourceName, void* module = nullptr,
                                                       std::string* error = nullptr);

  size_t count() const { return entries_.size(); }
  std::string_view path(size_t i) const { return entries_[i].path; }
  std::string_view data(size_t i) const { return entries_[i].data; }
  int kind(size_t i) const { return entries_[i].kind; }
  std::optional<size_t> find(std::string_view path) const;
  std::string_view bytes() const { return blob_; }

 private:
  struct Entry {
    std::string_view path, data;
    int kind;
  };
  std::string owned_;
  std::string_view blob_;
  std::vector<Entry> entries_;
};

// SQ_THEME_RES_* for a (relative) path: theme.xml / *.colors.xml = COLORS, *.skin.xml = SKIN,
// png/jpg/jpeg/bmp/gif/tif/tiff/webp = IMAGE, ttf/otf/woff/woff2 = FONT, else OTHER.
int themeResourceKind(std::string_view path);
// "icons/play@2x.png" -> ("icons/play.png", 2). Names without a density suffix are density 1.
void splitDensity(std::string_view path, std::string& base, float& density);
// Index into `densities` of the variant for `scale` (rule above). densities must be non-empty.
size_t chooseDensity(const std::vector<float>& densities, float scale);
// Normalised relative theme path ("a/b.png"): false for empty, absolute, "..", "." or backslashes.
bool validThemePath(std::string_view path);
// A theme name: one valid path segment and not "current" (reserved by theme://current/).
bool validThemeName(std::string_view name);

// ---- resolver ---------------------------------------------------------------------------------------

struct ThemeInfo {
  std::string name, parent, title;
};

class ThemeResolver final : public sq::Object<ThemeResolver, IThemeResolver, ISubject> {
 public:
  static sq::Ptr<ThemeResolver> create();

  // Sources. Themes with the same name merge: later sources override files, colors and metrics.
  // A theme needs theme.xml (whose name= must match its directory / pack prefix).
  bool addPack(const std::shared_ptr<const ThemePack>& pack, std::string* error = nullptr);
  // `root` is either one theme (root/theme.xml) or a folder of themes (root/<name>/theme.xml).
  bool addDirectory(const std::string& root, std::string* error = nullptr);

  std::vector<ThemeInfo> themes() const;
  bool hasTheme(std::string_view name) const;
  std::string activeTheme() const;

  // Bytes of a resource (+ what resolve() reports). nullopt if absent.
  std::optional<std::string> read(const std::string& url, float scale = 1.0f, sq_theme_resource* info = nullptr) const;
  // Decoded image for a theme:// URL at `scale`, cached; Image::density() is the chosen variant.
  ImagePtr image(const std::string& url, float scale, std::string* error = nullptr) const;
  // For WidgetTree::imageProvider.
  std::function<ImagePtr(const std::string&, float)> imageProvider();
  // Skin defines of the active theme: theme.name, theme.color.<Name> = "#RRGGBBAA",
  // theme.metric.<Name> = number. Pass to Skin::build so skins can say $(theme.color.Text).
  SkinDefines skinDefines() const;
  // Same for an explicit theme (empty map if unknown).
  SkinDefines skinDefinesFor(std::string_view theme) const;
  // "theme://" resolver for IObjectRegistry (openStream; resolve() hands out IByteStream).
  sq::Ptr<IUrlResolver> urlResolver();

  // IThemeResolver
  sq_result SQ_CALL getActiveTheme(char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL setActiveTheme(const char* name) override;
  sq_result SQ_CALL resolve(const char* url, float scale, sq_theme_resource* out) override;
  sq_result SQ_CALL openResource(const char* url, float scale, IByteStream** out) override;
  sq_result SQ_CALL getColor(const char* name, uint32_t* rgba) override;
  sq_result SQ_CALL getMetric(const char* name, double* value) override;
  // ISubject
  sq_result SQ_CALL addObserver(IObserver* observer) override;
  sq_result SQ_CALL removeObserver(IObserver* observer) override;

  struct Theme;  // internal

 private:
  friend class sq::Object<ThemeResolver, IThemeResolver, ISubject>;
  ThemeResolver();
  ~ThemeResolver() override;

  struct Hit;
  bool findLocked(std::string_view url, float scale, Hit& hit) const;
  bool colorLocked(const std::string& start, std::string_view name, uint32_t& rgba) const;
  bool mergeTheme(const std::string& name, std::map<std::string, std::pair<std::shared_ptr<const void>, std::string_view>>& files,
                  std::string* error);
  std::vector<const Theme*> chainLocked(std::string_view name) const;
  SkinDefines definesLocked(std::string_view theme) const;

  mutable std::shared_mutex mu_;
  std::map<std::string, std::unique_ptr<Theme>, std::less<>> themes_;
  std::string active_;
  mutable std::mutex cacheMu_;
  mutable std::map<std::string, ImagePtr, std::less<>> cache_;
  uint64_t cacheGen_ = 0;  // bumped (under mu_ + cacheMu_) whenever theme content changes
  std::mutex obsMu_;
  std::vector<sq::Ptr<IObserver>> observers_;
};

}  // namespace sq::ui
