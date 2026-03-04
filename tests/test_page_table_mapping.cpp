#include <array>

#include <gtest/gtest.h>

#include "dsf_mmu/mmu_api.h"
#include "internal/mmu_test_api.h"

namespace {

TEST(test_page_table_mapping, ResolveReturnsExpectedPhysicalRoute) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 32;
    cfg.block_size_bytes = 256;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);
    ASSERT_NE(mmu, nullptr);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_NE(ctx, nullptr);

    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 3), MMU_STATUS_OK);
    EXPECT_EQ(mmu_get_free_block_count(mmu), 29u);

    // Remap to PBID 8,2,5 as requested by test target.
    ASSERT_EQ(mmu_test_update_mapping(mmu, ctx, 2, 5, MMU_TIER_L1), MMU_STATUS_OK);
    ASSERT_EQ(mmu_test_update_mapping(mmu, ctx, 0, 8, MMU_TIER_L1), MMU_STATUS_OK);
    ASSERT_EQ(mmu_test_update_mapping(mmu, ctx, 1, 2, MMU_TIER_L1), MMU_STATUS_OK);

    const std::array<int, 3> expected_pbids = {8, 2, 5};

    for (uint32_t lvid = 0; lvid < 3; ++lvid) {
        physical_route_t route{};
        ASSERT_EQ(mmu_resolve_routing(mmu, ctx, lvid, &route), MMU_STATUS_OK);
        EXPECT_EQ(route.tier, MMU_TIER_L1);
        EXPECT_EQ(route.physical_block_id, expected_pbids[lvid]);
        EXPECT_EQ(route.byte_offset, static_cast<uint64_t>(expected_pbids[lvid]) * cfg.block_size_bytes);
    }

    std::array<uint32_t, 3> logical_blocks = {0u, 1u, 2u};
    std::array<physical_route_t, 3> routes{};
    ASSERT_EQ(mmu_resolve_routing_batch(mmu, ctx, logical_blocks.data(), logical_blocks.size(), routes.data()),
              MMU_STATUS_OK);
    EXPECT_EQ(routes[0].physical_block_id, 8);
    EXPECT_EQ(routes[1].physical_block_id, 2);
    EXPECT_EQ(routes[2].physical_block_id, 5);

    EXPECT_EQ(mmu_get_free_block_count(mmu), 29u);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

}  // namespace
