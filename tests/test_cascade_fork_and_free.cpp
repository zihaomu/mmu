#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "dsf_mmu/mmu_api.h"

namespace {

TEST(Phase2Fork, test_cascade_fork_and_free) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 32;
    cfg.l1_resident_block_limit = 32;
    cfg.l2_block_count = 32;
    cfg.block_size_bytes = 64;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    ctx_handle_t ctx_a = nullptr;
    ASSERT_EQ(mmu_create_context(mmu, &ctx_a), MMU_STATUS_OK);
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx_a, 5), MMU_STATUS_OK);

    // Initialize A's 5 blocks with deterministic data.
    for (uint32_t lvid = 0; lvid < 5; ++lvid) {
        mmu_mapped_block_t mapped{};
        ASSERT_EQ(mmu_map_logical_block(mmu, ctx_a, lvid, MMU_MAP_READ | MMU_MAP_WRITE, &mapped), MMU_STATUS_OK);
        auto* p = static_cast<uint8_t*>(mapped.host_ptr);
        p[0] = static_cast<uint8_t>(0x10u + lvid);
        p[1] = static_cast<uint8_t>(0x20u + lvid);
        ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_a, &mapped), MMU_STATUS_OK);
    }

    ctx_handle_t ctx_b = nullptr;
    ASSERT_EQ(mmu_fork_context(mmu, ctx_a, &ctx_b), MMU_STATUS_OK);

    ctx_handle_t ctx_c = nullptr;
    ASSERT_EQ(mmu_fork_context(mmu, ctx_b, &ctx_c), MMU_STATUS_OK);

    std::array<int32_t, 5> shared_pbid{};
    for (uint32_t lvid = 0; lvid < 5; ++lvid) {
        physical_route_t ra{};
        physical_route_t rb{};
        physical_route_t rc{};
        ASSERT_EQ(mmu_resolve_routing(mmu, ctx_a, lvid, &ra), MMU_STATUS_OK);
        ASSERT_EQ(mmu_resolve_routing(mmu, ctx_b, lvid, &rb), MMU_STATUS_OK);
        ASSERT_EQ(mmu_resolve_routing(mmu, ctx_c, lvid, &rc), MMU_STATUS_OK);

        EXPECT_EQ(ra.physical_block_id, rb.physical_block_id);
        EXPECT_EQ(ra.physical_block_id, rc.physical_block_id);
        EXPECT_EQ(mmu_get_ref_count(mmu, ra.physical_block_id), 3);
        shared_pbid[lvid] = ra.physical_block_id;
    }

    const uint32_t free_before_b_exclusive = mmu_get_free_block_count(mmu);

    // B-only CoW on tail block (lvid=4), creates an exclusive block for B.
    mmu_mapped_block_t b_tail_write{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx_b, 4, MMU_MAP_READ | MMU_MAP_WRITE, &b_tail_write), MMU_STATUS_OK);
    static_cast<uint8_t*>(b_tail_write.host_ptr)[0] = 0xEE;
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_b, &b_tail_write), MMU_STATUS_OK);

    physical_route_t rb_tail_after{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_b, 4, &rb_tail_after), MMU_STATUS_OK);
    EXPECT_NE(rb_tail_after.physical_block_id, shared_pbid[4]);
    EXPECT_EQ(mmu_get_ref_count(mmu, rb_tail_after.physical_block_id), 1);
    EXPECT_EQ(mmu_get_ref_count(mmu, shared_pbid[4]), 2);  // now shared by A/C only

    // B-only appended block (lvid=5), also exclusive to B.
    ASSERT_EQ(mmu_allocate_blocks(mmu, ctx_b, 1), MMU_STATUS_OK);
    EXPECT_EQ(mmu_get_logical_block_count(mmu, ctx_b), 6u);
    EXPECT_EQ(mmu_get_logical_block_count(mmu, ctx_a), 5u);
    EXPECT_EQ(mmu_get_logical_block_count(mmu, ctx_c), 5u);

    physical_route_t rb_extra{};
    ASSERT_EQ(mmu_resolve_routing(mmu, ctx_b, 5, &rb_extra), MMU_STATUS_OK);
    EXPECT_EQ(mmu_get_ref_count(mmu, rb_extra.physical_block_id), 1);

    EXPECT_EQ(mmu_get_free_block_count(mmu), free_before_b_exclusive - 2u);

    // Destroy middle branch B.
    mmu_destroy_context(mmu, ctx_b);

    // A/C should remain valid and still share their 5 logical blocks.
    for (uint32_t lvid = 0; lvid < 5; ++lvid) {
        physical_route_t ra{};
        physical_route_t rc{};
        ASSERT_EQ(mmu_resolve_routing(mmu, ctx_a, lvid, &ra), MMU_STATUS_OK);
        ASSERT_EQ(mmu_resolve_routing(mmu, ctx_c, lvid, &rc), MMU_STATUS_OK);
        EXPECT_EQ(ra.physical_block_id, rc.physical_block_id);
    }

    // Shared refcounts after B destruction:
    // lvid 0..3 were shared by A/B/C => now A/C, so refcount should be 2.
    for (uint32_t lvid = 0; lvid < 4; ++lvid) {
        EXPECT_EQ(mmu_get_ref_count(mmu, shared_pbid[lvid]), 2);
    }
    // lvid 4 old shared block was already A/C only after B CoW => remains 2.
    EXPECT_EQ(mmu_get_ref_count(mmu, shared_pbid[4]), 2);

    // B-exclusive blocks should be released.
    EXPECT_EQ(mmu_get_ref_count(mmu, rb_tail_after.physical_block_id), 0);
    EXPECT_EQ(mmu_get_ref_count(mmu, rb_extra.physical_block_id), 0);

    // Data integrity for A/C: B's write must not leak.
    mmu_mapped_block_t a_tail_read{};
    mmu_mapped_block_t c_tail_read{};
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx_a, 4, MMU_MAP_READ, &a_tail_read), MMU_STATUS_OK);
    ASSERT_EQ(mmu_map_logical_block(mmu, ctx_c, 4, MMU_MAP_READ, &c_tail_read), MMU_STATUS_OK);
    const auto* pa = static_cast<const uint8_t*>(a_tail_read.host_ptr);
    const auto* pc = static_cast<const uint8_t*>(c_tail_read.host_ptr);
    EXPECT_EQ(pa[0], static_cast<uint8_t>(0x14));
    EXPECT_EQ(pa[1], static_cast<uint8_t>(0x24));
    EXPECT_EQ(pc[0], static_cast<uint8_t>(0x14));
    EXPECT_EQ(pc[1], static_cast<uint8_t>(0x24));
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_c, &c_tail_read), MMU_STATUS_OK);
    ASSERT_EQ(mmu_unmap_logical_block(mmu, ctx_a, &a_tail_read), MMU_STATUS_OK);

    // Only A/C shared 5 PBIDs remain allocated.
    EXPECT_EQ(mmu_get_free_block_count(mmu), cfg.l1_block_count - 5u);

    mmu_destroy_context(mmu, ctx_c);
    mmu_destroy_context(mmu, ctx_a);
    EXPECT_EQ(mmu_get_free_block_count(mmu), cfg.l1_block_count);

    mmu_shutdown(mmu);
}

}  // namespace
