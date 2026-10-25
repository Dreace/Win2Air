#include "ReceiverFixture.h"
#include "../../src/Audio/AudioClock.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
namespace Win2Air::Tests {
sender_result VirtualReceiver::TakeMediaCommand(sender_runtime*, uint64_t,
                                                uint32_t* command,
                                                sender_error*) const {
  CheckThread();
  std::lock_guard lock(mutex);
  *command = 0;
  if (!media_commands.empty()) {
    *command = media_commands.front();
    media_commands.pop_front();
  }
  return SENDER_OK;
}
sender_result VirtualReceiver::ReceiverVolume(sender_runtime*, uint64_t,
                                              uint32_t* percent,
                                              uint64_t* revision,
                                              sender_error*) const {
  CheckThread();
  std::lock_guard lock(mutex);
  *percent = observed_volume;
  *revision = observed_volume_revision;
  return SENDER_OK;
}
sender_result VirtualReceiver::NowPlayingMetadata(
    sender_runtime*, uint64_t, char const* title, char const* artist,
    char const* album, char const* type, void const* artwork, size_t bytes,
    sender_error*) const {
  CheckThread();
  std::lock_guard lock(mutex);
  if (metadata_backpressure) return SENDER_BACKPRESSURE;
  ++metadata_requests;
  media_title = title;
  media_artist = artist;
  media_album = album;
  media_type = type;
  media_artwork.clear();
  if (bytes) {
    auto data = static_cast<uint8_t const*>(artwork);
    media_artwork.assign(data, data + bytes);
  }
  return SENDER_OK;
}
void VirtualReceiver::CheckThread() const {
  if (owner_ != std::this_thread::get_id())
    throw std::runtime_error("Sender boundary escaped its serial owner thread");
}
sender_result VirtualReceiver::Error(sender_error* error, sender_result code,
                                     char const* message, int peer) const {
  if (error) {
    *error = {};
    error->code = code;
    error->stage = state_;
    error->generation = generation_;
    error->peer_code = peer;
    error->platform_code = code == SENDER_PLATFORM ? ERROR_ACCESS_DENIED : 0;
    std::snprintf(error->operation, sizeof(error->operation),
                  "virtual receiver");
    std::snprintf(error->message, sizeof(error->message), "%s", message);
  }
  return code;
}
sender_result VirtualReceiver::Create(sender_create_options const& options,
                                      sender_runtime** runtime,
                                      sender_error* error) const {
  std::lock_guard lock(mutex);
  if (live_)
    return Error(error, SENDER_OCCUPIED,
                 "Previous receiver runtime remains owned");
  owner_ = std::this_thread::get_id();
  options_ = options;
  startup_ms = options.buffers.startup_ms;
  pending_transport_frames = 0;
  live_ = true;
  state_ = SENDER_IDLE;
  pending_presentation_.reset();
  ++created;
  *runtime =
      reinterpret_cast<sender_runtime*>(const_cast<VirtualReceiver*>(this));
  return SENDER_OK;
}
sender_result VirtualReceiver::Connect(
    sender_runtime*, sender_target const* target, sender_audio_format const*,
    sender_credentials const* credentials, char const* password,
    uint64_t* generation, sender_error* error) const {
  std::lock_guard lock(mutex);
  CheckThread();
  if (!live_ || std::string(target->address) != "127.0.0.1")
    return Error(error, SENDER_INPUT,
                 "Virtual receiver only accepts its loopback endpoint");
  if (connect_failure != SENDER_OK)
    return Error(error, connect_failure,
                 "Controlled receiver connection failure");
  if (!required_password.empty()) {
    if (!password || required_password != password)
      return Error(error, SENDER_AUTHENTICATION,
                   "The saved receiver password was rejected", 403);
    ++password_connections;
  }
  if (credentials) {
    if (credentials->sender_id != options_.options.sender_id ||
        std::string(credentials->receiver_id) != target->receiver_id ||
        !credentials->material ||
        std::string(credentials->material) != std::string(192, 'A'))
      return Error(error, SENDER_AUTHENTICATION,
                   "Credential binding differs from target");
    ++imported;
  }
  ++generation_;
  metrics = {};
  identity_ = target->receiver_id;
  initial_volumes.push_back(target->volume_percent);
  *generation = generation_;
  state_ = SENDER_CONNECTING;
  step_ = 0;
  timeline_ = next_sample_ = first_capture_ = 0;
  presentations.push_back(options_.initial_presentation);
  return SENDER_OK;
}
sender_result VirtualReceiver::Poll(sender_runtime*, unsigned,
                                    sender_error*) const {
  std::unique_lock lock(mutex);
  CheckThread();
  auto generation = generation_;
  dispatched_frames += pending_transport_frames;
  pending_transport_frames = 0;
  auto late_state = std::exchange(injected_state, std::nullopt);
  auto options = options_;
  sender_state event = state_;
  sender_step step{};
  bool send_step = false, send_state = false, send_credentials = false;
  sender_error failure{};
  std::optional<sender_command_completion> completion;
  bool late_command = injected_completion.has_value();
  if (injected_completion) {
    completion = std::exchange(injected_completion, std::nullopt);
  } else if (pending_volume_ && !hold_volume) {
    completion = std::exchange(pending_volume_, std::nullopt);
    if (reject_volume) {
      completion->result = Error(&completion->error, SENDER_NETWORK,
                                 "Controlled volume request rejected", 451);
    } else {
      applied_volumes.push_back(completion->volume_percent);
    }
  }
  auto identity = identity_;
  if (pending_presentation_ &&
      (!hold_presentation || state_ == SENDER_ENDING) && !completion) {
    completion = std::exchange(pending_presentation_, std::nullopt);
    if (state_ == SENDER_ENDING)
      completion->result = SENDER_CANCELED;
    else if (reject_presentation ||
             rejected_presentation_base == completion->presentation.base_ms)
      completion->result = Error(&completion->error, SENDER_CAPABILITY,
                                 "Controlled presentation rejected", 451);
    else {
      options_.initial_presentation = completion->presentation;
      presentations.push_back(completion->presentation);
    }
    if (state_ != SENDER_ENDING) {
      if (completion->result != SENDER_OK && interrupt_rejected_presentation) {
        completion->result = Error(&completion->error, SENDER_NETWORK,
                                   "Receiver rejected live stream setup", 400);
        state_ = event = SENDER_FAILED;
        failure = completion->error;
      } else {
        state_ = event = SENDER_READY;
      }
      send_state = true;
      ++flushed;
      timeline_ = next_sample_ = first_capture_ = 0;
    }
  }
  if (session_failure) {
    auto [code, peer] = *std::exchange(session_failure, std::nullopt);
    state_ = event = SENDER_FAILED;
    Error(&failure, code, "Controlled receiver session interrupted", peer);
    send_state = true;
  } else if (state_ == SENDER_CONNECTING) {
    state_ = event = SENDER_AUTHENTICATING;
    step_ = 1;
    ++paired;
    step = SENDER_STEP_PAIRING;
    send_step = send_state = true;
  } else if (state_ == SENDER_AUTHENTICATING && step_ == 1 && !hold_pairing) {
    if (reject_auth) {
      state_ = event = SENDER_FAILED;
      Error(&failure, SENDER_AUTHENTICATION,
            "Controlled authentication rejected", 470);
      send_state = true;
    } else {
      step_ = 2;
      ++configured;
      step = SENDER_STEP_SETTING_UP;
      send_step = true;
    }
  } else if (state_ == SENDER_AUTHENTICATING && step_ == 2 && !hold_setup) {
    state_ = event = SENDER_READY;
    send_state = true;
    send_credentials = export_credentials;
  } else if (state_ == SENDER_ENDING) {
    state_ = event = SENDER_ENDED;
    send_state = true;
  }
  lock.unlock();
  if (options.on_log && (send_state || send_step || completion)) {
    sender_log_record record{};
    record.size = sizeof(record);
    record.version = 1;
    record.generation = completion ? completion->generation : generation;
    record.event = completion ? 2 : send_step ? 1 : 0;
    record.state = static_cast<uint32_t>(event);
    record.code = completion ? completion->result : failure.code;
    record.platform_code =
        completion ? completion->error.platform_code : failure.platform_code;
    record.peer_code =
        completion ? completion->error.peer_code : failure.peer_code;
    options.on_log(options.options.context, &record);
  }
  if (send_step && options.on_step)
    options.on_step(options.options.context, generation, step);
  if (send_credentials && options.options.on_credentials) {
    std::string secret(192, 'A');
    sender_credentials value{identity.c_str(), options.options.sender_id,
                             secret.c_str()};
    options.options.on_credentials(options.options.context, generation, &value);
    SecureZeroMemory(secret.data(), secret.size());
  }
  if (send_state && options.options.on_state)
    options.options.on_state(options.options.context, generation, event,
                             event == SENDER_FAILED ? &failure : nullptr);
  if (completion && options.on_command) {
    options.on_command(options.options.context, &*completion);
    SecureZeroMemory(&*completion, sizeof(*completion));
    if (late_command) {
      std::lock_guard delivered(mutex);
      ++late_commands_delivered;
    }
  }
  if (late_state && options.options.on_state) {
    options.options.on_state(options.options.context, late_state->first,
                             late_state->second, nullptr);
    std::lock_guard delivered(mutex);
    ++late_states_delivered;
  }
  return SENDER_OK;
}
sender_result VirtualReceiver::End(sender_runtime*, uint64_t,
                                   sender_error*) const {
  std::lock_guard lock(mutex);
  CheckThread();
  state_ = SENDER_ENDING;
  return SENDER_OK;
}
sender_result VirtualReceiver::Destroy(sender_runtime*,
                                       sender_error* error) const {
  std::lock_guard lock(mutex);
  CheckThread();
  if (reject_destroy)
    return Error(error, SENDER_PLATFORM, "Controlled destroy failure");
  live_ = false;
  pending_volume_.reset();
  pending_presentation_.reset();
  state_ = SENDER_IDLE;
  ++destroyed;
  return SENDER_OK;
}
sender_result VirtualReceiver::Probe(sender_runtime*,
                                     sender_probe_request const* request,
                                     sender_probe_info* info,
                                     sender_error* error) const {
  std::lock_guard lock(mutex);
  CheckThread();
  if (std::string(request->address) != "127.0.0.1")
    return Error(error, SENDER_INPUT, "Virtual probe requires loopback");
  info->probe_id = request->probe_id;
  std::snprintf(info->receiver_id, sizeof(info->receiver_id),
                "AA:BB:CC:DD:EE:FF");
  std::snprintf(info->name, sizeof(info->name), "Virtual audio receiver");
  std::snprintf(info->model, sizeof(info->model), "Software receiver");
  info->features_available = 1;
  info->features =
      (uint64_t(1) << 9) | (uint64_t(1) << 41) | (uint64_t(1) << 48);
  return SENDER_OK;
}
sender_result VirtualReceiver::Submit(sender_runtime*, uint64_t generation,
                                      sender_timed_audio const* timing,
                                      sender_audio_format const* format,
                                      void const* pcm, size_t bytes,
                                      sender_error* error) const {
  std::lock_guard lock(mutex);
  CheckThread();
  if (generation != generation_ || !bytes || bytes % 4 ||
      format->sample_rate != 44100 || format->bits_per_sample != 16 ||
      format->channels != 2)
    return Error(error, SENDER_INPUT,
                 "Virtual receiver rejected sample contract");
  auto begin = static_cast<uint8_t const*>(pcm);
  ReceivedAudio record{*timing, {begin, begin + bytes}};
  attempted.push_back(record);
  if (backpressure) {
    --backpressure;
    return SENDER_BACKPRESSURE;
  }
  if (timeline_ && timeline_ != timing->timeline_epoch)
    return Error(error, SENDER_SYNC,
                 "New source timeline requires receiver FLUSH");
  if (timing->sample_index != next_sample_)
    return Error(error, SENDER_SYNC,
                 "Receiver sample order was not contiguous");
  timeline_ = timing->timeline_epoch;
  if (!first_capture_) first_capture_ = timing->capture_monotonic_ns;
  next_sample_ += bytes / 4;
  accepted.push_back(std::move(record));
  pending_transport_frames += bytes / 4;
  ++metrics.sent_packets;
  metrics.sent_bytes += bytes;
  return SENDER_OK;
}
sender_result VirtualReceiver::Flush(sender_runtime*, uint64_t,
                                     sender_error*) const {
  std::lock_guard lock(mutex);
  CheckThread();
  ++flushed;
  pending_transport_frames = 0;
  timeline_ = next_sample_ = first_capture_ = 0;
  return SENDER_OK;
}
sender_state VirtualReceiver::State(sender_runtime*, uint64_t) const {
  std::lock_guard lock(mutex);
  CheckThread();
  return state_;
}
sender_result VirtualReceiver::Timing(sender_runtime*, uint64_t,
                                      sender_timing_info* info,
                                      sender_error*) const {
  std::lock_guard lock(mutex);
  CheckThread();
  info->base_ms = options_.initial_presentation.base_ms;
  if (pending_transport_frames) ++timing_with_pending_audio;
  info->offset_ms = options_.initial_presentation.offset_ms;
  info->effective_ms = info->base_ms + info->offset_ms;
  auto render = options_.buffers.render_ms
                    ? int(options_.buffers.render_ms)
                    : std::min({info->base_ms, info->effective_ms, 250});
  info->buffer_samples =
      static_cast<uint32_t>((info->base_ms - render) * 44100 / 1000);
  info->setup_latency_min_samples = render * 44100 / 1000;
  info->setup_latency_max_samples =
      static_cast<uint32_t>(info->effective_ms * 44100 / 1000);
  info->setup_applied = state_ == SENDER_READY || state_ == SENDER_STREAMING;
  info->timeline_epoch = timeline_;
  info->next_sample = next_sample_;
  info->first_capture_ns = first_capture_;
  info->capacity_bytes = options_.options.audio_capacity_bytes;
  info->receiver_render_available = receiver_render_ms.has_value();
  info->receiver_render_ms = receiver_render_ms.value_or(0);
  return SENDER_OK;
}
sender_result VirtualReceiver::SetVolume(sender_runtime*, uint64_t generation,
                                         uint64_t command, uint32_t percent,
                                         sender_error* error) const {
  std::lock_guard lock(mutex);
  CheckThread();
  if (generation != generation_ || !command || percent > 100 ||
      pending_volume_ || (state_ != SENDER_READY && state_ != SENDER_STREAMING))
    return Error(error, SENDER_STATE, "Virtual volume request is invalid");
  sender_command_completion completion{};
  completion.size = sizeof(completion);
  completion.version = 1;
  completion.generation = generation;
  completion.command_id = command;
  completion.volume_percent = percent;
  completion.result = SENDER_OK;
  pending_volume_ = completion;
  return SENDER_OK;
}
sender_result VirtualReceiver::SetPresentation(sender_runtime*,
                                               uint64_t generation,
                                               uint64_t command,
                                               sender_presentation const* value,
                                               sender_error* error) const {
  std::lock_guard lock(mutex);
  CheckThread();
  if (generation != generation_ || !command || !value ||
      pending_presentation_ ||
      (state_ != SENDER_READY && state_ != SENDER_STREAMING))
    return Error(error, SENDER_STATE,
                 "Virtual presentation requires a current ready receiver");
  sender_command_completion completion{};
  completion.size = sizeof(completion);
  completion.version = 1;
  completion.generation = generation;
  completion.command_id = command;
  completion.kind = SENDER_COMMAND_PRESENTATION;
  ++presentation_requests;
  completion.presentation = *value;
  pending_presentation_ = completion;
  state_ = SENDER_FLUSHING;
  return SENDER_OK;
}
sender_result VirtualReceiver::RetargetPresentation(
    sender_runtime*, uint64_t generation, sender_presentation const* value,
    sender_error* error) const {
  std::lock_guard lock(mutex);
  CheckThread();
  if (generation != generation_ || !value || pending_presentation_ ||
      (state_ != SENDER_READY && state_ != SENDER_STREAMING) ||
      value->base_ms < SENDER_MIN_PRESENTATION_MS ||
      value->base_ms > SENDER_MAX_PRESENTATION_MS || value->offset_ms < -1000 ||
      value->offset_ms > 1000 ||
      value->base_ms + value->offset_ms < SENDER_MIN_PRESENTATION_MS ||
      value->base_ms + value->offset_ms > SENDER_MAX_PRESENTATION_MS)
    return Error(error, SENDER_STATE,
                 "Virtual timeline retarget requires a current ready receiver");
  if (rejected_retarget_base && *rejected_retarget_base == value->base_ms)
    return Error(error, SENDER_STATE, "Virtual timeline retarget was rejected");
  options_.initial_presentation = *value;
  ++retarget_requests;
  return SENDER_OK;
}
sender_result VirtualReceiver::Snapshot(sender_runtime*, uint64_t generation,
                                        sender_snapshot* value,
                                        sender_error* error) const {
  std::lock_guard lock(mutex);
  CheckThread();
  if (!value || value->size != sizeof(*value) || value->version != 1 ||
      generation != generation_)
    return Error(error, SENDER_INPUT, "Invalid virtual snapshot");
  *value = metrics;
  value->size = sizeof(*value);
  value->version = 1;
  value->generation = generation;
  value->sample_monotonic_ns = MonotonicNs();
  value->available =
      SENDER_SNAPSHOT_SEND | SENDER_SNAPSHOT_RESEND | SENDER_SNAPSHOT_PENDING;
  value->capacity_bytes = options_.options.audio_capacity_bytes;
  std::snprintf(value->clock_reason, sizeof(value->clock_reason),
                "Virtual receiver has no measured remote clock");
  return SENDER_OK;
}
ReceiverFixture::ReceiverFixture() {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data))
    throw std::runtime_error("Fixture Winsock initialization failed");
  listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (listener_ == INVALID_SOCKET ||
      bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
      listen(listener_, 2)) {
    if (listener_ != INVALID_SOCKET) closesocket(listener_);
    WSACleanup();
    throw std::runtime_error("Fixture listener failed");
  }
  int length = sizeof(address);
  if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length)) {
    closesocket(listener_);
    WSACleanup();
    throw std::runtime_error("Fixture address query failed");
  }
  port = ntohs(address.sin_port);
  worker_ = std::thread([this] {
    while (!stopping_) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(listener_, &set);
      timeval wait{0, 50000};
      if (select(0, &set, nullptr, nullptr, &wait) > 0) {
        auto client = accept(listener_, nullptr, nullptr);
        if (client != INVALID_SOCKET) {
          ++connections;
          closesocket(client);
        }
      }
    }
  });
}
ReceiverFixture::~ReceiverFixture() {
  stopping_ = true;
  worker_.join();
  closesocket(listener_);
  WSACleanup();
}
}  // namespace Win2Air::Tests
