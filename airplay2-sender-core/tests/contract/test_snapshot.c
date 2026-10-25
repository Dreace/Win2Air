#include <assert.h>
#include <stdatomic.h>
#define protocol_start established_protocol_start
#define protocol_end established_protocol_end
#define protocol_cleanup established_protocol_cleanup
#define protocol_write established_protocol_write
#include "session.c"
#undef protocol_start
#undef protocol_end
#undef protocol_cleanup
#undef protocol_write
static platform_socket listener;
static platform_socket control_listener;
static uint16_t control_port;
static _Atomic uint32_t flushed_position;
static struct protocol_peer *peer_current;
static unsigned logs;
static bool measure_latency;
static uint32_t read_word(const uint8_t *data) {
  return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
         (uint32_t)data[2] << 8 | data[3];
}
static uint64_t read_time(const uint8_t *data) {
  uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) value = value << 8 | data[index];
  return value;
}
static uint64_t render_time(const uint8_t *sync, uint32_t position,
                            uint32_t render) {
  int64_t delta = (int32_t)(position - read_word(sync + 4));
  return (uint64_t)((int64_t)read_time(sync + 8) +
                    delta * INT64_C(1000000000) / 44100 +
                    (int64_t)render * INT64_C(1000000000) / 44100);
}
static void heartbeat(evutil_socket_t fd, short events, void *context) {
  (void)fd;
  (void)events;
  (void)context;
}
static void log_event(void *context, const sender_log_record *record) {
  sender_runtime *runtime = *(sender_runtime **)context;
  assert(record->size == sizeof(*record) && record->version == 1 &&
         record->event <= 2);
  sender_snapshot info = {0};
  sender_error error;
  info.size = sizeof(info);
  info.version = 1;
  assert(sender_get_snapshot(runtime, record->generation, &info, &error) ==
         SENDER_STATE);
  ++logs;
}
int protocol_start(struct event_base *base, struct protocol_peer *peer,
                   uint64_t identity, const char *client, const char *agent) {
  airplay_client_name = client;
  airplay_user_agent = agent;
  evbase_player = base;
  airplay_device_id = identity;
  peer_current = peer;
  keep_alive_timer = evtimer_new(base, heartbeat, NULL);
  assert(keep_alive_timer);
  struct airplay_session *session = calloc(1, sizeof(*session));
  assert(session);
  session->device_id = peer->id;
  session->devname = strdup("Virtual statistics receiver");
  session->address = strdup("127.0.0.1");
  session->family = AF_INET;
  session->state = AIRPLAY_STATE_CONNECTED;
  strcpy(session->session_url, "rtsp://127.0.0.1/virtual");
  session->master_session = master_session_make(&peer->quality, false, peer);
  assert(session->master_session);
  if (measure_latency)
    session->master_session->rtp_session->ptp_clock_id =
        UINT64_C(0x123456789abcdef0);
  session->server_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  assert(session->server_fd >= 0);
  struct sockaddr_in address;
  socklen_t length = sizeof(address);
  assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
  assert(connect(session->server_fd, (struct sockaddr *)&address, length) == 0);
  session->naddr.sin = address;
  session->control_port = ntohs(address.sin_port);
  session->control_svc = &airplay_control_svc;
  airplay_control_svc.socket.fd4 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  airplay_control_svc.socket.fd6 = -1;
  session->deferredev = evtimer_new(base, deferred_session_failure_cb, session);
  session->ctrl = evrtsp_connection_new("127.0.0.1", control_port);
  assert(session->deferredev && session->ctrl);
  evrtsp_connection_set_base(session->ctrl, base);
  airplay_sessions = session;
  peer->session = session;
  protocol_timing_applied(
      session->master_session->output_buffer_samples,
      session->master_session->rtp_session->render_samples,
      receiver_window_samples(
          (uint32_t)((peer->presentation_base_ms + peer->offset_ms) * 44100 /
                     1000)));
  protocol_state_changed(0, peer->id, OUTPUT_STATE_CONNECTED);
  return 0;
}
int protocol_write(const void *pcm, size_t bytes, struct timespec time) {
  assert(established_protocol_write(pcm, bytes, time) == 0);
  struct airplay_session *session = peer_current->session;
  uint16_t last = session->master_session->rtp_session->seqnum - 1;
  packets_resend(session, last, 1);
  packets_resend(
      session,
      (uint16_t)(last - session->master_session->rtp_session->pktbuf_size - 1),
      1);
  return 0;
}
int protocol_end(struct protocol_peer *peer) {
  if (peer->session) session_cleanup(peer->session);
  protocol_state_changed(0, peer->id, OUTPUT_STATE_STOPPED);
  return 0;
}
void protocol_cleanup(void) {
  while (airplay_sessions) session_cleanup(airplay_sessions);
  if (keep_alive_timer) event_free(keep_alive_timer);
  keep_alive_timer = NULL;
  net_socket_close(&airplay_control_svc.socket);
  evbase_player = NULL;
}
static void verify_flush_anchor(void) {
  struct media_quality quality = {44100, 16, 2, 0};
  struct airplay_master_session *master = calloc(1, sizeof(*master));
  assert(master);
  master->rtp_session = rtp_session_new(&quality, 8, 0, 1);
  assert(master->rtp_session);
  master->quality = quality;
  master->rtp_session->pos = 1000000;
  master->rtp_session->render_samples = 352;
  master->output_buffer_samples = 44100 - 352;
  master->use_ptp = true;
  struct timespec playing = {10, 0};
  timestamp_set(master, playing);
  assert(master->rtp_session->pos == 1000000 && master->stamp_valid);
  struct timespec covered = {40, 0};
  flush_rtp_timeline(master, covered);
  uint32_t announced = master->rtp_session->pos;
  assert(announced == 1000000u + 30u * 44100u && !master->stamp_valid);
  struct timespec late = {40, 250000000};
  timestamp_set(master, late);
  assert(master->rtp_session->pos == announced && master->stamp_valid &&
         master->cur_stamp.ts.tv_sec == 40 &&
         master->cur_stamp.ts.tv_nsec == 250000000);
  struct timespec later = {50, 0};
  flush_rtp_timeline(master, later);
  uint32_t held = master->rtp_session->pos;
  assert(held ==
         rtp_cover_elapsed(announced, announced, INT64_C(9750000000), 44100));
  struct timespec early = {49, 900000000};
  timestamp_set(master, early);
  assert(master->rtp_session->pos == held &&
         master->cur_stamp.ts.tv_sec == 49 &&
         master->cur_stamp.ts.tv_nsec == 900000000);
  struct rtp_packet *sync =
      rtp_sync_packet_next(master->rtp_session, master->cur_stamp, 0x90);
  assert(sync && sync->data);
  uint64_t rendered = render_time(sync->data, held, 352);
  uint64_t audio_ns = 49ull * 1000000000ull + 900000000ull;
  uint64_t expected = audio_ns + 1000000000ull;
  assert(rendered + 50000 >= expected && expected + 50000 >= rendered);
  rtp_session_free(master->rtp_session);
  free(master);
}
static void *receive_flush(void *context) {
  (void)context;
  platform_socket connection = accept(control_listener, NULL, NULL);
  assert(connection >= 0);
  char request[4096] = {0};
  size_t length = 0;
  while (!strstr(request, "\r\n\r\n")) {
    ev_ssize_t count = platform_recv(connection, request + length,
                                     sizeof(request) - length - 1, 0);
    assert(count > 0);
    length += (size_t)count;
    request[length] = 0;
  }
  assert(strncmp(request, "FLUSH ", 6) == 0);
  char *info = strstr(request, "RTP-Info: ");
  unsigned sequence, position;
  assert(info && sscanf(info, "RTP-Info: seq=%u;rtptime=%u", &sequence,
                        &position) == 2);
  assert(sequence == UINT16_MAX);
  flushed_position = position;
  char *cseq = strstr(request, "CSeq:");
  assert(cseq);
  unsigned number = (unsigned)strtoul(cseq + 5, NULL, 10);
  char response[128];
  int size = snprintf(
      response, sizeof(response),
      "RTSP/1.0 200 OK\r\nCSeq: %u\r\nContent-Length: 0\r\n\r\n", number);
  assert(platform_send(connection, response, (size_t)size, 0) == size);
  char end;
  assert(platform_recv(connection, &end, 1, 0) <= 0);
  platform_socket_close(connection);
  return NULL;
}
static void verify_quiet_retarget(void) {
  struct airplay_session *session = peer_current->session;
  struct airplay_master_session *master = session->master_session;
  sender_presentation original = {peer_current->presentation_base_ms,
                                  peer_current->offset_ms};
  if (original.base_ms <= SENDER_MIN_PRESENTATION_MS) return;
  sender_presentation lower = {original.base_ms - 10, original.offset_ms};
  uint8_t pcm[352 * 4];
  memset(pcm, 0x11, sizeof(pcm));
  struct timespec stamp = master->cur_stamp.ts;
  assert(established_protocol_write(pcm, sizeof(pcm), stamp) == 0);
  assert(master->silent_samples == 0);
  uint32_t buffer_samples = master->output_buffer_samples;
  uint32_t position = master->rtp_session->pos;
  struct rtp_packet *sync =
      rtp_sync_packet_next(master->rtp_session, master->cur_stamp, 0x80);
  uint64_t before =
      render_time(sync->data, position, master->rtp_session->render_samples);
  assert(protocol_retarget_presentation(peer_current, &lower) < 0);
  sync = rtp_sync_packet_next(master->rtp_session, master->cur_stamp, 0x80);
  assert(render_time(sync->data, position,
                     master->rtp_session->render_samples) == before);
  assert(master->output_buffer_samples == buffer_samples &&
         peer_current->presentation_base_ms == original.base_ms);
  memset(pcm, 0, sizeof(pcm));
  uint64_t quiet_needed =
      10u * 44100 / 1000 + master->rtp_session->render_samples +
      (uint64_t)peer_current->buffers.safety_ms * 44100 / 1000 + 352;
  unsigned packets = (unsigned)(quiet_needed / 352 + 1);
  for (unsigned index = 0; index < packets; ++index) {
    stamp.tv_nsec += (long)(352ull * 1000000000 / 44100);
    if (stamp.tv_nsec >= 1000000000L) {
      ++stamp.tv_sec;
      stamp.tv_nsec -= 1000000000L;
    }
    assert(established_protocol_write(pcm, sizeof(pcm), stamp) == 0);
  }
  assert(master->silent_samples >= quiet_needed);
  // Even one pending voiced sample prevents consuming the silent boundary.
  uint8_t voiced[4] = {1, 0, 0, 0};
  assert(evbuffer_add(master->input_buffer, voiced, sizeof(voiced)) == 0);
  assert(protocol_retarget_presentation(peer_current, &lower) < 0);
  evbuffer_drain(master->input_buffer, sizeof(voiced));
  assert(protocol_retarget_presentation(peer_current, &lower) >= 0);
  assert(peer_current->presentation_base_ms == lower.base_ms);
  assert(protocol_retarget_presentation(peer_current, &original) >= 0);
  for (;;) {
    fd_set read;
    FD_ZERO(&read);
    FD_SET(listener, &read);
    struct timeval wait = {0, 20000};
    if (select((int)listener + 1, &read, NULL, NULL, &wait) <= 0) break;
    uint8_t packet[8192];
    assert(platform_recv(listener, packet, sizeof(packet), 0) > 0);
  }
}
static void verify_standby_resume(sender_runtime *runtime,
                                  uint64_t generation) {
  struct airplay_session *session = peer_current->session;
  struct airplay_master_session *master = session->master_session;
  struct timespec now;
  assert(platform_time(PLATFORM_MONOTONIC, &now) == 0);
  master->rtp_session->seqnum = UINT16_MAX;
  master->pos_at_stamp = master->rtp_session->pos;
  uint32_t old_position = master->rtp_session->pos;
  master->cur_stamp.ts = now;
  master->cur_stamp.ts.tv_sec -= 30;
  master->stamp_valid = true;
  platform_thread worker;
  assert(platform_thread_start(&worker, receive_flush, NULL) == 0);
  sender_error error;
  assert(sender_flush(runtime, generation, &error) == SENDER_OK);
  for (unsigned attempt = 0;
       sender_get_state(runtime, generation) != SENDER_READY; ++attempt) {
    assert(attempt < 400);
    assert(sender_poll(runtime, 5, &error) == SENDER_OK);
  }
  assert(master->rtp_session->seqnum == UINT16_MAX);
  assert(master->silent_samples == 0);
  assert((uint32_t)(master->rtp_session->pos - old_position) >= 30u * 44100u);
  assert(platform_time(PLATFORM_MONOTONIC, &now) == 0);
  uint64_t capture = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec -
                     352ull * 1000000000 / 44100;
  sender_audio_format format = {44100, 16, 2};
  sender_timed_audio timing = {sizeof(timing), 1, 2, 0, capture};
  uint8_t pcm[352 * 4] = {0};
  assert(sender_submit_timed(runtime, generation, &timing, &format, pcm,
                             sizeof(pcm) / 2, &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(session->state == AIRPLAY_STATE_CONNECTED);
  assert(sender_flush(runtime, generation, &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_get_state(runtime, generation) == SENDER_READY);
  assert(evbuffer_get_length(master->input_buffer) == 0 &&
         master->input_buffer_samples == 0);
  timing.timeline_epoch = 3;
  assert(sender_submit_timed(runtime, generation, &timing, &format, pcm,
                             sizeof(pcm) / 2, &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  sender_timed_audio remainder = timing;
  remainder.sample_index = 176;
  remainder.capture_monotonic_ns += 176ull * 1000000000 / 44100;
  assert(sender_submit_timed(runtime, generation, &remainder, &format,
                             pcm + sizeof(pcm) / 2, sizeof(pcm) / 2,
                             &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  timing.sample_index = 352;
  timing.capture_monotonic_ns += 352ull * 1000000000 / 44100;
  memset(pcm, 0x11, sizeof(pcm));
  assert(sender_submit_timed(runtime, generation, &timing, &format, pcm,
                             sizeof(pcm), &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(master->rtp_session->seqnum == 1);
  unsigned boundary = 0, voice = 0;
  bool resumed_sync = false;
  for (;;) {
    fd_set read;
    FD_ZERO(&read);
    FD_SET(listener, &read);
    struct timeval wait = {0, 20000};
    if (select((int)listener + 1, &read, NULL, NULL, &wait) <= 0) break;
    uint8_t packet[8192];
    int size = platform_recv(listener, packet, sizeof(packet), 0);
    assert(size > 0);
    if (measure_latency && size == 28 && packet[1] == 0xd7) {
      uint64_t output = render_time(packet, flushed_position,
                                    master->rtp_session->render_samples);
      sender_timing_info negotiated = {0};
      negotiated.size = sizeof(negotiated);
      negotiated.version = 1;
      assert(sender_get_timing(runtime, generation, &negotiated, &error) ==
             SENDER_OK);
      uint64_t expected = capture + (uint64_t)negotiated.effective_ms * 1000000;
      assert(output + 23000 >= expected && expected + 23000 >= output);
      resumed_sync = true;
    }
    if ((packet[1] & 0x7f) != AIRPLAY_RTP_PAYLOADTYPE) continue;
    unsigned sequence = (unsigned)packet[2] << 8 | packet[3];
    if (sequence == UINT16_MAX) {
      assert(read_word(packet + 4) == flushed_position);
      if (!boundary) assert(packet[1] & RTP_MARKER_BIT);
      ++boundary;
    } else {
      assert(sequence == 0 && read_word(packet + 4) == flushed_position + 352);
      ++voice;
    }
  }
  assert(boundary == 2 && voice == 2);
  assert(!measure_latency || resumed_sync);
  assert(sender_end(runtime, generation, &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(platform_thread_join(worker) == 0);
}
static void verify_ipv4_control_bind(void) {
  struct net_socket socket = {-1, -1};
  unsigned short port = 0;
  assert(net_bind(&socket, &port, SOCK_DGRAM, "control-bind") == 0);
  assert(socket.fd4 >= 0 && port != 0);
  net_socket_close(&socket);
}
int main(int argc, char **argv) {
  assert(argc == 1 || argc == 2);
  measure_latency = argc == 2;
  assert(platform_init() == 0);
  verify_ipv4_control_bind();
  verify_flush_anchor();
  listener = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  assert(listener >= 0);
  struct sockaddr_in local = {0};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, (struct sockaddr *)&local, sizeof(local)) == 0);
  control_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(control_listener >= 0);
  assert(bind(control_listener, (struct sockaddr *)&local, sizeof(local)) == 0);
  assert(listen(control_listener, 1) == 0);
  socklen_t control_length = sizeof(local);
  assert(getsockname(control_listener, (struct sockaddr *)&local,
                     &control_length) == 0);
  control_port = ntohs(local.sin_port);
  sender_runtime *runtime = NULL;
  sender_error error;
  sender_create_options options = {0};
  options.version = SENDER_ABI_VERSION;
  options.size = sizeof(options);
  options.options.sender_id = 1;
  options.options.client_name = options.options.user_agent =
      "Snapshot consumer";
  options.options.audio_capacity_bytes = 4096;
  options.options.context = &runtime;
  options.initial_presentation.base_ms = measure_latency ? atoi(argv[1]) : 1000;
  if (measure_latency) {
    assert(options.initial_presentation.base_ms >= SENDER_MIN_PRESENTATION_MS &&
           options.initial_presentation.base_ms <= SENDER_MAX_PRESENTATION_MS);
    options.buffers = (sender_buffer_config){0, 8, 2, 50};
  }
  options.on_log = log_event;
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_OK);
  sender_target target = {
      "127.0.0.1",
      65000,
      "AA:BB:CC:DD:EE:FF",
      "Virtual",
      (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48),
      0,
      50};
  sender_audio_format format = {44100, 16, 2};
  uint64_t generation;
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  struct timespec now;
  assert(platform_time(PLATFORM_MONOTONIC, &now) == 0);
  sender_timed_audio timing = {sizeof(timing), 1, 1, 0,
                               (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec};
  uint8_t pcm[1408] = {0};
  assert(sender_submit_timed(runtime, generation, &timing, &format, pcm,
                             sizeof(pcm) / 2, &error) == SENDER_OK);
  sender_snapshot info = {0};
  info.size = sizeof(info);
  info.version = 1;
  assert(sender_get_snapshot(runtime, generation, &info, &error) == SENDER_OK &&
         info.queue_bytes == sizeof(pcm) / 2);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(((struct airplay_session *)peer_current->session)->state ==
         AIRPLAY_STATE_CONNECTED);
  sender_timed_audio remainder = timing;
  remainder.sample_index = 176;
  remainder.capture_monotonic_ns += 176ull * 1000000000 / 44100;
  assert(sender_submit_timed(runtime, generation, &remainder, &format,
                             pcm + sizeof(pcm) / 2, sizeof(pcm) / 2,
                             &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_get_snapshot(runtime, generation, &info, &error) == SENDER_OK);
  assert(info.sent_packets > 0 && info.sent_bytes > 0 &&
         info.resend_requests == 4 && info.resend_success == 1 &&
         info.resend_expired == 3 && info.queue_bytes == 0);
  assert(info.clock_reason[0] && info.sample_monotonic_ns && logs);
  unsigned audio_packets = 0;
  uint64_t audio_bytes = 0;
  uint8_t sync_wire[28] = {0};
  uint32_t first_rtp = 0;
  bool have_sync = false;
  for (;;) {
    fd_set read;
    FD_ZERO(&read);
    FD_SET(listener, &read);
    struct timeval wait = {0, 20000};
    if (select((int)listener + 1, &read, NULL, NULL, &wait) <= 0) break;
    uint8_t buffer[8192];
    int size = platform_recv(listener, buffer, sizeof(buffer), 0);
    assert(size > 0);
    if ((buffer[1] & 0x7f) == AIRPLAY_RTP_PAYLOADTYPE) {
      if (!audio_packets) {
        assert(buffer[1] & RTP_MARKER_BIT);
        first_rtp = read_word(buffer + 4);
      }
      ++audio_packets;
      audio_bytes += (uint64_t)size;
    } else if (measure_latency && size == 28 && buffer[1] == 0xd7) {
      if (!have_sync) memcpy(sync_wire, buffer, sizeof(sync_wire));
      have_sync = true;
    }
  }
  assert(audio_packets == info.sent_packets + info.resend_success &&
         audio_bytes == info.sent_bytes);
  if (measure_latency) {
    assert(have_sync);
    sender_timing_info negotiated = {0};
    negotiated.size = sizeof(negotiated);
    negotiated.version = 1;
    assert(sender_get_timing(runtime, generation, &negotiated, &error) ==
           SENDER_OK);
    assert(negotiated.setup_latency_min_samples ==
           (uint32_t)options.buffers.render_ms * 44100 / 1000);
    assert(negotiated.buffer_samples + negotiated.setup_latency_min_samples ==
           (uint32_t)options.initial_presentation.base_ms * 44100 / 1000);
    assert(read_word(sync_wire + 16) == first_rtp);
    assert((int32_t)(read_word(sync_wire + 16) - read_word(sync_wire + 4)) ==
           (int32_t)negotiated.buffer_samples);
    uint32_t requested_samples =
        (uint32_t)options.initial_presentation.base_ms * 44100 / 1000;
    uint32_t window_floor = (uint32_t)AIRPLAY_AUDIO_LATENCY_MS * 44100 / 1000;
    assert(
        negotiated.setup_latency_max_samples ==
        (requested_samples > window_floor ? requested_samples : window_floor));
    uint64_t rendered =
        render_time(sync_wire, first_rtp, negotiated.setup_latency_min_samples);
    uint64_t expected =
        timing.capture_monotonic_ns +
        (uint64_t)options.initial_presentation.base_ms * 1000000;
    assert(rendered >= expected - 23000 && rendered <= expected + 23000);
    struct airplay_session *session = peer_current->session;
    struct airplay_master_session *master = session->master_session;
    for (unsigned second = 1; second <= 3600; ++second) {
      uint64_t capture =
          timing.capture_monotonic_ns + (uint64_t)second * 1000000000;
      uint32_t position = first_rtp + second * 44100;
      master->rtp_session->pos = position;
      struct timespec stamp = {(time_t)(capture / 1000000000),
                               (long)(capture % 1000000000)};
      timestamp_set(master, stamp);
      struct rtp_packet *sync =
          rtp_sync_packet_next(master->rtp_session, master->cur_stamp, 0x80);
      uint64_t output = render_time(sync->data, position,
                                    negotiated.setup_latency_min_samples);
      uint64_t target =
          capture + (uint64_t)options.initial_presentation.base_ms * 1000000;
      assert(output >= target - 23000 && output <= target + 23000);
    }
    printf(
        "virtual-receiver target=%dms wire-playout=%.3fms "
        "virtual-duration=3600s drift<=0.023ms retransmitted=%llu "
        "expired=%llu\n",
        options.initial_presentation.base_ms,
        (double)(rendered - timing.capture_monotonic_ns) / 1000000,
        (unsigned long long)info.resend_success,
        (unsigned long long)info.resend_expired);
  }
  assert(sender_get_snapshot(runtime, generation + 1, &info, &error) ==
         SENDER_STATE);
  verify_quiet_retarget();
  verify_standby_resume(runtime, generation);
  for (unsigned i = 0;
       i < 10 && sender_get_state(runtime, generation) != SENDER_ENDED; ++i)
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_destroy(runtime, &error) == SENDER_OK);
  platform_socket_close(listener);
  platform_socket_close(control_listener);
  platform_cleanup();
  return 0;
}
