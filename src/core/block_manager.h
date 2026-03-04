#ifndef DSF_MMU_CORE_BLOCK_MANAGER_H_
#define DSF_MMU_CORE_BLOCK_MANAGER_H_

#include <cstdint>
#include <vector>

namespace dsf_mmu {
namespace core {

class BlockManager {
public:
    explicit BlockManager(uint32_t total_blocks);

    bool allocate(int* out_physical_block_id);
    bool allocate_specific(int physical_block_id);
    bool retain(int physical_block_id);
    bool release(int physical_block_id);

    bool is_valid(int physical_block_id) const;
    int ref_count(int physical_block_id) const;

    uint32_t free_blocks() const;
    uint32_t capacity() const;

private:
    std::vector<int> ref_counts_;
    std::vector<int> free_list_;
};

}  // namespace core
}  // namespace dsf_mmu

#endif  // DSF_MMU_CORE_BLOCK_MANAGER_H_
