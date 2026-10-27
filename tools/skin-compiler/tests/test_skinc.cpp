// sq-skinc: stock themes validate and pack; the pack round-trips every source byte; output is
// deterministic (two in-process runs and the build-time run of the real executable agree, with
// the same SHA-256); outputs (.rc / .h / .json) are written; broken inputs are diagnosed.
#include <cstdio>
#include <string>
#include <vector>

#include "sq/platform.hpp"
#include "sq/tools/skin_compiler/skinc.hpp"
#include "sq/ui/image.hpp"
#include "sq/ui/theme.hpp"
#include "sqtest.hpp"

using namespace sq::tools::skinc;

namespace {

const std::string kThemes = SQ_UI_THEMES_DIR;

void dump(const Result& r) {
  for (const auto& d : r.diagnostics) std::printf("  %s: %s: %s\n", d.file.c_str(), d.error ? "error" : "warning", d.message.c_str());
}

bool has(const Result& r, const std::string& file, const char* text, bool error = true) {
  for (const auto& d : r.diagnostics)
    if (d.error == error && d.file.find(file) != std::string::npos && d.message.find(text) != std::string::npos) return true;
  return false;
}

std::string tempDir(const char* name) {
  std::string d = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), name);
  sq_fs_remove_all(d.c_str());
  sq_fs_create_dirs(d.c_str());
  return d;
}

void put(const std::string& dir, const std::string& rel, const std::string& data) {
  std::string p = sq::joinPath(dir, rel);
  std::string parent = p.substr(0, p.find_last_of('/'));
  sq_fs_create_dirs(parent.c_str());
  sq::writeFile(p, data);
}

std::string png(int w, int h) {
  sq::ui::PixelBuffer pb;
  pb.width = w;
  pb.height = h;
  pb.rgba.assign(static_cast<size_t>(w) * h * 4, 200);
  std::string out;
  sq::ui::encodePngCompressed(pb, out);
  return out;
}

}  // namespace

SQ_TEST("skinc: stock themes validate cleanly under --strict and pack deterministically") {
  Options o;
  o.inputs = {kThemes};
  o.validateOnly = true;
  o.strict = true;
  Result a = compile(o);
  dump(a);
  REQUIRE(a.ok);
  CHECK_EQ(a.diagnostics.size(), size_t{0});
  CHECK_EQ(a.themes, (std::vector<std::string>{"base", "dark", "light"}));
  CHECK(a.files > 30);
  Result b = compile(o);
  REQUIRE(b.ok);
  CHECK(a.pack == b.pack);
  CHECK_EQ(a.sha256, b.sha256);
  CHECK_EQ(a.sha256, sq::sha256Hex(a.pack));

  // Same themes given one by one, in another order: same bytes.
  Options o2 = o;
  o2.inputs = {sq::joinPath(kThemes, "light"), sq::joinPath(kThemes, "base"), sq::joinPath(kThemes, "dark")};
  Result c = compile(o2);
  REQUIRE(c.ok);
  CHECK_EQ(c.sha256, a.sha256);

  // The build ran the real sq-skinc.exe over the same folder: byte-identical output.
  auto built = sq::readFile(SQ_STOCK_PACK);
  REQUIRE(built);
  CHECK_EQ(sq::sha256Hex(*built), a.sha256);
  auto rc = sq::readFile(SQ_STOCK_RC);
  REQUIRE(rc);
  CHECK(rc->find("SQTHEMES RCDATA \"") != std::string::npos);
  CHECK(rc->find(a.sha256) != std::string::npos);
}

SQ_TEST("skinc: the pack round-trips every source file and resolves like the folders") {
  Options o;
  o.inputs = {kThemes};
  o.validateOnly = true;
  Result r = compile(o);
  REQUIRE(r.ok);
  std::string err;
  auto pack = sq::ui::ThemePack::fromMemory(r.pack.data(), r.pack.size(), true, &err);
  REQUIRE(pack);
  CHECK_EQ(pack->count(), r.files);
  for (size_t i = 0; i < pack->count(); ++i) {
    auto disk = sq::readFile(sq::joinPath(kThemes, std::string(pack->path(i))));
    REQUIRE(disk);
    CHECK(*disk == pack->data(i));
  }
  auto themes = sq::ui::ThemeResolver::create();
  REQUIRE(themes->addPack(pack, &err));
  REQUIRE_EQ(themes->setActiveTheme("light"), SQ_OK);
  sq_theme_resource info{};
  info.struct_size = sizeof info;
  REQUIRE_EQ(themes->resolve("theme://current/icons/loop.png", 2.0f, &info), SQ_OK);
  CHECK_EQ(info.scale, 2.0f);
  CHECK_EQ(info.width, 32u);
  uint32_t rgba = 0;
  CHECK_EQ(themes->getColor("Window.Background", &rgba), SQ_OK);
  CHECK_EQ(rgba, 0xF2F3F5FFu);
}

SQ_TEST("skinc: writes pack, .rc, header and manifest") {
  std::string dir = tempDir("sq_skinc_outputs");
  Options o;
  o.inputs = {kThemes};
  o.out = sq::joinPath(dir, "out.sqtheme");
  o.rc = sq::joinPath(dir, "out.rc");
  o.header = sq::joinPath(dir, "out.h");
  o.manifest = sq::joinPath(dir, "out.json");
  o.resourceName = "MY_THEMES";
  Result r = compile(o);
  dump(r);
  REQUIRE(r.ok);
  CHECK(*sq::readFile(o.out) == r.pack);
  std::string rc = *sq::readFile(o.rc);
  CHECK(rc.find("MY_THEMES RCDATA \"") != std::string::npos);
  CHECK(rc.find("out.sqtheme\"") != std::string::npos);
  CHECK(rc.find('\\') == std::string::npos);  // forward slashes only
  std::string h = *sq::readFile(o.header);
  CHECK(h.find("#define SQ_THEME_PACK_RESOURCE \"MY_THEMES\"") != std::string::npos);
  CHECK(h.find(r.sha256) != std::string::npos);
  std::string err;
  auto json = sq::jsonParse(*sq::readFile(o.manifest), &err);
  REQUIRE(json);
  std::string m = *sq::readFile(o.manifest);
  CHECK(m.find("\"dark/icons/play@2x.png\"") != std::string::npos);
  CHECK(m.find("\"kind\": \"skin\"") != std::string::npos);
  // Bad resource names and outputs without --out are refused.
  Options bad = o;
  bad.resourceName = "1 bad";
  CHECK(!compile(bad).ok);
  Options noOut = o;
  noOut.out.clear();
  CHECK(!compile(noOut).ok);
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("skinc: diagnoses broken themes, skins, images and variants") {
  std::string dir = tempDir("sq_skinc_broken");
  put(dir, "parent/theme.xml", R"~(<Theme name="parent" format="1"><Colors><Color name="A" value="#112233"/></Colors><Junk/></Theme>)~");
  put(dir, "child/theme.xml",
      R"~(<Theme name="child" format="1" inherits="parent"><Colors><Color name="B" value="@Nope"/><Color name="C" value="@A"/></Colors></Theme>)~");
  put(dir, "child/icons/ok.png", png(8, 8));
  put(dir, "child/icons/ok@2x.png", png(13, 16));  // should be 16x16
  put(dir, "child/icons/only@2x.png", png(16, 16));
  put(dir, "child/icons/broken.png", "\x89PNG\r\n\x1a\nnot really");
  put(dir, "parent/ui/main.skin.xml", R"~(<Skin><Forms>
      <Form name="M"><Include form="Gone"/><Button image="theme://current/icons/ok.png"/><Button image="theme://current/icons/absent.png"/>
        <switch value="$(k)"><case value="a"><Knob min="x"/></case></switch></Form>
    </Forms><RemotePages><Page name="P" form="M"/></RemotePages></Skin>)~");
  put(dir, "cyc1/theme.xml", R"~(<Theme name="cyc1" inherits="cyc2"/>)~");
  put(dir, "cyc2/theme.xml", R"~(<Theme name="cyc2" inherits="cyc1"/>)~");
  put(dir, "mismatch/theme.xml", R"~(<Theme name="other"/>)~");
  put(dir, "notatheme/readme.txt", "x");
  Options o;
  o.inputs = {dir};
  o.validateOnly = true;
  Result r = compile(o);
  dump(r);
  CHECK(!r.ok);
  CHECK(r.pack.empty());
  CHECK(has(r, "notatheme", "no theme.xml", false));
  CHECK(has(r, "parent/theme.xml", "unknown element <Junk>", false));
  CHECK(has(r, "child/theme.xml", "color 'B' does not resolve"));
  CHECK(!has(r, "child/theme.xml", "color 'C'"));  // alias into the parent is fine
  CHECK(has(r, "child/icons/ok.png", "expected about 16x16", false));
  CHECK(has(r, "child/icons/only.png", "no 1x image", false));
  CHECK(has(r, "child/icons/broken.png", "does not decode"));
  CHECK(has(r, "parent/ui/main.skin.xml", "unknown form 'Gone'"));
  CHECK(has(r, "parent/ui/main.skin.xml", "bad value 'x'"));
  CHECK(has(r, "parent/ui/main.skin.xml", "missing theme asset 'theme://current/icons/absent.png'"));
  CHECK(has(r, "parent/ui/main.skin.xml", "missing in theme(s): child"));
  CHECK(has(r, "parent/ui/main.skin.xml", "remote pages fail under theme 'child'"));
  CHECK(has(r, "cyc", "inheritance cycle"));
  // A folder of themes is also loadable by ThemeResolver::addDirectory, which requires the
  // declared name to match the folder: the compiler refuses the same input.
  CHECK(has(r, "mismatch", "must match its folder 'mismatch'"));
  CHECK(r.errors() > 0);

  // A theme folder whose theme.xml name differs from the folder: packs under its declared name.
  std::string one = tempDir("sq_skinc_single");
  put(one, "theme.xml", R"~(<Theme name="solo" format="1"><Metrics><Metric name="M" value="1"/><Metric name="M" value="2"/></Metrics></Theme>)~");
  Options s;
  s.inputs = {one};
  s.validateOnly = true;
  Result rs = compile(s);
  dump(rs);
  CHECK(rs.ok);
  CHECK(has(rs, "solo/theme.xml", "defined twice", false));
  s.strict = true;
  CHECK(!compile(s).ok);  // the warning fails --strict
  Options twice = s;
  twice.strict = false;
  twice.inputs = {one, one};
  CHECK(!compile(twice).ok);
  Options missing;
  missing.inputs = {sq::joinPath(dir, "nope")};
  missing.validateOnly = true;
  CHECK(!compile(missing).ok);
  // "current" is reserved by theme://current/.
  std::string cur = tempDir("sq_skinc_current");
  put(cur, "theme.xml", R"~(<Theme name="current" format="1"/>)~");
  Options co;
  co.inputs = {cur};
  co.validateOnly = true;
  Result rc = compile(co);
  CHECK(!rc.ok);
  CHECK(has(rc, "theme.xml", "not \"current\""));
  sq_fs_remove_all(cur.c_str());
  sq_fs_remove_all(dir.c_str());
  sq_fs_remove_all(one.c_str());
}

SQ_TEST("skinc: command line front end") {
  std::string dir = tempDir("sq_skinc_cli");
  std::string out = sq::joinPath(dir, "x.sqtheme");
  std::string themes = kThemes;
  std::vector<std::string> args = {"sq-skinc", "-q", "--strict", "-o", out, themes};
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  std::FILE* devnull = std::tmpfile();
  CHECK_EQ(runCli(static_cast<int>(argv.size()), argv.data(), devnull, devnull), 0);
  CHECK(sq_fs_is_file(out.c_str()));
  std::vector<std::string> bad = {"sq-skinc", "--bogus"};
  std::vector<char*> bv;
  for (auto& a : bad) bv.push_back(a.data());
  CHECK_EQ(runCli(static_cast<int>(bv.size()), bv.data(), devnull, devnull), 2);
  std::vector<std::string> noOut = {"sq-skinc", themes};
  std::vector<char*> nv;
  for (auto& a : noOut) nv.push_back(a.data());
  CHECK_EQ(runCli(static_cast<int>(nv.size()), nv.data(), devnull, devnull), 2);
  if (devnull) std::fclose(devnull);
  sq_fs_remove_all(dir.c_str());
}
