// sqlite-index through the real module loader: classes, categories, basic use, clean unload.
#include <string>

#include "sq/app/module_loader.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/index.h"
#include "sq/platform.hpp"
#include "sq/sqlite_index/index_scanner.h"
#include "sqtest.hpp"

SQ_TEST("sqlite-index module: loads, serves both classes, unloads") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  const std::string path = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"),
                                        std::string("sqlite-index") + sq_dylib_suffix());
  REQUIRE(loader.load(path).ok());

  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(reg.findClass(&sq::index::kSqliteIndexClassId, &ci));
  CHECK_EQ(std::string(ci.category), std::string("Index"));
  REQUIRE_OK(reg.findClass(&sq::index::kIndexScannerClassId, &ci));
  CHECK_EQ(std::string(ci.category), std::string("IndexScanner"));

  {
    auto ix = sq::createInstance<sq::IContentIndex>(&reg, sq::index::kSqliteIndexClassId);
    REQUIRE(ix);
    REQUIRE_OK(ix->open(":memory:", SQ_INDEX_CREATE));
    sq_index_item it{};
    it.struct_size = sizeof it;
    it.url = "plugin:clap:org.example.synth";
    it.kind = "plugin";
    it.title = "Example Synth";
    it.tags = "Instrument;Synth";
    REQUIRE_OK(ix->putItem(&it, nullptr, nullptr));
    sq_index_query q{};
    q.struct_size = sizeof q;
    q.text = "exam";
    q.tags_all = "SYNTH";
    sq::Ptr<sq::IIndexResult> r;
    REQUIRE_OK(ix->query(&q, r.put()));
    CHECK_EQ(r->getCount(), 1u);
    auto sc = sq::createInstance<sq::IIndexScanner>(&reg, sq::index::kIndexScannerClassId);
    REQUIRE(sc);
    CHECK_OK(sc->wait(0));
    // A module object is alive: unload must refuse (and roll back).
    CHECK_EQ(loader.unload("sqlite-index"), SQ_E_BUSY);
  }
  CHECK_OK(loader.unload("sqlite-index"));
}
