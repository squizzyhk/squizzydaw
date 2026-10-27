// ui/ core: layout, skin XML preprocessor + widget build, raster backend, PNG round trip.
#include <cmath>

#include "sq/platform.h"
#include "sq/ui/skin.hpp"
#include "sq/ui/widgets.hpp"
#include "sqtest.hpp"

using namespace sq::ui;

SQ_TEST("ui: flex row distributes grow and gap") {
  FlexBox root;
  root.style.direction = FlexDirection::Row;
  root.style.gap = 10;
  LayoutStyle a;
  a.width = Dim::pt(100);
  LayoutStyle b;
  b.grow = 1;
  root.add(a);
  root.add(b);
  computeLayout(root, 410, 50);
  CHECK_EQ(root.child(0)->box.w, 100.0f);
  CHECK_EQ(root.child(1)->box.x, 110.0f);
  CHECK_EQ(root.child(1)->box.w, 300.0f);
  CHECK_EQ(root.child(1)->box.h, 50.0f);  // stretch
}

SQ_TEST("ui: skin expand and colors") {
  SkinDefines d{{"a", "x"}, {"track.name", "Bass"}};
  CHECK_EQ(Skin::expand("$(a)-$a-$(track.name)-$$-$(missing)", d), std::string("x-x-Bass-$-"));
  Color c;
  CHECK(Skin::parseColor("#FF8000", c));
  CHECK_EQ(c.toRgba(), 0xFF8000FFu);
  CHECK(Skin::parseColor("#0f0", c));
  CHECK_EQ(c.toRgba(), 0x00FF00FFu);
  CHECK(!Skin::parseColor("red", c));
  CHECK(!Skin::parseColor("#12345", c));
}

static const char* kSkin = R"SKIN(<Skin name="t" format="1">
  <Defines><define name="accent" value="#4FB3A9"/></Defines>
  <Forms>
    <Form name="Main" direction="column" background="#202020">
      <View name="bar" direction="row" height="30" gap="4" padding="2">
        <Button name="play" text="Play" command="Transport.Play"/>
        <if defined="withRecord"><Button name="rec" text="Rec" command="Transport.Record"/>
          <else><Label name="norec" text="no record"/></else></if>
      </View>
      <View name="strips" direction="row" grow="1">
        <Include form="Strip" track="A" kind="audio"/>
        <Include form="Strip" track="B" kind="instrument"/>
      </View>
    </Form>
    <Form name="Strip" width="80" direction="column">
      <define name="label" value="Track $(track)"/>
      <Label name="name-$(track)" text="$(label)"/>
      <Fader name="vol-$(track)" target="object://doc/active/node/$(track)" property="volume" min="-60" max="6" value="0" grow="1"/>
      <switch value="$(kind)">
        <case value="audio"><Button name="arm-$(track)" toggles="1" text="R" accent="$(accent)"/></case>
        <default><Knob name="cut-$(track)"/></default>
      </switch>
    </Form>
    <Form name="Loop"><Include form="Loop"/></Form>
  </Forms>
</Skin>)SKIN";

SQ_TEST("ui: skin builds forms with defines, if/else, switch and include") {
  std::string err;
  auto skin = Skin::parse(kSkin, &err);
  REQUIRE(skin);
  CHECK(skin->hasForm("Main"));
  CHECK(skin->hasForm("Strip"));
  auto root = skin->build("Main", {}, &err);
  REQUIRE(root);
  CHECK(root->findByName("play") != nullptr);
  CHECK_EQ(root->findByName("play")->command, std::string("Transport.Play"));
  CHECK(root->findByName("rec") == nullptr);
  CHECK(root->findByName("norec") != nullptr);
  auto* lbl = root->findByName("name-A");
  REQUIRE(lbl);
  CHECK_EQ(lbl->text, std::string("Track A"));
  auto* vol = root->findByName("vol-B");
  REQUIRE(vol);
  CHECK_EQ(vol->target.url, std::string("object://doc/active/node/B"));
  CHECK_EQ(vol->target.property, std::string("volume"));
  CHECK_EQ(vol->minValue, -60.0);
  CHECK(root->findByName("arm-A") != nullptr);
  CHECK(root->findByName("arm-B") == nullptr);
  CHECK(root->findByName("cut-B") != nullptr);

  auto withRec = skin->build("Main", {{"withRecord", "1"}}, &err);
  REQUIRE(withRec);
  CHECK(withRec->findByName("rec") != nullptr);
  CHECK(withRec->findByName("norec") == nullptr);

  std::string e2;
  CHECK(skin->build("Loop", {}, &e2) == nullptr);
  CHECK(e2.find("16") != std::string::npos);
  std::string e3;
  CHECK(skin->build("Nope", {}, &e3) == nullptr);
  CHECK(!e3.empty());
}

SQ_TEST("ui: skin rejects unknown elements, attributes and bad values") {
  std::string err;
  auto s = Skin::parse("<Skin><Forms><Form name=\"A\"><Bogus/></Form><Form name=\"B\"><View width=\"x\"/></Form>"
                       "<Form name=\"C\"><View frobnicate=\"1\"/></Form></Forms></Skin>",
                       &err);
  REQUIRE(s);
  CHECK(s->build("A", {}, &err) == nullptr);
  err.clear();
  CHECK(s->build("B", {}, &err) == nullptr);
  err.clear();
  CHECK(s->build("C", {}, &err) == nullptr);
  CHECK(Skin::parse("<NotSkin/>", &err) == nullptr);
  CHECK(Skin::parse("<Skin><Forms><Form name=\"A\"/><Form name=\"A\"/></Forms></Skin>", &err) == nullptr);
}

SQ_TEST("ui: custom widgets, tree layout, hit test and command dispatch") {
  std::string err;
  auto skin = Skin::parse(kSkin, &err);
  REQUIRE(skin);
  int customs = 0;
  skin->registerWidget("Knob", [&] {
    ++customs;
    return std::make_unique<Knob>();
  });
  WidgetTree tree;
  tree.setRoot(skin->build("Main", {}, &err));
  REQUIRE(tree.root());
  CHECK_EQ(customs, 1);  // only strip B has a knob
  tree.setScale(2.0f);
  tree.layout(800, 600);
  Widget* play = tree.findByName("play");
  REQUIRE(play);
  CHECK(play->frame().w > 0);
  std::string fired;
  tree.onCommand = [&](Widget& w) { fired = w.command; };
  float cx = (play->frame().x + play->frame().w / 2) * 2, cy = (play->frame().y + play->frame().h / 2) * 2;
  MouseEvent down{MouseAction::Down, MouseButton::Left, cx, cy, 0, 0};
  MouseEvent up{MouseAction::Up, MouseButton::Left, cx, cy, 0, 0};
  tree.dispatchMouse(down);
  tree.dispatchMouse(up);
  CHECK_EQ(fired, std::string("Transport.Play"));
}

SQ_TEST("ui: raster backend is deterministic and draws") {
  auto paint = [](IRenderer& r) {
    r.clear(Color::rgb(0x101010));
    r.fillRect(Rect{10, 10, 20, 20}, Color::rgb(0xFF0000));
    r.drawLine(Point{0, 50}, Point{63, 50}, Color::rgb(0x00FF00), 1);
    r.drawLine(Point{0, 0}, Point{63, 63}, Color::rgb(0x0000FF), 1);
    r.drawArc(Point{32, 32}, 12, 0, 270, Color::rgb(0xFFFFFF), 1);
    r.drawText("abc", Rect{0, 0, 64, 16}, Color::rgb(0xFFFF00), 10);
  };
  PixelBuffer a, b;
  REQUIRE(renderOffscreen(Backend::Raster, 64, 64, 1.0f, paint, a));
  REQUIRE(renderOffscreen(Backend::Raster, 64, 64, 1.0f, paint, b));
  CHECK_EQ(a.checksum(), b.checksum());
  CHECK_EQ(a.pixel(25, 12), 0xFF0000FFu);
  CHECK_EQ(a.pixel(40, 50), 0x00FF00FFu);
  CHECK_EQ(a.pixel(60, 5), 0x101010FFu);
  CHECK(!renderOffscreen(Backend::Raster, 0, 10, 1.0f, paint, a));
}

SQ_TEST("ui: PNG encode / probe / decode round trip") {
  PixelBuffer pb;
  REQUIRE(renderOffscreen(Backend::Raster, 300, 250, 1.0f, [](IRenderer& r) {
    r.clear(Color::rgb(0x336699));
    r.fillRect(Rect{5, 5, 10, 10}, Color::rgb(0xFF0000, 0.5f));
  }, pb));
  std::string png;
  REQUIRE(encodePng(pb, png));
  uint32_t w = 0, h = 0;
  CHECK(pngSize(png.data(), png.size(), &w, &h));
  CHECK_EQ(w, 300u);
  CHECK_EQ(h, 250u);
  std::string err;
  ImagePtr img = decodeImage(png.data(), png.size(), 2.0f, &err);
  REQUIRE(img);
  CHECK_EQ(img->width(), 300);
  CHECK_EQ(img->density(), 2.0f);
  const uint8_t* p = img->pixels() + (100 * img->stride()) + 100 * 4;  // BGRA
  CHECK_EQ(int{p[0]}, 0x99);
  CHECK_EQ(int{p[2]}, 0x33);
  CHECK(decodeImage("nope", 4, 1.0f, &err) == nullptr);
}

// Regression ("billion laughs" through skin XML): only the include DEPTH was bounded, so forms
// including the next form N times multiplied to N^L widgets (10 levels x 4 = 1M widgets, GBs),
// and chained defines doubled their text. Both must now fail fast with an error.
namespace {
std::string fanoutSkin(int levels, int fanout) {
  std::string x = "<Skin name=\"bomb\"><Forms>";
  for (int l = 0; l < levels; ++l) {
    x += "<Form name=\"F" + std::to_string(l) + "\">";
    if (l + 1 < levels)
      for (int k = 0; k < fanout; ++k) x += "<Include form=\"F" + std::to_string(l + 1) + "\"/>";
    else
      x += "<Label text=\"x\"/>";
    x += "</Form>";
  }
  return x + "</Forms></Skin>";
}
}  // namespace

SQ_TEST("skin: exponential include fan-out is stopped by the widget budget") {
  std::string err;
  auto skin = Skin::parse(fanoutSkin(12, 4), &err);  // 4^11 ~ 4M forms if unbounded
  REQUIRE(skin);
  const uint64_t t0 = sq_time_monotonic_ns();
  auto w = skin->build("F0", {}, &err);
  const double secs = static_cast<double>(sq_time_monotonic_ns() - t0) / 1e9;
  CHECK(!w);
  CHECK(err.find("widgets") != std::string::npos);
  CHECK(secs < 20.0);
  // A legitimately large but bounded skin still builds.
  auto ok = Skin::parse(fanoutSkin(5, 4), &err);  // 341 forms
  REQUIRE(ok);
  err.clear();
  CHECK(ok->build("F0", {}, &err) != nullptr);
}

SQ_TEST("skin: chained defines doubling their text are stopped by the text budget") {
  std::string x = "<Skin><Forms><Form name=\"main\"><define name=\"d0\" value=\"0123456789abcdef\"/>";
  for (int i = 1; i <= 40; ++i)
    x += "<define name=\"d" + std::to_string(i) + "\" value=\"$d" + std::to_string(i - 1) + "$d" + std::to_string(i - 1) + "\"/>";
  x += "<Label text=\"$d40\"/></Form></Forms></Skin>";  // 16 * 2^40 bytes if unbounded
  std::string err;
  auto skin = Skin::parse(x, &err);
  REQUIRE(skin);
  auto w = skin->build("main", {}, &err);
  CHECK(!w);
  CHECK(err.find("expands beyond") != std::string::npos);
}

SQ_TEST("skin: the include depth limit still applies and a failed build can be retried") {
  std::string err;
  auto skin = Skin::parse(fanoutSkin(20, 1), &err);
  REQUIRE(skin);
  CHECK(!skin->build("F0", {}, &err));
  CHECK(err.find("deeper than 16") != std::string::npos);
  err.clear();
  CHECK(skin->build("F5", {}, &err) != nullptr);  // 15 levels: fine
}
