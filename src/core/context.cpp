#include "core/context.h"

#include <vector>

namespace dsf_mmu {
namespace core {

namespace {
constexpr int kUnallocatedBlock = -1;
}

Context::Context(std::shared_ptr<BlockManager> block_manager, uint32_t block_size_bytes)
    : block_manager_(std::move(block_manager)), block_size_bytes_(block_size_bytes) {}

Context::~Context() {
    ReleaseAllBlocks();
}

/*
Allocate的逻辑：
1. 验证输入参数是否合法（num_blocks >= 0）。如果不合法，返回MMU_ERR_INVALID_ARGUMENT。
2. 创建一个临时的vector来存储成功分配的物理块ID，以便在分配过程中如果发生错误时能够回滚已经分配的块。
3. 循环num_blocks次，尝试分配物理块：
   a. 调用BlockManager的allocate方法获取一个新的物理块ID。
   b. 如果分配失败（返回false），则回滚之前成功分配的所有块（通过调用BlockManager的release方法），并返回MMU_ERR_OUT_OF_MEMORY。
   c. 如果分配成功，将物理块ID添加到临时vector中，并在PageTable中为当前逻辑块追加一个新的映射，映射到分配的物理块ID，并设置tier为L1。
4. 如果循环完成且所有块都成功分配，返回MMU_STATUS_OK。
*/
mmu_status_t Context::AllocateBlocks(int num_blocks) {
    return AllocateBlocks(num_blocks, nullptr);
}

mmu_status_t Context::AllocateBlocks(int num_blocks, std::vector<int>* out_new_physical_blocks) {
    if (num_blocks < 0) 
    {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    std::vector<int> allocated;
    allocated.reserve(static_cast<size_t>(num_blocks));

    for (int i = 0; i < num_blocks; ++i) {
        int pbid = -1;
        if (!block_manager_->allocate(&pbid)) {
            for (int rollback_pbid : allocated) {
                block_manager_->release(rollback_pbid);
            }
            return MMU_ERR_OUT_OF_MEMORY;
        }

        allocated.push_back(pbid);
        page_table_.append(pbid, MMU_TIER_L1);
    }

    if (out_new_physical_blocks != nullptr) 
    {
        out_new_physical_blocks->insert(out_new_physical_blocks->end(), allocated.begin(), allocated.end());
    }

    return MMU_STATUS_OK;
}

mmu_status_t Context::ReleaseLogicalBlock(int logical_block) {
    PageEntry entry;
    if (!page_table_.get(logical_block, &entry)) {
        return MMU_ERR_UNALLOCATED;
    }

    if (entry.physical_block_id == kUnallocatedBlock) {
        return MMU_ERR_UNALLOCATED;
    }

    if (!block_manager_->release(entry.physical_block_id)) {
        return MMU_ERR_INTERNAL;
    }

    entry.physical_block_id = kUnallocatedBlock;
    page_table_.set(logical_block, entry);
    return MMU_STATUS_OK;
}

mmu_status_t Context::ResolveRouting(int logical_block, physical_route_t* out_route) const {
    if (out_route == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    PageEntry entry;
    if (!page_table_.get(logical_block, &entry)) {
        return MMU_ERR_UNALLOCATED;
    }

    if (entry.physical_block_id == kUnallocatedBlock) {
        return MMU_ERR_UNALLOCATED;
    }

    out_route->tier = entry.tier;
    out_route->physical_block_id = entry.physical_block_id;
    out_route->byte_offset =
        static_cast<uint64_t>(entry.physical_block_id) * static_cast<uint64_t>(block_size_bytes_);
    return MMU_STATUS_OK;
}

mmu_status_t Context::UpdateMapping(int logical_block, int new_physical_block, mmu_tier_t new_tier) {
    PageEntry current;
    if (!page_table_.get(logical_block, &current)) {
        return MMU_ERR_UNALLOCATED;
    }

    if (!block_manager_->is_valid(new_physical_block)) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    if (current.physical_block_id == new_physical_block) {
        current.tier = new_tier;
        page_table_.set(logical_block, current);
        return MMU_STATUS_OK;
    }

    if (!block_manager_->allocate_specific(new_physical_block)) {
        return MMU_ERR_OUT_OF_MEMORY;
    }

    if (current.physical_block_id != kUnallocatedBlock) {
        if (!block_manager_->release(current.physical_block_id)) {
            block_manager_->release(new_physical_block);
            return MMU_ERR_INTERNAL;
        }
    }

    page_table_.set(logical_block, PageEntry{new_physical_block, new_tier});
    return MMU_STATUS_OK;
}

/*
PrepareBlockForWrite的逻辑：
1. 验证输入参数是否合法（out_physical_block != nullptr）。如果不合法，返回MMU_ERR_INVALID_ARGUMENT。
2. 从PageTable中获取给定logical_block的PageEntry。如果logical_block未分配（即get方法返回false或physical_block_id为kUnallocatedBlock），返回MMU_ERR_UNALLOCATED。
3. 获取当前映射的物理块的引用计数。如果引用计数小于等于0，返回MMU_ERR_INTERNAL（这表示内部状态不一致）。如果引用计数等于1，说明当前逻辑块独占该物理块，可以直接返回当前物理块ID，无需修改映射。
4. 如果引用计数大于1，说明当前逻辑块共享该物理块，需要为写入操作准备一个新的物理块：
   a. 调用BlockManager的allocate方法分配一个新的物理块ID。如果分配失败，返回MMU_ERR_OUT_OF_MEMORY。
   b. 调用BlockManager的release方法释放当前映射的物理块ID，因为当前逻辑块将不再使用它。如果释放失败，回滚之前分配的新物理块，并返回MMU_ERR_INTERNAL。
   c. 更新PageTable中logical_block的映射，将physical_block_id更新为新分配的物理块ID，并保持tier不变。
5. 将新分配的物理块ID写入out_physical_block，并返回MMU_STATUS_OK。
*/
mmu_status_t Context::PrepareBlockForWrite(int logical_block, int* out_physical_block) {
    if (out_physical_block == nullptr) {
        return MMU_ERR_INVALID_ARGUMENT;
    }

    PageEntry entry;
    if (!page_table_.get(logical_block, &entry)) {
        return MMU_ERR_UNALLOCATED;
    }

    if (entry.physical_block_id == kUnallocatedBlock) {
        return MMU_ERR_UNALLOCATED;
    }

    const int ref_count = block_manager_->ref_count(entry.physical_block_id);
    if (ref_count <= 0) {
        return MMU_ERR_INTERNAL;
    }

    if (ref_count == 1) {
        *out_physical_block = entry.physical_block_id;
        return MMU_STATUS_OK;
    }

    int new_pbid = -1;
    if (!block_manager_->allocate(&new_pbid)) {
        return MMU_ERR_OUT_OF_MEMORY;
    }

    if (!block_manager_->release(entry.physical_block_id)) {
        block_manager_->release(new_pbid);
        return MMU_ERR_INTERNAL;
    }

    page_table_.set(logical_block, PageEntry{new_pbid, entry.tier});
    *out_physical_block = new_pbid;
    return MMU_STATUS_OK;
}

void Context::SetTierForPhysicalBlock(int physical_block_id, mmu_tier_t new_tier) {
    for (PageEntry& entry : page_table_.mutable_entries()) {
        if (entry.physical_block_id == physical_block_id) {
            entry.tier = new_tier;
        }
    }
}

Context Context::Fork() const {
    Context child(block_manager_, block_size_bytes_);
    for (const PageEntry& entry : page_table_.entries()) {
        if (entry.physical_block_id != kUnallocatedBlock) {
            block_manager_->retain(entry.physical_block_id);
        }
        child.page_table_.append(entry.physical_block_id, entry.tier);
    }

    return child;
}

uint32_t Context::AllocatedLogicalBlocks() const {
    uint32_t allocated = 0;
    for (const PageEntry& entry : page_table_.entries()) {
        if (entry.physical_block_id != kUnallocatedBlock) {
            ++allocated;
        }
    }
    return allocated;
}

void Context::ReleaseAllBlocks() {
    for (const PageEntry& entry : page_table_.entries()) {
        if (entry.physical_block_id != kUnallocatedBlock) {
            block_manager_->release(entry.physical_block_id);
        }
    }
}

}  // namespace core
}  // namespace dsf_mmu
