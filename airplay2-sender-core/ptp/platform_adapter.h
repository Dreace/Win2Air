#ifndef AIRPTP_PLATFORM_ADAPTER_H
#define AIRPTP_PLATFORM_ADAPTER_H

#include "../platform/platform.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
/* Cached shared-memory calls are POSIX-only; the sentinel keeps the frozen
   error checks compiling where mmap does not exist. */
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif
#endif

/* Daemon wakeup pairs: nonblocking ends join the event loop, blocking ends
   carry the synchronous start handshake; TCP loopback pairs can short-read,
   so receives accumulate until the full payload arrives. */
int
airptp_wakeup_pair(platform_socket pair[2]);

int
airptp_wakeup_pair_blocking(platform_socket pair[2]);

ev_ssize_t
airptp_wakeup_send(platform_socket fd, const void *buffer, size_t length);

ev_ssize_t
airptp_wakeup_recv(platform_socket fd, void *buffer, size_t length);

void
airptp_wakeup_close(platform_socket fd);

/* Bounded sleep between Sync and Follow_Up; the two-step cadence stays 125 ms. */
void
airptp_sleep_us(unsigned useconds);

/* Last socket error rendered for diagnostics; POSIX keeps strerror, Windows maps WSA codes. */
const char *
airptp_socket_error_message(char *buffer, size_t capacity);

/* Shared daemon discovery and publishing require POSIX shared memory. */
bool
airptp_shared_supported(void);

#endif // AIRPTP_PLATFORM_ADAPTER_H
