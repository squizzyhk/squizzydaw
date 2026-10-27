// scripting-js internals: script engine, packages, script objects, object registry.
// Everything here is private to the service DLL. Threading: the JS runtime is single-threaded and
// bound to the thread that created the engine (the app main thread); calls from any other thread
// are refused with SQ_E_ACCESS. IObjectRegistry is thread-safe.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "qjs.hpp"
#include "sq/interfaces/document.h"
#include "sq/interfaces/scripting.h"
#include "sq/sdk.hpp"

namespace sqjs {

class Engine;
class Package;
class ScriptObject;

inline constexpr const char* kModuleName = "scripting-js";
inline constexpr uint32_t kModuleFourcc = SQ_FOURCC('S', 'Q', 'J', 'S');

// Private marker so the engine recognises its own script objects when they come back from native
// code (they are then unwrapped to the underlying JS value instead of being double-wrapped).
struct IScriptObjectPrivate : sq::IUnknown {
  SQ_INTERFACE(IScriptObjectPrivate, sq::IUnknown, "{D1DD4637-B1EB-4F9B-B15C-DC7C4DB608E8}")
  virtual void* SQ_CALL engineTag() = 0;
};

// Per-JSContext data (JS_GetContextOpaque).
struct ContextData {
  Engine* engine = nullptr;
  Package* package = nullptr;   // nullptr for the global evaluate() context
  JSValue wrapFn = JS_UNDEFINED;  // bootstrap's wrap(): handle -> typed wrapper
};

struct Limits {
  uint64_t timeMs = 10000;
  uint64_t memoryBytes = 256ull << 20;
  uint64_t stackBytes = 512ull << 10;
};

// ---- packages ---------------------------------------------------------------------------
struct CommandDef {
  std::string category, name, title;
};

class ScriptClassFactory;

struct ClassDef {
  sq_guid id{};
  std::string category, name, sourceFile, functionName;
  std::vector<CommandDef> commands;
  sq::Ptr<ScriptClassFactory> factory;
  bool registered = false;
};

// Package contents: a directory, or a ZIP archive (.zip / .package) read into memory at load.
class PackageSource {
 public:
  virtual ~PackageSource() = default;
  virtual bool read(const std::string& relPath, std::string& out) const = 0;
  virtual std::string describe() const = 0;
  static std::unique_ptr<PackageSource> open(const std::string& path, std::string* err);
  // Rejects absolute paths, backslashes and ".." segments.
  static bool safeRelative(const std::string& p);
};

class Package final : public sq::Object<Package, sq::IScriptPackage> {
 public:
  Package(Engine* engine, std::unique_ptr<PackageSource> src);
  ~Package() override;

  // Parses package.xml. Returns SQ_OK or an error with *err set.
  sq_result parseManifest(uint32_t hostAbi, std::string* err);

  // ---- sq::IScriptPackage ----
  sq_result SQ_CALL getId(char* buf, uint32_t bufSize, uint32_t* needed) override;
  uint32_t SQ_CALL getClassCount() override;
  sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) override;
  sq_result SQ_CALL unload() override;

  const std::string& id() const { return id_; }
  const std::string& displayName() const { return name_; }
  const std::string& version() const { return version_; }
  std::vector<ClassDef>& classes() { return classes_; }
  bool readFile(const std::string& rel, std::string& out) const { return src_ && PackageSource::safeRelative(rel) && src_->read(rel, out); }
  std::string origin() const { return src_ ? src_->describe() : std::string(); }

  // JS state (main thread only).
  JSContext* ctx = nullptr;
  std::set<std::string> evaluated;  // source files already run in ctx
  std::atomic<int> liveObjects{0};  // ScriptObjects created from this package
  Engine* engine = nullptr;         // null once unloaded

 private:
  std::unique_ptr<PackageSource> src_;
  std::string id_, name_, version_;
  std::vector<ClassDef> classes_;
};

// IClassFactory registered for one <ScriptClass> (dynamic class in the app registry).
class ScriptClassFactory final : public sq::Object<ScriptClassFactory, sq::IClassFactory> {
 public:
  ScriptClassFactory(Package* pkg, size_t index) : pkg_(pkg), index_(index) {}
  void detach() { pkg_ = nullptr; }

  sq_result SQ_CALL getModuleInfo(sq_module_info* out) override;
  uint32_t SQ_CALL getClassCount() override { return pkg_ ? 1u : 0u; }
  sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) override;
  sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) override;
  sq_bool SQ_CALL canUnload() override { return (!pkg_ || pkg_->liveObjects == 0) ? SQ_TRUE : SQ_FALSE; }

 private:
  Package* pkg_;
  size_t index_;
};

// ---- script objects -----------------------------------------------------------------------
// A JS object exposed to native code. QI answers IUnknown, IController (always: property access)
// and the interfaces the object declares in `interfaces: [Host.Interfaces.X, ...]`, or, when it
// declares none, those whose methods it has (interpretCommand, onNotify, initialize/terminate,
// perform). Calls are main-thread only.
class ScriptObject final : public sq::Object<ScriptObject, sq::ICommandHandler, sq::IObserver, sq::IController, sq::IComponent,
                                             sq::IAction, IScriptObjectPrivate> {
 public:
  ScriptObject(Engine* engine, Package* pkg, JSContext* ctx, JSValueConst value);
  ~ScriptObject() override;

  sq_result SQ_CALL queryInterface(const sq_guid* riid, void** out) override;
  // Own refcount: the final 1 -> 0 step happens under the engine's live-object lock, so the
  // engine never hands out (findLive) an object another thread is destroying, and the JS value of
  // an object released on a foreign thread is queued atomically with its removal from the list.
  uint32_t SQ_CALL addRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
  uint32_t SQ_CALL release() override;

  sq_result SQ_CALL interpretCommand(const sq_command* command, sq::IAttributes* args, sq_bool checkOnly) override;
  void SQ_CALL onNotify(sq::IUnknown* subject, const char* topic, sq::IAttributes* args) override;
  sq_result SQ_CALL getProperty(const char* name, sq::IAttributes* out) override;
  sq_result SQ_CALL setProperty(const char* name, sq::IAttributes* in) override;
  sq_result SQ_CALL getChild(const char* name, const sq_guid* riid, void** out) override;
  sq_result SQ_CALL initialize(sq::IUnknown* context) override;
  sq_result SQ_CALL terminate() override;
  sq_result SQ_CALL perform(sq::IDocument* doc, sq::IEditor* editor, sq::IAttributes* params, sq::IAttributes* result) override;
  void* SQ_CALL engineTag() override { return engine_.get(); }

  // Engine shutdown: drop the JS value; every later call fails with SQ_E_CLOSED.
  void detach();
  JSValueConst value() const { return value_; }
  JSContext* context() const { return ctx_; }
  void* jsPointer() const { return JS_VALUE_GET_PTR(value_); }

 private:
  enum : uint32_t { kCmd = 1, kObs = 2, kComp = 4, kAct = 8 };
  bool supports(const sq_guid& riid) const;
  // Calls method `name` (if present) with argv. *missing = true if there is no such method.
  sq_result call(const char* name, int argc, JSValue* argv, JSValue* ret, bool* missing);

  sq::Ptr<Engine> engine_;
  sq::Ptr<Package> pkg_;
  JSContext* ctx_ = nullptr;
  JSValue value_ = JS_UNDEFINED;
  uint32_t ifaces_ = 0;
  std::atomic<uint32_t> refs_{1};
};

// ---- object registry ---------------------------------------------------------------------
class ObjectRegistry final : public sq::Object<ObjectRegistry, sq::IObjectRegistry> {
 public:
  sq_result SQ_CALL registerResolver(const char* prefix, sq::IUrlResolver* resolver) override;
  sq_result SQ_CALL unregisterResolver(const char* prefix, sq::IUrlResolver* resolver) override;
  sq_result SQ_CALL registerObject(const char* url, sq::IUnknown* object) override;
  sq_result SQ_CALL unregisterObject(const char* url) override;
  sq_result SQ_CALL resolve(const char* url, const sq_guid* riid, void** out) override;
  sq_result SQ_CALL openStream(const char* url, sq::IByteStream** out) override;
  void clear();

 private:
  // Longest registered prefix of url (with the object://app/settings/ -> object://settings/
  // fallback applied when nothing else matches). Returns resolver + the url to pass on.
  sq::Ptr<sq::IUrlResolver> find(const std::string& url, std::string* effectiveUrl);

  std::mutex m_;
  std::map<std::string, sq::Ptr<sq::IUrlResolver>> resolvers_;
  std::map<std::string, sq::Ptr<sq::IUnknown>> objects_;
};

// object://settings/<dotted.key> (also reachable as object://app/settings/<key> unless the app
// registers that prefix itself). Each URL resolves to an IController over the key.
class SettingsResolver final : public sq::Object<SettingsResolver, sq::IUrlResolver> {
 public:
  explicit SettingsResolver(sq::ISettings* settings) : settings_(settings) {}
  sq_result SQ_CALL resolve(const char* url, const sq_guid* riid, void** out) override;
  sq_result SQ_CALL openStream(const char*, sq::IByteStream**) override { return SQ_E_NOTIMPL; }

 private:
  sq::ISettings* settings_;  // borrowed (host lifetime)
};

// package://<id>/<path>: bytes of a file inside a loaded package; package://<id> -> IScriptPackage.
class PackageResolver final : public sq::Object<PackageResolver, sq::IUrlResolver> {
 public:
  explicit PackageResolver(Engine* engine) : engine_(engine) {}
  void detach() {
    std::lock_guard<std::mutex> lk(m_);
    engine_ = nullptr;
  }
  sq_result SQ_CALL resolve(const char* url, const sq_guid* riid, void** out) override;
  sq_result SQ_CALL openStream(const char* url, sq::IByteStream** out) override;

 private:
  std::mutex m_;
  Engine* engine_;
};

inline constexpr const char* kSettingsPrefix = "object://settings/";
inline constexpr const char* kAppSettingsPrefix = "object://app/settings/";
inline constexpr const char* kPackagePrefix = "package://";
inline constexpr const char* kEngineUrl = "object://scripting";

// ---- engine -------------------------------------------------------------------------------
class Engine final : public sq::Object<Engine, sq::IScriptEngine, sq::IController> {
 public:
  // host may be null (then settings/registry/services are unavailable: unit use only).
  Engine(sq::IHostContext* host, sq::IObjectRegistry* objects);
  ~Engine() override;

  sq_result start();
  void stop();

  // ---- sq::IScriptEngine ----
  sq_result SQ_CALL getEngineInfo(char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL loadPackage(const char* pathOrUrl, sq::IScriptPackage** out) override;
  sq_result SQ_CALL evaluate(const char* source, const char* sourceName, sq::IAttributes* result) override;
  sq_result SQ_CALL getLastError(char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL callFactory(const char* packageId, const char* sourceFile, const char* functionName, const sq_guid* riid,
                                void** out) override;
  sq_result SQ_CALL idle() override;

  // ---- sq::IController (object://scripting) ----
  sq_result SQ_CALL getProperty(const char* name, sq::IAttributes* out) override;
  sq_result SQ_CALL setProperty(const char* name, sq::IAttributes* in) override;
  sq_result SQ_CALL getChild(const char* name, const sq_guid* riid, void** out) override;

  // ---- internal API ----
  sq::IHostContext* hostContext() const { return host_; }
  sq::IObjectRegistry* objects() const { return objects_; }
  JSRuntime* runtime() const { return rt_; }
  JSClassID nativeClassId() const { return nativeClass_; }
  bool running() const { return rt_ != nullptr && !stopping_; }
  bool onOwnerThread() const;
  const Limits& limits() const { return limits_; }
  std::string engineInfo() const;

  // Call bracket for every entry into JS (CallScope below).
  sq_result enter();
  void leave();
  int depth() const { return depth_; }
  bool timedOut() const { return timedOut_; }
  // Interrupt handler: true when the current outermost call ran past its deadline.
  bool checkInterrupt();

  // Takes the pending exception of ctx, records it as last error, returns the sq_result.
  sq_result captureException(JSContext* ctx);
  void setLastError(const std::string& e);
  std::string takeNestedError();

  // Script objects.
  void addLive(ScriptObject* o);
  // The *Locked variants require liveMutex() to be held by the caller.
  std::mutex& liveMutex() { return liveMutex_; }
  void removeLiveLocked(ScriptObject* o);
  void deferFreeLocked(JSValue v);  // from foreign threads; freed on the next entry/idle
  // Live wrapper of a JS object (add-ref'd), or empty.
  sq::Ptr<ScriptObject> findLive(void* jsPtr);

  // Wraps a JS value as a ScriptObject (reusing a live one for the same JS object) and QIs riid.
  sq_result wrapScriptValue(JSContext* ctx, JSValueConst v, Package* pkg, const sq_guid* riid, void** out);
  sq_result createFromClass(Package* pkg, size_t index, const sq_guid* riid, void** out);

  // Packages.
  sq::Ptr<Package> findPackage(const std::string& id);
  sq_result unloadPackage(Package* pkg, bool force);
  std::vector<sq::Ptr<Package>> packages();
  // Runs (once) a package source file in the package context.
  sq_result ensureSource(Package* pkg, const std::string& file);
  sq_result ensurePackageContext(Package* pkg);

  // Transactions opened by scripts (cancelled if a script call ends abnormally).
  void txBegin(sq::IEditor* ed);
  void txEnd(sq::IEditor* ed);

  // Timers and settings observers (JS callbacks).
  int32_t addTimer(JSContext* ctx, JSValueConst fn, double ms, bool repeat);
  void cancelTimer(int32_t id);
  int32_t addSettingsObserver(JSContext* ctx, const std::string& prefix, JSValueConst fn);
  void removeSettingsObserver(int32_t id);

  // Converts a sq::IUnknown into a JS value for ctx: own script objects unwrap to their JS value,
  // everything else becomes a native handle. wrapped=true passes it through the bootstrap wrap().
  JSValue box(JSContext* ctx, sq::IUnknown* obj, bool wrapped);

  void loadBundledPackages();

 private:
  friend class CallScope;
  JSContext* newContext(Package* pkg);
  void freeContext(JSContext* ctx);
  void refreshLimits();
  void cancelDanglingTransactions(size_t mark);
  void flushDeferred();
  void runTimers();
  void deliverSettingsChanges();
  void runJobs();

  sq::IHostContext* host_;
  sq::IObjectRegistry* objects_;  // borrowed: owned by the module state
  uint64_t ownerThread_ = 0;
  JSRuntime* rt_ = nullptr;
  JSContext* global_ = nullptr;
  JSClassID nativeClass_ = 0;
  bool stopping_ = false;
  Limits limits_;

  int depth_ = 0;
  uint64_t deadline_ = 0;
  bool timedOut_ = false;

  std::string lastError_;
  std::string nestedError_;

  std::recursive_mutex pkgMutex_;
  std::map<std::string, sq::Ptr<Package>> packages_;

  std::mutex liveMutex_;
  std::vector<ScriptObject*> live_;
  std::vector<JSValue> deferred_;

  std::vector<sq::Ptr<sq::IEditor>> openTx_;

  struct Timer {
    int32_t id;
    uint64_t due;
    uint64_t intervalNs;
    bool repeat;
    JSContext* ctx;
    JSValue fn;
  };
  std::vector<Timer> timers_;
  int32_t nextTimer_ = 1;

  struct SettingsWatch {
    int32_t id;
    std::string prefix;
    JSContext* ctx;
    JSValue fn;
    uint32_t cookie;
    sq::Ptr<sq::ISettingsObserver> observer;
  };
  std::vector<SettingsWatch> watches_;
  int32_t nextWatch_ = 1;
  std::mutex changeMutex_;
  std::vector<std::pair<int32_t, std::string>> pendingChanges_;
  sq_result invokeFactory(Package* pkg, const std::string& sourceFile, const std::string& functionName, const ClassDef* cls,
                          const sq_guid* riid, void** out);
  void dispatchSettingChange(int32_t watchId, const std::string& key);
  std::vector<size_t> txMarks_;
  bool capturing_ = false;

 public:
  // Settings observer callback (any thread): main thread dispatches now, others queue for idle().
  void onSettingChangedFromBridge(int32_t watchId, const char* key);
};

// RAII bracket around every native -> JS entry.
class CallScope {
 public:
  explicit CallScope(Engine& e) : e_(e), status_(e.enter()) {}
  ~CallScope() {
    if (status_ == SQ_OK) e_.leave();
  }
  CallScope(const CallScope&) = delete;
  CallScope& operator=(const CallScope&) = delete;
  sq_result status() const { return status_; }
  bool ok() const { return status_ == SQ_OK; }

 private:
  Engine& e_;
  sq_result status_;
};

// ---- bridge (bridge.cpp) -------------------------------------------------------------------
// Installs the native binding object (`N` in bootstrap.js) for ctx.
JSValue makeNativeBinding(JSContext* ctx);
// sq::IAttributes <-> JS conversions.
void jsToAttrs(JSContext* ctx, JSValueConst obj, sq::IAttributes* a, int depth = 0);
bool jsToAttrValue(JSContext* ctx, sq::IAttributes* a, const char* key, JSValueConst v, int depth = 0);
JSValue attrsToJs(JSContext* ctx, sq::IAttributes* a, int depth = 0);
JSValue attrValueToJs(JSContext* ctx, sq::IAttributes* a, const char* key, int depth = 0);
// JS value -> add-ref'd sq::IUnknown (native handle, or a script object wrapper). nullptr if not an object.
sq::Ptr<sq::IUnknown> jsToUnknown(JSContext* ctx, JSValueConst v);
// Native handle -> borrowed IUnknown (nullptr if v is not a handle).
sq::IUnknown* unboxHandle(JSContext* ctx, JSValueConst v);
// Calls ctx's wrap() on v (consumes v).
JSValue wrapForScript(JSContext* ctx, JSValue v);
// Converts a script return value into an sq_result.
sq_result resultFromJs(JSContext* ctx, JSValueConst v, sq_result disabledCode);

inline ContextData* contextData(JSContext* ctx) { return static_cast<ContextData*>(JS_GetContextOpaque(ctx)); }

}  // namespace sqjs
