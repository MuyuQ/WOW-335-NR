"""Capture the selected game's desktop region for later visual comparison.

This records desktop pixels inside the game rectangle, including any visible overlays
or notifications. Review and share the resulting local file only when appropriate.
"""

import argparse
import ctypes as ct
from ctypes import wintypes as wt
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import time

from collect import GAME_SHA, HOST_SHA, SCRATCH, checked, foreground_is, processes, sha, write_json

user32 = ct.WinDLL("user32", use_last_error=True)
user32.SetProcessDpiAwarenessContext.argtypes = [ct.c_void_p]
user32.SetProcessDpiAwarenessContext.restype = wt.BOOL
user32.GetThreadDpiAwarenessContext.argtypes = []
user32.GetThreadDpiAwarenessContext.restype = ct.c_void_p
user32.GetAwarenessFromDpiAwarenessContext.argtypes = [ct.c_void_p]
user32.GetAwarenessFromDpiAwarenessContext.restype = ct.c_int
user32.GetForegroundWindow.argtypes = []
user32.GetForegroundWindow.restype = wt.HWND
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ct.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ct.c_int]
user32.GetClassNameW.restype = ct.c_int
user32.GetClientRect.argtypes = [wt.HWND, ct.POINTER(wt.RECT)]
user32.GetClientRect.restype = wt.BOOL
user32.ClientToScreen.argtypes = [wt.HWND, ct.POINTER(wt.POINT)]
user32.ClientToScreen.restype = wt.BOOL
user32.GetSystemMetrics.argtypes = [ct.c_int]
user32.GetSystemMetrics.restype = ct.c_int


def game_rectangle(game_pid):
    if not user32.SetProcessDpiAwarenessContext(ct.c_void_p(-4)):
        context = user32.GetThreadDpiAwarenessContext()
        if not context or user32.GetAwarenessFromDpiAwarenessContext(context) != 2:
            raise RuntimeError("per-monitor DPI awareness is unavailable")
    hwnd = user32.GetForegroundWindow()
    owner = wt.DWORD()
    if not hwnd or not user32.GetWindowThreadProcessId(hwnd, ct.byref(owner)) or owner.value != game_pid:
        raise RuntimeError("selected game is not the foreground window")
    name = ct.create_unicode_buffer(256)
    if not user32.GetClassNameW(hwnd, name, len(name)) or name.value != "GxWindowClassD3d":
        raise RuntimeError("foreground window is not the expected game class")
    rect, top_left, bottom_right = wt.RECT(), wt.POINT(), wt.POINT()
    if not user32.GetClientRect(hwnd, ct.byref(rect)):
        raise ct.WinError(ct.get_last_error())
    top_left.x, top_left.y = rect.left, rect.top
    bottom_right.x, bottom_right.y = rect.right, rect.bottom
    if not user32.ClientToScreen(hwnd, ct.byref(top_left)) or not user32.ClientToScreen(hwnd, ct.byref(bottom_right)):
        raise ct.WinError(ct.get_last_error())
    left, top, right, bottom = top_left.x, top_left.y, bottom_right.x, bottom_right.y
    virtual_left, virtual_top = user32.GetSystemMetrics(76), user32.GetSystemMetrics(77)
    virtual_right = virtual_left + user32.GetSystemMetrics(78)
    virtual_bottom = virtual_top + user32.GetSystemMetrics(79)
    if not (virtual_left <= left < right <= virtual_right and virtual_top <= top < bottom <= virtual_bottom):
        raise RuntimeError("game client rectangle is not entirely within the virtual desktop")
    return [left, top, right, bottom]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--kind", choices=("image", "video"), required=True)
    parser.add_argument("--mode", choices=("aio", "baseline"), required=True)
    parser.add_argument("--seconds", type=int, default=8)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 30:
        parser.error("video duration must be between 1 and 30 seconds")
    client = checked(args.client, SCRATCH, exists=True)
    output = checked(args.output, SCRATCH, exists=False)
    suffix = ".png" if args.kind == "image" else ".mkv"
    if output.suffix.lower() != suffix or output.is_relative_to(client):
        parser.error("output must use the correct extension and be outside the client")
    ffmpeg = args.ffmpeg.resolve(strict=True)
    if not ffmpeg.is_file() or ffmpeg.suffix.lower() != ".exe":
        parser.error("ffmpeg must be an existing executable")
    game = checked(client / "wow.exe", SCRATCH, exists=True)
    host = checked(client / "host64" / "AIO DLSS5 32-bit Wrapper.exe", SCRATCH, exists=args.mode == "aio")
    if sha(game) != GAME_SHA or (args.mode == "aio" and sha(host) != HOST_SHA):
        parser.error("game or host identity differs from the lock")
    game_pid, _ = processes(game, host, require_host=args.mode == "aio")
    rectangle = game_rectangle(game_pid)
    meta, error_log = output.with_name(output.name + ".capture.json"), output.with_name(output.name + ".ffmpeg.log")
    for path in (output, meta, error_log):
        checked(path, SCRATCH, exists=False)
        if path.exists():
            parser.error("a capture output already exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    left, top, right, bottom = rectangle
    rate = 1 if args.kind == "image" else 30
    command = [str(ffmpeg), "-hide_banner", "-n", "-f", "gdigrab", "-framerate", str(rate),
               "-offset_x", str(left), "-offset_y", str(top), "-video_size", f"{right-left}x{bottom-top}",
               "-draw_mouse", "0", "-i", "desktop", "-an"]
    if args.kind == "image":
        command += ["-frames:v", "1"]
    else:
        command += ["-t", str(args.seconds), "-c:v", "ffv1", "-level", "3"]
    command.append(str(output))
    result = {"kind": args.kind, "mode": args.mode, "rectangle": rectangle, "game_pid": game_pid,
              "ffmpeg_sha256": sha(ffmpeg), "started_utc": datetime.now(timezone.utc).isoformat(),
              "desktop_region_includes_overlays": True, "focus_lost": False, "timed_out": False,
              "limits": ["Desktop-region capture may include notifications or other visible windows.",
                         "The detached AIO composition must be verified visually in the resulting capture.",
                         "Video capture affects performance; do not use it during timing benchmarks."]}
    child = None
    with error_log.open("xb") as errors:
        try:
            child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=errors,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + args.seconds + 15
            while child.poll() is None and time.monotonic() < deadline:
                if not foreground_is(game_pid):
                    result["focus_lost"] = True
                    break
                time.sleep(0.2)
            if child.poll() is None:
                result["timed_out"] = not result["focus_lost"]
                try:
                    child.stdin.write(b"q\n")
                    child.stdin.flush()
                    child.wait(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    child.terminate()
                    child.wait(timeout=5)
            result["exit_code"] = child.returncode
        except (Exception, KeyboardInterrupt) as exc:
            result["error"] = f"{type(exc).__name__}: {exc}"
        finally:
            if child is not None and child.poll() is None:
                child.terminate()
                child.wait(timeout=5)
            if child is not None and child.stdin is not None:
                child.stdin.close()
    result["ended_utc"] = datetime.now(timezone.utc).isoformat()
    result["file_exists"] = output.is_file()
    result["file_bytes"] = output.stat().st_size if output.is_file() else 0
    result["requires_review"] = True
    write_json(meta, result)
    print(f"capture finished; review the local file: {output}")
    return 0 if (result.get("exit_code") == 0 and not result["focus_lost"] and
                 not result["timed_out"] and result["file_bytes"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
