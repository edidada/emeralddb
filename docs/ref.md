# ref
# **数据库内核完整功能设计：事务、多机复制、查询优化器、持久化索引**

如果要从零开发一个真正具备生产级能力的数据库（类似 MySQL InnoDB、PostgreSQL、TiDB、CockroachDB），不能只实现 SQL 解析、数据增删改查，还需要构建一套完整的**存储引擎、事务引擎、查询执行引擎、分布式复制与故障恢复体系**。

下面按照数据库内核的工程设计方式，介绍各个子系统的功能点、内部算法、核心数据结构、模块接口、执行流程以及测试要求。

## **一、数据库整体架构**

**客户端与协议层**

JDBC / ODBC / MySQL Protocol / PostgreSQL Wire Protocol

SQL 处理层

SQL Parser

Binder / Catalog

Query Optimizer

Physical Planner

Executor

Statistics

事务与分布式协调层

Transaction Manager

MVCC / Locks

Deadlock Detector

Raft Replication

Distributed Txn

Timestamp / TSO

存储引擎

B+ Tree / LSM

Buffer Pool

WAL / Recovery

Page Manager

Index Manager

Compaction / GC

**持久化与基础设施层**

Filesystem / SSD / NVMe / Network / Object Storage

这里是逻辑分层，不表示所有数据库都采用完全相同的实现。例如 PostgreSQL 主要采用基于堆表的 MVCC，InnoDB 使用聚簇 B+Tree，RocksDB 使用 LSM Tree，而 TiDB 将 SQL 层和分布式 KV 存储层分离。

## **二、数据库核心功能模块总表**

| 子系统    | 主要功能点                 | 核心技术                  |
| ------ | --------------------- | --------------------- |
| SQL 引擎 | 词法分析、语法解析、语义绑定        | AST、Parser、Catalog    |
| 查询优化器  | 谓词下推、Join 重排、索引选择     | CBO、Memo、DP           |
| 查询执行器  | 扫描、Join、排序、聚合、窗口      | Volcano、Vectorization |
| 事务管理   | Begin、Commit、Rollback | ACID、MVCC             |
| 并发控制   | 行锁、范围锁、死锁检测           | 2PL、SSI、OCC           |
| 持久化索引  | 主键、二级、联合、唯一索引         | B+Tree、LSM Tree       |
| 缓冲池    | 页缓存、淘汰、脏页刷盘           | LRU-K、Clock           |
| 日志系统   | Redo、Undo、Checkpoint  | WAL、ARIES             |
| 故障恢复   | 崩溃重放、回滚、数据校验          | Analysis、Redo、Undo    |
| 多机复制   | Leader/Follower、日志复制  | Raft、Paxos            |
| 分布式事务  | 跨分片原子提交               | 2PC、MVCC              |
| 分片管理   | 数据分布、迁移、负载均衡          | Range、Hash、Rebalance  |
| 元数据管理  | Schema、表、索引、版本        | System Catalog        |
| 统计信息   | 行数、直方图、基数估计           | Histogram、HLL         |
| 运维管理   | 备份、恢复、监控、审计           | PITR、Metrics、Tracing  |

这些子系统不是完全独立的。例如：

- 事务提交依赖 WAL 的持久化语义。
- 查询优化器依赖索引元数据与统计信息。
- 多机复制必须与事务提交和日志顺序协调。
- 崩溃恢复需要理解事务状态、页版本和索引结构。
- 在线 DDL 必须兼顾 MVCC、并发读写及复制状态。

## **三、事务管理系统（Transaction Manager）**

事务管理是数据库正确性的核心。

### **3.1 功能点详细设计**

| 编号      | 功能    | 设计内容                      |
| ------- | ----- | ------------------------- |
| TXN-001 | 创建事务  | 分配事务 ID、读取快照、初始化状态        |
| TXN-002 | 提交事务  | 验证冲突、写入提交日志、发布提交结果        |
| TXN-003 | 回滚事务  | 撤销未提交修改、释放资源              |
| TXN-004 | 保存点   | SAVEPOINT、ROLLBACK TO     |
| TXN-005 | 隔离级别  | RC、RR、Serializable        |
| TXN-006 | MVCC  | 多版本读取、可见性判断               |
| TXN-007 | 锁管理   | 行锁、表锁、范围锁、意向锁             |
| TXN-008 | 死锁检测  | Wait-for Graph、选择牺牲事务     |
| TXN-009 | 超时控制  | Lock Timeout、Txn Timeout  |
| TXN-010 | 垃圾回收  | 旧版本清理、长事务保护               |
| TXN-011 | 崩溃恢复  | 恢复已提交事务、清理未提交事务           |
| TXN-012 | 分布式事务 | Prepare、Commit、Abort、协调恢复 |

### **3.2 事务状态机**

简化的事务状态机。单机事务可以不设置独立的 PREPARING 阶段；分布式事务通常需要持久化准备状态。

### **3.3 MVCC 多版本并发控制**

假设数据库存在：

```
CREATE TABLE account (
    id BIGINT PRIMARY KEY,
    balance DECIMAL(18,2)
);

INSERT INTO account VALUES (1, 1000);
```

两个事务并发运行：

```
-- 事务 T1
BEGIN;
UPDATE account
SET balance = 900
WHERE id = 1;

-- 尚未提交

-- 事务 T2
BEGIN;
SELECT balance
FROM account
WHERE id = 1;
```

在常见的 MVCC 快照读取机制下，T2 不会直接读取 T1 尚未提交的 900，而是读取符合自身快照可见性规则的旧版本。

记录版本链示意

版本链是概念模型。不同引擎可能采用 Undo 链、版本记录或独立 MVCC KV 编码。

典型版本元数据：

```
typedef struct {
    uint64_t txn_id;
    uint64_t begin_ts;
    uint64_t end_ts;
    uint64_t prev_version_ptr;
    uint32_t flags;
} MVCCVersion;
```

这些字段是教学性设计，并非所有数据库都同时存储这些值。使用事务 ID 和快照集合的实现，可能根本不需要直接在记录中保存 `begin_ts` 和 `end_ts`。

**关键接口：**

```
Txn *txn_begin(IsolationLevel level);
int txn_commit(Txn *txn);
int txn_rollback(Txn *txn);

bool mvcc_is_visible(
    const MVCCVersion *version,
    const Snapshot *snapshot
);

int lock_acquire(
    Txn *txn,
    ResourceId resource,
    LockMode mode
);

void lock_release_all(Txn *txn);
```

### **3.4 ACID 如何实现**

| 特性              | 机制                 | 典型验证方法      |
| --------------- | ------------------ | ----------- |
| Atomicity 原子性   | Undo / MVCC / 恢复日志 | 事务中途崩溃      |
| Consistency 一致性 | 约束、事务语义、应用不变量      | 唯一键、外键、余额约束 |
| Isolation 隔离性   | MVCC、锁、SSI、OCC     | 并发异常测试      |
| Durability 持久性  | WAL、fsync、可靠复制     | 提交后断电恢复     |

需要特别注意：MVCC 并不自动保证可串行化，WAL 也不自动保证所有数据库约束都正确。

## **四、持久化索引系统（Persistent Index）**

持久化索引的核心要求是：**数据库进程退出、服务器重启甚至发生崩溃后，索引仍然正确，且能够恢复到与事务提交状态一致的结果。**

### **4.1 索引功能点**

| 编号      | 功能    | 详细设计                     |
| ------- | ----- | ------------------------ |
| IDX-001 | 主键索引  | 唯一性检查、主键定位               |
| IDX-002 | 二级索引  | 索引键到记录定位符的映射             |
| IDX-003 | 联合索引  | 多列键编码、字典序比较              |
| IDX-004 | 唯一索引  | 并发唯一性检查                  |
| IDX-005 | 范围查询  | Lower Bound、Upper Bound  |
| IDX-006 | 索引扫描  | 正向、反向、游标遍历               |
| IDX-007 | 索引创建  | 全表扫描、排序、批量构建             |
| IDX-008 | 在线建索引 | 并发增量变更捕获、追赶、发布           |
| IDX-009 | 索引删除  | 元数据删除、延迟回收物理空间           |
| IDX-010 | 崩溃恢复  | WAL 重放、页一致性恢复            |
| IDX-011 | 页分裂合并 | Split、Merge、Redistribute |
| IDX-012 | 索引校验  | 键顺序、指针合法性、校验和            |
| IDX-013 | 空间回收  | Free List、页复用、碎片整理       |

### **4.2 B+Tree 设计**

B+Tree 是传统关系数据库最重要的索引结构之一。

B+Tree 三级结构示意

概念图：内部节点负责导航，叶子节点保存键与记录定位信息；实际分隔键由具体实现定义。

**磁盘页结构：**

```
typedef struct {
    uint32_t page_id;
    uint16_t page_type;
    uint16_t key_count;

    uint64_t page_lsn;

    uint32_t parent_page_id;
    uint32_t next_page_id;

    uint16_t free_space_offset;
    uint16_t slot_count;

    uint32_t checksum;
} BTreePageHeader;
```

建议将每个页设计成固定大小，例如 8 KiB 或 16 KiB。页内采用 Slotted Page 结构，支持变长键和变长记录。

核心接口：

```
int btree_insert(BTree *tree,
                 const void *key,
                 size_t key_len,
                 RecordId rid,
                 Txn *txn);

int btree_search(BTree *tree,
                 const void *key,
                 size_t key_len,
                 RecordId *result);

int btree_delete(BTree *tree,
                 const void *key,
                 size_t key_len,
                 Txn *txn);

BTreeCursor *btree_scan(
    BTree *tree,
    const KeyRange *range,
    const Snapshot *snapshot
);
```

这些接口还需要进一步规定错误码、事务可见性、并发锁存及迭代器生命周期。

### **4.3 B+Tree 和 LSM Tree 对比**

| 维度   | B+Tree     | LSM Tree           |
| ---- | ---------- | ------------------ |
| 典型代表 | InnoDB     | RocksDB            |
| 写入方式 | 原地页修改为主    | MemTable + SSTable |
| 随机读取 | 通常较好       | 可能访问多个层级           |
| 顺序写入 | 需要页管理与 WAL | 写入路径顺序化程度高         |
| 写放大  | 页分裂及刷盘     | Compaction         |
| 空间放大 | 页碎片、空闲空间   | 多层文件及过期版本          |
| 范围查询 | 叶子页顺序扫描    | 多路归并               |
| 适用场景 | OLTP、范围访问  | 高写入吞吐 KV、分布式存储     |

如果是学习数据库内核，我建议先实现 B+Tree，再扩展 LSM Tree。二者可通过统一的 KV/Index API 抽象，但不能简单认为其事务和恢复机制完全可互换。

### **4.4 持久化写入流程**

例如：

```
INSERT INTO users(id, name)
VALUES (100, 'Alice');
```

对于采用 WAL 的 B+Tree 引擎，可以设计如下流程：

1. 事务获取写入权限，定位目标叶子页。
2. 执行唯一性检查，必要时等待其他事务。
3. 生成足以支持 Redo/Undo 的日志记录，分配 LSN。
4. 在 Buffer Pool 中修改数据页和索引页，标记为 Dirty。
5. 事务提交时，按照持久化策略确保提交日志已落盘。
6. 后台线程异步刷写脏页，确保相关 WAL 先于数据页持久化。
7. 崩溃后根据日志恢复已提交修改，并处理未提交修改。

最关键的约束：

\\[ \text{DurableWAL\\\_LSN} \geq \text{PageLSN} \\]

也就是说，**一个数据页被持久化之前，保护该页修改的 WAL 必须先持久化。**

此外，还必须处理写入过程中断电造成的 Torn Page，可以采用 Full Page Image、Doublewrite Buffer、页校验及恢复机制。

## **五、WAL 日志与故障恢复**

WAL（Write-Ahead Logging）不仅用于数据库重启，也与事务提交、索引恢复、复制和备份直接相关。

### **5.1 功能设计**

| 编号      | 功能           | 说明                        |
| ------- | ------------ | ------------------------- |
| WAL-001 | 日志追加         | Append-only Log           |
| WAL-002 | LSN 分配       | 全局或分区日志序号                 |
| WAL-003 | Group Commit | 批量提交减少 fsync              |
| WAL-004 | 日志分段         | Segment 创建、切换、删除          |
| WAL-005 | Checkpoint   | 记录恢复起点与脏页信息               |
| WAL-006 | Redo         | 重放尚未落盘的已记录修改              |
| WAL-007 | Undo         | 撤销失败或未提交事务                |
| WAL-008 | 日志校验         | CRC、长度、版本                 |
| WAL-009 | 日志归档         | 备份与 PITR                  |
| WAL-010 | 日志截断         | 结合 Checkpoint、复制进度和备份保留策略 |

注意：Undo 不一定必须存储在同一种 WAL 记录里，具体取决于存储引擎设计。

### **5.2 WAL 记录格式**

```
typedef struct {
    uint64_t lsn;
    uint64_t prev_lsn;
    uint64_t txn_id;

    uint32_t record_type;
    uint32_t payload_length;
    uint32_t checksum;

    uint64_t page_id;
} WALRecordHeader;
```

日志类型可包含：

```
typedef enum {
    WAL_TXN_BEGIN,
    WAL_TXN_COMMIT,
    WAL_TXN_ABORT,

    WAL_PAGE_INSERT,
    WAL_PAGE_UPDATE,
    WAL_PAGE_DELETE,

    WAL_BTREE_SPLIT,
    WAL_BTREE_MERGE,

    WAL_CHECKPOINT_BEGIN,
    WAL_CHECKPOINT_END,

    WAL_COMPENSATION
} WALRecordType;
```

### **5.3 崩溃恢复算法**

可以参考 ARIES 思想，将恢复分成三个阶段：

**Analysis — 分析阶段**

重建事务状态表、脏页表，确定需要 Redo 的日志范围。

**Redo — 重做阶段**

根据日志和 PageLSN 判断哪些修改需要重新执行。

**Undo — 撤销阶段**

回滚未完成事务，必要时写入 Compensation Log Record。

实现时必须测试每个关键写入点的断电场景，包括日志写入一半、Checkpoint 中断、B+Tree 分裂中断以及提交确认前后崩溃。

## **六、查询优化器（Query Optimizer）**

查询优化器的任务是将 SQL 转换为低成本的物理执行计划。

例如：

```
SELECT u.name, SUM(o.amount)
FROM users u
JOIN orders o ON u.id = o.user_id
WHERE u.age > 30
GROUP BY u.name;
```

优化器需要回答：

- 先扫描 `users` 还是 `orders`？
- `age > 30` 能否下推？
- 是否存在合适的索引？
- 使用 Hash Join、Nested Loop Join 还是 Merge Join？
- 是否需要 Sort？
- 如何估算中间结果行数？
- 是否能够利用并行执行？

### **6.1 功能点详细设计**

| 编号      | 功能       | 设计内容                       |
| ------- | -------- | -------------------------- |
| OPT-001 | SQL 解析   | Lexer、Parser、AST           |
| OPT-002 | 语义绑定     | 列名、类型、表、函数解析               |
| OPT-003 | 逻辑计划     | Scan、Filter、Join、Aggregate |
| OPT-004 | 常量折叠     | `1 + 2` → `3`              |
| OPT-005 | 谓词下推     | 将 Filter 移近数据源             |
| OPT-006 | 列裁剪      | 删除不需要读取的列                  |
| OPT-007 | Join 重排  | 搜索不同连接顺序                   |
| OPT-008 | 子查询优化    | 去关联、半连接转换                  |
| OPT-009 | 统计信息     | NDV、直方图、行数                 |
| OPT-010 | 代价模型     | CPU、I/O、内存、网络              |
| OPT-011 | 索引选择     | Index Scan、Table Scan      |
| OPT-012 | 物理算子选择   | Hash Join、Merge Join 等     |
| OPT-013 | Top-N 优化 | LIMIT + ORDER BY           |
| OPT-014 | 分布式优化    | Exchange、Shuffle、Broadcast |
| OPT-015 | 执行计划缓存   | 参数化、失效、重新优化                |
| OPT-016 | EXPLAIN  | 输出计划与实际执行统计                |

### **6.2 查询优化流程**

SQL

输入查询

Parser + Binder

AST 与语义解析

Logical Optimizer

谓词下推、列裁剪、关系代数重写

Cost-Based Optimizer

Join 顺序、访问路径、物理算法

Physical Plan

可执行算子树

Execution Engine

迭代器或向量化执行

### **6.3 Cost-Based Optimizer**

一个简化的代价模型：

\\[ C\_{\text{total}} = C\_{\text{I/O}} + C\_{\text{CPU}} + C\_{\text{Memory}} + C\_{\text{Network}} \\]

这里各项必须转换为可比较的估计成本，不能直接将不同单位的原始数值相加。

例如两种 Join 策略：

| 方案 | 执行方式                   | 适用条件                 |
| -- | ---------------------- | -------------------- |
| A  | Full Scan + Hash Join  | 大批量匹配、可建立 Hash Table |
| B  | Index Nested Loop Join | 外表结果较少、内表有高选择性索引     |

优化器利用统计信息估算两种方案的成本，再选择较优者。

工业级优化器还要考虑数据倾斜、统计信息过期、内存溢出、并行度及网络数据传输。

### **6.4 Join Order 优化**

对于：

```
SELECT *
FROM A
JOIN B ON A.id = B.a_id
JOIN C ON B.id = C.b_id
JOIN D ON C.id = D.c_id;
```

可能存在不同的合法 Join 顺序。优化器可以使用：

- Dynamic Programming：适合连接表数量较少的场景。
- Greedy Search：计算开销较低，但可能错过全局较优计划。
- Cascades / Memo：使用等价表达式组、转换规则与代价搜索。

需要注意外连接、相关子查询和非确定性表达式会限制合法的重排空间。

### **6.5 查询优化器核心接口**

```
class QueryOptimizer {
public:
    virtual LogicalPlan optimizeLogical(
        const LogicalPlan& plan) = 0;

    virtual PhysicalPlan optimizePhysical(
        const LogicalPlan& plan,
        const Statistics& stats) = 0;

    virtual Cost estimateCost(
        const PhysicalPlan& plan) = 0;

    virtual ~QueryOptimizer() = default;
};
```

推荐参考的开源实现包括：

- Apache Calcite：关系代数、规则优化、CBO。
- DuckDB：查询优化和向量化执行。
- PostgreSQL：统计信息、Join 搜索和执行计划。
- TiDB：分布式 SQL 优化。

## **七、查询执行引擎（Execution Engine）**

优化器生成计划后，由执行器真正读取数据、完成 Join、聚合和排序。

### **7.1 功能点**

| 编号       | 功能                   | 核心实现         |
| -------- | -------------------- | ------------ |
| EXEC-001 | Table Scan           | 顺序扫描表        |
| EXEC-002 | Index Scan           | 按索引查找        |
| EXEC-003 | Filter               | 谓词计算         |
| EXEC-004 | Projection           | 列选择、表达式计算    |
| EXEC-005 | Hash Join            | Hash Table   |
| EXEC-006 | Merge Join           | 有序流合并        |
| EXEC-007 | Nested Loop          | 嵌套循环连接       |
| EXEC-008 | Hash Aggregate       | 哈希分组聚合       |
| EXEC-009 | Sort                 | 内存排序、外部归并    |
| EXEC-010 | Window               | 窗口函数计算       |
| EXEC-011 | Limit / Top-N        | 限制输出行数       |
| EXEC-012 | Parallel Scan        | 并行数据扫描       |
| EXEC-013 | Spill to Disk        | 内存不足时写临时文件   |
| EXEC-014 | Vectorized Execution | 批量处理列数据      |
| EXEC-015 | Cancellation         | 查询超时、中断      |
| EXEC-016 | Runtime Metrics      | 实际行数、CPU、I/O |

### **7.2 Volcano 执行模型**

传统迭代器模型：

```
class Executor {
public:
    virtual void open() = 0;
    virtual bool next(Row& row) = 0;
    virtual void close() = 0;
    virtual ~Executor() = default;
};
```

执行器通过 `next()` 不断拉取下一行。

向量化模型则使用：

```
class VectorizedExecutor {
public:
    virtual void open() = 0;

    virtual size_t nextBatch(
        ColumnBatch& batch
    ) = 0;

    virtual void close() = 0;
    virtual ~VectorizedExecutor() = default;
};
```

例如一次处理 1024 行，而不是每次只处理一行，从而减少函数调用和解释执行开销，提高 CPU Cache 与 SIMD 利用率。

对于分析型数据库，可以重点研究 DuckDB 的向量化执行；对于事务型数据库，则需要同时考虑低延迟点查询与短事务性能。

## **八、多机复制系统（Replication）**

多机复制需要解决的不仅是将数据从 A 复制到 B，还包括：

1. Leader 故障后如何自动选举。
2. 多节点如何保证日志顺序一致。
3. 已提交数据是否允许丢失。
4. 网络分区时哪些节点可以继续写入。
5. 新节点如何追赶历史数据。
6. 如何防止旧 Leader 继续接受写入。
7. 如何在线添加、移除和替换节点。

### **8.1 复制系统功能点**

| 编号      | 功能        | 详细设计                             |
| ------- | --------- | -------------------------------- |
| REP-001 | 节点注册      | Node ID、地址、状态                    |
| REP-002 | Leader 选举 | Raft Election                    |
| REP-003 | 心跳        | Heartbeat、超时检测                   |
| REP-004 | 日志复制      | AppendEntries                    |
| REP-005 | 多数派提交     | Quorum Commit                    |
| REP-006 | 日志冲突处理    | Term、Index、一致性检查                 |
| REP-007 | Snapshot  | 状态快照生成与安装                        |
| REP-008 | 节点追赶      | Log Catch-up                     |
| REP-009 | 成员变更      | Joint Consensus 等安全配置变更          |
| REP-010 | Leader 切换 | 故障检测、重新选举                        |
| REP-011 | 读一致性      | Leader Read、ReadIndex、Lease Read |
| REP-012 | 流量控制      | Backpressure、复制积压限制              |
| REP-013 | 数据校验      | 副本状态、校验和、快照校验                    |
| REP-014 | 故障注入      | 网络分区、延迟、丢包、进程崩溃                  |

### **8.2 三节点 Raft 架构**

三副本 Raft Group：通常需要至少 2 个节点形成多数派才能提交新日志。

### **8.3 写入复制流程**

客户端执行：

```
UPDATE account
SET balance = balance - 100
WHERE id = 1;
```

在采用 Raft 复制状态机的数据库中，可以设计如下流程：

1. 客户端将写请求发送给当前 Leader。
2. Leader 进行事务协调，生成需要复制的状态机命令或事务记录。
3. Leader 将 Raft 日志持久化，并向 Follower 发送 AppendEntries。
4. Follower 验证日志连续性，将日志写入持久化存储后确认。
5. 满足 Raft 提交规则后，Leader 推进 Commit Index。
6. 各节点按日志顺序应用已提交的状态机命令。
7. 数据库在满足自身事务提交与可见性约束后向客户端确认。

**特别注意：Raft 日志已提交不等于 SQL 事务必然已经提交。** 分布式数据库可能还需要执行事务冲突验证、两阶段提交或提交时间戳发布。

### **8.4 Raft 核心数据结构**

```
struct RaftNode {
    uint64_t node_id;
    uint64_t current_term;
    uint64_t voted_for;

    uint64_t commit_index;
    uint64_t last_applied;

    RaftRole role;
};

struct RaftLogEntry {
    uint64_t term;
    uint64_t index;

    std::vector<uint8_t> command;
};
```

实际实现还必须持久化 `current_term`、`voted_for` 和日志，并维护各 Follower 的 `nextIndex`、`matchIndex` 等复制状态。

### **8.5 故障处理矩阵**

| 故障             | 预期行为                 |
| -------------- | -------------------- |
| 单个 Follower 宕机 | 多数派仍可正常提交            |
| Leader 宕机      | 重新选举 Leader          |
| 三节点中两个宕机       | 无法形成多数派，停止新写入        |
| 网络分区 2:1       | 多数派一侧可以继续            |
| 网络分区 1:1:1     | 无多数派，无法提交新日志         |
| Follower 日志落后  | 日志追赶或安装快照            |
| 旧 Leader 恢复    | 按较新 Term 转为 Follower |

这些保证依赖正确实现 Raft、持久化和状态机应用逻辑，而不是仅仅部署三个节点。

## **九、分布式事务与分片管理**

Raft 解决的是一个复制组内的共识问题。如果一笔事务同时修改多个分片，还需要跨分片事务协调。

例如转账：

```
BEGIN;

UPDATE account
SET balance = balance - 100
WHERE id = 1001;

UPDATE account
SET balance = balance + 100
WHERE id = 2002;

COMMIT;
```

假设账户 1001 在 Shard A，账户 2002 在 Shard B，就不能让两个分片各自独立提交，否则可能出现只扣款、不入账。

### **9.1 分布式事务功能点**

| 编号      | 功能      | 设计内容         |
| ------- | ------- | ------------ |
| DTX-001 | 全局事务 ID | 唯一标识事务       |
| DTX-002 | 分片路由    | 找到事务涉及的分片    |
| DTX-003 | Prepare | 各参与者持久化准备状态  |
| DTX-004 | Commit  | 全局提交决定       |
| DTX-005 | Abort   | 全局回滚决定       |
| DTX-006 | 协调者恢复   | 恢复未完成事务      |
| DTX-007 | 超时处理    | 区分未知结果与确定失败  |
| DTX-008 | 冲突检测    | 跨分片写冲突、隔离性保证 |
| DTX-009 | 时间戳服务   | 生成快照及提交时间戳   |
| DTX-010 | 幂等重试    | 防止重复提交产生重复效果 |
| DTX-011 | 分片迁移    | 在线数据迁移与路由切换  |
| DTX-012 | 负载均衡    | 热点分片检测、自动迁移  |

### **9.2 两阶段提交（2PC）**

Phase 1：Prepare

Coordinator

发送 PREPARE

Shard A

持久化 Prepared

Shard B

持久化 Prepared

Phase 2：Commit / Abort

Coordinator

持久化全局决定

Shard A

提交或回滚

Shard B

提交或回滚

概念流程。只有参与者都成功 Prepare，协调者才能决定 Commit；否则通常决定 Abort。

传统 2PC 存在协调者故障导致参与者阻塞的问题。工程实现通常通过持久化协调状态、复制事务元数据、恢复协议等机制提高可用性。

### **9.3 分片管理**

分片策略通常包括：

| 策略              | 优点          | 缺点      |
| --------------- | ----------- | ------- |
| Hash Sharding   | 数据分布较均匀     | 范围查询困难  |
| Range Sharding  | 范围扫描方便      | 容易出现热点  |
| Consistent Hash | 节点变化时减少数据迁移 | 范围局部性较差 |
| Hybrid          | 可结合范围和负载管理  | 实现复杂    |

如果目标是 TiDB 类分布式数据库，推荐采用 **Range Partition + 多副本 Raft Group + 动态分裂与迁移** 的整体方向。

## **十、Buffer Pool、Catalog、备份与运维**

除了四大核心引擎，以下功能决定数据库能否长期稳定运行。

### **10.1 Buffer Pool**

| 功能               | 详细设计              |
| ---------------- | ----------------- |
| Page Cache       | 以 Page ID 缓存磁盘页   |
| Pin / Unpin      | 防止正在使用的页被淘汰       |
| Dirty Page       | 跟踪修改但尚未落盘的页       |
| Eviction         | LRU-K、Clock 等淘汰策略 |
| Page Latch       | 保护内存页内部结构         |
| Background Flush | 后台脏页刷盘            |
| Prefetch         | 顺序扫描预读            |
| Memory Limit     | 缓存内存预算与压力控制       |

必须区分 **Latch** 和 **Lock**：Latch 主要保护短时间的内存数据结构操作，事务 Lock 则保护逻辑数据的并发访问语义。

### **10.2 Catalog 元数据管理**

需要设计系统表：

```
sys_databases
sys_schemas
sys_tables
sys_columns
sys_indexes
sys_constraints
sys_statistics
sys_transactions
sys_partitions
sys_nodes
```

功能包括 Schema Version、DDL 原子性、元数据缓存失效、表和索引生命周期管理、在线 Schema Change。

### **10.3 备份恢复**

| 功能                  | 设计要求         |
| ------------------- | ------------ |
| Full Backup         | 完整备份         |
| Incremental Backup  | 增量备份         |
| PITR                | 基于日志恢复到指定时间点 |
| Snapshot Backup     | 一致性快照        |
| Backup Verification | 校验备份完整性      |
| Restore Test        | 定期执行恢复演练     |
| Distributed Backup  | 协调跨分片一致性     |
| Encryption          | 备份加密与密钥管理    |

生产级数据库还需要权限管理、TLS、审计、资源配额、慢查询日志、监控指标和版本升级机制。

## **十一、推荐的数据库源码工程结构**

如果你准备自己实现数据库，可以采用 C++20 构建内核，CMake 管理跨平台编译，先实现单机数据库，再逐步扩展为分布式数据库。

```
mydb/
├── CMakeLists.txt
├── include/mydb/               # 对外公共 API
│   ├── database.h
│   ├── transaction.h
│   └── result.h
│
├── src/
│   ├── common/
│   │   ├── status.cpp
│   │   ├── config.cpp
│   │   └── logging.cpp
│   │
│   ├── storage/
│   │   ├── disk/
│   │   │   ├── disk_manager.cpp
│   │   │   └── file_manager.cpp
│   │   ├── page/
│   │   │   ├── page.cpp
│   │   │   └── slotted_page.cpp
│   │   ├── buffer/
│   │   │   ├── buffer_pool.cpp
│   │   │   └── replacer.cpp
│   │   ├── index/
│   │   │   ├── bplus_tree.cpp
│   │   │   └── index_iterator.cpp
│   │   └── table/
│   │       ├── table_heap.cpp
│   │       └── tuple.cpp
│   │
│   ├── transaction/
│   │   ├── transaction_manager.cpp
│   │   ├── mvcc.cpp
│   │   ├── lock_manager.cpp
│   │   └── deadlock_detector.cpp
│   │
│   ├── recovery/
│   │   ├── wal_manager.cpp
│   │   ├── checkpoint.cpp
│   │   └── recovery_manager.cpp
│   │
│   ├── sql/
│   │   ├── parser/
│   │   ├── binder/
│   │   ├── planner/
│   │   ├── optimizer/
│   │   └── executor/
│   │
│   ├── catalog/
│   ├── statistics/
│   │
│   ├── distributed/
│   │   ├── raft/
│   │   ├── replication/
│   │   ├── shard/
│   │   ├── distributed_txn/
│   │   └── metadata/
│   │
│   ├── network/
│   ├── security/
│   └── server/
│
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── concurrency/
│   ├── recovery/
│   ├── distributed/
│   └── sql/
│
├── benchmarks/
│   ├── ycsb/
│   ├── tpcc/
│   └── tpch/
│
├── tools/
│   ├── db_shell/
│   ├── db_inspect/
│   └── db_repair/
│
└── docs/
    ├── architecture/
    ├── detailed-design/
    ├── storage-format/
    ├── recovery/
    ├── protocols/
    ├── adr/
    └── performance/
```

其中 `include/mydb/` 放稳定的公共接口；各模块内部头文件放在相应 `src/` 目录或 `internal/` 目录中，避免暴露实现细节。

## **十二、如何分阶段实现完整数据库**

Phase 1

**单机持久化 KV**

Disk Manager、Page、Buffer Pool、B+Tree、基础 WAL

验收：进程重启后数据可读；随机崩溃测试通过

Phase 2

**单机事务型数据库**

MVCC、锁管理、Undo、Commit、Rollback、Checkpoint、Recovery

验收：并发事务与崩溃恢复正确

Phase 3

**关系型 SQL 数据库**

Catalog、Parser、Binder、Executor、Index Scan、Join、Aggregate

验收：SQL 功能测试、TPC-H 子集查询通过

Phase 4

**成本优化与并行执行**

Statistics、CBO、Join Reorder、Vectorization、EXPLAIN

验收：查询结果正确，优化计划具备可解释的成本收益

Phase 5

**多副本数据库**

Raft、Snapshot、Leader Election、Failover、Read Consistency

验收：网络分区、故障切换与复制一致性测试通过

Phase 6

**分布式 SQL 数据库**

Sharding、2PC、分布式 MVCC、TSO、Rebalance

验收：跨分片事务正确，扩缩容与故障恢复通过

Phase 7

**生产工程化**

Backup、PITR、Online DDL、Security、Observability

验收：长稳测试、恢复演练、基准测试和升级测试通过

## **十三、建议研究的开源数据库源码**

以下项目分别覆盖数据库内核的不同技术方向：

| 项目          | 重点学习内容                      | 源码                                                                        |
| ----------- | --------------------------- | ------------------------------------------------------------------------- |
| BusTub      | Buffer Pool、B+Tree、事务、查询优化  | [GitHub](https://github.com/cmu-db/bustub?utm_source=chatgpt.com)         |
| SQLite      | 单文件存储、B-Tree、事务、WAL         | [GitHub](https://github.com/sqlite/sqlite?utm_source=chatgpt.com)         |
| RocksDB     | LSM Tree、SST、Compaction、WAL | [GitHub](https://github.com/facebook/rocksdb?utm_source=chatgpt.com)      |
| DuckDB      | SQL 优化器、向量化执行、分析查询          | [GitHub](https://github.com/duckdb/duckdb?utm_source=chatgpt.com)         |
| PostgreSQL  | MVCC、WAL、SQL、优化器            | [GitHub](https://github.com/postgres/postgres?utm_source=chatgpt.com)     |
| TiDB        | 分布式 SQL、事务协调、查询优化           | [GitHub](https://github.com/pingcap/tidb?utm_source=chatgpt.com)          |
| TiKV        | Raft、分布式 KV、MVCC、事务存储       | [GitHub](https://github.com/tikv/tikv?utm_source=chatgpt.com)             |
| CockroachDB | 分布式 SQL、一致性、分片事务            | [GitHub](https://github.com/cockroachdb/cockroach?utm_source=chatgpt.com) |

## **十四、数据库详细设计文档应该写到什么程度？**

真正能够指导开发的详细设计，不应该只写“使用 Raft 实现复制”“使用 B+Tree 实现索引”。

每个子系统至少应有以下交付物：

| 设计交付物  | 必须明确的问题                 |
| ------ | ----------------------- |
| 需求与边界  | 支持什么、不支持什么              |
| 模块架构   | 依赖关系、线程模型、生命周期          |
| API 设计 | 参数、返回值、错误码、并发约束         |
| 数据结构   | 内存结构、磁盘布局、编码格式          |
| 核心算法   | 伪代码、复杂度、正确性不变量          |
| 状态机    | 状态转移、非法状态、恢复规则          |
| 并发设计   | 锁顺序、Latch、死锁、可见性        |
| 持久化协议  | 写入顺序、fsync、故障原子性        |
| 异常处理   | 断电、磁盘满、网络中断、超时          |
| 测试设计   | 单测、并发、崩溃、故障注入           |
| 性能设计   | 吞吐、P99、CPU、内存、I/O       |
| 可观测性   | Metrics、Tracing、日志、诊断工具 |

例如 B+Tree 详细设计必须明确：页分裂过程中发生断电如何恢复、两个线程同时插入相邻叶子页如何避免死锁、索引根页发生变化如何原子发布、索引页与表数据页如何保持事务一致性。

**推荐的实现路线是：BusTub 学单机内核 → SQLite/PostgreSQL 学事务和恢复 → DuckDB 学优化器 → TiKV 学 Raft 与分布式事务 → 最后构建自己的分布式 SQL 数据库。**

这条路线能够将事务、多机复制、查询优化器、持久化索引等能力分别拆解成可编码、可测试、可验收的工程模块，而不是一开始就把全部复杂性耦合在一起。
