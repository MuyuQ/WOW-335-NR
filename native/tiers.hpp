// WOW-335-NR 出厂档位与参数刻度——唯一事实源。
// zh: console.cpp 与 panel.cpp 共用本表；PowerShell 侧 schemes/*.json 与
// Dlss5.Core.psm1 的校验边界、templates/host64-ReShade.ini 的出厂值都必须与这里一致。
// 20261109 收敛：此前控制台（0–2 刻度、值 0.4/0.7/1.2）、面板（0–1 刻度、
// 值 0.20/0.35/0.60）与 schemes/*.json（第三套数值）同名档位互相矛盾。
// 刻度遵循载体 033 引擎契约：intensity/structure 0–1（1.0=满），tone 0–2（1.0=中性），
// skin -1~1（0=中性）。档位=强度阶梯（社区参考 Igor's Lab：强度 20–35%、结构 10–25%
// 起步），三档间距刻意拉开。
#pragma once

struct NrTier {
    const char *name;
    bool nr;
    int model;
    float intensity, tone, structure, skin;
};

static const NrTier kNrTiers[] = {
    { "原画", false, 1, 1.0f,  1.0f, 1.0f,  -1.0f },
    { "轻量", true,  1, 0.20f, 1.0f, 0.10f, -1.0f },
    { "均衡", true,  1, 0.35f, 1.0f, 0.25f, -1.0f },
    { "浓郁", true,  1, 0.60f, 1.3f,  0.50f, -1.0f },
};
static const int kNrTierCount = static_cast<int>(sizeof(kNrTiers) / sizeof(kNrTiers[0]));

// 参数刻度边界（载体 033 schema；upstream 0–2 菜单范围偏宽，按引擎真实范围收紧）
static constexpr float kNrIntensityMin = 0.0f, kNrIntensityMax = 1.0f;
static constexpr float kNrToneMin = 0.0f,       kNrToneMax = 2.0f;
static constexpr float kNrStructureMin = 0.0f,  kNrStructureMax = 1.0f;
static constexpr float kNrSkinMin = -1.0f,      kNrSkinMax = 1.0f;
