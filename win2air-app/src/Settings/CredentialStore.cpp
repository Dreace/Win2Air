#include "CredentialStore.h"

#include <dpapi.h>

#include <cstring>

#include "AppSettings.h"
#include "StorageIO.h"
namespace Win2Air {
SecureBuffer& SecureBuffer::operator=(SecureBuffer&& other) noexcept {
  if (this != &other) {
    Clear();
    bytes_ = std::move(other.bytes_);
  }
  return *this;
}
namespace {
std::filesystem::path CredentialPath(std::filesystem::path const& root,
                                     std::string receiver) {
  if (!ValidReceiverIdentity(receiver))
    throw std::runtime_error("Verified canonical receiver identity required");
  for (auto& c : receiver) {
    if (c == ':')
      c = '-';
    else if (c >= 'a' && c <= 'f')
      c = char(c - 'a' + 'A');
  }
  return root / (receiver + ".bin");
}
struct Blob {
  DATA_BLOB value{};
  ~Blob() {
    if (value.pbData) {
      SecureZeroMemory(value.pbData, value.cbData);
      LocalFree(value.pbData);
    }
  }
};
std::string Binding(std::string const& identity, uint64_t sender) {
  if (!ValidReceiverIdentity(identity) || !sender)
    throw std::runtime_error("Verified receiver and sender binding required");
  auto normalized = identity;
  for (auto& c : normalized)
    if (c >= 'a' && c <= 'f') c = char(c - 'a' + 'A');
  return "Win2Air.credentials|schema=1|" + normalized + "|" +
         std::to_string(sender);
}
}  // namespace
CredentialStore::CredentialStore(std::filesystem::path root)
    : root_(std::move(root)) {}
void CredentialStore::Save(std::string const& identity, uint64_t sender,
                           std::span<uint8_t const> material) {
  std::scoped_lock lock(writer_, StorageIO::WriterGate);
  auto path = CredentialPath(root_, identity);
  auto binding = Binding(identity, sender);
  if (material.empty() || material.size() > 65536)
    throw std::runtime_error("Credential material exceeds bound or is empty");
  SecureBuffer copy(material);
  DATA_BLOB input{static_cast<DWORD>(copy.Bytes().size()),
                  const_cast<BYTE*>(copy.Bytes().data())};
  DATA_BLOB entropy{static_cast<DWORD>(binding.size()),
                    reinterpret_cast<BYTE*>(binding.data())};
  Blob protected_blob;
  StorageIO::Check(
      CryptProtectData(&input, nullptr, &entropy, nullptr, nullptr,
                       CRYPTPROTECT_UI_FORBIDDEN, &protected_blob.value),
      "Protect credential for current user");
  std::vector<uint8_t> file(16 + protected_blob.value.cbData);
  uint32_t magic = 0x52433257, version = 1;
  FILETIME updated{};
  GetSystemTimeAsFileTime(&updated);
  std::memcpy(file.data(), &magic, 4);
  std::memcpy(file.data() + 4, &version, 4);
  std::memcpy(file.data() + 8, &updated, 8);
  std::memcpy(file.data() + 16, protected_blob.value.pbData,
              protected_blob.value.cbData);
  StorageIO::PrivateDirectory(root_);
  StorageIO::AtomicWrite(path, file);
  auto marker = path;
  marker += L".revoked";
  if (!DeleteFileW(marker.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
    StorageIO::Check(FALSE, "Clear credential revocation marker");
  revoked_.erase(path);
}
SecureBuffer CredentialStore::Load(std::string const& identity,
                                   uint64_t sender) {
  std::scoped_lock lock(writer_, StorageIO::WriterGate);
  auto path = CredentialPath(root_, identity);
  auto binding = Binding(identity, sender);
  auto marker = path;
  marker += L".revoked";
  if (revoked_.contains(path) || std::filesystem::exists(marker))
    throw std::runtime_error("Credential reuse blocked after revocation");
  auto file = StorageIO::Read(path, 128 * 1024);
  uint32_t magic{}, version{};
  if (file.size() <= 16)
    throw std::runtime_error("Credential envelope truncated");
  std::memcpy(&magic, file.data(), 4);
  std::memcpy(&version, file.data() + 4, 4);
  if (magic != 0x52433257 || version != 1)
    throw std::runtime_error("Credential envelope version unsupported");
  DATA_BLOB input{static_cast<DWORD>(file.size() - 16), file.data() + 16};
  DATA_BLOB entropy{static_cast<DWORD>(binding.size()),
                    reinterpret_cast<BYTE*>(binding.data())};
  Blob decrypted;
  StorageIO::Check(
      CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &decrypted.value),
      "Unprotect bound current-user credential");
  if (!decrypted.value.cbData || decrypted.value.cbData > 65536)
    throw std::runtime_error("Decrypted credential size invalid");
  return SecureBuffer({decrypted.value.pbData, decrypted.value.cbData});
}
void CredentialStore::Delete(std::string const& identity) {
  std::scoped_lock lock(writer_, StorageIO::WriterGate);
  auto path = CredentialPath(root_, identity);
  revoked_.insert(path);
  auto marker = path;
  marker += L".revoked";
  StorageIO::PrivateDirectory(root_);
  StorageIO::AtomicText(marker, "authorization revoked");
  if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
    StorageIO::Check(FALSE, "Delete credential");
}
}  // namespace Win2Air
