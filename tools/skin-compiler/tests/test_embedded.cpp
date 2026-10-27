// The stock theme pack embedded through the .rc sq-skinc generated: this test EXE carries it as
// RCDATA "SQTHEMES" and the resolver serves theme:// from its own resource section.
#include <cstdio>
#include <string>

#include "sq/platform.hpp"
#include "sq/ui/theme.hpp"
#include "sqthemes.h"
#include "sqtest.hpp"

using namespace sq::ui;

SQ_TEST("embedded: the EXE's RCDATA theme pack loads and resolves") {
#ifndef _WIN32
  SQ_SKIP("resource embedding is Windows-only (other platforms load the .sqtheme file)");
#else
  std::string err;
  auto pack = ThemePack::fromResource(SQ_THEME_PACK_RESOURCE, nullptr, &err);
  if (!pack) std::printf("  %s\n", err.c_str());
  REQUIRE(pack);
  CHECK_EQ(pack->bytes().size(), static_cast<size_t>(SQ_THEME_PACK_SIZE));
  CHECK_EQ(pack->count(), static_cast<size_t>(SQ_THEME_PACK_FILES));
  CHECK_EQ(sq::sha256Hex(pack->bytes()), std::string(SQ_THEME_PACK_SHA256));
  auto file = sq::readFile(SQ_STOCK_PACK);
  REQUIRE(file);
  CHECK(pack->bytes() == *file);

  auto themes = ThemeResolver::create();
  REQUIRE(themes->addPack(pack, &err));
  CHECK_EQ(themes->themes().size(), size_t{3});
  REQUIRE_EQ(themes->setActiveTheme("dark"), SQ_OK);
  sq_theme_resource r{};
  r.struct_size = sizeof r;
  REQUIRE_EQ(themes->resolve("theme://current/knob/cap.png", 2.0f, &r), SQ_OK);
  CHECK_EQ(r.scale, 2.0f);
  CHECK_EQ(r.width, 64u);
  ImagePtr img = themes->image("theme://current/icons/record.png", 1.0f, &err);
  REQUIRE(img);
  CHECK_EQ(img->width(), 16);
  double m = 0;
  CHECK_EQ(themes->getMetric("Strip.Width", &m), SQ_OK);
  CHECK_EQ(m, 84.0);
  CHECK(ThemePack::fromResource("NO_SUCH_RESOURCE", nullptr, &err) == nullptr);
#endif
}
