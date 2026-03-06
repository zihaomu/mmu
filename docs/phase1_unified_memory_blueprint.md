# MMU 一阶段统一内存蓝图

## 1. 文档目标

本文档用于将当前 MMU 项目的一阶段目标、架构边界、核心数据结构、状态机、API 约束、迁移路径与验收标准正式化。

本文档聚焦：

- 一阶段：`CPU RAM + SSD` 的统一分页内存系统
- 二阶段预留：`CUDA VRAM` 作为更高层级的热数据驻留层

本文档不追求直接描述最终全部实现细节，而是给出一个可以稳定推进一阶段重构与交付的工程蓝图。

---

## 2. 一阶段目标

一阶段的目标不是“用两块堆内存模拟分层”，而是构建一个真正可扩展的统一内存分页系统：

- 对上层推理引擎暴露稳定的逻辑块视图
- 在内部以固定大小 Block 管理 KV Cache
- 支持 `Context Fork + Copy-on-Write`
- 支持 `L1(CPU RAM)` 与 `L2(SSD)` 之间的换入换出
- 支持 L1 驻留窗口受限时的淘汰与提升
- 为二阶段加入 `L0(CUDA VRAM)` 预留稳定扩展点

### 一阶段必须满足的能力

1. `LVID -> BlockId` 的逻辑映射稳定
2. Block 在不同层级迁移时身份不变
3. 只有写时复制才产生新的 Block
4. `fork` 不复制块数据，只增加引用计数
5. `unmap(write)` 不强制同步写回 SSD
6. 淘汰脏块时才写回 L2
7. L2 必须是真实文件后端，而不是另一块 DRAM

---

## 3. 一阶段非目标

以下内容不属于一阶段必须完成项：

- CUDA 显存管理
- Pinned host memory 优化
- io_uring 异步 I/O
- 跨进程持久化恢复
- 多副本冗余
- NUMA 感知优化
- 高级预取策略

这些内容应建立在一阶段元数据与分层模型稳定之后，再逐步引入。

---

## 4. 当前实现的定位

当前项目已经具备以下原型能力：

- 固定块分配
- 逻辑块到物理块的页表映射
- `fork + CoW`
- L1/L2 概念上的驻留切换
- Lazy commit 与 chunk 化内存提交

但当前实现的本质更接近：

> “具备分页和 CoW 语义的内存分页器原型”

而不是：

> “完整的 CPU RAM ↔ SSD 统一内存架构”

主要原因：

- 当前 `L2` 仍然是堆内存模拟，而不是真实 SSD 后端
- `arch` 只是配置项，未进入真实后端分流
- Tier 元数据仍分散在各个 Context 页表中
- `unmap(write)` 默认执行写穿透式同步到 L2
- 物理路由语义仍混合了“块身份”和“驻留位置”

---

## 5. 架构设计原则

### 5.1 身份与位置分离

一阶段必须明确区分四种概念：

- `LVID`：Context 内的逻辑块编号
- `BlockId`：全局块身份
- `FrameId`：L1 内存驻留帧编号
- `SlotId`：L2 SSD 槽位编号

它们不能混用。

#### 基本原则

- `BlockId` 代表“数据对象是谁”
- `FrameId` 代表“当前驻留在 RAM 的哪里”
- `SlotId` 代表“SSD 上备份在哪个位置”
- `LVID` 代表“某个 Context 第几个逻辑块”

### 5.2 Block 身份稳定

Block 在 `L1 <-> L2` 迁移时，`BlockId` 不变。

只有以下情况允许创建新 `BlockId`：

- `fork` 后共享块被写入，触发 CoW
- 未来如果支持显式 clone/copy API

### 5.3 页表只做逻辑映射

Context 页表不应该承载全局 Tier 状态。

页表只负责：

- `LVID -> BlockId`

不负责：

- 当前 tier
- resident 状态
- dirty 状态
- pin 计数
- L2 文件位置

这些应该统一由全局目录维护。

### 5.4 写回策略必须是 write-back

一阶段采用 `write-back`，而不是 `write-through`。

这意味着：

- `map(write)` 允许直接在 L1 上修改
- `unmap(write)` 只标记 dirty
- 仅在淘汰 dirty block 时写回 L2
- 必要时可提供显式 flush API，但不是默认路径

### 5.5 失败必须可回滚

以下流程必须具备事务化语义：

- CoW
- 提升 `L2 -> L1`
- 淘汰 `L1 -> L2`
- batch resolve / batch map

任何一步失败后，系统状态必须保持一致，不允许出现：

- 引用计数已变，但映射未完成
- 页表已换新块，但数据未复制成功
- 输出数组被部分写入

---

## 6. 目标架构总览

建议将运行时拆为以下核心模块：

### 6.1 ContextTable

负责每个 Context 的逻辑页表。

职责：

- 创建/销毁 Context
- 维护 `LVID -> BlockId`
- `fork` 时复制逻辑映射

### 6.2 BlockDirectory

负责所有 Block 的全局元数据。

职责：

- `refcount`
- `dirty`
- `resident`
- `pin_count`
- `l1_frame_id`
- `l2_slot_id`
- `last_access_epoch`
- `zero_fill`
- `io_state`

### 6.3 L1FramePool

负责 CPU RAM 驻留窗口管理。

职责：

- 分配和回收 Frame
- 管理 pin_count
- LRU 或近似 LRU
- zero-fill
- 选取 eviction victim

### 6.4 L2SlotPool

负责 SSD 槽位分配。

职责：

- 分配/回收 Slot
- 将 `SlotId` 映射到文件偏移
- 维护 backing file 元信息

### 6.5 L2FileBackend

负责真实 SSD 文件读写。

一阶段建议实现：

- 单一 sparse file
- `pread/pwrite`
- 固定偏移块读写

后续可升级为：

- `mmap`
- `io_uring`
- 多文件或多设备

### 6.6 SwapEngine

负责跨层级迁移。

一阶段先做同步版本。

职责：

- `ensure_resident(block_id)`
- `evict_one_frame()`
- `flush_if_dirty(block_id)`
- `promote_from_l2(block_id)`

### 6.7 MapRegistry

负责管理 map/unmap 生命周期。

职责：

- 为一次 map 生成稳定 cookie
- 跟踪 `cookie -> block_id/frame_id/map_flags`
- unmap 时根据 cookie 回收 pin

这样可以避免通过“当前页表是否仍指向某个物理块”来判断句柄是否合法。

---

## 7. 核心数据结构蓝图

以下为建议的数据结构方向，不要求字段名完全一致。

### 7.1 逻辑页表

```cpp
struct LogicalEntry {
    uint32_t block_id;
};
```

### 7.2 Block 元数据

```cpp
struct BlockMeta {
    uint32_t refcount;
    uint32_t pin_count;
    uint32_t l1_frame_id;
    uint32_t l2_slot_id;
    uint64_t last_access_epoch;
    bool resident_in_l1;
    bool dirty;
    bool zero_fill;
    bool has_l2_copy;
    IoState io_state;
};
```

说明：

- `l1_frame_id` 仅在 resident 时有效
- `l2_slot_id` 仅在已有 SSD backing 时有效
- `zero_fill` 表示逻辑存在但尚未 materialize
- `has_l2_copy` 用于区分是否可以直接丢弃 clean frame

### 7.3 L1 帧元数据

```cpp
struct FrameMeta {
    uint32_t block_id;
    uint32_t pin_count;
    uint64_t last_access_epoch;
};
```

### 7.4 L2 槽位元数据

```cpp
struct SlotMeta {
    uint32_t block_id;
    uint64_t file_offset_bytes;
};
```

### 7.5 映射注册表

```cpp
struct MapToken {
    uint64_t cookie;
    uint32_t block_id;
    uint32_t frame_id;
    uint32_t logical_block_id;
    uint32_t map_flags;
};
```

---

## 8. Block 生命周期状态机

建议将 Block 生命周期收敛为以下状态：

### 8.1 `UNMATERIALIZED_ZERO`

- 刚分配
- 逻辑上已存在
- 尚未拥有 L1 frame
- 尚未拥有 L2 slot
- 读取时返回零页语义

### 8.2 `RESIDENT_CLEAN`

- 已驻留在 L1
- 数据有效
- 当前未被修改或已写回

### 8.3 `RESIDENT_DIRTY`

- 已驻留在 L1
- 数据被写入
- 尚未同步到 L2

### 8.4 `NONRESIDENT_CLEAN`

- 不在 L1
- 在 L2 中有有效副本

### 8.5 过渡态

- `PROMOTING`
- `EVICTING`
- `COW_COPYING`

这些状态仅在内部短暂存在，用于保证失败可回滚。

---

## 9. 核心流程设计

## 9.1 allocate_blocks

目标：

- 为 Context 追加逻辑块
- 只分配 `BlockId`
- 不立即 materialize L1 或 L2

流程：

1. 申请新的 `BlockId`
2. 初始化 `BlockMeta`
3. 设置 `refcount = 1`
4. 设置 `zero_fill = true`
5. 在 Context 页表末尾追加 `block_id`

注意：

- 不应在 `allocate_blocks` 时默认占用 L1 frame
- 不应在 `allocate_blocks` 时默认创建 L2 槽位

## 9.2 map READ

目标：

- 返回可读 host 指针

流程：

1. `LVID -> BlockId`
2. 查询 `BlockMeta`
3. 若 block 已 resident：
   - 增加 pin_count
   - 更新时间戳
   - 返回 L1 指针
4. 若 block 非 resident：
   - 选择或回收一个 L1 frame
   - 若 block 有 L2 backing，则从 SSD 读入
   - 若 block 处于 `zero_fill`，则直接零填充
   - 建立 `BlockId -> FrameId`
   - 返回指针

## 9.3 map WRITE

目标：

- 返回可写 host 指针
- 对共享块执行 CoW

流程：

1. 先解析 `LVID -> BlockId`
2. 若 `refcount == 1`：
   - 确保 resident
   - 标记可写
3. 若 `refcount > 1`：
   - 分配新 `BlockId`
   - 为新块准备内容来源
   - 将原块内容复制到新块
   - 原子更新当前 Context 页表到新块
   - 原块 refcount--
   - 新块 refcount = 1
4. 返回新块或原块的 resident 指针

关键点：

- CoW 必须在“数据复制成功”之后再提交页表更新
- 不允许先更新映射，再补做复制

## 9.4 unmap READ

流程：

- 仅通过 map token 回收 pin
- 更新访问时间

## 9.5 unmap WRITE

流程：

- 通过 map token 回收 pin
- 设置 `dirty = true`
- 更新时间戳

注意：

- 一阶段不应默认执行写穿透到 SSD

## 9.6 evict

目标：

- 在 L1 frame 不够时回收 resident frame

流程：

1. 选择未 pin 的 victim frame
2. 若 victim 对应 block 是 dirty：
   - 若无 L2 slot，则先分配 slot
   - 写回 SSD
   - 清除 dirty
3. 解除 `BlockId -> FrameId`
4. 回收 frame

## 9.7 promote

目标：

- 将非 resident block 提升到 L1

流程：

1. 分配或回收一个 L1 frame
2. 从 SSD 读取到 frame
3. 更新 resident 状态

## 9.8 fork

目标：

- 零拷贝复制逻辑上下文

流程：

1. 创建新 Context
2. 复制逻辑页表中的 `BlockId`
3. 对每个 block 执行 `refcount++`

注意：

- 不复制 frame
- 不复制 slot
- 不复制块数据

---

## 10. 后端设计

## 10.1 L1：RAM Backend

一阶段建议保留 chunk 化分配思想，但只服务于 L1 frame。

职责：

- 按 chunk 提交 RAM
- 维护 `FrameId -> host_ptr`
- 支持 zero-fill
- 支持 frame 回收

说明：

- `chunk_block_count` 应只描述 L1 frame 的分配粒度
- 它不应该再承担 L2 的含义

## 10.2 L2：File-backed SSD Backend

一阶段建议实现方式：

- 单文件
- 稀疏文件
- 固定偏移
- 同步 `pread/pwrite`

建议配置项：

- `l2_backing_path`
- `l2_slot_count`
- `block_size_bytes`

文件大小：

```text
file_size = l2_slot_count * block_size_bytes
```

偏移计算：

```text
offset = slot_id * block_size_bytes
```

### 一阶段不建议直接上 io_uring 的原因

- 当前项目先要稳定元数据一致性
- 异步 I/O 会立即放大状态机复杂度
- 同步实现更利于先完成正确性验证

---

## 11. API 演进建议

## 11.1 保留的外部 API

以下 API 可继续保留：

- `mmu_init`
- `mmu_shutdown`
- `mmu_create_context`
- `mmu_destroy_context`
- `mmu_fork_context`
- `mmu_allocate_blocks`
- `mmu_resolve_routing`
- `mmu_map_logical_block`
- `mmu_unmap_logical_block`
- batch map/unmap 接口

## 11.2 建议调整的配置结构

当前命名容易让人把“Block 身份”和“驻留帧数量”混淆，建议逐步调整：

- `l1_block_count` -> `max_block_count`
- `l1_resident_block_limit` -> `l1_frame_count`
- `l2_block_count` -> `l2_slot_count`
- 新增 `l2_backing_path`

`chunk_block_count` 保留，但限定含义为：

- L1 chunk 的 frame 数量

## 11.3 建议调整的路由结构

`physical_route_t` 不应再用 `pbid * block_size` 伪装为“块内偏移”。

建议演进方向：

```cpp
struct physical_route_t {
    mmu_tier_t tier;
    uint32_t block_id;
    uint32_t frame_id;
    uint64_t device_offset_bytes;
    uint32_t flags;
};
```

说明：

- `tier` 表示当前主要访问层
- `block_id` 表示稳定身份
- `frame_id` 在 resident 时有效
- `device_offset_bytes` 在 L2 backing 存在时有效

## 11.4 flush 接口

一阶段可以不强制引入，但建议预留：

- `mmu_flush_context`
- `mmu_flush_block`

作用：

- 在测试和调试时验证 dirty/write-back 行为

---

## 12. 并发模型建议

一阶段允许保守实现，但需要明确后续扩展方向。

### 12.1 一阶段可接受方案

- 全局运行时大锁
- 单次 API 调用内部串行完成

### 12.2 一阶段应避免的问题

- map 与 unmap 生命周期无法追踪
- eviction 时依赖遍历所有 Context 更新 tier
- 失败后状态不一致

### 12.3 二阶段建议演进

- Context 锁
- Block 元数据锁
- Frame pool 锁
- 后端 I/O 队列锁

---

## 13. 迁移方案

建议分四步完成一阶段重构。

## 13.1 P0：元数据解耦

目标：

- 将 `tier/resident/dirty/pin/l2位置` 从 Context 页表中剥离

要点：

- 页表项只保留 `BlockId`
- 新增 `BlockDirectory`
- 引入 `FrameId` 和 `SlotId`

完成标志：

- Context 不再负责全局 tier 传播
- Block 在迁移时不再扫描所有 Context

## 13.2 P1：真实 L2 backend

目标：

- 将当前 L2 堆内存模拟替换为文件后端

要点：

- 新增 L2 file backend
- 用 `pread/pwrite` 完成固定块读写
- slot allocator 独立于 L1 frame allocator

完成标志：

- L2 数据来源于 backing file，而不是 `new[]`

## 13.3 P2：写回与回滚语义

目标：

- 改写 `unmap(write)`、CoW、evict 的一致性行为

要点：

- `unmap(write)` 只置 dirty
- eviction 时才 write-back
- CoW 改为事务式提交流程
- batch 输出失败时保持原子性

完成标志：

- 失败测试全部通过
- 不再依赖 write-through 行为

## 13.4 P3：接口收口与二阶段预留

目标：

- 将 L1/L2 语义稳定下来，为引入 L0 铺路

要点：

- 清理命名
- 明确 route 结构
- 预留 L0 backend 接口

完成标志：

- 加入 CUDA 层时无需推翻一阶段结构

---

## 14. 代码落点建议

建议目录组织如下：

```text
src/
├── core/
│   ├── block_directory.h
│   ├── block_directory.cpp
│   ├── context_table.h
│   ├── context_table.cpp
│   ├── frame_pool.h
│   ├── frame_pool.cpp
│   ├── slot_pool.h
│   ├── slot_pool.cpp
│   ├── swap_engine.h
│   └── swap_engine.cpp
├── backends/
│   ├── l1_ram_backend.h
│   ├── l1_ram_backend.cpp
│   ├── l2_file_backend.h
│   └── l2_file_backend.cpp
├── runtime/
│   ├── mmu_runtime.h
│   └── mmu_runtime.cpp
└── mmu_api.cpp
```

其中：

- `mmu_api.cpp` 只保留 ABI 封装
- 复杂逻辑不再堆积在单个文件里

---

## 15. 验收测试蓝图

建议新增或重写以下测试，作为一阶段完成标准。

### 15.1 分配与物化

- `AllocateDoesNotCommitFrameOrSlot`
- `ReadMapMaterializesZeroFilledBlock`

### 15.2 写回策略

- `WriteMapMarksDirtyButDoesNotWriteBackImmediately`
- `DirtyEvictionWritesBackToL2`
- `CleanEvictionDoesNotRewriteL2`

### 15.3 提升与恢复

- `PromotionLoadsBytesFromL2Correctly`
- `ReReadAfterEvictionPreservesContent`

### 15.4 CoW

- `ForkSharesBlockIdWithoutCopy`
- `WriteAfterForkCreatesNewBlockId`
- `CowFailureRollsBackAtomically`

### 15.5 原子性

- `BatchResolveFailureLeavesOutputUntouched`
- `L2SlotExhaustionFailsAtomically`
- `EvictionIoFailureLeavesResidentStateConsistent`

### 15.6 生命周期

- `DestroySharedContextOnlyDropsRefcount`
- `ReleaseLastReferenceReclaimsFrameAndSlot`

---

## 16. 一阶段完成标准

只有满足以下条件，才应认为一阶段完成：

1. `L2` 已替换为真实 SSD 文件后端
2. 页表不再存储 tier/resident 元数据
3. `unmap(write)` 不再默认写穿透
4. CoW 具备失败回滚能力
5. eviction / promotion 具备一致性保障
6. 所有关键路径有对应单元测试
7. 二阶段加入 CUDA 时无需推翻 `BlockId/FrameId/SlotId` 模型

---

## 17. 二阶段预留

二阶段引入 `L0 = CUDA VRAM` 时，建议遵循相同抽象：

- `BlockId` 不变
- 新增 `l0_frame_id`
- `SwapEngine` 扩展为多层迁移
- `Map API` 可保留 host map，也可增加 device route 查询

关键点是：

> 二阶段应建立在一阶段“身份稳定、位置解耦、写回正确、失败可回滚”的基础之上。

如果一阶段仍将 `BlockId`、`FrameId`、`SlotId` 混为一谈，那么二阶段会显著放大复杂度与 bug 面。

---

## 18. 总结

当前项目最有价值的部分，是已经把以下最难的“语义原型”跑出来了：

- 固定块分页
- 逻辑/物理解耦
- `fork + CoW`
- 基础换页语义

下一步的关键不是继续在现有大单体实现上叠功能，而是完成一次明确的架构收敛：

- 页表只存逻辑映射
- 全局目录统一维护块元数据
- L1 与 L2 使用不同资源模型
- L2 使用真实文件后端
- 写回改为 write-back
- 关键路径全部具备失败回滚能力

做到这一步后，本项目的一阶段才会真正从“能跑的原型”升级为“可进入二阶段 CUDA 扩展的统一内存基座”。
