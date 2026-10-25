#ifndef SENDER_CODEC_H
#define SENDER_CODEC_H
#include "protocol.h"
struct sender_codec;
struct sender_codec *codec_alac_new(const struct media_quality *);
void codec_alac_free(struct sender_codec *);
int codec_alac_encode(struct evbuffer *, struct sender_codec *, const uint8_t *, size_t, int, const struct media_quality *);
#endif
