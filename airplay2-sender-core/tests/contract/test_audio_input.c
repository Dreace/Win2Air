#include "sender.h"
#include <assert.h>
#include <stdint.h>

int main(void)
{
    sender_runtime *runtime;
    sender_error error;
    uint64_t generation;
    sender_options options = { 0x12345678, "Client", "OwnTone/29.3", 8192, NULL, NULL, NULL };
    sender_target target = { "127.0.0.1", 1, "AA:BB:CC:DD:EE:FF", "Receiver",
        (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48), 0, 50 };
    sender_audio_format format = { 44100, 16, 2 };
    int16_t pcm[704] = { 0 };
    assert(sender_create(&options, &runtime, &error) == SENDER_OK);
    assert(sender_submit(runtime, 1, &format, pcm, sizeof(pcm), &error) == SENDER_STATE);
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation, &error) == SENDER_OK);
    assert(sender_submit(runtime, generation, &format, pcm, 3, &error) == SENDER_INPUT);
    assert(sender_submit(runtime, generation, &format, pcm, SIZE_MAX, &error) == SENDER_INPUT);
    format.channels = 1;
    assert(sender_submit(runtime, generation, &format, pcm, sizeof(pcm), &error) == SENDER_INPUT);
    format.channels = 2;
    assert(sender_submit(runtime, generation, &format, pcm, sizeof(pcm), &error) == SENDER_STATE);
    assert(sender_end(runtime, generation, &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(sender_destroy(runtime, &error) == SENDER_OK);
    return 0;
}
