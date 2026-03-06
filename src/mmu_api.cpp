#include "dsf_mmu/mmu_api.h"

#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "core/block_directory.h"
#include "core/block_manager.h"
#include "core/context.h"
#include "internal/mmu_test_api.h"

struct mmu_runtime_t;

struct mmu_context_t {
    explicit mmu_context_t(dsf_mmu::core::Context&& impl_in)
        : impl(std::move(impl_in)) {}

    dsf_mmu::core::Context impl;
};

struct ChunkPool {
    explicit ChunkPool(uint32_t block_count)
        : owner_pages(block_count, nullptr),
          committed_pages(0) {}

    std::vector<std::unique_ptr<uint8_t[]>> chunks;
    std::vector<uint8_t*> free_blocks;
    std::vector<uint8_t*> owner_pages;
    uint32_t committed_pages;
};

struct mmu_runtime_t {
    explicit mmu_runtime_t(mmu_config_t cfg)
        : config(cfg),
          l1_allocator(std::make_shared<dsf_mmu::core::BlockManager>(cfg.l1_block_count)),
          block_directory(std::make_shared<dsf_mmu::core::BlockDirectory>(cfg.l1_block_count)),
          l1_pool(cfg.l1_block_count),
          l2_pool(cfg.l1_block_count),
          resident_limit(cfg.l1_resident_block_limit),
          resident_count(0),
          epoch(0) {}

    mmu_config_t config;
    std::shared_ptr<dsf_mmu::core::BlockManager> l1_allocator;
    std::shared_ptr<dsf_mmu::core::BlockDirectory> block_directory;
    std::unordered_map<ctx_handle_t, std::unique_ptr<mmu_context_t>> contexts;
    std::recursive_mutex api_mutex;

    ChunkPool l1_pool;
    ChunkPool l2_pool;
    uint32_t resident_limit;
    uint32_t resident_count;
    uint64_t epoch;
};

namespace {

constexpr uint32_t kDefaultChunkBlockCount = 4096u;
constexpr uint64_t kAutoResidentBudgetDivisor = 2u;

bool U32ToInt(uint32_t value, int* out) {
    if (out == nullptr) {
        return false;
    }
    if (value > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    *out = static_cast<int>(value);
    return true;
}

uint8_t* RawPage(ChunkPool* pool, int physical_block_id) {
    return pool->owner_pages[physical_block_id];
}

uint64_t QueryAvailableHostMemoryBytes() {
#if defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    const long page_count = sysconf(_SC_AVPHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_count <= 0 || page_size <= 0) {
        return 0;
    }

    const uint64_t pages = static_cast<uint64_t>(page_count);
    const uint64_t size = static_cast<uint64_t>(page_size);
    if (pages > std::numeric_limits<uint64_t>::max() / size) {
        return 0;
    }
    return pages * size;
#else
    return 0;
#endif
}

uint32_t ResolveAutoResidentLimit(const mmu_config_t& cfg) {
    if (cfg.l1_block_count == 0 || cfg.block_size_bytes == 0) {
        return 0;
    }

    const uint64_t available_bytes = QueryAvailableHostMemoryBytes();
    if (available_bytes == 0) {
        return cfg.l1_block_count;
    }

    const uint64_t budget_bytes = available_bytes / kAutoResidentBudgetDivisor;
    uint64_t blocks = budget_bytes / static_cast<uint64_t>(cfg.block_size_bytes);
    if (blocks == 0) {
        blocks = 1;
    }
    if (blocks > static_cast<uint64_t>(cfg.l1_block_count)) {
        blocks = static_cast<uint64_t>(cfg.l1_block_count);
    }
    return static_cast<uint32_t>(blocks);
}

bool IsL2Pool(const mmu_runtime_t* mmu, const ChunkPool* pool) {
    return pool == &mmu->l2_pool;
}

mmu_status_t AllocateChunk(mmu_runtime_t* mmu, ChunkPool* pool, uint32_t blocks_per_chunk) {
    try {
        const uint32_t block_size = mmu->config.block_size_bytes;
        if (blocks_per_chunk == 0 || block_size == 0) {
            return MMU_ERR_INVALID_ARGUMENT;
        }

        const size_t chunk_bytes = static_cast<size_t>(blocks_per_chunk) * static_cast<size_t>(block_size);
        if (chunk_bytes / static_cast<size_t>(blocks_per_chunk) != static_cast<size_t>(block_size)) {
            return MMU_ERR_INVALID_ARGUMENT;
        }

        std::unique_ptr<uint8_t[]> chunk(new (std::nothrow) uint8_t[chunk_bytes]);
        if (!chunk) {
            return MMU_ERR_OUT_OF_MEMORY;
        }

        uint8_t* base = chunk.get();
        pool->chunks.push_back(std::move(chunk));
        for (uint32_t i = 0; i < blocks_per_chunk; ++i) {
            const size_t offset = static_cast<size_t>(i) * static_cast<size_t>(block_size);
            pool->free_blocks.push_back(base + offset);
        }

        return MMU_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MMU_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return MMU_ERR_INTERNAL;
    }
}

mmu_status_t AcquirePage(mmu_runtime_t* mmu, ChunkPool* pool, int physical_block_id, uint8_t** out_page) {
    if (physical_block_id < 0 ||
        physical_block_id >= static_cast<int>(pool->owner_pages.size()) ||
        out_page == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    uint8_t*& slot = pool->owner_pages[physical_block_id];
    if (slot == nullptr) {
        if (IsL2Pool(mmu, pool) && mmu->config.l2_block_count > 0 &&
            pool->committed_pages >= mmu->config.l2_block_count) {
            return MMU_ERR_OUT_OF_MEMORY;
        }

        if (pool->free_blocks.empty()) {
            uint32_t blocks_per_chunk = mmu->config.chunk_block_count;
            if (IsL2Pool(mmu, pool) && mmu->config.l2_block_count > 0) {
                const uint32_t remaining = mmu->config.l2_block_count - pool->committed_pages;
                if (remaining == 0) {
                    return MMU_ERR_OUT_OF_MEMORY;
                }
                if (remaining < blocks_per_chunk) {
                    blocks_per_chunk = remaining;
                }
            }

            mmu_status_t status = AllocateChunk(mmu, pool, blocks_per_chunk);
            if (status != MMU_STATUS_OK) {
                return status;
            }
        }

        if (pool->free_blocks.empty()) {
            return MMU_ERR_OUT_OF_MEMORY;
        }

        slot = pool->free_blocks.back();
        pool->free_blocks.pop_back();
        std::memset(slot, 0, mmu->config.block_size_bytes);
        ++pool->committed_pages;
    }

    *out_page = slot;
    return MMU_STATUS_OK;
}

void ReleasePage(ChunkPool* pool, int physical_block_id) {
    if (physical_block_id < 0 ||
        physical_block_id >= static_cast<int>(pool->owner_pages.size())) {
        return;
    }

    uint8_t*& slot = pool->owner_pages[physical_block_id];
    if (slot != nullptr) {
        pool->free_blocks.push_back(slot);
        slot = nullptr;
        if (pool->committed_pages > 0) {
            --pool->committed_pages;
        }
    }
}

void CopyOrZero(const uint8_t* src, uint8_t* dst, uint32_t size_bytes) {
    if (src == nullptr) {
        std::memset(dst, 0, size_bytes);
        return;
    }

    std::memcpy(dst, src, size_bytes);
}

const dsf_mmu::core::BlockMeta* BlockMetaFor(const mmu_runtime_t* mmu, int block_id) {
    return mmu->block_directory->get(block_id);
}

dsf_mmu::core::BlockMeta* MutableBlockMeta(mmu_runtime_t* mmu, int block_id) {
    return mmu->block_directory->mutable_get(block_id);
}

void TouchBlock(mmu_runtime_t* mmu, int physical_block_id) {
    (void)mmu->block_directory->set_last_access_epoch(physical_block_id, ++mmu->epoch);
}

mmu_status_t SyncL1ToL2(mmu_runtime_t* mmu, int physical_block_id) {
    uint8_t* dst_l2 = nullptr;
    mmu_status_t status = AcquirePage(mmu, &mmu->l2_pool, physical_block_id, &dst_l2);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    const uint8_t* src_l1 = RawPage(&mmu->l1_pool, physical_block_id);
    CopyOrZero(src_l1, dst_l2, mmu->config.block_size_bytes);
    (void)mmu->block_directory->set_has_l2_backing(physical_block_id, true);
    (void)mmu->block_directory->set_l2_slot_id(physical_block_id, physical_block_id);
    return MMU_STATUS_OK;
}

mmu_status_t SyncL2ToL1(mmu_runtime_t* mmu, int physical_block_id) {
    uint8_t* dst_l1 = nullptr;
    mmu_status_t status = AcquirePage(mmu, &mmu->l1_pool, physical_block_id, &dst_l1);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    const uint8_t* src_l2 = RawPage(&mmu->l2_pool, physical_block_id);
    CopyOrZero(src_l2, dst_l1, mmu->config.block_size_bytes);
    (void)mmu->block_directory->set_l1_frame_id(physical_block_id, physical_block_id);
    return MMU_STATUS_OK;
}

void ResetBlockData(mmu_runtime_t* mmu, int physical_block_id) {
    ReleasePage(&mmu->l1_pool, physical_block_id);
    ReleasePage(&mmu->l2_pool, physical_block_id);

    if (dsf_mmu::core::BlockMeta* meta = MutableBlockMeta(mmu, physical_block_id)) {
        meta->resident_in_l1 = false;
        meta->dirty = false;
        meta->pin_count = 0;
        meta->last_access_epoch = 0;
        meta->l1_frame_id = dsf_mmu::core::kInvalidFrameId;
        meta->l2_slot_id = dsf_mmu::core::kInvalidSlotId;
        meta->has_l2_backing = false;
    }
}

void ReclaimUnusedResidentBlocks(mmu_runtime_t* mmu) {
    const int block_count = static_cast<int>(mmu->config.l1_block_count);
    for (int pbid = 0; pbid < block_count; ++pbid) {
        const int ref_count = mmu->l1_allocator->ref_count(pbid);
        if (ref_count == 0) {
            const dsf_mmu::core::BlockMeta* meta = BlockMetaFor(mmu, pbid);
            if (meta != nullptr && meta->resident_in_l1) {
                if (mmu->resident_count > 0) {
                    --mmu->resident_count;
                }
            }

            ResetBlockData(mmu, pbid);
            mmu->block_directory->reset(pbid);
        }
    }
}

int PickEvictionCandidate(const mmu_runtime_t* mmu, int protected_pbid) {
    const int block_count = static_cast<int>(mmu->config.l1_block_count);
    int victim = -1;
    uint64_t oldest_epoch = std::numeric_limits<uint64_t>::max();

    for (int pbid = 0; pbid < block_count; ++pbid) {
        const dsf_mmu::core::BlockMeta* meta = BlockMetaFor(mmu, pbid);
        if (meta == nullptr || !meta->allocated || !meta->resident_in_l1) {
            continue;
        }
        if (pbid == protected_pbid) {
            continue;
        }
        if (meta->pin_count > 0) {
            continue;
        }

        if (meta->last_access_epoch < oldest_epoch) {
            oldest_epoch = meta->last_access_epoch;
            victim = pbid;
        }
    }

    return victim;
}

mmu_status_t MoveBlockToL2(mmu_runtime_t* mmu, int physical_block_id) {
    if (!mmu->l1_allocator->is_valid(physical_block_id)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    dsf_mmu::core::BlockMeta* meta = MutableBlockMeta(mmu, physical_block_id);
    if (meta == nullptr || !meta->allocated) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    if (meta->resident_in_l1) {
        mmu_status_t status = SyncL1ToL2(mmu, physical_block_id);
        if (status != MMU_STATUS_OK) {
            return status;
        }

        meta->resident_in_l1 = false;
        meta->l1_frame_id = dsf_mmu::core::kInvalidFrameId;
        if (mmu->resident_count > 0) {
            --mmu->resident_count;
        }
    }

    ReleasePage(&mmu->l1_pool, physical_block_id);
    meta->tier = MMU_TIER_L2;
    return MMU_STATUS_OK;
}

mmu_status_t EnsureResidentInL1(mmu_runtime_t* mmu, int physical_block_id) {
    if (!mmu->l1_allocator->is_valid(physical_block_id)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    ReclaimUnusedResidentBlocks(mmu);

    dsf_mmu::core::BlockMeta* meta = MutableBlockMeta(mmu, physical_block_id);
    if (meta == nullptr || !meta->allocated) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    if (meta->resident_in_l1) {
        TouchBlock(mmu, physical_block_id);
        meta->tier = MMU_TIER_L1;
        return MMU_STATUS_OK;
    }

    while (mmu->resident_count >= mmu->resident_limit) {
        const int victim = PickEvictionCandidate(mmu, physical_block_id);
        if (victim < 0) {
            return MMU_ERR_OUT_OF_MEMORY;
        }

        mmu_status_t status = MoveBlockToL2(mmu, victim);
        if (status != MMU_STATUS_OK) {
            return status;
        }
    }

    mmu_status_t status = SyncL2ToL1(mmu, physical_block_id);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    meta->resident_in_l1 = true;
    meta->l1_frame_id = physical_block_id;
    meta->tier = MMU_TIER_L1;
    ++mmu->resident_count;
    TouchBlock(mmu, physical_block_id);
    return MMU_STATUS_OK;
}

void RegisterFreshBlock(mmu_runtime_t* mmu, int physical_block_id) {
    (void)mmu->block_directory->initialize_fresh_block(physical_block_id);
    ResetBlockData(mmu, physical_block_id);
    dsf_mmu::core::BlockMeta* meta = MutableBlockMeta(mmu, physical_block_id);
    if (meta == nullptr) {
        return;
    }

    if (mmu->resident_count < mmu->resident_limit) {
        if (!meta->resident_in_l1) {
            meta->resident_in_l1 = true;
            meta->l1_frame_id = physical_block_id;
            ++mmu->resident_count;
        }
        meta->tier = MMU_TIER_L1;
        TouchBlock(mmu, physical_block_id);
    } else {
        meta->resident_in_l1 = false;
        meta->l1_frame_id = dsf_mmu::core::kInvalidFrameId;
        meta->last_access_epoch = 0;
        meta->tier = MMU_TIER_L2;
    }
}

mmu_status_t CloneBlockDataForCow(mmu_runtime_t* mmu,
                                  int src_physical_block,
                                  mmu_tier_t src_tier,
                                  int dst_physical_block) {
    if (!mmu->l1_allocator->is_valid(src_physical_block) ||
        !mmu->l1_allocator->is_valid(dst_physical_block)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    const dsf_mmu::core::BlockMeta* src_meta = BlockMetaFor(mmu, src_physical_block);
    const dsf_mmu::core::BlockMeta* dst_meta = BlockMetaFor(mmu, dst_physical_block);
    if (src_meta == nullptr || dst_meta == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    const uint8_t* src_ptr = nullptr;
    if (src_tier == MMU_TIER_L1 && src_meta->resident_in_l1) {
        src_ptr = RawPage(&mmu->l1_pool, src_physical_block);
    }
    if (src_ptr == nullptr) {
        src_ptr = RawPage(&mmu->l2_pool, src_physical_block);
    }
    if (src_ptr == nullptr) {
        src_ptr = RawPage(&mmu->l1_pool, src_physical_block);
    }

    uint8_t* dst_l2 = nullptr;
    mmu_status_t status = AcquirePage(mmu, &mmu->l2_pool, dst_physical_block, &dst_l2);
    if (status != MMU_STATUS_OK) {
        return status;
    }
    CopyOrZero(src_ptr, dst_l2, mmu->config.block_size_bytes);
    (void)mmu->block_directory->set_has_l2_backing(dst_physical_block, true);
    (void)mmu->block_directory->set_l2_slot_id(dst_physical_block, dst_physical_block);

    if (dst_meta->resident_in_l1) {
        uint8_t* dst_l1 = nullptr;
        status = AcquirePage(mmu, &mmu->l1_pool, dst_physical_block, &dst_l1);
        if (status != MMU_STATUS_OK) {
            return status;
        }
        std::memcpy(dst_l1, dst_l2, mmu->config.block_size_bytes);
    } else {
        ReleasePage(&mmu->l1_pool, dst_physical_block);
    }

    return MMU_STATUS_OK;
}

bool ValidateMapFlags(uint32_t map_flags) {
    const uint32_t allowed = MMU_MAP_READ | MMU_MAP_WRITE;
    if ((map_flags & allowed) == 0) {
        return false;
    }
    if ((map_flags & ~allowed) != 0) {
        return false;
    }
    return true;
}

mmu_context_t* FindContext(mmu_handle_t mmu, ctx_handle_t ctx) {
    if (mmu == nullptr || ctx == nullptr) {
        return nullptr;
    }

    const auto it = mmu->contexts.find(ctx);
    if (it == mmu->contexts.end()) {
        return nullptr;
    }

    return it->second.get();
}

uint32_t CountCommittedPages(const ChunkPool& pool) {
    return pool.committed_pages;
}

uint32_t CountAllocatedChunks(const ChunkPool& pool) {
    return static_cast<uint32_t>(pool.chunks.size());
}

}  // namespace

extern "C" {

mmu_status_t mmu_init(const mmu_config_t* config, mmu_handle_t* out_mmu) {
    if (config == nullptr || out_mmu == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    *out_mmu = nullptr;

    if (config->size < static_cast<uint32_t>(sizeof(mmu_config_t)) ||
        config->api_version != MMU_API_VERSION) {
        return MMU_ERR_VERSION_MISMATCH;
    }

    if (config->l1_block_count == 0 || config->block_size_bytes == 0) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    try {
        mmu_config_t resolved = *config;
        if (resolved.l1_resident_block_limit == 0) {
            resolved.l1_resident_block_limit = ResolveAutoResidentLimit(resolved);
        }
        if (resolved.l1_resident_block_limit == 0 ||
            resolved.l1_resident_block_limit > resolved.l1_block_count) {
            resolved.l1_resident_block_limit = resolved.l1_block_count;
        }

        if (resolved.l2_block_count == 0 || resolved.l2_block_count > resolved.l1_block_count) {
            resolved.l2_block_count = resolved.l1_block_count;
        }

        if (resolved.chunk_block_count == 0) {
            resolved.chunk_block_count = kDefaultChunkBlockCount;
        }
        if (resolved.chunk_block_count > resolved.l1_block_count) {
            resolved.chunk_block_count = resolved.l1_block_count;
        }

        *out_mmu = new mmu_runtime_t(resolved);
        return MMU_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MMU_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return MMU_ERR_INTERNAL;
    }
}

void mmu_shutdown(mmu_handle_t mmu) {
    if (mmu == nullptr) {
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);
        mmu->contexts.clear();
    }
    delete mmu;
}

mmu_status_t mmu_create_context(mmu_handle_t mmu, ctx_handle_t* out_ctx) {
    if (mmu == nullptr || out_ctx == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    *out_ctx = nullptr;

    try {
        dsf_mmu::core::Context impl(mmu->l1_allocator, mmu->block_directory, mmu->config.block_size_bytes);
        auto ctx = std::make_unique<mmu_context_t>(std::move(impl));
        const ctx_handle_t handle = ctx.get();
        mmu->contexts.emplace(handle, std::move(ctx));
        *out_ctx = handle;
        return MMU_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MMU_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return MMU_ERR_INTERNAL;
    }
}

void mmu_destroy_context(mmu_handle_t mmu, ctx_handle_t ctx) {
    if (mmu == nullptr || ctx == nullptr) {
        return;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    auto it = mmu->contexts.find(ctx);
    if (it != mmu->contexts.end()) {
        mmu->contexts.erase(it);
        ReclaimUnusedResidentBlocks(mmu);
    }
}

mmu_status_t mmu_fork_context(mmu_handle_t mmu, ctx_handle_t src_ctx, ctx_handle_t* out_ctx) {
    if (mmu == nullptr || src_ctx == nullptr || out_ctx == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    *out_ctx = nullptr;

    mmu_context_t* src = FindContext(mmu, src_ctx);
    if (src == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }

    try {
        dsf_mmu::core::Context child_impl = src->impl.Fork();
        auto child = std::make_unique<mmu_context_t>(std::move(child_impl));
        const ctx_handle_t handle = child.get();
        mmu->contexts.emplace(handle, std::move(child));
        *out_ctx = handle;
        return MMU_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MMU_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return MMU_ERR_INTERNAL;
    }
}

mmu_status_t mmu_allocate_blocks(mmu_handle_t mmu, ctx_handle_t ctx, uint32_t num_blocks) {
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_context_t* context = FindContext(mmu, ctx);
    if (context == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }

    int count = 0;
    if (!U32ToInt(num_blocks, &count)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    ReclaimUnusedResidentBlocks(mmu);

    std::vector<int> new_blocks;
    mmu_status_t status = context->impl.AllocateBlocks(count, &new_blocks);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    for (int pbid : new_blocks) {
        RegisterFreshBlock(mmu, pbid);
    }

    return MMU_STATUS_OK;
}

mmu_status_t mmu_release_logical_block(mmu_handle_t mmu, ctx_handle_t ctx, uint32_t logical_block) {
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_context_t* context = FindContext(mmu, ctx);
    if (context == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }

    int lvid = 0;
    if (!U32ToInt(logical_block, &lvid)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    physical_route_t route{};
    mmu_status_t status = context->impl.ResolveRouting(lvid, &route);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    status = context->impl.ReleaseLogicalBlock(lvid);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    if (mmu->l1_allocator->ref_count(route.physical_block_id) == 0) {
        const dsf_mmu::core::BlockMeta* meta = BlockMetaFor(mmu, route.physical_block_id);
        if (meta != nullptr && meta->resident_in_l1 && mmu->resident_count > 0) {
            --mmu->resident_count;
        }
        ResetBlockData(mmu, route.physical_block_id);
        mmu->block_directory->reset(route.physical_block_id);
    }

    return MMU_STATUS_OK;
}

mmu_status_t mmu_resolve_routing(mmu_handle_t mmu,
                                 ctx_handle_t ctx,
                                 uint32_t logical_block,
                                 physical_route_t* out_route) {
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_context_t* context = FindContext(mmu, ctx);
    if (context == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }

    int lvid = 0;
    if (!U32ToInt(logical_block, &lvid)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    return context->impl.ResolveRouting(lvid, out_route);
}

mmu_status_t mmu_resolve_routing_batch(mmu_handle_t mmu,
                                       ctx_handle_t ctx,
                                       const uint32_t* logical_blocks,
                                       uint32_t num_blocks,
                                       physical_route_t* out_routes) {
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_context_t* context = FindContext(mmu, ctx);
    if (context == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }

    if ((num_blocks > 0 && logical_blocks == nullptr) ||
        (num_blocks > 0 && out_routes == nullptr)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    for (uint32_t i = 0; i < num_blocks; ++i) {
        int lvid = 0;
        if (!U32ToInt(logical_blocks[i], &lvid)) {
            return MMU_ERR_INVALID_ARGUMENT;
        }

        mmu_status_t status = context->impl.ResolveRouting(lvid, &out_routes[i]);
        if (status != MMU_STATUS_OK) {
            return status;
        }
    }

    return MMU_STATUS_OK;
}

mmu_status_t mmu_prepare_block_for_write(mmu_handle_t mmu,
                                         ctx_handle_t ctx,
                                         uint32_t logical_block,
                                         int32_t* out_physical_block) {
    if (out_physical_block == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_context_t* context = FindContext(mmu, ctx);
    if (context == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }

    int lvid = 0;
    if (!U32ToInt(logical_block, &lvid)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    physical_route_t before{};
    mmu_status_t status = context->impl.ResolveRouting(lvid, &before);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    int pbid = -1;
    status = context->impl.PrepareBlockForWrite(lvid, &pbid);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    if (pbid != before.physical_block_id) {
        (void)mmu->block_directory->initialize_fresh_block(pbid);
        (void)mmu->block_directory->set_tier(pbid, MMU_TIER_L2);
        status = CloneBlockDataForCow(mmu, before.physical_block_id, before.tier, pbid);
        if (status != MMU_STATUS_OK) {
            return status;
        }
    }

    *out_physical_block = static_cast<int32_t>(pbid);
    return MMU_STATUS_OK;
}

mmu_status_t mmu_map_logical_block(mmu_handle_t mmu,
                                   ctx_handle_t ctx,
                                   uint32_t logical_block,
                                   uint32_t map_flags,
                                   mmu_mapped_block_t* out_mapped_block) {
    if (out_mapped_block == nullptr || !ValidateMapFlags(map_flags)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_context_t* context = FindContext(mmu, ctx);
    if (context == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }

    int lvid = 0;
    if (!U32ToInt(logical_block, &lvid)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    if ((map_flags & MMU_MAP_WRITE) != 0) {
        int32_t pbid_after_write = -1;
        mmu_status_t status = mmu_prepare_block_for_write(mmu, ctx, logical_block, &pbid_after_write);
        if (status != MMU_STATUS_OK) {
            return status;
        }
    }

    physical_route_t route{};
    mmu_status_t status = context->impl.ResolveRouting(lvid, &route);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    status = EnsureResidentInL1(mmu, route.physical_block_id);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    uint8_t* l1_page = nullptr;
    status = AcquirePage(mmu, &mmu->l1_pool, route.physical_block_id, &l1_page);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    if (!mmu->block_directory->increment_pin_count(route.physical_block_id)) {
        return MMU_ERR_INTERNAL;
    }
    TouchBlock(mmu, route.physical_block_id);

    out_mapped_block->host_ptr = l1_page;
    out_mapped_block->size_bytes = mmu->config.block_size_bytes;
    out_mapped_block->physical_block_id = route.physical_block_id;
    out_mapped_block->logical_block_id = logical_block;
    out_mapped_block->map_flags = map_flags;

    return MMU_STATUS_OK;
}

mmu_status_t mmu_map_logical_blocks(mmu_handle_t mmu,
                                    ctx_handle_t ctx,
                                    const uint32_t* logical_blocks,
                                    uint32_t num_blocks,
                                    uint32_t map_flags,
                                    mmu_mapped_block_t* out_mapped_blocks) {
    if ((num_blocks > 0 && logical_blocks == nullptr) ||
        (num_blocks > 0 && out_mapped_blocks == nullptr)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    for (uint32_t i = 0; i < num_blocks; ++i) {
        mmu_status_t status = mmu_map_logical_block(mmu, ctx, logical_blocks[i], map_flags, &out_mapped_blocks[i]);
        if (status != MMU_STATUS_OK) {
            for (uint32_t j = 0; j < i; ++j) {
                (void)mmu_unmap_logical_block(mmu, ctx, &out_mapped_blocks[j]);
            }
            return status;
        }
    }

    return MMU_STATUS_OK;
}

mmu_status_t mmu_unmap_logical_block(mmu_handle_t mmu,
                                     ctx_handle_t ctx,
                                     const mmu_mapped_block_t* mapped_block) {
    if (mapped_block == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_context_t* context = FindContext(mmu, ctx);
    if (context == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }

    const int pbid = static_cast<int>(mapped_block->physical_block_id);
    if (!mmu->l1_allocator->is_valid(pbid)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    int lvid = 0;
    if (!U32ToInt(mapped_block->logical_block_id, &lvid)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    physical_route_t route{};
    mmu_status_t status = context->impl.ResolveRouting(lvid, &route);
    if (status != MMU_STATUS_OK) {
        return status;
    }
    if (route.physical_block_id != pbid) {
        return MMU_ERR_BAD_HANDLE;
    }

    const dsf_mmu::core::BlockMeta* meta = BlockMetaFor(mmu, pbid);
    if (meta == nullptr || !meta->allocated) {
        return MMU_ERR_INTERNAL;
    }

    if (meta->pin_count > 0) {
        if (!mmu->block_directory->decrement_pin_count(pbid)) {
            return MMU_ERR_INTERNAL;
        }
    }

    if ((mapped_block->map_flags & MMU_MAP_WRITE) != 0) {
        (void)mmu->block_directory->set_dirty(pbid, true);
    }

    if ((mapped_block->map_flags & MMU_MAP_WRITE) != 0 && meta->resident_in_l1) {
        mmu_status_t sync_status = SyncL1ToL2(mmu, pbid);
        if (sync_status != MMU_STATUS_OK) {
            return sync_status;
        }
        (void)mmu->block_directory->set_dirty(pbid, false);
    }

    TouchBlock(mmu, pbid);
    return MMU_STATUS_OK;
}

mmu_status_t mmu_unmap_logical_blocks(mmu_handle_t mmu,
                                      ctx_handle_t ctx,
                                      const mmu_mapped_block_t* mapped_blocks,
                                      uint32_t num_blocks) {
    if (num_blocks > 0 && mapped_blocks == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_status_t first_error = MMU_STATUS_OK;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        mmu_status_t status = mmu_unmap_logical_block(mmu, ctx, &mapped_blocks[i]);
        if (status != MMU_STATUS_OK && first_error == MMU_STATUS_OK) {
            first_error = status;
        }
    }

    return first_error;
}

uint32_t mmu_get_free_block_count(mmu_handle_t mmu) {
    if (mmu == nullptr) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    return mmu->l1_allocator->free_blocks();
}

int32_t mmu_get_ref_count(mmu_handle_t mmu, int32_t physical_block_id) {
    if (mmu == nullptr) {
        return -1;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    return mmu->l1_allocator->ref_count(static_cast<int>(physical_block_id));
}

uint32_t mmu_get_logical_block_count(mmu_handle_t mmu, ctx_handle_t ctx) {
    if (mmu == nullptr) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_context_t* context = FindContext(mmu, ctx);
    if (context == nullptr) {
        return 0;
    }

    return context->impl.AllocatedLogicalBlocks();
}

mmu_status_t mmu_test_update_mapping(mmu_handle_t mmu,
                                     ctx_handle_t ctx,
                                     uint32_t logical_block,
                                     int32_t new_physical_block,
                                     mmu_tier_t new_tier) {
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    mmu_context_t* context = FindContext(mmu, ctx);
    if (context == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }

    if (new_physical_block < 0) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    int lvid = 0;
    if (!U32ToInt(logical_block, &lvid)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    physical_route_t previous_route{};
    mmu_status_t status = context->impl.ResolveRouting(lvid, &previous_route);
    if (status != MMU_STATUS_OK) {
        return status;
    }

    const bool same_block = previous_route.physical_block_id == new_physical_block;
    status = context->impl.UpdateMapping(lvid, static_cast<int>(new_physical_block));
    if (status != MMU_STATUS_OK) {
        return status;
    }

    if (!same_block) {
        (void)mmu->block_directory->initialize_fresh_block(static_cast<int>(new_physical_block));
        if (mmu->l1_allocator->ref_count(previous_route.physical_block_id) == 0) {
            const dsf_mmu::core::BlockMeta* old_meta = BlockMetaFor(mmu, previous_route.physical_block_id);
            if (old_meta != nullptr && old_meta->resident_in_l1 && mmu->resident_count > 0) {
                --mmu->resident_count;
            }
            ResetBlockData(mmu, previous_route.physical_block_id);
            mmu->block_directory->reset(previous_route.physical_block_id);
        }
    }

    if (new_tier == MMU_TIER_L1) {
        return EnsureResidentInL1(mmu, static_cast<int>(new_physical_block));
    }
    if (new_tier == MMU_TIER_L2) {
        return MoveBlockToL2(mmu, static_cast<int>(new_physical_block));
    }

    return MMU_ERR_INVALID_ARGUMENT;
}

mmu_status_t mmu_test_get_block_metadata(mmu_handle_t mmu,
                                         int32_t block_id,
                                         mmu_test_block_metadata_t* out_metadata) {
    if (mmu == nullptr) {
        return MMU_ERR_BAD_HANDLE;
    }
    if (out_metadata == nullptr || block_id < 0) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    dsf_mmu::core::BlockMeta meta{};
    if (!mmu->block_directory->snapshot(static_cast<int>(block_id), &meta)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    out_metadata->allocated = meta.allocated ? 1u : 0u;
    out_metadata->tier = meta.tier;
    out_metadata->resident_in_l1 = meta.resident_in_l1 ? 1u : 0u;
    out_metadata->dirty = meta.dirty ? 1u : 0u;
    out_metadata->pin_count = meta.pin_count;
    out_metadata->last_access_epoch = meta.last_access_epoch;
    out_metadata->l1_frame_id = meta.l1_frame_id;
    out_metadata->l2_slot_id = meta.l2_slot_id;
    out_metadata->has_l2_backing = meta.has_l2_backing ? 1u : 0u;
    return MMU_STATUS_OK;
}

mmu_status_t mmu_test_get_committed_page_counts(mmu_handle_t mmu,
                                                uint32_t* out_l1_pages,
                                                uint32_t* out_l2_pages) {
    if (mmu == nullptr || out_l1_pages == nullptr || out_l2_pages == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    *out_l1_pages = CountCommittedPages(mmu->l1_pool);
    *out_l2_pages = CountCommittedPages(mmu->l2_pool);
    return MMU_STATUS_OK;
}

mmu_status_t mmu_test_get_committed_chunk_counts(mmu_handle_t mmu,
                                                 uint32_t* out_l1_chunks,
                                                 uint32_t* out_l2_chunks) {
    if (mmu == nullptr || out_l1_chunks == nullptr || out_l2_chunks == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> guard(mmu->api_mutex);

    *out_l1_chunks = CountAllocatedChunks(mmu->l1_pool);
    *out_l2_chunks = CountAllocatedChunks(mmu->l2_pool);
    return MMU_STATUS_OK;
}

}  // extern "C"
