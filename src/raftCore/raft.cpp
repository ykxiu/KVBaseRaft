#include "raft.h"
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <memory>
#include "config.h"
#include "util.h"

void Raft::AppendEntries1(const raftRpcProctoc::AppendEntriesArgs* args, raftRpcProctoc::AppendEntriesReply* reply) {
  std::lock_guard<std::mutex> locker(m_mtx);
  reply->set_appstate(AppNormal);  // 能接收到代表网络是正常的
  //	不同的人收到AppendEntries的反应是不同的，要注意无论什么时候收到rpc请求和响应都要检查term

  if (args->term() < m_currentTerm) {
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    reply->set_updatenextindex(-100);  // 论文中：让领导人可以及时更新自己
    DPrintf("[func-AppendEntries-rf{%d}] 拒绝了 因为Leader{%d}的term{%v}< rf{%d}.term{%d}\n", m_me, args->leaderid(),
            args->term(), m_me, m_currentTerm);
    return;  // 注意从过期的领导人收到消息不要重设定时器
  }
  //由于这个局部变量创建在锁之后，因此执行persist的时候应该也是拿到锁的.
  DEFER { persist(); };  // 由于这个局部变量创建在锁之后，因此执行persist的时候应该也是拿到锁的.
  if (args->term() > m_currentTerm) {
    // 三变 ,防止遗漏，无论什么时候都是三变
    m_status = Follower;
    m_currentTerm = args->term();
    m_votedFor = -1;
    // 这里可不返回，应该改成让改节点尝试接收日志
    // 如果是领导人和candidate突然转到Follower好像也不用其他操作
    // 如果本来就是Follower，那么其term变化，相当于“不言自明”的换了追随的对象，因为原来的leader的term更小，是不会再接收其消息了
  }
  if (args->term() != m_currentTerm) {
    DPrintf("[func-AppendEntries1-rf{%d}] args->term()(%d) != m_currentTerm(%d), ignoring", m_me, args->term(),
            m_currentTerm);
    return;
  }
  // 如果发生网络分区，那么candidate可能会收到同一个term的leader的消息，要转变为Follower，为了和上面，因此直接写
  m_status = Follower;  // 这里是有必要的，因为如果candidate收到同一个term的leader的AE，需要变成follower
  // term相等
  m_lastResetElectionTime = now();
 

  // 不能无脑的从prevlogIndex开始阶段日志，因为rpc可能会延迟，导致发过来的log是很久之前的

  //	那么就比较日志，日志有3种情况
  if (args->prevlogindex() > getLastLogIndex()) {
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    reply->set_updatenextindex(getLastLogIndex() + 1);
    return;
  } else if (args->prevlogindex() < m_lastSnapshotIncludeIndex) {
    // 如果prevlogIndex还没有更上快照
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    reply->set_updatenextindex(m_lastSnapshotIncludeIndex + 1);
    DPrintf("[func-AppendEntries-rf{%d}] prevlogindex{%d} < lastSnapshotIncludeIndex{%d}, reject", m_me,
            args->prevlogindex(), m_lastSnapshotIncludeIndex);
    return;  // 必须 return，否则 fall-through 到 matchLog 会触发越界 assert
  }
 
  if (matchLog(args->prevlogindex(), args->prevlogterm())) {
    for (int i = 0; i < args->entries_size(); i++) {
      auto log = args->entries(i);
      // 跳过已经被快照截断的日志条目，防止 getSlicesIndexFromLogIndex 越界 assert
      if (log.logindex() <= m_lastSnapshotIncludeIndex) {
        continue;
      }
      if (log.logindex() > getLastLogIndex()) {
        // 超过就直接添加日志
        m_logs.push_back(log);
      } else {
        
        if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() == log.logterm() &&
            m_logs[getSlicesIndexFromLogIndex(log.logindex())].command() != log.command()) {
          // 相同位置的log ，其logTerm相等，但是命令却不相同，不符合raft的前向匹配，异常了！
          DPrintf("[func-AppendEntries-rf{%d}] 两节点logIndex{%d}和term{%d}相同，但是其command不同，跳过\n",
                  m_me, log.logindex(), log.logterm());
          continue;
        }
        if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() != log.logterm()) {
          // 不匹配就更新
          m_logs[getSlicesIndexFromLogIndex(log.logindex())] = log;
        }
      }
    }

    
    if (getLastLogIndex() < args->prevlogindex() + args->entries_size()) {
      DPrintf(
          "[func-AppendEntries1-rf{%d}] warn: getLastLogIndex{%d} < prevlogindex{%d}+entries_size{%d} (entries in "
          "snapshot skipped)",
          m_me, getLastLogIndex(), args->prevlogindex(), args->entries_size());
    }

    if (args->leadercommit() > m_commitIndex) {
      m_commitIndex = std::min(args->leadercommit(), getLastLogIndex());
      // 优化2：commitIndex 推进，通知 applierTicker 立即 apply，无需等 10ms
      m_applyCv.notify_one();
    }

    // 防止 commitIndex 超过 lastLogIndex（快照安装后可能出现边界情况）
    if (getLastLogIndex() < m_commitIndex) {
      DPrintf("[func-AppendEntries1-rf{%d}] warn: lastLogIndex{%d} < commitIndex{%d}, clamping commitIndex", m_me,
              getLastLogIndex(), m_commitIndex);
      m_commitIndex = getLastLogIndex();
    }
    reply->set_success(true);
    reply->set_term(m_currentTerm);
    return;
  } else {

    reply->set_updatenextindex(args->prevlogindex());

    for (int index = args->prevlogindex(); index >= m_lastSnapshotIncludeIndex; --index) {
      if (getLogTermFromLogIndex(index) != getLogTermFromLogIndex(args->prevlogindex())) {
        reply->set_updatenextindex(index + 1);
        break;
      }
    }
    reply->set_success(false);
    reply->set_term(m_currentTerm);

    return;
  }
}

void Raft::applierTicker() {
  // 优化2：用条件变量替代固定 10ms sleep。
  // commitIndex 推进时（follower AppendEntries / leader sendAppendEntries / InstallSnapshot）
  // 均会 notify_one，使 applier 立即 apply，消除固定轮询延迟。
  while (true) {
    {
      std::unique_lock<std::mutex> lk(m_applyCvMtx);
      // 等待 commitIndex 推进通知，或 10ms 兜底超时
      m_applyCv.wait_for(lk, std::chrono::milliseconds(ApplyInterval));
    }

    m_mtx.lock();
    if (m_status == Leader) {
      DPrintf("[Raft::applierTicker() - raft{%d}]  m_lastApplied{%d}   m_commitIndex{%d}", m_me, m_lastApplied,
              m_commitIndex);
    }
    auto applyMsgs = getApplyLogs();
    m_mtx.unlock();

    if (!applyMsgs.empty()) {
      DPrintf("[func- Raft::applierTicker()-raft{%d}] 向kvserver報告的applyMsgs長度爲：{%d}", m_me, applyMsgs.size());
    }
    for (auto& message : applyMsgs) {
      applyChan->Push(message);
    }
  }
}

bool Raft::CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot) { return true; }

void Raft::doElection() {
  std::lock_guard<std::mutex> g(m_mtx);

  if (m_status != Leader) {
    DPrintf("[       ticker-func-rf(%d)              ]  选举定时器到期且不是leader，开始选举 \n", m_me);
    // 当选举的时候定时器超时就必须重新选举，不然没有选票就会一直卡主
    // 重竞选超时，term也会增加的
    m_status = Candidate;
    /// 开始新一轮的选举
    m_currentTerm += 1;
    m_votedFor = m_me;  // 即是自己给自己投，也避免candidate给同辈的candidate投
    persist();
    std::shared_ptr<int> votedNum = std::make_shared<int>(1);  // 使用 make_shared 函数初始化 !! 亮点
    //	重新设置定时器
    m_lastResetElectionTime = now();
    //	发布RequestVote RPC
    for (int i = 0; i < m_peers.size(); i++) {
      if (i == m_me) {
        continue;
      }
      int lastLogIndex = -1, lastLogTerm = -1;
      getLastLogIndexAndTerm(&lastLogIndex, &lastLogTerm);  // 获取最后一个log的term和下标

      std::shared_ptr<raftRpcProctoc::RequestVoteArgs> requestVoteArgs =
          std::make_shared<raftRpcProctoc::RequestVoteArgs>();
      requestVoteArgs->set_term(m_currentTerm);
      requestVoteArgs->set_candidateid(m_me);
      requestVoteArgs->set_lastlogindex(lastLogIndex);
      requestVoteArgs->set_lastlogterm(lastLogTerm);
      auto requestVoteReply = std::make_shared<raftRpcProctoc::RequestVoteReply>();

      // 使用匿名函数执行避免其拿到锁

      m_threadPool.submit([this, i, requestVoteArgs, requestVoteReply, votedNum]() {
        this->sendRequestVote(i, requestVoteArgs, requestVoteReply, votedNum);
      });
    }
  }
}

void Raft::doHeartBeat() {
  std::lock_guard<std::mutex> g(m_mtx);

  if (m_status == Leader) {
    DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了且拿到mutex，开始发送AE\n", m_me);
    auto appendNums = std::make_shared<int>(1);  // 正确返回的节点的数量

    // 对Follower（除了自己外的所有节点发送AE）
    //  todo 这里肯定是要修改的，最好使用一个单独的goruntime来负责管理发送log，因为后面的log发送涉及优化之类的
    // 最少要单独写一个函数来管理，而不是在这一坨
    for (int i = 0; i < m_peers.size(); i++) {
      if (i == m_me) {
        continue;
      }
      DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了 index:{%d}\n", m_me, i);
      if (m_nextIndex[i] < 1) {
        DPrintf("[func-doHeartBeat-rf{%d}] warn: m_nextIndex[%d]=%d < 1, clamping to 1", m_me, i, m_nextIndex[i]);
        m_nextIndex[i] = 1;
      }
      // 日志压缩加入后要判断是发送快照还是发送AE
      if (m_nextIndex[i] <= m_lastSnapshotIncludeIndex) {
         // 创建新线程并执行b函数，并传递参数
        m_threadPool.submit([this, i]() { this->leaderSendSnapShot(i); });
        continue;
      }
      // 构造发送值
      int preLogIndex = -1;
      int PrevLogTerm = -1;
      getPrevLogInfo(i, &preLogIndex, &PrevLogTerm);
      std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> appendEntriesArgs =
          std::make_shared<raftRpcProctoc::AppendEntriesArgs>();
      appendEntriesArgs->set_term(m_currentTerm);
      appendEntriesArgs->set_leaderid(m_me);
      appendEntriesArgs->set_prevlogindex(preLogIndex);
      appendEntriesArgs->set_prevlogterm(PrevLogTerm);
      appendEntriesArgs->clear_entries();
      appendEntriesArgs->set_leadercommit(m_commitIndex);
      if (preLogIndex != m_lastSnapshotIncludeIndex) {
        for (int j = getSlicesIndexFromLogIndex(preLogIndex) + 1; j < m_logs.size(); ++j) {
          raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
          *sendEntryPtr = m_logs[j];  //=是可以点进去的，可以点进去看下protobuf如何重写这个的
        }
      } else {
        for (const auto& item : m_logs) {
          raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
          *sendEntryPtr = item;  //=是可以点进去的，可以点进去看下protobuf如何重写这个的
        }
      }
      int lastLogIndex = getLastLogIndex();
      // leader对每个节点发送的日志长短不一，但是都保证从prevIndex发送直到最后
      if (appendEntriesArgs->prevlogindex() + appendEntriesArgs->entries_size() != lastLogIndex) {
        DPrintf(
            "[func-doHeartBeat-rf{%d}] warn: prevlogindex{%d}+entries_size{%d} != lastLogIndex{%d}",
            m_me, appendEntriesArgs->prevlogindex(), appendEntriesArgs->entries_size(), lastLogIndex);
      }
      // 构造返回值
      const std::shared_ptr<raftRpcProctoc::AppendEntriesReply> appendEntriesReply =
          std::make_shared<raftRpcProctoc::AppendEntriesReply>();
      appendEntriesReply->set_appstate(Disconnected);

      m_threadPool.submit([this, i, appendEntriesArgs, appendEntriesReply, appendNums]() {
        this->sendAppendEntries(i, appendEntriesArgs, appendEntriesReply, appendNums);
      });
    }
    m_lastResetHearBeatTime = now();  // leader发送心跳，就不是随机时间了
  }
}

void Raft::electionTimeOutTicker() {
  // Check if a Leader election should be started.
  while (true) {
    /**
     * 如果不睡眠，那么对于leader，这个函数会一直空转，浪费cpu。且加入协程之后，空转会导致其他协程无法运行，对于时间敏感的AE，会导致心跳无法正常发送导致异常
     */
    while (m_status == Leader) {
      // 优化A：从 HeartBeatTimeout(25ms) 提高到 100ms，减少 Leader 节点无效唤醒频率 75%。
      // 100ms << minRandomizedElectionTime(300ms)，不影响选举时序安全性。
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
    std::chrono::system_clock::time_point wakeTime{};
    {
      m_mtx.lock();
      wakeTime = now();
      suitableSleepTime = getRandomizedElectionTimeout() + m_lastResetElectionTime - wakeTime;
      m_mtx.unlock();
    }

    if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) {
      // 获取当前时间点
      auto start = std::chrono::steady_clock::now();

      usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
      // std::this_thread::sleep_for(suitableSleepTime);

      // 获取函数运行结束后的时间点
      auto end = std::chrono::steady_clock::now();

      // 计算时间差并输出结果（单位为毫秒）
      std::chrono::duration<double, std::milli> duration = end - start;

      // 使用ANSI控制序列将输出颜色修改为紫色
      std::cout << "\033[1;35m electionTimeOutTicker();函数设置睡眠时间为: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                << std::endl;
      std::cout << "\033[1;35m electionTimeOutTicker();函数实际睡眠时间为: " << duration.count() << " 毫秒\033[0m"
                << std::endl;
    }

    if (std::chrono::duration<double, std::milli>(m_lastResetElectionTime - wakeTime).count() > 0) {
      // 说明睡眠的这段时间有重置定时器，那么就没有超时，再次睡眠
      continue;
    }
    doElection();
  }
}

std::vector<ApplyMsg> Raft::getApplyLogs() {
  std::vector<ApplyMsg> applyMsgs;
  if (m_commitIndex > getLastLogIndex()) {
    DPrintf("[func-getApplyLogs-rf{%d}] warn: commitIndex{%d} > getLastLogIndex{%d}, clamping", m_me, m_commitIndex,
            getLastLogIndex());
    m_commitIndex = getLastLogIndex();
  }

  while (m_lastApplied < m_commitIndex) {
    m_lastApplied++;
    if (m_lastApplied <= m_lastSnapshotIncludeIndex) {
      // 该 index 已在快照中，跳过
      continue;
    }
    if (m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex() != m_lastApplied) {
      DPrintf("rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogIndex{%d} != rf.lastApplied{%d}, break",
              m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex(), m_lastApplied);
      break;
    }
    ApplyMsg applyMsg;
    applyMsg.CommandValid = true;
    applyMsg.SnapshotValid = false;
    applyMsg.Command = m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].command();
    applyMsg.CommandIndex = m_lastApplied;
    applyMsgs.emplace_back(applyMsg);
    //        DPrintf("[	applyLog func-rf{%v}	] apply Log,logIndex:%v  ，logTerm：{%v},command：{%v}\n",
    //        rf.me, rf.lastApplied, rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogTerm,
    //        rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].Command)
  }
  return applyMsgs;
}

// 获取新命令应该分配的Index
int Raft::getNewCommandIndex() {
  //	如果len(logs)==0,就为快照的index+1，否则为log最后一个日志+1
  auto lastLogIndex = getLastLogIndex();
  return lastLogIndex + 1;
}

// getPrevLogInfo
// leader调用，传入：服务器index，传出：发送的AE的preLogIndex和PrevLogTerm
void Raft::getPrevLogInfo(int server, int* preIndex, int* preTerm) {
  // logs长度为0返回0,0，不是0就根据nextIndex数组的数值返回
  if (m_nextIndex[server] == m_lastSnapshotIncludeIndex + 1) {
    // 要发送的日志是第一个日志，因此直接返回m_lastSnapshotIncludeIndex和m_lastSnapshotIncludeTerm
    *preIndex = m_lastSnapshotIncludeIndex;
    *preTerm = m_lastSnapshotIncludeTerm;
    return;
  }
  auto nextIndex = m_nextIndex[server];
  *preIndex = nextIndex - 1;
  *preTerm = m_logs[getSlicesIndexFromLogIndex(*preIndex)].logterm();
}

// GetState return currentTerm and whether this server
// believes it is the Leader.
void Raft::GetState(int* term, bool* isLeader) {
  m_mtx.lock();
  DEFER {
    // todo 暂时不清楚会不会导致死锁
    m_mtx.unlock();
  };

  // Your code here (2A).
  *term = m_currentTerm;
  *isLeader = (m_status == Leader);
}

void Raft::InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest* args,
                           raftRpcProctoc::InstallSnapshotResponse* reply) {
  m_mtx.lock();
  DEFER { m_mtx.unlock(); };
  if (args->term() < m_currentTerm) {
    reply->set_term(m_currentTerm);

    return;
  }
  if (args->term() > m_currentTerm) {
    // 后面两种情况都要接收日志
    m_currentTerm = args->term();
    m_votedFor = -1;
    m_status = Follower;
    persist();
  }
  m_status = Follower;
  m_lastResetElectionTime = now();
  // outdated snapshot
  if (args->lastsnapshotincludeindex() <= m_lastSnapshotIncludeIndex) {
    //        DPrintf("[func-InstallSnapshot-rf{%v}] leader{%v}.LastSnapShotIncludeIndex{%v} <=
    //        rf{%v}.lastSnapshotIncludeIndex{%v} ", rf.me, args.LeaderId, args.LastSnapShotIncludeIndex, rf.me,
    //        rf.lastSnapshotIncludeIndex)
    return;
  }
  // 截断日志，修改commitIndex和lastApplied
  // 截断日志包括：日志长了，截断一部分，日志短了，全部清空，其实两个是一种情况
  // 但是由于现在getSlicesIndexFromLogIndex的实现，不能传入不存在logIndex，否则会panic
  auto lastLogIndex = getLastLogIndex();

  if (lastLogIndex > args->lastsnapshotincludeindex()) {
    m_logs.erase(m_logs.begin(), m_logs.begin() + getSlicesIndexFromLogIndex(args->lastsnapshotincludeindex()) + 1);
  } else {
    m_logs.clear();
  }
  m_commitIndex = std::max(m_commitIndex, args->lastsnapshotincludeindex());
  m_lastApplied = std::max(m_lastApplied, args->lastsnapshotincludeindex());
  m_lastSnapshotIncludeIndex = args->lastsnapshotincludeindex();
  m_lastSnapshotIncludeTerm = args->lastsnapshotincludeterm();
  // 优化2：快照安装后 commitIndex 推进，通知 applierTicker
  m_applyCv.notify_one();

  reply->set_term(m_currentTerm);
  ApplyMsg msg;
  msg.SnapshotValid = true;
  msg.Snapshot = args->data();
  msg.SnapshotTerm = args->lastsnapshotincludeterm();
  msg.SnapshotIndex = args->lastsnapshotincludeindex();

  // 原代码用 &msg 捕获局部变量，函数返回后 msg 已析构，是悬空引用 bug；改为值捕获
  m_threadPool.submit([this, msg]() mutable { this->pushMsgToKvServer(msg); });

  m_persister->Save(persistData(), args->data());
}

void Raft::pushMsgToKvServer(ApplyMsg& msg) { applyChan->Push(msg); }

void Raft::leaderHearBeatTicker() {
  // 优化1：用条件变量替代固定 usleep。
  // Start() 写入新日志后会 notify，使 ticker 立即触发复制，而非等满 25ms。
  // 若无新日志则 wait_for 超时后发送纯心跳，维持 Leader 权威。
  while (true) {
    // 非 Leader 时低频轮询，避免空转
    // 优化A：从 HeartBeatTimeout(25ms) 提高到 50ms，减少 Follower/Candidate 无效唤醒。
    // doElection() 胜选后立即广播心跳，leaderHearBeatTicker 最多延迟 50ms 接管后续心跳，仍远小于选举超时下限 300ms。
    while (m_status != Leader) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    {
      std::unique_lock<std::mutex> lk(m_replicateMtx);
      // 等待新日志通知，或 25ms 超时（周期性心跳）
      m_replicateCv.wait_for(lk, std::chrono::milliseconds(HeartBeatTimeout),
                             [this] { return m_hasNewEntry.load(); });
      m_hasNewEntry.store(false);
    }

    if (m_status == Leader) {
      doHeartBeat();
    }
  }
}

void Raft::leaderSendSnapShot(int server) {
  m_mtx.lock();
  raftRpcProctoc::InstallSnapshotRequest args;
  args.set_leaderid(m_me);
  args.set_term(m_currentTerm);
  args.set_lastsnapshotincludeindex(m_lastSnapshotIncludeIndex);
  args.set_lastsnapshotincludeterm(m_lastSnapshotIncludeTerm);
  args.set_data(m_persister->ReadSnapshot());

  raftRpcProctoc::InstallSnapshotResponse reply;
  m_mtx.unlock();
  bool ok = m_peers[server]->InstallSnapshot(&args, &reply);
  m_mtx.lock();
  DEFER { m_mtx.unlock(); };
  if (!ok) {
    return;
  }
  if (m_status != Leader || m_currentTerm != args.term()) {
    return;  // 中间释放过锁，可能状态已经改变了
  }
  //	无论什么时候都要判断term
  if (reply.term() > m_currentTerm) {
    // 三变
    m_currentTerm = reply.term();
    m_votedFor = -1;
    m_status = Follower;
    persist();
    m_lastResetElectionTime = now();
    return;
  }
  m_matchIndex[server] = args.lastsnapshotincludeindex();
  m_nextIndex[server] = m_matchIndex[server] + 1;
}

void Raft::leaderUpdateCommitIndex() {
  m_commitIndex = m_lastSnapshotIncludeIndex;
  // for index := rf.commitIndex+1;index < len(rf.log);index++ {
  // for index := rf.getLastIndex();index>=rf.commitIndex+1;index--{
  for (int index = getLastLogIndex(); index >= m_lastSnapshotIncludeIndex + 1; index--) {
    int sum = 0;
    for (int i = 0; i < m_peers.size(); i++) {
      if (i == m_me) {
        sum += 1;
        continue;
      }
      if (m_matchIndex[i] >= index) {
        sum += 1;
      }
    }

    //        !!!只有当前term有新提交的，才会更新commitIndex！！！！
    // log.Printf("lastSSP:%d, index: %d, commitIndex: %d, lastIndex: %d",rf.lastSSPointIndex, index, rf.commitIndex,
    // rf.getLastIndex())
    if (sum >= m_peers.size() / 2 + 1 && getLogTermFromLogIndex(index) == m_currentTerm) {
      m_commitIndex = index;
      break;
    }
  }
  //    DPrintf("[func-leaderUpdateCommitIndex()-rf{%v}] Leader %d(term%d) commitIndex
  //    %d",rf.me,rf.me,rf.currentTerm,rf.commitIndex)
}

// 进来前要保证logIndex是存在的，即≥rf.lastSnapshotIncludeIndex	，而且小于等于rf.getLastLogIndex()
bool Raft::matchLog(int logIndex, int logTerm) {
  if (logIndex < m_lastSnapshotIncludeIndex || logIndex > getLastLogIndex()) {
    DPrintf("[matchLog-rf{%d}] out of range: logIndex{%d} lastSnapshot{%d} lastLog{%d}, return false", m_me, logIndex,
            m_lastSnapshotIncludeIndex, getLastLogIndex());
    return false;
  }
  return logTerm == getLogTermFromLogIndex(logIndex);
  // if logIndex == rf.lastSnapshotIncludeIndex {
  // 	return logTerm == rf.lastSnapshotIncludeTerm
  // } else {
  // 	return logTerm == rf.logs[rf.getSlicesIndexFromLogIndex(logIndex)].LogTerm
  // }
}

void Raft::persist() {
  // Your code here (2C).
  auto data = persistData();
  m_persister->SaveRaftState(data);
  // fmt.Printf("RaftNode[%d] persist starts, currentTerm[%d] voteFor[%d] log[%v]\n", rf.me, rf.currentTerm,
  // rf.votedFor, rf.logs) fmt.Printf("%v\n", string(data))
}

void Raft::RequestVote(const raftRpcProctoc::RequestVoteArgs* args, raftRpcProctoc::RequestVoteReply* reply) {
  std::lock_guard<std::mutex> lg(m_mtx);

  DEFER {
    persist();
  };
  // 对args的term的三种情况分别进行处理，大于小于等于自己的term都是不同的处理
  //  reason: 出现网络分区，该竞选者已经OutOfDate(过时）
  if (args->term() < m_currentTerm) {
    reply->set_term(m_currentTerm);
    reply->set_votestate(Expire);
    reply->set_votegranted(false);
    return;
  }
  // fig2:右下角，如果任何时候rpc请求或者响应的term大于自己的term，更新term，并变成follower
  if (args->term() > m_currentTerm) {
    m_status = Follower;
    m_currentTerm = args->term();
    m_votedFor = -1;

    //	重置定时器：收到leader的ae，开始选举，透出票
    // 这时候更新了term之后，votedFor也要置为-1
  }
  if (args->term() != m_currentTerm) {
    DPrintf("[func-RequestVote-rf{%d}] args->term()(%d) != m_currentTerm(%d), ignoring", m_me, args->term(),
            m_currentTerm);
    return;
  }
  //	现在节点任期都是相同的(任期小的也已经更新到新的args的term了)，还需要检查log的term和index是不是匹配的了

  int lastLogTerm = getLastLogTerm();
  // 只有没投票，且candidate的日志的新的程度 ≥ 接受者的日志新的程度 才会授票
  if (!UpToDate(args->lastlogindex(), args->lastlogterm())) {
    // args.LastLogTerm < lastLogTerm || (args.LastLogTerm == lastLogTerm && args.LastLogIndex < lastLogIndex) {
    // 日志太旧了

    reply->set_term(m_currentTerm);
    reply->set_votestate(Voted);
    reply->set_votegranted(false);

    return;
  }
  // todo ： 啥时候会出现rf.votedFor == args.CandidateId ，就算candidate选举超时再选举，其term也是不一样的呀
  //     当因为网络质量不好导致的请求丢失重发就有可能！！！！
  if (m_votedFor != -1 && m_votedFor != args->candidateid()) {
    reply->set_term(m_currentTerm);
    reply->set_votestate(Voted);
    reply->set_votegranted(false);

    return;
  } else {
    m_votedFor = args->candidateid();
    m_lastResetElectionTime = now();  // 认为必须要在投出票的时候才重置定时器，
    reply->set_term(m_currentTerm);
    reply->set_votestate(Normal);
    reply->set_votegranted(true);

    return;
  }
}

bool Raft::UpToDate(int index, int term) {
  // lastEntry := rf.log[len(rf.log)-1]

  int lastIndex = -1;
  int lastTerm = -1;
  getLastLogIndexAndTerm(&lastIndex, &lastTerm);
  return term > lastTerm || (term == lastTerm && index >= lastIndex);
}

void Raft::getLastLogIndexAndTerm(int* lastLogIndex, int* lastLogTerm) {
  if (m_logs.empty()) {
    *lastLogIndex = m_lastSnapshotIncludeIndex;
    *lastLogTerm = m_lastSnapshotIncludeTerm;
    return;
  } else {
    *lastLogIndex = m_logs[m_logs.size() - 1].logindex();
    *lastLogTerm = m_logs[m_logs.size() - 1].logterm();
    return;
  }
}
/**
 *
 * @return 最新的log的logindex，即log的逻辑index。区别于log在m_logs中的物理index
 * 可见：getLastLogIndexAndTerm()
 */
int Raft::getLastLogIndex() {
  int lastLogIndex = -1;
  int _ = -1;
  getLastLogIndexAndTerm(&lastLogIndex, &_);
  return lastLogIndex;
}

int Raft::getLastLogTerm() {
  int _ = -1;
  int lastLogTerm = -1;
  getLastLogIndexAndTerm(&_, &lastLogTerm);
  return lastLogTerm;
}

/**
 *
 * @param logIndex log的逻辑index。注意区别于m_logs的物理index
 * @return
 */
int Raft::getLogTermFromLogIndex(int logIndex) {
  if (logIndex < m_lastSnapshotIncludeIndex) {
    DPrintf("[func-getLogTermFromLogIndex-rf{%d}] warn: logIndex(%d) < lastSnapshotIncludeIndex(%d), returning snapshot term",
            m_me, logIndex, m_lastSnapshotIncludeIndex);
    return m_lastSnapshotIncludeTerm;
  }

  int lastLogIndex = getLastLogIndex();

  if (logIndex > lastLogIndex) {
    DPrintf("[func-getLogTermFromLogIndex-rf{%d}] warn: logIndex(%d) > lastLogIndex(%d), returning 0", m_me, logIndex,
            lastLogIndex);
    return 0;
  }

  if (logIndex == m_lastSnapshotIncludeIndex) {
    return m_lastSnapshotIncludeTerm;
  } else {
    return m_logs[getSlicesIndexFromLogIndex(logIndex)].logterm();
  }
}

int Raft::GetRaftStateSize() { return m_persister->RaftStateSize(); }

// 找到index对应的真实下标位置！！！
// 限制，输入的logIndex必须保存在当前的logs里面（不包含snapshot）
int Raft::getSlicesIndexFromLogIndex(int logIndex) {
  if (logIndex <= m_lastSnapshotIncludeIndex) {
    DPrintf("[func-getSlicesIndexFromLogIndex-rf{%d}] warn: logIndex(%d) <= lastSnapshotIncludeIndex(%d), returning 0",
            m_me, logIndex, m_lastSnapshotIncludeIndex);
    return 0;
  }
  int lastLogIndex = getLastLogIndex();
  if (logIndex > lastLogIndex) {
    DPrintf("[func-getSlicesIndexFromLogIndex-rf{%d}] warn: logIndex(%d) > lastLogIndex(%d), clamping", m_me, logIndex,
            lastLogIndex);
    logIndex = lastLogIndex;
  }
  int SliceIndex = logIndex - m_lastSnapshotIncludeIndex - 1;
  return SliceIndex;
}

bool Raft::sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                           std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum) {
  auto start = now();
  DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 发送 RequestVote 开始", m_me, server);
  bool ok = m_peers[server]->RequestVote(args.get(), reply.get());
  DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 发送 RequestVote 完毕，耗时:{%d} ms", m_me, server,
          now() - start);

  if (!ok) {
    return ok;  // 不知道为什么不加这个的话如果服务器宕机会出现问题的，通不过2B  todo
  }
  // 这里是发送出去了，但是不能保证他一定到达
  //  对回应进行处理，要记得无论什么时候收到回复就要检查term
  std::lock_guard<std::mutex> lg(m_mtx);
  if (reply->term() > m_currentTerm) {
    m_status = Follower;  // 三变：身份，term，和投票
    m_currentTerm = reply->term();
    m_votedFor = -1;
    persist();
    return true;
  } else if (reply->term() < m_currentTerm) {
    return true;
  }
  if (reply->term() != m_currentTerm) {
    DPrintf("[func-sendRequestVote-rf{%d}] reply->term()(%d) != m_currentTerm(%d), ignoring", m_me, reply->term(),
            m_currentTerm);
    return true;
  }

  // todo：这里没有按博客写
  if (!reply->votegranted()) {
    return true;
  }

  *votedNum = *votedNum + 1;
  if (*votedNum >= m_peers.size() / 2 + 1) {
    // 变成leader
    *votedNum = 0;
    if (m_status == Leader) {
      // 已经是 leader，说明本轮选票已经通过其他回复确认过了，忽略这次重复触发，避免 exit 使节点崩溃
      DPrintf("[func-sendRequestVote-rf{%d}]  term:{%d} 已是leader，忽略重复选举成功", m_me, m_currentTerm);
      return true;
    }
    //	第一次变成leader，初始化状态和nextIndex、matchIndex
    m_status = Leader;
    DPrintf("[func-sendRequestVote rf{%d}] elect success  ,current term:{%d} ,lastLogIndex:{%d}\n", m_me, m_currentTerm,
            getLastLogIndex());

    int lastLogIndex = getLastLogIndex();
    for (int i = 0; i < m_nextIndex.size(); i++) {
      m_nextIndex[i] = lastLogIndex + 1;  // 有效下标从1开始，因此要+1
      m_matchIndex[i] = 0;                // 每换一个领导都是从0开始，见fig2
    }
    m_threadPool.submit([this]() { this->doHeartBeat(); });  // 马上向其他节点宣告自己就是leader

    persist();
  }
  return true;
}

bool Raft::sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                             std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,
                             std::shared_ptr<int> appendNums) {

  DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc開始 ， args->entries_size():{%d}", m_me,
          server, args->entries_size());
  bool ok = m_peers[server]->AppendEntries(args.get(), reply.get());

  if (!ok) {
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc失敗", m_me, server);
    return ok;
  }
  DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc成功", m_me, server);
  if (reply->appstate() == Disconnected) {
    return ok;
  }
  std::lock_guard<std::mutex> lg1(m_mtx);

  // 对reply进行处理
  //  对于rpc通信，无论什么时候都要检查term
  if (reply->term() > m_currentTerm) {
    m_status = Follower;
    m_currentTerm = reply->term();
    m_votedFor = -1;
    return ok;
  } else if (reply->term() < m_currentTerm) {
    DPrintf("[func -sendAppendEntries  rf{%d}]  节点：{%d}的term{%d}<rf{%d}的term{%d}\n", m_me, server, reply->term(),
            m_me, m_currentTerm);
    return ok;
  }

  if (m_status != Leader) {
    // 如果不是leader，那么就不要对返回的情况进行处理了
    return ok;
  }
  // term相等

  if (reply->term() != m_currentTerm) {
    DPrintf("[func-sendAppendEntries-rf{%d}] reply->term()(%d) != m_currentTerm(%d), ignoring", m_me, reply->term(),
            m_currentTerm);
    return ok;
  }
  if (!reply->success()) {
    // 日志不匹配，正常来说就是index要往前-1，既然能到这里，第一个日志（idnex =
    //  1）发送后肯定是匹配的，因此不用考虑变成负数 因为真正的环境不会知道是服务器宕机还是发生网络分区了
    if (reply->updatenextindex() != -100) {
      // todo:待总结，就算term匹配，失败的时候nextIndex也不是照单全收的，因为如果发生rpc延迟，leader的term可能从不符合term要求
      // 变得符合term要求
      // 但是不能直接赋值reply.UpdateNextIndex
      DPrintf("[func -sendAppendEntries  rf{%d}]  返回的日志term相等，但是不匹配，回缩nextIndex[%d]：{%d}\n", m_me,
              server, reply->updatenextindex());
      m_nextIndex[server] = reply->updatenextindex();  // 失败是不更新mathIndex的
    }
    //	怎么越写越感觉rf.nextIndex数组是冗余的呢，看下论文fig2，其实不是冗余的
  } else {
    *appendNums = *appendNums + 1;
    DPrintf("---------------------------tmp------------------------- 节点{%d}返回true,当前*appendNums{%d}", server,
            *appendNums);
    // rf.matchIndex[server] = len(args.Entries) //只要返回一个响应就对其matchIndex应该对其做出反应，
    // 但是这么修改是有问题的，如果对某个消息发送了多遍（心跳时就会再发送），那么一条消息会导致n次上涨
    m_matchIndex[server] = std::max(m_matchIndex[server], args->prevlogindex() + args->entries_size());
    m_nextIndex[server] = m_matchIndex[server] + 1;
    int lastLogIndex = getLastLogIndex();

    if (m_nextIndex[server] > lastLogIndex + 1) {
      DPrintf("warn: rf.nextIndex[%d]{%d} > lastLogIndex{%d}+1, clamping", server, m_nextIndex[server], lastLogIndex);
      m_nextIndex[server] = lastLogIndex + 1;
    }
    if (*appendNums >= 1 + m_peers.size() / 2) {
      // 可以commit了
      // 两种方法保证幂等性，1.赋值为0 	2.上面≥改为==

      *appendNums = 0;
      
      if (args->entries_size() > 0) {
        DPrintf("args->entries(args->entries_size()-1).logterm(){%d}   m_currentTerm{%d}",
                args->entries(args->entries_size() - 1).logterm(), m_currentTerm);
      }
      if (args->entries_size() > 0 && args->entries(args->entries_size() - 1).logterm() == m_currentTerm) {
        DPrintf(
            "---------------------------tmp------------------------- 當前term有log成功提交，更新leader的m_commitIndex "
            "from{%d} to{%d}",
            m_commitIndex, args->prevlogindex() + args->entries_size());

        m_commitIndex = std::max(m_commitIndex, args->prevlogindex() + args->entries_size());
        // 优化2：commitIndex 推进，通知 applierTicker 立即 apply
        m_applyCv.notify_one();
      }
      if (m_commitIndex > lastLogIndex) {
        DPrintf("[func-sendAppendEntries-rf{%d}] warn: m_commitIndex(%d) > lastLogIndex(%d), clamping", m_me,
                m_commitIndex, lastLogIndex);
        m_commitIndex = lastLogIndex;
      }
    }
  }
  return ok;
}

void Raft::AppendEntries(google::protobuf::RpcController* controller,
                         const ::raftRpcProctoc::AppendEntriesArgs* request,
                         ::raftRpcProctoc::AppendEntriesReply* response, ::google::protobuf::Closure* done) {
  AppendEntries1(request, response);
  done->Run();
}

void Raft::InstallSnapshot(google::protobuf::RpcController* controller,
                           const ::raftRpcProctoc::InstallSnapshotRequest* request,
                           ::raftRpcProctoc::InstallSnapshotResponse* response, ::google::protobuf::Closure* done) {
  InstallSnapshot(request, response);

  done->Run();
}

void Raft::RequestVote(google::protobuf::RpcController* controller, const ::raftRpcProctoc::RequestVoteArgs* request,
                       ::raftRpcProctoc::RequestVoteReply* response, ::google::protobuf::Closure* done) {
  RequestVote(request, response);
  done->Run();
}

void Raft::Start(Op command, int* newLogIndex, int* newLogTerm, bool* isLeader) {
  std::lock_guard<std::mutex> lg1(m_mtx);
  //    m_mtx.lock();
  //    Defer ec1([this]()->void {
  //       m_mtx.unlock();
  //    });
  if (m_status != Leader) {
    DPrintf("[func-Start-rf{%d}]  is not leader");
    *newLogIndex = -1;
    *newLogTerm = -1;
    *isLeader = false;
    return;
  }

  raftRpcProctoc::LogEntry newLogEntry;
  newLogEntry.set_command(command.asString());
  newLogEntry.set_logterm(m_currentTerm);
  newLogEntry.set_logindex(getNewCommandIndex());
  m_logs.emplace_back(newLogEntry);

  int lastLogIndex = getLastLogIndex();

  DPrintf("[func-Start-rf{%d}]  lastLogIndex:%d,command:%s\n", m_me, lastLogIndex, &command);
  persist();
  *newLogIndex = newLogEntry.logindex();
  *newLogTerm = newLogEntry.logterm();
  *isLeader = true;

  // 优化1：通知 leaderHearBeatTicker 有新日志，立即触发复制，无需等待下个 25ms 窗口
  m_hasNewEntry.store(true);
  m_replicateCv.notify_one();
}


void Raft::init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh) {
  m_peers = peers;
  m_persister = persister;
  m_me = me;
  // Your initialization code here (2A, 2B, 2C).
  m_mtx.lock();

  // applier
  this->applyChan = applyCh;
  //    rf.ApplyMsgQueue = make(chan ApplyMsg)
  m_currentTerm = 0;
  m_status = Follower;
  m_commitIndex = 0;
  m_lastApplied = 0;
  m_logs.clear();
  for (int i = 0; i < m_peers.size(); i++) {
    m_matchIndex.push_back(0);
    m_nextIndex.push_back(0);
  }
  m_votedFor = -1;

  m_lastSnapshotIncludeIndex = 0;
  m_lastSnapshotIncludeTerm = 0;
  m_lastResetElectionTime = now();
  m_lastResetHearBeatTime = now();

  // initialize from state persisted before a crash
  readPersist(m_persister->ReadRaftState());
  if (m_lastSnapshotIncludeIndex > 0) {
    m_commitIndex = m_lastSnapshotIncludeIndex;
    m_lastApplied = m_lastSnapshotIncludeIndex;
  }

  DPrintf("[Init&ReInit] Sever %d, term %d, lastSnapshotIncludeIndex {%d} , lastSnapshotIncludeTerm {%d}", m_me,
          m_currentTerm, m_lastSnapshotIncludeIndex, m_lastSnapshotIncludeTerm);

  m_mtx.unlock();

  // start ticker fiber to start elections
  // 启动三个循环定时器

  std::thread t(&Raft::leaderHearBeatTicker, this);
  t.detach();

  std::thread t2(&Raft::electionTimeOutTicker, this);
  t2.detach();

  std::thread t3(&Raft::applierTicker, this);
  t3.detach();
}

std::string Raft::persistData() {
  BoostPersistRaftNode boostPersistRaftNode;
  boostPersistRaftNode.m_currentTerm = m_currentTerm;
  boostPersistRaftNode.m_votedFor = m_votedFor;
  boostPersistRaftNode.m_lastSnapshotIncludeIndex = m_lastSnapshotIncludeIndex;
  boostPersistRaftNode.m_lastSnapshotIncludeTerm = m_lastSnapshotIncludeTerm;
  for (auto& item : m_logs) {
    boostPersistRaftNode.m_logs.push_back(item.SerializeAsString());
  }

  std::stringstream ss;
  boost::archive::text_oarchive oa(ss);
  oa << boostPersistRaftNode;
  return ss.str();
}

void Raft::readPersist(std::string data) {
  if (data.empty()) {
    return;
  }
  std::stringstream iss(data);
  boost::archive::text_iarchive ia(iss);
  // read class state from archive
  BoostPersistRaftNode boostPersistRaftNode;
  ia >> boostPersistRaftNode;

  m_currentTerm = boostPersistRaftNode.m_currentTerm;
  m_votedFor = boostPersistRaftNode.m_votedFor;
  m_lastSnapshotIncludeIndex = boostPersistRaftNode.m_lastSnapshotIncludeIndex;
  m_lastSnapshotIncludeTerm = boostPersistRaftNode.m_lastSnapshotIncludeTerm;
  m_logs.clear();
  for (auto& item : boostPersistRaftNode.m_logs) {
    raftRpcProctoc::LogEntry logEntry;
    logEntry.ParseFromString(item);
    m_logs.emplace_back(logEntry);
  }
}

// 优化4：ReadIndex 读优化，KvServer 调用获取当前 commitIndex
int Raft::GetCommitIndex() {
  std::lock_guard<std::mutex> lg(m_mtx);
  return m_commitIndex;
}

void Raft::Snapshot(int index, std::string snapshot) {
  // 将内存状态更新和磁盘持久化分离：
  //   - 内存状态（裁截日志、更新 snapshotIndex/Term）需持锁保护
  //   - 磁盘 Save() 是纯 IO，不依赖运行时状态，移出锁避免阻塞所有其他 Raft RPC
  std::string persistStr;
  {
    std::lock_guard<std::mutex> lg(m_mtx);

    if (m_lastSnapshotIncludeIndex >= index || index > m_commitIndex) {
      DPrintf(
          "[func-Snapshot-rf{%d}] rejects replacing log with snapshotIndex %d as current snapshotIndex %d is larger or "
          "smaller ",
          m_me, index, m_lastSnapshotIncludeIndex);
      return;
    }
    auto lastLogIndex = getLastLogIndex();

    int newLastSnapshotIncludeIndex = index;
    int newLastSnapshotIncludeTerm = m_logs[getSlicesIndexFromLogIndex(index)].logterm();
    std::vector<raftRpcProctoc::LogEntry> trunckedLogs;
    for (int i = index + 1; i <= getLastLogIndex(); i++) {
      trunckedLogs.push_back(m_logs[getSlicesIndexFromLogIndex(i)]);
    }
    m_lastSnapshotIncludeIndex = newLastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = newLastSnapshotIncludeTerm;
    m_logs = trunckedLogs;
    m_commitIndex = std::max(m_commitIndex, index);
    m_lastApplied = std::max(m_lastApplied, index);

    // 持锁期间序列化内存状态（纯 CPU，快），不做 IO
    persistStr = persistData();

    DPrintf("[SnapShot]Server %d snapshot snapshot index {%d}, term {%d}, loglen {%d}", m_me, index,
            m_lastSnapshotIncludeTerm, m_logs.size());
    if (m_logs.size() + m_lastSnapshotIncludeIndex != lastLogIndex) {
      DPrintf("[Snapshot-rf{%d}] warn: logs.size{%d}+lastSnapshotIncludeIndex{%d} != lastLogIndex{%d}", m_me,
              m_logs.size(), m_lastSnapshotIncludeIndex, lastLogIndex);
    }
  }  // ← 锁在此释放，AppendEntries / RequestVote 可以继续

  // 锁外执行磁盘写入，不再阻塞其他 Raft RPC
  m_persister->Save(persistStr, snapshot);
}
