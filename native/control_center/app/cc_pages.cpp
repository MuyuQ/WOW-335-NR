// WOW-335-NR 原生控制中心：页面绘制。
//
// 布局尺寸沿用旧 WinForms 界面的 96dpi 逻辑像素（见 cc_theme.hpp）；所有坐标按
// app.layoutScale 缩放，以便在 96dpi 机器上用 --dpi-scale 1.25 检查 125% 布局。
// 页面只读 App 状态并登记命中区域，不改业务状态；动作在 AppHandleClick 中分发。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cc_app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace cc {

using namespace theme;

namespace {

// ---------- 小工具 ----------

template <typename... Args>
std::wstring Fmt(const wchar_t* format, Args... args) {
    wchar_t buffer[768]{};
    swprintf_s(buffer, format, args...);
    return std::wstring(buffer);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring ClientErrorText(ClientError error) {
    switch (error) {
        case ClientError::None: return L"客户端已识别";
        case ClientError::InvalidRoot: return L"目录无效或不可读";
        case ClientError::ProtectedRoot: return L"目录位于受保护位置";
        case ClientError::MissingOrUnreadable: return L"目录不可读";
        case ClientError::UnsupportedFile: return L"wow.exe 不是普通文件";
        case ClientError::InvalidPe: return L"wow.exe 不是有效 PE 文件";
        case ClientError::NotX86: return L"wow.exe 不是 32 位客户端";
    }
    return L"客户端不可用";
}

std::wstring ProcessText(const ClientSnapshot& snapshot) {
    switch (snapshot.processState) {
        case ProcessState::Stopped: return L"游戏与载体均未运行";
        case ProcessState::Running:
            return snapshot.blockingProcess.empty() ? std::wstring(L"游戏正在运行")
                                                    : (L"运行中：" + snapshot.blockingProcess);
        case ProcessState::Unknown: return L"进程状态未知";
    }
    return L"进程状态未知";
}

bool StateInstalled(MemberState state) {
    return state == MemberState::Installed || state == MemberState::InstalledVerified;
}

unsigned StateColor(MemberState state) {
    switch (state) {
        case MemberState::InstalledVerified: return kJade;
        case MemberState::Installed: return kJadeText;
        case MemberState::Modified: return kWarn;
        case MemberState::Missing: return kDanger;
        case MemberState::Unsafe: return kDanger;
        default: return kText3;
    }
}

const MemberStatus* FindMember(const ClientSnapshot& snapshot, const wchar_t* target) {
    const std::wstring name(target);
    for (const auto& member : snapshot.members) {
        if (control_center::SameTarget(member.target, name)) return &member;
    }
    return nullptr;
}

std::wstring PreviewStateTag(UninstallItemState state) {
    switch (state) {
        case UninstallItemState::DeleteInstalled:
        case UninstallItemState::DeleteModified: return L"删除";
        case UninstallItemState::RestoreOriginal: return L"还原";
        case UninstallItemState::AlreadyAbsent:
        case UninstallItemState::AlreadyRestored: return L"保留";
        default: return L"无法卸载";
    }
}

std::wstring UninstallIssueText(const UninstallPreviewItem& item) {
    switch (item.state) {
        case UninstallItemState::MissingBackup: return L"备份缺失";
        case UninstallItemState::BackupMismatch: return L"备份身份或哈希不符";
        case UninstallItemState::UnverifiedBackup: return L"备份无法校验";
        case UninstallItemState::Unavailable: return L"目标或备份不可读";
        default: return std::wstring();
    }
}

// ---------- 页面上下文 ----------

// 内容列宽（DIP）：窗口够宽时保持 568 设计单位，否则随窗口收窄。
float ContentWidthDip(const App& app) {
    const float u = AppScale(app);
    const float available = app.gfx.ClientWidthDip() - 2.0f * kPagePaddingX * u;
    return std::max(200.0f * u, std::min(kContentWidth * u, available));
}

float ContentLeftDip(const App& app) {
    return (app.gfx.ClientWidthDip() - ContentWidthDip(app)) * 0.5f;
}

struct Ctx {
    App& app;
    Gfx& g;
    Ui& ui;
    float u = 1.0f;
    float left = 0.0f;   // 内容左边界（DIP）
    float top = 0.0f;    // 内容顶部（DIP，已含滚动偏移）
    float width = 0.0f;  // 内容宽度（DIP）
    float y = 0.0f;      // 设计单位游标

    float X(float x) const { return left + x * u; }
    float Y(float value) const { return top + value * u; }
    float S(float v) const { return v * u; }
    Rect R(float x, float yTop, float w, float h) const {
        return Rect(X(x), Y(yTop), X(x + w), Y(yTop + h));
    }
    float Cw() const { return width / u; }
    void Gap(float h) { y += h; }
};

Ctx MakeCtx(App& app, float topDip) {
    Ctx c{app, app.gfx, app.ui};
    c.u = AppScale(app);
    c.left = ContentLeftDip(app);
    c.width = ContentWidthDip(app);
    c.top = topDip - app.scroll * c.u;
    return c;
}

// 正文裁剪区：滚动只移动正文，页头、步骤条与页脚保持固定。
Rect ContentClipRect(App& app, float topDip) {
    const Rect body = AppBodyRect(app);
    const float footerTop = AppFooterRect(app).top;
    return Rect(body.left, topDip, body.right, footerTop);
}

struct ScopedClip {
    Gfx* gfx;
    ScopedClip(Gfx* g, const Rect& rect) : gfx(g) {
        if (gfx != nullptr) gfx->PushClip(rect);
    }
    ~ScopedClip() {
        if (gfx != nullptr) gfx->PopClip();
    }
    ScopedClip(const ScopedClip&) = delete;
    ScopedClip& operator=(const ScopedClip&) = delete;
};

struct HeaderInfo {
    float stepsTop = 0.0f;      // 设计单位
    float contentStart = 0.0f;  // 设计单位
};

// 绘制页头（标题 + 副标题），返回步骤条与正文起始位置（设计单位）。
HeaderInfo DrawPageHeader(App& app, const std::wstring& title, const std::wstring& subtitle, bool withSteps) {
    Gfx& g = app.gfx;
    const float u = AppScale(app);
    const float left = ContentLeftDip(app);
    const float width = ContentWidthDip(app);
    const float bodyTop = AppBodyRect(app).top;
    auto* titleFmt = g.Format(FontUi(), kFontTitle * u, 700, HAlign::Left, VAlign::Top);
    g.Text(Rect(left, bodyTop + 12.0f * u, left + width, bodyTop + 36.0f * u), title, titleFmt, Color(kText));
    auto* subFmt = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Top, true);
    const D2D1_SIZE_F size = g.Measure(subtitle, subFmt, width);
    const float subH = std::max(16.0f * u, size.height + 2.0f * u);
    g.Text(Rect(left, bodyTop + 38.0f * u, left + width, bodyTop + 38.0f * u + subH + 2.0f * u), subtitle, subFmt,
           Color(kText3));
    HeaderInfo info;
    const float base = 38.0f + (subH + 2.0f * u) / u;
    if (withSteps) {
        info.stepsTop = base + 10.0f;
        info.contentStart = info.stepsTop + 26.0f + 14.0f;
    } else {
        info.stepsTop = base;
        info.contentStart = base + 14.0f;
    }
    return info;
}

// ---------- 基础控件 ----------

void CardRect(const Ctx& c, const Rect& rect, unsigned border = kLineSoft) {
    c.g.FillRound(rect, kCardRadius * c.u, Color(kSurface));
    c.g.StrokeRound(rect, kCardRadius * c.u, Color(border));
}

void Tag(const Ctx& c, const Rect& rect, const std::wstring& text, unsigned color) {
    c.g.FillRound(rect, 3.0f * c.u, Color(color, 0.16f));
    c.g.StrokeRound(rect, 3.0f * c.u, Color(color, 0.55f));
    auto* fmt = c.g.Format(FontUi(), kFontSmall * c.u, 600, HAlign::Center, VAlign::Middle);
    c.g.Text(rect, text, fmt, Color(color));
}

float TagWidth(const Ctx& c, const std::wstring& text) {
    auto* fmt = c.g.Format(FontUi(), kFontSmall * c.u, 600, HAlign::Left, VAlign::Middle);
    return c.g.Measure(text, fmt).width + 14.0f * c.u;
}

void Pill(const Ctx& c, const Rect& rect, const std::wstring& label, const std::wstring& value, unsigned dot,
          unsigned valueColor, unsigned fill, unsigned border) {
    c.g.FillRound(rect, rect.Height() * 0.5f, Color(fill));
    c.g.StrokeRound(rect, rect.Height() * 0.5f, Color(border));
    const float cy = rect.CenterY();
    const float dotX = rect.left + 12.0f * c.u;
    c.g.FillEllipse(dotX, cy, 3.2f * c.u, 3.2f * c.u, Color(dot));
    auto* smallFmt = c.g.Format(FontUi(), kFontSmall * c.u, 400, HAlign::Left, VAlign::Middle);
    float x = dotX + 8.0f * c.u;
    if (!label.empty()) {
        const D2D1_SIZE_F ls = c.g.Measure(label, smallFmt);
        c.g.Text(Rect(x, rect.top, x + ls.width + 2.0f * c.u, rect.bottom), label, smallFmt, Color(kText2));
        x += ls.width + 7.0f * c.u;
    }
    c.g.Text(Rect(x, rect.top, rect.right - 6.0f * c.u, rect.bottom), value, smallFmt, Color(valueColor));
}

float PillWidth(const Ctx& c, const std::wstring& label, const std::wstring& value) {
    auto* smallFmt = c.g.Format(FontUi(), kFontSmall * c.u, 400, HAlign::Left, VAlign::Middle);
    float width = 20.0f * c.u + c.g.Measure(value, smallFmt).width + 12.0f * c.u;
    if (!label.empty()) width += c.g.Measure(label, smallFmt).width + 7.0f * c.u;
    return width;
}

void CheckRow(const Ctx& c, const Rect& row, bool ok, const std::wstring& label, const std::wstring& detail,
              const std::wstring& status, float detailRightInset = 0.0f) {
    const float u = c.u;
    const float cy = row.CenterY();
    c.g.FillEllipse(row.left + 12.0f * u, cy, 3.2f * u, 3.2f * u, Color(ok ? kJade : kMuted));
    auto* smallFmt = c.g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
    auto* body = c.g.Format(FontUi(), kFontBody * u, 400, HAlign::Left, VAlign::Middle);
    c.g.Text(Rect(row.left + 20.0f * u, row.top, row.left + 88.0f * u, row.bottom), label, smallFmt, Color(kText3));
    const float detailLeft = row.left + 92.0f * u;
    const float statusW = 92.0f * u;
    const float detailRight = row.right - statusW - 12.0f * u - detailRightInset;
    const std::wstring shown =
        c.g.Ellipsize(detail, body, std::max(10.0f, detailRight - detailLeft));
    c.g.Text(Rect(detailLeft, row.top, detailRight, row.bottom), shown, body, Color(ok ? kText2 : kText3));
    const std::wstring statusShown = c.g.Ellipsize(status, smallFmt, statusW);
    c.g.Text(Rect(row.right - statusW - 12.0f * u, row.top, row.right - 12.0f * u, row.bottom), statusShown, smallFmt,
             Color(ok ? kJadeText : kText3));
}

void KvRow(const Ctx& c, const Rect& row, const std::wstring& key, const std::wstring& value,
           unsigned valueColor = kText2) {
    const float u = c.u;
    auto* smallFmt = c.g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
    auto* body = c.g.Format(FontUi(), kFontBody * u, 400, HAlign::Left, VAlign::Middle);
    c.g.Text(Rect(row.left + 16.0f * u, row.top, row.left + 72.0f * u, row.bottom), key, smallFmt, Color(kText3));
    const float left = row.left + 78.0f * u;
    const float right = row.right - 16.0f * u;
    c.g.Text(Rect(left, row.top, right, row.bottom),
             c.g.Ellipsize(value, body, std::max(10.0f, right - left)), body, Color(valueColor));
}

void FileRow(const Ctx& c, const Rect& row, const std::wstring& target, const std::wstring& tag,
             unsigned tagColor, const std::wstring& note) {
    const float u = c.u;
    const float tagW = TagWidth(c, tag);
    const Rect tagRect(row.right - 16.0f * u - tagW, row.CenterY() - 10.0f * u, row.right - 16.0f * u,
                       row.CenterY() + 10.0f * u);
    Tag(c, tagRect, tag, tagColor);
    auto* body = c.g.Format(FontUi(), kFontBody * u, 400, HAlign::Left, VAlign::Middle);
    float noteW = 0.0f;
    if (!note.empty()) {
        auto* smallFmt = c.g.Format(FontUi(), kFontSmall * u, 400, HAlign::Right, VAlign::Middle);
        noteW = c.g.Measure(note, smallFmt).width + 12.0f * u;
        c.g.Text(Rect(tagRect.left - noteW, row.top, tagRect.left - 6.0f * u, row.bottom), note, smallFmt,
                 Color(kText3));
    }
    const float textLeft = row.left + 16.0f * u;
    const float textRight = tagRect.left - 8.0f * u - noteW;
    c.g.Text(Rect(textLeft, row.top, std::max(textLeft + 10.0f, textRight), row.bottom),
             c.g.Ellipsize(target, body, std::max(10.0f, textRight - textLeft)), body, Color(kText));
}

void Section(const Ctx& c, const std::wstring& title, const std::wstring& note = std::wstring()) {
    const float u = c.u;
    const Rect r = c.R(0, c.y, c.Cw(), 24);
    c.g.GaugeMark(r.left + 4.5f * u, r.top + 11.0f * u, 3.6f * u, Color(kAccent), Color(kAccentBright),
                  Color(kJade), Color(kAccentLine), false);
    auto* bold = c.g.Format(FontUi(), kFontBody * u, 700, HAlign::Left, VAlign::Middle);
    const D2D1_SIZE_F size = c.g.Measure(title, bold);
    c.g.Text(Rect(r.left + 14.0f * u, r.top, r.left + 14.0f * u + size.width + 2.0f * u, r.bottom), title, bold,
             Color(kText));
    if (!note.empty()) {
        auto* smallFmt = c.g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
        c.g.Text(Rect(r.left + 14.0f * u + size.width + 10.0f * u, r.top, r.right, r.bottom), note, smallFmt,
                 Color(kText3));
    }
}

void MutedText(const Ctx& c, const Rect& rect, const std::wstring& text, float size = kFontSmall) {
    auto* fmt = c.g.Format(FontUi(), size * c.u, 400, HAlign::Left, VAlign::Top, true);
    c.g.Text(rect, text, fmt, Color(kText3));
}

void EmptyRow(const Ctx& c, const Rect& row, const std::wstring& text) {
    c.g.Text(row, text, c.g.Format(FontUi(), kFontSmall * c.u, 400, HAlign::Left, VAlign::Middle), Color(kText3));
}

// 滚动条。
void DrawScrollbar(App& app, const Rect& body) {
    if (app.scrollMax <= 0.5f) return;
    Gfx& g = app.gfx;
    const float u = AppScale(app);
    const float viewport = (body.bottom - body.top) / u;
    const float content = viewport + app.scrollMax;
    const float trackTop = body.top + 8.0f * u;
    const float trackBottom = body.bottom - 8.0f * u;
    const float trackH = trackBottom - trackTop;
    if (trackH <= 10.0f * u) return;
    const float thumbH = std::max(24.0f * u, trackH * viewport / std::max(1.0f, content));
    const float ratio = std::min(1.0f, std::max(0.0f, app.scroll / std::max(1.0f, app.scrollMax)));
    const float thumbTop = trackTop + ratio * (trackH - thumbH);
    const float x = g.ClientWidthDip() - 8.0f * u;
    g.FillRound(Rect(x, trackTop, x + 4.0f * u, trackBottom), 2.0f * u, Color(kLineSoft, 0.7f));
    g.FillRound(Rect(x, thumbTop, x + 4.0f * u, thumbTop + thumbH), 2.0f * u, Color(kLine));
}

// 页面内容结束：更新滚动范围（设计单位）。
void FinishScroll(App& app, const Ctx& c, float extraBottom = 12.0f) {
    const Rect body = AppBodyRect(app);
    const float viewport = (body.bottom - body.top) / c.u;
    // Ctx::top 已经减去滚动偏移，这里还原内容起点，否则滚动后总高会随偏移缩小，
    // 导致 scrollMax 每帧变小、列表滚不到底部。
    const float contentTop = c.top + app.scroll * c.u;
    const float used = (contentTop - body.top) / c.u + c.y + extraBottom;
    app.scrollMax = std::max(0.0f, used - viewport);
    if (app.scroll > app.scrollMax) app.scroll = app.scrollMax;
}

// 页脚左侧返回按钮位置。
Rect FooterBackRect(const App& app) {
    const float u = AppScale(app);
    const Rect footer = AppFooterRect(app);
    return Rect(footer.left + 12.0f * u, footer.CenterY() - 16.0f * u, footer.left + 122.0f * u,
                footer.CenterY() + 16.0f * u);
}

// 写事务阶段文案（与 cc_tasks.hpp 的 progress->stage 对应）。
const wchar_t* OperationStageText(int stage, bool uninstall) {
    switch (stage) {
        case 0: return L"准备与预检";
        case 1: return uninstall ? L"逐项处理文件" : L"写入客户端";
        case 2: return L"提交与收尾";
        case 3: return L"完成";
        default: return L"进行中";
    }
}

// 写事务进度卡：只用真实阶段与计数，不用假百分比。
void DrawOperationProgress(Ctx& c, const App::OperationRun& run, bool uninstall,
                           const wchar_t* runningTitle, const wchar_t* doneTitle,
                           const wchar_t* failedTitle) {
    const float cw = c.Cw();
    const float u = c.u;
    Gfx& g = c.g;
    const bool failed = run.finished && !run.ok;
    const bool done = run.finished && run.ok;
    const float h = 132.0f;
    CardRect(c, c.R(0, c.y, cw, h), failed ? kWarn : kLineSoft);
    const wchar_t* title = run.running ? runningTitle : (failed ? failedTitle : doneTitle);
    g.Text(c.R(16, c.y + 12, cw - 32, 24), title,
           g.Format(FontUi(), kFontBody * u, 700, HAlign::Left, VAlign::Middle),
           Color(failed ? kWarnText : kAccent));
    std::wstring stageText;
    if (run.running) {
        const int stage = run.progress ? run.progress->stage.load() : 0;
        const size_t doneCount = run.progress ? run.progress->done.load() : 0;
        const size_t total = run.progress ? run.progress->total.load() : 0;
        stageText = std::wstring(L"阶段：") + OperationStageText(stage, uninstall);
        if (total > 0) stageText += Fmt(L" · 已处理 %zu / %zu", doneCount, total);
    } else if (done) {
        stageText = run.detail.empty() ? std::wstring(L"已完成。") : run.detail;
    } else {
        stageText = run.error.empty() ? std::wstring(L"未完成。") : run.error;
    }
    auto* smallFmt = g.Format(FontUi(), kFontSmall * u, failed ? 500 : 400, HAlign::Left, VAlign::Top, true);
    g.Text(c.R(16, c.y + 42, cw - 32, 42), stageText, smallFmt, Color(failed ? kWarnText : kText2));
    // 进度条：卸载按成员数确定；其他事务用移动段表示仍在进行，不伪造百分比。
    const Rect track = c.R(16, c.y + 92, cw - 32, 4);
    g.FillRound(track, 2.0f * u, Color(kLineSoft));
    if (run.running) {
        const size_t total = run.progress ? run.progress->total.load() : 0;
        const size_t doneCount = run.progress ? run.progress->done.load() : 0;
        if (uninstall && total > 0) {
            const float ratio = std::min(1.0f, static_cast<float>(doneCount) / static_cast<float>(total));
            g.FillRound(Rect(track.left, track.top, track.left + track.Width() * ratio, track.bottom),
                        2.0f * u, Color(kAccent));
        } else {
            const float phase = static_cast<float>(GetTickCount64() % 1400) / 1400.0f;
            const float span = track.Width() * 0.28f;
            const float x = track.left + (track.Width() + span) * phase - span;
            const float left = std::max(track.left, x);
            const float right = std::min(track.right, x + span);
            if (right > left)
                g.FillRound(Rect(left, track.top, right, track.bottom), 2.0f * u, Color(kAccent, 0.85f));
        }
    } else if (done) {
        g.FillRound(track, 2.0f * u, Color(kJade));
    } else {
        g.FillRound(track, 2.0f * u, Color(kWarn, 0.85f));
    }
    const wchar_t* note =
        run.running
            ? L"写事务在后台线程执行，界面保持可操作；请勿强制结束程序，中断后下次启动会自动恢复。"
            : (failed ? L"失败不会掩盖：已写入内容由事务日志保护，可重试或在维护页检查状态。"
                      : L"事务已提交；可返回控制台查看最新状态。");
    auto* noteFmt = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Top, true);
    g.Text(c.R(16, c.y + 102, cw - 32, 26), note, noteFmt, Color(kText3));
    c.Gap(h + 10.0f);
}

}  // namespace

// ---------- 布局辅助（对外） ----------

float AppScale(const App& app) { return app.layoutScale > 0.0f ? app.layoutScale : 1.0f; }

float AppContentWidth(const App& app) { return ContentWidthDip(app); }

Rect AppBodyRect(const App& app) {
    const float u = AppScale(app);
    const float width = app.gfx.ClientWidthDip();
    const float height = app.gfx.ClientHeightDip();
    const float footerTop = std::max(kTitleBarHeight * u + 120.0f * u, height - kFooterHeight * u);
    return Rect(0.0f, kTitleBarHeight * u, width, footerTop);
}

Rect AppFooterRect(const App& app) {
    const Rect body = AppBodyRect(app);
    return Rect(0.0f, body.bottom, app.gfx.ClientWidthDip(), app.gfx.ClientHeightDip());
}

Rect AppChromeButtonRect(const App& app, int index) {
    const float u = AppScale(app);
    const float width = app.gfx.ClientWidthDip();
    const float right = width - 8.0f * u;
    const float left = right - static_cast<float>(3 - index) * kWindowButtonWidth * u;
    const float top = (kTitleBarHeight * u - kWindowButtonHeight * u) * 0.5f;
    return Rect(left, top, left + kWindowButtonWidth * u, top + kWindowButtonHeight * u);
}

int AppChromeHitTest(const App& app, float x, float y) {
    for (int i = 0; i < 3; ++i) {
        if (AppChromeButtonRect(app, i).Contains(x, y)) return i + 1;
    }
    return 0;
}

// ---------- 标题栏 ----------

void DrawChrome(App& app) {
    Gfx& g = app.gfx;
    Ui& ui = app.ui;
    const float u = AppScale(app);
    const float width = g.ClientWidthDip();
    const float height = kTitleBarHeight * u;
    g.FillRect(Rect(0.0f, 0.0f, width, height), Color(kChrome));
    g.Line(0.0f, height - 0.5f, width, height - 0.5f, Color(kLineSoft));

    const float cx = 23.0f * u;
    const float cy = height * 0.5f;
    g.GaugeMark(cx, cy, 7.5f * u, Color(kAccent), Color(kAccentBright), Color(kJadeText), Color(kAccentLine), true);

    auto* brand = g.Format(FontDisplay(), kFontBrand * u, 700, HAlign::Left, VAlign::Middle);
    const std::wstring title = L"WOW-335-NR";
    const D2D1_SIZE_F titleSize = g.Measure(title, brand);
    g.Text(Rect(36.0f * u, 0.0f, 36.0f * u + titleSize.width + 4.0f * u, height), title, brand, Color(kText));
    auto* smallFmt = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
    g.Text(Rect(36.0f * u + titleSize.width + 14.0f * u, 0.0f, width, height), L"控制台", smallFmt, Color(kText3));

    const std::wstring version = L"Mcode · v1.2.0";
    const D2D1_SIZE_F versionSize = g.Measure(version, smallFmt);
    const float versionRight = width - 112.0f * u - 14.0f * u;
    g.Text(Rect(versionRight - versionSize.width - 2.0f * u, 0.0f, versionRight, height), version, smallFmt,
           Color(kText3));

    static const char* ids[3] = {"chrome.min", "chrome.max", "chrome.close"};
    for (int i = 0; i < 3; ++i) {
        const Rect r = AppChromeButtonRect(app, i);
        const bool hot = ui.Hot(ids[i]);
        const bool active = ui.Active(ids[i]);
        if (hot || active) g.FillRect(r, Color(active ? kSurface3 : kSurface));
        const float x = r.CenterX();
        const float y = r.CenterY();
        const float s = 4.5f * u;
        const D2D1_COLOR_F ink = Color(hot ? kText : kText2);
        if (i == 0) {
            g.Line(x - s, y, x + s, y, ink, 1.0f);
        } else if (i == 1) {
            g.StrokeRound(Rect(x - s, y - s, x + s, y + s), 0.0f, ink, 1.0f);
        } else {
            g.Line(x - s, y - s, x + s, y + s, ink, 1.0f);
            g.Line(x - s, y + s, x + s, y - s, ink, 1.0f);
        }
        ui.AddHit(ids[i], r, true);
    }
}

// ---------- 页脚 ----------

void DrawFooter(App& app) {
    Gfx& g = app.gfx;
    Ui& ui = app.ui;
    const float u = AppScale(app);
    const Rect footer = AppFooterRect(app);
    const float width = g.ClientWidthDip();
    g.FillRect(footer, Color(kWindow));
    g.Line(26.0f * u, footer.top + 0.5f, width - 26.0f * u, footer.top + 0.5f, Color(kLineSoft));

    if (app.closeRequested) {
        auto* format = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
        g.Text(footer.Inset(24.0f * u, 4.0f * u), L"正在退出：已取消未开始的任务，等待当前任务安全结束…", format, Color(kText));
        return;
    }
    struct Action {
        const char* id;
        std::wstring label;
        ButtonKind kind;
        bool enabled;
        float w;
    };
    std::vector<Action> actions;
    const bool busy = app.installRun.running || app.repairRun.running || app.uninstallRun.running;
    const bool canInstall = app.install.ready && app.install.ok && app.snapshotReady &&
                            app.snapshot.clientValid &&
                            app.snapshot.processState == control_center::ProcessState::Stopped;
    const bool canUninstall = app.uninstall.ready && app.uninstall.ok &&
                              app.uninstall.preview.valid && app.uninstall.preview.canUninstall;
    switch (app.page) {
        case Page::Home:
            actions.push_back({"ftr.switch", L"切换客户端", ButtonKind::Ghost, true, 96.0f});
            actions.push_back({"ftr.maint", L"维护", ButtonKind::Ghost, true, 64.0f});
            break;
        case Page::Wizard:
            switch (app.wizardStep) {
                case WizardStep::Client:
                    actions.push_back({"ftr.next", L"开始检查", ButtonKind::Primary, !busy, 112.0f});
                    break;
                case WizardStep::Checks:
                    actions.push_back({"ftr.prev", L"上一步", ButtonKind::Secondary, !busy, 96.0f});
                    actions.push_back({"ftr.next", L"查看清单", ButtonKind::Primary, !busy, 112.0f});
                    break;
                case WizardStep::Files:
                    actions.push_back({"ftr.prev", L"上一步", ButtonKind::Secondary, !busy, 96.0f});
                    actions.push_back({"ftr.install", L"开始安装", ButtonKind::Primary,
                                       canInstall && !busy, 112.0f});
                    break;
                case WizardStep::Install:
                    actions.push_back({"ftr.next", L"返回控制台", ButtonKind::Primary,
                                       !app.installRun.running, 112.0f});
                    break;
                case WizardStep::Done:
                    actions.push_back({"ftr.next", L"返回控制台", ButtonKind::Primary, true, 112.0f});
                    break;
            }
            break;
        case Page::Maintenance:
            break;
        case Page::Uninstall:
            actions.push_back({"ftr.confirm", L"确认卸载", ButtonKind::Danger,
                               canUninstall && !busy, 112.0f});
            break;
    }

    const float gap = 8.0f * u;
    const float rightPad = 12.0f * u;
    float total = 0.0f;
    for (const auto& action : actions) total += action.w * u + gap;
    if (!actions.empty()) total -= gap;
    float x = width - rightPad - total;
    const float h = 32.0f * u;
    const float y = footer.CenterY() - h * 0.5f;
    for (const auto& action : actions) {
        const Rect r(x, y, x + action.w * u, y + h);
        ui.Button(action.id, r, action.label, action.kind, action.enabled, kFontSmall);
        x += action.w * u + gap;
    }

    if (app.page == Page::Home) {
        auto* smallFmt = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
        const float textRight = std::max(10.0f, width - rightPad - total - 16.0f * u);
        const std::wstring clientLine =
            app.clientRoot.empty() ? std::wstring(L"未选择客户端") : (L"客户端：" + app.clientRoot);
        g.Text(Rect(16.0f * u, footer.top + 8.0f * u, textRight, footer.top + 26.0f * u),
               g.Ellipsize(clientLine, smallFmt, textRight - 16.0f * u), smallFmt, Color(kText3));
        const bool installed = app.snapshot.recordState == control_center::CapturedRecordState::Present;
        const bool game = app.gameRunning || app.snapshot.processState == ProcessState::Running;
        const std::wstring stateLine =
            std::wstring(installed ? L"已安装" : L"未安装") + L"  ·  " +
            (game ? L"游戏运行中" : L"游戏未运行");
        g.Text(Rect(16.0f * u, footer.top + 27.0f * u, textRight, footer.top + 45.0f * u), stateLine, smallFmt,
               Color(kText3));
    } else {
        ui.Button("ftr.back", FooterBackRect(app), L"← 返回", ButtonKind::Ghost, true, kFontSmall);
    }
}

// ---------- 主页 ----------

void DrawHome(App& app) {
    Gfx& g = app.gfx;
    Ui& ui = app.ui;
    const float u = AppScale(app);
    const Rect body = AppBodyRect(app);
    ScopedClip contentClip(&g, ContentClipRect(app, body.top));
    Ctx c = MakeCtx(app, body.top + 8.0f * u);
    const float cw = c.Cw();
    const bool running = app.gameRunning || app.snapshot.processState == ProcessState::Running;
    const bool telemetry = app.telemetry.available;
    const bool installed = app.snapshot.recordState == control_center::CapturedRecordState::Present;
    const bool busy = app.installRun.running || app.repairRun.running || app.uninstallRun.running;

    // 1. FPS Hero 卡。
    {
        const Rect hero = c.R(0, 0, cw, 118);
        CardRect(c, hero, kLine);
        g.Text(c.R(16, 10, 160, 16), L"输出帧率",
               g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle), Color(kAccent));
        auto* gauge = g.Format(FontUi(), kFontGauge * u, 700, HAlign::Left, VAlign::Top);
        const std::wstring gaugeText =
            app.fpsOutput >= 0.0 ? Fmt(L"%.1f", app.fpsOutput) : std::wstring(L"—");
        g.Text(c.R(14, 22, 300, 58), gaugeText, gauge,
               Color(app.fpsOutput >= 0.0 ? kText : kText3));
        const D2D1_SIZE_F gaugeSize = g.Measure(gaugeText, gauge);
        g.Text(Rect(c.X(16) + gaugeSize.width + 4.0f * u, c.Y(22) + gaugeSize.height - 16.0f * u, c.X(300),
                    c.Y(22) + gaugeSize.height),
               L"FPS", g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle), Color(kAccent));

        const std::wstring pillText = running ? L"游戏运行中" : L"游戏未运行";
        const float pillW = PillWidth(c, L"", pillText);
        const Rect pill(c.X(cw - 16) - pillW, c.Y(12), c.X(cw - 16), c.Y(34));
        Pill(c, pill, L"", pillText, running ? kJade : kMuted, running ? kJadeText : kText3, kSurface, kLine);

        const Rect mainBtn = c.R(cw - 16 - 108, 46, 108, 30);
        if (running) {
            ui.Button("home.panel", mainBtn, L"▶  打开面板", ButtonKind::Primary, !busy);
        } else {
            ui.Button("home.launch", mainBtn, L"▶  启动游戏", ButtonKind::Primary, app.snapshot.clientValid && !busy);
        }

        auto* note = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Top, true);
        std::wstring caption;
        if (app.fpsOutput >= 0.0) {
            if (app.fpsSource >= 0.0 && app.fpsSource > 0.0) {
                caption = Fmt(L"源 %.1f  →  输出 %.1f FPS  ·  增益 %.2f×", app.fpsSource, app.fpsOutput,
                              app.fpsOutput / app.fpsSource);
            } else {
                caption = Fmt(L"输出 %.1f FPS  ·  实时采样", app.fpsOutput);
            }
        } else if (running) {
            caption = app.telemetryNote.empty() ? L"实时采样 · 每秒刷新" : app.telemetryNote;
        } else {
            caption = L"启动游戏后显示实时数据；游戏运行后这里显示源/输出帧率。";
        }
        g.Text(c.R(16, 92, cw - 32, 22), caption, note, Color(kText2));
        c.Gap(118);
    }

    // 2. 参数行（维护页高级工具读取到 ReShade.ini 后显示真实值）。
    c.Gap(6);
    {
        std::wstring params;
        if (app.advanced.loaded && app.advanced.settings.readable) {
            params = L"模型 " + app.advanced.modelName + L" · 强度 " +
                     Fmt(L"%.2f", app.advanced.settings.intensity) + L" · 档位 " + app.advanced.tierName;
        } else if (app.advanced.loading) {
            params = L"正在读取客户端配置…";
        } else {
            params = L"模型 — · 强度 — · 档位未读取（维护页 → 高级工具）";
        }
        g.Text(c.R(0, c.y, cw, 16), params,
               g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle), Color(kText3));
        c.Gap(20);
    }

    // 3. 状态胶囊行。
    {
        struct ChipDef {
            std::wstring value;
            unsigned dot;
            unsigned valueColor;
        };
        auto chipState = [](const MemberStatus* member) -> ChipDef {
            if (!member) return {L"未安装", kMuted, kText3};
            if (member->state == MemberState::Modified) return {L"已修改", kWarn, kWarn};
            if (StateInstalled(member->state)) return {L"已安装", kJade, kJadeText};
            if (member->state == MemberState::Missing) return {L"文件缺失", kDanger, kDanger};
            return {L"未安装", kMuted, kText3};
        };
        const MemberStatus* core = FindMember(app.snapshot, L"d3d9.dll");
        const MemberStatus* sr = FindMember(app.snapshot, L"host64/nvngx_dlss.dll");
        const MemberStatus* fg = FindMember(app.snapshot, L"host64/nvngx_dlssg.dll");
        const MemberStatus* layerIni = FindMember(app.snapshot, L"host64/ReShade.ini");
        ChipDef chips[4];
        chips[0] = chipState(sr);
        chips[1] = chipState(fg);
        chips[2] = chipState(core);
        if (core && StateInstalled(core->state) && layerIni &&
            (layerIni->state == MemberState::Missing || layerIni->state == MemberState::NotInstalled ||
             layerIni->state == MemberState::Unsafe)) {
            chips[2] = {L"部分缺失", kWarn, kWarn};
        }
        // 有遥测时以载体实际 flags 为准（与旧界面一致：NR / 补帧真实开关状态）。
        if (telemetry) {
            chips[0] = app.telemetry.NrEnabled() ? ChipDef{L"已启用", kJade, kJadeText}
                                                 : ChipDef{L"未启用", kMuted, kText3};
            chips[1] = app.telemetry.FrameGeneration() ? ChipDef{L"已启用", kJade, kJadeText}
                                                       : ChipDef{L"未启用", kMuted, kText3};
        }
        if (app.advanced.loaded && app.advanced.settings.readable) {
            chips[3] = {app.advanced.tierName, kAccent, kAccentBright};
        } else {
            chips[3] = {L"未读取", kMuted, kText3};
        }
        static const wchar_t* labels[4] = {L"画面增强", L"补帧", L"图层", L"档位"};
        const float gap = 8.0f;
        const float chipW = (cw - 3.0f * gap) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            const float x = static_cast<float>(i) * (chipW + gap);
            Pill(c, c.R(x, c.y, chipW, 26), labels[i], chips[i].value, chips[i].dot, chips[i].valueColor,
                 kSurface, kLine);
        }
        c.Gap(26);
        c.Gap(10);
    }

    // 4. 运行状态。
    {
        Section(c, L"运行状态", running ? L"实时采样 · 每秒刷新" : L"实时采样");
        c.Gap(24);
        CardRect(c, c.R(0, c.y, cw, 134));
        const bool componentOk = installed && app.snapshot.installed > 0;
        const std::wstring componentDetail =
            componentOk
                ? (app.snapshot.hashesChecked
                       ? Fmt(L"已安装 %zu 项（%zu 项已校验）", app.snapshot.installed, app.snapshot.verified)
                       : Fmt(L"已安装 %zu 项", app.snapshot.installed))
                : std::wstring(L"未安装，切换客户端后进入安装向导");
        CheckRow(c, c.R(0, c.y + 4, cw, 42), app.snapshot.clientValid, L"客户端",
                 app.snapshot.clientValid ? app.snapshot.displayName : std::wstring(L"未选择客户端"),
                 app.snapshot.clientValid ? L"就绪" : L"未就绪");
        CheckRow(c, c.R(0, c.y + 47, cw, 42), app.snapshot.gpu.ok, L"显卡",
                 app.snapshot.gpu.ok ? app.snapshot.gpu.name : std::wstring(L"未检测到显卡"),
                 app.snapshot.gpu.ok ? L"正常" : L"未知");
        CheckRow(c, c.R(0, c.y + 90, cw, 42), componentOk, L"组件", componentDetail,
                 componentOk ? L"正常" : L"未安装", componentOk ? 0.0f : 120.0f * u);
        if (!componentOk) {
            ui.Button("home.switch", c.R(cw - 16 - 104, c.y + 97, 104, 28), L"切换客户端", ButtonKind::Secondary, true,
                      kFontSmall);
        }
        c.Gap(134);
        if (telemetry) {
            const control_center::TelemetrySample& s = app.telemetry;
            auto* keyFmt = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
            auto* valFmt = g.Format(FontUi(), kFontSmall * u, 600, HAlign::Right, VAlign::Middle);
            CardRect(c, c.R(0, c.y, cw, 134));
            const double scale = (s.inputWidth > 0 && s.outputWidth > 0)
                                     ? static_cast<double>(s.outputWidth) / static_cast<double>(s.inputWidth)
                                     : 0.0;
            const std::wstring scaleText = scale > 0.0 ? Fmt(L"%.2f×", scale) : std::wstring(L"—");
            const std::wstring res = Fmt(L"%ux%u  →  %ux%u  (%ls)", s.inputWidth, s.inputHeight, s.outputWidth,
                                         s.outputHeight, scaleText.c_str());
            const double latencyMs =
                s.gpuTotalUs > 0 ? static_cast<double>(s.gpuTotalUs) / 1000.0
                                 : static_cast<double>(s.sourceFrameAverageUs) / 1000.0;
            const std::wstring latency = latencyMs > 0.0 ? Fmt(L"%.1f ms", latencyMs) : std::wstring(L"—");
            const std::wstring counts = Fmt(L"NR %llu · SR %llu · FG %llu",
                                            static_cast<unsigned long long>(s.nrFrames),
                                            static_cast<unsigned long long>(s.srFrames),
                                            static_cast<unsigned long long>(s.fgFrames));
            const std::wstring sourceText =
                app.fpsSource >= 0.0 ? Fmt(L"%.1f FPS", app.fpsSource) : std::wstring(L"—");
            const std::wstring vramText =
                app.gpuStats.vramGb >= 0.0 ? Fmt(L"%.1f GB", app.gpuStats.vramGb) : std::wstring(L"—");
            const std::wstring loadText =
                app.gpuStats.loadPercent >= 0.0 ? Fmt(L"%.1f%%", app.gpuStats.loadPercent) : std::wstring(L"—");
            static const wchar_t* keyNames[6] = {L"分辨率", L"延迟", L"帧计数", L"源帧率", L"显存", L"GPU 负载"};
            const std::wstring values[6] = {res, latency, counts, sourceText, vramText, loadText};
            for (int i = 0; i < 6; ++i) {
                const float rowTop = c.y + 6.0f + 21.0f * static_cast<float>(i);
                g.Text(c.R(16, rowTop, 90, 20), keyNames[i], keyFmt, Color(kText3));
                g.Text(c.R(96, rowTop, cw - 112, 20), values[i], valFmt, Color(kText));
            }
            c.Gap(134);
        }
        c.Gap(10);
    }

    // 5. 管线图。
    {
        CardRect(c, c.R(0, c.y, cw, 34));
        const MemberStatus* core = FindMember(app.snapshot, L"d3d9.dll");
        const MemberStatus* sr = FindMember(app.snapshot, L"host64/nvngx_dlss.dll");
        const MemberStatus* fg = FindMember(app.snapshot, L"host64/nvngx_dlssg.dll");
        bool lit[4] = {core && StateInstalled(core->state), sr && StateInstalled(sr->state),
                       sr && StateInstalled(sr->state), fg && StateInstalled(fg->state)};
        if (telemetry) {
            lit[0] = running;
            lit[1] = app.telemetry.NrEnabled();
            lit[2] = app.telemetry.NrEnabled();
            lit[3] = app.telemetry.FrameGeneration();
        }
        static const wchar_t* names[4] = {L"抓帧", L"画面增强", L"超分 / 抗锯齿", L"补帧"};
        const float segW = (cw - 24.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            const float segLeft = 12.0f + static_cast<float>(i) * segW;
            auto* fmt = g.Format(FontUi(), kFontSmall * u, lit[i] ? 600 : 400, HAlign::Center, VAlign::Middle);
            const D2D1_SIZE_F size = g.Measure(names[i], fmt);
            const float centerX = c.X(segLeft + segW * 0.5f);
            g.FillEllipse(centerX - size.width * 0.5f - 8.0f * u, c.Y(c.y + 17), 3.0f * u, 3.0f * u,
                          Color(lit[i] ? kJade : kMuted));
            g.Text(Rect(centerX - segW * 0.5f + 6.0f * u, c.Y(c.y), centerX + segW * 0.5f, c.Y(c.y + 34)),
                   names[i], fmt, Color(lit[i] ? kText2 : kText3));
            if (i < 3) {
                const float ax = c.X(segLeft + segW);
                g.Line(ax - 6.0f * u, c.Y(c.y + 17), ax + 6.0f * u, c.Y(c.y + 17), Color(kArrow), 1.0f);
                g.Line(ax + 3.0f * u, c.Y(c.y + 14), ax + 6.0f * u, c.Y(c.y + 17), Color(kArrow), 1.0f);
                g.Line(ax + 3.0f * u, c.Y(c.y + 20), ax + 6.0f * u, c.Y(c.y + 17), Color(kArrow), 1.0f);
            }
        }
        c.Gap(34);
        c.Gap(12);
    }

    // 6. 源帧率上限。
    {
        Section(c, L"源帧率上限", L"写入 WTF\\Config.wtf");
        c.Gap(24);
        auto* smallFmt = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
        g.Text(c.R(0, c.y, 80, 18), L"前台（0=不限）", smallFmt, Color(kText3));
        g.Text(c.R(98, c.y, 82, 18), L"后台（0=不限）", smallFmt, Color(kText3));
        app.editFrontRect = c.R(0, c.y + 20, 72, 28);
        app.editBackRect = c.R(98, c.y + 20, 72, 28);
        const bool fpsEditable = app.snapshot.fpsCap.exists && app.snapshot.clientValid && !app.fpsSaving && !busy;
        auto* fpsFmt = g.Format(FontUi(), kFontSmall * u, 600, HAlign::Center, VAlign::Middle);
        auto drawFpsBox = [&](const Rect& box, const std::wstring& text, int focus, const char* id) {
            const bool focused = app.fpsEditFocus == focus;
            g.FillRect(box, Color(app.snapshot.fpsCap.exists ? kSurface3 : kSurface2));
            g.StrokeRound(box, 0.0f, Color(focused ? kAccent : kLine), 1.0f);
            const std::wstring shown = app.snapshot.fpsCap.exists ? text : std::wstring(L"—");
            if (!shown.empty()) g.Text(box, shown, fpsFmt, Color(fpsEditable ? kText : kText3));
            if (focused && app.fpsCaretOn) {
                const D2D1_SIZE_F size = shown.empty() ? D2D1_SIZE_F{0.0f, 0.0f} : g.Measure(shown, fpsFmt);
                const float caretX = (box.left + box.right) * 0.5f + size.width * 0.5f + 1.0f * u;
                g.Line(caretX, box.top + 6.0f * u, caretX, box.bottom - 6.0f * u, Color(kAccent), 1.0f * u);
            }
            ui.AddHit(id, box, fpsEditable);
        };
        drawFpsBox(app.editFrontRect, app.fpsFrontText, 1, "home.fpsfront");
        drawFpsBox(app.editBackRect, app.fpsBackText, 2, "home.fpsback");
        ui.Button("home.savefps", c.R(cw - 146, c.y + 20, 146, 30),
                  app.fpsSaving ? L"正在写入…" : L"保存到 Config.wtf", ButtonKind::Secondary,
                  app.snapshot.fpsCap.exists && app.snapshot.clientValid && !app.fpsSaving && !busy, kFontSmall);
        auto* note = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Top, true);
        std::wstring caption;
        if (!app.snapshot.fpsCap.exists) {
            caption = L"客户端内没有 WTF\\Config.wtf，请先启动一次游戏。";
        } else if (app.fpsSaving) {
            caption = L"正在后台写入 Config.wtf，完成后会回读校验并在日志中给出备份路径。";
        } else {
            caption = L"写入前会备份原文件并回读校验；建议先退出游戏。游戏内输入 /console MaxFPS 30 可即时生效。";
        }
        g.Text(c.R(0, c.y + 58, cw, 20), caption, note, Color(kText3));
        c.Gap(80);
        c.Gap(12);
    }

    // 7. 日志。
    {
        Section(c, L"日志", L"最近输出");
        c.Gap(24);
        CardRect(c, c.R(0, c.y, cw, 108));
        const std::vector<std::wstring> lines = app.log.Snapshot();
        auto* mono = g.Format(FontMono(), 11.0f * u, 400, HAlign::Left, VAlign::Top);
        const size_t visible = 6;
        const size_t start = lines.size() > visible ? lines.size() - visible : 0;
        float lineY = c.y + 8;
        for (size_t i = start; i < lines.size(); ++i) {
            const float width = std::max(10.0f, c.S(cw - 24));
            g.Text(c.R(12, lineY, cw - 24, 15), g.Ellipsize(lines[i], mono, width), mono, Color(kText2));
            lineY += 15.0f;
        }
        c.Gap(108);
        c.Gap(10);
    }

    // 8. 动作行。
    {
        ui.Button("home.refresh", c.R(0, c.y, 96, 32), L"检查状态", ButtonKind::Secondary, true, kFontSmall);
        ui.Button("home.panellog", c.R(104, c.y, 132, 32), L"打开悬浮面板日志", ButtonKind::Secondary,
                  !app.lastPanelLog.empty() && !app.toolRun.running, kFontSmall);
        ui.Button("home.carrierlog", c.R(244, c.y, 122, 32), L"打开载体日志", ButtonKind::Secondary,
                  !app.lastWatchLog.empty() && !app.toolRun.running, kFontSmall);
        c.Gap(32);
    }

    FinishScroll(app, c, 10.0f);
}

// ---------- 安装向导 ----------

namespace {

void DrawWizardSteps(App& app, float topDip) {
    Gfx& g = app.gfx;
    const float u = AppScale(app);
    const float left = ContentLeftDip(app);
    const float width = ContentWidthDip(app);
    static const wchar_t* names[5] = {L"选择客户端", L"环境检查", L"文件清单", L"安装", L"完成"};
    const int current = static_cast<int>(app.wizardStep);
    auto* smallFmt = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
    std::vector<float> widths;
    float total = 0.0f;
    for (int i = 0; i < 5; ++i) {
        const float w = 23.0f * u + g.Measure(names[i], smallFmt).width;
        widths.push_back(w);
        total += w;
    }
    const float gap = std::max(8.0f * u, (width - total) / 4.0f);
    float x = left;
    for (int i = 0; i < 5; ++i) {
        const bool done = i < current;
        const bool active = i == current;
        const Rect dot(x, topDip, x + 18.0f * u, topDip + 18.0f * u);
        const unsigned fill = done ? kJadeDeep : (active ? kAccentDeep : kSurface);
        const unsigned border = done ? kLineJade : (active ? kAccentLine : kLineSoft);
        g.FillRound(dot, 9.0f * u, Color(fill));
        g.StrokeRound(dot, 9.0f * u, Color(border));
        auto* glyph = g.Format(FontUi(), 10.0f * u, 700, HAlign::Center, VAlign::Middle);
        if (done) {
            g.Text(dot, L"✓", glyph, Color(kJade));
        } else {
            g.Text(dot, std::to_wstring(i + 1), glyph, Color(active ? kAccentBright : kText3));
        }
        if (i < 4) {
            const float lineX = x + 4.0f * u;
            g.Line(lineX, topDip + 9.0f * u, lineX + gap - 8.0f * u, topDip + 9.0f * u,
                   Color(done ? kLineJade : kLineSoft), 1.0f);
        }
        auto* nameFmt = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
        g.Text(Rect(x + 23.0f * u, topDip, x + 23.0f * u + widths[static_cast<size_t>(i)] + 2.0f * u,
                    topDip + 18.0f * u),
               names[i], nameFmt, Color(done ? kText2 : (active ? kText : kText3)));
        x += widths[static_cast<size_t>(i)] + gap;
    }
}

// 客户端 / 来源选择卡。占用 56 设计单位。
void DrawPathCard(Ctx& c, const std::wstring& path, const std::wstring& placeholder, const char* browseId) {
    const float u = c.u;
    const float cw = c.Cw();
    CardRect(c, c.R(0, c.y, cw, 56), kLine);
    c.g.Text(c.R(16, c.y, 20, 56), L"▤", c.g.Format(FontUi(), 13.0f * u, 700, HAlign::Left, VAlign::Middle),
             Color(kAccent));
    auto* body = c.g.Format(FontUi(), kFontBody * u, 400, HAlign::Left, VAlign::Middle);
    const float right = cw - 16.0f - 78.0f - 10.0f;
    const std::wstring shown = path.empty() ? placeholder : path;
    c.g.Text(c.R(38, c.y, right - 38, 56),
             c.g.Ellipsize(shown, body, std::max(10.0f, c.S(right - 38))), body,
             Color(path.empty() ? kText3 : kText));
    c.ui.Button(browseId, c.R(cw - 16 - 78, c.y + 14, 78, 28), L"浏览", ButtonKind::Secondary, true, kFontSmall);
    c.Gap(56);
}

}  // namespace

void DrawWizard(App& app) {
    std::wstring title;
    std::wstring subtitle;
    switch (app.wizardStep) {
        case WizardStep::Client:
            title = L"安装到客户端";
            subtitle = L"只向客户端写入发行包内的文件。已有的一致文件会跳过；需要改动的文件先备份，"
                       L"再合并写入本工具需要的配置项，不覆盖玩家原有设置。";
            break;
        case WizardStep::Checks:
            title = L"环境检查";
            subtitle = L"只读预检，不改动客户端。";
            break;
        case WizardStep::Files:
            title = L"文件清单";
            subtitle = L"逐项核对将在客户端中处理的目标文件。";
            break;
        case WizardStep::Install:
        case WizardStep::Done:
            title = L"安装";
            subtitle = app.wizardStep == WizardStep::Done
                           ? L"安装事务已结束，下面是本次真实执行结果。"
                           : L"写入在后台线程执行，界面保持可操作；完成前请勿强制结束程序。";
            break;
    }
    const bool withSteps = app.wizardStep != WizardStep::Install && app.wizardStep != WizardStep::Done;
    const HeaderInfo header = DrawPageHeader(app, title, subtitle, withSteps);
    const float u = AppScale(app);
    const float bodyTop = AppBodyRect(app).top;
    if (withSteps) DrawWizardSteps(app, bodyTop + header.stepsTop * u);
    const float contentTop = bodyTop + header.contentStart * u;
    ScopedClip contentClip(&app.gfx, ContentClipRect(app, contentTop));
    switch (app.wizardStep) {
        case WizardStep::Client:
        case WizardStep::Checks:
        case WizardStep::Files:
        case WizardStep::Install:
        case WizardStep::Done: break;
    }
    // 步骤内容以统一的内容顶部绘制。
    const float savedScroll = app.scroll;
    App& appRef = app;
    (void)savedScroll;
    (void)appRef;
    switch (app.wizardStep) {
        case WizardStep::Client: {
            Ctx c = MakeCtx(app, contentTop);
            const float cw = c.Cw();
            const float uu = c.u;
            Gfx& g = c.g;

            // 当前客户端：一张卡同时显示名称、路径与识别状态，避免同一路径在页面里重复出现。
            Section(c, L"当前客户端", L"安装目标");
            c.Gap(24);
            {
                const bool hasPath = !app.clientRoot.empty();
                const bool ok = app.snapshot.clientValid;
                CardRect(c, c.R(0, c.y, cw, 64), ok ? kAccentLine : kLine);
                auto* iconFmt = g.Format(FontUi(), 13.0f * uu, 700, HAlign::Left, VAlign::Middle);
                g.Text(c.R(16, c.y, 20, 64), L"▤", iconFmt, Color(kAccent));
                const std::wstring name = ok ? app.snapshot.displayName
                                             : (hasPath ? std::wstring(L"无法识别该客户端目录")
                                                        : std::wstring(L"尚未选择客户端目录"));
                const std::wstring pathText =
                    hasPath ? app.clientRoot
                            : std::wstring(L"选择包含 wow.exe 与 Data 目录的客户端根目录");
                const std::wstring badge = ok ? L"已识别" : (hasPath ? L"无效" : L"未选择");
                const unsigned badgeColor = ok ? kJadeText : (hasPath ? kWarn : kText3);
                const float tagW = TagWidth(c, badge) / uu;
                const float rightPx = c.X(cw - 16.0f);
                const float textRight =
                    rightPx - c.S(78.0f) - c.S(10.0f) - c.S(tagW) - c.S(10.0f);
                const float textLeft = c.X(38.0f);
                auto* nameFmt = g.Format(FontUi(), kFontBody * uu, 600, HAlign::Left, VAlign::Middle);
                auto* pathFmt = g.Format(FontUi(), kFontSmall * uu, 400, HAlign::Left, VAlign::Middle);
                g.Text(Rect(textLeft, c.Y(c.y + 8.0f), std::max(textLeft + 10.0f, textRight), c.Y(c.y + 30.0f)),
                       g.Ellipsize(name, nameFmt, std::max(10.0f, textRight - textLeft)), nameFmt,
                       Color(hasPath ? kText : kText3));
                g.Text(Rect(textLeft, c.Y(c.y + 32.0f), std::max(textLeft + 10.0f, textRight), c.Y(c.y + 52.0f)),
                       g.Ellipsize(pathText, pathFmt, std::max(10.0f, textRight - textLeft)), pathFmt, Color(kText3));
                const Rect tagRect(rightPx - c.S(78.0f) - c.S(10.0f) - c.S(tagW), c.Y(c.y + 21.0f),
                                   rightPx - c.S(78.0f) - c.S(10.0f), c.Y(c.y + 43.0f));
                Tag(c, tagRect, badge, badgeColor);
                c.ui.Button("wiz.browse", c.R(cw - 16.0f - 78.0f, c.y + 18.0f, 78.0f, 28.0f), L"浏览",
                            ButtonKind::Secondary, true, kFontSmall);
                c.Gap(64);
                std::wstring note;
                if (ok) {
                    note = L"已就绪：安装只写入发行包中的文件，控制台可独立于客户端安装。";
                } else if (!hasPath) {
                    note = L"需要包含 wow.exe 与 Data 目录的客户端根目录。";
                } else {
                    note = ClientErrorText(app.snapshot.clientError);
                }
                MutedText(c, c.R(2, c.y + 8, cw - 4, 24), note, kFontSmall);
                c.Gap(36);
            }

            // 更换客户端：最近使用与自动扫描合并成一个去重列表，点击整行即切换。
            {
                Section(c, L"更换客户端", app.scanLoading ? L"正在后台扫描…" : L"点击列表项切换");
                c.ui.Button("wiz.scan", c.R(cw - 92.0f, c.y + 1.0f, 92.0f, 26.0f),
                            app.scanLoading ? L"扫描中…" : L"重新扫描", ButtonKind::Ghost, !app.scanLoading,
                            kFontSmall);
                c.Gap(30);
                struct PickerRow {
                    std::wstring path;
                    std::wstring name;
                    size_t recentIndex = 0;
                    size_t candidateIndex = 0;
                    bool fromCandidate = false;
                    bool current = false;
                    bool hasRecord = false;
                };
                std::vector<PickerRow> rows;
                constexpr size_t kMaxPickerRows = 5;
                for (size_t i = 0; i < app.settings.recent.size() && rows.size() < kMaxPickerRows; ++i) {
                    const auto& recent = app.settings.recent[i];
                    if (!DirectoryExistsNoReparse(recent.path) || !TestClientRoot(recent.path)) continue;
                    PickerRow row;
                    row.path = recent.path;
                    row.name = recent.name.empty() ? DisplayNameForClientPath(recent.path) : recent.name;
                    row.recentIndex = i;
                    rows.push_back(std::move(row));
                }
                auto findRow = [&rows](const std::wstring& path) -> PickerRow* {
                    for (auto& row : rows) {
                        if (SamePath(row.path, path)) return &row;
                    }
                    return nullptr;
                };
                if (app.candidatesReady) {
                    for (size_t i = 0; i < app.candidates.size() && rows.size() < kMaxPickerRows; ++i) {
                        const auto& candidate = app.candidates[i];
                        if (PickerRow* existing = findRow(candidate.path)) {
                            existing->hasRecord = candidate.hasRecord;
                            continue;
                        }
                        PickerRow row;
                        row.path = candidate.path;
                        row.name = candidate.name;
                        row.candidateIndex = i;
                        row.fromCandidate = true;
                        row.hasRecord = candidate.hasRecord;
                        rows.push_back(std::move(row));
                    }
                }
                for (auto& row : rows) row.current = SamePath(row.path, app.clientRoot);
                if (rows.empty()) {
                    const std::wstring empty =
                        app.scanLoading
                            ? std::wstring(L"正在后台扫描固定磁盘，界面不受影响。")
                            : (app.candidatesReady
                                   ? std::wstring(L"未发现包含 wow.exe 与 Data 目录的客户端；可用「浏览」手动选择。")
                                   : std::wstring(L"尚未扫描；进入本页后会自动扫描一次，也可手动重新扫描。"));
                    MutedText(c, c.R(0, c.y, cw, 24), empty, kFontSmall);
                    c.Gap(30);
                } else {
                    auto* body = g.Format(FontUi(), kFontBody * uu, 500, HAlign::Left, VAlign::Middle);
                    auto* smallFmt = g.Format(FontUi(), kFontSmall * uu, 400, HAlign::Left, VAlign::Middle);
                    for (const auto& row : rows) {
                        CardRect(c, c.R(0, c.y, cw, 56), row.current ? kAccentLine : kLineSoft);
                        if (row.current) {
                            g.FillRound(c.R(0, c.y, cw, 56), kCardRadius * uu, Color(kAccentRow));
                        }
                        const std::wstring tag = row.current ? L"当前" : (row.hasRecord ? L"已安装" : L"可用");
                        const unsigned tagColor = row.current ? kJadeText : (row.hasRecord ? kText2 : kText3);
                        const float tagWpx = TagWidth(c, tag);
                        const float rightPx = c.X(cw - 16.0f);
                        const Rect tagRect(rightPx - tagWpx, c.Y(c.y + 18.0f), rightPx, c.Y(c.y + 38.0f));
                        Tag(c, tagRect, tag, tagColor);
                        const float textLeft = c.X(14.0f);
                        const float textRight = std::max(textLeft + 10.0f, tagRect.left - c.S(10.0f));
                        g.Text(Rect(textLeft, c.Y(c.y + 6.0f), textRight, c.Y(c.y + 26.0f)),
                               g.Ellipsize(row.name, body, textRight - textLeft), body, Color(kText));
                        g.Text(Rect(textLeft, c.Y(c.y + 28.0f), textRight, c.Y(c.y + 46.0f)),
                               g.Ellipsize(row.path, smallFmt, textRight - textLeft), smallFmt, Color(kText3));
                        char hitId[32] = {};
                        if (row.fromCandidate) {
                            std::snprintf(hitId, sizeof(hitId), "wiz.cand.%zu", row.candidateIndex);
                        } else {
                            std::snprintf(hitId, sizeof(hitId), "wiz.recent.%zu", row.recentIndex);
                        }
                        c.ui.AddHit(hitId, c.R(0, c.y, cw, 56), true);
                        c.Gap(64);
                    }
                }
            }

            // 组件来源：路径、来源徽章与浏览按钮合并到一张卡，状态不再单独占一行。
            {
                c.Gap(4);
                Section(c, L"组件来源", L"发行包或暂存目录，含清单文件");
                c.Gap(24);
                const bool exists = !app.stageRoot.empty() && DirectoryExistsNoReparse(app.stageRoot);
                CardRect(c, c.R(0, c.y, cw, 56), exists ? kAccentLine : kLine);
                auto* iconFmt = g.Format(FontUi(), 13.0f * uu, 700, HAlign::Left, VAlign::Middle);
                g.Text(c.R(16, c.y, 20, 56), L"▤", iconFmt, Color(kAccent));
                const std::wstring pathText =
                    app.stageRoot.empty() ? std::wstring(L"尚未选择组件来源") : app.stageRoot;
                const std::wstring badge =
                    app.stageRoot.empty() ? std::wstring(L"未配置") : (exists ? L"存在" : L"缺失");
                const unsigned badgeColor =
                    app.stageRoot.empty() ? kText3 : (exists ? kJadeText : kWarn);
                const float tagW = TagWidth(c, badge) / uu;
                const float rightPx = c.X(cw - 16.0f);
                const float textRight =
                    rightPx - c.S(78.0f) - c.S(10.0f) - c.S(tagW) - c.S(10.0f);
                const float textLeft = c.X(38.0f);
                auto* pathFmt = g.Format(FontUi(), kFontBody * uu, 400, HAlign::Left, VAlign::Middle);
                g.Text(Rect(textLeft, c.Y(c.y), std::max(textLeft + 10.0f, textRight), c.Y(c.y + 56.0f)),
                       g.Ellipsize(pathText, pathFmt, std::max(10.0f, textRight - textLeft)), pathFmt,
                       Color(app.stageRoot.empty() ? kText3 : kText));
                const Rect tagRect(rightPx - c.S(78.0f) - c.S(10.0f) - c.S(tagW), c.Y(c.y + 17.0f),
                                   rightPx - c.S(78.0f) - c.S(10.0f), c.Y(c.y + 39.0f));
                Tag(c, tagRect, badge, badgeColor);
                c.ui.Button("wiz.browsestage", c.R(cw - 16.0f - 78.0f, c.y + 14.0f, 78.0f, 28.0f), L"浏览",
                            ButtonKind::Secondary, true, kFontSmall);
                c.Gap(56);
                MutedText(c, c.R(2, c.y + 8, cw - 4, 24),
                          exists ? std::wstring(L"安装时从此目录读取发行包文件与文件清单。")
                                 : std::wstring(L"选择包含 install-manifest.json 的发行包目录。"),
                          kFontSmall);
                c.Gap(36);
            }
            FinishScroll(app, c, 10.0f);
            break;
        }
        case WizardStep::Checks: {
            Ctx c = MakeCtx(app, contentTop);
            const float uu = c.u;
            const float cw = c.Cw();
            Section(c, L"环境检查", L"只读，不改动客户端");
            c.Gap(24);
            const float rowH = 44.0f;
            CardRect(c, c.R(0, c.y, cw, rowH * 4.0f + 8.0f));
            const bool clientOk = app.snapshot.clientValid;
            const std::wstring clientDetail = clientOk ? (app.snapshot.displayName + L" · 32 位客户端")
                                                       : ClientErrorText(app.snapshot.clientError);
            const bool processOk = app.snapshot.processState == ProcessState::Stopped;
            const bool sourceConfigured = !app.stageRoot.empty() && DirectoryExistsNoReparse(app.stageRoot);
            // 预检完成时以真实源文件哈希结果为准；未完成前只确认来源目录存在。
            const bool sourceOk =
                app.install.ready ? (app.install.ok && app.install.preview.filesReady) : sourceConfigured;
            const std::wstring sourceDetail =
                app.install.ready
                    ? (!app.install.ok
                           ? std::wstring(L"组件来源预检失败：") + app.install.error
                           : (app.install.preview.filesReady
                                  ? app.stageRoot
                                  : (app.install.error.empty() ? std::wstring(L"组件来源缺少必需文件")
                                                               : std::wstring(L"预检未通过：") + app.install.error)))
                    : (sourceConfigured ? app.stageRoot : std::wstring(L"未配置组件来源"));
            const bool recordPresent = app.snapshot.recordState == control_center::CapturedRecordState::Present;
            CheckRow(c, c.R(0, c.y + 4, cw, rowH), clientOk, L"客户端可识别", clientDetail,
                     clientOk ? L"通过" : L"注意");
            CheckRow(c, c.R(0, c.y + 4 + rowH, cw, rowH), processOk, L"游戏与载体已退出", ProcessText(app.snapshot),
                     processOk ? L"通过" : L"注意");
            CheckRow(c, c.R(0, c.y + 4 + rowH * 2, cw, rowH), sourceOk,
                     app.install.ready ? L"安装预检" : L"组件来源完整", sourceDetail, sourceOk ? L"通过" : L"注意");
            CheckRow(c, c.R(0, c.y + 4 + rowH * 3, cw, rowH), !recordPresent, L"安装记录",
                     recordPresent ? Fmt(L"已有安装记录（%zu 项），将按新清单更新", app.snapshot.recordEntries)
                                   : std::wstring(L"首次安装，没有旧记录"),
                     recordPresent ? L"注意" : L"通过");
            c.Gap(rowH * 4.0f + 8.0f);
            c.Gap(10);
            const bool canInstall = AppCanInstall(app);
            std::wstring summary;
            if (app.install.ready && app.install.ok) {
                size_t add = 0, modify = 0, skip = 0;
                for (const auto& item : app.install.preview.items) {
                    switch (item.state) {
                        case PreviewState::Absent: ++add; break;
                        case PreviewState::Present: ++modify; break;
                        case PreviewState::Same:
                        case PreviewState::MissingOptional: ++skip; break;
                        default: break;
                    }
                }
                summary = Fmt(L"可安装：%s　新增 %zu · 修改 %zu · 跳过 %zu",
                              canInstall ? L"是" : L"否，请先处理冲突", add, modify, skip);
            } else {
                summary = L"可安装：否　（组件来源未配置，本阶段只核对目标文件状态）";
            }
            auto* note = c.g.Format(FontUi(), kFontSmall * uu, 400, HAlign::Left, VAlign::Top, true);
            c.g.Text(c.R(0, c.y, cw, 24), summary, note, Color(canInstall ? kText2 : kWarn));
            c.Gap(28);
            MutedText(c, c.R(0, c.y, cw, 40),
                      L"以上为真实预检结果（源文件与目标文件哈希）；开始安装会按此清单写入客户端。");
            c.Gap(46);
            FinishScroll(app, c, 10.0f);
            break;
        }
        case WizardStep::Files: {
            Ctx c = MakeCtx(app, contentTop);
            const float uu = c.u;
            const float cw = c.Cw();
            Gfx& g = c.g;
            if (app.install.ready && app.install.ok) {
                Section(c, L"文件清单", L"来自真实预检");
                c.Gap(24);
                struct Group {
                    const wchar_t* title;
                    const wchar_t* tag;
                    unsigned color;
                    std::wstring note;
                    std::vector<const PreviewItem*> rows;
                };
                Group groups[3];
                groups[0].title = L"新增";
                groups[0].tag = L"新增";
                groups[0].color = kJade;
                groups[1].title = L"修改";
                groups[1].tag = L"修改";
                groups[1].color = kAccent;
                groups[2].title = L"跳过";
                groups[2].tag = L"跳过";
                groups[2].color = kText3;
                std::vector<const PreviewItem*> conflicts;
                for (const auto& item : app.install.preview.items) {
                    switch (item.state) {
                        case PreviewState::Absent: groups[0].rows.push_back(&item); break;
                        case PreviewState::Present: groups[1].rows.push_back(&item); break;
                        case PreviewState::Invalid: conflicts.push_back(&item); break;
                        default: groups[2].rows.push_back(&item); break;
                    }
                }
                groups[0].note = Fmt(L"%zu 项 · 客户端原本没有，卸载时删除", groups[0].rows.size());
                groups[1].note = Fmt(L"%zu 项 · 备份后写入，卸载时还原", groups[1].rows.size());
                groups[2].note = Fmt(L"%zu 项 · 内容一致或可选缺失，不动", groups[2].rows.size());
                for (const auto& group : groups) {
                    if (group.rows.empty()) continue;
                    Section(c, group.title, group.note);
                    c.Gap(22);
                    CardRect(c, c.R(0, c.y, cw, static_cast<float>(group.rows.size()) * 40.0f + 8.0f));
                    float rowY = c.y + 4;
                    for (const auto* item : group.rows) {
                        const std::wstring note =
                            item->state == PreviewState::MissingOptional ? L"可选组件缺失" : L"";
                        FileRow(c, c.R(0, rowY, cw, 40), item->member.target, group.tag, group.color, note);
                        rowY += 40;
                    }
                    c.Gap(static_cast<float>(group.rows.size()) * 40.0f + 8.0f);
                    c.Gap(10);
                }
                if (!conflicts.empty()) {
                    Section(c, L"冲突", Fmt(L"%zu 项需要处理", conflicts.size()));
                    c.Gap(22);
                    CardRect(c, c.R(0, c.y, cw, static_cast<float>(conflicts.size()) * 40.0f + 8.0f), kWarn);
                    float rowY = c.y + 4;
                    for (const auto* item : conflicts) {
                        std::wstring issue;
                        const int needed = MultiByteToWideChar(CP_UTF8, 0, item->issue.c_str(), -1, nullptr, 0);
                        if (needed > 1) {
                            issue.resize(static_cast<size_t>(needed - 1));
                            MultiByteToWideChar(CP_UTF8, 0, item->issue.c_str(), -1, issue.data(), needed);
                        }
                        FileRow(c, c.R(0, rowY, cw, 40), item->member.target, L"冲突", kWarn, issue);
                        rowY += 40;
                    }
                    c.Gap(static_cast<float>(conflicts.size()) * 40.0f + 8.0f);
                    c.Gap(10);
                }
                CardRect(c, c.R(0, c.y, cw, 44));
                MutedText(c, c.R(16, c.y + 13, cw - 32, 20),
                          L"修改的原件备份到客户端内 host64\\dlss5-backup，卸载时按安装记录还原。");
                c.Gap(52);
            } else if (app.installLoading) {
                CardRect(c, c.R(0, c.y, cw, 90));
                MutedText(c, c.R(16, c.y + 18, cw - 32, 24), L"正在后台读取组件来源与文件清单…");
                MutedText(c, c.R(16, c.y + 46, cw - 32, 24), L"界面保持可操作，结果返回后自动刷新。");
                c.Gap(100);
            } else {
                Section(c, L"文件清单", app.stageRoot.empty() ? L"目标状态（组件来源未配置）" : L"目标状态");
                c.Gap(24);
                if (app.install.ready && !app.install.ok) {
                    auto* note = g.Format(FontUi(), kFontSmall * uu, 400, HAlign::Left, VAlign::Top, true);
                    g.Text(c.R(0, c.y, cw, 26), L"组件来源读取失败：" + app.install.error, note, Color(kWarn));
                    c.Gap(32);
                }
                if (app.snapshot.members.empty()) {
                    CardRect(c, c.R(0, c.y, cw, 60));
                    MutedText(c, c.R(16, c.y + 18, cw - 32, 24), L"安装清单不可用；请确认产品目录内有清单文件。");
                    c.Gap(62);
                } else {
                    const float rows = static_cast<float>(app.snapshot.members.size());
                    CardRect(c, c.R(0, c.y, cw, rows * 40.0f + 8.0f));
                    float rowY = c.y + 4;
                    for (const auto& member : app.snapshot.members) {
                        FileRow(c, c.R(0, rowY, cw, 40), member.target,
                                MemberStateText(member.state), StateColor(member.state), std::wstring());
                        rowY += 40;
                    }
                    c.Gap(rows * 40.0f + 8.0f);
                }
                c.Gap(10);
                MutedText(c, c.R(0, c.y, cw, 34),
                          L"以上为客户端内目标文件的真实状态（只读）。配置组件来源后，这里会显示新增 / 修改 / 跳过分类。");
                c.Gap(40);
            }
            {
                const bool canInstallNow = AppCanInstall(app);
                std::wstring hint;
                if (app.installRun.running) {
                    hint = L"安装正在后台执行，界面保持可操作；完成前请勿强制结束程序。";
                } else if (app.installRun.finished && !app.installRun.ok) {
                    hint = L"上次安装未完成：" + app.installRun.error + L" 可修正后重试。";
                } else if (app.installRun.finished && app.installRun.ok) {
                    hint = L"本次安装已提交：" + app.installRun.detail;
                } else if (canInstallNow) {
                    hint = L"预检通过；点「开始安装」会真实写入客户端，修改项先备份。";
                } else if (app.install.ready && !app.install.ok) {
                    hint = L"组件来源预检未通过：" + app.install.error;
                } else if (app.snapshot.processState != control_center::ProcessState::Stopped) {
                    hint = L"请先退出游戏与载体，再开始安装。";
                } else {
                    hint = L"开始安装前需要完成环境检查与组件来源校验。";
                }
                auto* hintFmt = g.Format(FontUi(), kFontSmall * uu, 400, HAlign::Left, VAlign::Top, true);
                g.Text(c.R(0, c.y, cw, 44), hint, hintFmt, Color(kText3));
                c.Gap(48);
            }
            FinishScroll(app, c, 10.0f);
            break;
        }
        case WizardStep::Install:
        case WizardStep::Done: {
            Ctx c = MakeCtx(app, contentTop);
            const float uu = c.u;
            const float cw = c.Cw();
            Gfx& g = c.g;
            const bool running = app.installRun.running;
            const bool failedRun = app.installRun.finished && !app.installRun.ok;
            const bool succeeded = app.installRun.finished && app.installRun.ok;
            if (running || failedRun) {
                DrawOperationProgress(c, app.installRun, false, L"正在安装…", L"安装完成", L"安装失败");
            }
            if (succeeded && app.wizardStep == WizardStep::Done) {
                CardRect(c, c.R(0, c.y, cw, 76), kLineSoft);
                const Rect badge = c.R(16, c.y + 12, 52, 52);
                g.FillEllipse(badge.CenterX(), badge.CenterY(), 26.0f * uu, 26.0f * uu, Color(kJadeDeep));
                g.StrokeRound(badge, 26.0f * uu, Color(kLineJade));
                g.Text(badge, L"✓", g.Format(FontUi(), kFontStat * uu, 600, HAlign::Center, VAlign::Middle),
                       Color(kJade));
                g.Text(c.R(84, c.y + 14, cw - 100, 24), L"安装完成",
                       g.Format(FontUi(), kFontBody * uu, 700, HAlign::Left, VAlign::Middle), Color(kText));
                g.Text(c.R(84, c.y + 42, cw - 100, 20),
                       app.installRun.detail.empty() ? std::wstring(L"事务已提交。") : app.installRun.detail,
                       g.Format(FontUi(), kFontSmall * uu, 400, HAlign::Left, VAlign::Middle), Color(kText2));
                c.Gap(86);
                CardRect(c, c.R(0, c.y, cw, 3 * 40.0f + 8.0f));
                KvRow(c, c.R(0, c.y + 6, cw, 36), L"客户端",
                      app.clientRoot.empty() ? std::wstring(L"—") : app.clientRoot);
                KvRow(c, c.R(0, c.y + 46, cw, 36), L"事务状态", L"已提交（安装记录已发布）", kJadeText);
                KvRow(c, c.R(0, c.y + 86, cw, 36), L"结果",
                      app.installRun.detail.empty() ? std::wstring(L"—") : app.installRun.detail);
                c.Gap(138);
                MutedText(c, c.R(0, c.y, cw, 40),
                          L"下一步可在维护页查看已安装组件、备份位置，或执行修复与卸载。");
                c.Gap(46);
            } else if (!running && !failedRun && app.wizardStep == WizardStep::Install) {
                CardRect(c, c.R(0, c.y, cw, 112), kLineSoft);
                g.Text(c.R(16, c.y + 12, cw - 32, 24), L"安装会真实写入客户端",
                       g.Format(FontUi(), kFontBody * uu, 600, HAlign::Left, VAlign::Middle), Color(kAccent));
                MutedText(c, c.R(16, c.y + 44, cw - 32, 54),
                          L"点页脚「开始安装」后会创建事务目录、逐项备份并写入文件，最后提交安装记录；"
                          L"失败会按事务日志回滚。进度只显示真实阶段，不伪造百分比。");
                c.Gap(122);
            } else if (failedRun && app.wizardStep == WizardStep::Done) {
                MutedText(c, c.R(0, c.y, cw, 40),
                          L"安装未完成，已写入的内容由事务日志保护；可返回上一步修正后重试。");
                c.Gap(46);
            }
            FinishScroll(app, c, 10.0f);
            break;
        }
    }
}

// ---------- 维护页 ----------

void DrawMaintenance(App& app) {
    const HeaderInfo header = DrawPageHeader(app, L"维护",
                                             L"查看已安装组件与备份位置，或移除本工具写入客户端的文件。", false);
    const float u = AppScale(app);
    const float bodyTop = AppBodyRect(app).top;
    const float contentTop = bodyTop + header.contentStart * u;
    ScopedClip contentClip(&app.gfx, ContentClipRect(app, contentTop));
    Ctx c = MakeCtx(app, contentTop);
    const float cw = c.Cw();
    Gfx& g = c.g;
    const bool installed = app.snapshot.recordState == control_center::CapturedRecordState::Present;

    Section(c, L"已安装", installed ? L"组件正常" : L"未安装");
    c.Gap(24);
    {
        CardRect(c, c.R(0, c.y, cw, 156));
        for (int i = 0; i < 4; ++i) {
            const wchar_t* key = i == 0 ? L"客户端" : (i == 1 ? L"安装记录" : (i == 2 ? L"校验" : L"备份"));
            std::wstring value;
            switch (i) {
                case 0: value = app.clientRoot.empty() ? L"—" : app.clientRoot; break;
                case 1:
                    value = installed ? Fmt(L"%zu 项 · schema 1", app.snapshot.recordEntries) : std::wstring(L"—");
                    break;
                case 2:
                    if (!app.snapshot.hashesChecked) {
                        value = L"未校验（点「检查状态」在后台校验）";
                    } else {
                        value = Fmt(L"已校验 %zu / %zu 项", app.snapshot.verified, app.snapshot.catalogMembers);
                        if (app.snapshot.modified > 0 || app.snapshot.missing > 0) {
                            value += Fmt(L"（修改 %zu · 缺失 %zu）", app.snapshot.modified, app.snapshot.missing);
                        }
                    }
                    break;
                case 3:
                    if (installed) {
                        value = DirectoryExistsNoReparse(app.clientRoot + L"\\host64\\dlss5-backup")
                                    ? L"host64\\dlss5-backup（存在）"
                                    : L"host64\\dlss5-backup（未找到）";
                    } else {
                        value = L"—";
                    }
                    break;
                default: break;
            }
            KvRow(c, c.R(0, c.y + 8 + static_cast<float>(i) * 37, cw, 36), key, value);
        }
        c.Gap(156);
    }

    c.Gap(10);
    Section(c, L"组件");
    c.Gap(24);
    {
        CardRect(c, c.R(0, c.y, cw, 144));
        const MemberStatus* core = FindMember(app.snapshot, L"d3d9.dll");
        const MemberStatus* sr = FindMember(app.snapshot, L"host64/nvngx_dlss.dll");
        const MemberStatus* fg = FindMember(app.snapshot, L"host64/nvngx_dlssg.dll");
        struct RowDef {
            const wchar_t* label;
            const MemberStatus* member;
            std::wstring detail;
        };
        const RowDef rows[3] = {
            {L"载体 d3d9.dll", core, L"ReShade 载体"},
            {L"超分 / 抗锯齿", sr, L"DLSS SR · 模型待读取"},
            {L"补帧", fg, L"DLSS FG · MFG 待读取"},
        };
        for (int i = 0; i < 3; ++i) {
            const bool ok = rows[i].member && StateInstalled(rows[i].member->state);
            const std::wstring status =
                rows[i].member ? MemberStateText(rows[i].member->state) : std::wstring(L"未知");
            CheckRow(c, c.R(0, c.y + 4 + static_cast<float>(i) * 46, cw, 46), ok, rows[i].label, rows[i].detail,
                     status);
        }
        c.Gap(144);
    }

    c.Gap(10);
    {
        const bool busy = app.installRun.running || app.repairRun.running || app.uninstallRun.running;
        app.ui.Button("maint.check", c.R(0, c.y, 252, 32), L"检查状态", ButtonKind::Secondary, !busy, kFontSmall);
        const bool canRepair = installed && app.catalog.ok && !app.clientRoot.empty() && !busy;
        app.ui.Button("maint.repair", c.R(268, c.y, 252, 32), L"修复安装", ButtonKind::Secondary, canRepair,
                      kFontSmall);
        c.Gap(32);
    }
    {
        const bool backupExists = installed && DirectoryExistsNoReparse(app.clientRoot + L"\\host64\\dlss5-backup");
        c.Gap(8);
        app.ui.Button("maint.backup", c.R(0, c.y, cw, 32), L"打开备份目录", ButtonKind::Secondary,
                      backupExists, kFontSmall);
        c.Gap(32);
    }
    MutedText(c, c.R(0, c.y, cw, 34),
              L"检查状态会重新读取客户端记录并在后台校验文件哈希；修复安装会按事务日志重写缺失或被改动的成员。");
    c.Gap(40);
    if (app.repairRun.running || app.repairRun.finished) {
        DrawOperationProgress(c, app.repairRun, false, L"正在修复…", L"修复完成", L"修复失败");
    }

    c.Gap(4);
    {
        const bool busy = app.installRun.running || app.repairRun.running || app.uninstallRun.running;
        const AdvancedSummary& adv = app.advanced;
        std::wstring note = L"ReShade.ini";
        if (adv.loading) note = L"读取中…";
        else if (!adv.error.empty()) note = L"读取失败";
        else if (adv.loaded) note = adv.tierName;
        Section(c, L"高级工具", note.c_str());
        c.Gap(24);

        const bool hasError = !adv.error.empty();
        const float cardTop = c.y;
        const float cardH = 10 + 28 + 8 + 170 + 8 + 28 + 8 + 28 + 8 + 34 + (hasError ? 24.0f : 0.0f) + 8;
        CardRect(c, c.R(0, cardTop, cw, cardH));
        MutedText(c, c.R(16, cardTop + 10, 120, 28), L"档位预设");
        for (int i = 0; i < 4; ++i) {
            const auto& tier = Ds5Tiers()[static_cast<size_t>(i)];
            const std::string id = "maint.adv.tier." + std::to_string(i);
            app.ui.Button(id.c_str(), c.R(16 + 136.0f * i, cardTop + 10, 128, 28), tier.name,
                          ButtonKind::Secondary, !busy && !adv.saving, kFontSmall);
        }

        const float paramTop = cardTop + 46;
        const float minusX = cw - 16 - 28 - 8 - 28;
        const float plusX = cw - 16 - 28;
        struct ParamDef {
            const wchar_t* label;
            std::wstring value;
            const char* key;
        };
        const ParamDef params[5] = {
            {L"模型", Ds5ModelName(adv.edit.model), "model"},
            {L"强度", Fmt(L"%.2f", adv.edit.intensity), "intensity"},
            {L"色调结构", Fmt(L"%.2f", adv.edit.localTone), "tone"},
            {L"结构强度", Fmt(L"%.2f", adv.edit.localStructure), "structure"},
            {L"皮肤结构", Fmt(L"%.2f", adv.edit.skinStructure), "skin"},
        };
        for (int i = 0; i < 5; ++i) {
            const float rowTop = paramTop + static_cast<float>(i) * 34;
            MutedText(c, c.R(16, rowTop + 4, 90, 26), params[i].label);
            g.Text(c.R(112, rowTop, 240, 32), params[i].value,
                   g.Format(FontUi(), kFontBody * u, 600, HAlign::Left, VAlign::Middle), Color(kText));
            const std::string minusId = std::string("maint.adv.") + params[i].key + ".minus";
            const std::string plusId = std::string("maint.adv.") + params[i].key + ".plus";
            app.ui.Button(minusId.c_str(), c.R(minusX, rowTop + 3, 28, 26), L"−", ButtonKind::Secondary,
                          !busy && !adv.saving, kFontSmall);
            app.ui.Button(plusId.c_str(), c.R(plusX, rowTop + 3, 28, 26), L"+", ButtonKind::Secondary,
                          !busy && !adv.saving, kFontSmall);
        }

        const float switchTop = cardTop + 224;
        const std::wstring nrLabel = std::wstring(L"补帧 NeuralRendering：") +
                                     (adv.edit.neuralRendering ? L"开" : L"关");
        const std::wstring detachedLabel =
            std::wstring(L"独立呈现：") + (adv.editDetached ? L"开" : L"关");
        const std::wstring opticalLabel =
            std::wstring(L"Optical Flow：") + (adv.editOpticalFlow ? L"开" : L"关");
        app.ui.Button("maint.adv.nr.toggle", c.R(16, switchTop, 172, 28), nrLabel.c_str(), ButtonKind::Secondary,
                      !busy && !adv.saving, kFontSmall);
        app.ui.Button("maint.adv.detached.toggle", c.R(196, switchTop, 172, 28), detachedLabel.c_str(),
                      ButtonKind::Secondary, !busy && !adv.saving, kFontSmall);
        app.ui.Button("maint.adv.optical.toggle", c.R(376, switchTop, 172, 28), opticalLabel.c_str(),
                      ButtonKind::Secondary, !busy && !adv.saving, kFontSmall);

        const float actionTop = cardTop + 260;
        const bool canWrite = adv.loaded && adv.settings.readable && !busy && !adv.saving;
        app.ui.Button("maint.adv.read", c.R(16, actionTop, 128, 28), adv.loading ? L"读取中…" : L"读取当前值",
                      ButtonKind::Secondary, !busy && !adv.loading, kFontSmall);
        app.ui.Button("maint.adv.save", c.R(152, actionTop, 128, 28), adv.saving ? L"写入中…" : L"保存设置",
                      ButtonKind::Primary, canWrite, kFontSmall);
        app.ui.Button("maint.adv.fill", c.R(288, actionTop, 128, 28), L"填入当前值", ButtonKind::Secondary,
                      canWrite, kFontSmall);
        app.ui.Button("maint.adv.defaults", c.R(424, actionTop, 128, 28), L"恢复候选默认", ButtonKind::Secondary,
                      !busy && !adv.saving, kFontSmall);

        std::wstring statusLine = adv.status;
        if (statusLine.empty() && adv.loaded && adv.settings.readable) {
            statusLine = L"当前：档位 " + adv.tierName + L" · 模型 " + adv.modelName;
        }
        if (adv.loading && statusLine.empty()) statusLine = L"正在后台读取 host64\\ReShade.ini…";
        if (!statusLine.empty()) {
            MutedText(c, c.R(16, cardTop + 296, cw - 32, 34), statusLine, kFontSmall);
        }
        if (hasError) {
            MutedText(c, c.R(16, cardTop + 330, cw - 32, 22), adv.error, kFontSmall);
        }
        c.Gap(cardH + 10);

        // 方案列表与保存。
        const size_t shown = (std::min)(adv.schemes.size(), static_cast<size_t>(6));
        const float rowsH = static_cast<float>(shown == 0 ? 1 : shown) * 30.0f;
        const float schemeTop = c.y;
        const float schemeH = 10 + 28 + rowsH + 8 + 20 + 4 + 28 + 8 + 20 + 8;
        CardRect(c, c.R(0, schemeTop, cw, schemeH));
        MutedText(c, c.R(16, schemeTop + 10, 260, 28), L"方案");
        app.ui.Button("maint.adv.schemes.refresh", c.R(cw - 16 - 96, schemeTop + 8, 96, 26),
                      adv.schemesLoading ? L"扫描中…" : L"重新扫描", ButtonKind::Secondary,
                      !busy && !adv.schemesLoading, kFontSmall);
        float rowY = schemeTop + 38;
        if (shown == 0) {
            MutedText(c, c.R(16, rowY, cw - 32, 30),
                      adv.schemesError.empty() ? L"暂无方案；保存当前参数后会出现在这里。" : adv.schemesError);
        } else {
            for (size_t i = 0; i < shown; ++i) {
                const Ds5Scheme& scheme = adv.schemes[i];
                std::wstring label = scheme.name + (scheme.builtin ? L"（内置）" : L"（用户）");
                MutedText(c, c.R(16, rowY + 2, cw - 32 - 96, 26), label);
                const std::string applyId = "maint.adv.scheme.apply." + std::to_string(i);
                app.ui.Button(applyId.c_str(), c.R(cw - 16 - 80, rowY + 2, 80, 26), L"应用",
                              ButtonKind::Secondary, !busy && !adv.schemeSaving, kFontSmall);
                rowY += 30;
            }
        }
        const float nameLabelTop = schemeTop + 38 + rowsH + 8;
        MutedText(c, c.R(16, nameLabelTop, cw - 32, 20), L"保存当前参数为方案（1-24 个字符）", kFontSmall);
        const Rect schemeEditRect = c.R(16, nameLabelTop + 24, 280, 28);
        app.editSchemeNameRect = schemeEditRect;
        const Rect body = AppBodyRect(app);
        const float footerTop = AppFooterRect(app).top;
        app.schemeNameVisible = schemeEditRect.top >= body.top && schemeEditRect.bottom <= footerTop;
        app.ui.Button("maint.adv.scheme.save", c.R(cw - 16 - 120, nameLabelTop + 24, 120, 28),
                      adv.schemeSaving ? L"保存中…" : L"保存方案", ButtonKind::Secondary,
                      !busy && !adv.schemeSaving && !app.clientRoot.empty(), kFontSmall);
        MutedText(c, c.R(16, nameLabelTop + 58, cw - 32, 20),
                  L"方案保存在本机用户数据目录，应用方案会按当前参数写入 ReShade.ini。", kFontSmall);
        c.Gap(schemeH + 12);
    }

    // 阶段二工具：诊断、恢复、清单、面板、助手与管线开关。
    {
        Section(c, L"工具", app.toolRun.running ? L"正在执行" : L"诊断与修复助手");
        c.Gap(24);
        const float toolH = 168;
        CardRect(c, c.R(0, c.y, cw, toolH));
        auto* toolFmt = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Middle);
        g.Text(c.R(16, c.y + 12, cw - 32, 16), L"以下操作都在后台线程执行；失败会写入日志，不会吞掉错误。", toolFmt,
               Color(kText3));
        const float btnW = (cw - 32 - 8) / 2.0f;
        const bool canAct = !app.toolRun.running;
        const bool hasClient = app.snapshot.clientValid && !app.clientRoot.empty();
        app.ui.Button("maint.tools.diagnostics", c.R(16, c.y + 36, btnW, 28), L"导出诊断", ButtonKind::Secondary,
                  canAct && hasClient, kFontSmall);
        app.ui.Button("maint.tools.manifest", c.R(16 + btnW + 8, c.y + 36, btnW, 28), L"导出移入清单",
                  ButtonKind::Secondary, canAct && app.catalog.ok, kFontSmall);
        app.ui.Button("maint.tools.restore", c.R(16, c.y + 68, btnW, 28),
                  app.restoreArmed ? std::wstring(L"确认恢复安装前文件") : std::wstring(L"恢复安装前文件"),
                  app.restoreArmed ? ButtonKind::Danger : ButtonKind::Secondary, canAct && hasClient, kFontSmall);
        app.ui.Button("maint.tools.pipeline", c.R(16 + btnW + 8, c.y + 68, btnW, 28), L"切换 AIO 管线开关",
                  ButtonKind::Secondary, canAct && hasClient, kFontSmall);
        app.ui.Button("maint.tools.panel", c.R(16, c.y + 100, btnW, 28), L"启动游戏内面板", ButtonKind::Secondary,
                  canAct && hasClient, kFontSmall);
        app.ui.Button("maint.tools.watch", c.R(16 + btnW + 8, c.y + 100, btnW, 28), L"启动鼠标助手",
                  ButtonKind::Secondary, canAct && hasClient, kFontSmall);
        std::wstring statusText;
        if (app.toolRun.running) {
            statusText = L"正在后台执行工具操作，请稍候…";
        } else if (app.toolRun.finished && !app.toolRun.ok && !app.toolRun.error.empty()) {
            statusText = L"上次操作失败：" + app.toolRun.error;
        } else if (app.toolRun.finished && app.toolRun.ok) {
            statusText = L"上次操作已完成，结果见日志。";
        } else {
            statusText = L"诊断文件不包含运行时 DLL、账号、聊天、截图或原始日志。";
        }
        g.Text(c.R(16, c.y + 136, cw - 32, 20),
               g.Ellipsize(statusText, toolFmt, std::max(10.0f, c.S(cw - 32))), toolFmt, Color(kText3));
        c.Gap(toolH + 12);
    }

    {
        CardRect(c, c.R(0, c.y, cw, 120), kDanger);
        g.Text(c.R(16, c.y + 12, cw - 32, 24), L"⚠  移除组件",
               g.Format(FontUi(), kFontBody * u, 700, HAlign::Left, VAlign::Middle), Color(kWarnText));
        MutedText(c, c.R(16, c.y + 42, cw - 32, 40),
                  L"先逐项核对将要删除与还原的文件，再确认卸载。备份缺失时不会删除当前文件。");
        app.ui.Button("maint.uninstall", c.R(cw - 16 - 132, c.y + 80, 132, 30), L"查看卸载清单",
                      ButtonKind::Danger, installed, kFontSmall);
        c.Gap(120);
    }
    FinishScroll(app, c, 12.0f);
}

// ---------- 卸载页 ----------

void DrawUninstall(App& app) {
    const HeaderInfo header =
        DrawPageHeader(app, L"卸载组件", L"逐项核对将在客户端中处理的文件，确认后执行。", false);
    const float u = AppScale(app);
    const float bodyTop = AppBodyRect(app).top;
    const float contentTop = bodyTop + header.contentStart * u;
    ScopedClip contentClip(&app.gfx, ContentClipRect(app, contentTop));
    Ctx c = MakeCtx(app, contentTop);
    const float cw = c.Cw();
    Gfx& g = c.g;
    const bool installed = app.snapshot.recordState == control_center::CapturedRecordState::Present;

    if (app.uninstallRun.running || app.uninstallRun.finished) {
        DrawOperationProgress(c, app.uninstallRun, true, L"正在卸载…", L"卸载完成", L"卸载失败");
        if (app.uninstallRun.running) {
            MutedText(c, c.R(0, c.y, cw, 46),
                      L"卸载在后台线程逐项执行；请勿强制结束程序，中断后下次启动会自动恢复并继续收尾。");
            c.Gap(52);
        } else if (app.uninstallRun.ok) {
            MutedText(c, c.R(0, c.y, cw, 46),
                      L"安装记录、备份与残留项已按清单处理；客户端 WTF 与个人数据未改动。");
            c.Gap(52);
        }
        FinishScroll(app, c, 12.0f);
        return;
    }

    if (!installed) {
        CardRect(c, c.R(0, c.y, cw, 80));
        MutedText(c, c.R(16, c.y + 18, cw - 32, 24), L"当前客户端没有安装记录。");
        MutedText(c, c.R(16, c.y + 44, cw - 32, 24), L"先回到维护页确认客户端，再查看卸载清单。");
        c.Gap(80);
        FinishScroll(app, c, 12.0f);
        return;
    }
    if (app.uninstallLoading) {
        CardRect(c, c.R(0, c.y, cw, 96));
        MutedText(c, c.R(16, c.y + 18, cw - 32, 24), L"正在后台读取卸载清单并校验文件与备份…");
        MutedText(c, c.R(16, c.y + 46, cw - 32, 24), L"后台任务持有客户端锁，界面保持可操作。");
        c.Gap(96);
        FinishScroll(app, c, 12.0f);
        return;
    }
    if (!app.uninstall.ready || !app.uninstall.ok) {
        CardRect(c, c.R(0, c.y, cw, 96), kWarn);
        MutedText(c, c.R(16, c.y + 18, cw - 32, 24),
                  app.uninstall.ready ? L"读取卸载清单失败：" + app.uninstall.error : std::wstring(L"尚未读取卸载清单。"));
        c.ui.Button("home.refresh", c.R(16, c.y + 52, 132, 30), L"重新读取", ButtonKind::Secondary, true,
                    kFontSmall);
        c.Gap(96);
        FinishScroll(app, c, 12.0f);
        return;
    }

    const UninstallPreview& preview = app.uninstall.preview;
    const auto& items = preview.items;

    std::vector<const UninstallPreviewItem*> deletes;
    std::vector<const UninstallPreviewItem*> restores;
    std::vector<const UninstallPreviewItem*> keeps;
    std::vector<const UninstallPreviewItem*> problems;
    size_t modifiedCount = 0;
    for (const auto& item : items) {
        switch (item.state) {
            case UninstallItemState::DeleteInstalled: deletes.push_back(&item); break;
            case UninstallItemState::DeleteModified:
                deletes.push_back(&item);
                ++modifiedCount;
                break;
            case UninstallItemState::RestoreOriginal: restores.push_back(&item); break;
            case UninstallItemState::AlreadyAbsent:
            case UninstallItemState::AlreadyRestored: keeps.push_back(&item); break;
            default: problems.push_back(&item); break;
        }
    }

    if (modifiedCount > 0) {
        CardRect(c, c.R(0, c.y, cw, 58), kWarn);
        auto* note = g.Format(FontUi(), kFontSmall * u, 400, HAlign::Left, VAlign::Top, true);
        std::wstring names;
        size_t shown = 0;
        for (const auto* item : deletes) {
            if (item->state != UninstallItemState::DeleteModified) continue;
            if (!names.empty()) names += L"，";
            names += item->entry.target;
            ++shown;
            if (shown >= 2) break;
        }
        g.Text(c.R(16, c.y + 9, cw - 32, 44),
               Fmt(L"有 %zu 个修改文件已被改动，还原会覆盖当前内容：%s", modifiedCount, names.c_str()), note,
               Color(kWarnText));
        c.Gap(58);
        c.Gap(10);
    }

    struct Group {
        const wchar_t* title;
        const wchar_t* tag;
        unsigned color;
        std::wstring note;
        std::vector<const UninstallPreviewItem*> rows;
    };
    std::vector<Group> groups;
    groups.push_back({L"将删除", L"删除", kDanger, Fmt(L"%zu 个文件", deletes.size()), deletes});
    groups.push_back({L"将还原", L"还原", kAccent, Fmt(L"%zu 个文件", restores.size()), restores});
    groups.push_back({L"将保留", L"保留", kText3,
                      Fmt(L"%zu 项，含用户数据", keeps.size() + preview.residue.size() + 2), keeps});
    if (!problems.empty()) {
        groups.push_back({L"无法处理", L"无法卸载", kWarn, Fmt(L"%zu 项，需人工处理", problems.size()),
                          problems});
    }

    for (const auto& group : groups) {
        Section(c, group.title, group.note);
        c.Gap(22);
        size_t rows = group.rows.size();
        if (group.title == std::wstring(L"将保留")) rows += preview.residue.size() + 2;
        if (rows == 0) {
            CardRect(c, c.R(0, c.y, cw, 40));
            EmptyRow(c, c.R(0, c.y, cw, 40), L"没有需要处理的文件。");
            c.Gap(50);
            continue;
        }
        CardRect(c, c.R(0, c.y, cw, static_cast<float>(rows) * 40.0f + 8.0f));
        float rowY = c.y + 4;
        for (const auto* item : group.rows) {
            std::wstring note;
            if (item->state == UninstallItemState::DeleteModified) {
                note = L"已改动";
            } else if (item->state == UninstallItemState::AlreadyAbsent) {
                note = L"已不存在";
            } else if (item->state == UninstallItemState::AlreadyRestored) {
                note = L"已还原";
            } else {
                note = UninstallIssueText(*item);
            }
            FileRow(c, c.R(0, rowY, cw, 40), item->entry.target, group.tag, group.color, note);
            rowY += 40;
        }
        if (group.title == std::wstring(L"将保留")) {
            for (const auto& residue : preview.residue) {
                FileRow(c, c.R(0, rowY, cw, 40), residue.entry.path, L"保留", kText3, L"残留项");
                rowY += 40;
            }
            FileRow(c, c.R(0, rowY, cw, 40), L"用户配置（WTF）", L"保留", kText3, L"用户数据");
            rowY += 40;
            FileRow(c, c.R(0, rowY, cw, 40), L"日志与存档", L"保留", kText3, L"用户数据");
            rowY += 40;
        }
        c.Gap(static_cast<float>(rows) * 40.0f + 8.0f);
        c.Gap(10);
    }

    c.Gap(2);
    MutedText(c, c.R(0, c.y, cw, 46),
              L"卸载成功后同时清理安装记录与 host64\\dlss5-backup 备份目录；失败不掩盖，备份缺失时不删当前文件。");
    c.Gap(50);
    MutedText(c, c.R(0, c.y, cw, 34),
              L"确认后会按清单逐项还原或删除，并清理安装记录与备份；执行前会再次校验文件身份与备份哈希。");
    c.Gap(38);
    FinishScroll(app, c, 12.0f);
}

// ---------- 日志查看器（悬浮面板 / 载体日志尾部） ----------

void DrawLogViewer(App& app) {
    if (!app.logViewer.visible) return;
    Gfx& g = app.gfx;
    Ui& ui = app.ui;
    const float u = AppScale(app);
    const Rect body = AppBodyRect(app);
    const Rect card(body.left + 10.0f * u, body.top + 10.0f * u, body.right - 10.0f * u, body.bottom - 10.0f * u);
    // 实底卡片：遮住下方页面，避免日志与页面文字重叠。
    g.FillRound(card, 10.0f * u, Color(kSurface2));
    g.StrokeRound(card, 10.0f * u, Color(kLine), 1.0f);
    // 页面控件先于本层登记命中区域；这里丢掉被卡片盖住的部分，
    // 否则点击日志区会误触底下看不见的页面按钮。
    ui.RemoveHitsInside(card);
    auto* titleFmt = g.Format(FontUi(), kFontBody * u, 700, HAlign::Left, VAlign::Middle);
    g.Text(Rect(card.left + 16.0f * u, card.top + 12.0f * u, card.right - 104.0f * u, card.top + 40.0f * u),
           app.logViewer.title.empty() ? std::wstring(L"日志") : app.logViewer.title, titleFmt, Color(kText));
    ui.Button("logview.close",
              Rect(card.right - 16.0f * u - 72.0f * u, card.top + 12.0f * u, card.right - 16.0f * u,
                   card.top + 40.0f * u),
              L"关闭", ButtonKind::Secondary, true, kFontSmall);
    const Rect list(card.left + 16.0f * u, card.top + 48.0f * u, card.right - 16.0f * u, card.bottom - 16.0f * u);
    g.FillRound(list, 6.0f * u, Color(kSurface));
    ScopedClip clip(&g, list);
    auto* mono = g.Format(FontMono(), 11.0f * u, 400, HAlign::Left, VAlign::Top);
    const float lineH = 15.0f * u;
    float y = list.top + 8.0f * u;
    if (app.logViewer.lines.empty()) {
        g.Text(Rect(list.left + 8.0f * u, y, list.right - 8.0f * u, y + lineH), L"日志为空。", mono, Color(kText3));
        return;
    }
    const float width = (std::max)(10.0f, list.Width() - 16.0f * u);
    for (const std::wstring& line : app.logViewer.lines) {
        if (y + lineH > list.bottom) break;
        g.Text(Rect(list.left + 8.0f * u, y, list.right - 8.0f * u, y + lineH), g.Ellipsize(line, mono, width), mono,
               Color(kText2));
        y += lineH;
    }
}

}  // namespace cc
