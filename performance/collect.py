"""Collect bounded game and AIO timings without starting the game or tracking input."""

import argparse
import configparser
from contextlib import nullcontext
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import threading
import time
from uuid import uuid4
import ctypes as ct
from ctypes import wintypes as wt

from analyze_presentmon import summarize
from correlate import correlate, desired_state
from telemetry_reader import open_host
from resource_probe import gpu_devices, memory, x86_address_space

PROJECT = Path(__file__).resolve().parents[3]
MODULE = Path(__file__).resolve().parents[1]
SCRATCH = (PROJECT / "_scratch" / "dlss5-aio").resolve(strict=True)
# 组件身份唯一事实源是仓库根的 payload-lock.json（20261109 去重：此前此处与
# payload-lock.json、window_watch.cpp 三处硬编码同一批哈希，改锁必漏）。
# PresentMon 尚无锁条目，保持本地常量。
PM_SHA = "b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af"


def _lock_identities():
    lock = json.loads((MODULE / "payload-lock.json").read_text(encoding="utf-8"))
    members = {item["target"]: item["sha256"] for item in lock["members"]}
    return {
        "GAME_SHA": lock["game_sha256"],
        "DXVK_SHA": lock["original_d3d9_sha256"],
        "RESHADE_SHA": members["d3d9.dll"],
        "HOST_SHA": members["host64/AIO DLSS5 32-bit Wrapper.exe"],
    }


_lock_ids = _lock_identities()
GAME_SHA = _lock_ids["GAME_SHA"]
DXVK_SHA = _lock_ids["DXVK_SHA"]
RESHADE_SHA = _lock_ids["RESHADE_SHA"]
HOST_SHA = _lock_ids["HOST_SHA"]
PROCESS_QUERY = r"""$ErrorActionPreference='Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$rows = @(Get-CimInstance Win32_Process -Filter "Name='wow.exe' OR Name='AIO DLSS5 32-bit Wrapper.exe'" |
    Select-Object Name,ProcessId,ExecutablePath)
ConvertTo-Json -InputObject $rows -Compress
"""
user32 = ct.WinDLL("user32", use_last_error=True)
user32.GetForegroundWindow.argtypes = []
user32.GetForegroundWindow.restype = wt.HWND
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ct.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype = wt.DWORD


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def checked(path, root, *, exists):
    path = Path(os.path.abspath(path))
    if not path.is_relative_to(root) or path == root:
        raise ValueError("path is outside the isolated DLSS workspace")
    for item in (path, *path.parents):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if item.is_symlink() or (getattr(info, "st_file_attributes", 0) & 0x400):
            raise ValueError("path contains a link or reparse point")
    if exists and not path.exists():
        raise ValueError("required path is missing")
    return path


def processes(game_path, host_path, *, require_host):
    result = subprocess.run(["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", PROCESS_QUERY],
                            capture_output=True, encoding="utf-8", errors="strict", timeout=15,
                            creationflags=subprocess.CREATE_NO_WINDOW, check=True)
    rows = json.loads(result.stdout)
    matches = {"game": [], "host": []}
    for row in rows:
        actual = row.get("ExecutablePath")
        if not actual:
            raise RuntimeError("a matching process name has an unreadable path")
        actual = os.path.normcase(os.path.abspath(actual))
        if actual == os.path.normcase(str(game_path)):
            matches["game"].append(int(row["ProcessId"]))
        elif actual == os.path.normcase(str(host_path)):
            matches["host"].append(int(row["ProcessId"]))
    expected_hosts = 1 if require_host else 0
    if len(matches["game"]) != 1 or len(matches["host"]) != expected_hosts:
        raise RuntimeError("selected game or host process count does not match the selected mode")
    return matches["game"][0], matches["host"][0] if require_host else None


def foreground_is(pid):
    owner = wt.DWORD()
    hwnd = user32.GetForegroundWindow()
    if not hwnd or not user32.GetWindowThreadProcessId(hwnd, ct.byref(owner)):
        return False
    return owner.value == pid


def stop_session(pm, name):
    try:
        subprocess.run([str(pm), "--session_name", name, "--terminate_existing_session"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10,
                       creationflags=subprocess.CREATE_NO_WINDOW, check=False)
    except (OSError, subprocess.TimeoutExpired):
        pass


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def warmup(mapping, game_pid, seconds, deadline, *, nr_enabled):
    since = None
    while time.monotonic() < deadline:
        current = time.monotonic()
        try:
            valid = foreground_is(game_pid)
            if mapping is not None:
                snapshot = mapping.read()
                age = time.perf_counter() - snapshot["qpc"] / snapshot["qpc_frequency"]
                valid = valid and -0.1 <= age <= 3.0 and desired_state(snapshot, nr_enabled=nr_enabled)
        except (OSError, RuntimeError, ValueError):
            valid = False
        if not valid:
            since = None
        elif since is None:
            since = current
        elif current - since >= seconds:
            return
        time.sleep(0.25)
    raise RuntimeError("foreground and expected NR state did not remain stable before the deadline")


def record_one(out, run, pm, game_pid, host_pid, mapping, seconds, *, nr_enabled):
    token = uuid4().hex[:12]
    names = {"game": "WOW335Game" + token, "host": "WOW335Host" + token}
    children, streams, telemetry = {}, [], []
    focus_samples = lost_samples = telemetry_errors = 0
    resource_stop = threading.Event()
    resource_state = {"samples": 0, "error": None}
    resource_path = out / f"run-{run}-resources.jsonl"
    gpu_query = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "nvidia-smi.exe"

    def resource_worker():
        try:
            with resource_path.open("x", encoding="utf-8", newline="\n") as resource_log:
                while not resource_stop.is_set():
                    entry = {"qpc_ms": time.perf_counter() * 1000, "errors": []}
                    for role, pid in (("game", game_pid), ("host", host_pid)):
                        if pid is None:
                            continue
                        try:
                            entry[role] = memory(pid)
                        except OSError as exc:
                            entry[role] = None
                            entry["errors"].append(f"{role} memory: {exc}")
                    try:
                        entry["gpu_devices"] = gpu_devices(gpu_query)
                    except (OSError, ValueError, subprocess.SubprocessError) as exc:
                        entry["gpu_devices"] = None
                        entry["errors"].append(f"device-wide GPU query: {exc}")
                    resource_log.write(json.dumps(entry, ensure_ascii=False) + "\n")
                    resource_log.flush()
                    resource_state["samples"] += 1
                    resource_stop.wait(2.0)
        except Exception as exc:
            resource_state["error"] = f"{type(exc).__name__}: {exc}"

    def address_snapshot(label):
        try:
            data = x86_address_space(game_pid)
        except OSError as exc:
            data = {"complete": False, "error": str(exc)}
        write_json(out / f"run-{run}-address-{label}.json", data)
        return bool(data.get("complete"))

    address_before = address_snapshot("before")
    resource_thread = threading.Thread(target=resource_worker, name="resource-sampler", daemon=True)
    telemetry_file = ((out / f"run-{run}-telemetry.jsonl").open("x", encoding="utf-8", newline="\n")
                      if mapping is not None else nullcontext(None))
    with telemetry_file as log:
        resource_thread.start()
        def sample():
            nonlocal telemetry_errors
            try:
                if mapping is None:
                    return
                value = mapping.read()
                if telemetry and value["qpc_frequency"] != telemetry[-1]["qpc_frequency"]:
                    raise ValueError("telemetry clock frequency changed")
                if not telemetry or value["qpc"] > telemetry[-1]["qpc"]:
                    telemetry.append(value)
                    log.write(json.dumps(value, ensure_ascii=False) + "\n")
                    log.flush()
                elif value["qpc"] < telemetry[-1]["qpc"]:
                    raise ValueError("telemetry clock moved backwards")
            except (OSError, RuntimeError, ValueError):
                telemetry_errors += 1
        sample()
        try:
            roles = [("game", game_pid)] + ([("host", host_pid)] if host_pid is not None else [])
            for role, pid in roles:
                stdout = (out / f"run-{run}-{role}.stdout").open("xb")
                stderr = (out / f"run-{run}-{role}.stderr").open("xb")
                streams.extend((stdout, stderr))
                command = [str(pm), "--process_id", str(pid), "--timed", str(seconds),
                           "--terminate_after_timed", "--terminate_on_proc_exit", "--no_track_input",
                           "--qpc_time_ms", "--output_file", str(out / f"run-{run}-{role}.csv"),
                           "--session_name", names[role]]
                children[role] = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                                  creationflags=subprocess.CREATE_NO_WINDOW)
            end = time.monotonic() + seconds + 20
            while time.monotonic() < end and any(child.poll() is None for child in children.values()):
                if foreground_is(game_pid):
                    focus_samples += 1
                else:
                    lost_samples += 1
                    break
                sample()
                time.sleep(0.25)
            interrupted = any(child.poll() is None for child in children.values())
            if interrupted:
                for role, child in children.items():
                    if child.poll() is None:
                        child.terminate()
                        stop_session(pm, names[role])
                        child.wait(timeout=10)
            sample()
        finally:
            for role, child in children.items():
                if child.poll() is None:
                    child.terminate()
                    stop_session(pm, names[role])
            for stream in streams:
                stream.close()
            resource_stop.set()
            resource_thread.join(timeout=7)
    address_after = address_snapshot("after")
    result = {"run": run, "focus_samples": focus_samples, "other_foreground_samples": lost_samples,
              "telemetry_samples": len(telemetry), "telemetry_errors": telemetry_errors,
              "resource_samples": resource_state["samples"], "resource_error": resource_state["error"],
              "address_space_before_complete": address_before, "address_space_after_complete": address_after,
              "resource_sampler_still_running": resource_thread.is_alive(),
              "interrupted": interrupted, "outputs": {}}
    for role, pid in roles:
        path = out / f"run-{run}-{role}.csv"
        info = {"exit_code": children[role].returncode, "exists": path.is_file()}
        if path.is_file() and path.stat().st_size:
            try:
                with path.open("r", encoding="utf-8-sig", newline="") as stream:
                    reader = csv.DictReader(stream)
                    rows = list(reader)
                    report = summarize(reader.fieldnames or [], rows, expected_seconds=seconds)
                info["expected_pid_only"] = {g["process_id"] for g in report["groups"]} == {pid}
                info["sufficient_groups"] = all(g["meets_capture_length_and_count"] for g in report["groups"])
                write_json(out / f"run-{run}-{role}-analysis.json", report)
                if role == "host":
                    matches = correlate(telemetry, rows, host_pid, nr_enabled=nr_enabled)
                    write_json(out / f"run-{run}-correlation.json", matches)
                    info["proxy_candidate"] = matches["proxy_candidate"]
                    proxy = next((g for g in matches["groups"] if g["swapchain"] == matches["proxy_candidate"]), None)
                    info["nr_state_fully_observed"] = bool(proxy and proxy["desired_nr_state_rows"] == proxy["rows"])
            except (OSError, ValueError) as exc:
                info["analysis_error"] = str(exc)
        result["outputs"][role] = info
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--presentmon", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("dxvk", "d3d9", "nr-off", "nr-on", "nr-on-flow"), default="nr-on")
    parser.add_argument("--scene", default="")
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.warmup <= 120 or not 1 <= args.seconds <= 300 or not 1 <= args.repeats <= 5:
        parser.error("warmup, capture duration, or repeat count is out of bounds")
    client = checked(args.client, SCRATCH, exists=True)
    pm = checked(args.presentmon, SCRATCH, exists=True)
    output = checked(args.output, SCRATCH, exists=False)
    if output.exists() or output.is_relative_to(client):
        parser.error("output must be a new directory outside the client")
    game = checked(client / "wow.exe", SCRATCH, exists=True)
    require_host = args.mode in ("nr-off", "nr-on", "nr-on-flow")
    nr_enabled = args.mode != "nr-off"
    host = checked(client / "host64" / "AIO DLSS5 32-bit Wrapper.exe", SCRATCH, exists=require_host)
    entry = checked(client / "d3d9.dll", SCRATCH, exists=args.mode != "d3d9")
    if sha(game) != GAME_SHA or sha(pm) != PM_SHA:
        parser.error("game or PresentMon identity differs from the lock")
    if args.mode == "d3d9" and entry.exists():
        parser.error("native D3D9 mode requires the game-local d3d9.dll to be absent")
    if args.mode == "dxvk" and sha(entry) != DXVK_SHA:
        parser.error("DXVK identity differs from the lock")
    if require_host and (sha(host) != HOST_SHA or sha(entry) != RESHADE_SHA):
        parser.error("AIO host or ReShade identity differs from the lock")
    config_path = client / "host64" / "ReShade.ini"
    config_sha = None
    settings = None
    if require_host:
        checked(config_path, SCRATCH, exists=True)
        config = configparser.ConfigParser(interpolation=None, strict=True)
        with config_path.open("r", encoding="utf-8-sig") as stream:
            config.read_file(stream)
        section = config["Standalone.DLSSNR"]
        expected_flow = "1" if args.mode == "nr-on-flow" else "0"
        if (section.get("NeuralRendering", "1") != ("1" if nr_enabled else "0") or
                section.get("FrameGeneration", "0") != "0" or
                section.get("NvidiaOpticalFlowMotion", "0") != expected_flow):
            parser.error("host configuration does not match the selected NR, FG, or optical-flow mode")
        config_sha = sha(config_path)
        settings = {name: section.get(name, None) for name in (
            "NeuralRendering", "FrameGeneration", "NvidiaOpticalFlowMotion", "Model", "Intensity",
            "SourceResolutionOverride", "DetachedPresentation", "WindowedInputScaling")}
    game_pid, host_pid = processes(game, host, require_host=require_host)
    output.mkdir(parents=True, exist_ok=False)
    summary = {"state": "in_progress", "started_utc": datetime.now(timezone.utc).isoformat(),
               "mode": args.mode, "scene": args.scene, "config_sha256": config_sha, "settings": settings,
               "game_pid": game_pid, "host_pid": host_pid, "input_tracking": False,
               "warmup_seconds": args.warmup, "capture_seconds": args.seconds,
               "requested_repeats": args.repeats, "runs": [],
               "limits": ["One configuration and scene per invocation; comparisons require separate captures.",
                          "Pointer correlation is a candidate, not proof of on-screen visibility.",
                          "GPU utilization and VRAM are device-wide, not attributed to the game or host.",
                          "Resource sampling can affect performance and is not an input-latency measurement."]}
    try:
        mapping_context = open_host(host_pid) if require_host else nullcontext(None)
        with mapping_context as mapping:
            for run in range(1, args.repeats + 1):
                if processes(game, host, require_host=require_host) != (game_pid, host_pid):
                    raise RuntimeError("game or host changed during collection")
                print(f"run {run}: waiting for {args.warmup} seconds of stable foreground and NR state", flush=True)
                warmup(mapping, game_pid, args.warmup, time.monotonic() + 900, nr_enabled=nr_enabled)
                print(f"run {run}: collecting {args.seconds} seconds", flush=True)
                result = record_one(output, run, pm, game_pid, host_pid, mapping, args.seconds, nr_enabled=nr_enabled)
                summary["runs"].append(result)
                host_info = result["outputs"].get("host", {})
                if (result["interrupted"] or result["other_foreground_samples"] or
                        any(x.get("exit_code") != 0 or not x.get("expected_pid_only") or not x.get("sufficient_groups")
                            for x in result["outputs"].values()) or
                        (require_host and (not host_info.get("proxy_candidate") or
                                           not host_info.get("nr_state_fully_observed") or sha(config_path) != config_sha))):
                    raise RuntimeError("capture, focus, NR state, or proxy correlation requires review")
        summary["state"] = "completed_requires_review"
    except (Exception, KeyboardInterrupt) as exc:
        summary["state"] = "incomplete"
        summary["error"] = f"{type(exc).__name__}: {exc}"
    summary["ended_utc"] = datetime.now(timezone.utc).isoformat()
    write_json(output / "summary.json", summary)
    print(f"{summary['state']}: {output}", flush=True)
    return 0 if summary["state"] == "completed_requires_review" else 1


if __name__ == "__main__":
    raise SystemExit(main())
