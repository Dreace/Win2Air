#include "codec.h"
#include "rtp_common.h"
#include <gcrypt.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "operation=packet-history check=%s line=%d\n", #condition, __LINE__); \
    return 1; } } while (0)

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

int main(void)
{
    struct media_quality quality = {44100, 16, 2, 0};
    struct sender_codec *codec;
    struct rtp_session *session;
    struct evbuffer *encoded = evbuffer_new();
    uint8_t pcm[352 * 4];
    uint8_t saved[5][2048];
    size_t lengths[5];
    uint16_t sequences[5];

    CHECK(gcry_check_version(NULL) != NULL);
    CHECK(encoded != NULL);
    codec = codec_alac_new(&quality);
    CHECK(codec != NULL);
    session = rtp_session_new(&quality, 3, 0, UINT64_C(0xaabbccddeeff));
    CHECK(session != NULL);
    session->seqnum = UINT16_MAX - 1;
    session->pos = UINT32_MAX - 351;

    CHECK(rtp_packet_get(session, session->seqnum) == NULL);
    for (size_t index = 0; index < 5; ++index) {
        uint32_t position = session->pos;
        int bytes;
        struct rtp_packet *packet;
        for (size_t offset = 0; offset < sizeof(pcm); ++offset)
            pcm[offset] = (uint8_t)(offset * 31 + index * 17);
        CHECK(evbuffer_get_length(encoded) == 0);
        bytes = codec_alac_encode(encoded, codec, pcm, sizeof(pcm), 352, &quality);
        CHECK(bytes > 0 && (size_t)bytes == evbuffer_get_length(encoded));
        packet = rtp_packet_next(session, (size_t)bytes, 352,
                                 (uint8_t)(96 | (index == 0 ? RTP_MARKER_BIT : 0)));
        CHECK(packet != NULL && packet->header_len == 12);
        CHECK(packet->data_len <= sizeof(saved[index]));
        CHECK(packet->header[0] == 0x80);
        CHECK(packet->header[1] == (uint8_t)(96 | (index == 0 ? RTP_MARKER_BIT : 0)));
        CHECK(read_be16(packet->header + 2) == session->seqnum);
        CHECK(read_be32(packet->header + 4) == position);
        CHECK(read_be32(packet->header + 8) == 0);
        CHECK(packet->payload_len == (size_t)bytes);
        CHECK(evbuffer_remove(encoded, packet->payload, (size_t)bytes) == bytes);
        sequences[index] = packet->seqnum;
        lengths[index] = packet->data_len;
        memcpy(saved[index], packet->data, packet->data_len);
        CHECK(rtp_packet_get(session, packet->seqnum) == NULL);
        rtp_packet_commit(session, packet);
        CHECK(session->pos == (uint32_t)(position + 352));
        CHECK(session->seqnum == (uint16_t)(sequences[index] + 1));
        for (size_t retained = index < 3 ? 0 : index - 2; retained <= index; ++retained) {
            struct rtp_packet *history = rtp_packet_get(session, sequences[retained]);
            CHECK(history != NULL && history->data_len == lengths[retained]);
            CHECK(memcmp(history->data, saved[retained], lengths[retained]) == 0);
        }
    }
    CHECK(sequences[0] == UINT16_MAX - 1 && sequences[1] == UINT16_MAX);
    CHECK(sequences[2] == 0 && session->pktbuf_len == 3);
    CHECK(rtp_packet_get(session, sequences[0]) == NULL);
    CHECK(rtp_packet_get(session, sequences[1]) == NULL);
    CHECK(rtp_packet_get(session, session->seqnum) == NULL);

    CHECK(codec_alac_encode(encoded, codec, pcm, sizeof(pcm) - 1, 352, &quality) < 0);
    CHECK(evbuffer_get_length(encoded) == 0);
    CHECK(codec_alac_encode(encoded, codec, pcm, sizeof(pcm), 351, &quality) < 0);
    CHECK(evbuffer_get_length(encoded) == 0);
    {
        uint16_t sequence = session->seqnum;
        uint32_t position = session->pos;
        struct rtp_packet *packet;
        rtp_session_flush(session);
        CHECK(session->seqnum == sequence && session->pos == position);
        CHECK(session->pktbuf_len == 0 && session->sync_counter == 0);
        CHECK(rtp_packet_get(session, sequences[4]) == NULL);
        packet = rtp_packet_next(session, 1, 352, 96);
        CHECK(packet != NULL);
        packet->payload[0] = 0x5a;
        rtp_packet_commit(session, packet);
        CHECK(rtp_packet_get(session, sequence)->payload[0] == 0x5a);
        CHECK(rtp_packet_get(session, sequences[4]) == NULL);
    }
    CHECK(rtp_cover_elapsed(1000, 648, 352LL * 1000000000 / 44100, 44100) == 1000);
    CHECK(rtp_cover_elapsed(1000, 1000, 50000000, 44100) == 1000);
    CHECK(rtp_cover_elapsed(2000, 1000, 1000000, 44100) == 2000);
    CHECK(rtp_cover_elapsed(1000, 1000, 2000000000LL, 44100) == 89200);
    CHECK(rtp_cover_elapsed(1000 + 44100, 1000, 2000000000LL, 44100) == 89200);
    CHECK(rtp_cover_elapsed(0xfffff000u, 0xfffff000u, 1000000000LL, 44100) ==
          (uint32_t)(0xfffff000u + 44100u));
    CHECK(rtp_cover_elapsed(1000, 1000, -1, 44100) == 1000);
    CHECK(rtp_cover_elapsed(1000, 1000, 2000000000LL, 0) == 1000);
    rtp_session_free(session);
    codec_alac_free(codec);
    evbuffer_free(encoded);
    return 0;
}
