//
// Created by swx on 23-5-30.
//

#ifndef SKIP_LIST_ON_RAFT_PERSISTER_H
#define SKIP_LIST_ON_RAFT_PERSISTER_H
#include <cstdint>
#include <mutex>
#include <string>
class Persister {
 private:
  std::mutex m_mtx;
  std::string m_raftState;
  std::string m_snapshot;
  /**
   * 持久化 bundle 文件名（权威数据源）
   */
  const std::string m_bundleFileName;
  /**
   * 兼容旧实现保留的 raftState 文件名
   */
  const std::string m_raftStateFileName;
  /**
   * 兼容旧实现保留的 snapshot 文件名
   */
  const std::string m_snapshotFileName;
  /**
   * 保存raftStateSize的大小
   * 避免每次都读取文件来获取具体的大小
   */
  long long m_raftStateSize;

 public:
  void Save(std::string raftstate, std::string snapshot);
  std::string ReadSnapshot();
  void SaveRaftState(const std::string& data);
  long long RaftStateSize();
  std::string ReadRaftState();
  explicit Persister(int me);
  ~Persister();

 private:
  void writeBundleLocked();
  void loadBundleLocked();
  static std::string readWholeFile(const std::string& fileName);
  static void writeFileAtomically(const std::string& fileName, const std::string& data);
};

#endif  // SKIP_LIST_ON_RAFT_PERSISTER_H
