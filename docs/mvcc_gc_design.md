# MVCC 版本链垃圾回收 — 架构与概要设计

## 1. 背景与问题

当前 `mvsManager` 为每个 `_id` 维护一条版本链 `std::vector<mvsVersion>`，版本区间为
`[beginTS, endTS)`：

- `txnManager::commit()` 提交时，把该键所有 `endTS == MVS_MAX_TS` 的旧版本改为
  `endTS = ts`，再追加 `beginTS = ts, endTS = MVS_MAX_TS` 的新版本。
- `mvsManager::visible()` 从链尾向前扫描，返回满足 `beginTS <= snapshot < endTS`
  且非删除的版本。
- `mvsManager::checkConflicts()` 扫描整条链，只要存在 `beginTS > snapshot` 的版本
  就判定写写冲突。
- 每次提交都会 `mvsManager::save()`，把**全部版本**写入临时文件后原子替换
  `<dbpath>.mvs`。

版本链目前只增不减，带来四个问题：

1. **内存膨胀**：同一条记录被更新 N 次，链长为 N+1，且每次 `save()` 都把全部历史
   版本序列化一遍。
2. **磁盘膨胀**：`.mvs` 文件随更新次数单调增长，启动 `load()` 时间也随之增长。
3. **查询变慢**：`visible()` 最坏 O(k)，k 为链长。
4. **冲突检测变慢**：`checkConflicts()` 最坏 O(k)。

垃圾回收（GC）的目标是：**回收任何活跃快照都不可能再访问、也不可能影响冲突判定的
版本**，使链长有界，同时不破坏快照隔离语义。

## 2. 目标与非目标

### 2.1 设计目标

- **正确性优先**：GC 后所有活跃事务的读结果、冲突判定与 GC 前完全一致。
- **空间有界**：在不存在超长事务的前提下，单键版本链长度有上限。
- **在线执行**：GC 不要求停服，与提交、读事务并发安全。
- **崩溃安全**：GC 过程中崩溃，重启后 `.mvs` 要么是 GC 前状态，要么是 GC 后状态，
  不能是中间状态。
- **低侵入**：不改变 BSON、DMS、WAL、2PC、副本集选举的现有协议；GC 仅作用于
  `mvsManager` 内部版本链及其持久化文件。
- **主备独立**：主节点与备节点各自按本地活跃快照做 GC，不需要跨节点协调。

### 2.2 非目标

- 不实现增量日志式版本存储（本次仍复用现有全量快照 `save()`）。
- 不实现在线加节点、副本集成员变更。
- 不实现 MVCC 的 schema 变更、长事务强制终止策略（只提供监控与告警接口）。
- 不改变现有 `begin/commit/rollback` 客户端协议。

## 3. 核心概念与不变式

### 3.1 安全水位（GC watermark）

记所有活跃事务的快照集合为 `S = { s1, s2, ... }`，则：

```
W = min(S)      （若 S 为空，W = currentTS）
```

`W` 称为 GC 安全水位。含义：任何活跃事务看到的版本，其可见区间必须覆盖某个
`si >= W`。

### 3.2 关键不变式

设某版本区间为 `[b, e)`，若 `e < W`，则：

1. **可见性不变式**：对任意活跃快照 `s >= W >= e`，有 `s >= e`，因此 `s` 不在
   `[b, e)` 内。该版本对任何活跃事务都不可见。
2. **冲突检测不变式**：`b < e < W <= s`，因此 `b > s` 不成立。该版本不会触发
   任何活跃事务的写写冲突。
3. **未来事务不变式**：新事务的快照 `s' >= currentTS >= W > e`，同样看不到该版本。

因此：**满足 `endTS < W` 的版本可以安全删除**。

另外，`endTS == MVS_MAX_TS` 的最新版本必须保留，它代表当前状态；即使是删除标记
（tombstone），也代表“当前不存在”，删除它会改变语义，除非满足下述特殊情况。

### 3.3 Tombstone 整体删除

若某键的版本链只剩一个 tombstone（`deleted == true, endTS == MVS_MAX_TS`），且其
`beginTS <= W`，则整条链可以删除：删除后 `visible()` 返回
`EDB_IXM_ID_NOT_EXIST`，与 tombstone 的语义等价；对未来事务也不会产生冲突。

## 4. 架构设计

### 4.1 组件划分

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| 快照采集器 | `txnManager` | 暴露活跃事务快照集合，提供 `activeSnapshots()` |
| GC 决策器 | `mvsManager` | 计算水位、扫描版本链、决定删除集合 |
| GC 执行器 | `mvsManager` | 在独占锁下修改 `_store` 并调用 `save()` |
| GC 触发器 | 后台线程 / 提交阈值 | 周期性或按版本数阈值触发 GC |
| 指标采集 | `mvsManager` | 记录 GC 轮次、删除版本数、释放字节数、水位 |

### 4.2 锁与顺序

- `mvsManager::_mutex`（`ossSLatch`）保护 `_store` 与 `_clock`：
  - `visible()` / `checkConflicts()` / `getAll()` 持有共享锁；
  - `commit()` / `save()` / GC 持有独占锁。
- 水位计算在**获取 mvs 独占锁之前**完成，避免在持锁状态下回调 `txnManager` 造成
  锁顺序反转。该顺序是安全的，因为：
  - 活跃事务的快照在 `begin()` 时分配且不再变化，都 `>= W`；
  - 新事务快照 `>= currentTS >= W`，不可能小于已算出的 `W`。
- GC 与 `commit()` 串行化：二者都需要 mvs 独占锁，且都以 `save()` 结束。

### 4.3 读取竞态的修正（前置条件）

当前 `rtn::rtnFind()` 的读路径是：**先取 `currentTS()` 作为快照，再调用
`visible()` 取共享锁**。两步之间存在窗口：GC 可能在取快照后、取共享锁前完成并删除
版本，导致该读使用过期快照。

设计上需要把“取快照”和“读版本”合并到同一个共享锁临界区内，可选方案：

- 方案 A：新增 `mvsManager::visibleCurrent(key, out)`，在共享锁内同时取
  `currentTS()` 并执行可见性判断；
- 方案 B：引入 `beginRead()` / `endRead()` 读守卫，调用方在共享锁内保存快照直到
  读取完成。

推荐方案 A，对现有调用方改动最小。该修正属于 GC 实施的第一阶段前置工作。

## 5. GC 算法概要

### 5.1 触发条件（满足任一）

1. **周期触发**：后台线程每 `T` 秒执行一次（默认建议 60 秒，可配置）。
2. **阈值触发**：`commit()` 后统计版本总数超过 `MAX_VERSIONS`，或单链长度超过
   `MAX_CHAIN_LEN`（建议 1000），或 `.mvs` 文件超过 `MAX_FILE_BYTES`。
3. **启动触发**：`initialize()` 完成 `load()` 与 WAL 恢复后执行一次。

阈值触发只做标记，实际执行仍在后台线程，避免拖慢提交路径。

### 5.2 单键回收流程

对每个键的版本链（按 `beginTS` 升序）：

1. 找到最后一个 `endTS == MVS_MAX_TS` 的版本（当前版本），标记为**必须保留**。
2. 遍历其余版本，删除所有满足 `endTS < W` 的版本。
3. 若链中只剩当前版本且它是 tombstone 且 `beginTS <= W`，删除整条链。
4. 若链为空，从 `_store` 中移除该键。

伪代码（仅描述，不落地为代码）：

```
for each key in _store:
    chain = _store[key]
    keep = [v for v in chain if v.endTS >= W] + [current(chain)]
    if keep.size() == 1 and keep[0].deleted and keep[0].beginTS <= W:
        erase key
    else:
        _store[key] = dedup(keep)
```

### 5.3 持久化

GC 修改 `_store` 后复用 `mvsManager::save()` 的“临时文件 + `rename`”原子替换机制。
建议补强：

- `fflush()` 后对文件描述符执行 `fsync()`；
- `rename()` 后对目录执行 `fsync()`（POSIX）；
- Windows 使用 `FlushFileBuffers()` 等价语义。

崩溃时：

- 替换前崩溃：旧 `.mvs` 完整，重启后仍是 GC 前状态，最多只是没回收；
- 替换后崩溃：新 `.mvs` 完整，重启后是 GC 后状态，被删版本永不需要。

WAL 不记录 GC：GC 只删除不可达版本；WAL 重放只针对已提交事务生成新版本，不会
要求恢复被 GC 删除的旧版本。

## 6. 接口设计（概要）

| 接口 | 所属类 | 说明 |
| --- | --- | --- |
| `activeSnapshots(std::vector<uint64_t>&)` | `txnManager` | 返回当前所有活跃事务快照 |
| `collectGarbage(uint64_t watermark)` | `mvsManager` | 同步执行一次 GC，返回删除数量 |
| `garbageCollect()` | `mvsManager` | 采集水位并调用 `collectGarbage()` |
| `startGC() / stopGC()` | `mvsManager` | 启动/停止后台 GC 线程 |
| `gcStats()` | `mvsManager` | 返回轮次、删除数、释放字节数、最近水位 |

`commit()` 在成功提交后递增版本计数并置“需要 GC”标记；后台线程轮询该标记与周期
计时器。

## 7. 正确性论证小结

- 活跃事务：快照 `s >= W`，被删版本 `endTS < W <= s`，不可见、不冲突。
- 未来事务：快照 `s' >= currentTS >= W`，同样不可见、不冲突。
- 当前状态：`endTS == MVS_MAX_TS` 的最新版本始终保留，读语义不变。
- 删除语义：tombstone 整体删除仅在 `beginTS <= W` 时进行，与“不存在”等价。
- 并发：GC 与读共享/互斥锁串行；水位单调安全，无需暂停新事务。
- 持久化：原子替换保证崩溃后状态一致。

## 8. 性能分析

- `visible()` / `checkConflicts()` 的复杂度从 O(k) 降为 O(k')，k' 为 GC 后的有界链长。
- GC 本身为 O(V)，V 为版本总数；可配置周期与阈值控制频率。
- `save()` 仍为全量重写 O(V·记录大小)，但 GC 后 V 显著下降；增量持久化可作为后续优化。
- 长事务会把 `W` 拉低，使 GC 无法回收其快照之后的版本，属于预期行为，需监控告警。

## 9. 风险与规避

| 风险 | 规避措施 |
| --- | --- |
| 读快照与 GC 的竞态 | 前置修正：快照获取与版本读取在同一共享锁内完成 |
| 长事务长期阻塞 GC | 记录 `W` 与 `currentTS` 差值，超阈值告警；提供事务超时策略配置 |
| GC 与提交争抢独占锁 | GC 放后台线程、批量执行；提交路径只置标记 |
| 原子替换失败 | 保留旧文件、记录错误、下一轮重试 |
| 备节点 GC 与主节点不一致 | 备节点按自身活跃快照独立 GC，不影响 2PC 回放的可见性 |
| tombstone 误删 | 仅允许“单 tombstone 且 beginTS <= W”时整链删除 |

## 10. 测试方案

1. **单元测试（版本链层）**
   - 构造多版本链，固定活跃快照集合，执行 GC，断言 `visible()` 结果不变。
   - 断言 `checkConflicts()` 对同一事务的判定不变。
   - 覆盖 tombstone 保留与整链删除两种边界。
2. **并发测试**
   - 后台长事务持有旧快照，另一连接持续更新同一 `_id`，触发 GC；断言长事务仍看到旧版本。
   - GC 与提交并发循环，断言无死锁、无数据损坏。
3. **崩溃恢复测试**
   - 在 GC 的 `save()` 前后模拟进程退出，重启后校验 `.mvs` 可读且查询结果一致。
4. **端到端回归**
   - 现有 `test/smoke.sh` 必须继续通过。
   - 2PC 复制、事务回滚、`explain` 行为不变。
5. **指标验证**
   - 大量更新后触发 GC，断言版本数与 `.mvs` 文件大小下降，且链长不超过配置阈值。

## 11. 实施阶段建议

1. **阶段一**：修正读快照竞态；新增 `txnManager::activeSnapshots()`。
2. **阶段二**：实现 `mvsManager::collectGarbage()` 与水位计算，先提供手动触发接口。
3. **阶段三**：接入后台线程与阈值触发；补强 `save()` 的 `fsync` 语义。
4. **阶段四**：增加指标、告警、测试用例与文档，纳入 CI 端到端测试。

> 本文档只描述架构与概要设计，不包含代码实现；后续实现按上述阶段逐步落地。
