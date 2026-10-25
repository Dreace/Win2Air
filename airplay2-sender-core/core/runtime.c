#include <ctype.h>
#include <errno.h>
#include <gcrypt.h>
#include <math.h>
#include <strings.h>

#include "protocol.h"
#include "target_probe.h"

enum command_type {
  COMMAND_CONNECT,
  COMMAND_AUDIO,
  COMMAND_FLUSH,
  COMMAND_END,
  COMMAND_VOLUME,
  COMMAND_PRESENTATION
};
struct command {
  enum command_type type;
  uint64_t generation;
  size_t bytes;
  struct command *next;
  uint64_t capture_ns;
  uint64_t command_id;
  uint32_t volume_percent;
  sender_presentation presentation;
  unsigned char pcm[];
};
struct notification {
  uint64_t generation;
  sender_state state;
  sender_error error;
  char *material;
  struct notification *next;
  bool is_step;
  sender_step step;
  bool is_command;
  sender_command_completion completion;
};
struct sender_runtime {
  sender_options options;
  struct event_base *base;
  struct protocol_peer peer;
  char receiver_id[18];
  uint64_t generation;
  sender_state state;
  struct command *head, *tail;
  struct notification *notifications, *notification_tail;
  struct notification emergency;
  bool emergency_queued;
  size_t queued_bytes;
  bool polling, callback, protocol_initialized;
  sender_error error;
  struct timespec audio_time;
  uint64_t samples;
  sender_step_callback on_step;
  sender_command_callback on_command;
  sender_log_callback on_log;
  sender_snapshot metrics;
  uint64_t last_command_id, volume_command_id;
  uint64_t volume_deadline_ns;
  uint32_t pending_volume;
  uint32_t receiver_volume;
  uint64_t receiver_volume_revision;
  bool receiver_volume_valid;
  uint32_t media_commands[16];
  unsigned media_head, media_count;
  uint64_t presentation_command_id, presentation_deadline_ns;
  sender_presentation pending_presentation;
  sender_presentation presentation;
  bool timed_enabled, timeline_valid;
  sender_buffer_config buffers;
  uint64_t timeline_epoch, first_capture_ns, next_sample;
  uint32_t setup_min, setup_max, buffer_samples;
  bool setup_applied;
  uint64_t sync_packets, sync_capture_ns;
  uint32_t sync_current_pos, sync_minimum_rtp, sync_next_rtp,
      sync_pending_samples;
};

static platform_mutex gate = PLATFORM_MUTEX_INIT;
static platform_mutex protocol_gate = PLATFORM_MUTEX_INIT;
static platform_once crypto_once = PLATFORM_ONCE_INIT;
static sender_runtime *owner;
static _Thread_local sender_runtime *executing;
static uint64_t next_generation;
static int crypto_status;

static void crypto_init(void) {
  crypto_status = sodium_init();
  if (!gcry_check_version(GCRYPT_VERSION)) crypto_status = -1;
  if (!gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P)) {
    gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
    gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
  }
}

static sender_result reject(sender_runtime *runtime, sender_error *error,
                            sender_result code, const char *operation,
                            const char *reason) {
  if (error) {
    platform_mutex_lock(&gate);
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->generation = runtime ? runtime->generation : 0;
    error->stage = runtime ? runtime->state : SENDER_IDLE;
    snprintf(error->operation, sizeof(error->operation), "%s", operation);
    snprintf(error->message, sizeof(error->message), "%s", reason);
    platform_mutex_unlock(&gate);
  }
  return code;
}

static bool active(sender_state state) {
  return state >= SENDER_CONNECTING && state <= SENDER_ENDING;
}

static bool enqueue(sender_runtime *runtime, enum command_type type,
                    const void *pcm, size_t bytes) {
  struct command *command = calloc(1, sizeof(*command) + bytes);
  if (!command) return false;
  command->type = type;
  command->generation = runtime->generation;
  command->bytes = bytes;
  if (bytes) memcpy(command->pcm, pcm, bytes);
  if (runtime->tail)
    runtime->tail->next = command;
  else
    runtime->head = command;
  runtime->tail = command;
  runtime->queued_bytes += bytes;
  return true;
}

static void requeue_front(sender_runtime *runtime, struct command *command) {
  command->next = runtime->head;
  runtime->head = command;
  if (!runtime->tail) runtime->tail = command;
  runtime->queued_bytes += command->bytes;
}

static void notify(sender_runtime *runtime, sender_state state,
                   const char *material) {
  struct notification *notification = calloc(1, sizeof(*notification));
  bool appended = true;
  if (!notification) {
    notification = &runtime->emergency;
    appended = !runtime->emergency_queued;
    runtime->emergency_queued = true;
    secure_string_free(notification->material);
    notification->material = NULL;
    runtime->error.code = SENDER_PLATFORM;
    runtime->error.generation = runtime->generation;
    runtime->error.stage = state;
    snprintf(runtime->error.operation, sizeof(runtime->error.operation),
             "allocate-notification");
    snprintf(runtime->error.message, sizeof(runtime->error.message),
             "Notification allocation failed; the session is released");
    runtime->state = state = SENDER_FAILED;
    material = NULL;
  }
  notification->generation = runtime->generation;
  notification->state = state;
  notification->error = runtime->error;
  if (material) notification->material = strdup(material);
  if (material && !notification->material) {
    notification->error.code = SENDER_PLATFORM;
    snprintf(notification->error.operation,
             sizeof(notification->error.operation), "copy-material");
    snprintf(
        notification->error.message, sizeof(notification->error.message),
        "Pairing material notification copy failed; the session is released");
    runtime->error = notification->error;
    runtime->state = notification->state = SENDER_FAILED;
  }
  if (!appended) return;
  if (runtime->notification_tail)
    runtime->notification_tail->next = notification;
  else
    runtime->notifications = notification;
  runtime->notification_tail = notification;
}

static void deliver(sender_runtime *runtime) {
  for (;;) {
    platform_mutex_lock(&gate);
    struct notification *notification = runtime->notifications;
    if (!notification) {
      platform_mutex_unlock(&gate);
      break;
    }
    if (notification->state == SENDER_FAILED && owner == runtime) {
      platform_mutex_unlock(&gate);
      break;
    }
    runtime->notifications = notification->next;
    if (!runtime->notifications) runtime->notification_tail = NULL;
    struct notification delivered = *notification;
    if (notification == &runtime->emergency) {
      memset(&runtime->emergency, 0, sizeof(runtime->emergency));
      runtime->emergency_queued = false;
    } else
      free(notification);
    notification = &delivered;
    runtime->callback = true;
    platform_mutex_unlock(&gate);
    if (notification->is_command && runtime->on_command) {
      runtime->on_command(runtime->options.context, &notification->completion);
    } else if (notification->is_step && runtime->on_step) {
      runtime->on_step(runtime->options.context, notification->generation,
                       notification->step);
    } else if (notification->material && runtime->options.on_credentials) {
      sender_credentials credentials = {runtime->receiver_id,
                                        runtime->options.sender_id,
                                        notification->material};
      runtime->options.on_credentials(runtime->options.context,
                                      notification->generation, &credentials);
    } else if (!notification->material && runtime->options.on_state)
      runtime->options.on_state(
          runtime->options.context, notification->generation,
          notification->state,
          notification->error.code ? &notification->error : NULL);
    if (runtime->on_log && !notification->material) {
      sender_log_record record;
      memset(&record, 0, sizeof(record));
      record.size = sizeof(record);
      record.version = 1;
      record.generation = notification->generation;
      record.event = notification->is_step      ? 1u
                     : notification->is_command ? 2u
                                                : 0u;
      record.state = (uint32_t)notification->state;
      record.code = notification->is_command ? notification->completion.result
                                             : notification->error.code;
      record.platform_code = notification->is_command
                                 ? notification->completion.error.platform_code
                                 : notification->error.platform_code;
      record.peer_code = notification->is_command
                             ? notification->completion.error.peer_code
                             : notification->error.peer_code;
      runtime->on_log(runtime->options.context, &record);
    }
    secure_string_free(notification->material);
    platform_mutex_lock(&gate);
    runtime->callback = false;
    platform_mutex_unlock(&gate);
  }
}

static void peer_clear(struct protocol_peer *peer) {
  free(peer->name);
  free(peer->v4_address);
  free(peer->v6_address);
  secure_string_free(peer->password);
  secure_string_free(peer->auth_key);
  memset(peer, 0, sizeof(*peer));
}

/* Called with gate held; completion is copied before invoking caller code. */
static void volume_notify(sender_runtime *runtime, uint64_t command_id,
                          uint32_t volume, sender_result result, int peer_code,
                          int platform_code) {
  notify(runtime, runtime->state, NULL);
  struct notification *item = runtime->notification_tail;
  if (item == &runtime->emergency) return;
  item->is_command = true;
  item->completion.size = sizeof(item->completion);
  item->completion.version = 1;
  item->completion.generation = runtime->generation;
  item->completion.command_id = command_id;
  item->completion.volume_percent = volume;
  item->completion.result = result;
  item->completion.kind = SENDER_COMMAND_VOLUME;
  if (result != SENDER_OK) {
    sender_error *error = &item->completion.error;
    error->code = result;
    error->generation = runtime->generation;
    error->stage = runtime->state;
    error->peer_code = peer_code;
    error->platform_code = platform_code;
    snprintf(error->operation, sizeof(error->operation), "set-volume");
    snprintf(error->message, sizeof(error->message),
             "Volume command did not apply (result=%d, peer=%d, platform=%d)",
             result, peer_code, platform_code);
  }
}

static void presentation_notify(sender_runtime *runtime, uint64_t command_id,
                                sender_result result, int peer_code,
                                int platform_code) {
  volume_notify(runtime, command_id, 0, result, peer_code, platform_code);
  struct notification *item = runtime->notification_tail;
  if (item == &runtime->emergency) return;
  item->completion.kind = SENDER_COMMAND_PRESENTATION;
  item->completion.presentation = runtime->presentation;
  if (result != SENDER_OK) {
    snprintf(item->completion.error.operation,
             sizeof(item->completion.error.operation), "set-presentation");
    snprintf(item->completion.error.message,
             sizeof(item->completion.error.message),
             "Presentation did not apply (result=%d, peer=%d, platform=%d)",
             result, peer_code, platform_code);
  }
}
static void finish(sender_runtime *runtime, sender_state state) {
  if (runtime->protocol_initialized) {
    protocol_cleanup();
    runtime->protocol_initialized = false;
  }
  platform_mutex_lock(&gate);
  runtime->state = state;
  if (runtime->presentation_command_id) {
    presentation_notify(runtime, runtime->presentation_command_id,
                        SENDER_CANCELED, 0, 0);
    runtime->presentation_command_id = 0;
  }
  if (runtime->volume_command_id) {
    volume_notify(runtime, runtime->volume_command_id, runtime->pending_volume,
                  SENDER_CANCELED, 0, 0);
    runtime->volume_command_id = 0;
  }
  while (runtime->head) {
    struct command *pending = runtime->head;
    runtime->head = pending->next;
    if (pending->type == COMMAND_VOLUME)
      volume_notify(runtime, pending->command_id, pending->volume_percent,
                    SENDER_CANCELED, 0, 0);
    if (pending->type == COMMAND_PRESENTATION)
      presentation_notify(runtime, pending->command_id, SENDER_CANCELED, 0, 0);
    free(pending);
  }
  runtime->tail = NULL;
  runtime->queued_bytes = 0;
  if (owner == runtime) owner = NULL;
  runtime->metrics.pending_samples = 0;
  peer_clear(&runtime->peer);
  notify(runtime, state, NULL);
  platform_mutex_unlock(&gate);
}

sender_result sender_get_abi(uint32_t version, uint32_t size,
                             sender_abi_info *output) {
  if (!output || version != SENDER_ABI_VERSION || size != sizeof(*output))
    return SENDER_INPUT;
  *output = sender_header_abi();
  return SENDER_OK;
}

sender_result sender_create_ex(const sender_create_options *options,
                               sender_runtime **output, sender_error *error) {
  if (output) *output = NULL;
  if (!options || options->version != SENDER_ABI_VERSION ||
      options->size != sizeof(*options))
    return reject(NULL, error, SENDER_INPUT, "create-ex",
                  "Create options version or size is unsupported");
  int64_t effective = (int64_t)options->initial_presentation.base_ms +
                      options->initial_presentation.offset_ms;
  if (options->initial_presentation.base_ms < SENDER_MIN_PRESENTATION_MS ||
      options->initial_presentation.base_ms > SENDER_MAX_PRESENTATION_MS ||
      options->initial_presentation.offset_ms < -1000 ||
      options->initial_presentation.offset_ms > 1000 ||
      effective < SENDER_MIN_PRESENTATION_MS ||
      effective > SENDER_MAX_PRESENTATION_MS)
    return reject(NULL, error, SENDER_INPUT, "create-ex",
                  "Initial base, offset and effective presentation are outside "
                  "their bounds");
  uint32_t render = options->buffers.render_ms ? options->buffers.render_ms
                                              : (uint32_t)MIN(MIN(effective, options->initial_presentation.base_ms), 250);
  if (render > (uint64_t)effective ||
      render > (uint32_t)options->initial_presentation.base_ms ||
      options->buffers.safety_ms > (uint64_t)effective - render ||
      options->buffers.startup_ms >
          (uint64_t)effective - render - options->buffers.safety_ms ||
      options->buffers.history_ms > 60000)
    return reject(NULL, error, SENDER_INPUT, "create-ex",
                  "Render, startup and safety must fit presentation; "
                  "history must be within 60000 ms");
  sender_options bounded = options->options;
  size_t limit = (size_t)(effective + 250) * 44100 / 1000 * 4;
  if (bounded.audio_capacity_bytes > limit)
    bounded.audio_capacity_bytes = limit;
  sender_result result = sender_create(&bounded, output, error);
  if (result == SENDER_OK) {
    (*output)->timed_enabled = true;
    (*output)->presentation = options->initial_presentation;
    (*output)->buffers = options->buffers;
    (*output)->on_step = options->on_step;
    (*output)->on_command = options->on_command;
    (*output)->on_log = options->on_log;
  }
  return result;
}

sender_result sender_create(const sender_options *options,
                            sender_runtime **output, sender_error *error) {
  if (!options || !output || !options->sender_id || !options->client_name ||
      !options->client_name[0] || !options->user_agent ||
      !options->user_agent[0] || options->audio_capacity_bytes < 1408 ||
      options->audio_capacity_bytes > 64 * 1024 * 1024 ||
      options->audio_capacity_bytes % 4)
    return reject(NULL, error, SENDER_INPUT, "create",
                  "Sender identity, client name, user agent and frame-aligned "
                  "audio capacity are required");
  *output = NULL;
  platform_call_once(&crypto_once, crypto_init);
  if (crypto_status < 0)
    return reject(NULL, error, SENDER_PLATFORM, "create",
                  "Cryptographic backend initialization failed");
  if (platform_init() < 0) {
    sender_result result = reject(NULL, error, SENDER_PLATFORM, "create",
                                  "Platform backend initialization failed");
    if (error) error->platform_code = platform_last_failure()->code;
    return result;
  }
  sender_runtime *runtime = calloc(1, sizeof(*runtime));
  if (!runtime) {
    platform_cleanup();
    return reject(NULL, error, SENDER_PLATFORM, "create",
                  "Runtime allocation failed");
  }
  runtime->options = *options;
  runtime->options.client_name = strdup(options->client_name);
  runtime->options.user_agent = strdup(options->user_agent);
  runtime->base = event_base_new();
  if (!runtime->base || !runtime->options.client_name ||
      !runtime->options.user_agent) {
    if (runtime->base) event_base_free(runtime->base);
    free((void *)runtime->options.client_name);
    free((void *)runtime->options.user_agent);
    free(runtime);
    platform_cleanup();
    return reject(NULL, error, SENDER_PLATFORM, "create",
                  "Event base or runtime option allocation failed");
  }
  *output = runtime;
  return SENDER_OK;
}

sender_result sender_connect(sender_runtime *runtime,
                             const sender_target *target,
                             const sender_audio_format *format,
                             const sender_credentials *credentials,
                             const char *pairing_code, uint64_t *generation,
                             sender_error *error) {
  sender_result result = sender_validate_target(target, format, error);
  if (result != SENDER_OK) return result;
  if (!runtime || !generation)
    return reject(runtime, error, SENDER_INPUT, "connect",
                  "Runtime and generation output are required");
  if (credentials &&
      (!credentials->receiver_id || !credentials->material ||
       !credentials->material[0] ||
       strcasecmp(credentials->receiver_id, target->receiver_id) ||
       credentials->sender_id != runtime->options.sender_id))
    return reject(
        runtime, error, SENDER_AUTHENTICATION, "connect",
        "Credential receiver and sender identities must match this connection");
  if (credentials) {
    if (strlen(credentials->material) != 192)
      return reject(runtime, error, SENDER_AUTHENTICATION, "connect",
                    "Credential material must include the original private "
                    "identity and receiver public key");
    for (size_t i = 0; i < 192; ++i)
      if (!isxdigit((unsigned char)credentials->material[i]))
        return reject(
            runtime, error, SENDER_AUTHENTICATION, "connect",
            "Credential material contains an invalid hexadecimal byte");
  }
  if (pairing_code && (!pairing_code[0] || strlen(pairing_code) > 128))
    return reject(runtime, error, SENDER_INPUT, "connect",
                  "Pairing code must have a bounded nonempty value");
  platform_mutex_lock(&gate);
  if (owner || active(runtime->state) || runtime->polling || runtime->head ||
      runtime->notifications) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_OCCUPIED, "connect",
                  "An active session or pending runtime operation must be "
                  "released first");
  }
  struct protocol_peer *peer = &runtime->peer;
  peer->name = strdup(target->name);
  char scoped[INET6_ADDRSTRLEN + 257];
  if (strchr(target->address, ':')) {
    char interface_name[256];
    if (target->scope_id &&
        platform_interface_name(target->scope_id, interface_name,
                                sizeof(interface_name)) < 0) {
      int code = platform_last_failure()->code;
      char message[256];
      snprintf(message, sizeof(message),
               "IPv6 scope %u does not identify an available interface "
               "(platform=%d)",
               target->scope_id, code);
      peer_clear(peer);
      platform_mutex_unlock(&gate);
      result = reject(runtime, error, SENDER_INPUT, "connect", message);
      if (error) error->platform_code = code;
      return result;
    }
    if (target->scope_id)
      snprintf(scoped, sizeof(scoped), "%s%%%u", target->address,
               target->scope_id);
    else
      snprintf(scoped, sizeof(scoped), "%s", target->address);
    peer->v6_address = strdup(scoped);
    peer->v6_port = target->control_port;
  } else {
    peer->v4_address = strdup(target->address);
    peer->v4_port = target->control_port;
  }
  peer->quality = (struct media_quality){44100, 16, 2, 0};
  peer->volume = target->volume_percent;
  peer->features = target->features;
  peer->presentation_base_ms =
      runtime->timed_enabled ? runtime->presentation.base_ms : 0;
  peer->offset_ms =
      runtime->timed_enabled ? runtime->presentation.offset_ms : 0;
  peer->buffers = runtime->buffers;
  char identity[13];
  size_t position = 0;
  for (size_t i = 0; i < 17; ++i)
    if (target->receiver_id[i] != ':')
      identity[position++] = target->receiver_id[i];
  identity[position] = 0;
  peer->id = strtoull(identity, NULL, 16);
  snprintf(runtime->receiver_id, sizeof(runtime->receiver_id), "%s",
           target->receiver_id);
  if (credentials) peer->auth_key = strdup(credentials->material);
  if (pairing_code) peer->password = strdup(pairing_code);
  if (!peer->name || (!peer->v4_address && !peer->v6_address) ||
      (credentials && !peer->auth_key) || (pairing_code && !peer->password) ||
      next_generation == UINT64_MAX) {
    peer_clear(peer);
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_PLATFORM, "connect",
                  "Target copy allocation or generation allocation failed");
  }
  runtime->generation = ++next_generation;
  runtime->receiver_volume_revision = 0;
  runtime->receiver_volume = 0;
  runtime->receiver_volume_valid = false;
  runtime->media_head = runtime->media_count = 0;
  runtime->last_command_id = 0;
  memset(&runtime->metrics, 0, sizeof(runtime->metrics));
  runtime->state = SENDER_CONNECTING;
  memset(&runtime->error, 0, sizeof(runtime->error));
  runtime->samples = 0;
  runtime->timeline_epoch = 0;
  runtime->timeline_valid = false;
  runtime->first_capture_ns = 0;
  runtime->next_sample = 0;
  runtime->setup_applied = false;
  runtime->sync_packets = 0;
  runtime->sync_capture_ns = 0;
  runtime->sync_current_pos = 0;
  runtime->sync_minimum_rtp = 0;
  runtime->sync_next_rtp = 0;
  runtime->sync_pending_samples = 0;
  if (!enqueue(runtime, COMMAND_CONNECT, NULL, 0)) {
    runtime->state = SENDER_IDLE;
    peer_clear(peer);
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_PLATFORM, "connect",
                  "Connection command allocation failed");
  }
  owner = runtime;
  *generation = runtime->generation;
  notify(runtime, runtime->state, NULL);
  platform_mutex_unlock(&gate);
  return SENDER_OK;
}

sender_result sender_submit(sender_runtime *runtime, uint64_t generation,
                            const sender_audio_format *format, const void *pcm,
                            size_t bytes, sender_error *error) {
  if (runtime && runtime->timed_enabled)
    return reject(runtime, error, SENDER_INPUT, "submit",
                  "Extended sessions require capture-timed audio");
  if (!runtime || !format || !pcm || !bytes || bytes % 4 ||
      bytes > SIZE_MAX - sizeof(struct command) ||
      format->sample_rate != 44100 || format->bits_per_sample != 16 ||
      format->channels != 2)
    return reject(runtime, error, SENDER_INPUT, "submit",
                  "Audio requires complete interleaved PCM frames in the "
                  "declared format");
  if (bytes > runtime->options.audio_capacity_bytes)
    return reject(runtime, error, SENDER_INPUT, "submit",
                  "Audio block exceeds the declared runtime capacity");
  platform_mutex_lock(&gate);
  if (generation != runtime->generation ||
      (runtime->state != SENDER_READY && runtime->state != SENDER_STREAMING)) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "submit",
                  "Audio requires the active ready or streaming generation");
  }
  if (bytes > runtime->options.audio_capacity_bytes - runtime->queued_bytes) {
    platform_mutex_unlock(&gate);
    return reject(
        runtime, error, SENDER_BACKPRESSURE, "submit",
        "Audio queue has insufficient capacity; no frames were accepted");
  }
  bool accepted = enqueue(runtime, COMMAND_AUDIO, pcm, bytes);
  platform_mutex_unlock(&gate);
  return accepted
             ? SENDER_OK
             : reject(
                   runtime, error, SENDER_PLATFORM, "submit",
                   "Audio command allocation failed; no frames were accepted");
}

sender_result sender_submit_timed(sender_runtime *runtime, uint64_t generation,
                                  const sender_timed_audio *audio,
                                  const sender_audio_format *format,
                                  const void *pcm, size_t bytes,
                                  sender_error *error) {
  if (!runtime || !runtime->timed_enabled || !audio || audio->version != 1 ||
      audio->size != sizeof(*audio) || !audio->timeline_epoch || !format ||
      !pcm || !bytes || bytes % 4 ||
      bytes > SIZE_MAX - sizeof(struct command) ||
      format->sample_rate != 44100 || format->bits_per_sample != 16 ||
      format->channels != 2 || bytes > runtime->options.audio_capacity_bytes)
    return reject(
        runtime, error, SENDER_INPUT, "submit-timed",
        "Timed input requires versioned complete fixed-format PCM frames");
  struct timespec now;
  if (platform_time(PLATFORM_MONOTONIC, &now) < 0)
    return reject(runtime, error, SENDER_PLATFORM, "submit-timed",
                  "Read capture clock failed");
  uint64_t current = (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
  uint64_t budget = (uint64_t)(runtime->presentation.base_ms +
                               runtime->presentation.offset_ms - runtime->buffers.safety_ms) *
                    1000000;
  if (audio->capture_monotonic_ns > current + 5000000 ||
      (current > audio->capture_monotonic_ns &&
       current - audio->capture_monotonic_ns > budget))
    return reject(runtime, error, SENDER_SYNC, "submit-timed",
                  "Capture timestamp is future or older than the effective "
                  "presentation budget");
  platform_mutex_lock(&gate);
  if (generation != runtime->generation ||
      (runtime->state != SENDER_READY && runtime->state != SENDER_STREAMING)) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "submit-timed",
                  "Audio belongs to a stale or unready session");
  }
  if ((runtime->timeline_valid &&
       (audio->timeline_epoch != runtime->timeline_epoch ||
        audio->sample_index != runtime->next_sample)) ||
      (!runtime->timeline_valid &&
       (audio->timeline_epoch <= runtime->timeline_epoch ||
        audio->sample_index != 0)) ||
      audio->sample_index > UINT64_MAX - bytes / 4) {
    platform_mutex_unlock(&gate);
    return reject(
        runtime, error, SENDER_SYNC, "submit-timed",
        "Sample order or source timeline requires a new FLUSH anchor");
  }
  if (runtime->timeline_valid) {
    if (audio->sample_index / 44100 >
        (UINT64_MAX - runtime->first_capture_ns) / 1000000000) {
      platform_mutex_unlock(&gate);
      return reject(runtime, error, SENDER_SYNC, "submit-timed",
                    "Sample clock overflows");
    }
    uint64_t whole =
        runtime->first_capture_ns + audio->sample_index / 44100 * 1000000000;
    uint64_t fraction = (audio->sample_index % 44100) * 1000000000 / 44100;
    if (fraction > UINT64_MAX - whole) {
      platform_mutex_unlock(&gate);
      return reject(runtime, error, SENDER_SYNC, "submit-timed",
                    "Sample fraction overflows");
    }
    uint64_t expected = whole + fraction;
    uint64_t drift = expected > audio->capture_monotonic_ns
                         ? expected - audio->capture_monotonic_ns
                         : audio->capture_monotonic_ns - expected;
    if (drift > 20000000) {
      platform_mutex_unlock(&gate);
      return reject(runtime, error, SENDER_SYNC, "submit-timed",
                    "Source clock discontinuity requires FLUSH and reanchor");
    }
  }
  if (bytes > runtime->options.audio_capacity_bytes - runtime->queued_bytes) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_BACKPRESSURE, "submit-timed",
                  "Queue full; zero frames accepted");
  }
  if (!enqueue(runtime, COMMAND_AUDIO, pcm, bytes)) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_PLATFORM, "submit-timed",
                  "Audio allocation failed; zero frames accepted");
  }
  runtime->tail->capture_ns = audio->capture_monotonic_ns;
  if (!runtime->timeline_valid)
    runtime->first_capture_ns = audio->capture_monotonic_ns;
  runtime->timeline_valid = true;
  runtime->timeline_epoch = audio->timeline_epoch;
  runtime->next_sample = audio->sample_index + bytes / 4;
  platform_mutex_unlock(&gate);
  return SENDER_OK;
}

sender_result sender_get_timing(sender_runtime *runtime, uint64_t generation,
                                sender_timing_info *info, sender_error *error) {
  if (!runtime || !info || info->size != sizeof(*info) || info->version != 1)
    return reject(runtime, error, SENDER_INPUT, "get-timing",
                  "Versioned caller-owned timing output required");
  platform_mutex_lock(&gate);
  if (generation != runtime->generation || !generation || runtime->polling) {
    platform_mutex_unlock(&gate);
    return reject(
        runtime, error, SENDER_STATE, "get-timing",
        "Read timing from the active serial context after poll returns");
  }
  memset(info, 0, sizeof(*info));
  info->size = sizeof(*info);
  info->version = 1;
  info->base_ms = runtime->timed_enabled ? runtime->presentation.base_ms : 2000;
  info->offset_ms = runtime->presentation.offset_ms;
  info->effective_ms = info->base_ms + info->offset_ms;
  info->buffer_samples = runtime->buffer_samples;
  info->setup_latency_min_samples = runtime->setup_min;
  info->setup_latency_max_samples = runtime->setup_max;
  info->setup_applied = runtime->setup_applied;
  info->timeline_epoch = runtime->timeline_epoch;
  info->first_capture_ns = runtime->first_capture_ns;
  info->next_sample = runtime->next_sample;
  info->queue_bytes = runtime->queued_bytes;
  info->capacity_bytes = runtime->options.audio_capacity_bytes;
  info->sync_packets = runtime->sync_packets;
  info->sync_capture_ns = runtime->sync_capture_ns;
  info->sync_current_pos = runtime->sync_current_pos;
  info->sync_minimum_rtp = runtime->sync_minimum_rtp;
  info->sync_next_rtp = runtime->sync_next_rtp;
  info->sync_pending_samples = runtime->sync_pending_samples;
  info->receiver_render_ms = runtime->peer.receiver_render_ms;
  info->receiver_render_available = runtime->peer.receiver_render_available;
  platform_mutex_unlock(&gate);
  return SENDER_OK;
}

static sender_result control(sender_runtime *runtime, uint64_t generation,
                             enum command_type type, sender_error *error) {
  if (!runtime)
    return reject(NULL, error, SENDER_INPUT, "control", "Runtime is required");
  platform_mutex_lock(&gate);
  if (generation != runtime->generation || !generation) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "control",
                  "Operation belongs to a stale or invalid generation");
  }
  if (type == COMMAND_END && runtime->state == SENDER_ENDING) {
    platform_mutex_unlock(&gate);
    return SENDER_OK;
  }
  if (type == COMMAND_END && !active(runtime->state)) {
    platform_mutex_unlock(&gate);
    return SENDER_ALREADY_ENDED;
  }
  if (type == COMMAND_FLUSH && runtime->state != SENDER_READY &&
      runtime->state != SENDER_STREAMING) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "flush",
                  "Flush requires a ready or streaming session");
  }
  if (!enqueue(runtime, type, NULL, 0)) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_PLATFORM, "control",
                  "Control command allocation failed");
  }
  runtime->state = type == COMMAND_END ? SENDER_ENDING : SENDER_FLUSHING;
  notify(runtime, runtime->state, NULL);
  platform_mutex_unlock(&gate);
  return SENDER_OK;
}
sender_result sender_flush(sender_runtime *runtime, uint64_t generation,
                           sender_error *error) {
  return control(runtime, generation, COMMAND_FLUSH, error);
}
sender_result sender_set_volume(sender_runtime *runtime, uint64_t generation,
                                uint64_t command_id, uint32_t volume,
                                sender_error *error) {
  if (!runtime || !command_id || volume > 100)
    return reject(runtime, error, SENDER_INPUT, "set-volume",
                  "Runtime, nonzero command identity and volume within 0..100 "
                  "are required");
  platform_mutex_lock(&gate);
  if (generation != runtime->generation || !generation ||
      (runtime->state != SENDER_READY && runtime->state != SENDER_STREAMING) ||
      !runtime->on_command || command_id <= runtime->last_command_id) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "set-volume",
                  "Volume requires a current ready session, completion "
                  "callback and increasing command identity");
  }
  if (!enqueue(runtime, COMMAND_VOLUME, NULL, 0)) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_PLATFORM, "set-volume",
                  "Volume command allocation failed; no change accepted");
  }
  runtime->tail->command_id = command_id;
  runtime->tail->volume_percent = volume;
  runtime->last_command_id = command_id;
  platform_mutex_unlock(&gate);
  return SENDER_OK;
}
void protocol_metadata_completed(sender_result result, int peer_code) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  volume_notify(executing, 0, 0, result, peer_code, 0);
  struct notification *item = executing->notification_tail;
  if (item != &executing->emergency) {
    item->completion.kind = SENDER_COMMAND_METADATA;
    if (result != SENDER_OK) {
      snprintf(item->completion.error.operation,
               sizeof(item->completion.error.operation), "now-playing");
      snprintf(item->completion.error.message,
               sizeof(item->completion.error.message),
               "Receiver rejected media metadata (result=%d, peer=%d)", result,
               peer_code);
    }
  }
  platform_mutex_unlock(&gate);
}
void protocol_receiver_media(uint32_t command) {
  if (!executing || command < 1 || command > 4)
    return;
  platform_mutex_lock(&gate);
  if (executing->media_count < 16) {
    executing->media_commands[(executing->media_head + executing->media_count) %
                              16] = command;
    ++executing->media_count;
  }
  platform_mutex_unlock(&gate);
}
sender_result sender_take_media_command(sender_runtime *runtime,
                                        uint64_t generation, uint32_t *command,
                                        sender_error *error) {
  if (!runtime || !command)
    return reject(runtime, error, SENDER_INPUT, "media-command",
                  "Command output is required");
  platform_mutex_lock(&gate);
  if (!generation || generation != runtime->generation || runtime->polling ||
      (runtime->state != SENDER_READY && runtime->state != SENDER_STREAMING)) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "media-command",
                  "Current ready session is required");
  }
  *command = 0;
  if (runtime->media_count) {
    *command = runtime->media_commands[runtime->media_head];
    runtime->media_head = (runtime->media_head + 1) % 16;
    --runtime->media_count;
  }
  platform_mutex_unlock(&gate);
  return SENDER_OK;
}
void protocol_receiver_volume(double db) {
  if (!executing || !isfinite(db) || (db != -144.0 && (db < -30.0 || db > 0.0)))
    return;
  platform_mutex_lock(&gate);
  uint32_t percent =
      db == -144.0 ? 0 : (uint32_t)lround((db + 30.0) * 100.0 / 30.0);
  if (!executing->receiver_volume_valid ||
      executing->receiver_volume != percent) {
    executing->receiver_volume = percent;
    ++executing->receiver_volume_revision;
  }
  executing->receiver_volume_valid = true;
  platform_mutex_unlock(&gate);
}
sender_result sender_get_receiver_volume(sender_runtime *runtime,
                                         uint64_t generation, uint32_t *percent,
                                         uint64_t *revision,
                                         sender_error *error) {
  if (!runtime || !percent || !revision)
    return reject(runtime, error, SENDER_INPUT, "receiver-volume",
                  "Volume output pointers are required");
  platform_mutex_lock(&protocol_gate);
  platform_mutex_lock(&gate);
  if (!generation || generation != runtime->generation || runtime->polling ||
      (runtime->state != SENDER_READY && runtime->state != SENDER_STREAMING)) {
    platform_mutex_unlock(&gate);
    platform_mutex_unlock(&protocol_gate);
    return reject(runtime, error, SENDER_STATE, "receiver-volume",
                  "Volume requires the current ready session");
  }
  *percent = runtime->receiver_volume;
  *revision =
      runtime->receiver_volume_valid ? runtime->receiver_volume_revision : 0;
  platform_mutex_unlock(&gate);
  protocol_refresh_volume(&runtime->peer);
  platform_mutex_unlock(&protocol_gate);
  return SENDER_OK;
}
sender_result sender_set_now_playing(sender_runtime *runtime,
                                     uint64_t generation, const char *title,
                                     const char *artist, const char *album,
                                     const char *type, const void *artwork,
                                     size_t bytes, sender_error *error) {
  if (!runtime || !title || !artist || !album || !type ||
      strnlen(title, 4097) > 4096 || strnlen(artist, 4097) > 4096 ||
      strnlen(album, 4097) > 4096 || bytes > 1024 * 1024 ||
      (bytes &&
       (!artwork || (strcmp(type, "image/jpeg") && strcmp(type, "image/png")))))
    return reject(runtime, error, SENDER_INPUT, "now-playing",
                  "Metadata text or artwork is invalid or oversized");
  platform_mutex_lock(&protocol_gate);
  platform_mutex_lock(&gate);
  bool ready =
      generation && generation == runtime->generation && !runtime->polling &&
      (runtime->state == SENDER_READY || runtime->state == SENDER_STREAMING);
  platform_mutex_unlock(&gate);
  int result = ready ? protocol_now_playing(&runtime->peer, title, artist,
                                            album, type, artwork, bytes)
                     : -1;
  platform_mutex_unlock(&protocol_gate);
  if (!ready || result < 0)
    return reject(runtime, error, ready ? SENDER_NETWORK : SENDER_STATE,
                  "now-playing",
                  "Metadata requires a current session with an available "
                  "control connection");
  return result ? SENDER_BACKPRESSURE : SENDER_OK;
}
sender_result sender_retarget_presentation(sender_runtime *runtime,
                                          uint64_t generation,
                                          const sender_presentation *value,
                                          sender_error *error) {
  if (!runtime || !value || value->base_ms < SENDER_MIN_PRESENTATION_MS ||
      value->base_ms > SENDER_MAX_PRESENTATION_MS || value->offset_ms < -1000 ||
      value->offset_ms > 1000 ||
      (int64_t)value->base_ms + value->offset_ms < SENDER_MIN_PRESENTATION_MS ||
      (int64_t)value->base_ms + value->offset_ms > SENDER_MAX_PRESENTATION_MS)
    return reject(
        runtime, error, SENDER_INPUT, "retarget-presentation",
        "Base, offset and effective presentation are outside their range");
  uint32_t render = runtime->buffers.render_ms ? runtime->buffers.render_ms
      : (uint32_t)MIN(MIN(value->base_ms + value->offset_ms, value->base_ms), 250);
  if (render > (uint32_t)value->base_ms ||
      (uint64_t)render + runtime->buffers.safety_ms + runtime->buffers.startup_ms >
      (uint64_t)(value->base_ms + value->offset_ms))
    return reject(runtime, error, SENDER_INPUT, "retarget-presentation",
                  "Configured render, safety and startup exceed the target");
  platform_mutex_lock(&protocol_gate);
  platform_mutex_lock(&gate);
  if (!runtime->timed_enabled || !generation ||
      generation != runtime->generation || runtime->polling ||
      runtime->presentation_command_id ||
      (runtime->state != SENDER_READY && runtime->state != SENDER_STREAMING)) {
    platform_mutex_unlock(&gate);
    platform_mutex_unlock(&protocol_gate);
    return reject(runtime, error, SENDER_STATE, "retarget-presentation",
                  "Timeline retarget requires the current ready session and "
                  "no presentation command in flight");
  }
  int buffer_samples =
      protocol_retarget_presentation(&runtime->peer, value);
  if (buffer_samples < 0) {
    platform_mutex_unlock(&gate);
    platform_mutex_unlock(&protocol_gate);
    return reject(runtime, error, SENDER_STATE, "retarget-presentation",
                  "The current stream cannot move its sender timeline");
  }
  runtime->presentation = *value;
  runtime->buffer_samples = (uint32_t)buffer_samples;
  runtime->setup_max =
      (uint32_t)((value->base_ms + value->offset_ms) * 44100 / 1000);
  platform_mutex_unlock(&gate);
  platform_mutex_unlock(&protocol_gate);
  return SENDER_OK;
}
sender_result sender_set_presentation(sender_runtime *runtime,
                                      uint64_t generation, uint64_t command_id,
                                      const sender_presentation *value,
                                      sender_error *error) {
  if (!runtime || !value || !command_id ||
      value->base_ms < SENDER_MIN_PRESENTATION_MS ||
      value->base_ms > SENDER_MAX_PRESENTATION_MS || value->offset_ms < -1000 ||
      value->offset_ms > 1000 ||
      (int64_t)value->base_ms + value->offset_ms < SENDER_MIN_PRESENTATION_MS ||
      (int64_t)value->base_ms + value->offset_ms > SENDER_MAX_PRESENTATION_MS)
    return reject(
        runtime, error, SENDER_INPUT, "set-presentation",
        "Base, offset and effective presentation are outside their range");
  uint32_t render = runtime->buffers.render_ms ? runtime->buffers.render_ms
      : (uint32_t)MIN(MIN(value->base_ms + value->offset_ms, value->base_ms), 250);
  if (render > (uint32_t)value->base_ms ||
      (uint64_t)render + runtime->buffers.safety_ms + runtime->buffers.startup_ms >
      (uint64_t)(value->base_ms + value->offset_ms))
    return reject(runtime, error, SENDER_INPUT, "set-presentation",
                  "Configured render, safety and startup exceed the target");
  platform_mutex_lock(&gate);
  if (!runtime->timed_enabled || !generation ||
      generation != runtime->generation || !runtime->on_command ||
      command_id <= runtime->last_command_id ||
      (runtime->state != SENDER_READY && runtime->state != SENDER_STREAMING)) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "set-presentation",
                  "Presentation requires a current ready session and "
                  "completion callback");
  }
  if (!enqueue(runtime, COMMAND_PRESENTATION, NULL, 0)) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_PLATFORM, "set-presentation",
                  "Presentation command allocation failed");
  }
  runtime->tail->command_id = command_id;
  runtime->tail->presentation = *value;
  runtime->last_command_id = command_id;
  runtime->state = SENDER_FLUSHING;
  platform_mutex_unlock(&gate);
  return SENDER_OK;
}
sender_result sender_end(sender_runtime *runtime, uint64_t generation,
                         sender_error *error) {
  return control(runtime, generation, COMMAND_END, error);
}

static void timeout_callback(evutil_socket_t fd, short events, void *context) {
  (void)fd;
  (void)events;
  (void)context;
}

sender_result sender_poll(sender_runtime *runtime, unsigned timeout_ms,
                          sender_error *error) {
  if (!runtime || timeout_ms > 1000)
    return reject(runtime, error, SENDER_INPUT, "poll",
                  "Poll timeout must be within 1000 milliseconds");
  platform_mutex_lock(&gate);
  if (runtime->polling) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "poll",
                  "Runtime already has an execution context");
  }
  runtime->polling = true;
  platform_mutex_unlock(&gate);
  deliver(runtime);
  platform_mutex_lock(&protocol_gate);
  executing = runtime;
  platform_context(runtime->generation, runtime->state, NULL);
  for (;;) {
    platform_mutex_lock(&gate);
    if (runtime->state == SENDER_ENDING && runtime->volume_command_id) {
      volume_notify(runtime, runtime->volume_command_id,
                    runtime->pending_volume, SENDER_CANCELED, 0, 0);
      runtime->volume_command_id = 0;
    }
    struct command *command = runtime->head;
    if (!command) {
      platform_mutex_unlock(&gate);
      break;
    }
    if (command->type == COMMAND_AUDIO && runtime->samples == 0 &&
        runtime->state == SENDER_READY && runtime->buffers.startup_ms &&
        runtime->queued_bytes < (size_t)runtime->buffers.startup_ms * 44100 / 1000 * 4) {
      struct timespec now;
      if (platform_time(PLATFORM_MONOTONIC, &now) < 0) {
        platform_mutex_unlock(&gate);
        reject(runtime, &runtime->error, SENDER_PLATFORM, "startup-buffer",
               "Reading the startup buffer clock failed");
        finish(runtime, SENDER_FAILED);
        break;
      }
      uint64_t current = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
      if (current < command->capture_ns || current - command->capture_ns <
          (uint64_t)runtime->buffers.startup_ms * 1000000) {
        platform_mutex_unlock(&gate);
        break;
      }
    }
    if (command->type == COMMAND_VOLUME && runtime->volume_command_id &&
        runtime->state != SENDER_ENDING) {
      platform_mutex_unlock(&gate);
      break;
    }
    if (command->type == COMMAND_PRESENTATION && runtime->volume_command_id &&
        runtime->state != SENDER_ENDING) {
      platform_mutex_unlock(&gate);
      break;
    }
    runtime->head = command->next;
    if (!runtime->head) runtime->tail = NULL;
    runtime->queued_bytes -= command->bytes;
    sender_state state = runtime->state;
    platform_mutex_unlock(&gate);
    bool retain = false;
    if (command->generation == runtime->generation) {
      if (command->type == COMMAND_PRESENTATION) {
        if (state == SENDER_ENDING || state == SENDER_FAILED) {
          platform_mutex_lock(&gate);
          presentation_notify(runtime, command->command_id, SENDER_CANCELED, 0,
                              0);
          platform_mutex_unlock(&gate);
        } else {
          struct timespec now;
          runtime->presentation_command_id = command->command_id;
          runtime->pending_presentation = command->presentation;
          runtime->presentation_deadline_ns = 0;
          if (platform_time(PLATFORM_MONOTONIC, &now) == 0)
            runtime->presentation_deadline_ns =
                (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec +
                UINT64_C(1500000000);
          if (!runtime->presentation_deadline_ns)
            protocol_presentation_completed(command->command_id,
                                            SENDER_PLATFORM, 0,
                                            platform_last_failure()->code);
          else if (protocol_set_presentation(&runtime->peer,
                                             command->command_id,
                                             &command->presentation) < 0)
            protocol_presentation_completed(command->command_id, SENDER_NETWORK,
                                            0, platform_last_failure()->code);
        }
      } else if (command->type == COMMAND_CONNECT &&
                 state == SENDER_CONNECTING) {
        runtime->protocol_initialized = true;
        protocol_operation("initialize-protocol", 0);
        if (protocol_start(runtime->base, &runtime->peer,
                           runtime->options.sender_id,
                           runtime->options.client_name,
                           runtime->options.user_agent) < 0) {
          reject(runtime, &runtime->error, SENDER_SYNC, "initialize-protocol",
                 "Protocol initialization or session construction failed");
          platform_mutex_lock(&gate);
          runtime->error.platform_code = platform_last_failure()->code
                                             ? platform_last_failure()->code
                                             : errno;
          platform_mutex_unlock(&gate);
          finish(runtime, SENDER_FAILED);
        }
      } else if (command->type == COMMAND_AUDIO &&
                 (state == SENDER_READY || state == SENDER_STREAMING)) {
        if (runtime->timed_enabled) {
          struct timespec now;
          if (platform_time(PLATFORM_MONOTONIC, &now) < 0) {
            reject(runtime, &runtime->error, SENDER_PLATFORM, "send-deadline",
                   "Reading the audio send deadline clock failed");
            finish(runtime, SENDER_FAILED);
            free(command);
            continue;
          }
          uint64_t current = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
          uint64_t budget = (uint64_t)(runtime->presentation.base_ms +
              runtime->presentation.offset_ms - runtime->buffers.safety_ms) * 1000000;
          if (current > command->capture_ns &&
              current - command->capture_ns > budget) {
            reject(runtime, &runtime->error, SENDER_SYNC, "send-deadline",
                   "Queued audio exceeded its presentation safety deadline");
            finish(runtime, SENDER_FAILED);
            free(command);
            continue;
          }
        }
        if (!runtime->samples)
          platform_time(PLATFORM_MONOTONIC, &runtime->audio_time);
        struct timespec stamp = runtime->audio_time;
        stamp.tv_sec += runtime->samples / 44100;
        stamp.tv_nsec +=
            (runtime->samples % 44100) * UINT64_C(1000000000) / 44100;
        if (stamp.tv_nsec >= 1000000000) {
          ++stamp.tv_sec;
          stamp.tv_nsec -= 1000000000;
        }
        if (runtime->timed_enabled) {
          stamp.tv_sec = command->capture_ns / 1000000000;
          stamp.tv_nsec = command->capture_ns % 1000000000;
        }
        platform_context(runtime->generation, state, NULL);
        errno = 0;
        protocol_operation("encode-send", 0);
        if (protocol_write(command->pcm, command->bytes, stamp) < 0) {
          platform_mutex_lock(&gate);
          runtime->error.code =
              platform_last_failure()->code ? SENDER_NETWORK : SENDER_CODEC;
          runtime->error.generation = runtime->generation;
          runtime->error.stage = state;
          runtime->error.platform_code = platform_last_failure()->code;
          if (!runtime->error.message[0])
            snprintf(runtime->error.message, sizeof(runtime->error.message),
                     "ALAC encoding or encrypted RTP send failed");
          platform_mutex_unlock(&gate);
          finish(runtime, SENDER_FAILED);
        } else {
          runtime->samples += command->bytes / 4;
          platform_mutex_lock(&gate);
          if (runtime->state == SENDER_READY) {
            runtime->state = SENDER_STREAMING;
            notify(runtime, runtime->state, NULL);
          }
          platform_mutex_unlock(&gate);
        }
      } else if (command->type == COMMAND_FLUSH && runtime->peer.session) {
        int flushed = protocol_flush(&runtime->peer);
        if (flushed == 2) {
          platform_mutex_lock(&gate);
          requeue_front(runtime, command);
          platform_mutex_unlock(&gate);
          retain = true;
        } else {
          runtime->samples = 0;
          runtime->timeline_valid = false;
          runtime->first_capture_ns = 0;
          runtime->next_sample = 0;
          if (flushed == 0) {
            platform_mutex_lock(&gate);
            if (runtime->state == SENDER_FLUSHING) {
              runtime->state = SENDER_READY;
              notify(runtime, runtime->state, NULL);
            }
            platform_mutex_unlock(&gate);
          }
        }
      } else if (command->type == COMMAND_VOLUME) {
        if (state == SENDER_ENDING || state == SENDER_FAILED) {
          platform_mutex_lock(&gate);
          volume_notify(runtime, command->command_id, command->volume_percent,
                        SENDER_CANCELED, 0, 0);
          platform_mutex_unlock(&gate);
        } else {
          struct timespec volume_start;
          uint64_t deadline = 0;
          if (platform_time(PLATFORM_MONOTONIC, &volume_start) == 0)
            deadline = (uint64_t)volume_start.tv_sec * 1000000000 +
                       (uint64_t)volume_start.tv_nsec + UINT64_C(1500000000);
          if (!deadline) {
            platform_mutex_lock(&gate);
            runtime->volume_command_id = command->command_id;
            runtime->pending_volume = command->volume_percent;
            runtime->volume_deadline_ns = 0;
            platform_mutex_unlock(&gate);
            protocol_volume_completed(command->command_id, SENDER_PLATFORM, 0,
                                      platform_last_failure()->code);
          } else {
            int started = protocol_set_volume(&runtime->peer, command->command_id,
                                              command->volume_percent);
            if (started > 0) {
              platform_mutex_lock(&gate);
              requeue_front(runtime, command);
              platform_mutex_unlock(&gate);
              retain = true;
            } else {
              platform_mutex_lock(&gate);
              runtime->volume_command_id = command->command_id;
              runtime->pending_volume = command->volume_percent;
              runtime->volume_deadline_ns = deadline;
              platform_mutex_unlock(&gate);
              if (started < 0)
                protocol_volume_completed(command->command_id, SENDER_NETWORK, 0,
                                          platform_last_failure()->code);
            }
          }
        }
      } else if (command->type == COMMAND_END) {
        if (runtime->peer.session)
          protocol_end(&runtime->peer);
        else
          finish(runtime, SENDER_ENDED);
      }
    }
    if (retain) break;
    free(command);
  }
  platform_mutex_lock(&gate);
  sender_state current_state = runtime->state;
  bool holds_owner = owner == runtime;
  platform_mutex_unlock(&gate);
  if ((runtime->protocol_initialized || holds_owner) &&
      current_state == SENDER_FAILED)
    finish(runtime, SENDER_FAILED);
  if (runtime->protocol_initialized && active(current_state)) {
    unsigned wait_ms = timeout_ms;
    struct timespec stamp;
    if ((runtime->volume_command_id || runtime->presentation_command_id) &&
        platform_time(PLATFORM_MONOTONIC, &stamp) == 0) {
      uint64_t now =
          (uint64_t)stamp.tv_sec * 1000000000 + (uint64_t)stamp.tv_nsec;
      uint64_t deadline =
          runtime->volume_command_id ? runtime->volume_deadline_ns : UINT64_MAX;
      if (runtime->presentation_command_id &&
          runtime->presentation_deadline_ns < deadline)
        deadline = runtime->presentation_deadline_ns;
      uint64_t remaining = deadline > now ? deadline - now : 0;
      unsigned deadline_ms = (unsigned)((remaining + 999999) / 1000000);
      if (wait_ms > deadline_ms) wait_ms = deadline_ms;
    }
    struct timeval timeout = {wait_ms / 1000, (wait_ms % 1000) * 1000};
    struct event *timer = NULL;
    if (wait_ms) {
      timer = evtimer_new(runtime->base, timeout_callback, NULL);
      if (timer) evtimer_add(timer, &timeout);
    }
    event_base_loop(runtime->base, wait_ms ? EVLOOP_ONCE : EVLOOP_NONBLOCK);
    if (timer) event_free(timer);
  }
  if (runtime->presentation_command_id) {
    struct timespec now;
    if (platform_time(PLATFORM_MONOTONIC, &now) == 0 &&
        (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec >=
            runtime->presentation_deadline_ns) {
      uint64_t command_id = runtime->presentation_command_id;
      protocol_cancel_presentation(&runtime->peer);
      protocol_presentation_completed(command_id, SENDER_TIMEOUT, 0, 0);
    }
  }
  if (runtime->protocol_initialized) {
    struct timespec volume_now;
    if (runtime->volume_command_id &&
        platform_time(PLATFORM_MONOTONIC, &volume_now) == 0 &&
        (uint64_t)volume_now.tv_sec * 1000000000 +
                (uint64_t)volume_now.tv_nsec >=
            runtime->volume_deadline_ns) {
      protocol_volume_completed(runtime->volume_command_id, SENDER_TIMEOUT, 0,
                                ETIMEDOUT);
      reject(runtime, &runtime->error, SENDER_TIMEOUT, "set-volume",
             "Receiver did not acknowledge volume before the command deadline; "
             "session released");
      platform_mutex_lock(&gate);
      runtime->error.platform_code = ETIMEDOUT;
      runtime->state = SENDER_FAILED;
      platform_mutex_unlock(&gate);
    }
    platform_mutex_lock(&gate);
    sender_state state = runtime->state;
    platform_mutex_unlock(&gate);
    if (state == SENDER_FAILED)
      finish(runtime, SENDER_FAILED);
    else if (!runtime->peer.session)
      finish(runtime, SENDER_ENDED);
  }
  executing = NULL;
  platform_mutex_unlock(&protocol_gate);
  deliver(runtime);
  platform_mutex_lock(&gate);
  runtime->polling = false;
  platform_mutex_unlock(&gate);
  return SENDER_OK;
}

sender_state sender_get_state(sender_runtime *runtime, uint64_t generation) {
  if (!runtime) return SENDER_IDLE;
  platform_mutex_lock(&gate);
  sender_state state =
      generation == runtime->generation ? runtime->state : SENDER_IDLE;
  platform_mutex_unlock(&gate);
  return state;
}
sender_result sender_get_snapshot(sender_runtime *runtime, uint64_t generation,
                                  sender_snapshot *info, sender_error *error) {
  if (!runtime || !info || info->size != sizeof(*info) || info->version != 1)
    return reject(runtime, error, SENDER_INPUT, "get-snapshot",
                  "Versioned caller-owned snapshot required");
  platform_mutex_lock(&gate);
  if (!generation || generation != runtime->generation || runtime->polling ||
      runtime->callback) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "get-snapshot",
                  "Read the current generation after poll returns");
  }
  *info = runtime->metrics;
  info->size = sizeof(*info);
  info->version = 1;
  info->generation = generation;
  struct timespec now;
  if (platform_time(PLATFORM_MONOTONIC, &now) < 0) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_PLATFORM, "get-snapshot",
                  "Read snapshot clock failed");
  }
  info->sample_monotonic_ns = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
  info->queue_bytes = runtime->queued_bytes;
  info->capacity_bytes = runtime->options.audio_capacity_bytes;
  snprintf(info->clock_reason, sizeof(info->clock_reason),
           "Receiver clock lock, offset and drift have no trusted observation");
  platform_mutex_unlock(&gate);
  return SENDER_OK;
}
void protocol_statistic(enum protocol_statistic event, uint64_t value) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  sender_snapshot *m = &executing->metrics;
  if (event == PROTOCOL_PENDING) {
    m->pending_samples = value;
    m->available |= SENDER_SNAPSHOT_PENDING;
  } else {
    uint64_t *counter = event == PROTOCOL_SENT             ? &m->sent_packets
                        : event == PROTOCOL_BYTES          ? &m->sent_bytes
                        : event == PROTOCOL_RESEND_REQUEST ? &m->resend_requests
                        : event == PROTOCOL_RESEND_SUCCESS ? &m->resend_success
                        : event == PROTOCOL_RESEND_EXPIRED ? &m->resend_expired
                                                           : &m->send_failures;
    *counter = UINT64_MAX - *counter < value ? UINT64_MAX : *counter + value;
  }
  platform_mutex_unlock(&gate);
}

sender_result sender_probe_target(sender_runtime *runtime,
                                  const sender_probe_request *request,
                                  sender_probe_info *output,
                                  sender_error *error) {
  if (!runtime || !request || !output || request->size != sizeof(*request) ||
      request->version != 1 || output->size != sizeof(*output) ||
      output->version != 1 || !request->address || !request->address[0] ||
      !request->port || !request->probe_id || request->timeout_ms < 1 ||
      request->timeout_ms > 10000)
    return reject(runtime, error, SENDER_INPUT, "probe-info",
                  "Versioned probe, resolved address, port, request identity "
                  "and bounded deadline are required");
  platform_mutex_lock(&gate);
  if (active(runtime->state) || owner || runtime->polling ||
      runtime->callback || runtime->head || runtime->notifications) {
    platform_mutex_unlock(&gate);
    return reject(
        runtime, error, SENDER_STATE, "probe-info",
        "Probe requires an idle serial runtime with no active session");
  }
  runtime->polling = true;
  platform_mutex_unlock(&gate);
  platform_mutex_lock(&protocol_gate);
  sender_result result = target_probe_run(request, output, error);
  platform_mutex_unlock(&protocol_gate);
  platform_mutex_lock(&gate);
  runtime->polling = false;
  platform_mutex_unlock(&gate);
  return result;
}

sender_result sender_destroy(sender_runtime *runtime, sender_error *error) {
  if (!runtime)
    return reject(NULL, error, SENDER_INPUT, "destroy", "Runtime is required");
  platform_mutex_lock(&gate);
  if (active(runtime->state) || owner == runtime || runtime->polling ||
      runtime->callback || runtime->head || runtime->notifications) {
    platform_mutex_unlock(&gate);
    return reject(runtime, error, SENDER_STATE, "destroy",
                  "End the session and drain operations and callbacks before "
                  "destroying the runtime");
  }
  platform_mutex_unlock(&gate);
  event_base_free(runtime->base);
  peer_clear(&runtime->peer);
  free((void *)runtime->options.client_name);
  free((void *)runtime->options.user_agent);
  free(runtime);
  platform_cleanup();
  return SENDER_OK;
}

struct protocol_peer *protocol_peer_get(uint64_t identity) {
  return executing && executing->peer.id == identity ? &executing->peer : NULL;
}
void protocol_peer_session_add(uint64_t identity, void *session) {
  struct protocol_peer *peer = protocol_peer_get(identity);
  if (peer) peer->session = session;
}
void protocol_peer_session_remove(uint64_t identity) {
  struct protocol_peer *peer = protocol_peer_get(identity);
  if (peer) peer->session = NULL;
}
void protocol_operation(const char *name, int peer_code) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  snprintf(executing->error.operation, sizeof(executing->error.operation), "%s",
           name);
  executing->error.peer_code = peer_code;
  executing->error.message[0] = 0;
  platform_mutex_unlock(&gate);
}
void protocol_volume_completed(uint64_t command_id, sender_result result,
                               int peer_code, int platform_code) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  if (command_id == executing->volume_command_id) {
    if (executing->state == SENDER_ENDING) result = SENDER_CANCELED;
    if (result == SENDER_OK) {
      executing->peer.volume = executing->pending_volume;
      // A query completed before this command no longer describes the device.
      // Wait for a subsequent receiver observation before reporting it again.
      executing->receiver_volume_valid = false;
    }
    volume_notify(executing, command_id, executing->pending_volume, result,
                  peer_code, platform_code);
    executing->volume_command_id = 0;
  }
  platform_mutex_unlock(&gate);
}
void protocol_presentation_completed(uint64_t command_id, sender_result result,
                                     int peer_code, int platform_code) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  if (command_id == executing->presentation_command_id) {
    if (executing->state == SENDER_ENDING) result = SENDER_CANCELED;
    if (result == SENDER_OK) {
      executing->presentation = executing->pending_presentation;
      executing->samples = 0;
      executing->timeline_valid = false;
      executing->first_capture_ns = executing->next_sample = 0;
      if (executing->state == SENDER_FLUSHING) executing->state = SENDER_READY;
    } else if (executing->state == SENDER_FLUSHING) {
      executing->error.code = result;
      executing->error.stage = SENDER_FLUSHING;
      executing->error.generation = executing->generation;
      executing->error.peer_code = peer_code;
      executing->error.platform_code = platform_code;
      snprintf(executing->error.operation, sizeof(executing->error.operation),
               "set-presentation");
      snprintf(executing->error.message, sizeof(executing->error.message),
               "Receiver presentation change failed (peer=%d, platform=%d)",
               peer_code, platform_code);
      executing->state = SENDER_FAILED;
      notify(executing, executing->state, NULL);
    }
    presentation_notify(executing, command_id, result, peer_code,
                        platform_code);
    executing->presentation_command_id = 0;
  }
  platform_mutex_unlock(&gate);
}
void protocol_authenticating(void) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  if (executing->state == SENDER_CONNECTING) {
    executing->state = SENDER_AUTHENTICATING;
    notify(executing, executing->state, NULL);
  }
  platform_mutex_unlock(&gate);
}
void protocol_step_changed(sender_step step) {
  if (!executing || !executing->on_step) return;
  platform_mutex_lock(&gate);
  notify(executing, executing->state, NULL);
  if (executing->state != SENDER_FAILED) {
    executing->notification_tail->is_step = true;
    executing->notification_tail->step = step;
  }
  platform_mutex_unlock(&gate);
}
void protocol_timing_applied(uint32_t buffer_samples, uint32_t minimum,
                             uint32_t maximum) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  executing->buffer_samples = buffer_samples;
  executing->setup_min = minimum;
  executing->setup_max = maximum;
  executing->setup_applied = true;
  executing->metrics.available |= SENDER_SNAPSHOT_SEND | SENDER_SNAPSHOT_RESEND;
  platform_mutex_unlock(&gate);
}
void protocol_timing_sync(uint32_t current_pos, uint32_t minimum_rtp,
                          uint32_t next_rtp, uint32_t pending,
                          uint64_t capture_ns) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  ++executing->sync_packets;
  executing->sync_capture_ns = capture_ns;
  executing->sync_current_pos = current_pos;
  executing->sync_minimum_rtp = minimum_rtp;
  executing->sync_next_rtp = next_rtp;
  executing->sync_pending_samples = pending;
  platform_mutex_unlock(&gate);
}
void protocol_end_error(int peer_code) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  executing->error.code = SENDER_NETWORK;
  executing->error.generation = executing->generation;
  executing->error.stage = SENDER_ENDING;
  executing->error.peer_code = peer_code;
  snprintf(executing->error.message, sizeof(executing->error.message),
           "Receiver rejected TEARDOWN (peer=%d); local resources are released",
           peer_code);
  platform_mutex_unlock(&gate);
}
void protocol_state_changed(int callback, uint64_t identity,
                            enum output_device_state state) {
  (void)callback;
  (void)identity;
  if (!executing) return;
  platform_mutex_lock(&gate);
  if (state == OUTPUT_STATE_FAILED || state == OUTPUT_STATE_PASSWORD) {
    executing->error.code =
        state == OUTPUT_STATE_PASSWORD ||
                !strncmp(executing->error.operation, "pair verify", 11)
            ? SENDER_AUTHENTICATION
        : !strncmp(executing->error.operation, "pair setup", 10)
            ? SENDER_PAIRING
            : SENDER_NETWORK;
    executing->error.generation = executing->generation;
    executing->error.stage = executing->state;
    executing->error.platform_code = executing->error.peer_code ? 0
                                     : platform_last_failure()->code
                                         ? platform_last_failure()->code
                                         : errno;
    if (!executing->error.message[0])
      snprintf(executing->error.message, sizeof(executing->error.message),
               "Protocol operation failed during %s (peer=%d, platform=%d)",
               executing->error.operation, executing->error.peer_code,
               executing->error.platform_code);
    executing->state = SENDER_FAILED;
  } else if (state == OUTPUT_STATE_STOPPED)
    executing->state = SENDER_ENDING;
  else if (state == OUTPUT_STATE_CONNECTED && executing->state != SENDER_ENDING)
    executing->state = SENDER_READY;
  else if (state == OUTPUT_STATE_STREAMING && executing->state != SENDER_ENDING)
    executing->state = SENDER_STREAMING;
  if (active(executing->state)) notify(executing, executing->state, NULL);
  platform_mutex_unlock(&gate);
}
void protocol_credentials_changed(const char *material) {
  if (!executing) return;
  platform_mutex_lock(&gate);
  notify(executing, executing->state, material);
  platform_mutex_unlock(&gate);
}

void protocol_diagnostic(const char *format, ...) {
  if (!executing) return;
  int platform_code =
      platform_last_failure()->code ? platform_last_failure()->code : errno;
  va_list arguments;
  va_start(arguments, format);
  platform_mutex_lock(&gate);
  vsnprintf(executing->error.message, sizeof(executing->error.message), format,
            arguments);
  executing->error.platform_code = platform_code;
  platform_mutex_unlock(&gate);
  va_end(arguments);
}
