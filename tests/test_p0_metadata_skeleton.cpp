#include <gtest/gtest.h>

#include "core/block_directory.h"
#include "dsf_mmu/mmu_api.h"
#include "internal/mmu_test_api.h"

namespace {

TEST(P0BlockDirectory, FreshBlockStartsWithStableDefaults) {
    dsf_mmu::core::BlockDirectory directory(4);

    ASSERT_TRUE(directory.initialize_fresh_block(2));

    dsf_mmu::core::BlockMeta meta{};
    ASSERT_TRUE(directory.snapshot(2, &meta));
    EXPECT_TRUE(meta.allocated);
    EXPECT_EQ(meta.tier, MMU_TIER_L1);
    EXPECT_FALSE(meta.resident_in_l1);
    EXPECT_FALSE(meta.dirty);
    EXPECT_EQ(meta.pin_count, 0u);
    EXPECT_EQ(meta.l1_frame_id, dsf_mmu::core::kInvalidFrameId);
    EXPECT_EQ(meta.l2_slot_id, dsf_mmu::core::kInvalidSlotId);
    EXPECT_FALSE(meta.has_l2_backing);
}

TEST(P0BlockDirectory, ResetClearsGlobalMetadata) {
    dsf_mmu::core::BlockDirectory directory(2);

    ASSERT_TRUE(directory.initialize_fresh_block(1));
    ASSERT_TRUE(directory.set_tier(1, MMU_TIER_L2));
    ASSERT_TRUE(directory.set_resident_in_l1(1, true));
    ASSERT_TRUE(directory.increment_pin_count(1));
    ASSERT_TRUE(directory.set_dirty(1, true));
    ASSERT_TRUE(directory.set_l1_frame_id(1, 7));
    ASSERT_TRUE(directory.set_l2_slot_id(1, 9));
    ASSERT_TRUE(directory.set_has_l2_backing(1, true));

    directory.reset(1);

    dsf_mmu::core::BlockMeta meta{};
    ASSERT_TRUE(directory.snapshot(1, &meta));
    EXPECT_FALSE(meta.allocated);
    EXPECT_EQ(meta.pin_count, 0u);
    EXPECT_FALSE(meta.resident_in_l1);
    EXPECT_FALSE(meta.dirty);
    EXPECT_EQ(meta.l1_frame_id, dsf_mmu::core::kInvalidFrameId);
    EXPECT_EQ(meta.l2_slot_id, dsf_mmu::core::kInvalidSlotId);
    EXPECT_FALSE(meta.has_l2_backing);
}

TEST(P0RuntimeMetadata, PinCountFlowsThroughGlobalDirectory) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 8;
    cfg.l1_resident_block_limit = 8;
    cfg.block_size_bytes = 64;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 1), MMU_STATUS_OK);

    physical_route_t route{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx, 0, &route), MMU_STATUS_OK);

    mmu_test_block_metadata_t meta{};
    ASSERT_EQ(mmu_test_get_block_metadata(mmu, route.physical_block_id, &meta), MMU_STATUS_OK);
    EXPECT_EQ(meta.pin_count, 0u);

    mmu_mapped_block_t mapped{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 0, MMU_MAP_READ | MMU_MAP_WRITE, &mapped), MMU_STATUS_OK);

    ASSERT_EQ(mmu_test_get_block_metadata(mmu, route.physical_block_id, &meta), MMU_STATUS_OK);
    EXPECT_EQ(meta.pin_count, 1u);
    EXPECT_TRUE(meta.resident_in_l1);

    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &mapped), MMU_STATUS_OK);

    ASSERT_EQ(mmu_test_get_block_metadata(mmu, route.physical_block_id, &meta), MMU_STATUS_OK);
    EXPECT_EQ(meta.pin_count, 0u);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

TEST(P0RuntimeMetadata, SharedTierResolvesFromGlobalBlockDirectory) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 4;
    cfg.l1_resident_block_limit = 1;
    cfg.l2_block_count = 4;
    cfg.block_size_bytes = 64;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx_a = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx_a), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx_a, 2), MMU_STATUS_OK);

    mmu_mapped_block_t block0{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx_a, 0, MMU_MAP_READ | MMU_MAP_WRITE, &block0), MMU_STATUS_OK);
    static_cast<uint8_t*>(block0.host_ptr)[0] = 0x11;
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_a, &block0), MMU_STATUS_OK);

    ctx_handle_t ctx_b = nullptr;
    ASSERT_EQ(mmu_fork_context(mmu, ctx_a, &ctx_b), MMU_STATUS_OK);

    mmu_mapped_block_t block1{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx_a, 1, MMU_MAP_READ | MMU_MAP_WRITE, &block1), MMU_STATUS_OK);
    static_cast<uint8_t*>(block1.host_ptr)[0] = 0x22;
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_a, &block1), MMU_STATUS_OK);

    physical_route_t route_a{};
    physical_route_t route_b{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_a, 0, &route_a), MMU_STATUS_OK);
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_b, 0, &route_b), MMU_STATUS_OK);
    EXPECT_EQ(route_a.physical_block_id, route_b.physical_block_id);
    EXPECT_EQ(route_a.tier, MMU_TIER_L2);
    EXPECT_EQ(route_b.tier, MMU_TIER_L2);

    mmu_test_block_metadata_t meta{};
    ASSERT_EQ(mmu_test_get_block_metadata(mmu, route_a.physical_block_id, &meta), MMU_STATUS_OK);
    EXPECT_EQ(meta.tier, MMU_TIER_L2);
    EXPECT_FALSE(meta.resident_in_l1);

    mmu_destroy_context(mmu, ctx_b);
    mmu_destroy_context(mmu, ctx_a);
    mmu_shutdown(mmu);
}

TEST(P0RuntimeMetadata, ReleasingLastReferenceResetsBlockDirectoryEntry) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 8;
    cfg.l1_resident_block_limit = 8;
    cfg.block_size_bytes = 64;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 1), MMU_STATUS_OK);

    physical_route_t route{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx, 0, &route), MMU_STATUS_OK);

    ASSERT_EQ(mmu_release_logical_block(mmu, ctx, 0), MMU_STATUS_OK);

    mmu_test_block_metadata_t meta{};
    ASSERT_EQ(mmu_test_get_block_metadata(mmu, route.physical_block_id, &meta), MMU_STATUS_OK);
    EXPECT_FALSE(meta.allocated);
    EXPECT_EQ(meta.pin_count, 0u);
    EXPECT_FALSE(meta.resident_in_l1);
    EXPECT_FALSE(meta.has_l2_backing);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

}  // namespace
