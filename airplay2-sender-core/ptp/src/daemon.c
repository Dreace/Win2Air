/*
MIT License

Copyright (c) 2026 OwnTone

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
// For shm_open
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#include "airptp_internal.h"
#include "ptp_msg_handle.h"
#include "../platform_adapter.h"

#define DAEMON_INTERVAL_SECS_SHM_UPDATE 5

struct daemon_start_result
{
  enum airptp_error retval;
  const char *errmsg;
  struct airptp_daemon_info daemon_info;
};

static struct timeval daemon_send_announce_tv =
{
  .tv_sec = AIRPTP_INTERVAL_MS_ANNOUNCE / 1000,
  .tv_usec = (AIRPTP_INTERVAL_MS_ANNOUNCE % 1000) * 1000
};
static struct timeval daemon_send_signaling_tv =
{
  .tv_sec = AIRPTP_INTERVAL_MS_SIGNALING / 1000,
  .tv_usec = (AIRPTP_INTERVAL_MS_SIGNALING % 1000) * 1000
};
static struct timeval daemon_send_sync_tv =
{
  .tv_sec = AIRPTP_INTERVAL_MS_SYNC / 1000,
  .tv_usec = (AIRPTP_INTERVAL_MS_SYNC % 1000) * 1000
};
#ifndef _WIN32
static struct timeval daemon_shm_update_tv =
{
  .tv_sec = DAEMON_INTERVAL_SECS_SHM_UPDATE,
  .tv_usec = 0
};
#endif

static void
daemon_info_fill(struct airptp_daemon_info *info, uint64_t clock_id, struct airptp_service *event_svc, struct airptp_service *general_svc)
{
  info->version_major = AIRPTP_SHM_STRUCTS_VERSION_MAJOR;
  info->version_minor = AIRPTP_SHM_STRUCTS_VERSION_MINOR;
  info->clock_id = clock_id;
  info->ts = time(NULL);
  info->event_port = event_svc->port;
  info->general_port = general_svc->port;
  info->ipv4_enabled = (event_svc->socket.fd4 != (platform_socket)EVUTIL_INVALID_SOCKET && general_svc->socket.fd4 != (platform_socket)EVUTIL_INVALID_SOCKET);
  info->ipv6_enabled = (event_svc->socket.fd6 != (platform_socket)EVUTIL_INVALID_SOCKET && general_svc->socket.fd6 != (platform_socket)EVUTIL_INVALID_SOCKET);
}

#ifndef _WIN32
static void
daemon_shm_destroy(struct airptp_daemon_info *shm, int fd)
{
  if (shm != MAP_FAILED)
    munmap(shm, sizeof(struct airptp_daemon_info));
  if (fd >= 0)
    close(fd);
  shm_unlink(AIRPTP_SHM_NAME);
}

static int
daemon_shm_create(struct airptp_daemon_info **shm, uint64_t clock_id, struct airptp_service *event_svc, struct airptp_service *general_svc)
{
  struct airptp_daemon_info *info = MAP_FAILED;
  int fd;
  int ret;

  shm_unlink(AIRPTP_SHM_NAME);

  fd = shm_open(AIRPTP_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0644);
  if (fd < 0)
    goto error;

  ret = ftruncate(fd, sizeof(struct airptp_daemon_info));
  if (ret < 0)
    goto error;

  info = mmap(NULL, sizeof(struct airptp_daemon_info), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (info == MAP_FAILED)
    goto error;

  daemon_info_fill(info, clock_id, event_svc, general_svc);

  *shm = info;

  return fd;

 error:
  daemon_shm_destroy(info, fd);
  return -1;   
}
#endif

static void
service_stop(struct airptp_service *svc)
{
  if (svc->ev4)
    event_free(svc->ev4);
  if (svc->ev6)
    event_free(svc->ev6);

  svc->ev4 = NULL;
  svc->ev6 = NULL;
}

static int
service_start(struct airptp_service *svc, event_callback_fn cb, struct airptp_daemon *daemon)
{
  if (svc->socket.fd4 != (platform_socket)EVUTIL_INVALID_SOCKET) {
    svc->ev4 = event_new(daemon->evbase, svc->socket.fd4, EV_READ | EV_PERSIST, cb, daemon);
    if (!svc->ev4)
      goto error;

    if (event_add(svc->ev4, NULL) < 0)
      goto error;
  }

  if (svc->socket.fd6 != (platform_socket)EVUTIL_INVALID_SOCKET) {
    svc->ev6 = event_new(daemon->evbase, svc->socket.fd6, EV_READ | EV_PERSIST, cb, daemon);
    if (!svc->ev6)
      goto error;

    if (event_add(svc->ev6, NULL) < 0)
      goto error;
  }

  return 0;

 error:
  service_stop(svc);
  return -1;
}


/* ------------------------------ Peer handling ----------------------------- */

static void
peer_clear(struct airptp_peer *peer)
{
  memset(peer, 0, sizeof(struct airptp_peer));
}

static void
peers_prune(struct airptp_daemon *daemon)
{
  struct airptp_peer *peer;
  int i;
  int n_pruned;

  for (i = 0, n_pruned = 0; i < daemon->num_peers; i++)
    {
      peer = &daemon->peers[i];
      if (!peer->is_active)
	{
	  airptp_logmsg("Removing inactive peer with id %" PRIu32, peer->id);
	  peer_clear(peer);
	  n_pruned++;
	  continue;
	}

      if (n_pruned > 0)
	daemon->peers[i - n_pruned] = *peer;
    }

  daemon->num_peers -= n_pruned;
}

static void
peer_last_seen_update(struct airptp_daemon *daemon, union utils_net_sockaddr *peer_addr, socklen_t peer_addrlen)
{
  int i;

  for (i = 0; i < daemon->num_peers; i++) {
    if (utils_net_address_is_same(peer_addr, &daemon->peers[i].naddr)) {
      daemon->peers[i].last_seen = time(NULL);
      break;
    }
  }
}

static bool
peer_exists(struct airptp_daemon *daemon, struct airptp_peer *peer)
{
  int i;

  for (i = 0; i < daemon->num_peers; i++) {
    if (peer->id == daemon->peers[i].id)
      return true;
  }

  return false;
}

int
daemon_peer_add(struct airptp_daemon *daemon, struct airptp_peer *peer)
{
  char straddr[64];
  uint32_t scope_id;

  // Clean up dead peers
  peers_prune(daemon);

  utils_net_address_get(straddr, sizeof(straddr), &peer->naddr);

  if (daemon->num_peers >= AIRPTP_MAX_PEERS) {
    airptp_logmsg("Max number of PTP peers reached (num_peers %d), can't add %s", daemon->num_peers, straddr);
    return -1;
  }

  if (peer_exists(daemon, peer)) {
    airptp_logmsg("PTP peer %s already in list, num_peers %d", straddr, daemon->num_peers);
    return -1;
  }

  peer->last_seen = time(NULL);
  peer->is_active = true;
  memcpy(&daemon->peers[daemon->num_peers], peer, sizeof(struct airptp_peer));
  daemon->num_peers++;

  // Trigger announce and signaling immediately
  event_active(daemon->send_announce_timer, 0, 0);
  event_active(daemon->send_signaling_timer, 0, 0);

  // We should send sync's at specific interval, so if already running don't
  // disturb the rhythm. I.e. only trigger if not running already.
  if (!event_pending(daemon->send_sync_timer, EV_TIMEOUT, NULL))
    event_add(daemon->send_sync_timer, &daemon_send_sync_tv);

  scope_id = (peer->naddr.sa.sa_family == AF_INET6) ? peer->naddr.sin6.sin6_scope_id : 0;
  airptp_logmsg("Added peer id %" PRIu32 ", address %s, scope id %u, num_peers %d", peer->id, straddr, scope_id, daemon->num_peers);
  return 0;
}

int
daemon_peer_del(struct airptp_daemon *daemon, struct airptp_peer *peer)
{
  uint32_t peer_id = peer->id;
  bool found;
  int i;

  for (i = 0, found = false; i < daemon->num_peers; i++)
    {
      peer = &daemon->peers[i];
      if (!found && (peer_id == peer->id))
	{
	  peer_clear(peer);
	  found = true;
	  continue;
	}

      // Make sure the list is sequential
      if (found)
	daemon->peers[i - 1] = *peer;
    }

  if (!found) {
    airptp_logmsg("Can't remove PTP peer, not in our list");
    return -1;
  }

  daemon->num_peers--;
  airptp_logmsg("Removed peer id %" PRIu32 ", num_peers %d", peer_id, daemon->num_peers);
  return 0;
}


/* ------------------------------ Event handling ---------------------------- */

static void
send_announce_cb(platform_socket fd, short what, void *arg)
{
  struct airptp_daemon *daemon = arg;

  if (daemon->num_peers == 0)
    return; // Don't reschedule

  ptp_msg_announce_send(daemon);

  event_add(daemon->send_announce_timer, &daemon_send_announce_tv);
}

static void
send_signaling_cb(platform_socket fd, short what, void *arg)
{
  struct airptp_daemon *daemon = arg;

  if (daemon->num_peers == 0)
    return; // Don't reschedule

  ptp_msg_signaling_send(daemon);

  event_add(daemon->send_signaling_timer, &daemon_send_signaling_tv);
}

static void
send_sync_cb(platform_socket fd, short what, void *arg)
{
  struct airptp_daemon *daemon = arg;

  if (daemon->num_peers == 0)
    return; // Don't reschedule

  ptp_msg_sync_send(daemon);

  event_add(daemon->send_sync_timer, &daemon_send_sync_tv);
}

static void
incoming_cb(platform_socket fd, short what, void *arg)
{
  struct airptp_daemon *daemon = arg;
  union utils_net_sockaddr peer_addr;
  socklen_t peer_addrlen = sizeof(peer_addr);
  uint8_t req[1024];
  ev_ssize_t len;
  char error_buffer[64];

  // Shouldn't be necessary, but silences scan-build complaint about sa_family
  // possibly being garbage after recvfrom()
  peer_addr.sa.sa_family = AF_UNSPEC;

  len = platform_recvfrom(fd, req, sizeof(req), 0, &peer_addr.sa, &peer_addrlen);
  if (len <= 0 || peer_addr.sa.sa_family == AF_UNSPEC)
    {
      if (len < 0)
	airptp_logmsg("Service read error: %s", airptp_socket_error_message(error_buffer, sizeof(error_buffer)));
      return;
    }

  peer_last_seen_update(daemon, &peer_addr, peer_addrlen);

  ptp_msg_handle(daemon, req, len, &peer_addr, peer_addrlen);
}

#ifndef _WIN32
static void
shm_update_cb(platform_socket fd, short what, void *arg)
{
  struct airptp_daemon *daemon = arg;

  daemon->info->ts = time(NULL);

  event_add(daemon->shm_update_timer, &daemon_shm_update_tv);
}
#endif

// Daemon thread
static int
loop_start_signal(enum airptp_error retval, const char *errmsg, struct airptp_daemon *daemon, struct airptp_daemon_info *info)
{
  struct daemon_start_result start_result = { .retval = retval, .errmsg = errmsg };
  ev_ssize_t ret;

  if (info)
    memcpy(&start_result.daemon_info, info, sizeof(struct airptp_daemon_info));

  ret = airptp_wakeup_send(daemon->start_pipe[1], &start_result, sizeof(start_result));
  airptp_wakeup_close(daemon->start_pipe[1]);
  daemon->start_pipe[1] = (platform_socket)EVUTIL_INVALID_SOCKET;
  if (ret != (ev_ssize_t)sizeof(start_result))
    airptp_logmsg("Error writing thread start result");
  return ret == (ev_ssize_t)sizeof(start_result) ? 0 : -1;
}

// Parent thread
static enum airptp_error
loop_start_wait(const char **errmsg, platform_socket start_fd, struct airptp_daemon_info *info)
{
  struct daemon_start_result start_result = {0};
  ev_ssize_t ret;

  ret = airptp_wakeup_recv(start_fd, &start_result, sizeof(start_result));
  if (ret != (ev_ssize_t)sizeof(start_result)) {
    start_result.retval = AIRPTP_ERR_INTERNAL;
    start_result.errmsg = "Error reading thread start result";
  }

  memcpy(info, &start_result.daemon_info, sizeof(struct airptp_daemon_info));

  *errmsg = start_result.errmsg;
  return start_result.retval;
}

static void
start_stop_cb(platform_socket fd, short what, void *arg)
{
  struct airptp_daemon *daemon = arg;
  char buf[1];

  if (what != EV_READ) {
    airptp_logmsg("Starting airptp event loop");
    if (event_add(daemon->start_stop_ev, NULL) < 0) {
      loop_start_signal(AIRPTP_ERR_INTERNAL, "Error registering loop stop event", daemon, NULL);
      event_base_loopbreak(daemon->evbase);
    } else if (loop_start_signal(AIRPTP_OK, NULL, daemon, daemon->info) < 0)
      event_base_loopbreak(daemon->evbase);
  } else {
    airptp_logmsg("Stopping airptp event loop");
    if (airptp_wakeup_recv(fd, buf, 1) < 0)
      airptp_logmsg("Unexpected error from start_stop_cb read");

    event_base_loopbreak(daemon->evbase);
  }
}


/* ------------------------------- Main loop -------------------------------- */

// Runs a PTP clock daemon either shared (with a shared mem interface) or
// private
static void *
run(void *arg)
{
  struct airptp_daemon *daemon = arg;
  struct airptp_daemon_info info = { 0 };
  struct timeval now = { 0 };
#ifndef _WIN32
  int shm_fd = -1;
#endif
  int ret;

  airptp_callbacks_register(&daemon->cb);
  airptp_thread_name_set("libairptp");

  ret = service_start(&daemon->event_svc, incoming_cb, daemon);
  if (ret < 0)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error creating ptp event service");

  ret = service_start(&daemon->general_svc, incoming_cb, daemon);
  if (ret < 0)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error creating ptp general service");

  daemon->start_stop_ev = event_new(daemon->evbase, daemon->exit_pipe[0], EV_READ, start_stop_cb, daemon);
  if (!daemon->start_stop_ev)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error creating loop start stop event");
  if (event_add(daemon->start_stop_ev, &now) < 0)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error registering loop start event");

  daemon->send_announce_timer = evtimer_new(daemon->evbase, send_announce_cb, daemon);
  daemon->send_signaling_timer = evtimer_new(daemon->evbase, send_signaling_cb, daemon);
  daemon->send_sync_timer = evtimer_new(daemon->evbase, send_sync_cb, daemon);
  if (!daemon->send_announce_timer || !daemon->send_signaling_timer || !daemon->send_sync_timer)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error creating ptp timers");

#ifndef _WIN32
  if (daemon->is_shared) {
    shm_fd = daemon_shm_create(&daemon->info, daemon->clock_id, &daemon->event_svc, &daemon->general_svc);
    if (shm_fd < 0)
      RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error creating shared memory");

    daemon->shm_update_timer = evtimer_new(daemon->evbase, shm_update_cb, daemon);
    if (!daemon->shm_update_timer)
      RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error creating shared memory update timer");
    if (event_add(daemon->shm_update_timer, &daemon_shm_update_tv) < 0)
      RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error registering shared memory update timer");
  } else
#endif
  {
    daemon_info_fill(&info, daemon->clock_id, &daemon->event_svc, &daemon->general_svc);
    daemon->info = &info;
  }

  event_base_dispatch(daemon->evbase);

 error:
  if (daemon->shm_update_timer)
    event_free(daemon->shm_update_timer);
  if (daemon->send_announce_timer)
    event_free(daemon->send_announce_timer);
  if (daemon->send_signaling_timer)
    event_free(daemon->send_signaling_timer);
  if (daemon->send_sync_timer)
    event_free(daemon->send_sync_timer);
  if (daemon->start_stop_ev)
    event_free(daemon->start_stop_ev);
  daemon->shm_update_timer = NULL;
  daemon->send_announce_timer = NULL;
  daemon->send_signaling_timer = NULL;
  daemon->send_sync_timer = NULL;
  daemon->start_stop_ev = NULL;
#ifndef _WIN32
  if (daemon->is_shared)
    daemon_shm_destroy(daemon->info, shm_fd);
#endif
  service_stop(&daemon->general_svc);
  service_stop(&daemon->event_svc);

  // Initialization error before event loop dispatch, tell our parent
  if (ret != 0)
    loop_start_signal(ret, airptp_errmsg, daemon, NULL);

  daemon->info = MAP_FAILED;

  return NULL;
}

static void
daemon_cleanup(struct airptp_daemon *daemon)
{
  if (daemon->start_pipe[0] != (platform_socket)EVUTIL_INVALID_SOCKET)
    airptp_wakeup_close(daemon->start_pipe[0]);
  if (daemon->start_pipe[1] != (platform_socket)EVUTIL_INVALID_SOCKET)
    airptp_wakeup_close(daemon->start_pipe[1]);
  if (daemon->exit_pipe[0] != (platform_socket)EVUTIL_INVALID_SOCKET)
    airptp_wakeup_close(daemon->exit_pipe[0]);
  if (daemon->exit_pipe[1] != (platform_socket)EVUTIL_INVALID_SOCKET)
    airptp_wakeup_close(daemon->exit_pipe[1]);
  if (daemon->evbase)
    event_base_free(daemon->evbase);
  daemon->start_pipe[0] = daemon->start_pipe[1] = (platform_socket)EVUTIL_INVALID_SOCKET;
  daemon->exit_pipe[0] = daemon->exit_pipe[1] = (platform_socket)EVUTIL_INVALID_SOCKET;
  daemon->evbase = NULL;
}

enum airptp_error
daemon_start(struct airptp_daemon *daemon, struct airptp_daemon_info *info, bool is_shared, uint64_t clock_id, struct airptp_callbacks cb)
{
  int ret;
  bool thread_started = false;

  daemon->start_pipe[0] = (platform_socket)EVUTIL_INVALID_SOCKET;
  daemon->start_pipe[1] = (platform_socket)EVUTIL_INVALID_SOCKET;
  daemon->exit_pipe[0] = (platform_socket)EVUTIL_INVALID_SOCKET;
  daemon->exit_pipe[1] = (platform_socket)EVUTIL_INVALID_SOCKET;

  ret = ptp_msg_handle_init();
  if (ret < 0)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Message handler failed to initialize");

  ret = airptp_wakeup_pair(daemon->exit_pipe);
  if (ret < 0)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Couldn't create daemon exit pipe");

  ret = airptp_wakeup_pair_blocking(daemon->start_pipe);
  if (ret < 0)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Couldn't create daemon start pipe");

  daemon->info = MAP_FAILED;
  daemon->is_shared = is_shared;
  daemon->clock_id = clock_id;
  daemon->cb = cb;

  daemon->evbase = event_base_new();
  if (!daemon->evbase)
    RETURN_ERROR(AIRPTP_ERR_OOM, "Out of memory");

  ret = platform_thread_start(&daemon->tid, run, daemon);
  if (ret < 0)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error spawning daemon thread");
  thread_started = true;

  ret = loop_start_wait(&airptp_errmsg, daemon->start_pipe[0], info);
  if (ret < 0)
    RETURN_ERROR(ret, airptp_errmsg);

  daemon->is_running = true;

  return AIRPTP_OK;

 error:
  if (thread_started) {
    airptp_wakeup_close(daemon->exit_pipe[1]);
    daemon->exit_pipe[1] = (platform_socket)EVUTIL_INVALID_SOCKET;
    platform_thread_join(daemon->tid);
  }
  daemon_cleanup(daemon);
  return ret;
}

enum airptp_error
daemon_stop(struct airptp_daemon *daemon)
{
  char byte = 1;
  int ret = AIRPTP_OK;

  if (!daemon->is_running)
    return AIRPTP_OK; // No-op

  ret = airptp_wakeup_send(daemon->exit_pipe[1], &byte, 1);
  bool wakeup_failed = ret != 1;
  if (wakeup_failed) {
    airptp_logmsg("Error writing to exit pipe; closing writer to wake daemon");
    airptp_wakeup_close(daemon->exit_pipe[1]);
    daemon->exit_pipe[1] = (platform_socket)EVUTIL_INVALID_SOCKET;
  }

  ret = platform_thread_join(daemon->tid);
  if (ret != 0)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "Error joining daemon thread");
  daemon->is_running = false;
  if (wakeup_failed) {
    ret = AIRPTP_ERR_INTERNAL;
    airptp_errmsg = "Error writing to exit pipe; daemon joined and cleaned up";
  }

 error:
  daemon_cleanup(daemon);
  return ret;
}
