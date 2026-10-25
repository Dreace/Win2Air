#pragma once
#include <windows.h>

#include <sddl.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>
namespace Win2Air::StorageIO {
inline std::mutex WriterGate;
inline void Check(BOOL ok, char const* operation) {
  if (!ok)
    throw std::runtime_error(std::string(operation) +
                             " failed: " + std::to_string(GetLastError()));
}
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  explicit Handle(HANDLE h) : value(h) {
    if (h == INVALID_HANDLE_VALUE) Check(FALSE, "Open storage file");
  }
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
  Handle(Handle const&) = delete;
};
inline void PrivateDirectory(std::filesystem::path const& root) {
  std::filesystem::create_directories(root);
  for (auto part = root; !part.empty() && part != part.root_path();
       part = part.parent_path()) {
    auto attributes = GetFileAttributesW(part.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      throw std::runtime_error(
          "Storage directory must be a real local directory");
  }
  HANDLE token{};
  Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token),
        "Read current user token");
  DWORD size{};
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<uint8_t> bytes(size);
  BOOL result =
      GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
  CloseHandle(token);
  Check(result, "Read current user identity");
  LPWSTR sid{};
  Check(ConvertSidToStringSidW(
            reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid),
        "Format user identity");
  std::wstring sddl = L"D:P(A;OICI;FA;;;" + std::wstring(sid) + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor{};
  Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr),
        "Create private storage ACL");
  result = SetFileSecurityW(
      root.c_str(),
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      descriptor);
  LocalFree(descriptor);
  Check(result, "Apply private storage ACL");
}
inline std::vector<uint8_t> Read(std::filesystem::path const& path,
                                 size_t limit = 1024 * 1024) {
  if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) {
    if (std::filesystem::exists(path))
      throw std::runtime_error("Storage file must not be a reparse point");
  }
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  LARGE_INTEGER size{};
  Check(GetFileSizeEx(file.value, &size), "Read storage size");
  if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > limit)
    throw std::runtime_error("Storage file exceeds size bound");
  std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
  DWORD read{};
  Check(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()),
                 &read, nullptr),
        "Read storage data");
  if (read != bytes.size())
    throw std::runtime_error("Storage file read was incomplete");
  return bytes;
}
inline void AtomicWrite(std::filesystem::path const& path,
                        std::span<uint8_t const> bytes) {
  if (std::filesystem::exists(path) &&
      (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("Storage target must not be a reparse point");
  static std::atomic<uint64_t> next{};
  auto temporary = path;
  temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
               std::to_wstring(++next) + L".pending";
  bool created = false;
  try {
    {
      Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
      created = true;
      DWORD written{};
      Check(WriteFile(file.value, bytes.data(),
                      static_cast<DWORD>(bytes.size()), &written, nullptr),
            "Write storage data");
      if (written != bytes.size())
        throw std::runtime_error("Storage write was incomplete");
      Check(FlushFileBuffers(file.value), "Flush storage data");
    }
    Check(MoveFileExW(temporary.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
          "Commit storage file");
  } catch (...) {
    if (created) DeleteFileW(temporary.c_str());
    throw;
  }
}
inline void AtomicText(std::filesystem::path const& path,
                       std::string const& text) {
  AtomicWrite(path,
              {reinterpret_cast<uint8_t const*>(text.data()), text.size()});
}
}  // namespace Win2Air::StorageIO
