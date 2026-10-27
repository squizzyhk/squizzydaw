// ui/ UI Automation bridge: providers expose names, control types, structure, bounds, runtime
// ids and the RangeValue / Toggle / Invoke patterns of a skin-built widget tree; a real window
// answers WM_GETOBJECT and a UIA client (on another thread) sees the same tree.
#include <string>
#include <vector>

#include "sq/ui/skin.hpp"
#include "sq/ui/uia.hpp"
#include "sq/ui/widgets.hpp"
#include "sqtest.hpp"

#ifdef _WIN32
#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <atomic>
#include <thread>
#endif

using namespace sq::ui;

#ifdef _WIN32
namespace {

const char* kSkin = R"~(<Skin name="a11y"><Forms>
  <Form name="Main" direction="column" width="400" height="300">
    <Label name="title" text="Tracks" height="20"/>
    <View name="strip" direction="row" height="120" tooltip="Channel strip">
      <Knob name="pan" text="Pan" width="40" min="-1" max="1"/>
      <Fader name="vol" text="Volume" width="30" min="-60" max="6" value="0"/>
      <Button name="mute" text="Mute" toggles="1" width="40"/>
      <Button name="play" text="Play" command="Transport.Play" width="60" tooltip="Start playback"/>
      <Meter name="peak" text="Peak" width="10" enabled="0"/>
    </View>
    <Image name="logo" text="Logo" height="40"/>
  </Form>
</Forms></Skin>)~";

struct Tree {
  WidgetTree tree;
  std::string fired;
  Tree() {
    std::string err;
    auto skin = Skin::parse(kSkin, &err);
    tree.setRoot(skin->build("Main", {}, &err));
    tree.setScale(1.5f);
    tree.layout(600, 450);
    tree.onCommand = [this](Widget& w) { fired = w.command; };
  }
};

template <class T>
struct ComPtr {
  T* p = nullptr;
  ~ComPtr() {
    if (p) p->Release();
  }
  T** put() {
    if (p) p->Release();
    p = nullptr;
    return &p;
  }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

std::string narrow(const wchar_t* w) {
  if (!w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 1 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string nameOf(IRawElementProviderSimple* p) {
  VARIANT v;
  if (FAILED(p->GetPropertyValue(UIA_NamePropertyId, &v)) || v.vt != VT_BSTR) return "<none>";
  std::string s = narrow(v.bstrVal);
  VariantClear(&v);
  return s;
}
long typeOf(IRawElementProviderSimple* p) {
  VARIANT v;
  if (FAILED(p->GetPropertyValue(UIA_ControlTypePropertyId, &v)) || v.vt != VT_I4) return -1;
  return v.lVal;
}

// Depth-first "name:type" list through Navigate.
void walk(IRawElementProviderFragment* f, std::vector<std::string>& out, int depth = 0) {
  ComPtr<IRawElementProviderSimple> s;
  f->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(s.put()));
  out.push_back(std::string(static_cast<size_t>(depth), ' ') + nameOf(s.p) + ":" + std::to_string(typeOf(s.p)));
  ComPtr<IRawElementProviderFragment> c;
  f->Navigate(NavigateDirection_FirstChild, c.put());
  while (c) {
    walk(c.p, out, depth + 1);
    ComPtr<IRawElementProviderFragment> next;
    c->Navigate(NavigateDirection_NextSibling, next.put());
    std::swap(c.p, next.p);
  }
}

ComPtr<IRawElementProviderSimple> providerOf(UiaBridge& b, Widget* w) {
  ComPtr<IRawElementProviderSimple> p;
  p.p = static_cast<IRawElementProviderSimple*>(b.providerFor(w->accessibleId()));
  return p;
}

}  // namespace

SQ_TEST("uia provider: names, control types and structure of a widget tree") {
  Tree t;
  UiaBridge bridge(nullptr);
  CHECK(bridge.rootProvider() == nullptr);  // no tree yet
  bridge.setRoot(t.tree.root());
  ComPtr<IRawElementProviderSimple> root;
  root.p = static_cast<IRawElementProviderSimple*>(bridge.rootProvider());
  REQUIRE(root);
  ComPtr<IRawElementProviderFragment> frag;
  REQUIRE(SUCCEEDED(root->QueryInterface(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(frag.put()))));
  std::vector<std::string> got;
  walk(frag.p, got);
  const std::vector<std::string> want = {
      "Main:" + std::to_string(UIA_GroupControlTypeId),
      " Tracks:" + std::to_string(UIA_TextControlTypeId),
      " Channel strip:" + std::to_string(UIA_GroupControlTypeId),  // tooltip names an unlabeled view
      "  Pan:" + std::to_string(UIA_SliderControlTypeId),
      "  Volume:" + std::to_string(UIA_SliderControlTypeId),
      "  Mute:" + std::to_string(UIA_ButtonControlTypeId),
      "  Play:" + std::to_string(UIA_ButtonControlTypeId),
      "  Peak:" + std::to_string(UIA_ProgressBarControlTypeId),
      " Logo:" + std::to_string(UIA_ImageControlTypeId),
  };
  CHECK_EQ(got.size(), want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i) CHECK_EQ(got[i], want[i]);

  // Other properties
  auto play = providerOf(bridge, t.tree.findByName("play"));
  REQUIRE(play);
  VARIANT v;
  REQUIRE(SUCCEEDED(play->GetPropertyValue(UIA_AutomationIdPropertyId, &v)));
  CHECK_EQ(narrow(v.bstrVal), std::string("play"));
  VariantClear(&v);
  REQUIRE(SUCCEEDED(play->GetPropertyValue(UIA_HelpTextPropertyId, &v)));
  CHECK_EQ(narrow(v.bstrVal), std::string("Start playback"));
  VariantClear(&v);
  REQUIRE(SUCCEEDED(play->GetPropertyValue(UIA_IsKeyboardFocusablePropertyId, &v)));
  CHECK(v.vt == VT_BOOL && v.boolVal == VARIANT_TRUE);
  auto peak = providerOf(bridge, t.tree.findByName("peak"));
  REQUIRE(SUCCEEDED(peak->GetPropertyValue(UIA_IsEnabledPropertyId, &v)));
  CHECK(v.vt == VT_BOOL && v.boolVal == VARIANT_FALSE);
  REQUIRE(SUCCEEDED(play->GetPropertyValue(UIA_ClassNamePropertyId, &v)));
  CHECK(v.vt == VT_EMPTY);  // unknown properties: empty, not an error
  ProviderOptions opts{};
  CHECK(SUCCEEDED(play->get_ProviderOptions(&opts)));
  CHECK((opts & ProviderOptions_ServerSideProvider) != 0);

  // Parent / siblings / runtime id / bounds (client pixels without a window, scale 1.5)
  ComPtr<IRawElementProviderFragment> pf, parent, prev, rootFromChild;
  REQUIRE(SUCCEEDED(play->QueryInterface(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(pf.put()))));
  pf->Navigate(NavigateDirection_Parent, parent.put());
  REQUIRE(parent);
  ComPtr<IRawElementProviderSimple> ps;
  parent->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(ps.put()));
  CHECK_EQ(nameOf(ps.p), std::string("Channel strip"));
  pf->Navigate(NavigateDirection_PreviousSibling, prev.put());
  REQUIRE(prev);
  ComPtr<IRawElementProviderFragmentRoot> fr;
  CHECK(SUCCEEDED(pf->get_FragmentRoot(fr.put())));
  CHECK(static_cast<bool>(fr));
  ComPtr<IRawElementProviderFragment> rootParent;
  frag->Navigate(NavigateDirection_Parent, rootParent.put());
  CHECK(!rootParent);  // the root's parent is the host window (none here)
  SAFEARRAY* rid = nullptr;
  REQUIRE(SUCCEEDED(pf->GetRuntimeId(&rid)));
  REQUIRE(rid != nullptr);
  LONG idx = 1;
  int idv = 0;
  SafeArrayGetElement(rid, &idx, &idv);
  CHECK_EQ(static_cast<uint32_t>(idv), t.tree.findByName("play")->accessibleId());
  SafeArrayDestroy(rid);
  UiaRect r{};
  REQUIRE(SUCCEEDED(pf->get_BoundingRectangle(&r)));
  const Rect& f = t.tree.findByName("play")->frame();
  CHECK_EQ(r.left, static_cast<double>(f.x * 1.5f));
  CHECK_EQ(r.width, static_cast<double>(f.w * 1.5f));
  // Hit testing through the fragment root
  ComPtr<IRawElementProviderFragmentRoot> rootFr;
  REQUIRE(SUCCEEDED(root->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(rootFr.put()))));
  ComPtr<IRawElementProviderFragment> hit;
  REQUIRE(SUCCEEDED(rootFr->ElementProviderFromPoint(r.left + r.width / 2, r.top + r.height / 2, hit.put())));
  REQUIRE(hit);
  ComPtr<IRawElementProviderSimple> hs;
  hit->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(hs.put()));
  CHECK_EQ(nameOf(hs.p), std::string("Play"));
  // Only the root answers IRawElementProviderFragmentRoot.
  ComPtr<IRawElementProviderFragmentRoot> notRoot;
  CHECK(FAILED(play->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(notRoot.put()))));
}

SQ_TEST("uia provider: RangeValue, Toggle and Invoke patterns drive the widgets") {
  Tree t;
  UiaBridge bridge(nullptr);
  bridge.setRoot(t.tree.root());
  int userChanges = 0;
  t.tree.onValueChanged = [&](Widget&) { ++userChanges; };

  auto vol = providerOf(bridge, t.tree.findByName("vol"));
  ComPtr<IUnknown> unk;
  REQUIRE(SUCCEEDED(vol->GetPatternProvider(UIA_RangeValuePatternId, unk.put())));
  REQUIRE(unk);
  ComPtr<IRangeValueProvider> rv;
  REQUIRE(SUCCEEDED(unk->QueryInterface(__uuidof(IRangeValueProvider), reinterpret_cast<void**>(rv.put()))));
  double d = 0;
  rv->get_Minimum(&d);
  CHECK_EQ(d, -60.0);
  rv->get_Maximum(&d);
  CHECK_EQ(d, 6.0);
  CHECK(SUCCEEDED(rv->SetValue(-12.0)));
  CHECK_EQ(t.tree.findByName("vol")->value(), -12.0);
  CHECK_EQ(userChanges, 1);
  CHECK_EQ(rv->SetValue(100.0), static_cast<HRESULT>(E_INVALIDARG));
  ComPtr<IUnknown> none;
  vol->GetPatternProvider(UIA_TogglePatternId, none.put());
  CHECK(!none);

  auto mute = providerOf(bridge, t.tree.findByName("mute"));
  ComPtr<IUnknown> tu;
  REQUIRE(SUCCEEDED(mute->GetPatternProvider(UIA_TogglePatternId, tu.put())));
  REQUIRE(tu);
  ComPtr<IToggleProvider> tp;
  tu->QueryInterface(__uuidof(IToggleProvider), reinterpret_cast<void**>(tp.put()));
  ToggleState ts{};
  tp->get_ToggleState(&ts);
  CHECK(ts == ToggleState_Off);
  CHECK(SUCCEEDED(tp->Toggle()));
  tp->get_ToggleState(&ts);
  CHECK(ts == ToggleState_On);
  ComPtr<IUnknown> noRange;
  mute->GetPatternProvider(UIA_RangeValuePatternId, noRange.put());
  CHECK(!noRange);  // toggles expose Toggle, not RangeValue

  auto play = providerOf(bridge, t.tree.findByName("play"));
  ComPtr<IUnknown> iu;
  REQUIRE(SUCCEEDED(play->GetPatternProvider(UIA_InvokePatternId, iu.put())));
  REQUIRE(iu);
  ComPtr<IInvokeProvider> ip;
  iu->QueryInterface(__uuidof(IInvokeProvider), reinterpret_cast<void**>(ip.put()));
  CHECK(SUCCEEDED(ip->Invoke()));
  CHECK_EQ(t.fired, std::string("Transport.Play"));

  auto peak = providerOf(bridge, t.tree.findByName("peak"));
  ComPtr<IUnknown> pr;
  peak->GetPatternProvider(UIA_RangeValuePatternId, pr.put());
  ComPtr<IRangeValueProvider> prv;
  pr->QueryInterface(__uuidof(IRangeValueProvider), reinterpret_cast<void**>(prv.put()));
  BOOL ro = FALSE;
  prv->get_IsReadOnly(&ro);
  CHECK(ro == TRUE);
  CHECK_EQ(prv->SetValue(0.5), static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED));

  // Providers outlive the tree safely: they report "element not available".
  t.tree.setRoot(nullptr);
  bridge.setRoot(nullptr);
  CHECK_EQ(rv->SetValue(0.0), static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE));
  VARIANT v;
  CHECK_EQ(play->GetPropertyValue(UIA_NamePropertyId, &v), static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE));
  CHECK_EQ(tp->Toggle(), static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE));
}

SQ_TEST("uia window: WM_GETOBJECT serves the tree and a UIA client reads names and roles") {
  Tree t;
  auto window = createWindow(WindowDesc{"uia test", 640, 480, false, Backend::Auto});
  if (!window) SQ_SKIP("no window backend");
  auto hwnd = static_cast<HWND>(window->nativeHandle());
  window->setAccessibilityRoot(t.tree.root());
  LRESULT lr = SendMessageW(hwnd, WM_GETOBJECT, 0, static_cast<LPARAM>(UiaRootObjectId));
  CHECK(lr != 0);

  // A real client: CUIAutomation on a worker thread (UIA must not be called from the provider's
  // UI thread); this thread keeps pumping messages so the cross-thread calls are served.
  std::atomic<bool> done{false};
  std::vector<std::string> names;
  std::string failure;
  std::thread client([&] {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
      ComPtr<IUIAutomation> uia;
      if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                  reinterpret_cast<void**>(uia.put())))) {
        failure = "skip: CUIAutomation unavailable";
      } else {
        ComPtr<IUIAutomationElement> el;
        ComPtr<IUIAutomationCondition> all;
        ComPtr<IUIAutomationElementArray> found;
        if (FAILED(uia->ElementFromHandle(hwnd, el.put())) || !el) failure = "ElementFromHandle failed";
        else if (FAILED(uia->CreateTrueCondition(all.put())) || FAILED(el->FindAll(TreeScope_Descendants, all.p, found.put())) || !found)
          failure = "FindAll failed";
        else {
          int n = 0;
          found->get_Length(&n);
          for (int i = 0; i < n; ++i) {
            ComPtr<IUIAutomationElement> e;
            found->GetElement(i, e.put());
            BSTR b = nullptr;
            CONTROLTYPEID ct = 0;
            e->get_CurrentName(&b);
            e->get_CurrentControlType(&ct);
            names.push_back(narrow(b) + ":" + std::to_string(ct));
            SysFreeString(b);
          }
        }
      }
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    done = true;
  });
  while (!done) {  // UIA's own timeouts bound this; CTest's timeout is the backstop
    window->pumpEvents(false);
    Sleep(5);
  }
  client.join();
  if (failure.rfind("skip:", 0) == 0) SQ_SKIP(failure);
  CHECK_EQ(failure, std::string());
  auto has = [&](const std::string& s) {
    for (const auto& n : names)
      if (n == s) return true;
    return false;
  };
  CHECK(has("Tracks:" + std::to_string(UIA_TextControlTypeId)));
  CHECK(has("Pan:" + std::to_string(UIA_SliderControlTypeId)));
  CHECK(has("Mute:" + std::to_string(UIA_ButtonControlTypeId)));
  CHECK(has("Play:" + std::to_string(UIA_ButtonControlTypeId)));
  CHECK(has("Peak:" + std::to_string(UIA_ProgressBarControlTypeId)));
  window->setAccessibilityRoot(nullptr);
}

#else

SQ_TEST("uia provider: UI Automation is Windows-only") { SQ_SKIP("UI Automation is Windows-only"); }
SQ_TEST("uia window: UI Automation is Windows-only") { SQ_SKIP("UI Automation is Windows-only"); }

#endif
