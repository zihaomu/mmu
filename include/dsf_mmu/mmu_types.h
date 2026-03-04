#ifndef DSF_MMU_MMU_TYPES_H_
#define DSF_MMU_MMU_TYPES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmu_runtime_t* mmu_handle_t;
typedef struct mmu_context_t* ctx_handle_t;

typedef enum mmu_tier_t {
    MMU_TIER_L0 = 0,
    MMU_TIER_L1 = 1,
    MMU_TIER_L2 = 2
} mmu_tier_t;

typedef enum mmu_status_t {
    MMU_STATUS_OK = 0,
    MMU_ERR_INVALID_ARGUMENT = 1,
    MMU_ERR_OUT_OF_MEMORY = 2,
    MMU_ERR_UNALLOCATED = 3,
    MMU_ERR_NOT_FOUND = 4,
    MMU_ERR_INTERNAL = 5,
    MMU_ERR_VERSION_MISMATCH = 6,
    MMU_ERR_BAD_HANDLE = 7
} mmu_status_t;

// 实际物理地址的路由信息，包括所在的内存层级、物理块ID以及在块内的字节偏移量
typedef struct physical_route_t {
    mmu_tier_t tier;
    int32_t physical_block_id;
    uint64_t byte_offset;
} physical_route_t;

typedef enum mmu_map_flags_t {
    MMU_MAP_READ = 1u << 0,
    MMU_MAP_WRITE = 1u << 1
} mmu_map_flags_t;

typedef struct mmu_mapped_block_t {
    void* host_ptr;
    uint32_t size_bytes;
    int32_t physical_block_id;
    uint32_t logical_block_id;
    uint32_t map_flags;
} mmu_mapped_block_t;

#ifdef __cplusplus
}
#endif

#endif  // DSF_MMU_MMU_TYPES_H_
