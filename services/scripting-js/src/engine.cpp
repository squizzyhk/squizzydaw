// scripting-js: QuickJS-ng runtime, contexts, limits, error reporting, packages, timers.
#include "engine.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <regex>

#include "bootstrap_js.h"
#include "sq/cpp/attributes.hpp"
#include "sq/platform.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace sqjs {

namespace {

constexpr const char* kKeyTimeLimit = "Services.scripting-js.TimeLimitMs";
constexpr const char* kKeyMemoryLimit = "Services.scripting-js.MemoryLimitMB";
constexpr const char* kKeyStackSize = "Services.scripting-js.StackSizeKB";
constexpr const char* kKeyPackagePath = "Services.scripting-js.PackagePath";
constexpr const char* kKeyLoadUser = "Services.scripting-js.LoadUserPackages";
constexpr int kMaxDepth = 32;
constexpr const char* kBootstrapName = "host:bootstrap.js";

sq_result copyOut(const std::string& s, char* buf, uint32_t bufSize, uint32_t* needed) {
  const uint32_t need = static_cast<uint32_t>(s.size() + 1);
  if (needed) *needed = need;
  if (!buf || bufSize < need) {
    if (buf && bufSize) {
      std::memcpy(buf, s.data(), bufSize - 1);
      buf[bufSize - 1] = '\0';
    }
    return SQ_E_BUFFER_TOO_SMALL;
  }
  std::memcpy(buf, s.c_str(), need);
  return SQ_OK;
}

int interruptHandler(JSRuntime* rt, void*) {
  auto* e = static_cast<Engine*>(JS_GetRuntimeOpaque(rt));
  return (e && e->checkInterrupt()) ? 1 : 0;
}

void nativeFinalizer(JSRuntime* rt, JSValueConst val) {
  auto* e = static_cast<Engine*>(JS_GetRuntimeOpaque(rt));
  if (!e) return;
  if (auto* p = static_cast<sq::IUnknown*>(JS_GetOpaque(val, e->nativeClassId()))) p->release();
}

std::string propString(JSContext* ctx, JSValueConst obj, const char* name) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(v)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return {};
  }
  std::string s = (JS_IsUndefined(v) || JS_IsNull(v)) ? std::string() : toStdString(ctx, v);
  JS_FreeValue(ctx, v);
  return s;
}

// First "file:line" in a stack trace, skipping host bootstrap frames.
std::string findLocation(const std::string& stack) {
  static const std::regex re(R"(([^\s()]+?):(\d+)(?::\d+)?(?=[)\s]|$))");
  std::string first;
  for (auto it = std::sregex_iterator(stack.begin(), stack.end(), re); it != std::sregex_iterator(); ++it) {
    std::string loc = (*it)[1].str() + ":" + (*it)[2].str();
    if (first.empty()) first = loc;
    if (loc.rfind("host:", 0) != 0) return loc;
  }
  return first;
}

// Bytes of native stack left below the caller on the current thread (0 = unknown).
size_t stackHeadroom() {
  volatile char probe = 0;
  const uintptr_t sp = reinterpret_cast<uintptr_t>(&probe);
#if defined(_WIN32)
  ULONG_PTR lo = 0, hi = 0;
  GetCurrentThreadStackLimits(&lo, &hi);
  return (lo && sp > lo) ? static_cast<size_t>(sp - lo) : 0;
#elif defined(__APPLE__)
  pthread_t self = pthread_self();
  const uintptr_t top = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
  const uintptr_t lo = top - pthread_get_stacksize_np(self);
  return sp > lo ? static_cast<size_t>(sp - lo) : 0;
#elif defined(__linux__)
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
  void* addr = nullptr;
  size_t size = 0;
  pthread_attr_getstack(&attr, &addr, &size);
  pthread_attr_destroy(&attr);
  const uintptr_t lo = reinterpret_cast<uintptr_t>(addr);
  return (lo && sp > lo) ? static_cast<size_t>(sp - lo) : 0;
#else
  return 0;
#endif
}

// QuickJS stack budget: the requested size (0 or negative = default), never more than what the
// thread really has left minus a reserve for native frames (host calls made from scripts, the
// guard page). A budget of 0 would switch QuickJS' check off and let deep recursion overflow the
// native stack, i.e. crash the host.
size_t effectiveStackBytes(uint64_t requested) {
  constexpr size_t kDefault = 512u << 10, kMin = 16u << 10;
  size_t want = requested ? static_cast<size_t>(std::min<uint64_t>(requested, uint64_t(1) << 30)) : kDefault;
  const size_t room = stackHeadroom();
  if (room) {
    const size_t reserve = std::max<size_t>(room / 4, 96u << 10);
    const size_t cap = room > reserve + kMin ? room - reserve : kMin;
    want = std::min(want, cap);
  }
  return std::max(want, kMin);
}

bool validIdentifier(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (i > 0 && c >= '0' && c <= '9');
    if (!ok) return false;
  }
  return true;
}

class SettingsBridge final : public sq::Object<SettingsBridge, sq::ISettingsObserver> {
 public:
  SettingsBridge(Engine* e, int32_t id) : e_(e), id_(id) {}
  void detach() { e_.store(nullptr); }
  void SQ_CALL onSettingChanged(const char* key) override {
    if (Engine* e = e_.load()) e->onSettingChangedFromBridge(id_, key);
  }

 private:
  std::atomic<Engine*> e_;
  int32_t id_;
};

}  // namespace

// ============================================================================================
Engine::Engine(sq::IHostContext* host, sq::IObjectRegistry* objects) : host_(host), objects_(objects) {
  ownerThread_ = sq_thread_current_id();
}

Engine::~Engine() { stop(); }

bool Engine::onOwnerThread() const { return sq_thread_current_id() == ownerThread_; }

std::string Engine::engineInfo() const { return std::string("QuickJS-ng ") + JS_GetVersion() + " (Squizzy Studio scripting-js)"; }

bool Engine::checkInterrupt() {
  if (depth_ <= 0 || deadline_ == 0 || capturing_) return false;
  if (sq_time_monotonic_ns() <= deadline_) return false;
  timedOut_ = true;
  return true;
}

void Engine::refreshLimits() {
  if (host_) {
    sq::ISettings* s = host_->getSettings();
    int64_t t = s->getInt(kKeyTimeLimit, 10000);
    int64_t m = s->getInt(kKeyMemoryLimit, 256);
    int64_t k = s->getInt(kKeyStackSize, 512);
    // Clamped so the unit conversions below cannot overflow (0 / negative = no limit for time and
    // memory; the stack always keeps a limit, see effectiveStackBytes).
    limits_.timeMs = t > 0 ? static_cast<uint64_t>(std::min<int64_t>(t, int64_t(1) << 40)) : 0;
    limits_.memoryBytes = m > 0 ? static_cast<uint64_t>(std::min<int64_t>(m, int64_t(1) << 30)) << 20 : 0;
    limits_.stackBytes = k > 0 ? static_cast<uint64_t>(std::min<int64_t>(k, int64_t(1) << 30)) << 10 : 0;
  }
  if (rt_) {
    JS_SetMemoryLimit(rt_, limits_.memoryBytes ? static_cast<size_t>(limits_.memoryBytes) : static_cast<size_t>(-1));
    JS_SetMaxStackSize(rt_, effectiveStackBytes(limits_.stackBytes));
  }
}

sq_result Engine::start() {
  if (rt_) return SQ_S_FALSE;
  stopping_ = false;
  rt_ = JS_NewRuntime();
  if (!rt_) return SQ_E_OUTOFMEMORY;
  JS_SetRuntimeOpaque(rt_, this);
  JS_SetRuntimeInfo(rt_, "squizzy-scripting");
  refreshLimits();
  JS_SetInterruptHandler(rt_, &interruptHandler, this);
  nativeClass_ = 0;
  JS_NewClassID(rt_, &nativeClass_);
  JSClassDef def{};
  def.class_name = "SqNative";
  def.finalizer = &nativeFinalizer;
  if (JS_NewClass(rt_, nativeClass_, &def) != 0) {
    JS_FreeRuntime(rt_);
    rt_ = nullptr;
    return SQ_E_FAIL;
  }
  {
    CallScope scope(*this);
    if (!scope.ok()) return scope.status();
    global_ = newContext(nullptr);
  }
  if (!global_) {
    sq::logf(SQ_LOG_ERROR, "bootstrap failed: %s", lastError_.c_str());
    stop();
    return SQ_E_FAIL;
  }
  return SQ_OK;
}

void Engine::stop() {
  if (!rt_) return;
  stopping_ = true;
  // 1. script objects still held by native code become inert. They are add-ref'd under the lock
  // (an object in the list is never mid-destruction, see ScriptObject::release) so a concurrent
  // release on another thread cannot free one while it is being detached.
  std::vector<sq::Ptr<ScriptObject>> live;
  {
    std::lock_guard<std::mutex> lk(liveMutex_);
    for (ScriptObject* o : live_) live.emplace_back(o);
    live_.clear();
  }
  for (auto& o : live) o->detach();
  live.clear();
  // 2. transactions a script left open.
  cancelDanglingTransactions(0);
  // 3. timers + settings observers.
  for (Timer& t : timers_) JS_FreeValueRT(rt_, t.fn);
  timers_.clear();
  for (SettingsWatch& w : watches_) {
    if (host_) host_->getSettings()->removeObserver(w.cookie);
    static_cast<SettingsBridge*>(w.observer.get())->detach();
    JS_FreeValueRT(rt_, w.fn);
  }
  watches_.clear();
  {
    std::lock_guard<std::mutex> lk(changeMutex_);
    pendingChanges_.clear();
  }
  flushDeferred();
  // 4. packages (unregisters their classes).
  for (auto& p : packages()) unloadPackage(p.get(), true);
  // 5. global context + runtime.
  if (global_) freeContext(global_);
  global_ = nullptr;
  JS_RunGC(rt_);
  flushDeferred();
  JS_FreeRuntime(rt_);
  rt_ = nullptr;
  depth_ = 0;
}

JSContext* Engine::newContext(Package* pkg) {
  JSContext* ctx = JS_NewContext(rt_);
  if (!ctx) {
    setLastError("cannot create a script context (out of memory?)");
    return nullptr;
  }
  auto* cd = new ContextData{this, pkg, JS_UNDEFINED};
  JS_SetContextOpaque(ctx, cd);
  JS_SetClassProto(ctx, nativeClass_, JS_NewObject(ctx));

  JSValue fn = JS_Eval(ctx, reinterpret_cast<const char*>(kBootstrapJs), sizeof(kBootstrapJs) - 1, kBootstrapName, JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(fn)) {
    captureException(ctx);
    freeContext(ctx);
    return nullptr;
  }
  JSValue binding = makeNativeBinding(ctx);
  JSValue info = JS_NULL;
  if (pkg) {
    info = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, info, "id", newString(ctx, pkg->id()));
    JS_SetPropertyStr(ctx, info, "name", newString(ctx, pkg->displayName()));
    JS_SetPropertyStr(ctx, info, "version", newString(ctx, pkg->version()));
  }
  JSValue argv[3] = {JS_GetGlobalObject(ctx), binding, info};
  JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 3, argv);
  for (JSValue& v : argv) JS_FreeValue(ctx, v);
  JS_FreeValue(ctx, fn);
  if (JS_IsException(r)) {
    captureException(ctx);
    freeContext(ctx);
    return nullptr;
  }
  cd->wrapFn = JS_GetPropertyStr(ctx, r, "wrap");
  JS_FreeValue(ctx, r);
  return ctx;
}

void Engine::freeContext(JSContext* ctx) {
  if (!ctx) return;
  // Callbacks bound to this context go with it.
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (it->ctx == ctx) {
      JS_FreeValueRT(rt_, it->fn);
      it = timers_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = watches_.begin(); it != watches_.end();) {
    if (it->ctx == ctx) {
      if (host_) host_->getSettings()->removeObserver(it->cookie);
      static_cast<SettingsBridge*>(it->observer.get())->detach();
      JS_FreeValueRT(rt_, it->fn);
      it = watches_.erase(it);
    } else {
      ++it;
    }
  }
  if (auto* cd = contextData(ctx)) {
    JS_FreeValue(ctx, cd->wrapFn);
    delete cd;
  }
  JS_SetContextOpaque(ctx, nullptr);
  JS_FreeContext(ctx);
}

// ---- call bracket ------------------------------------------------------------------------
sq_result Engine::enter() {
  if (!rt_ || stopping_) return SQ_E_CLOSED;
  if (!onOwnerThread()) return SQ_E_ACCESS;
  if (depth_ >= kMaxDepth) {
    setLastError("script call nesting too deep (native <-> script recursion?)");
    return SQ_E_BUSY;
  }
  if (depth_ == 0) {
    flushDeferred();
    refreshLimits();
    timedOut_ = false;
    deadline_ = limits_.timeMs ? sq_time_monotonic_ns() + limits_.timeMs * 1000000ull : 0;
    JS_UpdateStackTop(rt_);
  }
  txMarks_.push_back(openTx_.size());
  ++depth_;
  return SQ_OK;
}

void Engine::leave() {
  size_t mark = txMarks_.empty() ? 0 : txMarks_.back();
  if (!txMarks_.empty()) txMarks_.pop_back();
  cancelDanglingTransactions(mark);
  if (--depth_ == 0) deadline_ = 0;
}

void Engine::txBegin(sq::IEditor* ed) { openTx_.emplace_back(ed); }

void Engine::txEnd(sq::IEditor* ed) {
  for (size_t i = openTx_.size(); i-- > 0;) {
    if (openTx_[i].get() == ed) {
      openTx_.erase(openTx_.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
}

void Engine::cancelDanglingTransactions(size_t mark) {
  while (openTx_.size() > mark) {
    sq::Ptr<sq::IEditor> ed = std::move(openTx_.back());
    openTx_.pop_back();
    if (ed) {
      sq::logf(SQ_LOG_WARN, "script left a transaction open: cancelled");
      ed->cancelTransaction();
    }
  }
}

// ---- errors ----------------------------------------------------------------------------------
sq_result Engine::captureException(JSContext* ctx) {
  capturing_ = true;
  JSValue ex = JS_GetException(ctx);
  std::string name, message, stack;
  if (JS_IsObject(ex)) {
    name = propString(ctx, ex, "name");
    message = propString(ctx, ex, "message");
    stack = propString(ctx, ex, "stack");
  } else {
    message = toStdString(ctx, ex);
  }
  JS_FreeValue(ctx, ex);
  sq_result code = SQ_E_FAIL;
  if (timedOut_) {
    name = "TimeoutError";
    message = "script time limit exceeded (" + std::to_string(limits_.timeMs) + " ms)";
  } else if (name == "SyntaxError") {
    code = SQ_E_PARSE;
  }
  std::string text;
  std::string loc = findLocation(stack);
  if (!loc.empty()) text = loc + ": ";
  text += (name.empty() ? std::string("Error") : name) + ": " + message;
  if (!stack.empty()) {
    text += "\n";
    text += stack;
  }
  if (message.find("out of memory") != std::string::npos) JS_RunGC(rt_);
  if (depth_ > 1) nestedError_ = text;
  setLastError(text);
  capturing_ = false;
  return code;
}

void Engine::setLastError(const std::string& e) {
  lastError_ = e;
  sq::logf(SQ_LOG_WARN, "%s", e.c_str());
}

std::string Engine::takeNestedError() {
  std::string s;
  s.swap(nestedError_);
  return s;
}

// ---- live script objects ---------------------------------------------------------------------
void Engine::addLive(ScriptObject* o) {
  std::lock_guard<std::mutex> lk(liveMutex_);
  live_.push_back(o);
}

void Engine::removeLiveLocked(ScriptObject* o) {
  auto it = std::find(live_.begin(), live_.end(), o);
  if (it != live_.end()) live_.erase(it);
}

sq::Ptr<ScriptObject> Engine::findLive(void* jsPtr) {
  std::lock_guard<std::mutex> lk(liveMutex_);
  for (ScriptObject* o : live_)
    if (o->jsPointer() == jsPtr) return sq::Ptr<ScriptObject>(o);  // refs >= 1 while listed
  return {};
}

void Engine::deferFreeLocked(JSValue v) { deferred_.push_back(v); }

void Engine::flushDeferred() {
  for (;;) {
    std::vector<JSValue> d;
    {
      std::lock_guard<std::mutex> lk(liveMutex_);
      d.swap(deferred_);
    }
    if (d.empty() || !rt_) return;
    for (JSValue& v : d) JS_FreeValueRT(rt_, v);
  }
}

sq_result Engine::wrapScriptValue(JSContext* ctx, JSValueConst v, Package* pkg, const sq_guid* riid, void** out) {
  if (!out || !riid) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!JS_IsObject(v)) return SQ_E_TYPE;
  if (sq::IUnknown* n = unboxHandle(ctx, v)) return n->queryInterface(riid, out);
  if (sq::Ptr<ScriptObject> so = findLive(JS_VALUE_GET_PTR(v))) return so->queryInterface(riid, out);
  if (!pkg) {
    if (ContextData* cd = contextData(ctx)) pkg = cd->package;
  }
  ScriptObject* so = nullptr;
  try {
    so = new ScriptObject(this, pkg, ctx, v);
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  sq_result r = so->queryInterface(riid, out);
  so->release();
  return r;
}

JSValue Engine::box(JSContext* ctx, sq::IUnknown* obj, bool wrapped) {
  if (!obj) return JS_NULL;
  sq::Ptr<IScriptObjectPrivate> priv;
  if (obj->queryInterface(&IScriptObjectPrivate::iid, priv.putVoid()) == SQ_OK && priv && priv->engineTag() == this) {
    auto* so = static_cast<ScriptObject*>(priv.get());
    if (so->context()) return JS_DupValue(ctx, so->value());
  }
  sq::Ptr<sq::IUnknown> unk;
  if (obj->queryInterface(&sq::IUnknown::iid, unk.putVoid()) != SQ_OK || !unk) return JS_NULL;
  JSValue h = JS_NewObjectClass(ctx, static_cast<int>(nativeClass_));
  if (JS_IsException(h)) return h;
  JS_SetOpaque(h, unk.detach());
  return wrapped ? wrapForScript(ctx, h) : h;
}

// ---- IScriptEngine ---------------------------------------------------------------------------
sq_result SQ_CALL Engine::getEngineInfo(char* buf, uint32_t bufSize, uint32_t* needed) {
  return copyOut(engineInfo(), buf, bufSize, needed);
}

sq_result SQ_CALL Engine::getLastError(char* buf, uint32_t bufSize, uint32_t* needed) {
  if (!onOwnerThread()) return SQ_E_ACCESS;  // lastError_ is written by the engine thread
  return copyOut(lastError_, buf, bufSize, needed);
}

sq_result SQ_CALL Engine::evaluate(const char* source, const char* sourceName, sq::IAttributes* result) {
  if (!source) return SQ_E_INVALIDARG;
  try {
    CallScope scope(*this);
    if (!scope.ok()) return scope.status();
    if (result) result->remove("value");
    const std::string name = (sourceName && *sourceName) ? sourceName : "<eval>";
    JSValue v = JS_Eval(global_, source, std::strlen(source), name.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) return captureException(global_);
    if (result && !JS_IsUndefined(v) && !JS_IsNull(v)) {
      if (JS_IsNumber(v)) {
        double d = 0;
        JS_ToFloat64(global_, &d, v);
        result->setDouble("value", d);
      } else if (JS_IsBool(v)) {
        result->setBool("value", JS_ToBool(global_, v) ? SQ_TRUE : SQ_FALSE);
      } else if (JS_IsObject(v)) {
        sq::Ptr<sq::IUnknown> obj;
        if (sq::IUnknown* n = unboxHandle(global_, v)) {
          obj = sq::Ptr<sq::IUnknown>(n);
        } else {
          sq::Ptr<sq::IController> ctl;
          if (wrapScriptValue(global_, v, nullptr, &sq::IController::iid, ctl.putVoid()) == SQ_OK) obj = ctl.as<sq::IUnknown>();
        }
        if (obj) result->setObject("value", obj.get());
      } else {
        result->setString("value", toStdString(global_, v).c_str());
      }
    }
    JS_FreeValue(global_, v);
    runJobs();
    return SQ_OK;
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

void Engine::runJobs() {
  for (int i = 0; i < 10000; ++i) {
    JSContext* jctx = nullptr;
    int r = JS_ExecutePendingJob(rt_, &jctx);
    if (r == 0) return;
    if (r < 0 && jctx) captureException(jctx);
  }
}

sq_result SQ_CALL Engine::idle() {
  if (!rt_ || stopping_) return SQ_E_CLOSED;
  if (!onOwnerThread()) return SQ_E_ACCESS;
  if (depth_ > 0) return SQ_S_FALSE;  // re-entered from a script callback: nothing to do
  try {
    flushDeferred();
    deliverSettingsChanges();
    runTimers();
    CallScope scope(*this);
    if (!scope.ok()) return scope.status();
    runJobs();
    return SQ_OK;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL Engine::callFactory(const char* packageId, const char* sourceFile, const char* functionName, const sq_guid* riid,
                                      void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!packageId || !sourceFile || !functionName || !riid) return SQ_E_INVALIDARG;
  sq::Ptr<Package> pkg = findPackage(packageId);
  if (!pkg) {
    setLastError(std::string("no loaded script package '") + packageId + "'");
    return SQ_E_NOTFOUND;
  }
  return invokeFactory(pkg.get(), sourceFile, functionName, nullptr, riid, out);
}

sq_result Engine::createFromClass(Package* pkg, size_t index, const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!pkg || index >= pkg->classes().size()) return SQ_E_NOTFOUND;
  const ClassDef& c = pkg->classes()[index];
  return invokeFactory(pkg, c.sourceFile, c.functionName, &c, riid, out);
}

sq_result Engine::invokeFactory(Package* pkg, const std::string& sourceFile, const std::string& functionName, const ClassDef* cls,
                                const sq_guid* riid, void** out) {
  try {
    CallScope scope(*this);
    if (!scope.ok()) return scope.status();
    if (!validIdentifier(functionName)) {
      setLastError(pkg->id() + ": invalid factory function name '" + functionName + "'");
      return SQ_E_INVALIDARG;
    }
    sq_result r = ensurePackageContext(pkg);
    if (SQ_FAILED(r)) return r;
    r = ensureSource(pkg, sourceFile);
    if (SQ_FAILED(r)) return r;
    JSContext* ctx = pkg->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, functionName.c_str());
    JS_FreeValue(ctx, global);
    if (JS_IsException(fn)) return captureException(ctx);
    if (!JS_IsFunction(ctx, fn)) {
      JS_FreeValue(ctx, fn);
      setLastError(pkg->id() + "/" + sourceFile + ": factory function '" + functionName + "' not found");
      return SQ_E_NOTFOUND;
    }
    JSValue arg = JS_UNDEFINED;
    if (cls) {
      arg = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, arg, "classId", newString(ctx, guidToString(cls->id)));
      JS_SetPropertyStr(ctx, arg, "category", newString(ctx, cls->category));
      JS_SetPropertyStr(ctx, arg, "name", newString(ctx, cls->name));
      JS_SetPropertyStr(ctx, arg, "packageId", newString(ctx, pkg->id()));
    }
    JSValue obj = JS_Call(ctx, fn, JS_UNDEFINED, cls ? 1 : 0, &arg);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(obj)) return captureException(ctx);
    if (!JS_IsObject(obj)) {
      JS_FreeValue(ctx, obj);
      setLastError(pkg->id() + "/" + sourceFile + ": " + functionName + "() did not return an object");
      return SQ_E_TYPE;
    }
    r = wrapScriptValue(ctx, obj, pkg, riid, out);
    JS_FreeValue(ctx, obj);
    return r;
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result Engine::ensurePackageContext(Package* pkg) {
  if (pkg->ctx) return SQ_OK;
  if (pkg->engine != this) return SQ_E_CLOSED;
  pkg->ctx = newContext(pkg);
  return pkg->ctx ? SQ_OK : SQ_E_FAIL;
}

sq_result Engine::ensureSource(Package* pkg, const std::string& file) {
  if (pkg->evaluated.count(file)) return SQ_OK;
  if (!PackageSource::safeRelative(file)) {
    setLastError(pkg->id() + ": invalid source path '" + file + "'");
    return SQ_E_INVALIDARG;
  }
  std::string src;
  if (!pkg->readFile(file, src)) {
    setLastError(pkg->id() + ": source file not found: " + file);
    return SQ_E_NOTFOUND;
  }
  pkg->evaluated.insert(file);  // before running: include() cycles stop here
  const std::string name = std::string(kPackagePrefix) + pkg->id() + "/" + file;
  JSValue v = JS_Eval(pkg->ctx, src.c_str(), src.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(v)) {
    pkg->evaluated.erase(file);
    return captureException(pkg->ctx);
  }
  JS_FreeValue(pkg->ctx, v);
  return SQ_OK;
}

// ---- packages --------------------------------------------------------------------------------
sq_result SQ_CALL Engine::loadPackage(const char* pathOrUrl, sq::IScriptPackage** out) {
  if (out) *out = nullptr;
  if (!pathOrUrl || !*pathOrUrl) return SQ_E_INVALIDARG;
  if (!rt_ || stopping_) return SQ_E_CLOSED;
  if (!onOwnerThread()) return SQ_E_ACCESS;
  if (!host_) return SQ_E_UNSUPPORTED;
  try {
    std::string path = pathOrUrl;
    if (path.rfind("file:///", 0) == 0) {
      path = path.substr(8);
#ifndef _WIN32
      path = "/" + path;
#endif
    } else if (path.rfind("file://", 0) == 0) {
      path = path.substr(7);
    }
    std::string err;
    auto src = PackageSource::open(path, &err);
    if (!src) {
      setLastError("loadPackage(" + path + "): " + err);
      return sq_fs_exists(path.c_str()) ? SQ_E_UNSUPPORTED : SQ_E_NOTFOUND;
    }
    sq::Ptr<Package> pkg = sq::Ptr<Package>::adopt(new Package(this, std::move(src)));
    sq_result r = pkg->parseManifest(host_->getHostAbi(), &err);
    if (SQ_FAILED(r)) {
      setLastError("loadPackage(" + path + "): " + err);
      return r;
    }
    std::lock_guard<std::recursive_mutex> lk(pkgMutex_);
    if (packages_.count(pkg->id())) {
      setLastError("loadPackage(" + path + "): package '" + pkg->id() + "' is already loaded");
      return SQ_E_EXISTS;
    }
    sq::IClassRegistry* reg = host_->getClassRegistry();
    auto& classes = pkg->classes();
    for (size_t i = 0; i < classes.size(); ++i) {
      ClassDef& c = classes[i];
      c.factory = sq::Ptr<ScriptClassFactory>::adopt(new ScriptClassFactory(pkg.get(), i));
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      ci.class_id = c.id;
      ci.category = c.category.c_str();
      ci.name = c.name.c_str();
      r = reg->registerClass(&ci, c.factory.get());
      if (SQ_FAILED(r)) {
        for (size_t j = 0; j < i; ++j) {
          reg->unregisterClass(&classes[j].id);
          classes[j].registered = false;
        }
        for (auto& cc : classes)
          if (cc.factory) cc.factory->detach();
        setLastError("loadPackage(" + path + "): class " + guidToString(c.id) + " (" + c.name + ") cannot be registered: " +
                     sq_result_string(r));
        pkg->engine = nullptr;
        return r;
      }
      c.registered = true;
    }
    packages_[pkg->id()] = pkg;
    sq::logf(SQ_LOG_INFO, "script package '%s' %s loaded (%u classes) from %s", pkg->id().c_str(), pkg->version().c_str(),
             static_cast<unsigned>(classes.size()), pkg->origin().c_str());
    if (out) *out = static_cast<sq::IScriptPackage*>(pkg.detach());
    return SQ_OK;
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq::Ptr<Package> Engine::findPackage(const std::string& id) {
  std::lock_guard<std::recursive_mutex> lk(pkgMutex_);
  auto it = packages_.find(id);
  return it == packages_.end() ? sq::Ptr<Package>() : it->second;
}

std::vector<sq::Ptr<Package>> Engine::packages() {
  std::lock_guard<std::recursive_mutex> lk(pkgMutex_);
  std::vector<sq::Ptr<Package>> v;
  for (auto& [id, p] : packages_) v.push_back(p);
  return v;
}

sq_result Engine::unloadPackage(Package* pkg, bool force) {
  if (!pkg || pkg->engine != this) return SQ_S_FALSE;
  if (!stopping_ && !onOwnerThread()) return SQ_E_ACCESS;  // stop() tears down from any thread
  if (!force && depth_ > 0) {
    // Called from native code that a script invoked: the package's code may be on the stack.
    setLastError("package '" + pkg->id() + "' cannot be unloaded from inside a script call; retry from idle");
    return SQ_E_BUSY;
  }
  if (!force && pkg->liveObjects.load() > 0) {
    setLastError("package '" + pkg->id() + "' still has " + std::to_string(pkg->liveObjects.load()) + " live script objects");
    return SQ_E_BUSY;
  }
  sq::Ptr<Package> keep(pkg);
  if (host_) {
    sq::IClassRegistry* reg = host_->getClassRegistry();
    for (auto& c : pkg->classes()) {
      if (c.registered) reg->unregisterClass(&c.id);
      c.registered = false;
      if (c.factory) c.factory->detach();
    }
  }
  if (pkg->ctx) {
    freeContext(pkg->ctx);
    pkg->ctx = nullptr;
  }
  pkg->evaluated.clear();
  {
    std::lock_guard<std::recursive_mutex> lk(pkgMutex_);
    packages_.erase(pkg->id());
  }
  pkg->engine = nullptr;
  sq::logf(SQ_LOG_INFO, "script package '%s' unloaded", pkg->id().c_str());
  return SQ_OK;
}

void Engine::loadBundledPackages() {
  std::vector<std::string> roots;
  roots.push_back(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "scripts"));
  if (host_) {
    sq::ISettings* s = host_->getSettings();
    if (s->getBool(kKeyLoadUser, SQ_TRUE)) {
      std::string user = sq::specialDir(SQ_DIR_USER_CONFIG);
      if (!user.empty()) roots.push_back(sq::joinPath(user, "scripts"));
    }
    std::string extra = sq::getString(s, kKeyPackagePath);
    size_t start = 0;
    while (start <= extra.size()) {
      size_t semi = extra.find(';', start);
      std::string part = extra.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
      if (!part.empty()) roots.push_back(part);
      if (semi == std::string::npos) break;
      start = semi + 1;
    }
  }
  for (const std::string& root : roots) {
    if (!sq_fs_is_dir(root.c_str())) continue;
    std::vector<std::string> candidates;
    if (sq_fs_is_file(sq::joinPath(root, "package.xml").c_str())) {
      candidates.push_back(root);
    } else {
      sq_fs_list_dir(
          root.c_str(),
          [](void* user, const char* name, sq_bool) -> sq_bool {
            static_cast<std::vector<std::string>*>(user)->push_back(name);
            return SQ_TRUE;
          },
          &candidates);
      std::sort(candidates.begin(), candidates.end());
      for (auto& c : candidates) c = sq::joinPath(root, c);
    }
    for (const std::string& c : candidates) {
      bool isPkgFile = c.size() > 8 && (c.compare(c.size() - 8, 8, ".package") == 0);
      bool isZip = c.size() > 4 && (c.compare(c.size() - 4, 4, ".zip") == 0);
      if (!(sq_fs_is_dir(c.c_str()) && sq_fs_is_file(sq::joinPath(c, "package.xml").c_str())) && !isPkgFile && !isZip) continue;
      sq_result r = loadPackage(c.c_str(), nullptr);
      if (SQ_FAILED(r)) sq::logf(SQ_LOG_WARN, "script package %s not loaded: %s", c.c_str(), sq_result_string(r));
    }
  }
}

// ---- timers ----------------------------------------------------------------------------------
int32_t Engine::addTimer(JSContext* ctx, JSValueConst fn, double ms, bool repeat) {
  if (!JS_IsFunction(ctx, fn)) return -1;
  if (!(ms >= 0)) ms = 0;
  const uint64_t ns = static_cast<uint64_t>(ms * 1e6);
  Timer t{nextTimer_++, sq_time_monotonic_ns() + ns, repeat ? std::max<uint64_t>(ns, 1000000ull) : 0, repeat, ctx, JS_DupValue(ctx, fn)};
  timers_.push_back(t);
  return t.id;
}

void Engine::cancelTimer(int32_t id) {
  for (auto it = timers_.begin(); it != timers_.end(); ++it) {
    if (it->id == id) {
      JS_FreeValueRT(rt_, it->fn);
      timers_.erase(it);
      return;
    }
  }
}

void Engine::runTimers() {
  const uint64_t now = sq_time_monotonic_ns();
  std::vector<int32_t> due;
  for (const Timer& t : timers_)
    if (t.due <= now) due.push_back(t.id);
  for (int32_t id : due) {
    auto it = std::find_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.id == id; });
    if (it == timers_.end()) continue;  // cancelled by an earlier callback
    JSContext* ctx = it->ctx;
    JSValue fn = JS_DupValue(ctx, it->fn);
    if (it->repeat) {
      it->due = now + it->intervalNs;
    } else {
      JS_FreeValueRT(rt_, it->fn);
      timers_.erase(it);
    }
    CallScope scope(*this);
    if (scope.ok()) {
      JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
      if (JS_IsException(r))
        captureException(ctx);
      else
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
  }
}

// ---- settings observers ----------------------------------------------------------------------
int32_t Engine::addSettingsObserver(JSContext* ctx, const std::string& prefix, JSValueConst fn) {
  if (!host_ || !JS_IsFunction(ctx, fn)) return -1;
  const int32_t id = nextWatch_++;
  sq::Ptr<sq::ISettingsObserver> obs = sq::Ptr<sq::ISettingsObserver>::adopt(new SettingsBridge(this, id));
  uint32_t cookie = 0;
  if (host_->getSettings()->addObserver(prefix.c_str(), obs.get(), &cookie) != SQ_OK) return -1;
  watches_.push_back(SettingsWatch{id, prefix, ctx, JS_DupValue(ctx, fn), cookie, obs});
  return id;
}

void Engine::removeSettingsObserver(int32_t id) {
  for (auto it = watches_.begin(); it != watches_.end(); ++it) {
    if (it->id == id) {
      if (host_) host_->getSettings()->removeObserver(it->cookie);
      static_cast<SettingsBridge*>(it->observer.get())->detach();
      JS_FreeValueRT(rt_, it->fn);
      watches_.erase(it);
      return;
    }
  }
}

void Engine::onSettingChangedFromBridge(int32_t watchId, const char* key) {
  if (!key) return;
  if (!onOwnerThread() || stopping_) {
    std::lock_guard<std::mutex> lk(changeMutex_);
    if (pendingChanges_.size() < 4096) pendingChanges_.emplace_back(watchId, key);
    return;
  }
  dispatchSettingChange(watchId, key);
}

void Engine::dispatchSettingChange(int32_t watchId, const std::string& key) {
  auto it = std::find_if(watches_.begin(), watches_.end(), [&](const SettingsWatch& w) { return w.id == watchId; });
  if (it == watches_.end()) return;
  JSContext* ctx = it->ctx;
  JSValue fn = JS_DupValue(ctx, it->fn);
  CallScope scope(*this);
  if (scope.ok()) {
    JSValue arg = newString(ctx, key);
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(r))
      captureException(ctx);
    else
      JS_FreeValue(ctx, r);
  }
  JS_FreeValue(ctx, fn);
}

void Engine::deliverSettingsChanges() {
  std::vector<std::pair<int32_t, std::string>> changes;
  {
    std::lock_guard<std::mutex> lk(changeMutex_);
    changes.swap(pendingChanges_);
  }
  for (auto& [id, key] : changes) dispatchSettingChange(id, key);
}

// ---- IController: object://scripting ----------------------------------------------------------
sq_result SQ_CALL Engine::getProperty(const char* name, sq::IAttributes* out) {
  if (!name || !out) return SQ_E_INVALIDARG;
  const std::string n = name;
  if (n == "info" || n == "value") return out->setString("value", engineInfo().c_str());
  // Everything else is engine-thread state (last error, limits, package list).
  if (!onOwnerThread()) return SQ_E_ACCESS;
  if (n == "lastError") return out->setString("value", lastError_.c_str());
  if (n == "timeLimitMs") return out->setInt("value", static_cast<int64_t>(limits_.timeMs));
  if (n == "memoryLimitMB") return out->setInt("value", static_cast<int64_t>(limits_.memoryBytes >> 20));
  if (n == "stackSizeKB") return out->setInt("value", static_cast<int64_t>(limits_.stackBytes >> 10));
  if (n == "packages" || n == "commands") {
    std::string s;
    for (auto& p : packages()) {
      if (n == "packages") {
        if (!s.empty()) s += ';';
        s += p->id();
        continue;
      }
      for (auto& c : p->classes()) {
        for (auto& cmd : c.commands) {
          s += cmd.category + "\t" + cmd.name + "\t" + cmd.title + "\t" + guidToString(c.id) + "\t" + p->id() + "\n";
        }
      }
    }
    return out->setString("value", s.c_str());
  }
  return SQ_E_NOTFOUND;
}

sq_result SQ_CALL Engine::setProperty(const char* name, sq::IAttributes* in) {
  if (!name || !in) return SQ_E_INVALIDARG;
  if (!host_) return SQ_E_UNSUPPORTED;
  const std::string n = name;
  const char* key = n == "timeLimitMs" ? kKeyTimeLimit : n == "memoryLimitMB" ? kKeyMemoryLimit : n == "stackSizeKB" ? kKeyStackSize : nullptr;
  if (!key) return SQ_E_ACCESS;
  if (in->getType("value") == SQ_VALUE_NONE) return SQ_E_INVALIDARG;
  return host_->getSettings()->setInt(key, in->getInt("value", 0));
}

sq_result SQ_CALL Engine::getChild(const char* name, const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!name || !riid) return SQ_E_INVALIDARG;
  sq::Ptr<Package> p = findPackage(name);
  if (!p) return SQ_E_NOTFOUND;
  return p->queryInterface(riid, out);
}

}  // namespace sqjs
