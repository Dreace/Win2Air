#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>

static void* context_calloc(size_t count, size_t size);
static void context_free(void* pointer);

/* Keep the production RTSP volume path, replacing only stream establishment. */
#define protocol_start established_protocol_start
#define protocol_end established_protocol_end
#define protocol_cleanup established_protocol_cleanup
#define calloc context_calloc
#define free context_free
#include "session.c"
#undef calloc
#undef free
#undef protocol_start
#undef protocol_end
#undef protocol_cleanup

static platform_socket listener;
static uint16_t port;
static unsigned starts, completed;
static _Atomic unsigned received;
static sender_command_completion outcomes[16];
static struct protocol_peer* current_peer;
static unsigned context_created, context_released;
static void* live_context;
static bool fail_context_allocation;
static bool initial_volume_sequence;
static sender_error initial_error;
static unsigned preserved_authorizations;

static void initial_failure(struct airplay_session* session) {
  start_failure(session);
  assert(current_peer->auth_key && strlen(current_peer->auth_key) == 192);
  assert(!current_peer->requires_auth);
  ++preserved_authorizations;
}

static void state_changed(void* context, uint64_t generation,
                          sender_state state, const sender_error* error) {
  (void)context;
  (void)generation;
  (void)state;
  if (error) initial_error = *error;
}

static void* context_calloc(size_t count, size_t size) {
  if (count == 1 && size == sizeof(struct volume_request)) {
    if (fail_context_allocation) return NULL;
    assert(!live_context);
    live_context = calloc(count, size);
    if (live_context) ++context_created;
    return live_context;
  }
  return calloc(count, size);
}

static void context_free(void* pointer) {
  if (pointer && pointer == live_context) {
    ++context_released;
    live_context = NULL;
  }
  free(pointer);
}

static void* receiver(void* unused) {
  (void)unused;
  platform_socket socket = accept(listener, NULL, NULL);
  assert(socket >= 0);
  const char* bodies[] = {"volume: -21.000000\r\n", "volume: -9.000000\r\n",
                          "volume: -144.000000\r\n", "volume: -0.000000\r\n",
                          "volume: -27.000000\r\n"};
  for (unsigned i = 0; i < 5; ++i) {
    char buffer[2048] = {0};
    size_t length = 0;
    char* header_end = NULL;
    size_t body_length = strlen(bodies[i]);
    while (!header_end ||
           length < (size_t)(header_end - buffer) + 4 + body_length) {
      ev_ssize_t read = platform_recv(socket, buffer + length,
                                      sizeof(buffer) - length - 1, 0);
      assert(read > 0);
      length += (size_t)read;
      buffer[length] = 0;
      header_end = strstr(buffer, "\r\n\r\n");
    }
    assert(
        strstr(buffer, "SET_PARAMETER rtsp://127.0.0.1/virtual RTSP/1.0\r\n") ==
        buffer);
    assert(strstr(buffer, "Content-Type: text/parameters\r\n"));
    assert(strcmp(header_end + 4, bodies[i]) == 0);
    ++received;
    if (i == 4) break;
    char response[128];
    snprintf(response, sizeof(response),
             "RTSP/1.0 %s\r\nCSeq: %u\r\nContent-Length: 0\r\n\r\n",
             i == 1 ? "403 Forbidden" : "200 OK", i + 1);
    size_t sent = 0;
    while (sent < strlen(response)) {
      ev_ssize_t count =
          platform_send(socket, response + sent, strlen(response) - sent, 0);
      assert(count > 0);
      sent += (size_t)count;
    }
  }
  /* Remain connected until explicit cleanup; a peer EOF is a session failure.
   */
  char end;
  assert(platform_recv(socket, &end, 1, 0) <= 0);
  platform_socket_close(socket);
  return NULL;
}

static void* unresponsive_receiver(void* unused) {
  (void)unused;
  platform_socket socket = accept(listener, NULL, NULL);
  assert(socket >= 0);
  char buffer[2048] = {0};
  size_t length = 0;
  while (!strstr(buffer, "volume: -18.000000\r\n")) {
    ev_ssize_t count =
        platform_recv(socket, buffer + length, sizeof(buffer) - length - 1, 0);
    assert(count > 0);
    length += (size_t)count;
    buffer[length] = 0;
  }
  ++received;
  char end;
  assert(platform_recv(socket, &end, 1, 0) <= 0);
  platform_socket_close(socket);
  return NULL;
}

static void* initial_receiver(void* context) {
  int code = *(const int*)context;
  platform_socket socket = accept(listener, NULL, NULL);
  assert(socket >= 0);
  char buffer[2048] = {0};
  size_t length = 0;
  while (!strstr(buffer, "volume: -15.000000\r\n")) {
    ev_ssize_t count =
        platform_recv(socket, buffer + length, sizeof(buffer) - length - 1, 0);
    assert(count > 0);
    length += (size_t)count;
    buffer[length] = 0;
  }
  assert(strstr(buffer, "SET_PARAMETER ") == buffer);
  ++received;
  if (code == 0) {
    platform_socket_close(socket);
    return NULL;
  }
  if (code < 0) {
    char end;
    assert(platform_recv(socket, &end, 1, 0) <= 0);
    platform_socket_close(socket);
    return NULL;
  }
  char response[128];
  snprintf(response, sizeof(response),
           "RTSP/1.0 %d %s\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n", code,
           code == 200 ? "OK" : "Forbidden");
  assert(platform_send(socket, response, strlen(response), 0) ==
         (ev_ssize_t)strlen(response));
  char end;
  assert(platform_recv(socket, &end, 1, 0) <= 0);
  platform_socket_close(socket);
  return NULL;
}

int protocol_start(struct event_base* base, struct protocol_peer* peer,
                   uint64_t identity, const char* client, const char* agent) {
  ++starts;
  current_peer = peer;
  evbase_player = base;
  airplay_device_id = identity;
  airplay_client_name = client;
  airplay_user_agent = agent;
  struct airplay_session* session = calloc(1, sizeof(*session));
  assert(session);
  session->device_id = peer->id;
  session->state = AIRPLAY_STATE_CONNECTED;
  session->devname = strdup("Virtual receiver");
  session->server_fd = -1;
  session->volume = peer->volume;
  session->cseq = 1;
  strcpy(session->session_url, "rtsp://127.0.0.1/virtual");
  session->ctrl = evrtsp_connection_new("127.0.0.1", port);
  assert(session->ctrl);
  evrtsp_connection_set_base(session->ctrl, base);
  session->deferredev = evtimer_new(base, deferred_session_failure_cb, session);
  assert(session->deferredev);
  airplay_sessions = session;
  peer->session = session;
  if (initial_volume_sequence) {
    assert(!peer->auth_key);
    peer->auth_key = calloc(193, 1);
    assert(peer->auth_key);
    memset(peer->auth_key, 'A', 192);
    session->ctrl->timeout = 1;
    struct airplay_seq_ctx* sequence = calloc(1, sizeof(*sequence));
    assert(sequence);
    sequence->session = session;
    sequence->cur_request = airplay_seq_request[AIRPLAY_SEQ_START_PLAYBACK];
    while (strcmp(sequence->cur_request->name, "SET_PARAMETER (volume)"))
      ++sequence->cur_request;
    sequence->on_success = session_status;
    sequence->on_error = initial_failure;
    sequence->log_caller = "initial volume contract";
    sequence_continue(sequence);
  } else {
    protocol_state_changed(0, peer->id, OUTPUT_STATE_CONNECTED);
    for (unsigned command = 1; command <= 4; ++command)
      protocol_receiver_media(command);
  }
  return 0;
}

int protocol_end(struct protocol_peer* peer) {
  if (peer->session) session_cleanup(peer->session);
  protocol_state_changed(0, peer->id, OUTPUT_STATE_STOPPED);
  return 0;
}

void protocol_cleanup(void) {
  while (airplay_sessions) session_cleanup(airplay_sessions);
  evbase_player = NULL;
  current_peer = NULL;
}

static void completion(void* context, const sender_command_completion* value) {
  (void)context;
  assert(value->size == sizeof(*value) && value->version == 1);
  assert(completed < sizeof(outcomes) / sizeof(outcomes[0]));
  outcomes[completed++] = *value;
}

static void wait_completed(sender_runtime* runtime, unsigned count) {
  sender_error error;
  struct timespec start, now;
  assert(platform_time(PLATFORM_MONOTONIC, &start) == 0);
  while (completed < count) {
    assert(sender_poll(runtime, 10, &error) == SENDER_OK);
    assert(platform_time(PLATFORM_MONOTONIC, &now) == 0);
    assert(now.tv_sec - start.tv_sec < 4);
  }
}

static void* media_receiver(void* unused) {
  (void)unused;
  platform_socket socket = accept(listener, NULL, NULL);
  assert(socket >= 0);
  for (unsigned index = 0; index < 9; ++index) {
    unsigned char buffer[16384] = {0};
    size_t length = 0, body_size = 0;
    char* end = NULL;
    do {
      ev_ssize_t count = platform_recv(socket, buffer + length,
                                       sizeof(buffer) - length - 1, 0);
      assert(count > 0);
      length += (size_t)count;
      end = strstr((char*)buffer, "\r\n\r\n");
      char* size = strstr((char*)buffer, "Content-Length:");
      if (end && size) body_size = strtoul(size + 15, NULL, 10);
    } while (!end || length < (size_t)(end - (char*)buffer) + 4 + body_size);
    unsigned char* body = (unsigned char*)end + 4;
    if (index == 7) {
      assert(strstr((char*)buffer, "SET_PARAMETER ") == (char*)buffer);
      assert(protocol_bytes_find(body, body_size, "volume: -9.000000", 17));
    } else if (index == 0 || index >= 6) {
      assert(strstr((char*)buffer, "GET_PARAMETER ") == (char*)buffer);
      assert(body_size == 8 && !memcmp(body, "volume\r\n", 8));
    } else {
      assert(strstr((char*)buffer, "SET_PARAMETER ") == (char*)buffer);
      assert(strstr((char*)buffer, "RTP-Info: rtptime="));
      if (index == 1 || index == 3 || index == 5) {
        assert(strstr((char*)buffer, "application/x-dmap-tagged"));
        assert(!memcmp(body, "mlit", 4));
        uint32_t size;
        memcpy(&size, body + 4, 4);
        assert(ntohl(size) == body_size - 8);
        assert(protocol_bytes_find(body, body_size, "minm", 4));
        assert(protocol_bytes_find(body, body_size, "asar", 4));
        assert(protocol_bytes_find(body, body_size, "asal", 4));
        if (index == 1)
          assert(protocol_bytes_find(body, body_size, "System title", 12));
        else if (index == 3)
          assert(body_size == 32);
      } else {
        assert(strstr((char*)buffer, "image/jpeg"));
        assert(body_size == (index == 2 ? 4u : 0u));
        if (body_size) assert(body[0] == 0xff && body[1] == 0xd8);
      }
    }
    char* sequence = strstr((char*)buffer, "CSeq:");
    assert(sequence);
    unsigned number = (unsigned)strtoul(sequence + 5, NULL, 10);
    char response[256];
    const char* value = index == 0   ? "volume: -15.000000\r\n"
                        : index == 6 ? "volume: -18.900000\r\n"
                                     : "";
    int code = index == 5 ? 403 : index == 8 ? 501 : 200;
    int size = snprintf(
        response, sizeof(response),
        "RTSP/1.0 %d Reply\r\nCSeq: %u\r\nContent-Length: %zu\r\n\r\n%s", code,
        number, strlen(value), value);
    assert(platform_send(socket, response, (size_t)size, 0) == size);
  }
  char end;
  assert(platform_recv(socket, &end, 1, 0) <= 0);
  platform_socket_close(socket);
  return NULL;
}

int main(void) {
  sender_create_options options = {0};
  options.version = SENDER_ABI_VERSION;
  options.size = sizeof(options);
  options.options.sender_id = 1;
  options.options.client_name = "Volume consumer";
  options.options.user_agent = "Volume consumer";
  options.options.audio_capacity_bytes = 4096;
  options.options.on_state = state_changed;
  options.initial_presentation.base_ms = 1000;
  options.on_command = completion;
  sender_runtime* runtime = NULL;
  sender_error error;
  assert(sender_create_ex(&options, &runtime, &error) == SENDER_OK);
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(listener >= 0);
  struct sockaddr_in local = {0};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, (struct sockaddr*)&local, sizeof(local)) == 0);
  assert(listen(listener, 1) == 0);
  socklen_t length = sizeof(local);
  assert(getsockname(listener, (struct sockaddr*)&local, &length) == 0);
  port = ntohs(local.sin_port);
  platform_thread worker;
  assert(platform_thread_start(&worker, receiver, NULL) == 0);
  sender_audio_format format = {44100, 16, 2};
  sender_target target = {
      "127.0.0.1",
      port,
      "AA:BB:CC:DD:EE:FF",
      "Virtual",
      (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48),
      0,
      50};
  uint64_t generation;
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_get_state(runtime, generation) == SENDER_READY);
  {
    uint32_t command = 99;
    assert(sender_take_media_command(runtime, generation + 1, &command, &error) == SENDER_STATE);
    assert(sender_take_media_command(runtime, generation, NULL, &error) == SENDER_INPUT);
    for (uint32_t expected = 1; expected <= 4; ++expected) {
      assert(sender_take_media_command(runtime, generation, &command, &error) == SENDER_OK);
      assert(command == expected);
    }
    assert(sender_take_media_command(runtime, generation, &command, &error) == SENDER_OK && command == 0);
  }
  {
    struct airplay_session* established = current_peer->session;
    established->reqs_in_flight = 1;
    assert(protocol_set_volume(current_peer, 9, 40) == 1);
    assert(!established->volume_request && !live_context);
    established->reqs_in_flight = 0;
  }
  assert(sender_set_volume(runtime, generation - 1, 1, 30, &error) ==
         SENDER_STATE);
  assert(sender_set_volume(runtime, generation, 0, 30, &error) == SENDER_INPUT);
  assert(sender_set_volume(runtime, generation, 1, 101, &error) ==
         SENDER_INPUT);
  assert(sender_set_volume(runtime, generation, 1, 30, &error) == SENDER_OK);
  assert(completed == 0 && current_peer->volume == 50);
  assert(sender_set_volume(runtime, generation, 1, 30, &error) == SENDER_STATE);
  assert(sender_set_volume(runtime, generation, 2, 70, &error) == SENDER_OK);
  wait_completed(runtime, 1);
  assert(outcomes[0].command_id == 1 && outcomes[0].result == SENDER_OK);
  assert(outcomes[0].generation == generation &&
         outcomes[0].volume_percent == 30);
  assert(current_peer->volume == 30);
  wait_completed(runtime, 2);
  assert(outcomes[1].command_id == 2 && outcomes[1].result == SENDER_NETWORK);
  assert(outcomes[1].error.peer_code == 403);
  assert(current_peer->volume == 30 &&
         sender_get_state(runtime, generation) == SENDER_READY);
  assert(sender_set_volume(runtime, generation, 3, 0, &error) == SENDER_OK);
  assert(sender_set_volume(runtime, generation, 4, 100, &error) == SENDER_OK);
  wait_completed(runtime, 4);
  assert(outcomes[2].volume_percent == 0 && outcomes[2].result == SENDER_OK);
  assert(outcomes[3].volume_percent == 100 && outcomes[3].result == SENDER_OK);
  assert(starts == 1 && current_peer->volume == 100);
  assert(!live_context && context_created == 4 && context_released == 4);
  fail_context_allocation = true;
  assert(protocol_set_volume(current_peer, 100, 45) < 0);
  fail_context_allocation = false;
  assert(!live_context &&
         !((struct airplay_session*)current_peer->session)->volume_request);
  assert(sender_set_volume(runtime, generation, 5, 10, &error) == SENDER_OK);
  for (unsigned i = 0; received < 5; ++i) {
    assert(i < 1000);
    assert(sender_poll(runtime, 1, &error) == SENDER_OK);
  }
  assert(completed == 4 && current_peer->volume == 100);
  assert(live_context && context_created == 5 && context_released == 4);
  assert(sender_set_volume(runtime, generation, 6, 20, &error) == SENDER_OK);
  assert(sender_end(runtime, generation, &error) == SENDER_OK);
  wait_completed(runtime, 6);
  assert(outcomes[4].command_id == 5 && outcomes[4].result == SENDER_CANCELED);
  assert(outcomes[5].command_id == 6 && outcomes[5].result == SENDER_CANCELED);
  assert(sender_get_state(runtime, generation) == SENDER_ENDED);
  assert(!live_context && context_created == 5 && context_released == 5);
  assert(platform_thread_join(worker) == 0);
  assert(received == 5 && starts == 1);
  assert(platform_thread_start(&worker, unresponsive_receiver, NULL) == 0);
  uint64_t previous_generation = generation;
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(generation > previous_generation);
  assert(sender_set_volume(runtime, previous_generation, 7, 40, &error) ==
         SENDER_STATE);
  assert(sender_set_volume(runtime, generation, 1, 40, &error) == SENDER_OK);
  wait_completed(runtime, 7);
  assert(outcomes[6].result == SENDER_TIMEOUT && outcomes[6].command_id == 1);
  assert(outcomes[6].generation == generation &&
         outcomes[6].error.platform_code == ETIMEDOUT);
  for (unsigned i = 0; sender_get_state(runtime, generation) != SENDER_FAILED;
       ++i) {
    assert(i < 100);
    assert(sender_poll(runtime, 10, &error) == SENDER_OK);
  }
  assert(platform_thread_join(worker) == 0);
  assert(received == 6 && starts == 2);
  assert(!live_context && context_created == 6 && context_released == 6);
  initial_volume_sequence = true;
  const int initial_responses[] = {200, 403, 0, -1};
  for (unsigned index = 0; index < 4; ++index) {
    memset(&initial_error, 0, sizeof(initial_error));
    assert(platform_thread_start(&worker, initial_receiver,
                                 (void*)&initial_responses[index]) == 0);
    assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                          &error) == SENDER_OK);
    assert(sender_get_state(runtime, generation) == SENDER_CONNECTING);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    sender_state expected = index == 0 ? SENDER_READY : SENDER_FAILED;
    for (unsigned i = 0; sender_get_state(runtime, generation) != expected;
         ++i) {
      assert(i < 1000);
      assert(sender_poll(runtime, 5, &error) == SENDER_OK);
    }
    if (expected == SENDER_READY) {
      assert(initial_error.code == SENDER_OK);
      assert(sender_end(runtime, generation, &error) == SENDER_OK);
      assert(sender_poll(runtime, 0, &error) == SENDER_OK);
    } else {
      assert(initial_error.code == SENDER_NETWORK);
      assert(initial_error.peer_code == (index == 1 ? 403 : 0));
      assert(strstr(initial_error.operation, "SET_PARAMETER (volume)"));
    }
    assert(platform_thread_join(worker) == 0);
  }
  assert(received == 10 && starts == 6 && !live_context);
  assert(preserved_authorizations == 3);
  initial_volume_sequence = false;
  assert(platform_thread_start(&worker, media_receiver, NULL) == 0);
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  struct airplay_session* media_session = current_peer->session;
  media_session->master_session =
      master_session_make(&current_peer->quality, false, current_peer);
  assert(media_session->master_session);
  uint32_t percent = 0;
  uint64_t revision = 0;
  assert(sender_get_receiver_volume(runtime, generation, &percent, &revision,
                                    &error) == SENDER_OK &&
         revision == 0);
  for (unsigned i = 0; media_session->volume_query_pending; ++i) {
    assert(i < 1000);
    assert(sender_poll(runtime, 5, &error) == SENDER_OK);
  }
  assert(sender_get_receiver_volume(runtime, generation, &percent, &revision,
                                    &error) == SENDER_OK &&
         revision == 1 && percent == 50);
  const unsigned char artwork[] = {0xff, 0xd8, 0xff, 0xd9};
  assert(sender_set_now_playing(runtime, generation, "System title", "Artist",
                                "Album", "image/jpeg", artwork, sizeof(artwork),
                                &error) == SENDER_OK);
  assert(sender_set_now_playing(runtime, generation, "pending", "", "", "",
                                NULL, 0, &error) == SENDER_BACKPRESSURE);
  for (unsigned i = 0; media_session->metadata_pending; ++i) {
    assert(i < 1000);
    assert(sender_poll(runtime, 5, &error) == SENDER_OK);
  }
  assert(sender_set_now_playing(runtime, generation, "", "", "", "", NULL, 0,
                                &error) == SENDER_OK);
  for (unsigned i = 0; media_session->metadata_pending; ++i) {
    assert(i < 1000);
    assert(sender_poll(runtime, 5, &error) == SENDER_OK);
  }
  unsigned before_rejection = completed;
  assert(sender_set_now_playing(runtime, generation, "Rejected title", "", "",
                                "", NULL, 0, &error) == SENDER_OK);
  for (unsigned i = 0; media_session->metadata_pending; ++i) {
    assert(i < 1000);
    assert(sender_poll(runtime, 5, &error) == SENDER_OK);
  }
  assert(completed == before_rejection + 1 &&
         outcomes[completed - 1].kind == SENDER_COMMAND_METADATA);
  assert(outcomes[completed - 1].result == SENDER_NETWORK &&
         outcomes[completed - 1].error.peer_code == 403);
  assert(sender_get_state(runtime, generation) == SENDER_READY);
  media_session->next_volume_query = 0;
  assert(sender_get_receiver_volume(runtime, generation, &percent, &revision,
                                    &error) == SENDER_OK);
  for (unsigned i = 0; media_session->volume_query_pending; ++i) {
    assert(i < 1000);
    assert(sender_poll(runtime, 5, &error) == SENDER_OK);
  }
  assert(sender_get_receiver_volume(runtime, generation, &percent, &revision,
                                    &error) == SENDER_OK &&
         percent == 37 && revision == 2);
  unsigned after_local_volume = completed + 1;
  assert(sender_set_volume(runtime, generation, 1, 70, &error) == SENDER_OK);
  wait_completed(runtime, after_local_volume);
  media_session->next_volume_query += 60;
  assert(sender_get_receiver_volume(runtime, generation, &percent, &revision,
                                    &error) == SENDER_OK &&
         revision == 0);
  media_session->next_volume_query = 0;
  assert(sender_get_receiver_volume(runtime, generation, &percent, &revision,
                                    &error) == SENDER_OK);
  for (unsigned i = 0; media_session->volume_query_pending; ++i) {
    assert(i < 1000);
    assert(sender_poll(runtime, 5, &error) == SENDER_OK);
  }
  assert(media_session->volume_query_unsupported &&
         sender_get_state(runtime, generation) == SENDER_READY);
  assert(sender_get_receiver_volume(runtime, generation, &percent, &revision,
                                    &error) == SENDER_OK &&
         revision == 0);
  assert(!media_session->volume_query_pending);
  assert(sender_set_now_playing(runtime, generation, "", "", "", "image/gif",
                                artwork, sizeof(artwork),
                                &error) == SENDER_INPUT);
  assert(sender_end(runtime, generation, &error) == SENDER_OK);
  assert(sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(platform_thread_join(worker) == 0);
  assert(sender_get_receiver_volume(runtime, generation, &percent, &revision,
                                    &error) == SENDER_STATE);
  assert(sender_destroy(runtime, &error) == SENDER_OK);
  platform_socket_close(listener);
  return 0;
}
