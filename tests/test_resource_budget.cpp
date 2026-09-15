#include <logos_test.h>

#include "resource_budget.h"

LOGOS_TEST(resource_budget_reserves_a_quarter_of_a_small_posix_limit) {
    LOGOS_ASSERT_EQ(descriptorReserve(256), 64);
    LOGOS_ASSERT_EQ(descriptorOperationCapacity({ 256, 36 }, 12), 13);
}

LOGOS_TEST(resource_budget_never_consumes_the_hosts_reserve) {
    LOGOS_ASSERT_EQ(descriptorOperationCapacity({ 256, 192 }, 12), 0);
    LOGOS_ASSERT_EQ(descriptorOperationCapacity({ 64, 48 }, 4), 0);
}

LOGOS_TEST(resource_budget_scales_and_caps_its_reserve) {
    LOGOS_ASSERT_EQ(descriptorReserve(64), 16);
    LOGOS_ASSERT_EQ(descriptorReserve(4096), 128);
}

LOGOS_TEST(resource_budget_reports_unknown_when_the_os_cannot_supply_a_limit) {
    LOGOS_ASSERT_EQ(descriptorOperationCapacity({ -1, 30 }, 12), -1);
    LOGOS_ASSERT_EQ(descriptorOperationCapacity({ 256, -1 }, 12), -1);
}

LOGOS_TEST(resource_budget_can_inspect_the_current_process) {
    const DescriptorSnapshot snapshot = currentProcessDescriptorSnapshot();
#if defined(_WIN32)
    LOGOS_ASSERT_GT(snapshot.openCount, 0);
#else
    LOGOS_ASSERT_GT(snapshot.softLimit, 0);
    LOGOS_ASSERT_GE(snapshot.openCount, 0);
    LOGOS_ASSERT_LT(snapshot.openCount, snapshot.softLimit);
#endif
}
