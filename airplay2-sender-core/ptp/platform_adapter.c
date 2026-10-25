#include "platform_adapter.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

int
airptp_wakeup_pair(platform_socket pair[2])
{
  return platform_wakeup_pair(pair);
}

int
airptp_wakeup_pair_blocking(platform_socket pair[2])
{
  if (platform_wakeup_pair(pair) < 0)
    return -1;
  if (platform_socket_configure(pair[0], false) < 0 || platform_socket_configure(pair[1], false) < 0) {
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
    pair[0] = pair[1] = (platform_socket)EVUTIL_INVALID_SOCKET;
    return -1;
  }
  return 0;
}

ev_ssize_t
airptp_wakeup_send(platform_socket fd, const void *buffer, size_t length)
{
  size_t sent = 0;
  while (sent < length) {
#ifdef _WIN32
    ev_ssize_t n = platform_send(fd, (const char *)buffer + sent, length - sent, 0);
#else
    ssize_t n = write(fd, (const char *)buffer + sent, length - sent);
#endif
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    sent += (size_t)n;
  }
  return (ev_ssize_t)sent;
}

ev_ssize_t
airptp_wakeup_recv(platform_socket fd, void *buffer, size_t length)
{
  size_t received = 0;

  while (received < length) {
#ifdef _WIN32
    ev_ssize_t n = platform_recv(fd, (char *)buffer + received, length - received, 0);
#else
    ssize_t n = read(fd, (char *)buffer + received, length - received);
#endif
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return (ev_ssize_t)-1;
    received += (size_t)n;
  }

  return (ev_ssize_t)received;
}

void
airptp_wakeup_close(platform_socket fd)
{
  platform_socket_close(fd);
}

void
airptp_sleep_us(unsigned useconds)
{
#ifdef _WIN32
  struct timespec started, now;
  int64_t target = (int64_t)useconds * 1000;
  int64_t elapsed;

  if (target <= 0 || platform_time(PLATFORM_MONOTONIC, &started) < 0)
    return;
  for (;;) {
    if (platform_time(PLATFORM_MONOTONIC, &now) < 0)
      return;
    elapsed = ((int64_t)now.tv_sec - (int64_t)started.tv_sec) * INT64_C(1000000000)
        + (int64_t)now.tv_nsec - (int64_t)started.tv_nsec;
    if (elapsed >= target)
      return;
  }
#else
  usleep(useconds);
#endif
}

const char *
airptp_socket_error_message(char *buffer, size_t capacity)
{
#ifdef _WIN32
  snprintf(buffer, capacity, "WSA error %d", (int)WSAGetLastError());
  return buffer;
#else
  (void)buffer;
  (void)capacity;
  return strerror(errno);
#endif
}

bool
airptp_shared_supported(void)
{
#ifdef _WIN32
  return false;
#else
  return true;
#endif
}
