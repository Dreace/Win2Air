#pragma once
#include <winsock2.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../src/Streaming/SenderLibrary.h"
namespace Win2Air::Tests {
struct ReceivedAudio {
  sender_timed_audio timing;
  std::vector<uint8_t> pcm;
};
class VirtualReceiver final : public SenderBackend {
  mutable std::thread::id owner_;
  mutable bool live_{};
  mutable sender_create_options options_{};
  mutable sender_state state_{SENDER_IDLE};
  mutable uint64_t generation_{};
  mutable unsigned step_{};
  mutable uint64_t timeline_{}, next_sample_{}, first_capture_{};
  mutable std::string identity_;
  mutable std::optional<sender_command_completion> pending_volume_;
  mutable std::optional<sender_command_completion> pending_presentation_;
  void CheckThread() const;
  sender_result Error(sender_error*, sender_result, char const*,
                      int peer = 0) const;

 public:
  mutable std::deque<uint32_t> media_commands;
  sender_result TakeMediaCommand(sender_runtime*, uint64_t, uint32_t*,
                                 sender_error*) const override;
  mutable std::mutex mutex;
  mutable std::vector<ReceivedAudio> accepted, attempted;
  mutable std::vector<sender_presentation> presentations;
  mutable unsigned created{}, destroyed{}, flushed{}, paired{}, configured{},
      imported{};
  mutable bool hold_pairing{}, hold_setup{}, reject_auth{}, reject_destroy{},
      export_credentials{};
  mutable unsigned backpressure{};
  mutable std::string required_password;
  mutable unsigned password_connections{};
  mutable uint64_t pending_transport_frames{}, dispatched_frames{};
  mutable unsigned timing_with_pending_audio{}, startup_ms{};
  mutable bool hold_volume{}, reject_volume{};
  mutable bool hold_presentation{}, reject_presentation{};
  mutable bool interrupt_rejected_presentation{};
  mutable unsigned presentation_requests{}, retarget_requests{};
  mutable std::optional<int32_t> rejected_presentation_base;
  mutable std::optional<int32_t> rejected_retarget_base;
  mutable std::vector<unsigned> initial_volumes, applied_volumes;
  mutable std::optional<sender_command_completion> injected_completion;
  mutable std::optional<std::pair<uint64_t, sender_state>> injected_state;
  mutable std::optional<std::pair<sender_result, int>> session_failure;
  mutable sender_result connect_failure{SENDER_OK};
  mutable sender_snapshot metrics{};
  mutable std::optional<uint32_t> receiver_render_ms{120};
  sender_result Snapshot(sender_runtime*, uint64_t, sender_snapshot*,
                         sender_error*) const override;
  mutable unsigned late_states_delivered{}, late_commands_delivered{};
  void RetainUntilProcessExit() noexcept override {}
  sender_result Create(sender_create_options const&, sender_runtime**,
                       sender_error*) const override;
  sender_result Connect(sender_runtime*, sender_target const*,
                        sender_audio_format const*, sender_credentials const*,
                        char const*, uint64_t*, sender_error*) const override;
  sender_result Poll(sender_runtime*, unsigned, sender_error*) const override;
  sender_result End(sender_runtime*, uint64_t, sender_error*) const override;
  sender_result Destroy(sender_runtime*, sender_error*) const override;
  sender_result Probe(sender_runtime*, sender_probe_request const*,
                      sender_probe_info*, sender_error*) const override;
  sender_result Submit(sender_runtime*, uint64_t, sender_timed_audio const*,
                       sender_audio_format const*, void const*, size_t,
                       sender_error*) const override;
  sender_result Flush(sender_runtime*, uint64_t, sender_error*) const override;
  sender_state State(sender_runtime*, uint64_t) const override;
  sender_result Timing(sender_runtime*, uint64_t, sender_timing_info*,
                       sender_error*) const override;
  sender_result SetVolume(sender_runtime*, uint64_t, uint64_t, uint32_t,
                          sender_error*) const override;
  sender_result SetPresentation(sender_runtime*, uint64_t, uint64_t,
                                sender_presentation const*,
                                sender_error*) const override;
  sender_result RetargetPresentation(sender_runtime*, uint64_t,
                                     sender_presentation const*,
                                     sender_error*) const override;
  mutable unsigned observed_volume{}, metadata_requests{};
  mutable uint64_t observed_volume_revision{};
  mutable std::string media_title, media_artist, media_album, media_type;
  mutable std::vector<uint8_t> media_artwork;
  mutable bool metadata_backpressure{};
  sender_result ReceiverVolume(sender_runtime*, uint64_t, uint32_t*, uint64_t*,
                               sender_error*) const override;
  sender_result NowPlayingMetadata(sender_runtime*, uint64_t, char const*,
                                   char const*, char const*, char const*,
                                   void const*, size_t,
                                   sender_error*) const override;
};
class ReceiverFixture {
  SOCKET listener_{INVALID_SOCKET};
  std::thread worker_;
  std::atomic<bool> stopping_{};

 public:
  unsigned port{};
  std::atomic<unsigned> connections{};
  ReceiverFixture();
  ~ReceiverFixture();
  ReceiverFixture(ReceiverFixture const&) = delete;
};
}  // namespace Win2Air::Tests
