/* Squizzy Studio - document model: read API + command (Action/transaction/undo) API (plan 2.3, M1/M3/M6).
   OWNER: orchestrator (frozen).
   IMPLEMENTS: engine (document tree, XmlArchive, UndoKeeper; registers IDocumentManager with
               host->registerService and every built-in action class in the class registry,
               category "Action").
   CONSUMES: formats (import/export), scripting-js (Host.Objects, macros), ui, app, tools.
   THREADING: MAIN THREAD ONLY for everything in this header (reads included). The engine keeps
   its own RT snapshot of the document; nothing here is touched by the audio thread.

   EDIT RULE: the read interfaces never mutate. Every change goes through IEditor::execute() with
   an action class GUID + IAttributes params, inside a transaction (one is opened implicitly when
   none is). Undo/redo works on whole transactions. Actions implemented outside the engine
   (IAction, e.g. script macros) only compose other actions through the IEditor they receive.

   TIME: timeline positions/lengths are musical (sq_ppq_time, quarter notes from song start);
   ITempoMap converts to seconds. Audio source offsets are in seconds of the source file.

   URLS (object://, resolved by IDocumentManager::findNodeByUrl):
     object://doc/<docGuid>                    a document          (<docGuid> or "active")
     object://doc/<docGuid>/node/<nodeGuid>    any node, by its persistent GUID
     object://doc/active/track/<index>         track by position (convenience, not persistent)
   GUIDs in URLs are written in canonical braced form "{XXXXXXXX-...}"; lookups also accept
   them without braces.

   OBSERVER TOPICS (IObserver::onNotify, subject = the IDocument or the IDocumentManager):
     "document.opened" / "document.closed" / "document.activated" / "document.saved"
         args: docId (guid)
     "document.changed"   after a committed transaction, undo or redo. args: docId (guid),
         reason (string "commit" | "undo" | "redo"), description (string),
         nodes (blob = packed sq_guid[] of changed/added/removed nodes)
     "document.modified"  modified flag toggled. args: docId, modified (bool) */
#ifndef SQ_INTERFACES_DOCUMENT_H
#define SQ_INTERFACES_DOCUMENT_H

#include "sq/media.h"
#include "sq/module.h"
#include "sq/interfaces/common.h"

/* Document kinds */
#define SQ_DOC_SONG    1 /* arrangement */
#define SQ_DOC_PROJECT 2 /* mastering / album assembly */
#define SQ_DOC_SHOW    3 /* live performance set */

/* INode::getKind */
#define SQ_NODE_DOCUMENT  1
#define SQ_NODE_TRACK     2
#define SQ_NODE_CLIP      3
#define SQ_NODE_TEMPO_MAP 4
#define SQ_NODE_MARKER    5
#define SQ_NODE_FOLDER    6  /* track folder / group container */
#define SQ_NODE_OTHER     99 /* engine-private kinds; read via getProperties */

/* Track kinds */
#define SQ_TRACK_AUDIO      1
#define SQ_TRACK_INSTRUMENT 2
#define SQ_TRACK_BUS        3
#define SQ_TRACK_FX         4
#define SQ_TRACK_FOLDER     5
#define SQ_TRACK_MASTER     6

/* sq_track_info.flags */
#define SQ_TRACK_MUTE     0x01u
#define SQ_TRACK_SOLO     0x02u
#define SQ_TRACK_ARMED    0x04u
#define SQ_TRACK_MONITOR  0x08u /* input monitoring on */
#define SQ_TRACK_HIDDEN   0x10u

/* Clip kinds / flags */
#define SQ_CLIP_AUDIO 1
#define SQ_CLIP_MIDI  2
#define SQ_CLIP_MUTED  0x01u
#define SQ_CLIP_LOCKED 0x02u

/* sq_tempo_point.flags / sq_doc_marker.flags */
#define SQ_TEMPO_RAMP   0x01u /* linear ramp to the next point */
#define SQ_DOCMARKER_REGION 0x01u
#define SQ_DOCMARKER_LOOP   0x02u /* the loop range */
#define SQ_DOCMARKER_PUNCH  0x04u /* the punch range */

typedef struct sq_document_info {
  uint32_t struct_size;
  int32_t kind;            /* SQ_DOC_* */
  sq_guid id;
  double sample_rate;      /* document rate */
  sq_bool modified;
  uint32_t track_count;
  sq_ppq_time length;      /* end of last content */
} sq_document_info;

typedef struct sq_track_info {
  uint32_t struct_size;
  int32_t kind;            /* SQ_TRACK_* */
  uint32_t channels;
  uint32_t flags;          /* SQ_TRACK_* */
  uint32_t color;          /* 0xRRGGBBAA */
  double volume_db;
  double pan;              /* -1..1 */
  sq_guid output;          /* destination bus track id; null = master */
  uint32_t clip_count;
} sq_track_info;

typedef struct sq_clip_info {
  uint32_t struct_size;
  int32_t kind;            /* SQ_CLIP_* */
  sq_ppq_time start;
  sq_ppq_time length;
  double source_offset_seconds; /* audio: where in the source file the clip starts */
  double gain_db;
  double fade_in_seconds;
  double fade_out_seconds;
  uint32_t flags;          /* SQ_CLIP_* */
  uint32_t color;
  uint32_t event_count;    /* MIDI */
} sq_clip_info;

/* MIDI clip content. Notes: status 0x9n with length > 0 (note-off implied). Positions relative
   to clip start. No pointers: arrays of this struct can be passed as one blob. */
typedef struct sq_clip_event {
  uint32_t struct_size;
  sq_ppq_time position;
  sq_ppq_time length;
  uint8_t bytes[4];        /* status, data1, data2 */
  uint32_t size;
} sq_clip_event;

typedef struct sq_tempo_point {
  uint32_t struct_size;
  sq_ppq_time position;
  double bpm;
  uint32_t flags;          /* SQ_TEMPO_* */
} sq_tempo_point;

typedef struct sq_time_signature {
  uint32_t struct_size;
  sq_ppq_time position;
  uint32_t numerator;
  uint32_t denominator;
} sq_time_signature;

typedef struct sq_doc_marker {
  uint32_t struct_size;
  sq_guid id;
  sq_ppq_time position;
  sq_ppq_time length;      /* 0 = point */
  uint32_t flags;          /* SQ_DOCMARKER_* */
  uint32_t color;
  char name[128];
} sq_doc_marker;

#ifdef __cplusplus
namespace sq {

struct IEditor;

/* Every element of the tree. QI to the typed views below by kind. */
struct INode : IUnknown {
  SQ_INTERFACE(INode, IUnknown, "{CF16DC04-1B59-48F4-BDAB-82EFF3F38CAB}")
  virtual int32_t SQ_CALL getKind() = 0;
  virtual sq_result SQ_CALL getId(sq_guid* out) = 0; /* persistent, unique within the document */
  virtual sq_result SQ_CALL getName(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL getUrl(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL getParent(INode** out) = 0; /* SQ_E_NOTFOUND for the document */
  virtual uint32_t SQ_CALL getChildCount() = 0;
  virtual sq_result SQ_CALL getChild(uint32_t index, INode** out) = 0;
  /* Copies all properties (documented per kind by the engine; stable names, also used as
     SetProperty keys) into dst. */
  virtual sq_result SQ_CALL getProperties(IAttributes* dst) = 0;
};

struct ITempoMap : INode {
  SQ_INTERFACE(ITempoMap, INode, "{3BC73E4C-358F-45CA-AA84-2D0691E45F38}")
  virtual uint32_t SQ_CALL getTempoCount() = 0;
  virtual sq_result SQ_CALL getTempo(uint32_t index, sq_tempo_point* out) = 0;
  virtual uint32_t SQ_CALL getSignatureCount() = 0;
  virtual sq_result SQ_CALL getSignature(uint32_t index, sq_time_signature* out) = 0;
  virtual double SQ_CALL ppqToSeconds(sq_ppq_time ppq) = 0;
  virtual sq_ppq_time SQ_CALL secondsToPpq(double seconds) = 0;
};

struct IClip : INode {
  SQ_INTERFACE(IClip, INode, "{BC0D42B3-D34B-4C68-B7F4-20277325398B}")
  virtual sq_result SQ_CALL getClipInfo(sq_clip_info* out) = 0;
  /* Audio: absolute path of the media file. */
  virtual sq_result SQ_CALL getSourcePath(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL getEvent(uint32_t index, sq_clip_event* out) = 0;
};

struct ITrack : INode {
  SQ_INTERFACE(ITrack, INode, "{F8A4C035-56FE-40A5-AC02-F47518E7C425}")
  virtual sq_result SQ_CALL getTrackInfo(sq_track_info* out) = 0;
  virtual sq_result SQ_CALL getClip(uint32_t index, IClip** out) = 0; /* sorted by start */
};

/* Root node of a document (kind SQ_NODE_DOCUMENT). QI: ISubject for observers. */
struct IDocument : INode {
  SQ_INTERFACE(IDocument, INode, "{2F51C0CD-531D-402C-ABC7-2B1BE3C131E3}")
  virtual sq_result SQ_CALL getDocumentInfo(sq_document_info* out) = 0;
  virtual sq_result SQ_CALL getPath(char* buf, uint32_t bufSize, uint32_t* needed) = 0; /* "" if unsaved */
  virtual sq_result SQ_CALL getTrack(uint32_t index, ITrack** out) = 0; /* flat, display order */
  virtual sq_result SQ_CALL getTempoMap(ITempoMap** out) = 0;
  virtual uint32_t SQ_CALL getMarkerCount() = 0;
  virtual sq_result SQ_CALL getMarker(uint32_t index, sq_doc_marker* out) = 0; /* sorted by position */
  virtual sq_result SQ_CALL findNode(const sq_guid* id, INode** out) = 0;
  virtual sq_result SQ_CALL getEditor(IEditor** out) = 0;
};

/* Transactions + undo for one document. */
struct IEditor : IUnknown {
  SQ_INTERFACE(IEditor, IUnknown, "{34934659-00B1-4FE6-9E02-5E6233C04E6A}")
  /* Nestable: inner commit/cancel only close the inner level; the outermost commit creates one
     undo step named by the outermost description. */
  virtual sq_result SQ_CALL beginTransaction(const char* description) = 0;
  virtual sq_result SQ_CALL commitTransaction() = 0;
  /* Rolls back every action executed since the matching begin. */
  virtual sq_result SQ_CALL cancelTransaction() = 0;
  virtual sq_bool SQ_CALL inTransaction() = 0;
  /* Runs the action class `actionClassId` (class registry, category "Action"). params borrowed;
     result (nullable) receives outputs such as "node" (guid of a created node). A failing action
     leaves the document unchanged and returns its error. */
  virtual sq_result SQ_CALL execute(const sq_guid* actionClassId, IAttributes* params, IAttributes* result) = 0;
  virtual sq_bool SQ_CALL canUndo() = 0;
  virtual sq_bool SQ_CALL canRedo() = 0;
  virtual sq_result SQ_CALL undo() = 0;
  virtual sq_result SQ_CALL redo() = 0;
  virtual uint32_t SQ_CALL getUndoCount() = 0;
  virtual uint32_t SQ_CALL getRedoCount() = 0;
  /* index 0 = next step to undo/redo */
  virtual sq_result SQ_CALL getUndoDescription(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL getRedoDescription(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL clearHistory() = 0;
};

/* Action implemented outside the engine (category "Action", e.g. script macros). It runs
   inside an open transaction and may only call editor->execute() / read the document. */
struct IAction : IUnknown {
  SQ_INTERFACE(IAction, IUnknown, "{5E1C622A-E658-40F8-BE82-BA9334B98346}")
  virtual sq_result SQ_CALL perform(IDocument* doc, IEditor* editor, IAttributes* params, IAttributes* result) = 0;
};

/* Engine singleton: host->queryService(&IDocumentManager::iid). QI: ISubject. */
struct IDocumentManager : IUnknown {
  SQ_INTERFACE(IDocumentManager, IUnknown, "{70B84E7D-F098-46A6-893A-B62ABCBC65BF}")
  virtual sq_result SQ_CALL createDocument(int32_t kind, IDocument** out) = 0;
  virtual sq_result SQ_CALL openDocument(const char* path, IDocument** out) = 0;
  /* path NULL = current path. Saving is not undoable and does not need a transaction. */
  virtual sq_result SQ_CALL saveDocument(IDocument* doc, const char* path) = 0;
  virtual sq_result SQ_CALL closeDocument(IDocument* doc) = 0;
  virtual uint32_t SQ_CALL getDocumentCount() = 0;
  virtual sq_result SQ_CALL getDocument(uint32_t index, IDocument** out) = 0;
  virtual sq_result SQ_CALL getActiveDocument(IDocument** out) = 0; /* SQ_E_NOTFOUND if none */
  virtual sq_result SQ_CALL setActiveDocument(IDocument* doc) = 0;
  /* object:// lookup (see URLS above). */
  virtual sq_result SQ_CALL findNodeByUrl(const char* url, INode** out) = 0;
};

namespace doc {
inline constexpr const char* kActionCategory = "Action";

/* Built-in action classes (implemented by the engine). Param keys and types:
   kSetProperty   node:guid key:string value:any                    (any property from getProperties)
   kAddTrack      kind:int(SQ_TRACK_*) name:string? index:int? parent:guid? channels:int?  -> node
   kRemoveNode    node:guid                                          (tracks, clips, markers)
   kAddClip       track:guid kind:int start:double length:double source:string?
                  source_offset:double?                              -> node
   kMoveClip      node:guid start:double track:guid?
   kResizeClip    node:guid start:double length:double               (trim; keeps source alignment)
   kSplitClip     node:guid at:double                                -> node (the right part)
   kSetClipEvents clip:guid events:blob(sq_clip_event[])             (replaces all events)
   kAddMarker     position:double length:double? name:string? flags:int? -> node
   kSetTempo      position:double bpm:double ramp:bool?              (adds or replaces a point)
   kRemoveTempo   index:int
   kSetSignature  position:double numerator:int denominator:int */
inline constexpr sq_guid kSetProperty = sq::guid("{A74C9D29-533B-4CD3-894E-6BB31847632E}");
inline constexpr sq_guid kAddTrack = sq::guid("{B23ED9B8-02F8-409B-933C-35A45DE288AC}");
inline constexpr sq_guid kRemoveNode = sq::guid("{D3573E2F-2FC0-4723-9898-8B1B1BC329B2}");
inline constexpr sq_guid kAddClip = sq::guid("{BF344A8B-2F75-4C13-8C70-50B0E99A14B6}");
inline constexpr sq_guid kMoveClip = sq::guid("{D693B087-9EE7-4086-9A61-FE68D79A6F58}");
inline constexpr sq_guid kResizeClip = sq::guid("{F901C047-161A-4ABA-9698-A640C2B73A03}");
inline constexpr sq_guid kSplitClip = sq::guid("{DEBCF0FF-26CE-48D7-A769-0C9405CEA544}");
inline constexpr sq_guid kSetClipEvents = sq::guid("{80E21F18-7D4F-4AF6-901D-DA515E282435}");
inline constexpr sq_guid kAddMarker = sq::guid("{2E5F652A-1427-4E61-BAAB-65A7F32E9085}");
inline constexpr sq_guid kSetTempo = sq::guid("{A44A21D4-C33E-47D0-AEFC-EE8C6615A988}");
inline constexpr sq_guid kRemoveTempo = sq::guid("{D9838C9C-7AE9-431A-BB13-5B64D9E9163B}");
inline constexpr sq_guid kSetSignature = sq::guid("{F670E578-6E20-419A-BBC3-D267F8592828}");
}  // namespace doc

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_DOCUMENT_H */
