#include "resource_budget.h"

#include <algorithm>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#else
#include <dirent.h>
#include <sys/resource.h>
#endif

namespace {

int64_t posixSoftLimit() noexcept {
#if defined(_WIN32)
    return -1;
#else
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY)
        return -1;
    if (limit.rlim_cur > static_cast<rlim_t>(std::numeric_limits<int64_t>::max()))
        return -1;
    return static_cast<int64_t>(limit.rlim_cur);
#endif
}

} // namespace

int64_t descriptorReserve(int64_t softLimit) {
    if (softLimit <= 0) return 0;
    return std::min<int64_t>(128, std::max<int64_t>(16, softLimit / 4));
}

int64_t descriptorOperationCapacity(const DescriptorSnapshot& snapshot,
                                    int64_t descriptorsPerOperation) {
    if (snapshot.softLimit < 0 || snapshot.openCount < 0
        || descriptorsPerOperation <= 0)
        return -1;

    const int64_t available = snapshot.softLimit - snapshot.openCount
                            - descriptorReserve(snapshot.softLimit);
    return std::max<int64_t>(0, available / descriptorsPerOperation);
}

DescriptorSnapshot currentProcessDescriptorSnapshot() noexcept {
    DescriptorSnapshot snapshot;
    snapshot.softLimit = posixSoftLimit();

#if defined(_WIN32)
    DWORD count = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &count))
        snapshot.openCount = static_cast<int64_t>(count);
#elif defined(__APPLE__)
    // One record per descriptor is sufficient because no process can have
    // more open descriptors than its soft limit. Cap pathological/infinite
    // values before converting the byte count to the API's int parameter.
    const int64_t slots64 = snapshot.softLimit > 0
        ? std::min<int64_t>(snapshot.softLimit, 1'000'000)
        : 16'384;
    std::vector<proc_fdinfo> fds(static_cast<size_t>(slots64));
    const size_t bytes64 = fds.size() * sizeof(proc_fdinfo);
    const int bytes = static_cast<int>(std::min<size_t>(
        bytes64, static_cast<size_t>(std::numeric_limits<int>::max())));
    const int used = ::proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0,
                                    fds.data(), bytes);
    if (used >= 0)
        snapshot.openCount = used / static_cast<int>(sizeof(proc_fdinfo));
#else
    // Opening the directory adds one descriptor to the listing. Subtract it
    // so callers see the count from immediately before this query.
    if (DIR* dir = ::opendir("/proc/self/fd")) {
        int64_t count = 0;
        while (const dirent* entry = ::readdir(dir)) {
            if (entry->d_name[0] == '.'
                && (entry->d_name[1] == '\0'
                    || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
                continue;
            ++count;
        }
        ::closedir(dir);
        snapshot.openCount = std::max<int64_t>(0, count - 1);
    }
#endif

    return snapshot;
}
