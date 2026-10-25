#include "sender.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    sender_error error;
    sender_target target = { "127.0.0.1", 7000, "AA:BB:CC:DD:EE:FF", "Receiver",
        (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48), 0, 50 };
    sender_audio_format format = { 44100, 16, 2 };
    assert(sender_validate_target(&target, &format, &error) == SENDER_OK);
    target.address = "receiver.local";
    assert(sender_validate_target(&target, &format, &error) == SENDER_INPUT);
    assert(error.operation[0] && error.message[0]);
    target.address = "127.0.0.1";
    target.control_port = 0;
    assert(sender_validate_target(&target, &format, &error) == SENDER_INPUT);
    target.control_port = 7000;
    target.receiver_id = "AA:BB";
    assert(sender_validate_target(&target, &format, &error) == SENDER_INPUT);
    target.receiver_id = "AA:BB:CC:DD:EE:FF";
    target.features &= ~(UINT64_C(1) << 48);
    assert(sender_validate_target(&target, &format, &error) == SENDER_CAPABILITY);
    target.features |= UINT64_C(1) << 48;
    target.address = "fe80::1";
    assert(sender_validate_target(&target, &format, &error) == SENDER_INPUT);
    target.scope_id = 1;
    assert(sender_validate_target(&target, &format, &error) == SENDER_OK);
    target.address = "::1";
    target.scope_id = 0;
    assert(sender_validate_target(&target, &format, &error) == SENDER_OK);
    format.sample_rate = 48000;
    assert(sender_validate_target(&target, &format, &error) == SENDER_INPUT);
    assert(sender_validate_target(NULL, &format, &error) == SENDER_INPUT);
    return 0;
}
