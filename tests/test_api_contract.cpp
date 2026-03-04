#include <gtest/gtest.h>

#include "dsf_mmu/mmu_api.h"

namespace {

TEST(ApiContract, InitRejectsConfigVersionOrSizeMismatch) {
    mmu_config_t cfg = mmu_default_config();

    mmu_handle_t mmu = nullptr;
    cfg.api_version = MMU_API_VERSION + 1;
    EXPECT_EQ(mmu_init(&cfg, &mmu), MMU_ERR_VERSION_MISMATCH);
    EXPECT_EQ(mmu, nullptr);

    cfg = mmu_default_config();
    cfg.size = 0;
    EXPECT_EQ(mmu_init(&cfg, &mmu), MMU_ERR_VERSION_MISMATCH);
    EXPECT_EQ(mmu, nullptr);
}

TEST(ApiContract, RejectsBadOrCrossOwnerContextHandle) {
    mmu_config_t cfg = mmu_default_config();

    mmu_handle_t mmu_a = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu_a), MMU_STATUS_OK);
    ASSERT_NE(mmu_a, nullptr);

    mmu_handle_t mmu_b = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu_b), MMU_STATUS_OK);
    ASSERT_NE(mmu_b, nullptr);

    ctx_handle_t ctx_a = nullptr;
    ASSERT_EQ(mmu_create_context(mmu_a, &ctx_a), MMU_STATUS_OK);
    ASSERT_NE(ctx_a, nullptr);

    EXPECT_EQ(mmu_allocate_blocks(mmu_b, ctx_a, 1), MMU_ERR_BAD_HANDLE);

    mmu_destroy_context(mmu_a, ctx_a);
    EXPECT_EQ(mmu_allocate_blocks(mmu_a, ctx_a, 1), MMU_ERR_BAD_HANDLE);

    mmu_shutdown(mmu_b);
    mmu_shutdown(mmu_a);
}

}  // namespace
