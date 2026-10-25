#include "Activation.h"

#include <windows.h>

#include <shellapi.h>

#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include "../Settings/AppSettings.h"
namespace Win2Air {
namespace {
std::vector<std::wstring> Arguments(std::wstring const& text, bool executable) {
  if (text.size() > 4096 || text.find(L'\0') != std::wstring::npos ||
      text.find_first_of(L"\r\n") != std::wstring::npos)
    throw std::runtime_error(
        "Activation arguments are oversized or contain invalid characters");
  auto line = executable ? text : L"Win2Air.exe " + text;
  int count{};
  auto parsed = CommandLineToArgvW(line.c_str(), &count);
  if (!parsed)
    throw std::runtime_error("Parse activation command line failed: " +
                             std::to_string(GetLastError()));
  std::unique_ptr<void, decltype(&LocalFree)> owner(parsed, LocalFree);
  std::vector<std::wstring> result;
  for (int index = 0; index < count; ++index)
    result.emplace_back(parsed[index]);
  return result;
}
AppPage Page(std::wstring const& value) {
  if (value == L"playback") return AppPage::playback;
  if (value == L"devices") return AppPage::devices;
  if (value == L"statistics") return AppPage::statistics;
  if (value == L"settings") return AppPage::settings;
  throw std::runtime_error(
      "Activation page must be playback, devices, statistics or settings");
}
}  // namespace
ActivationRequest ParseProtocol(std::wstring const& uri) {
  constexpr wchar_t prefix[] = L"win2air://open/";
  if (!uri.starts_with(prefix))
    throw std::runtime_error(
        "Protocol activation requires win2air://open and a supported page");
  ActivationRequest request;
  request.page = Page(uri.substr(std::size(prefix) - 1));
  request.explicit_page = true;
  return request;
}
ActivationRequest ParseActivation(std::wstring const& text) {
  auto tokens = Arguments(text, false);
  if (tokens.size() == 2 && tokens[1].starts_with(L"win2air:"))
    return ParseProtocol(tokens[1]);
  ActivationRequest request;
  std::set<std::wstring> seen;
  for (size_t index = 1; index < tokens.size(); ++index) {
    auto option = tokens[index];
    if (!seen.insert(option).second)
      throw std::runtime_error("Activation option is duplicated");
    if (option == L"--open" || option == L"--connect") {
      if (++index == tokens.size())
        throw std::runtime_error("Activation option is missing its value");
      if (option == L"--open") {
        request.page = Page(tokens[index]);
        request.explicit_page = true;
      } else {
        auto identity = tokens[index];
        if (identity.size() != 17 ||
            identity.find_first_not_of(L"0123456789abcdefABCDEF:") !=
                std::wstring::npos)
          throw std::runtime_error("Activation receiver identity is invalid");
        std::string receiver;
        for (auto character : identity)
          receiver.push_back(static_cast<char>(character));
        request.receiver_id = std::move(receiver);
        if (!ValidReceiverIdentity(*request.receiver_id))
          throw std::runtime_error("Activation receiver identity is invalid");
      }
    } else if (option == L"--disconnect")
      request.disconnect = true;
    else if (option == L"--minimized")
      request.minimized = true;
    else
      throw std::runtime_error("Unsupported activation option");
  }
  if (request.receiver_id && request.disconnect)
    throw std::runtime_error("Connect and disconnect activations conflict");
  return request;
}
std::wstring LaunchArguments(std::wstring const& arguments,
                             std::wstring const& executable) {
  if (arguments.find_first_not_of(L" \t") == std::wstring::npos) return L"";
  auto tokens = Arguments(arguments, true);
  auto supplied = tokens.empty()
                      ? std::wstring{}
                      : std::filesystem::path(tokens[0]).filename().wstring();
  auto expected = std::filesystem::path(executable).filename().wstring();
  if (!supplied.empty() &&
      CompareStringOrdinal(supplied.c_str(), -1, expected.c_str(), -1, TRUE) ==
          CSTR_EQUAL) {
    auto first = arguments.find_first_not_of(L" \t");
    if (arguments[first] == L'"') {
      auto end = arguments.find(L'"', first + 1);
      if (end == std::wstring::npos)
        throw std::runtime_error("Executable quote is incomplete");
      return arguments.substr(end + 1);
    }
    auto end = arguments.find_first_of(L" \t", first);
    return end == std::wstring::npos ? L"" : arguments.substr(end);
  }
  return arguments;
}
void ActivationQueue::Submit(ActivationRequest request) {
  std::lock_guard lock(mutex_);
  if (closed_)
    throw std::runtime_error(
        "Application is shutting down; activation rejected");
  if (pending_.size() >= 64)
    throw std::runtime_error("Pending activation queue exceeded its bound");
  pending_.push_back(std::move(request));
}
std::vector<ActivationRequest> ActivationQueue::Take() {
  std::lock_guard lock(mutex_);
  std::vector<ActivationRequest> requests;
  requests.assign(std::make_move_iterator(pending_.begin()),
                  std::make_move_iterator(pending_.end()));
  pending_.clear();
  return requests;
}
void ActivationQueue::Close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
  pending_.clear();
}
}  // namespace Win2Air
