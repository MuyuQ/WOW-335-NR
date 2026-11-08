"""Read the public AIO v1 telemetry mapping without creating or modifying it."""

import ctypes as ct
import struct
import time
from ctypes import wintypes as wt

MAGIC = 0x4F494144
VERSION = 1
SIZE = 320
PREFIX = "Local\\DLSS5_AIO_Telemetry_"
FILE_MAP_READ = 0x0004
kernel = ct.WinDLL("kernel32", use_last_error=True)
kernel.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
kernel.OpenFileMappingW.restype = wt.HANDLE
kernel.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ct.c_size_t]
kernel.MapViewOfFile.restype = ct.c_void_p
kernel.UnmapViewOfFile.argtypes = [ct.c_void_p]
kernel.UnmapViewOfFile.restype = wt.BOOL
kernel.CloseHandle.argtypes = [wt.HANDLE]
kernel.CloseHandle.restype = wt.BOOL


def parse_snapshot(data, expected_pid):
    if len(data) != SIZE:
        raise ValueError("incorrect telemetry snapshot length")
    magic, version, size, pid = struct.unpack_from("<IIII", data, 0)
    if magic != MAGIC or version != VERSION or size < SIZE:
        raise ValueError("unrecognized telemetry magic, version, or size")
    if pid != expected_pid or pid <= 0:
        raise ValueError("telemetry belongs to a different process")
    sequence, qpc, frequency = struct.unpack_from("<qqq", data, 16)
    if sequence & 1 or qpc <= 0 or frequency <= 0:
        raise ValueError("unstable telemetry sequence or invalid clock")
    epoch, mode, flags, graphics_api = struct.unpack_from("<IIII", data, 40)
    input_width, input_height, output_width, output_height = struct.unpack_from("<IIII", data, 56)
    primary, proxy = struct.unpack_from("<QQ", data, 304)
    return {
        "process_id": pid, "sequence": sequence, "qpc": qpc,
        "qpc_frequency": frequency, "qpc_ms": qpc * 1000.0 / frequency,
        "benchmark_epoch": epoch, "benchmark_mode": mode, "flags": flags,
        "addon_enabled": bool(flags & (1 << 0)),
        "nr_enabled": bool(flags & (1 << 1)),
        "frame_generation_enabled": bool(flags & (1 << 2)),
        "neural_pipeline_ready": bool(flags & (1 << 3)),
        "processed_output_visible": bool(flags & (1 << 5)),
        "dlaa_mode": bool(flags & (1 << 7)),
        "proxy_hidden": bool(flags & (1 << 8)),
        "proxy_failed": bool(flags & (1 << 9)),
        "reshade_overlay_open": bool(flags & (1 << 11)),
        "graphics_api": graphics_api,
        "input_size": [input_width, input_height],
        "output_size": [output_width, output_height],
        "nr_frames": struct.unpack_from("<Q", data, 200)[0],
        "sr_frames": struct.unpack_from("<Q", data, 208)[0],
        "fg_frames": struct.unpack_from("<Q", data, 216)[0],
        "frames_presented": struct.unpack_from("<Q", data, 224)[0],
        "primary_swapchain": hex(primary) if primary else None,
        "proxy_swapchain": hex(proxy) if proxy else None,
    }


def read_stable(read_bytes, expected_pid, attempts=20):
    for _ in range(attempts):
        before = struct.unpack("<q", read_bytes(16, 8))[0]
        if before & 1:
            time.sleep(0.001)
            continue
        payload = read_bytes(0, SIZE)
        after = struct.unpack("<q", read_bytes(16, 8))[0]
        inside = struct.unpack_from("<q", payload, 16)[0]
        if before == inside == after and not (after & 1):
            return parse_snapshot(payload, expected_pid)
    raise RuntimeError("could not obtain a stable telemetry snapshot")


class TelemetryMap:
    def __init__(self, name, expected_pid):
        if expected_pid <= 0 or not name:
            raise ValueError("a positive process ID and mapping name are required")
        self.expected_pid = expected_pid
        self.handle = kernel.OpenFileMappingW(FILE_MAP_READ, False, name)
        if not self.handle:
            raise ct.WinError(ct.get_last_error())
        self.view = kernel.MapViewOfFile(self.handle, FILE_MAP_READ, 0, 0, SIZE)
        if not self.view:
            error = ct.get_last_error()
            kernel.CloseHandle(self.handle)
            self.handle = None
            raise ct.WinError(error)

    def read(self):
        if not self.view:
            raise RuntimeError("telemetry view is closed")
        return read_stable(lambda offset, length: ct.string_at(self.view + offset, length), self.expected_pid)

    def close(self):
        if self.view:
            kernel.UnmapViewOfFile(self.view)
            self.view = None
        if self.handle:
            kernel.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.close()


def open_host(pid):
    return TelemetryMap(PREFIX + str(pid), pid)
