#include "backends/linux_system_clock_backend.h"

#include <cstring>
#include <sstream>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>
#endif

namespace tbox {
namespace time {

namespace {

int64_t realtimeMs() {
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    return 0;
#endif
}

int64_t bootMonotonicMs() {
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    return 0;
#endif
}

}  // namespace

LinuxSystemClockBackend::LinuxSystemClockBackend(LinuxBackendConfig cfg)
    : cfg_(std::move(cfg)) {}

BackendCapability LinuxSystemClockBackend::capability() const {
#if defined(__linux__)
    std::string detail;
    if (detectCompetingLocked(&detail) && !cfg_.allow_competing_writers) {
        return BackendCapability::kCompeting;
    }
    return BackendCapability::kReady;
#else
    return BackendCapability::kUnsupported;
#endif
}

ClockRead LinuxSystemClockBackend::readClock() {
    ClockRead r;
#if defined(__linux__)
    r.valid = true;
    r.realtime_epoch_ms = realtimeMs();
    r.boot_monotonic_ms = bootMonotonicMs();
#else
    r.valid = false;
    r.detail = "linux backend unavailable on this platform";
#endif
    return r;
}

BackendResult LinuxSystemClockBackend::stepTo(int64_t utc_epoch_ms) {
    BackendResult res;
#if defined(__linux__)
    struct timespec ts;
    ts.tv_sec = utc_epoch_ms / 1000;
    ts.tv_nsec = (utc_epoch_ms % 1000) * 1000000;
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        res.ok = false;
        res.errno_code = errno;
        res.detail = "clock_settime failed";
        return res;
    }
    res.ok = true;
#else
    res.ok = false;
    res.detail = "linux backend unavailable on this platform";
#endif
    return res;
}

BackendResult LinuxSystemClockBackend::slewBy(int64_t offset_ms) {
    BackendResult res;
#if defined(__linux__)
    struct timex tx;
    std::memset(&tx, 0, sizeof(tx));
    // 用 ADJ_OFFSET 的微秒制 slew；内核按时间常数渐进调整
    tx.modes = ADJ_OFFSET | ADJ_TIMECONST;
    tx.offset = offset_ms * 1000;  // 微秒
    tx.constant = 4;               // 时间常数（收敛速度，登记实际值）
    if (adjtimex(&tx) == -1) {
        res.ok = false;
        res.errno_code = errno;
        res.detail = "adjtimex failed";
        return res;
    }
    res.ok = true;
#else
    res.ok = false;
    res.detail = "linux backend unavailable on this platform";
#endif
    return res;
}

BackendResult LinuxSystemClockBackend::verifyAlignment(int64_t utc_epoch_ms,
                                                       int64_t error_upper_ms,
                                                       int64_t* alignment_error_ms) {
    BackendResult res;
#if defined(__linux__)
    // slew 渐进收敛：多次读回直到偏差落入误差区间内（或重试耗尽）
    int64_t best_dev = INT64_MAX;
    for (int i = 0; i < cfg_.verify_retries; ++i) {
        const int64_t wall = realtimeMs();
        int64_t dev = wall - utc_epoch_ms;
        if (dev < 0) dev = -dev;
        if (dev < best_dev) best_dev = dev;
        if (dev <= error_upper_ms) break;
        struct timespec pause{};
        pause.tv_nsec = 100 * 1000000;  // 100ms
        nanosleep(&pause, nullptr);
    }
    if (alignment_error_ms) {
        *alignment_error_ms = static_cast<int64_t>(realtimeMs()) - utc_epoch_ms;
    }
    res.ok = (best_dev <= error_upper_ms);
    if (!res.ok) {
        res.detail = "clock not aligned within error budget";
    }
#else
    res.ok = false;
    res.detail = "linux backend unavailable on this platform";
    if (alignment_error_ms) *alignment_error_ms = 0;
#endif
    return res;
}

bool LinuxSystemClockBackend::detectCompetingWriters(std::string* detail) {
#if defined(__linux__)
    return detectCompetingLocked(detail);
#else
    if (detail) *detail = "non-linux";
    return false;
#endif
}

#if defined(__linux__)
bool LinuxSystemClockBackend::detectCompetingLocked(std::string* detail) const {
    // 扫描 /proc 中的进程名，匹配 chronyd/ntpd/systemd-timesyncd/openntpd
    std::vector<std::string> names;
    {
        std::stringstream ss(cfg_.competing_processes);
        std::string item;
        while (std::getline(ss, item, ':')) {
            if (!item.empty()) names.push_back(item);
        }
    }
    DIR* dir = opendir("/proc");
    if (!dir) {
        if (detail) *detail = "cannot scan /proc";
        return false;
    }
    bool found = false;
    std::string found_name;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        char path[64], buf[256];
        std::snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        const ssize_t n = readlink(path, buf, sizeof(buf) - 1);
        if (n < 0) {
            // comm 是普通文件，非符号链接：改用读取
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                ssize_t m = read(fd, buf, sizeof(buf) - 1);
                ::close(fd);
                if (m > 0) {
                    buf[m] = '\0';
                    buf[m - 1] = '\0';  // 去掉换行
                    for (const auto& nm : names) {
                        if (std::strcmp(buf, nm.c_str()) == 0) {
                            found = true;
                            found_name = nm;
                            break;
                        }
                    }
                }
            }
            if (found) break;
            continue;
        }
        buf[n] = '\0';
        for (const auto& nm : names) {
            if (std::strcmp(buf, nm.c_str()) == 0) {
                found = true;
                found_name = nm;
                break;
            }
        }
        if (found) break;
    }
    closedir(dir);
    if (detail) *detail = found ? ("competing writer: " + found_name) : "";
    return found;
}
#endif

}  // namespace time
}  // namespace tbox
