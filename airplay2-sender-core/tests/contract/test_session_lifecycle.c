#include "sender.h"
#include <assert.h>

static sender_target target = { "127.0.0.1", 1, "AA:BB:CC:DD:EE:FF", "Receiver",
    (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48), 0, 50 };
static sender_audio_format format = { 44100, 16, 2 };
static sender_runtime *runtime;
static int callback_seen;
static void changed(void *context, uint64_t generation, sender_state state, const sender_error *error)
{
    (void)context; (void)error;
    sender_error detail;
    if (state == SENDER_CONNECTING) {
        callback_seen = 1;
        assert(sender_destroy(runtime, &detail) == SENDER_STATE);
        assert(sender_end(runtime, generation, &detail) == SENDER_OK);
        assert(sender_end(runtime, generation, &detail) == SENDER_OK);
    }
}
int main(void)
{
    sender_runtime *other;
    sender_error error;
    uint64_t generation, next;
    sender_options options = { 0x12345678, "Client", "OwnTone/29.3", 8192, changed, NULL, NULL };
    assert(sender_create(&options, &runtime, &error) == SENDER_OK);
    options.on_state = NULL;
    assert(sender_create(&options, &other, &error) == SENDER_OK);
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation, &error) == SENDER_OK);
    assert(sender_connect(other, &target, &format, NULL, NULL, &next, &error) == SENDER_OCCUPIED);
    assert(sender_destroy(runtime, &error) == SENDER_STATE);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(callback_seen);
    assert(sender_end(runtime, generation, &error) == SENDER_ALREADY_ENDED);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(sender_end(runtime, generation, &error) == SENDER_ALREADY_ENDED);
    assert(sender_connect(other, &target, &format, NULL, NULL, &next, &error) == SENDER_OK);
    assert(next > generation);
    assert(sender_flush(other, generation, &error) == SENDER_STATE);
    assert(sender_end(other, generation, &error) == SENDER_STATE);
    assert(sender_end(other, next, &error) == SENDER_OK);
    assert(sender_poll(other, 0, &error) == SENDER_OK);
    assert(sender_destroy(other, &error) == SENDER_OK);
    assert(sender_destroy(runtime, &error) == SENDER_OK);
    return 0;
}
