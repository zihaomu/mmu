#include "core/block_manager.h"

#include <algorithm>

namespace dsf_mmu {
namespace core {


BlockManager::BlockManager(uint32_t total_blocks)
    : ref_counts_(total_blocks, 0) {
    free_list_.reserve(total_blocks);
    for (int i = static_cast<int>(total_blocks) - 1; i >= 0; --i) {
        free_list_.push_back(i);
    }
}

bool BlockManager::allocate(int* out_physical_block_id) {
    if (out_physical_block_id == nullptr || free_list_.empty()) {
        return false;
    }

    const int pbid = free_list_.back();
    free_list_.pop_back();
    ref_counts_[pbid] = 1;
    *out_physical_block_id = pbid;
    return true;
}

bool BlockManager::allocate_specific(int physical_block_id) {
    if (!is_valid(physical_block_id)) {
        return false;
    }

    const auto it = std::find(free_list_.begin(), free_list_.end(), physical_block_id);
    if (it == free_list_.end()) {
        return false;
    }

    free_list_.erase(it);
    ref_counts_[physical_block_id] = 1;
    return true;
}

bool BlockManager::retain(int physical_block_id) {
    if (!is_valid(physical_block_id)) {
        return false;
    }
    if (ref_counts_[physical_block_id] <= 0) {
        return false;
    }

    ++ref_counts_[physical_block_id];
    return true;
}

bool BlockManager::release(int physical_block_id) {
    if (!is_valid(physical_block_id)) {
        return false;
    }
    if (ref_counts_[physical_block_id] <= 0) {
        return false;
    }

    --ref_counts_[physical_block_id];
    if (ref_counts_[physical_block_id] == 0) {
        free_list_.push_back(physical_block_id);
    }
    return true;
}

bool BlockManager::is_valid(int physical_block_id) const {
    return physical_block_id >= 0 &&
           physical_block_id < static_cast<int>(ref_counts_.size());
}

int BlockManager::ref_count(int physical_block_id) const {
    if (!is_valid(physical_block_id)) {
        return -1;
    }
    return ref_counts_[physical_block_id];
}

uint32_t BlockManager::free_blocks() const {
    return static_cast<uint32_t>(free_list_.size());
}

uint32_t BlockManager::capacity() const {
    return static_cast<uint32_t>(ref_counts_.size());
}

}  // namespace core
}  // namespace dsf_mmu
