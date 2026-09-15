#pragma once

#include <cstdint>

/// Snapshot of the process-wide file-descriptor budget.
///
/// POSIX counts sockets, Qt thread wake-up pipes, files and library internals
/// against the same RLIMIT_NOFILE ceiling.  A value of -1 means the platform
/// cannot report that part of the snapshot reliably.
struct DescriptorSnapshot {
    int64_t softLimit = -1;
    int64_t openCount = -1;
};

/// Keep room for the host, Qt, logs, DNS/TLS and module lifecycle work.  The
/// reserve scales with a small process but is capped so a large server limit
/// does not strand thousands of otherwise usable descriptors.
int64_t descriptorReserve(int64_t softLimit);

/// Number of new operations that can safely be admitted, or -1 when the OS
/// budget is unknown. `descriptorsPerOperation` must be positive.
int64_t descriptorOperationCapacity(const DescriptorSnapshot& snapshot,
                                    int64_t descriptorsPerOperation);

/// Query the live process. Linux uses /proc/self/fd, macOS uses proc_pidinfo,
/// and Windows reports process handles (but no comparable hard socket limit,
/// so capacity remains unknown there).
DescriptorSnapshot currentProcessDescriptorSnapshot() noexcept;
