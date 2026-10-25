#include "sender.h"
#include "../platform/platform.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static sender_result reject(sender_error *error, sender_result code, const char *reason)
{
    if (error) {
        memset(error, 0, sizeof(*error));
        error->code = code;
        snprintf(error->operation, sizeof(error->operation), "validate-target");
        snprintf(error->message, sizeof(error->message), "%s", reason);
    }
    return code;
}

sender_result sender_validate_target(const sender_target *target, const sender_audio_format *format, sender_error *error)
{
    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (!target || !format || !target->address || !target->receiver_id ||
        !target->name || !target->name[0] || !target->control_port || target->volume_percent > 100)
        return reject(error, SENDER_INPUT, "Address, control port, receiver identity, name and valid volume are required");
    if (strlen(target->receiver_id) != 17)
        return reject(error, SENDER_INPUT, "Receiver identity must be a colon-separated MAC identity");
    for (size_t i = 0; i < 17; ++i)
        if (i % 3 == 2 ? target->receiver_id[i] != ':' : !isxdigit((unsigned char)target->receiver_id[i]))
            return reject(error, SENDER_INPUT, "Receiver identity contains an invalid octet or separator");
    if (inet_pton(AF_INET, target->address, &ipv4) == 1) {
        if (target->scope_id || ipv4.s_addr == INADDR_ANY || IN_MULTICAST(ntohl(ipv4.s_addr)))
            return reject(error, SENDER_INPUT, "IPv4 target must be unicast and have no IPv6 scope");
    } else if (inet_pton(AF_INET6, target->address, &ipv6) == 1) {
        if (IN6_IS_ADDR_UNSPECIFIED(&ipv6) || IN6_IS_ADDR_MULTICAST(&ipv6) ||
            (IN6_IS_ADDR_LINKLOCAL(&ipv6) && !target->scope_id))
            return reject(error, SENDER_INPUT, "IPv6 target must be unicast; link-local targets require an interface scope");
    } else return reject(error, SENDER_INPUT, "Target requires a numeric IPv4 or IPv6 address");
    /* OwnTone airplay.c features_map and airplay_device_cb require these bits.
     * This library requires the verified PTP path and rejects NTP fallback. */
    const uint64_t required = (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48);
    if ((target->features & required) != required)
        return reject(error, SENDER_CAPABILITY, "Target requires AirPlay audio, CoreUtils pairing/encryption and PTP");
    if (format->sample_rate != 44100 || format->bits_per_sample != 16 || format->channels != 2)
        return reject(error, SENDER_INPUT, "Audio must be interleaved little-endian signed PCM at 44100 Hz, 16 bit, stereo");
    if (error) memset(error, 0, sizeof(*error));
    return SENDER_OK;
}
