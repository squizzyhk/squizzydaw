// sqlite-index service module: IContentIndex (category "Index") + IIndexScanner ("IndexScanner").
#include "sq/sdk.hpp"
#include "sqlite3.h"
#include "sqlite_index.hpp"

namespace {

sq_result createIndex(const sq_guid* riid, void** out) noexcept {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  try {
    sq::Ptr<sq::IContentIndex> ix = sq::sqlite_index::createContentIndex();
    return ix->queryInterface(riid, out);
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
}

sq_result createScanner(const sq_guid* riid, void** out) noexcept {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  try {
    sq::Ptr<sq::IIndexScanner> sc = sq::sqlite_index::createIndexScanner();
    return sc->queryInterface(riid, out);
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
}

sq_result onInit(sq::IHostContext* host) {
  if (sqlite3_threadsafe() == 0) return SQ_E_UNSUPPORTED;
  sq::sqlite_index::setClassRegistry(host->getClassRegistry());
  sq::logf(SQ_LOG_DEBUG, "sqlite %s, index schema %u", sqlite3_libversion(), sq::sqlite_index::kSchemaVersion);
  return SQ_OK;
}

void onShutdown() { sq::sqlite_index::setClassRegistry(nullptr); }

constexpr sq::ClassEntry kClasses[] = {
    sq::ClassEntry{sq::index::kSqliteIndexClassId, sq::index::kCategory, "SQLite Content Index", 0, &createIndex},
    sq::ClassEntry{sq::index::kIndexScannerClassId, sq::index::kScannerCategory, "Content Index Folder Scanner", 0,
                   &createScanner},
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('S', 'Q', 'I', 'X'),
    .name = "sqlite-index",
    .description = "SQLite content browser index (*.tags.db) with FTS5 search and folder scanner",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
