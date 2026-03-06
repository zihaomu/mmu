#ifndef DSF_MMU_CORE_PAGE_TABLE_H_
#define DSF_MMU_CORE_PAGE_TABLE_H_

#include <cstdint>
#include <vector>

namespace dsf_mmu {
namespace core {

struct LogicalEntry {
    int block_id;
};

using PageEntry = LogicalEntry;

class PageTable {
public:
    bool append(int block_id);
    bool get(int logical_block_id, PageEntry* out_entry) const;
    bool set(int logical_block_id, PageEntry entry);

    uint32_t size() const;

    const std::vector<PageEntry>& entries() const;
    std::vector<PageEntry>& mutable_entries();

private:
    std::vector<PageEntry> entries_;
};

}  // namespace core
}  // namespace dsf_mmu

#endif  // DSF_MMU_CORE_PAGE_TABLE_H_
