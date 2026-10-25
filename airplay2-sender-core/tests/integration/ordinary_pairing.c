#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <plist/plist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pairing/pair.h"
#include "platform.h"
#include "sender.h"
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#define open _open
#define read _read
#define write _write
#define close _close
#define unlink _unlink
#define access _access
#define rmdir _rmdir
#define F_OK 0
#else
#include <spawn.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
static const uint64_t sender_identity = UINT64_C(0x12345678);
static char material[193], client_id[64];
static uint8_t client_key[32];
static unsigned updates, setups, verified, completed, connections, rejected;
static platform_socket listener;
static unsigned short event_port, data_port, control_port;
static const char *material_path;
static sender_error observed_error;

static void state_changed(void *context, uint64_t generation,
                          sender_state state, const sender_error *error) {
  (void)context;
  (void)generation;
  if (state == SENDER_FAILED && error) observed_error = *error;
}

static int save_key(uint8_t key[32], const char *id, void *context) {
  (void)context;
  assert(strlen(id) < sizeof(client_id));
  strcpy(client_id, id);
  memcpy(client_key, key, sizeof(client_key));
  return 0;
}
static int load_key(uint8_t key[32], const char *id, void *context) {
  (void)context;
  if (strcmp(id, client_id)) return -1;
  memcpy(key, client_key, sizeof(client_key));
  return 0;
}
static void transfer(platform_socket fd, void *data, size_t length,
                     int writing) {
  uint8_t *bytes = data;
  while (length) {
    ssize_t count = writing ? platform_send(fd, bytes, length, 0)
                            : platform_recv(fd, bytes, length, 0);
    if (count < 0 && errno == EINTR) continue;
    assert(count > 0);
    bytes += count;
    length -= (size_t)count;
  }
}
struct channel {
  platform_socket fd;
  struct pair_cipher_context *cipher;
  uint8_t pending[16384];
  size_t offset, length;
};
static int next_byte(struct channel *channel, uint8_t *byte) {
  if (!channel->cipher) return (int)platform_recv(channel->fd, byte, 1, 0);
  if (channel->offset == channel->length) {
    uint8_t frame[1042];
    ssize_t count = platform_recv(channel->fd, frame, 1, 0);
    if (count <= 0) return (int)count;
    transfer(channel->fd, frame + 1, 1, 0);
    size_t length = (size_t)frame[0] | ((size_t)frame[1] << 8);
    assert(length <= 1024);
    transfer(channel->fd, frame + 2, length + 16, 0);
    uint8_t *plain = NULL;
    size_t plain_length = 0;
    assert(pair_decrypt(&plain, &plain_length, frame, length + 18,
                        channel->cipher) == (ssize_t)(length + 18));
    assert(plain_length && plain_length <= sizeof(channel->pending));
    memcpy(channel->pending, plain, plain_length);
    free(plain);
    channel->offset = 0;
    channel->length = plain_length;
  }
  *byte = channel->pending[channel->offset++];
  return 1;
}
static int request(struct channel *channel, char header[4096],
                   uint8_t body[8192], size_t *body_length) {
  size_t length = 0;
  while (length < 4095) {
    int count = next_byte(channel, (uint8_t *)header + length);
    if (!count && !length) return 0;
    assert(count == 1);
    ++length;
    header[length] = 0;
    if (length >= 4 && !memcmp(header + length - 4, "\r\n\r\n", 4)) break;
  }
  assert(length < 4095);
  const char *field = strstr(header, "Content-Length:");
  *body_length = field ? (size_t)strtoul(field + 15, NULL, 10) : 0;
  assert(*body_length <= 8192);
  for (size_t i = 0; i < *body_length; ++i)
    assert(next_byte(channel, body + i) == 1);
  return 1;
}
static void response(struct channel *channel, const void *body, size_t length) {
  uint8_t data[10000];
  int header =
      snprintf((char *)data, sizeof(data),
               "RTSP/1.0 200 OK\r\nContent-Type: "
               "application/octet-stream\r\nContent-Length: %zu\r\n\r\n",
               length);
  assert(header > 0 && (size_t)header + length < sizeof(data));
  if (length) memcpy(data + header, body, length);
  if (channel->cipher) {
    uint8_t *encrypted = NULL;
    size_t encrypted_length = 0;
    assert(pair_encrypt(&encrypted, &encrypted_length, data,
                        (size_t)header + length,
                        channel->cipher) == header + (ssize_t)length);
    transfer(channel->fd, encrypted, encrypted_length, 1);
    free(encrypted);
  } else
    transfer(channel->fd, data, (size_t)header + length, 1);
}
static void *peer(void *unused) {
  (void)unused;
  while (completed < 4 || rejected < 1) {
    struct channel channel = {0};
    channel.fd = accept(listener, NULL, NULL);
    assert(channel.fd != (platform_socket)EVUTIL_INVALID_SOCKET);
    ++connections;
#ifdef _WIN32
    DWORD timeout = 5000;
    assert(!setsockopt(channel.fd, SOL_SOCKET, SO_RCVTIMEO,
                       (const char *)&timeout, sizeof(timeout)));
#else
    struct timeval timeout = {5, 0};
    assert(!setsockopt(channel.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)));
#endif
    struct pair_setup_context *setup = NULL;
    struct pair_verify_context *verify = NULL;
    char header[4096];
    uint8_t body[8192];
    size_t length;
    while (request(&channel, header, body, &length)) {
      if (!strncmp(header, "GET /info ", 10)) {
        const char *info =
            "<?xml version=\"1.0\"?><plist "
            "version=\"1.0\"><dict><key>statusFlags</key><integer>128</"
            "integer></dict></plist>";
        plist_t dictionary = NULL;
        char *binary = NULL;
        uint32_t binary_length = 0;
        plist_from_xml(info, (uint32_t)strlen(info), &dictionary);
        assert(dictionary);
        plist_to_bin(dictionary, &binary, &binary_length);
        assert(binary);
        response(&channel, binary, binary_length);
        free(binary);
        plist_free(dictionary);
      } else if (strstr(header, "POST /pair-setup ")) {
        if (!setup) {
          setup = pair_setup_new(PAIR_SERVER_HOMEKIT, "314-15-926", save_key,
                                 NULL, "AA:BB:CC:DD:EE:FF");
          assert(setup);
          ++setups;
        }
        uint8_t *reply = NULL;
        size_t reply_length = 0;
        assert(!pair_setup(&reply, &reply_length, setup, body, length));
        response(&channel, reply, reply_length);
        free(reply);
      } else if (strstr(header, "POST /pair-verify ")) {
        assert(!setup);
        if (!verify) {
          verify = pair_verify_new(PAIR_SERVER_HOMEKIT, NULL, load_key, NULL,
                                   "AA:BB:CC:DD:EE:FF");
          assert(verify);
        }
        uint8_t *reply = NULL;
        size_t reply_length = 0;
        assert(!pair_verify(&reply, &reply_length, verify, body, length));
        response(&channel, reply, reply_length);
        free(reply);
        struct pair_result *result;
        if (!pair_verify_result(&result, verify)) {
          channel.cipher = pair_cipher_new(
              PAIR_SERVER_HOMEKIT, PAIR_CHANNEL_CONTROL, result->shared_secret,
              result->shared_secret_len, NULL);
          assert(channel.cipher);
          ++verified;
        }
      } else if (completed == 3) {
        assert(channel.cipher);
        assert(!strncmp(header, "SETUP ", 6));
        /* A deliberate receiver rejection proves encrypted request and response
         * handling. */
        uint8_t *encrypted = NULL;
        size_t encrypted_length = 0;
        const char rejection[] =
            "RTSP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        assert(pair_encrypt(&encrypted, &encrypted_length,
                            (const uint8_t *)rejection, sizeof(rejection) - 1,
                            channel.cipher) == (ssize_t)sizeof(rejection) - 1);
        transfer(channel.fd, encrypted, encrypted_length, 1);
        free(encrypted);
        ++completed;
        break;
      } else if (!strncmp(header, "SETUP ", 6)) {
        assert(channel.cipher);
        plist_t input = NULL;
        plist_from_bin((const char *)body, (uint32_t)length, &input);
        assert(input);
        plist_t output = plist_new_dict();
        if (plist_dict_get_item(input, "streams")) {
          plist_t streams = plist_new_array(), stream = plist_new_dict();
          plist_dict_set_item(stream, "dataPort", plist_new_uint(data_port));
          plist_dict_set_item(stream, "controlPort",
                              plist_new_uint(control_port));
          plist_array_append_item(streams, stream);
          plist_dict_set_item(output, "streams", streams);
        } else {
          plist_dict_set_item(output, "eventPort", plist_new_uint(event_port));
          plist_t timing = plist_new_dict(), addresses = plist_new_array();
          plist_array_append_item(addresses, plist_new_string("127.0.0.1"));
          plist_dict_set_item(timing, "Addresses", addresses);
          plist_dict_set_item(output, "timingPeerInfo", timing);
        }
        char *binary = NULL;
        uint32_t binary_length = 0;
        plist_to_bin(output, &binary, &binary_length);
        assert(binary);
        response(&channel, binary, binary_length);
        free(binary);
        plist_free(input);
        plist_free(output);
      } else {
        assert(channel.cipher);
        assert(!strncmp(header, "RECORD ", 7) ||
               !strncmp(header, "SETPEERS ", 9) ||
               !strncmp(header, "SET_PARAMETER ", 14) ||
               !strncmp(header, "FLUSH ", 6) ||
               !strncmp(header, "TEARDOWN ", 9));
        response(&channel, NULL, 0);
        if (!strncmp(header, "TEARDOWN ", 9)) {
          ++completed;
          break;
        }
      }
    }
    if (verify && !channel.cipher) ++rejected;
    assert(!platform_socket_close(channel.fd));
    pair_setup_free(setup);
    pair_verify_free(verify);
    pair_cipher_free(channel.cipher);
  }
  return NULL;
}
static void credentials_changed(void *context, uint64_t generation,
                                const sender_credentials *credentials) {
  (void)context;
  assert(generation && credentials->sender_id == sender_identity);
  assert(!strcmp(credentials->receiver_id, "AA:BB:CC:DD:EE:FF"));
  assert(strlen(credentials->material) == 192);
  int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef _WIN32
  flags |= O_BINARY;
#endif
  int fd = open(material_path, flags, 0600);
  assert(fd >= 0);
  assert(write(fd, credentials->material, 192) == 192);
  assert(!close(fd));
  ++updates;
}
static int client(unsigned short port, const char *path, int reuse) {
  material_path = path;
  sender_options options = {sender_identity,
                            "Pairing test",
                            "OwnTone/29.3",
                            8192,
                            state_changed,
                            credentials_changed,
                            NULL};
  sender_target target = {
      "127.0.0.1",
      port,
      "AA:BB:CC:DD:EE:FF",
      "Pairing peer",
      (UINT64_C(1) << 9) | (UINT64_C(1) << 41) | (UINT64_C(1) << 48),
      0,
      50};
  sender_audio_format format = {44100, 16, 2};
  sender_error error;
  sender_runtime *runtime;
  uint64_t generation;
  sender_credentials credentials = {target.receiver_id, sender_identity,
                                    material};
  if (reuse && reuse != 4) {
    int flags = O_RDONLY;
#ifdef _WIN32
    flags |= O_BINARY;
#endif
    int fd = open(path, flags);
    assert(fd >= 0);
#ifndef _WIN32
    struct stat metadata;
    assert(!fstat(fd, &metadata) && (metadata.st_mode & 0777) == 0600 &&
           metadata.st_size == 192);
#endif
    assert(read(fd, material, 192) == 192);
    assert(!close(fd));
    material[192] = 0;
    if (reuse == 3 || reuse == 5)
      material[191] = material[191] == '0' ? '1' : '0';
    if (reuse == 5) assert(!unlink(path));
  }
  assert(sender_create(&options, &runtime, &error) == SENDER_OK);
  assert(sender_connect(runtime, &target, &format,
                        reuse && reuse != 4 ? &credentials : NULL,
                        reuse && reuse != 5 ? NULL : "314-15-926", &generation,
                        &error) == SENDER_OK);
  memset(material, 0, sizeof(material));
  sender_state expected =
      reuse >= 2 && reuse != 5 ? SENDER_FAILED : SENDER_READY;
  for (unsigned i = 0;
       i < 500 && sender_get_state(runtime, generation) != expected; ++i)
    (void)sender_poll(runtime, 20, &error);
  if (sender_get_state(runtime, generation) != expected)
    fprintf(stderr, "operation=%s code=%d peer=%d message=%s\n",
            observed_error.operation, observed_error.code,
            observed_error.peer_code, observed_error.message);
  assert(sender_get_state(runtime, generation) == expected);
  if (reuse == 2)
    assert(observed_error.code == SENDER_NETWORK &&
           observed_error.peer_code == 500 &&
           observed_error.generation == generation);
  else if (reuse == 3)
    assert(observed_error.code == SENDER_AUTHENTICATION &&
           observed_error.generation == generation &&
           !strcmp(observed_error.operation, "pair verify 1"));
  else if (reuse == 4)
    assert(observed_error.code == SENDER_AUTHENTICATION &&
           observed_error.generation == generation &&
           !strcmp(observed_error.operation, "GET /info") &&
           strstr(observed_error.message, "statusFlags=128") && !updates);
  else {
    uint8_t audio[1408] = {0};
    assert(sender_submit(runtime, generation, &format, audio, sizeof(audio),
                         &error) == SENDER_OK);
    assert(sender_poll(runtime, 0, &error) == SENDER_OK &&
           sender_get_state(runtime, generation) == SENDER_STREAMING);
    assert(sender_flush(runtime, generation, &error) == SENDER_OK);
    for (unsigned i = 0;
         i < 250 && sender_get_state(runtime, generation) != SENDER_READY; ++i)
      (void)sender_poll(runtime, 20, &error);
    assert(sender_get_state(runtime, generation) == SENDER_READY);
    assert(sender_end(runtime, generation, &error) == SENDER_OK);
    for (unsigned i = 0;
         i < 250 && sender_get_state(runtime, generation) != SENDER_ENDED; ++i)
      (void)sender_poll(runtime, 20, &error);
    assert(sender_get_state(runtime, generation) == SENDER_ENDED);
  }
  assert(updates == (!reuse || reuse == 5 ? 1U : 0U));
  assert(sender_destroy(runtime, &error) == SENDER_OK);
  return 0;
}
int main(int argc, char **argv) {
#ifndef _WIN32
  if (geteuid() != 0) {
    fputs(
        "operation=ordinary-pairing "
        "reason=root-required-for-original-ptp-ports\n",
        stderr);
    return 2;
  }
#endif
  assert(!platform_init());
  if (argc == 5)
    return client((unsigned short)strtoul(argv[2], NULL, 10), argv[3],
                  atoi(argv[4]));
  assert(argc == 1);
#ifndef _WIN32
  alarm(25);
#endif
  sender_runtime *runtime;
  sender_error error;
  sender_options options = {
      sender_identity, "Init", "OwnTone/29.3", 8192, NULL, NULL, NULL};
  assert(sender_create(&options, &runtime, &error) == SENDER_OK);
  assert(sender_destroy(runtime, &error) == SENDER_OK);
  listener = platform_socket_open(AF_INET, SOCK_STREAM, 0);
  assert(listener != (platform_socket)EVUTIL_INVALID_SOCKET);
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(!bind(listener, (struct sockaddr *)&address, sizeof(address)) &&
         !listen(listener, 4));
  socklen_t address_length = sizeof(address);
  assert(!getsockname(listener, (struct sockaddr *)&address, &address_length));
  platform_socket event_listener =
                      platform_socket_open(AF_INET, SOCK_STREAM, 0),
                  data_socket = platform_socket_open(AF_INET, SOCK_DGRAM, 0),
                  control_socket = platform_socket_open(AF_INET, SOCK_DGRAM, 0);
  assert(event_listener != (platform_socket)EVUTIL_INVALID_SOCKET &&
         data_socket != (platform_socket)EVUTIL_INVALID_SOCKET &&
         control_socket != (platform_socket)EVUTIL_INVALID_SOCKET);
  platform_socket sockets[] = {event_listener, data_socket, control_socket};
  unsigned short *ports[] = {&event_port, &data_port, &control_port};
  for (unsigned i = 0; i < 3; ++i) {
    struct sockaddr_in endpoint = {0};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!bind(sockets[i], (struct sockaddr *)&endpoint, sizeof(endpoint)));
    socklen_t size = sizeof(endpoint);
    assert(!getsockname(sockets[i], (struct sockaddr *)&endpoint, &size));
    *ports[i] = ntohs(endpoint.sin_port);
  }
  assert(!listen(event_listener, 4));
  char port[8];
  snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
#ifdef _WIN32
  char directory[MAX_PATH], temporary[MAX_PATH], identity[37];
  assert(GetTempPathA(sizeof(temporary), temporary) &&
         !platform_uuid(identity));
  assert(snprintf(directory, sizeof(directory), "%swin2air-pairing-%s",
                  temporary, identity) > 0);
  assert(CreateDirectoryA(directory, NULL));
#else
  char directory[] = "/tmp/win2air-pairing-XXXXXX";
  assert(mkdtemp(directory));
#endif
  char path[512];
  assert(snprintf(path, sizeof(path), "%s/material", directory) > 0);
  platform_thread worker;
  assert(!platform_thread_start(&worker, peer, NULL));
  const char *modes[] = {"4", "0", "1", "5", "2", "3"};
  for (unsigned round = 0; round < sizeof(modes) / sizeof(*modes); ++round) {
    const char *mode = modes[round];
#ifdef _WIN32
    char command[2048];
    int length =
        snprintf(command, sizeof(command), "\"%s\" --client %s \"%s\" %s",
                 argv[0], port, path, mode);
    assert(length > 0 && (size_t)length < sizeof(command));
    STARTUPINFOA startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {0};
    assert(CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                          NULL, NULL, &startup, &process));
    assert(WaitForSingleObject(process.hProcess, 20000) == WAIT_OBJECT_0);
    DWORD status;
    assert(GetExitCodeProcess(process.hProcess, &status) && !status);
    assert(CloseHandle(process.hThread) && CloseHandle(process.hProcess));
#else
    char *arguments[] = {argv[0], "--client", port, path, (char *)mode, NULL};
    pid_t pid;
    assert(!posix_spawn(&pid, argv[0], NULL, NULL, arguments, environ));
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           !WEXITSTATUS(status));
#endif
  }
  assert(!platform_thread_join(worker));
  assert(connections == 8 && setups == 2 && verified == 4 && completed == 4 &&
         rejected == 1);
  assert(!unlink(path) && access(path, F_OK) == -1 && errno == ENOENT);
  assert(!rmdir(directory) && !platform_socket_close(listener));
#ifndef _WIN32
  alarm(0);
#endif
  for (unsigned i = 0; i < 3; ++i) assert(!platform_socket_close(sockets[i]));
  platform_cleanup();
  puts(
      "ordinary pairing, fresh control connection, caller storage and "
      "cross-process encrypted verification passed");
  return 0;
}
