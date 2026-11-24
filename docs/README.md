# EmeraldDB 测试与功能说明

## 1. 如何构建并测试

### 1.1 依赖

- C++ 编译器：MSVC（Windows）、GCC/Clang（Linux/macOS）
- CMake 3.15+
- **Boost 1.49.0**（源码）。代码使用了后来被移除的 `boost::xtime`、Spirit Classic 等 API，不能用新版 Boost 替代。
- 需要编译的 Boost 库：`thread`、`program_options`、`date_time`、`system`

Boost 需要放在仓库根目录的 `boost/` 下（即 `boost/boost/version.hpp` 存在），编译产物放到 `boost/stage/lib/`。

> CI 已完整演示该过程：`.github/workflows/windows-build_cmake.yml`、
> `.github/workflows/nix-build_cmake.yml`、`windows-build.yml`。

### 1.2 准备 Boost（Linux/macOS 示例）

```bash
curl -fsSL -o boost.tar.bz2 \
  https://archives.boost.io/release/1.49.0/source/boost_1_49_0.tar.bz2
tar -xjf boost.tar.bz2
mv boost_1_49_0 boost

# libc 的 TIME_UTC 宏与 Boost 1.49 的枚举名冲突，源码期望 TIME_UTC_
perl -pi -e 's/TIME_UTC=1/TIME_UTC_=1/; \
  s/clock_type == TIME_UTC/clock_type == TIME_UTC_/' \
  boost/boost/thread/xtime.hpp

cd boost
./bootstrap.sh --with-libraries=thread,program_options,date_time,system
./b2 -j4 link=static runtime-link=shared threading=multi variant=debug stage
cd ..
```

Windows 上解压 `boost_1_49_0.7z` 并重命名为 `boost/` 后，对
`boost/boost/thread/xtime.hpp` 做同样的 `TIME_UTC` → `TIME_UTC_` 替换，
然后在 MSVC 开发者命令行里执行：

```bat
cd boost
call bootstrap.bat msvc
b2 --with-thread --with-program_options --with-date_time --with-system ^
   link=static runtime-link=shared threading=multi variant=debug toolset=msvc stage
cd ..
```

### 1.3 用 CMake 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

Windows 上若使用 NMake：

```bat
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

构建出两个可执行文件：

- `emeralddb` —— 数据库服务端（`src/pmd/pmdMain.cpp`）
- `client` —— 交互式 shell 客户端（`src/client/edb.cpp`）

旧的构建方式仍然保留：`src/build.sh`（autotools）、`emeralddb.sln`（VS 工程）。

### 1.4 启动服务端

```bash
./build/emeralddb --dbpath ./edb.data --svcname 48127 --logpath ./diag.log --maxpool 20
```
```ps
.\cmake-build-debug\emeralddb.exe --dbpath ./edb.data --svcname 48127 --logpath ./diag.log --maxpool 20
```

| 参数 | 短选项 | 说明 | 默认值 |
| --- | --- | --- | --- |
| `--dbpath` | `-d` | 数据文件路径 | `./edb.data` |
| `--svcname` | `-s` | 监听端口 | `48127` |
| `--logpath` | `-l` | 诊断日志文件 | `./diag.log` |
| `--maxpool` | `-m` | 最大 agent 线程数 | `20` |
| `--confpath` | `-c` | 配置文件路径 | `./edb.conf` |
| `--role` | `-r` | 复制角色 `primary` / `secondary` | `primary` |
| `--peer` | `-p` | 主节点推送目标 `host:port`（备节点监听端口为服务端口+1） | 空 |
| `--help` | `-h` | 帮助 | — |

服务端启动后会加载数据文件、从已有记录重建 `_id` 哈希索引，并监听 TCP 端口。

### 1.5 用客户端测试基本 CRUD

```bash
./build/client
```
```ps
.\cmake-build-debug\client.exe
```

进入 `edb>` 提示符后：

```
edb> connect localhost 48127
edb> insert {"_id":1,"name":"tom","age":20}
edb> insert {"_id":2,"name":"zhangsan","age":21}
edb> query {"_id":1}
edb> snapshot
edb> delete {"_id":1}
edb> query {"_id":1}
edb> quit
```

说明：

- `insert` / `query` / `delete` 的参数是 JSON 字符串，并且会被转换成 BSON
  后再按自定义协议发给服务端；客户端按空格切分输入，因此 **JSON 要写成单行、
  不要带空格**。
- `_id` 是索引键，插入时必须提供，且不允许重复。
- `query` 按 `_id` 精确查找，命中时打印 BSON 字符串。
- `snapshot` 打印 `insertTimes`、`delTimes`、`queryTimes` 和 `serverRunTime`（分钟），
  可用来确认请求确实到达了服务端。
- `begin` / `commit` / `rollback` 控制事务；`explain <json>` 查看查询计划。
- `help` 列出所有命令，`quit` 断开连接并退出。
- 服务端日志写入 `diag.log`（默认级别 `PDWARNING`），可用于排查问题。

### 1.6 在 CI 上验证

三个 workflow 覆盖不同平台和版本：

- `windows-build.yml`：MSBuild 方式（windows-2022 / 2025 / latest）
- `windows-build_cmake.yml`：CMake 方式（windows-2022 / 2025 / latest）
- `nix-build_cmake.yml`：Linux 与 macOS（ubuntu-22.04 / 24.04 / latest，
  macos-13 / 15 / latest）

```bash
gh workflow list --repo edidada/emeralddb
gh run list --repo edidada/emeralddb --limit 5
gh run view <run-id> --repo edidada/emeralddb --log
```

---

## 2. 代码实现了哪些数据库功能

### 2.1 存储管理（DMS，`src/dms/`、`src/include/dms.hpp`）

- 基于文件的页式存储：`DMS_PAGESIZE = 4MB`，段大小 128MB（每段 32 页），
  最多 262144 页；通过 `mmap`/`CreateFileMapping` 映射访问。
- 文件头（64KB）+ 数据页；页内使用 slot 目录管理变长记录。
- 空闲空间表 `_freeSpaceMap` 按剩余空间选页，支持记录插入、删除、按 `dmsRecordID`
  定位读取、空间回收（`_recoverSpace`）。
- 文件扩展：`_extendSegment` / `_extendFile` 按需增长；启动时 `_loadData`
  扫描已有数据并重建索引。

### 2.2 索引管理（IXM，`src/ixm/`、`src/include/ixmBucket.hpp`）

- 对 `_id` 字段建哈希索引，1000 个 bucket，每个 bucket 内部是
  `std::multimap<hash, ixmEleHash>`，并用读写锁保护。
- 提供 `isIDExist` / `createIndex` / `findIndex` / `removeIndex`。
- 索引是内存结构，启动时由 DMS 扫描数据重建（未持久化索引文件）。

### 2.3 记录层（RTN，`src/rtn/rtn.cpp`）

- `rtnInsert`：校验 `_id` → 写入数据文件 → 建索引；重复 `_id` 返回
  `EDB_IXM_ID_EXIST`，缺少 `_id` 返回 `EDB_NO_ID`。
- `rtnFind`：先用索引查到 `dmsRecordID`，再从数据文件读取 BSON。
- `rtnRemove`：删除索引项并移除数据记录。

### 2.4 网络与协议（`src/oss/ossSocket.cpp`、`src/msg/msg.cpp`、`src/include/msg.hpp`）

- 自定义二进制消息：`MsgHeader`（消息长度 + 操作码）后跟负载。
- 操作码：`OP_REPLY(1)`、`OP_INSERT(2)`、`OP_DELETE(3)`、`OP_QUERY(4)`、
  `OP_COMMAND(5)`、`OP_DISCONNECT(6)`、`OP_CONNECT(7)`、`OP_SNAPSHOT(8)`。
- 请求/应答结构：`MsgInsert`、`MsgDelete`、`MsgQuery`、`MsgCommand`、`MsgReply`。
- 基于 BSD socket（Windows 用 Winsock），支持超时、Nagle 开关、地址/端口解析。

### 2.5 服务端并发模型（PMD，`src/pmd/`、`src/include/pmdEDU.hpp`）

- EDU（Execution Unit）模型：`pmdEDUMgr` 管理线程，`pmdTcpListener` 负责监听，
  每个连接由 `pmdAgent` 处理；线程池上限由 `--maxpool` 控制（默认 20）。
- `pmdAgent` 按操作码分发：insert / query / delete / snapshot / disconnect / command。
- 快照统计：累计插入次数、删除次数、查询次数和服务端运行时长。
- 同步原语：`ossSLatch` / `ossXLatch`（封装互斥锁），`boost::thread` 管理线程。

### 2.6 客户端 Shell（`src/client/`）

- 命令工厂（`commandFactory`）把命令名映射到处理类。
- `connect`、`insert`、`query`、`delete`、`snapshot`、`test`、`help`、`quit`。
- 使用 Boost.Spirit 的 `fromjson` 把 JSON 转成 BSON，收到应答后用
  `BSONObj::toString()` 打印结果。

### 2.7 BSON 层（`src/bson/`）

- 从 MongoDB 早期版本移植的 BSON 实现：`BSONObj`、`BSONElement`、
  `BSONObjBuilder`、`BSONObjIterator`、`OID` 等。
- JSON ↔ BSON 解析、base64、md5、nonce 等工具。

### 2.8 诊断与配置（`src/pd/`、`src/pmd/pmdOptions.cpp`）

- 分级日志（`PDSEVERE` … `PDDEBUG`，默认 `PDWARNING`），输出带函数名、文件、行号。
- 命令行参数与配置文件（`edb.conf`）解析基于 `boost::program_options`。

### 2.9 跨平台

- Windows：MSVC + CMake/MSBuild 工程，POSIX 调用由 `src/include/wininc.h`、
  `src/win/emeralddb_win.cpp` 适配。
- Linux/macOS：原生 `unistd` / `mmap` / `pthread`，由 CMake 或 autotools 构建。

### 2.10 持久化索引（`src/include/ixmPersist.hpp`、`src/ixm/ixmBucket.cpp`）

- `_id` 哈希索引写入 `<dbpath>.idx` 追加日志，记录 bucket、hash、记录位置和原始
  BSON 元素；启动时直接加载，无需全表扫描重建索引。索引文件缺失时自动回退到扫描重建。

### 2.11 事务（`src/include/txn.hpp`、`src/txn/txn.cpp`）

- 客户端 `begin` / `commit` / `rollback`，每个连接一个事务。
- MVCC 快照隔离：事务开始时取快照时间戳，读按 `[beginTS, endTS)` 返回可见版本，
  版本持久化到 `<dbpath>.mvs`。
- 写写冲突检测：提交时若发现更高版本已提交，返回 `EDB_CONFLICT` 并中止事务。
- WAL 记录 `BEGIN` / 写操作 / `COMMIT`，提交先写版本再刷 WAL，重启后重放已提交事务。

### 2.12 查询优化器（`src/include/qry.hpp`、`src/qry/qry.cpp`）

- 有 `_id` 等值条件时选 `ID_INDEX_LOOKUP`（代价 1），否则选 `TABLE_SCAN`（代价为记录数）。
- 支持任意字段的等值、 `$eq` / `$ne` / `$gt` / `$gte` / `$lt` / `$lte` 谓词。
- `explain <json>` 返回所选计划、代价估算和记录数。

### 2.13 多机复制（`src/include/repl.hpp`、`src/repl/repl.cpp`）

- 单节点可用 `--role primary`；副本集用 `--peers host:port,host:port` 配置成员
  （副本端口为服务端口+1）。
- 基于 term 的心跳与多数派投票选举，只有 leader 接受写入， follower 返回 `EDB_QUIESCED`。
- 事务提交时走两阶段提交：leader 发送 PREPARE，收集投票后发送 COMMIT/ABORT；
  备节点把决策写入 `<dbpath>.2pc` 后回放，保证复制的原子性。

> 说明：已具备 MVCC、两阶段提交、副本集选举和提交时复制等核心机制，但仍是精简实现：
> 2PC 为阻塞式（无超时协调者恢复）、副本集成员需静态配置、无日志压缩与在线加节点、
> MVCC 版本链未做垃圾回收，查询优化器仅做索引/全表扫描二选一。
