# 基于raft分布式kv存储

# Raft共识算法

Raft将来自集群外部的客户端请求称之为一条提案proposal，提案在被提交commit之前是不稳定的，需要Raft来让整个集群来决定，产生共识，确定是提交该提案还是抛弃它，此过程中该提案会被打包成为一条日志项log entry，在集群之间传播。在确定可以提交该log entry之后，会将其送入每个节点内部的状态机，成为一个无法被改变的事实。

## 复制状态机

**相同初始状态+相同输入 = 相同结束状态** 

多个节点上从相同数值状态开始，执行相同的一串命令，会产生相同的最终状态。

在Raft中。leader将客户段请求 `command` 封装到一个个日志 `log entry` 中，并将这些`log entries` 复制到所有foller节点，然后节点按照相同顺序应用命令，按照复制状态机的理念，最终结束状态是一样的。

## 状态简化

任何时刻，每一个服务器节点都处于 **leader, follower, candidate这三个状态之一。**

正常状态下，集群中只有一个leader，其余全部都是follower。follower是被动的，它们不会主动发出消息，只响应leader或candidate的消息。**在当前实现中，follower 收到客户端 KV 请求时不会转发给 leader，而是直接返回 `ErrWrongLeader`，由 Clerk 自行重试并寻找 leader。** candidate是一种临时的状态。

![image.png](image.png)

raft将时间分割成任意长度的任期（**term）任期用连续正数标记**

每一段任期从一次选举开始，如果一次选举无法选出leader，比如收到了相同的票数，此时这一任期就以无leader结束，一个新的任期很快就会重新开始，raft保证一个任期最多只有一个leader。

![image.png](image%201.png)

可以通过任期判断一个服务器的历史状态，比如根据服务器中是否有t2任期内的日志判断其在这个时间段内是否宕机。

服务器节点直接通过RPC通信，有两种RPC：

1. **`RequestVote RPC`** 请求投票，有candidate在选举期发起
2. `AppendEntries RPC` 追加条目，由leader发起，用来复制日志和提供一种心跳机制

服务器之间通信时会 **交换当前任期号** 如果一个服务器上的当前任期小于其他的，就将自己任期更新为较大的那个值。

如果一个候选人或者领导者发现自己任期号过期，则会立刻回到follower状态

**如果一个节点接收到一个包含过期的任期号请求， 直接拒绝**

## 领导者选举

- raft内部有**心跳机制**，leader会周期性向所有follower发送心跳以维持地位， 如果follower**一段时间内**未收到心跳，则可以认为领导者不可用，然后开始选举。
    
    > **follower怎么知道何时应该发起竞选？**
    > 
    
    每个节点内部都有一个被称之为选举时停electionTime的属性，当在此时间段内，没有收到来自leader的心跳消息，就可以认为当前没有leader存在，可以发起竞选。
    
- 开始选举后，follower会 **增加自己的当前任期号** 然后转换到candidate状态，然后 **投票给自己** 并并行的向集群中的其他服务器节点发送投票请求。

选举会产生三种结果：

1. 获得 **超过半数选票** 赢得选举 → 成为leader开始发送心跳
2. 其他节点赢得选举 → 收到**新的心跳**后， 如果 **新leader任期号大于等于自己的任期号** 则从candidate状态回到follower状态
3. 一段时间后没有获胜者， → 每个candidate在一个自己的 **随机选举时间后** 增加任期号开始新一轮投票 

*注：没有获胜者这一情况不需要集群中所有节点达成共识，因为领导者选举成功后会发送心跳*

**follower依据什么向candidate投赞成票**

投票的原则是先来先投票，在一个Term内只能投一次赞成票，如若下一个来请求投票的candidat的Term更大，那么投票者会重置自己的Term为最新然后重新投票。**节点不会向Term小于自身的节点投赞成票，同时日志Index小于自己的也不会赞成**，Term小于自身的消息可能来自新加入的节点，或者那些因为网络延迟而导致自身状态落后于集群的节点，这些节点自然无资格获得赞成票。

```cpp
// 请求投票RPC 请求Request
struct RequestVoteRequest {
	int term; //自己当前任期号
	int candidateId; //自己ID
	int lastLogIndex;//自己最后一个日志号
	int lastLogTerm;//自己最后一个日志任期号
};

//请求投票RPC 响应Response
struct RequestVoteResponse{
	int term; //自己当前任期号
	bool voteGranted; //自己会不会投票给这个候选人
}
```

对于没有成为候选人的follower节点，对同一个任期，会按照 **先来先得** 的原则投票

**选举流程总结：**

- follower长时间未收到来自leader的心跳消息，触发选举时停，转变为candidate，将Term加一，然后将新Term和自身的最新日志序号进行广播以期获取选票。
- 其他收到投票请求的节点先过滤掉Term小于自己的请求，然后判断：1. 自己是否已投票；2. 是否认为现在有leader；3. 该投票请求的日志index是否大于自己。若判断全通过则投赞成票。
- 收到过半数节点的赞成票的candidate将转变成为leader，开始定时发送心跳消息抑制其他节点的选举行为。
- 投票条件被用来保证leader选举的正确性（唯一），随机的选举时停则被用来保证选举的效率，因为集群处于选举状态时是无法对外提供服务的，应当尽量缩短该状态的持续时间，尽量减小竞选失败发生的概率。

## 日志复制

客户端如何得知新leader节点？

当前实现中，客户端 `Clerk` 会缓存一个最近成功的 `leaderId`：

1. 优先访问 `recentLeaderId`
2. 如果 RPC 失败或收到 `ErrWrongLeader`，则以轮询方式尝试下一个节点
3. 成功后更新 `recentLeaderId`

也就是说，**leader 发现是由客户端重试机制完成的，而不是 follower 把 leader ID 显式返回给客户端。**

当客户端向集群发送一条写请求时，Raft规定只有leader有权处理该请求。在当前实现中，如果 follower 或 candidate 收到该请求，会直接返回 `ErrWrongLeader`，由客户端自行重试，而不是服务端内部转发。

leader接收到指令就把指令作为新条目追加到日志，一个日志中需要有三个信息：

1. 状态机指令
2. leader任期号
3. 日志号（日志索引） 

![image.png](image%202.png)

*现在我们假设节点1此时发生故障重启，由leader变成follower，此时五个节点开始试图发起竞选。那么哪些节点有资格竞选成功呢？考虑到投赞成票的条件之一是日志需要和投票者相同或者更加新，所以我们就可以确定2、4号节点是不可能赢得选举(当然4号节点有Term落后的原因)，那么1、3、5号节点都有可能胜出，假设5号节点抢先一步获得了2、4节点的选票，那么它将成为leader，**我们可以看到成为leader的必要条件不是拥有最新的日志，而是要拥有最新的共识达成阶段的日志。***

日志号通常是自增且唯一的，但是由于节点可能存在的宕机问题，会出现日志号相同但是日志内容不同的问题，因此， **日志号和任期号两个因素才能唯一确定一个日志**

leader并行发送AppendEntries RPC 给follower 让他们复制条目。当这个条目被半数以上follower复制，则leader可以在 **本地执行该指令并将结果返回给客户端，** 将leader应用日志到状态机这一步称作**提交**。

*注： 在图中logindex为7的日志以及有包括leader在内的超过半数复制，则这个日志可以提交*

在此过程中,leader或follower随时都有崩溃或缓慢的可能性,Raft必须要在有宕机的情况下继续支持日志复制,并且保证每个副本日志顺序的一致(以保证复制状态机的实现)。具体有三种可能:

1. 如果有follower因为某些原因没有给leader响应,那么leader会不断地重发追加条目请求(AppendEntries RPC),哪怕leader已经回复了客户端，也就是说即使leader已经提交了，也不能放弃这个follower，仍然要不断重发，使得follower日志追上leader。
2. 如果有follower崩溃后恢复,这时Raft追加条目的一致性检查生效,保证follower能按顺序恢复崩溃后的缺失的日志。
    - **Raft的一致性检查**: leader在每一个发往follower的追加条目RPC中,会放入**前一个日志条目的索引位置和任期号，**如果follower在它的日志中找不到前一个日志,那么它就会拒绝此日志，leader收到follower的拒绝后，会发送**前一个日志条目，从而逐渐向前定位到follower第一个缺失的日志**。
3. 如果leader崩溃，那么崩溃的leader**可能已经复制了日志到部分follower，但还没有提交**，而被选出的新leader又可能不具备这些日志,这样就**有部分follower中的日志和新leader的日志不相同**。
    - Raft在这种情况下， leader通过**强制follower复制它的日志**来解决不一致的问题，这意味着**follower中跟leader冲突的日志条目会被新leader的日志条目覆盖**(因为没有提交,所以不违背外部一致性)。

通过这种机制，leader在当权后**不需要进行特殊的操作**就能使日志恢复到一致状态。Leader只需要进行正常的操作,然后日志就能在回复AppendEntries一致性检查失败的时候自动趋于一致。

而Leader从来不会覆盖或者删除自己的日志条目。(Append-Only)

通过这种日志复制机制，就可以保证一致性特性：

- 只要过半的服务器能正常运行，Raft就能够接受、复制并应用新的日志条且;
- 在正常情况下，新的日志条目可以在一个RPC来回中被复制给集群中的过半机器；
- 单个运行慢的follower不会影响整体的性能。

![image.png](image%203.png)

说明： 

1. `prevLogIndex` 和 `prevLogTerm` 用于进行一致性检查，只有这两个都与follower中的日志相同，follower才会认为日志一致。
2. follower在接收到leader的日志后不能立刻提交，只有当leader确认日志被复制到大多数节点后，leader才会提交这个日志，也就是应用到自己的状态机，并在RPC请求中通过 `leaderCommit`告知follower，然后follower就可以提交

### **Q:在raft算法中，leader commit后就返回给客户端，此时leader挂了。 flower还没有提交状态，是否会造成安全问题？**

A: 不会。通过**日志提交**规则以及**选举约束**保证

通过提交规则可以确定，此时leader以及确定当前日志被超过半数follower收到。而在leader给客户端提交后，还未广播就宕机，此时follower在心跳超时后开始选举，此时能赢得选举的必然是拥有这个日志的follower，因为这个日志是最新的，没有这个日志的follower不会赢得选票。

新leader在上任后，选举成功之后，新leader会立即广播一条空提案（`lastLogIndex、lastLogTerm`）（no-op机制），试图借此复制之前的所有日志到follower中，以期获得回应。而follower接收到以后，如果没有这个日志，就会拒绝这个RPC，leader会发送再前一个，以期使follower日志与leader同步；如果有这个日志，就返回给leader成功信息。

此时由于这个日志使大多数拥有的，所以leader可以知道这个日志是可以提交的，但是**新 leader 不能直接提交前任 leader 任期内的日志，**所以新 leader 先同步一条 “空日志”，待这条日志被大多数拥有 后，leader发送同步广播，即可安全提交所有≤该索引的、已被半数复制的前任日志。

## 安全性

定义规则使得算法在各类宕机问题下都不会出错：

- **leader** 宕机处理：被选举出的新leader一定包含了之前各任期的所有被提交的日志条目。
- **选举安全**：在一次任期内最多只有一个领导者被选出
- **leader 只添加操作**：领导者在其日志中只添加新条目，不覆盖删除条目
- **日志匹配**：如果两个log包含拥有相同索引和任期的条目，那么这两个log从之前到给定索引处的所有日志条目都是相同的
- **leader完整性**：如果在给定的任期中提交了一个日志条目，那么该entry将出现在所有后续更高任期号的领导者的日志中
- **状态机安全性**: 如果服务器已将给定索引处的日志条目应用于其状态机，则其他服务器不能在该索引位置应用别的日志条目

## 集群成员变更

在需要改变集群配置的时候（例如增减节点，替换宕机的机器），raft可以进行配置变更自动化。

在此过程中，需要防止转换过程中在一个任期内出现两个leader。

![image.png](image%204.png)

原本集群中有1、2、3三个节点，随后加入了4、5两个节点。集群有新成员的加入首先会通知当前leader，加入节点3是leader，他得知了成员变更的消息之后，其对于“大多数”的理解发生了改变，也就是说他现在认为当前集群成员共5个，3个成员才可以视为一个多数派。但是，在节点3将此信息同步给1、2之前，1、2节点仍然认为此时集群的多数派应该是2个。假如不幸地，集群在此时发生分区，1、2节点共处一个分区，剩下的节点在另一个分区。那么1、2节点会自己 开始选举，且能够选出一个合法的leader，只要某个节点获得2票即可。在另一边，也可以合法选出另一个leader，因为它有收获3票的可能性。此时一个集群内部出现了两个合法的leader，且它们也都有能力对外提供服务。这种情况称为“脑裂”

### 联合共识

为了解决一次加入多个新节点的问题，raft论文提出了一种称之为“联合共识”的技巧。我们知道如果一次加入多个节点，无法阻止两个多数派无交集的问题，但是只要限定两个多数派不能做出不同决定即可。联合共识的核心思想是引入一种中间状态，即让节点明白此时正处于集群成员变更的特殊时期，让它知晓两个集群的成员情况，

当该节点知道当前处于中间状态时，它的投票必须要同时获得两个集群的多数派的支持。以此保证两个多数派不会做出截然相反的行为。举个例子来说明联合共识的内容。这里我们用新旧配置表示节点对于集群情况的认知（因此成员变更也被成为配置变更），例如旧配置有3个节点的地址和序号，新配置有5个。

<aside>
💡

集群从 C_旧 切换到 C_新 的过程中，**任何需要 “法定人数（多数派）” 确认的操作（选举、日志提交），必须同时获得「C_旧 的多数派」和「C_ 新的多数派」的支持**。

但这个规则的**前提**是：**操作涉及的节点集，需同时覆盖新旧配置**；如果操作仅在**纯旧配置节点**中进行，那么「满足 C_ 旧多数派」就**等价于满足双配置多数派**。

</aside>

## **raft的可用性和一致性**

### 只读请求

对于客户端的只读请求,我们可以有以下几种方法进行处理:

- **follower自行处理**:一致性最低,最无法保证,效率最高。
- **leader立即回复**:leader可能并不合法(分区),导致数据不一致。
- **将读请求与写请求等同处理**:保证强一致,但性能最差。
- **ReadIndex方法**:经典做法是由 leader 在读前做一次合法性确认,随后等待状态机 apply 到请求到来时的 `commitIndex`,再执行本地读。
- **LeaseRead方法**:Leader取一个比ElectionTimeout小的租期,在租期内不会发生选举,确保Leader不会变,即可跳过群发确认消息的步骤。性能较高,一致性可能会受服务器时钟频率影响。

当前项目中的实际实现更接近“简化版 ReadIndex”：

- `KvServer::Get()` 会先检查自己是不是 leader
- 然后读取当前 `commitIndex` 作为 `readIndex`
- 再等待 `m_lastKVApplied >= readIndex`
- 满足后直接读取本地 `skipList`

也就是说，代码里**没有实现 LeaseRead**，也**没有实现一个完整的、每次读都向多数派再次确认 leader 身份的 ReadIndex 协议流程**。

对于读请求的处理能够让我们明显地看出共识算法和分布式系统一致性之间的关系。当共识达成时,即leader已将某entry提交,则整个系统已经具备了提供强一致服务的能力。但是,可能由于状态机执行失败、选择其他读请求处理方案等因素,导致在外部看来系统是不一致的。

### 网络分区

节点数量超过 n/2+1的一边可以继续提供服务，即使可能没有leader，也可以通过再次选举产生。而另一边的旧leader并不能发挥其作用，因为它并不能联系到大多数节点。

问题：raft属于A型还是C型？我们认为raft是一个强一致、高可用的共识算法。即使网络分区发生，只要由半数以上的节点处于同一个分区（假设60%），那么系统就能提供60%的服务，并且还能保证是强一致的。

### 预选举（概念说明）

由于接收不到leader的心跳，它们会自己开始选举，但由于人数过少不能成功选举出leader，所以会一直尝试竞选。它们的term可能会一直增加到一个危险的值。但分区结束时，通过心跳回应，原leader知道了集群中存在一群Term如此大的节点，它会修改自己的Term到已知的最大值，然后变为follower。此时集群中没有leader，需要重新选举。但是那几个原先被隔离的节点可能由于日志较为落后，而无法获得选举，新leader仍然会在原来的正常工作的那一部分节点中产生。

虽说最终结果没错，但是选举阶段会使得集群暂时无法正常工作，应该尽量避免这种扰乱集群正常工作的情况的发生，所以raft存在一种预选举的机制。真正的选举尽管失败还是会增加自己的Term，预选举失败后会将term恢复原样。只有预选举获得成功的candidate才能开始真正的选举。这样就可以避免不必要的term持续增加的情况发生。

这一节是 Raft 理论扩展说明，**当前代码并没有实现预选举**。现有实现中，选举超时后会直接进入 `doElection()`，并递增 `m_currentTerm`。

### 快照

长时间运作的集群，其内部的日志会不断增长。因此需要对其进行压缩，并持久化保存。对于新加入集群的节点，可以直接将压缩好的日志（称之为快照Snapshot）发送过去即可。

Raft使用的是一种快照机制，在每个节点达到一定条件后，可以把当前日志中的命令都写入快照，然后就可以把已经并入快照的日志删除。

此时如果一个节点长时间宕机或者新加入集群，那么leader直接向follower发送自己的快照

## 当前实现中的持久化、恢复与宕机语义

这一节描述的是**当前代码实现**，重点对应以下文件：

- `src/raftCore/raft.cpp`
- `src/raftCore/kvServer.cpp`
- `src/raftCore/Persister.cpp`
- `src/raftCore/include/Persister.h`

### 持久化层当前保存什么

当前实现中，每个节点的持久化由 `Persister` 统一管理。`Persister` 在内存中维护两份内容：

1. `raftState`
2. `snapshot`

其中：

- `raftState` 由 `Raft::persistData()` 序列化得到，包含：
  - `currentTerm`
  - `votedFor`
  - `lastSnapshotIncludeIndex`
  - `lastSnapshotIncludeTerm`
  - `logs`
- `snapshot` 由 `KvServer::MakeSnapShot()` 生成，包含：
  - 当前 KV 数据（跳表序列化结果）
  - `m_lastRequestId` 去重表

需要特别注意：以下状态**不持久化**，它们在重启后由 Leader 重新驱动恢复：

- `commitIndex`
- `lastApplied`
- `nextIndex[]`
- `matchIndex[]`

这符合 Raft 的常见实现方式：**真正的事实来源是“持久化日志 + 快照”，而不是运行时内存中的提交进度。**

### 持久化文件格式与本次修复

当前版本的 `Persister` 不再在启动时清空旧文件，而是把 `raftState + snapshot` 打包写入一个权威 bundle 文件：

```text
persisterPersist{nodeId}.bin
┌──────────────────────────────────────────────┐
│ header(magic, version, raftStateSize, ...)  │
├──────────────────────────────────────────────┤
│ raftState bytes                             │
├──────────────────────────────────────────────┤
│ snapshot bytes                              │
└──────────────────────────────────────────────┘
```

同时保留了对旧文件名的兼容读取：

- `raftstatePersist{nodeId}.txt`
- `snapshotPersist{nodeId}.txt`

如果新 bundle 不存在，节点会尝试从旧格式中恢复一次，然后立刻写回新 bundle。

本次修复后的写盘流程为：

1. 写入 `*.tmp`
2. `fsync(tmp)`
3. `rename(tmp, real)`
4. `fsync` 父目录

这样做的意义是：

- 宕机时不会破坏已经存在的旧持久化文件
- 不会出现“读到半截 raftState / snapshot”的问题
- 最坏情况下只是丢掉“本次尚未完成的持久化更新”

### Leader 的持久化流程

#### 1. 接收客户端写请求

客户端 `Put/Append` 到达 Leader 后，`KvServer::PutAppend()` 会调用 `Raft::Start(op, ...)`。

`Start()` 中的顺序是：

1. 检查当前节点是否仍是 Leader
2. 构造新的 `LogEntry`
3. 追加到 `m_logs`
4. 立刻调用 `persist()`
5. 通知复制线程尽快向 Follower 发送 `AppendEntries`

因此 Leader 的语义是：

**先把新日志持久化到本地，再开始复制给其他节点。**

#### 2. 日志提交与 apply

Leader 收到多数副本的成功确认后，才会推进 `commitIndex`。注意：

- `commitIndex` 是易失状态，不单独持久化
- 日志是否“真实存在”，看的是它有没有写进 `raftState`
- 日志是否“已经生效”，看的是它有没有被 apply 到状态机

Leader 的 `applierTicker()` 会在 `commitIndex > lastApplied` 时，把日志包装成 `ApplyMsg` 推给 `KvServer`，再由 `KvServer` 真正执行 `Put/Append` 到跳表。

所以 Leader 这边的完整顺序是：

```text
客户端请求
  -> 追加本地日志
  -> 持久化 raftState
  -> 复制到多数副本
  -> 推进 commitIndex
  -> apply 到 KV
  -> RPC 返回 OK
```

### Follower 的持久化流程

Follower 不会主动生成新日志，它的持久化主要来自两类 RPC：

1. `AppendEntries`
2. `InstallSnapshot`

#### 1. 接收普通日志：AppendEntries

Follower 在 `AppendEntries1()` 中会：

1. 检查并更新 term / status / votedFor
2. 校验 `prevLogIndex` / `prevLogTerm`
3. 合并新日志到 `m_logs`
4. 根据 `leaderCommit` 推进自己的 `commitIndex`
5. 在函数退出前执行 `persist()`

代码里使用了：

```cpp
DEFER { persist(); };
```

这意味着：

**Follower 回复 `AppendEntries` 成功之前，本地日志一定已经完成持久化。**

之后才是：

- `applierTicker()` 根据 `commitIndex` 推进 apply
- `KvServer::ReadRaftApplyCommandLoop()` 消费 `ApplyMsg`
- KV 状态机真正写入跳表

这也是 Raft 的关键分层：

- **日志持久化**：表示“这条日志已经被这个节点记住”
- **状态机 apply**：表示“这条日志已经真正生效到数据库”

两者不是同一步。

#### 2. 接收快照：InstallSnapshot

当 Follower 落后太多、Leader 已经把旧日志截断为快照后，Leader 会改为发送 `InstallSnapshot`。

Follower 收到后会：

1. 更新 term / status
2. 丢弃被快照覆盖的日志
3. 更新：
   - `m_commitIndex`
   - `m_lastApplied`
   - `m_lastSnapshotIncludeIndex`
   - `m_lastSnapshotIncludeTerm`
4. 构造 `ApplyMsg{SnapshotValid=true,...}` 交给 KV 层安装快照
5. 调用 `m_persister->Save(persistData(), snapshotBytes)` 同时持久化新的 `raftState + snapshot`

注意：当前实现里，**KV 层安装快照的异步通知先于最终 `Save()` 发出**。这不会破坏 Raft 安全性，但意味着存在一个小窗口：

- 内存里的新快照已经开始生效
- 磁盘上的新 bundle 还没完全 durable

如果此时宕机，节点重启后会回到“最后一次成功持久化”的状态，然后再由 Leader 重新把它拉齐。

### 重启恢复流程

#### 1. Raft 层恢复

节点启动时：

1. 先构造 `Persister`
2. 从 `ReadRaftState()` 恢复：
   - `currentTerm`
   - `votedFor`
   - `lastSnapshotIncludeIndex`
   - `lastSnapshotIncludeTerm`
   - `logs`
3. 如果存在快照边界，则把：
   - `commitIndex`
   - `lastApplied`
   初始化到快照点

这里恢复的是：**“最后一次成功持久化的 Raft 事实”**。

#### 2. KV 层恢复

`KvServer` 启动后会读取 `ReadSnapshot()`：

1. 若 snapshot 不为空，则反序列化恢复：
   - 跳表中的 KV 数据
   - `m_lastRequestId`
2. 把 `m_lastSnapShotRaftLogIndex` 与 `m_lastKVApplied` 对齐到快照边界

这一步的含义是：

- snapshot 内的数据直接恢复
- snapshot 之后但尚未 apply 的日志，将在后续重新由 Leader 驱动 apply

### 典型宕机场景与正确性分析

#### 1. Follower 在收到 AppendEntries 之前宕机

没有接收到日志，也不会返回成功响应。Leader 不会把它计入多数派，恢复后继续重发即可。

#### 2. Follower 在写内存后、持久化前宕机

这条日志等价于没接收成功，因为它来不及持久化，也来不及安全返回成功响应。恢复后继续同步。

#### 3. Follower 已持久化日志，但还没 apply 到 KV 就宕机

这是最典型也最正常的情况。

- 若该日志还未提交：它只是一个本地持久化但未生效的候选日志，未来可能被覆盖
- 若该日志已提交：重启后它会从磁盘恢复日志，再由 Leader 的 `leaderCommit` 或 snapshot 驱动继续 apply

因此这里不会破坏一致性。关键点是：

**日志是事实源，KV 数据库是由日志和快照重建出来的状态。**

#### 4. Leader 本地持久化了新日志，但还没复制到多数就宕机

这条日志未提交，未来可能被新 Leader 覆盖。Raft 允许未提交日志丢失，不影响正确性。

#### 5. Leader 已复制到多数，但自己还没 apply / 还没回复客户端就宕机

这条日志已经提交。后续能赢得选举的新 Leader 必须拥有不旧于多数派的日志，因此该日志不会丢。

客户端如果在收到响应前超时，会发起重试；KV 层通过 `(clientId, requestId)` 去重，不会重复执行写操作。

#### 6. Leader 已回复客户端 OK，部分 Follower 还没 apply 就宕机

只要客户端已经收到 `OK`，说明原 Leader 已经把该日志走到了“提交并 apply”的阶段。落后的 Follower 之后会通过继续收日志或收快照补齐状态。

#### 7. 在本地生成 snapshot 的过程中宕机

当前实现先在内存里更新快照边界与日志裁剪，再调用 `Persister::Save()` 原子写盘。若在 `Save()` 完成前崩溃，重启后会回到旧 bundle，即“这次 snapshot 没做成”，但不会出现持久化层面的半状态。

#### 8. 在安装 snapshot 的过程中宕机

可能发生两种情况：

- snapshot 已开始在内存中生效，但 bundle 还没 durable
- bundle 已 durable，但 KV 层还没完成安装

两种情况都不会破坏最终正确性，因为重启后节点只相信最后一次成功写入的 bundle，落后部分仍会由 Leader 重新同步。

### 目前实现中的剩余风险

当前版本已经解决了“启动时清空持久化文件”和“写一半留下坏文件”的问题，但仍有几个工程层面的注意点：

1. 若 bundle 文件本身已损坏，`Persister` 当前会直接抛异常，节点可能启动失败
2. `readPersist()` 依赖 Boost 反序列化，若数据损坏也可能恢复失败
3. `InstallSnapshot()` 的顺序是“先通知 KV 层安装，再执行最终持久化”，因此存在短暂的“内存状态领先 durable 状态”的窗口

这些问题更多影响的是**可用性与恢复体验**，而不是 Raft 的核心安全性。

# 项目示意图

![image.png](image%205.png)

# clerk 客户端代码解析

clerk是一个外部的客户端，用于向整个raft集群发起命令并接收相应。

**clerk 类代码：**

```cpp
class Clerk {
 private:
  std::vector<std::shared_ptr<raftServerRpcUtil>> m_servers;  //保存所有raft节点的fd
  std::string m_clientId;
  int m_requestId;
  int m_recentLeaderId;  //只是有可能是领导

  std::string Uuid() {
    return std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand());
  }  //用于返回随机的clientId

  //    MakeClerk  todo
  void PutAppend(std::string key, std::string value, std::string op);

 public:
  //对外暴露的三个功能和初始化
  void Init(std::string configFileName);
  std::string Get(std::string key);

  void Put(std::string key, std::string value);
  void Append(std::string key, std::string value);

 public:
  Clerk();
};
```

**clerk调用代码如下：**

```cpp
int main(){
    Clerk client;
    client.Init("test.conf");
    auto start = now();
    int count = 500;
    int tmp = count;
    while (tmp --){
        client.Put("x",std::to_string(tmp));
        std::string get1 = client.Get("x");
        std::printf("get return :{%s}\r\n",get1.c_str());
    }
    return 0;
	}
```

## 调用代码分析：

1. 初始化

客户端从一个config文件中读取了所有raft节点的ip和端口号，用于初始化客户端与其他raft节点的链接，从代码中可以看出，初始化实际上是将节点的ip与端口传入 `raftServerRpcUtil` 在这里面封装了**`kvServerRpc_Stub`** 也就是节点用于和上层状态机通信的服务器。

由rpc的知识可知，stub类的初始化是需要一个重写的channel类作为参数，在重写的mprpcChannel类中，设置了一个clientFd的成员函数，用以保存客户端用于和节点通信的socket。

```cpp

//clerk中的属性
std::vector<std::shared_ptr<raftServerRpcUtil>> m_servers;  //保存所有raft节点的fd

//clerk中链接所有raft节点的过程
for (const auto& item : ipPortVt) {
    std::string ip = item.first;
    short port = item.second;
    m_servers.push_back(std::make_shared<raftServerRpcUtil>(ip, port));
     //make_shared 提供了更高效、更安全的方式来构造 shared_ptr。它不仅优化了内存分配、减少了内存泄漏的风险，还能提升缓存的性能。
  }
  
  //rpcUtil的构造
  raftServerRpcUtil::raftServerRpcUtil(std::string ip, short port) {
  stub = new raftKVRpcProctoc::kvServerRpc_Stub(new MprpcChannel(ip, port, false));
}

//具体的rpc链接
MprpcChannel::MprpcChannel(string ip, short port, bool connectNow) : m_ip(ip), m_port(port), m_clientFd(-1) {
  if (!connectNow) {
    return;
  }  //可以允许延迟连接
  std::string errMsg;
  //通过调用newConnect函数，得到了一个与<ip,port>通信的clientFd
  auto rt = newConnect(ip.c_str(), port, &errMsg);
  int tryCount = 3;
  while (!rt && tryCount--) {
    std::cout << errMsg << std::endl;
    rt = newConnect(ip.c_str(), port, &errMsg);
  }
```

1. putAppend函数

`void **Clerk**::**PutAppend**(**std**::**string** key, **std**::**string** value, **std**::**string** op)`

将参数初始化后，调用 `m_server` 中stub类的方法然后检查返回值。这两个方法由于是rpc方法，具体的使用在server端详细介绍。

# KVserver代码解析

kvserver实际上是一个中间组件，负责沟通kvDB和raft节点，同时也会接收外部的请求完成相应。

**在该实现中，每个  `KVServer` 实例内部都包含一个 `Raft`节点。`KVServer` 负责对外提供 KV RPC 接口，并将请求提交给本地 Raft 节点进行一致性复制；Raft 在日志提交后通过 apply 通道回调 `KVServer` 执行状态机。两者运行在同一进程中，但在逻辑上严格分层。**

## KVServer初始化：启动一个 KVServer + 一个本地 Raft 节点

```cpp
KvServer::KvServer(...) {
  auto persister = std::make_shared<Persister>(me);
  applyChan = std::make_shared<LockQueue<ApplyMsg>>();
  m_raftNode = std::make_shared<Raft>();

  // 1. 启动 RPC Provider：同时发布 KVServer RPC 与 Raft RPC
  std::thread t([this, port]() {
    RpcProvider provider;
    provider.NotifyService(this);
    provider.NotifyService(this->m_raftNode.get());
    provider.Run(m_me, port);
  });
  t.detach();

  // 2. 读取配置文件，建立到其他 Raft 节点的 RPC 连接
  // 3. 调用 m_raftNode->init(...)
  // 4. 若本地存在 snapshot，则先恢复 KV 数据
  // 5. 启动 ReadRaftApplyCommandLoop()，持续消费 applyChan
}
```

当前代码中的真实启动顺序比上面的伪代码稍微复杂一些：

1. 先启动 RPC 服务线程
2. 休眠一段时间，等待所有节点都把监听端口拉起来
3. 从配置文件中读取所有节点的 `ip:port`
4. 为其他节点构造 `RaftRpcUtil`
5. 调用 `m_raftNode->init(servers, me, persister, applyChan)`
6. 若 `persister` 中已有 snapshot，则先恢复本地 KV 状态
7. 最后启动 `ReadRaftApplyCommandLoop()` 永久消费 apply 消息

## 如何处理外部请求：通过rpc

外部请求在当前实现中主要分为两类 RPC：

- `Get`
- `PutAppend`（由客户端侧 `Put` / `Append` 复用）

### Get()请求

当前代码中的 `Get` 已经不是“提交一条读日志再等待 apply”的实现，而是一个简化版的 `ReadIndex` 本地读。

实际流程如下：

1. `KvServer::Get()` 先调用 `m_raftNode->GetState(&term, &isLeader)` 检查当前节点是否还是 leader
2. 如果不是 leader，直接返回 `ErrWrongLeader`
3. 如果是 leader，则读取当前 `commitIndex`，把它当作本次读请求的 `readIndex`
4. 在条件变量 `m_kvApplyCv` 上等待，直到 `m_lastKVApplied >= readIndex`
5. 条件满足后，直接在本地 `skipList` 上执行读
6. 若 key 存在则返回 `OK + value`，否则返回 `ErrNoKey`

这意味着当前实现里：

- `Get` 不会调用 `Raft::Start()`
- `Get` 不会创建 `waitApplyCh[raftIndex]`
- `Get` 不会等待某条“读请求日志”被 apply
- `Get` 只要求“在开始读时已经提交的写入，必须已经被本机状态机应用完”

这样做仍然可以保证线性一致性，因为：

- leader 先拿到一个 `readIndex = commitIndex`
- 只要本机状态机已经 apply 到这个位置，那么这次本地读至少覆盖了该时刻全部已提交写入
- 如果等待期间 leader 发生切换，或者本机迟迟没有 apply 到位，就会超时返回 `ErrWrongLeader`，由 `Clerk` 重试

在当前代码中，推动 `m_lastKVApplied` 前进的是 `GetCommandFromRaft()`：

1. `ReadRaftApplyCommandLoop()` 持续从 `applyChan` 取出已经提交的日志
2. `KvServer` 对写请求执行状态机更新
3. 然后把 `m_lastKVApplied` 更新到 `message.CommandIndex`
4. 最后 `notify_all()` 唤醒可能在等待 `readIndex` 的 `Get`

因此，当前项目里的 `Get` 本质上是“先确认本机已经追平某个已提交边界，再做本地读”。

### PutAppend()请求

写请求仍然走完整的 Raft 日志复制流程。

当前实现中的执行路径是：

1. `KvServer::PutAppend()` 把 RPC 参数组装成 `Op`
2. 调用 `m_raftNode->Start(op, &raftIndex, &term, &isLeader)`
3. 如果当前节点不是 leader，则直接返回 `ErrWrongLeader`
4. 如果是 leader，则为这个 `raftIndex` 创建一个 `waitApplyCh[raftIndex]`
5. RPC 线程在这个队列上等待最多 `CONSENSUS_TIMEOUT`
6. 后台 `ReadRaftApplyCommandLoop()` 持续从 `applyChan` 取出已提交日志
7. `GetCommandFromRaft()` 先做幂等判断，再执行状态机更新
8. 执行完成后，调用 `SendMessageToWaitChan(op, message.CommandIndex)` 把结果发回等待中的 RPC 线程
9. RPC 线程收到与自己 `ClientId + RequestId` 匹配的 `Op` 后返回 `OK`

这里有几个和旧 README 不同、但很重要的事实：

1. 真正修改数据库的地方不是 RPC 线程，而是 `GetCommandFromRaft()`
2. RPC 超时不等于写入失败，只表示客户端没在超时时间内拿到确认
3. 幂等性由 `m_lastRequestId[ClientId]` 维护，重复请求不会重复执行
4. 当前代码里的 `Append` 并不是字符串拼接，而是和 `Put` 一样调用 `insert_set_element`，效果更接近覆盖写

因此，下面这个场景在当前实现中是允许的：

- leader 已经把某条写日志复制到多数派并提交
- 但客户端侧 RPC 因网络或等待超时，没有及时收到 `OK`
- 客户端重试时，可能已经连到了新的 leader

这时正确性仍然由两层机制保证：

1. 旧 leader 上已经提交的日志，按照 Raft 的领导者完备性，新的 leader 也必须最终持有
2. 新 leader 对同一个 `ClientId + RequestId` 再次 apply 时，会因为幂等检查而跳过重复执行

所以，客户端看到一次超时，不代表这条写入丢了；它表示“结果未知，需要继续重试确认”。

## raftCore 代码解析

### Init函数

`Raft::init()` 当前做的事情可以概括为：

1. 初始化内存状态：
   - `m_currentTerm = 0`
   - `m_status = Follower`
   - `m_commitIndex = 0`
   - `m_lastApplied = 0`
   - `m_votedFor = -1`
   - 初始化 `m_matchIndex` / `m_nextIndex`
2. 初始化快照边界：
   - `m_lastSnapshotIncludeIndex = 0`
   - `m_lastSnapshotIncludeTerm = 0`
3. 调用 `readPersist(m_persister->ReadRaftState())`，恢复持久化的 term、vote、snapshot 边界和日志
4. 如果恢复后已经存在快照边界，则把 `m_commitIndex` 和 `m_lastApplied` 对齐到 `m_lastSnapshotIncludeIndex`
5. 启动 3 个后台线程：

```cpp
std::thread t(&Raft::leaderHearBeatTicker, this);
t.detach();

std::thread t2(&Raft::electionTimeOutTicker, this);
t2.detach();

std::thread t3(&Raft::applierTicker, this);
t3.detach();
```

这 3 个线程分别负责：

- `leaderHearBeatTicker()`：leader 心跳与日志复制触发
- `electionTimeOutTicker()`：选举超时检测
- `applierTicker()`：把已经提交的日志转换成 `ApplyMsg` 推给 `KvServer`

### 选举流程以及主要函数：

![image.png](image%206.png)

1. **electionTimeOutTicker：负责检查是否发起选举，如果是时候则执行doElection**

这个循环会一直运行：

- 如果当前已经是 leader，就低频 sleep，避免空转
- 如果是 follower/candidate，就根据
  `随机选举超时时间 + m_lastResetElectionTime - 当前时间`
  计算这次还应该睡多久
- 睡醒后再检查：这段时间里 `m_lastResetElectionTime` 是否被刷新过
- 如果没有被刷新，说明真的超时，调用 `doElection()`

这里依赖的核心状态是 `m_lastResetElectionTime`。它会在收到有效 `AppendEntries`、给别人投票、或者自己发起新一轮选举时被更新。

1. **doElection**()

当前代码中的 `doElection()` 会：

1. 把自己切成 `Candidate`
2. `m_currentTerm += 1`
3. `m_votedFor = m_me`
4. 立即 `persist()`
5. 重置 `m_lastResetElectionTime`
6. 构造 `RequestVoteArgs`
7. 通过内部 `ThreadPool` 并发向其他节点发送 `RequestVote`
8. `sendRequestVote` 

`sendRequestVote()` 的处理规则和标准 Raft 一致：

- RPC 失败则忽略
- 如果 `reply.term() > m_currentTerm`，说明自己过期，退回 follower，并更新 term / votedFor
- 如果 `reply.term() < m_currentTerm`，说明这是旧回复，忽略
- 只有 term 对齐且 `votegranted == true` 时才累加票数
- 一旦达到多数票，就切成 leader，并初始化：
  - `m_nextIndex = lastLogIndex + 1`
  - `m_matchIndex = 0`
- 然后立刻通过线程池提交一次 `doHeartBeat()`，马上广播领导者身份

当前代码已经处理了“同一任期重复收到胜选回复”的情况：如果这时已经是 leader，会直接忽略，而不是像旧文档里那样断言退出。

1. `RequestVote()` 

`RequestVote()` 当前的判断顺序是：

1. 候选人的 `term` 更小则拒绝
2. 候选人的 `term` 更大则先把自己更新到该 term，并退回 follower
3. 检查候选人的日志是否至少和自己一样新
4. 如果自己已经投给别人，则拒绝
5. 否则投票给候选人，并重置 `m_lastResetElectionTime`

另外，这个函数末尾有 `DEFER { persist(); }`，因此 term 变化和投票结果会落盘。

### 日志复制与心跳机制主要函数：

![image.png](image%207.png)

1. leaderHeartBeatTicker:负责检查是否要发送心跳，如果发起就执行heartbeat

当前实现已经不再使用“固定 sleep，到期后再发心跳”的方式。

现在的 `leaderHearBeatTicker()` 是：

- 不是 leader 时，每 50ms 低频轮询一次
- 成为 leader 后，在 `m_replicateCv` 上等待
- 两种情况会唤醒它：
  - `HeartBeatTimeout` 到期，发送周期性心跳
  - `Start()` 写入新日志后把 `m_hasNewEntry` 设为 `true` 并 `notify_one()`，于是立刻触发复制

所以它本质上是“周期性心跳 + 新日志即时复制”共用一个循环。
1. `doHeartBeat()` 

`doHeartBeat()` 在 leader 身份下执行，主要负责给每个 follower 决定“这次发快照还是发日志”：

1. 如果 `m_nextIndex[i] <= m_lastSnapshotIncludeIndex`，说明 follower 落后得太多，普通日志已经被 leader 的快照裁掉了
   - 这时不发 `AppendEntries`
   - 而是通过线程池提交 `leaderSendSnapShot(i)`
2. 否则构造 `AppendEntriesArgs`
   - `prevLogIndex` / `prevLogTerm` 由 `getPrevLogInfo()` 给出
   - `leaderCommit = m_commitIndex`
   - `entries` 从 follower 需要的位置一直带到 leader 当前最后一条日志
3. 然后通过线程池提交 `sendAppendEntries(...)`
4. 本轮广播结束后刷新 `m_lastResetHearBeatTime`

1. **`sendAppendEntries()`** 

`sendAppendEntries()` 的当前逻辑可以概括为：

1. 先发 RPC
2. 如果网络失败或返回 `Disconnected`，本轮放弃
3. 如果发现对方 term 更大，自己降级为 follower
4. 如果自己已经不是 leader，也忽略这份回复
5. 如果 `reply.success() == false`
   - 使用 follower 返回的 `updatenextindex` 回退 `m_nextIndex[server]`
6. 如果 `reply.success() == true`
   - 更新 `m_matchIndex[server]`
   - 更新 `m_nextIndex[server]`
   - 统计成功复制数量
   - 如果这次复制已经达到多数派，并且这批日志的最后一条属于当前任期，就推进 `m_commitIndex`
   - `m_commitIndex` 推进后会 `notify_one()` 唤醒 `applierTicker()`

这里保留了 Raft 的关键约束：leader 只会用“当前任期已经复制到多数派的日志”推进 `commitIndex`，而不会直接用旧任期日志推进。

1. `AppendEntries()` 

Follower 侧的 `AppendEntries1()` 当前流程是：

1. 先比较 term
   - leader term 更小：拒绝
   - leader term 更大：更新自己 term，并降为 follower
2. 函数退出时会 `persist()`，因此 term / vote / 日志变化会落盘
3. 刷新 `m_lastResetElectionTime`
4. 检查 `prevLogIndex`
   - 如果比自己最后日志还大，返回 `getLastLogIndex() + 1`
   - 如果已经落到自己快照之前，返回 `m_lastSnapshotIncludeIndex + 1`
5. 如果 `prevLogIndex + prevLogTerm` 能匹配
   - 逐条合并新日志
   - 已经被快照截断的日志会直接跳过
   - 如果 `leaderCommit > m_commitIndex`，就把本地 `m_commitIndex` 推到
     `min(leaderCommit, getLastLogIndex())`
   - 然后唤醒 `applierTicker()`
6. 如果前缀不匹配
   - 按冲突 term 回退 `updatenextindex`
   - 让 leader 下一轮更快找到共同前缀

当前实现还做了一个和旧版本不同的取舍：对于“同 index、同 term、但 command 不同”的异常情况，不再 `assert` 直接退出，而是记录日志后跳过，避免节点本身崩溃。

### applierTicker() 与状态机交付

当前实现里的 `applierTicker()` 也已经不是固定 `sleepNMilliseconds(ApplyInterval)` 轮询。

现在它的工作模式是：

1. 在 `m_applyCv` 上等待
2. 如果：
   - leader 推进了 `commitIndex`
   - follower 因 `AppendEntries` 推进了 `commitIndex`
   - 安装快照后推进了边界
   就会收到 `notify_one()`
3. 被唤醒后调用 `getApplyLogs()`
4. 把 `lastApplied < commitIndex` 的日志转换成 `ApplyMsg`
5. 推到 `applyChan`

随后 `KvServer::ReadRaftApplyCommandLoop()` 再把这些 `ApplyMsg` 真正交给 KV 状态机执行。

# 跳表部分

跳表是一种多层级的有序链表，是一种概率型的数据结构，可以实现平均 `O(logn)` ， 最坏 `O(n)` 的查询速度

实现思路：

1. 底层（Level 0） ：包含了所有节点的完整有序链表
2. 上层（Level 1/2/…) ： 索引层，越上层索引的节点越少
3. 查询时先从最上层索引定位到目标范围，逐层的下降到最底层，最终找到目标节点

## 并发控制

当前项目使用了读写锁，对于读操作，使用读锁，对于插入以及删除操作使用独占锁。

### 读写锁

相比互斥锁,读写锁允许更高的并行性。互斥量要么锁住状态,要么不加锁,而且一次只有一个线程可以加锁。读写锁可以有三种状态:

- 读模式加锁状态;
- 写模式加锁状态;
- 不加锁状态。

只有一个线程可以占有写模式的读写锁,但是可以有多个线程占有读模式的读写锁。

**读写锁也叫做"共享-独占锁",当读写锁以读模式锁住时,它是以共享模式锁住的;当它以写模式锁住时,它是以独占模式锁住的。**

1. 当锁处于写加锁状态时,在其解锁之前,所有尝试对其加锁的线程都会被阻塞;
2. 当锁处于读加锁状态时,所有试图以读模式对其加锁的线程都可以得到访问权,但是如果想以写模式对其加锁,线程将阻塞。这样也有问题,如果读者很多,那么写者将会长时间等待。如果有线程尝试以写模式加锁,那么后续的读线程将会被阻塞,这样可以避免锁长期被读者占有

### `shared_mutex`

C++17起。 `shared_mutex` 类是一个同步原语，可用于保护共享数据不被多个线程同时访问。与便于独占访问的其他互斥类型不同， `shared_mutex` 拥有二个访问级别：

- 共享 - 多个线程能共享同一互斥的所有权；
- 独占性 - 仅一个线程能占有互斥。
1. 若一个线程已经通过lock或try_lock获取独占锁（写锁），则无其他线程能获取该锁（包括共享的）。尝试获得读锁的线程也会被阻塞。
2. 仅当任何线程均未获取独占性锁时，共享锁（读锁）才能被多个线程获取（通过 lock_shared 、try_lock_shared ）。
3. 在一个线程内，同一时刻只能获取一个锁（共享或独占性）。

[面试问题](https://www.notion.so/31f6d0b6f53f8075be4cd6cf444be7c2?pvs=21)
