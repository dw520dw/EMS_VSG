#include "crash_handler.h"

#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <string>

#include <unistd.h>
#include <fcntl.h>

#if defined(__linux__) && defined(__GLIBC__)
#include <execinfo.h>
#endif

namespace {

// 与 db/logger.cpp 保持同一路径；崩溃时不能用 LOG_ACTION（非信号安全），直接裸写。
const char kCrashLogPath[] = "/userdata/iEMS-MG1000/collectLog/collect.log";

const char* signalName(int sig)
{
    switch (sig) {
        case SIGSEGV: return "SIGSEGV 段错误(空指针/内存越界)";
        case SIGABRT: return "SIGABRT abort(断言/terminate)";
#ifdef SIGBUS
        case SIGBUS:  return "SIGBUS 总线错误(越界/对齐)";
#endif
        case SIGILL:  return "SIGILL 非法指令";
        case SIGFPE:  return "SIGFPE 除零/浮点异常";
        case SIGTERM: return "SIGTERM 外部 kill -TERM";
        case SIGINT:  return "SIGINT Ctrl+C";
        default:      return "其他信号";
    }
}

/** 把信号号 + 调用栈追加到 collect.log。
 *  只使用 async-signal-safe 函数（open/write/close/time/getpid/backtrace），
 *  不分配内存、不上锁、不用 iostream，避免在崩溃现场二次崩溃。 */
void writeCrashNote(int sig)
{
    const int fd = ::open(kCrashLogPath, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        return;
    }
    char buf[192];
    const int n = snprintf(buf, sizeof(buf),
                           "\n========== 程序异常终止 ==========\n"
                           "时间: %lld\n信号: %d (%s)\nPID: %d\n调用栈:\n",
                           static_cast<long long>(::time(nullptr)), sig, signalName(sig),
                           ::getpid());
    if (n > 0) {
        ::write(fd, buf, static_cast<size_t>(n));
    }

#if defined(__linux__) && defined(__GLIBC__)
    void* frames[32];
    const int cnt = backtrace(frames, 32);
    if (cnt > 0) {
        backtrace_symbols_fd(frames, cnt, fd);
    }
#else
    static const char kNoBt[] = "(当前环境无 backtrace 支持)\n";
    ::write(fd, kNoBt, sizeof(kNoBt) - 1);
#endif

    static const char kEnd[] = "========== 结束 ==========\n";
    ::write(fd, kEnd, sizeof(kEnd) - 1);
    ::close(fd);
}

void crashHandler(int sig)
{
    // 先恢复默认动作，随后重发信号：以标准退出码退出并可产生 core dump
    ::signal(sig, SIG_DFL);
    writeCrashNote(sig);
    ::raise(sig);
    ::_exit(128 + sig);  // 兜底：若 raise 未生效
}

/** std::terminate（未捕获异常 / 异常发生在析构中 / noexcept 违例）时，
 *  先把当前异常信息写进 collect.log，再 abort 让 crashHandler 补调用栈。 */
void terminateHandler()
{
    std::string msg = "std::terminate 被调用（未捕获异常或二次异常）";
    try {
        std::rethrow_exception(std::current_exception());
    } catch (const std::exception& e) {
        msg = std::string("未捕获异常: ") + e.what();
    } catch (...) {
        msg = "未捕获未知类型异常";
    }

    const int fd = ::open(kCrashLogPath, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
        const std::string line = "\n---------- std::terminate ----------\n" + msg + "\n";
        ::write(fd, line.data(), line.size());
        ::close(fd);
    }
    std::abort();  // 触发 SIGABRT → crashHandler → 记录调用栈
}

}  // namespace

void installCrashHandler()
{
    // SIGPIPE 忽略：stdout/stderr 写到已断开/无读端的管道（SSH断开、supervisor退出等）时
    // 不再让进程死亡，而是让读写返回 EPIPE。这是“跑着跑着突然没、零日志”的经典来源。
#ifdef SIGPIPE
    ::signal(SIGPIPE, SIG_IGN);
#endif

    const int kSignals[] = {
        SIGSEGV, SIGABRT,
#ifdef SIGBUS
        SIGBUS,
#endif
        SIGILL, SIGFPE, SIGTERM, SIGINT
    };
    for (int s : kSignals) {
        ::signal(s, crashHandler);
    }

    std::set_terminate(terminateHandler);
}
