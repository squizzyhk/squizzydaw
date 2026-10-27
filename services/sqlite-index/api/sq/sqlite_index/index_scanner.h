/* Squizzy Studio - content index folder scanner (extension of interfaces/index.h).
   OWNER: services/sqlite-index. Not a frozen area header: it lives in the service directory
   until the orchestrator promotes it (requested). Consumers add the CMake target
   sq::sqlite_index_api and #include "sq/sqlite_index/index_scanner.h".
   IMPLEMENTS: services/sqlite-index (class kIndexScannerClassId, category "IndexScanner").
   CONSUMES: ui (browser "rescan folder"), tools (indexer).

   The scanner walks a folder on its own worker thread and feeds any IContentIndex (normally the
   sqlite one) with audio / MIDI / preset files. It reads METADATA ONLY (container headers, tag
   chunks, MIDI meta events, preset metainfo.xml); it never decodes audio. Rescans are
   incremental: files whose mtime and size match the index are skipped, and with
   SQ_INDEX_SCAN_PRUNE items under the root whose file vanished are removed. Pruning only
   touches items of the types the scan looked for (its extension filter), and never items below
   a folder that could not be listed (counted in `errors`). Folder links / junctions are
   followed, but every physical folder is walked once (loops terminate); depth is capped at 64.
   Hidden entries (name starting with '.') are skipped.

   Item urls are absolute paths with '/' separators. Item kinds: "audio", "midi", "preset".
   Metadata keys written (when known):
     file.ext, file.name
     audio.container, audio.sample_rate, audio.channels, audio.bits, audio.frames,
     audio.duration (seconds, double), audio.root_note, audio.loop (bool)
     midi.format, midi.tracks, midi.ppq, midi.tempo (bpm, double), midi.signature ("4/4"),
     midi.key ("A minor"), midi.length_ticks, midi.duration (seconds), midi.notes, midi.track_names
     preset.<key> (every key/value of the preset's metainfo.xml)
     title, artist, genre, comment, description, bpm, key (text tags of the file)
   Tags added automatically: the kind, the file extension, "loop" for looped/looping files,
   "<n>bpm" when a tempo is known, the genre/category, plus sq_index_scan_options.extra_tags.

   THREADING: every method is thread-safe and returns quickly; the scan itself runs on a
   private worker thread. Never call from the audio thread. Releasing the scanner cancels and
   joins a running scan. */
#ifndef SQ_SQLITE_INDEX_INDEX_SCANNER_H
#define SQ_SQLITE_INDEX_INDEX_SCANNER_H

#include "sq/interfaces/index.h"

/* sq_index_scan_options.flags */
#define SQ_INDEX_SCAN_RECURSIVE 0x01u /* descend into sub folders */
#define SQ_INDEX_SCAN_PRUNE     0x02u /* remove items under root whose file no longer exists */
#define SQ_INDEX_SCAN_FORCE     0x04u /* re-read every file even when mtime/size are unchanged */
#define SQ_INDEX_SCAN_DEFAULT   (SQ_INDEX_SCAN_RECURSIVE | SQ_INDEX_SCAN_PRUNE)

/* sq_index_scan_status.state */
#define SQ_INDEX_SCAN_IDLE      0
#define SQ_INDEX_SCAN_RUNNING   1
#define SQ_INDEX_SCAN_DONE      2
#define SQ_INDEX_SCAN_CANCELLED 3
#define SQ_INDEX_SCAN_FAILED    4

typedef struct sq_index_scan_options {
  uint32_t struct_size;
  const char* root;        /* folder to scan, UTF-8 (copied) */
  const char* extensions;  /* ';'-list without dots ("wav;mid"); NULL/"" = every known type */
  const char* extra_tags;  /* ';'-list added to every indexed item; may be NULL */
  uint32_t flags;          /* SQ_INDEX_SCAN_*; 0 = non-recursive, no pruning */
  uint32_t batch_size;     /* items per index transaction; 0 = 256 */
} sq_index_scan_options;

typedef struct sq_index_scan_status {
  uint32_t struct_size;
  int32_t state;            /* SQ_INDEX_SCAN_* */
  sq_result result;         /* final result once DONE/FAILED/CANCELLED */
  uint64_t files_seen;      /* candidate files found (matching extension) */
  uint64_t files_indexed;   /* (re)written into the index */
  uint64_t files_unchanged; /* skipped: same mtime and size */
  uint64_t files_removed;   /* pruned items */
  uint64_t errors;          /* unreadable files / failed writes (scan continues) */
  int64_t elapsed_ms;
} sq_index_scan_status;

#ifdef __cplusplus
namespace sq {

struct IIndexScanner : IUnknown {
  SQ_INTERFACE(IIndexScanner, IUnknown, "{37217D22-35D5-48FD-A3F0-D7DACA4FD7CC}")
  /* Starts an asynchronous scan into `index` (add-ref'd for the scan's duration).
     SQ_E_BUSY if a scan is running, SQ_E_NOTFOUND if root is not a folder. */
  virtual sq_result SQ_CALL start(IContentIndex* index, const sq_index_scan_options* options) = 0;
  /* Requests cancellation; SQ_S_FALSE if nothing is running. Items already written stay. */
  virtual sq_result SQ_CALL cancel() = 0;
  /* Waits for the running scan to finish: SQ_OK (also when idle) or SQ_E_TIMEOUT.
     timeout_ms < 0 = infinite. */
  virtual sq_result SQ_CALL wait(int32_t timeout_ms) = 0;
  virtual sq_result SQ_CALL getStatus(sq_index_scan_status* out) = 0;
};

namespace index {
inline constexpr const char* kScannerCategory = "IndexScanner";
inline constexpr sq_guid kIndexScannerClassId = sq::guid("{D7D2AD67-6AD3-41B7-8811-ADC0A9376657}");
}  // namespace index

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_SQLITE_INDEX_INDEX_SCANNER_H */
