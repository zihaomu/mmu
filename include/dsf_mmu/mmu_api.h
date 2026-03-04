#ifndef DSF_MMU_MMU_API_H_
#define DSF_MMU_MMU_API_H_

#include <stdint.h>

#include "dsf_mmu/mmu_config.h"
#include "dsf_mmu/mmu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// MMU API functions
mmu_status_t mmu_init(const mmu_config_t* config, mmu_handle_t* out_mmu);

// Clean up all resources associated with the MMU instance. After this call, the mmu_handle_t is no longer valid and should not be used.
void mmu_shutdown(mmu_handle_t mmu);

// Context management functions
mmu_status_t mmu_create_context(mmu_handle_t mmu, ctx_handle_t* out_ctx);

// Clean up all resources associated with the context. After this call, the ctx_handle_t is no longer valid and should not be used.
void mmu_destroy_context(mmu_handle_t mmu, ctx_handle_t ctx);

// Fork a context, creating a new context that shares the same logical-to-physical mappings as the source context. The new context should have its own reference counts for the physical blocks, and modifications to the mappings in one context should not affect the other context.
mmu_status_t mmu_fork_context(mmu_handle_t mmu, ctx_handle_t src_ctx, ctx_handle_t* out_ctx);

// Memory management functions
mmu_status_t mmu_allocate_blocks(mmu_handle_t mmu, ctx_handle_t ctx, uint32_t num_blocks);

// Release a logical block, decrementing the reference count of the corresponding physical block. If the reference count reaches zero, the physical block should be freed and made available for future allocations.
mmu_status_t mmu_release_logical_block(mmu_handle_t mmu, ctx_handle_t ctx, uint32_t logical_block);

// Resolve the physical route for a given logical block, returning the physical block ID, tier, and byte offset within the block. If the logical block is not allocated, return an appropriate error code.
mmu_status_t mmu_resolve_routing(mmu_handle_t mmu,
                                 ctx_handle_t ctx,
                                 uint32_t logical_block,
                                 physical_route_t* out_route);

// Resolve the physical routes for a batch of logical blocks, returning an array of physical routes corresponding to each logical block. If any logical block in the batch is not allocated, return an appropriate error code and do not modify the output array.
mmu_status_t mmu_resolve_routing_batch(mmu_handle_t mmu,
                                       ctx_handle_t ctx,
                                       const uint32_t* logical_blocks,
                                       uint32_t num_blocks,
                                       physical_route_t* out_routes);

// Update the mapping of a logical block to a new physical block and tier. This can be used for remapping or migrating data between different memory tiers. If the logical block is not allocated, return an appropriate error code.
mmu_status_t mmu_prepare_block_for_write(mmu_handle_t mmu,
                                         ctx_handle_t ctx,
                                         uint32_t logical_block,
                                         int32_t* out_physical_block);

// Map a logical block to a host pointer for CPU access, with the specified map flags (e.g., read/write permissions). This should ensure that the corresponding physical block is resident in L1 memory and return a pointer to the block's data. If the logical block is not allocated or if the map flags are invalid, return an appropriate error code.
mmu_status_t mmu_map_logical_block(mmu_handle_t mmu,
                                   ctx_handle_t ctx,
                                   uint32_t logical_block,
                                   uint32_t map_flags,
                                   mmu_mapped_block_t* out_mapped_block);

// Handle multiple blocks in one call with the same map flags for all blocks.
// On failure, previously mapped entries in this call are rolled back via unmap.
mmu_status_t mmu_map_logical_blocks(mmu_handle_t mmu,
                                    ctx_handle_t ctx,
                                    const uint32_t* logical_blocks,
                                    uint32_t num_blocks,
                                    uint32_t map_flags,
                                    mmu_mapped_block_t* out_mapped_blocks);

// Unmap a previously mapped logical block, decrementing the reference count of the corresponding physical block and allowing it to be evicted from L1 if necessary. The mmu_mapped_block_t structure should contain the physical block ID and map flags that were returned by the corresponding mmu_map_logical_block call. If the logical block is not currently mapped, return an appropriate error code.
mmu_status_t mmu_unmap_logical_block(mmu_handle_t mmu,
                                     ctx_handle_t ctx,
                                     const mmu_mapped_block_t* mapped_block);

// Unmap multiple blocks in one call. The function continues processing all blocks
// and returns the first error code (if any).
mmu_status_t mmu_unmap_logical_blocks(mmu_handle_t mmu,
                                      ctx_handle_t ctx,
                                      const mmu_mapped_block_t* mapped_blocks,
                                      uint32_t num_blocks);

// Getters for testing and debugging purposes
uint32_t mmu_get_free_block_count(mmu_handle_t mmu);

// Get the reference count of a physical block, which indicates how many logical blocks are currently mapped to it. This can be used for testing and debugging purposes to verify that reference counting is working correctly.
int32_t mmu_get_ref_count(mmu_handle_t mmu, int32_t physical_block_id);

// Get the total number of logical blocks allocated in the context, which can be used for testing and debugging purposes to verify that block allocation and release are working correctly.
uint32_t mmu_get_logical_block_count(mmu_handle_t mmu, ctx_handle_t ctx);

#ifdef __cplusplus
}
#endif

#endif  // DSF_MMU_MMU_API_H_
