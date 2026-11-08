"""Conservative per-process/swapchain analysis for isolated PresentMon CSV captures."""

import argparse
import csv
import json
import math
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

SCRATCH = (Path(__file__).resolve().parents[3] / "_scratch" / "dlss5-aio").resolve(strict=True)


def number(value, *, positive=False):
    if value is None or str(value).strip().lower() in ("", "na", "n/a", "nan"):
        return None
    try:
        result = float(value)
    except ValueError:
        return None
    if not math.isfinite(result) or (positive and result <= 0):
        return None
    return result


def first_number(row, names, *, positive=False):
    for name in names:
        result = number(row.get(name), positive=positive)
        if result is not None:
            return result
    return None


def percentile(values, fraction):
    data = sorted(values)
    position = (len(data) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(data) - 1)
    return data[lower] + (data[upper] - data[lower]) * (position - lower)


def summarize(headers, rows, expected_seconds=60.0, minimum_coverage=0.9):
    if (not math.isfinite(expected_seconds) or expected_seconds <= 0 or
        not math.isfinite(minimum_coverage) or not 0 < minimum_coverage <= 1):
        raise ValueError("invalid expected duration or coverage fraction")
    required = {"ProcessID", "SwapChainAddress"}
    if not required.issubset(headers) or not ({"MsBetweenPresents", "FrameTime"} & set(headers)):
        raise ValueError("CSV lacks process/swapchain identity or present interval")
    groups = defaultdict(list)
    for row in rows:
        try:
            pid = int(str(row.get("ProcessID", "")).strip())
        except ValueError as exc:
            raise ValueError("invalid ProcessID") from exc
        address = str(row.get("SwapChainAddress", "")).strip().lower()
        if pid <= 0 or not address:
            raise ValueError("missing process/swapchain identity")
        groups[(pid, address)].append({
            "time_ms": first_number(row, ("CPUStartQPCTimeInMs", "TimeInMs", "CPUStartTimeInMs")),
            "present_ms": first_number(row, ("MsBetweenPresents", "FrameTime"), positive=True),
            "display_ms": first_number(row, ("MsBetweenDisplayChange", "DisplayedTime"), positive=True),
        })
    if not groups:
        raise ValueError("CSV contains no frame rows")
    reports = []
    for (pid, address), samples in sorted(groups.items()):
        # The first row may span time before capture began. Do not include it.
        data = samples[1:]
        presents = [x["present_ms"] for x in data if x["present_ms"] is not None]
        displayed = [x["display_ms"] for x in data if x["display_ms"] is not None]
        timestamps = [x["time_ms"] for x in samples if x["time_ms"] is not None]
        timestamps_complete = len(timestamps) == len(samples)
        monotonic = all(a <= b for a, b in zip(timestamps, timestamps[1:]))
        span = (timestamps[-1] - timestamps[0]) / 1000.0 if len(timestamps) >= 2 and monotonic else None
        eligible = (len(presents) >= 100 and timestamps_complete and span is not None and
                    span >= expected_seconds * minimum_coverage)
        p99 = percentile(presents, 0.99) if len(presents) >= 100 else None
        p999 = percentile(presents, 0.999) if len(presents) >= 1000 else None
        reports.append({
            "process_id": pid, "swapchain": address, "rows": len(samples),
            "first_row_discarded": True, "valid_present_intervals": len(presents),
            "valid_display_intervals": len(displayed), "capture_span_seconds": span,
            "timestamps_complete": timestamps_complete, "timestamps_monotonic": monotonic,
            "meets_capture_length_and_count": eligible,
            "mean_present_fps_estimate": 1000.0 / (sum(presents) / len(presents)) if len(presents) >= 2 else None,
            "mean_display_fps_estimate": 1000.0 / (sum(displayed) / len(displayed)) if len(displayed) >= 2 else None,
            "present_p99_ms": p99, "one_percent_low_fps_p99_reciprocal": 1000.0 / p99 if p99 else None,
            "present_p999_ms": p999, "point_one_percent_low_fps_p999_reciprocal": 1000.0 / p999 if p999 else None,
        })
    return {"generated_utc": datetime.now(timezone.utc).isoformat(),
            "expected_seconds": expected_seconds, "minimum_coverage_fraction": minimum_coverage,
            "method": "Group by process and swapchain; discard first row; FPS=1000/mean interval; "
                      "1% low=1000/interpolated P99 interval (>=100 intervals); "
                      "0.1% low=1000/interpolated P99.9 interval (>=1000 intervals). "
                      "Missing displayed timing remains null. Coverage is a data sufficiency check, "
                      "not a performance threshold. Role requires independent correlation.",
            "groups": reports}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--expected-seconds", type=float, default=60.0)
    parser.add_argument("--minimum-coverage", type=float, default=0.9)
    args = parser.parse_args()
    source, target = args.csv.resolve(strict=True), args.output.resolve()
    if not source.is_relative_to(SCRATCH) or not target.is_relative_to(SCRATCH) or source == target:
        parser.error("input and output must be distinct files in the DLSS scratch workspace")
    with source.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        result = summarize(reader.fieldnames or [], reader, args.expected_seconds, args.minimum_coverage)
    with target.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    print(f"wrote {len(result['groups'])} separate process/swapchain groups: {target}")


if __name__ == "__main__":
    main()
