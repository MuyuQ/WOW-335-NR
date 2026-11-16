// WOW-335-NR 原生控制中心：即时模式控件与输入状态。
// 控件只绘制并登记命中区域；事件处理在窗口回调中完成，业务动作在下一帧绘制时读取
// clicked id 后执行，避免在绘制过程中改变布局。
#pragma once

#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

#include "cc_gfx.hpp"
#include "cc_theme.hpp"

namespace cc {

enum class ButtonKind { Primary, Secondary, Ghost, Danger };

struct HitTarget {
    std::string id;
    Rect rect;
    bool enabled = true;
};

class Ui {
public:
    Gfx* g = nullptr;
    // 当前鼠标状态（DIP）。
    float mx = -1, my = -1;
    bool mouseDown = false;
    // 本帧要处理的点击（上一帧登记命中区域、本帧开始时结算）。
    std::string clicked;
    std::string hot;
    std::string active;
    bool clickWasEnabled = true;

    void BeginFrame(Gfx* gfx) {
        g = gfx;
        clicked.clear();
        clickWasEnabled = true;
        hits_.clear();
        // 结算挂起的点击。
        if (pendingClick_) {
            clicked = pendingId_;
            clickWasEnabled = pendingEnabled_;
            pendingClick_ = false;
            pendingId_.clear();
        }
        hot.clear();
        for (const auto& h : lastHits_) {
            if (h.enabled && h.rect.Contains(mx, my)) {
                hot = h.id;
                break;
            }
        }
    }
    void EndFrame() {
        lastHits_ = std::move(hits_);
        hits_.clear();
    }

    // 窗口消息调用。坐标为 DIP。
    void OnMouseMove(float x, float y) { mx = x; my = y; }
    void OnMouseDown(float x, float y) {
        mx = x;
        my = y;
        mouseDown = true;
        active.clear();
        for (const auto& h : lastHits_) {
            if (h.enabled && h.rect.Contains(x, y)) {
                active = h.id;
                break;
            }
        }
    }
    void OnMouseUp(float x, float y) {
        mx = x;
        my = y;
        mouseDown = false;
        if (!active.empty()) {
            for (const auto& h : lastHits_) {
                if (h.id == active && h.rect.Contains(x, y)) {
                    pendingClick_ = true;
                    pendingId_ = h.id;
                    pendingEnabled_ = h.enabled;
                    break;
                }
            }
        }
        active.clear();
    }
    void OnMouseLeave() {
        mx = -1;
        my = -1;
        mouseDown = false;
        active.clear();
    }
    void OnCaptureLost() {
        mouseDown = false;
        active.clear();
    }

    // 命中区裁剪：滚动内容超出视口的部分不参与点击，避免被隐藏的控件抢走页脚等外层按钮。
    void SetHitClip(const Rect& area) {
        hitClipActive_ = true;
        hitClip_ = area;
    }
    void ClearHitClip() { hitClipActive_ = false; }
    void AddHit(const char* id, const Rect& r, bool enabled) {
        const Rect rect = hitClipActive_ ? r.Intersect(hitClip_) : r;
        if (rect.Empty()) return;
        HitTarget t;
        t.id = id;
        t.rect = rect;
        t.enabled = enabled;
        hits_.push_back(std::move(t));
    }
    // 覆盖层描完后丢弃被它遮住的命中区域：页面控件仍然照常登记，
    // 但点击落在覆盖层上时不会误触底下看不见的按钮（上层控件在调用后登记）。
    void RemoveHitsInside(const Rect& area) {
        hits_.erase(std::remove_if(hits_.begin(), hits_.end(),
                                   [&area](const HitTarget& hit) {
                                       return area.Contains(hit.rect.CenterX(), hit.rect.CenterY());
                                   }),
                    hits_.end());
    }
    bool Clicked(const char* id) const { return clicked == id; }
    bool ClickedEnabled(const char* id) const { return clicked == id && clickWasEnabled; }
    bool Hot(const char* id) const { return hot == id; }
    bool Active(const char* id) const { return active == id; }
    bool Hovering() const { return !hot.empty(); }
    size_t HitCount() const { return lastHits_.size(); }
    const std::vector<HitTarget>& LastHits() const { return lastHits_; }

    // ---- 控件 ----
    bool Button(const char* id, const Rect& r, const std::wstring& label, ButtonKind kind, bool enabled,
                float fontSize = theme::kFontBody) {
        const bool hovered = enabled && rectContains(r);
        const bool pressed = enabled && active == id;
        unsigned bg = theme::kSurface;
        unsigned fg = theme::kText2;
        unsigned border = theme::kLineSoft;
        bool drawBorder = true;
        switch (kind) {
            case ButtonKind::Primary:
                bg = pressed ? theme::kAccent : (hovered ? theme::kAccentBright : theme::kAccent);
                fg = theme::kInk;
                border = theme::kAccent;
                drawBorder = false;
                break;
            case ButtonKind::Secondary:
                bg = pressed ? theme::kSurface3 : (hovered ? theme::kSurface2 : theme::kSurface);
                fg = enabled ? theme::kText : theme::kText3;
                break;
            case ButtonKind::Ghost:
                bg = pressed ? theme::kSurface2 : (hovered ? theme::kSurface : 0x00000000);
                fg = theme::kText3;
                border = theme::kLineSoft;
                drawBorder = hovered || pressed;
                break;
            case ButtonKind::Danger:
                bg = pressed ? theme::kDanger : (hovered ? theme::kDangerBright : theme::kDanger);
                fg = theme::kText;
                drawBorder = false;
                break;
        }
        if (!enabled) {
            bg = theme::kSurface;
            fg = theme::kMuted;
            border = theme::kLineSoft;
        }
        if (auto* gfx = g) {
            if (bg != 0x00000000 || kind != ButtonKind::Ghost) gfx->FillRound(r, theme::kFieldRadius, theme::Color(bg));
            if (drawBorder) gfx->StrokeRound(r, theme::kFieldRadius, theme::Color(border));
            auto* fmt = gfx->Format(theme::FontUi(), fontSize, 600, HAlign::Center, VAlign::Middle);
            gfx->Text(r.Inset(6, 0), label, fmt, theme::Color(fg));
        }
        AddHit(id, r, enabled);
        (void)hovered;
        if (clicked == id) return clickWasEnabled;
        return false;
    }

    // 状态胶囊：圆点 + 标签 + 值。
    void Chip(const char* id, const Rect& r, const std::wstring& label, const std::wstring& value, unsigned dot,
              unsigned valueColor) {
        if (auto* gfx = g) {
            gfx->FillRound(r, r.Height() * 0.5f, theme::Color(theme::kSurface));
            const float cy = r.CenterY();
            const float dotX = r.left + 12.0f;
            gfx->FillEllipse(dotX, cy, 3.2f, 3.2f, theme::Color(dot));
            auto* smallFmt = gfx->Format(theme::FontUi(), theme::kFontSmall, 400, HAlign::Left, VAlign::Middle);
            auto* valueFmt = gfx->Format(theme::FontUi(), theme::kFontSmall, 400, HAlign::Left, VAlign::Middle);
            const float x = dotX + 8.0f;
            const D2D1_SIZE_F labelSize = gfx->Measure(label, smallFmt);
            gfx->Text(Rect(x, r.top, x + labelSize.width + 2, r.bottom), label, smallFmt, theme::Color(theme::kText2));
            const float vx = x + labelSize.width + 8.0f;
            gfx->Text(Rect(vx, r.top, r.right - 10, r.bottom), value, valueFmt, theme::Color(valueColor));
        }
        AddHit(id, r, false);
    }

    void Card(const Rect& r) {
        if (auto* gfx = g) {
            gfx->FillRound(r, theme::kCardRadius, theme::Color(theme::kSurface));
            gfx->StrokeRound(r, theme::kCardRadius, theme::Color(theme::kLineSoft));
        }
    }

    // 区块标题：金色分区量规标记 + 粗体标题 + 可选说明。
    void SectionTitle(const Rect& r, const std::wstring& title, const std::wstring& note) {
        if (auto* gfx = g) {
            gfx->GaugeMark(r.left + 4.5f, r.CenterY(), 3.6f, theme::Color(theme::kAccent),
                           theme::Color(theme::kAccentBright), theme::Color(theme::kJade),
                           theme::Color(theme::kAccentLine), false);
            auto* bold = gfx->Format(theme::FontUi(), theme::kFontBody, 700, HAlign::Left, VAlign::Middle);
            const D2D1_SIZE_F size = gfx->Measure(title, bold);
            gfx->Text(Rect(r.left + 14.0f, r.top, r.left + 14.0f + size.width + 2, r.bottom), title, bold,
                      theme::Color(theme::kText));
            if (!note.empty()) {
                auto* smallFmt = gfx->Format(theme::FontUi(), theme::kFontSmall, 400, HAlign::Left, VAlign::Middle);
                gfx->Text(Rect(r.left + 14.0f + size.width + 10.0f, r.top, r.right, r.bottom), note, smallFmt,
                          theme::Color(theme::kText3));
            }
        }
    }

    // 键盘提示/空状态文字。
    void Muted(const Rect& r, const std::wstring& text, float size = theme::kFontSmall) {
        if (auto* gfx = g) {
            auto* fmt = gfx->Format(theme::FontUi(), size, 400, HAlign::Left, VAlign::Top, true);
            gfx->Text(r, text, fmt, theme::Color(theme::kText3));
        }
    }

private:
    bool rectContains(const Rect& r) const { return r.Contains(mx, my); }
    std::vector<HitTarget> hits_;
    std::vector<HitTarget> lastHits_;
    bool hitClipActive_ = false;
    Rect hitClip_;
    bool pendingClick_ = false;
    bool pendingEnabled_ = true;
    std::string pendingId_;
};

}  // namespace cc
