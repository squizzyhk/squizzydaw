#include <string>

#include "sq/platform.hpp"
#include "sqtest.hpp"

SQ_TEST("xml: parse elements, attributes, entities, CDATA, comments") {
  const char* src =
      "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE skin [ <!ELEMENT x ANY> ]>\n"
      "<!-- header comment -->\n"
      "<Skin version='2' name=\"a &amp; b\">\n"
      "  <Forms>\n"
      "    <View id=\"main\" w=\"100\"/>\n"
      "    <Knob id=\"gain\">&lt;text&gt; &#65;&#x42; <![CDATA[<raw & stuff>]]></Knob>\n"
      "    <!-- inner comment -->\n"
      "    <?pi ignored?>\n"
      "  </Forms>\n"
      "  <Utf8>\xC3\xA9\xE2\x82\xAC</Utf8>\n"
      "</Skin>\n";
  std::string err;
  auto doc = sq::xmlParse(src, &err);
  REQUIRE(doc);
  sq::XmlNode root = sq::xmlRoot(doc);
  CHECK_EQ(std::string(root.name()), std::string("Skin"));
  CHECK_EQ(root.attr("version"), std::string("2"));
  CHECK_EQ(root.attr("name"), std::string("a & b"));
  CHECK_EQ(root.attr("missing", "def"), std::string("def"));
  CHECK_EQ(std::string(root.text()), std::string(""));  // whitespace-only text dropped
  sq::XmlNode forms = root.child("Forms");
  REQUIRE(forms);
  CHECK_EQ(sq_xml_child_count(forms.raw()), 2u);
  sq::XmlNode knob = forms.child("Knob");
  REQUIRE(knob);
  CHECK_EQ(std::string(knob.text()), std::string("<text> AB <raw & stuff>"));
  CHECK(knob.parent().raw() == forms.raw());
  int views = 0;
  for (sq::XmlNode c : forms.children()) {
    (void)c;
    ++views;
  }
  CHECK_EQ(views, 2);
  CHECK_EQ(std::string(root.child("Utf8").text()), std::string("\xC3\xA9\xE2\x82\xAC"));
}

SQ_TEST("xml: build, write, re-parse roundtrip") {
  auto doc = sq::xmlCreate("Archive");
  sq::XmlNode root = sq::xmlRoot(doc);
  root.setAttr("format", "1");
  sq::XmlNode part = root.append("Part");
  part.setAttr("name", "quote\" amp& lt< nl\n");
  part.setText("body <&> text");
  root.append("Empty");
  std::string pretty = sq::xmlWrite(doc, true);
  std::string compact = sq::xmlWrite(doc, false);
  CHECK(pretty.find("<Empty/>") != std::string::npos);
  CHECK(compact.find('\n') == std::string::npos);
  for (const std::string& s : {pretty, compact}) {
    auto d2 = sq::xmlParse(s);
    REQUIRE(d2);
    sq::XmlNode p2 = sq::xmlRoot(d2).child("Part");
    CHECK_EQ(p2.attr("name"), std::string("quote\" amp& lt< nl\n"));
    CHECK_EQ(std::string(p2.text()), std::string("body <&> text"));
    CHECK_EQ(sq::xmlWrite(d2, true), pretty);
  }
  // Removal + attribute API
  sq_xml_node* r = sq_xml_root(doc.get());
  CHECK_OK(sq_xml_remove_child(r, sq_xml_first_child(r, "Empty")));
  CHECK_EQ(sq_xml_child_count(r), 1u);
  CHECK_OK(sq_xml_remove_attr(r, "format"));
  CHECK_EQ(sq_xml_remove_attr(r, "format"), SQ_E_NOTFOUND);
  CHECK(sq_xml_append_child(r, "bad name") == nullptr);
}

SQ_TEST("xml: malformed documents are rejected with a line number") {
  const char* bad[] = {"", "<a>", "<a></b>", "<a b=1/>", "<a b='1' b='2'/>", "<a>&bogus;</a>", "<a/><b/>", "text", "<a><!-- x</a>",
                       "<a b='<'/>", "<a>&#0;</a>"};
  for (const char* s : bad) {
    std::string err;
    auto d = sq::xmlParse(s, &err);
    CHECK(!d);
    CHECK(err.rfind("line ", 0) == 0);
  }
  std::string deep;
  for (int i = 0; i < 400; ++i) deep += "<a>";
  CHECK(!sq::xmlParse(deep));
}

SQ_TEST("json: parse all value kinds") {
  const char* src = R"({"n": -12.5e1, "i": 42, "t": true, "f": false, "z": null,
    "s": "q\"\\\/\b\f\n\r\t\u00e9\ud83c\udfb5", "a": [1, [2, {}], "x"], "o": {"k": "v"}})";
  std::string err;
  auto v = sq::jsonParse(src, &err);
  REQUIRE(v);
  CHECK_EQ(sq_json_type_of(v.get()), SQ_JSON_OBJECT);
  CHECK_EQ(sq_json_size(v.get()), size_t(8));
  CHECK_EQ(sq_json_get_number(sq_json_get(v.get(), "n")), -125.0);
  CHECK_EQ(sq_json_get_int(sq_json_get(v.get(), "i")), 42);
  CHECK_EQ(sq_json_get_bool(sq_json_get(v.get(), "t")), SQ_TRUE);
  CHECK_EQ(sq_json_type_of(sq_json_get(v.get(), "z")), SQ_JSON_NULL);
  size_t len = 0;
  const char* s = sq_json_get_string(sq_json_get(v.get(), "s"), &len);
  CHECK_EQ(std::string(s, len), std::string("q\"\\/\b\f\n\r\t\xC3\xA9\xF0\x9F\x8E\xB5"));
  sq_json* a = sq_json_get(v.get(), "a");
  CHECK_EQ(sq_json_size(a), size_t(3));
  CHECK_EQ(sq_json_type_of(sq_json_at(sq_json_at(a, 1), 1)), SQ_JSON_OBJECT);
  CHECK_EQ(std::string(sq_json_key_at(v.get(), 7)), std::string("o"));
  CHECK(sq_json_get(v.get(), "missing") == nullptr);
}

SQ_TEST("json: build, write, re-parse roundtrip") {
  sq::JsonPtr root(sq_json_new_object());
  sq_json_set(root.get(), "name", sq_json_new_string("snap\"shots\n", static_cast<size_t>(-1)));
  sq_json_set(root.get(), "pi", sq_json_new_number(3.141592653589793));
  sq_json_set(root.get(), "big", sq_json_new_number(1e300));
  sq_json* arr = sq_json_new_array();
  for (int i = 0; i < 3; ++i) sq_json_append(arr, sq_json_new_number(i));
  sq_json_set(root.get(), "list", arr);
  sq_json_set(root.get(), "pi", sq_json_new_number(3.0));  // replace keeps position
  CHECK_EQ(sq_json_append(arr, arr), SQ_E_INVALIDARG);
  std::string compact = sq::jsonWrite(root.get());
  CHECK_EQ(compact, std::string(R"({"name":"snap\"shots\n","pi":3,"big":1e+300,"list":[0,1,2]})"));
  std::string pretty = sq::jsonWrite(root.get(), true);
  auto back = sq::jsonParse(pretty);
  REQUIRE(back);
  CHECK_EQ(sq::jsonWrite(back.get()), compact);
  CHECK_OK(sq_json_remove(root.get(), "big"));
  CHECK_EQ(sq_json_size(root.get()), size_t(3));
  sq_json_free(arr);  // owned by root: must be a no-op
  CHECK_EQ(sq_json_size(sq_json_get(root.get(), "list")), size_t(3));
}

SQ_TEST("json: malformed input rejected") {
  const char* bad[] = {"", "{", "[1,]", "{\"a\" 1}", "01", "1.", "-", "\"\\x\"", "\"unterminated", "tru", "[1] 2", "{\"a\":1,}",
                       "\"tab\there\""};
  for (const char* s : bad) {
    std::string err;
    CHECK(!sq::jsonParse(s, &err));
    CHECK(!err.empty());
  }
  std::string deep(600, '[');
  CHECK(!sq::jsonParse(deep));
}
