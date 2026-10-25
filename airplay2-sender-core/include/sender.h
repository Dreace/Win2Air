#ifndef AIRPLAY2_SENDER_H
#define AIRPLAY2_SENDER_H
#include <stddef.h>
#include <stdint.h>
#define SENDER_MIN_PRESENTATION_MS 20
#define SENDER_MAX_PRESENTATION_MS 5000
#if defined(_WIN32)
#define SENDER_CALL __cdecl
#if defined(SENDER_SHARED_BUILD)
#define SENDER_API __declspec(dllexport)
#elif defined(SENDER_SHARED_IMPORT)
#define SENDER_API __declspec(dllimport)
#else
#define SENDER_API
#endif
#else
#define SENDER_CALL
#define SENDER_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif

typedef struct sender_runtime sender_runtime;
typedef enum {
  SENDER_OK,
  SENDER_INPUT,
  SENDER_OCCUPIED,
  SENDER_STATE,
  SENDER_CAPABILITY,
  SENDER_PAIRING,
  SENDER_AUTHENTICATION,
  SENDER_NETWORK,
  SENDER_CODEC,
  SENDER_SYNC,
  SENDER_TIMEOUT,
  SENDER_PLATFORM,
  SENDER_BACKPRESSURE,
  SENDER_ALREADY_ENDED,
  SENDER_CANCELED
} sender_result;
typedef enum {
  SENDER_IDLE,
  SENDER_CONNECTING,
  SENDER_AUTHENTICATING,
  SENDER_READY,
  SENDER_STREAMING,
  SENDER_FLUSHING,
  SENDER_ENDING,
  SENDER_ENDED,
  SENDER_FAILED
} sender_state;
typedef struct {
  sender_result code;
  sender_state stage;
  uint64_t generation;
  int platform_code;
  int peer_code;
  char operation[48];
  char message[256];
} sender_error;
typedef struct {
  const char *address;
  uint16_t control_port;
  const char *receiver_id;
  const char *name;
  uint64_t features;
  uint32_t scope_id;
  unsigned volume_percent;
} sender_target;
typedef struct {
  unsigned sample_rate, bits_per_sample, channels;
} sender_audio_format;
typedef struct {
  const char *receiver_id;
  uint64_t sender_id;
  const char *material;
} sender_credentials;
typedef void(SENDER_CALL *sender_state_callback)(void *, uint64_t, sender_state,
                                                 const sender_error *);
/* The credential callback receives borrowed material valid until it returns.
 * The caller copies it into secure storage and owns replacement and deletion.
 */
typedef void(SENDER_CALL *sender_credentials_callback)(
    void *, uint64_t, const sender_credentials *);
typedef struct {
  uint64_t sender_id;
  const char *client_name;
  const char *user_agent;
  size_t audio_capacity_bytes;
  sender_state_callback on_state;
  sender_credentials_callback on_credentials;
  void *context;
} sender_options;

#define SENDER_ABI_VERSION 3u
typedef enum { SENDER_STEP_PAIRING, SENDER_STEP_SETTING_UP } sender_step;
typedef void(SENDER_CALL *sender_step_callback)(void *, uint64_t, sender_step);
typedef struct {
  int32_t base_ms, offset_ms;
} sender_presentation;

typedef enum {
  SENDER_COMMAND_VOLUME,
  SENDER_COMMAND_PRESENTATION,
  SENDER_COMMAND_METADATA
} sender_command_kind;

typedef struct {
  uint32_t size, version;
  uint64_t generation, command_id;
  sender_result result;
  uint32_t volume_percent;
  sender_error error;
  sender_command_kind kind;
  sender_presentation presentation;
} sender_command_completion;
/* Completion storage is borrowed until the callback returns. */
typedef void(SENDER_CALL *sender_command_callback)(
    void *, const sender_command_completion *);
typedef struct {
  uint32_t size, version;
  uint64_t generation;
  uint32_t event, state;
  int32_t code, platform_code, peer_code;
} sender_log_record;
/* Fixed numeric events only; no protocol strings or authentication bytes. */
typedef void(SENDER_CALL *sender_log_callback)(void *,
                                               const sender_log_record *);
typedef struct {
  uint32_t size, version;
  uint64_t generation, sample_monotonic_ns, available;
  uint64_t sent_packets, sent_bytes, resend_requests, resend_success,
      resend_expired, send_failures, pending_samples, queue_bytes,
      capacity_bytes;
  char clock_reason[96];
} sender_snapshot;
#define SENDER_SNAPSHOT_SEND 1u
#define SENDER_SNAPSHOT_RESEND 2u
#define SENDER_SNAPSHOT_PENDING 4u
typedef struct {
  uint32_t startup_ms, render_ms, safety_ms, history_ms;
} sender_buffer_config;
/* Zero selects immediate startup, automatic render lead, no safety reserve,
 * or the standard retransmission history, respectively. */
typedef struct {
  uint32_t version, size;
  sender_options options;
  sender_presentation initial_presentation;
  sender_step_callback on_step;
  sender_command_callback on_command;
  sender_log_callback on_log;
  sender_buffer_config buffers;
} sender_create_options;
typedef struct {
  uint32_t size, version;
  uint64_t timeline_epoch, sample_index, capture_monotonic_ns;
} sender_timed_audio;
typedef int(SENDER_CALL *sender_cancel_callback)(void *);
typedef struct {
  uint32_t size, version;
  const char *address;
  uint16_t port;
  uint32_t scope_id;
  uint64_t probe_id;
  uint32_t timeout_ms;
  sender_cancel_callback should_cancel;
  void *context;
} sender_probe_request;
typedef struct {
  uint32_t size, version;
  uint64_t probe_id, features, status_flags;
  uint32_t features_available;
  char receiver_id[18], name[256], model[128], firmware[128];
} sender_probe_info;
typedef struct {
  uint32_t size, version;
  int32_t base_ms, offset_ms, effective_ms;
  uint32_t buffer_samples, setup_latency_min_samples, setup_latency_max_samples,
      setup_applied;
  uint64_t timeline_epoch, first_capture_ns, next_sample, queue_bytes,
      capacity_bytes;
  uint64_t sync_packets, sync_capture_ns;
  uint32_t sync_current_pos, sync_minimum_rtp, sync_next_rtp,
      sync_pending_samples;
  /* Optional stream SETUP response, independent of sender-requested latency.
   * Availability distinguishes an omitted field from a reported zero. */
  uint32_t receiver_render_ms, receiver_render_available;
} sender_timing_info;
typedef struct {
  uint32_t version, size, pointer_bits, entry_count;
  uint32_t layout[192];
} sender_abi_info;
/* The descriptor covers every public field; reserved slots remain zero. */
static inline sender_abi_info sender_header_abi(void) {
  sender_abi_info value = {0};
  value.version = SENDER_ABI_VERSION;
  value.size = (uint32_t)sizeof(value);
  value.pointer_bits = (uint32_t)(sizeof(void *) * 8);
#define SENDER_SIZE(type) \
  value.layout[value.entry_count++] = (uint32_t)sizeof(type)
#define SENDER_OFFSET(type, field) \
  value.layout[value.entry_count++] = (uint32_t)offsetof(type, field)
  SENDER_SIZE(sender_result);
  SENDER_SIZE(sender_state);
  SENDER_SIZE(sender_error);
  SENDER_OFFSET(sender_error, code);
  SENDER_OFFSET(sender_error, stage);
  SENDER_OFFSET(sender_error, generation);
  SENDER_OFFSET(sender_error, platform_code);
  SENDER_OFFSET(sender_error, peer_code);
  SENDER_OFFSET(sender_error, operation);
  SENDER_OFFSET(sender_error, message);
  SENDER_SIZE(sender_target);
  SENDER_OFFSET(sender_target, address);
  SENDER_OFFSET(sender_target, control_port);
  SENDER_OFFSET(sender_target, receiver_id);
  SENDER_OFFSET(sender_target, name);
  SENDER_OFFSET(sender_target, features);
  SENDER_OFFSET(sender_target, scope_id);
  SENDER_OFFSET(sender_target, volume_percent);
  SENDER_SIZE(sender_audio_format);
  SENDER_OFFSET(sender_audio_format, sample_rate);
  SENDER_OFFSET(sender_audio_format, bits_per_sample);
  SENDER_OFFSET(sender_audio_format, channels);
  SENDER_SIZE(sender_credentials);
  SENDER_OFFSET(sender_credentials, receiver_id);
  SENDER_OFFSET(sender_credentials, sender_id);
  SENDER_OFFSET(sender_credentials, material);
  SENDER_SIZE(sender_options);
  SENDER_OFFSET(sender_options, sender_id);
  SENDER_OFFSET(sender_options, client_name);
  SENDER_OFFSET(sender_options, user_agent);
  SENDER_OFFSET(sender_options, audio_capacity_bytes);
  SENDER_OFFSET(sender_options, on_state);
  SENDER_OFFSET(sender_options, on_credentials);
  SENDER_OFFSET(sender_options, context);
  SENDER_SIZE(sender_create_options);
  SENDER_OFFSET(sender_create_options, version);
  SENDER_OFFSET(sender_create_options, size);
  SENDER_OFFSET(sender_create_options, options);
  SENDER_OFFSET(sender_create_options, initial_presentation);
  SENDER_OFFSET(sender_create_options, on_step);
  SENDER_OFFSET(sender_create_options, buffers);
  SENDER_SIZE(sender_buffer_config);
  SENDER_OFFSET(sender_buffer_config, startup_ms);
  SENDER_OFFSET(sender_buffer_config, render_ms);
  SENDER_OFFSET(sender_buffer_config, safety_ms);
  SENDER_OFFSET(sender_buffer_config, history_ms);
  SENDER_SIZE(sender_presentation);
  SENDER_OFFSET(sender_presentation, base_ms);
  SENDER_OFFSET(sender_presentation, offset_ms);
  SENDER_SIZE(sender_timed_audio);
  SENDER_OFFSET(sender_timed_audio, size);
  SENDER_OFFSET(sender_timed_audio, version);
  SENDER_OFFSET(sender_timed_audio, timeline_epoch);
  SENDER_OFFSET(sender_timed_audio, sample_index);
  SENDER_OFFSET(sender_timed_audio, capture_monotonic_ns);
  SENDER_SIZE(sender_step);
  SENDER_SIZE(sender_step_callback);
  SENDER_SIZE(sender_cancel_callback);
  SENDER_SIZE(sender_probe_request);
  SENDER_OFFSET(sender_probe_request, size);
  SENDER_OFFSET(sender_probe_request, version);
  SENDER_OFFSET(sender_probe_request, address);
  SENDER_OFFSET(sender_probe_request, port);
  SENDER_OFFSET(sender_probe_request, scope_id);
  SENDER_OFFSET(sender_probe_request, probe_id);
  SENDER_OFFSET(sender_probe_request, timeout_ms);
  SENDER_OFFSET(sender_probe_request, should_cancel);
  SENDER_OFFSET(sender_probe_request, context);
  SENDER_SIZE(sender_probe_info);
  SENDER_OFFSET(sender_probe_info, size);
  SENDER_OFFSET(sender_probe_info, version);
  SENDER_OFFSET(sender_probe_info, probe_id);
  SENDER_OFFSET(sender_probe_info, features);
  SENDER_OFFSET(sender_probe_info, status_flags);
  SENDER_OFFSET(sender_probe_info, features_available);
  SENDER_OFFSET(sender_probe_info, receiver_id);
  SENDER_OFFSET(sender_probe_info, name);
  SENDER_OFFSET(sender_probe_info, model);
  SENDER_OFFSET(sender_probe_info, firmware);
  SENDER_SIZE(sender_timing_info);
  SENDER_OFFSET(sender_timing_info, size);
  SENDER_OFFSET(sender_timing_info, version);
  SENDER_OFFSET(sender_timing_info, base_ms);
  SENDER_OFFSET(sender_timing_info, offset_ms);
  SENDER_OFFSET(sender_timing_info, effective_ms);
  SENDER_OFFSET(sender_timing_info, buffer_samples);
  SENDER_OFFSET(sender_timing_info, setup_latency_min_samples);
  SENDER_OFFSET(sender_timing_info, setup_latency_max_samples);
  SENDER_OFFSET(sender_timing_info, setup_applied);
  SENDER_OFFSET(sender_timing_info, timeline_epoch);
  SENDER_OFFSET(sender_timing_info, first_capture_ns);
  SENDER_OFFSET(sender_timing_info, next_sample);
  SENDER_OFFSET(sender_timing_info, queue_bytes);
  SENDER_OFFSET(sender_timing_info, capacity_bytes);
  SENDER_SIZE(sender_state_callback);
  SENDER_SIZE(sender_credentials_callback);
  SENDER_OFFSET(sender_timing_info, sync_packets);
  SENDER_OFFSET(sender_timing_info, sync_capture_ns);
  SENDER_OFFSET(sender_timing_info, sync_current_pos);
  SENDER_OFFSET(sender_timing_info, sync_minimum_rtp);
  SENDER_OFFSET(sender_timing_info, sync_next_rtp);
  SENDER_OFFSET(sender_timing_info, sync_pending_samples);
  SENDER_OFFSET(sender_timing_info, receiver_render_ms);
  SENDER_OFFSET(sender_timing_info, receiver_render_available);
  SENDER_OFFSET(sender_create_options, on_command);
  SENDER_SIZE(sender_command_callback);
  SENDER_SIZE(sender_command_completion);
  SENDER_OFFSET(sender_command_completion, size);
  SENDER_OFFSET(sender_command_completion, version);
  SENDER_OFFSET(sender_command_completion, generation);
  SENDER_OFFSET(sender_command_completion, command_id);
  SENDER_OFFSET(sender_command_completion, result);
  SENDER_OFFSET(sender_command_completion, volume_percent);
  SENDER_OFFSET(sender_command_completion, error);
  SENDER_OFFSET(sender_command_completion, kind);
  SENDER_OFFSET(sender_command_completion, presentation);
  SENDER_SIZE(sender_command_kind);
  SENDER_OFFSET(sender_create_options, on_log);
  SENDER_SIZE(sender_log_callback);
  SENDER_SIZE(sender_log_record);
  SENDER_OFFSET(sender_log_record, size);
  SENDER_OFFSET(sender_log_record, version);
  SENDER_OFFSET(sender_log_record, generation);
  SENDER_OFFSET(sender_log_record, event);
  SENDER_OFFSET(sender_log_record, state);
  SENDER_OFFSET(sender_log_record, code);
  SENDER_OFFSET(sender_log_record, platform_code);
  SENDER_OFFSET(sender_log_record, peer_code);
  SENDER_SIZE(sender_snapshot);
  SENDER_OFFSET(sender_snapshot, size);
  SENDER_OFFSET(sender_snapshot, version);
  SENDER_OFFSET(sender_snapshot, generation);
  SENDER_OFFSET(sender_snapshot, sample_monotonic_ns);
  SENDER_OFFSET(sender_snapshot, available);
  SENDER_OFFSET(sender_snapshot, sent_packets);
  SENDER_OFFSET(sender_snapshot, sent_bytes);
  SENDER_OFFSET(sender_snapshot, resend_requests);
  SENDER_OFFSET(sender_snapshot, resend_success);
  SENDER_OFFSET(sender_snapshot, resend_expired);
  SENDER_OFFSET(sender_snapshot, send_failures);
  SENDER_OFFSET(sender_snapshot, pending_samples);
  SENDER_OFFSET(sender_snapshot, queue_bytes);
  SENDER_OFFSET(sender_snapshot, capacity_bytes);
  SENDER_OFFSET(sender_snapshot, clock_reason);
#undef SENDER_SIZE
#undef SENDER_OFFSET
  return value;
}
SENDER_API sender_result SENDER_CALL sender_get_abi(uint32_t version,
                                                    uint32_t size,
                                                    sender_abi_info *);
SENDER_API sender_result SENDER_CALL sender_create_ex(
    const sender_create_options *, sender_runtime **, sender_error *);
SENDER_API sender_result SENDER_CALL sender_validate_target(
    const sender_target *, const sender_audio_format *, sender_error *);
SENDER_API sender_result SENDER_CALL sender_create(const sender_options *,
                                                   sender_runtime **,
                                                   sender_error *);
/* All accepted operations are queued. Drive them and network events with poll.
 * Call poll from one execution context per runtime; callbacks may submit audio,
 * flush or end. Start a new connection after the previous poll returns. */
SENDER_API sender_result SENDER_CALL sender_connect(sender_runtime *,
                                                    const sender_target *,
                                                    const sender_audio_format *,
                                                    const sender_credentials *,
                                                    const char *pairing_code,
                                                    uint64_t *, sender_error *);
SENDER_API sender_result SENDER_CALL sender_submit(sender_runtime *, uint64_t,
                                                   const sender_audio_format *,
                                                   const void *, size_t,
                                                   sender_error *);
SENDER_API sender_result SENDER_CALL sender_submit_timed(
    sender_runtime *, uint64_t, const sender_timed_audio *,
    const sender_audio_format *, const void *, size_t, sender_error *);
SENDER_API sender_result SENDER_CALL
sender_probe_target(sender_runtime *, const sender_probe_request *,
                    sender_probe_info *, sender_error *);
SENDER_API sender_result SENDER_CALL sender_get_timing(sender_runtime *,
                                                       uint64_t,
                                                       sender_timing_info *,
                                                       sender_error *);
SENDER_API sender_result SENDER_CALL sender_get_snapshot(sender_runtime *,
                                                         uint64_t,
                                                         sender_snapshot *,
                                                         sender_error *);
SENDER_API sender_result SENDER_CALL sender_flush(sender_runtime *, uint64_t,
                                                  sender_error *);
/* A successful return queues the command; on_command reports the RTSP result.
 * Command identities must increase within each connection generation. */
SENDER_API sender_result SENDER_CALL sender_set_volume(sender_runtime *,
                                                       uint64_t generation,
                                                       uint64_t command_id,
                                                       uint32_t volume_percent,
                                                       sender_error *);
SENDER_API sender_result SENDER_CALL sender_set_presentation(
    sender_runtime *, uint64_t generation, uint64_t command_id,
    const sender_presentation *, sender_error *);
/* Observations are receiver-reported; revision zero means unobserved.
 * Reading also schedules a nonblocking volume query on supported receivers. */
SENDER_API sender_result SENDER_CALL sender_get_receiver_volume(
    sender_runtime *, uint64_t generation, uint32_t *percent,
    uint64_t *revision, sender_error *);
/* Consumes one receiver command: 0 empty, 1 pause, 2 play, 3 next, 4 previous. */
SENDER_API sender_result SENDER_CALL sender_take_media_command(
    sender_runtime *, uint64_t generation, uint32_t *command, sender_error *);
/* UTF-8 text and optional JPEG/PNG bytes are copied before returning.
 * BACKPRESSURE leaves the current metadata untouched; retry the latest item.
 * Completion uses SENDER_COMMAND_METADATA with command_id zero. */
SENDER_API sender_result SENDER_CALL sender_set_now_playing(
    sender_runtime *, uint64_t generation, const char *title,
    const char *artist, const char *album, const char *artwork_type,
    const void *artwork, size_t artwork_size, sender_error *);
/* Moves the sender timeline to a new lead. Does not FLUSH or SETUP, and does
 * not change the latencyMin negotiated for the current stream. */
SENDER_API sender_result SENDER_CALL sender_retarget_presentation(
    sender_runtime *, uint64_t generation, const sender_presentation *,
    sender_error *);
SENDER_API sender_result SENDER_CALL sender_end(sender_runtime *, uint64_t,
                                                sender_error *);
SENDER_API sender_result SENDER_CALL sender_poll(sender_runtime *,
                                                 unsigned timeout_ms,
                                                 sender_error *);
SENDER_API sender_state SENDER_CALL sender_get_state(sender_runtime *,
                                                     uint64_t);
SENDER_API sender_result SENDER_CALL sender_destroy(sender_runtime *,
                                                    sender_error *);
#ifdef __cplusplus
}
#endif
#endif
