#include <chrono>

#include <gtest/gtest.h>

#include "dsf_mmu/mmu_api.h"

namespace {

TEST(Phase2Fork, test_sys_fork_zero_copy) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 32;
    cfg.block_size_bytes = 128;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);
    ASSERT_NE(mmu, nullptr);

    ctx_handle_t ctx_a = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx_a), MMU_STATUS_OK);
    ASSERT_NE(ctx_a, nullptr);

    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx_a, 5), MMU_STATUS_OK);
    EXPECT_EQ(mmu_get_logical_block_count(mmu, ctx_a), 5u);

    const uint32_t free_before_fork = mmu_get_free_block_count(mmu);

    ctx_handle_t ctx_b = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_EQ(mmu_fork_context(mmu, ctx_a, &ctx_b), MMU_STATUS_OK);
    const auto t1 = std::chrono::steady_clock::now();
    ASSERT_NE(ctx_b, nullptr);

    const auto fork_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    EXPECT_LT(fork_us, 50'000);  // Soft upper-bound; zero-copy fork should be very fast.

    EXPECT_EQ(mmu_get_logical_block_count(mmu, ctx_b), 5u);
    EXPECT_EQ(mmu_get_free_block_count(mmu), free_before_fork)
        << "Zero-copy fork should not allocate new physical blocks.";

    for (uint32_t lvid = 0; lvid < 5; ++lvid) {
        physical_route_t route_a{};
        physical_route_t route_b{};

        ASSERT_EQ(mmu_resolve_routing(mmu, ctx_a, lvid, &route_a), MMU_STATUS_OK);
        ASSERT_EQ(mmu_resolve_routing(mmu, ctx_b, lvid, &route_b), MMU_STATUS_OK);

        EXPECT_EQ(route_a.physical_block_id, route_b.physical_block_id);
        EXPECT_EQ(route_a.tier, route_b.tier);
        EXPECT_EQ(mmu_get_ref_count(mmu, route_a.physical_block_id), 2);
    }

    mmu_destroy_context(mmu, ctx_b);
    mmu_destroy_context(mmu, ctx_a);
    mmu_shutdown(mmu);
}

}  // namespace
