#include "sq/formats/core/preset.hpp"

#include "sq/formats/core/xmlutil.hpp"
#include "sq/formats/core/zip.hpp"

namespace sq::formats {

namespace {
constexpr std::string_view kAttachDir = "attachments/";

std::string slotStateMember(size_t i) { return "slots/" + std::to_string(i) + ".state"; }

Status checkSlots(const Preset& p) {
  size_t n = p.slots.size();
  switch (p.kind) {
    case PresetKind::Preset:
    case PresetKind::Instrument:
      if (n != 1) return Status::error(SQ_E_INVALIDARG, std::string(presetKindName(p.kind)) + " needs exactly one slot");
      break;
    case PresetKind::MultiPreset:
    case PresetKind::TrackPreset:
      if (n < 1) return Status::error(SQ_E_INVALIDARG, std::string(presetKindName(p.kind)) + " needs at least one slot");
      break;
  }
  if (n > 4096) return Status::error(SQ_E_INVALIDARG, "preset: too many slots");
  return {};
}
}  // namespace

const char* presetKindName(PresetKind k) {
  switch (k) {
    case PresetKind::Preset: return "preset";
    case PresetKind::MultiPreset: return "multipreset";
    case PresetKind::Instrument: return "instrument";
    case PresetKind::TrackPreset: return "trackpreset";
  }
  return "preset";
}

const char* presetKindExtension(PresetKind k) { return presetKindName(k); }

bool presetKindFromName(std::string_view s, PresetKind& out) {
  if (!s.empty() && s.front() == '.') s.remove_prefix(1);
  for (PresetKind k : {PresetKind::Preset, PresetKind::MultiPreset, PresetKind::Instrument, PresetKind::TrackPreset}) {
    if (s == presetKindName(k)) {
      out = k;
      return true;
    }
  }
  return false;
}

Status presetWrite(const Preset& p, std::string& zipOut) {
  SQF_TRY(checkSlots(p));
  MetaInfo meta = p.meta;
  meta.set(meta::kPresetKind, presetKindName(p.kind));
  if (!p.slots.empty() && !meta.has(meta::kPresetClass)) meta.set(meta::kPresetClass, p.slots[0].classId);

  auto doc = xmlCreate("Preset");
  XmlNode root = xmlRoot(doc);
  root.setAttr("format", std::to_string(kPresetFormat));
  root.setAttr("kind", presetKindName(p.kind));
  if (!p.settings.empty()) {
    XmlNode s = root.append("Settings");
    for (const auto& [k, v] : p.settings) {
      XmlNode e = s.append("Setting");
      e.setAttr("key", k);
      e.setAttr("value", v);
    }
  }
  ZipArchive zip;
  zip.add(kMetaInfoFile, metaInfoToXml(meta));
  for (size_t i = 0; i < p.slots.size(); ++i) {
    const PresetSlot& sl = p.slots[i];
    if (sl.classId.empty()) return Status::error(SQ_E_INVALIDARG, "preset: slot " + std::to_string(i) + " has no classId");
    XmlNode s = root.append("Slot");
    s.setAttr("index", std::to_string(i));
    s.setAttr("classId", sl.classId);
    if (!sl.name.empty()) s.setAttr("name", sl.name);
    if (!sl.format.empty()) s.setAttr("pluginFormat", sl.format);
    if (!sl.vendor.empty()) s.setAttr("vendor", sl.vendor);
    if (sl.bypassed) s.setAttr("bypassed", "true");
    for (const auto& [id, v] : sl.params) {
      XmlNode pe = s.append("Param");
      pe.setAttr("id", id);
      pe.setAttr("value", formatDouble(v));
    }
    std::string member = slotStateMember(i);
    XmlNode st = s.append("State");
    st.setAttr("member", member);
    st.setAttr("size", std::to_string(sl.state.size()));
    zip.add(member, sl.state);
  }
  for (const auto& a : p.attachments) {
    std::string name = std::string(kAttachDir) + a.name;
    if (!isSafeRelativePath(a.name) || zip.contains(name))
      return Status::error(SQ_E_INVALIDARG, "preset: invalid attachment name '" + a.name + "'");
    zip.add(name, a.data);
  }
  zip.entries.insert(zip.entries.begin() + 1, FileEntry{kPresetXml, xmlWrite(doc)});
  return zipWrite(zip, zipOut);
}

Status presetRead(std::string_view zipBytes, Preset& out) {
  out = Preset{};
  ZipArchive zip;
  SQF_TRY(zipRead(zipBytes, zip, ZipLimits::metadata()));
  const std::string* mi = zip.find(kMetaInfoFile);
  const std::string* px = zip.find(kPresetXml);
  if (!mi || !px) return Status::error(SQ_E_PARSE, "preset: missing metainfo.xml or preset.xml");
  SQF_TRY(metaInfoFromXml(*mi, out.meta));
  XmlDocPtr doc;
  XmlNode root;
  SQF_TRY(parseVersionedXml(*px, "Preset", kPresetFormat, doc, root));
  if (!presetKindFromName(root.attr("kind"), out.kind)) return Status::error(SQ_E_PARSE, "preset: unknown kind '" + root.attr("kind") + "'");
  std::string metaKind = out.meta.get(meta::kPresetKind);
  if (!metaKind.empty() && metaKind != presetKindName(out.kind))
    return Status::error(SQ_E_PARSE, "preset: metainfo kind and preset.xml kind disagree");
  for (XmlNode s : root.child("Settings").children("Setting")) setValue(out.settings, s.attr("key"), s.attr("value"));
  for (XmlNode s : root.children("Slot")) {
    PresetSlot sl;
    sl.classId = s.attr("classId");
    sl.name = s.attr("name");
    sl.format = s.attr("pluginFormat");
    sl.vendor = s.attr("vendor");
    sl.bypassed = xmlAttrBool(s, "bypassed", false);
    for (XmlNode pe : s.children("Param")) {
      double v = 0;
      if (!parseDouble(pe.attr("value"), v)) return Status::error(SQ_E_PARSE, "preset: bad parameter value");
      sl.params.emplace_back(pe.attr("id"), v);
    }
    XmlNode st = s.child("State");
    if (st) {
      std::string member = st.attr("member");
      const std::string* data = zip.find(member);
      if (!data) return Status::error(SQ_E_PARSE, "preset: state member '" + member + "' missing");
      int64_t size = xmlAttrInt(st, "size", -1);
      if (size >= 0 && static_cast<uint64_t>(size) != data->size()) return Status::error(SQ_E_PARSE, "preset: state size mismatch");
      sl.state = *data;
    }
    if (sl.classId.empty()) return Status::error(SQ_E_PARSE, "preset: slot without classId");
    out.slots.push_back(std::move(sl));
  }
  SQF_TRY(checkSlots(out));
  for (const FileEntry* e : zip.withPrefix(kAttachDir))
    out.attachments.push_back(FileEntry{e->name.substr(kAttachDir.size()), e->data});
  return {};
}

}  // namespace sq::formats
