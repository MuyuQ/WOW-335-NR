// WOW-335-NR 原生控制中心：后台任务队列。
//
// 设计目标（对应交接文档「后台读取与界面更新分离」「切页不卡」）：
//   - UI 线程永不执行哈希、目录扫描、事务预览等重活；
//   - 单个后台工作线程串行执行任务（安装/修复/卸载事务要求单写者，且
//     ClientLock 要求在同一线程 Acquire/Release，worker 内自行加锁）；
//   - 工作线程只把结果装进 shared_ptr，真正改状态在 UI 线程的 apply 回调里做，
//     因此界面状态没有跨线程写入；
//   - 结果通过 PostMessage 唤醒 UI 线程；UI 侧再校验 generation 与客户端，
//     切换客户端后旧任务结果被丢弃；
//   - 队列有上限，满时返回 false，由调用方显示错误，不阻塞 UI。
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cc {

// 后台任务完成消息：UI 收到后调用 JobQueue::DrainApply。
constexpr UINT WM_APP_JOB = WM_APP + 1;

class JobQueue {
public:
    // 工作线程执行体：捕获 shared_ptr 结果，不得访问 App 成员。
    using WorkFn = std::function<void()>;
    // UI 线程回调：error 为空表示工作成功；错误与结果都通过捕获的 shared_ptr 传给 UI。
    using ApplyFn = std::function<void(const std::wstring& error)>;

    JobQueue() = default;
    ~JobQueue() { Stop(); }
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    void Start(HWND hwnd) {
        hwnd_.store(hwnd, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return;
        stopping_ = false;
        exited_ = false;
        running_ = true;
        worker_ = std::thread([this] { Run(); });
    }

    // Discard work that has not started. The current task owns its transaction
    // and finishes normally; the UI remains alive to apply its result.
    void RequestStop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            queue_.clear();
        }
        cv_.notify_all();
    }
    bool HasExited() const { return exited_.load(std::memory_order_acquire); }
    void Stop() {
        RequestStop();
        if (worker_.joinable()) worker_.join();
        running_ = false;
        hwnd_.store(nullptr, std::memory_order_release);
        std::lock_guard<std::mutex> lock(doneMutex_);
        done_.clear();
    }

    // 入队；队列满或未启动时返回 false。
    bool Post(WorkFn work, ApplyFn apply) {
        Pending p;
        p.work = std::move(work);
        p.apply = std::move(apply);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || stopping_) return false;
            if (queue_.size() >= 32) return false;
            queue_.push_back(std::move(p));
        }
        cv_.notify_one();
        return true;
    }

    // UI 线程：取出并在当前线程执行所有已完成任务的 apply 回调。
    void DrainApply() {
        std::vector<std::pair<ApplyFn, std::wstring>> ready;
        {
            std::lock_guard<std::mutex> lock(doneMutex_);
            ready.swap(done_);
        }
        for (auto& item : ready) {
            if (item.first) item.first(item.second);
        }
    }

private:
    struct Pending {
        WorkFn work;
        ApplyFn apply;
    };

    void Run() {
        for (;;) {
            Pending p;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) { exited_.store(true, std::memory_order_release); return; }
                p = std::move(queue_.front());
                queue_.pop_front();
            }
            std::wstring error;
            if (p.work) {
                try {
                    p.work();
                } catch (const std::exception& ex) {
                    error = L"后台任务异常：";
                    error += Utf8ToWide(ex.what());
                } catch (...) {
                    error = L"后台任务异常：未知错误。";
                }
            }
            Complete(std::move(p.apply), std::move(error));
        }
    }

    void Complete(ApplyFn apply, std::wstring error) {
        {
            std::lock_guard<std::mutex> lock(doneMutex_);
            done_.emplace_back(std::move(apply), std::move(error));
        }
        HWND hwnd = hwnd_.load(std::memory_order_acquire);
        if (hwnd != nullptr) PostMessageW(hwnd, WM_APP_JOB, 0, 0);
    }

    static std::wstring Utf8ToWide(const char* text) {
        if (text == nullptr || *text == '\0') return std::wstring();
        int need = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (need <= 1) return std::wstring();
        std::wstring out(static_cast<size_t>(need - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], need);
        return out;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Pending> queue_;
    std::atomic<bool> running_{false};
    bool stopping_ = false;
    std::atomic<bool> exited_{true};
    std::thread worker_;

    std::mutex doneMutex_;
    std::vector<std::pair<ApplyFn, std::wstring>> done_;

    std::atomic<HWND> hwnd_{nullptr};
};

}  // namespace cc
