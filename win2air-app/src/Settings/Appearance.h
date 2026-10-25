#pragma once
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
namespace Win2Air {
inline std::string resource_language{"en-US"};
enum class ThemePreference { system, light, dark };
inline ThemePreference ResolveTheme(std::string const& value) {
  if (value == "system") return ThemePreference::system;
  if (value == "light") return ThemePreference::light;
  if (value == "dark") return ThemePreference::dark;
  throw std::invalid_argument("Unsupported application theme");
}
inline std::string ResolveLanguage(std::string const& saved,
                                   std::span<std::string const> system) {
  if (saved == "zh-CN" || saved == "en-US") return saved;
  if (saved != "system")
    throw std::invalid_argument("Unsupported application language");
  for (auto const& tag : system) {
    if (_stricmp(tag.c_str(), "zh-CN") == 0 ||
        _stricmp(tag.c_str(), "zh-Hans") == 0 ||
        _strnicmp(tag.c_str(), "zh-Hans-", 8) == 0)
      return "zh-CN";
    if (_stricmp(tag.c_str(), "en") == 0 ||
        _strnicmp(tag.c_str(), "en-", 3) == 0)
      return "en-US";
  }
  return "en-US";
}
}  // namespace Win2Air
