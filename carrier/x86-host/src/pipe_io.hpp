#pragma once
#include <windows.h>
#include <algorithm>

namespace feed_pipe {
// The handle must be opened with FILE_FLAG_OVERLAPPED. A deadline covers the
// entire message, including partial reads. Retire cancelled I/O before returning
// so neither OVERLAPPED nor the caller's buffer can outlive this stack frame.
inline bool Transfer(HANDLE pipe, HANDLE peer, void* buffer, DWORD size, bool writing, DWORD timeoutMs) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    DWORD offset = 0, error = ERROR_SUCCESS;
    while (offset < size) {
        if (peer && WaitForSingleObject(peer, 0) == WAIT_OBJECT_0) { error = ERROR_BROKEN_PIPE; break; }
        if (GetTickCount64() >= deadline) { error = ERROR_TIMEOUT; break; }
        OVERLAPPED operation{};
        operation.hEvent = event;
        ResetEvent(event);
        DWORD count = 0;
        auto* bytes = static_cast<unsigned char*>(buffer) + offset;
        BOOL ok = writing ? WriteFile(pipe, bytes, size - offset, &count, &operation)
                          : ReadFile(pipe, bytes, size - offset, &count, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[2] = {event, peer};
            const ULONGLONG now = GetTickCount64();
            const DWORD remaining = now < deadline ? static_cast<DWORD>((std::min)(deadline - now, ULONGLONG(MAXDWORD - 1))) : 0;
            const DWORD wait = WaitForMultipleObjects(peer ? 2 : 1, waits, FALSE, remaining);
            if (wait != WAIT_OBJECT_0) {
                error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT :
                        (wait == WAIT_OBJECT_0 + 1 ? ERROR_BROKEN_PIPE : GetLastError());
                CancelIoEx(pipe, &operation);
                GetOverlappedResult(pipe, &operation, &count, TRUE);
                break;
            }
            ok = GetOverlappedResult(pipe, &operation, &count, FALSE);
        }
        if (!ok) { error = GetLastError(); break; }
        if (count == 0) { error = ERROR_BROKEN_PIPE; break; }
        offset += count;
    }
    CloseHandle(event);
    SetLastError(error);
    return error == ERROR_SUCCESS && offset == size;
}
inline bool Write(HANDLE pipe, HANDLE peer, const void* buffer, DWORD size, DWORD timeoutMs = 250) {
    return Transfer(pipe, peer, const_cast<void*>(buffer), size, true, timeoutMs);
}
inline bool Read(HANDLE pipe, HANDLE peer, void* buffer, DWORD size, DWORD timeoutMs = 5000) {
    return Transfer(pipe, peer, buffer, size, false, timeoutMs);
}
}
