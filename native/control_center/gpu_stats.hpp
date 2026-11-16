// WOW-335-NR 原生控制中心：GPU 统计（显存占用与 3D 引擎负载）。
//
// 旧实现通过 CIM（Win32_PerfFormattedData_GPUPerformanceCounters_*）每 5 秒查询
// 一次；这里改用 PDH 读取同一批性能计数器，语义保持一致：
//   - 显存 = GPU Adapter Memory 各实例 Dedicated Usage 之和（字节）；
//   - 负载 = GPU Engine 中 *engtype_3D* 实例 Utilization Percentage 之和；
//   - 失败返回 valid=false，UI 显示 “—”，不把 0 当成有效值。
//
// 查询代价较高，因此内部带 5 秒缓存：后台线程可以频繁调用，只有缓存过期才真正
// 打开查询。任何失败同样写入缓存时间，避免 5 秒内反复重试。
//
// 为保持生产编译测试只链接 bcrypt.lib，pdh 函数从 pdh.dll 动态解析，不依赖 pdh.lib。
#pragma once

#include <windows.h>

#include <mutex>
#include <vector>

#include <pdh.h>
#include <pdhmsg.h>

namespace control_center {

struct GpuStats {
    bool valid = false;
    // 分项用负值表示“未取到”：显存读到了但负载失败（或反之）时，UI 必须显示 “—”，
    // 不能把默认 0 当成真实读数显示成 “0.0 GB”/“0.0%”。
    double vramGb = -1.0;      // 专用显存占用（GiB，1024^3）
    double loadPercent = -1.0; // 3D 引擎占用百分比之和
};

namespace gpu_detail {

constexpr ULONGLONG kGpuStatsCacheMs = 5000;
constexpr DWORD kGpuStatsSettleMs = 120; // GPU 计数器需要两次采样才有速率值

using PdhOpenQueryWFn = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
using PdhAddEnglishCounterWFn = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
using PdhCollectQueryDataFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
using PdhCloseQueryFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
using PdhGetFormattedCounterArrayWFn =
    PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);

struct PdhApi {
    HMODULE module = nullptr;
    PdhOpenQueryWFn openQuery = nullptr;
    PdhAddEnglishCounterWFn addEnglishCounter = nullptr;
    PdhCollectQueryDataFn collectQueryData = nullptr;
    PdhCloseQueryFn closeQuery = nullptr;
    PdhGetFormattedCounterArrayWFn formattedCounterArray = nullptr;
    bool ok = false;
};

inline void ResolvePdhApi(PdhApi& api) {
    api.module = LoadLibraryW(L"pdh.dll");
    if (api.module == nullptr) return;
    api.openQuery = reinterpret_cast<PdhOpenQueryWFn>(GetProcAddress(api.module, "PdhOpenQueryW"));
    api.addEnglishCounter =
        reinterpret_cast<PdhAddEnglishCounterWFn>(GetProcAddress(api.module, "PdhAddEnglishCounterW"));
    api.collectQueryData =
        reinterpret_cast<PdhCollectQueryDataFn>(GetProcAddress(api.module, "PdhCollectQueryData"));
    api.closeQuery = reinterpret_cast<PdhCloseQueryFn>(GetProcAddress(api.module, "PdhCloseQuery"));
    api.formattedCounterArray =
        reinterpret_cast<PdhGetFormattedCounterArrayWFn>(GetProcAddress(api.module, "PdhGetFormattedCounterArrayW"));
    api.ok = api.openQuery != nullptr && api.addEnglishCounter != nullptr && api.collectQueryData != nullptr &&
             api.closeQuery != nullptr && api.formattedCounterArray != nullptr;
}

inline const PdhApi& LoadPdh() {
    static PdhApi api = [] {
        PdhApi resolved;
        ResolvePdhApi(resolved);
        return resolved;
    }();
    return api;
}

inline bool FormatArrayTotal(const PdhApi& api, PDH_HCOUNTER counter, double& total, bool skipBogus) {
    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = api.formattedCounterArray(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
    if (status != PDH_MORE_DATA || bufferSize == 0) return false;
    std::vector<unsigned char> buffer(bufferSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = api.formattedCounterArray(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items);
    if (status != ERROR_SUCCESS) return false;
    double sum = 0.0;
    size_t used = 0;
    for (DWORD i = 0; i < itemCount; ++i) {
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        const double value = items[i].FmtValue.doubleValue;
        if (skipBogus && (value <= 0.0 || value >= 100.0)) continue;
        if (value < 0.0) continue;
        sum += value;
        ++used;
    }
    if (used == 0) return false;
    total = sum;
    return true;
}

inline GpuStats QueryGpuStats() {
    GpuStats result;
    const PdhApi& api = LoadPdh();
    if (!api.ok) return result;
    PDH_HQUERY query = nullptr;
    if (api.openQuery(nullptr, 0, &query) != ERROR_SUCCESS || query == nullptr) return result;

    PDH_HCOUNTER usage = nullptr;
    PDH_HCOUNTER memory = nullptr;
    const bool usageAdded =
        api.addEnglishCounter(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &usage) ==
        ERROR_SUCCESS;
    const bool memoryAdded =
        api.addEnglishCounter(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &memory) == ERROR_SUCCESS;
    if (!usageAdded && !memoryAdded) {
        api.closeQuery(query);
        return result;
    }
    if (api.collectQueryData(query) != ERROR_SUCCESS) {
        api.closeQuery(query);
        return result;
    }
    ::Sleep(kGpuStatsSettleMs);
    if (api.collectQueryData(query) != ERROR_SUCCESS) {
        api.closeQuery(query);
        return result;
    }
    double vram = 0.0;
    double load = 0.0;
    const bool vramOk = memoryAdded && FormatArrayTotal(api, memory, vram, false);
    const bool loadOk = usageAdded && FormatArrayTotal(api, usage, load, true);
    api.closeQuery(query);
    if (!vramOk && !loadOk) return result;
    result.valid = true;
    if (vramOk && vram > 0.0) result.vramGb = vram / (1024.0 * 1024.0 * 1024.0);
    if (loadOk) result.loadPercent = load;
    return result;}

}  // namespace gpu_detail

// 带 5 秒缓存；可在任意后台线程调用（内部串行化）。
inline GpuStats ReadGpuStats() {
    using namespace gpu_detail;
    static std::mutex guard;
    static ULONGLONG lastQuery = 0;
    static GpuStats cached;
    std::lock_guard<std::mutex> lock(guard);
    const ULONGLONG now = GetTickCount64();
    if (lastQuery != 0 && now - lastQuery < kGpuStatsCacheMs) return cached;
    cached = QueryGpuStats();
    lastQuery = now;
    return cached;
}

}  // namespace control_center
