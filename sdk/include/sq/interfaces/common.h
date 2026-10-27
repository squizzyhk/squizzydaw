/* Squizzy Studio - shared helper interfaces used by several area headers.
   OWNER: orchestrator (frozen). Implemented by whoever needs an instance; sq/cpp/attributes.hpp
   and sq/cpp/bytestream.hpp give ready-made header-only implementations.
   THREADING: none of these objects is thread-safe unless the provider says so; never use them on
   the audio thread (they may allocate). Observers are called on the main thread unless the
   subject's header says otherwise. */
#ifndef SQ_INTERFACES_COMMON_H
#define SQ_INTERFACES_COMMON_H

#include "sq/module.h"

/* IAttributes value types */
#define SQ_VALUE_NONE   0
#define SQ_VALUE_INT    1 /* int64 */
#define SQ_VALUE_DOUBLE 2
#define SQ_VALUE_BOOL   3
#define SQ_VALUE_STRING 4 /* UTF-8 */
#define SQ_VALUE_GUID   5
#define SQ_VALUE_BLOB   6 /* raw bytes */
#define SQ_VALUE_OBJECT 7 /* IUnknown*, add-ref'd while stored */

/* IByteStream::seek origins */
#define SQ_SEEK_SET 0
#define SQ_SEEK_CUR 1
#define SQ_SEEK_END 2

#ifdef __cplusplus
namespace sq {

/* Ordered key/value bag: action parameters, codec options, index metadata, script values.
   Keys are UTF-8, case-sensitive, unique; insertion order is preserved and setting an existing
   key keeps its position. Getters convert int<->double<->bool where sensible, else return the
   default / SQ_E_TYPE. Strings and blobs come out through caller buffers (contract 4.3). */
struct IAttributes : IUnknown {
  SQ_INTERFACE(IAttributes, IUnknown, "{CDB4583C-29D7-4136-842B-2CFF4A45E33A}")
  virtual uint32_t SQ_CALL getCount() = 0;
  virtual sq_result SQ_CALL getKey(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual int32_t SQ_CALL getType(const char* key) = 0; /* SQ_VALUE_NONE if missing */
  virtual int64_t SQ_CALL getInt(const char* key, int64_t def) = 0;
  virtual double SQ_CALL getDouble(const char* key, double def) = 0;
  virtual sq_bool SQ_CALL getBool(const char* key, sq_bool def) = 0;
  virtual sq_result SQ_CALL getString(const char* key, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL getGuid(const char* key, sq_guid* out) = 0;
  virtual sq_result SQ_CALL getBlob(const char* key, void* buf, uint32_t bufSize, uint32_t* needed) = 0;
  /* add-ref'd object (QI'd to riid) */
  virtual sq_result SQ_CALL getObject(const char* key, const sq_guid* riid, void** out) = 0;
  virtual sq_result SQ_CALL setInt(const char* key, int64_t value) = 0;
  virtual sq_result SQ_CALL setDouble(const char* key, double value) = 0;
  virtual sq_result SQ_CALL setBool(const char* key, sq_bool value) = 0;
  virtual sq_result SQ_CALL setString(const char* key, const char* value) = 0;
  virtual sq_result SQ_CALL setGuid(const char* key, const sq_guid* value) = 0;
  virtual sq_result SQ_CALL setBlob(const char* key, const void* data, uint32_t size) = 0;
  virtual sq_result SQ_CALL setObject(const char* key, IUnknown* value) = 0;
  virtual sq_result SQ_CALL remove(const char* key) = 0; /* SQ_S_FALSE if absent */
  virtual void SQ_CALL clear() = 0;
};

/* Seekable byte stream (files, memory, archive members, plugin state, package resources). */
struct IByteStream : IUnknown {
  SQ_INTERFACE(IByteStream, IUnknown, "{881EBDD6-F10B-4CE0-8E4C-17485CB01FB4}")
  /* *got < size only at end of stream. */
  virtual sq_result SQ_CALL read(void* buf, uint32_t size, uint32_t* got) = 0;
  virtual sq_result SQ_CALL write(const void* data, uint32_t size, uint32_t* written) = 0; /* SQ_E_ACCESS if read-only */
  virtual sq_result SQ_CALL seek(int64_t offset, int32_t origin, int64_t* newPos) = 0; /* SQ_E_UNSUPPORTED if not seekable */
  virtual sq_result SQ_CALL tell(int64_t* pos) = 0;
  virtual sq_result SQ_CALL getSize(int64_t* size) = 0; /* SQ_E_UNSUPPORTED if unknown */
  virtual sq_result SQ_CALL flush() = 0;
};

/* Generic change notification (also the scripting layer's IObserver analogue).
   `topic` is a dotted string defined by the subject's header (e.g. "document.changed").
   `args` is borrowed for the call (may be null). */
struct IObserver : IUnknown {
  SQ_INTERFACE(IObserver, IUnknown, "{513ECDA7-D776-4C7C-9678-63DED45D861E}")
  virtual void SQ_CALL onNotify(IUnknown* subject, const char* topic, IAttributes* args) = 0;
};

/* Anything observable. Observers are add-ref'd until removed; adding twice is SQ_S_FALSE. */
struct ISubject : IUnknown {
  SQ_INTERFACE(ISubject, IUnknown, "{F7C20B9E-F572-4345-9B6D-3D5A7147B613}")
  virtual sq_result SQ_CALL addObserver(IObserver* observer) = 0;
  virtual sq_result SQ_CALL removeObserver(IObserver* observer) = 0;
};

}  // namespace sq

typedef sq::IAttributes sq_IAttributes;
typedef sq::IByteStream sq_IByteStream;
#else
/* C: opaque handles so POD structs below and in other area headers stay valid C. */
typedef struct sq_IAttributes sq_IAttributes;
typedef struct sq_IByteStream sq_IByteStream;
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_COMMON_H */
