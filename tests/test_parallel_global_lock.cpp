#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "dsf_mmu/mmu_api.h"

namespace {

TEST(ParallelGlobalLock, SharedMmuHandlesConcurrentContextRequests) {
    mmu_config_t cfg = mmu_default_config();
    cfg.l1_block_count = 32;
    cfg.l1_resident_block_limit = 32;
    cfg.l2_block_count = 32;
    cfg.block_size_bytes = 64;
    cfg.chunk_block_count = 4;

    mmu_handle_t mmu = nullptr;
    ASSERT_EQ(mmu_init(&cfg, &mmu), MMU_STATUS_OK);

    constexpr uint32_t kThreadCount = 8;
    constexpr uint32_t kIters = 200;

    std::vector<ctx_handle_t> contexts(kThreadCount, nullptr);
    for (uint32_t i = 0; i < kThreadCount; ++i) {
        ASSERT_EQ(mmu_create_context(mmu, &contexts[i]), MMU_STATUS_OK);
        ASSERT_EQ(mmu_allocate_blocks(mmu, contexts[i], 1), MMU_STATUS_OK);
    }

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::atomic<int> first_error_status{MMU_STATUS_OK};
    std::atomic<int> first_error_tid{-1};
    std::atomic<int> first_error_iter{-1};
    std::atomic<int> first_error_stage{0};  // 1=map, 2=unmap
    std::vector<uint8_t> expected_last(kThreadCount, 0);

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (uint32_t tid = 0; tid < kThreadCount; ++tid) {
        threads.emplace_back([&, tid]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            uint8_t last_value = 0;
            for (uint32_t iter = 0; iter < kIters; ++iter) {
                if (failed.load(std::memory_order_relaxed)) {
                    return;
                }

                mmu_mapped_block_t mapped{};
                mmu_status_t status = mmu_map_logical_block(
                    mmu, contexts[tid], 0, MMU_MAP_READ | MMU_MAP_WRITE, &mapped);
                if (status != MMU_STATUS_OK) {
                    first_error_status.store(static_cast<int>(status), std::memory_order_relaxed);
                    first_error_tid.store(static_cast<int>(tid), std::memory_order_relaxed);
                    first_error_iter.store(static_cast<int>(iter), std::memory_order_relaxed);
                    first_error_stage.store(1, std::memory_order_relaxed);
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }

                last_value = static_cast<uint8_t>((tid * 31u + iter) & 0xFFu);
                static_cast<uint8_t*>(mapped.host_ptr)[0] = last_value;

                status = mmu_unmap_logical_block(mmu, contexts[tid], &mapped);
                if (status != MMU_STATUS_OK) {
                    first_error_status.store(static_cast<int>(status), std::memory_order_relaxed);
                    first_error_tid.store(static_cast<int>(tid), std::memory_order_relaxed);
                    first_error_iter.store(static_cast<int>(iter), std::memory_order_relaxed);
                    first_error_stage.store(2, std::memory_order_relaxed);
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }

            expected_last[tid] = last_value;
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : threads) {
        t.join();
    }

    if (failed.load(std::memory_order_relaxed)) {
        ADD_FAILURE() << "parallel worker failed: status=" << first_error_status.load(std::memory_order_relaxed)
                      << ", tid=" << first_error_tid.load(std::memory_order_relaxed)
                      << ", iter=" << first_error_iter.load(std::memory_order_relaxed)
                      << ", stage=" << first_error_stage.load(std::memory_order_relaxed);
    }

    for (uint32_t tid = 0; tid < kThreadCount; ++tid) {
        mmu_mapped_block_t mapped_read{};
        ASSERT_EQ(mmu_map_logical_block(mmu, contexts[tid], 0, MMU_MAP_READ, &mapped_read), MMU_STATUS_OK);
        EXPECT_EQ(static_cast<const uint8_t*>(mapped_read.host_ptr)[0], expected_last[tid]);
        ASSERT_EQ(mmu_unmap_logical_block(mmu, contexts[tid], &mapped_read), MMU_STATUS_OK);
    }

    for (ctx_handle_t ctx : contexts) {
        mmu_destroy_context(mmu, ctx);
    }
    mmu_shutdown(mmu);
}

}  // namespace
