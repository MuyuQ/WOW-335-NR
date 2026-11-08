"""Correlate host PresentMon rows with independently sampled AIO telemetry."""

from bisect import bisect_right
from collections import Counter, defaultdict
import math


def address(value):
    if not value:
        return None
    try:
        result = int(str(value), 16)
    except ValueError:
        return None
    return hex(result) if result else None


def desired_state(sample, nr_enabled=True):
    return (sample.get("addon_enabled") is True and sample.get("nr_enabled") is nr_enabled and
            sample.get("neural_pipeline_ready") is True and sample.get("processed_output_visible") is True and
            sample.get("frame_generation_enabled") is False and sample.get("proxy_hidden") is False and
            sample.get("proxy_failed") is False and sample.get("reshade_overlay_open") is False)


def correlate(samples, rows, host_pid, maximum_age_ms=2000, minimum_match_fraction=0.9, nr_enabled=True):
    if host_pid <= 0 or maximum_age_ms <= 0 or not 0 < minimum_match_fraction <= 1:
        raise ValueError("invalid correlation parameters")
    if not samples:
        raise ValueError("no telemetry samples")
    times = []
    for item in samples:
        timestamp = item.get("qpc_ms")
        if (item.get("process_id") != host_pid or not isinstance(timestamp, (int, float)) or
                not math.isfinite(timestamp) or timestamp <= 0):
            raise ValueError("invalid telemetry process or QPC")
        if times and timestamp <= times[-1]:
            raise ValueError("telemetry QPC must be strictly increasing")
        times.append(timestamp)
    groups = defaultdict(Counter)
    for row in rows:
        try:
            pid = int(str(row.get("ProcessID", "")))
            timestamp = float(row.get("CPUStartQPCTimeInMs", ""))
        except (TypeError, ValueError) as exc:
            raise ValueError("invalid PresentMon PID or QPC") from exc
        chain = address(row.get("SwapChainAddress"))
        if pid != host_pid or not math.isfinite(timestamp) or timestamp <= 0 or not chain:
            raise ValueError("PresentMon row has wrong PID, QPC, or swapchain")
        counter = groups[chain]
        counter["rows"] += 1
        index = bisect_right(times, timestamp) - 1
        if index < 0 or timestamp - times[index] > maximum_age_ms:
            counter["no_fresh_telemetry"] += 1
            continue
        sample = samples[index]
        counter["fresh_telemetry"] += 1
        if desired_state(sample, nr_enabled=nr_enabled):
            counter["desired_nr_state"] += 1
        else:
            counter["other_or_incomplete_state"] += 1
        primary, proxy = address(sample.get("primary_swapchain")), address(sample.get("proxy_swapchain"))
        if primary == proxy and primary == chain:
            counter["ambiguous"] += 1
        elif chain == primary:
            counter["primary"] += 1
        elif chain == proxy:
            counter["proxy"] += 1
        else:
            counter["address_unmatched"] += 1
    if not groups:
        raise ValueError("no PresentMon rows")
    result = []
    for chain, count in sorted(groups.items()):
        total = count["rows"]
        role = None
        for candidate, opposite in (("primary", "proxy"), ("proxy", "primary")):
            if (count[candidate] / total >= minimum_match_fraction and count[opposite] == 0 and
                    count["ambiguous"] == 0):
                role = candidate
        result.append({"swapchain": chain, "rows": total, "role_hint": role,
                       "fresh_telemetry_rows": count["fresh_telemetry"],
                       "unknown_or_stale_rows": count["no_fresh_telemetry"] + count["address_unmatched"],
                       "ambiguous_rows": count["ambiguous"],
                       "desired_nr_state_rows": count["desired_nr_state"],
                       "other_or_incomplete_state_rows": count["other_or_incomplete_state"]})
    proxy = [item["swapchain"] for item in result if item["role_hint"] == "proxy"]
    primary = [item["swapchain"] for item in result if item["role_hint"] == "primary"]
    return {"host_pid": host_pid, "telemetry_samples": len(samples), "maximum_age_ms": maximum_age_ms,
            "minimum_match_fraction": minimum_match_fraction, "expected_nr_enabled": nr_enabled,
            "primary_candidate": primary[0] if len(primary) == 1 else None,
            "proxy_candidate": proxy[0] if len(proxy) == 1 else None,
            "limits": ["Exact pointer matching is a candidate correlation, not proof of on-screen visibility.",
                       "Samples are applied forward only until maximum_age_ms; stale or mismatched rows remain unknown.",
                       "Confirm NR state and sampling coverage independently before comparing performance."],
            "groups": result}
