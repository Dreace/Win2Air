#include <assert.h>
#include <stdatomic.h>
#define protocol_start established_protocol_start
#define protocol_end established_protocol_end
#define protocol_cleanup established_protocol_cleanup
#include "session.c"
#undef protocol_start
#undef protocol_end
#undef protocol_cleanup
static platform_socket listener;
static uint16_t port;
static sender_command_completion outcome;
static unsigned completed;
static _Atomic unsigned requests;
static struct protocol_peer *current;
static sender_presentation requested;
static int response_code;
static bool only_flush;
static uint32_t word(const uint8_t *data) {
  uint32_t value;
  memcpy(&value, data, 4);
  return ntohl(value);
}
static void *receiver(void *unused) {
  (void)unused;
  platform_socket socket = accept(listener, NULL, NULL);
  assert(socket >= 0);
  if (response_code == -1) {
    char pending[4096];
    while (platform_recv(socket, pending, sizeof(pending), 0) > 0) {
    }
    platform_socket_close(socket);
    return NULL;
  }
  for (unsigned index = 0; index < (only_flush ? 1u : 2u); ++index) {
    char buffer[8192] = {0};
    size_t length = 0, body = 0;
    char *end = NULL;
    do {
      ev_ssize_t amount = platform_recv(socket, buffer + length,
                                        sizeof(buffer) - length - 1, 0);
      assert(amount > 0);
      length += (size_t)amount;
      buffer[length] = 0;
      end = strstr(buffer, "\r\n\r\n");
      char *size = strstr(buffer, "Content-Length:");
      if (end && size)
        body = (size_t)strtoul(size + strlen("Content-Length:"), NULL, 10);
    } while (!end || length < (size_t)(end - buffer) + 4 + body);
    assert(strstr(buffer, index == 0 ? "FLUSH rtsp://127.0.0.1/virtual"
                                     : "SETUP rtsp://127.0.0.1/virtual"));
    char cseq[32];
    char *sequence = strstr(buffer, "CSeq:");
    assert(sequence);
    unsigned number = (unsigned)strtoul(sequence + 5, NULL, 10);
    snprintf(cseq, sizeof(cseq), "%u", number);
    char *payload = NULL;
    uint32_t payload_size = 0;
    if (index == 1) {
      plist_t input = NULL;
      plist_from_bin(end + 4, (uint32_t)body, &input);
      assert(input);
      plist_t stream =
          plist_array_get_item(plist_dict_get_item(input, "streams"), 0);
      uint64_t maximum = 0, minimum = 0;
      plist_get_uint_val(plist_dict_get_item(stream, "latencyMax"), &maximum);
      plist_get_uint_val(plist_dict_get_item(stream, "latencyMin"), &minimum);
      assert(maximum == (uint64_t)(requested.base_ms + requested.offset_ms) *
                            44100 / 1000 &&
             minimum == 11025);
      plist_free(input);
      plist_t root = plist_new_dict(), streams = plist_new_array(),
              reply = plist_new_dict();
      plist_dict_set_item(reply, "dataPort", plist_new_uint(65001));
      plist_dict_set_item(reply, "controlPort", plist_new_uint(65002));
      if (requested.base_ms == 300)
        plist_dict_set_item(reply, "arrivalToRenderLatencyMs",
                            plist_new_uint(69));
      plist_array_append_item(streams, reply);
      plist_dict_set_item(root, "streams", streams);
      plist_to_bin(root, &payload, &payload_size);
      plist_free(root);
    }
    char headers[256];
    int code = index == 0 || response_code == 0 ? 200 : response_code;
    if (code != 200 || response_code == 0) payload_size = 0;
    int size =
        snprintf(headers, sizeof(headers),
                 "RTSP/1.0 %d Reply\r\nCSeq: %s\r\nContent-Length: %u\r\n\r\n",
                 code, cseq, payload_size);
    assert(platform_send(socket, headers, (size_t)size, 0) == size);
    if (payload_size)
      assert(platform_send(socket, payload, payload_size, 0) == payload_size);
    free(payload);
    ++requests;
  }
  char end;
  assert(platform_recv(socket, &end, 1, 0) <= 0);
  platform_socket_close(socket);
  return NULL;
}
int protocol_start(struct event_base *base, struct protocol_peer *peer,
                   uint64_t identity, const char *client, const char *agent) {
  evbase_player = base;
  airplay_device_id = identity;
  airplay_client_name = client;
  airplay_user_agent = agent;
  current = peer;
  struct airplay_session *session = calloc(1, sizeof(*session));
  assert(session);
  session->device_id = peer->id;
  session->state =
      only_flush ? AIRPLAY_STATE_STREAMING : AIRPLAY_STATE_CONNECTED;
  session->devname = strdup("Virtual timing receiver");
  session->address = strdup("127.0.0.1");
  session->family = AF_INET;
  session->server_fd = -1;
  session->cseq = 1;
  session->master_session = master_session_make(&peer->quality, false, peer);
  assert(session->master_session);
  uint8_t queued[148] = {0};
  assert(evbuffer_add(session->master_session->input_buffer, queued,
                      sizeof(queued)) == 0);
  session->master_session->input_buffer_samples = sizeof(queued) / 4;
  protocol_statistic(PROTOCOL_PENDING,
                     session->master_session->input_buffer_samples);
  session->offset_samples = peer->offset_ms * 44100 / 1000;
  session->control_svc = &airplay_control_svc;
  strcpy(session->session_url, "rtsp://127.0.0.1/virtual");
  session->ctrl = evrtsp_connection_new("127.0.0.1", port);
  assert(session->ctrl);
  evrtsp_connection_set_base(session->ctrl, base);
  session->deferredev = evtimer_new(base, deferred_session_failure_cb, session);
  assert(session->deferredev);
  airplay_sessions = session;
  peer->session = session;
  protocol_timing_applied(
      session->master_session->output_buffer_samples, 11025,
      (uint32_t)((peer->presentation_base_ms + peer->offset_ms) * 44100 /
                 1000));
  protocol_state_changed(0, peer->id, OUTPUT_STATE_CONNECTED);
  return 0;
}
int protocol_end(struct protocol_peer *peer) {
  if (peer->session) session_cleanup(peer->session);
  protocol_state_changed(0, peer->id, OUTPUT_STATE_STOPPED);
  return 0;
}
void protocol_cleanup(void) {
  while (airplay_sessions) session_cleanup(airplay_sessions);
  evbase_player = NULL;
  current = NULL;
}
static void completion(void *context, const sender_command_completion *value) {
  (void)context;
  outcome = *value;
  ++completed;
}
static sender_timing_info timing(sender_runtime *runtime, uint64_t generation) {
  sender_timing_info info = {0};
  sender_error error;
  info.size = sizeof(info);
  info.version = 1;
  assert(sender_get_timing(runtime, generation, &info, &error) == SENDER_OK);
  return info;
}
int main(void) {
  plist_t observation = plist_new_dict();
  struct receiver_latency_observation reported =
      receiver_latency_read(observation);
  assert(reported.available == 0 && reported.invalid == 0);
  plist_dict_set_item(observation, "latencyMin", plist_new_uint(0));
  plist_dict_set_item(observation, "latencyMax", plist_new_uint(88200));
  plist_dict_set_item(observation, "arrivalToRenderLatencyMs",
                      plist_new_uint(100));
  reported = receiver_latency_read(observation);
  assert(reported.available == 7 && reported.invalid == 0 &&
         reported.values[0] == 0 && reported.values[1] == 88200 &&
         reported.values[2] == 100);
  plist_dict_set_item(observation, "latencyMin", plist_new_string("11025"));
  reported = receiver_latency_read(observation);
  assert(reported.available == 6 && reported.invalid == 1);
  plist_dict_set_item(observation, "arrivalToRenderLatencyMs",
                      plist_new_real(69));
  reported = receiver_latency_read(observation);
  assert(reported.available == 2 && reported.invalid == 5);
  plist_free(observation);
  observation = plist_new_array();
  reported = receiver_latency_read(observation);
  assert(reported.available == 0 && reported.invalid == 0);
  plist_free(observation);
  sender_create_options options = {0};
  options.version = SENDER_ABI_VERSION;
  options.size = sizeof(options);
  options.options.sender_id = 1;
  options.options.client_name = options.options.user_agent =
      "Presentation consumer";
  options.options.audio_capacity_bytes = 4096;
  options.initial_presentation.base_ms = 300;
  options.on_command = completion;
  sender_runtime *runtime = NULL;
  sender_error error;
  uint64_t generation;
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_OK);
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(listener >= 0);
  struct sockaddr_in local = {0};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, (struct sockaddr *)&local, sizeof(local)) == 0 &&
         listen(listener, 1) == 0);
  socklen_t size = sizeof(local);
  assert(getsockname(listener, (struct sockaddr *)&local, &size) == 0);
  port = ntohs(local.sin_port);
  sender_target target = {
      "127.0.0.1",
      port,
      "AA:BB:CC:DD:EE:FF",
      "Virtual",
      (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48),
      0,
      50};
  sender_audio_format format = {44100, 16, 2};
  const sender_presentation candidates[] = {{300, 0},  {2000, -120}, {1000, 0},
                                            {1500, 0}, {1000, 100},  {1000, 0},
                                            {1000, 0}};
  const int responses[] = {200, 200, 451, 0, -1, 400, 500};
  for (unsigned index = 0; index < 7; ++index) {
    requested = candidates[index];
    response_code = responses[index];
    platform_thread worker;
    assert(platform_thread_start(&worker, receiver, NULL) == 0);
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                          &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    sender_snapshot metrics = {0};
    metrics.size = sizeof(metrics);
    metrics.version = 1;
    assert(sender_get_snapshot(runtime, generation, &metrics, &error) ==
               SENDER_OK &&
           metrics.pending_samples == 37);
    sender_timing_info previous = timing(runtime, generation);
    assert(!previous.receiver_render_available && !previous.receiver_render_ms);
    if (index == 0) {
      sender_presentation initial = {290, 0}, original = {300, 0};
      assert(!((struct airplay_session *)current->session)
                  ->master_session->stamp_valid);
      assert(sender_retarget_presentation(runtime, generation, &initial,
                                          &error) == SENDER_OK);
      assert(timing(runtime, generation).base_ms == 290);
      assert(sender_retarget_presentation(runtime, generation, &original,
                                          &error) == SENDER_OK);
    }
    sender_presentation invalid = {20, -1};
    assert(sender_set_presentation(runtime, generation, index + 1, &invalid,
                                   &error) == SENDER_INPUT);
    assert(sender_set_presentation(runtime, generation + 1, index + 1,
                                   &requested, &error) == SENDER_STATE);
    assert(sender_set_presentation(runtime, generation, index + 1, &requested,
                                   &error) == SENDER_OK);
    assert(completed == index &&
           timing(runtime, generation).base_ms == previous.base_ms);
    for (unsigned wait = 0; completed <= index; ++wait) {
      assert(wait < 500);
      assert(sender_poll(runtime, 5, &error) == SENDER_OK);
    }
    sender_timing_info applied = timing(runtime, generation);
    assert(outcome.kind == SENDER_COMMAND_PRESENTATION &&
           outcome.command_id == index + 1);
    if (response_code == 200) {
      assert(applied.receiver_render_available == (requested.base_ms == 300));
      assert(applied.receiver_render_ms ==
             (requested.base_ms == 300 ? 69u : 0u));
      assert(sender_get_snapshot(runtime, generation, &metrics, &error) ==
                 SENDER_OK &&
             metrics.pending_samples == 0);
      assert(outcome.result == SENDER_OK &&
             applied.base_ms == requested.base_ms &&
             applied.offset_ms == requested.offset_ms);
      assert(
          applied.setup_latency_max_samples ==
          (uint32_t)((requested.base_ms + requested.offset_ms) * 44100 / 1000));
      struct airplay_session *session = current->session;
      struct timespec stamp = {1, 0};
      timestamp_set(session->master_session, stamp);
      struct rtcp_timestamp cur = session->master_session->cur_stamp;
      cur.pos -= session->offset_samples;
      struct rtp_packet *packet =
          rtp_sync_packet_next(session->master_session->rtp_session, cur, 0x90);
      assert(packet);
      assert(
          session->master_session->rtp_session->pos - word(packet->data + 4) +
              applied.setup_latency_min_samples ==
          (uint32_t)((requested.base_ms + requested.offset_ms) * 44100 / 1000));
    } else {
      assert(outcome.result == (response_code > 0    ? SENDER_NETWORK
                                : response_code == 0 ? SENDER_CAPABILITY
                                                     : SENDER_TIMEOUT));
      assert(outcome.error.peer_code == (response_code > 0    ? response_code
                                         : response_code == 0 ? 200
                                                              : 0));
      assert(applied.base_ms == previous.base_ms &&
             applied.offset_ms == previous.offset_ms);
      assert(sender_get_state(runtime, generation) == SENDER_FAILED);
    }
    sender_result ended = sender_end(runtime, generation, &error);
    assert(ended == SENDER_OK || ended == SENDER_ALREADY_ENDED);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    assert(platform_thread_join(worker) == 0);
  }
  only_flush = true;
  response_code = 200;
  platform_thread flush_worker;
  assert(platform_thread_start(&flush_worker, receiver, NULL) == 0);
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  sender_snapshot flushed = {0};
  flushed.size = sizeof(flushed);
  flushed.version = 1;
  assert(sender_get_snapshot(runtime, generation, &flushed, &error) ==
             SENDER_OK &&
         flushed.pending_samples == 37);
  assert(sender_flush(runtime, generation, &error) == SENDER_OK);
  for (unsigned wait = 0;
       requests < 9 || sender_get_state(runtime, generation) != SENDER_READY;
       ++wait) {
    assert(wait < 500);
    assert(sender_poll(runtime, 5, &error) == SENDER_OK);
  }
  assert(sender_get_snapshot(runtime, generation, &flushed, &error) ==
             SENDER_OK &&
         flushed.pending_samples == 0);
  assert(sender_end(runtime, generation, &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(platform_thread_join(flush_worker) == 0);
  only_flush = false;
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_set_presentation(runtime, generation, 1, &requested, &error) ==
         SENDER_OK);
  assert(sender_end(runtime, generation, &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(outcome.result == SENDER_CANCELED && completed == 8);
  assert(requests == 13 && sender_destroy(runtime, &error) == SENDER_OK);
  platform_socket_close(listener);
  return 0;
}
