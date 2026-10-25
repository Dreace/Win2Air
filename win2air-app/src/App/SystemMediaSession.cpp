#include "SystemMediaSession.h"
#include <windows.h>
#include <stdexcept>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

namespace Win2Air {
namespace {
template <typename T>
T Await(winrt::Windows::Foundation::IAsyncOperation<T> const& operation) {
  if (operation.wait_for(std::chrono::seconds(2)) ==
      winrt::Windows::Foundation::AsyncStatus::Started) {
    operation.Cancel();
    throw std::runtime_error("Windows media operation exceeded its deadline");
  }
  return operation.GetResults();
}
}  // namespace
SystemMediaSession::SystemMediaSession(
    std::function<void(LogRecord const&)> log_sink)
    : log_sink_(std::move(log_sink)), worker_([this] { Run(); }) {}
SystemMediaSession::~SystemMediaSession() {
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
  }
  state_->wake.notify_one();
  worker_.join();
}
std::shared_ptr<NowPlaying const> SystemMediaSession::Snapshot() {
  std::lock_guard lock(state_->mutex);
  return state_->snapshot;
}
bool SystemMediaSession::Control(uint32_t command) {
  std::lock_guard lock(state_->mutex);
  if (state_->stopping || command < 1 || command > 4 ||
      state_->commands.size() >= 16)
    return false;
  state_->commands.push_back(command);
  state_->wake.notify_one();
  return true;
}
void SystemMediaSession::Run() {
  using namespace winrt::Windows::Media::Control;
  using namespace winrt::Windows::Storage::Streams;
  uint64_t revision = 0;
  bool apartment = false;
  auto changed = [state = state_](auto const&, auto const&) {
    {
      std::lock_guard lock(state->mutex);
      state->dirty = true;
    }
    state->wake.notify_one();
  };
  auto publish = [&](NowPlaying item) {
    item.revision = ++revision;
    std::lock_guard lock(state_->mutex);
    state_->snapshot = std::make_shared<NowPlaying const>(std::move(item));
  };
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    apartment = true;
    {
      auto manager = Await(
          GlobalSystemMediaTransportControlsSessionManager::RequestAsync());
      auto manager_changed =
          manager.CurrentSessionChanged(winrt::auto_revoke, changed);
      GlobalSystemMediaTransportControlsSession current{nullptr};
      GlobalSystemMediaTransportControlsSession::MediaPropertiesChanged_revoker
          media_changed;
      while (true) {
        std::deque<uint32_t> commands;
        {
          std::unique_lock lock(state_->mutex);
          state_->wake.wait(lock, [&] {
            return state_->stopping || state_->dirty ||
                   !state_->commands.empty();
          });
          if (state_->stopping) break;
          state_->dirty = false;
          commands.swap(state_->commands);
        }
        auto next = manager.GetCurrentSession();
        if (next != current) {
          media_changed.revoke();
          current = next;
          if (current)
            media_changed =
                current.MediaPropertiesChanged(winrt::auto_revoke, changed);
        }
        NowPlaying item;
        try {
          for (auto command : commands) {
            {
              std::lock_guard lock(state_->mutex);
              if (state_->stopping) break;
            }
            bool applied = false;
            std::string failure;
            try {
              if (!current)
                throw std::runtime_error(
                    "Receiver media control: no current Windows session");
              auto operation = command == 1   ? current.TryPauseAsync()
                               : command == 2 ? current.TryPlayAsync()
                               : command == 3 ? current.TrySkipNextAsync()
                                              : current.TrySkipPreviousAsync();
              if (!Await(operation))
                throw std::runtime_error(
                    "Windows media session rejected receiver control");
              applied = true;
            } catch (winrt::hresult_error const& error) {
              failure = winrt::to_string(error.message());
            } catch (std::exception const& error) {
              failure = error.what();
            }
            if (!failure.empty()) item.error = failure;
            try {
              constexpr char const* actions[]{"", "pause", "play", "next",
                                              "previous"};
              LogRecord record;
              record.level = applied ? LogLevel::info : LogLevel::warning;
              record.event = LogEvent::media_control;
              std::string detail =
                  std::string("action=") + actions[command] +
                  (applied ? " outcome=applied" : " outcome=rejected");
              if (!failure.empty()) detail += " error=" + failure;
              LogDiagnostic(log_sink_, record, detail);
            } catch (...) {
              OutputDebugStringW(L"Win2Air: media control log write failed\n");
            }
          }
          if (current) {
            auto properties = Await(current.TryGetMediaPropertiesAsync());
            item.title = winrt::to_string(properties.Title());
            item.artist = winrt::to_string(properties.Artist());
            item.album = winrt::to_string(properties.AlbumTitle());
            if (item.title.size() > 4096 || item.artist.size() > 4096 ||
                item.album.size() > 4096)
              throw std::runtime_error(
                  "Windows media text exceeds the supported limit");
            if (auto thumbnail = properties.Thumbnail()) {
              auto stream = Await(thumbnail.OpenReadAsync());
              auto size = stream.Size();
              item.artwork_type = winrt::to_string(stream.ContentType());
              if (size > 1024 * 1024 ||
                  (size && item.artwork_type != "image/jpeg" &&
                   item.artwork_type != "image/png"))
                throw std::runtime_error(
                    "Windows media artwork must be JPEG or PNG within 1 MiB");
              if (size) {
                // Read only the image header here. Bound the decoded size and
                // aspect ratio before the UI or receiver processes its bytes.
                auto decoder = Await(winrt::Windows::Graphics::Imaging::
                                         BitmapDecoder::CreateAsync(stream));
                uint64_t width = decoder.PixelWidth(),
                         height = decoder.PixelHeight();
                if (!width || !height || width > 8192 || height > 8192 ||
                    width * height > 16777216 || width > height * 32 ||
                    height > width * 32)
                  throw std::runtime_error(
                      "Windows media artwork dimensions exceed the supported "
                      "limit");
                stream.Seek(0);
                DataReader reader(stream);
                auto count =
                    Await(reader.LoadAsync(static_cast<uint32_t>(size)));
                if (count != size)
                  throw std::runtime_error(
                      "Windows media artwork was truncated");
                item.artwork.resize(count);
                reader.ReadBytes(item.artwork);
                reader.DetachStream();
              }
            }
          }
        } catch (winrt::hresult_error const& error) {
          item.error = winrt::to_string(error.message());
        } catch (std::exception const& error) {
          item.error = error.what();
        }
        publish(std::move(item));
      }
    }
  } catch (winrt::hresult_error const& error) {
    NowPlaying item;
    item.error = winrt::to_string(error.message());
    publish(std::move(item));
  } catch (std::exception const& error) {
    NowPlaying item;
    item.error = error.what();
    publish(std::move(item));
  }
  if (apartment) winrt::uninit_apartment();
  std::lock_guard lock(state_->mutex);
  state_->stopping = true;
}
}  // namespace Win2Air
