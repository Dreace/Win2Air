#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../Diagnostics/Logger.h"

namespace Win2Air {
struct NowPlaying {
  std::string title, artist, album, artwork_type, error;
  std::vector<uint8_t> artwork;
  uint64_t revision{};
};
// Reads Windows' current media session on its own MTA thread. Session callbacks
// only wake that thread, so thumbnail I/O never blocks audio or the UI.
class SystemMediaSession {
  struct State {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping{}, dirty{true};
    std::deque<uint32_t> commands;
    std::shared_ptr<NowPlaying const> snapshot;
  };
  std::shared_ptr<State> state_{std::make_shared<State>()};
  std::function<void(LogRecord const&)> log_sink_;
  std::thread worker_;
  void Run();

 public:
  explicit SystemMediaSession(
      std::function<void(LogRecord const&)> log_sink = {});
  ~SystemMediaSession();
  std::shared_ptr<NowPlaying const> Snapshot();
  bool Control(uint32_t command);
};
}  // namespace Win2Air
