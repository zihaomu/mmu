#include <gtest/gtest.h>

#include "dsf_mmu/mmu_api.h"

namespace {

TEST(Phase1Fork, ForkSharesBlocksAndCowKeepsParentStable) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 16;
    cfg.block_size_bytes = 128;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);
    ASSERT_NE(mmu, nullptr);

    ctx_handle_t ctx_a = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx_a), MMU_STATUS_OK);
    ASSERT_NE(ctx_a, nullptr);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx_a, 5), MMU_STATUS_OK);

    ctx_handle_t ctx_b = nullptr;
    ASSERT_EQ(mmu_fork_context(mmu, ctx_a, &ctx_b), MMU_STATUS_OK);
    ASSERT_NE(ctx_b, nullptr);

    for (uint32_t lvid = 0; lvid < 5; ++lvid) {
        physical_route_t route_a{};
        physical_route_t route_b{};
        ASSERT_EQ(mmu_resolve_routing(mmu, ctx_a, lvid, &route_a), MMU_STATUS_OK);
        ASSERT_EQ(mmu_resolve_routing(mmu, ctx_b, lvid, &route_b), MMU_STATUS_OK);

        EXPECT_EQ(route_a.physical_block_id, route_b.physical_block_id);
        EXPECT_EQ(mmu_get_ref_count(mmu, route_a.physical_block_id), 2);
    }

    physical_route_t old_tail_a{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_a, 4, &old_tail_a), MMU_STATUS_OK);

    int32_t new_tail_b = -1;
    ASSERT_EQ(mmu_prepare_block_for_write(mmu, ctx_b, 4, &new_tail_b), MMU_STATUS_OK);

    physical_route_t tail_b{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_b, 4, &tail_b), MMU_STATUS_OK);

    EXPECT_NE(new_tail_b, old_tail_a.physical_block_id);
    EXPECT_EQ(tail_b.physical_block_id, new_tail_b);
    EXPECT_EQ(mmu_get_ref_count(mmu, old_tail_a.physical_block_id), 1);
    EXPECT_EQ(mmu_get_ref_count(mmu, new_tail_b), 1);

    physical_route_t tail_a_after{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_a, 4, &tail_a_after), MMU_STATUS_OK);
    EXPECT_EQ(tail_a_after.physical_block_id, old_tail_a.physical_block_id);

    mmu_destroy_context(mmu, ctx_b);
    mmu_destroy_context(mmu, ctx_a);
    EXPECT_EQ(mmu_get_free_block_count(mmu), cfg.l1_block_count);

    mmu_shutdown(mmu);
}

}  // namespace
