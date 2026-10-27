// Compile-checks every area interface header (sdk/include/sq/interfaces/*.h): IIDs and class IDs
// are unique, POD structs start with struct_size and match the C compiler's layout, and the
// header-only helpers (attributes.hpp, bytestream.hpp, codec::findCodec) work.
#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>

#include "sq/sdk.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/cpp/bytestream.hpp"
#include "sq/interfaces/dummy.h"
#include "sq/interfaces/common.h"
#include "sq/interfaces/audiodevice.h"
#include "sq/interfaces/midi.h"
#include "sq/interfaces/codec.h"
#include "sq/interfaces/document.h"
#include "sq/interfaces/audioengine.h"
#include "sq/interfaces/pluginhost.h"
#include "sq/interfaces/index.h"
#include "sq/interfaces/scripting.h"
#include "sq/interfaces/dsp.h"
#include "sq/interfaces/ui.h"
#include "sq/interfaces/remote.h"
#include "sq/interfaces/video.h"
#include "sq/interfaces/surface.h"
#include "sq/interfaces/asio.h"
#include "sq/app/class_registry.hpp"
#include "sqtest.hpp"

extern "C" size_t sq_c_iface_sizes(size_t* out, size_t cap);

namespace {

constexpr sq_guid kAllIds[] = {
  // core
  sq::IUnknown::iid, sq::IClassFactory::iid, sq::IHostContext::iid, sq::ISettings::iid,
  sq::ISettingsObserver::iid, sq::IClassRegistry::iid, sq::IDummyService::iid, sq::IDummyCounter::iid,
  sq::dummy::kServiceClassId, sq::dummy::kCounterClassId,
  // common
  sq::IAttributes::iid, sq::IByteStream::iid, sq::IObserver::iid, sq::ISubject::iid,
  // audiodevice
  sq::IAudioCallback::iid, sq::IAudioDeviceListener::iid, sq::IAudioDevice::iid, sq::IAudioDriver::iid,
  sq::audiodevice::kWasapiDriverClassId, sq::audiodevice::kAsioDriverClassId,
  // midi
  sq::IMidiInputCallback::iid, sq::IMidiDeviceListener::iid, sq::IMidiInputPort::iid, sq::IMidiOutputPort::iid,
  sq::IMidiDriver::iid, sq::IMidiFileReader::iid, sq::IMidiFileWriter::iid,
  sq::midi::kWinMidiDriverClassId, sq::midi::kMidiFileReaderClassId, sq::midi::kMidiFileWriterClassId,
  // codec
  sq::IAudioFileReader::iid, sq::IAudioFileWriter::iid, sq::ICodecService::iid,
  sq::codec::kWavCodecClassId, sq::codec::kFlacCodecClassId, sq::codec::kMp3CodecClassId,
  sq::codec::kOggCodecClassId, sq::codec::kAacCodecClassId,
  // document
  sq::INode::iid, sq::ITempoMap::iid, sq::IClip::iid, sq::ITrack::iid, sq::IDocument::iid, sq::IEditor::iid,
  sq::IAction::iid, sq::IDocumentManager::iid,
  sq::doc::kSetProperty, sq::doc::kAddTrack, sq::doc::kRemoveNode, sq::doc::kAddClip, sq::doc::kMoveClip,
  sq::doc::kResizeClip, sq::doc::kSplitClip, sq::doc::kSetClipEvents, sq::doc::kAddMarker, sq::doc::kSetTempo,
  sq::doc::kRemoveTempo, sq::doc::kSetSignature,
  // audioengine
  sq::IRenderObserver::iid, sq::IRenderTask::iid, sq::ITransport::iid, sq::IAudioEngine::iid,
  // pluginhost
  sq::IPluginHostCallback::iid, sq::IPluginScanSink::iid, sq::IPluginInstance::iid, sq::IPluginFormat::iid,
  sq::IPluginRegistry::iid, sq::pluginhost::kClapFormatClassId, sq::pluginhost::kVst3FormatClassId,
  // index
  sq::IIndexResult::iid, sq::IContentIndex::iid, sq::index::kSqliteIndexClassId,
  // scripting
  sq::ICommandHandler::iid, sq::IController::iid, sq::IComponent::iid, sq::IUrlResolver::iid,
  sq::IObjectRegistry::iid, sq::IScriptPackage::iid, sq::IScriptEngine::iid,
  // dsp
  sq::ITimeStretcher::iid, sq::IAnalysisResult::iid, sq::IAnalyzer::iid, sq::INNModel::iid, sq::INNRuntime::iid,
  sq::dsp::kTimeStretchClassId, sq::dsp::kBeatAnalyzerClassId, sq::dsp::kChordAnalyzerClassId,
  sq::dsp::kStemSeparatorClassId, sq::dsp::kTunerClassId,
  // ui
  sq::IThemeResolver::iid, sq::IRemotePage::iid, sq::IRemotePageSet::iid, sq::IRemotePageProvider::iid,
  // remote
  sq::IDiscoveryListener::iid, sq::IDiscoveryService::iid, sq::IOscListener::iid, sq::IOscService::iid,
  sq::IControlSurfaceListener::iid, sq::IControlSurfaceService::iid, sq::ITempoSync::iid, sq::ICloudAccount::iid,
  // video
  sq::IVideoReader::iid, sq::IVideoEngine::iid, sq::video::kVideoEngineClassId,
  // surface (hardware-surface extension), asio status
  sq::ISurfacePump::iid, sq::surface::kHardwareSurfaceClassId, sq::IAsioDriverStatus::iid,
};

constexpr bool allUnique() {
  constexpr size_t n = sizeof(kAllIds) / sizeof(kAllIds[0]);
  for (size_t i = 0; i < n; ++i) {
    if (kAllIds[i] == sq_guid{}) return false;
    for (size_t j = i + 1; j < n; ++j)
      if (kAllIds[i] == kAllIds[j]) return false;
  }
  return true;
}
static_assert(allUnique(), "duplicate or null GUID among interface IIDs/class IDs");

}  // namespace

// Every POD crossing the boundary that carries struct_size has it first; all are standard layout.
#define SQ_CHECK_POD(T)                                                                             \
  static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>, #T " must be POD"); \
  static_assert(offsetof(T, struct_size) == 0, #T ": struct_size must be the first field")
SQ_CHECK_POD(sq_audio_driver_info);   SQ_CHECK_POD(sq_audio_device_info);  SQ_CHECK_POD(sq_audio_device_config);
SQ_CHECK_POD(sq_audio_io);            SQ_CHECK_POD(sq_midi_port_info);     SQ_CHECK_POD(sq_midi_file_info);
SQ_CHECK_POD(sq_midi_file_event);     SQ_CHECK_POD(sq_codec_info);         SQ_CHECK_POD(sq_codec_format_info);
SQ_CHECK_POD(sq_audio_file_format);   SQ_CHECK_POD(sq_audio_file_marker);  SQ_CHECK_POD(sq_document_info);
SQ_CHECK_POD(sq_track_info);          SQ_CHECK_POD(sq_clip_info);          SQ_CHECK_POD(sq_clip_event);
SQ_CHECK_POD(sq_tempo_point);         SQ_CHECK_POD(sq_time_signature);     SQ_CHECK_POD(sq_doc_marker);
SQ_CHECK_POD(sq_engine_latency);      SQ_CHECK_POD(sq_engine_status);      SQ_CHECK_POD(sq_transport_state);
SQ_CHECK_POD(sq_render_request);      SQ_CHECK_POD(sq_plugin_desc);        SQ_CHECK_POD(sq_plugin_format_info);
SQ_CHECK_POD(sq_plugin_bus_info);     SQ_CHECK_POD(sq_param_info);         SQ_CHECK_POD(sq_plugin_setup);
SQ_CHECK_POD(sq_plugin_transport);    SQ_CHECK_POD(sq_plugin_process);     SQ_CHECK_POD(sq_index_item);
SQ_CHECK_POD(sq_index_row);           SQ_CHECK_POD(sq_index_query);        SQ_CHECK_POD(sq_index_store_info);
SQ_CHECK_POD(sq_command);             SQ_CHECK_POD(sq_vector_ops);         SQ_CHECK_POD(sq_stretch_config);
SQ_CHECK_POD(sq_analyzer_info);       SQ_CHECK_POD(sq_analysis_event);     SQ_CHECK_POD(sq_tuner_value);
SQ_CHECK_POD(sq_tensor_info);         SQ_CHECK_POD(sq_tensor);             SQ_CHECK_POD(sq_theme_resource);
SQ_CHECK_POD(sq_remote_control);      SQ_CHECK_POD(sq_discovery_record);   SQ_CHECK_POD(sq_surface_info);
SQ_CHECK_POD(sq_tempo_sync_state);    SQ_CHECK_POD(sq_account_state);
SQ_CHECK_POD(sq_video_info);          SQ_CHECK_POD(sq_video_frame);        SQ_CHECK_POD(sq_video_backend_info);
static_assert(std::is_standard_layout_v<sq_param_change> && std::is_standard_layout_v<sq_osc_arg> &&
              std::is_standard_layout_v<sq_meter_value>, "POD");
static_assert(sizeof(sq_clip_event) % 8 == 0, "sq_clip_event is packed into blobs: keep it 8-byte sized");
static_assert(std::is_base_of_v<sq::INode, sq::IDocument> && std::is_same_v<sq::IDocument::sq_base, sq::INode>, "derivation");

SQ_TEST("interfaces: IIDs and class IDs are unique and non-null") { CHECK(allUnique()); }

SQ_TEST("interfaces: C and C++ agree on POD sizes") {
  size_t c[64] = {};
  size_t n = sq_c_iface_sizes(c, 64);
  const size_t cpp[] = {
    sizeof(sq_audio_device_info), sizeof(sq_audio_device_config), sizeof(sq_audio_io),
    sizeof(sq_midi_port_info), sizeof(sq_midi_timed_event), sizeof(sq_midi_file_event),
    sizeof(sq_audio_file_format), sizeof(sq_audio_file_marker), sizeof(sq_codec_format_info),
    sizeof(sq_track_info), sizeof(sq_clip_info), sizeof(sq_clip_event), sizeof(sq_doc_marker),
    sizeof(sq_engine_status), sizeof(sq_transport_state), sizeof(sq_render_request),
    sizeof(sq_plugin_desc), sizeof(sq_plugin_process), sizeof(sq_param_info), sizeof(sq_param_change),
    sizeof(sq_index_item), sizeof(sq_index_row), sizeof(sq_index_query), sizeof(sq_command),
    sizeof(sq_vector_ops), sizeof(sq_stretch_config), sizeof(sq_analysis_event), sizeof(sq_tensor),
    sizeof(sq_theme_resource), sizeof(sq_remote_control), sizeof(sq_osc_arg), sizeof(sq_tempo_sync_state),
    sizeof(sq_account_state), sizeof(sq_discovery_record), sizeof(sq_video_info), sizeof(sq_video_frame),
    sizeof(sq_video_backend_info)};
  REQUIRE_EQ(n, sizeof(cpp) / sizeof(cpp[0]));
  for (size_t i = 0; i < n; ++i) CHECK_EQ(c[i], cpp[i]);
}

SQ_TEST("interfaces: AttributeSet stores, converts and copies out") {
  auto a = sq::makeAttributes();
  CHECK_OK(a->setInt("n", 42));
  CHECK_OK(a->setDouble("x", 1.5));
  CHECK_OK(a->setString("s", "hello"));
  sq_guid g = sq::doc::kAddTrack;
  CHECK_OK(a->setGuid("g", &g));
  const uint8_t blob[3] = {1, 2, 3};
  CHECK_OK(a->setBlob("b", blob, 3));
  CHECK_EQ(a->getCount(), 5u);
  CHECK_EQ(a->getInt("n", 0), 42);
  CHECK_EQ(a->getDouble("n", 0), 42.0);
  CHECK_EQ(a->getInt("x", 0), 1);
  CHECK_EQ(a->getType("s"), SQ_VALUE_STRING);
  CHECK_EQ(sq::getAttrString(a.get(), "s"), std::string("hello"));
  char small[3];
  uint32_t need = 0;
  CHECK_EQ(a->getString("s", small, sizeof small, &need), SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(need, 6u);
  sq_guid g2{};
  CHECK_OK(a->getGuid("g", &g2));
  CHECK(g2 == g);
  uint8_t out[3] = {};
  CHECK_OK(a->getBlob("b", out, 3, &need));
  CHECK_EQ(out[2], 3);
  CHECK_OK(a->setInt("s", 7));  // retype keeps position
  char key[8];
  CHECK_OK(a->getKey(2, key, sizeof key, nullptr));
  CHECK_EQ(std::string(key), std::string("s"));
  auto inner = sq::makeAttributes();
  CHECK_OK(a->setObject("o", inner.get()));
  sq::Ptr<sq::IAttributes> back;
  CHECK_OK(a->getObject("o", &sq::IAttributes::iid, back.putVoid()));
  CHECK(back.get() == inner.get());
  CHECK_OK(a->remove("o"));
  CHECK_EQ(a->remove("o"), SQ_S_FALSE);
  a->clear();
  CHECK_EQ(a->getCount(), 0u);
}

SQ_TEST("interfaces: MemoryStream read/write/seek") {
  auto s = sq::makeMemoryStream();
  uint32_t n = 0;
  CHECK_OK(s->write("abcdef", 6, &n));
  CHECK_EQ(n, 6u);
  int64_t pos = 0;
  CHECK_OK(s->seek(2, SQ_SEEK_SET, &pos));
  char buf[8] = {};
  CHECK_OK(s->read(buf, 8, &n));
  CHECK_EQ(n, 4u);
  CHECK_EQ(std::string(buf, 4), std::string("cdef"));
  int64_t size = 0;
  CHECK_OK(s->getSize(&size));
  CHECK_EQ(size, 6);
  auto ro = sq::makeMemoryStream("xy", 2, false);
  CHECK_EQ(ro->write("z", 1, &n), SQ_E_ACCESS);
}

namespace {
// Pattern for consumers: mock the interface of a unit another agent implements.
class MockCodec final : public sq::Object<MockCodec, sq::ICodecService> {
 public:
  sq_result SQ_CALL getCodecInfo(sq_codec_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getFormatCount() override { return 0; }
  sq_result SQ_CALL getFormatInfo(uint32_t, sq_codec_format_info*) override { return SQ_E_NOTFOUND; }
  sq_bool SQ_CALL supports(const char* ext, const char*, uint32_t flags) override {
    return (ext && std::strcmp(ext, "mock") == 0 && !(flags & SQ_CODEC_WRITE)) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void*, uint32_t) override { return 0; }
  sq_result SQ_CALL createReader(sq::IAudioFileReader**) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL createWriter(const char*, sq::IAudioFileWriter**) override { return SQ_E_NOTIMPL; }
};
class MockFactory final : public sq::Object<MockFactory, sq::IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getClassCount() override { return 0; }
  sq_result SQ_CALL getClassInfo(uint32_t, sq_class_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL createInstance(const sq_guid*, const sq_guid* riid, void** out) override {
    auto* c = new MockCodec();
    sq_result r = c->queryInterface(riid, out);
    c->release();
    return r;
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};
}  // namespace

SQ_TEST("interfaces: codec::findCodec finds a registered codec by extension") {
  sq::app::ClassRegistry reg;
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  ci.class_id = sq::codec::kWavCodecClassId;
  ci.category = sq::codec::kCategory;
  ci.name = "Mock codec";
  auto f = sq::Ptr<sq::IClassFactory>::adopt(new MockFactory());
  REQUIRE_OK(reg.registerClass(&ci, f.get()));
  auto hit = sq::Ptr<sq::ICodecService>::adopt(sq::codec::findCodec(&reg, "mock", nullptr, SQ_CODEC_READ));
  CHECK(static_cast<bool>(hit));
  auto miss = sq::Ptr<sq::ICodecService>::adopt(sq::codec::findCodec(&reg, "mock", nullptr, SQ_CODEC_WRITE));
  CHECK(!miss);
  CHECK_OK(reg.unregisterClass(&ci.class_id));
}
