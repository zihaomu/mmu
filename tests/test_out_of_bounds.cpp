#include <gtest/gtest.h>

#include "dsf_mmu/mmu_api.h"

namespace {

TEST(test_out_of_bounds, ResolveUnallocatedLogicalBlockReturnsError) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 16;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);
    ASSERT_NE(mmu, nullptr);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_NE(ctx, nullptr);

    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 3), MMU_STATUS_OK);

    physical_route_t route{};
    EXPECT_EQ(mmu_resolve_routing(mmu, ctx, 10, &route), MMU_ERR_UNALLOCATED);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

}  // namespace
