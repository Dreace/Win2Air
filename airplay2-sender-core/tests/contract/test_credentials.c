#include "sender.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    sender_runtime *runtime;
    sender_error error;
    sender_options options = { 0x12345678, "Client", "OwnTone/29.3", 8192, NULL, NULL, NULL };
    sender_target target = { "127.0.0.1", 1, "AA:BB:CC:DD:EE:FF", "Receiver",
        (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48), 0, 50 };
    sender_audio_format format = { 44100, 16, 2 };
    char material[193]; memset(material, 'a', 192); material[192] = 0;
    sender_credentials credentials = { "00:11:22:33:44:55", options.sender_id, material };
    uint64_t generation;
    assert(sender_create(&options, &runtime, &error) == SENDER_OK);
    assert(sender_connect(runtime, &target, &format, &credentials, NULL, &generation, &error) == SENDER_AUTHENTICATION);
    assert(!strstr(error.message, material));
    credentials.receiver_id = target.receiver_id;
    credentials.sender_id++;
    assert(sender_connect(runtime, &target, &format, &credentials, NULL, &generation, &error) == SENDER_AUTHENTICATION);
    credentials.sender_id = options.sender_id;
    credentials.material = "invalid-material";
    assert(sender_connect(runtime, &target, &format, &credentials, NULL, &generation, &error) == SENDER_AUTHENTICATION);
    credentials.material = material;
    material[0] = 'z';
    assert(sender_connect(runtime, &target, &format, &credentials, NULL, &generation, &error) == SENDER_AUTHENTICATION);
    material[0] = 'a';
    assert(sender_connect(runtime, &target, &format, &credentials, NULL, &generation, &error) == SENDER_OK);
    memset(material, 0, sizeof(material));
    assert(sender_end(runtime, generation, &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(sender_destroy(runtime, &error) == SENDER_OK);
    return 0;
}
