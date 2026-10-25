#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "temporary_file.h"

struct observations {
    uint64_t first_origin, last_origin, max_interval;
    unsigned messages, last_sequence, last_follow;
};
static int reject(const char *reason)
{
    fprintf(stderr, "operation=verify-sync stage=receiver reason=%s\n", reason);
    return 1;
}

static int verify(FILE *timing, FILE *receiver, uint64_t expected,
                  const char *source, struct observations *out)
{
    char line[4096], address[64];
    int accepted = 0, setup = 0, record = 0, output = 0;
    memset(out, 0, sizeof *out);
    while (fgets(line, sizeof line, timing)) {
        uint64_t clock, grandmaster, origin;
        unsigned sequence, follow;
        const char *entry = strstr(line, "Clock ");
        const char *stamp = strstr(line, "Raw Precise Origin Timestamp: ");
        const char *number = strstr(line, "ID: ");
        if (strstr(line, "Timing discontinuity on clock")) return reject("timing-discontinuity");
        if (!entry || !strstr(entry, ", grandmaster ")) continue;
        if (!stamp || !number ||
            sscanf(entry, "Clock %" SCNx64 ", grandmaster %" SCNx64, &clock, &grandmaster) != 2 ||
            sscanf(stamp, "Raw Precise Origin Timestamp: %" SCNx64, &origin) != 1 ||
            sscanf(number, "ID: %u, Follow_Up Number: %u. Source: %63s", &sequence, &follow, address) != 3 ||
            clock != expected || grandmaster != expected || strcmp(address, source) ||
            !origin || !follow || sequence > UINT16_MAX) return reject("timing-record-or-identity");
        if (out->messages) {
            unsigned advance = (sequence - out->last_sequence) & UINT16_MAX;
            /* The frozen NQPTP receiver saturates this diagnostic counter at 100. */
            unsigned expected_follow = out->last_follow < 100 ? out->last_follow + 1 : 100;
            if (!advance || advance > UINT16_MAX / 2 || follow != expected_follow ||
                origin <= out->last_origin) return reject("sequence-follow-count-or-origin-order");
            uint64_t interval = origin - out->last_origin;
            if (interval > out->max_interval) out->max_interval = interval;
        } else out->first_origin = origin;
        out->last_origin = origin;
        out->last_sequence = sequence; out->last_follow = follow; ++out->messages;
    }
    while (fgets(line, sizeof line, receiver)) {
        const char *clock_line = strstr(line, "NQPTP master clock ");
        if (clock_line) {
            uint64_t clock;
            if (sscanf(clock_line, "NQPTP master clock %" SCNx64, &clock) != 1 || clock != expected) return reject("receiver-master-identity");
            accepted = 1;
        }
        if (strstr(line, "SETUP (AirPlay 2) on PTP stream")) setup = 1;
        if (strstr(line, "RECORD (AP2) on PTP stream")) record = 1;
        if (strstr(line, "ALAC/44100/S16_LE/2")) output = 1;
    }
    if (ferror(timing) || ferror(receiver)) return reject("read-error");
    if (!accepted || !setup || !record || !output) return reject("receiver-mode-or-format-missing");
    if (out->messages < 8 || out->last_origin - out->first_origin < UINT64_C(1000000000)) {
        fprintf(stderr, "messages=%u origin_span_ns=%" PRIu64 "\n", out->messages, out->last_origin - out->first_origin);
        return reject("insufficient-active-synchronization-timeline");
    }
    return 0;
}

static int fixture(unsigned mutation)
{
    FILE *timing = temporary_file(), *receiver = temporary_file();
    struct observations out;
    if (!timing || !receiver) { if (timing) fclose(timing); if (receiver) fclose(receiver); return 1; }
    for (unsigned i = 0; i <= 150; ++i) {
        uint64_t origin = UINT64_C(100000000000) + UINT64_C(1000000000) * i;
        unsigned sequence = (65520 + i) & UINT16_MAX;
        if (mutation == 1 && i == 10) sequence = (65520 + i - 1) & UINT16_MAX;
        if (mutation == 2 && i == 10) origin -= UINT64_C(1000000000);
        if (mutation == 3 && i == 1) break;
        fprintf(timing, "Clock abc, grandmaster abc. Offset: 0. Raw Precise Origin Timestamp: %" PRIx64 ". ID: %u, Follow_Up Number: %u. Source: %s\n", origin, sequence, i < 100 ? i + 1 : 100, mutation == 4 ? "10.77.0.3" : "10.77.0.1");
    }
    fprintf(receiver, "SETUP (AirPlay 2) on PTP stream\nRECORD (AP2) on PTP stream\nNQPTP master clock %s.\n%s\n", mutation == 5 ? "abd" : "abc", mutation == 6 ? "ALAC/48000/S16_LE/2" : "ALAC/44100/S16_LE/2");
    if (mutation == 7) fputs("Timing discontinuity on clock abc\n", timing);
    rewind(timing); rewind(receiver);
    int status = verify(timing, receiver, UINT64_C(0xabc), "10.77.0.1", &out);
    fclose(timing); fclose(receiver);
    return mutation ? status == 0 : status != 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--clock") == 0) {
        FILE *receiver = fopen(argv[2], "r");
        char line[4096];
        uint64_t identity = 0;
        if (!receiver) return reject("open-receiver-log");
        while (fgets(line, sizeof line, receiver)) {
            const char *entry = strstr(line, "NQPTP master clock ");
            uint64_t clock;
            if (!entry) continue;
            if (sscanf(entry, "NQPTP master clock %" SCNx64, &clock) != 1 || !clock || (identity && identity != clock)) {
                fclose(receiver); return reject("receiver-clock-identity-change");
            }
            identity = clock;
        }
        int error = ferror(receiver); fclose(receiver);
        if (error || !identity) return reject("receiver-clock-missing");
        printf("{\"clock_id\":\"%016" PRIx64 "\"}\n", identity);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        for (unsigned mutation = 0; mutation <= 7; ++mutation)
            if (fixture(mutation)) { fprintf(stderr, "operation=verify-sync stage=self-test code=%u\n", mutation); return 1; }
        return 0;
    }
    if (argc != 5 || strlen(argv[3]) != 16 || strspn(argv[3], "0123456789abcdef") != 16 ||
        strcmp(argv[4], "10.77.0.1")) {
        fprintf(stderr, "operation=verify-sync stage=input reason=expected-timing-log-receiver-log-full-clock-and-frozen-source\n");
        return 2;
    }
    uint64_t clock = strtoull(argv[3], NULL, 16);
    FILE *timing = fopen(argv[1], "r"), *receiver = fopen(argv[2], "r");
    struct observations out;
    int status = 1;
    if (timing && receiver && clock) status = verify(timing, receiver, clock, argv[4], &out);
    if (timing) fclose(timing);
    if (receiver) fclose(receiver);
    if (status) fprintf(stderr, "operation=verify-sync stage=receiver reason=clock-message-order-time-span-or-output-mismatch\n");
    else printf("{\"summary_id\":\"receiver-ptp-clock-and-timeline-verified\",\"clock_id\":\"%016" PRIx64 "\",\"messages\":%u,\"origin_span_ns\":%" PRIu64 ",\"max_origin_interval_ns\":%" PRIu64 ",\"receiver_verified\":true}\n", clock, out.messages, out.last_origin - out.first_origin, out.max_interval);
    return status;
}
