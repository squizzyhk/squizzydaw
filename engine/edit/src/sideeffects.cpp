// Squizzy Studio engine/edit - side effects (audio-time-follow, fade-clamp), their installation, and
// the native action registration.
#include <algorithm>
#include <new>
#include <set>

#include "common.hpp"

namespace sq::edit {
namespace {

using detail::ClipRec;
using detail::kEps;

bool placementChanged(const edoc::Change& c) {
  if (c.kind != edoc::kChangeClip) return false;
  const auto& d = std::get<edoc::Delta<ClipRec>>(c.data);
  if (!d.after) return false;
  if (!d.before) return true;
  return d.before->start != d.after->start || d.before->length != d.after->length || d.before->track != d.after->track;
}

bool userFlag(const ClipRec& c, const char* key) {
  auto it = c.user.find(key);
  if (it == c.user.end()) return false;
  const edoc::Value& v = it->second;
  return (v.type == SQ_VALUE_BOOL || v.type == SQ_VALUE_INT) ? v.i != 0 : (v.type == SQ_VALUE_STRING && v.s == "true");
}

class AudioTimeFollow final : public edoc::SideEffect {
 public:
  const char* name() const override { return "audio-time-follow"; }
  uint32_t triggers() const override { return edoc::kChangeTempo; }
  void propagate(edoc::Editing& ed, const std::vector<edoc::Change>& fresh) override {
    const edoc::TempoMapRec* oldMap = nullptr;
    size_t firstTempo = 0;
    for (size_t i = 0; i < fresh.size(); ++i) {
      if (fresh[i].kind == edoc::kChangeTempo) {
        oldMap = &*std::get<edoc::Delta<edoc::TempoMapRec>>(fresh[i].data).before;
        firstTempo = i;
        break;
      }
    }
    if (!oldMap) return;
    const edoc::TempoMapRec newMap = ed.store().tempo;
    if (*oldMap == newMap) return;
    std::set<sq_guid> placedAfter;  // positioned against the new map already
    for (size_t i = firstTempo + 1; i < fresh.size(); ++i)
      if (fresh[i].tag != edoc::kTagSideEffect && placementChanged(fresh[i])) placedAfter.insert(fresh[i].id);
    for (const sq_guid& tid : std::vector<sq_guid>(ed.store().trackOrder)) {
      const edoc::TrackRec* t = ed.store().track(tid);
      if (!t || t->timebase == "time") continue;  // engine/doc's time-map owns those
      const std::vector<sq_guid> clips = ed.store().clipsOf(tid);
      for (const sq_guid& cid : clips) {
        if (placedAfter.count(cid)) continue;
        ClipRec c = *ed.store().clip(cid);
        if (c.kind != SQ_CLIP_AUDIO || userFlag(c, "user.stretch")) continue;
        const double dur = oldMap->ppqToSeconds(c.end()) - oldMap->ppqToSeconds(c.start);
        const double end = newMap.secondsToPpq(newMap.ppqToSeconds(c.start) + dur);
        if (std::fabs(end - c.end()) < 1e-9) continue;
        c.length = std::max(end - c.start, 1e-9);
        ed.putClip(c);
      }
    }
  }
};

class FadeClamp final : public edoc::SideEffect {
 public:
  const char* name() const override { return "fade-clamp"; }
  uint32_t triggers() const override { return edoc::kChangeClip | edoc::kChangeTempo; }
  void propagate(edoc::Editing& ed, const std::vector<edoc::Change>& fresh) override {
    std::vector<sq_guid> ids;
    bool tempo = false;
    for (const edoc::Change& c : fresh) {
      if (c.kind == edoc::kChangeTempo) tempo = true;
      if (c.kind == edoc::kChangeClip && std::find(ids.begin(), ids.end(), c.id) == ids.end()) ids.push_back(c.id);
    }
    if (tempo) {  // every clip's duration in seconds may have changed
      ids.clear();
      for (const sq_guid& tid : ed.store().trackOrder)
        for (const sq_guid& cid : ed.store().clipsOf(tid)) ids.push_back(cid);
    }
    for (const sq_guid& id : ids) {
      const ClipRec* cp = ed.store().clip(id);
      if (!cp) continue;
      const double dur = detail::clipSeconds(ed.store(), *cp);
      const double sum = cp->fadeIn + cp->fadeOut;
      if (sum <= dur + kEps || sum <= 0) continue;
      ClipRec c = *cp;
      const double k = std::max(0.0, dur) / sum;
      c.fadeIn *= k;
      c.fadeOut *= k;
      ed.putClip(c);
    }
  }
};

// Marker observer: a document gets the side effects once (ISubject::addObserver answers
// SQ_S_FALSE for an observer that is already attached). It does nothing on notifications.
class InstallTag final : public Object<InstallTag, IObserver> {
 public:
  void SQ_CALL onNotify(IUnknown*, const char*, IAttributes*) override {}
};
InstallTag* installTag() {
  static InstallTag* t = new InstallTag();  // intentionally leaked: documents may outlive statics
  return t;
}

}  // namespace

std::shared_ptr<edoc::SideEffect> makeAudioTimeFollow() { return std::make_shared<AudioTimeFollow>(); }
std::shared_ptr<edoc::SideEffect> makeFadeClamp() { return std::make_shared<FadeClamp>(); }

void installSideEffects(edoc::DocumentImpl& doc) {
  if (doc.closed()) return;
  if (doc.addObserver(installTag()) != SQ_OK) return;  // already installed
  doc.addSideEffect(makeAudioTimeFollow());
  doc.addSideEffect(makeFadeClamp());
}

// ---- native actions --------------------------------------------------------------------------------
namespace {
class EditAction final : public edoc::Action {
 public:
  explicit EditAction(const detail::ActionDef& d) : def_(d) {}
  const char* name() const override { return def_.name; }
  sq_result perform(edoc::Editing& ed, IAttributes* p, IAttributes* r) override {
    if (!p) return SQ_E_INVALIDARG;
    installSideEffects(ed.document());  // lazily, for documents the host did not equip
    try {
      return def_.fn(ed, p, r);
    } catch (const std::bad_alloc&) {
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

 private:
  detail::ActionDef def_;
};
}  // namespace

std::vector<detail::ActionDef> allActionDefs() {
  std::vector<detail::ActionDef> all;
  for (auto* list : {&detail::clipActionDefs(), &detail::audioActionDefs(), &detail::timeActionDefs()})
    all.insert(all.end(), list->begin(), list->end());
  return all;
}

void registerEditActions() {
  for (const detail::ActionDef& d : allActionDefs())
    if (!edoc::findAction(d.id)) edoc::registerAction(d.id, std::make_shared<EditAction>(d));
}

}  // namespace sq::edit
