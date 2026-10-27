/* Squizzy Studio SDK - module ABI: class factory, host context, settings, class registry.

   Every module DLL (bin/services/<name>.dll) exports EXACTLY two C symbols:

     sq_IClassFactory* SQ_CALL GetClassFactory(void);
         Returns the module's lazily-constructed singleton factory, ADD-REF'D (caller releases).
         Must be side-effect free (the loader calls it before deciding to accept the module).

     sq_result SQ_CALL ModuleMain(uint32_t reason, sq_IHostContext* host);
         reason == SQ_MODULE_INIT:     version handshake + init. Return SQ_E_ABI_MISMATCH if
                                       host->getHostAbi() is not acceptable; any failure => the
                                       module is unloaded and its classes never registered.
                                       `host` stays valid until SHUTDOWN returns.
         reason == SQ_MODULE_SHUTDOWN: release everything taken from the host (services,
                                       observers, log sinks). Called before the DLL is unloaded.

   Load sequence (app/ModuleLoader): open lib -> resolve both exports -> GetClassFactory ->
   getModuleInfo -> reject if SQ_PLATFORM_ABI of the host is outside [abi_min, abi_max] ->
   ModuleMain(INIT) -> register classes. Unload: unregister classes -> ModuleMain(SHUTDOWN) ->
   if canUnload() (no live objects, factory unreferenced): release factory -> close lib;
   otherwise roll back (ModuleMain(INIT) again, re-register classes) and report busy. So INIT
   may follow SHUTDOWN in the same loaded image: SHUTDOWN must reset all module state.
*/
#ifndef SQ_MODULE_H
#define SQ_MODULE_H

#include "sq/unknown.h"

#define SQ_EXPORT_GET_CLASS_FACTORY "GetClassFactory"
#define SQ_EXPORT_MODULE_MAIN "ModuleMain"

#define SQ_MODULE_INIT 1u
#define SQ_MODULE_SHUTDOWN 2u

/* Metadata a module reports through IClassFactory::getModuleInfo. Strings are owned by the
   module and only valid while it is loaded: the host copies them. */
typedef struct sq_module_info {
  uint32_t struct_size;     /* = sizeof(sq_module_info) */
  uint32_t fourcc;          /* SQ_FOURCC tag, unique per module */
  const char* name;         /* unique module name, e.g. "audiodriver-wasapi" */
  uint32_t version;         /* SQ_MAKE_VERSION of the module */
  uint32_t abi_min;         /* required platform ABI range (SQ_ABI_MAKE) */
  uint32_t abi_max;
  uint32_t sdk_abi;         /* SQ_PLATFORM_ABI the module was compiled against (stamp) */
  const char* build_stamp;  /* SQ_BUILD_STAMP of the SDK it was built with */
  const char* description;  /* human readable, may be "" */
} sq_module_info;

/* Category strings in use (free-form, extend as needed): "Service", "AudioDriver", "MidiDriver",
   "PluginHost", "Codec", "Analysis", "Command", "EditTask", "ScriptClass", "Test". */
typedef struct sq_class_info {
  uint32_t struct_size;   /* = sizeof(sq_class_info) */
  sq_guid class_id;
  const char* category;
  const char* name;
  uint32_t flags;         /* reserved, 0 */
  uint32_t module_fourcc; /* filled in by the registry; 0 for dynamically registered classes */
} sq_class_info;

/* Settings value types (ISettings::getType). */
#define SQ_SETTING_NONE   0
#define SQ_SETTING_STRING 1
#define SQ_SETTING_INT    2
#define SQ_SETTING_DOUBLE 3
#define SQ_SETTING_BOOL   4

typedef void(SQ_CALL* sq_settings_enum_fn)(void* user, const char* key);

/* {6DF0A651-4ACD-4400-8581-DD69D80D976A} */
#define SQ_IID_ICLASSFACTORY_INIT SQ_GUID_INIT(0x6DF0A651, 0x4ACD, 0x4400, 0x85, 0x81, 0xDD, 0x69, 0xD8, 0x0D, 0x97, 0x6A)
/* {9D626F83-62FD-408C-B6F7-3C9D9A4BC509} */
#define SQ_IID_IHOSTCONTEXT_INIT SQ_GUID_INIT(0x9D626F83, 0x62FD, 0x408C, 0xB6, 0xF7, 0x3C, 0x9D, 0x9A, 0x4B, 0xC5, 0x09)

#ifdef __cplusplus
namespace sq {

struct ISettingsObserver : IUnknown {
  SQ_INTERFACE(ISettingsObserver, IUnknown, "{9377654A-182E-4391-B28B-4A7047AEC1ED}")
  /* Called on the thread that changed the value, after the settings lock is released.
     `key` is the full dotted key that changed (or was removed). */
  virtual void SQ_CALL onSettingChanged(const char* key) = 0;
};

/* Hierarchical dotted-key registry: "Engine.Audio.BlockSize". Segments: [A-Za-z0-9_-]+.
   Top-level namespaces: Engine.*, Devices.*, Media.*, Platform.*, UI.*, Services.<module>.*
   Thread-safe. Never call from the audio thread. */
struct ISettings : IUnknown {
  SQ_INTERFACE(ISettings, IUnknown, "{E4960329-896F-47FC-BAEE-7F6C56D323BE}")
  virtual sq_bool SQ_CALL has(const char* key) = 0;
  virtual int32_t SQ_CALL getType(const char* key) = 0;
  /* Any type, as text. *needed (optional) receives the byte size incl. NUL.
     Returns SQ_E_NOTFOUND or SQ_E_BUFFER_TOO_SMALL (buffer gets a truncated NUL-terminated copy). */
  virtual sq_result SQ_CALL getString(const char* key, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual int64_t SQ_CALL getInt(const char* key, int64_t def) = 0;
  virtual double SQ_CALL getDouble(const char* key, double def) = 0;
  virtual sq_bool SQ_CALL getBool(const char* key, sq_bool def) = 0;
  virtual sq_result SQ_CALL setString(const char* key, const char* value) = 0;
  virtual sq_result SQ_CALL setInt(const char* key, int64_t value) = 0;
  virtual sq_result SQ_CALL setDouble(const char* key, double value) = 0;
  virtual sq_result SQ_CALL setBool(const char* key, sq_bool value) = 0;
  /* Removes the key and every key below it. */
  virtual sq_result SQ_CALL remove(const char* key) = 0;
  /* Calls fn for each key equal to or below prefix ("" = all), sorted. Snapshot: safe to mutate in fn. */
  virtual sq_result SQ_CALL enumerate(const char* prefix, sq_settings_enum_fn fn, void* user) = 0;
  /* Observer is add-ref'd until removeObserver. prefix "" observes everything. */
  virtual sq_result SQ_CALL addObserver(const char* prefix, ISettingsObserver* observer, uint32_t* cookie) = 0;
  virtual sq_result SQ_CALL removeObserver(uint32_t cookie) = 0;
};

struct IClassFactory : IUnknown {
  static constexpr sq_guid iid = sq_guid SQ_IID_ICLASSFACTORY_INIT;
  using sq_base = IUnknown;
  virtual sq_result SQ_CALL getModuleInfo(sq_module_info* out) = 0;
  virtual uint32_t SQ_CALL getClassCount() = 0;
  virtual sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) = 0;
  virtual sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) = 0;
  /* True when no object created by this module is alive (the module may be unloaded). */
  virtual sq_bool SQ_CALL canUnload() = 0;
};

/* GUID-addressed registry of every class in the app: native module classes AND dynamically
   registered (script) classes. Strings in returned sq_class_info are valid until the next
   registry mutation (module load/unload/register): copy them if you keep them. */
struct IClassRegistry : IUnknown {
  SQ_INTERFACE(IClassRegistry, IUnknown, "{95AEB0A9-0EB6-433B-8482-CE9B967178EF}")
  virtual uint32_t SQ_CALL getClassCount() = 0;
  virtual sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) = 0;
  virtual sq_result SQ_CALL findClass(const sq_guid* classId, sq_class_info* out) = 0;
  virtual sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) = 0;
  /* Dynamic classes (e.g. script classes): `factory` is add-ref'd and serves createInstance. */
  virtual sq_result SQ_CALL registerClass(const sq_class_info* info, IClassFactory* factory) = 0;
  virtual sq_result SQ_CALL unregisterClass(const sq_guid* classId) = 0;
};

/* Passed to ModuleMain. Host-owned; valid from INIT until SHUTDOWN returns. */
struct IHostContext : IUnknown {
  static constexpr sq_guid iid = sq_guid SQ_IID_IHOSTCONTEXT_INIT;
  using sq_base = IUnknown;
  virtual uint32_t SQ_CALL getHostAbi() = 0;     /* SQ_PLATFORM_ABI of the running host */
  virtual uint32_t SQ_CALL getHostVersion() = 0; /* SQ_VERSION of the app */
  virtual void SQ_CALL log(int32_t level, const char* tag, const char* message) = 0;
  virtual ISettings* SQ_CALL getSettings() = 0;           /* borrowed, never null */
  virtual IClassRegistry* SQ_CALL getClassRegistry() = 0; /* borrowed, never null */
  /* Process-wide service locator keyed by interface IID (singletons such as the engine).
     queryService returns an add-ref'd pointer or SQ_E_NOTFOUND. A module must unregister what
     it registered before/at SHUTDOWN. */
  virtual sq_result SQ_CALL queryService(const sq_guid* riid, void** out) = 0;
  virtual sq_result SQ_CALL registerService(const sq_guid* riid, IUnknown* service) = 0;
  virtual sq_result SQ_CALL unregisterService(const sq_guid* riid, IUnknown* service) = 0;
};

}  // namespace sq

typedef sq::IClassFactory sq_IClassFactory;
typedef sq::IHostContext sq_IHostContext;
typedef sq::ISettings sq_ISettings;
typedef sq::IClassRegistry sq_IClassRegistry;

#else /* ---------------- C view of the loader-facing interfaces ---------------- */

typedef struct sq_ISettings sq_ISettings;           /* opaque in C */
typedef struct sq_IClassRegistry sq_IClassRegistry; /* opaque in C */

typedef struct sq_IClassFactory sq_IClassFactory;
typedef struct sq_IClassFactoryVtbl {
  sq_result(SQ_CALL* queryInterface)(sq_IClassFactory* self, const sq_guid* riid, void** out);
  uint32_t(SQ_CALL* addRef)(sq_IClassFactory* self);
  uint32_t(SQ_CALL* release)(sq_IClassFactory* self);
  sq_result(SQ_CALL* getModuleInfo)(sq_IClassFactory* self, sq_module_info* out);
  uint32_t(SQ_CALL* getClassCount)(sq_IClassFactory* self);
  sq_result(SQ_CALL* getClassInfo)(sq_IClassFactory* self, uint32_t index, sq_class_info* out);
  sq_result(SQ_CALL* createInstance)(sq_IClassFactory* self, const sq_guid* classId, const sq_guid* riid, void** out);
  sq_bool(SQ_CALL* canUnload)(sq_IClassFactory* self);
} sq_IClassFactoryVtbl;
struct sq_IClassFactory {
  const sq_IClassFactoryVtbl* vtbl;
};

typedef struct sq_IHostContext sq_IHostContext;
typedef struct sq_IHostContextVtbl {
  sq_result(SQ_CALL* queryInterface)(sq_IHostContext* self, const sq_guid* riid, void** out);
  uint32_t(SQ_CALL* addRef)(sq_IHostContext* self);
  uint32_t(SQ_CALL* release)(sq_IHostContext* self);
  uint32_t(SQ_CALL* getHostAbi)(sq_IHostContext* self);
  uint32_t(SQ_CALL* getHostVersion)(sq_IHostContext* self);
  void(SQ_CALL* log)(sq_IHostContext* self, int32_t level, const char* tag, const char* message);
  sq_ISettings*(SQ_CALL* getSettings)(sq_IHostContext* self);
  sq_IClassRegistry*(SQ_CALL* getClassRegistry)(sq_IHostContext* self);
  sq_result(SQ_CALL* queryService)(sq_IHostContext* self, const sq_guid* riid, void** out);
  sq_result(SQ_CALL* registerService)(sq_IHostContext* self, const sq_guid* riid, sq_IUnknown* service);
  sq_result(SQ_CALL* unregisterService)(sq_IHostContext* self, const sq_guid* riid, sq_IUnknown* service);
} sq_IHostContextVtbl;
struct sq_IHostContext {
  const sq_IHostContextVtbl* vtbl;
};

#endif /* __cplusplus */

typedef sq_IClassFactory*(SQ_CALL* sq_GetClassFactoryFn)(void);
typedef sq_result(SQ_CALL* sq_ModuleMainFn)(uint32_t reason, sq_IHostContext* host);

#endif /* SQ_MODULE_H */
