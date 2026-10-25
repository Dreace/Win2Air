#pragma once
#include <filesystem>
#include <mutex>

#include "AppSettings.h"
namespace Win2Air {
class SettingsStore {
  std::filesystem::path root_;
  std::mutex writer_;

 public:
  explicit SettingsStore(std::filesystem::path root);
  SettingsSnapshot Load();
  void Save(SettingsSnapshot const& value);
};
}  // namespace Win2Air
