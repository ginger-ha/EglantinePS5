// SPDX-License-Identifier: GPL-3.0-or-later
// Log streams without storage stalls. On the console a small write to a file under /data takes
// ~25 ms or more, and the GL driver's batch summary line on the unbuffered stderr held up present
// for ~300 ms every 10000 draws. The streams' descriptors cannot be pointed at a pipe there
// (fcntl(F_DUPFD): EINVAL, dup2: EPERM), so both streams are fully buffered and a background
// thread writes them out once a second. A thread that prints only waits for the storage when it
// prints while that write is going on, or when a second's lines overfill the buffer. Lines still
// buffered when the process is killed are lost (a second's worth); the crash handler writes them
// out (crash_report.cpp). To find a fault that kills the process anyway, Settings > Diagnostics >
// "Write logs at once" has every line written as it comes, as before: stderr unbuffered, stdout
// by lines, and games stutter again.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include "storage_paths.h"

namespace Eden {

inline void MirrorLogsToMountedUsb() noexcept {
    try {
        std::vector<std::string> valid_dirs;
        for (int i = 0; i <= 7; ++i) {
            const std::string candidate = "/mnt/usb" + std::to_string(i);
            std::error_code ec;
            if (!std::filesystem::is_directory(candidate, ec)) continue;
            const bool likely = std::filesystem::is_directory(candidate + "/games", ec) ||
                                std::filesystem::is_directory(candidate + "/PPSA98515", ec) ||
                                std::filesystem::is_directory(candidate + "/homebrew", ec) ||
                                std::filesystem::is_directory(candidate + "/eglantine_logs", ec);
            const std::string logs_dir = candidate + "/eglantine_logs";
            std::filesystem::create_directories(logs_dir, ec);
            if (!ec) {
                const std::string probe = logs_dir + "/.probe";
                std::ofstream probe_file(probe, std::ios::trunc);
                if (probe_file) {
                    probe_file << "ok";
                    probe_file.close();
                    std::filesystem::remove(probe, ec);
                    if (likely) {
                        valid_dirs.insert(valid_dirs.begin(), logs_dir);
                    } else {
                        valid_dirs.push_back(logs_dir);
                    }
                }
            }
        }
        if (valid_dirs.empty()) return;

        auto copy_file_safe = [&](const std::string& src, const std::string& dst) {
            std::error_code err;
            if (!std::filesystem::exists(src, err)) return;
            std::ifstream in(src, std::ios::binary);
            if (!in) return;
            std::ofstream out(dst, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out << in.rdbuf();
            out.flush();
        };

        for (const auto& logs_dir : valid_dirs) {
            copy_file_safe(LogFile("stderr.log"), logs_dir + "/stderr.log");
            copy_file_safe(LogFile("heap.log"), logs_dir + "/heap.log");
            copy_file_safe(UserDir() + "/log/eden_log.txt", logs_dir + "/eden_log.txt");
            copy_file_safe(LogFile("stderr.prev.log"), logs_dir + "/stderr.prev.log");
            copy_file_safe(LogFile("crash-note.txt"), logs_dir + "/crash-note.txt");
        }
    } catch (...) {}
}

class LogFlusher {
public:
    LogFlusher() = default;
    LogFlusher(const LogFlusher&) = delete;
    LogFlusher& operator=(const LogFlusher&) = delete;
    ~LogFlusher() { Stop(); }

    // The streams must already write to their log files. At once: every line is written as it
    // comes, with no thread.
    bool Start(bool at_once = false) {
        static char stderr_buffer[64 * 1024], stdout_buffer[64 * 1024];
        if (worker.joinable()) return true;
        if (at_once)
            return std::setvbuf(stderr, nullptr, _IONBF, 0) == 0 &&
                   (std::setvbuf(stdout, stdout_buffer, _IOLBF, sizeof(stdout_buffer)) == 0 ||
                    std::setvbuf(stdout, nullptr, _IONBF, 0) == 0);
        if (std::setvbuf(stderr, stderr_buffer, _IOFBF, sizeof(stderr_buffer)) != 0 ||
            std::setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer)) != 0) return false;
        worker = std::thread([this] { Run(); });
        return true;
    }

    // Writes out what is buffered; the streams stay buffered.
    void Stop() {
        if (!worker.joinable()) return;
        {
            std::lock_guard lock{mutex};
            stopping = true;
        }
        wake.notify_all();
        worker.join();
        MirrorLogsToMountedUsb();
    }

private:
    void Run() {
        for (bool stop = false; !stop;) {
            {
                std::unique_lock lock{mutex};
                stop = wake.wait_for(lock, std::chrono::seconds(1), [this] { return stopping; });
            }
            std::fflush(stderr);
            std::fflush(stdout);
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;
};
}
