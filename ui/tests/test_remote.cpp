// ui/ remote pages generated from skin XML (the same language as on-screen UIs), the
// IRemotePageProvider COM surface, and the static skin validator.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sq/platform.hpp"
#include "sq/ui/remote_pages.hpp"
#include "sqtest.hpp"

using namespace sq::ui;

namespace {

const std::string kThemes = SQ_UI_THEMES_DIR;

sq::Ptr<ThemeResolver> stockThemes() {
  auto t = ThemeResolver::create();
  t->addDirectory(kThemes);
  t->setActiveTheme("dark");
  return t;
}

const sq_remote_control* findControl(const RemotePageSpec& p, const char* id) {
  for (const auto& c : p.controls)
    if (std::strcmp(c.id, id) == 0) return &c;
  return nullptr;
}

std::string pageName(sq::IRemotePage* p) {
  char buf[64];
  uint32_t need = 0;
  return p->getName(buf, sizeof buf, &need) == SQ_OK ? std::string(buf) : std::string();
}

bool hasIssue(const std::vector<SkinIssue>& issues, const char* text, bool error = true) {
  for (const auto& i : issues)
    if (i.error == error && i.message.find(text) != std::string::npos) return true;
  return false;
}

}  // namespace

SQ_TEST("remote: the stock mixer skin yields pages, typed controls and bound targets") {
  auto themes = stockThemes();
  auto xml = themes->read("theme://current/remote/mixer.skin.xml");
  REQUIRE(xml);
  std::string err;
  auto skin = Skin::parse(*xml, &err);
  REQUIRE(skin);
  std::vector<RemotePageSpec> pages;
  REQUIRE(buildRemotePages(*skin, themes->skinDefines(), "object://doc/active", pages, &err));
  REQUIRE_EQ(pages.size(), size_t{3});
  CHECK_EQ(pages[0].name, std::string("Mixer"));
  CHECK_EQ(pages[1].name, std::string("Mixer (no solo)"));
  CHECK_EQ(pages[2].name, std::string("Transport"));
  CHECK_EQ(pages[0].controls.size(), size_t{40});  // 8 x (display, pan, volume, mute, solo)
  CHECK_EQ(pages[1].controls.size(), size_t{32});  // the <if> drops solo
  CHECK_EQ(pages[2].controls.size(), size_t{5});

  const auto& mix = pages[0];
  CHECK_EQ(std::string(mix.controls[0].id), std::string("display1"));  // document order
  const sq_remote_control* knob = findControl(mix, "knob3");
  REQUIRE(knob);
  CHECK_EQ(knob->struct_size, static_cast<uint32_t>(sizeof(sq_remote_control)));
  CHECK_EQ(knob->type, SQ_REMOTE_KNOB);
  CHECK_EQ(std::string(knob->label), std::string("Pan 3"));
  CHECK_EQ(std::string(knob->target), std::string("object://doc/active/track/3"));
  CHECK_EQ(std::string(knob->property), std::string("pan"));
  CHECK_EQ(knob->flags, 0u);
  const sq_remote_control* fader = findControl(mix, "fader8");
  REQUIRE(fader);
  CHECK_EQ(fader->type, SQ_REMOTE_FADER);
  CHECK_EQ(fader->flags, static_cast<uint32_t>(SQ_REMOTE_FEEDBACK));
  const sq_remote_control* mute = findControl(mix, "mute1");
  REQUIRE(mute);
  CHECK_EQ(mute->type, SQ_REMOTE_BUTTON);
  CHECK_EQ(mute->flags, static_cast<uint32_t>(SQ_REMOTE_TOGGLE));
  const sq_remote_control* disp = findControl(mix, "display2");
  REQUIRE(disp);
  CHECK_EQ(disp->type, SQ_REMOTE_DISPLAY);
  CHECK_EQ(disp->flags, static_cast<uint32_t>(SQ_REMOTE_FEEDBACK));
  CHECK_EQ(std::string(disp->property), std::string("name"));
  CHECK(findControl(mix, "solo4") != nullptr);
  CHECK(findControl(pages[1], "solo4") == nullptr);

  const auto& tr = pages[2];
  const sq_remote_control* play = findControl(tr, "play");
  REQUIRE(play);
  CHECK_EQ(std::string(play->target), std::string("object://engine/transport"));  // absolute stays
  CHECK_EQ(play->flags, static_cast<uint32_t>(SQ_REMOTE_TOGGLE));
  const sq_remote_control* stop = findControl(tr, "stop");
  REQUIRE(stop);
  CHECK_EQ(std::string(stop->target), std::string("command://Transport.Stop"));
  CHECK_EQ(std::string(stop->property), std::string("invoke"));
  CHECK_EQ(findControl(tr, "jog")->type, SQ_REMOTE_ENCODER);
  CHECK_EQ(findControl(tr, "level")->type, SQ_REMOTE_LED);
  CHECK_EQ(findControl(tr, "level")->flags, static_cast<uint32_t>(SQ_REMOTE_FEEDBACK));

  // Without a context, relative targets stay relative for the consumer to resolve.
  REQUIRE(buildRemotePages(*skin, themes->skinDefines(), "", pages, &err));
  CHECK_EQ(std::string(findControl(pages[0], "knob3")->target), std::string("track/3"));
}

SQ_TEST("remote: implicit pages, and errors for ambiguous or broken controls") {
  std::string err;
  auto skin = Skin::parse(R"~(<Skin><Forms>
      <Form name="B"><Knob remote="k" target="p"/></Form>
      <Form name="A"><Fader remote="f" name="vol"/><Include form="Helper" x="1"/></Form>
      <Form name="Helper"><Knob value="$(x)" remote="h$(x)"/></Form>
      <Form name="Plain"><Label text="no controls"/></Form>
    </Forms></Skin>)~", &err);
  REQUIRE(skin);
  std::vector<RemotePageSpec> pages;
  REQUIRE(buildRemotePages(*skin, {}, "object://ctx/", pages, &err));
  // A (2 controls), B (1); Helper fails without x (skipped), Plain has no controls (skipped).
  REQUIRE_EQ(pages.size(), size_t{2});
  CHECK_EQ(pages[0].name, std::string("A"));
  CHECK_EQ(pages[0].controls.size(), size_t{2});
  CHECK_EQ(std::string(pages[0].controls[0].label), std::string("vol"));  // name when no text
  CHECK_EQ(std::string(pages[0].controls[1].label), std::string("h1"));   // id as last resort
  CHECK_EQ(std::string(pages[1].controls[0].target), std::string("object://ctx/p"));

  auto dup = Skin::parse(R"~(<Skin><Forms><Form name="A"><Knob remote="x"/><Fader remote="x"/></Form></Forms></Skin>)~");
  CHECK(!buildRemotePages(*dup, {}, "", pages, &err));
  CHECK(err.find("duplicate") != std::string::npos);
  auto untyped = Skin::parse(R"~(<Skin><Forms><Form name="A"><View remote="v"/></Form></Forms></Skin>)~");
  CHECK(!buildRemotePages(*untyped, {}, "", pages, &err));
  CHECK(err.find("remoteType") != std::string::npos);
  auto typed = Skin::parse(R"~(<Skin><Forms><Form name="A"><View remote="v" remoteType="pad"/></Form></Forms></Skin>)~");
  REQUIRE(buildRemotePages(*typed, {}, "", pages, &err));
  CHECK_EQ(pages[0].controls[0].type, SQ_REMOTE_PAD);
  auto longId = Skin::parse("<Skin><Forms><Form name=\"A\"><Knob remote=\"" + std::string(80, 'k') + "\"/></Form></Forms></Skin>");
  CHECK(!buildRemotePages(*longId, {}, "", pages, &err));
  auto badPage = Skin::parse(R"~(<Skin><Forms><Form name="A"><Knob remote="k"/></Form></Forms>
      <RemotePages><Page name="P" form="Missing"/></RemotePages></Skin>)~");
  REQUIRE(badPage);
  CHECK(!buildRemotePages(*badPage, {}, "", pages, &err));
  CHECK(err.find("'P'") != std::string::npos);
  auto none = Skin::parse(R"~(<Skin><Forms><Form name="A"><Knob/></Form></Forms></Skin>)~");
  CHECK(!buildRemotePages(*none, {}, "", pages, &err));
  CHECK(!Skin::parse(R"~(<Skin><RemotePages><Page name="x"/></RemotePages></Skin>)~", &err));  // Page without form
  CHECK(!Skin::parse(R"~(<Skin><Forms><Form name="A"><Knob remoteType="dial"/></Form></Forms></Skin>)~")->build("A", {}, &err));
}

SQ_TEST("remote: IRemotePageProvider loads files and theme URLs into immutable COM snapshots") {
  auto themes = stockThemes();
  auto provider = sq::Ptr<RemotePageProvider>::adopt(new RemotePageProvider(themes));
  sq::Ptr<sq::IRemotePageSet> set;
  REQUIRE_EQ(provider->loadRemotePages("theme://current/remote/mixer.skin.xml", "object://doc/track", set.put()), SQ_OK);
  REQUIRE_EQ(set->getPageCount(), 3u);
  sq::Ptr<sq::IRemotePage> page;
  REQUIRE_EQ(set->getPage(2, page.put()), SQ_OK);
  CHECK_EQ(pageName(page.get()), std::string("Transport"));
  char tiny[4];
  uint32_t need = 0;
  CHECK_EQ(page->getName(tiny, sizeof tiny, &need), SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(need, 10u);
  CHECK_EQ(page->getControlCount(), 5u);
  sq_remote_control c{};
  c.struct_size = sizeof c;
  REQUIRE_EQ(page->getControl(0, &c), SQ_OK);
  CHECK_EQ(std::string(c.id), std::string("play"));
  CHECK_EQ(page->getControl(5, &c), SQ_E_INVALIDARG);
  sq::Ptr<sq::IRemotePage> none;
  CHECK_EQ(set->getPage(3, none.put()), SQ_E_INVALIDARG);

  // file path (the same bytes written to a temp file), no context
  std::string tmp = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sq_ui_remote_test.skin.xml");
  REQUIRE_EQ(sq::writeFile(tmp, *themes->read("theme://base/remote/mixer.skin.xml")), SQ_OK);
  sq::Ptr<sq::IRemotePageSet> fromFile;
  CHECK_EQ(provider->loadRemotePages(tmp.c_str(), nullptr, fromFile.put()), SQ_OK);
  REQUIRE(fromFile);
  CHECK_EQ(fromFile->getPageCount(), 3u);
  sq_fs_remove(tmp.c_str());

  sq::Ptr<sq::IRemotePageSet> bad;
  CHECK_EQ(provider->loadRemotePages("theme://current/nope.skin.xml", nullptr, bad.put()), SQ_E_NOTFOUND);
  CHECK_EQ(provider->loadRemotePages("Z:/definitely/missing.skin.xml", nullptr, bad.put()), SQ_E_NOTFOUND);
  CHECK_EQ(provider->loadRemotePages(nullptr, nullptr, bad.put()), SQ_E_INVALIDARG);
  // Without a resolver, theme:// cannot work.
  auto bare = sq::Ptr<RemotePageProvider>::adopt(new RemotePageProvider());
  CHECK_EQ(bare->loadRemotePages("theme://current/remote/mixer.skin.xml", nullptr, bad.put()), SQ_E_NOTFOUND);
}

SQ_TEST("skin validate: the stock remote skin is clean and its assets resolve") {
  auto themes = stockThemes();
  auto xml = themes->read("theme://current/remote/mixer.skin.xml");
  REQUIRE(xml);
  std::vector<SkinIssue> issues;
  std::vector<std::string> assets;
  SkinValidateOptions opt;
  opt.assetExists = [&](const std::string& url) { return themes->read(url).has_value(); };
  CHECK(Skin::validate(*xml, issues, opt, &assets));
  for (const auto& i : issues) std::printf("  %s: %s: %s\n", i.error ? "error" : "warning", i.where.c_str(), i.message.c_str());
  CHECK(issues.empty());
  REQUIRE_EQ(assets.size(), size_t{3});
  CHECK_EQ(assets[0], std::string("theme://current/icons/play.png"));
}

SQ_TEST("skin validate: every branch is checked and structural mistakes are reported") {
  const char* xml = R"~(<Skin name="bad">
    <Defines><define value="x"/><View/></Defines>
    <Forms>
      <Form name="A" direction="sideways">
        <if defined="a" equals="b"><Knob/></if>
        <if equals="x"><Label/></if>
        <if defined="q"><Knob/><else><Fader frobnicate="1"/></else><else/></if>
        <switch><case><Label width="wide"/></case><case value="1"/><case value="1"/><default/><default/><Knob/></switch>
        <else/>
        <Include/>
        <Include form="Nope"/>
        <Include form="$(dynamic)"/>
        <Button image="theme://current/icons/missing.png" color="$(c)" bogusAttr="$(c)"/>
        <Image image="file.png"/>
        <Gadget/>
        <define/>
        <Knob remote=""/>
      </Form>
      <Form name="Loop1"><Include form="Loop2"/></Form>
      <Form name="Loop2"><Include form="Loop1"/></Form>
      <Form name="Rec"><if defined="more"><Include form="Rec"/></if></Form>
    </Forms>
    <RemotePages><Page form="Nope"/><Page name="X" form="A"/><Page name="X" form="A"/><Junk/></RemotePages>
    <Whatever/>
  </Skin>)~";
  std::vector<SkinIssue> issues;
  SkinValidateOptions opt;
  opt.assetExists = [](const std::string& url) { return url.find("missing") == std::string::npos; };
  CHECK(!Skin::validate(xml, issues, opt));
  CHECK(hasIssue(issues, "<define> needs a name"));
  CHECK(hasIssue(issues, "<Defines> may only contain"));
  CHECK(hasIssue(issues, "bad value 'sideways'"));
  CHECK(hasIssue(issues, "exactly one of"));
  CHECK(hasIssue(issues, "needs value="));
  CHECK(hasIssue(issues, "unknown attribute 'frobnicate'"));  // inside <else>
  CHECK(hasIssue(issues, "more than one <else>"));
  CHECK(hasIssue(issues, "<switch> needs value="));
  CHECK(hasIssue(issues, "<case> needs value="));
  CHECK(hasIssue(issues, "bad value 'wide'"));                // inside a <case>
  CHECK(hasIssue(issues, "never chosen", false));
  CHECK(hasIssue(issues, "more than one <default>"));
  CHECK(hasIssue(issues, "may only contain <case> and <default>"));
  CHECK(hasIssue(issues, "<else> outside"));
  CHECK(hasIssue(issues, "<Include> needs form="));
  CHECK(hasIssue(issues, "unknown form 'Nope'"));
  CHECK(hasIssue(issues, "missing theme asset"));
  CHECK(hasIssue(issues, "unknown attribute 'bogusAttr'"));  // name checked even with a $ value
  CHECK(!hasIssue(issues, "attribute 'color'"));              // $ value: not judged
  CHECK(hasIssue(issues, "not a theme:// URL", false));
  CHECK(hasIssue(issues, "unknown skin element <Gadget>"));
  CHECK(hasIssue(issues, "remote id must be"));
  CHECK(hasIssue(issues, "unconditional include cycle"));
  CHECK(hasIssue(issues, "guarded by a conditional", false));
  CHECK(hasIssue(issues, "remote page uses unknown form 'Nope'"));
  CHECK(hasIssue(issues, "duplicate remote page name 'X'"));
  CHECK(hasIssue(issues, "may only contain <Page>"));
  CHECK(hasIssue(issues, "unknown top-level element <Whatever>", false));

  // Custom host elements are accepted when declared; malformed XML is one error.
  std::vector<SkinIssue> i2;
  SkinValidateOptions custom;
  custom.customElements = {"Arrange"};
  CHECK(Skin::validate(R"~(<Skin><Forms><Form name="M"><Arrange grow="1" anything="x"/></Form></Forms></Skin>)~", i2, custom));
  CHECK(!Skin::validate("<Skin><Forms>", i2));
}
