
# 测试目标

第一阶段：基础组件单元测试 (Unit Tests)
目标：验证核心数据结构（页表、引用计数、空闲链表）的绝对正确。

test_allocator_basic (基础分配与释放)

操作：初始化一个仅有 100 个 Block 的 L1 内存池。连续申请 10 个 Block，然后释放其中 5 个，再申请 5 个。

断言 (Assert)：

第一次申请的物理 ID 应为 0-9。

新申请的 5 个 Block 必须复用刚才释放的 5 个物理 ID（验证空闲链表 FreeList 的 O(1) 回收与分配）。

验证系统报告的剩余可用 Block 数完全一致（杜绝内存泄漏）。

test_page_table_mapping (逻辑到物理的路由映射)

操作：创建一个 Context，分配 3 个逻辑块（LVID 0, 1, 2）。打乱顺序将其映射到物理块（PBID 8, 2, 5）。

断言：调用 mmu_resolve_routing(mmu, ctx, LVID)，必须准确返回对应的硬件设备（L0/L1）和物理偏移量。

test_out_of_bounds (越界与异常处理)

操作：请求访问尚未分配的逻辑块 LVID 10。

断言：系统必须拦截并返回特定的错误码（如 MMU_ERR_PAGE_FAULT 或 MMU_ERR_UNALLOCATED），绝不能发生段错误（Segfault）。

第二阶段：高级机制测试 (CoW & Forking)
目标：验证 Agent 多分支场景下，最重要的写时复制（Copy-on-Write）逻辑。

test_sys_fork_zero_copy (零拷贝 Fork 验证)

操作：创建 Context A，分配 5 个 Block。调用 mmu_fork_context(mmu, A, &B) 生成 Context B。

断言： 

B 的分配耗时应接近 0（只复制页表）。

遍历 B 的逻辑块 0-4，其指向的物理块必须与 A 完全相同。

这 5 个物理块的引用计数 (Ref Count) 必须等于 2。

test_cow_trigger (写时复制触发机制)

操作：在上述 Fork 后，向 Context B 的最后一个逻辑块写入新数据（模拟 Token 生成）。

断言： 

触发 CoW，系统需为 B 分配一个新的物理块。

将原物理块的数据拷贝到新块。

B 的页表更新，指向新块。

原物理块的引用计数降为 1。

Context A 对应逻辑块的数据与映射保持不变（子分支写入不污染父分支）。

test_cascade_fork_and_free (级联 Fork 与安全释放)

操作：A fork 出 B，B fork 出 C。然后销毁 Context B。

断言：A 和 C 的执行不受任何影响。只有仅被 B 独占的物理块被回收，共享块的引用计数正确减 1。
