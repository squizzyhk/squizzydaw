/* Squizzy Studio - content browser index (plan 2.6 "*.tags.db", M3).
   OWNER: orchestrator (frozen).
   IMPLEMENTS: services/sqlite-index (IContentIndex class, category "Index"; one instance per
               store file).
   CONSUMES: ui (browser), tools (indexer), formats (preset metadata), scripting-js, pluginscanner.
   THREADING: an IContentIndex instance is thread-safe (calls are serialized internally) but may
   block on disk: never call it from the audio thread, and run bulk work on sq_pool workers.
   IIndexResult objects are immutable snapshots, safe to read from any non-RT thread.
   Tags: case-insensitive, stored lowercase, ';'-separated in string form ("drums;loop;120bpm").
   Items are keyed by url (a file path or a package:// / theme:// / plugin:<format>:<id> URL). */
#ifndef SQ_INTERFACES_INDEX_H
#define SQ_INTERFACES_INDEX_H

#include "sq/module.h"
#include "sq/interfaces/common.h"

/* IContentIndex::open flags */
#define SQ_INDEX_CREATE   0x01u
#define SQ_INDEX_READONLY 0x02u

/* sq_index_query.sort */
#define SQ_INDEX_SORT_RELEVANCE 0
#define SQ_INDEX_SORT_TITLE     1
#define SQ_INDEX_SORT_MTIME     2

typedef struct sq_index_item {
  uint32_t struct_size;
  const char* url;        /* required, unique key */
  const char* kind;       /* "audio", "midi", "preset", "plugin", "project", "loop", ... */
  const char* title;
  const char* tags;       /* ';'-separated, may be NULL */
  int64_t mtime;          /* unix seconds of the source, for incremental rescans */
  int64_t size;
} sq_index_item;

/* Row of a result; pointers valid while the IIndexResult lives. */
typedef struct sq_index_row {
  uint32_t struct_size;
  uint64_t id;
  const char* url;
  const char* kind;
  const char* title;
  const char* tags;
  int64_t mtime;
  int64_t size;
  double score;           /* relevance, higher = better */
} sq_index_row;

typedef struct sq_index_query {
  uint32_t struct_size;
  const char* text;       /* words matched (prefix, all words) against title, tags and metadata text; NULL/"" = all */
  const char* tags_all;   /* ';'-list: item must have every tag */
  const char* tags_any;   /* ';'-list: item must have at least one */
  const char* kind;       /* exact kind or NULL */
  const char* url_prefix; /* e.g. a folder path; NULL = anywhere */
  int32_t sort;           /* SQ_INDEX_SORT_* */
  uint32_t offset;
  uint32_t limit;         /* 0 = no limit */
} sq_index_query;

typedef struct sq_index_store_info {
  uint32_t struct_size;
  uint32_t schema_version;
  uint64_t item_count;
  int64_t created;        /* unix seconds */
  int64_t modified;
  char name[128];
} sq_index_store_info;

typedef void(SQ_CALL* sq_index_tag_fn)(void* user, const char* tag, uint64_t count);

#ifdef __cplusplus
namespace sq {

struct IIndexResult : IUnknown {
  SQ_INTERFACE(IIndexResult, IUnknown, "{DBAF6169-3A43-4E04-8F7B-7605A6C2B2C6}")
  virtual uint32_t SQ_CALL getCount() = 0;
  virtual uint64_t SQ_CALL getTotalCount() = 0; /* matches ignoring offset/limit */
  virtual sq_result SQ_CALL getRow(uint32_t index, sq_index_row* out) = 0;
  virtual sq_result SQ_CALL getMetadata(uint32_t index, IAttributes* dst) = 0;
};

struct IContentIndex : IUnknown {
  SQ_INTERFACE(IContentIndex, IUnknown, "{A0CBFEC9-D099-40A8-8608-2944BF68AEA8}")
  virtual sq_result SQ_CALL open(const char* path, uint32_t flags) = 0;
  virtual sq_result SQ_CALL close() = 0;
  virtual sq_result SQ_CALL getStoreInfo(sq_index_store_info* out) = 0;
  virtual sq_result SQ_CALL setStoreName(const char* name) = 0;
  /* Batches group many writes into one disk transaction (required for fast bulk indexing). */
  virtual sq_result SQ_CALL beginBatch() = 0;
  virtual sq_result SQ_CALL commitBatch() = 0;
  virtual sq_result SQ_CALL rollbackBatch() = 0;
  /* Insert or replace by url. metadata (nullable): free key/values (string/int/double/bool),
     string values are searchable text. *id (nullable) receives the item id. */
  virtual sq_result SQ_CALL putItem(const sq_index_item* item, IAttributes* metadata, uint64_t* id) = 0;
  virtual sq_result SQ_CALL removeItem(const char* url) = 0; /* SQ_S_FALSE if absent */
  virtual sq_result SQ_CALL removeByPrefix(const char* urlPrefix, uint64_t* removed) = 0;
  virtual sq_result SQ_CALL addTags(const char* url, const char* tags) = 0;
  virtual sq_result SQ_CALL removeTags(const char* url, const char* tags) = 0;
  virtual sq_result SQ_CALL query(const sq_index_query* q, IIndexResult** out) = 0;
  /* Every tag with its item count, filtered by prefix (NULL = all), sorted by tag. */
  virtual sq_result SQ_CALL listTags(const char* prefix, sq_index_tag_fn fn, void* user) = 0;
};

namespace index {
inline constexpr const char* kCategory = "Index";
inline constexpr sq_guid kSqliteIndexClassId = sq::guid("{9C0BC7B5-816A-4369-9E06-2402F42BECFA}");
}  // namespace index

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_INDEX_H */
