/* Extracted ALAC configuration from OwnTone transcode.c, GPL-2.0-or-later.
 * Preserve S16P, raw packets, sample time base and 352-sample frame size. */
#include "codec.h"
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

struct sender_codec { AVCodecContext *context; };

void codec_alac_free(struct sender_codec *codec)
{
    if (!codec) return;
    avcodec_free_context(&codec->context);
    free(codec);
}

struct sender_codec *codec_alac_new(const struct media_quality *quality)
{
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_ALAC);
    struct sender_codec *codec = calloc(1, sizeof(*codec));
    if (!encoder || !codec) { free(codec); return NULL; }
    codec->context = avcodec_alloc_context3(encoder);
    if (!codec->context) { free(codec); return NULL; }
    codec->context->sample_rate = quality->sample_rate;
    codec->context->sample_fmt = AV_SAMPLE_FMT_S16P;
    codec->context->channel_layout = AV_CH_LAYOUT_STEREO;
    codec->context->channels = quality->channels;
    codec->context->time_base = (AVRational){ 1, quality->sample_rate };
    codec->context->bit_rate = quality->bit_rate;
    if (avcodec_open2(codec->context, encoder, NULL) < 0) { codec_alac_free(codec); return NULL; }
    codec->context->frame_size = 352;
    return codec;
}

int codec_alac_encode(struct evbuffer *output, struct sender_codec *codec, const uint8_t *pcm,
                      size_t bytes, int samples, const struct media_quality *quality)
{
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    int result = -1;
    if (!frame || !packet || !codec || !pcm || samples != 352 || bytes != (size_t)samples * 4 ||
        quality->sample_rate != 44100 || quality->bits_per_sample != 16 || quality->channels != 2)
        goto done;
    frame->format = AV_SAMPLE_FMT_S16P;
    frame->sample_rate = quality->sample_rate;
    frame->nb_samples = samples;
    frame->channel_layout = AV_CH_LAYOUT_STEREO;
    frame->channels = 2;
    if (av_frame_get_buffer(frame, 0) < 0) goto done;
    for (int i = 0; i < samples; ++i) {
        memcpy(frame->data[0] + i * 2, pcm + i * 4, 2);
        memcpy(frame->data[1] + i * 2, pcm + i * 4 + 2, 2);
    }
    if (avcodec_send_frame(codec->context, frame) < 0 || avcodec_receive_packet(codec->context, packet) < 0)
        goto done;
    if (evbuffer_add(output, packet->data, packet->size) < 0) goto done;
    result = packet->size;
done:
    av_packet_free(&packet);
    av_frame_free(&frame);
    return result;
}
