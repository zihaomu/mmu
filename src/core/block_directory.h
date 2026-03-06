#ifndef DSF_MMU_CORE_BLOCK_DIRECTORY_H_
#define DSF_MMU_CORE_BLOCK_DIRECTORY_H_

#include <cstdint>
#include <vector>

#include "dsf_mmu/mmu_types.h"

namespace dsf_mmu {
namespace core {

constexpr int kInvalidBlockId = -1;
constexpr int kInvalidFrameId = -1;
constexpr int kInvalidSlotId = -1;

struct BlockMeta {
    bool allocated;
    mmu_tier_t tier;
    bool resident_in_l1;
    bool dirty;
    uint32_t pin_count;
    uint64_t last_access_epoch;
    int l1_frame_id;
    int l2_slot_id;
    bool has_l2_backing;
};

class BlockDirectory {
public:
    explicit BlockDirectory(uint32_t capacity);

    bool is_valid(int block_id) const;

    bool initialize_fresh_block(int block_id);
    void reset(int block_id);

    bool snapshot(int block_id, BlockMeta* out_meta) const;
    const BlockMeta* get(int block_id) const;
    BlockMeta* mutable_get(int block_id);

    bool set_tier(int block_id, mmu_tier_t tier);
    bool set_resident_in_l1(int block_id, bool resident);
    bool set_dirty(int block_id, bool dirty);
    bool set_pin_count(int block_id, uint32_t pin_count);
    bool increment_pin_count(int block_id);
    bool decrement_pin_count(int block_id);
    bool set_last_access_epoch(int block_id, uint64_t epoch);
    bool set_l1_frame_id(int block_id, int frame_id);
    bool set_l2_slot_id(int block_id, int slot_id);
    bool set_has_l2_backing(int block_id, bool has_l2_backing);

private:
    static BlockMeta MakeDefaultMeta();

    std::vector<BlockMeta> metas_;
};

}  // namespace core
}  // namespace dsf_mmu

#endif  // DSF_MMU_CORE_BLOCK_DIRECTORY_H_
