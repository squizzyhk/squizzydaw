// Squizzy Studio engine/audio - source node types (plan 2.3: file, clip, modification, event,
// media) plus the device-input node.
//
// A source node renders timeline audio for a track: render() ADDS the node's output for the
// timeline range [ctx.timelineFrame, +ctx.frames) into the track buffer. Nodes are prototypes in a
// GraphDesc; each compiled Graph clone()s its own instances and prepare()s them on the control
// thread. render() is RT.
//
// Media is fully decoded into memory when the graph is built (MediaBuffer, shared between graphs
// and render tasks). Streaming from disk during playback is not implemented yet.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sq/sdk.hpp"
#include "sq/interfaces/codec.h"
#include "sq/engine/audio/processor.hpp"

namespace sq::engine::audio {

// Immutable decoded audio (non-interleaved float32).
struct MediaBuffer {
  double sampleRate = 48000.0;
  uint32_t channels = 0;
  int64_t frames = 0;
  std::vector<std::vector<float>> data;
  std::string path;
  const float* channel(uint32_t c) const noexcept { return data[c].data(); }
};
using MediaPtr = std::shared_ptr<const MediaBuffer>;

// Decode a whole file through an opened-capable reader (control/worker thread).
sq_result loadMedia(IAudioFileReader* reader, const char* path, MediaPtr* out);
// Finds a codec by the file extension (codec::findCodec) and decodes the file.
sq_result loadMediaFile(IClassRegistry* reg, const char* path, MediaPtr* out);

class SourceNode {
 public:
  enum class Kind : uint8_t { File, Clip, Modification, Event, Media, Input };
  virtual ~SourceNode() = default;
  virtual Kind kind() const noexcept = 0;
  virtual std::unique_ptr<SourceNode> clone() const = 0;
  virtual bool prepare(double sampleRate, uint32_t maxFrames, uint32_t channels) {
    (void)sampleRate; (void)maxFrames; (void)channels;
    return true;
  }
  virtual void reset() noexcept {}
  // RT: add output into out[0..channels).
  virtual void render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept = 0;
  // Timeline end (exclusive) of content incl. tail, for render length; 0 = none.
  virtual int64_t endFrame() const noexcept { return 0; }
  // True if this node needs the transport to be playing to produce output.
  virtual bool needsPlayback() const noexcept { return true; }
};
using SourcePtr = std::unique_ptr<SourceNode>;
using SourceProto = std::shared_ptr<const SourceNode>;

// Media node: media frame == timeline frame (place it with a ClipNode). Missing channels of a
// mono source are duplicated into every output channel.
class MediaNode : public SourceNode {
 public:
  explicit MediaNode(MediaPtr media) : media_(std::move(media)) {}
  Kind kind() const noexcept override { return Kind::Media; }
  std::unique_ptr<SourceNode> clone() const override { return std::make_unique<MediaNode>(media_); }
  void render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept override;
  int64_t endFrame() const noexcept override { return media_ ? media_->frames : 0; }
  const MediaPtr& media() const noexcept { return media_; }

 protected:
  MediaPtr media_;
};

// File node: a media node whose buffer was decoded from a file by a codec.
class FileNode final : public MediaNode {
 public:
  using MediaNode::MediaNode;
  static sq_result load(IClassRegistry* reg, const char* path, std::unique_ptr<FileNode>* out);
  Kind kind() const noexcept override { return Kind::File; }
  std::unique_ptr<SourceNode> clone() const override { return std::make_unique<FileNode>(media_); }
};

// Clip: places a child on the timeline with gain and linear fades.
struct ClipParams {
  int64_t start = 0;        // timeline frame
  int64_t length = 0;       // frames
  int64_t sourceOffset = 0; // child frame at clip start
  float gain = 1.0f;
  int64_t fadeIn = 0;       // frames
  int64_t fadeOut = 0;
};
class ClipNode final : public SourceNode {
 public:
  ClipNode(SourceProto child, const ClipParams& p) : childProto_(std::move(child)), p_(p) {}
  Kind kind() const noexcept override { return Kind::Clip; }
  std::unique_ptr<SourceNode> clone() const override { return std::make_unique<ClipNode>(childProto_, p_); }
  bool prepare(double sr, uint32_t maxFrames, uint32_t channels) override;
  void reset() noexcept override;
  void render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept override;
  int64_t endFrame() const noexcept override { return p_.start + p_.length; }
  const ClipParams& params() const noexcept { return p_; }

 private:
  SourceProto childProto_;
  SourcePtr child_;
  ClipParams p_;
  std::vector<float> scratch_;
  std::vector<float*> ptrs_;
  uint32_t maxFrames_ = 0;
};

// Modification: applies a breakpoint gain envelope (linear between points, timeline frames) to a
// child's output (clip/track gain automation, the "modification" node of plan 2.3).
struct GainPoint {
  int64_t frame;
  float gain;
};
class ModificationNode final : public SourceNode {
 public:
  ModificationNode(SourceProto child, std::vector<GainPoint> points);
  Kind kind() const noexcept override { return Kind::Modification; }
  std::unique_ptr<SourceNode> clone() const override { return std::make_unique<ModificationNode>(childProto_, points_); }
  bool prepare(double sr, uint32_t maxFrames, uint32_t channels) override;
  void reset() noexcept override;
  void render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept override;
  int64_t endFrame() const noexcept override;
  float gainAt(int64_t frame) const noexcept;

 private:
  SourceProto childProto_;
  SourcePtr child_;
  std::vector<GainPoint> points_;
  std::vector<float> scratch_;
  std::vector<float*> ptrs_;
  uint32_t maxFrames_ = 0;
};

// Event node: timeline MIDI events driving a synth processor.
struct TimedMidi {
  int64_t frame;    // timeline frame
  uint8_t bytes[3];
  uint8_t size;
};
class EventNode final : public SourceNode {
 public:
  // events need not be sorted; the node sorts them. synth prototype is cloned per graph.
  EventNode(std::vector<TimedMidi> events, ProcessorProto synth);
  Kind kind() const noexcept override { return Kind::Event; }
  std::unique_ptr<SourceNode> clone() const override { return std::make_unique<EventNode>(events_, synthProto_); }
  bool prepare(double sr, uint32_t maxFrames, uint32_t channels) override;
  void reset() noexcept override;
  void render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept override;
  int64_t endFrame() const noexcept override;

 private:
  std::vector<TimedMidi> events_;
  ProcessorProto synthProto_;
  ProcessorPtr synth_;
  std::vector<sq_midi_event> blockEvents_;  // preallocated
  std::vector<float> scratch_;
  std::vector<float*> ptrs_;
  int64_t expectNext_ = INT64_MIN;
  size_t cursor_ = 0;
  uint32_t maxFrames_ = 0;
};

// Device input (monitoring / recording). Renders regardless of transport state.
class InputNode final : public SourceNode {
 public:
  explicit InputNode(std::vector<uint32_t> inputChannels) : channels_(std::move(inputChannels)) {}
  Kind kind() const noexcept override { return Kind::Input; }
  std::unique_ptr<SourceNode> clone() const override { return std::make_unique<InputNode>(channels_); }
  void render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept override;
  bool needsPlayback() const noexcept override { return false; }

 private:
  std::vector<uint32_t> channels_;
};

}  // namespace sq::engine::audio
