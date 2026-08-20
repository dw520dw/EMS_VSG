#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

/**
 * 崩溃诊断与加固：
 *  - 忽略 SIGPIPE（stdout/stderr 写到断开的管道时不再杀死进程，只返回 EPIPE）
 *  - 捕获崩溃信号（SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE）与 SIGTERM/SIGINT，
 *    把信号号 + 调用栈写入 collect.log，再按默认行为退出（保留 core dump）
 *  - 捕获未捕获异常（std::terminate），把异常 what() 写入 collect.log 后再 abort
 *
 * 必须在 main 最开头、创建任何线程之前调用一次。
 * 只做日志，不改变任何采集逻辑。
 */
void installCrashHandler();

#endif  // CRASH_HANDLER_H
