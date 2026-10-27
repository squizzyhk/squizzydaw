// hardware-surface: MIDI matching and feedback encoding (see matcher.hpp).
#include "matcher.hpp"

#include <algorithm>
#include <cmath>

namespace sq::hwsurface {

void Matcher::build(const SurfaceMap& map) {
  controls_ = map.controls;
  table_.assign(kTableSize, 0);
  state_.assign(controls_.size(), State{});
  toggles_.reset(new std::atomic<uint8_t>[controls_.size() ? controls_.size() : 1]);
  for (size_t i = 0; i < controls_.size(); ++i) toggles_[i].store(0, std::memory_order_relaxed);
  for (size_t i = 0; i < controls_.size(); ++i) {
    const Control& c = controls_[i];
    // Output-only controls never match input; a validated map has no duplicates, otherwise the
    // first control wins.
    if (!c.hasMidi || c.type == SQ_REMOTE_LED || c.type == SQ_REMOTE_DISPLAY) continue;
    uint16_t& e = table_[slot(c.midi.kind, c.midi.channel, c.midi.address)];
    if (!e) e = uint16_t(i + 1);
    if (c.midi.fourteenBit && c.midi.kind == MsgKind::ControlChange) {
      uint16_t& l = table_[slot(c.midi.kind, c.midi.channel, c.midi.lsbAddress)];
      if (!l) l = uint16_t((i + 1) | kLsbRole);
    }
  }
}

bool Matcher::toggleState(uint32_t control) const noexcept {
  return control < controls_.size() && toggles_[control].load(std::memory_order_acquire) != 0;
}

void Matcher::setToggleState(uint32_t control, bool on) noexcept {
  if (control < controls_.size()) toggles_[control].store(on ? 1 : 0, std::memory_order_release);
}

bool Matcher::match(const uint8_t* b, uint32_t size, ControlEvent& out) noexcept {
  if (!b || size < 2 || table_.empty()) return false;
  const uint8_t status = b[0];
  if (status < 0x80 || status >= 0xF0) return false;
  const uint8_t hi = status & 0xF0;
  const uint32_t ch = status & 0x0F;
  MsgKind kind;
  switch (hi) {
    case 0x80:
    case 0x90: kind = MsgKind::Note; break;
    case 0xA0: kind = MsgKind::PolyPressure; break;
    case 0xB0: kind = MsgKind::ControlChange; break;
    case 0xC0: kind = MsgKind::Program; break;
    case 0xD0: kind = MsgKind::ChannelPressure; break;
    default: kind = MsgKind::PitchBend; break;
  }
  const bool twoBytes = kind == MsgKind::Program || kind == MsgKind::ChannelPressure;
  if (!twoBytes && size < 3) return false;
  const uint8_t d1 = b[1] & 0x7F;
  const uint8_t d2 = twoBytes ? 0 : uint8_t(b[2] & 0x7F);
  const uint32_t address = (kind == MsgKind::ChannelPressure || kind == MsgKind::PitchBend) ? 0 : d1;
  const uint16_t entry = table_[slot(kind, ch, address)];
  if (!entry) return false;
  const uint32_t idx = uint32_t(entry & ~kLsbRole) - 1;
  const bool lsbRole = (entry & kLsbRole) != 0;
  const Control& c = controls_[idx];
  State& st = state_[idx];
  out.control = idx;
  out.relative = false;

  int32_t raw = 0;
  bool pressed = false;
  switch (kind) {
    case MsgKind::Note:
      raw = hi == 0x80 ? 0 : d2;
      pressed = raw > 0;
      break;
    case MsgKind::Program:
      raw = 127;
      pressed = true;
      break;
    case MsgKind::ChannelPressure: raw = d1; pressed = raw > 0; break;
    case MsgKind::PitchBend: raw = int32_t(d1) | (int32_t(d2) << 7); break;
    case MsgKind::PolyPressure: raw = d2; pressed = raw > 0; break;
    case MsgKind::ControlChange:
      if (c.midi.fourteenBit) {
        if (lsbRole) {
          st.lsbSeen = true;
          raw = (int32_t(st.msb) << 7) | d2;
        } else {
          st.msb = d2;
          if (st.lsbSeen) return false;  // the LSB that follows carries the full value
          raw = int32_t(d2) << 7;
        }
      } else {
        raw = d2;
      }
      pressed = raw > 0;
      break;
    default: return false;
  }

  if (c.type == SQ_REMOTE_BUTTON || c.type == SQ_REMOTE_PAD) {
    if (c.flags & SQ_REMOTE_TOGGLE) {
      if (!pressed) return false;  // releases do nothing for latching buttons
      const bool now = toggles_[idx].load(std::memory_order_acquire) == 0;
      toggles_[idx].store(now ? 1 : 0, std::memory_order_release);
      out.value = now ? 1.0 : 0.0;
      return true;
    }
    if (kind == MsgKind::Program) {
      out.value = 1.0;  // a trigger: there is no release message
      return true;
    }
    // Pads are velocity sensitive on press; buttons are on/off.
    if (c.type == SQ_REMOTE_PAD && pressed) {
      const double span = double(c.midi.max - c.midi.min);
      out.value = std::clamp((raw - c.midi.min) / (span > 0 ? span : 1.0), 0.0, 1.0);
      if (out.value <= 0.0) out.value = 1.0 / 127.0;  // a press never reads as a release
    } else {
      out.value = pressed ? 1.0 : 0.0;
    }
    return true;
  }

  if (c.mode != EncoderMode::Absolute) {
    int32_t ticks = 0;
    switch (c.mode) {
      case EncoderMode::TwosComplement: ticks = raw < 64 ? raw : raw - 128; break;
      case EncoderMode::SignMagnitude: ticks = (raw & 0x40) ? -(raw & 0x3F) : (raw & 0x3F); break;
      case EncoderMode::Offset64: ticks = raw - 64; break;
      default: break;
    }
    if (ticks == 0) return false;
    out.relative = true;
    out.value = std::clamp(ticks * c.step, -1.0, 1.0);
    return true;
  }

  const double span = double(c.midi.max - c.midi.min);
  out.value = std::clamp((raw - c.midi.min) / (span > 0 ? span : 1.0), 0.0, 1.0);
  return true;
}

int32_t toRaw(const Control& c, double value) noexcept {
  if (!std::isfinite(value)) value = 0.0;
  value = std::clamp(value, 0.0, 1.0);
  return c.midi.min + int32_t(std::lround(value * double(c.midi.max - c.midi.min)));
}

uint32_t encodeFeedback(const Control& c, double value, sq_midi_timed_event out[2]) noexcept {
  if (!c.hasMidi || c.type == SQ_REMOTE_DISPLAY || c.midi.kind == MsgKind::Program) return 0;
  const bool onOff = c.type == SQ_REMOTE_BUTTON || c.type == SQ_REMOTE_LED || (c.type == SQ_REMOTE_PAD && (c.flags & SQ_REMOTE_TOGGLE));
  int32_t raw = onOff ? (value >= 0.5 ? c.onValue : c.offValue) : toRaw(c, value);
  const uint8_t ch = c.midi.channel;
  out[0] = sq_midi_timed_event{};
  out[1] = sq_midi_timed_event{};
  sq_midi_timed_event& e = out[0];
  switch (c.midi.kind) {
    case MsgKind::Note:
      raw = std::clamp(raw, 0, 127);
      e.bytes[0] = uint8_t(0x90 | ch);
      e.bytes[1] = c.midi.address;
      e.bytes[2] = uint8_t(raw);
      e.size = 3;
      return 1;
    case MsgKind::PolyPressure:
      e.bytes[0] = uint8_t(0xA0 | ch);
      e.bytes[1] = c.midi.address;
      e.bytes[2] = uint8_t(std::clamp(raw, 0, 127));
      e.size = 3;
      return 1;
    case MsgKind::ChannelPressure:
      e.bytes[0] = uint8_t(0xD0 | ch);
      e.bytes[1] = uint8_t(std::clamp(raw, 0, 127));
      e.size = 2;
      return 1;
    case MsgKind::PitchBend:
      raw = std::clamp(raw, 0, 16383);
      e.bytes[0] = uint8_t(0xE0 | ch);
      e.bytes[1] = uint8_t(raw & 0x7F);
      e.bytes[2] = uint8_t(raw >> 7);
      e.size = 3;
      return 1;
    case MsgKind::ControlChange:
      e.bytes[0] = uint8_t(0xB0 | ch);
      e.bytes[1] = c.midi.address;
      e.size = 3;
      if (c.midi.fourteenBit) {
        raw = std::clamp(raw, 0, 16383);
        e.bytes[2] = uint8_t(raw >> 7);
        out[1].bytes[0] = uint8_t(0xB0 | ch);
        out[1].bytes[1] = c.midi.lsbAddress;
        out[1].bytes[2] = uint8_t(raw & 0x7F);
        out[1].size = 3;
        return 2;
      }
      e.bytes[2] = uint8_t(std::clamp(raw, 0, 127));
      return 1;
    default: return 0;
  }
}

std::vector<uint8_t> encodeDisplay(const Control& c, const char* text) {
  std::vector<uint8_t> msg(c.sysexHeader);
  if (msg.empty()) msg.push_back(0xF0);
  const char* p = text ? text : "";
  for (uint32_t i = 0; i < c.textLength; ++i) {
    uint8_t ch = ' ';
    if (*p) {
      const auto u = static_cast<uint8_t>(*p++);
      if (u >= 0x80) {
        while ((static_cast<uint8_t>(*p) & 0xC0) == 0x80) ++p;  // skip the rest of a UTF-8 sequence
        ch = '?';
      } else {
        ch = u < 0x20 ? uint8_t(' ') : u;
      }
    }
    msg.push_back(ch);
  }
  msg.push_back(0xF7);
  return msg;
}

}  // namespace sq::hwsurface
