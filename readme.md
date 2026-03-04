# MMU: Memory Management Unit

这个项目想做一个统一内存管理器，主要的目标是实现一个paged memory的管理器。
这个项目不仅能直接解决当前 LLM 推理框架（如 vLLM, llama.cpp）在长上下文、多并发、Agent 多分支（如 MCTS/ToT）场景下的内存碎片问题，还能通过“多级存储（Tiered Storage）”机制极大降低本地部署的硬件门槛。

以下是该项目的开发文档纲要、架构设计及详细的工程结构。

MMU 项目开发文档
1. 项目简介与核心理念
MMU 是一个专为大语言模型（LLM）推理设计的独立内存分配与管理子系统。它借鉴了操作系统的虚拟内存机制，在应用程序（推理引擎）和物理内存（显存/内存/磁盘）之间引入了页表抽象（Page Table）。

核心能力：
彻底消除碎片化： 将 KV Cache 切分为固定大小的 Block（如 16 或 32 tokens），通过非连续的物理块拼装连续的逻辑上下文。

多级存储路由 (Tiered Storage)： * 独立显卡架构 (CUDA Host): L0 (GPU VRAM) <-> L1 (CPU Pinned RAM) <-> L2 (NVMe SSD)

统一内存架构 (UMA / 移动端 / 纯 CPU): L1 (Unified RAM) <-> L2 (NVMe SSD)

零拷贝的分支机制 (Zero-Copy Fork): 原生支持基于写时复制（CoW）的 sys_fork，极速拉起多个相同前缀的 Agent 进程。

2. 核心架构与模块设计
系统分为三大核心模块：

A. 逻辑层：Context Page Table (CPT)
为每个推理请求（Sequence/Context）维护一张虚拟页表。

Logical Block ID (LVID): 推理引擎看到的连续块索引。

Physical Block ID (PBID): 底层实际分配的物理块索引。

状态位追踪：记录每个 Block 是在 L0、L1 还是 L2。

B. 分配层：Block Allocator (Slab 机制)
负责物理内存池的初始化与分配。采用按需提交（Lazy Commit）+ Chunk 机制：

初始化时不向 OS 申请实际数据页，只初始化元数据（FreeList/ChunkList）。

首次缺页时向 OS 一次申请一个 Chunk，并切分成 N 个等长 Block（N = `chunk_block_count`）。

后续优先从 FreeList O(1) 复用；只有 FreeList 为空时才再次申请新 Chunk。

维护 Free List（空闲块链表）和 Ref Count（引用计数表，用于 CoW）。

C. 调度层：Swap Engine (后台异步搬运引擎)
基于预判或 LRU 策略，在存储层级之间搬运数据。例如，当 L0 (VRAM) 满时，自动将不活跃请求的 Block 降级到 L1 (RAM) 或 L2 (SSD)。

3. 对外核心 API 接口 (C/C++ 标准接口)
为了方便接入 llama.cpp 或自研 Kernel，接口需保持极简并支持 C 语言链接（ABI 兼容）：

```C++
// mmu_api.h
// 1. 初始化系统，指定各级存储大小与架构类型
mmu_status_t mmu_init(const mmu_config_t* config, mmu_handle_t* out_mmu);

// 2. 上下文操作
mmu_status_t mmu_create_context(mmu_handle_t mmu, ctx_handle_t* out_ctx);
void mmu_destroy_context(mmu_handle_t mmu, ctx_handle_t ctx);

// 3. 极速 Fork（写时复制）
mmu_status_t mmu_fork_context(mmu_handle_t mmu, ctx_handle_t src_ctx, ctx_handle_t* out_ctx);

// 4. 内存分配与映射请求
// 为指定的上下文在逻辑上追加 N 个 Token 的空间，返回对应的物理信息
mmu_status_t mmu_allocate_blocks(mmu_handle_t mmu, ctx_handle_t ctx, uint32_t num_blocks);

// 5. 核心：逻辑到物理的转换（在算子 Launch 前调用，生成 Block Table）
// 将逻辑块索引转换为当前所在的物理硬件地址和物理块 ID
mmu_status_t mmu_resolve_routing(mmu_handle_t mmu, ctx_handle_t ctx, uint32_t logical_block, physical_route_t* out_route);
mmu_status_t mmu_resolve_routing_batch(mmu_handle_t mmu, ctx_handle_t ctx, const uint32_t* logical_blocks, uint32_t num_blocks, physical_route_t* out_routes);

// 6. 快速获取可操作指针（自动处理 CoW 和 L1/L2 迁移）
mmu_status_t mmu_map_logical_block(mmu_handle_t mmu, ctx_handle_t ctx, uint32_t logical_block, uint32_t map_flags, mmu_mapped_block_t* out_mapped_block);
mmu_status_t mmu_unmap_logical_block(mmu_handle_t mmu, ctx_handle_t ctx, const mmu_mapped_block_t* mapped_block);
mmu_status_t mmu_map_logical_blocks(mmu_handle_t mmu, ctx_handle_t ctx, const uint32_t* logical_blocks, uint32_t num_blocks, uint32_t map_flags, mmu_mapped_block_t* out_mapped_blocks);
mmu_status_t mmu_unmap_logical_blocks(mmu_handle_t mmu, ctx_handle_t ctx, const mmu_mapped_block_t* mapped_blocks, uint32_t num_blocks);
```

关键配置：`mmu_config_t::chunk_block_count`，控制每次向 OS 提交的 Chunk 大小：

`chunk_size_bytes = chunk_block_count * block_size_bytes`

例如 `block_size_bytes = 4KB` 且 `chunk_block_count = 4096` 时，每次提交 `4KB*4096 = 16MB`。

容量相关配置（上限模型）：

- `l1_block_count`：系统可分配的 PBID 总上限（全局块容量上限）。
- `l1_resident_block_limit`：L1 驻留上限，`0` 表示自动估算。
- `l2_block_count`：L2 可提交块数上限，`0` 表示自动（默认等于 `l1_block_count`）。
4. 项目结构与文件夹组织
推荐使用现代 C++ (C++17/20) 和 CMake 进行构建，并使用面向接口的编程模式（PIMPL）来隔离底层的 CUDA 或 OS 强相关代码。

Plaintext

```bash
mmu/
├── 3rdparty                    # 第三方库
│   └──googletest/              # 测试框架
│   └──libnpy/                  # 用于方便读取python的numpy数据，可能用到
│
├── CMakeLists.txt              # 构建系统配置
├── README.md
├── docs/                       # 架构图、API 说明、设计文档
│
├── include/                    # 对外暴露的公共头文件 (Header-only or Public API)
│   └── dsf_mmu/
│       ├── mmu_types.h         # 数据类型、枚举 (L0/L1/L2 定义)
│       ├── mmu_api.h           # 对外 C/C++ 核心接口
│       └── mmu_config.h        # 初始化配置结构体
│
├── src/                        # 内部实现代码 (用户不可见)
│   ├── core/                   # 核心逻辑层 (设备无关)
│   │   ├── page_table.cpp      # 页表与逻辑-物理映射实现
│   │   ├── block_manager.cpp   # 空闲块、引用计数管理 (CoW 实现)
│   │   ├── swap_scheduler.cpp  # 异步调度器，处理 L0/L1/L2 置换
│   │   └── context.cpp         # Context 句柄生命周期管理
│   │
│   ├── backends/               # 硬件相关的底层分配器 (适配多平台)
│   │   ├── backend_interface.h # 统一后端接口类 (IBackendAllocator)
│   │   ├── cuda/
│   │   │   └── cuda_allocator.cu # L0 显存池初始化与销毁
│   │   ├── cpu/
│   │   │   └── cpu_allocator.cpp # L1 内存池 (支持 Pinned Memory)
│   │   └── storage/
│   │       └── ssd_io_uring.cpp  # L2 SSD 异步读写实现 (Linux io_uring / POSIX AIO)
│   │
│   └── platform/               # 平台探测 (区分 CUDA 独立环境与 UMA 环境)
│       └── arch_detector.cpp   
│
├── tests/                      # 测试用例 (GTest)
│   ├── test_page_table.cpp     # 测试虚拟地址转换的正确性
│   ├── test_cow_fork.cpp       # 测试 fork 的引用计数与写时复制
│   └── test_swap.cpp           # 测试内存换入换出逻辑
│   └── ...                     # 根据测试目标编写测试。
│
└── examples/                   # 接入示例
    └── llm_kv_prefill_decode.cpp # 多层 Attention KV Cache 示例（prefill + decode）
```

示例运行：

```bash
cmake -S . -B build
cmake --build build -j
./build/mmu_example_llm_kv
```

