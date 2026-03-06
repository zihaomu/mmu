#include "core/block_directory.h"

namespace dsf_mmu {
namespace core {

namespace {

BlockMeta MakeFreshMeta() {
    return BlockMeta{
        true,
        MMU_TIER_L1,
        false,
        false,
        0u,
        0u,
        kInvalidFrameId,
        kInvalidSlotId,
        false,
    };
}

}  // namespace

BlockDirectory::BlockDirectory(uint32_t capacity)
    : metas_(capacity, MakeDefaultMeta()) {}

bool BlockDirectory::is_valid(int block_id) const {
    return block_id >= 0 && block_id < static_cast<int>(metas_.size());
}

bool BlockDirectory::initialize_fresh_block(int block_id) {
    if (!is_valid(block_id)) {
        return false;
    }

    metas_[block_id] = MakeFreshMeta();
    return true;
}

void BlockDirectory::reset(int block_id) {
    if (!is_valid(block_id)) {
        return;
    }

    metas_[block_id] = MakeDefaultMeta();
}

bool BlockDirectory::snapshot(int block_id, BlockMeta* out_meta) const {
    if (!is_valid(block_id) || out_meta == nullptr) {
        return false;
    }

    *out_meta = metas_[block_id];
    return true;
}

const BlockMeta* BlockDirectory::get(int block_id) const {
    if (!is_valid(block_id)) {
        return nullptr;
    }

    return &metas_[block_id];
}

BlockMeta* BlockDirectory::mutable_get(int block_id) {
    if (!is_valid(block_id)) {
        return nullptr;
    }

    return &metas_[block_id];
}

bool BlockDirectory::set_tier(int block_id, mmu_tier_t tier) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr) {
        return false;
    }

    meta->tier = tier;
    return true;
}

bool BlockDirectory::set_resident_in_l1(int block_id, bool resident) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr) {
        return false;
    }

    meta->resident_in_l1 = resident;
    return true;
}

bool BlockDirectory::set_dirty(int block_id, bool dirty) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr) {
        return false;
    }

    meta->dirty = dirty;
    return true;
}

bool BlockDirectory::set_pin_count(int block_id, uint32_t pin_count) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr) {
        return false;
    }

    meta->pin_count = pin_count;
    return true;
}

bool BlockDirectory::increment_pin_count(int block_id) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr) {
        return false;
    }

    ++meta->pin_count;
    return true;
}

bool BlockDirectory::decrement_pin_count(int block_id) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr || meta->pin_count == 0) {
        return false;
    }

    --meta->pin_count;
    return true;
}

bool BlockDirectory::set_last_access_epoch(int block_id, uint64_t epoch) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr) {
        return false;
    }

    meta->last_access_epoch = epoch;
    return true;
}

bool BlockDirectory::set_l1_frame_id(int block_id, int frame_id) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr) {
        return false;
    }

    meta->l1_frame_id = frame_id;
    return true;
}

bool BlockDirectory::set_l2_slot_id(int block_id, int slot_id) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr) {
        return false;
    }

    meta->l2_slot_id = slot_id;
    return true;
}

bool BlockDirectory::set_has_l2_backing(int block_id, bool has_l2_backing) {
    BlockMeta* meta = mutable_get(block_id);
    if (meta == nullptr) {
        return false;
    }

    meta->has_l2_backing = has_l2_backing;
    return true;
}

BlockMeta BlockDirectory::MakeDefaultMeta() {
    return BlockMeta{
        false,
        MMU_TIER_L2,
        false,
        false,
        0u,
        0u,
        kInvalidFrameId,
        kInvalidSlotId,
        false,
    };
}

}  // namespace core
}  // namespace dsf_mmu
