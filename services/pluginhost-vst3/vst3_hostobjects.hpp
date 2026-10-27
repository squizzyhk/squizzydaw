// pluginhost-vst3: host-implemented VST3 objects (application context, messages, process-call
// structures, connection proxy).
#pragma once

#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "vst3_common.hpp"

namespace sqvst3 {

namespace V = Steinberg::Vst;

// ---- IAttributeList / IMessage --------------------------------------------------------------------
class AttributeList final : public FObject<AttributeList, V::IAttributeList> {
 public:
  tresult PLUGIN_API setInt(AttrID id, Steinberg::int64 value) override;
  tresult PLUGIN_API getInt(AttrID id, Steinberg::int64& value) override;
  tresult PLUGIN_API setFloat(AttrID id, double value) override;
  tresult PLUGIN_API getFloat(AttrID id, double& value) override;
  tresult PLUGIN_API setString(AttrID id, const V::TChar* string) override;
  tresult PLUGIN_API getString(AttrID id, V::TChar* string, Steinberg::uint32 sizeInBytes) override;
  tresult PLUGIN_API setBinary(AttrID id, const void* data, Steinberg::uint32 sizeInBytes) override;
  tresult PLUGIN_API getBinary(AttrID id, const void*& data, Steinberg::uint32& sizeInBytes) override;

 private:
  enum class Kind { Int, Float, String, Binary };
  struct Value {
    Kind kind = Kind::Int;
    Steinberg::int64 i = 0;
    double f = 0;
    std::u16string s;
    std::vector<uint8_t> b;
  };
  std::mutex m_;
  std::map<std::string, Value> values_;
};

class Message final : public FObject<Message, V::IMessage> {
 public:
  Message();
  Steinberg::FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
  void PLUGIN_API setMessageID(Steinberg::FIDString id) override { id_ = id ? id : ""; }
  V::IAttributeList* PLUGIN_API getAttributes() override { return attrs_; }  // borrowed

 private:
  std::string id_;
  IPtr<AttributeList> attrs_;
};

// ---- IHostApplication (+ IPlugInterfaceSupport) -----------------------------------------------------
class HostApplication final : public FObject<HostApplication, V::IHostApplication, V::IPlugInterfaceSupport> {
 public:
  tresult PLUGIN_API getName(V::String128 name) override;
  tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override;
  tresult PLUGIN_API isPlugInterfaceSupported(const TUID iid) override;
};

// ---- process-call structures (preallocated, audio-thread safe) -------------------------------------
class ParamValueQueue final : public FStatic<ParamValueQueue, V::IParamValueQueue> {
 public:
  static constexpr int32_t kMaxPoints = 128;
  V::ParamID PLUGIN_API getParameterId() override { return id_; }
  Steinberg::int32 PLUGIN_API getPointCount() override { return count_; }
  tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& sampleOffset, V::ParamValue& value) override;
  tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset, V::ParamValue value, Steinberg::int32& index) override;

  void reset(V::ParamID id) noexcept {
    id_ = id;
    count_ = 0;
  }

 private:
  struct Point {
    int32_t offset;
    double value;
  };
  V::ParamID id_ = V::kNoParamId;
  int32_t count_ = 0;
  Point points_[kMaxPoints] = {};
};

class ParameterChanges final : public FStatic<ParameterChanges, V::IParameterChanges> {
 public:
  void prepare(uint32_t maxQueues) { queues_ = std::vector<ParamValueQueue>(maxQueues ? maxQueues : 1); }
  void clear() noexcept { used_ = 0; }
  Steinberg::int32 PLUGIN_API getParameterCount() override { return used_; }
  V::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override;
  V::IParamValueQueue* PLUGIN_API addParameterData(const V::ParamID& id, Steinberg::int32& index) override;
  // RT helper: append a point for `id` (sample offsets must be non-decreasing per id).
  bool add(V::ParamID id, int32_t offset, double value) noexcept;
  ParamValueQueue* queue(int32_t i) noexcept { return i >= 0 && i < used_ ? &queues_[static_cast<size_t>(i)] : nullptr; }

 private:
  std::vector<ParamValueQueue> queues_;
  int32_t used_ = 0;
};

class EventList final : public FStatic<EventList, V::IEventList> {
 public:
  void prepare(uint32_t capacity) { events_.assign(capacity ? capacity : 1, V::Event{}); }
  void clear() noexcept { count_ = 0; }
  Steinberg::int32 PLUGIN_API getEventCount() override { return count_; }
  tresult PLUGIN_API getEvent(Steinberg::int32 index, V::Event& e) override;
  tresult PLUGIN_API addEvent(V::Event& e) override;
  const V::Event* at(int32_t i) const noexcept { return i >= 0 && i < count_ ? &events_[static_cast<size_t>(i)] : nullptr; }

 private:
  std::vector<V::Event> events_;
  int32_t count_ = 0;
};

// ---- IConnectionPoint proxy -------------------------------------------------------------------------
// The host connects component and controller through two proxies (one per direction) instead of
// directly, as the VST3 model expects: a message sent from a non-main thread is queued and
// delivered on the main thread by deliverPending() (never on the audio thread).
class ConnectionProxy final : public FObject<ConnectionProxy, V::IConnectionPoint> {
 public:
  ConnectionProxy(V::IConnectionPoint* peer, uint64_t mainThread) : peer_(peer), mainThread_(mainThread) {}
  tresult PLUGIN_API connect(V::IConnectionPoint*) override { return Steinberg::kResultFalse; }
  tresult PLUGIN_API disconnect(V::IConnectionPoint*) override { return Steinberg::kResultOk; }
  tresult PLUGIN_API notify(V::IMessage* message) override;
  void deliverPending();  // main thread
  void close();           // drop the peer; later messages are discarded

 private:
  std::mutex m_;
  IPtr<V::IConnectionPoint> peer_;
  std::vector<IPtr<V::IMessage>> pending_;
  uint64_t mainThread_;
};

}  // namespace sqvst3
