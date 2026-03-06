#include "core/context.h"

#include <vector>

namespace dsf_mmu {
namespace core {

namespace {
constexpr int kUnallocatedBlock = kInvalidBlockId;
}

Context::Context(std::shared_ptr<BlockManager> block_manager,
                 std::shared_ptr<BlockDirectory> block_directory,
                 uint32_t block_size_bytes)
    : block_manager_(std::move(block_manager)),
      block_directory_(std::move(block_directory)),
      block_size_bytes_(block_size_bytes) {}

Context::~Context() {
    ReleaseAllBlocks();
}

mmu_status_t Context::AllocateBlocks(int num_blocks) {
    return AllocateBlocks(num_blocks, nullptr);
}

mmu_status_t Context::AllocateBlocks(int num_blocks, std::vector<int>* out_new_block_ids) {
    if (num_blocks < 0) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    std::vector<int> allocated;
    allocated.reserve(static_cast<size_t>(num_blocks));

    for (int i = 0; i < num_blocks; ++i) {
        int block_id = kInvalidBlockId;
        if (!block_manager_->allocate(&block_id)) {
            for (int rollback_block_id : allocated) {
                block_manager_->release(rollback_block_id);
            }
            return MMU_ERR_OUT_OF_MEMORY;
        }

        allocated.push_back(block_id);
        page_table_.append(block_id);
    }

    if (out_new_block_ids != nullptr) {
        out_new_block_ids->insert(out_new_block_ids->end(), allocated.begin(), allocated.end());
    }

    return MMU_STATUS_OK;
}

mmu_status_t Context::ReleaseLogicalBlock(int logical_block) {
    PageEntry entry{};
    if (!page_table_.get(logical_block, &entry)) {
        return MMU_ERR_UNALLOCATED;
    }

    if (entry.block_id == kUnallocatedBlock) {
        return MMU_ERR_UNALLOCATED;
    }

    if (!block_manager_->release(entry.block_id)) {
        return MMU_ERR_INTERNAL;
    }

    entry.block_id = kUnallocatedBlock;
    page_table_.set(logical_block, entry);
    return MMU_STATUS_OK;
}

mmu_status_t Context::ResolveRouting(int logical_block, physical_route_t* out_route) const {
    if (out_route == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    PageEntry entry{};
    if (!page_table_.get(logical_block, &entry)) {
        return MMU_ERR_UNALLOCATED;
    }

    if (entry.block_id == kUnallocatedBlock) {
        return MMU_ERR_UNALLOCATED;
    }

    BlockMeta meta{};
    if (!block_directory_->snapshot(entry.block_id, &meta) || !meta.allocated) {
        return MMU_ERR_INTERNAL;
    }

    out_route->tier = meta.tier;
    out_route->physical_block_id = entry.block_id;
    out_route->byte_offset =
        static_cast<uint64_t>(entry.block_id) * static_cast<uint64_t>(block_size_bytes_);
    return MMU_STATUS_OK;
}

mmu_status_t Context::UpdateMapping(int logical_block, int new_block_id) {
    PageEntry current{};
    if (!page_table_.get(logical_block, &current)) {
        return MMU_ERR_UNALLOCATED;
    }

    if (!block_manager_->is_valid(new_block_id)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    if (current.block_id == new_block_id) {
        return MMU_STATUS_OK;
    }

    if (!block_manager_->allocate_specific(new_block_id)) {
        return MMU_ERR_OUT_OF_MEMORY;
    }

    if (current.block_id != kUnallocatedBlock) {
        if (!block_manager_->release(current.block_id)) {
            block_manager_->release(new_block_id);
            return MMU_ERR_INTERNAL;
        }
    }

    page_table_.set(logical_block, PageEntry{new_block_id});
    return MMU_STATUS_OK;
}

mmu_status_t Context::PrepareBlockForWrite(int logical_block, int* out_block_id) {
    if (out_block_id == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    PageEntry entry{};
    if (!page_table_.get(logical_block, &entry)) {
        return MMU_ERR_UNALLOCATED;
    }

    if (entry.block_id == kUnallocatedBlock) {
        return MMU_ERR_UNALLOCATED;
    }

    const int ref_count = block_manager_->ref_count(entry.block_id);
    if (ref_count <= 0) {
        return MMU_ERR_INTERNAL;
    }

    if (ref_count == 1) {
        *out_block_id = entry.block_id;
        return MMU_STATUS_OK;
    }

    int new_block_id = kInvalidBlockId;
    if (!block_manager_->allocate(&new_block_id)) {
        return MMU_ERR_OUT_OF_MEMORY;
    }

    if (!block_manager_->release(entry.block_id)) {
        block_manager_->release(new_block_id);
        return MMU_ERR_INTERNAL;
    }

    page_table_.set(logical_block, PageEntry{new_block_id});
    *out_block_id = new_block_id;
    return MMU_STATUS_OK;
}

Context Context::Fork() const {
    Context child(block_manager_, block_directory_, block_size_bytes_);
    for (const PageEntry& entry : page_table_.entries()) {
        if (entry.block_id != kUnallocatedBlock) {
            block_manager_->retain(entry.block_id);
        }
        child.page_table_.append(entry.block_id);
    }

    return child;
}

uint32_t Context::AllocatedLogicalBlocks() const {
    uint32_t allocated = 0;
    for (const PageEntry& entry : page_table_.entries()) {
        if (entry.block_id != kUnallocatedBlock) {
            ++allocated;
        }
    }
    return allocated;
}

void Context::ReleaseAllBlocks() {
    for (const PageEntry& entry : page_table_.entries()) {
        if (entry.block_id != kUnallocatedBlock) {
            block_manager_->release(entry.block_id);
        }
    }
}

}  // namespace core
}  // namespace dsf_mmu
