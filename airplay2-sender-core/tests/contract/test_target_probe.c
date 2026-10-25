#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "sender.h"
struct server {
  platform_socket listener;
  const char* response;
};
static void* serve(void* context) {
  struct server* server = context;
  assert(platform_wait(server->listener, false, 1000) > 0);
  platform_socket client = accept(server->listener, NULL, NULL);
  assert(client != (platform_socket)EVUTIL_INVALID_SOCKET);
  assert(!platform_socket_configure(client, false));
  char input[1024] = {0};
  size_t received = 0;
  while (!strstr(input, "\r\n\r\n")) {
    assert(platform_wait(client, false, 1000) > 0);
    ev_ssize_t count = platform_recv(client, input + received,
                                     sizeof(input) - received - 1, 0);
    assert(count > 0);
    received += (size_t)count;
    input[received] = 0;
    assert(received < sizeof(input) - 1);
  }
  assert(strstr(input, "GET /info RTSP/1.0\r\n"));
  assert(strstr(input, "User-Agent: AirPlay/"));
  if (!server->response) {
    assert(platform_wait(client, false, 1000) > 0);
    assert(platform_recv(client, input, sizeof(input), 0) == 0);
    platform_socket_close(client);
    return NULL;
  }
  size_t size = strlen(server->response), sent = 0;
  while (sent < size) {
    ev_ssize_t count =
        platform_send(client, server->response + sent, size - sent, 0);
    assert(count > 0);
    sent += (size_t)count;
  }
  platform_socket_close(client);
  return NULL;
}
static int SENDER_CALL cancel(void* context) { return *(bool*)context; }
static int SENDER_CALL cancel_after_poll(void* context) {
  return ++*(unsigned*)context >= 3;
}
static sender_result run_at(sender_runtime* runtime, const char* response,
                            const char* address, int family, uint32_t timeout,
                            sender_cancel_callback should_cancel, void* context,
                            sender_probe_info* info, sender_error* error) {
  unsigned short port = 0;
  struct server server = {
      platform_bound_socket(address, &port, SOCK_STREAM, family, false),
      response};
  assert(server.listener != (platform_socket)EVUTIL_INVALID_SOCKET);
  assert(!listen(server.listener, 1));
  platform_thread thread;
  assert(!platform_thread_start(&thread, serve, &server));
  sender_probe_request request = {
      sizeof(request), 1, address, port, 0, 7, timeout, should_cancel, context};
  sender_result result = sender_probe_target(runtime, &request, info, error);
  assert(!platform_thread_join(thread));
  platform_socket_close(server.listener);
  return result;
}
static sender_result run(sender_runtime* runtime, const char* response,
                         sender_probe_info* info, sender_error* error) {
  return run_at(runtime, response, "127.0.0.1", AF_INET, 500, NULL, NULL, info,
                error);
}
static sender_result run_body(sender_runtime* runtime, const char* body,
                              sender_probe_info* info, sender_error* error) {
  char response[2048];
  assert(strlen(body) < 1500);
  snprintf(response, sizeof(response),
           "RTSP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n%s", strlen(body),
           body);
  return run(runtime, response, info, error);
}
int main(void) {
  sender_options options = {
      1, "Probe consumer", "Probe consumer", 4096, NULL, NULL, NULL};
  sender_runtime* runtime = NULL;
  sender_error error = {0};
  assert(sender_create(&options, &runtime, &error) == SENDER_OK);
  const char body[] =
      "<?xml version=\"1.0\"?><plist "
      "version=\"1.0\"><dict><key>deviceID</key><string>aa:bb:cc:dd:ee:ff</"
      "string><key>name</key><string>Receiver</string><key>model</"
      "key><string>AudioAccessory</string><key>features</key><integer>512</"
      "integer></dict></plist>";
  char response[2048];
  snprintf(response, sizeof(response),
           "RTSP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n%s", strlen(body),
           body);
  sender_probe_info info = {0};
  info.size = sizeof(info);
  info.version = 1;
  assert(run(runtime, response, &info, &error) == SENDER_OK);
  assert(info.probe_id == 7 && info.features_available &&
         info.features == 512 &&
         !strcmp(info.receiver_id, "AA:BB:CC:DD:EE:FF"));
  assert(!strcmp(info.name, "Receiver") &&
         !strcmp(info.model, "AudioAccessory"));
  assert(run_at(runtime, response, "::1", AF_INET6, 500, NULL, NULL, &info,
                &error) == SENDER_OK);
  const char unknown[] =
      "<plist "
      "version=\"1.0\"><dict><key>deviceID</key><string>aa:bb:cc:dd:ee:ff</"
      "string><key>name</key><string>Receiver</string><key>model</"
      "key><string>AudioAccessory</string></dict></plist>";
  snprintf(response, sizeof(response),
           "RTSP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n%s", strlen(unknown),
           unknown);
  assert(run(runtime, response, &info, &error) == SENDER_OK &&
         !info.features_available);
  const char typed_features[] =
      "<plist "
      "version=\"1.0\"><dict><key>deviceID</key><string>aa:bb:cc:dd:ee:ff</"
      "string><key>name</key><string>Receiver</string><key>model</"
      "key><string>AudioAccessory</string><key>features</key><string>0x200</"
      "string><key>statusFlags</key><integer>4</integer></dict></plist>";
  assert(run_body(runtime, typed_features, &info, &error) == SENDER_OK &&
         info.features_available && info.features == 512 &&
         info.status_flags == 4);
  const char invalid_identity[] =
      "<plist "
      "version=\"1.0\"><dict><key>deviceID</key><string>not-a-public-id</"
      "string><key>name</key><string>Receiver</string><key>model</"
      "key><string>AudioAccessory</string></dict></plist>";
  sender_probe_info previous = info;
  assert(run_body(runtime, invalid_identity, &info, &error) == SENDER_INPUT);
  assert(!memcmp(&previous, &info, sizeof(info)));
  const char missing_identity[] =
      "<plist "
      "version=\"1.0\"><dict><key>name</key><string>Receiver</"
      "string><key>model</key><string>AudioAccessory</string></dict></plist>";
  assert(run_body(runtime, missing_identity, &info, &error) == SENDER_INPUT);
  const char missing_model[] =
      "<plist "
      "version=\"1.0\"><dict><key>deviceID</key><string>aa:bb:cc:dd:ee:ff</"
      "string><key>name</key><string>Receiver</string></dict></plist>";
  assert(run_body(runtime, missing_model, &info, &error) == SENDER_INPUT);
  assert(run(runtime, "RTSP/1.0 401 Unauthorized\r\nContent-Length: 0\r\n\r\n",
             &info, &error) == SENDER_AUTHENTICATION &&
         error.peer_code == 401);
  assert(run(runtime, "RTSP/1.0 403 Forbidden\r\nContent-Length: 0\r\n\r\n",
             &info, &error) == SENDER_AUTHENTICATION &&
         error.peer_code == 403);
  assert(
      run(runtime,
          "RTSP/1.0 470 Authentication Required\r\nContent-Length: 0\r\n\r\n",
          &info, &error) == SENDER_AUTHENTICATION &&
      error.peer_code == 470);
  assert(run(runtime, "RTSP/1.0 503 Unavailable\r\nContent-Length: 0\r\n\r\n",
             &info, &error) == SENDER_NETWORK &&
         error.peer_code == 503);
  assert(run(runtime, "RTSP/1.0 200 OK\r\nContent-Length: 999999999\r\n\r\n",
             &info, &error) == SENDER_INPUT);
  assert(run(runtime, "RTSP/1.0 200 OK\r\nContent-Length: 4\r\n\r\nnope", &info,
             &error) == SENDER_INPUT);
  assert(run(runtime,
             "RTSP/1.0 200 OK\r\nContent-Length: 4\r\nContent-Length: "
             "4\r\n\r\nnope",
             &info, &error) == SENDER_INPUT);
  assert(run(runtime, "RTSP/1.0 200 OK\r\n\r\nnope", &info, &error) ==
         SENDER_INPUT);
  assert(run(runtime, "RTSP/1.0 200 OK\r\nContent-Length: 100\r\n\r\nshort",
             &info, &error) == SENDER_NETWORK);
  assert(run_at(runtime, NULL, "127.0.0.1", AF_INET, 100, NULL, NULL, &info,
                &error) == SENDER_TIMEOUT);
  unsigned polls = 0;
  assert(run_at(runtime, NULL, "127.0.0.1", AF_INET, 500, cancel_after_poll,
                &polls, &info, &error) == SENDER_CANCELED &&
         polls == 3);
  bool canceled = true;
  sender_probe_request request = {
      sizeof(request), 1, "127.0.0.1", 9, 0, 8, 500, cancel, &canceled};
  assert(sender_probe_target(runtime, &request, &info, &error) ==
         SENDER_CANCELED);
  request.port = 0;
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.port = 7000;
  request.address = "fe80::1";
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.address = "::";
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.address = "ff02::1";
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.address = "224.0.0.1";
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.address = "255.255.255.255";
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.address = "0.0.0.0";
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.address = "127.0.0.1";
  request.scope_id = 1;
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.scope_id = 0;
  request.timeout_ms = 0;
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.timeout_ms = 10001;
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.timeout_ms = 500;
  request.probe_id = 0;
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.probe_id = 8;
  request.version = 2;
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_INPUT);
  request.version = 1;
  sender_target target = {
      "127.0.0.1",
      9,
      "AA:BB:CC:DD:EE:FF",
      "Probe ownership peer",
      (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48),
      0,
      50};
  sender_audio_format format = {44100, 16, 2};
  uint64_t generation;
  assert(sender_connect(runtime, &target, &format, NULL, NULL, &generation,
                        &error) == SENDER_OK);
  request.address = "127.0.0.1";
  assert(sender_probe_target(runtime, &request, &info, &error) == SENDER_STATE);
  assert(sender_end(runtime, generation, &error) == SENDER_OK &&
         sender_poll(runtime, 0, &error) == SENDER_OK);
  assert(sender_destroy(runtime, &error) == SENDER_OK);
  return 0;
}
