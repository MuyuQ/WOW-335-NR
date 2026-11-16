// WOW-335-NR 原生控制中心：Direct2D / DirectWrite 绘制层。
// 只负责绘制与文字排版；不读文件、不做系统查询。设备丢失时由调用方重建资源。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace cc {

struct Rect {
    float left = 0, top = 0, right = 0, bottom = 0;
    Rect() = default;
    Rect(float l, float t, float r, float b) : left(l), top(t), right(r), bottom(b) {}
    static Rect FromLTWH(float l, float t, float w, float h) { return Rect(l, t, l + w, t + h); }
    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
    float CenterX() const { return (left + right) * 0.5f; }
    float CenterY() const { return (top + bottom) * 0.5f; }
    D2D1_RECT_F D2D() const { return D2D1::RectF(left, top, right, bottom); }
    bool Contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
    Rect Inset(float dx, float dy) const { return Rect(left + dx, top + dy, right - dx, bottom - dy); }
    Rect Offset(float dx, float dy) const { return Rect(left + dx, top + dy, right + dx, bottom + dy); }
    Rect Intersect(const Rect& o) const {
        return Rect(std::max(left, o.left), std::max(top, o.top), std::min(right, o.right), std::min(bottom, o.bottom));
    }
    bool Empty() const { return right <= left || bottom <= top; }
};

enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Middle, Bottom };

class Gfx {
public:
    Gfx() = default;
    ~Gfx() { Shutdown(); }
    Gfx(const Gfx&) = delete;
    Gfx& operator=(const Gfx&) = delete;

    bool Initialize(HWND hwnd, float dpi);
    void Shutdown();
    bool Resize(UINT widthPx, UINT heightPx);
    void SetDpi(float dpi);
    float Dpi() const { return dpi_; }
    // 逻辑像素（DIP，96dpi 基准）与物理像素互转。
    float Dip(int px) const { return static_cast<float>(px) * 96.0f / dpi_; }
    int Px(float dip) const { return static_cast<int>(dip * dpi_ / 96.0f + 0.5f); }
    float ClientWidthDip() const { return clientW_ * 96.0f / dpi_; }
    float ClientHeightDip() const { return clientH_ * 96.0f / dpi_; }

    bool BeginDraw();   // 设备丢失时返回 false（调用方跳过本帧）
    void EndDraw();     // 内部处理 D2DERR_RECREATE_TARGET
    bool DeviceReady() const { return rt_ != nullptr; }

    ID2D1HwndRenderTarget* RT() { return rt_; }

    // 基础绘制（坐标均为 DIP）。
    void Clear(D2D1_COLOR_F color);
    void FillRect(const Rect& r, D2D1_COLOR_F color);
    void FillRound(const Rect& r, float radius, D2D1_COLOR_F color);
    void StrokeRound(const Rect& r, float radius, D2D1_COLOR_F color, float width = 1.0f);
    void FillEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F color);
    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float width = 1.0f);
    void Diamond(float cx, float cy, float half, D2D1_COLOR_F color);
    // 应用图标同款「分区量规」标记：金色弧 + 玉色高性能分区 + 指针。
    // detail=true 时额外绘制外圈刻度与分区起点标记（标题栏用），小尺寸区块标题用 false。
    void GaugeMark(float cx, float cy, float radius, D2D1_COLOR_F gold, D2D1_COLOR_F goldBright,
                   D2D1_COLOR_F jade, D2D1_COLOR_F dim, bool detail);
    // 竖直渐隐/分隔等简单水平线。
    void HLine(const Rect& area, float y, D2D1_COLOR_F color);
    // 页面过渡：按画面中心缩放 + 平移，并提供整页不透明度（用于淡入类过渡）。
    void SetPageTransform(float dx, float dy, float scale, float cx, float cy);
    void EndPageTransform();
    void PushOpacity(float opacity);
    void PopOpacity();

    // 文字。
    IDWriteTextFormat* Format(const wchar_t* family, float size, int weight = 400,
                              HAlign halign = HAlign::Left, VAlign valign = VAlign::Top, bool wrap = false);
    void Text(const Rect& r, const std::wstring& text, IDWriteTextFormat* fmt, D2D1_COLOR_F color);
    D2D1_SIZE_F Measure(const std::wstring& text, IDWriteTextFormat* fmt, float maxWidth = 1.0e6f);
    std::wstring Ellipsize(const std::wstring& text, IDWriteTextFormat* fmt, float maxWidth);
    // 多段不同样式文字并排（同一基线/竖直居中），返回绘制结束的 x。
    float TextRow(const Rect& r, const std::wstring& text, IDWriteTextFormat* fmt, D2D1_COLOR_F color,
                  VAlign valign = VAlign::Middle);
    void PushClip(const Rect& r);
    void PopClip();
    int ClipDepth() const { return static_cast<int>(clips_.size()); }

private:
    void DiscardTarget();
    bool CreateTarget();
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F color);
    void ReleaseBrushes();
    void ReleaseFormats();

    HWND hwnd_ = nullptr;
    ID2D1Factory* factory_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    float dpi_ = 96.0f;
    UINT clientW_ = 0, clientH_ = 0;
    std::vector<ID2D1SolidColorBrush*> brushes_;
    std::vector<unsigned> brushKeys_;
    struct FormatKey {
        const wchar_t* family = nullptr;
        float size = 0;
        int weight = 400;
        HAlign halign = HAlign::Left;
        VAlign valign = VAlign::Top;
        bool wrap = false;
        bool operator==(const FormatKey& o) const {
            return size == o.size && weight == o.weight && halign == o.halign && valign == o.valign &&
                   wrap == o.wrap && (family == o.family || (family && o.family && wcscmp(family, o.family) == 0));
        }
    };
    std::vector<IDWriteTextFormat*> formats_;
    std::vector<FormatKey> formatKeys_;
    std::vector<Rect> clips_;
};

namespace gfxdetail {
inline unsigned ColorKey(D2D1_COLOR_F c) {
    auto q = [](float v) { return static_cast<unsigned>(std::max(0.0f, std::min(1.0f, v)) * 255.0f + 0.5f); };
    return (q(c.r) << 24) | (q(c.g) << 16) | (q(c.b) << 8) | q(c.a);
}
inline bool SameColor(D2D1_COLOR_F a, D2D1_COLOR_F b) {
    return gfxdetail::ColorKey(a) == gfxdetail::ColorKey(b);
}
}  // namespace gfxdetail

inline bool Gfx::Initialize(HWND hwnd, float dpi) {
    hwnd_ = hwnd;
    dpi_ = dpi > 0 ? dpi : 96.0f;
    if (!factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_))) return false;
    if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(&dwrite_))))
        return false;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    clientW_ = static_cast<UINT>(std::max<LONG>(0, rc.right - rc.left));
    clientH_ = static_cast<UINT>(std::max<LONG>(0, rc.bottom - rc.top));
    return CreateTarget();
}

inline void Gfx::Shutdown() {
    DiscardTarget();
    ReleaseBrushes();
    ReleaseFormats();
    if (dwrite_) { dwrite_->Release(); dwrite_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
}

inline void Gfx::DiscardTarget() {
    ReleaseBrushes();
    if (rt_) { rt_->Release(); rt_ = nullptr; }
}

inline bool Gfx::CreateTarget() {
    if (!factory_ || !hwnd_) return false;
    if (clientW_ == 0 || clientH_ == 0) {
        RECT rc{};
        if (GetClientRect(hwnd_, &rc)) {
            clientW_ = static_cast<UINT>(std::max<LONG>(0, rc.right - rc.left));
            clientH_ = static_cast<UINT>(std::max<LONG>(0, rc.bottom - rc.top));
        }
    }
    if (clientW_ == 0 || clientH_ == 0) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = dpi_;
    props.dpiY = dpi_;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps =
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(clientW_, clientH_), D2D1_PRESENT_OPTIONS_NONE);
    HRESULT hr = factory_->CreateHwndRenderTarget(props, hwndProps, &rt_);
    if (FAILED(hr)) {
        rt_ = nullptr;
        return false;
    }
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return true;
}

inline bool Gfx::Resize(UINT widthPx, UINT heightPx) {
    clientW_ = widthPx;
    clientH_ = heightPx;
    if (!rt_) return CreateTarget();
    if (widthPx == 0 || heightPx == 0) return true;
    HRESULT hr = rt_->Resize(D2D1::SizeU(widthPx, heightPx));
    if (FAILED(hr)) {
        DiscardTarget();
        return CreateTarget();
    }
    return true;
}

inline void Gfx::SetDpi(float dpi) {
    if (dpi <= 0) dpi = 96.0f;
    if (dpi == dpi_) return;
    dpi_ = dpi;
    if (rt_) {
        rt_->SetDpi(dpi_, dpi_);
        RECT rc{};
        if (GetClientRect(hwnd_, &rc)) {
            clientW_ = static_cast<UINT>(std::max<LONG>(0, rc.right - rc.left));
            clientH_ = static_cast<UINT>(std::max<LONG>(0, rc.bottom - rc.top));
            rt_->Resize(D2D1::SizeU(clientW_, clientH_));
        }
    }
}

inline bool Gfx::BeginDraw() {
    if (!rt_ && !CreateTarget()) return false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    clips_.clear();
    return true;
}

inline void Gfx::EndDraw() {
    if (!rt_) return;
    const HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) DiscardTarget();
    else if (FAILED(hr)) DiscardTarget();
}

inline ID2D1SolidColorBrush* Gfx::Brush(D2D1_COLOR_F color) {
    if (!rt_) return nullptr;
    const unsigned key = gfxdetail::ColorKey(color);
    for (size_t i = 0; i < brushKeys_.size(); ++i) {
        if (brushKeys_[i] == key) return brushes_[i];
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(rt_->CreateSolidColorBrush(color, &brush))) return nullptr;
    brushes_.push_back(brush);
    brushKeys_.push_back(key);
    return brush;
}

inline void Gfx::ReleaseBrushes() {
    for (auto* b : brushes_) b->Release();
    brushes_.clear();
    brushKeys_.clear();
}

inline void Gfx::ReleaseFormats() {
    for (auto* f : formats_) f->Release();
    formats_.clear();
    formatKeys_.clear();
}

inline void Gfx::Clear(D2D1_COLOR_F color) {
    if (!rt_) return;
    rt_->Clear(color);
}

inline void Gfx::FillRect(const Rect& r, D2D1_COLOR_F color) {
    if (!rt_ || r.Empty()) return;
    if (auto* b = Brush(color)) rt_->FillRectangle(r.D2D(), b);
}

inline void Gfx::FillRound(const Rect& r, float radius, D2D1_COLOR_F color) {
    if (!rt_ || r.Empty()) return;
    const float maxR = std::min(r.Width(), r.Height()) * 0.5f;
    const float rr = std::max(0.0f, std::min(radius, maxR));
    if (auto* b = Brush(color)) rt_->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(), rr, rr), b);
}

inline void Gfx::StrokeRound(const Rect& r, float radius, D2D1_COLOR_F color, float width) {
    if (!rt_ || r.Empty()) return;
    const float maxR = std::min(r.Width(), r.Height()) * 0.5f;
    const float rr = std::max(0.0f, std::min(radius, maxR));
    if (auto* b = Brush(color)) {
        // 描边居中，内缩半像素让边框落在像素格内。
        const float inset = width * 0.5f;
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(r.Inset(inset, inset).D2D(), rr, rr), b, width);
    }
}

inline void Gfx::FillEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F color) {
    if (!rt_ || rx <= 0 || ry <= 0) return;
    if (auto* b = Brush(color)) rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b);
}

inline void Gfx::Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float width) {
    if (!rt_) return;
    if (auto* b = Brush(color)) rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, width);
}

inline void Gfx::Diamond(float cx, float cy, float half, D2D1_COLOR_F color) {
    if (!rt_ || half <= 0) return;
    if (auto* b = Brush(color)) {
        const float side = half * 1.41421356f;
        D2D1::Matrix3x2F saved = D2D1::Matrix3x2F::Identity();
        rt_->GetTransform(&saved);
        rt_->SetTransform(D2D1::Matrix3x2F::Rotation(45.0f, D2D1::Point2F(cx, cy)));
        rt_->FillRectangle(D2D1::RectF(cx - side * 0.5f, cy - side * 0.5f, cx + side * 0.5f, cy + side * 0.5f), b);
        rt_->SetTransform(saved);
    }
}

inline void Gfx::SetPageTransform(float dx, float dy, float scale, float cx, float cy) {
    if (!rt_) return;
    if (scale < 0.001f) scale = 1.0f;
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(cx, cy)) *
                      D2D1::Matrix3x2F::Translation(dx, dy));
}

inline void Gfx::EndPageTransform() {
    if (rt_) rt_->SetTransform(D2D1::Matrix3x2F::Identity());
}

inline void Gfx::PushOpacity(float opacity) {
    if (!rt_) return;
    D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters();
    params.opacity = opacity;
    rt_->PushLayer(params, nullptr);
}

inline void Gfx::PopOpacity() {
    if (rt_) rt_->PopLayer();
}

inline void Gfx::GaugeMark(float cx, float cy, float radius, D2D1_COLOR_F gold, D2D1_COLOR_F goldBright,
                           D2D1_COLOR_F jade, D2D1_COLOR_F dim, bool detail) {
    if (!rt_ || radius <= 0.0f) return;
    const float width = std::max(1.0f, radius * 0.26f);
    const float pi = 3.14159265f;
    const auto point = [&](float degrees, float r) {
        const float rad = degrees * pi / 180.0f;
        return D2D1::Point2F(cx + std::cos(rad) * r, cy + std::sin(rad) * r);
    };
    const auto arc = [&](float from, float to, D2D1_COLOR_F color, float stroke) {
        const int segments = std::max(8, static_cast<int>(radius * 3.5f));
        D2D1_POINT_2F prev = point(from, radius);
        for (int i = 1; i <= segments; ++i) {
            const D2D1_POINT_2F next = point(from + (to - from) * static_cast<float>(i) / segments, radius);
            Line(prev.x, prev.y, next.x, next.y, color, stroke);
            prev = next;
        }
    };
    // 主弧 150°→390°，高性能分区 300°→390°，与桌面图标一致。
    arc(150.0f, 390.0f, gold, width);
    arc(300.0f, 390.0f, jade, width);
    if (detail) {
        const float tick = std::max(1.0f, radius * 0.06f);
        for (int i = 0; i < 9; ++i) {
            const D2D1_POINT_2F inner = point(150.0f + 30.0f * static_cast<float>(i), radius + width * 1.05f);
            const D2D1_POINT_2F outer = point(150.0f + 30.0f * static_cast<float>(i), radius + width * 2.25f);
            Line(inner.x, inner.y, outer.x, outer.y, dim, tick);
        }
        const D2D1_POINT_2F start = point(300.0f, radius);
        const D2D1_POINT_2F startOut = point(300.0f, radius + width * 1.75f);
        Line(start.x, start.y, startOut.x, startOut.y, goldBright, std::max(1.4f, width * 0.55f));
    }
    // 指针：金色杆 + 玉色尖端，轴心金色 + 玉色内芯。
    const float needleAngle = 310.0f;
    const D2D1_POINT_2F tip = point(needleAngle, radius * 0.80f);
    const D2D1_POINT_2F base = point(needleAngle, radius * 0.30f);
    Line(cx, cy, base.x, base.y, goldBright, width * 0.80f);
    Line(base.x, base.y, tip.x, tip.y, jade, width * 0.80f);
    FillEllipse(cx, cy, radius * 0.16f, radius * 0.16f, gold);
    FillEllipse(cx, cy, radius * 0.07f, radius * 0.07f, jade);
}

inline void Gfx::HLine(const Rect& area, float y, D2D1_COLOR_F color) {
    Line(area.left, y, area.right, y, color);
}

inline IDWriteTextFormat* Gfx::Format(const wchar_t* family, float size, int weight, HAlign halign, VAlign valign,
                                      bool wrap) {
    if (!dwrite_) return nullptr;
    FormatKey key{family, size, weight, halign, valign, wrap};
    for (size_t i = 0; i < formats_.size(); ++i) {
        if (formatKeys_[i] == key) return formats_[i];
    }
    IDWriteTextFormat* fmt = nullptr;
    const DWRITE_FONT_WEIGHT w = static_cast<DWRITE_FONT_WEIGHT>(weight);
    if (FAILED(dwrite_->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                         L"zh-CN", &fmt)))
        return nullptr;
    fmt->SetTextAlignment(halign == HAlign::Left    ? DWRITE_TEXT_ALIGNMENT_LEADING
                          : halign == HAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                     : DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt->SetParagraphAlignment(valign == VAlign::Top      ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                               : valign == VAlign::Middle ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                          : DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    formats_.push_back(fmt);
    formatKeys_.push_back(key);
    return fmt;
}

inline void Gfx::Text(const Rect& r, const std::wstring& text, IDWriteTextFormat* fmt, D2D1_COLOR_F color) {
    if (!rt_ || !fmt || text.empty() || r.Empty()) return;
    if (auto* b = Brush(color)) {
        rt_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, r.D2D(), b,
                       D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }
}

inline D2D1_SIZE_F Gfx::Measure(const std::wstring& text, IDWriteTextFormat* fmt, float maxWidth) {
    D2D1_SIZE_F size{0, 0};
    if (!dwrite_ || !fmt || text.empty()) return size;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, maxWidth, 1.0e6f,
                                         &layout)))
        return size;
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics))) {
        size.width = metrics.widthIncludingTrailingWhitespace;
        size.height = metrics.height;
    }
    layout->Release();
    return size;
}

inline std::wstring Gfx::Ellipsize(const std::wstring& text, IDWriteTextFormat* fmt, float maxWidth) {
    if (text.empty() || !fmt) return text;
    if (Measure(text, fmt, maxWidth).width <= maxWidth) return text;
    const std::wstring ellipsis = L"\u2026";
    size_t low = 0, high = text.size();
    std::wstring best = ellipsis;
    while (low <= high) {
        const size_t mid = (low + high) / 2;
        std::wstring candidate = text.substr(0, mid) + ellipsis;
        if (Measure(candidate, fmt, maxWidth).width <= maxWidth) {
            best = candidate;
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }
    return best;
}

inline float Gfx::TextRow(const Rect& r, const std::wstring& text, IDWriteTextFormat* fmt, D2D1_COLOR_F color,
                          VAlign valign) {
    if (text.empty()) return r.left;
    const D2D1_SIZE_F size = Measure(text, fmt, r.Width());
    const float x = r.left;
    const Rect line(x, r.top, r.right, r.bottom);
    if (valign == VAlign::Middle) {
        const float y = r.top + (r.Height() - size.height) * 0.5f;
        Text(Rect(x, y, x + std::max(size.width + 2.0f, 1.0f), y + size.height + 2.0f), text, fmt, color);
    } else {
        Text(line, text, fmt, color);
    }
    return x + size.width;
}

inline void Gfx::PushClip(const Rect& r) {
    if (!rt_) return;
    clips_.push_back(r);
    rt_->PushAxisAlignedClip(r.D2D(), D2D1_ANTIALIAS_MODE_ALIASED);
}

inline void Gfx::PopClip() {
    if (!rt_ || clips_.empty()) return;
    clips_.pop_back();
    rt_->PopAxisAlignedClip();
}

}  // namespace cc
