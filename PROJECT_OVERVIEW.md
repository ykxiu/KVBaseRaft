# KVstorageBaseRaft-cpp — 项目总览

基于 **Raft 共识算法**的分布式键值存储系统，使用 C++20 实现。
多个服务节点通过复制状态机保持数据一致性，向客户端提供线性一致的读写服务。

---

## 目录

1. [整体架构](#1-整体架构)
2. [模块职责](#2-模块职责)
3. [核心工作流](#3-核心工作流)
4. [时序与配置参数](#4-时序与配置参数)
5. [安全性与活性保证](#5-安全性与活性保证)

---

## 1. 整体架构

### 层次视图

```
┌──────────────────────────────────────────────────────┐
│                   客户端 (Clerk)                      │
│     Get(key)  /  Put(key,val)  /  Append(key,val)    │
└────────────────────┬─────────────────────────────────┘
                     │ KV RPC（protobuf）
      ┌──────────────┼──────────────┐
      ▼              ▼              ▼
┌──────────┐   ┌──────────┐   ┌──────────┐
│ KvServer │   │ KvServer │   │ KvServer │  ← 应用层
│  节点 0  │   │  节点 1   │   │  节点 2  │
├──────────┤   ├──────────┤   ├──────────┤
│  Raft 0  │◄─►│  Raft 1  │◄─►│  Raft 2  │  ← 共识层
│ (Leader) │   │(Follower)│   │(Follower)│
├──────────┤   ├──────────┤   ├──────────┤
│ SkipList │   │ SkipList │   │ SkipList │  ← 存储层
│Persister │   │Persister │   │Persister │
└──────────┘   └──────────┘   └──────────┘
```

### 目录结构

```
src/
├── raftCore/          # Raft 共识引擎 + KV 状态机
│   ├── raft.h/.cpp        Raft 算法（选举、日志复制、快照）
│   ├── kvServer.h/.cpp    构建于 Raft 之上的 KV 服务
│   └── Persister.h/.cpp   基于文件的持久化存储
├── raftClerk/         # 客户端库
│   └── clerk.h/.cpp       Clerk：Leader 发现 + 请求重试
├── raftRpcPro/        # Protobuf 服务定义
│   ├── raftRPC.proto       Raft 节点间 RPC
│   └── kvServerRPC.proto   客户端 RPC
├── rpc/               # 自定义 RPC 框架（MPRPC，基于 Muduo）
│   ├── rpcprovider.*       服务端请求分发
│   └── mprpcchannel.*      客户端 Channel
├── skipList/          # 内存数据结构
│   └── include/skipList.h  支持序列化的模板跳表
└── common/            # 公共工具
    ├── config.h            时序常量配置
    └── util.h/.cpp         Op 结构体、LockQueue、辅助函数
```

### 技术栈

| 关注点 | 技术 |
|---|---|
| 编程语言 | C++20 |
| 网络库 | Muduo（事件驱动异步 I/O） |
| RPC 序列化 | Protocol Buffers |
| 状态持久化 | Boost.Serialization |
| KV 数据结构 | 跳表（自实现，`std::shared_mutex`） |
| 构建系统 | CMake 3.22+ |

---

## 2. 模块职责

### 2.1 Raft 共识引擎（`src/raftCore/raft.h` / `raft.cpp`）

共识核心，管理节点状态、日志以及所有 Raft RPC。

**节点状态：** `Follower` → `Candidate` → `Leader`

**关键状态变量**

| 变量 | 作用 |
|---|---|
| `m_currentTerm` | 单调递增的选举任期号 |
| `m_votedFor` | 当前任期内投票给的候选人（-1 表示未投） |
| `m_logs` | 日志条目列表（`LogEntry{Command, LogTerm, LogIndex}`） |
| `m_commitIndex` | 已知已提交的最高日志索引 |
| `m_lastApplied` | 已应用到状态机的最高日志索引 |
| `m_nextIndex[i]` | 需发送给 follower i 的下一条日志索引 |
| `m_matchIndex[i]` | 已在 follower i 上复制的最高日志索引 |
| `m_lastSnapshotIncludeIndex/Term` | 最新快照的元数据 |

**后台线程**

| 线程 | 周期 | 职责 |
|---|---|---|
| `electionTimeOutTicker` | 300–500 ms（随机） | 检测 Leader 丢失，触发选举 |
| `leaderHearBeatTicker` | 25 ms | 维持 Leader 权威，复制日志 |
| `applierTicker` | 10 ms | 将已提交的日志推送至状态机 |

**KvServer 调用的公开 API**

```cpp
Start(command)          → (logIndex, term, isLeader)   // 提交新命令
Snapshot(index, data)   → void                          // 在 index 处压缩日志
GetState()              → (term, isLeader)
GetRaftStateSize()      → int                           // 驱动快照决策
```

**RPC 处理（节点间）**

```
AppendEntries   ← 日志复制 + 心跳
RequestVote     ← Leader 选举
InstallSnapshot ← 向落后的 follower 发送完整快照
```

---

### 2.2 KV 服务器（`src/raftCore/kvServer.h` / `kvServer.cpp`）

位于 Raft 之上的**状态机**。将客户端 RPC 转化为 Raft 日志条目，将已提交的命令应用到跳表，并管理快照。

**关键状态**

| 变量 | 作用 |
|---|---|
| `m_skipList` | 实际的 KV 数据库 |
| `waitApplyCh[raftIndex]` | 每请求独立 Channel，用于唤醒等待中的 RPC 处理线程 |
| `m_lastRequestId[clientId]` | 每个客户端已应用的最高 requestId（去重） |
| `m_maxRaftState` | 触发快照的 Raft 状态大小阈值 |

**客户端 RPC 处理**

```
Get(key, clientId, requestId)                         → (err, value)
PutAppend(key, value, op, clientId, requestId)        → (err)
```

**内部循环（`ReadRaftApplyCommandLoop`）**

独立线程持续消费 `applyChan`：

- `ApplyMsg.CommandValid = true` → `GetCommandFromRaft()`：去重、执行操作、唤醒等待者
- `ApplyMsg.SnapshotValid = true` → `GetSnapShotFromRaft()`：将快照安装到跳表

**快照触发条件**

每次应用命令后检查：
```
if raftNode.GetRaftStateSize() > m_maxRaftState / 10.0 → 调用 raftNode.Snapshot(...)
```
10% 阈值提供提前压缩，防止日志爆炸式增长。

---

### 2.3 持久化（`src/raftCore/Persister.h`）

每个节点对应两个独立文件：

```
raftstatePersist{nodeId}.txt   ← currentTerm, votedFor, lastSnapshot{Index,Term}, logs[]
snapshotPersist{nodeId}.txt    ← KV 状态序列化（skipList + lastRequestId 映射）
```

方法：`Save`、`SaveRaftState`、`ReadRaftState`、`ReadSnapshot`、`RaftStateSize`。

---

### 2.4 跳表（`src/skipList/include/skipList.h`）

模板类 `SkipList<K, V>`，概率平衡数据结构，插入/查找/删除平均 O(log n)。

- **线程安全：** `std::shared_mutex`——读操作并发，写操作独占。
- **持久化：** `dump_file()` 序列化为字符串；`load_file()` 恢复——直接用于快照机制。

---

### 2.5 Clerk 客户端（`src/raftClerk/clerk.h` / `clerk.cpp`）

客户端库，暴露简单的 `Get / Put / Append` 接口，屏蔽集群复杂性。

**去重标识**

```cpp
m_clientId   = UUID 字符串（每个进程唯一）
m_requestId  = 每次操作单调递增
```

**Leader 发现**

Clerk 缓存 `m_recentLeaderId`。收到 `ErrWrongLeader` 或 RPC 失败时，轮询所有服务器直到收到 `OK`。

---

### 2.6 RPC 框架（`src/rpc/`）

基于 Muduo 构建的自定义 RPC，使用 Protobuf 编码消息。

**协议格式**

```
[4B: header_len][header_bytes][4B: payload_len][payload_bytes]
```

Header 是包含 `service_name`、`method_name`、`args_size` 的 Protobuf 消息。

| 组件 | 职责 |
|---|---|
| `RpcProvider` | Muduo TcpServer；将请求分发到注册的 Protobuf 服务 |
| `MprpcChannel` | Protobuf `RpcChannel` 实现；管理到远端的 TCP 连接 |
| `MprpcController` | 跟踪每次 RPC 的状态与错误 |

---

### 2.7 配置参数（`src/common/config.h`）

```cpp
HeartBeatTimeout             = 25 ms     // 心跳/复制周期
ApplyInterval                = 10 ms     // Applier 线程轮询间隔
minRandomizedElectionTime    = 300 ms    // 选举超时下限
maxRandomizedElectionTime    = 500 ms    // 选举超时上限
CONSENSUS_TIMEOUT            = 500 ms    // 客户端等待提交的最大时间
```

选举超时（300–500 ms）是心跳（25 ms）的 12–20 倍，可容忍网络抖动而不触发误选举。

---

## 3. 核心工作流

### 3.1 Leader 选举

```
electionTimeOutTicker() 检测到 [300, 500] ms 内无心跳
         │
         ▼
doElection()
  status = Candidate
  currentTerm++
  votedFor = me
  persist()
         │
         │  向所有其他节点发送 RequestVote RPC
         ▼
每个节点检查：
  1. 如果 args.term < currentTerm → 拒绝
  2. 如果（候选人日志 ≥ 自身日志新）且（本任期未投票）→ 授予选票，重置选举计时器
         │
         ▼
候选人统计选票
  当 votes ≥ ⌊n/2⌋ + 1 时：
    status = Leader
    nextIndex[i]  = lastLogIndex + 1   （对所有 i）
    matchIndex[i] = 0
    立即广播心跳，抑制其他节点发起选举
```

**安全性：** 每个节点每个任期最多投一票；只有日志最新的候选人（更高的 lastLogTerm，或相同任期下更高的 lastLogIndex）才能当选。

---

### 3.2 日志复制

```
客户端 → KvServer.PutAppend()
           │
           ▼
      raft.Start(Op)          ← Op 追加到 Leader 日志，persist()
           │
           │  心跳触发（每 25 ms）
           ▼
doHeartBeat() 对每个 follower i：
  if nextIndex[i] ≤ lastSnapshotIncludeIndex：
    → leaderSendSnapShot(i)          （走快照路径）
  else：
    构建 AppendEntriesArgs{prevLogIndex, prevLogTerm, entries[nextIndex[i]..], leaderCommit}
    → sendAppendEntries(i)（新线程）
           │
           ▼
Follower AppendEntries1() 处理：
  Leader 任期 < Follower 任期 → 拒绝
  prevLogIndex/Term 不匹配 → 拒绝，返回 updateNextIndex 优化提示
  截断冲突后缀；追加新条目
  推进 commitIndex = min(leaderCommit, lastLogIndex)
           │
           ▼
Leader 收到 Success 回复：
  matchIndex[i] = prevLogIndex + len(entries)
  nextIndex[i]  = matchIndex[i] + 1
  leaderUpdateCommitIndex()：
    从 lastLogIndex 向前扫描：
      若 ≥ 多数节点已复制 且 该条目属于当前任期 → 更新 commitIndex
           │
           ▼
applierTicker()（每 10 ms）：
  对 [lastApplied+1, commitIndex] 中每条日志：
    推送 ApplyMsg{Command=serialized(Op), CommandIndex} → applyChan
    lastApplied++
           │
           ▼
KvServer.ReadRaftApplyCommandLoop() 收到 ApplyMsg
  → GetCommandFromRaft()：执行操作，唤醒等待中的 RPC 处理线程
```

---

### 3.3 客户端请求处理（Put / Append / Get）

```
Clerk.PutAppend(key, value, op)
  requestId++
  循环：
    向 servers[recentLeaderId] 发送 PutAppendArgs{key, value, op, clientId, requestId}
    如果 ErrWrongLeader / 超时 → 尝试下一个节点
    如果 OK → 返回
──────────────────────────────────────────────────────────
KvServer.PutAppend(args)：
  1. 非 Leader → 返回 ErrWrongLeader
  2. 构造 Op = {op, key, value, clientId, requestId}
  3. (raftIndex, _, _) = raft.Start(Op)
  4. 为 raftIndex 创建等待 Channel
  5. 在 Channel 上等待最多 CONSENSUS_TIMEOUT（500 ms）
     │
     ├─ 超时：
     │   ifRequestDuplicate(clientId, requestId) → 返回 OK（已应用）
     │   否则 → 返回 ErrWrongLeader
     │
     └─ 收到 Op：
         如果提交的 Op 与本次请求匹配 → 返回 OK
         否则 → 返回 ErrWrongLeader（日志已被覆盖）
──────────────────────────────────────────────────────────
GetCommandFromRaft(ApplyMsg)：
  解析 Op
  if requestId <= m_lastRequestId[clientId] → 跳过（重复请求）
  else：
    执行 Put / Append / Get 到 skipList
    m_lastRequestId[clientId] = requestId
  SendMessageToWaitChan(op, raftIndex)  ← 唤醒步骤 5 中的等待线程
  IfNeedToSendSnapShotCommand(raftIndex)
```

**线性一致性保证：** 每个操作都经过 Raft 共识；`(clientId, requestId)` 去重确保即使重试也只执行一次。

---

### 3.4 快照与日志压缩

**创建快照（由 KvServer 触发）**

```
应用第 index 条命令后：
  if raftStateSize > maxRaftState * 0.9：
    snapshotData = serialize(skipList + m_lastRequestId)
    raft.Snapshot(index, snapshotData)
         │
         ▼
Raft.Snapshot(index, data)：
  记录 lastSnapshotIncludeIndex = index 及对应 term
  丢弃 logs[0..index]，仅保留 logs[index+1..]
  persister.Save(raftState, snapshotData)
```

**安装快照（Leader → 落后的 Follower）**

```
Leader 检测到 nextIndex[i] ≤ lastSnapshotIncludeIndex
  → leaderSendSnapShot(i)
       发送 InstallSnapshotRequest{term, lastSnapshotIncludeIndex,
                                   lastSnapshotIncludeTerm, data}
         │
         ▼
Follower InstallSnapshot()：
  Leader 任期 < currentTerm → 拒绝
  丢弃快照点之前的日志
  更新 lastSnapshotInclude*、commitIndex、lastApplied
  推送 ApplyMsg{SnapshotValid=true, Snapshot=data} → applyChan
  persister.Save(raftState, data)
         │
         ▼
KvServer.GetSnapShotFromRaft()：
  ReadSnapShotToInstall(data)：
    skipList.load_file(...)
    m_lastRequestId = 反序列化的映射表
  更新 m_lastSnapShotRaftLogIndex
```

---

### 3.5 节点启动与崩溃恢复

```
raftKvDB main()：
  for i in 0..nodeCount-1：
    fork() → 子进程运行 KvServer(i, maxRaftState, configFile, port)

KvServer 构造函数：
  1. 创建 Persister(me)
  2. 分配共享 Raft 实例 + applyChan
  3. 启动 RPC 服务线程
       RpcProvider.NotifyService(kvServer)    ← 注册客户端 RPC
       RpcProvider.NotifyService(raftNode)    ← 注册节点间 RPC
       RpcProvider.Run(port)                  ← 启动 Muduo TcpServer
  4. Sleep 6s（等待所有节点 RPC 监听就绪）
  5. 读取配置文件 → 构建 peer 连接列表
  6. Sleep (n - myId)s（错峰连接，避免惊群效应）
  7. raft.init(peers, me, persister, applyChan)
       readPersist() → 恢复 currentTerm、votedFor、logs
       若存在快照：lastApplied = lastSnapshotIncludeIndex
       启动三个后台线程：leaderHearBeatTicker、electionTimeOutTicker、applierTicker
  8. 从持久化层加载快照到 KV 层（恢复 skipList + lastRequestId）
  9. 启动 ReadRaftApplyCommandLoop() 线程（永久运行）
```

**崩溃恢复场景**

| 场景 | 行为 |
|---|---|
| 全新启动 | 所有状态为零，选举后产生第一个 Leader |
| 崩溃后重启（有持久化状态） | 从磁盘恢复 term/logs；快照恢复 KV 状态；快照之后的日志重新应用 |
| 崩溃时为 Leader | 以 Follower 身份重启，选举超时触发新一轮选举 |
| 部分复制的条目 | 未提交的条目由新 Leader 提交或安全覆盖，不影响一致性 |

---

## 4. 时序与配置参数

```
HeartBeatTimeout          25 ms   → 复制周期，决定写延迟上界
electionTimeout       300–500 ms  → 随机化防止选票分裂
                                    约为心跳的 12–20 倍，容忍网络延迟
ApplyInterval             10 ms   → 已提交操作到达状态机的最大延迟
CONSENSUS_TIMEOUT        500 ms   → 客户端请求等待提交的超时时间
```

---

## 5. 安全性与活性保证

### 安全性（坏事不会发生）

| 属性 | 实现机制 |
|---|---|
| 选举安全性 | `votedFor` 保证每个节点每个任期最多投一票 |
| Leader 只追加 | Leader 从不覆盖或删除自身日志 |
| 日志匹配 | AppendEntries 前检查 `prevLogIndex` / `prevLogTerm` |
| Leader 完整性 | 只有日志最新的候选人才能当选 |
| 状态机安全性 | `(clientId, requestId)` 去重保证每条命令恰好执行一次 |

### 活性（好事终将发生）

| 属性 | 实现机制 |
|---|---|
| Leader 选举 | 随机化超时打破平票，最终选出 Leader |
| 日志收敛 | Leader 持续重试失败的复制 |
| 客户端进展 | Clerk 不断重试直到找到 Leader；服务端超时后提示客户端重试 |
| 快照进展 | 日志压缩防止磁盘/内存耗尽 |
