#ifdef _MSC_VER
// VST3 interface methods take a parameter named `iid`, which newer MSVC flags as hiding the
// interface's static `iid` member (C4458). The names come from the SDK signatures.
#pragma warning(disable : 4458)
#endif
#include "vst3_hostobjects.hpp"

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <algorithm>

namespace sqvst3 {

using namespace Steinberg;

// ---- AttributeList ------------------------------------------------------------------------------
tresult PLUGIN_API AttributeList::setInt(AttrID id, int64 value) {
  if (!id) return kInvalidArgument;
  std::lock_guard<std::mutex> l(m_);
  Value& v = values_[id];
  v = Value{};
  v.kind = Kind::Int;
  v.i = value;
  return kResultOk;
}
tresult PLUGIN_API AttributeList::getInt(AttrID id, int64& value) {
  if (!id) return kInvalidArgument;
  std::lock_guard<std::mutex> l(m_);
  auto it = values_.find(id);
  if (it == values_.end() || it->second.kind != Kind::Int) return kResultFalse;
  value = it->second.i;
  return kResultOk;
}
tresult PLUGIN_API AttributeList::setFloat(AttrID id, double value) {
  if (!id) return kInvalidArgument;
  std::lock_guard<std::mutex> l(m_);
  Value& v = values_[id];
  v = Value{};
  v.kind = Kind::Float;
  v.f = value;
  return kResultOk;
}
tresult PLUGIN_API AttributeList::getFloat(AttrID id, double& value) {
  if (!id) return kInvalidArgument;
  std::lock_guard<std::mutex> l(m_);
  auto it = values_.find(id);
  if (it == values_.end() || it->second.kind != Kind::Float) return kResultFalse;
  value = it->second.f;
  return kResultOk;
}
tresult PLUGIN_API AttributeList::setString(AttrID id, const V::TChar* string) {
  if (!id || !string) return kInvalidArgument;
  std::lock_guard<std::mutex> l(m_);
  Value& v = values_[id];
  v = Value{};
  v.kind = Kind::String;
  for (const V::TChar* p = string; *p; ++p) v.s.push_back(static_cast<char16_t>(*p));
  return kResultOk;
}
tresult PLUGIN_API AttributeList::getString(AttrID id, V::TChar* string, uint32 sizeInBytes) {
  if (!id || !string || sizeInBytes < sizeof(V::TChar)) return kInvalidArgument;
  std::lock_guard<std::mutex> l(m_);
  auto it = values_.find(id);
  if (it == values_.end() || it->second.kind != Kind::String) return kResultFalse;
  size_t cap = sizeInBytes / sizeof(V::TChar);
  size_t n = std::min(it->second.s.size(), cap - 1);
  for (size_t i = 0; i < n; ++i) string[i] = static_cast<V::TChar>(it->second.s[i]);
  string[n] = 0;
  return kResultOk;
}
tresult PLUGIN_API AttributeList::setBinary(AttrID id, const void* data, uint32 sizeInBytes) {
  if (!id || (!data && sizeInBytes)) return kInvalidArgument;
  std::lock_guard<std::mutex> l(m_);
  Value& v = values_[id];
  v = Value{};
  v.kind = Kind::Binary;
  v.b.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + sizeInBytes);
  return kResultOk;
}
tresult PLUGIN_API AttributeList::getBinary(AttrID id, const void*& data, uint32& sizeInBytes) {
  if (!id) return kInvalidArgument;
  std::lock_guard<std::mutex> l(m_);
  auto it = values_.find(id);
  if (it == values_.end() || it->second.kind != Kind::Binary) return kResultFalse;
  // Valid while the attribute is not overwritten and the list lives (VST3 convention).
  data = it->second.b.data();
  sizeInBytes = static_cast<uint32>(it->second.b.size());
  return kResultOk;
}

Message::Message() : attrs_(owned(new AttributeList())) {}

// ---- HostApplication ----------------------------------------------------------------------------
tresult PLUGIN_API HostApplication::getName(V::String128 name) {
  if (!name) return kInvalidArgument;
  toUtf16("Squizzy Studio", name, 128);
  return kResultOk;
}

tresult PLUGIN_API HostApplication::createInstance(TUID cid, TUID iid, void** obj) {
  if (!obj) return kInvalidArgument;
  *obj = nullptr;
  if (!cid) return kInvalidArgument;
  if (iidIs(cid, V::IMessage::iid)) {
    IPtr<Message> m = owned(new (std::nothrow) Message());
    if (!m) return kOutOfMemory;
    return m->queryInterface(iid ? iid : V::IMessage::iid.toTUID(), obj);
  }
  if (iidIs(cid, V::IAttributeList::iid)) {
    IPtr<AttributeList> a = owned(new (std::nothrow) AttributeList());
    if (!a) return kOutOfMemory;
    return a->queryInterface(iid ? iid : V::IAttributeList::iid.toTUID(), obj);
  }
  return kNoInterface;
}

tresult PLUGIN_API HostApplication::isPlugInterfaceSupported(const TUID iid) {
  const FUID* supported[] = {
      &IPluginBase::iid,          &V::IComponent::iid,         &V::IAudioProcessor::iid,
      &V::IEditController::iid,   &V::IConnectionPoint::iid,   &V::IMidiMapping::iid,
      &IPlugView::iid,            &V::IProcessContextRequirements::iid,
  };
  for (const FUID* f : supported)
    if (iidIs(iid, *f)) return kResultTrue;
  return kResultFalse;
}

// ---- ParamValueQueue / ParameterChanges -----------------------------------------------------------
tresult PLUGIN_API ParamValueQueue::getPoint(int32 index, int32& sampleOffset, V::ParamValue& value) {
  if (index < 0 || index >= count_) return kResultFalse;
  sampleOffset = points_[index].offset;
  value = points_[index].value;
  return kResultOk;
}

tresult PLUGIN_API ParamValueQueue::addPoint(int32 sampleOffset, V::ParamValue value, int32& index) {
  // Points are kept sorted by offset; a point at an existing offset replaces it.
  int32_t i = count_;
  while (i > 0 && points_[i - 1].offset > sampleOffset) --i;
  if (i > 0 && points_[i - 1].offset == sampleOffset) {
    points_[i - 1].value = value;
    index = i - 1;
    return kResultOk;
  }
  if (count_ >= kMaxPoints) {
    // Full: keep the newest value by overwriting the last point (never allocate).
    if (i < count_) return kResultFalse;
    points_[count_ - 1] = Point{sampleOffset, value};
    index = count_ - 1;
    return kResultOk;
  }
  for (int32_t k = count_; k > i; --k) points_[k] = points_[k - 1];
  points_[i] = Point{sampleOffset, value};
  ++count_;
  index = i;
  return kResultOk;
}

V::IParamValueQueue* PLUGIN_API ParameterChanges::getParameterData(int32 index) { return queue(index); }

V::IParamValueQueue* PLUGIN_API ParameterChanges::addParameterData(const V::ParamID& id, int32& index) {
  for (int32_t i = 0; i < used_; ++i)
    if (queues_[static_cast<size_t>(i)].getParameterId() == id) {
      index = i;
      return &queues_[static_cast<size_t>(i)];
    }
  if (static_cast<size_t>(used_) >= queues_.size()) return nullptr;
  ParamValueQueue& q = queues_[static_cast<size_t>(used_)];
  q.reset(id);
  index = used_++;
  return &q;
}

bool ParameterChanges::add(V::ParamID id, int32_t offset, double value) noexcept {
  int32 idx = 0;
  V::IParamValueQueue* q = addParameterData(id, idx);
  if (!q) return false;
  int32 pi = 0;
  return q->addPoint(offset, value, pi) == kResultOk;
}

// ---- EventList ----------------------------------------------------------------------------------
tresult PLUGIN_API EventList::getEvent(int32 index, V::Event& e) {
  if (index < 0 || index >= count_) return kInvalidArgument;
  e = events_[static_cast<size_t>(index)];
  return kResultOk;
}

tresult PLUGIN_API EventList::addEvent(V::Event& e) {
  if (static_cast<size_t>(count_) >= events_.size()) return kResultFalse;
  events_[static_cast<size_t>(count_++)] = e;
  return kResultOk;
}

// ---- ConnectionProxy ----------------------------------------------------------------------------
tresult PLUGIN_API ConnectionProxy::notify(V::IMessage* message) {
  if (!message) return kInvalidArgument;
  IPtr<V::IConnectionPoint> peer;
  {
    std::lock_guard<std::mutex> l(m_);
    if (!peer_) return kResultFalse;
    if (sq_thread_current_id() != mainThread_) {
      try {
        pending_.emplace_back(message);  // add-ref'd by IPtr
      } catch (...) {
        return kOutOfMemory;
      }
      return kResultOk;
    }
    peer = peer_;
  }
  return peer->notify(message);
}

void ConnectionProxy::deliverPending() {
  std::vector<IPtr<V::IMessage>> batch;
  IPtr<V::IConnectionPoint> peer;
  {
    std::lock_guard<std::mutex> l(m_);
    if (pending_.empty() || !peer_) return;
    batch.swap(pending_);
    peer = peer_;
  }
  for (auto& m : batch) peer->notify(m);
}

void ConnectionProxy::close() {
  std::vector<IPtr<V::IMessage>> drop;
  IPtr<V::IConnectionPoint> peer;
  {
    std::lock_guard<std::mutex> l(m_);
    drop.swap(pending_);
    peer = peer_;
    peer_ = nullptr;
  }
  // `drop` and `peer` are released here, outside the lock.
}

}  // namespace sqvst3
