#ifndef DSF_MMU_MMU_CONFIG_H_
#define DSF_MMU_MMU_CONFIG_H_

#include <stdint.h>

#define MMU_API_VERSION 1u

typedef enum mmu_arch_t {
    MMU_ARCH_UMA = 0,
    MMU_ARCH_DISCRETE_GPU = 1,
    MMU_ARCH_CUDA_HOST = 2
} mmu_arch_t;

typedef struct mmu_config_t {
    uint32_t size;
    uint32_t api_version;
    mmu_arch_t arch;
    // Upper bound of allocatable physical blocks (PBID capacity).
    uint32_t l1_block_count;
    // Maximum number of PBIDs resident in L1 at once. 0 means auto.
    uint32_t l1_resident_block_limit;
    // Upper bound of blocks that can have committed L2 backing. 0 means auto.
    uint32_t l2_block_count;

    // Size of each block in bytes. All blocks are of the same size.
    uint32_t block_size_bytes;
    // Number of blocks committed in one lazy chunk allocation:
    // chunk_size_bytes = chunk_block_count * block_size_bytes.
    uint32_t chunk_block_count;
} mmu_config_t;

static inline mmu_config_t mmu_default_config(void) {
    mmu_config_t cfg;
    cfg.size = (uint32_t)sizeof(mmu_config_t);
    cfg.api_version = MMU_API_VERSION;
    cfg.arch = MMU_ARCH_UMA;
    cfg.l1_block_count = 1024;
    cfg.l1_resident_block_limit = 0;
    cfg.l2_block_count = 0;
    cfg.block_size_bytes = 4096;
    cfg.chunk_block_count = 4096;
    return cfg;
}

#endif  // DSF_MMU_MMU_CONFIG_H_
