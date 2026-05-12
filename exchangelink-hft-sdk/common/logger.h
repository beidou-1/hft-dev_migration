/**
 * @file logger.h
 * @brief 日志系统封装
 * @details 基于fmtlog库的日志系统，提供异步日志记录功能，支持日切
 */

#pragma once

#include "fmtlog/fmtlog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include <sys/stat.h>

#define INFRA_LOG_DEBUG(...) logd(__VA_ARGS__)
#define INFRA_LOG_INFO(...) logi(__VA_ARGS__)
#define INFRA_LOG_WARN(...) logw(__VA_ARGS__)
#define INFRA_LOG_ERROR(...) loge(__VA_ARGS__)

namespace infra {

inline constexpr auto k_logger_poll_interval = std::chrono::milliseconds(1);
inline constexpr auto k_logger_rotate_check_interval = std::chrono::seconds(1);

struct LoggerState {
    // active_path 始终是当前正在写入的文件，例如 logs/themis.log。
    // 日切时再把它归档为 logs/themis.YYYY-MM-DD.log，避免进程运行中直接写带日期的文件。
    std::string active_path;

    // current_day 记录当前 active 文件对应的本地日期，用于内部日切线程判断是否需要滚动。
    std::string current_day;
    bool initialized{false};
};

inline LoggerState g_logger_state;
inline std::mutex g_logger_mutex;
inline std::thread g_logger_thread;
inline std::mutex g_logger_thread_mutex;
inline std::condition_variable g_logger_thread_cv;
inline std::atomic_bool g_logger_stop_requested{false};
inline std::once_flag g_logger_atexit_once;

inline std::string local_day_string(time_t t) {
    struct tm tm_info;
    localtime_r(&t, &tm_info);

    char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_info);
    return date_buf;
}

inline std::string current_local_day() {
    return local_day_string(time(nullptr));
}

inline std::string file_local_day(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return {};

    // 启动时根据遗留 active 文件的 mtime 判断它属于哪一天。
    return local_day_string(st.st_mtime);
}

inline std::filesystem::path archive_path_for_day(const std::string& active_path, const std::string& day) {
    std::filesystem::path active(active_path);
    // logs/themis.log + 2026-04-28 -> logs/themis.2026-04-28.log
    auto archive_name = active.stem().string() + "." + day + active.extension().string();
    return active.parent_path() / archive_name;
}

inline void ensure_parent_dir(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty())
        return;

    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
}

inline bool append_file_to_archive(const std::filesystem::path& source, const std::filesystem::path& archive) {
    std::ifstream in(source, std::ios::binary);
    std::ofstream out(archive, std::ios::binary | std::ios::app);
    if (!in || !out)
        return false;

    // 归档文件已存在时追加，避免重启/多次日切覆盖同一天历史日志。
    out << in.rdbuf();
    return static_cast<bool>(out);
}

inline void archive_active_file(const std::string& day) {
    if (g_logger_state.active_path.empty() || day.empty())
        return;

    std::filesystem::path active(g_logger_state.active_path);
    std::error_code ec;
    if (!std::filesystem::exists(active, ec) || std::filesystem::is_empty(active, ec))
        return;

    auto archive = archive_path_for_day(g_logger_state.active_path, day);
    ensure_parent_dir(archive);

    if (std::filesystem::exists(archive, ec)) {
        // 目标归档已存在时采用 append，成功后再删除 active，避免异常情况下丢日志。
        if (append_file_to_archive(active, archive))
            std::filesystem::remove(active, ec);
    } else {
        // 首选 rename，通常是同目录原子操作；失败时再退化为 append + remove。
        std::filesystem::rename(active, archive, ec);
        if (ec) {
            if (append_file_to_archive(active, archive))
                std::filesystem::remove(active, ec);
        }
    }
}

inline void archive_stale_active_file(const std::string& today) {
    if (g_logger_state.active_path.empty())
        return;

    std::filesystem::path active(g_logger_state.active_path);
    std::error_code ec;
    if (!std::filesystem::exists(active, ec))
        return;

    auto active_day = file_local_day(active);
    // 进程跨天重启时，先把昨天遗留的 logs/themis.log 归档，再打开今天的 active 文件。
    if (!active_day.empty() && active_day != today)
        archive_active_file(active_day);
}

inline void shutdown_logger();

inline void logger_tick_once(bool check_rotate, bool force_flush) {
    std::lock_guard lk(g_logger_mutex);

    if (!g_logger_state.initialized) {
        fmtlog::poll(force_flush);
        return;
    }

    if (check_rotate) {
        auto today = current_local_day();
        if (today != g_logger_state.current_day) {
            // 日切顺序：
            // 1. 先强制 poll/flush，确保旧日期日志全部落到 active 文件；
            // 2. 关闭 active 文件，避免归档时仍被 fmtlog 持有；
            // 3. 将 active 文件归档为带日期文件；
            // 4. 重新打开新的 active 文件继续写今天日志。
            fmtlog::poll(true);
            fmtlog::closeLogFile();
            archive_active_file(g_logger_state.current_day);
            fmtlog::setLogFile(g_logger_state.active_path.c_str(), false);
            g_logger_state.current_day = today;
        }
    }

    fmtlog::poll(force_flush);
}

inline void logger_poll_loop() {
    auto next_rotate_check = std::chrono::steady_clock::now() + k_logger_rotate_check_interval;
    std::unique_lock lk(g_logger_thread_mutex);
    while (!g_logger_stop_requested.load(std::memory_order_acquire)) {
        if (g_logger_thread_cv.wait_for(lk, k_logger_poll_interval, [] {
                return g_logger_stop_requested.load(std::memory_order_acquire);
            })) {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        bool check_rotate = now >= next_rotate_check;
        if (check_rotate)
            next_rotate_check = now + k_logger_rotate_check_interval;

        lk.unlock();
        logger_tick_once(check_rotate, false);
        lk.lock();
    }
}

inline void start_logger_thread() {
    std::lock_guard lk(g_logger_thread_mutex);
    if (g_logger_thread.joinable())
        return;

    g_logger_stop_requested.store(false, std::memory_order_release);
    g_logger_thread = std::thread(logger_poll_loop);
}

inline void stop_logger_thread() {
    std::thread thread_to_join;
    {
        std::lock_guard lk(g_logger_thread_mutex);
        g_logger_stop_requested.store(true, std::memory_order_release);
        g_logger_thread_cv.notify_all();
        if (g_logger_thread.joinable())
            thread_to_join = std::move(g_logger_thread);
    }

    if (thread_to_join.joinable())
        thread_to_join.join();
}

inline void init_logger(const std::string& active_path) {
    stop_logger_thread();

    {
        std::lock_guard lk(g_logger_mutex);

        if (g_logger_state.initialized) {
            fmtlog::poll(true);
            fmtlog::closeLogFile();
            g_logger_state.initialized = false;
        }

        g_logger_state.active_path = active_path;
        g_logger_state.current_day = current_local_day();

        std::filesystem::path active(active_path);
        ensure_parent_dir(active);
        archive_stale_active_file(g_logger_state.current_day);

        // truncate=false 表示追加写 active 文件；日滚动由 logger 内部线程统一处理。
        fmtlog::setLogFile(g_logger_state.active_path.c_str(), false);
        fmtlog::setLogLevel(fmtlog::INF);
        fmtlog::setHeaderPattern("[{YmdHMSF}] [{l}] ");

        // 不使用 fmtlog callback 做日切。callback 发生在单条日志写入过程中，里面 close/reopen
        // 可能把同一条日志拆到两个文件；这里统一在 logger 内部 poll 线程中切换文件。
        fmtlog::setLogCB(nullptr, fmtlog::OFF);

        g_logger_state.initialized = true;
    }

    std::call_once(g_logger_atexit_once, [] {
        // 调用方只需要 init_logger；进程正常退出时自动停止后台线程并关闭日志文件。
        std::atexit([] { infra::shutdown_logger(); });
    });

    start_logger_thread();
}

inline void shutdown_logger() {
    stop_logger_thread();

    std::lock_guard lk(g_logger_mutex);
    if (!g_logger_state.initialized) {
        fmtlog::poll(true);
        fmtlog::closeLogFile();
        return;
    }

    // 退出前强制 flush/close，减少尾部日志停留在 fmtlog 队列中的风险。
    fmtlog::poll(true);
    fmtlog::closeLogFile();
    g_logger_state.initialized = false;
}

} // namespace infra
