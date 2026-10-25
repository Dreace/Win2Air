#pragma once
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../Settings/CredentialStore.h"
#include "../Settings/StorageIO.h"
#include "FailureDetail.h"
#include "StreamCoordinator.h"
namespace Win2Air {
// Shared between the StreamCoordinator implementation files; call sites keep
// the unqualified names the single translation unit used before the split.
inline uint64_t SenderIdentity(std::filesystem::path const &root) {
  auto path = root / L"sender-identity.bin";
  uint64_t identity{};
  if (std::filesystem::exists(path)) {
    auto bytes = StorageIO::Read(path, 8);
    if (bytes.size() != 8)
      throw std::runtime_error("Sender identity record is incomplete");
    std::memcpy(&identity, bytes.data(), 8);
  } else {
    auto status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&identity),
                                  8, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0)
      throw std::runtime_error("Generate persistent sender identity failed: " +
                               std::to_string(status));
    if (!identity)
      throw std::runtime_error("Generated sender identity is invalid");
    StorageIO::AtomicWrite(path,
                           {reinterpret_cast<uint8_t const *>(&identity), 8});
  }
  if (!identity) throw std::runtime_error("Stored sender identity is invalid");
  return identity;
}
inline void Checked(sender_result result, sender_error const &error) {
  if (result != SENDER_OK)
    throw SenderFailure(std::string(error.operation) + ": " + error.message +
                            "; code=" + std::to_string(result) + "; platform=" +
                            std::to_string(error.platform_code) +
                            "; peer=" + std::to_string(error.peer_code),
                        error.platform_code, error.peer_code, result);
}
inline SecureBuffer Terminated(SecureBuffer const &input) {
  auto size = input.Bytes().size();
  if (!size || size > 65536 ||
      std::find(input.Bytes().begin(), input.Bytes().end(), 0) !=
          input.Bytes().end())
    throw std::runtime_error(
        "Credential or pairing input has invalid byte shape");
  auto memory = std::make_unique<uint8_t[]>(size + 1);
  std::memcpy(memory.get(), input.Bytes().data(), size);
  memory[size] = 0;
  try {
    SecureBuffer output({memory.get(), size + 1});
    SecureZeroMemory(memory.get(), size + 1);
    return output;
  } catch (...) {
    SecureZeroMemory(memory.get(), size + 1);
    throw;
  }
}
inline bool TimelineAnchor(std::string_view reason) {
  return reason == "silence-standby" || reason == "standby-resume" ||
         reason == "reconnected-standby" || reason == "standby-disabled" ||
         reason == "source-change";
}
}  // namespace Win2Air
