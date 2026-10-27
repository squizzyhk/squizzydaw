// IContentIndex over SQLite: one *.tags.db per location.
//
// Schema (version 1):
//   StoreInfo(key PRIMARY KEY, value)                    schema_version, created, modified, name
//   TaggedItem(id, url UNIQUE, kind, title, _tags, mtime, size)   _tags = "a;b;c" (lowercase)
//   Tag(id, name UNIQUE)  +  TaggedItemTag(item_id, tag_id)       normalized tags
//   DataItem(item_id, seq, key, type, value)                      free metadata per item
//   TaggedItemText  FTS5(title, tags, meta), rowid = TaggedItem.id
// Every public method takes the instance mutex. Results of up to 256 rows are complete snapshots
// (metadata included); larger results read their metadata lazily through the index on first
// access per row (SQ_E_CLOSED if the store was closed/reopened meanwhile).
#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sq/cpp/attributes.hpp"
#include "sq/cpp/object.hpp"
#include "sq/platform.hpp"
#include "sqlite3.h"
#include "sqlite_index.hpp"

namespace sq::sqlite_index {

// ---------------------------------------------------------------------------------------
// helpers

namespace {

std::atomic<IClassRegistry*> g_registry{nullptr};

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && static_cast<unsigned char>(s[b]) <= ' ') ++b;
  while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') --e;
  return s.substr(b, e - b);
}

int64_t nowUnix() { return static_cast<int64_t>(std::time(nullptr)); }

sq_result mapSqlite(int rc) {
  switch (rc & 0xFF) {
    case SQLITE_OK: case SQLITE_DONE: case SQLITE_ROW: return SQ_OK;
    case SQLITE_BUSY: case SQLITE_LOCKED: return SQ_E_BUSY;
    case SQLITE_NOMEM: return SQ_E_OUTOFMEMORY;
    case SQLITE_READONLY: case SQLITE_PERM: case SQLITE_AUTH: return SQ_E_ACCESS;
    case SQLITE_CANTOPEN: case SQLITE_IOERR: case SQLITE_FULL: return SQ_E_IO;
    case SQLITE_CORRUPT: case SQLITE_NOTADB: case SQLITE_FORMAT: return SQ_E_PARSE;
    case SQLITE_CONSTRAINT: case SQLITE_MISMATCH: case SQLITE_RANGE: return SQ_E_INVALIDARG;
    default: return SQ_E_FAIL;
  }
}

const char* colText(sqlite3_stmt* s, int i) {
  const unsigned char* t = sqlite3_column_text(s, i);
  return t ? reinterpret_cast<const char*>(t) : "";
}

void bindText(sqlite3_stmt* s, int i, const std::string& v) {
  sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

void bindTextOrNull(sqlite3_stmt* s, int i, const char* v) {
  if (v) sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT);
  else sqlite3_bind_text(s, i, "", 0, SQLITE_STATIC);
}

std::string attrKey(IAttributes* a, uint32_t index) {
  uint32_t need = 0;
  if (a->getKey(index, nullptr, 0, &need) != SQ_E_BUFFER_TOO_SMALL || need == 0) return {};
  std::string k(need, '\0');
  if (a->getKey(index, k.data(), need, &need) != SQ_OK) return {};
  k.resize(need - 1);
  return k;
}

}  // namespace

void setClassRegistry(IClassRegistry* reg) { g_registry.store(reg); }
IClassRegistry* classRegistry() { return g_registry.load(); }

namespace {

// Decodes one UTF-8 sequence at s[i]; returns its length (1 for an invalid byte, cp = byte).
size_t decodeUtf8(const std::string& s, size_t i, uint32_t& cp) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
  if (n == 0 || i + n > s.size()) {
    cp = c;
    return 1;
  }
  cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
  for (size_t k = 1; k < n; ++k) {
    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) {
      cp = c;
      return 1;
    }
    cp = (cp << 6) | (cc & 0x3Fu);
  }
  return n;
}

void encodeUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Simple (1:1) lowercase mapping for the scripts tags are realistically written in: Latin
// (incl. Latin-1, Extended-A, Vietnamese), Greek, Cyrillic, Armenian and fullwidth Latin.
uint32_t lowerCodepoint(uint32_t c) {
  if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
  if (c >= 0x100 && c <= 0x17F) {
    if (c == 0x130) return 'i';
    if (c == 0x178) return 0xFF;
    if ((c <= 0x137 || (c >= 0x14A && c <= 0x177)) && !(c & 1)) return c + 1;
    if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && (c & 1)) return c + 1;
    return c;
  }
  if (c >= 0x386 && c <= 0x3AB) {
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x391 && c != 0x3A2) return c + 32;
    return c;
  }
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF)) && !(c & 1)) return c + 1;
  if (c >= 0x531 && c <= 0x556) return c + 48;
  if (((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) && !(c & 1)) return c + 1;
  if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
  return c;
}

// Non-ASCII code points that the FTS5 unicode61 tokenizer treats as separators (punctuation,
// spaces, symbols, emoji). A search "word" made only of these would become an empty phrase.
bool isSeparatorCodepoint(uint32_t c) {
  if (c < 0x80) return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
  if (c <= 0xBF)
    return !(c == 0xAA || c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB9 || c == 0xBA ||
             (c >= 0xBC && c <= 0xBE));
  if (c == 0xD7 || c == 0xF7) return true;
  if (c >= 0x2000 && c <= 0x206F) return true;  // general punctuation, spaces, ZWJ
  if (c >= 0x20A0 && c <= 0x20CF) return true;  // currency
  if (c >= 0x2190 && c <= 0x2BFF) return true;  // arrows, math, box drawing, shapes, symbols
  if (c >= 0x3000 && c <= 0x3004) return true;  // CJK spaces / punctuation
  if (c >= 0x3008 && c <= 0x3020) return true;
  if (c >= 0xFE00 && c <= 0xFE0F) return true;  // variation selectors
  if (c >= 0xFE30 && c <= 0xFE4F) return true;
  if (c >= 0xFF00 && c <= 0xFF0F) return true;  // fullwidth punctuation
  if ((c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65)) return true;
  if (c >= 0x1F000 && c <= 0x1FAFF) return true;  // emoji & pictographs
  return false;
}

}  // namespace

std::string foldTag(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    uint32_t cp = 0;
    const size_t n = decodeUtf8(in, i, cp);
    if (n == 1 && static_cast<unsigned char>(in[i]) >= 0x80) out += in[i];  // invalid byte: verbatim
    else encodeUtf8(lowerCodepoint(cp), out);
    i += n;
  }
  return out;
}

std::vector<std::string> splitTags(const char* list) {
  std::vector<std::string> out;
  if (!list) return out;
  std::string cur;
  auto flush = [&] {
    std::string t = foldTag(trim(cur));
    cur.clear();
    if (t.empty()) return;
    if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(std::move(t));
  };
  for (const char* p = list; *p; ++p) {
    if (*p == ';') flush();
    else cur += *p;
  }
  flush();
  return out;
}

std::string joinTags(const std::vector<std::string>& tags) {
  std::string s;
  for (const auto& t : tags) {
    if (!s.empty()) s += ';';
    s += t;
  }
  return s;
}

std::string ftsExpression(const char* text) {
  std::string expr;
  if (!text) return expr;
  std::string word;
  auto flush = [&] {
    if (word.empty()) return;
    if (!expr.empty()) expr += ' ';
    expr += '"';
    expr += word;  // only word characters: no quote can appear inside
    expr += "\"*";
    word.clear();
  };
  const std::string s(text);
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    const size_t n = decodeUtf8(s, i, cp);
    // invalid bytes are kept inside the word (never '"'); separators split words
    const bool invalid = n == 1 && static_cast<unsigned char>(s[i]) >= 0x80;
    if (invalid || !isSeparatorCodepoint(cp)) word.append(s, i, n);
    else flush();
    i += n;
  }
  flush();
  return expr;
}

std::string prefixUpperBound(const std::string& prefix) {
  std::string s = prefix;
  while (!s.empty()) {
    unsigned char last = static_cast<unsigned char>(s.back());
    if (last < 0xFF) {
      s.back() = static_cast<char>(last + 1);
      return s;
    }
    s.pop_back();
  }
  return s;
}

// ---------------------------------------------------------------------------------------

namespace {

constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS StoreInfo (key TEXT PRIMARY KEY, value) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS TaggedItem (
  id INTEGER PRIMARY KEY,
  url TEXT NOT NULL UNIQUE,
  kind TEXT NOT NULL DEFAULT '',
  title TEXT NOT NULL DEFAULT '',
  _tags TEXT NOT NULL DEFAULT '',
  mtime INTEGER NOT NULL DEFAULT 0,
  size INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS TaggedItem_kind ON TaggedItem(kind);
CREATE INDEX IF NOT EXISTS TaggedItem_title ON TaggedItem(title COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS TaggedItem_mtime ON TaggedItem(mtime);
CREATE TABLE IF NOT EXISTS Tag (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
CREATE TABLE IF NOT EXISTS TaggedItemTag (
  item_id INTEGER NOT NULL REFERENCES TaggedItem(id) ON DELETE CASCADE,
  tag_id INTEGER NOT NULL REFERENCES Tag(id) ON DELETE CASCADE,
  PRIMARY KEY (item_id, tag_id)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS TaggedItemTag_tag ON TaggedItemTag(tag_id, item_id);
CREATE TABLE IF NOT EXISTS DataItem (
  item_id INTEGER NOT NULL REFERENCES TaggedItem(id) ON DELETE CASCADE,
  seq INTEGER NOT NULL,
  key TEXT NOT NULL,
  type INTEGER NOT NULL,
  value,
  PRIMARY KEY (item_id, key)) WITHOUT ROWID;
CREATE VIRTUAL TABLE IF NOT EXISTS TaggedItemText USING fts5(
  title, tags, meta, tokenize = 'unicode61 remove_diacritics 2', prefix = '2 3');
CREATE TRIGGER IF NOT EXISTS TaggedItem_after_delete AFTER DELETE ON TaggedItem BEGIN
  DELETE FROM TaggedItemText WHERE rowid = old.id;
END;
)SQL";

struct RowData {
  uint64_t id = 0;
  std::string url, kind, title, tags;
  int64_t mtime = 0, size = 0;
  double score = 0.0;
};

class SqliteContentIndex;

class IndexResult final : public Object<IndexResult, IIndexResult> {
 public:
  IndexResult(SqliteContentIndex* owner, uint64_t epoch, std::vector<RowData> rows, uint64_t total,
              std::vector<std::vector<MetaValue>> eagerMeta);
  uint32_t SQ_CALL getCount() override { return static_cast<uint32_t>(rows_.size()); }
  uint64_t SQ_CALL getTotalCount() override { return total_; }
  sq_result SQ_CALL getRow(uint32_t index, sq_index_row* out) override {
    if (!out || out->struct_size < sizeof(sq_index_row)) return SQ_E_INVALIDARG;
    if (index >= rows_.size()) return SQ_E_NOTFOUND;
    const RowData& r = rows_[index];
    out->id = r.id;
    out->url = r.url.c_str();
    out->kind = r.kind.c_str();
    out->title = r.title.c_str();
    out->tags = r.tags.c_str();
    out->mtime = r.mtime;
    out->size = r.size;
    out->score = r.score;
    return SQ_OK;
  }
  sq_result SQ_CALL getMetadata(uint32_t index, IAttributes* dst) override;

 private:
  Ptr<IContentIndex> owner_;
  SqliteContentIndex* impl_;
  uint64_t epoch_;
  std::vector<RowData> rows_;
  uint64_t total_;
  std::mutex cacheMutex_;
  std::unordered_map<uint32_t, std::vector<MetaValue>> cache_;
};

class SqliteContentIndex final : public Object<SqliteContentIndex, IContentIndex> {
 public:
  ~SqliteContentIndex() override { closeLocked(); }

  // ---- IContentIndex -------------------------------------------------------------------
  sq_result SQ_CALL open(const char* path, uint32_t flags) override {
    if (!path || !*path) return SQ_E_INVALIDARG;
    if ((flags & SQ_INDEX_CREATE) && (flags & SQ_INDEX_READONLY)) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(m_);
    try {
      closeLocked();
      const bool memory = std::strcmp(path, ":memory:") == 0;
      if (!memory && !sq_fs_is_file(path)) {
        if (!(flags & SQ_INDEX_CREATE)) return SQ_E_NOTFOUND;
        std::string p(path);
        size_t slash = p.find_last_of("/\\");
        if (slash != std::string::npos && slash > 0) sq_fs_create_dirs(p.substr(0, slash).c_str());
      }
      readonly_ = (flags & SQ_INDEX_READONLY) != 0;
      int oflags = SQLITE_OPEN_NOMUTEX | (readonly_ ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
      int rc = sqlite3_open_v2(path, &db_, oflags, nullptr);
      if (rc != SQLITE_OK) {
        sq_result r = mapSqlite(rc);
        closeLocked();
        return r;
      }
      sqlite3_extended_result_codes(db_, 1);
      sqlite3_busy_timeout(db_, 5000);
      // Before any pragma that persists (journal_mode): never adopt someone else's database.
      if (sq_result own = checkOwnership(); SQ_FAILED(own)) {
        closeLocked();
        return own;
      }
      exec("PRAGMA foreign_keys = ON");
      exec("PRAGMA temp_store = MEMORY");
      exec("PRAGMA cache_size = -8000");
      if (!readonly_) {
        if (!memory) exec("PRAGMA journal_mode = WAL");
        exec("PRAGMA synchronous = NORMAL");
      }
      sq_result r = readonly_ ? checkSchemaReadOnly() : initSchema(path);
      if (SQ_FAILED(r)) {
        closeLocked();
        return r;
      }
      ++epoch_;
      return SQ_OK;
    } catch (...) {
      closeLocked();
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL close() override {
    std::lock_guard<std::mutex> lock(m_);
    if (!db_) return SQ_S_FALSE;
    closeLocked();
    return SQ_OK;
  }

  sq_result SQ_CALL getStoreInfo(sq_index_store_info* out) override {
    if (!out || out->struct_size < sizeof(sq_index_store_info)) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(m_);
    if (!db_) return SQ_E_CLOSED;
    try {
      out->schema_version = static_cast<uint32_t>(infoInt("schema_version", 0));
      out->created = infoInt("created", 0);
      out->modified = infoInt("modified", 0);
      std::string name = infoText("name");
      std::memset(out->name, 0, sizeof out->name);
      std::memcpy(out->name, name.data(), std::min(name.size(), sizeof out->name - 1));
      sqlite3_stmt* s = stmt("SELECT COUNT(*) FROM TaggedItem");
      out->item_count = (s && sqlite3_step(s) == SQLITE_ROW) ? static_cast<uint64_t>(sqlite3_column_int64(s, 0)) : 0;
      if (s) sqlite3_reset(s);
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL setStoreName(const char* name) override {
    if (!name) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(m_);
    if (sq_result r = writable(); SQ_FAILED(r)) return r;
    try {
      Tx tx(*this);
      if (SQ_FAILED(tx.result)) return tx.result;
      sq_result r = setInfo("name", name);
      if (SQ_FAILED(r)) return r;
      return tx.commit();
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL beginBatch() override {
    std::lock_guard<std::mutex> lock(m_);
    if (sq_result r = writable(); SQ_FAILED(r)) return r;
    if (batchDepth_ == 0) {
      int rc = exec("BEGIN IMMEDIATE");
      if (rc != SQLITE_OK) return mapSqlite(rc);
    }
    ++batchDepth_;
    return SQ_OK;
  }

  sq_result SQ_CALL commitBatch() override {
    std::lock_guard<std::mutex> lock(m_);
    if (!db_) return SQ_E_CLOSED;
    if (batchDepth_ == 0) return SQ_S_FALSE;
    if (--batchDepth_ > 0) return SQ_OK;
    try {
      finishWrite();
    } catch (...) {
    }
    int rc = exec("COMMIT");
    if (rc != SQLITE_OK) {
      exec("ROLLBACK");
      return mapSqlite(rc);
    }
    return SQ_OK;
  }

  sq_result SQ_CALL rollbackBatch() override {
    std::lock_guard<std::mutex> lock(m_);
    if (!db_) return SQ_E_CLOSED;
    if (batchDepth_ == 0) return SQ_S_FALSE;
    batchDepth_ = 0;
    pendingOrphans_ = false;
    return mapSqlite(exec("ROLLBACK"));
  }

  sq_result SQ_CALL putItem(const sq_index_item* item, IAttributes* metadata, uint64_t* id) override {
    if (!item || item->struct_size < sizeof(sq_index_item) || !item->url || !*item->url) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(m_);
    if (sq_result r = writable(); SQ_FAILED(r)) return r;
    try {
      std::vector<std::string> tags = splitTags(item->tags);
      std::vector<MetaValue> meta;
      std::string metaText;
      if (metadata) collectMetadata(metadata, meta, metaText);

      Tx tx(*this);
      if (SQ_FAILED(tx.result)) return tx.result;
      sqlite3_stmt* up = stmt(
          "INSERT INTO TaggedItem(url, kind, title, _tags, mtime, size) VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
          "ON CONFLICT(url) DO UPDATE SET kind = excluded.kind, title = excluded.title, _tags = excluded._tags, "
          "mtime = excluded.mtime, size = excluded.size RETURNING id");
      if (!up) return SQ_E_FAIL;
      const std::string joined = joinTags(tags);
      sqlite3_bind_text(up, 1, item->url, -1, SQLITE_TRANSIENT);
      bindTextOrNull(up, 2, item->kind);
      bindTextOrNull(up, 3, item->title);
      bindText(up, 4, joined);
      sqlite3_bind_int64(up, 5, item->mtime);
      sqlite3_bind_int64(up, 6, item->size);
      int rc = sqlite3_step(up);
      if (rc != SQLITE_ROW) {
        sqlite3_reset(up);
        return mapSqlite(rc);
      }
      const int64_t rowId = sqlite3_column_int64(up, 0);
      sqlite3_step(up);  // drain RETURNING
      sqlite3_reset(up);

      if (sq_result r = replaceTags(rowId, tags); SQ_FAILED(r)) return r;

      sqlite3_stmt* delMeta = stmt("DELETE FROM DataItem WHERE item_id = ?1");
      sqlite3_bind_int64(delMeta, 1, rowId);
      rc = stepDone(delMeta);
      if (rc != SQLITE_DONE) return mapSqlite(rc);
      if (!meta.empty()) {
        sqlite3_stmt* ins = stmt("INSERT INTO DataItem(item_id, seq, key, type, value) VALUES(?1, ?2, ?3, ?4, ?5)");
        int seq = 0;
        for (const MetaValue& mv : meta) {
          sqlite3_bind_int64(ins, 1, rowId);
          sqlite3_bind_int(ins, 2, seq++);
          bindText(ins, 3, mv.key);
          sqlite3_bind_int(ins, 4, mv.type);
          switch (mv.type) {
            case SQ_VALUE_INT: case SQ_VALUE_BOOL: sqlite3_bind_int64(ins, 5, mv.i); break;
            case SQ_VALUE_DOUBLE: sqlite3_bind_double(ins, 5, mv.d); break;
            default: bindText(ins, 5, mv.s); break;
          }
          rc = stepDone(ins);
          if (rc != SQLITE_DONE) return mapSqlite(rc);
        }
      }

      sqlite3_stmt* delText = stmt("DELETE FROM TaggedItemText WHERE rowid = ?1");
      sqlite3_bind_int64(delText, 1, rowId);
      rc = stepDone(delText);
      if (rc != SQLITE_DONE) return mapSqlite(rc);
      sqlite3_stmt* insText = stmt("INSERT INTO TaggedItemText(rowid, title, tags, meta) VALUES(?1, ?2, ?3, ?4)");
      sqlite3_bind_int64(insText, 1, rowId);
      bindTextOrNull(insText, 2, item->title);
      bindText(insText, 3, tagsAsText(tags));
      bindText(insText, 4, metaText);
      rc = stepDone(insText);
      if (rc != SQLITE_DONE) return mapSqlite(rc);

      pendingOrphans_ = true;  // the item's previous tags may now be unused
      sq_result r = tx.commit();
      if (SQ_SUCCEEDED(r) && id) *id = static_cast<uint64_t>(rowId);
      return r;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL removeItem(const char* url) override {
    if (!url) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(m_);
    if (sq_result r = writable(); SQ_FAILED(r)) return r;
    try {
      Tx tx(*this);
      if (SQ_FAILED(tx.result)) return tx.result;
      sqlite3_stmt* s = stmt("DELETE FROM TaggedItem WHERE url = ?1");
      sqlite3_bind_text(s, 1, url, -1, SQLITE_TRANSIENT);
      int rc = stepDone(s);
      if (rc != SQLITE_DONE) return mapSqlite(rc);
      if (sqlite3_changes(db_) == 0) return SQ_S_FALSE;  // Tx rolls back (nothing done anyway)
      pendingOrphans_ = true;
      return tx.commit();
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL removeByPrefix(const char* urlPrefix, uint64_t* removed) override {
    if (!urlPrefix) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(m_);
    if (removed) *removed = 0;
    if (sq_result r = writable(); SQ_FAILED(r)) return r;
    try {
      Tx tx(*this);
      if (SQ_FAILED(tx.result)) return tx.result;
      const std::string prefix(urlPrefix);
      const std::string upper = prefixUpperBound(prefix);
      sqlite3_stmt* s = nullptr;
      if (prefix.empty()) {
        s = stmt("DELETE FROM TaggedItem");
      } else if (upper.empty()) {
        s = stmt("DELETE FROM TaggedItem WHERE url >= ?1");
        bindText(s, 1, prefix);
      } else {
        s = stmt("DELETE FROM TaggedItem WHERE url >= ?1 AND url < ?2");
        bindText(s, 1, prefix);
        bindText(s, 2, upper);
      }
      int rc = stepDone(s);
      if (rc != SQLITE_DONE) return mapSqlite(rc);
      const uint64_t n = static_cast<uint64_t>(sqlite3_changes64(db_));
      if (n) pendingOrphans_ = true;
      sq_result r = tx.commit();
      if (SQ_SUCCEEDED(r) && removed) *removed = n;
      return r;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL addTags(const char* url, const char* tags) override { return editTags(url, tags, true); }
  sq_result SQ_CALL removeTags(const char* url, const char* tags) override { return editTags(url, tags, false); }

  sq_result SQ_CALL query(const sq_index_query* q, IIndexResult** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (q && q->struct_size < sizeof(sq_index_query)) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(m_);
    if (!db_) return SQ_E_CLOSED;
    try {
      sq_index_query defaults{};
      defaults.struct_size = sizeof defaults;
      if (!q) q = &defaults;

      const std::string match = ftsExpression(q->text);
      const std::vector<std::string> all = splitTags(q->tags_all);
      const std::vector<std::string> any = splitTags(q->tags_any);
      const std::string prefix = q->url_prefix ? q->url_prefix : "";
      const std::string upper = prefixUpperBound(prefix);
      const bool hasKind = q->kind && *q->kind;

      // FROM + WHERE (shared by the row and count queries); binds collected in order.
      std::string from = " FROM TaggedItem i";
      if (!match.empty())
        from += " JOIN (SELECT rowid AS fid, bm25(TaggedItemText, 10.0, 4.0, 1.0) AS r FROM TaggedItemText "
                "WHERE TaggedItemText MATCH ?) f ON f.fid = i.id";
      std::string where = " WHERE 1";
      std::vector<std::string> binds;
      if (!match.empty()) binds.push_back(match);
      if (hasKind) {
        where += " AND i.kind = ?";
        binds.emplace_back(q->kind);
      }
      if (!prefix.empty()) {
        where += " AND i.url >= ?";
        binds.push_back(prefix);
        if (!upper.empty()) {
          where += " AND i.url < ?";
          binds.push_back(upper);
        }
      }
      auto inList = [](size_t n) {
        std::string s = "(";
        for (size_t k = 0; k < n; ++k) s += k ? ",?" : "?";
        return s + ")";
      };
      if (!all.empty()) {
        where += " AND i.id IN (SELECT t.item_id FROM TaggedItemTag t JOIN Tag g ON g.id = t.tag_id WHERE g.name IN " +
                 inList(all.size()) + " GROUP BY t.item_id HAVING COUNT(*) = " + std::to_string(all.size()) + ")";
        binds.insert(binds.end(), all.begin(), all.end());
      }
      if (!any.empty()) {
        where += " AND i.id IN (SELECT t.item_id FROM TaggedItemTag t JOIN Tag g ON g.id = t.tag_id WHERE g.name IN " +
                 inList(any.size()) + ")";
        binds.insert(binds.end(), any.begin(), any.end());
      }

      std::string order;
      switch (q->sort) {
        case SQ_INDEX_SORT_TITLE: order = " ORDER BY i.title COLLATE NOCASE, i.id"; break;
        case SQ_INDEX_SORT_MTIME: order = " ORDER BY i.mtime DESC, i.id"; break;
        default:
          order = match.empty() ? " ORDER BY i.title COLLATE NOCASE, i.id" : " ORDER BY f.r, i.title COLLATE NOCASE, i.id";
          break;
      }
      const std::string rowSql = std::string("SELECT i.id, i.url, i.kind, i.title, i._tags, i.mtime, i.size, ") +
                                 (match.empty() ? "0.0" : "-f.r") + from + where + order + " LIMIT ? OFFSET ?";

      sqlite3_stmt* s = stmt(rowSql.c_str());
      if (!s) return SQ_E_INVALIDARG;
      int bi = 1;
      for (const auto& b : binds) bindText(s, bi++, b);
      sqlite3_bind_int64(s, bi++, q->limit ? static_cast<sqlite3_int64>(q->limit) : -1);
      sqlite3_bind_int64(s, bi++, q->offset);
      std::vector<RowData> rows;
      if (q->limit) rows.reserve(q->limit);
      int rc;
      while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        RowData r;
        r.id = static_cast<uint64_t>(sqlite3_column_int64(s, 0));
        r.url = colText(s, 1);
        r.kind = colText(s, 2);
        r.title = colText(s, 3);
        r.tags = colText(s, 4);
        r.mtime = sqlite3_column_int64(s, 5);
        r.size = sqlite3_column_int64(s, 6);
        r.score = sqlite3_column_double(s, 7);
        rows.push_back(std::move(r));
      }
      sqlite3_reset(s);
      if (rc != SQLITE_DONE) return rc == SQLITE_ERROR ? SQ_E_INVALIDARG : mapSqlite(rc);

      uint64_t total = 0;
      const bool complete = (q->limit == 0 || rows.size() < q->limit) && (!rows.empty() || q->offset == 0);
      if (complete) {
        total = q->offset + rows.size();
      } else {
        const std::string countSql = "SELECT COUNT(*)" + from + where;
        sqlite3_stmt* c = stmt(countSql.c_str());
        if (!c) return SQ_E_FAIL;
        bi = 1;
        for (const auto& b : binds) bindText(c, bi++, b);
        if (sqlite3_step(c) == SQLITE_ROW) total = static_cast<uint64_t>(sqlite3_column_int64(c, 0));
        sqlite3_reset(c);
      }
      // Page-sized results carry their metadata (a true snapshot, as index.h promises); big
      // unpaged results (bulk tools, the scanner) load it lazily per row on first access.
      std::vector<std::vector<MetaValue>> eager;
      if (rows.size() <= kEagerMetadataRows) {
        eager.resize(rows.size());
        for (size_t k = 0; k < rows.size(); ++k)
          if (sq_result mr = loadMetadataLocked(rows[k].id, eager[k]); SQ_FAILED(mr)) return mr;
      }
      *out = new IndexResult(this, epoch_, std::move(rows), total, std::move(eager));
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL listTags(const char* prefix, sq_index_tag_fn fn, void* user) override {
    if (!fn) return SQ_E_INVALIDARG;
    std::vector<std::pair<std::string, uint64_t>> tags;
    {
      std::lock_guard<std::mutex> lock(m_);
      if (!db_) return SQ_E_CLOSED;
      try {
        const std::string p = foldTag(prefix ? prefix : "");
        const std::string upper = prefixUpperBound(p);
        sqlite3_stmt* s;
        if (p.empty()) {
          s = stmt("SELECT g.name, COUNT(*) FROM Tag g JOIN TaggedItemTag t ON t.tag_id = g.id GROUP BY g.id ORDER BY g.name");
        } else if (upper.empty()) {
          s = stmt("SELECT g.name, COUNT(*) FROM Tag g JOIN TaggedItemTag t ON t.tag_id = g.id WHERE g.name >= ?1 "
                   "GROUP BY g.id ORDER BY g.name");
          bindText(s, 1, p);
        } else {
          s = stmt("SELECT g.name, COUNT(*) FROM Tag g JOIN TaggedItemTag t ON t.tag_id = g.id WHERE g.name >= ?1 AND "
                   "g.name < ?2 GROUP BY g.id ORDER BY g.name");
          bindText(s, 1, p);
          bindText(s, 2, upper);
        }
        while (sqlite3_step(s) == SQLITE_ROW)
          tags.emplace_back(colText(s, 0), static_cast<uint64_t>(sqlite3_column_int64(s, 1)));
        sqlite3_reset(s);
      } catch (...) {
        return SQ_E_FAIL;
      }
    }
    // Callback outside the lock: it may call back into the index.
    for (const auto& t : tags) fn(user, t.first.c_str(), t.second);
    return SQ_OK;
  }

  // Used by IndexResult::getMetadata.
  sq_result loadMetadata(uint64_t epoch, uint64_t itemId, std::vector<MetaValue>& out) {
    std::lock_guard<std::mutex> lock(m_);
    if (!db_ || epoch != epoch_) return SQ_E_CLOSED;
    return loadMetadataLocked(itemId, out);
  }

 private:
  static constexpr size_t kEagerMetadataRows = 256;

  sq_result loadMetadataLocked(uint64_t itemId, std::vector<MetaValue>& out) {
    sqlite3_stmt* s = stmt("SELECT key, type, value FROM DataItem WHERE item_id = ?1 ORDER BY seq");
    if (!s) return SQ_E_FAIL;
    sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(itemId));
    while (sqlite3_step(s) == SQLITE_ROW) {
      MetaValue mv;
      mv.key = colText(s, 0);
      mv.type = sqlite3_column_int(s, 1);
      switch (mv.type) {
        case SQ_VALUE_INT: case SQ_VALUE_BOOL: mv.i = sqlite3_column_int64(s, 2); break;
        case SQ_VALUE_DOUBLE: mv.d = sqlite3_column_double(s, 2); break;
        default: mv.type = SQ_VALUE_STRING; mv.s = colText(s, 2); break;
      }
      out.push_back(std::move(mv));
    }
    sqlite3_reset(s);
    return SQ_OK;
  }

  // Savepoint-based write scope: nests inside a batch, standalone otherwise.
  struct Tx {
    SqliteContentIndex& ix;
    sq_result result = SQ_OK;
    bool done = false;
    explicit Tx(SqliteContentIndex& i) : ix(i) {
      int rc = ix.exec("SAVEPOINT sqw");
      result = mapSqlite(rc);
      done = rc != SQLITE_OK;
    }
    sq_result commit() {
      if (ix.batchDepth_ == 0) {
        try {
          ix.finishWrite();
        } catch (...) {
        }
      }
      int rc = ix.exec("RELEASE sqw");
      done = true;
      if (rc != SQLITE_OK) {
        ix.exec("ROLLBACK TO sqw");
        ix.exec("RELEASE sqw");
        return mapSqlite(rc);
      }
      return SQ_OK;
    }
    ~Tx() {
      if (!done) {
        ix.exec("ROLLBACK TO sqw");
        ix.exec("RELEASE sqw");
      }
    }
  };

  int exec(const char* sql) {
    if (!db_) return SQLITE_MISUSE;
    return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
  }

  static int stepDone(sqlite3_stmt* s) {
    if (!s) return SQLITE_ERROR;
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc;
  }

  // Cached prepared statement (reset + bindings cleared), or nullptr on a prepare error.
  sqlite3_stmt* stmt(const char* sql) {
    auto it = stmts_.find(sql);
    if (it != stmts_.end()) {
      sqlite3_reset(it->second);
      sqlite3_clear_bindings(it->second);
      return it->second;
    }
    if (stmts_.size() > 256) clearStatements();
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v3(db_, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, nullptr) != SQLITE_OK) {
      sqlite3_finalize(s);
      return nullptr;
    }
    stmts_.emplace(sql, s);
    return s;
  }

  void clearStatements() {
    for (auto& kv : stmts_) sqlite3_finalize(kv.second);
    stmts_.clear();
  }

  void closeLocked() {
    if (!db_) return;
    if (batchDepth_ > 0) exec("ROLLBACK");
    batchDepth_ = 0;
    pendingOrphans_ = false;
    clearStatements();
    sqlite3_close_v2(db_);
    db_ = nullptr;
    ++epoch_;
  }

  sq_result writable() const {
    if (!db_) return SQ_E_CLOSED;
    if (readonly_) return SQ_E_ACCESS;
    return SQ_OK;
  }

  // SQ_OK for an empty database (writable opens initialise it) or one of our stores;
  // SQ_E_PARSE for a non-SQLite file or a foreign SQLite database (left untouched).
  sq_result checkOwnership() {
    sqlite3_stmt* s = nullptr;
    int rc = sqlite3_prepare_v2(db_,
                                "SELECT (SELECT COUNT(*) FROM sqlite_master), "
                                "(SELECT value FROM StoreInfo WHERE key = 'schema_version')",
                                -1, &s, nullptr);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(s);
      if ((rc & 0xFF) == SQLITE_ERROR) {
        // No StoreInfo table: fine only if the database is completely empty.
        rc = sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM sqlite_master", -1, &s, nullptr);
        if (rc != SQLITE_OK) {
          sqlite3_finalize(s);
          return mapSqlite(rc);
        }
        rc = sqlite3_step(s);
        const int64_t objects = rc == SQLITE_ROW ? sqlite3_column_int64(s, 0) : -1;
        sqlite3_finalize(s);
        if (rc != SQLITE_ROW) return mapSqlite(rc);
        return objects == 0 ? SQ_OK : SQ_E_PARSE;
      }
      return mapSqlite(rc);
    }
    rc = sqlite3_step(s);
    const bool tagged = rc == SQLITE_ROW && sqlite3_column_type(s, 1) != SQLITE_NULL && sqlite3_column_int64(s, 1) > 0;
    sqlite3_finalize(s);
    if (rc != SQLITE_ROW) return mapSqlite(rc);
    return tagged ? SQ_OK : SQ_E_PARSE;
  }

  sq_result checkSchemaReadOnly() {
    sqlite3_stmt* s = stmt("SELECT value FROM StoreInfo WHERE key = 'schema_version'");
    if (!s) return SQ_E_PARSE;  // not one of our stores
    int64_t v = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
    sqlite3_reset(s);
    if (v <= 0) return SQ_E_PARSE;
    if (v > kSchemaVersion) return SQ_E_UNSUPPORTED;
    return SQ_OK;
  }

  sq_result initSchema(const char* path) {
    int rc = exec("BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) return mapSqlite(rc);
    rc = exec(kSchemaSql);
    if (rc != SQLITE_OK) {
      exec("ROLLBACK");
      return mapSqlite(rc);
    }
    const int64_t v = infoInt("schema_version", 0);
    if (v > kSchemaVersion) {
      exec("ROLLBACK");
      return SQ_E_UNSUPPORTED;
    }
    if (v == 0) {
      const int64_t now = nowUnix();
      setInfoInt("schema_version", kSchemaVersion);
      setInfoInt("created", now);
      setInfoInt("modified", now);
      setInfo("name", defaultStoreName(path).c_str());
    }
    rc = exec("COMMIT");
    if (rc != SQLITE_OK) {
      exec("ROLLBACK");
      return mapSqlite(rc);
    }
    return SQ_OK;
  }

  static std::string defaultStoreName(const char* path) {
    std::string p(path);
    if (p == ":memory:") return "memory";
    size_t slash = p.find_last_of("/\\");
    std::string n = slash == std::string::npos ? p : p.substr(slash + 1);
    for (const char* ext : {".tags.db", ".db"}) {
      const size_t el = std::strlen(ext);
      if (n.size() > el && n.compare(n.size() - el, el, ext) == 0) {
        n.resize(n.size() - el);
        break;
      }
    }
    return n;
  }

  int64_t infoInt(const char* key, int64_t def) {
    sqlite3_stmt* s = stmt("SELECT value FROM StoreInfo WHERE key = ?1");
    if (!s) return def;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    int64_t v = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : def;
    sqlite3_reset(s);
    return v;
  }
  std::string infoText(const char* key) {
    sqlite3_stmt* s = stmt("SELECT value FROM StoreInfo WHERE key = ?1");
    if (!s) return {};
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    std::string v = sqlite3_step(s) == SQLITE_ROW ? colText(s, 0) : "";
    sqlite3_reset(s);
    return v;
  }
  sq_result setInfo(const char* key, const char* value) {
    sqlite3_stmt* s = stmt("INSERT INTO StoreInfo(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!s) return SQ_E_FAIL;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, value, -1, SQLITE_TRANSIENT);
    int rc = stepDone(s);
    return rc == SQLITE_DONE ? SQ_OK : mapSqlite(rc);
  }
  sq_result setInfoInt(const char* key, int64_t value) {
    sqlite3_stmt* s = stmt("INSERT INTO StoreInfo(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!s) return SQ_E_FAIL;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, value);
    int rc = stepDone(s);
    return rc == SQLITE_DONE ? SQ_OK : mapSqlite(rc);
  }

  // End of an outermost write: drop unused tags and stamp the modification time.
  void finishWrite() {
    if (pendingOrphans_) {
      exec("DELETE FROM Tag WHERE NOT EXISTS (SELECT 1 FROM TaggedItemTag t WHERE t.tag_id = Tag.id)");
      pendingOrphans_ = false;
    }
    setInfoInt("modified", nowUnix());
  }

  static std::string tagsAsText(const std::vector<std::string>& tags) {
    std::string s;
    for (const auto& t : tags) {
      if (!s.empty()) s += ' ';
      s += t;
    }
    return s;
  }

  sq_result replaceTags(int64_t rowId, const std::vector<std::string>& tags) {
    sqlite3_stmt* del = stmt("DELETE FROM TaggedItemTag WHERE item_id = ?1");
    sqlite3_bind_int64(del, 1, rowId);
    int rc = stepDone(del);
    if (rc != SQLITE_DONE) return mapSqlite(rc);
    for (const auto& t : tags) {
      sqlite3_stmt* tg = stmt("INSERT INTO Tag(name) VALUES(?1) ON CONFLICT(name) DO UPDATE SET name = excluded.name RETURNING id");
      bindText(tg, 1, t);
      rc = sqlite3_step(tg);
      if (rc != SQLITE_ROW) {
        sqlite3_reset(tg);
        return mapSqlite(rc);
      }
      const int64_t tagId = sqlite3_column_int64(tg, 0);
      sqlite3_step(tg);
      sqlite3_reset(tg);
      sqlite3_stmt* link = stmt("INSERT OR IGNORE INTO TaggedItemTag(item_id, tag_id) VALUES(?1, ?2)");
      sqlite3_bind_int64(link, 1, rowId);
      sqlite3_bind_int64(link, 2, tagId);
      rc = stepDone(link);
      if (rc != SQLITE_DONE) return mapSqlite(rc);
    }
    return SQ_OK;
  }

  sq_result editTags(const char* url, const char* list, bool add) {
    if (!url || !list) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(m_);
    if (sq_result r = writable(); SQ_FAILED(r)) return r;
    try {
      Tx tx(*this);
      if (SQ_FAILED(tx.result)) return tx.result;
      sqlite3_stmt* s = stmt("SELECT id, _tags FROM TaggedItem WHERE url = ?1");
      sqlite3_bind_text(s, 1, url, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_reset(s);
        return SQ_E_NOTFOUND;
      }
      const int64_t rowId = sqlite3_column_int64(s, 0);
      std::vector<std::string> tags = splitTags(colText(s, 1));
      sqlite3_reset(s);
      const std::vector<std::string> edit = splitTags(list);
      bool changed = false;
      for (const auto& t : edit) {
        auto it = std::find(tags.begin(), tags.end(), t);
        if (add && it == tags.end()) {
          tags.push_back(t);
          changed = true;
        } else if (!add && it != tags.end()) {
          tags.erase(it);
          changed = true;
        }
      }
      if (!changed) return SQ_S_FALSE;
      if (sq_result r = replaceTags(rowId, tags); SQ_FAILED(r)) return r;
      sqlite3_stmt* up = stmt("UPDATE TaggedItem SET _tags = ?1 WHERE id = ?2");
      bindText(up, 1, joinTags(tags));
      sqlite3_bind_int64(up, 2, rowId);
      int rc = stepDone(up);
      if (rc != SQLITE_DONE) return mapSqlite(rc);
      sqlite3_stmt* ft = stmt("UPDATE TaggedItemText SET tags = ?1 WHERE rowid = ?2");
      bindText(ft, 1, tagsAsText(tags));
      sqlite3_bind_int64(ft, 2, rowId);
      rc = stepDone(ft);
      if (rc != SQLITE_DONE) return mapSqlite(rc);
      if (!add) pendingOrphans_ = true;
      return tx.commit();
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  static void collectMetadata(IAttributes* a, std::vector<MetaValue>& out, std::string& text) {
    const uint32_t n = a->getCount();
    for (uint32_t k = 0; k < n; ++k) {
      MetaValue mv;
      mv.key = attrKey(a, k);
      if (mv.key.empty()) continue;
      const int32_t type = a->getType(mv.key.c_str());
      switch (type) {
        case SQ_VALUE_INT: mv.type = type; mv.i = a->getInt(mv.key.c_str(), 0); break;
        case SQ_VALUE_BOOL: mv.type = type; mv.i = a->getBool(mv.key.c_str(), SQ_FALSE) ? 1 : 0; break;
        case SQ_VALUE_DOUBLE: mv.type = type; mv.d = a->getDouble(mv.key.c_str(), 0.0); break;
        case SQ_VALUE_STRING:
          mv.type = type;
          mv.s = getAttrString(a, mv.key.c_str());
          if (!mv.s.empty()) {
            if (!text.empty()) text += ' ';
            text += mv.s;
          }
          break;
        default: continue;  // guid / blob / object: not indexed
      }
      out.push_back(std::move(mv));
    }
  }

  std::mutex m_;
  sqlite3* db_ = nullptr;
  bool readonly_ = false;
  int batchDepth_ = 0;
  bool pendingOrphans_ = false;
  uint64_t epoch_ = 0;
  std::unordered_map<std::string, sqlite3_stmt*> stmts_;
};

IndexResult::IndexResult(SqliteContentIndex* owner, uint64_t epoch, std::vector<RowData> rows, uint64_t total,
                         std::vector<std::vector<MetaValue>> eagerMeta)
    : owner_(owner), impl_(owner), epoch_(epoch), rows_(std::move(rows)), total_(total) {
  for (size_t k = 0; k < eagerMeta.size() && k < rows_.size(); ++k)
    cache_.emplace(static_cast<uint32_t>(k), std::move(eagerMeta[k]));
}

sq_result SQ_CALL IndexResult::getMetadata(uint32_t index, IAttributes* dst) {
  if (!dst) return SQ_E_INVALIDARG;
  if (index >= rows_.size()) return SQ_E_NOTFOUND;
  try {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = cache_.find(index);
    if (it == cache_.end()) {
      std::vector<MetaValue> meta;
      sq_result r = impl_->loadMetadata(epoch_, rows_[index].id, meta);
      if (SQ_FAILED(r)) return r;
      it = cache_.emplace(index, std::move(meta)).first;
    }
    for (const MetaValue& mv : it->second) {
      switch (mv.type) {
        case SQ_VALUE_INT: dst->setInt(mv.key.c_str(), mv.i); break;
        case SQ_VALUE_BOOL: dst->setBool(mv.key.c_str(), mv.i ? SQ_TRUE : SQ_FALSE); break;
        case SQ_VALUE_DOUBLE: dst->setDouble(mv.key.c_str(), mv.d); break;
        default: dst->setString(mv.key.c_str(), mv.s.c_str()); break;
      }
    }
    return SQ_OK;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

}  // namespace

Ptr<IContentIndex> createContentIndex() { return Ptr<IContentIndex>::adopt(new SqliteContentIndex()); }

}  // namespace sq::sqlite_index
