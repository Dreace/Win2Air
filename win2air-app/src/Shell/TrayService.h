#pragma once
#include <windows.h>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../Streaming/StreamCoordinator.h"
namespace Win2Air {
inline constexpr UINT kTrayMessage = WM_APP + 71;
enum class TrayEvent { activate, open, menu, shell_restarted };
enum class TrayAction { open, settings, exit, disconnect, connect, flyout };
struct TrayCommand {
  TrayAction action{TrayAction::open};
  std::string receiver_id;
};
struct TrayMenuItem {
  unsigned id{};
  TrayCommand command;
  std::wstring label;
  bool enabled{true}, checked{};
};
struct TrayLabels {
  std::wstring open, settings, exit, disconnect;
};
struct TrayPointer {
  int x{}, y{};
};
struct TrayArea {
  int x{}, y{}, width{}, height{};
};
enum class TrayFlyoutAction { show, hide, keep_closed };
inline constexpr std::chrono::milliseconds kTrayFlyoutToggle{250};
TrayArea PlaceTrayFlyout(TrayArea const& work, int pointer_x, int pointer_y,
                         int width, int height);
TrayFlyoutAction TrayFlyoutClick(
    bool visible,
    std::optional<std::chrono::steady_clock::time_point> hidden_at,
    std::chrono::steady_clock::time_point now);
class TrayBackend {
 public:
  virtual ~TrayBackend() = default;
  virtual void Add() = 0;
  virtual void Remove() = 0;
  virtual void SetConnected(bool connected) = 0;
  virtual void SetTooltip(std::wstring const& text) = 0;
  virtual unsigned Menu(std::vector<TrayMenuItem> const& items) = 0;
};
class TrayService {
  std::unique_ptr<TrayBackend> backend_;
  std::function<void(TrayCommand const&)> dispatch_;
  bool added_{};
  std::optional<bool> connected_;

 public:
  TrayService(std::unique_ptr<TrayBackend> backend,
              std::function<void(TrayCommand const&)> dispatch);
  ~TrayService();
  void Start();
  void Stop();
  void Update(PlaybackSnapshot const& snapshot);
  void Handle(TrayEvent event, PlaybackSnapshot const& snapshot,
              TrayLabels const& labels);
  bool Visible() const { return added_; }
};
std::unique_ptr<TrayBackend> NativeTrayBackend(
    HWND window,
    std::function<void(TrayEvent, std::optional<TrayPointer>)> notify);
std::vector<TrayMenuItem> TrayMenu(PlaybackSnapshot const& snapshot,
                                   TrayLabels const& labels);
enum class CloseAction { hide, exit };
CloseAction WindowClose(bool close_to_tray, bool icon_visible);
}  // namespace Win2Air
