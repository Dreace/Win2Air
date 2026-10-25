#pragma once
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>
namespace Win2Air {
class SecureBuffer {
  std::vector<uint8_t> bytes_;

 public:
  SecureBuffer() = default;
  explicit SecureBuffer(std::span<uint8_t const> bytes)
      : bytes_(bytes.begin(), bytes.end()) {}
  explicit SecureBuffer(std::string const& value)
      : bytes_(value.begin(), value.end()) {}
  SecureBuffer(SecureBuffer const&) = delete;
  SecureBuffer& operator=(SecureBuffer const&) = delete;
  SecureBuffer(SecureBuffer&& other) noexcept
      : bytes_(std::move(other.bytes_)) {}
  SecureBuffer& operator=(SecureBuffer&& other) noexcept;
  ~SecureBuffer() { Clear(); }
  void Clear() noexcept {
    if (!bytes_.empty()) SecureZeroMemory(bytes_.data(), bytes_.size());
    bytes_.clear();
  }
  std::span<uint8_t const> Bytes() const noexcept { return bytes_; }
};
class CredentialStore {
  std::filesystem::path root_;
  std::mutex writer_;
  std::set<std::filesystem::path> revoked_;

 public:
  explicit CredentialStore(std::filesystem::path root);
  void Save(std::string const& verified_receiver, uint64_t sender_id,
            std::span<uint8_t const> material);
  SecureBuffer Load(std::string const& verified_receiver, uint64_t sender_id);
  void Delete(std::string const& verified_receiver);
};
}  // namespace Win2Air
