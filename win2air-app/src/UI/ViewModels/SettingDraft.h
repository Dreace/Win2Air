#pragma once
#include <cstdint>
#include <string>
#include <utility>
namespace Win2Air {
template <class T>
class SettingDraft {
  T value_{};
  bool initialized_{};
  uint64_t version_{};
  bool dirty_{};

 public:
  void Update(T const& saved) {
    if (!initialized_ || !dirty_) value_ = saved;
    initialized_ = true;
  }
  void Edit(T value) {
    value_ = std::move(value);
    ++version_;
    dirty_ = true;
  }
  uint64_t Version() const { return version_; }
  void Acknowledge(uint64_t version, T const& value) {
    if (version == version_ && value_ == value) dirty_ = false;
  }
  void Reject(uint64_t version, T const& saved) {
    if (version == version_) {
      value_ = saved;
      dirty_ = false;
    }
  }
  T const& Value() const { return value_; }
  bool Dirty() const { return dirty_; }
};
struct SettingsDraft {
  SettingDraft<std::string> theme, language, source;
};
struct StreamingDraft {
  SettingDraft<std::string> mode;
  SettingDraft<double> custom, offset;
  std::string receiver;
};
}  // namespace Win2Air
