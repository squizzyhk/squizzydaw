// ui/ themes: theme:// resolution, colors/metrics with inheritance and aliases, @2x/@3x selection,
// observers, skin defines, theme packs (determinism, validation) and host registration.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "sq/app/class_registry.hpp"
#include "sq/app/host_context.hpp"
#include "sq/app/settings.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/platform.hpp"
#include "sq/ui/remote_pages.hpp"
#include "sq/ui/theme.hpp"
#include "sqtest.hpp"

using namespace sq::ui;

namespace {

const std::string kThemes = SQ_UI_THEMES_DIR;

sq::Ptr<ThemeResolver> stockThemes() {
  auto t = ThemeResolver::create();
  std::string err;
  if (!t->addDirectory(kThemes, &err)) std::printf("addDirectory: %s\n", err.c_str());
  return t;
}

sq_theme_resource res(ThemeResolver& t, const char* url, float scale, sq_result* rc = nullptr) {
  sq_theme_resource r{};
  r.struct_size = sizeof r;
  sq_result x = t.resolve(url, scale, &r);
  if (rc) *rc = x;
  return r;
}

uint32_t color(ThemeResolver& t, const char* name) {
  uint32_t c = 0;
  return t.getColor(name, &c) == SQ_OK ? c : 0xDEADBEEFu;
}

// Every file under the stock themes as pack entries "<theme>/<rel>".
std::vector<ThemePackEntry> stockEntries() {
  std::vector<ThemePackEntry> out;
  std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& abs, const std::string& rel) {
    std::vector<std::pair<std::string, bool>> list;
    sq_fs_list_dir(
        abs.c_str(),
        [](void* u, const char* n, sq_bool d) -> sq_bool {
          static_cast<std::vector<std::pair<std::string, bool>>*>(u)->emplace_back(n, d != 0);
          return SQ_TRUE;
        },
        &list);
    for (auto& [n, d] : list) {
      if (n[0] == '.') continue;
      std::string a = sq::joinPath(abs, n), r = rel.empty() ? n : rel + "/" + n;
      if (d) walk(a, r);
      else out.push_back(ThemePackEntry{r, *sq::readFile(a)});
    }
  };
  walk(kThemes, "");
  return out;
}

class RecordingObserver final : public sq::Object<RecordingObserver, sq::IObserver> {
 public:
  void SQ_CALL onNotify(sq::IUnknown*, const char* topic, sq::IAttributes* args) override {
    topics.push_back(topic);
    themes.push_back(args ? sq::getAttrString(args, "theme") + "<-" + sq::getAttrString(args, "previous") : std::string());
  }
  std::vector<std::string> topics, themes;
};

}  // namespace

SQ_TEST("theme: stock themes load with inheritance, colors, aliases and metrics") {
  auto t = stockThemes();
  auto list = t->themes();
  REQUIRE_EQ(list.size(), size_t{3});
  std::map<std::string, std::string> parents;
  for (auto& i : list) parents[i.name] = i.parent;
  CHECK_EQ(parents["dark"], std::string("base"));
  CHECK_EQ(parents["light"], std::string("base"));
  CHECK_EQ(parents["base"], std::string());

  CHECK_EQ(t->setActiveTheme("dark"), SQ_OK);
  CHECK_EQ(t->setActiveTheme("dark"), SQ_S_FALSE);
  CHECK_EQ(t->setActiveTheme("nope"), SQ_E_NOTFOUND);
  CHECK_EQ(t->activeTheme(), std::string("dark"));
  CHECK_EQ(color(*t, "Window.Background"), 0x1C1D21FFu);
  CHECK_EQ(color(*t, "Track.Audio"), 0x4FB3A9FFu);       // base alias -> base Accent
  CHECK_EQ(color(*t, "Transport.Record"), 0xE0604AFFu);  // alias chain
  double m = 0;
  CHECK_EQ(t->getMetric("Track.Height", &m), SQ_OK);  // inherited from base
  CHECK_EQ(m, 46.0);
  CHECK_EQ(t->getMetric("Corner.Radius", &m), SQ_OK);
  CHECK_EQ(m, 3.0);

  CHECK_EQ(t->setActiveTheme("light"), SQ_OK);
  CHECK_EQ(color(*t, "Window.Background"), 0xF2F3F5FFu);
  CHECK_EQ(color(*t, "Track.Audio"), 0x2E8C83FFu);  // light overrides Accent: base alias follows
  CHECK_EQ(t->getMetric("Corner.Radius", &m), SQ_OK);
  CHECK_EQ(m, 4.0);
  uint32_t c = 0;
  CHECK_EQ(t->getColor("No.Such.Color", &c), SQ_E_NOTFOUND);
  CHECK_EQ(t->getMetric("No.Such.Metric", &m), SQ_E_NOTFOUND);
  CHECK_EQ(t->getColor(nullptr, &c), SQ_E_INVALIDARG);

  char small[3];
  uint32_t need = 0;
  CHECK_EQ(t->getActiveTheme(small, sizeof small, &need), SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(need, 6u);
  char buf[16];
  CHECK_EQ(t->getActiveTheme(buf, sizeof buf, &need), SQ_OK);
  CHECK_EQ(std::string(buf), std::string("light"));
}

SQ_TEST("theme: density helpers") {
  std::string base;
  float d = 0;
  splitDensity("icons/play@2x.png", base, d);
  CHECK_EQ(base, std::string("icons/play.png"));
  CHECK_EQ(d, 2.0f);
  splitDensity("a/b@1.5x.png", base, d);
  CHECK_EQ(base, std::string("a/b.png"));
  CHECK_EQ(d, 1.5f);
  splitDensity("mail@home.png", base, d);  // not a density suffix
  CHECK_EQ(base, std::string("mail@home.png"));
  CHECK_EQ(d, 1.0f);
  splitDensity("dir@2x/file.png", base, d);  // suffix only counts on the file name
  CHECK_EQ(d, 1.0f);
  std::vector<float> ds{1, 2, 3};
  CHECK_EQ(chooseDensity(ds, 1.0f), size_t{0});
  CHECK_EQ(chooseDensity(ds, 1.25f), size_t{1});
  CHECK_EQ(chooseDensity(ds, 2.0f), size_t{1});
  CHECK_EQ(chooseDensity(ds, 2.01f), size_t{2});
  CHECK_EQ(chooseDensity(ds, 4.0f), size_t{2});  // largest available
  CHECK_EQ(chooseDensity(ds, 0.5f), size_t{0});
  CHECK_EQ(chooseDensity(ds, -1.0f), size_t{0});  // invalid scale = 1
  CHECK(validThemePath("icons/play.png"));
  CHECK(!validThemePath("../x.png"));
  CHECK(!validThemePath("a//b"));
  CHECK(!validThemePath("/abs"));
  CHECK(!validThemePath("a\\b"));
  CHECK_EQ(themeResourceKind("x/theme.xml"), SQ_THEME_RES_COLORS);
  CHECK_EQ(themeResourceKind("remote/mixer.skin.xml"), SQ_THEME_RES_SKIN);
  CHECK_EQ(themeResourceKind("icons/a.PNG"), SQ_THEME_RES_IMAGE);
  CHECK_EQ(themeResourceKind("fonts/a.ttf"), SQ_THEME_RES_FONT);
  CHECK_EQ(themeResourceKind("readme.txt"), SQ_THEME_RES_OTHER);
}

SQ_TEST("theme: @2x / @3x variant selection through resolve, openResource and image") {
  auto t = stockThemes();
  REQUIRE_EQ(t->setActiveTheme("dark"), SQ_OK);
  sq_result rc = 0;
  auto r1 = res(*t, "theme://current/icons/play.png", 1.0f, &rc);
  REQUIRE_EQ(rc, SQ_OK);
  CHECK_EQ(r1.kind, SQ_THEME_RES_IMAGE);
  CHECK_EQ(r1.scale, 1.0f);
  CHECK_EQ(r1.width, 16u);
  auto r2 = res(*t, "theme://current/icons/play.png", 2.0f);
  CHECK_EQ(r2.scale, 2.0f);
  CHECK_EQ(r2.width, 32u);
  CHECK_EQ(r2.height, 32u);
  CHECK_EQ(res(*t, "theme://current/icons/play.png", 1.5f).scale, 2.0f);  // round up
  CHECK_EQ(res(*t, "theme://current/icons/play.png", 3.0f).scale, 2.0f);  // largest there is
  CHECK_EQ(res(*t, "theme://current/knob/cap.png", 2.5f).width, 96u);     // @3x
  CHECK_EQ(res(*t, "theme://current/knob/cap.png", 0.75f).width, 32u);
  // inherited from base, explicit theme name, explicit variant name
  auto seg = res(*t, "theme://current/meter/segments.png", 2.0f, &rc);
  CHECK_EQ(rc, SQ_OK);
  CHECK_EQ(seg.height, 128u);
  CHECK_EQ(res(*t, "theme://light/icons/stop.png", 1.0f, &rc).width, 16u);
  CHECK_EQ(rc, SQ_OK);
  CHECK_EQ(res(*t, "theme://dark/icons/play@2x.png", 1.0f).scale, 2.0f);
  auto colors = res(*t, "theme://current/theme.xml", 1.0f, &rc);
  CHECK_EQ(rc, SQ_OK);
  CHECK_EQ(colors.kind, SQ_THEME_RES_COLORS);
  CHECK_EQ(res(*t, "theme://current/remote/mixer.skin.xml", 1.0f).kind, SQ_THEME_RES_SKIN);
  // misses and hostile URLs
  res(*t, "theme://current/icons/none.png", 1.0f, &rc);
  CHECK_EQ(rc, SQ_E_NOTFOUND);
  res(*t, "theme://dark/../light/theme.xml", 1.0f, &rc);
  CHECK_EQ(rc, SQ_E_NOTFOUND);
  res(*t, "file:///etc/passwd", 1.0f, &rc);
  CHECK_EQ(rc, SQ_E_NOTFOUND);
  res(*t, "theme://nosuchtheme/icons/play.png", 1.0f, &rc);
  CHECK_EQ(rc, SQ_E_NOTFOUND);
  CHECK_EQ(t->resolve("theme://current/icons/play.png", 1.0f, nullptr), SQ_E_INVALIDARG);

  // openResource returns exactly the chosen file
  sq::Ptr<sq::IByteStream> s;
  REQUIRE_EQ(t->openResource("theme://current/icons/play.png", 2.0f, s.put()), SQ_OK);
  int64_t size = 0;
  s->getSize(&size);
  auto disk = sq::readFile(sq::joinPath(kThemes, "dark/icons/play@2x.png"));
  REQUIRE(disk);
  CHECK_EQ(static_cast<size_t>(size), disk->size());
  std::string got(static_cast<size_t>(size), '\0');
  uint32_t n = 0;
  s->read(got.data(), static_cast<uint32_t>(size), &n);
  CHECK(got == *disk);
  sq::Ptr<sq::IByteStream> none;
  CHECK_EQ(t->openResource("theme://current/x.png", 1.0f, none.put()), SQ_E_NOTFOUND);

  // decoded + cached images carry the variant density
  std::string err;
  ImagePtr img = t->image("theme://current/knob/cap.png", 3.0f, &err);
  REQUIRE(img);
  CHECK_EQ(img->width(), 96);
  CHECK_EQ(img->density(), 3.0f);
  CHECK(t->image("theme://current/knob/cap.png", 3.0f) == img);  // cache hit
  ImagePtr dark = t->image("theme://current/icons/play.png", 1.0f);
  REQUIRE_EQ(t->setActiveTheme("light"), SQ_OK);
  ImagePtr light = t->image("theme://current/icons/play.png", 1.0f);
  REQUIRE(dark);
  REQUIRE(light);
  CHECK(dark != light);
  CHECK((dark->pixelRgba(8, 8) & 0xFF) > 0);
  CHECK_EQ(dark->pixelRgba(8, 8) >> 8, 0xE4E6EBu);  // dark icon ink
  CHECK_EQ(light->pixelRgba(8, 8) >> 8, 0x2A2C33u);  // light icon ink
  auto provider = t->imageProvider();
  CHECK(provider("theme://current/icons/play.png", 2.0f)->width() == 32);
}

SQ_TEST("theme: observers hear theme.changed; skin defines expose colors and metrics") {
  auto t = stockThemes();
  REQUIRE_EQ(t->setActiveTheme("dark"), SQ_OK);
  auto obs = sq::Ptr<RecordingObserver>::adopt(new RecordingObserver());
  sq::Ptr<sq::ISubject> subject;
  REQUIRE_EQ(t->queryInterface(&sq::ISubject::iid, subject.putVoid()), SQ_OK);
  CHECK_EQ(subject->addObserver(obs.get()), SQ_OK);
  CHECK_EQ(subject->addObserver(obs.get()), SQ_S_FALSE);
  CHECK_EQ(t->setActiveTheme("light"), SQ_OK);
  CHECK_EQ(t->setActiveTheme("light"), SQ_S_FALSE);  // no change, no event
  REQUIRE_EQ(obs->topics.size(), size_t{1});
  CHECK_EQ(obs->topics[0], std::string("theme.changed"));
  CHECK_EQ(obs->themes[0], std::string("light<-dark"));
  CHECK_EQ(subject->removeObserver(obs.get()), SQ_OK);
  t->setActiveTheme("dark");
  CHECK_EQ(obs->topics.size(), size_t{1});

  SkinDefines d = t->skinDefines();
  CHECK_EQ(d["theme.name"], std::string("dark"));
  CHECK_EQ(d["theme.color.Window.Background"], std::string("#1C1D21FF"));
  CHECK_EQ(d["theme.color.Track.Audio"], std::string("#4FB3A9FF"));
  CHECK_EQ(d["theme.metric.Track.Height"], std::string("46"));
  SkinDefines light = t->skinDefinesFor("light");
  CHECK_EQ(light["theme.color.Track.Audio"], std::string("#2E8C83FF"));
  CHECK(t->skinDefinesFor("nope").empty());

  std::string err;
  auto skin = Skin::parse(R"~(<Skin><Forms><Form name="M" background="$(theme.color.Window.Background)">
      <Label name="l" color="$theme.color.Text" height="$(theme.metric.Track.Height)" image="theme://current/icons/play.png"/>
    </Form></Forms></Skin>)~", &err);
  REQUIRE(skin);
  auto w = skin->build("M", d, &err);
  REQUIRE(w);
  CHECK_EQ(w->visual.background.toRgba(), 0x1C1D21FFu);
  Widget* l = w->findByName("l");
  REQUIRE(l);
  CHECK_EQ(l->visual.foreground.toRgba(), 0xE4E6EBFFu);
  WidgetTree tree;
  tree.imageProvider = t->imageProvider();
  tree.setRoot(std::move(w));
  tree.setScale(2.0f);
  ImagePtr img = tree.image("theme://current/icons/play.png");
  REQUIRE(img);
  CHECK_EQ(img->density(), 2.0f);
}

SQ_TEST("theme: packs are deterministic, validated, and resolve like the source folders") {
  auto entries = stockEntries();
  REQUIRE(entries.size() > 20);
  std::string a, b, err;
  REQUIRE(writeThemePack(entries, a, &err));
  auto shuffled = entries;
  std::mt19937 rng(1234);
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  REQUIRE(writeThemePack(shuffled, b, &err));
  CHECK(a == b);
  CHECK_EQ(a.size() % 8, size_t{0});

  auto pack = ThemePack::fromMemory(a.data(), a.size(), true, &err);
  REQUIRE(pack);
  CHECK_EQ(pack->count(), entries.size());
  for (const auto& e : entries) {
    auto i = pack->find(e.path);
    REQUIRE(i.has_value());
    CHECK(pack->data(*i) == e.data);
  }
  CHECK(!pack->find("dark/nope.png").has_value());

  auto fromPack = ThemeResolver::create();
  REQUIRE(fromPack->addPack(pack, &err));
  auto fromDir = stockThemes();
  for (const char* theme : {"dark", "light"}) {
    fromPack->setActiveTheme(theme);
    fromDir->setActiveTheme(theme);
    CHECK(fromPack->skinDefines() == fromDir->skinDefines());
    for (float s : {1.0f, 2.0f, 3.0f})
      for (const char* u : {"theme://current/icons/record.png", "theme://current/knob/cap.png", "theme://current/shadow/round.png"})
        CHECK(*fromPack->read(u, s) == *fromDir->read(u, s));
  }

  // Validation: corruption, truncation, bad magic, duplicate / invalid paths.
  std::string bad2 = a;
  size_t firstData = static_cast<size_t>(pack->data(0).data() - pack->bytes().data());
  bad2[firstData] ^= 0x01;
  CHECK(ThemePack::fromMemory(bad2.data(), bad2.size(), true, &err) == nullptr);
  CHECK(err.find("CRC") != std::string::npos);
  CHECK(ThemePack::fromMemory(a.data(), a.size() - 8, true, &err) == nullptr);
  CHECK(ThemePack::fromMemory("SQTHMPK2", 8, true, &err) == nullptr);
  std::string magic = a;
  magic[0] = 'X';
  CHECK(ThemePack::fromMemory(magic.data(), magic.size(), true, &err) == nullptr);
  std::string out;
  CHECK(!writeThemePack({{"dark/a.png", "1"}, {"dark/a.png", "2"}}, out, &err));
  CHECK(!writeThemePack({{"dark/../a.png", "1"}}, out, &err));
  CHECK(!writeThemePack({{"noprefix", "1"}}, out, &err));
  // A theme without theme.xml is refused by the resolver.
  REQUIRE(writeThemePack({{"orphan/icons/a.png", "x"}}, out, &err));
  auto orphan = ThemePack::fromMemory(out.data(), out.size(), true, &err);
  REQUIRE(orphan);
  CHECK(!ThemeResolver::create()->addPack(orphan, &err));
  CHECK(err.find("theme.xml") != std::string::npos);
}

SQ_TEST("theme: later sources override; mismatched theme.xml names are rejected") {
  auto t = stockThemes();
  std::string over, err;
  REQUIRE(writeThemePack({{"dark/theme.xml", R"~(<Theme name="dark" format="1" inherits="base"><Colors><Color name="Text" value="#010203"/></Colors></Theme>)~"}},
                         over, &err));
  REQUIRE(t->addPack(ThemePack::fromMemory(over.data(), over.size(), true), &err));
  t->setActiveTheme("dark");
  CHECK_EQ(color(*t, "Text"), 0x010203FFu);
  CHECK_EQ(color(*t, "Window.Background"), 0x1C1D21FFu);  // untouched values survive the merge
  sq_result rc = 0;
  res(*t, "theme://current/icons/play.png", 2.0f, &rc);  // files survive too
  CHECK_EQ(rc, SQ_OK);
  REQUIRE(writeThemePack({{"x/theme.xml", R"~(<Theme name="y"/>)~"}}, over, &err));
  CHECK(!t->addPack(ThemePack::fromMemory(over.data(), over.size(), true), &err));
  CHECK(err.find("must match") != std::string::npos);
  REQUIRE(writeThemePack({{"x/theme.xml", R"~(<Theme name="x"><Colors><Color name="A" value="red"/></Colors></Theme>)~"}}, over, &err));
  CHECK(!t->addPack(ThemePack::fromMemory(over.data(), over.size(), true), &err));
  CHECK(!t->addDirectory(sq::joinPath(kThemes, "does-not-exist"), &err));
}

namespace {
// Minimal IObjectRegistry stand-in (scripting-js implements the real one).
class FakeObjects final : public sq::Object<FakeObjects, sq::IObjectRegistry> {
 public:
  std::map<std::string, sq::Ptr<sq::IUrlResolver>> resolvers;
  sq_result SQ_CALL registerResolver(const char* prefix, sq::IUrlResolver* r) override {
    if (resolvers.count(prefix)) return SQ_E_EXISTS;
    resolvers[prefix] = sq::Ptr<sq::IUrlResolver>(r);
    return SQ_OK;
  }
  sq_result SQ_CALL unregisterResolver(const char* prefix, sq::IUrlResolver*) override {
    return resolvers.erase(prefix) ? SQ_OK : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL registerObject(const char*, sq::IUnknown*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL unregisterObject(const char*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL resolve(const char*, const sq_guid*, void**) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL openStream(const char* url, sq::IByteStream** out) override {
    auto it = resolvers.find("theme://");
    return it == resolvers.end() ? SQ_E_NOTFOUND : it->second->openStream(url, out);
  }
};
}  // namespace

SQ_TEST("theme: UiServices registers the resolver, remote pages and theme:// with a real host") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  auto objects = sq::Ptr<FakeObjects>::adopt(new FakeObjects());
  REQUIRE_EQ(host.registerService(&sq::IObjectRegistry::iid, static_cast<sq::IObjectRegistry*>(objects.get())), SQ_OK);
  {
    UiServices ui(&host, stockThemes());
    sq::Ptr<sq::IThemeResolver> tr;
    REQUIRE_EQ(host.queryService(&sq::IThemeResolver::iid, tr.putVoid()), SQ_OK);
    CHECK_EQ(tr->setActiveTheme("light"), SQ_OK);
    sq::Ptr<sq::IRemotePageProvider> rp;
    REQUIRE_EQ(host.queryService(&sq::IRemotePageProvider::iid, rp.putVoid()), SQ_OK);
    CHECK(ui.themeUrlsRegistered());
    // theme:// through the object registry, with ?scale=
    sq::Ptr<sq::IByteStream> s;
    REQUIRE_EQ(objects->openStream("theme://current/knob/cap.png?scale=3", s.put()), SQ_OK);
    int64_t size = 0;
    s->getSize(&size);
    CHECK_EQ(static_cast<size_t>(size), sq::readFile(sq::joinPath(kThemes, "light/knob/cap@3x.png"))->size());
    sq::Ptr<sq::IRemotePageSet> pages;
    CHECK_EQ(rp->loadRemotePages("theme://current/remote/mixer.skin.xml", "object://doc/active", pages.put()), SQ_OK);
    REQUIRE(pages);
    CHECK_EQ(pages->getPageCount(), 3u);
  }
  sq::Ptr<sq::IThemeResolver> gone;
  CHECK_EQ(host.queryService(&sq::IThemeResolver::iid, gone.putVoid()), SQ_E_NOTFOUND);
  sq::Ptr<sq::IRemotePageProvider> gone2;
  CHECK_EQ(host.queryService(&sq::IRemotePageProvider::iid, gone2.putVoid()), SQ_E_NOTFOUND);
  CHECK(objects->resolvers.empty());
  host.unregisterService(&sq::IObjectRegistry::iid, static_cast<sq::IObjectRegistry*>(objects.get()));
}
