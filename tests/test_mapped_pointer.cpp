#include <cstdint>

#include <gtest/gtest.h>

#include "dsf_mmu/mmu_api.h"
#include "internal/mmu_test_api.h"

namespace {

TEST(MappedPointer, MapWriteAndReadBackThroughPointer) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 8;
    cfg.l1_resident_block_limit = 8;
    cfg.block_size_bytes = 64;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 1), MMU_STATUS_OK);

    mmu_mapped_block_t mapped{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 0, MMU_MAP_READ | MMU_MAP_WRITE, &mapped), MMU_STATUS_OK);
    ASSERT_NE(mapped.host_ptr, nullptr);
    ASSERT_EQ(mapped.size_bytes, cfg.block_size_bytes);

    auto* bytes = static_cast<uint8_t*>(mapped.host_ptr);
    for (uint32_t i = 0; i < mapped.size_bytes; ++i) {
        bytes[i] = static_cast<uint8_t>(i);
    }

    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &mapped), MMU_STATUS_OK);

    mmu_mapped_block_t mapped_read{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 0, MMU_MAP_READ, &mapped_read), MMU_STATUS_OK);
    const auto* read_bytes = static_cast<const uint8_t*>(mapped_read.host_ptr);
    for (uint32_t i = 0; i < mapped_read.size_bytes; ++i) {
        EXPECT_EQ(read_bytes[i], static_cast<uint8_t>(i));
    }
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &mapped_read), MMU_STATUS_OK);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

TEST(MappedPointer, LazyCommitAllocatesAndReclaimsPagesOnDemand) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 8;
    cfg.l1_resident_block_limit = 8;
    cfg.block_size_bytes = 64;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 1), MMU_STATUS_OK);

    uint32_t l1_pages = 0;
    uint32_t l2_pages = 0;
    ASSERT_EQ(mmu_test_get_committed_page_counts(mmu, &l1_pages, &l2_pages), MMU_STATUS_OK);
    EXPECT_EQ(l1_pages, 0u);
    EXPECT_EQ(l2_pages, 0u);

    mmu_mapped_block_t mapped_read{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 0, MMU_MAP_READ, &mapped_read), MMU_STATUS_OK);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &mapped_read), MMU_STATUS_OK);
    ASSERT_EQ(mmu_test_get_committed_page_counts(mmu, &l1_pages, &l2_pages), MMU_STATUS_OK);
    EXPECT_EQ(l1_pages, 1u);
    EXPECT_EQ(l2_pages, 0u);

    mmu_mapped_block_t mapped_write{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 0, MMU_MAP_WRITE, &mapped_write), MMU_STATUS_OK);
    static_cast<uint8_t*>(mapped_write.host_ptr)[0] = 0x7F;
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &mapped_write), MMU_STATUS_OK);
    ASSERT_EQ(mmu_test_get_committed_page_counts(mmu, &l1_pages, &l2_pages), MMU_STATUS_OK);
    EXPECT_EQ(l1_pages, 1u);
    EXPECT_EQ(l2_pages, 1u);

    ASSERT_EQ(mmu_release_logical_block(mmu, ctx, 0), MMU_STATUS_OK);
    ASSERT_EQ(mmu_test_get_committed_page_counts(mmu, &l1_pages, &l2_pages), MMU_STATUS_OK);
    EXPECT_EQ(l1_pages, 0u);
    EXPECT_EQ(l2_pages, 0u);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

TEST(MappedPointer, LazyCommitAllocatesMemoryByChunk) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 8;
    cfg.l1_resident_block_limit = 8;
    cfg.block_size_bytes = 64;
    cfg.chunk_block_count = 2;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 3), MMU_STATUS_OK);

    uint32_t l1_chunks = 0;
    uint32_t l2_chunks = 0;
    ASSERT_EQ(mmu_test_get_committed_chunk_counts(mmu, &l1_chunks, &l2_chunks), MMU_STATUS_OK);
    EXPECT_EQ(l1_chunks, 0u);
    EXPECT_EQ(l2_chunks, 0u);

    mmu_mapped_block_t block0{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 0, MMU_MAP_READ, &block0), MMU_STATUS_OK);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &block0), MMU_STATUS_OK);
    ASSERT_EQ(mmu_test_get_committed_chunk_counts(mmu, &l1_chunks, &l2_chunks), MMU_STATUS_OK);
    EXPECT_EQ(l1_chunks, 1u);
    EXPECT_EQ(l2_chunks, 0u);

    mmu_mapped_block_t block1{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 1, MMU_MAP_READ, &block1), MMU_STATUS_OK);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &block1), MMU_STATUS_OK);
    ASSERT_EQ(mmu_test_get_committed_chunk_counts(mmu, &l1_chunks, &l2_chunks), MMU_STATUS_OK);
    EXPECT_EQ(l1_chunks, 1u);  // still same chunk

    mmu_mapped_block_t block2{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 2, MMU_MAP_READ, &block2), MMU_STATUS_OK);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &block2), MMU_STATUS_OK);
    ASSERT_EQ(mmu_test_get_committed_chunk_counts(mmu, &l1_chunks, &l2_chunks), MMU_STATUS_OK);
    EXPECT_EQ(l1_chunks, 2u);  // next chunk is allocated here

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

TEST(MappedPointer, EnforcesL2BlockUpperBound) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 2;
    cfg.l1_resident_block_limit = 1;
    cfg.l2_block_count = 1;
    cfg.block_size_bytes = 32;
    cfg.chunk_block_count = 1;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 2), MMU_STATUS_OK);

    mmu_mapped_block_t block0{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 0, MMU_MAP_READ | MMU_MAP_WRITE, &block0), MMU_STATUS_OK);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &block0), MMU_STATUS_OK);

    mmu_mapped_block_t block1{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 1, MMU_MAP_READ | MMU_MAP_WRITE, &block1), MMU_STATUS_OK);
    EXPECT_EQ(mmu_unmap_logical_block(mmu, ctx, &block1), MMU_ERR_OUT_OF_MEMORY);

    uint32_t l1_pages = 0;
    uint32_t l2_pages = 0;
    ASSERT_EQ(mmu_test_get_committed_page_counts(mmu, &l1_pages, &l2_pages), MMU_STATUS_OK);
    EXPECT_EQ(l2_pages, 1u);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

TEST(MappedPointer, MapAutomaticallySwapsBetweenL1AndL2) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 4;
    cfg.l1_resident_block_limit = 1;
    cfg.block_size_bytes = 32;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 2), MMU_STATUS_OK);

    mmu_mapped_block_t block0{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 0, MMU_MAP_READ | MMU_MAP_WRITE, &block0), MMU_STATUS_OK);
    static_cast<uint8_t*>(block0.host_ptr)[0] = 0xAA;
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &block0), MMU_STATUS_OK);

    mmu_mapped_block_t block1{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 1, MMU_MAP_READ | MMU_MAP_WRITE, &block1), MMU_STATUS_OK);
    static_cast<uint8_t*>(block1.host_ptr)[0] = 0xBB;
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &block1), MMU_STATUS_OK);

    physical_route_t route0{};
    physical_route_t route1{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx, 0, &route0), MMU_STATUS_OK);
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx, 1, &route1), MMU_STATUS_OK);
    EXPECT_EQ(route0.tier, MMU_TIER_L2);
    EXPECT_EQ(route1.tier, MMU_TIER_L1);

    mmu_mapped_block_t block0_read{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 0, MMU_MAP_READ, &block0_read), MMU_STATUS_OK);
    EXPECT_EQ(static_cast<const uint8_t*>(block0_read.host_ptr)[0], 0xAA);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &block0_read), MMU_STATUS_OK);

    ASSERT_EQ(mmu_resolve_routing(mmu, ctx, 0, &route0), MMU_STATUS_OK);
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx, 1, &route1), MMU_STATUS_OK);
    EXPECT_EQ(route0.tier, MMU_TIER_L1);
    EXPECT_EQ(route1.tier, MMU_TIER_L2);

    mmu_mapped_block_t block1_read{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 1, MMU_MAP_READ, &block1_read), MMU_STATUS_OK);
    EXPECT_EQ(static_cast<const uint8_t*>(block1_read.host_ptr)[0], 0xBB);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &block1_read), MMU_STATUS_OK);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

TEST(MappedPointer, BatchMapAndUnmapRoundTrip) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 8;
    cfg.l1_resident_block_limit = 8;
    cfg.block_size_bytes = 64;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 3), MMU_STATUS_OK);

    const uint32_t logical_blocks[3] = {0u, 1u, 2u};
    mmu_mapped_block_t mapped[3]{};
    ASSERT_EQ(mmu_map_logical_blocks(mmu, ctx, logical_blocks, 3, MMU_MAP_READ | MMU_MAP_WRITE, mapped),
              MMU_STATUS_OK);

    for (uint32_t i = 0; i < 3; ++i) {
        auto* p = static_cast<uint8_t*>(mapped[i].host_ptr);
        p[0] = static_cast<uint8_t>(0x10 + i);
        p[1] = static_cast<uint8_t>(0x20 + i);
    }

    ASSERT_EQ(mmu_unmap_logical_blocks(mmu, ctx, mapped, 3), MMU_STATUS_OK);

    mmu_mapped_block_t mapped_read[3]{};
    ASSERT_EQ(mmu_map_logical_blocks(mmu, ctx, logical_blocks, 3, MMU_MAP_READ, mapped_read), MMU_STATUS_OK);
    for (uint32_t i = 0; i < 3; ++i) {
        const auto* p = static_cast<const uint8_t*>(mapped_read[i].host_ptr);
        EXPECT_EQ(p[0], static_cast<uint8_t>(0x10 + i));
        EXPECT_EQ(p[1], static_cast<uint8_t>(0x20 + i));
    }
    ASSERT_EQ(mmu_unmap_logical_blocks(mmu, ctx, mapped_read, 3), MMU_STATUS_OK);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

TEST(MappedPointer, BatchMapRollbackOnErrorKeepsSystemUsable) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 4;
    cfg.l1_resident_block_limit = 1;
    cfg.block_size_bytes = 32;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx, 2), MMU_STATUS_OK);

    const uint32_t bad_logical_blocks[2] = {0u, 99u};
    mmu_mapped_block_t mapped_bad[2]{};
    EXPECT_EQ(mmu_map_logical_blocks(mmu, ctx, bad_logical_blocks, 2, MMU_MAP_READ, mapped_bad),
              MMU_ERR_UNALLOCATED);

    // If rollback works, previous partial map should be released and this call should still succeed.
    mmu_mapped_block_t single{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx, 1, MMU_MAP_READ, &single), MMU_STATUS_OK);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx, &single), MMU_STATUS_OK);

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
}

}  // namespace
