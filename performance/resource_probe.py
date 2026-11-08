"""Read bounded process memory, 32-bit address space, and device-wide GPU metrics."""

import csv
import ctypes as ct
from ctypes import wintypes as wt
from pathlib import Path
import subprocess

kernel = ct.WinDLL("kernel32", use_last_error=True)
psapi = ct.WinDLL("psapi", use_last_error=True)
kernel.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel.OpenProcess.restype = wt.HANDLE
kernel.CloseHandle.argtypes = [wt.HANDLE]
kernel.CloseHandle.restype = wt.BOOL
kernel.VirtualQueryEx.argtypes = [wt.HANDLE, ct.c_void_p, ct.c_void_p, ct.c_size_t]
kernel.VirtualQueryEx.restype = ct.c_size_t


class ProcessMemoryCountersEx(ct.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD),
                ("PeakWorkingSetSize", ct.c_size_t), ("WorkingSetSize", ct.c_size_t),
                ("QuotaPeakPagedPoolUsage", ct.c_size_t), ("QuotaPagedPoolUsage", ct.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ct.c_size_t), ("QuotaNonPagedPoolUsage", ct.c_size_t),
                ("PagefileUsage", ct.c_size_t), ("PeakPagefileUsage", ct.c_size_t), ("PrivateUsage", ct.c_size_t)]


class MemoryBasicInformation(ct.Structure):
    _fields_ = [("BaseAddress", ct.c_void_p), ("AllocationBase", ct.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ct.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]


psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ct.POINTER(ProcessMemoryCountersEx), wt.DWORD]
psapi.GetProcessMemoryInfo.restype = wt.BOOL


def memory(pid):
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        raise ct.WinError(ct.get_last_error())
    try:
        counters = ProcessMemoryCountersEx()
        counters.cb = ct.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(handle, ct.byref(counters), counters.cb):
            raise ct.WinError(ct.get_last_error())
        return {"working_set_bytes": counters.WorkingSetSize, "private_commit_bytes": counters.PrivateUsage,
                "pagefile_usage_bytes": counters.PagefileUsage}
    finally:
        kernel.CloseHandle(handle)


def x86_address_space(pid):
    handle = kernel.OpenProcess(0x0400, False, pid)
    if not handle:
        raise ct.WinError(ct.get_last_error())
    result = {"range_bytes": 1 << 32, "committed_bytes": 0, "reserved_bytes": 0,
              "free_bytes": 0, "largest_free_region_bytes": 0, "complete": False}
    try:
        address, maximum = 0, 1 << 32
        while address < maximum:
            region = MemoryBasicInformation()
            size = kernel.VirtualQueryEx(handle, ct.c_void_p(address), ct.byref(region), ct.sizeof(region))
            if size != ct.sizeof(region) or not region.RegionSize:
                result["error"] = "VirtualQueryEx did not cover the full 32-bit range"
                break
            base = int(region.BaseAddress or 0)
            end = min(base + int(region.RegionSize), maximum)
            if base > address or end <= address:
                result["error"] = "invalid or non-contiguous address region"
                break
            length = end - address
            if region.State == 0x1000:
                result["committed_bytes"] += length
            elif region.State == 0x2000:
                result["reserved_bytes"] += length
            elif region.State == 0x10000:
                result["free_bytes"] += length
                result["largest_free_region_bytes"] = max(result["largest_free_region_bytes"], length)
            else:
                result["error"] = "unrecognized memory region state"
                break
            address = end
        result["complete"] = address == maximum
        return result
    finally:
        kernel.CloseHandle(handle)


def gpu_devices(executable):
    executable = Path(executable)
    if not executable.is_file():
        raise FileNotFoundError("NVIDIA system query utility is missing")
    command = [str(executable), "--query-gpu=index,name,utilization.gpu,memory.used,memory.total",
               "--format=csv,noheader,nounits"]
    result = subprocess.run(command, capture_output=True, encoding="utf-8", errors="replace", timeout=5,
                            creationflags=subprocess.CREATE_NO_WINDOW, check=True)
    devices = []
    for row in csv.reader(result.stdout.splitlines(), skipinitialspace=True):
        if len(row) != 5:
            raise ValueError("unexpected GPU query output")
        devices.append({"index": int(row[0]), "name": row[1].strip(), "utilization_percent": float(row[2]),
                        "memory_used_mib": float(row[3]), "memory_total_mib": float(row[4])})
    return devices
