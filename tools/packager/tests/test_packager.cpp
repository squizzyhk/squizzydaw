// tools/packager: sq-pack build -> verify -> unpack -> build round trips (byte identical) for every
// container, inspection output, ABI / app gating of extensions and script packages, rejection of
// broken input, and the real sq-pack executable's exit codes.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sq/platform.hpp"
#include "sq/tools/packager/packager.hpp"
#include "sqtest.hpp"
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace sq::packager;
namespace sf = sq::formats;

namespace {

std::string tempDir(const std::string& tag) {
  std::string d = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest_tools_packager"), tag);
  sq_fs_remove_all(d.c_str());
  sq_fs_create_dirs(d.c_str());
  return d;
}
void put(const std::string& dir, const std::string& rel, const std::string& data) {
  REQUIRE(sf::writeFileBytes(sq::joinPath(dir, rel), data).ok());
}
std::string get(const std::string& path) {
  std::string s;
  REQUIRE(sf::readFileBytes(path, s).ok());
  return s;
}
int cli(std::vector<std::string> args, std::string* outText = nullptr) {
  std::string out, err;
  int code = runCli(args, out, err);
  if (outText) *outText = out + err;
  return code;
}
bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

const char* kManifest = R"(<?xml version="1.0" encoding="UTF-8"?>
<ScriptPackage id="test.pack" name="Pack Test" version="1.0.0" abi="1.0">
  <ClassFactory>
    <ScriptClass classID="{7A0C3E52-6D1B-4F8A-9E27-3C5B1D0F4A68}" category="Command" name="Pack: Hello"
                 sourceFile="src/hello.js" functionName="createHello">
      <Command category="Pack" name="Hello"/>
    </ScriptClass>
  </ClassFactory>
</ScriptPackage>
)";

std::string scriptTree(const std::string& tag) {
  std::string d = tempDir(tag);
  put(d, "package.xml", kManifest);
  put(d, "src/hello.js", "function createHello() { return { interpretCommand() { return 0; } }; }\n");
  put(d, "res/notes.txt", "binary-safe \x01\x02 payload");
  return d;
}

// Minimal PNG header (signature + IHDR) for pngDimensions; the pixel data is irrelevant here.
std::string fakePng(uint32_t w, uint32_t h) {
  std::string s("\x89PNG\r\n\x1a\n", 8);
  auto be32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); };
  be32(13);
  s += "IHDR";
  be32(w);
  be32(h);
  s += std::string("\x08\x06\x00\x00\x00", 5);
  be32(0);  // CRC (not checked by the header probe)
  return s;
}

}  // namespace

SQ_TEST("sq-pack: script bundle build is deterministic, verifies, and unpack -> build is byte identical") {
  std::string src = scriptTree("script_src");
  std::string a, b;
  REQUIRE(build(BuildType::Script, src, a).ok());
  REQUIRE(build(BuildType::Script, src, b).ok());
  CHECK(a == b);
  CHECK(sf::packageProbe(a));
  std::string work = tempDir("script_work");
  std::string pkg = sq::joinPath(work, "test.package");
  REQUIRE(sf::writeFileBytes(pkg, a).ok());

  VerifyOptions vo;
  vo.canonical = true;
  std::string report;
  Status s = verify(pkg, vo, report);
  if (!s.ok()) std::printf("%s\n", report.c_str());
  REQUIRE(s.ok());
  CHECK(has(report, "format: script-package"));
  CHECK(has(report, "kind: script"));
  CHECK(has(report, "content: bundle"));
  CHECK(has(report, "package-id: test.pack"));
  CHECK(has(report, "classes: 1"));
  CHECK(has(report, "canonical: yes"));
  CHECK(has(report, "gate: accepted"));
  CHECK(has(report, "result: ok"));

  std::string out = sq::joinPath(work, "unpacked");
  std::vector<std::string> written;
  REQUIRE(unpack(pkg, out, false, &written).ok());
  REQUIRE_EQ(written.size(), size_t(3));
  CHECK_EQ(get(sq::joinPath(out, "res/notes.txt")), std::string("binary-safe \x01\x02 payload"));
  std::string again;
  REQUIRE(build(BuildType::Script, out, again).ok());
  CHECK(again == a);
}

SQ_TEST("sq-pack: script manifest checks, ABI gate, single-file payloads, skins") {
  // missing source file
  std::string bad = scriptTree("script_missing");
  sq_fs_remove(sq::joinPath(bad, "src/hello.js").c_str());
  std::string bytes;
  Status s = build(BuildType::Script, bad, bytes);
  CHECK(!s.ok());
  CHECK(has(s.message, "src/hello.js"));
  // no manifest
  std::string none = tempDir("script_none");
  put(none, "main.js", "1");
  CHECK(!build(BuildType::Script, none, bytes).ok());
  // one top-level folder is accepted (the engine tolerates it too)
  std::string wrapped = tempDir("script_wrapped");
  put(wrapped, "inner/package.xml", kManifest);
  put(wrapped, "inner/src/hello.js", "function createHello() {}");
  CHECK(build(BuildType::Script, wrapped, bytes).ok());

  // future ABI: builds, but verify gates it (exit 3), --no-gate passes
  std::string future = scriptTree("script_future");
  std::string m = kManifest;
  m.replace(m.find("abi=\"1.0\""), 9, "abi=\"9.0\"");
  put(future, "package.xml", m);
  REQUIRE(build(BuildType::Script, future, bytes).ok());
  std::string work = tempDir("script_future_out");
  std::string pkg = sq::joinPath(work, "future.package");
  REQUIRE(sf::writeFileBytes(pkg, bytes).ok());
  std::string report;
  CHECK_EQ(verify(pkg, VerifyOptions{}, report).code, SQ_E_ABI_MISMATCH);
  CHECK(has(report, "gate: rejected"));
  CHECK_EQ(cli({"verify", pkg}), kExitAbiMismatch);
  CHECK_EQ(cli({"verify", "--no-gate", pkg}), kExitOk);
  CHECK_EQ(cli({"verify", "--abi", "9.3", pkg}), kExitOk);

  // single-file payloads: .xml -> Xml content, .js -> JavaScript content; unpack writes payload.*
  std::string single = tempDir("single");
  put(single, "manifest.xml", kManifest);
  put(single, "tool.js", "function f() {}\n");
  put(single, "broken.xml", "<ScriptPackage><oops>");
  // (a script Xml payload has no source files: only a class-less manifest is valid, see below)
  put(single, "classless.xml", "<ScriptPackage id=\"test.classless\" abi=\"1.0\"><ClassFactory/></ScriptPackage>");
  CHECK(!build(BuildType::Script, sq::joinPath(single, "manifest.xml"), bytes).ok());
  REQUIRE(build(BuildType::Script, sq::joinPath(single, "classless.xml"), bytes).ok());
  sf::ScriptPackage sp;
  REQUIRE(sf::packageRead(bytes, sp).ok());
  CHECK(sp.content == sf::PackageContent::Xml);
  // a bare .js script package could never be loaded by the engine: refused; skins may carry one
  CHECK_EQ(build(BuildType::Script, sq::joinPath(single, "tool.js"), bytes).code, SQ_E_UNSUPPORTED);
  REQUIRE(build(BuildType::Skin, sq::joinPath(single, "tool.js"), bytes).ok());
  REQUIRE(sf::packageRead(bytes, sp).ok());
  CHECK(sp.content == sf::PackageContent::JavaScript);
  std::string jsPkg = sq::joinPath(single, "tool.package");
  REQUIRE(sf::writeFileBytes(jsPkg, bytes).ok());
  REQUIRE(unpack(jsPkg, sq::joinPath(single, "out")).ok());
  CHECK_EQ(get(sq::joinPath(single, "out/payload.js")), std::string("function f() {}\n"));
  CHECK(!build(BuildType::Script, sq::joinPath(single, "broken.xml"), bytes).ok());

  // skins: any tree, but every .xml must be well-formed; kind recorded as skin
  std::string skin = tempDir("skin");
  put(skin, "skin.xml", "<Skin><Forms/></Skin>");
  put(skin, "images/knob.png", fakePng(32, 32));
  REQUIRE(build(BuildType::Skin, skin, bytes).ok());
  REQUIRE(sf::packageRead(bytes, sp).ok());
  CHECK(sp.kind == sf::PackageKind::Skin);
  put(skin, "bad.xml", "<Skin>");
  CHECK(!build(BuildType::Skin, skin, bytes).ok());
}

SQ_TEST("sq-pack: langpack from .po sources; unpack (mo or po) -> build is byte identical") {
  std::string src = tempDir("lang_src");
  sf::MetaInfo mi;
  mi.set(sf::meta::kId, "squizzy.lang.de");
  mi.set(sf::meta::kLanguage, "de");
  mi.set(sf::meta::kName, "Deutsch");
  put(src, "metainfo.xml", sf::metaInfoToXml(mi));
  put(src, "catalogs/app.po",
      "msgid \"\"\nmsgstr \"Content-Type: text/plain; charset=UTF-8\\n\"\n\nmsgid \"Play\"\nmsgstr \"Abspielen\"\n\n"
      "msgctxt \"menu\"\nmsgid \"File\"\nmsgstr \"Datei\"\n");
  put(src, "properties/ui.properties", "title = Squizzy\nsize: gro\\u00DF\n");
  std::string a;
  REQUIRE(build(BuildType::LangPack, src, a).ok());
  std::string work = tempDir("lang_work");
  std::string lp = sq::joinPath(work, "de.langpack");
  REQUIRE(sf::writeFileBytes(lp, a).ok());
  std::string report;
  VerifyOptions vo;
  vo.canonical = true;
  REQUIRE(verify(lp, vo, report).ok());
  CHECK(has(report, "format: langpack"));
  CHECK(has(report, "language: de"));
  CHECK(has(report, "app (3 messages)"));  // header entry + 2
  CHECK(has(report, "ui (2 keys)"));
  for (bool po : {false, true}) {
    std::string out = sq::joinPath(work, po ? "po" : "mo");
    REQUIRE(unpack(lp, out, po).ok());
    CHECK(sq_fs_is_file(sq::joinPath(out, po ? "catalogs/app.po" : "catalogs/app.mo").c_str()));
    std::string again;
    REQUIRE(build(BuildType::LangPack, out, again).ok());
    CHECK(again == a);
  }
  sf::LangPack back;
  REQUIRE(sf::langPackRead(a, back).ok());
  REQUIRE(back.catalogs["app"].find("File", "menu"));
  CHECK_EQ(*back.catalogs["app"].find("File", "menu"), std::string("Datei"));
  // .po and .mo for the same module, or a stray file: refused
  put(src, "catalogs/app.mo", "x");
  CHECK(!build(BuildType::LangPack, src, a).ok());
  sq_fs_remove(sq::joinPath(src, "catalogs/app.mo").c_str());
  put(src, "notes.txt", "stray");
  CHECK(!build(BuildType::LangPack, src, a).ok());
}

SQ_TEST("sq-pack: every preset kind round trips; kind mismatch and stray files are refused") {
  const struct {
    sf::PresetKind kind;
    BuildType type;
    const char* ext;
    int slots;
  } kinds[] = {{sf::PresetKind::Preset, BuildType::Preset, "preset", 1},
               {sf::PresetKind::MultiPreset, BuildType::MultiPreset, "multipreset", 2},
               {sf::PresetKind::Instrument, BuildType::Instrument, "instrument", 1},
               {sf::PresetKind::TrackPreset, BuildType::TrackPreset, "trackpreset", 2}};
  for (const auto& k : kinds) {
    sf::Preset p;
    p.kind = k.kind;
    p.meta.set(sf::meta::kId, std::string("test.") + k.ext);
    p.meta.set(sf::meta::kName, std::string("Test ") + k.ext);
    sf::setValue(p.settings, "volume", "-3.5");
    for (int i = 0; i < k.slots; ++i) {
      sf::PresetSlot s;
      s.classId = "{0F6E8A41-2B7C-4D93-A15E-6C8D2F0B7E39}";
      s.name = "Gain " + std::to_string(i);
      s.format = "clap";
      s.params = {{"gain", 0.25 * i}};
      s.state = std::string("\0state\xFF", 7) + std::to_string(i);
      p.slots.push_back(s);
    }
    p.attachments.push_back(sf::FileEntry{"readme.txt", "hello"});
    std::string bytes;
    REQUIRE(sf::presetWrite(p, bytes).ok());
    std::string work = tempDir(std::string("preset_") + k.ext);
    std::string file = sq::joinPath(work, std::string("x.") + k.ext);
    REQUIRE(sf::writeFileBytes(file, bytes).ok());
    std::string report;
    VerifyOptions vo;
    vo.canonical = true;
    Status vs = verify(file, vo, report);
    if (!vs.ok()) std::printf("%s\n", report.c_str());
    REQUIRE(vs.ok());
    CHECK(has(report, std::string("kind: ") + k.ext));
    CHECK(has(report, "slots: " + std::to_string(k.slots)));
    std::string tree = sq::joinPath(work, "tree");
    REQUIRE(unpack(file, tree).ok());
    std::string again;
    Status bs = build(k.type, tree, again);
    if (!bs.ok()) std::printf("%s\n", bs.message.c_str());
    REQUIRE(bs.ok());
    CHECK(again == bytes);  // formats writer output is already canonical
    BuildType wrong = k.type == BuildType::Preset ? BuildType::TrackPreset : BuildType::Preset;
    CHECK(!build(wrong, tree, again).ok());
    put(tree, "stray.bin", "x");
    CHECK(!build(k.type, tree, again).ok());
  }
}

SQ_TEST("sq-pack: snapshot packages (auto from images, then from snapshots.json) round trip") {
  std::string src = tempDir("snap_src");
  put(src, "plugins/gain.png", fakePng(64, 48));
  put(src, "plugins/gain@2x.png", fakePng(128, 96));
  std::string a;
  BuildOptions bo;
  bo.info = {{"name", "Test Snapshots"}, {"vendor", "Squizzy"}};
  REQUIRE(build(BuildType::Snapshot, src, a, bo).ok());
  std::string work = tempDir("snap_work");
  std::string file = sq::joinPath(work, "snaps.package");
  REQUIRE(sf::writeFileBytes(file, a).ok());
  std::string report;
  VerifyOptions vo;
  vo.canonical = true;
  REQUIRE(verify(file, vo, report).ok());
  CHECK(has(report, "format: snapshot-package"));
  CHECK(has(report, "info.name: Test Snapshots"));
  CHECK(has(report, "plugins/gain plugins/gain.png 64x48 @1x"));
  CHECK(has(report, "plugins/gain@2x plugins/gain@2x.png 128x96 @2x"));
  std::string tree = sq::joinPath(work, "tree");
  REQUIRE(unpack(file, tree).ok());
  CHECK(sq_fs_is_file(sq::joinPath(tree, "snapshots.json").c_str()));
  std::string again;
  REQUIRE(build(BuildType::Snapshot, tree, again).ok());
  CHECK(again == a);
  put(tree, "plugins/unlisted.png", fakePng(1, 1));
  CHECK(!build(BuildType::Snapshot, tree, again).ok());
  std::string junk = tempDir("snap_junk");
  put(junk, "readme.txt", "no images");
  CHECK(!build(BuildType::Snapshot, junk, again).ok());
}

SQ_TEST("sq-pack: extensions - init, verify with the ABI / app / platform gate, install, scan, uninstall") {
  std::string root = tempDir("ext");
  std::string src = sq::joinPath(root, "src/good");
  std::string dst = sq::joinPath(root, "installed");
  std::string out;
  REQUIRE_EQ(cli({"ext", "init", src, "--id", "squizzy.test.good", "--version", "1.2.0", "--name", "Good",
                  "--abi-min", "1.0", "--abi-max", "1.*"},
                 &out),
             kExitOk);
  put(src, "scripts/main.js", "// payload");
  CHECK_EQ(cli({"verify", src}, &out), kExitOk);
  CHECK(has(out, "format: extension"));
  CHECK(has(out, "id: squizzy.test.good"));
  CHECK(has(out, "gate: accepted"));
  CHECK_EQ(cli({"verify", "--abi", "2.0", src}, &out), kExitAbiMismatch);
  CHECK(has(out, "gate: rejected"));
  CHECK_EQ(cli({"verify", "--app", "other.app", src}), kExitUnsupported);
  CHECK_EQ(cli({"inspect", src}), kExitOk);  // inspect reports, never gates

  // platform-specific target
  std::string mac = sq::joinPath(root, "src/maconly");
  REQUIRE_EQ(cli({"ext", "init", mac, "--id", "squizzy.test.mac", "--version", "1", "--os", "macos", "--arch", "arm64"}), kExitOk);
  CHECK_EQ(cli({"verify", "--os", "windows", "--arch", "x64", mac}), kExitUnsupported);
  CHECK_EQ(cli({"verify", "--os", "macos", "--arch", "arm64", mac}), kExitOk);

  // install / scan / uninstall; a future-ABI extension is refused and nothing is written
  REQUIRE_EQ(cli({"ext", "install", src, dst}, &out), kExitOk);
  CHECK(sq_fs_is_file(sq::joinPath(dst, "squizzy.test.good/scripts/main.js").c_str()));
  std::string future = sq::joinPath(root, "src/future");
  REQUIRE_EQ(cli({"ext", "init", future, "--id", "squizzy.test.future", "--version", "1", "--abi-min", "9.0", "--abi-max", "9.*"}), kExitOk);
  CHECK_EQ(cli({"ext", "install", future, dst}), kExitAbiMismatch);
  CHECK(!sq_fs_exists(sq::joinPath(dst, "squizzy.test.future").c_str()));
  CHECK_EQ(cli({"ext", "scan", dst}, &out), kExitOk);
  CHECK(has(out, "accepted squizzy.test.good 1.2.0"));
  CHECK_EQ(cli({"ext", "scan", "--abi", "2.0", dst}, &out), kExitAbiMismatch);  // host upgrade invalidates it
  CHECK(has(out, "rejected"));
  CHECK_EQ(cli({"ext", "uninstall", dst, "squizzy.test.good"}), kExitOk);
  CHECK(!sq_fs_exists(sq::joinPath(dst, "squizzy.test.good").c_str()));
  // bad init input
  CHECK(cli({"ext", "init", sq::joinPath(root, "src/bad"), "--id", "../evil", "--version", "1"}) != kExitOk);
  CHECK_EQ(cli({"ext", "init", sq::joinPath(root, "src/bad2"), "--id", "x", "--version", "1", "--abi-min", "zz"}), kExitUsage);
}

SQ_TEST("sq-pack: dsppreset and plain-XML data inspect / verify canonically") {
  std::string d = tempDir("data");
  sf::DspPreset dp;
  dp.name = "Vocal Chain";
  dp.deviceVendor = "Squizzy";
  dp.deviceModel = "Box 2";
  sf::DspBlock blk;
  blk.id = "eq1";
  blk.type = "eq";
  blk.params = {{"gain", sf::DspValue::num(1.5)}, {"on", sf::DspValue::boolean(true)}};
  dp.blocks.push_back(blk);
  std::string json;
  REQUIRE(sf::dspPresetWrite(dp, json).ok());
  put(d, "vocal.dsppreset", json);
  sf::ChordSet cs;
  cs.name = "Basics";
  cs.chords = {{"major", "M", {0, 4, 7}}, {"minor", "m", {0, 3, 7}}};
  put(d, "basics.chords", sf::chordSetToXml(cs));
  std::string out;
  CHECK_EQ(cli({"verify", "--canonical", sq::joinPath(d, "vocal.dsppreset")}, &out), kExitOk);
  CHECK(has(out, "format: dsppreset"));
  CHECK(has(out, "blocks: 1"));
  CHECK_EQ(cli({"verify", "--canonical", sq::joinPath(d, "basics.chords")}, &out), kExitOk);
  CHECK(has(out, "chords: 2"));
  put(d, "broken.chords", "<ChordSet format=\"99\"/>");
  CHECK(cli({"verify", sq::joinPath(d, "broken.chords")}) != kExitOk);
  CHECK_EQ(cli({"unpack", sq::joinPath(d, "vocal.dsppreset"), "-o", sq::joinPath(d, "x")}), kExitUnsupported);
}

SQ_TEST("sq-pack: corrupted and truncated containers fail verification") {
  std::string src = scriptTree("corrupt_src");
  std::string a;
  REQUIRE(build(BuildType::Script, src, a).ok());
  std::string d = tempDir("corrupt");
  std::string flipped = a;
  flipped[flipped.size() - 3] = static_cast<char>(flipped[flipped.size() - 3] ^ 0x40);
  put(d, "flipped.package", flipped);
  put(d, "cut.package", a.substr(0, a.size() / 2));
  std::string out;
  CHECK_EQ(cli({"verify", sq::joinPath(d, "flipped.package")}, &out), kExitInvalid);
  CHECK(has(out, "result: FAILED"));
  CHECK_EQ(cli({"verify", sq::joinPath(d, "cut.package")}), kExitInvalid);
  // a langpack cut in half
  std::string lsrc = tempDir("corrupt_lang");
  sf::MetaInfo mi;
  mi.set(sf::meta::kId, "x");
  mi.set(sf::meta::kLanguage, "fr");
  put(lsrc, "metainfo.xml", sf::metaInfoToXml(mi));
  put(lsrc, "properties/a.properties", "k=v\n");
  std::string lp;
  REQUIRE(build(BuildType::LangPack, lsrc, lp).ok());
  put(d, "cut.langpack", lp.substr(0, lp.size() - 10));
  CHECK_EQ(cli({"verify", sq::joinPath(d, "cut.langpack")}), kExitInvalid);
  CHECK_EQ(cli({"verify", sq::joinPath(d, "missing.package")}), kExitInvalid);
  CHECK_EQ(cli({"bogus"}), kExitUsage);
  CHECK_EQ(cli({"build", "nonsense", src, "-o", sq::joinPath(d, "x")}), kExitUsage);
  CHECK_EQ(cli({"build", "script", src}), kExitUsage);  // no -o
}

SQ_TEST("sq-pack: the real executable builds, verifies and reports exit codes") {
  std::string exe = sq::joinPath(sq::specialDir(SQ_DIR_EXE), "sq-pack");
#if defined(_WIN32)
  exe += ".exe";
#endif
  if (!sq_fs_is_file(exe.c_str())) SQ_SKIP("sq-pack executable not built next to the test");
  std::string src = scriptTree("exe_src");
  std::string d = tempDir("exe_out");
  std::string pkg = sq::joinPath(d, "exe.package");
  std::string log = sq::joinPath(d, "log.txt");
  auto run = [&](const std::string& args) {
    std::string cmd = "\"\"" + exe + "\" " + args + " > \"" + log + "\" 2>&1\"";
#if !defined(_WIN32)
    cmd = "\"" + exe + "\" " + args + " > \"" + log + "\" 2>&1";
#endif
    int rc = std::system(cmd.c_str());
#if !defined(_WIN32)
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    return rc;
  };
  CHECK_EQ(run("build script \"" + src + "\" -o \"" + pkg + "\""), 0);
  CHECK(has(get(log), "built script"));
  std::string lib;
  REQUIRE(build(BuildType::Script, src, lib).ok());
  CHECK(get(pkg) == lib);  // CLI and library produce the same bytes
  CHECK_EQ(run("verify --canonical \"" + pkg + "\""), 0);
  CHECK(has(get(log), "result: ok"));
  CHECK_EQ(run("verify --abi 2.0 \"" + pkg + "\""), kExitAbiMismatch);
  CHECK_EQ(run("frobnicate"), kExitUsage);
}

// ---- verifier hardening (adversarial review) ------------------------------------------------------

SQ_TEST("sq-pack: script packages the engine cannot load are refused by build and verify") {
  std::string d = tempDir("unloadable");
  // Xml payload with a ScriptClass: its sourceFile can never be in the container
  sf::ScriptPackage xp;
  xp.kind = sf::PackageKind::Script;
  xp.content = sf::PackageContent::Xml;
  xp.text = kManifest;
  std::string bytes;
  REQUIRE(sf::packageWrite(xp, bytes).ok());
  put(d, "manifest.package", bytes);
  std::string report;
  Status s = verify(sq::joinPath(d, "manifest.package"), VerifyOptions{}, report);
  CHECK(!s.ok());
  CHECK(has(s.message, "src/hello.js"));
  CHECK_EQ(cli({"verify", sq::joinPath(d, "manifest.package")}), kExitInvalid);
  // bare JavaScript payload in a script package: verify rejects (gate), --no-gate only reports it
  sf::ScriptPackage jp;
  jp.kind = sf::PackageKind::Script;
  jp.content = sf::PackageContent::JavaScript;
  jp.text = "function f() {}\n";
  REQUIRE(sf::packageWrite(jp, bytes).ok());
  put(d, "bare.package", bytes);
  CHECK_EQ(cli({"verify", sq::joinPath(d, "bare.package")}), kExitUnsupported);
  std::string out;
  CHECK_EQ(cli({"verify", "--no-gate", sq::joinPath(d, "bare.package")}, &out), kExitOk);
  CHECK(has(out, "loadable: no"));
  // package ids the engine refuses (they end up in package:// URLs)
  for (const char* id : {"bad id", "-lead", ".hidden", "a/b"}) {
    std::string tree = scriptTree("badid");
    std::string m = kManifest;
    m.replace(m.find("id=\"test.pack\""), 14, std::string("id=\"") + id + "\"");
    put(tree, "package.xml", m);
    CHECK(!build(BuildType::Script, tree, bytes).ok());
  }
  // no id: the file name becomes the id, so verify checks it
  std::string tree = scriptTree("noid");
  std::string m = kManifest;
  m.replace(m.find("id=\"test.pack\" "), 15, "");
  put(tree, "package.xml", m);
  REQUIRE(build(BuildType::Script, tree, bytes).ok());
  put(d, "good-name.package", bytes);
  put(d, "bad name.package", bytes);
  CHECK(verify(sq::joinPath(d, "good-name.package"), VerifyOptions{}, report).ok());
  CHECK(!verify(sq::joinPath(d, "bad name.package"), VerifyOptions{}, report).ok());
}

SQ_TEST("sq-pack: unpack refuses member names that would collide or are not portable, before writing") {
  std::string d = tempDir("collide");
  auto pack = [&](const std::string& file, std::vector<sf::FileEntry> files) {
    sf::ScriptPackage p;
    p.kind = sf::PackageKind::Skin;
    p.content = sf::PackageContent::Bundle;
    p.files = std::move(files);
    std::string bytes;
    REQUIRE(sf::packageWrite(p, bytes).ok());
    put(d, file, bytes);
    return sq::joinPath(d, file);
  };
  struct Case {
    const char* file;
    std::vector<sf::FileEntry> files;
  } cases[] = {
      {"case.package", {{"aaa.txt", "keep"}, {"Readme.txt", "one"}, {"README.txt", "two"}}},
      {"filedir.package", {{"aaa.txt", "keep"}, {"x", "file"}, {"x/y.txt", "in dir"}}},
      {"device.package", {{"aaa.txt", "keep"}, {"aux.txt", "dev"}}},
      {"dot.package", {{"aaa.txt", "keep"}, {"name.", "trailing dot"}}},
  };
  for (auto& c : cases) {
    std::string file = pack(c.file, c.files);
    std::string out = sq::joinPath(d, std::string("out_") + c.file);
    Status s = unpack(file, out);
    CHECK(!s.ok());
    CHECK(!sq_fs_is_file(sq::joinPath(out, "aaa.txt").c_str()));  // nothing written
  }
  // unpacking over a non-empty directory would mix stale files into the next build
  std::string src = scriptTree("stale_src");
  std::string bytes;
  REQUIRE(build(BuildType::Script, src, bytes).ok());
  put(d, "s.package", bytes);
  std::string out = sq::joinPath(d, "stale_out");
  put(out, "old.txt", "stale");
  CHECK_EQ(unpack(sq::joinPath(d, "s.package"), out).code, SQ_E_EXISTS);
  sq_fs_remove(sq::joinPath(out, "old.txt").c_str());
  REQUIRE(unpack(sq::joinPath(d, "s.package"), out).ok());  // empty existing dir is fine
  std::string again;
  REQUIRE(build(BuildType::Script, out, again).ok());
  CHECK(again == bytes);
}
