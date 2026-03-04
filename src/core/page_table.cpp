#include "core/page_table.h"

namespace dsf_mmu {
namespace core {

bool PageTable::append(int physical_block_id, mmu_tier_t tier) {
    entries_.push_back(PageEntry{physical_block_id, tier});
    return true;
}

bool PageTable::get(int logical_block_id, PageEntry* out_entry) const {
    if (out_entry == nullptr) {
        return false;
    }
    if (logical_block_id < 0 || logical_block_id >= static_cast<int>(entries_.size())) {
        return false;
    }

    *out_entry = entries_[logical_block_id];
    return true;
}

bool PageTable::set(int logical_block_id, PageEntry entry) {
    if (logical_block_id < 0 || logical_block_id >= static_cast<int>(entries_.size())) {
        return false;
    }

    entries_[logical_block_id] = entry;
    return true;
}

uint32_t PageTable::size() const {
    return static_cast<uint32_t>(entries_.size());
}

const std::vector<PageEntry>& PageTable::entries() const {
    return entries_;
}

std::vector<PageEntry>& PageTable::mutable_entries() {
    return entries_;
}

}  // namespace core
}  // namespace dsf_mmu
