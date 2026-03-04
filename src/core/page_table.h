#ifndef DSF_MMU_CORE_PAGE_TABLE_H_
#define DSF_MMU_CORE_PAGE_TABLE_H_

#include <cstdint>
#include <vector>

#include "dsf_mmu/mmu_types.h"

namespace dsf_mmu {
namespace core {

struct PageEntry {
    int physical_block_id;
    mmu_tier_t tier;
};

class PageTable {
public:
    // Append a new mapping for the logical block, returning false if the logical block ID is not sequential (i.e., must append at the end)
    bool append(int physical_block_id, mmu_tier_t tier);

    // Get the page entry for the given logical block ID, returning false if the logical block ID is out of bounds (i.e., not allocated)
    bool get(int logical_block_id, PageEntry* out_entry) const;

    // Update the page entry for the given logical block ID, returning false if the logical block ID is out of bounds (i.e., not allocated)
    bool set(int logical_block_id, PageEntry entry);

    uint32_t size() const;

    // Get a const reference to the internal vector of page entries (for iteration or debugging purposes)
    const std::vector<PageEntry>& entries() const;
    std::vector<PageEntry>& mutable_entries();

private:
    // The page table entries are stored in a vector, where the index corresponds to the logical block ID
    std::vector<PageEntry> entries_;
};

}  // namespace core
}  // namespace dsf_mmu

#endif  // DSF_MMU_CORE_PAGE_TABLE_H_
