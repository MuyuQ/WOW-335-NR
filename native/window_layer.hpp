#pragma once
#include <windows.h>

struct LayerResult {
    bool ok = false;
    DWORD error = 0;
    LONG_PTR before = 0;
    LONG_PTR after = 0;
};

inline LayerResult SetClickThroughLayer(HWND window, bool preview) {
    LayerResult result;
    if (!IsWindow(window)) { result.error = ERROR_INVALID_WINDOW_HANDLE; return result; }
    SetLastError(0);
    result.before = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!result.before && GetLastError()) { result.error = GetLastError(); return result; }
    const LONG_PTR main_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    const LONG_PTR preview_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
    if (result.before != (preview ? preview_style : main_style)) {
        result.error = ERROR_INVALID_STATE;
        return result;
    }
    result.after = result.before | WS_EX_LAYERED | (preview ? WS_EX_TRANSPARENT : 0);
    SetLastError(0);
    const LONG_PTR previous = SetWindowLongPtrW(window, GWL_EXSTYLE, result.after);
    if (!previous && GetLastError()) { result.error = GetLastError(); return result; }
    if (previous != result.before) {
        SetWindowLongPtrW(window, GWL_EXSTYLE, previous);
        result.error = ERROR_RETRY;
        return result;
    }
    BOOL ok = SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
    if (ok) ok = SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (ok) ok = GetWindowLongPtrW(window, GWL_EXSTYLE) == result.after;
    if (ok) {
        BYTE alpha = 0;
        DWORD flags = 0;
        ok = GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) && alpha == 255 && (flags & LWA_ALPHA);
    }
    if (!ok) {
        result.error = GetLastError();
        if (!result.error) result.error = ERROR_INVALID_STATE;
        SetWindowLongPtrW(window, GWL_EXSTYLE, result.before);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return result;
    }
    result.ok = true;
    return result;
}
