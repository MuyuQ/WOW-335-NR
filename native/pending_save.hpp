#pragma once

namespace nr_files {

// 面板参数的待保存状态机。
// dirty: 有改动尚未落盘；failed: 上次写入失败，按 1s 退避重试；
// closed: 退出流程已强制落盘/已尝试落盘，此后不再接受新请求，避免退出过程中又排队。
struct PendingSave {
    bool dirty = false;
    bool failed = false;
    bool closed = false;

    unsigned IntervalMs() const { return failed ? 1000u : 200u; }

    // UI 改动入队。退出流程关闭通道后返回 false（改动不再生效，调用方无需重试）。
    bool Request() {
        if (closed) return false;
        dirty = true;
        return true;
    }

    // 定时重试路径。
    template <class Save> bool Try(Save save) {
        if (closed) return false;
        if (!save()) {
            failed = true;
            return false;
        }
        dirty = false;
        failed = false;
        return true;
    }

    // 退出/注销前的强制落盘：不等重试间隔，立即写一次并关闭通道。
    // 失败时保留 dirty/failed 供调用方记录日志；重复调用不再重复写盘。
    template <class Save> bool Flush(Save save) {
        if (closed) return !failed;
        closed = true;
        if (!dirty) {
            failed = false;
            return true;
        }
        if (!save()) {
            failed = true;
            return false;
        }
        dirty = false;
        failed = false;
        return true;
    }

    // 注销被取消（WM_ENDSESSION 的 wParam 为 FALSE）时恢复保存通道。
    void Reopen() { closed = false; }
};

}  // namespace nr_files
