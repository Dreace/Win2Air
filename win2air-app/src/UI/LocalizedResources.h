#pragma once
#include <windows.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "../Diagnostics/Logger.h"
#include "../Settings/Appearance.h"
namespace Win2Air {
inline void ConfigureResources(std::string const& saved) {
  ULONG count{}, size{};
  if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size))
    winrt::throw_last_error();
  std::vector<wchar_t> buffer(size);
  if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(),
                                   &size))
    winrt::throw_last_error();
  std::vector<std::string> languages;
  for (auto text = buffer.data(); *text; text += wcslen(text) + 1)
    languages.push_back(winrt::to_string(text));
  resource_language = ResolveLanguage(saved, languages);
}
class LocalizedResources {
  winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceManager
      manager_;

 public:
  LocalizedResources() = default;
  explicit LocalizedResources(
      winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceManager
          manager)
      : manager_(std::move(manager)) {}
  winrt::hstring TextLanguage() const {
    return winrt::to_hstring(resource_language);
  }
  void ApplyLanguage(
      winrt::Microsoft::UI::Xaml::FrameworkElement const& element) const {
    element.Language(TextLanguage());
  }
  winrt::hstring GetString(winrt::hstring const& key) const {
    auto context = manager_.CreateResourceContext();
    context.QualifierValues().Insert(L"Language",
                                     winrt::to_hstring(resource_language));
    return manager_.MainResourceMap()
        .GetValue(winrt::hstring(L"Resources/") + key, context)
        .ValueAsString();
  }
  void LocalizeSwitch(
      winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch const& control) const {
    control.OnContent(winrt::box_value(GetString(L"SwitchOn")));
    control.OffContent(winrt::box_value(GetString(L"SwitchOff")));
  }
};
struct FailurePrompt {
  std::string shown;
  bool open{};
};
inline std::function<void(std::string const&)> ui_failure_log;
inline void LogUiFailure(std::string const& message) {
  if (message.empty() || !ui_failure_log) return;
  try {
    LogDiagnostic(
        [](LogRecord const& record) { ui_failure_log(record.detail); }, {},
        message);
  } catch (...) {
    OutputDebugStringW(L"Win2Air: diagnostic log write failed\n");
  }
}
inline bool& FailureDialogsAllowed() {
  static bool allowed = true;
  return allowed;
}
inline bool& FailureDialogBusy() {
  static bool busy = false;
  return busy;
}
inline winrt::fire_and_forget ShowBlockingFailure(
    winrt::Microsoft::UI::Xaml::FrameworkElement const&,
    std::string const& message, winrt::hstring const&, FailurePrompt& prompt) {
  // Backend diagnostics belong in the log, never in a generic error dialog.
  if (message != prompt.shown) LogUiFailure(message);
  prompt.shown = message;
  co_return;
}
}  // namespace Win2Air
