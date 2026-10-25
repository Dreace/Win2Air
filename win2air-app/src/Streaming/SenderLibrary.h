#pragma once
#include <windows.h>

#include <filesystem>

#include "../../../airplay2-sender-core/include/sender.h"
namespace Win2Air {
class SenderBackend {
 public:
  virtual sender_result TakeMediaCommand(sender_runtime*, uint64_t, uint32_t*,
                                         sender_error*) const = 0;
  virtual ~SenderBackend() = default;
  virtual void RetainUntilProcessExit() noexcept = 0;
  virtual sender_result Create(sender_create_options const&, sender_runtime**,
                               sender_error*) const = 0;
  virtual sender_result Connect(sender_runtime*, sender_target const*,
                                sender_audio_format const*,
                                sender_credentials const*, char const*,
                                uint64_t*, sender_error*) const = 0;
  virtual sender_result Poll(sender_runtime*, unsigned,
                             sender_error*) const = 0;
  virtual sender_result End(sender_runtime*, uint64_t, sender_error*) const = 0;
  virtual sender_result Destroy(sender_runtime*, sender_error*) const = 0;
  virtual sender_result Probe(sender_runtime*, sender_probe_request const*,
                              sender_probe_info*, sender_error*) const = 0;
  virtual sender_result Submit(sender_runtime*, uint64_t,
                               sender_timed_audio const*,
                               sender_audio_format const*, void const*, size_t,
                               sender_error*) const = 0;
  virtual sender_result Flush(sender_runtime*, uint64_t,
                              sender_error*) const = 0;
  virtual sender_state State(sender_runtime*, uint64_t) const = 0;
  virtual sender_result Timing(sender_runtime*, uint64_t, sender_timing_info*,
                               sender_error*) const = 0;
  virtual sender_result Snapshot(sender_runtime*, uint64_t, sender_snapshot*,
                                 sender_error*) const = 0;
  virtual sender_result SetVolume(sender_runtime*, uint64_t, uint64_t, uint32_t,
                                  sender_error*) const = 0;
  virtual sender_result SetPresentation(sender_runtime*, uint64_t, uint64_t,
                                        sender_presentation const*,
                                        sender_error*) const = 0;
  virtual sender_result RetargetPresentation(sender_runtime*, uint64_t,
                                             sender_presentation const*,
                                             sender_error*) const = 0;
  virtual sender_result ReceiverVolume(sender_runtime*, uint64_t, uint32_t*,
                                       uint64_t*, sender_error*) const = 0;
  virtual sender_result NowPlayingMetadata(sender_runtime*, uint64_t,
                                           char const*, char const*,
                                           char const*, char const*,
                                           void const*, size_t,
                                           sender_error*) const = 0;
};
class SenderLibrary : public SenderBackend {
  HMODULE module_{};
  decltype(&sender_get_abi) query_{};
  decltype(&sender_create_ex) create_{};
  decltype(&sender_connect) connect_{};
  decltype(&sender_poll) poll_{};
  decltype(&sender_end) end_{};
  decltype(&sender_destroy) destroy_{};
  decltype(&sender_probe_target) probe_{};
  decltype(&sender_submit_timed) submit_{};
  decltype(&sender_flush) flush_{};
  decltype(&sender_get_state) state_{};
  decltype(&sender_get_timing) timing_{};
  decltype(&sender_get_snapshot) snapshot_{};
  decltype(&sender_set_volume) volume_{};
  decltype(&sender_set_presentation) presentation_{};
  decltype(&sender_retarget_presentation) retarget_{};
  decltype(&sender_get_receiver_volume) receiver_volume_{};
  decltype(&sender_take_media_command) media_command_{};
  decltype(&sender_set_now_playing) now_playing_{};
  sender_abi_info abi_{};
  bool retain_module_{};
  template <class T>
  T Resolve(char const* name);

 public:
  explicit SenderLibrary(std::filesystem::path const& absolute_package_dll);
  sender_result TakeMediaCommand(sender_runtime* runtime, uint64_t generation,
                                 uint32_t* command,
                                 sender_error* error) const override {
    return media_command_(runtime, generation, command, error);
  }
  ~SenderLibrary() override;
  SenderLibrary(SenderLibrary const&) = delete;
  SenderLibrary& operator=(SenderLibrary const&) = delete;
  sender_abi_info Abi() const noexcept { return abi_; }
  bool HasExport(char const* name) const noexcept;
  void RetainUntilProcessExit() noexcept override { retain_module_ = true; }
  sender_result QueryAbi(uint32_t version, uint32_t size,
                         sender_abi_info* value) const {
    return query_(version, size, value);
  }
  sender_result Create(sender_create_options const& options,
                       sender_runtime** value,
                       sender_error* error) const override {
    return create_(&options, value, error);
  }
  sender_result Connect(sender_runtime* value, sender_target const* target,
                        sender_audio_format const* format,
                        sender_credentials const* credentials, char const* pin,
                        uint64_t* generation,
                        sender_error* error) const override {
    return connect_(value, target, format, credentials, pin, generation, error);
  }
  sender_result Poll(sender_runtime* value, unsigned wait,
                     sender_error* error) const override {
    return poll_(value, wait, error);
  }
  sender_result End(sender_runtime* value, uint64_t generation,
                    sender_error* error) const override {
    return end_(value, generation, error);
  }
  sender_result Destroy(sender_runtime* value,
                        sender_error* error) const override {
    return destroy_(value, error);
  }
  sender_result Probe(sender_runtime* value,
                      sender_probe_request const* request,
                      sender_probe_info* info,
                      sender_error* error) const override {
    return probe_(value, request, info, error);
  }
  sender_result Submit(sender_runtime* value, uint64_t generation,
                       sender_timed_audio const* timing,
                       sender_audio_format const* format, void const* pcm,
                       size_t bytes, sender_error* error) const override {
    return submit_(value, generation, timing, format, pcm, bytes, error);
  }
  sender_result Flush(sender_runtime* value, uint64_t generation,
                      sender_error* error) const override {
    return flush_(value, generation, error);
  }
  sender_state State(sender_runtime* value,
                     uint64_t generation) const override {
    return state_(value, generation);
  }
  sender_result Timing(sender_runtime* value, uint64_t generation,
                       sender_timing_info* info,
                       sender_error* error) const override {
    return timing_(value, generation, info, error);
  }
  sender_result SetVolume(sender_runtime* value, uint64_t generation,
                          uint64_t command_id, uint32_t percent,
                          sender_error* error) const override {
    return volume_(value, generation, command_id, percent, error);
  }
  sender_result Snapshot(sender_runtime* value, uint64_t generation,
                         sender_snapshot* snapshot,
                         sender_error* error) const override {
    return snapshot_(value, generation, snapshot, error);
  }
  sender_result SetPresentation(sender_runtime* value, uint64_t generation,
                                uint64_t command_id,
                                sender_presentation const* config,
                                sender_error* error) const override {
    return presentation_(value, generation, command_id, config, error);
  }
  sender_result ReceiverVolume(sender_runtime* value, uint64_t generation,
                               uint32_t* percent, uint64_t* revision,
                               sender_error* error) const override {
    return receiver_volume_(value, generation, percent, revision, error);
  }
  sender_result NowPlayingMetadata(sender_runtime* value, uint64_t generation,
                                   char const* title, char const* artist,
                                   char const* album, char const* type,
                                   void const* artwork, size_t bytes,
                                   sender_error* error) const override {
    return now_playing_(value, generation, title, artist, album, type, artwork,
                        bytes, error);
  }
  sender_result RetargetPresentation(sender_runtime* value, uint64_t generation,
                                     sender_presentation const* config,
                                     sender_error* error) const override {
    return retarget_(value, generation, config, error);
  }
};
}  // namespace Win2Air
