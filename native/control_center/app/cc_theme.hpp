// WOW-335-NR 原生控制中心：视觉主题常量。
// 颜色与字号沿用旧 WinForms 界面（Dlss5.ControlCenter.ps1 的 kBg..kDanger 与
// New-UiFont），布局尺寸按 96dpi 逻辑像素（DIP）书写，由 Direct2D 按窗口 DPI 缩放。
#pragma once

#include <d2d1.h>

namespace cc {
namespace theme {

// 颜色（0xRRGGBB）。命名与原界面 $script:k* 对应关系见注释。
constexpr unsigned kBg = 0x0F0C08;          // $kBg：深层背景（分隔/内嵌区域）
constexpr unsigned kChrome = 0x14100A;      // $kChrome：自定义标题栏
constexpr unsigned kWindow = 0x1B1610;      // $kSurface：窗口主体背景
constexpr unsigned kSurface = 0x231C14;     // $kSurface2：卡片填充
constexpr unsigned kSurface2 = kSurface;   // 旧界面命名别名（卡片填充）
constexpr unsigned kSurface3 = 0x2C2318;    // $kSurface3：输入框/次级填充
constexpr unsigned kLine = 0x3A2E1F;        // $kLine：强调边框
constexpr unsigned kLineSoft = 0x2A2117;    // $kLineSoft：卡片细边框
constexpr unsigned kText = 0xEADFC6;        // $kText：主文字
constexpr unsigned kText2 = 0xC2B295;       // $kText2：次级文字
constexpr unsigned kText3 = 0x9A8A6D;       // $kText3：弱化文字
constexpr unsigned kAccent = 0xC8A45C;      // $kAccent：金色强调
constexpr unsigned kAccentBright = 0xE3C884; // $kAccentBright：强调悬停
constexpr unsigned kJade = 0x6DAB86;        // $kJade：正常/运行
constexpr unsigned kJadeText = 0xC9E7D6;    // $kJadeText：正常文字
constexpr unsigned kWarn = 0xD98A4A;        // $kWarn：警告/待机
constexpr unsigned kDanger = 0xBF5F52;      // $kDanger：错误/危险
constexpr unsigned kDangerBright = 0xD07163; // 危险按钮悬停
constexpr unsigned kInk = 0x1A1206;         // $kInk：金色按钮上的深色文字
constexpr unsigned kMuted = 0x6A5B44;       // 弱化点/未确认状态
constexpr unsigned kLineJade = 0x3C5A46;     // 完成状态圆点描边/步骤连线
constexpr unsigned kJadeDeep = 0x1D2A22;     // 完成状态圆点填充
constexpr unsigned kAccentDeep = 0x33290F;   // 当前步骤圆点填充
constexpr unsigned kAccentLine = 0x5A4A2E;   // 金色系描边/选中卡片边框
constexpr unsigned kAccentRow = 0x28200F;    // 选中行填充
constexpr unsigned kArrow = 0x5A4A34;        // 管线箭头（未点亮）
constexpr unsigned kDangerCard = 0x1E1512;   // 危险操作卡填充
constexpr unsigned kWarnCard = 0x231811;     // 警告卡填充
constexpr unsigned kWarnText = 0xE8B4A8;     // 警告卡标题文字

inline D2D1_COLOR_F Color(unsigned rgb, float alpha = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFFu) / 255.0f, ((rgb >> 8) & 0xFFu) / 255.0f,
                        (rgb & 0xFFu) / 255.0f, alpha);
}
inline D2D1_COLOR_F Mix(unsigned rgb, float alpha) { return Color(rgb, alpha); }

// 布局（96dpi 逻辑像素）。
constexpr float kWindowWidth = 600.0f;
constexpr float kWindowHeight = 900.0f;
constexpr float kMinWindowWidth = 600.0f;
constexpr float kMinWindowHeight = 560.0f;
constexpr float kTitleBarHeight = 32.0f;
constexpr float kPagePaddingX = 16.0f;
constexpr float kPagePaddingTop = 14.0f;
constexpr float kCardRadius = 9.0f;
// 旧界面的按钮与输入框都是直角矩形，这里保持 0 以便 Ui::Button 复用同一路径。
constexpr float kFieldRadius = 0.0f;
constexpr float kGap = 10.0f;
constexpr float kFooterHeight = 52.0f;
constexpr float kContentWidth = 568.0f;
constexpr float kWindowButtonWidth = 30.0f;
constexpr float kWindowButtonHeight = 24.0f;

// 字号（逻辑像素，与旧界面 New-UiFont 的像素值一致）。
constexpr float kFontBody = 12.0f;
constexpr float kFontSmall = 11.0f;
constexpr float kFontTitle = 18.0f;
constexpr float kFontHero = 20.0f;
constexpr float kFontGauge = 40.0f;
constexpr float kFontStat = 15.0f;
constexpr float kFontBrand = 15.0f;

// 字体家族。
inline const wchar_t* FontUi() { return L"Microsoft YaHei UI"; }
inline const wchar_t* FontDisplay() { return L"Constantia"; }
inline const wchar_t* FontMono() { return L"Consolas"; }

}  // namespace theme
}  // namespace cc
