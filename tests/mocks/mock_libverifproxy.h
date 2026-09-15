#pragma once
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

/// Which thread first called `fn` (default-constructed id if never called).
std::thread::id mockThreadOf(const std::string& fn);

/// Every mocked C entry point, in call order.
std::vector<std::string> mockCallOrder();

/// RPC method names handed to proxyCall, in dispatch order.
std::vector<std::string> mockProxyMethods();

/// Clear the ledger and any queued completions between tests.
void mockReset();

/// Completions queued but not yet drained by processVerifProxyTasks.
size_t mockPendingCompletions();

/// Pause/resume completion delivery while still allowing the pump to run.
/// This makes admission-queue tests deterministic without manufacturing a
/// permanently leaked call.
void mockHoldCompletions(bool hold);

/// The params JSON handed to the last proxyCall for `method`, verbatim.
std::string mockParamsOf(const std::string& method);

/// Status sentinel meaning "this call never completes" — used to test the
/// timeout path and the joint-ownership CallBox under ASan.
constexpr int mockNeverCompletes() { return 0xDEAD; }
