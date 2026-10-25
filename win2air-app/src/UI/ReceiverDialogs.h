#pragma once
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <functional>
#include <future>
#include "../Settings/AppSettings.h"
#include "../Settings/CredentialStore.h"
#include "../Streaming/SessionModels.h"

namespace Win2Air {
using PairingSubmit = std::function<std::future<CommandResult>(SecureBuffer)>;
using AddReceiverSubmit =
    std::function<std::future<CommandResult>(std::string, unsigned)>;
winrt::Microsoft::UI::Xaml::Controls::ContentDialog ShowPairingDialog(
    winrt::Microsoft::UI::Xaml::FrameworkElement owner, ReceiverRecord receiver,
    PairingSubmit submit, std::function<void()> closed = {},
    std::function<void(std::string)> failed = {});
winrt::Microsoft::UI::Xaml::Controls::ContentDialog ShowAddReceiverDialog(
    winrt::Microsoft::UI::Xaml::FrameworkElement owner,
    AddReceiverSubmit submit, std::function<void(std::string)> failed = {});
}  // namespace Win2Air
