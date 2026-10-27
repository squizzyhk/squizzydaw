// UI Automation server-side providers over AccessibleNode (see uia.hpp).
#include "sq/ui/uia.hpp"

#ifdef _WIN32
#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <atomic>
#include <string>

#include "sq/ui/widgets.hpp"

namespace sq::ui {

struct UiaBridge::State {
  // Owned by the UI thread: only touched there (provider calls from other threads are first
  // marshalled onto it through `dispatch`).
  HWND hwnd = nullptr;
  AccessibleNode* root = nullptr;
  // Message-only window on the UI thread that runs provider calls arriving on other threads.
  std::atomic<HWND> dispatch{nullptr};
  DWORD uiThread = 0;
};

namespace {

// A provider call marshalled to the UI thread (lParam of kInvokeMessage).
struct Invocation {
  HRESULT (*fn)(void*);
  void* ctx;
  HRESULT hr;
  bool ran;
};

const UINT kInvokeMessage = WM_APP + 0x51A;
const wchar_t* kDispatchClass = L"SquizzyUiaDispatch";

LRESULT CALLBACK dispatchProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == kInvokeMessage && lp) {
    auto* inv = reinterpret_cast<Invocation*>(lp);
    inv->hr = inv->fn(inv->ctx);
    inv->ran = true;
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

HWND createDispatchWindow() {
  static std::atomic<bool> registered{false};
  HINSTANCE inst = GetModuleHandleW(nullptr);
  if (!registered.exchange(true)) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &dispatchProc;
    wc.hInstance = inst;
    wc.lpszClassName = kDispatchClass;
    RegisterClassExW(&wc);
  }
  return CreateWindowExW(0, kDispatchClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

void setBstr(VARIANT* v, const std::wstring& s) {
  v->vt = VT_BSTR;
  v->bstrVal = SysAllocStringLen(s.data(), static_cast<UINT>(s.size()));
}
void setBool(VARIANT* v, bool b) {
  v->vt = VT_BOOL;
  v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}
void setI4(VARIANT* v, long x) {
  v->vt = VT_I4;
  v->lVal = x;
}

long controlType(AccessibleRole r) {
  switch (r) {
    case AccessibleRole::Window: return UIA_WindowControlTypeId;
    case AccessibleRole::Group: return UIA_GroupControlTypeId;
    case AccessibleRole::Text: return UIA_TextControlTypeId;
    case AccessibleRole::Button: return UIA_ButtonControlTypeId;
    case AccessibleRole::ToggleButton: return UIA_ButtonControlTypeId;  // + Toggle pattern
    case AccessibleRole::Slider: return UIA_SliderControlTypeId;
    case AccessibleRole::Image: return UIA_ImageControlTypeId;
    case AccessibleRole::Meter: return UIA_ProgressBarControlTypeId;
    default: return UIA_CustomControlTypeId;
  }
}

class Provider final : public IRawElementProviderSimple,
                       public IRawElementProviderFragment,
                       public IRawElementProviderFragmentRoot,
                       public IRangeValueProvider,
                       public IToggleProvider,
                       public IInvokeProvider {
 public:
  Provider(std::shared_ptr<UiaBridge::State> st, uint32_t id, bool isRoot) : st_(std::move(st)), id_(id), isRoot_(isRoot) {}

  // UI thread only.
  static Provider* make(const std::shared_ptr<UiaBridge::State>& st, AccessibleNode* n) {
    if (!n) return nullptr;
    return new Provider(st, n->accessibleId(), n == st->root);
  }

  // IUnknown (thread-safe: touches no UI state)
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
    else if (riid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
    else if (riid == __uuidof(IRawElementProviderFragmentRoot) && isRoot_) *out = static_cast<IRawElementProviderFragmentRoot*>(this);
    else if (riid == __uuidof(IRangeValueProvider)) *out = static_cast<IRangeValueProvider*>(this);
    else if (riid == __uuidof(IToggleProvider)) *out = static_cast<IToggleProvider*>(this);
    else if (riid == __uuidof(IInvokeProvider)) *out = static_cast<IInvokeProvider*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }

  // IRawElementProviderSimple
  STDMETHODIMP get_ProviderOptions(ProviderOptions* ret) override {
    if (!ret) return E_POINTER;
    // Server-side, default UIA threading. Calls can still arrive on any thread (an in-process
    // client calls straight through), so every method below runs its work on the UI thread.
    *ret = ProviderOptions_ServerSideProvider;
    return S_OK;
  }
  STDMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** ret) override {
    if (!ret) return E_POINTER;
    *ret = nullptr;
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      if (pattern == UIA_TogglePatternId && n->isToggle()) *ret = static_cast<IToggleProvider*>(this);
      else if (pattern == UIA_RangeValuePatternId && n->hasRangeValue() && !n->isToggle()) *ret = static_cast<IRangeValueProvider*>(this);
      else if (pattern == UIA_InvokePatternId && n->accessibleRole() == AccessibleRole::Button) *ret = static_cast<IInvokeProvider*>(this);
      if (*ret) (*ret)->AddRef();
      return S_OK;
    });
  }
  STDMETHODIMP GetPropertyValue(PROPERTYID prop, VARIANT* ret) override {
    if (!ret) return E_POINTER;
    VariantInit(ret);
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      switch (prop) {
        case UIA_NamePropertyId: setBstr(ret, widen(n->accessibleName())); break;
        case UIA_ControlTypePropertyId: setI4(ret, controlType(n->accessibleRole())); break;
        case UIA_AutomationIdPropertyId: {
          const auto* w = dynamic_cast<const Widget*>(n);
          std::string id = w && !w->name.empty() ? w->name : "sq-" + std::to_string(n->accessibleId());
          setBstr(ret, widen(id));
          break;
        }
        case UIA_HelpTextPropertyId: {
          std::string h = n->accessibleHelp();
          if (!h.empty()) setBstr(ret, widen(h));
          break;
        }
        case UIA_IsEnabledPropertyId: setBool(ret, n->accessibleEnabled()); break;
        case UIA_IsKeyboardFocusablePropertyId: {
          const auto* w = dynamic_cast<const Widget*>(n);
          setBool(ret, w && w->focusable());
          break;
        }
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: setBool(ret, true); break;
        case UIA_FrameworkIdPropertyId: setBstr(ret, L"Squizzy"); break;
        case UIA_ProviderDescriptionPropertyId: setBstr(ret, L"Squizzy Studio UI"); break;
        default: break;
      }
      return S_OK;
    });
  }
  STDMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** ret) override {
    if (!ret) return E_POINTER;
    *ret = nullptr;
    return onUi([&]() -> HRESULT {
      if (isRoot_ && st_->hwnd) return UiaHostProviderFromHwnd(st_->hwnd, ret);
      return S_OK;
    });
  }

  // IRawElementProviderFragment
  STDMETHODIMP Navigate(NavigateDirection dir, IRawElementProviderFragment** ret) override {
    if (!ret) return E_POINTER;
    *ret = nullptr;
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      AccessibleNode* target = nullptr;
      switch (dir) {
        case NavigateDirection_Parent:
          if (!isRoot_) target = n->accessibleParent();
          break;
        case NavigateDirection_FirstChild:
          if (n->accessibleChildCount()) target = n->accessibleChild(0);
          break;
        case NavigateDirection_LastChild:
          if (size_t c = n->accessibleChildCount()) target = n->accessibleChild(c - 1);
          break;
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling: {
          AccessibleNode* p = isRoot_ ? nullptr : n->accessibleParent();
          if (!p) break;
          for (size_t i = 0, c = p->accessibleChildCount(); i < c; ++i)
            if (p->accessibleChild(i) == n) {
              if (dir == NavigateDirection_NextSibling && i + 1 < c) target = p->accessibleChild(i + 1);
              if (dir == NavigateDirection_PreviousSibling && i > 0) target = p->accessibleChild(i - 1);
              break;
            }
          break;
        }
        default: break;
      }
      if (target) *ret = static_cast<IRawElementProviderFragment*>(make(st_, target));
      return S_OK;
    });
  }
  STDMETHODIMP GetRuntimeId(SAFEARRAY** ret) override {
    if (!ret) return E_POINTER;
    *ret = nullptr;
    return onUi([&]() -> HRESULT {
      if (!node()) return UIA_E_ELEMENTNOTAVAILABLE;
      if (isRoot_ && st_->hwnd) return S_OK;  // the host HWND provides the root's runtime id
      SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 2);
      if (!sa) return E_OUTOFMEMORY;
      LONG idx = 0;
      int v0 = UiaAppendRuntimeId;
      int v1 = static_cast<int>(id_);
      SafeArrayPutElement(sa, &idx, &v0);
      idx = 1;
      SafeArrayPutElement(sa, &idx, &v1);
      *ret = sa;
      return S_OK;
    });
  }
  STDMETHODIMP get_BoundingRectangle(UiaRect* ret) override {
    if (!ret) return E_POINTER;
    *ret = UiaRect{};
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      Rect r = n->accessibleBounds();
      POINT origin{0, 0};
      if (st_->hwnd) ClientToScreen(st_->hwnd, &origin);
      *ret = UiaRect{r.x + origin.x, r.y + origin.y, r.w, r.h};
      return S_OK;
    });
  }
  STDMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** ret) override {
    if (!ret) return E_POINTER;
    *ret = nullptr;
    return S_OK;
  }
  STDMETHODIMP SetFocus() override {
    return onUi([&]() -> HRESULT { return node() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE; });
  }
  STDMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override {
    if (!ret) return E_POINTER;
    *ret = nullptr;
    return onUi([&]() -> HRESULT {
      if (!st_->root) return UIA_E_ELEMENTNOTAVAILABLE;
      *ret = static_cast<IRawElementProviderFragmentRoot*>(make(st_, st_->root));
      return S_OK;
    });
  }

  // IRawElementProviderFragmentRoot
  STDMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** ret) override {
    if (!ret) return E_POINTER;
    *ret = nullptr;
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = st_->root;
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      POINT origin{0, 0};
      if (st_->hwnd) ClientToScreen(st_->hwnd, &origin);
      const auto cx = static_cast<float>(x - origin.x), cy = static_cast<float>(y - origin.y);
      if (!n->accessibleBounds().contains(cx, cy)) return S_OK;
      for (bool descended = true; descended;) {
        descended = false;
        for (size_t i = n->accessibleChildCount(); i-- > 0;) {
          AccessibleNode* c = n->accessibleChild(i);
          if (c && c->accessibleBounds().contains(cx, cy)) {
            n = c;
            descended = true;
            break;
          }
        }
      }
      *ret = static_cast<IRawElementProviderFragment*>(make(st_, n));
      return S_OK;
    });
  }
  STDMETHODIMP GetFocus(IRawElementProviderFragment** ret) override {
    if (!ret) return E_POINTER;
    *ret = nullptr;
    return S_OK;
  }

  // IRangeValueProvider
  STDMETHODIMP SetValue(double v) override {
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      if (!n->accessibleEnabled()) return UIA_E_ELEMENTNOTENABLED;
      if (n->rangeReadOnly() || !(v >= n->rangeMin() && v <= n->rangeMax())) return E_INVALIDARG;  // NaN too
      return n->setRangeValue(v) ? S_OK : UIA_E_INVALIDOPERATION;
    });
  }
  STDMETHODIMP get_Value(double* ret) override { return range(ret, [](AccessibleNode* n) { return n->rangeValue(); }); }
  STDMETHODIMP get_IsReadOnly(BOOL* ret) override {
    if (!ret) return E_POINTER;
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      *ret = n->rangeReadOnly() ? TRUE : FALSE;
      return S_OK;
    });
  }
  STDMETHODIMP get_Maximum(double* ret) override { return range(ret, [](AccessibleNode* n) { return n->rangeMax(); }); }
  STDMETHODIMP get_Minimum(double* ret) override { return range(ret, [](AccessibleNode* n) { return n->rangeMin(); }); }
  STDMETHODIMP get_LargeChange(double* ret) override {
    return range(ret, [](AccessibleNode* n) { return (n->rangeMax() - n->rangeMin()) / 10; });
  }
  STDMETHODIMP get_SmallChange(double* ret) override {
    return range(ret, [](AccessibleNode* n) { return (n->rangeMax() - n->rangeMin()) / 100; });
  }

  // IToggleProvider
  STDMETHODIMP Toggle() override {
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      if (!n->accessibleEnabled()) return UIA_E_ELEMENTNOTENABLED;
      return n->toggle() ? S_OK : UIA_E_INVALIDOPERATION;
    });
  }
  STDMETHODIMP get_ToggleState(ToggleState* ret) override {
    if (!ret) return E_POINTER;
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      *ret = n->toggled() ? ToggleState_On : ToggleState_Off;
      return S_OK;
    });
  }

  // IInvokeProvider
  STDMETHODIMP Invoke() override {
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      if (!n->accessibleEnabled()) return UIA_E_ELEMENTNOTENABLED;
      return n->invoke() ? S_OK : UIA_E_INVALIDOPERATION;
    });
  }

 private:
  ~Provider() = default;

  // Runs `f` on the UI thread: directly when already there (or when there is no window, e.g.
  // tests), else synchronously through the bridge's message-only window. A bridge or window
  // that is gone yields UIA_E_ELEMENTNOTAVAILABLE.
  template <class F>
  HRESULT onUi(F&& f) {
    HWND d = st_->dispatch.load(std::memory_order_acquire);
    if (!d || GetCurrentThreadId() == st_->uiThread) {
      if (d == nullptr && st_->uiThread != 0 && GetCurrentThreadId() != st_->uiThread) return UIA_E_ELEMENTNOTAVAILABLE;
      return f();
    }
    Invocation inv{[](void* c) -> HRESULT { return (*static_cast<F*>(c))(); }, &f, E_FAIL, false};
    DWORD_PTR res = 0;
    // The timeout keeps a client from hanging forever on a UI thread that stopped pumping.
    if (!SendMessageTimeoutW(d, kInvokeMessage, 0, reinterpret_cast<LPARAM>(&inv), SMTO_NORMAL, 10000, &res) || !inv.ran)
      return UIA_E_ELEMENTNOTAVAILABLE;
    return inv.hr;
  }

  AccessibleNode* node() const {
    AccessibleNode* root = st_->root;
    if (!root) return nullptr;
    if (isRoot_) return root;
    return findAccessible(root, id_);
  }
  template <class G>
  HRESULT range(double* ret, G g) {
    if (!ret) return E_POINTER;
    return onUi([&]() -> HRESULT {
      AccessibleNode* n = node();
      if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
      *ret = g(n);
      return S_OK;
    });
  }

  std::atomic<ULONG> refs_{1};
  std::shared_ptr<UiaBridge::State> st_;
  uint32_t id_;
  bool isRoot_;
};

}  // namespace

UiaBridge::UiaBridge(void* hwnd) : state_(std::make_shared<State>()) {
  state_->hwnd = static_cast<HWND>(hwnd);
  if (state_->hwnd) {
    state_->uiThread = GetCurrentThreadId();
    state_->dispatch.store(createDispatchWindow(), std::memory_order_release);
  }
}

UiaBridge::~UiaBridge() { windowDestroyed(); }

void UiaBridge::setRoot(AccessibleNode* root) {
  state_->root = root;
  notifyChanged(nullptr);
}

AccessibleNode* UiaBridge::root() const { return state_->root; }

void* UiaBridge::rootProvider() {
  if (!state_->root) return nullptr;
  return static_cast<IRawElementProviderSimple*>(Provider::make(state_, state_->root));
}

void* UiaBridge::providerFor(uint32_t accessibleId) {
  AccessibleNode* n = state_->root ? findAccessible(state_->root, accessibleId) : nullptr;
  return n ? static_cast<IRawElementProviderSimple*>(Provider::make(state_, n)) : nullptr;
}

intptr_t UiaBridge::handleGetObject(uintptr_t wparam, intptr_t lparam, bool& handled) {
  handled = false;
  if (!state_->hwnd || !state_->root || static_cast<long>(lparam) != static_cast<long>(UiaRootObjectId)) return 0;
  auto* p = static_cast<IRawElementProviderSimple*>(rootProvider());
  if (!p) return 0;
  LRESULT r = UiaReturnRawElementProvider(state_->hwnd, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam), p);
  p->Release();
  handled = true;
  return static_cast<intptr_t>(r);
}

void UiaBridge::notifyChanged(AccessibleNode* node) {
  if (!state_->root || !UiaClientsAreListening()) return;
  if (!node || node == state_->root) {
    if (auto* p = static_cast<IRawElementProviderSimple*>(rootProvider())) {
      UiaRaiseStructureChangedEvent(p, StructureChangeType_ChildrenInvalidated, nullptr, 0);
      p->Release();
    }
    return;
  }
  auto* p = static_cast<IRawElementProviderSimple*>(providerFor(node->accessibleId()));
  if (!p) return;
  if (node->hasRangeValue()) {
    VARIANT oldV, newV;
    VariantInit(&oldV);
    VariantInit(&newV);
    if (node->isToggle()) {
      newV.vt = VT_I4;
      newV.lVal = node->toggled() ? ToggleState_On : ToggleState_Off;
      UiaRaiseAutomationPropertyChangedEvent(p, UIA_ToggleToggleStatePropertyId, oldV, newV);
    } else {
      newV.vt = VT_R8;
      newV.dblVal = node->rangeValue();
      UiaRaiseAutomationPropertyChangedEvent(p, UIA_RangeValueValuePropertyId, oldV, newV);
    }
  } else {
    UiaRaiseStructureChangedEvent(p, StructureChangeType_ChildrenInvalidated, nullptr, 0);
  }
  p->Release();
}

void UiaBridge::windowDestroyed() {
  // UI thread. Stop marshalling first: a provider call racing this sees no dispatch window (or a
  // destroyed one, whose SendMessageTimeout fails) and reports "element not available".
  if (HWND d = state_->dispatch.exchange(nullptr, std::memory_order_acq_rel)) DestroyWindow(d);
  if (state_->hwnd) UiaReturnRawElementProvider(state_->hwnd, 0, 0, nullptr);
  state_->hwnd = nullptr;
  state_->root = nullptr;
}

}  // namespace sq::ui

#else  // !_WIN32

namespace sq::ui {

struct UiaBridge::State {
  AccessibleNode* root = nullptr;
};

UiaBridge::UiaBridge(void*) : state_(std::make_shared<State>()) {}
UiaBridge::~UiaBridge() = default;
void UiaBridge::setRoot(AccessibleNode* root) { state_->root = root; }
AccessibleNode* UiaBridge::root() const { return state_->root; }
void* UiaBridge::rootProvider() { return nullptr; }
void* UiaBridge::providerFor(uint32_t) { return nullptr; }
intptr_t UiaBridge::handleGetObject(uintptr_t, intptr_t, bool& handled) {
  handled = false;
  return 0;
}
void UiaBridge::notifyChanged(AccessibleNode*) {}
void UiaBridge::windowDestroyed() { state_->root = nullptr; }

}  // namespace sq::ui

#endif
