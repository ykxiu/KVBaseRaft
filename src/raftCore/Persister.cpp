#include "Persister.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

constexpr uint32_t kBundleMagic = 0x52504654;  // "RPFT"
constexpr uint32_t kBundleVersion = 1;

struct BundleHeader {
  uint32_t magic;
  uint32_t version;
  uint64_t raftStateSize;
  uint64_t snapshotSize;
};

std::string buildBundle(const std::string& raftState, const std::string& snapshot) {
  BundleHeader header{ kBundleMagic, kBundleVersion, raftState.size(), snapshot.size() };
  std::string bundle(sizeof(BundleHeader), '\0');
  std::memcpy(bundle.data(), &header, sizeof(BundleHeader));
  bundle.append(raftState);
  bundle.append(snapshot);
  return bundle;
}

bool parseBundle(const std::string& bundle, std::string* raftState, std::string* snapshot) {
  if (bundle.size() < sizeof(BundleHeader)) {
    return false;
  }

  BundleHeader header{};
  std::memcpy(&header, bundle.data(), sizeof(BundleHeader));
  if (header.magic != kBundleMagic || header.version != kBundleVersion) {
    return false;
  }

  const uint64_t totalSize = sizeof(BundleHeader) + header.raftStateSize + header.snapshotSize;
  if (bundle.size() != totalSize) {
    return false;
  }

  size_t offset = sizeof(BundleHeader);
  raftState->assign(bundle.data() + offset, header.raftStateSize);
  offset += header.raftStateSize;
  snapshot->assign(bundle.data() + offset, header.snapshotSize);
  return true;
}

void writeAllOrThrow(int fd, const char* data, size_t size) {
  size_t written = 0;
  while (written < size) {
    ssize_t rc = ::write(fd, data + written, size - written);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
    }
    written += static_cast<size_t>(rc);
  }
}

void fsyncDirectory(const std::filesystem::path& path) {
  const std::filesystem::path parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  const int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
  if (dirFd < 0) {
    throw std::runtime_error(std::string("open directory failed: ") + std::strerror(errno));
  }
  if (::fsync(dirFd) != 0) {
    const std::string err = std::strerror(errno);
    ::close(dirFd);
    throw std::runtime_error(std::string("fsync directory failed: ") + err);
  }
  ::close(dirFd);
}

}  // namespace

void Persister::Save(const std::string raftstate, const std::string snapshot) {
  std::lock_guard<std::mutex> lg(m_mtx);
  m_raftState = raftstate;
  m_snapshot = snapshot;
  m_raftStateSize = static_cast<long long>(m_raftState.size());
  writeBundleLocked();
}

std::string Persister::ReadSnapshot() {
  std::lock_guard<std::mutex> lg(m_mtx);
  return m_snapshot;
}

void Persister::SaveRaftState(const std::string& data) {
  std::lock_guard<std::mutex> lg(m_mtx);
  m_raftState = data;
  m_raftStateSize = static_cast<long long>(m_raftState.size());
  writeBundleLocked();
}

long long Persister::RaftStateSize() {
  std::lock_guard<std::mutex> lg(m_mtx);
  return m_raftStateSize;
}

std::string Persister::ReadRaftState() {
  std::lock_guard<std::mutex> lg(m_mtx);
  return m_raftState;
}

Persister::Persister(const int me)
    : m_bundleFileName("persisterPersist" + std::to_string(me) + ".bin"),
      m_raftStateFileName("raftstatePersist" + std::to_string(me) + ".txt"),
      m_snapshotFileName("snapshotPersist" + std::to_string(me) + ".txt"),
      m_raftStateSize(0) {
  loadBundleLocked();
}

Persister::~Persister() = default;

void Persister::writeBundleLocked() {
  writeFileAtomically(m_bundleFileName, buildBundle(m_raftState, m_snapshot));
}

void Persister::loadBundleLocked() {
  m_raftState.clear();
  m_snapshot.clear();
  m_raftStateSize = 0;

  const std::string bundle = readWholeFile(m_bundleFileName);
  if (!bundle.empty()) {
    std::string raftState;
    std::string snapshot;
    if (!parseBundle(bundle, &raftState, &snapshot)) {
      throw std::runtime_error("persister bundle is corrupted");
    }
    m_raftState = std::move(raftState);
    m_snapshot = std::move(snapshot);
    m_raftStateSize = static_cast<long long>(m_raftState.size());
    return;
  }

  // 兼容旧版本：若 bundle 不存在，则尽力从旧的分离文件中恢复。
  m_raftState = readWholeFile(m_raftStateFileName);
  m_snapshot = readWholeFile(m_snapshotFileName);
  m_raftStateSize = static_cast<long long>(m_raftState.size());
  if (!m_raftState.empty() || !m_snapshot.empty()) {
    writeBundleLocked();
  }
}

std::string Persister::readWholeFile(const std::string& fileName) {
  std::ifstream ifs(fileName, std::ios::in | std::ios::binary);
  if (!ifs.good()) {
    return "";
  }

  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

void Persister::writeFileAtomically(const std::string& fileName, const std::string& data) {
  const std::filesystem::path path(fileName);
  const std::filesystem::path tmpPath = path.string() + ".tmp";

  const int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    throw std::runtime_error(std::string("open temp file failed: ") + std::strerror(errno));
  }

  try {
    writeAllOrThrow(fd, data.data(), data.size());
    if (::fsync(fd) != 0) {
      throw std::runtime_error(std::string("fsync temp file failed: ") + std::strerror(errno));
    }
    if (::close(fd) != 0) {
      throw std::runtime_error(std::string("close temp file failed: ") + std::strerror(errno));
    }
    if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
      throw std::runtime_error(std::string("rename temp file failed: ") + std::strerror(errno));
    }
    fsyncDirectory(path);
  } catch (...) {
    ::close(fd);
    ::unlink(tmpPath.c_str());
    throw;
  }
}
