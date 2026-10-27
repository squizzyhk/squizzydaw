#include "sq/formats/core/xmldata.hpp"

#include <set>

#include "sq/formats/core/xmlutil.hpp"

namespace sq::formats {

namespace {
std::string joinInts(const std::vector<int>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s.push_back(' ');
    s += std::to_string(v[i]);
  }
  return s;
}

bool splitInts(std::string_view s, std::vector<int>& out) {
  out.clear();
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == ',')) ++i;
    size_t j = i;
    while (j < s.size() && !(s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r' || s[j] == ',')) ++j;
    if (j > i) {
      int64_t v = 0;
      if (!parseInt(s.substr(i, j - i), v) || v < -128 || v > 128) return false;
      out.push_back(static_cast<int>(v));
    }
    i = j;
  }
  return true;
}
}  // namespace

std::string chordSetToXml(const ChordSet& s) {
  auto doc = xmlCreate("ChordSet");
  XmlNode root = xmlRoot(doc);
  root.setAttr("format", std::to_string(kChordSetFormat));
  root.setAttr("name", s.name);
  for (const auto& c : s.chords) {
    XmlNode n = root.append("Chord");
    n.setAttr("name", c.name);
    if (!c.symbol.empty()) n.setAttr("symbol", c.symbol);
    n.setAttr("intervals", joinInts(c.intervals));
  }
  return xmlWrite(doc);
}

Status chordSetFromXml(std::string_view xml, ChordSet& out) {
  out = ChordSet{};
  XmlDocPtr doc;
  XmlNode root;
  SQF_TRY(parseVersionedXml(xml, "ChordSet", kChordSetFormat, doc, root));
  out.name = root.attr("name");
  for (XmlNode n : root.children("Chord")) {
    ChordDef c;
    c.name = n.attr("name");
    c.symbol = n.attr("symbol");
    if (!splitInts(n.attr("intervals"), c.intervals) || c.intervals.empty() || c.intervals[0] != 0)
      return Status::error(SQ_E_PARSE, "chords: chord '" + c.name + "' needs intervals starting at 0");
    for (size_t i = 1; i < c.intervals.size(); ++i)
      if (c.intervals[i] <= c.intervals[i - 1]) return Status::error(SQ_E_PARSE, "chords: intervals must ascend in '" + c.name + "'");
    out.chords.push_back(std::move(c));
  }
  return {};
}

std::string musicalScaleToXml(const MusicalScale& s) {
  auto doc = xmlCreate("MusicalScale");
  XmlNode root = xmlRoot(doc);
  root.setAttr("format", std::to_string(kScaleFormat));
  root.setAttr("name", s.name);
  root.setAttr("periodCents", formatDouble(s.periodCents));
  for (const auto& d : s.degrees) {
    XmlNode n = root.append("Degree");
    n.setAttr("cents", formatDouble(d.cents));
    if (!d.name.empty()) n.setAttr("name", d.name);
  }
  return xmlWrite(doc);
}

Status musicalScaleFromXml(std::string_view xml, MusicalScale& out) {
  out = MusicalScale{};
  XmlDocPtr doc;
  XmlNode root;
  SQF_TRY(parseVersionedXml(xml, "MusicalScale", kScaleFormat, doc, root));
  out.name = root.attr("name");
  out.periodCents = xmlAttrDouble(root, "periodCents", 1200.0);
  if (!(out.periodCents > 0.0)) return Status::error(SQ_E_PARSE, "musicalscale: periodCents must be > 0");
  for (XmlNode n : root.children("Degree")) {
    ScaleDegree d;
    if (!parseDouble(n.attr("cents"), d.cents)) return Status::error(SQ_E_PARSE, "musicalscale: degree without cents");
    d.name = n.attr("name");
    if (d.cents < 0.0 || d.cents >= out.periodCents) return Status::error(SQ_E_PARSE, "musicalscale: degree outside the period");
    if (!out.degrees.empty() && d.cents <= out.degrees.back().cents)
      return Status::error(SQ_E_PARSE, "musicalscale: degrees must ascend");
    out.degrees.push_back(std::move(d));
  }
  if (out.degrees.empty() || out.degrees[0].cents != 0.0) return Status::error(SQ_E_PARSE, "musicalscale: first degree must be 0 cents");
  return {};
}

std::string colorSchemeToXml(const ColorScheme& s) {
  auto doc = xmlCreate("ColorScheme");
  XmlNode root = xmlRoot(doc);
  root.setAttr("format", std::to_string(kColorSchemeFormat));
  root.setAttr("name", s.name);
  if (!s.appearance.empty()) root.setAttr("appearance", s.appearance);
  for (const auto& c : s.colors) {
    XmlNode n = root.append("Color");
    n.setAttr("id", c.id);
    n.setAttr("value", formatColor(c.rgba));
  }
  return xmlWrite(doc);
}

Status colorSchemeFromXml(std::string_view xml, ColorScheme& out) {
  out = ColorScheme{};
  XmlDocPtr doc;
  XmlNode root;
  SQF_TRY(parseVersionedXml(xml, "ColorScheme", kColorSchemeFormat, doc, root));
  out.name = root.attr("name");
  out.appearance = root.attr("appearance");
  if (!out.appearance.empty() && out.appearance != "dark" && out.appearance != "light")
    return Status::error(SQ_E_PARSE, "colorscheme: appearance must be 'dark' or 'light'");
  std::set<std::string> ids;
  for (XmlNode n : root.children("Color")) {
    NamedColor c;
    c.id = n.attr("id");
    if (c.id.empty() || !ids.insert(c.id).second) return Status::error(SQ_E_PARSE, "colorscheme: empty or duplicate color id");
    if (!parseColor(n.attr("value"), c.rgba)) return Status::error(SQ_E_PARSE, "colorscheme: bad color value for '" + c.id + "'");
    out.colors.push_back(std::move(c));
  }
  return {};
}

}  // namespace sq::formats
