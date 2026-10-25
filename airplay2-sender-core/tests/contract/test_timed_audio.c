#include <assert.h>

#include "protocol.h"
static struct protocol_peer *peer;
static struct timespec written;
static size_t written_bytes;
static unsigned write_count;
static unsigned char written_first_byte;
void secure_string_free(char *value) {
  if (value) {
    sodium_memzero(value, strlen(value));
    free(value);
  }
}
int protocol_start(struct event_base *base, struct protocol_peer *target,
                   uint64_t identity, const char *client, const char *agent) {
  (void)base;
  (void)identity;
  (void)client;
  (void)agent;
  peer = target;
  peer->session = peer;
  protocol_step_changed(SENDER_STEP_PAIRING);
  protocol_step_changed(SENDER_STEP_SETTING_UP);
  protocol_state_changed(0, peer->id, OUTPUT_STATE_CONNECTED);
  return 0;
}
int protocol_write(const void *pcm, size_t bytes, struct timespec stamp) {
  written_first_byte = *(const unsigned char *)pcm;
  written = stamp;
  written_bytes = bytes;
  ++write_count;
  return 0;
}
int protocol_flush(struct protocol_peer *target) {
  (void)target;
  return 0;
}
int protocol_set_volume(struct protocol_peer *target, uint64_t command_id,
                        uint32_t volume) {
  (void)target;
  (void)command_id;
  (void)volume;
  return -1;
}
int protocol_end(struct protocol_peer *target) {
  target->session = NULL;
  protocol_state_changed(0, target->id, OUTPUT_STATE_STOPPED);
  return 0;
}
int protocol_set_presentation(struct protocol_peer *target, uint64_t id,
                              const sender_presentation *value) {
  (void)target;
  (void)id;
  (void)value;
  return -1;
}
int protocol_refresh_volume(struct protocol_peer *target) { (void)target; return 1; }
int protocol_now_playing(struct protocol_peer *target, const char *title,
    const char *artist, const char *album, const char *type, const void *artwork, size_t size) {
  (void)target; (void)title; (void)artist; (void)album; (void)type; (void)artwork; (void)size;
  return 0;
}
int protocol_retarget_presentation(struct protocol_peer *target,
                                   const sender_presentation *value) {
  (void)target;
  (void)value;
  return -1;
}
void protocol_cancel_presentation(struct protocol_peer *target) {
  (void)target;
}
void protocol_cleanup(void) {
  if (peer) peer->session = NULL;
  peer = NULL;
}
static unsigned pairing_steps, setting_up_steps;
static void step(void *context, uint64_t generation, sender_step value) {
  (void)context;
  assert(generation);
  if (value == SENDER_STEP_PAIRING)
    ++pairing_steps;
  else if (value == SENDER_STEP_SETTING_UP)
    ++setting_up_steps;
}
static uint64_t now_ns(void) {
  struct timespec stamp;
  assert(platform_time(PLATFORM_MONOTONIC, &stamp) == 0);
  return (uint64_t)stamp.tv_sec * 1000000000 + (uint64_t)stamp.tv_nsec;
}
static sender_timing_info timing(sender_runtime *runtime, uint64_t generation) {
  sender_timing_info info = {0};
  sender_error error;
  info.size = sizeof(info);
  info.version = 1;
  assert(sender_get_timing(runtime, generation, &info, &error) == SENDER_OK);
  return info;
}
static void presentation_boundaries(sender_create_options options) {
  const struct {
    int32_t base;
    int32_t offset;
    sender_result result;
  } cases[] = {{20, 0, SENDER_OK},          {100, 0, SENDER_OK},
               {5000, 0, SENDER_OK},
               {1250, -1000, SENDER_OK},    {4000, 1000, SENDER_OK},
               {19, 0, SENDER_INPUT},      {5001, 0, SENDER_INPUT},
               {1000, -1001, SENDER_INPUT}, {1000, 1001, SENDER_INPUT},
               {20, -1, SENDER_INPUT},      {5000, 1, SENDER_INPUT}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    sender_runtime *runtime = NULL;
    sender_error error;
    options.initial_presentation.base_ms = cases[i].base;
    options.initial_presentation.offset_ms = cases[i].offset;
    assert(sender_create_ex(&options, &runtime, &error) == cases[i].result);
    if (cases[i].result == SENDER_OK)
      assert(sender_destroy(runtime, &error) == SENDER_OK);
    else
      assert(!runtime);
  }
}
static void buffer_boundaries(sender_create_options options,
                               const sender_target *target,
                               const sender_audio_format *format) {
  sender_runtime *runtime = NULL;
  sender_error error;
  uint64_t generation;
  unsigned char pcm[1764] = {0};
  options.initial_presentation = (sender_presentation){1000, 0};
  options.buffers = (sender_buffer_config){300, 40, 10, 50};
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_OK);
  assert(sender_connect(runtime, target, format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(peer->buffers.startup_ms == 300 && peer->buffers.render_ms == 40 &&
         peer->buffers.safety_ms == 10 && peer->buffers.history_ms == 50);
  sender_timed_audio audio = {sizeof(audio), 1, 1, 0, now_ns()};
  unsigned previous_writes = write_count;
  assert(sender_submit_timed(runtime, generation, &audio, format, pcm,
                             sizeof(pcm), &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(write_count == previous_writes);
  assert(sender_flush(runtime, generation, &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(timing(runtime, generation).queue_bytes == 0 &&
         write_count == previous_writes);
  assert(sender_end(runtime, generation, &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_destroy(runtime, &error) == SENDER_OK);
  options.initial_presentation = (sender_presentation){100, 0};
  options.buffers.startup_ms = 51;
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_INPUT &&
         !runtime);
  options.buffers = (sender_buffer_config){0, 101, 0, 0};
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_INPUT);
  options.buffers = (sender_buffer_config){0, 40, 61, 0};
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_INPUT);
  options.buffers = (sender_buffer_config){0, 40, 10, 60001};
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_INPUT);
  options.buffers.history_ms = 50;
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_OK);
  assert(sender_connect(runtime, target, format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  audio.capture_monotonic_ns = now_ns();
  assert(sender_submit_timed(runtime, generation, &audio, format, pcm,
                             sizeof(pcm), &error) == SENDER_OK);
#ifdef _WIN32
  Sleep(110);
#else
  struct timespec wait = {0, 110000000};
  nanosleep(&wait, NULL);
#endif
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_get_state(runtime, generation) == SENDER_FAILED &&
         write_count == previous_writes);
  assert(sender_destroy(runtime, &error) == SENDER_OK);
}
int main(void) {
  sender_create_options options = {
      SENDER_ABI_VERSION,
      sizeof(options),
      {1, "Timed consumer", "Timed consumer", 4096, NULL, NULL, NULL},
      {1000, 100},
      step};
  sender_runtime *runtime = NULL;
  sender_error error;
  uint64_t generation;
  sender_target target = {
      "127.0.0.1",
      7000,
      "AA:BB:CC:DD:EE:FF",
      "Controlled timing peer",
      (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48),
      0,
      50};
  sender_audio_format format = {44100, 16, 2};
  unsigned char pcm[1764] = {0};
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_OK);
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(pairing_steps == 1 && setting_up_steps == 1 &&
         peer->presentation_base_ms == 1000 && peer->offset_ms == 100);
  sender_timed_audio audio = {sizeof(audio), 1, 1, 0, now_ns() - 50000000};
  sender_timed_audio invalid = audio;
  invalid.version = 2;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_INPUT);
  invalid = audio;
  --invalid.size;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_INPUT);
  invalid = audio;
  invalid.timeline_epoch = 0;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_INPUT);
  invalid = audio;
  invalid.sample_index = 1;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_SYNC);
  invalid = audio;
  invalid.capture_monotonic_ns = now_ns() + 100000000;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_SYNC);
  invalid = audio;
  invalid.capture_monotonic_ns = now_ns() - 2000000000;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_SYNC);
  sender_audio_format unsupported = format;
  unsupported.sample_rate = 48000;
  assert(sender_submit_timed(runtime, generation, &audio, &unsupported, pcm,
                             sizeof(pcm), &error) == SENDER_INPUT);
  assert(timing(runtime, generation).next_sample == 0 &&
         timing(runtime, generation).queue_bytes == 0);
  assert(sender_submit_timed(runtime, generation - 1, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_STATE);
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm) - 1, &error) == SENDER_INPUT);
  pcm[0] = 37;
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_OK);
  pcm[0] = 91;
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_SYNC);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK &&
         written_bytes == sizeof(pcm));
  assert(written_first_byte == 37 && write_count == 1);
  assert((uint64_t)written.tv_sec * 1000000000 + (uint64_t)written.tv_nsec ==
         audio.capture_monotonic_ns);
  audio.sample_index = 441;
  audio.capture_monotonic_ns += 10000000;
  invalid = audio;
  invalid.timeline_epoch = 2;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_SYNC);
  invalid = audio;
  invalid.sample_index = 882;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_SYNC);
  invalid = audio;
  invalid.capture_monotonic_ns -= 30000000;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_SYNC);
  assert(timing(runtime, generation).next_sample == 441);
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_OK);
  audio.sample_index = 882;
  audio.capture_monotonic_ns += 10000000;
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_OK);
  audio.sample_index = 1323;
  audio.capture_monotonic_ns += 10000000;
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_BACKPRESSURE);
  assert(timing(runtime, generation).next_sample == 1323 &&
         timing(runtime, generation).queue_bytes == sizeof(pcm) * 2);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_OK);
  assert(sender_flush(runtime, generation, &error) == SENDER_OK &&
         sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(timing(runtime, generation).next_sample == 0 &&
         timing(runtime, generation).first_capture_ns == 0 &&
         timing(runtime, generation).queue_bytes == 0);
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_SYNC);
  audio.timeline_epoch = 2;
  audio.sample_index = 0;
  audio.capture_monotonic_ns = now_ns();
  invalid = audio;
  invalid.sample_index = 441;
  assert(sender_submit_timed(runtime, generation, &invalid, &format, pcm,
                             sizeof(pcm), &error) == SENDER_SYNC);
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(timing(runtime, generation).timeline_epoch == 2 &&
         timing(runtime, generation).first_capture_ns ==
             audio.capture_monotonic_ns);
  assert(sender_end(runtime, generation, &error) == SENDER_OK &&
         sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_STATE);
  uint64_t previous_generation = generation;
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(generation > previous_generation);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  audio.timeline_epoch = 1;
  audio.sample_index = 0;
  audio.capture_monotonic_ns = now_ns();
  assert(sender_submit_timed(runtime, previous_generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_STATE);
  assert(sender_flush(runtime, previous_generation, &error) == SENDER_STATE);
  assert(timing(runtime, generation).timeline_epoch == 0);
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(timing(runtime, generation).timeline_epoch == 1);
  assert(sender_end(runtime, generation, &error) == SENDER_OK &&
         sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_destroy(runtime, &error) == SENDER_OK);
  runtime = NULL;
  options.initial_presentation.base_ms = 19;
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_INPUT &&
         !runtime);
  options.initial_presentation.base_ms = 1000;
  options.initial_presentation.offset_ms = -1001;
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_INPUT);
  presentation_boundaries(options);
  buffer_boundaries(options, &target, &format);
  assert(sender_create(&options.options, &runtime, &error) == SENDER_OK);
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK &&
         sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(peer->presentation_base_ms == 0 && peer->offset_ms == 0);
  assert(timing(runtime, generation).base_ms == 2000 &&
         timing(runtime, generation).offset_ms == 0);
  assert(sender_submit_timed(runtime, generation, &audio, &format, pcm,
                             sizeof(pcm), &error) == SENDER_INPUT);
  assert(sender_submit(runtime, generation, &format, pcm, sizeof(pcm),
                       &error) == SENDER_OK &&
         sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_end(runtime, generation, &error) == SENDER_OK &&
         sender_poll(runtime, 0, &error) == SENDER_OK &&
         sender_destroy(runtime, &error) == SENDER_OK);
  return 0;
}
