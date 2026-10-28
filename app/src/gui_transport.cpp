// Squizzy Studio GUI - transport bar: transport buttons (drawn icons), position (bars + time),
// tempo (drag / double-click to type / tap), time signature, loop / metronome / follow toggles,
// undo / redo, CPU load and the master meter.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gui_panels.hpp"

namespace sq::app::gui {
namespace {

enum class Icon { ToStart, Play, Stop, Record, Loop, Metronome, Undo, Redo, Tap };

void drawIcon(Canvas& c, Icon icon, const Rect& r, Color col) {
  const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  const float s = std::min(r.w, r.h) * 0.36f;
  switch (icon) {
    case Icon::ToStart:
      c.fillRect(Rect{cx - s, cy - s, 2.2f, 2 * s}, col);
      c.fillPolygon({Point{cx + s, cy - s}, Point{cx + s, cy + s}, Point{cx - s + 3, cy}}, col);
      break;
    case Icon::Play: c.fillPolygon({Point{cx - s * 0.8f, cy - s}, Point{cx + s, cy}, Point{cx - s * 0.8f, cy + s}}, col); break;
    case Icon::Stop: c.fillRoundRect(Rect{cx - s * 0.85f, cy - s * 0.85f, s * 1.7f, s * 1.7f}, 2, col); break;
    case Icon::Record: c.fillEllipse(Rect{cx - s, cy - s, 2 * s, 2 * s}, col); break;
    case Icon::Loop: {
      c.drawArc(Point{cx, cy}, s, 30, 300, col, 2);
      c.fillPolygon({Point{cx + s * 0.25f, cy - s * 1.45f}, Point{cx + s * 0.95f, cy - s * 0.85f}, Point{cx + s * 0.1f, cy - s * 0.45f}}, col);
      break;
    }
    case Icon::Metronome:
      c.fillPolygon({Point{cx - s * 0.75f, cy + s}, Point{cx - s * 0.3f, cy - s}, Point{cx + s * 0.3f, cy - s}, Point{cx + s * 0.75f, cy + s}}, col);
      c.drawLine(Point{cx, cy + s * 0.5f}, Point{cx + s * 0.85f, cy - s * 1.0f}, Color{0, 0, 0, 0.6f}, 1.6f);
      break;
    case Icon::Undo:
    case Icon::Redo: {
      const float dir = icon == Icon::Undo ? -1.0f : 1.0f;
      c.drawArc(Point{cx + dir * -s * 0.1f, cy + s * 0.2f}, s * 0.75f, dir < 0 ? -10.0f : -170.0f, dir < 0 ? 200.0f : -200.0f, col, 1.8f);
      const float ax = cx + dir * s * 0.65f;
      c.fillPolygon({Point{ax, cy - s * 0.95f}, Point{ax + dir * s * 0.65f, cy - s * 0.2f}, Point{ax - dir * s * 0.25f, cy + s * 0.05f}}, col);
      break;
    }
    case Icon::Tap: c.drawText("TAP", r, col, 10.5f, TextAlign::Center); break;
  }
}

class TransportBar final : public Widget {
 public:
  explicit TransportBar(GuiContext& ctx) : Widget("TransportBar"), ctx_(ctx) {}

  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.panel);
    c.fillRect(Rect{f.x, f.bottom() - 1, f.w, 1}, p.border);
    const auto ts = ctx_.transport();
    const bool playing = (ts.flags & SQ_TRANSPORT_PLAYING) != 0;
    const bool recording = (ts.flags & SQ_TRANSPORT_RECORDING) != 0;
    hits_.clear();
    const float bh = f.h - 16, by = f.y + 8;
    float x = f.x + 10;
    auto iconButton = [&](Icon icon, int id, bool on, Color onCol, const char* tip) {
      Rect r{x, by, bh + 6, bh};
      const bool hot = hot_ == id;
      c.fillRoundRect(r, 5, on ? onCol.withAlpha(0.9f) : hot ? p.panel3 : p.panel2);
      if (!on) c.strokeRoundRect(r, 5, p.border, 1);
      drawIcon(c, icon, r.inset(6), on ? (p.dark ? Color::rgb(0x111214) : Color::rgb(0xFFFFFF)) : (icon == Icon::Record ? p.rec : p.text));
      hits_.push_back(Hit{r, id, tip});
      x += r.w + 4;
    };
    iconButton(Icon::ToStart, kToStart, false, p.accent, "Go to start (Home)");
    iconButton(Icon::Stop, kStop, false, p.accent, "Stop (Space)");
    iconButton(Icon::Play, kPlay, playing && !recording, p.accent, "Play (Space)");
    iconButton(Icon::Record, kRecord, recording, p.rec, "Record (R)");
    x += 8;
    iconButton(Icon::Loop, kLoop, (ts.flags & SQ_TRANSPORT_LOOPING) != 0, p.accent, "Loop (L)");
    iconButton(Icon::Metronome, kMetronome, (ts.flags & SQ_TRANSPORT_METRONOME) != 0, p.accent, "Metronome (C)");
    x += 12;

    // position display
    Rect pos{x, by - 2, 190, bh + 4};
    c.fillRoundRect(pos, 6, p.bg);
    c.strokeRoundRect(pos, 6, p.border, 1);
    c.drawTextBold(ctx_.formatBars(ts.position), Rect{pos.x + 12, pos.y + 2, pos.w - 24, pos.h * 0.62f}, recording ? p.rec : p.accent, 19);
    c.drawText(GuiContext::formatTime(ts.seconds), Rect{pos.x + 12, pos.y + pos.h * 0.58f, pos.w - 24, pos.h * 0.38f}, p.dim, 11);
    hits_.push_back(Hit{pos, kPosition, "Song position (click to type a bar)"});
    x = pos.right() + 10;

    // tempo
    Rect tempo{x, by - 2, 104, bh + 4};
    c.fillRoundRect(tempo, 6, hot_ == kTempo || dragTempo_ ? p.panel3 : p.bg);
    c.strokeRoundRect(tempo, 6, p.border, 1);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", dragTempo_ ? dragBpm_ : ts.bpm);
    c.drawTextBold(buf, Rect{tempo.x + 10, tempo.y + 2, tempo.w - 20, tempo.h * 0.62f}, p.text, 17);
    c.drawText("BPM", Rect{tempo.x + 10, tempo.y + tempo.h * 0.58f, tempo.w - 20, tempo.h * 0.38f}, p.dim, 10.5f);
    hits_.push_back(Hit{tempo, kTempo, "Tempo: drag up/down (Shift = fine), double-click to type"});
    x = tempo.right() + 4;
    iconButton(Icon::Tap, kTap, false, p.accent, "Tap tempo");
    x += 4;

    // signature
    Rect sig{x, by - 2, 64, bh + 4};
    c.fillRoundRect(sig, 6, hot_ == kSignature ? p.panel3 : p.bg);
    c.strokeRoundRect(sig, 6, p.border, 1);
    std::snprintf(buf, sizeof buf, "%u/%u", ts.numerator ? ts.numerator : 4, ts.denominator ? ts.denominator : 4);
    c.drawTextBold(buf, Rect{sig.x, sig.y + 2, sig.w, sig.h * 0.62f}, p.text, 16, TextAlign::Center);
    c.drawText("TIME", Rect{sig.x, sig.y + sig.h * 0.58f, sig.w, sig.h * 0.38f}, p.dim, 10.5f, TextAlign::Center);
    hits_.push_back(Hit{sig, kSignature, "Time signature (click to change)"});
    x = sig.right() + 16;

    iconButton(Icon::Undo, kUndo, false, p.accent, "Undo (Ctrl+Z)");
    iconButton(Icon::Redo, kRedo, false, p.accent, "Redo (Ctrl+Y)");

    // right side: master meter + CPU
    const float mw = std::min(260.0f, f.w * 0.18f);
    Rect meterR{f.right() - mw - 12, by + 4, mw, bh - 8};
    float pk[2] = {0, 0};
    const uint32_t n = ctx_.meter(sq_guid{}, pk, nullptr, 2);
    if (n == 1) pk[1] = pk[0];
    const auto now = std::chrono::steady_clock::now();
    for (int ch = 0; ch < 2; ++ch) {
      const float decay = std::pow(0.5f, static_cast<float>(std::chrono::duration<double>(now - lastMeter_).count()) / 0.25f);
      level_[ch] = std::max(pk[ch], level_[ch] * decay);
      if (pk[ch] >= hold_[ch] || now > holdUntil_[ch]) {
        hold_[ch] = pk[ch];
        holdUntil_[ch] = now + std::chrono::milliseconds(1200);
      }
      if (pk[ch] >= 1.0f) clip_ = true;
    }
    lastMeter_ = now;
    const float rowH = (meterR.h - 2) / 2;
    for (int ch = 0; ch < 2; ++ch) hMeter(c, p, Rect{meterR.x, meterR.y + static_cast<float>(ch) * (rowH + 2), meterR.w - 16, rowH}, level_[ch], hold_[ch]);
    Rect clipR{meterR.right() - 12, meterR.y, 12, meterR.h};
    c.fillRoundRect(clipR, 2, clip_ ? p.meterHigh : p.panel2);
    hits_.push_back(Hit{clipR, kClip, "Clip indicator (click to reset)"});
    c.drawText("MASTER", Rect{meterR.x, f.y, meterR.w, 10}, p.faint, 8.5f);
    if (IAudioEngine* e = ctx_.shell().engine()) {
      sq_engine_status st{};
      st.struct_size = sizeof st;
      if (e->getStatus(&st) == SQ_OK) {
        std::snprintf(buf, sizeof buf, "CPU %d%%", static_cast<int>(st.cpu_load * 100 + 0.5));
        c.drawText(buf, Rect{meterR.x - 84, by, 76, bh}, st.cpu_load > 0.8 ? p.rec : p.dim, 11, TextAlign::Right);
      }
    }
    // tooltip line for the hovered control
    if (hot_ > 0)
      for (const auto& h : hits_)
        if (h.id == hot_ && h.tip) c.drawText(h.tip, Rect{x + 16, by, std::max(0.0f, meterR.x - x - 110), bh}, p.faint, 11);

    // finish pending tap-tempo / hover bookkeeping
  }

  bool onMouse(const WidgetMouse& e) override {
    int h = 0;
    for (const auto& hit : hits_)
      if (hit.r.contains(e.x, e.y)) h = hit.id;
    switch (e.action) {
      case MouseAction::Hover:
        if (h != hot_) {
          hot_ = h;
          ctx_.invalidate();
        }
        return true;
      case MouseAction::Leave:
        hot_ = 0;
        return false;
      case MouseAction::Down:
        if (e.button != MouseButton::Left || h == 0) return h != 0;
        pressed_ = h;
        if (h == kTempo) {
          dragTempo_ = true;
          dragStartY_ = e.y;
          dragBpm_ = startBpm_ = ctx_.transport().bpm;
        }
        return true;
      case MouseAction::Move:
        if (dragTempo_) {
          const double per = (e.modifiers & kModShift) ? 0.01 : 0.25;
          dragBpm_ = std::clamp(startBpm_ + static_cast<double>(dragStartY_ - e.y) * per, 20.0, 400.0);
          dragBpm_ = std::round(dragBpm_ * 100.0) / 100.0;
          ctx_.invalidate();
        }
        return true;
      case MouseAction::Up: {
        const int was = pressed_;
        pressed_ = 0;
        if (dragTempo_) {
          dragTempo_ = false;
          if (std::fabs(dragBpm_ - startBpm_) > 1e-6) setTempo(dragBpm_);
          return true;
        }
        if (was != 0 && was == h) click(h);
        return true;
      }
      case MouseAction::DoubleClick:
        if (h == kTempo) {
          char buf[32];
          std::snprintf(buf, sizeof buf, "%.2f", ctx_.transport().bpm);
          ctx_.prompt("Tempo (BPM)", buf, [this](const std::string& v) {
            const double bpm = std::atof(v.c_str());
            if (bpm >= 10 && bpm <= 999) setTempo(bpm);
            else ctx_.setStatus("tempo must be between 10 and 999 BPM");
          });
          return true;
        }
        return h != 0;
      case MouseAction::Wheel:
        if (h == kTempo) {
          setTempo(std::clamp(std::round(ctx_.transport().bpm) + (e.wheel > 0 ? 1.0 : -1.0), 20.0, 400.0));
          return true;
        }
        return false;
      default: return false;
    }
  }

 private:
  enum { kToStart = 1, kStop, kPlay, kRecord, kLoop, kMetronome, kPosition, kTempo, kTap, kSignature, kUndo, kRedo, kClip };
  struct Hit {
    Rect r;
    int id;
    const char* tip;
  };

  static void hMeter(Canvas& c, const Palette& p, const Rect& r, float lin, float hold) {
    c.fillRoundRect(r, 2, p.bg);
    auto norm = [](float v) {
      if (v <= 0) return 0.0f;
      return std::clamp((20.0f * std::log10(v) + 60.0f) / 66.0f, 0.0f, 1.0f);
    };
    const float w = r.w * norm(lin);
    const float x6 = r.w * norm(0.5f), x0 = r.w * norm(1.0f);
    if (w > 0) c.fillRect(Rect{r.x, r.y, std::min(w, x6), r.h}, p.meterLow);
    if (w > x6) c.fillRect(Rect{r.x + x6, r.y, std::min(w, x0) - x6, r.h}, p.meterMid);
    if (w > x0) c.fillRect(Rect{r.x + x0, r.y, w - x0, r.h}, p.meterHigh);
    if (hold > 0) c.fillRect(Rect{r.x + r.w * norm(hold) - 1, r.y, 2, r.h}, p.text.withAlpha(0.8f));
    for (float db : {-48.0f, -24.0f, -12.0f, -6.0f, 0.0f}) {
      const float xx = r.x + r.w * std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
      c.fillRect(Rect{xx, r.y, 1, r.h}, Color{0, 0, 0, 0.35f});
    }
  }

  void setTempo(double bpm) {
    auto a = sq::makeAttributes();
    a->setDouble("bpm", bpm);
    if (SQ_SUCCEEDED(ctx_.shell().execute("Song.SetTempo", a.get()))) {
      ctx_.setStatus("Tempo: " + std::to_string(static_cast<int>(std::lround(bpm))) + " BPM");
      ctx_.invalidate();
      return;
    }
    auto p = sq::makeAttributes();
    p->setDouble("position", 0.0);
    p->setDouble("bpm", bpm);
    if (SQ_SUCCEEDED(ctx_.exec(sq::doc::kSetTempo, p.get()))) ctx_.setStatus("Tempo: " + std::to_string(static_cast<int>(std::lround(bpm))) + " BPM");
  }

  void setSignature(uint32_t num, uint32_t den) {
    auto a = sq::makeAttributes();
    a->setInt("num", num);
    a->setInt("den", den);
    if (SQ_SUCCEEDED(ctx_.shell().execute("Song.SetSignature", a.get()))) return;
    auto p = sq::makeAttributes();
    p->setDouble("position", 0.0);
    p->setInt("numerator", num);
    p->setInt("denominator", den);
    ctx_.exec(sq::doc::kSetSignature, p.get());
  }

  void tap() {
    const auto now = std::chrono::steady_clock::now();
    if (!taps_.empty() && now - taps_.back() > std::chrono::seconds(2)) taps_.clear();
    taps_.push_back(now);
    if (taps_.size() > 8) taps_.erase(taps_.begin());
    if (taps_.size() >= 3) {
      const double secs = std::chrono::duration<double>(taps_.back() - taps_.front()).count() / static_cast<double>(taps_.size() - 1);
      if (secs > 0.1) setTempo(std::clamp(std::round(60.0 / secs * 10.0) / 10.0, 20.0, 400.0));
    } else {
      ctx_.setStatus("Tap tempo: keep tapping...");
    }
  }

  void click(int id) {
    const auto ts = ctx_.transport();
    switch (id) {
      case kToStart: ctx_.run("Transport.Rewind"); break;
      case kStop:
        if (!(ts.flags & SQ_TRANSPORT_PLAYING)) ctx_.run("Transport.Rewind");  // second stop returns to start
        else ctx_.run("Transport.Stop");
        break;
      case kPlay: ctx_.run((ts.flags & SQ_TRANSPORT_PLAYING) ? "Transport.Stop" : "Transport.Play"); break;
      case kRecord: ctx_.run("Transport.Record"); break;
      case kLoop: ctx_.run("Transport.ToggleLoop"); break;
      case kMetronome: ctx_.run("Transport.ToggleMetronome"); break;
      case kUndo: ctx_.run("Edit.Undo"); break;
      case kRedo: ctx_.run("Edit.Redo"); break;
      case kTap: tap(); break;
      case kClip:
        clip_ = false;
        hold_[0] = hold_[1] = 0;
        break;
      case kSignature: {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%u/%u", ts.numerator ? ts.numerator : 4, ts.denominator ? ts.denominator : 4);
        ctx_.prompt("Time Signature (e.g. 4/4, 6/8, 7/8)", buf, [this](const std::string& v) {
          const size_t sl = v.find('/');
          const int num = std::atoi(v.substr(0, sl).c_str());
          const int den = sl == std::string::npos ? 4 : std::atoi(v.substr(sl + 1).c_str());
          const bool denOk = den == 1 || den == 2 || den == 4 || den == 8 || den == 16 || den == 32;
          if (num < 1 || num > 32 || !denOk) {
            ctx_.setStatus("invalid time signature: " + v);
            return;
          }
          setSignature(static_cast<uint32_t>(num), static_cast<uint32_t>(den));
        });
        break;
      }
      case kPosition:
        ctx_.prompt("Go to bar", ctx_.formatBars(ts.position).substr(0, ctx_.formatBars(ts.position).find('.')), [this](const std::string& v) {
          const int bar = std::atoi(v.c_str());
          if (bar < 1) return;
          const auto t = ctx_.transport();
          const uint32_t num = t.numerator ? t.numerator : 4, den = t.denominator ? t.denominator : 4;
          const double ppq = static_cast<double>(bar - 1) * num * 4.0 / den;
          if (ITransport* tr = ctx_.transportIf()) tr->locate(ppq);
          ctx_.setCursor(ppq);
        });
        break;
      default: break;
    }
  }

  GuiContext& ctx_;
  std::vector<Hit> hits_;
  int hot_ = 0, pressed_ = 0;
  bool dragTempo_ = false;
  float dragStartY_ = 0;
  double dragBpm_ = 120, startBpm_ = 120;
  std::vector<std::chrono::steady_clock::time_point> taps_;
  float level_[2] = {0, 0}, hold_[2] = {0, 0};
  std::chrono::steady_clock::time_point holdUntil_[2]{};
  std::chrono::steady_clock::time_point lastMeter_ = std::chrono::steady_clock::now();
  bool clip_ = false;
};

}  // namespace

std::unique_ptr<Widget> makeTransportBar(GuiContext& ctx) { return std::make_unique<TransportBar>(ctx); }

}  // namespace sq::app::gui
