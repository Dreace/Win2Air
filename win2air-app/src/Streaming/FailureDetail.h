#pragma once
#include <winrt/base.h>
#include "../../../airplay2-sender-core/include/sender.h"

#include <exception>
#include <stdexcept>
#include <string>
namespace Win2Air {
struct FailureDetail {
  std::string message;
  int platform{}, peer{};
  int sender_code{SENDER_PLATFORM};
};
class SenderFailure : public std::runtime_error {
 public:
  int platform{}, peer{};
  int sender_code;
  SenderFailure(std::string message, int platform_code, int peer_code,
                int code = SENDER_PLATFORM)
      : std::runtime_error(std::move(message)),
        platform(platform_code),
        peer(peer_code),
        sender_code(code) {}
};
inline FailureDetail DescribeFailure(
    std::exception_ptr error = std::current_exception()) {
  if (!error)
    throw std::invalid_argument("Native failure detail requires an exception");
  try {
    std::rethrow_exception(error);
  } catch (winrt::hresult_error const& value) {
    return {winrt::to_string(value.message()) +
                "; HRESULT=" + std::to_string(value.code().value),
            value.code().value, 0};
  } catch (SenderFailure const& value) {
    return {value.what(), value.platform, value.peer, value.sender_code};
  } catch (std::exception const& value) {
    return {value.what(), 0, 0};
  } catch (...) {
    return {"Unknown native exception", static_cast<int>(0x80004005), 0};
  }
}
}  // namespace Win2Air
