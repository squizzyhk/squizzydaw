// engine/audio - insert spec grammar (builtin.hpp): the "s=<base64>" plugin-state token.
//   "<format>|<id>|<path>[|<i>=<v>]...[|s=<base64>]"   s= may appear anywhere after the three fixed
//   fields when parsed, formatSpec always writes it LAST, specStructure drops params and state,
//   specSetState adds / replaces / removes it, specSetParam leaves it alone.
#include <string>
#include <vector>

#include "sq/engine/audio/builtin.hpp"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {
const std::string kLegacy = "vst3|abc|C:/x.vst3|3=0.5|7=1";            // no state token
const std::string kWithState = "vst3|abc|C:/x.vst3|3=0.5|s=AAEC|7=1";  // state in the middle
}  // namespace

SQ_TEST("spec_state: parseSpec reads s= anywhere after the fixed fields, other keys stay ignored") {
  SpecParts p;
  REQUIRE(parseSpec(kWithState, &p));
  CHECK_EQ(p.format, std::string("vst3"));
  CHECK_EQ(p.id, std::string("abc"));
  CHECK_EQ(p.path, std::string("C:/x.vst3"));
  REQUIRE_EQ(p.params.size(), size_t(2));
  CHECK_EQ(p.params[0].first, 3u);
  CHECK_EQ(p.params[0].second, 0.5);
  CHECK_EQ(p.params[1].first, 7u);
  CHECK_EQ(p.params[1].second, 1.0);
  CHECK_EQ(p.state, std::string("AAEC"));

  // Standard base64 padding ('=') is part of the value: the token splits on its FIRST '=' only.
  SpecParts padded;
  REQUIRE(parseSpec("vst3|abc|C:/x.vst3|s=QQ==|3=1", &padded));
  CHECK_EQ(padded.state, std::string("QQ=="));
  REQUIRE_EQ(padded.params.size(), size_t(1));
  CHECK_EQ(padded.params[0].first, 3u);

  // Last token, built-in device, unknown non-numeric keys and junk values are still ignored.
  SpecParts last;
  REQUIRE(parseSpec("builtin|eq||0=-3|bogus|x=1|7=x|s=Zm9v", &last));
  CHECK_EQ(last.state, std::string("Zm9v"));
  REQUIRE_EQ(last.params.size(), size_t(1));
  CHECK_EQ(last.params[0].first, 0u);

  // A legacy spec without s= has an empty state.
  SpecParts legacy;
  REQUIRE(parseSpec(kLegacy, &legacy));
  CHECK(legacy.state.empty());
  REQUIRE_EQ(legacy.params.size(), size_t(2));
  SpecParts bare;
  REQUIRE(parseSpec("builtin|synth|", &bare));
  CHECK(bare.state.empty());
  CHECK(bare.params.empty());
}

SQ_TEST("spec_state: formatSpec writes s= as the last token and round trips") {
  SpecParts p;
  p.format = "vst3";
  p.id = "abc";
  p.path = "C:/x.vst3";
  p.params = {{3, 0.5}, {7, 1.0}};
  p.state = "AAEC";
  const std::string s = formatSpec(p);
  CHECK_EQ(s, std::string("vst3|abc|C:/x.vst3|3=0.5|7=1|s=AAEC"));
  SpecParts back;
  REQUIRE(parseSpec(s, &back));
  CHECK_EQ(back.state, p.state);
  CHECK(back.params == p.params);
  CHECK_EQ(back.path, p.path);

  // Parsing a spec with s= in the middle and formatting it again moves the token to the end.
  SpecParts mid;
  REQUIRE(parseSpec(kWithState, &mid));
  CHECK_EQ(formatSpec(mid), std::string("vst3|abc|C:/x.vst3|3=0.5|7=1|s=AAEC"));

  // An empty state writes no token at all (legacy form, byte for byte).
  p.state.clear();
  CHECK_EQ(formatSpec(p), kLegacy);
  SpecParts none;
  none.format = "builtin";
  none.id = "synth";
  CHECK_EQ(formatSpec(none), std::string(kDefaultInstrument));

  // State without parameters: directly after the path.
  SpecParts only;
  only.format = "clap";
  only.id = "com.vendor.fx";
  only.path = "/opt/fx.clap";
  only.state = "QQ==";
  CHECK_EQ(formatSpec(only), std::string("clap|com.vendor.fx|/opt/fx.clap|s=QQ=="));
}

SQ_TEST("spec_state: specStructure drops parameters and state") {
  CHECK_EQ(specStructure(kWithState), std::string("vst3|abc|C:/x.vst3"));
  CHECK_EQ(specStructure(kLegacy), std::string("vst3|abc|C:/x.vst3"));
  CHECK_EQ(specStructure("vst3|abc|C:/x.vst3|s=AAEC"), std::string("vst3|abc|C:/x.vst3"));
  // Two specs that differ only in state (or values) share one structure key: no recompile.
  CHECK_EQ(specStructure("vst3|abc|C:/x.vst3|3=0.5|s=AAEC"), specStructure("vst3|abc|C:/x.vst3|3=0.9|s=QUJD"));
  // Different plugin: different structure.
  CHECK(specStructure("vst3|abd|C:/x.vst3|s=AAEC") != specStructure(kWithState));
}

SQ_TEST("spec_state: specSetState adds, replaces and removes the state token") {
  // add to a legacy spec: appended last
  CHECK_EQ(specSetState(kLegacy, "QUJD"), std::string("vst3|abc|C:/x.vst3|3=0.5|7=1|s=QUJD"));
  // replace: one token, still last, parameters untouched
  CHECK_EQ(specSetState(kWithState, "Zm9v"), std::string("vst3|abc|C:/x.vst3|3=0.5|7=1|s=Zm9v"));
  CHECK_EQ(specSetState("vst3|abc|C:/x.vst3|s=AAEC", "QQ=="), std::string("vst3|abc|C:/x.vst3|s=QQ=="));
  // remove: empty base64 drops the token
  CHECK_EQ(specSetState(kWithState, ""), kLegacy);
  CHECK_EQ(specSetState("vst3|abc|C:/x.vst3|s=AAEC", ""), std::string("vst3|abc|C:/x.vst3"));
  // removing from a spec without state is a no-op
  CHECK_EQ(specSetState(kLegacy, ""), kLegacy);
  // no parameters: state right after the path
  CHECK_EQ(specSetState("builtin|eq|", "QUJD"), std::string("builtin|eq||s=QUJD"));
  // the result parses back to the state that was set
  SpecParts p;
  REQUIRE(parseSpec(specSetState(kLegacy, "QUJD"), &p));
  CHECK_EQ(p.state, std::string("QUJD"));
  REQUIRE_EQ(p.params.size(), size_t(2));
}

SQ_TEST("spec_state: specSetParam preserves the state token (and keeps it last)") {
  CHECK_EQ(specSetParam(kWithState, 3, 0.25), std::string("vst3|abc|C:/x.vst3|3=0.25|7=1|s=AAEC"));  // replace
  CHECK_EQ(specSetParam(kWithState, 9, 2.0), std::string("vst3|abc|C:/x.vst3|3=0.5|7=1|9=2|s=AAEC"));  // add
  CHECK_EQ(specSetParam("vst3|abc|C:/x.vst3|s=AAEC", 0, 1.0), std::string("vst3|abc|C:/x.vst3|0=1|s=AAEC"));
  // legacy specs stay legacy
  CHECK_EQ(specSetParam(kLegacy, 3, 0.25), std::string("vst3|abc|C:/x.vst3|3=0.25|7=1"));
  // state then param then state again: each helper leaves the other's work alone
  std::string s = specSetState(kLegacy, "AAEC");
  s = specSetParam(s, 7, 2.0);
  s = specSetState(s, "QUJD");
  CHECK_EQ(s, std::string("vst3|abc|C:/x.vst3|3=0.5|7=2|s=QUJD"));
  SpecParts p;
  REQUIRE(parseSpec(s, &p));
  CHECK_EQ(p.state, std::string("QUJD"));
  REQUIRE_EQ(p.params.size(), size_t(2));
  CHECK_EQ(p.params[1].second, 2.0);
}
