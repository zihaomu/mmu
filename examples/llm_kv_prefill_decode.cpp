#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "dsf_mmu/mmu_api.h"

namespace {

struct KvShape {
    uint32_t num_layers;
    uint32_t num_heads;
    uint32_t head_dim;
    uint32_t bytes_per_scalar;
    uint32_t tokens_per_block;
};

struct LayerCache {
    std::vector<uint32_t> logical_blocks;
};

uint32_t BytesPerTokenPerLayer(const KvShape& shape) {
    return shape.num_heads * shape.head_dim * 2u * shape.bytes_per_scalar;
}

uint32_t ComputeBlockSizeBytes(const KvShape& shape) {
    return BytesPerTokenPerLayer(shape) * shape.tokens_per_block;
}

const char* StatusToString(mmu_status_t status) {
    switch (status) {
        case MMU_STATUS_OK:
            return "MMU_STATUS_OK";
        case MMU_ERR_INVALID_ARGUMENT:
            return "MMU_ERR_INVALID_ARGUMENT";
        case MMU_ERR_OUT_OF_MEMORY:
            return "MMU_ERR_OUT_OF_MEMORY";
        case MMU_ERR_UNALLOCATED:
            return "MMU_ERR_UNALLOCATED";
        case MMU_ERR_NOT_FOUND:
            return "MMU_ERR_NOT_FOUND";
        case MMU_ERR_INTERNAL:
            return "MMU_ERR_INTERNAL";
        case MMU_ERR_VERSION_MISMATCH:
            return "MMU_ERR_VERSION_MISMATCH";
        case MMU_ERR_BAD_HANDLE:
            return "MMU_ERR_BAD_HANDLE";
        default:
            return "MMU_STATUS_UNKNOWN";
    }
}

bool CheckStatus(mmu_status_t status, const char* api_name) {
    if (status == MMU_STATUS_OK) {
        return true;
    }

    std::cerr << api_name << " failed: " << StatusToString(status) << " (" << status << ")\n";
    return false;
}

void WriteTokenSlice(uint8_t* block_ptr,
                     uint32_t block_size,
                     uint32_t bytes_per_token,
                     uint32_t token_slot,
                     uint8_t value) {
    const size_t offset = static_cast<size_t>(token_slot) * static_cast<size_t>(bytes_per_token);
    if (offset >= static_cast<size_t>(block_size)) {
        return;
    }

    size_t len = bytes_per_token;
    if (offset + len > static_cast<size_t>(block_size)) {
        len = static_cast<size_t>(block_size) - offset;
    }

    std::memset(block_ptr + offset, value, len);
}

bool Prefill(mmu_handle_t mmu,
             ctx_handle_t ctx,
             const KvShape& shape,
             uint32_t prompt_tokens,
             std::vector<LayerCache>* caches) {
    const uint32_t blocks_per_layer = (prompt_tokens + shape.tokens_per_block - 1u) / shape.tokens_per_block;
    const uint32_t total_blocks = blocks_per_layer * shape.num_layers;

    const uint32_t base_logical = mmu_get_logical_block_count(mmu, ctx);
    if (!CheckStatus(mmu_allocate_blocks(mmu, ctx, total_blocks), "mmu_allocate_blocks(prefill)")) {
        return false;
    }

    for (uint32_t layer = 0; layer < shape.num_layers; ++layer) {
        LayerCache& cache = (*caches)[layer];
        cache.logical_blocks.reserve(cache.logical_blocks.size() + blocks_per_layer);
        for (uint32_t b = 0; b < blocks_per_layer; ++b) {
            cache.logical_blocks.push_back(base_logical + layer * blocks_per_layer + b);
        }
    }

    const uint32_t bytes_per_token = BytesPerTokenPerLayer(shape);
    for (uint32_t layer = 0; layer < shape.num_layers; ++layer) {
        LayerCache& cache = (*caches)[layer];
        std::vector<mmu_mapped_block_t> mapped(blocks_per_layer);
        if (!CheckStatus(mmu_map_logical_blocks(mmu,
                                                ctx,
                                                cache.logical_blocks.data(),
                                                blocks_per_layer,
                                                MMU_MAP_READ | MMU_MAP_WRITE,
                                                mapped.data()),
                         "mmu_map_logical_blocks(prefill_write)")) {
            return false;
        }

        for (uint32_t b = 0; b < blocks_per_layer; ++b) {
            auto* block_ptr = static_cast<uint8_t*>(mapped[b].host_ptr);
            const uint32_t block_start_token = b * shape.tokens_per_block;
            const uint32_t valid_tokens = std::min(shape.tokens_per_block, prompt_tokens - block_start_token);
            for (uint32_t t = 0; t < valid_tokens; ++t) {
                const uint32_t absolute_token = block_start_token + t;
                const uint8_t tag = static_cast<uint8_t>((layer * 37u + absolute_token * 13u) & 0xFFu);
                WriteTokenSlice(block_ptr, mapped[b].size_bytes, bytes_per_token, t, tag);
            }
        }

        if (!CheckStatus(mmu_unmap_logical_blocks(mmu, ctx, mapped.data(), blocks_per_layer),
                         "mmu_unmap_logical_blocks(prefill_write)")) {
            return false;
        }
    }

    return true;
}

bool Decode(mmu_handle_t mmu,
            ctx_handle_t ctx,
            const KvShape& shape,
            uint32_t prompt_tokens,
            uint32_t decode_tokens,
            std::vector<LayerCache>* caches) {
    uint32_t total_tokens = prompt_tokens;
    const uint32_t bytes_per_token = BytesPerTokenPerLayer(shape);

    for (uint32_t step = 0; step < decode_tokens; ++step) {
        const uint32_t slot_in_block = total_tokens % shape.tokens_per_block;
        if (slot_in_block == 0) {
            const uint32_t base_logical = mmu_get_logical_block_count(mmu, ctx);
            if (!CheckStatus(mmu_allocate_blocks(mmu, ctx, shape.num_layers), "mmu_allocate_blocks(decode_extend)")) {
                return false;
            }

            for (uint32_t layer = 0; layer < shape.num_layers; ++layer) {
                (*caches)[layer].logical_blocks.push_back(base_logical + layer);
            }
        }

        // Simulate attention read: read the most recent up to 2 blocks per layer.
        uint64_t read_checksum = 0;
        for (uint32_t layer = 0; layer < shape.num_layers; ++layer) {
            LayerCache& cache = (*caches)[layer];
            const uint32_t read_count = std::min<uint32_t>(2u, static_cast<uint32_t>(cache.logical_blocks.size()));
            const uint32_t offset = static_cast<uint32_t>(cache.logical_blocks.size()) - read_count;

            std::vector<mmu_mapped_block_t> mapped(read_count);
            if (!CheckStatus(mmu_map_logical_blocks(mmu,
                                                    ctx,
                                                    cache.logical_blocks.data() + offset,
                                                    read_count,
                                                    MMU_MAP_READ,
                                                    mapped.data()),
                             "mmu_map_logical_blocks(decode_read)")) {
                return false;
            }

            for (uint32_t i = 0; i < read_count; ++i) {
                const auto* block_ptr = static_cast<const uint8_t*>(mapped[i].host_ptr);
                read_checksum += block_ptr[0];
            }

            if (!CheckStatus(mmu_unmap_logical_blocks(mmu, ctx, mapped.data(), read_count),
                             "mmu_unmap_logical_blocks(decode_read)")) {
                return false;
            }
        }

        // Write current token K/V for all layers to tail blocks in one batch map/unmap.
        std::vector<uint32_t> tail_blocks(shape.num_layers);
        for (uint32_t layer = 0; layer < shape.num_layers; ++layer) {
            tail_blocks[layer] = (*caches)[layer].logical_blocks.back();
        }

        std::vector<mmu_mapped_block_t> mapped_tail(shape.num_layers);
        if (!CheckStatus(mmu_map_logical_blocks(mmu,
                                                ctx,
                                                tail_blocks.data(),
                                                shape.num_layers,
                                                MMU_MAP_READ | MMU_MAP_WRITE,
                                                mapped_tail.data()),
                         "mmu_map_logical_blocks(decode_write)")) {
            return false;
        }

        for (uint32_t layer = 0; layer < shape.num_layers; ++layer) {
            auto* block_ptr = static_cast<uint8_t*>(mapped_tail[layer].host_ptr);
            const uint8_t tag = static_cast<uint8_t>((step * 17u + layer * 19u) & 0xFFu);
            WriteTokenSlice(block_ptr, mapped_tail[layer].size_bytes, bytes_per_token, slot_in_block, tag);
        }

        if (!CheckStatus(mmu_unmap_logical_blocks(mmu, ctx, mapped_tail.data(), shape.num_layers),
                         "mmu_unmap_logical_blocks(decode_write)")) {
            return false;
        }

        if ((step % 8u) == 0u) {
            physical_route_t route{};
            if (!CheckStatus(mmu_resolve_routing(mmu, ctx, tail_blocks[0], &route),
                             "mmu_resolve_routing(sample_tail)")) {
                return false;
            }
            std::cout << "decode step=" << step << ", checksum=" << read_checksum
                      << ", layer0_tail(pbid=" << route.physical_block_id << ", tier=" << route.tier << ")\n";
        }

        ++total_tokens;
    }

    return true;
}

bool ReleaseAllBlocks(mmu_handle_t mmu, ctx_handle_t ctx) {
    const uint32_t total = mmu_get_logical_block_count(mmu, ctx);
    for (uint32_t lvid = 0; lvid < total; ++lvid) {
        const mmu_status_t status = mmu_release_logical_block(mmu, ctx, lvid);
        if (status != MMU_STATUS_OK && status != MMU_ERR_UNALLOCATED) {
            std::cerr << "mmu_release_logical_block failed at lvid=" << lvid << ": "
                      << StatusToString(status) << "\n";
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    const KvShape shape{
        12u,  // num_layers
        8u,   // num_heads
        16u,  // head_dim
        2u,   // bytes_per_scalar (FP16/BF16)
        16u   // tokens_per_block
    };

    const uint32_t prompt_tokens = 96u;
    const uint32_t decode_tokens = 32u;

    mmu_config_t cfg = mmu_default_config();
    cfg.block_size_bytes = ComputeBlockSizeBytes(shape);
    cfg.l1_block_count = 512;
    cfg.l1_resident_block_limit = 64;  // Intentionally smaller than total to allow swap behavior.
    cfg.l2_block_count = 512;
    cfg.chunk_block_count = 32;

    mmu_handle_t mmu = nullptr;
    if (!CheckStatus(mmu_init(&cfg, &mmu), "mmu_init")) {
        return 1;
    }

    ctx_handle_t ctx = nullptr;
    if (!CheckStatus(mmu_create_context(mmu, &ctx), "mmu_create_context")) {
        mmu_shutdown(mmu);
        return 1;
    }

    std::vector<LayerCache> caches(shape.num_layers);

    std::cout << "=== Prefill phase ===\n";
    if (!Prefill(mmu, ctx, shape, prompt_tokens, &caches)) {
        mmu_destroy_context(mmu, ctx);
        mmu_shutdown(mmu);
        return 1;
    }

    std::cout << "prefill done: logical_blocks=" << mmu_get_logical_block_count(mmu, ctx)
              << ", free_pbid=" << mmu_get_free_block_count(mmu) << "\n";

    std::cout << "=== Decode phase ===\n";
    if (!Decode(mmu, ctx, shape, prompt_tokens, decode_tokens, &caches)) {
        mmu_destroy_context(mmu, ctx);
        mmu_shutdown(mmu);
        return 1;
    }

    std::cout << "decode done: logical_blocks=" << mmu_get_logical_block_count(mmu, ctx)
              << ", free_pbid=" << mmu_get_free_block_count(mmu) << "\n";

    if (!ReleaseAllBlocks(mmu, ctx)) {
        mmu_destroy_context(mmu, ctx);
        mmu_shutdown(mmu);
        return 1;
    }

    std::cout << "after release: free_pbid=" << mmu_get_free_block_count(mmu) << "\n";

    mmu_destroy_context(mmu, ctx);
    mmu_shutdown(mmu);
    return 0;
}
