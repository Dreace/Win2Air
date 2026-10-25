#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <gcrypt.h>
#include <plist/plist.h>
#include <sys/queue.h>

#include "codec.c"
#include "evrtsp.h"
#include "pairing/pair.h"
#include "rtp_common.h"
#include "rtsp-internal.h"
static platform_socket listener;
static int mode, completed;
static struct event_base *base;
static int cipher_response(struct evbuffer *output, struct evbuffer *input,
                           void *context, int encrypt) {
  uint8_t *converted = NULL;
  size_t length = 0, input_length = evbuffer_get_length(input);
  const uint8_t *bytes = evbuffer_pullup(input, -1);
  ev_ssize_t consumed =
      encrypt ? pair_encrypt(&converted, &length, bytes, input_length, context)
              : pair_decrypt(&converted, &length, bytes, input_length, context);
  assert(consumed >= 0);
  if (length) assert(!evbuffer_add(output, converted, length));
  free(converted);
  assert(!evbuffer_drain(input, (size_t)consumed));
  return 0;
}
static void *peer(void *unused) {
  (void)unused;
  platform_socket fd = accept(listener, NULL, NULL);
  assert(fd >= 0);
  char request[1024];
  assert(platform_recv(fd, request, sizeof(request), 0) > 0);
  const char *response =
      mode == 2   ? "RTSP/1.0 invalid\r\n\r\n"
      : mode == 3 ? "RTSP/1.0 20"
      : mode == 4 ? "RTSP/1.0 200 OK\r\nContent-Length: 4\r\nX-Unfinished:"
      : mode == 6
          ? "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n"
          : "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 4\r\n\r\nDATA";
  const uint8_t truncated_cipher[] = {8, 0, 1, 2, 3};
  if (mode == 7)
    response = "RTSP/1.0 200 OK\r\nContent-Length: 1048577\r\n\r\n";
  if (mode == 8)
    response = "RTSP/1.0 200 OK\r\nContent-Length: 4junk\r\n\r\nDATA";
  if (mode == 9)
    response =
        "RTSP/1.0 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n";
  size_t length = strlen(response), split = mode >= 3 ? length : length - 2;
  if (mode == 5) {
    response = (const char *)truncated_cipher;
    split = sizeof(truncated_cipher);
  }
  assert(platform_send(fd, response, split, 0) == (ev_ssize_t)split);
  if (!mode) {
#ifdef _WIN32
    Sleep(10);
#else
    struct timespec tick = {0, 10000000};
    nanosleep(&tick, NULL);
#endif
    assert(platform_send(fd, response + split, 2, 0) == 2);
  }
  platform_socket_close(fd);
  return NULL;
}
static void response(struct evrtsp_request *request, void *unused) {
  (void)unused;
  if (mode == 6)
    assert(request && request->response_code == 200 &&
           !evbuffer_get_length(request->input_buffer));
  else if (!mode) {
    assert(request && request->response_code == 200 &&
           evbuffer_get_length(request->input_buffer) == 4);
    uint8_t bytes[4];
    assert(evbuffer_remove(request->input_buffer, bytes, 4) == 4 &&
           !memcmp(bytes, "DATA", 4));
  } else {
#ifdef FROZEN_RTSP_BEHAVIOR
    if (mode == 1 || mode == 4)
      assert(request && request->response_code == 200 &&
             evbuffer_get_length(request->input_buffer) != 4);
    else if (mode == 3 || mode == 5)
      assert(request && !request->response_code);
    else
      assert(!request);
#else
    assert(!request);
#endif
  }
  ++completed;
  event_base_loopbreak(base);
}
static void rtsp_units(void) {
  for (mode = 0; mode < 11; ++mode) {
    listener = platform_socket_open(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!platform_bind(listener, (struct sockaddr *)&address,
                          sizeof(address)) &&
           !listen(listener, 1));
    socklen_t length = sizeof(address);
    assert(!getsockname(listener, (struct sockaddr *)&address, &length));
    platform_thread worker;
    assert(!platform_thread_start(&worker, peer, NULL));
    base = event_base_new();
    assert(base);
    struct evrtsp_connection *connection =
        evrtsp_connection_new("127.0.0.1", ntohs(address.sin_port));
    assert(connection);
    evrtsp_connection_set_base(connection, base);
    struct pair_cipher_context *cipher = NULL;
    if (mode == 5) {
      uint8_t public_secret[32] = {0};
      cipher = pair_cipher_new(PAIR_CLIENT_HOMEKIT_NORMAL, PAIR_CHANNEL_CONTROL,
                               public_secret, sizeof(public_secret), NULL);
      assert(cipher);
      evrtsp_connection_set_ciphercb(connection, cipher_response, cipher);
    }
    struct evrtsp_request *request =
        evrtsp_request_new(mode == 10 ? NULL : response, NULL);
    assert(request);
    assert(!evrtsp_add_header(request->output_headers, "X-Protocol-Test",
                              "fragmented-response"));
    assert(
        !strcmp(evrtsp_find_header(request->output_headers, "X-Protocol-Test"),
                "fragmented-response"));
    assert(!evrtsp_make_request(connection, request, EVRTSP_REQ_GET, "/info"));
    struct timeval deadline = {2, 0};
    event_base_loopexit(base, &deadline);
    assert(event_base_dispatch(base) >= 0);
    assert(completed == (mode == 10 ? 10 : mode + 1));
    evrtsp_connection_free(connection);
    pair_cipher_free(cipher);
    event_base_free(base);
    assert(!platform_thread_join(worker));
    platform_socket_close(listener);
  }
}
static void rtsp_bounds(void) {
  struct evrtsp_request *request = evrtsp_request_new(NULL, NULL);
  assert(request);
  request->kind = EVRTSP_RESPONSE;
  char line[RTSP_MAX_LINE_BYTES + 1];
  memset(line, 'A', sizeof(line));
  assert(!evbuffer_add(request->input_buffer, line, sizeof(line)));
  assert(evrtsp_parse_firstline(request, request->input_buffer) ==
         DATA_CORRUPTED);
  evbuffer_drain(request->input_buffer,
                 evbuffer_get_length(request->input_buffer));
  for (unsigned i = 0; i < 10000; ++i) {
    assert(evbuffer_add_printf(request->input_buffer, "X-Field: value\r\n") >
           0);
    enum message_read_status result =
        evrtsp_parse_headers(request, request->input_buffer);
    if (result == DATA_CORRUPTED) break;
    assert(i < 9999);
  }
  assert(request->header_bytes <= RTSP_MAX_HEADER_BYTES);
  evrtsp_request_free(request);
  struct evrtsp_connection *connection =
      evrtsp_connection_new("127.0.0.1", 7000);
  assert(connection);
  connection->family = -1;
  request = evrtsp_request_new(NULL, NULL);
  assert(request &&
         evrtsp_make_request(connection, request, EVRTSP_REQ_GET, "/info") < 0);
  assert(TAILQ_EMPTY(&connection->requests) && !request->evcon);
  evrtsp_request_free(request);
  evrtsp_connection_free(connection);
}
int main(void) {
  assert(platform_init() == 0 && gcry_check_version(NULL));
  const char binary_pattern[] = {0, 1, 2, 0, 3};
  assert(protocol_bytes_find(binary_pattern, sizeof(binary_pattern),
                             binary_pattern + 2, 2) == binary_pattern + 2);
  assert(!protocol_bytes_find(binary_pattern, sizeof(binary_pattern), "missing",
                              7));
  assert(protocol_bytes_find(binary_pattern, sizeof(binary_pattern), "", 0) ==
         binary_pattern);
  plist_t dictionary = plist_new_dict();
  plist_dict_set_item(dictionary, "sr", plist_new_uint(44100));
  plist_dict_set_item(dictionary, "spf", plist_new_uint(352));
  char *binary = NULL;
  uint32_t length = 0;
  plist_to_bin(dictionary, &binary, &length);
  assert(binary && length > 8);
  plist_t parsed = NULL;
  plist_from_bin(binary, length, &parsed);
  assert(parsed);
  uint64_t value;
  plist_get_uint_val(plist_dict_get_item(parsed, "spf"), &value);
  assert(value == 352);
  plist_free(parsed);
  parsed = NULL;
  plist_from_bin(binary, 7, &parsed);
  assert(!parsed);
  free(binary);
  plist_free(dictionary);
  struct media_quality quality = {44100, 16, 2, 0};
  struct sender_codec *encoder = codec_alac_new(&quality);
  assert(encoder);
  uint8_t pcm[1408];
  for (size_t i = 0; i < sizeof(pcm); ++i) pcm[i] = (uint8_t)(i * 31 + 17);
  struct evbuffer *encoded = evbuffer_new();
  assert(encoded);
  int bytes =
      codec_alac_encode(encoded, encoder, pcm, sizeof(pcm), 352, &quality);
  assert(bytes > 0);
  AVCodecContext *decoder =
      avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_ALAC));
  assert(decoder);
  decoder->extradata = av_mallocz((size_t)encoder->context->extradata_size +
                                  AV_INPUT_BUFFER_PADDING_SIZE);
  assert(decoder->extradata);
  decoder->extradata_size = encoder->context->extradata_size;
  memcpy(decoder->extradata, encoder->context->extradata,
         (size_t)decoder->extradata_size);
  assert(avcodec_open2(decoder, avcodec_find_decoder(AV_CODEC_ID_ALAC), NULL) >=
         0);
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  assert(packet && frame && !av_new_packet(packet, bytes));
  assert(evbuffer_remove(encoded, packet->data, (size_t)bytes) == bytes &&
         !avcodec_send_packet(decoder, packet) &&
         !avcodec_receive_frame(decoder, frame));
  assert(frame->nb_samples == 352 && frame->format == AV_SAMPLE_FMT_S16P);
  for (int i = 0; i < 352; ++i) {
    assert(!memcmp(frame->data[0] + i * 2, pcm + i * 4, 2));
    assert(!memcmp(frame->data[1] + i * 2, pcm + i * 4 + 2, 2));
  }
  assert(codec_alac_encode(encoded, encoder, pcm, sizeof(pcm) - 1, 352,
                           &quality) < 0 &&
         !evbuffer_get_length(encoded));
  assert(codec_alac_encode(encoded, encoder, pcm, sizeof(pcm), 351, &quality) <
             0 &&
         !evbuffer_get_length(encoded));
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoder);
  evbuffer_free(encoded);
  codec_alac_free(encoder);
  struct rtcp_packet feedback;
  uint8_t valid[] = {0x80, 201, 0, 1, 0, 0, 0, 1};
  assert(!rtcp_packet_parse(&feedback, valid, sizeof(valid)) &&
         feedback.ssrc == 1);
  for (size_t size = 0; size < sizeof(valid); ++size)
    assert(rtcp_packet_parse(&feedback, valid, size) < 0);
  valid[0] = 0;
  assert(rtcp_packet_parse(&feedback, valid, sizeof(valid)) < 0);
  rtsp_bounds();
  rtsp_units();
  platform_cleanup();
  puts(
      "plist, ALAC round-trip, partial frames, RTCP truncation and fragmented "
      "RTSP responses verified");
  return 0;
}
