// ui/ M5 hardening (adversarial review): hostile inputs must stay bounded and never crash, and the
// UI Automation providers must only ever run on the window's thread.
//   * a tiny PNG whose IHDR claims a huge image must not commit memory proportional to the claim
//     (portable decoder and decodeImage/WIC)
//   * Skin::validate on a very long include chain and Skin::build on deep nesting x includes must
//     not overflow the stack
//   * a theme cannot be called "current" (it would be unreachable behind theme://current/)
//   * header probes stay defined for hostile BMP sizes
// The memory case runs first in this executable so the process peak reflects only it.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sq/platform.h"
#include "sq/platform.hpp"
#include "sq/ui/image.hpp"
#include "sq/ui/remote_pages.hpp"
#include "sq/ui/skin.hpp"
#include "sq/ui/theme.hpp"
#include "sq/ui/widgets.hpp"
#include "sqtest.hpp"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <ole2.h>
#include <uiautomation.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#endif

using namespace sq::ui;

namespace {

void put32(std::string& s, uint32_t v) {
  for (int i = 3; i >= 0; --i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void chunk(std::string& out, const char* type, const std::string& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::string td(type, 4);
  td += data;
  out += td;
  put32(out, sq_crc32(0, td.data(), td.size()));
}
// Valid PNG framing; IHDR claims w x h, the IDAT is a complete zlib stream of `idatRaw` (stored).
std::string claimPng(uint32_t w, uint32_t h, int depth, int ctype, const std::string& idatZlib) {
  std::string out("\x89PNG\r\n\x1a\n", 8), ihdr;
  put32(ihdr, w);
  put32(ihdr, h);
  ihdr.push_back(static_cast<char>(depth));
  ihdr.push_back(static_cast<char>(ctype));
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", idatZlib);
  chunk(out, "IEND", "");
  return out;
}
// zlib stream holding one stored block of `n` zero bytes (n < 65536).
std::string zlibZeros(uint16_t n) {
  std::string z("\x78\x01", 2);
  z.push_back(1);  // BFINAL, stored
  z.push_back(static_cast<char>(n & 0xFF));
  z.push_back(static_cast<char>(n >> 8));
  z.push_back(static_cast<char>(~n & 0xFF));
  z.push_back(static_cast<char>((~n >> 8) & 0xFF));
  z.append(n, '\0');
  uint32_t a = 1, b = 0;
  for (uint16_t i = 0; i < n; ++i) b = (b + a) % 65521;  // adler32 of zeros
  put32(z, (b << 16) | a);
  return z;
}

#ifdef _WIN32
size_t peakCommit() {
  PROCESS_MEMORY_COUNTERS pmc{};
  return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.PeakPagefileUsage : 0;
}
size_t currentCommit() {
  PROCESS_MEMORY_COUNTERS pmc{};
  return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.PagefileUsage : 0;
}
#endif

}  // namespace

SQ_TEST("hardening: a tiny PNG claiming a huge image does not commit memory for the claim") {
#ifndef _WIN32
  SQ_SKIP("peak commit is measured with the Windows process counters");
#else
  // 16384 x 4096 RGBA16 = 512 MB of scanlines and 256 MB of pixels, from ~120 bytes of file.
  const std::string tiny = claimPng(16384, 4096, 16, 6, zlibZeros(64));
  REQUIRE(tiny.size() < 200);
  const size_t base = currentCommit();
  std::string err;
  CHECK(decodeImagePortable(tiny.data(), tiny.size(), 1.0f, &err) == nullptr);
  CHECK(!err.empty());
  CHECK(decodeImage(tiny.data(), tiny.size(), 1.0f, &err) == nullptr);
  const size_t grew = peakCommit() > base ? peakCommit() - base : 0;
  std::printf("  peak commit grew by %zu KB\n", grew / 1024);
  CHECK(grew < 32u * 1024 * 1024);
  // An honest image of the same claim still decodes once its data is there (bounded by size).
  const std::string honest = claimPng(16, 16, 8, 6, zlibZeros(16 * (16 * 4 + 1)));
  ImagePtr ok = decodeImage(honest.data(), honest.size(), 1.0f, &err);
  REQUIRE(ok);
  CHECK_EQ(ok->width(), 16);
  CHECK(decodeImagePortable(honest.data(), honest.size(), 1.0f, &err) != nullptr);
#endif
}

SQ_TEST("hardening: validating a 20000-form include chain does not overflow the stack") {
  std::string xml = "<Skin><Forms>";
  const int n = 20000;
  for (int i = 0; i < n; ++i)
    xml += "<Form name=\"F" + std::to_string(i) + "\">" + (i + 1 < n ? "<Include form=\"F" + std::to_string(i + 1) + "\"/>" : "") + "</Form>";
  xml += "</Forms></Skin>";
  std::vector<SkinIssue> issues;
  Skin::validate(xml, issues, SkinValidateOptions{});
  bool depthReported = false;
  for (const auto& i : issues)
    if (i.message.find("deeper than 16") != std::string::npos) depthReported = true;
  CHECK(depthReported);  // Skin::build would refuse this chain; the validator says so up front
  // A cycle at the end of a long chain is still found.
  std::string cyc = "<Skin><Forms>";
  for (int i = 0; i < n; ++i)
    cyc += "<Form name=\"F" + std::to_string(i) + "\"><Include form=\"F" + std::to_string(i + 1 < n ? i + 1 : n - 2) + "\"/></Form>";
  cyc += "</Forms></Skin>";
  issues.clear();
  CHECK(!Skin::validate(cyc, issues, SkinValidateOptions{}));
  bool cycle = false;
  for (const auto& i : issues)
    if (i.error && i.message.find("unconditional include cycle") != std::string::npos) cycle = true;
  CHECK(cycle);
}

SQ_TEST("hardening: deep nesting through 16 includes builds or fails cleanly (no stack overflow)") {
  // 16 forms, each 250 nested Views deep with the next include at the bottom: ~4000 levels.
  std::string xml = "<Skin><Forms>";
  for (int f = 0; f < 16; ++f) {
    xml += "<Form name=\"F" + std::to_string(f) + "\">";
    for (int d = 0; d < 250; ++d) xml += "<View>";
    if (f + 1 < 16) xml += "<Include form=\"F" + std::to_string(f + 1) + "\"/>";
    else xml += "<Knob remote=\"k\" target=\"t\"/>";
    for (int d = 0; d < 250; ++d) xml += "</View>";
    xml += "</Form>";
  }
  xml += "</Forms></Skin>";
  std::string err;
  auto skin = Skin::parse(xml, &err);
  REQUIRE(skin);
  auto w = skin->build("F0", {}, &err);
  if (!w) {
    std::printf("  refused: %s\n", err.c_str());
    CHECK(err.find("nest") != std::string::npos);
  }
  std::vector<RemotePageSpec> pages;
  buildRemotePages(*skin, {}, "object://x", pages, &err);  // must return, either way
  std::vector<SkinIssue> issues;
  Skin::validate(xml, issues, SkinValidateOptions{});
}

SQ_TEST("hardening: a theme named 'current' is refused") {
  auto themes = ThemeResolver::create();
  std::vector<ThemePackEntry> e = {{"current/theme.xml", R"(<Theme name="current" format="1"/>)"}};
  std::string blob, err;
  REQUIRE(writeThemePack(e, blob, &err));
  auto pack = ThemePack::fromMemory(blob.data(), blob.size(), true, &err);
  REQUIRE(pack);
  CHECK(!themes->addPack(pack, &err));
  CHECK(err.find("current") != std::string::npos);
  CHECK(!themes->hasTheme("current"));
}

SQ_TEST("hardening: header probes on hostile BMP sizes") {
  std::string bmp(32, '\0');
  bmp[0] = 'B';
  bmp[1] = 'M';
  for (int i = 18; i < 26; ++i) bmp[i] = static_cast<char>(i == 21 || i == 25 ? 0x80 : 0);  // INT32_MIN x INT32_MIN
  uint32_t w = 1, h = 1;
  CHECK(imageSize(bmp.data(), bmp.size(), &w, &h));
  CHECK_EQ(w, 0x80000000u);
  CHECK_EQ(h, 0x80000000u);
}

#ifdef _WIN32
namespace {
// A view whose accessible name records the calling thread.
class ProbeView final : public View {
 public:
  explicit ProbeView(std::mutex& mu, std::set<DWORD>& threads) : mu_(mu), threads_(threads) {}
  std::string accessibleName() const override {
    std::lock_guard<std::mutex> l(mu_);
    threads_.insert(GetCurrentThreadId());
    return "probe";
  }

 private:
  std::mutex& mu_;
  std::set<DWORD>& threads_;
};
}  // namespace

SQ_TEST("hardening uia: providers are only called on the window's thread") {
  std::mutex mu;
  std::set<DWORD> threads;
  WidgetTree tree;
  auto root = std::make_unique<View>();
  root->name = "root";
  for (int i = 0; i < 4; ++i) root->addChild(std::make_unique<ProbeView>(mu, threads));
  tree.setRoot(std::move(root));
  tree.layout(200, 100);
  auto window = createWindow(WindowDesc{"uia thread test", 320, 200, false, Backend::Auto});
  if (!window) SQ_SKIP("no window backend");
  window->setAccessibilityRoot(tree.root());
  auto hwnd = static_cast<HWND>(window->nativeHandle());
  const DWORD uiThread = GetCurrentThreadId();
  std::atomic<bool> done{false};
  std::string failure;
  int seen = 0;
  std::thread client([&] {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                reinterpret_cast<void**>(&uia)))) {
      failure = "skip: CUIAutomation unavailable";
    } else {
      IUIAutomationElement* el = nullptr;
      IUIAutomationCondition* all = nullptr;
      IUIAutomationElementArray* found = nullptr;
      if (SUCCEEDED(uia->ElementFromHandle(hwnd, &el)) && el && SUCCEEDED(uia->CreateTrueCondition(&all)) &&
          SUCCEEDED(el->FindAll(TreeScope_Descendants, all, &found)) && found) {
        int n = 0;
        found->get_Length(&n);
        for (int i = 0; i < n; ++i) {
          IUIAutomationElement* e = nullptr;
          if (SUCCEEDED(found->GetElement(i, &e)) && e) {
            BSTR b = nullptr;
            if (SUCCEEDED(e->get_CurrentName(&b)) && b && std::wstring(b) == L"probe") ++seen;
            SysFreeString(b);
            e->Release();
          }
        }
      } else {
        failure = "UIA tree walk failed";
      }
      if (found) found->Release();
      if (all) all->Release();
      if (el) el->Release();
      uia->Release();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    done = true;
  });
  while (!done) {
    window->pumpEvents(false);
    Sleep(2);
  }
  client.join();
  window->setAccessibilityRoot(nullptr);
  if (failure.rfind("skip:", 0) == 0) SQ_SKIP(failure);
  CHECK_EQ(failure, std::string());
  CHECK_EQ(seen, 4);
  std::lock_guard<std::mutex> l(mu);
  REQUIRE(!threads.empty());
  for (DWORD t : threads) {
    if (t != uiThread) std::printf("  provider called on thread %lu (UI thread %lu)\n", t, uiThread);
    CHECK_EQ(t, uiThread);
  }
}
#else
SQ_TEST("hardening uia: UI Automation is Windows-only") { SQ_SKIP("UI Automation is Windows-only"); }
#endif
