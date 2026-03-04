#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "core/block_manager.h"

namespace {

TEST(test_allocator_basic, ReuseFreedBlocksAndKeepAccurateFreeCount) {
    dsf_mmu::core::BlockManager allocator(100);

    std::vector<int> first_alloc;
    for (int i = 0; i < 10; ++i) {
        int pbid = -1;
        ASSERT_TRUE(allocator.allocate(&pbid));
        first_alloc.push_back(pbid);
    }

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(first_alloc[i], i);
    }
    EXPECT_EQ(allocator.free_blocks(), 90u);

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(allocator.release(first_alloc[i]));
    }
    EXPECT_EQ(allocator.free_blocks(), 95u);

    std::set<int> second_alloc;
    for (int i = 0; i < 5; ++i) {
        int pbid = -1;
        ASSERT_TRUE(allocator.allocate(&pbid));
        second_alloc.insert(pbid);
    }

    EXPECT_EQ(second_alloc, std::set<int>({0, 1, 2, 3, 4}));
    EXPECT_EQ(allocator.free_blocks(), 90u);
}

}  // namespace
