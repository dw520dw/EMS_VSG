#ifndef THREAD_MANAGER_H
#define THREAD_MANAGER_H

#include <atomic>
#include <csignal>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <iostream>
#include <string>
#include <chrono>

/** 停止信号：handler 唯一可做的安全操作就是置此标志（volatile sig_atomic_t 保证异步信号安全）。
 *  必须声明在类之前：类内成员函数体的名字查找只到类定义处。 */
inline volatile sig_atomic_t g_stopSignal = 0;

/**
 * 线程生命周期管理器
 * - 每个设备仍然是独立线程（保证实时性）
 * - 统一启动/停止
 * - 信号安全停止：handler 只置标志，绝不 join / cout / 上锁
 * - watchdog：工作线程异常退出或提前返回时标记 threadDied_，waitForStop 立即返回
 */
class ThreadManager {
public:
    using ThreadFunc = std::function<void()>;

    static ThreadManager& instance() {
        static ThreadManager mgr;
        return mgr;
    }

    /**
     * 注册一个工作线程；func 会被 runGuarded 包裹（异常/提前退出 → threadDied_）
     * @param name 线程名称（用于调试）
     * @param func 线程函数（内部应检查 isRunning()，且应为无限循环）
     */
    void registerThread(const std::string& name, ThreadFunc func) {
        threads_.emplace_back(name, runGuarded(name, std::move(func)));
    }

    /**
     * 启动所有线程；某线程创建失败时记入 threadDied_ 并继续，交由 watchdog 收尾
     */
    void startAll() {
        running_ = true;
        for (auto& entry : threads_) {
            try {
                entry.handle = std::thread(std::move(entry.func));
                std::cout << "线程启动: " << entry.name << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "线程创建失败: " << entry.name << " (" << e.what() << ")" << std::endl;
                threadDied_.store(true);
            }
        }
    }

    /**
     * 停止所有线程（优雅关闭）。
     * 由主线程在 waitForStop 之后调用；不得从信号 handler 中调用（join 非 async-signal-safe）。
     */
    void stopAll() {
        if (!running_.exchange(false)) {
            return;
        }
        std::cout << "正在停止所有线程..." << std::endl;
        for (auto& entry : threads_) {
            if (entry.handle.joinable()) {
                entry.handle.join();
                std::cout << "线程已停止: " << entry.name << std::endl;
            }
        }
    }

    /** 线程内应周期性检查此标志 */
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    /** 任一工作线程异常退出/提前返回（watchdog 触发） */
    bool anyThreadDied() const { return threadDied_.load(std::memory_order_acquire); }

    /**
     * 阻塞直到：收到停止信号(g_stopSignal) 或 任一工作线程死亡。
     * 100ms 轮询足够灵敏，主线程专用；返回 true 表示 watchdog 触发。
     */
    bool waitForStop() {
        while (running_.load(std::memory_order_acquire) && g_stopSignal == 0
               && !threadDied_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return threadDied_.load(std::memory_order_acquire);
    }

private:
    ThreadManager() = default;
    ~ThreadManager() {
        stopAll();
    }

    // 禁止拷贝和赋值
    ThreadManager(const ThreadManager&) = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;

    /**
     * 把用户线程函数包上 watchdog：
     * - 抛异常 → 记 threadDied_ 并打印
     * - 正常运行中提前返回（running_ 仍为 true 且无停止信号）→ 视为意外死亡，记 threadDied_
     */
    ThreadFunc runGuarded(const std::string& name, ThreadFunc func) {
        return [this, name, inner = std::move(func)]() {
            try {
                inner();
            } catch (const std::exception& e) {
                std::cerr << "线程 " << name << " 异常退出: " << e.what() << std::endl;
                threadDied_.store(true);
                return;
            } catch (...) {
                std::cerr << "线程 " << name << " 未知异常退出" << std::endl;
                threadDied_.store(true);
                return;
            }
            // 正常返回：项目内所有工作线程都应为无限循环，提前返回即异常
            if (running_.load(std::memory_order_acquire) && g_stopSignal == 0) {
                std::cerr << "线程 " << name << " 提前返回，视为异常退出" << std::endl;
                threadDied_.store(true);
            }
        };
    }

    struct ThreadEntry {
        std::string name;
        ThreadFunc func;
        std::thread handle;

        ThreadEntry(std::string n, ThreadFunc f)
            : name(std::move(n)), func(std::move(f)) {}
    };

    std::atomic<bool> running_{false};
    std::atomic<bool> threadDied_{false};
    std::vector<ThreadEntry> threads_;
};

/** 全局信号处理：只置标志，不 join / 不打日志 / 不上锁 —— 这些都是 async-signal-unsafe */
inline void signal_handler(int sig) {
    (void)sig;
    g_stopSignal = 1;
}

inline void setup_signal_handlers() {
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // kill
}

#endif  // THREAD_MANAGER_H
