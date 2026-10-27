/* Squizzy Studio - scripting engine + host object registry (plan 2.4, M6).
   OWNER: orchestrator (frozen).
   IMPLEMENTS: services/scripting-js (IScriptEngine service; IObjectRegistry service; registers each
               package <ScriptClass> with IClassRegistry::registerClass, category from the package,
               default "ScriptClass").
   CONSUMES: app (menu commands), ui, engine (registers the "object://doc/" resolver), formats.
   THREADING: the script engine is single-threaded: all IScriptEngine calls and every call INTO a
   script-implemented object must happen on the main thread. IObjectRegistry is thread-safe for
   registration, resolution happens on the caller's thread (resolvers must be thread-safe or
   document a main-thread requirement).

   SCRIPT-VISIBLE INTERFACES: script objects are exposed to native code as the interfaces below
   (and IObserver from common.h, IAction from document.h). The JS host API mirrors them:
     Host.Classes.createInstance(guid)          -> IClassRegistry::createInstance
     Host.Objects.getObjectByUrl(url)           -> IObjectRegistry::resolve
     Host.Interfaces.ICommandHandler / IObserver / IController / IComponent
   URL SCHEMES:
     object://<root>/...   live host objects; roots are registered by their owners:
                           object://app/settings/<dotted.key> (app), object://doc/... (engine,
                           see document.h), object://engine (engine)
     package://<packageId>/<path>   resource inside a loaded script/skin package
     theme://<theme>/<path>         theme resource (ui; see ui.h) */
#ifndef SQ_INTERFACES_SCRIPTING_H
#define SQ_INTERFACES_SCRIPTING_H

#include "sq/module.h"
#include "sq/interfaces/common.h"

/* ICommandHandler::interpretCommand result for checkOnly when the command is disabled. */
#define SQ_COMMAND_DISABLED SQ_S_FALSE

typedef struct sq_command {
  uint32_t struct_size;
  const char* category;   /* "Edit", "Transport", "File", ... */
  const char* name;       /* "Split", "Play", ... */
  uint32_t flags;         /* reserved, 0 */
} sq_command;

#ifdef __cplusplus
namespace sq {

/* Menu/key/remote commands. Classes with category "Command" implement this. */
struct ICommandHandler : IUnknown {
  SQ_INTERFACE(ICommandHandler, IUnknown, "{E03E59EA-39AF-45D9-B69F-C14833664CEE}")
  /* checkOnly: return SQ_OK if the command is enabled, SQ_COMMAND_DISABLED if not, SQ_E_NOTFOUND
     if not handled. Otherwise execute it (args borrowed, nullable). */
  virtual sq_result SQ_CALL interpretCommand(const sq_command* command, IAttributes* args, sq_bool checkOnly) = 0;
};

/* Named property + child-object access (UI bindings, remote pages, scripts). Single values go
   through an IAttributes under the key "value". */
struct IController : IUnknown {
  SQ_INTERFACE(IController, IUnknown, "{0195A2A0-D302-4AC3-A70A-05BD56841C38}")
  virtual sq_result SQ_CALL getProperty(const char* name, IAttributes* out) = 0;
  virtual sq_result SQ_CALL setProperty(const char* name, IAttributes* in) = 0;
  virtual sq_result SQ_CALL getChild(const char* name, const sq_guid* riid, void** out) = 0;
};

/* Lifecycle of script/native components (edit tasks, panels, surface handlers). */
struct IComponent : IUnknown {
  SQ_INTERFACE(IComponent, IUnknown, "{3628BD16-F7B6-4A77-B9DE-6776709C3A4A}")
  virtual sq_result SQ_CALL initialize(IUnknown* context) = 0; /* context borrowed for the call */
  virtual sq_result SQ_CALL terminate() = 0;
};

/* Resolves URLs below a prefix registered with IObjectRegistry. */
struct IUrlResolver : IUnknown {
  SQ_INTERFACE(IUrlResolver, IUnknown, "{15DC2E8B-E501-436C-BF22-F283FF496879}")
  virtual sq_result SQ_CALL resolve(const char* url, const sq_guid* riid, void** out) = 0;
  /* Resource bytes (package://, theme://). SQ_E_NOTIMPL for pure object roots. */
  virtual sq_result SQ_CALL openStream(const char* url, IByteStream** out) = 0;
};

/* Host object registry: host->queryService(&IObjectRegistry::iid). Longest prefix wins. */
struct IObjectRegistry : IUnknown {
  SQ_INTERFACE(IObjectRegistry, IUnknown, "{509C7AD6-D6FE-45C1-A332-768DE24182A7}")
  /* prefix like "object://doc/" or "theme://". resolver add-ref'd until unregistered (do it in
     your module's SHUTDOWN). SQ_E_EXISTS if the prefix is taken. */
  virtual sq_result SQ_CALL registerResolver(const char* prefix, IUrlResolver* resolver) = 0;
  virtual sq_result SQ_CALL unregisterResolver(const char* prefix, IUrlResolver* resolver) = 0;
  /* Convenience: a fixed object at an exact URL ("object://engine"). */
  virtual sq_result SQ_CALL registerObject(const char* url, IUnknown* object) = 0;
  virtual sq_result SQ_CALL unregisterObject(const char* url) = 0;
  virtual sq_result SQ_CALL resolve(const char* url, const sq_guid* riid, void** out) = 0;
  virtual sq_result SQ_CALL openStream(const char* url, IByteStream** out) = 0;
};

/* A loaded script package (.package file or directory). Releasing does not unload: call unload(). */
struct IScriptPackage : IUnknown {
  SQ_INTERFACE(IScriptPackage, IUnknown, "{3B3F3AEE-3B2F-4D78-B8B7-8D9D87DB40F9}")
  virtual sq_result SQ_CALL getId(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual uint32_t SQ_CALL getClassCount() = 0;
  /* Strings valid while the package is loaded. */
  virtual sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) = 0;
  /* Unregisters the package's classes; fails SQ_E_BUSY while script objects of it are alive. */
  virtual sq_result SQ_CALL unload() = 0;
};

/* host->queryService(&IScriptEngine::iid). */
struct IScriptEngine : IUnknown {
  SQ_INTERFACE(IScriptEngine, IUnknown, "{C3871EB7-89F0-46A5-A447-EBAEECDEE33C}")
  /* Engine name and version text (for about boxes and bug reports). */
  virtual sq_result SQ_CALL getEngineInfo(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  /* Loads + registers <ClassFactory><ScriptClass .../></ClassFactory> entries. */
  virtual sq_result SQ_CALL loadPackage(const char* pathOrUrl, IScriptPackage** out) = 0;
  /* Evaluates source in the global context. result (nullable) receives key "value" (converted:
     number -> double, string, bool, object -> IController wrapper). On error returns SQ_E_FAIL
     (or SQ_E_PARSE) and getLastError describes it with file:line. */
  virtual sq_result SQ_CALL evaluate(const char* source, const char* sourceName, IAttributes* result) = 0;
  virtual sq_result SQ_CALL getLastError(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  /* Calls a package's factory function (sourceFile + functionName as in <ScriptClass>) and wraps
     the returned JS object as riid (ICommandHandler, IObserver, IController, IComponent, IAction). */
  virtual sq_result SQ_CALL callFactory(const char* packageId, const char* sourceFile, const char* functionName,
                                        const sq_guid* riid, void** out) = 0;
  /* Runs pending jobs/promises and timers; the app calls it from its main-loop idle. */
  virtual sq_result SQ_CALL idle() = 0;
};

namespace scripting {
inline constexpr const char* kScriptClassCategory = "ScriptClass";
inline constexpr const char* kCommandCategory = "Command";
}  // namespace scripting

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_SCRIPTING_H */
