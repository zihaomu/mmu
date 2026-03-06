#ifndef DSF_MMU_CORE_CONTEXT_H_
#define DSF_MMU_CORE_CONTEXT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "core/block_directory.h"
#include "core/block_manager.h"
#include "core/page_table.h"
#include "dsf_mmu/mmu_types.h"

namespace dsf_mmu {
namespace core {

class Context {
public:
    Context(std::shared_ptr<BlockManager> block_manager,
            std::shared_ptr<BlockDirectory> block_directory,
            uint32_t block_size_bytes);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) noexcept = default;
    Context& operator=(Context&&) noexcept = default;

    mmu_status_t AllocateBlocks(int num_blocks);
    mmu_status_t AllocateBlocks(int num_blocks, std::vector<int>* out_new_block_ids);
    mmu_status_t ReleaseLogicalBlock(int logical_block);
    mmu_status_t ResolveRouting(int logical_block, physical_route_t* out_route) const;
    mmu_status_t UpdateMapping(int logical_block, int new_block_id);
    mmu_status_t PrepareBlockForWrite(int logical_block, int* out_block_id);

    Context Fork() const;
    uint32_t AllocatedLogicalBlocks() const;

private:
    void ReleaseAllBlocks();

    std::shared_ptr<BlockManager> block_manager_;
    std::shared_ptr<BlockDirectory> block_directory_;
    PageTable page_table_;
    uint32_t block_size_bytes_;
};

}  // namespace core
}  // namespace dsf_mmu

#endif  // DSF_MMU_CORE_CONTEXT_H_
