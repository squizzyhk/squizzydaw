#include <atomic>
#include <string>
#include <vector>

#include "sq/app/settings.hpp"
#include "sqtest.hpp"
#include "test_util.hpp"

using sq::app::Settings;

SQ_TEST("settings: key validation") {
  CHECK(Settings::validKey("Engine"));
  CHECK(Settings::validKey("Engine.Audio.Block_Size-2"));
  CHECK(!Settings::validKey(""));
  CHECK(!Settings::validKey(".Engine"));
  CHECK(!Settings::validKey("Engine."));
  CHECK(!Settings::validKey("Engine..X"));
  CHECK(!Settings::validKey("Engine X"));
  CHECK(!Settings::validKey(nullptr));
  Settings s;
  CHECK_EQ(s.setInt("bad key", 1), SQ_E_INVALIDARG);
}

SQ_TEST("settings: typed get/set and conversions") {
  Settings s;
  CHECK_OK(s.setInt("Engine.BlockSize", 256));
  CHECK_OK(s.setDouble("Engine.Gain", 0.5));
  CHECK_OK(s.setBool("Engine.LowLatency", SQ_TRUE));
  CHECK_OK(s.setString("Devices.Audio.Driver", "WASAPI"));
  CHECK_EQ(s.getInt("Engine.BlockSize", 0), 256);
  CHECK_EQ(s.getDouble("Engine.Gain", 0), 0.5);
  CHECK_EQ(s.getBool("Engine.LowLatency", SQ_FALSE), SQ_TRUE);
  CHECK_EQ(sq::getString(&s, "Devices.Audio.Driver"), std::string("WASAPI"));
  CHECK_EQ(s.getType("Engine.Gain"), SQ_SETTING_DOUBLE);
  CHECK_EQ(s.getType("Nope"), SQ_SETTING_NONE);
  CHECK_EQ(s.getInt("Missing.Key", -7), -7);
  CHECK_EQ(s.getInt("Devices.Audio.Driver", -1), -1);  // non-numeric string -> default
  CHECK_EQ(s.getInt("Engine.Gain", 9), 0);             // double truncates
  CHECK_EQ(s.getDouble("Engine.BlockSize", 0), 256.0);
  CHECK_EQ(s.setInt("Engine.BlockSize", 256), SQ_S_FALSE);  // unchanged

  char small[4];
  uint32_t need = 0;
  CHECK_EQ(s.getString("Devices.Audio.Driver", small, sizeof small, &need), SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(need, 7u);
  CHECK_EQ(std::string(small), std::string("WAS"));
  CHECK_EQ(s.getString("Nope", small, sizeof small, &need), SQ_E_NOTFOUND);
}

SQ_TEST("settings: hierarchy enumerate + remove subtree") {
  Settings s;
  s.setInt("Engine.A", 1);
  s.setInt("Engine.B.C", 2);
  s.setInt("Engine.B.D", 3);
  s.setInt("EngineX.E", 4);  // shares the textual prefix but is not under "Engine"
  s.setInt("Media.F", 5);
  auto keys = sq::listKeys(&s, "Engine");
  REQUIRE_EQ(keys.size(), size_t(3));
  CHECK_EQ(keys[0], std::string("Engine.A"));
  CHECK_EQ(keys[2], std::string("Engine.B.D"));
  CHECK_EQ(sq::listKeys(&s, "").size(), size_t(5));
  CHECK_OK(s.remove("Engine.B"));
  CHECK(!s.has("Engine.B.C"));
  CHECK(s.has("Engine.A"));
  CHECK(s.has("EngineX.E"));
  CHECK_EQ(s.remove("Engine.B"), SQ_E_NOTFOUND);
}

SQ_TEST("settings: observers by prefix (interface + function), removal") {
  Settings s;
  std::vector<std::string> engineEvents, allEvents;
  auto obs = sq::Ptr<sq::ISettingsObserver>::adopt(new sq::SettingsObserverFn([&](const char* k) { engineEvents.emplace_back(k); }));
  uint32_t cookie = 0;
  REQUIRE_OK(s.addObserver("Engine", obs.get(), &cookie));
  s.observe("", [&](const char* k) { allEvents.emplace_back(k); });

  s.setInt("Engine.BlockSize", 64);
  s.setInt("Engine.BlockSize", 64);  // no change -> no event
  s.setInt("EngineX.Foo", 1);        // not under "Engine"
  s.setString("Media.Dir", "x");
  CHECK_EQ(engineEvents.size(), size_t(1));
  CHECK_EQ(allEvents.size(), size_t(3));
  s.remove("Engine");
  CHECK_EQ(engineEvents.size(), size_t(2));
  CHECK_EQ(engineEvents.back(), std::string("Engine.BlockSize"));

  CHECK_OK(s.removeObserver(cookie));
  CHECK_EQ(s.removeObserver(cookie), SQ_E_NOTFOUND);
  s.setInt("Engine.BlockSize", 32);
  CHECK_EQ(engineEvents.size(), size_t(2));
}

SQ_TEST("settings: XML save/load roundtrip preserves types and hierarchy") {
  Settings a;
  a.setInt("Engine.SampleRate", 48000);
  a.setDouble("Engine.Master.Gain", -3.25);
  a.setBool("Engine.LowLatency", SQ_FALSE);
  a.setString("Devices.Audio.Driver", "ASIO <x & \"y\">");
  a.setString("Platform.Empty", "");
  a.setInt("Top", 1);
  std::string xml = a.toXml();
  CHECK(xml.find("<Section name=\"Engine\">") != std::string::npos);

  std::string dir = sqtest::makeTempDir("settings");
  std::string file = sq::joinPath(dir, "sub/settings.xml");
  REQUIRE_OK(a.saveFile(file));
  Settings b;
  std::string err;
  REQUIRE_OK(b.loadFile(file, &err));
  CHECK_EQ(b.size(), a.size());
  CHECK_EQ(b.getInt("Engine.SampleRate", 0), 48000);
  CHECK_EQ(b.getDouble("Engine.Master.Gain", 0), -3.25);
  CHECK_EQ(b.getType("Engine.LowLatency"), SQ_SETTING_BOOL);
  CHECK_EQ(b.getBool("Engine.LowLatency", SQ_TRUE), SQ_FALSE);
  CHECK_EQ(b.get("Devices.Audio.Driver"), std::string("ASIO <x & \"y\">"));
  CHECK(b.has("Platform.Empty"));
  CHECK_EQ(b.getInt("Top", 0), 1);
  CHECK_EQ(b.toXml(), xml);

  Settings c;
  CHECK_EQ(c.loadXml("<NotSettings/>", &err), SQ_E_PARSE);
  CHECK_EQ(c.loadXml("<Settings><Value", &err), SQ_E_PARSE);
  CHECK_EQ(c.loadFile(sq::joinPath(dir, "missing.xml")), SQ_E_NOTFOUND);
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("settings: setDefault does not override") {
  Settings s;
  CHECK(s.setDefault("Engine.BlockSize", SQ_SETTING_INT, "256"));
  s.setInt("Engine.BlockSize", 64);
  CHECK(!s.setDefault("Engine.BlockSize", SQ_SETTING_INT, "256"));
  CHECK_EQ(s.getInt("Engine.BlockSize", 0), 64);
}
