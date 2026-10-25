#include "StreamCoordinatorInternal.h"

#include <avrt.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <thread>
#include <utility>

#include "../Settings/StorageIO.h"
#include "FailureDetail.h"
#include "LatencyProfile.h"
#include "ReceiverSession.h"
namespace Win2Air {
std::future<CommandResult> StreamCoordinator::SetReceiverVolume(
    std::string identity, double value) {
  return Queue(false,
               [this, identity = std::move(identity), value](auto result) {
                 auto record = registry_.Find(identity);
                 record.volume = value;
                 return ChangeVolume(result, std::move(record));
               });
}
std::future<CommandResult> StreamCoordinator::SetReceiverMuted(
    std::string identity, bool muted) {
  return Queue(false,
               [this, identity = std::move(identity), muted](auto result) {
                 auto record = registry_.Find(identity);
                 record.muted = muted;
                 return ChangeVolume(result, std::move(record));
               });
}
std::future<CommandResult> StreamCoordinator::SetOffset(std::string identity,
                                                        int offset_ms) {
  return Queue(false, [this, identity = std::move(identity),
                       offset_ms](auto result) {
    auto record = registry_.Find(identity);
    record.offset_ms = offset_ms;
    return ChangePresentation(result, settings_.settings, std::move(record));
  });
}
std::future<CommandResult> StreamCoordinator::SetAppearance(
    std::string theme, std::string language) {
  return Queue(false, [this, theme = std::move(theme),
                       language = std::move(language)](auto result) {
    auto previous = settings_;
    settings_.settings.theme = theme;
    settings_.settings.language = language;
    try {
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      throw;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::SetStatistics(
    AppSettings::StatisticsPreference value) {
  return Queue(false, [this, value](auto result) {
    auto previous = settings_;
    settings_.settings.statistics = value;
    try {
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      throw;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::shared_future<CommandResult> StreamCoordinator::Shutdown(
    bool final_exit, std::optional<WindowPlacement> placement) {
  std::lock_guard lock(shutdown_mutex_);
  if (shutdown_result_.valid() &&
      (stopping_ ||
       shutdown_result_.wait_for(std::chrono::seconds(0)) !=
           std::future_status::ready ||
       shutdown_result_.get().status == CommandStatus::applied))
    return shutdown_result_;
  shutting_down_ = true;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(4500);
  shutdown_deadline_ = deadline;
  shutdown_result_ =
      Queue(
          true,
          [this, final_exit, placement, deadline](auto result) {
            desired_connected_ = false;
            reconnect_.Reset();
            reconnecting_ = false;
            std::string save_error;
            if (placement) {
              auto previous = settings_;
              try {
                settings_.settings.window_placement = *placement;
                ValidateSettings(settings_);
                Save();
              } catch (...) {
                settings_ = std::move(previous);
                save_error = DescribeFailure().message;
              }
            }
            try {
              Cleanup();
              StopDiscovery(deadline);
              stopping_ = true;
              result.status = CommandStatus::applied;
              result.stage = SessionState::disconnected;
              if (!save_error.empty()) {
                result.status = CommandStatus::failed;
                result.message = save_error;
              }
              return result;
            } catch (...) {
              if (final_exit) {
                auto detail = DescribeFailure();
                if (!save_error.empty())
                  detail.message += "; window persistence: " + save_error;
                try {
                  local_->Restore();
                } catch (...) {
                  detail.message +=
                      "; local restoration retry: " + DescribeFailure().message;
                }
                exit_cleanup_failed_ = true;
                stopping_ = true;
                try {
                  StopDiscovery(deadline);
                } catch (...) {
                  detail.message +=
                      "; discovery cleanup: " + DescribeFailure().message;
                }
                result.status = CommandStatus::failed;
                result.stage = SessionState::error;
                result.message = detail.message;
                result.platform_code = detail.platform;
                result.peer_code = detail.peer;
                return result;
              }
              shutting_down_ = false;
              shutdown_deadline_ = BrowseDeadline::max();
              throw;
            }
          },
          true)
          .share();
  return shutdown_result_;
}
std::future<CommandResult> StreamCoordinator::SetLifecycle(
    bool close_to_tray, bool minimized, bool restore_previous) {
  return Queue(false,
               [this, close_to_tray, minimized, restore_previous](auto result) {
                 auto previous = settings_;
                 settings_.settings.close_to_tray = close_to_tray;
                 settings_.settings.minimized = minimized;
                 settings_.settings.restore_previous = restore_previous;
                 try {
                   Save();
                 } catch (...) {
                   settings_ = std::move(previous);
                   throw;
                 }
                 result.status = CommandStatus::applied;
                 return result;
               });
}
std::future<CommandResult> StreamCoordinator::SetStartup(bool enabled) {
  return Queue(false, [this, enabled](auto result) {
    auto previous = settings_;
    settings_.settings.startup = enabled;
    try {
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      throw;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::SaveWindow(
    WindowPlacement placement) {
  return Queue(false, [this, placement](auto result) {
    auto previous = settings_;
    try {
      settings_.settings.window_placement = placement;
      ValidateSettings(settings_);
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      throw;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::SetSource(
    AudioSourceConfig source) {
  return Queue(false, [this, source = std::move(source)](auto result) {
    auto candidate = settings_;
    candidate.settings.audio_source = source;
    ValidateSettings(candidate);
    devices_->Resolve(source);
    auto previous = settings_;
    bool restart = capturing_;
    try {
      if (restart) {
        capture_->Stop();
        capturing_ = false;
        local_->Restore();
        FlushSource();
      }
      settings_.settings.audio_source = source;
      if (restart) StartSource();
      Save();
    } catch (...) {
      auto original = std::current_exception();
      settings_ = std::move(previous);
      try {
        if (restart) {
          capture_->Stop();
          capturing_ = false;
          local_->Restore();
          FlushSource();
          StartSource();
        }
      } catch (...) {
        auto message = DescribeFailure(original).message +
                       "; source rollback failed: " + DescribeFailure().message;
        Fail(message);
        try {
          Cleanup();
        } catch (...) {
          message += "; cleanup: " + DescribeFailure().message;
        }
        throw std::runtime_error(message);
      }
      std::rethrow_exception(original);
    }
    Note(LogLevel::info, LogModule::audio, LogEvent::settings_changed,
         std::string("operation=select-source follow_default=") +
             (source.follow_system_default ? "true" : "false"));
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::SetLogLevel(std::string level) {
  return Queue(false, [this, level = std::move(level)](auto result) {
    auto resolved = ResolveLogLevel(level);
    auto previous = settings_;
    settings_.settings.log_level = level;
    try {
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      throw;
    }
    if (audio_services_.log_level_changed)
      audio_services_.log_level_changed(resolved);
    Note(LogLevel::info, LogModule::settings, LogEvent::settings_changed,
         "log_level=" + std::string(LogLevelName(resolved)));
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::SetReceiverMediaControl(
    bool enabled) {
  return Queue(false, [this, enabled](auto result) {
    auto previous = settings_;
    settings_.settings.receiver_media_control = enabled;
    try {
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      throw;
    }
    Note(LogLevel::info, LogModule::settings, LogEvent::settings_changed,
         "receiver_media_control=" + std::to_string(enabled));
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::SetLocalPlayback(bool enabled) {
  return Queue(false, [this, enabled](auto result) {
    auto previous = settings_;
    try {
      if (enabled)
        local_->Restore();
      else if (capturing_)
        local_->Disable(endpoint_, source_epoch_);
      settings_.settings.local_playback = enabled;
      Save();
    } catch (...) {
      auto original = std::current_exception();
      settings_ = std::move(previous);
      try {
        if (settings_.settings.local_playback)
          local_->Restore();
        else if (capturing_)
          local_->Disable(endpoint_, source_epoch_);
      } catch (...) {
        throw std::runtime_error(
            DescribeFailure(original).message +
            "; playback rollback failed: " + DescribeFailure().message);
      }
      std::rethrow_exception(original);
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
}  // namespace Win2Air
