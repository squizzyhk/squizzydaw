// engine/doc: host wiring through the real app registry/host context: IDocumentManager service,
// built-in "Action" classes, external IAction macros (as scripts would register them), and the
// object://doc/ resolver registered with a mocked IObjectRegistry (scripting-js is another agent's).
#include <map>

#include "doc_test_util.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/app/host_context.hpp"
#include "sq/app/settings.hpp"

using namespace doctest;
using sq::Ptr;

namespace {

constexpr sq_guid kMacroClass = sq::guid("{D8825DBF-FCD5-4124-B84D-B932AA6E4C42}");
constexpr sq_guid kBadMacroClass = sq::guid("{DCF78C75-AEF3-4CFA-870C-F9728D204B8F}");

// A macro like a script package would provide: composes built-ins through the editor.
class Macro final : public sq::Object<Macro, sq::IAction> {
 public:
  explicit Macro(bool fail) : fail_(fail) {}
  sq_result SQ_CALL perform(sq::IDocument*, sq::IEditor* ed, sq::IAttributes*, sq::IAttributes* result) override {
    sq_guid t = addTrack(ed, SQ_TRACK_AUDIO, "Macro A");
    addTrack(ed, SQ_TRACK_AUDIO, "Macro B");
    if (fail_) return SQ_E_FAIL;
    auto p = sq::makeAttributes();
    p->setDouble("position", 4.0);
    p->setString("name", "From macro");
    if (sq_result r = ed->execute(&sq::doc::kAddMarker, p.get(), nullptr); SQ_FAILED(r)) return r;
    if (result) result->setGuid("node", &t);
    return SQ_OK;
  }

 private:
  bool fail_;
};

class MacroFactory final : public sq::Object<MacroFactory, sq::IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getClassCount() override { return 0; }
  sq_result SQ_CALL getClassInfo(uint32_t, sq_class_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) override {
    Ptr<Macro> m = Ptr<Macro>::adopt(new Macro(*classId == kBadMacroClass));
    return m->queryInterface(riid, out);
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};

class FakeObjects final : public sq::Object<FakeObjects, sq::IObjectRegistry> {
 public:
  std::map<std::string, Ptr<sq::IUrlResolver>> resolvers;
  sq_result SQ_CALL registerResolver(const char* prefix, sq::IUrlResolver* r) override {
    if (resolvers.count(prefix)) return SQ_E_EXISTS;
    resolvers[prefix] = Ptr<sq::IUrlResolver>(r);
    return SQ_OK;
  }
  sq_result SQ_CALL unregisterResolver(const char* prefix, sq::IUrlResolver*) override {
    return resolvers.erase(prefix) ? SQ_OK : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL registerObject(const char*, sq::IUnknown*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL unregisterObject(const char*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL resolve(const char* url, const sq_guid* riid, void** out) override {
    for (auto& [p, r] : resolvers)
      if (std::string(url).compare(0, p.size(), p) == 0) return r->resolve(url, riid, out);
    return SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL openStream(const char*, sq::IByteStream**) override { return SQ_E_NOTIMPL; }
};

uint32_t countCategory(sq::IClassRegistry* reg, const char* cat) {
  uint32_t n = 0;
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  for (uint32_t i = 0; i < reg->getClassCount(); ++i)
    if (reg->getClassInfo(i, &ci) == SQ_OK && std::string(ci.category) == cat) ++n;
  return n;
}

}  // namespace

SQ_TEST("host: services, action classes, external macros, object:// resolver") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  Ptr<FakeObjects> objects = Ptr<FakeObjects>::adopt(new FakeObjects());
  REQUIRE_OK(host.registerService(&sq::IObjectRegistry::iid, static_cast<sq::IObjectRegistry*>(objects.get())));

  REQUIRE_OK(sq::edoc::registerDocumentServices(&host));
  CHECK_EQ(sq::edoc::registerDocumentServices(&host), SQ_E_EXISTS);
  CHECK_EQ(countCategory(&reg, sq::doc::kActionCategory), 15u);
  CHECK_EQ(objects->resolvers.count("object://doc/"), size_t(1));

  Ptr<sq::IDocumentManager> mgr;
  REQUIRE_OK(host.queryService(&sq::IDocumentManager::iid, mgr.putVoid()));
  Ptr<sq::IDocument> doc;
  REQUIRE_OK(mgr->createDocument(SQ_DOC_SONG, doc.put()));
  Ptr<sq::IEditor> ed;
  REQUIRE_OK(doc->getEditor(ed.put()));

  // built-in action through the class registry (as a script would: Host.Classes.createInstance)
  Ptr<sq::IAction> addTrackAction;
  REQUIRE_OK(reg.createInstance(&sq::doc::kAddTrack, &sq::IAction::iid, addTrackAction.putVoid()));
  auto p = sq::makeAttributes();
  p->setInt("kind", SQ_TRACK_AUDIO);
  p->setString("name", "Via registry");
  REQUIRE_OK(ed->beginTransaction("registry"));
  REQUIRE_OK(addTrackAction->perform(doc.get(), ed.get(), p.get(), nullptr));
  REQUIRE_OK(ed->commitTransaction());
  CHECK_EQ(ed->getUndoCount(), 1u);

  // external macro: one undo step named after its class, fully undoable
  Ptr<MacroFactory> mf = Ptr<MacroFactory>::adopt(new MacroFactory());
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  ci.class_id = kMacroClass;
  ci.category = "Action";
  ci.name = "Make Two Tracks";
  REQUIRE_OK(reg.registerClass(&ci, mf.get()));
  ci.class_id = kBadMacroClass;
  ci.name = "Broken Macro";
  REQUIRE_OK(reg.registerClass(&ci, mf.get()));

  sq::edoc::DocumentImpl* impl = sq::edoc::DocumentImpl::from(doc.get());
  REQUIRE(impl);
  const std::string before = impl->stateHash();
  auto result = sq::makeAttributes();
  REQUIRE_OK(ed->execute(&kMacroClass, nullptr, result.get()));
  CHECK_EQ(ed->getUndoCount(), 2u);
  CHECK_EQ(doc->getMarkerCount(), 1u);
  CHECK_EQ(impl->store().trackOrder.size(), 4u);
  char d[64];
  uint32_t need = 0;
  REQUIRE_OK(ed->getUndoDescription(0, d, sizeof d, &need));
  CHECK_EQ(std::string(d), std::string("Make Two Tracks"));
  REQUIRE_OK(ed->undo());
  CHECK_EQ(impl->stateHash(), before);

  CHECK_EQ(ed->execute(&kBadMacroClass, nullptr, nullptr), SQ_E_FAIL);
  CHECK_EQ(impl->stateHash(), before);
  CHECK_EQ(ed->getUndoCount(), 1u);
  CHECK(!ed->inTransaction());
  const sq_guid unknown = sq::guid("{00000000-0000-0000-0000-0000000000AB}");
  CHECK_EQ(ed->execute(&unknown, nullptr, nullptr), SQ_E_NOTFOUND);

  // object:// through the (mocked) object registry
  Ptr<sq::ITrack> t;
  REQUIRE_OK(objects->resolve("object://doc/active/track/1", &sq::ITrack::iid, t.putVoid()));
  sq_track_info ti{};
  REQUIRE_OK(t->getTrackInfo(&ti));
  CHECK_EQ(ti.kind, SQ_TRACK_AUDIO);

  REQUIRE_OK(reg.unregisterClass(&kMacroClass));
  REQUIRE_OK(reg.unregisterClass(&kBadMacroClass));
  t = nullptr;
  ed = nullptr;
  doc = nullptr;
  mgr = nullptr;
  sq::edoc::unregisterDocumentServices(&host);
  CHECK_EQ(countCategory(&reg, sq::doc::kActionCategory), 0u);
  CHECK_EQ(objects->resolvers.size(), size_t(0));
  Ptr<sq::IDocumentManager> gone;
  CHECK_EQ(host.queryService(&sq::IDocumentManager::iid, gone.putVoid()), SQ_E_NOTFOUND);
  host.clearServices();
}
