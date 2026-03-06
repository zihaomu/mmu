#ifndef DSF_MMU_INTERNAL_MMU_TEST_API_H_
#define DSF_MMU_INTERNAL_MMU_TEST_API_H_

#include <stdint.h>

#include "dsf_mmu/mmu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmu_test_block_metadata_t {
    uint32_t allocated;
    mmu_tier_t tier;
    uint32_t resident_in_l1;
    uint32_t dirty;
    uint32_t pin_count;
    uint64_t last_access_epoch;
    int32_t l1_frame_id;
    int32_t l2_slot_id;
    uint32_t has_l2_backing;
} mmu_test_block_metadata_t;

mmu_status_t mmu_test_update_mapping(mmu_handle_t mmu,
                                     ctx_handle_t ctx,
                                     uint32_t logical_block,
                                     int32_t new_physical_block,
                                     mmu_tier_t new_tier);
mmu_status_t mmu_test_get_block_metadata(mmu_handle_t mmu,
                                         int32_t block_id,
                                         mmu_test_block_metadata_t* out_metadata);
mmu_status_t mmu_test_get_committed_page_counts(mmu_handle_t mmu,
                                                uint32_t* out_l1_pages,
                                                uint32_t* out_l2_pages);
mmu_status_t mmu_test_get_committed_chunk_counts(mmu_handle_t mmu,
                                                 uint32_t* out_l1_chunks,
                                                 uint32_t* out_l2_chunks);

#ifdef __cplusplus
}
#endif

#endif  // DSF_MMU_INTERNAL_MMU_TEST_API_H_
