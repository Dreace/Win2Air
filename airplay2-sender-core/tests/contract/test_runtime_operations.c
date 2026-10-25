#include "protocol.h"
#include <assert.h>

static struct protocol_peer *peer;
static unsigned teardown_count, cleanup_count;
static int write_failure, teardown_rejected;
static int start_failure;
static int notification_failure, fail_next_allocation;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_next_allocation) { fail_next_allocation = 0; return NULL; }
    return __real_calloc(count, size);
}
static sender_error last_error;
static sender_state last_state;
static char exported_material[193];
static unsigned material_callbacks;
static int caller_storage_failed;
static sender_runtime *concurrent_runtime;
static uint64_t concurrent_generation;
static void *concurrent_end(void *context)
{
    (void)context;
    sender_error error;
    assert(sender_end(concurrent_runtime, concurrent_generation, &error) == SENDER_OK);
    for (unsigned i = 0; i < 10000; ++i)
        assert(sender_submit(concurrent_runtime, concurrent_generation, NULL, NULL, 0, &error) == SENDER_INPUT);
    return NULL;
}

void secure_string_free(char *value)
{ if (value) { sodium_memzero(value, strlen(value)); free(value); } }
int protocol_start(struct event_base *base, struct protocol_peer *target, uint64_t identity,
                   const char *client, const char *agent)
{
    (void)base; (void)identity; (void)client; (void)agent;
    peer = target;
    peer->session = peer;
    if (notification_failure) fail_next_allocation = 1;
    if (start_failure) {
        protocol_operation("pair verify 1", 470);
        protocol_state_changed(0, peer->id, OUTPUT_STATE_FAILED);
        return 0;
    }
    protocol_state_changed(0, peer->id, OUTPUT_STATE_CONNECTED);
    char material[193]; memset(material, 'a', 192); material[192] = 0;
    protocol_credentials_changed(material);
    sodium_memzero(material, sizeof(material));
    return 0;
}
int protocol_write(const void *pcm, size_t bytes, struct timespec stamp)
{
    (void)pcm; (void)bytes; (void)stamp;
    if (write_failure == 2) {
        platform_fail("socket-send", 10054);
        protocol_diagnostic("Controlled network send failure");
        return -1;
    }
    if (write_failure) {
        platform_thread worker;
        assert(platform_thread_start(&worker, concurrent_end, NULL) == 0);
        for (unsigned i = 0; i < 10000; ++i) protocol_diagnostic("Controlled encoder failure");
        assert(platform_thread_join(worker) == 0);
        return -1;
    }
    return 0;
}
int protocol_set_volume(struct protocol_peer* target, uint64_t command_id,
                        uint32_t volume) {
  (void)target;
  (void)command_id;
  (void)volume;
  return -1;
}
int protocol_set_presentation(struct protocol_peer* target, uint64_t id,
                              const sender_presentation* value) {
  (void)target; (void)id; (void)value; return -1;
}
int protocol_refresh_volume(struct protocol_peer *target) { (void)target; return 1; }
int protocol_now_playing(struct protocol_peer *target, const char *title,
    const char *artist, const char *album, const char *type, const void *artwork, size_t size) {
  (void)target; (void)title; (void)artist; (void)album; (void)type; (void)artwork; (void)size;
  return 0;
}
int protocol_retarget_presentation(struct protocol_peer* target,
                                   const sender_presentation* value) {
  (void)target; (void)value; return -1;
}
void protocol_cancel_presentation(struct protocol_peer* target) { (void)target; }
int protocol_flush(struct protocol_peer *target)
{ (void)target; return 0; }
int protocol_end(struct protocol_peer *target)
{
    ++teardown_count;
    if (teardown_rejected) protocol_end_error(500);
    target->session = NULL;
    protocol_state_changed(0, target->id, OUTPUT_STATE_STOPPED);
    return 0;
}
void protocol_cleanup(void)
{ ++cleanup_count; if (peer) peer->session = NULL; peer = NULL; }
static void changed(void *context, uint64_t generation, sender_state state, const sender_error *error)
{
    (void)context; (void)generation;
    last_state = state;
    if (error) last_error = *error;
}
static void credentials_changed(void *context, uint64_t generation, const sender_credentials *credentials)
{
    (void)context;
    assert(generation && credentials->sender_id == 1);
    assert(!strcmp(credentials->receiver_id, "AA:BB:CC:DD:EE:FF"));
    assert(strlen(credentials->material) == 192 && credentials->material[0] == 'a');
    memcpy(exported_material, credentials->material, sizeof(exported_material));
    ++material_callbacks;
    caller_storage_failed = 1; /* Caller storage failure is reported by the caller. */
}
int main(void)
{
    sender_options options = { 1, "Runtime contract", "Contract", 1408, changed, credentials_changed, NULL };
    sender_target target = { "127.0.0.1", 7000, "AA:BB:CC:DD:EE:FF", "Receiver",
        (UINT64_C(1)<<9) | (UINT64_C(1)<<41) | (UINT64_C(1)<<48), 0, 50 };
    sender_audio_format format = { 44100, 16, 2 };
    sender_runtime *runtime;
    sender_error error;
    uint64_t generation;
    assert(sender_create(&options, &runtime, &error) == SENDER_OK);
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation, &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(sender_get_state(runtime, generation) == SENDER_READY);
    assert(material_callbacks == 1 && caller_storage_failed && exported_material[191] == 'a');
    assert(sender_flush(runtime, generation, &error) == SENDER_OK);
    assert(sender_end(runtime, generation, &error) == SENDER_OK);
    teardown_rejected = 1;
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(teardown_count == 1 && cleanup_count == 1);
    assert(last_state == SENDER_ENDED && last_error.code == SENDER_NETWORK && last_error.peer_code == 500);
    assert(sender_end(runtime, generation, &error) == SENDER_ALREADY_ENDED);
    teardown_rejected = 0;
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation, &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    unsigned char pcm[1408] = {0};
    assert(sender_submit(runtime, generation, &format, pcm, sizeof(pcm), &error) == SENDER_OK);
    assert(sender_submit(runtime, generation, &format, pcm, 4, &error) == SENDER_BACKPRESSURE);
    write_failure = 1;
    concurrent_runtime = runtime;
    concurrent_generation = generation;
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(last_state == SENDER_FAILED && last_error.code == SENDER_CODEC && cleanup_count == 2);
    assert(!strcmp(last_error.message, "Controlled encoder failure"));
    write_failure = 0;
    start_failure = 1;
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation, &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(last_state == SENDER_FAILED && last_error.code == SENDER_AUTHENTICATION && cleanup_count == 3);
    assert(sender_end(runtime, generation, &error) == SENDER_ALREADY_ENDED);
    start_failure = 0;
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation, &error) == SENDER_OK);
    assert(sender_end(runtime, generation, &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    notification_failure = 1;
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation, &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(last_state == SENDER_FAILED && last_error.code == SENDER_PLATFORM);
    assert(cleanup_count == 4);
    assert(sender_destroy(runtime, &error) == SENDER_OK);
    notification_failure = 0;
    write_failure = 2;
    assert(sender_create(&options, &runtime, &error) == SENDER_OK);
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation, &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(sender_submit(runtime, generation, &format, pcm, sizeof(pcm), &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(last_state == SENDER_FAILED && last_error.code == SENDER_NETWORK && last_error.platform_code == 10054);
    assert(!strcmp(last_error.message, "Controlled network send failure"));
    assert(sender_destroy(runtime, &error) == SENDER_OK);
    sodium_memzero(exported_material, sizeof(exported_material));
    return 0;
}
