#include <cstdint>

#include <gtest/gtest.h>

#include "dsf_mmu/mmu_api.h"

namespace {

TEST(Phase2Fork, test_cow_trigger) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 16;
    cfg.l1_resident_block_limit = 16;
    cfg.l2_block_count = 16;
    cfg.block_size_bytes = 64;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx_a = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx_a), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx_a, 5), MMU_STATUS_OK);

    mmu_mapped_block_t mapped_a_write{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx_a, 4, MMU_MAP_READ | MMU_MAP_WRITE, &mapped_a_write),
              MMU_STATUS_OK);
    auto* a_init = static_cast<uint8_t*>(mapped_a_write.host_ptr);
    for (uint32_t i = 0; i < mapped_a_write.size_bytes; ++i) {
        a_init[i] = static_cast<uint8_t>(i & 0xFFu);
    }
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_a, &mapped_a_write), MMU_STATUS_OK);

    physical_route_t route_a_before{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_a, 4, &route_a_before), MMU_STATUS_OK);

    ctx_handle_t ctx_b = nullptr;
    ASSERT_EQ(mmu_fork_context(mmu, ctx_a, &ctx_b), MMU_STATUS_OK);

    physical_route_t route_b_before{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_b, 4, &route_b_before), MMU_STATUS_OK);
    EXPECT_EQ(route_b_before.physical_block_id, route_a_before.physical_block_id);
    EXPECT_EQ(mmu_get_ref_count(mmu, route_a_before.physical_block_id), 2);

    // Trigger CoW by writing only one byte in B.
    mmu_mapped_block_t mapped_b_write{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx_b, 4, MMU_MAP_READ | MMU_MAP_WRITE, &mapped_b_write),
              MMU_STATUS_OK);
    auto* b_ptr = static_cast<uint8_t*>(mapped_b_write.host_ptr);
    b_ptr[0] = 0xEE;
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_b, &mapped_b_write), MMU_STATUS_OK);

    physical_route_t route_a_after{};
    physical_route_t route_b_after{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_a, 4, &route_a_after), MMU_STATUS_OK);
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_b, 4, &route_b_after), MMU_STATUS_OK);

    // 1) CoW should allocate a new PBID for B.
    EXPECT_NE(route_b_after.physical_block_id, route_a_before.physical_block_id);
    // 2) B page table should point to the new PBID.
    EXPECT_NE(route_b_after.physical_block_id, route_a_after.physical_block_id);
    // 3) Old shared PBID ref count should drop to 1.
    EXPECT_EQ(mmu_get_ref_count(mmu, route_a_before.physical_block_id), 1);
    EXPECT_EQ(mmu_get_ref_count(mmu, route_b_after.physical_block_id), 1);

    mmu_mapped_block_t mapped_a_read{};
    mmu_mapped_block_t mapped_b_read{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx_a, 4, MMU_MAP_READ, &mapped_a_read), MMU_STATUS_OK);
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx_b, 4, MMU_MAP_READ, &mapped_b_read), MMU_STATUS_OK);

    const auto* a_read = static_cast<const uint8_t*>(mapped_a_read.host_ptr);
    const auto* b_read = static_cast<const uint8_t*>(mapped_b_read.host_ptr);

    // 4) Data copy assertion: unchanged bytes in B should remain equal to A's pre-write data.
    EXPECT_EQ(a_read[0], static_cast<uint8_t>(0));
    EXPECT_EQ(a_read[17], static_cast<uint8_t>(17));
    EXPECT_EQ(b_read[0], static_cast<uint8_t>(0xEE));
    EXPECT_EQ(b_read[17], static_cast<uint8_t>(17));

    // Missing assertion补充: parent data remains stable after child CoW write.
    EXPECT_EQ(a_read[0], static_cast<uint8_t>(0));

    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_b, &mapped_b_read), MMU_STATUS_OK);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_a, &mapped_a_read), MMU_STATUS_OK);

    mmu_destroy_context(mmu, ctx_b);
    mmu_destroy_context(mmu, ctx_a);
    mmu_shutdown(mmu);
}

}  // namespace
