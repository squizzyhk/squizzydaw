// formats/core: write -> read round trips for every container, plus hostile-input rejection.
#include <cstring>

#include "sq/formats/core/all.hpp"
#include "sqtest.hpp"

using namespace sq::formats;

SQ_TEST("formats: zip round trip, unsafe names and probe") {
  ZipArchive z;
  CHECK(z.add("a.txt", "hello"));
  CHECK(z.add("dir/b.bin", std::string("\0\1\2\3", 4)));
  CHECK(!z.add("../evil", "x"));
  CHECK(!z.add("/abs", "x"));
  CHECK(!z.add("c:/x", "x"));
  CHECK(!z.add("a\\b", "x"));
  std::string bytes;
  REQUIRE(zipWrite(z, bytes).ok());
  CHECK(zipProbe(bytes));
  std::string bytes2;
  REQUIRE(zipWrite(z, bytes2).ok());
  CHECK(bytes == bytes2);  // deterministic
  ZipArchive back;
  REQUIRE(zipRead(bytes, back).ok());
  REQUIRE(back.find("dir/b.bin"));
  CHECK_EQ(back.find("dir/b.bin")->size(), size_t{4});
  CHECK_EQ(*back.find("a.txt"), std::string("hello"));
  CHECK_EQ(back.withPrefix("dir/").size(), size_t{1});
  ZipLimits tiny;
  tiny.maxEntrySize = 3;
  CHECK(!zipRead(bytes, back, tiny).ok());
  CHECK(!zipRead("PK\x03\x04garbage", back).ok());
  std::string zc, zd;
  REQUIRE(zlibCompress(std::string(1000, 'a'), zc).ok());
  REQUIRE(zlibDecompress(zc, 1000, zd).ok());
  CHECK_EQ(zd, std::string(1000, 'a'));
  CHECK(!zlibDecompress(zc, 999, zd).ok());
}

SQ_TEST("formats: script package xml / js / bundle") {
  for (PackageContent c : {PackageContent::Xml, PackageContent::JavaScript, PackageContent::Bundle}) {
    ScriptPackage p;
    p.kind = c == PackageContent::Xml ? PackageKind::Skin : PackageKind::Script;
    p.content = c;
    if (c == PackageContent::Bundle) p.files = {{"main.js", "1+1"}, {"res/a.xml", "<a/>"}};
    else p.text = c == PackageContent::Xml ? "<ClassFactory/>" : "function f(){}";
    std::string bytes;
    REQUIRE(packageWrite(p, bytes).ok());
    CHECK(packageProbe(bytes));
    ScriptPackage back;
    REQUIRE(packageRead(bytes, back).ok());
    CHECK(back.kind == p.kind);
    CHECK(back.content == p.content);
    CHECK_EQ(back.text, p.text);
    CHECK_EQ(back.files.size(), p.files.size());
    // Corrupt one payload byte: the CRC must catch it.
    std::string bad = bytes;
    bad[bad.size() - 5] = static_cast<char>(bad[bad.size() - 5] ^ 0x55);
    CHECK(!packageRead(bad, back).ok());
    CHECK(!packageRead(bytes.substr(0, 20), back).ok());
  }
}

SQ_TEST("formats: preset kinds round trip with state blobs") {
  Preset p;
  p.kind = PresetKind::TrackPreset;
  p.meta.set(meta::kName, "Lead Vocal");
  p.meta.set(meta::kVendor, "me");
  setValue(p.settings, "volume", "-3.5");
  PresetSlot s1;
  s1.classId = "{00000000-1111-2222-3333-444444444444}";
  s1.name = "Gain";
  s1.format = "clap";
  s1.params = {{"gain", 0.5}, {"mix", 1.0}};
  s1.state = std::string("\0\xFF state", 8);
  PresetSlot s2 = s1;
  s2.bypassed = true;
  s2.state = "second";
  p.slots = {s1, s2};
  p.attachments = {{"notes.txt", "hi"}};
  std::string zip;
  REQUIRE(presetWrite(p, zip).ok());
  Preset back;
  REQUIRE(presetRead(zip, back).ok());
  CHECK(back.kind == PresetKind::TrackPreset);
  CHECK_EQ(back.meta.get(meta::kName), std::string("Lead Vocal"));
  REQUIRE_EQ(back.slots.size(), size_t{2});
  CHECK_EQ(back.slots[0].state, s1.state);
  CHECK(back.slots[1].bypassed);
  CHECK_EQ(back.slots[0].params.size(), size_t{2});
  CHECK_EQ(back.attachments.size(), size_t{1});
  // A single-slot kind with two slots is invalid.
  p.kind = PresetKind::Preset;
  CHECK(!presetWrite(p, zip).ok());
}

SQ_TEST("formats: langpack with gettext catalogs and properties") {
  Catalog c;
  REQUIRE(poParse("msgid \"\"\nmsgstr \"Content-Type: text/plain; charset=UTF-8\\n\"\n\n"
                  "msgid \"Play\"\nmsgstr \"Abspielen\"\n\nmsgctxt \"menu\"\nmsgid \"File\"\nmsgstr \"Datei\"\n\n"
                  "#, fuzzy\nmsgid \"Skip\"\nmsgstr \"X\"\n",
                  c)
              .ok());
  REQUIRE(c.find("Play"));
  CHECK_EQ(*c.find("Play"), std::string("Abspielen"));
  CHECK_EQ(*c.find("File", "menu"), std::string("Datei"));
  CHECK(c.find("Skip") == nullptr);
  std::string mo;
  REQUIRE(moWrite(c, mo).ok());
  Catalog c2;
  REQUIRE(moRead(mo, c2).ok());
  CHECK(c2.messages == c.messages);

  LangPack lp;
  lp.meta.set(meta::kId, "de-pack");
  lp.meta.set(meta::kLanguage, "de");
  lp.catalogs["app"] = c;
  KeyValues props;
  REQUIRE(propertiesParse("a = 1\nb: zw\\u00F6lf\n# c\nlong = x \\\n  y\n", props).ok());
  CHECK_EQ(*findValue(props, "b"), std::string("zw\xC3\xB6lf"));
  CHECK_EQ(*findValue(props, "long"), std::string("x y"));
  lp.properties["ui"] = props;
  std::string zip;
  REQUIRE(langPackWrite(lp, zip).ok());
  LangPack back;
  REQUIRE(langPackRead(zip, back).ok());
  CHECK(back.catalogs["app"].messages == c.messages);
  CHECK(back.properties["ui"] == props);
  LangPack noLang;
  noLang.meta.set(meta::kId, "x");
  CHECK(!langPackWrite(noLang, zip).ok());
}

SQ_TEST("formats: dsppreset json, snapshots, plain xml data") {
  DspPreset d;
  d.name = "Room";
  d.deviceVendor = "v";
  d.deviceModel = "m";
  d.blocks.push_back(DspBlock{"eq1", "eq", true, {{"gain", DspValue::num(1.5)}, {"on", DspValue::boolean(true)}, {"mode", DspValue::str("hi")}}});
  std::string json;
  REQUIRE(dspPresetWrite(d, json).ok());
  DspPreset db;
  REQUIRE(dspPresetRead(json, db).ok());
  REQUIRE_EQ(db.blocks.size(), size_t{1});
  CHECK(db.blocks[0].params == d.blocks[0].params);
  CHECK(!dspPresetRead("{\"version\": 99}", db).ok());

  // 1x1 PNG header is enough for the dimension probe.
  std::string png("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\0\x20\0\0\0\x10\x08\x06\0\0\0", 29);
  SnapshotPackage sp;
  sp.info = {{"name", "thumbs"}};
  sp.snapshots.push_back(Snapshot{"plugin.a", "images/a.png", 0, 0, 2.0, png});
  std::string zip;
  REQUIRE(snapshotPackageWrite(sp, zip).ok());
  SnapshotPackage spb;
  REQUIRE(snapshotPackageRead(zip, spb).ok());
  REQUIRE_EQ(spb.snapshots.size(), size_t{1});
  CHECK_EQ(spb.snapshots[0].width, 32u);
  CHECK_EQ(spb.snapshots[0].scale, 2.0);

  ChordSet cs{"basic", {{"major", "M", {0, 4, 7}}, {"minor7", "m7", {0, 3, 7, 10}}}};
  ChordSet csb;
  REQUIRE(chordSetFromXml(chordSetToXml(cs), csb).ok());
  CHECK_EQ(csb.chords.size(), size_t{2});
  CHECK(csb.chords[1].intervals == cs.chords[1].intervals);
  MusicalScale ms{"pent", 1200.0, {{0, "1"}, {200, "2"}, {400, "3"}, {700, "5"}, {900, "6"}}};
  MusicalScale msb;
  REQUIRE(musicalScaleFromXml(musicalScaleToXml(ms), msb).ok());
  CHECK_EQ(msb.degrees.size(), size_t{5});
  ColorScheme sc{"dark", "dark", {{"ui.accent", 0x4FB3A9FFu}}};
  ColorScheme scb;
  REQUIRE(colorSchemeFromXml(colorSchemeToXml(sc), scb).ok());
  CHECK_EQ(scb.colors[0].rgba, 0x4FB3A9FFu);
  CHECK(!chordSetFromXml("<nope/>", csb).ok());
}

SQ_TEST("formats: detect formats from name and bytes") {
  ScriptPackage p;
  p.text = "<a/>";
  std::string pkg;
  REQUIRE(packageWrite(p, pkg).ok());
  CHECK(detectFormat("x.package", pkg) == FileFormat::ScriptPackage);
  SnapshotPackage sp;
  std::string snap;
  REQUIRE(snapshotPackageWrite(sp, snap).ok());
  CHECK(detectFormat("x.package", snap) == FileFormat::SnapshotPackage);
  CHECK(detectFormat("a.dsppreset", "{}") == FileFormat::DspPreset);
  CHECK(detectFormat("a.colorscheme", "<x/>") == FileFormat::ColorScheme);
  CHECK(detectFormat("a.wav", "RIFF") == FileFormat::Unknown);
}

// ---- hostile ZIP sizes (regression: zipRead pre-allocated the DECLARED size, so a 120-byte
// archive could commit a gigabyte, and bombs expanded to the 4 GiB default limit) ----
namespace {
void patchU32(std::string& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}
uint32_t readU32(const std::string& b, size_t at) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b[at + i]);
  return v;
}
// Rewrites the uncompressed size of every member (local + central headers).
void patchDeclaredSize(std::string& b, uint32_t size) {
  for (size_t i = 0; i + 4 <= b.size(); ++i) {
    const uint32_t sig = readU32(b, i);
    if (sig == 0x04034b50u && i + 26 <= b.size()) patchU32(b, i + 22, size);
    if (sig == 0x02014b50u && i + 28 <= b.size()) patchU32(b, i + 24, size);
  }
}
}  // namespace

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static uint64_t peakCommit() {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof pmc;
  return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.PeakPagefileUsage : 0;
}
#else
static uint64_t peakCommit() { return 0; }
#endif

SQ_TEST("formats: zip member lying about its size is rejected without allocating the claim") {
  ZipArchive z;
  REQUIRE(z.add("liar.txt", std::string(1000, 'a')));
  std::string bytes;
  REQUIRE(zipWrite(z, bytes).ok());
  patchDeclaredSize(bytes, 0x3FFFFFFFu);  // claims 1 GiB - 1, inside the default limits
  const uint64_t before = peakCommit();
  ZipArchive back;
  Status st = zipRead(bytes, back);
  CHECK(!st.ok());
  CHECK(back.entries.empty());
  const uint64_t grown = peakCommit() - before;
  CHECK(grown < 64ull * 1024 * 1024);  // was ~1 GiB: std::string(declared) before inflating

  // ...and the other way round: a member that inflates beyond its declared size
  std::string under;
  REQUIRE(zipWrite(z, under).ok());
  patchDeclaredSize(under, 100);
  CHECK(!zipRead(under, back).ok());
}

SQ_TEST("formats: zip bombs stop at the real output limits and the ratio guard") {
  ZipArchive z;
  for (int i = 0; i < 3; ++i) REQUIRE(z.add("zeros" + std::to_string(i) + ".bin", std::string(4u * 1024 * 1024, '\0')));
  std::string bytes;
  REQUIRE(zipWrite(z, bytes, 9).ok());
  CHECK(bytes.size() < 200u * 1024);
  ZipArchive back;
  ZipLimits total;
  total.maxTotalSize = 8u * 1024 * 1024;
  Status st = zipRead(bytes, back, total);
  CHECK(!st.ok());
  CHECK(back.entries.empty());
  ZipLimits ratio;
  ratio.maxRatio = 100;
  ratio.ratioFloor = 64 * 1024;
  CHECK(!zipRead(bytes, back, ratio).ok());
  // Real streams under the limits still read fine.
  REQUIRE(zipRead(bytes, back).ok());
  CHECK_EQ(back.entries.size(), size_t{3});
  CHECK_EQ(back.entries[2].data.size(), size_t{4u * 1024 * 1024});
  // Metadata containers use the small limits (bounded to a few hundred MB at most).
  const ZipLimits meta = ZipLimits::metadata();
  CHECK(meta.maxTotalSize <= 256ull * 1024 * 1024);
  CHECK(meta.maxRatio > 0);
  // Format detection only lists names (nothing is inflated).
  std::vector<std::string> names;
  REQUIRE(zipListNames(bytes, names).ok());
  CHECK_EQ(names.size(), size_t{3});
}

SQ_TEST("formats: zlib stream claiming an impossible size is rejected before allocating") {
  std::string zc, zd;
  REQUIRE(zlibCompress(std::string(1000, 'a'), zc).ok());
  const uint64_t before = peakCommit();
  CHECK(!zlibDecompress(zc, 0xF0000000u, zd).ok());
  CHECK(peakCommit() - before < 64ull * 1024 * 1024);
}
