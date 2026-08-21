#pragma once
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

/// Which thread first called `fn` (default-constructed id if never called).
std::thread::id mockThreadOf(const std::string& fn);

/// Every mocked C entry point, in call order.
std::vector<std::string> mockCallOrder();

/// Clear the ledger and any queued completions between tests.
void mockReset();

/// Completions queued but not yet drained by processVerifProxyTasks.
size_t mockPendingCompletions();

/// Status sentinel meaning "this call never completes" — used to test the
/// timeout path and the joint-ownership CallBox under ASan.
constexpr int mockNeverCompletes() { return 0xDEAD; }
