// scripting-js: JS objects exposed to native code as ICommandHandler / IObserver / IController /
// IComponent / IAction.
#include <cstring>

#include "engine.hpp"

namespace sqjs {

namespace {

bool hasFunction(JSContext* ctx, JSValueConst obj, const char* name) {
  JSValue f = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsException(f)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }
  bool r = JS_IsFunction(ctx, f);
  JS_FreeValue(ctx, f);
  return r;
}

}  // namespace

ScriptObject::ScriptObject(Engine* engine, Package* pkg, JSContext* ctx, JSValueConst value)
    : engine_(engine), pkg_(pkg), ctx_(ctx), value_(JS_DupValue(ctx, value)) {
  if (pkg_) pkg_->liveObjects.fetch_add(1);
  // Interfaces: the explicit `interfaces` list wins; otherwise infer from the methods present.
  JSValue list = JS_GetPropertyStr(ctx, value, "interfaces");
  if (JS_IsException(list)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    list = JS_UNDEFINED;
  }
  if (JS_IsArray(list)) {
    int64_t n = 0;
    if (JS_GetLength(ctx, list, &n) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      n = 0;
    }
    for (int64_t i = 0; i < n && i < 64; ++i) {
      JSValue e = JS_GetPropertyUint32(ctx, list, static_cast<uint32_t>(i));
      sq_guid g{};
      if (parseAnyGuid(toStdString(ctx, e), &g)) {
        if (g == sq::ICommandHandler::iid) ifaces_ |= kCmd;
        if (g == sq::IObserver::iid) ifaces_ |= kObs;
        if (g == sq::IComponent::iid) ifaces_ |= kComp;
        if (g == sq::IAction::iid) ifaces_ |= kAct;
      }
      JS_FreeValue(ctx, e);
    }
  } else {
    if (hasFunction(ctx, value, "interpretCommand")) ifaces_ |= kCmd;
    if (hasFunction(ctx, value, "onNotify")) ifaces_ |= kObs;
    if (hasFunction(ctx, value, "initialize") || hasFunction(ctx, value, "terminate")) ifaces_ |= kComp;
    if (hasFunction(ctx, value, "perform")) ifaces_ |= kAct;
  }
  JS_FreeValue(ctx, list);
  engine_->addLive(this);
}

ScriptObject::~ScriptObject() {
  // release() already unlisted the object and, off the engine thread, queued the value.
  if (ctx_ && engine_->runtime()) JS_FreeValueRT(engine_->runtime(), value_);
  if (pkg_) pkg_->liveObjects.fetch_sub(1);
}

uint32_t SQ_CALL ScriptObject::release() {
  uint32_t cur = refs_.load(std::memory_order_relaxed);
  while (cur > 1) {
    if (refs_.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) return cur - 1;
  }
  // Possibly the last reference: decide under the live lock (findLive / stop add references
  // only under it, so nobody can resurrect the object after this point).
  uint32_t r = 0;
  {
    std::lock_guard<std::mutex> lk(engine_->liveMutex());
    r = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) {
      engine_->removeLiveLocked(this);
      if (ctx_ && !engine_->onOwnerThread()) {
        engine_->deferFreeLocked(value_);  // freed by the engine thread on its next entry/idle
        value_ = JS_UNDEFINED;
        ctx_ = nullptr;
      }
    }
  }
  if (r == 0) delete this;  // outside the lock: freeing the JS value may release other objects
  return r;
}

void ScriptObject::detach() {
  if (!ctx_) return;
  JSValue v = value_;
  value_ = JS_UNDEFINED;
  ctx_ = nullptr;
  if (engine_->runtime()) JS_FreeValueRT(engine_->runtime(), v);
}

bool ScriptObject::supports(const sq_guid& riid) const {
  if (riid == sq::IUnknown::iid || riid == sq::IController::iid || riid == IScriptObjectPrivate::iid) return true;
  if (riid == sq::ICommandHandler::iid) return (ifaces_ & kCmd) != 0;
  if (riid == sq::IObserver::iid) return (ifaces_ & kObs) != 0;
  if (riid == sq::IComponent::iid) return (ifaces_ & kComp) != 0;
  if (riid == sq::IAction::iid) return (ifaces_ & kAct) != 0;
  return false;
}

sq_result SQ_CALL ScriptObject::queryInterface(const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!riid) return SQ_E_INVALIDARG;
  if (!supports(*riid)) return SQ_E_NOINTERFACE;
  if (*riid == sq::IUnknown::iid) {  // identity: always the same base pointer
    addRef();
    *out = static_cast<sq::IController*>(this);
    return SQ_OK;
  }
  return queryInterfaceImpl(riid, out);
}

sq_result ScriptObject::call(const char* name, int argc, JSValue* argv, JSValue* ret, bool* missing) {
  if (missing) *missing = false;
  if (ret) *ret = JS_UNDEFINED;
  if (!ctx_) return SQ_E_CLOSED;
  JSValue fn = JS_GetPropertyStr(ctx_, value_, name);
  if (JS_IsException(fn)) return engine_->captureException(ctx_);
  if (!JS_IsFunction(ctx_, fn)) {
    JS_FreeValue(ctx_, fn);
    if (missing) *missing = true;
    return SQ_E_NOTIMPL;
  }
  JSValue r = JS_Call(ctx_, fn, value_, argc, argv);
  JS_FreeValue(ctx_, fn);
  if (JS_IsException(r)) return engine_->captureException(ctx_);
  if (ret)
    *ret = r;
  else
    JS_FreeValue(ctx_, r);
  return SQ_OK;
}

#define SQJS_ENTER()                         \
  if (!ctx_) return SQ_E_CLOSED;             \
  CallScope scope(*engine_);                 \
  if (!scope.ok()) return scope.status();    \
  if (!ctx_) return SQ_E_CLOSED

sq_result SQ_CALL ScriptObject::interpretCommand(const sq_command* command, sq::IAttributes* args, sq_bool checkOnly) {
  if (!command) return SQ_E_INVALIDARG;
  try {
    SQJS_ENTER();
    JSValue cmd = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, cmd, "category", JS_NewString(ctx_, command->category ? command->category : ""));
    JS_SetPropertyStr(ctx_, cmd, "name", JS_NewString(ctx_, command->name ? command->name : ""));
    JS_SetPropertyStr(ctx_, cmd, "flags", JS_NewInt64(ctx_, command->flags));
    JSValue argv[3] = {cmd, args ? attrsToJs(ctx_, args) : JS_NULL, JS_NewBool(ctx_, checkOnly != 0)};
    JSValue ret = JS_UNDEFINED;
    sq_result r = call("interpretCommand", 3, argv, &ret, nullptr);
    for (JSValue& v : argv) JS_FreeValue(ctx_, v);
    if (SQ_FAILED(r)) return r;
    r = resultFromJs(ctx_, ret, SQ_COMMAND_DISABLED);
    JS_FreeValue(ctx_, ret);
    return r;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

void SQ_CALL ScriptObject::onNotify(sq::IUnknown* subject, const char* topic, sq::IAttributes* args) {
  try {
    if (!ctx_) return;
    CallScope scope(*engine_);
    if (!scope.ok() || !ctx_) return;
    JSValue argv[3] = {subject ? engine_->box(ctx_, subject, true) : JS_NULL, JS_NewString(ctx_, topic ? topic : ""),
                       args ? attrsToJs(ctx_, args) : JS_NULL};
    call("onNotify", 3, argv, nullptr, nullptr);
    for (JSValue& v : argv) JS_FreeValue(ctx_, v);
  } catch (...) {
  }
}

sq_result SQ_CALL ScriptObject::getProperty(const char* name, sq::IAttributes* out) {
  if (!name || !out) return SQ_E_INVALIDARG;
  try {
    SQJS_ENTER();
    JSValue arg = JS_NewString(ctx_, name);
    JSValue v = JS_UNDEFINED;
    bool missing = false;
    sq_result r = call("getProperty", 1, &arg, &v, &missing);
    JS_FreeValue(ctx_, arg);
    if (missing) {
      v = JS_GetPropertyStr(ctx_, value_, name);
      if (JS_IsException(v)) return engine_->captureException(ctx_);
    } else if (SQ_FAILED(r)) {
      return r;
    }
    if (JS_IsUndefined(v)) return SQ_E_NOTFOUND;
    out->remove("value");
    jsToAttrValue(ctx_, out, "value", v);
    JS_FreeValue(ctx_, v);
    return SQ_OK;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ScriptObject::setProperty(const char* name, sq::IAttributes* in) {
  if (!name) return SQ_E_INVALIDARG;
  try {
    SQJS_ENTER();
    JSValue argv[2] = {JS_NewString(ctx_, name), in ? attrValueToJs(ctx_, in, "value") : JS_UNDEFINED};
    JSValue ret = JS_UNDEFINED;
    bool missing = false;
    sq_result r = call("setProperty", 2, argv, &ret, &missing);
    if (missing) {
      r = JS_SetPropertyStr(ctx_, value_, name, JS_DupValue(ctx_, argv[1])) < 0 ? engine_->captureException(ctx_) : SQ_OK;
    } else if (SQ_SUCCEEDED(r)) {
      r = resultFromJs(ctx_, ret, SQ_E_ACCESS);
    }
    JS_FreeValue(ctx_, ret);
    for (JSValue& v : argv) JS_FreeValue(ctx_, v);
    return r;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ScriptObject::getChild(const char* name, const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!name || !riid) return SQ_E_INVALIDARG;
  try {
    SQJS_ENTER();
    JSValue arg = JS_NewString(ctx_, name);
    JSValue v = JS_UNDEFINED;
    bool missing = false;
    sq_result r = call("getChild", 1, &arg, &v, &missing);
    JS_FreeValue(ctx_, arg);
    if (missing) {
      v = JS_GetPropertyStr(ctx_, value_, name);
      if (JS_IsException(v)) return engine_->captureException(ctx_);
    } else if (SQ_FAILED(r)) {
      return r;
    }
    if (!JS_IsObject(v)) {
      JS_FreeValue(ctx_, v);
      return SQ_E_NOTFOUND;
    }
    r = engine_->wrapScriptValue(ctx_, v, pkg_.get(), riid, out);
    JS_FreeValue(ctx_, v);
    return r;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ScriptObject::initialize(sq::IUnknown* context) {
  try {
    SQJS_ENTER();
    JSValue arg = context ? engine_->box(ctx_, context, true) : JS_NULL;
    JSValue ret = JS_UNDEFINED;
    bool missing = false;
    sq_result r = call("initialize", 1, &arg, &ret, &missing);
    JS_FreeValue(ctx_, arg);
    if (missing) return SQ_OK;
    if (SQ_SUCCEEDED(r)) r = resultFromJs(ctx_, ret, SQ_E_FAIL);
    JS_FreeValue(ctx_, ret);
    return r;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ScriptObject::terminate() {
  try {
    SQJS_ENTER();
    JSValue ret = JS_UNDEFINED;
    bool missing = false;
    sq_result r = call("terminate", 0, nullptr, &ret, &missing);
    if (missing) return SQ_OK;
    if (SQ_SUCCEEDED(r)) r = resultFromJs(ctx_, ret, SQ_E_FAIL);
    JS_FreeValue(ctx_, ret);
    return r;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ScriptObject::perform(sq::IDocument* doc, sq::IEditor* editor, sq::IAttributes* params, sq::IAttributes* result) {
  try {
    SQJS_ENTER();
    JSValue resultObj = JS_NewObject(ctx_);
    JSValue argv[4] = {doc ? engine_->box(ctx_, doc, true) : JS_NULL, editor ? engine_->box(ctx_, editor, true) : JS_NULL,
                       params ? attrsToJs(ctx_, params) : JS_NewObject(ctx_), JS_DupValue(ctx_, resultObj)};
    JSValue ret = JS_UNDEFINED;
    sq_result r = call("perform", 4, argv, &ret, nullptr);
    for (JSValue& v : argv) JS_FreeValue(ctx_, v);
    if (SQ_SUCCEEDED(r)) {
      if (result) jsToAttrs(ctx_, JS_IsObject(ret) ? ret : resultObj, result);
      if (JS_IsNumber(ret) || JS_IsBool(ret)) r = resultFromJs(ctx_, ret, SQ_E_FAIL);
    }
    JS_FreeValue(ctx_, ret);
    JS_FreeValue(ctx_, resultObj);
    return r;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

// ---- ScriptClassFactory -----------------------------------------------------------------------
sq_result SQ_CALL ScriptClassFactory::getModuleInfo(sq_module_info* out) {
  if (!out || out->struct_size < sizeof(sq_module_info)) return SQ_E_INVALIDARG;
  out->fourcc = kModuleFourcc;
  out->name = kModuleName;
  out->version = SQ_MODULE_VERSION;
  out->abi_min = SQ_ABI_DEFAULT_MIN;
  out->abi_max = SQ_ABI_DEFAULT_MAX;
  out->sdk_abi = SQ_PLATFORM_ABI;
  out->build_stamp = SQ_BUILD_STAMP;
  out->description = "script package class";
  return SQ_OK;
}

sq_result SQ_CALL ScriptClassFactory::getClassInfo(uint32_t index, sq_class_info* out) {
  if (!out || out->struct_size < sizeof(sq_class_info)) return SQ_E_INVALIDARG;
  if (!pkg_ || index != 0) return SQ_E_NOTFOUND;
  return pkg_->getClassInfo(static_cast<uint32_t>(index_), out);
}

sq_result SQ_CALL ScriptClassFactory::createInstance(const sq_guid* classId, const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!classId || !riid) return SQ_E_INVALIDARG;
  if (!pkg_ || !pkg_->engine) return SQ_E_CLOSED;
  if (index_ >= pkg_->classes().size() || !(pkg_->classes()[index_].id == *classId)) return SQ_E_NOTFOUND;
  return pkg_->engine->createFromClass(pkg_, index_, riid, out);
}

}  // namespace sqjs
