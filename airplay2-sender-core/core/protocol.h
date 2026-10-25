#ifndef SENDER_PROTOCOL_H
#define SENDER_PROTOCOL_H
#include <event2/buffer.h>
#include <event2/event.h>
#include <limits.h>
#include <sodium.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../platform/platform.h"
#include "sender.h"

#define STOB(s, b, c) ((s) * (c) * (b) / 8)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
/* Retained protocol models from OwnTone misc.h and outputs.h. */
struct media_quality {
  int sample_rate, bits_per_sample, channels, bit_rate;
};
struct net_socket {
  platform_socket fd4, fd6;
};
union net_sockaddr {
  struct sockaddr_in sin;
  struct sockaddr_in6 sin6;
  struct sockaddr sa;
  struct sockaddr_storage ss;
};
struct protocol_peer {
  uint64_t id;
  char *name, *v4_address, *v6_address, *password, *auth_key;
  uint16_t v4_port, v6_port;
  int volume, offset_ms, requires_auth, v6_disabled;
  int presentation_base_ms;
  uint32_t receiver_render_ms;
  bool receiver_render_available;
  bool password_auth_started;
  sender_buffer_config buffers;
  uint64_t features;
  struct media_quality quality;
  void *session, *extra_device_info;
};
struct output_data {
  void *buffer;
  size_t bufsize;
  unsigned samples;
  struct media_quality quality;
};
struct output_buffer {
  struct timespec pts;
  struct output_data data[2];
};
enum output_device_state {
  OUTPUT_STATE_PASSWORD,
  OUTPUT_STATE_FAILED,
  OUTPUT_STATE_STOPPED,
  OUTPUT_STATE_STARTUP,
  OUTPUT_STATE_CONNECTED,
  OUTPUT_STATE_STREAMING
};
bool quality_is_equal(struct media_quality *, struct media_quality *);
int safe_hextou64(const char *, uint64_t *);
int net_address_get(char *, size_t, union net_sockaddr *);
int net_sockaddr_get(union net_sockaddr *, const char *, unsigned short);
int net_port_get(unsigned short *, union net_sockaddr *);
int net_if_get(char *, size_t, const char *);
int net_mac_get(uint8_t *, size_t, const char *);
platform_socket net_connect(const char *, unsigned short, int, const char *);
int net_bind(struct net_socket *, unsigned short *, int, const char *);
void net_socket_close(struct net_socket *);
int uuid_make(char *);
void thread_setname(const char *);
void secure_string_free(char *);
const void *protocol_bytes_find(const void *, size_t, const void *, size_t);
struct protocol_peer *protocol_peer_get(uint64_t);
void protocol_peer_session_add(uint64_t, void *);
void protocol_peer_session_remove(uint64_t);
void protocol_state_changed(int, uint64_t, enum output_device_state);
void protocol_credentials_changed(const char *);
void protocol_operation(const char *, int);
void protocol_end_error(int);
void protocol_authenticating(void);
void protocol_step_changed(sender_step);
void protocol_timing_applied(uint32_t, uint32_t, uint32_t);
void protocol_timing_sync(uint32_t current_pos, uint32_t minimum_rtp,
                          uint32_t next_rtp, uint32_t pending,
                          uint64_t capture_ns);
void protocol_diagnostic(const char *, ...);
enum protocol_statistic {
  PROTOCOL_SENT,
  PROTOCOL_BYTES,
  PROTOCOL_RESEND_REQUEST,
  PROTOCOL_RESEND_SUCCESS,
  PROTOCOL_RESEND_EXPIRED,
  PROTOCOL_SEND_FAILURE,
  PROTOCOL_PENDING
};
void protocol_statistic(enum protocol_statistic, uint64_t);
int protocol_start(struct event_base *, struct protocol_peer *, uint64_t,
                   const char *, const char *);
int protocol_write(const void *, size_t, struct timespec);
int protocol_flush(struct protocol_peer *);
int protocol_set_volume(struct protocol_peer *, uint64_t command_id,
                        uint32_t volume_percent);
int protocol_set_presentation(struct protocol_peer *, uint64_t command_id,
                              const sender_presentation *);
int protocol_retarget_presentation(struct protocol_peer *,
                                   const sender_presentation *);
void protocol_cancel_presentation(struct protocol_peer *);
void protocol_presentation_completed(uint64_t command_id, sender_result,
                                     int peer_code, int platform_code);
void protocol_volume_completed(uint64_t command_id, sender_result,
                               int peer_code, int platform_code);
void protocol_receiver_volume(double db);
void protocol_receiver_media(uint32_t command);
void protocol_metadata_completed(sender_result, int peer_code);
int protocol_refresh_volume(struct protocol_peer *);
int protocol_now_playing(struct protocol_peer *, const char *, const char *,
                         const char *, const char *, const void *, size_t);
int protocol_end(struct protocol_peer *);
void protocol_cleanup(void);
#endif
