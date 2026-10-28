// Squizzy Studio GUI - audio clip inspector of the clip editor panel.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "gui_editor.hpp"

namespace sq::app::gui {
namespace {

enum Field { kNone = -1, kName = 0, kGain, kFadeIn, kFadeOut, kOffset, kMute };

class AudioInspectorImpl final : public AudioInspector {
 public:
  explicit AudioInspectorImpl(GuiContext& ctx) : ctx_(ctx) {}

  void reset() override {
    drag_ = kNone;
    hot_ = kNone;
  }

  void paint(Canvas& c, const Rect& area, const ClipView& clip, Color trackColor) override {
    const Palette& p = ctx_.palette();
    rects_.assign(6, Rect{});
    const double secs = durationSeconds(clip);
    const float labelW = 92, rowH = 26;
    const float colW = std::min(460.0f, std::max(260.0f, area.w * 0.42f));
    float y = area.y + 8;
    const float x = area.x + 20;
    c.drawTextBold("Audio Clip", Rect{x, y, colW, 18}, p.text, 13);
    y += 24;
    auto row = [&](const char* label) {
      c.drawText(label, Rect{x, y, labelW, rowH}, p.dim, 12);
      return Rect{x + labelW, y + 4, colW - labelW, rowH - 8};
    };
    // name
    {
      const Rect r = row("Name");
      rects_[kName] = r;
      c.fillRoundRect(r, 4, hot_ == kName ? p.panel3 : p.panel2);
      c.strokeRoundRect(r, 4, p.border, 1);
      c.drawText(clip.name, Rect{r.x + 8, r.y, r.w - 16, r.h}, p.text, 12);
      y += rowH;
    }
    // source
    {
      c.drawText("Source", Rect{x, y, labelW, rowH}, p.dim, 12);
      std::string src = clip.source.empty() ? std::string("(no file)") : clip.source;
      c.drawText(src, Rect{x + labelW, y, colW - labelW, rowH}, p.text, 11.5f);
      y += rowH;
    }
    // position
    {
      c.drawText("Position", Rect{x, y, labelW, rowH}, p.dim, 12);
      char buf[128];
      std::snprintf(buf, sizeof buf, "%s  -  %s   (%s)", ctx_.formatBars(clip.info.start).c_str(),
                    ctx_.formatBars(clip.info.start + clip.info.length).c_str(), GuiContext::formatTime(secs).c_str());
      c.drawText(buf, Rect{x + labelW, y, colW - labelW, rowH}, p.text, 11.5f);
      y += rowH;
    }
    char buf[64];
    auto slider = [&](Field fld, const char* label, double v, double lo, double hi, const std::string& text) {
      const Rect r = row(label);
      rects_[fld] = r;
      const Rect track{r.x, r.y + r.h / 2 - 2, r.w - 86, 4};
      c.fillRoundRect(track, 2, p.panel3);
      const float t = static_cast<float>(hi > lo ? std::clamp((v - lo) / (hi - lo), 0.0, 1.0) : 0.0);
      c.fillRoundRect(Rect{track.x, track.y, track.w * t, track.h}, 2, trackColor);
      const float kx = track.x + track.w * t;
      c.fillEllipse(Rect{kx - 7, track.y - 5, 14, 14}, (hot_ == fld || drag_ == fld) ? p.text : p.dim);
      c.drawText(text, Rect{track.right() + 10, r.y, 76, r.h}, p.text, 12, TextAlign::Right);
      y += rowH;
    };
    std::snprintf(buf, sizeof buf, "%+.1f dB", clip.info.gain_db);
    slider(kGain, "Gain", clip.info.gain_db, kGainLo, kGainHi, buf);
    const double fadeMax = std::max(0.01, secs);
    std::snprintf(buf, sizeof buf, "%.0f ms", clip.info.fade_in_seconds * 1000);
    slider(kFadeIn, "Fade in", clip.info.fade_in_seconds, 0, fadeMax, buf);
    std::snprintf(buf, sizeof buf, "%.0f ms", clip.info.fade_out_seconds * 1000);
    slider(kFadeOut, "Fade out", clip.info.fade_out_seconds, 0, fadeMax, buf);
    std::snprintf(buf, sizeof buf, "%.3f s", clip.info.source_offset_seconds);
    slider(kOffset, "Offset", clip.info.source_offset_seconds, 0, drag_ == kOffset ? offsetHi_ : offsetRange(clip), buf);
    // mute
    {
      const Rect r0 = row("Muted");
      const Rect r{r0.x, r0.y, 64, r0.h};
      rects_[kMute] = r;
      const bool on = (clip.info.flags & SQ_CLIP_MUTED) != 0;
      draw::button(c, p, r, on ? "Muted" : "Off", hot_ == kMute, on, p.mute);
      y += rowH;
    }
    // envelope preview on the right
    const Rect pv{x + colW + 30, area.y + 36, std::clamp(area.right() - (x + colW + 30) - 20, 0.0f, 480.0f), std::min(150.0f, area.h - 56)};
    if (pv.w > 120 && pv.h > 50) {
      c.fillRoundRect(pv, 6, p.trackBg);
      c.strokeRoundRect(pv, 6, p.border, 1);
      const Rect in = pv.inset(10);
      const float g = static_cast<float>(std::clamp((clip.info.gain_db - kGainLo) / (kGainHi - kGainLo), 0.0, 1.0));
      const float top = in.bottom() - in.h * (0.15f + 0.8f * g);
      const float fi = static_cast<float>(std::clamp(clip.info.fade_in_seconds / fadeMax, 0.0, 1.0)) * in.w;
      const float fo = static_cast<float>(std::clamp(clip.info.fade_out_seconds / fadeMax, 0.0, 1.0)) * in.w;
      std::vector<Point> poly = {Point{in.x, in.bottom()}, Point{in.x + fi, top}, Point{in.right() - fo, top}, Point{in.right(), in.bottom()}};
      c.fillPolygon(poly, trackColor.withAlpha(0.35f));
      c.strokePolyline(poly, trackColor, 1.5f);
      c.drawText("gain / fades", Rect{in.x, in.y, in.w, 14}, p.faint, 10.5f, TextAlign::Right);
    }
  }

  bool mouse(const WidgetMouse& e, const ClipView& clip) override {
    int h = kNone;
    for (size_t i = 0; i < rects_.size(); ++i)
      if (rects_[i].contains(e.x, e.y)) h = static_cast<int>(i);
    switch (e.action) {
      case MouseAction::Hover:
        if (h != hot_) {
          hot_ = h;
          return true;
        }
        return false;
      case MouseAction::Down:
        if (e.button != MouseButton::Left || h == kNone) return false;
        if (h == kName) {
          const sq_guid id = clip.id;
          ctx_.prompt("Rename Clip", clip.name, [this, id](const std::string& n) {
            if (!n.empty()) ctx_.setPropertyString(id, "name", n);
          });
          return true;
        }
        if (h == kMute) {
          ctx_.setPropertyBool(clip.id, "muted", (clip.info.flags & SQ_CLIP_MUTED) == 0);
          return true;
        }
        drag_ = static_cast<Field>(h);
        first_ = true;
        offsetHi_ = offsetRange(clip);
        apply(e.x, clip);
        return true;
      case MouseAction::Move:
        if (drag_ == kNone) return false;
        apply(e.x, clip);
        return true;
      case MouseAction::Up:
        if (drag_ == kNone) return false;
        drag_ = kNone;
        return true;
      case MouseAction::DoubleClick:
        if (h == kGain) ctx_.setProperty(clip.id, "gainDb", 0.0);
        else if (h == kFadeIn) ctx_.setProperty(clip.id, "fadeIn", 0.0);
        else if (h == kFadeOut) ctx_.setProperty(clip.id, "fadeOut", 0.0);
        else return false;
        return true;
      default: return false;
    }
  }

 private:
  static constexpr double kGainLo = -48, kGainHi = 12;

  double durationSeconds(const ClipView& clip) {
    return ctx_.ppqToSeconds(clip.info.start + clip.info.length) - ctx_.ppqToSeconds(clip.info.start);
  }

  static double offsetRange(const ClipView& clip) { return std::max(10.0, std::ceil(clip.info.source_offset_seconds * 2)); }

  void apply(float x, const ClipView& clip) {
    if (drag_ < 0 || static_cast<size_t>(drag_) >= rects_.size()) return;
    const Rect& r = rects_[static_cast<size_t>(drag_)];
    const float w = r.w - 86;
    const double t = std::clamp(static_cast<double>((x - r.x) / std::max(1.0f, w)), 0.0, 1.0);
    const double fadeMax = std::max(0.01, durationSeconds(clip));
    const bool merge = !first_;
    first_ = false;
    switch (drag_) {
      case kGain: ctx_.setProperty(clip.id, "gainDb", std::round((kGainLo + t * (kGainHi - kGainLo)) * 10) / 10, merge); break;
      case kFadeIn: ctx_.setProperty(clip.id, "fadeIn", std::round(t * fadeMax * 1000) / 1000, merge); break;
      case kFadeOut: ctx_.setProperty(clip.id, "fadeOut", std::round(t * fadeMax * 1000) / 1000, merge); break;
      case kOffset: ctx_.setProperty(clip.id, "sourceOffset", std::round(t * offsetHi_ * 1000) / 1000, merge); break;
      default: break;
    }
  }

  GuiContext& ctx_;
  std::vector<Rect> rects_;
  int hot_ = kNone;
  Field drag_ = kNone;
  bool first_ = true;
  double offsetHi_ = 10;
};

}  // namespace

std::unique_ptr<AudioInspector> makeAudioInspector(GuiContext& ctx) { return std::make_unique<AudioInspectorImpl>(ctx); }

}  // namespace sq::app::gui
