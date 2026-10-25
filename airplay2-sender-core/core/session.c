/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <errno.h>
#include <sys/queue.h>
#include <time.h>


#include <event2/event.h>
#include <event2/buffer.h>
#include <gcrypt.h>

#include "plist_wrap.h"

#include "evrtsp.h"
#include "rtsp-internal.h"

#include "logger.h"

#include "protocol.h"
#include "crypto/chacha.h"




#include "rtp_common.h"
#include "codec.h"
#include "ptpd.h"


#include "airplay_events.h"
#include "pairing/pair.h"

/* List of TODO's for AirPlay 2
 *
 * inplace encryption
 * latency needs different handling
 * support ipv6, e.g. in SETPEERS
 *
 */

// Airplay 2 has a gazallion parameters, many of them unknown to us. With the
// below it is possible to easily try different variations.
#define AIRPLAY_USE_STREAMID                 0
#define AIRPLAY_USE_PAIRING_TRANSIENT        1
#define AIRPLAY_USE_AUTH_SETUP               0

// Full traffic dumps in the log in debug mode
#define AIRPLAY_DUMP_TRAFFIC                 0

#define AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT     44100
#define AIRPLAY_QUALITY_BITS_PER_SAMPLE_DEFAULT 16
#define AIRPLAY_QUALITY_CHANNELS_DEFAULT        2

// AirTunes v2 number of samples per packet
// Probably using this value because 44100/352 and 48000/352 has good 32 byte
// alignment, which improves performance of some encoders
#define AIRPLAY_SAMPLES_PER_PACKET              352

// 0x60 Real Time Audio, 0x67 Buffered
#define AIRPLAY_RTP_PAYLOADTYPE                 0x60

// Quoting shairport-sync's rtp.c:
// The value of 11025 (0.25 seconds) is a guess based on the "Audio-Latency"
// parameter returned by an AE. Sigh, it would be nice to have a published
// protocol...
#define AIRPLAY_AUDIO_LATENCY_MS                250

// For transient pairing the key_len will be 64 bytes, but only 32 are used for
// audio payload encryption. For normal pairing the key is 32 bytes.
#define AIRPLAY_AUDIO_KEY_LEN 32

// How many RTP packets to buffer for retransmission
#define AIRPLAY_PACKET_BUFFER_SIZE    1000

#define AIRPLAY_MD_DELAY_STARTUP      15360
#define AIRPLAY_MD_DELAY_SWITCH       (AIRPLAY_MD_DELAY_STARTUP * 2)
#define AIRPLAY_MD_WANTS_TEXT         (1 << 0)
#define AIRPLAY_MD_WANTS_ARTWORK      (1 << 1)
#define AIRPLAY_MD_WANTS_PROGRESS     (1 << 2)

// ATV4 and Homepod disconnect for reasons that are not clear, but sending them
// progress metadata at regular intervals reduces the problem. The below
// interval was determined via testing, see:
// https://github.com/owntone/owntone-server/issues/734#issuecomment-622959334
#define AIRPLAY_KEEP_ALIVE_INTERVAL   25

// This is an arbitrary value which just needs to be kept in sync with the config
#define AIRPLAY_CONFIG_MAX_VOLUME     11

/* Keep in sync with const char *airplay_devtype */
enum airplay_devtype {
  AIRPLAY_DEV_APEX2_80211N,
  AIRPLAY_DEV_APEX3_80211N,
  AIRPLAY_DEV_APPLETV,
  AIRPLAY_DEV_APPLETV4,
  AIRPLAY_DEV_HOMEPOD,
  AIRPLAY_DEV_OTHER,
};

// Session is starting up
#define AIRPLAY_STATE_F_STARTUP    (1 << 13)
// Streaming is up (connection established)
#define AIRPLAY_STATE_F_CONNECTED  (1 << 14)
// Couldn't start device
#define AIRPLAY_STATE_F_FAILED     (1 << 15)

enum airplay_state {
  // Device is stopped (no session)
  AIRPLAY_STATE_STOPPED   = 0,
  // Session startup
  AIRPLAY_STATE_INFO      = AIRPLAY_STATE_F_STARTUP | 0x01,
  AIRPLAY_STATE_ENCRYPTED = AIRPLAY_STATE_F_STARTUP | 0x02,
  AIRPLAY_STATE_SETUP     = AIRPLAY_STATE_F_STARTUP | 0x03,
  AIRPLAY_STATE_RECORD    = AIRPLAY_STATE_F_STARTUP | 0x04,
  // Session established
  // - streaming ready (RECORD sent and acked, connection established)
  // - commands (SET_PARAMETER) are possible
  AIRPLAY_STATE_CONNECTED = AIRPLAY_STATE_F_CONNECTED | 0x01,
  // Media data is being sent
  AIRPLAY_STATE_STREAMING = AIRPLAY_STATE_F_CONNECTED | 0x02,
  // Session teardown in progress (-> going to STOPPED state)
  AIRPLAY_STATE_TEARDOWN  = AIRPLAY_STATE_F_CONNECTED | 0x03,
  // Session is failed, couldn't startup or error occurred
  AIRPLAY_STATE_FAILED    = AIRPLAY_STATE_F_FAILED | 0x01,
  // Pending PIN or password
  AIRPLAY_STATE_AUTH      = AIRPLAY_STATE_F_FAILED | 0x02,
};

enum airplay_seq_type
{
  AIRPLAY_SEQ_ABORT = -1,
  AIRPLAY_SEQ_START,
  AIRPLAY_SEQ_START_PLAYBACK,
  AIRPLAY_SEQ_PROBE,
  AIRPLAY_SEQ_FLUSH,
  AIRPLAY_SEQ_STOP,
  AIRPLAY_SEQ_FAILURE,
  AIRPLAY_SEQ_PIN_START,
  AIRPLAY_SEQ_SEND_VOLUME,
  AIRPLAY_SEQ_PAIR_SETUP,
  AIRPLAY_SEQ_PAIR_VERIFY,
  AIRPLAY_SEQ_PAIR_TRANSIENT,
  AIRPLAY_SEQ_FEEDBACK,
  AIRPLAY_SEQ_CONTINUE, // Must be last element
};

// From https://openairplay.github.io/airplay-spec/status_flags.html
enum airplay_status_flags
{
  AIRPLAY_FLAG_PROBLEM_DETECTED               = (1 << 0),
  AIRPLAY_FLAG_NOT_CONFIGURED                 = (1 << 1),
  AIRPLAY_FLAG_AUDIO_CABLE_ATTACHED           = (1 << 2),
  AIRPLAY_FLAG_PIN_REQUIRED                   = (1 << 3),
  AIRPLAY_FLAG_SUPPORTS_FROM_CLOUD            = (1 << 6),
  AIRPLAY_FLAG_PASSWORD_REQUIRED              = (1 << 7),
  AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED      = (1 << 9),
  AIRPLAY_FLAG_SETUP_HK_ACCESS_CTRL           = (1 << 10),
  AIRPLAY_FLAG_SUPPORTS_RELAY                 = (1 << 11),
  AIRPLAY_FLAG_SILENT_PRIMARY                 = (1 << 12),
  AIRPLAY_FLAG_TIGHT_SYNC_IS_GRP_LEADER       = (1 << 13),
  AIRPLAY_FLAG_TIGHT_SYNC_BUDDY_NOT_REACHABLE = (1 << 14),
  AIRPLAY_FLAG_IS_APPLE_MUSIC_SUBSCRIBER      = (1 << 15),
  AIRPLAY_FLAG_CLOUD_LIBRARY_ON               = (1 << 16),
  AIRPLAY_FLAG_RECEIVER_IS_BUSY               = (1 << 17),
};

// Info about the device, which is not required by the player, only internally
struct airplay_extra
{
  enum airplay_devtype devtype;


  uint16_t wanted_metadata;
  bool supports_auth_setup;
  bool supports_encryption;
  bool use_ptp;
};

struct airplay_master_session
{
  struct evbuffer *input_buffer;
  uint32_t input_buffer_samples;

  // ALAC encoder and buffer for encoded data
  struct sender_codec *encode_ctx;
  struct evbuffer *encoded_buffer;

  struct rtp_session *rtp_session;

  struct rtcp_timestamp cur_stamp;
  bool stamp_valid;
  uint32_t pos_at_stamp;

  uint8_t *rawbuf;
  size_t rawbuf_size;
  uint32_t samples_per_packet;
  uint32_t silent_samples;

  // Session characteristics
  struct media_quality quality;
  bool use_ptp;

  // Number of samples that we tell the output to buffer (this will mean that
  // the position that we send in the sync packages are offset by this amount
  // compared to the rtptimes of the corresponding RTP packages we are sending)
  uint32_t output_buffer_samples;

  struct airplay_master_session *next;
};

struct volume_request {
  struct airplay_session *session;
  uint64_t command_id;
  int volume;
};
struct presentation_request {
  struct airplay_session *session;
  uint64_t command_id;
  sender_presentation candidate;
  bool setup;
};

struct airplay_session {
  uint64_t device_id;
  int callback_id;

  struct airplay_master_session *master_session;

  struct evrtsp_connection *ctrl;

  enum airplay_state state;

  enum airplay_seq_type next_seq;

  uint64_t statusflags;
  uint16_t wanted_metadata;
  bool req_has_auth;
  bool supports_auth_setup;
  bool supports_encryption;

  struct event *deferredev;
  struct event *pair_restart_event;
  bool pair_restart_pending;

  int reqs_in_flight;
  struct volume_request* volume_request;
  struct presentation_request* presentation_request;
  bool volume_query_pending, volume_query_unsupported, metadata_pending;
  time_t next_volume_query;
  int cseq;

  uint32_t session_id;
  char session_url[128];
  char session_uuid[37];

  char group_uuid[37];

  char *realm;
  char *nonce;
  const char *password;

  char *devname;
  char *address;
  int family;

  union net_sockaddr naddr;

  int volume;

  bool initial_volume_failed;

  // device->offset_ms in samples (user config for correction of static
  // amplifier or DSP delays)
  int offset_samples;

  char *local_v4_address;
  char *local_v6_address;
  char local_mac_address[18];

  unsigned short data_port;
  unsigned short control_port;
  unsigned short events_port;

  /* Pairing, see pair.h */
  enum pair_type pair_type;
  struct pair_cipher_context *control_cipher_ctx;
  struct pair_verify_context *pair_verify_ctx;
  struct pair_setup_context *pair_setup_ctx;

  uint8_t shared_secret[64];
  size_t shared_secret_len; // 32 or 64, see AIRPLAY_AUDIO_KEY_LEN for comment

  gcry_cipher_hd_t packet_cipher_hd;

  platform_socket server_fd;

  struct airplay_service *timing_svc;
  struct airplay_service *control_svc;

  uint32_t ptpd_slave_id;

  struct airplay_session *next;
};

struct airplay_service
{
  struct net_socket socket;
  unsigned short port;
  struct event *ev4;
  struct event *ev6;
};

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

// TODO move to rtp_common
struct ntp_stamp
{
  uint32_t sec;
  uint32_t frac;
};


/* --------------------------- SEQUENCE DEFINITIONS ------------------------- */

struct airplay_seq_definition
{
  enum airplay_seq_type seq_type;

  // Called when a sequence ends, successfully or not. Shoulds also, if
  // required, take care of notifying  player and free the session.
  void (*on_success)(struct airplay_session *session);
  void (*on_error)(struct airplay_session *session);
};

struct airplay_seq_request
{
  enum airplay_seq_type seq_type;
  const char *name; // Name of request (for logging)
  enum evrtsp_cmd_type rtsp_type;
  int (*payload_make)(struct evrtsp_request *req, struct airplay_session *session, void *arg);
  enum airplay_seq_type (*response_handler)(struct evrtsp_request *req, struct airplay_session *session);
  const char *content_type;
  const char *uri;
  bool proceed_on_rtsp_not_ok; // If true return code != RTSP_OK will not abort the sequence
};

struct airplay_seq_ctx
{
  struct airplay_seq_request *cur_request;
  void (*on_success)(struct airplay_session *session);
  void (*on_error)(struct airplay_session *session);
  struct airplay_session *session;
  void *payload_make_arg;
  const char *log_caller;
};


/* ------------------------------ MISC GLOBALS ------------------------------ */

#if AIRPLAY_USE_AUTH_SETUP
static const uint8_t airplay_auth_setup_pubkey[] =
  "\x59\x02\xed\xe9\x0d\x4e\xf2\xbd\x4c\xb6\x8a\x63\x30\x03\x82\x07"
  "\xa9\x4d\xbd\x50\xd8\xaa\x46\x5b\x5d\x8c\x01\x2a\x0c\x7e\x1d\x4e";
#endif

/* Struct with default quality levels */
static struct media_quality airplay_quality_default =
{
  AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT,
  AIRPLAY_QUALITY_BITS_PER_SAMPLE_DEFAULT,
  AIRPLAY_QUALITY_CHANNELS_DEFAULT,
  0
};

/* From player.c */
static struct event_base *evbase_player;

/* AirTunes v2 time synchronization */
static struct airplay_service airplay_timing_svc = { .socket = { -1, -1 } };

/* AirTunes v2 playback synchronization / control */
static struct airplay_service airplay_control_svc = { .socket = { -1, -1 } };

/* Sessions */
static struct airplay_master_session *airplay_master_sessions;
static struct airplay_session *airplay_sessions;

/* Our own device ID, name and user agent */
static uint64_t airplay_device_id;
static const char *airplay_client_name;
static const char *airplay_user_agent;
static struct event *keep_alive_timer;
static const struct timeval keep_alive_interval = {25, 0};

/* Precision Time Protocol */
static char airplay_ptp_clock_uuid[37];

// Forwards
static int
airplay_device_start(struct protocol_peer *device, int callback_id);
static void
sequence_start(enum airplay_seq_type seq_type, struct airplay_session *session, void *arg, const char *log_caller);
static void
sequence_continue(struct airplay_seq_ctx *seq_ctx);


/* ------------------------------- MISC HELPERS ----------------------------- */

static inline int
alac_encode(struct evbuffer *evbuf, struct sender_codec *encode_ctx, uint8_t *rawbuf, size_t rawbuf_size, int nsamples, struct media_quality *quality)
{
  return codec_alac_encode(evbuf, encode_ctx, rawbuf, rawbuf_size, nsamples, quality);
}

// AirTunes v2 time synchronization helpers
static inline void
timespec_to_ntp(struct timespec *ts, struct ntp_stamp *ns)
{
  // Seconds since NTP Epoch (1900-01-01)
  ns->sec = ts->tv_sec + NTP_EPOCH_DELTA;

  ns->frac = (uint32_t)((double)ts->tv_nsec * 1e-9 * FRAC);
}

/*
static inline void
ntp_to_timespec(struct ntp_stamp *ns, struct timespec *ts)
{
  // Seconds since Unix Epoch (1970-01-01)
  ts->tv_sec = ns->sec - NTP_EPOCH_DELTA;

  ts->tv_nsec = (long)((double)ns->frac / (1e-9 * FRAC));
}
*/

static inline int
timing_get_clock_ntp(struct ntp_stamp *ns)
{
  struct timespec ts;
  int ret;

  ret = platform_time(PLATFORM_MONOTONIC, &ts);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get clock: %s\n", strerror(errno));

      return -1;
    }

  timespec_to_ntp(&ts, ns);

  return 0;
}

// Converts uint64t airplay_device_id -> AA:BB:CC:DD:EE:FF:11:22
static void
device_id_colon_make(char *id_str, int size, uint64_t id)
{
  int r, w;

  snprintf(id_str, size, "%016" PRIX64, id);

  for (r = strlen(id_str) - 1, w = size - 2; r != w; r--, w--)
    {
      id_str[w] = id_str[r];
      if (r % 2 == 0)
        {
	  w--;
	  id_str[w] = ':';
	}
    }

  id_str[size - 1] = 0; // Zero terminate
}

// Converts AA:BB:CC:DD:EE:FF -> AABBCCDDEEFF -> uint64 id




/* ------------------------------- Crypto ----------------------------------- */





/* --------------------- Helpers for sending RTSP requests ------------------ */

static int
auth_header_add(struct evrtsp_request *req, const char *password, const char *realm, const char *nonce, const char *method, const char *uri)
{
  const char *hash_fmt = "%02x";
  const char *username = "";
  char ha1[33] = { 0 };
  char ha2[33] = { 0 };
  char ebuf[64];
  char auth[256];
  uint8_t *hash_bytes;
  size_t hashlen;
  gcry_md_hd_t hd;
  gpg_error_t gc_err;
  int i;
  int ret;

  if (!password)
    password = "";

  gc_err = gcry_md_open(&hd, GCRY_MD_MD5, 0);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_AIRPLAY, "Could not open MD5: %s\n", ebuf);
      return -1;
    }

  hashlen = gcry_md_get_algo_dlen(GCRY_MD_MD5);

  /* HA 1 */
  gcry_md_write(hd, username, strlen(username));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, realm, strlen(realm));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, password, strlen(password));

  hash_bytes = gcry_md_read(hd, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not read MD5 hash\n");
      return -1;
    }

  for (i = 0; i < hashlen; i++)
    sprintf(ha1 + (2 * i), hash_fmt, hash_bytes[i]);

  /* RESET */
  gcry_md_reset(hd);

  /* HA 2 */
  gcry_md_write(hd, method, strlen(method));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, uri, strlen(uri));

  hash_bytes = gcry_md_read(hd, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not read MD5 hash\n");
      return -1;
    }

  for (i = 0; i < hashlen; i++)
    sprintf(ha2 + (2 * i), hash_fmt, hash_bytes[i]);

  /* RESET */
  gcry_md_reset(hd);

  /* Final value */
  gcry_md_write(hd, ha1, 32);
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, nonce, strlen(nonce));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, ha2, 32);

  hash_bytes = gcry_md_read(hd, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not read MD5 hash\n");
      return -1;
    }

  for (i = 0; i < hashlen; i++)
    sprintf(ha1 + (2 * i), hash_fmt, hash_bytes[i]);

  gcry_md_close(hd);

  /* Build header */
  ret = snprintf(auth, sizeof(auth), "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
		 username, realm, nonce, uri, ha1);
  if ((ret < 0) || (ret >= sizeof(auth)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Authorization value header exceeds buffer size\n");
      return -1;
    }

  evrtsp_add_header(req->output_headers, "Authorization", auth);

  DPRINTF(E_SPAM, L_AIRPLAY, "Authorization header: %s\n", auth);

  return 0;
}

static int
auth_header_parse(struct airplay_session *session, struct evrtsp_request *req)
{
  const char *param;
  char *auth = NULL;
  char *token;
  char *ptr;

  if (session->realm)
    {
      free(session->realm);
      session->realm = NULL;
    }

  if (session->nonce)
    {
      free(session->nonce);
      session->nonce = NULL;
    }

  param = evrtsp_find_header(req->input_headers, "WWW-Authenticate");
  if (!param)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "WWW-Authenticate header not found\n");
      goto error;
    }

  DPRINTF(E_DBG, L_AIRPLAY, "WWW-Authenticate: %s\n", param);

  if (strncmp(param, "Digest ", strlen("Digest ")) != 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Unsupported authentication method: %s\n", param);
      goto error;
    }

  auth = strdup(param);
  if (!auth)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Out of memory for WWW-Authenticate header copy\n");
      goto error;
    }

  token = strchr(auth, ' ');
  if (!token)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Unexpected WWW-Authenticate auth\n");
      goto error;
    }

  token++;

  token = strtok_r(token, " =", &ptr);
  while (token) {
    if (strcmp(token, "realm") == 0) {
      token = strtok_r(NULL, "=\"", &ptr);
      if (!token) break;

      if (session->realm) goto error;
      session->realm = strdup(token);
      if (!session->realm) goto error;
    } else if (strcmp(token, "nonce") == 0) {
      token = strtok_r(NULL, "=\"", &ptr);
      if (!token) break;

      if (session->nonce) goto error;
      session->nonce = strdup(token);
      if (!session->nonce) goto error;
    }

    token = strtok_r(NULL, " =", &ptr);
  }

  if (!session->realm || !session->nonce)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not find realm/nonce in WWW-Authenticate header\n");

      if (session->realm)
	{
	  free(session->realm);
	  session->realm = NULL;
	}

      if (session->nonce)
	{
	  free(session->nonce);
	  session->nonce = NULL;
	}

      goto error;
    }

  DPRINTF(E_SPAM, L_AIRPLAY, "Found realm: [%s], nonce: [%s]\n", session->realm, session->nonce);

  free(auth);
  return 0;

error:
  free(session->realm);
  session->realm = NULL;
  free(session->nonce);
  session->nonce = NULL;
  free(auth);
  return -1;
}

static int
request_headers_add(struct evrtsp_request *req, struct airplay_session *session, enum evrtsp_cmd_type req_method)
{
  char buf[64];
  const char *method;
  const char *url;
  int ret;

  snprintf(buf, sizeof(buf), "%d", session->cseq);
  evrtsp_add_header(req->output_headers, "CSeq", buf);

  session->cseq++;

  evrtsp_add_header(req->output_headers, "User-Agent", airplay_user_agent);
  evrtsp_add_header(req->output_headers, "X-Apple-Client-Name", airplay_client_name);

  // If we have a realm + nonce it means that the device told us in the reply to
  // SETUP that www authentication with password is required
  if (session->realm && session->nonce)
    {
      method = evrtsp_method(req_method);
      url = (req_method == EVRTSP_REQ_OPTIONS) ? "*" : session->session_url;

      ret = auth_header_add(req, session->password, session->realm, session->nonce, method, url);
      if (ret < 0)
	return -1;

      session->req_has_auth = 1;
    }

  snprintf(buf, sizeof(buf), "%" PRIX64, airplay_device_id);
  evrtsp_add_header(req->output_headers, "Client-Instance", buf);
  evrtsp_add_header(req->output_headers, "DACP-ID", buf);

  // We set Active-Remote as 32 bit unsigned decimal, as at least my device
  // can't handle any larger. Must be aligned with volume_byactiveremote().
  snprintf(buf, sizeof(buf), "%" PRIu32, (uint32_t)session->device_id);
  evrtsp_add_header(req->output_headers, "Active-Remote", buf);

#if AIRPLAY_USE_STREAMID
  evrtsp_add_header(req->output_headers, "X-Apple-StreamID", "1");
#endif

  /* Content-Length added automatically by evrtsp */

  return 0;
}

static int rtsp_cipher(struct evbuffer *outbuf, struct evbuffer *inbuf,
                       void *arg, int encrypt) {
  struct airplay_session *session = arg;
  uint8_t *in;
  size_t in_len;
  uint8_t *out = NULL;
  size_t out_len = 0;
  ssize_t processed;

  in = evbuffer_pullup(inbuf, -1);
  in_len = evbuffer_get_length(inbuf);

  if (encrypt) {
#if AIRPLAY_DUMP_TRAFFIC
    if (in_len < 4096)
      DHEXDUMP(E_DBG, L_AIRPLAY, in, in_len, "Encrypting outgoing request\n");
    else
      DPRINTF(E_DBG, L_AIRPLAY, "Encrypting outgoing request (size %zu)\n",
              in_len);
#endif

    processed =
        pair_encrypt(&out, &out_len, in, in_len, session->control_cipher_ctx);
    if (processed < 0) goto error;
  } else {
    processed =
        pair_decrypt(&out, &out_len, in, in_len, session->control_cipher_ctx);
    if (processed < 0) goto error;

#if AIRPLAY_DUMP_TRAFFIC
    if (out_len < 4096)
      DHEXDUMP(E_DBG, L_AIRPLAY, out, out_len, "Decrypted incoming response\n");
    else
      DPRINTF(E_DBG, L_AIRPLAY, "Decrypted incoming response (size %zu)\n",
              out_len);
#endif
  }

  if (out_len && evbuffer_add(outbuf, out, out_len) < 0) {
    free(out);
    if (encrypt)
      pair_encrypt_rollback(session->control_cipher_ctx);
    else
      pair_decrypt_rollback(session->control_cipher_ctx);
    DPRINTF(E_LOG, L_AIRPLAY, "Could not append %s control message (len=%zu)\n",
            encrypt ? "encrypted" : "decrypted", out_len);
    return -1;
  }
  free(out);
  evbuffer_drain(inbuf, processed);

  return 0;

error:
  DPRINTF(E_LOG, L_AIRPLAY, "Error while %s (len=%zu): %s\n",
          encrypt ? "encrypting" : "decrypting", in_len,
          pair_cipher_errmsg(session->control_cipher_ctx));

  return -1;
}

/* ------------------------------ Session handling -------------------------- */

// Maps our internal state to the generic output state and then makes a callback
// to the player to tell that state
static void
session_status(struct airplay_session *session)
{
  enum output_device_state state;

  switch (session->state)
    {
      case AIRPLAY_STATE_AUTH:
	state = OUTPUT_STATE_PASSWORD;
	break;
      case AIRPLAY_STATE_FAILED:
	state = OUTPUT_STATE_FAILED;
	break;
      case AIRPLAY_STATE_STOPPED:
	state = OUTPUT_STATE_STOPPED;
	break;
      case AIRPLAY_STATE_INFO ... AIRPLAY_STATE_RECORD:
	state = OUTPUT_STATE_STARTUP;
	break;
      case AIRPLAY_STATE_CONNECTED:
	state = OUTPUT_STATE_CONNECTED;
	break;
      case AIRPLAY_STATE_STREAMING:
	state = OUTPUT_STATE_STREAMING;
	break;
      case AIRPLAY_STATE_TEARDOWN:
	DPRINTF(E_LOG, L_AIRPLAY, "Bug! session_status() called with transitional state (TEARDOWN)\n");
	state = OUTPUT_STATE_STOPPED;
	break;
      default:
	DPRINTF(E_LOG, L_AIRPLAY, "Bug! Unhandled state in session_status(): %d\n", session->state);
	state = OUTPUT_STATE_FAILED;
    }

  protocol_state_changed(session->callback_id, session->device_id, state);
  session->callback_id = -1;
}

static void
master_session_free(struct airplay_master_session *ams)
{
  if (!ams)
    return;


  rtp_session_free(ams->rtp_session);

  codec_alac_free(ams->encode_ctx);

  if (ams->input_buffer)
    evbuffer_free(ams->input_buffer);
  if (ams->encoded_buffer)
    evbuffer_free(ams->encoded_buffer);

  free(ams->rawbuf);
  free(ams);
}

static void
master_session_cleanup(struct airplay_master_session *ams)
{
  struct airplay_master_session *s;
  struct airplay_session *session;

  // First check if any other session is using the master session
  for (session = airplay_sessions; session; session=session->next)
    {
      if (session->master_session == ams)
	return;
    }

  if (ams == airplay_master_sessions)
    airplay_master_sessions = airplay_master_sessions->next;
  else
    {
      for (s = airplay_master_sessions; s && (s->next != ams); s = s->next)
	; /* EMPTY */

      if (!s)
	DPRINTF(E_WARN, L_AIRPLAY, "WARNING: struct airplay_master_session not found in list; BUG!\n");
      else
	s->next = ams->next;
    }

  master_session_free(ams);
}

/* latencyMin is the configured render margin. The sync lead keeps the rest of
 * the requested budget, so a 20 ms budget still has samples ahead of the play
 * position. An unset margin uses the 250 ms cap. */
static uint32_t
receiver_latency_ms(uint32_t render_ms, uint32_t base_ms, int offset_ms)
{
  int64_t effective = (int64_t)base_ms + offset_ms;
  if (effective < 0)
    effective = 0;
  uint32_t budget = effective < (int64_t)base_ms ? (uint32_t)effective : base_ms;
  uint32_t capped = MIN(budget, (uint32_t)AIRPLAY_AUDIO_LATENCY_MS);
  if (!render_ms)
    return capped;
  return render_ms < capped ? render_ms : capped;
}

/* SETUP latencyMax is the buffer the receiver is allowed to hold. A short
 * requested budget still gets a 250 ms window; the sync lead targets the
 * requested budget. */
static uint32_t
receiver_window_samples(uint32_t requested)
{
  uint32_t floor_samples = (uint32_t)AIRPLAY_AUDIO_LATENCY_MS * 44100 / 1000;
  return requested > floor_samples ? requested : floor_samples;
}

static struct airplay_master_session *
master_session_make(struct media_quality *quality, bool use_ptp, const struct protocol_peer *peer)
{
  struct airplay_master_session *ams;
  uint64_t buffer_duration_ms;

  uint64_t clock_id;

  // First check if we already have a suitable session
  for (ams = airplay_master_sessions; ams; ams = ams->next)
    {
      if (quality_is_equal(quality, &ams->rtp_session->quality) && use_ptp == ams->use_ptp)
	return ams;
    }

  // Let's create a master session
  ams = calloc(1, sizeof(struct airplay_master_session));
  if (!ams) return NULL;

  clock_id = use_ptp ? ptpd_clock_id_get() : 0;

  int history_packets = peer->buffers.history_ms
      ? MAX(1, ((uint64_t)peer->buffers.history_ms * quality->sample_rate + 351999) / 352000)
      : AIRPLAY_PACKET_BUFFER_SIZE;
  ams->rtp_session = rtp_session_new(quality, history_packets, 0, clock_id);
  if (!ams->rtp_session)
    {
      goto error;
    }

  ams->encode_ctx = codec_alac_new(quality);
  if (!ams->encode_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Will not be able to stream AirPlay 2, ffmpeg has no ALAC encoder\n");
      goto error;
    }

  buffer_duration_ms = peer->presentation_base_ms ? peer->presentation_base_ms : 2000;
  uint32_t effective_ms = buffer_duration_ms + peer->offset_ms;
  uint32_t render_ms = receiver_latency_ms(peer->buffers.render_ms,
                                           (uint32_t)buffer_duration_ms,
                                           peer->offset_ms);
  if (render_ms > effective_ms) goto error;
  ams->rtp_session->render_samples = render_ms * quality->sample_rate / 1000;

  ams->quality = *quality;
  ams->use_ptp = use_ptp;
  ams->samples_per_packet = AIRPLAY_SAMPLES_PER_PACKET;
  ams->rawbuf_size = STOB(ams->samples_per_packet, quality->bits_per_sample, quality->channels);
  ams->output_buffer_samples = (uint32_t)(buffer_duration_ms * quality->sample_rate / 1000) - ams->rtp_session->render_samples;

  ams->rawbuf = malloc(ams->rawbuf_size);
  ams->input_buffer = evbuffer_new();
  ams->encoded_buffer = evbuffer_new();
  if (!ams->rawbuf || !ams->input_buffer || !ams->encoded_buffer) goto error;

  ams->next = airplay_master_sessions;
  airplay_master_sessions = ams;

  return ams;

 error:
  master_session_free(ams);
  return NULL;
 }

static void volume_request_release(struct airplay_session *session) {
  struct volume_request *owned = session->volume_request;
  if (!owned) return;
  session->volume_request = NULL;
  if (session->ctrl) {
    struct evrtsp_request *request;
    TAILQ_FOREACH(request, &session->ctrl->requests, next) {
      if (request->cb_arg == owned) {
        request->cb_arg = NULL;
        request->cb = NULL;
      }
    }
  }
  owned->session = NULL;
  free(owned);
}

static void presentation_request_release(struct airplay_session *session) {
  struct presentation_request *owned = session->presentation_request;
  if (!owned) return;
  session->presentation_request = NULL;
  if (session->ctrl) {
    struct evrtsp_request *request;
    TAILQ_FOREACH(request, &session->ctrl->requests, next) {
      if (request->cb_arg == owned) {
        request->cb_arg = NULL;
        request->cb = NULL;
      }
    }
  }
  free(owned);
}
static void session_free(struct airplay_session *session) {
  if (!session) return;

  volume_request_release(session);
  presentation_request_release(session);

  if (session->master_session) master_session_cleanup(session->master_session);

  if (session->ctrl) {
    struct evrtsp_request *pending;
    TAILQ_FOREACH(pending, &session->ctrl->requests, next) {
      free(pending->cb_arg);
      pending->cb_arg = NULL;
    }
    evrtsp_connection_set_closecb(session->ctrl, NULL, NULL);
    evrtsp_connection_free(session->ctrl);
  }

  if (session->deferredev) event_free(session->deferredev);
  if (session->pair_restart_event) event_free(session->pair_restart_event);
  if (session->server_fd >= 0) platform_socket_close(session->server_fd);
  airplay_events_deinit();
  if (session->ptpd_slave_id > 0) ptpd_slave_remove(session->ptpd_slave_id);
  chacha_close(session->packet_cipher_hd);
  pair_setup_free(session->pair_setup_ctx);
  pair_verify_free(session->pair_verify_ctx);
  pair_cipher_free(session->control_cipher_ctx);
  free(session->local_v4_address);
  free(session->local_v6_address);
  free(session->realm);
  free(session->nonce);
  free(session->address);
  free(session->devname);
  sodium_memzero(session->shared_secret, sizeof(session->shared_secret));
  free(session);
}

static void
session_cleanup(struct airplay_session *session)
{
  struct airplay_session *s;

  if (session == airplay_sessions)
    airplay_sessions = airplay_sessions->next;
  else
    {
      for (s = airplay_sessions; s && (s->next != session); s = s->next)
	; /* EMPTY */

      if (!s)
	DPRINTF(E_WARN, L_AIRPLAY, "WARNING: struct airplay_session not found in list; BUG!\n");
      else
	s->next = session->next;
    }

  protocol_peer_session_remove(session->device_id);

  session_free(session);
}

static void
session_failure(struct airplay_session *session)
{
  /* Session failed, let our user know */
  if (session->state != AIRPLAY_STATE_AUTH)
    session->state = AIRPLAY_STATE_FAILED;

  session_status(session);

  session_cleanup(session);
}

static void
deferred_session_failure_cb(platform_socket fd, short what, void *arg)
{
  struct airplay_session *session = arg;

  DPRINTF(E_DBG, L_AIRPLAY, "Cleaning up failed session (deferred) on device '%s'\n", session->devname);
  session_failure(session);
}

static void
deferred_session_failure(struct airplay_session *session)
{
  struct timeval tv;

  if (session->state != AIRPLAY_STATE_AUTH)
    session->state = AIRPLAY_STATE_FAILED;

  evutil_timerclear(&tv);
  evtimer_add(session->deferredev, &tv);
}

static void
rtsp_close_cb(struct evrtsp_connection *evcon, void *arg)
{
  struct airplay_session *session = arg;

  DPRINTF(E_LOG, L_AIRPLAY, "Device '%s' closed RTSP connection\n", session->devname);

  deferred_session_failure(session);
}

static void
session_success(struct airplay_session *session)
{
  session_status(session);

  session_cleanup(session);
}

static void
session_connected(struct airplay_session *session)
{
  session->state = AIRPLAY_STATE_CONNECTED;
  struct protocol_peer *peer = protocol_peer_get(session->device_id);
  uint32_t maximum = peer && peer->presentation_base_ms ? (uint32_t)((peer->presentation_base_ms + peer->offset_ms) * 44100 / 1000) : 88200;
  protocol_timing_applied(session->master_session->output_buffer_samples, session->master_session->rtp_session->render_samples, receiver_window_samples(maximum));
  session_status(session);
}

static void
session_pair_restart_cb(platform_socket fd, short what, void *arg)
{
  struct airplay_session *session = arg;
  struct protocol_peer *device = protocol_peer_get(session->device_id);
  int callback_id = session->callback_id;

  session_cleanup(session);
  if (!device || airplay_device_start(device, callback_id) < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not establish verified control connection after pairing\n");
      protocol_state_changed(callback_id, device ? device->id : 0, OUTPUT_STATE_FAILED);
    }
}

static void
session_pair_success(struct airplay_session *session)
{
  if (session->pair_restart_pending)
    {
      struct timeval immediate = {0, 0};
      if (evtimer_add(session->pair_restart_event, &immediate) < 0)
        session_failure(session);
      return;
    }
  if (session->next_seq != AIRPLAY_SEQ_CONTINUE)
    {
      sequence_start(session->next_seq, session, NULL, "pair_success");
      session->next_seq = AIRPLAY_SEQ_CONTINUE;
      return;
    }

  session_success(session);
}

static int
session_connection_setup(struct airplay_session *session, struct protocol_peer *device, int family)
{
  char *address;
  char *intf;
  unsigned short port;
  int ret;

  session->naddr.ss.ss_family = family;

  switch (family)
    {
      case AF_INET:
	if (!device->v4_address)
	  return -1;

	address = device->v4_address;
	port = device->v4_port;


	ret = inet_pton(AF_INET, address, &session->naddr.sin.sin_addr);
	break;

      case AF_INET6:
	if (!device->v6_address)
	  return -1;

	address = device->v6_address;
	port = device->v6_port;

	intf = strchr(address, '%');
	if (intf)
	  *intf = '\0';

	ret = inet_pton(AF_INET6, address, &session->naddr.sin6.sin6_addr);

	if (intf)
	  {
	    *intf = '%';

	    intf++;

	    session->naddr.sin6.sin6_scope_id = platform_interface_index(intf);
	    if (session->naddr.sin6.sin6_scope_id == 0)
	      {
		DPRINTF(E_LOG, L_AIRPLAY, "Could not find interface %s\n", intf);

		ret = -1;
		break;
	      }
	  }

	break;

      default:
	return -1;
    }

  if (ret <= 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Device '%s' has invalid address (%s) for %s\n", device->name, address, (family == AF_INET) ? "ipv4" : "ipv6");
      return -1;
    }

  session->ctrl = evrtsp_connection_new(address, port);
  if (!session->ctrl)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create control connection to '%s' (%s)\n", device->name, address);
      return -1;
    }

  evrtsp_connection_set_base(session->ctrl, evbase_player);

  session->address = strdup(address);
  session->family = family;

  return 0;
}

static int
session_cipher_setup(struct airplay_session *session, const uint8_t *key, size_t key_len)
{
  struct pair_cipher_context *control_cipher_ctx = NULL;
  gcry_cipher_hd_t packet_cipher_hd = NULL;

  // For transient pairing the key_len will be 64 bytes, and session->shared_secret is 32 bytes
  if (key_len < AIRPLAY_AUDIO_KEY_LEN || key_len > sizeof(session->shared_secret))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Ciphering setup error: Unexpected key length (%zu)\n", key_len);
      goto error;
    }

  session->shared_secret_len = key_len;
  memcpy(session->shared_secret, key, key_len);

  control_cipher_ctx = pair_cipher_new(session->pair_type, PAIR_CHANNEL_CONTROL, key, key_len, NULL);
  if (!control_cipher_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create control ciphering context\n");
      goto error;
    }

  packet_cipher_hd = chacha_open(session->shared_secret, AIRPLAY_AUDIO_KEY_LEN);
  if (!packet_cipher_hd)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create packet ciphering handle\n");
      goto error;
    }

  DPRINTF(E_DBG, L_AIRPLAY, "Ciphering setup of '%s' completed succesfully, now using encrypted mode\n", session->devname);

  session->state = AIRPLAY_STATE_ENCRYPTED;
  session->control_cipher_ctx = control_cipher_ctx;
  session->packet_cipher_hd = packet_cipher_hd;

  evrtsp_connection_set_ciphercb(session->ctrl, rtsp_cipher, session);

  return 0;

 error:
  pair_cipher_free(control_cipher_ctx);
  chacha_close(packet_cipher_hd);
  return -1;
}

static int
session_ids_set(struct airplay_session *session)
{
  char *address = NULL;
  char *intf;
  char ifname[64];
  uint8_t mac[6];
  unsigned short port;
  int family;
  int ret;

  // Determine local address, needed for session URL
  evrtsp_connection_get_local_address(session->ctrl, &address, &port, &family);
  if (!address || (port == 0))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not determine local v4 address\n");
      goto error;
    }

  intf = strchr(address, '%');
  if (intf)
    {
      *intf = '\0';
      intf++;
    }
  else
    {
      ret = net_if_get(ifname, sizeof(ifname), address);
      intf = (ret < 0) ? NULL : ifname;
    }

  ret = net_mac_get(mac, sizeof(mac), intf);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not determine local MAC address\n");
      goto error;
    }

  ret = snprintf(session->local_mac_address, sizeof(session->local_mac_address), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if ((ret < 0) || (ret >= sizeof(session->local_mac_address)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Session URL length exceeds 127 characters\n");
      goto error;
    }

  DPRINTF(E_DBG, L_AIRPLAY, "Local address: %s port %d, if %s, mac %s\n", address, port, intf, session->local_mac_address);

  // Session UUID, ID and session URL
  if (uuid_make(session->session_uuid) < 0 || uuid_make(session->group_uuid) < 0)
    goto error;

  gcry_randomize(&session->session_id, sizeof(session->session_id), GCRY_STRONG_RANDOM);

  ret = snprintf(session->session_url, sizeof(session->session_url), (family == AF_INET) ? "rtsp://%s/%u" : "rtsp://[%s]/%u", address, session->session_id);
  if ((ret < 0) || (ret >= sizeof(session->session_url)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Session URL length exceeds 127 characters\n");
      goto error;
    }

  if (family == AF_INET)
    session->local_v4_address = address;
  else
    session->local_v6_address = address;

  return 0;

 error:
  free(address);
  return -1;
}

static struct airplay_session *
session_find_by_address(union net_sockaddr *peer_addr)
{
  struct airplay_session *session;
  uint32_t *addr_ptr;
  int family = peer_addr->sa.sa_family;

  for (session = airplay_sessions; session; session = session->next)
    {
      if (family == session->family)
	{
	  if (family == AF_INET && peer_addr->sin.sin_addr.s_addr == session->naddr.sin.sin_addr.s_addr)
	    break;

	  if (family == AF_INET6 && IN6_ARE_ADDR_EQUAL(&peer_addr->sin6.sin6_addr, &session->naddr.sin6.sin6_addr))
	    break;
	}
      else if (family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&peer_addr->sin6.sin6_addr))
	{
	  // ipv4 mapped to ipv6 consists of 16 bytes/4 words: 0x00000000 0x00000000 0x0000ffff 0x[IPv4]
	  addr_ptr = (uint32_t *)(&peer_addr->sin6.sin6_addr);
	  if (addr_ptr[3] == session->naddr.sin.sin_addr.s_addr)
	    break;
	}
    }

  return session;
}

static struct airplay_session *
session_make(struct protocol_peer *device, int callback_id)
{
  struct airplay_session *session;
  struct airplay_extra *extra;
  int ret;

  extra = device->extra_device_info;

  session = calloc(1, sizeof(struct airplay_session));
  if (!session) return NULL;
  session->server_fd = -1;
  session->deferredev = evtimer_new(evbase_player, deferred_session_failure_cb, session);
  if (!session->deferredev) goto error;
  session->pair_restart_event = evtimer_new(evbase_player, session_pair_restart_cb, session);
  if (!session->pair_restart_event) goto error;

  session->devname = strdup(device->name);
  if (!session->devname) goto error;
  session->volume = device->volume;
  session->offset_samples = device->offset_ms * device->quality.sample_rate / 1000;

  session->state = AIRPLAY_STATE_STOPPED;
  session->reqs_in_flight = 0;
  session->cseq = 1;

  session->device_id = device->id;
  session->callback_id = callback_id;

  session->server_fd = -1;

  session->password = device->password;

  session->supports_auth_setup = extra->supports_auth_setup;
  session->wanted_metadata = extra->wanted_metadata;
  session->supports_encryption = extra->supports_encryption;

  session->next_seq = AIRPLAY_SEQ_CONTINUE;
  session->timing_svc = &airplay_timing_svc;
  session->control_svc = &airplay_control_svc;

  ret = session_connection_setup(session, device, AF_INET6);
  if (ret < 0)
    {
      ret = session_connection_setup(session, device, AF_INET);
      if (ret < 0)
	goto error;
    }

  session->master_session = master_session_make(&device->quality, extra->use_ptp, device);
  if (!session->master_session)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not attach a master session for device '%s'\n", device->name);
      goto error;
    }

  // Attach to list of sessions
  session->next = airplay_sessions;
  airplay_sessions = session;

  // Publish the protocol session after successful construction
  protocol_peer_session_add(device->id, session);

  return session;

 error:
  session_free(session);

  return NULL;
}


/* ----------------------------- Metadata handling -------------------------- */





// *** Thread: worker ***











/* ------------------------------ Volume handling --------------------------- */



static float
airplay_volume_from_pct(int volume, const char *name)
{
  float airplay_volume;
  int max_volume;

  max_volume = AIRPLAY_CONFIG_MAX_VOLUME;

  /* RAOP volume
   *  -144.0 is off (not really used since we have no concept of muted/off)
   *  0 - 100 maps to -30.0 - 0 (if no max_volume set)
   */
  if (volume >= 0 && volume <= 100)
    airplay_volume = -30.0 + ((float)max_volume * (float)volume * 30.0) / (100.0 * AIRPLAY_CONFIG_MAX_VOLUME);
  else
    airplay_volume = -144.0;

  return airplay_volume;
}



/* Volume in [0 - 100] */





/* -------------------- Creation and sending of RTP packets  ---------------- */

static int
packet_encrypt(uint8_t **out, size_t *out_len, struct rtp_packet *pkt, struct airplay_session *session)
{
  uint8_t authtag[16];
  uint8_t nonce[12] = { 0 };
  int nonce_offset = 4;
  uint8_t *write_ptr;
  int ret;

  // Alloc so authtag and nonce can be appended
  *out_len = pkt->data_len + sizeof(authtag) + sizeof(nonce) - nonce_offset;
  *out = malloc(*out_len);
  if (!*out) {
    *out_len = 0;
    return -1;
  }
  write_ptr = *out;

  // Using seqnum as nonce not very secure, but means that when we resend
  // packets they will be identical to the original
  memcpy(nonce + nonce_offset, &pkt->seqnum, sizeof(pkt->seqnum));

  // The RTP header is not encrypted
  memcpy(write_ptr, pkt->header, pkt->header_len);
  write_ptr = *out + pkt->header_len;

  // Timestamp and SSRC are used as AAD = pkt->header + 4, len 8
  ret = chacha_encrypt(write_ptr, pkt->payload, pkt->payload_len, pkt->header + 4, 8, authtag, sizeof(authtag), nonce, sizeof(nonce), session->packet_cipher_hd);
  if (ret < 0)
    {
      free(*out);
      return -1;
    }

  write_ptr += pkt->payload_len;
  memcpy(write_ptr, authtag, sizeof(authtag));
  write_ptr += sizeof(authtag);
  memcpy(write_ptr, nonce + nonce_offset, sizeof(nonce) - nonce_offset);

  return 0;
}

static int
packet_send(struct airplay_session *session, struct rtp_packet *pkt)
{
  uint8_t *buf;
  size_t buf_len;
  ssize_t sent;
  int ret;

  if (!session)
    return -1;

  if (session->supports_encryption)
    {
      ret = packet_encrypt(&buf, &buf_len, pkt, session);
      if (ret < 0)
	return -1;

      sent = platform_send(session->server_fd, buf, buf_len, 0);
      free(buf);
    }
  else
    {
      buf = pkt->data;
      buf_len = pkt->data_len;
      sent = platform_send(session->server_fd, buf, buf_len, 0);
    }

  if (sent < 0)
    {
    protocol_statistic(PROTOCOL_SEND_FAILURE, 1);
    DPRINTF(E_LOG, L_AIRPLAY, "Send error for '%s': %s\n", session->devname,
            strerror(errno));

    // Can't free it right away, it would make the ->next in the calling
    // master_session and session loops invalid
    deferred_session_failure(session);
    return -1;
  } else if (sent != buf_len) {
    protocol_statistic(PROTOCOL_SEND_FAILURE, 1);
    DPRINTF(E_WARN, L_AIRPLAY, "Partial send (%zd) for '%s'\n", sent,
            session->devname);
    return -1;
  }

  protocol_statistic(PROTOCOL_BYTES, (uint64_t)sent);
  /*
    DPRINTF(E_DBG, L_AIRPLAY, "RTP PACKET seqnum %u, rtptime %u, payload 0x%x,
    pktbuf_s %zu\n", session->master_session->rtp_session->seqnum,
      session->master_session->rtp_session->pos,
      pkt->header[1],
      session->master_session->rtp_session->pktbuf_len
      );
  */

  return 0;
}

static void
control_packet_send(struct airplay_session *session, struct rtp_packet *pkt)
{
  socklen_t addrlen;
  platform_socket fd;
  int ret;

  switch (session->family)
    {
      case AF_INET:
	session->naddr.sin.sin_port = htons(session->control_port);
	addrlen = sizeof(session->naddr.sin);
	fd = session->control_svc->socket.fd4;
	break;

      case AF_INET6:
	session->naddr.sin6.sin6_port = htons(session->control_port);
	addrlen = sizeof(session->naddr.sin6);
	fd = session->control_svc->socket.fd6;
	break;

      default:
	DPRINTF(E_WARN, L_AIRPLAY, "Unknown family %d\n", session->family);
	return;
    }

  if (fd < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not send playback sync to device '%s': control socket is closed\n", session->devname);
      deferred_session_failure(session);
      return;
    }

  ret = platform_sendto(fd, pkt->data, pkt->data_len, 0, &session->naddr.sa, addrlen);
  if (ret == pkt->data_len && pkt->data_len == 28 && pkt->data[3] == 0x06)
    {
      uint32_t current, minimum;uint64_t capture;
      memcpy(&current,pkt->data+4,4);memcpy(&minimum,pkt->data+16,4);memcpy(&capture,pkt->data+8,8);
      protocol_timing_sync(ntohl(current),ntohl(minimum),session->master_session->rtp_session->pos,
        session->master_session->input_buffer_samples,platform_to_be64(capture));
    }
  if (ret < 0)
    DPRINTF(E_LOG, L_AIRPLAY, "Could not send playback sync to device '%s': %s\n", session->devname, strerror(errno));
}

static void
packets_resend(struct airplay_session *session, uint16_t seqnum, int len)
{
  struct rtp_session *rtp_session;
  struct rtp_packet *pkt;
  uint16_t s;
  int i;
  bool pkt_missing = false;

  rtp_session = session->master_session->rtp_session;
  protocol_statistic(PROTOCOL_RESEND_REQUEST, (uint64_t)len);

  DPRINTF(E_DBG, L_AIRPLAY, "Got retransmit request from '%s': seqnum %" PRIu16 " (len %d), next RTP session seqnum %" PRIu16 " (len %zu)\n",
    session->devname, seqnum, len, rtp_session->seqnum, rtp_session->pktbuf_len);

  // Seems iOS returns 80 d6 00 01 bd 0d 00 00 over control when the packet is
  // not in buffer. And also  resends over control, which my amp doesn't
  // understand, since it keeps asking.
  // Example of resend over control (bd0d=seqnum 48397):
  // 0x000000: 80 d6 00 ee 80 60 bd 0d b4 04 4e c0 00 00 00 00 .....`....N.....
  // 0x000010: 99 cf 26 e3 fa a0 ed da 57 50 82 c8 be 1b 81 9e ..&.....WP......
  // 0x000020: a4 a5 c5 3c 07 83 71 21 e2 91 89 6a 81 bb b0 d3 ...<..q!...j....
  // 0x000030: f4 0e 5c 7f 84 14 1f 0d e6 63 83 26 7d b3 95 24 ..\......c.&}..$
  // 0x000040: 0d 52 11 ef a5 fb 6f 9d 86 8e 69 28 79 0e 73 5b .R....o...i(y.s[
  // 0x000050: 46 1f 04 2d 6b 7e 7f 1d 1e a6 46 04 7b f8 e3 80 F..-k~....F.{...
  // 0x000060: 85 ff 11 4f f2 b5 de f7 cf 0f 7e 4b 55 e5 7b 25 ...O......~KU.{%
  // 0x000070: 13 9a 5f c3 4e f4 9d d1 f5 cb...

  // Note that seqnum may wrap around, so we don't use it for counting
  for (i = 0, s = seqnum; i < len; i++, s++)
    {
      pkt = rtp_packet_get(rtp_session, s);
      if (pkt) {
        if (packet_send(session, pkt) == 0)
          protocol_statistic(PROTOCOL_RESEND_SUCCESS, 1);
      } else {
        protocol_statistic(PROTOCOL_RESEND_EXPIRED, 1);
        pkt_missing = true;
      }
    }

  if (pkt_missing)
    DPRINTF(E_WARN, L_AIRPLAY, "Device '%s' retransmit request for seqnum %" PRIu16 " (len %d) is outside buffer range (next seqnum %" PRIu16 ", len %zu)\n",
      session->devname, seqnum, len, rtp_session->seqnum, rtp_session->pktbuf_len);
}

static int
packets_send(struct airplay_master_session *ams)
{
  struct rtp_packet *pkt;
  struct airplay_session *session;
  int len;

  len = alac_encode(ams->encoded_buffer, ams->encode_ctx, ams->rawbuf, ams->rawbuf_size, ams->samples_per_packet, &ams->quality);
  if (len < 0)
    return -1;

  pkt = rtp_packet_next(ams->rtp_session, len, ams->samples_per_packet, AIRPLAY_RTP_PAYLOADTYPE);
  if (!pkt || evbuffer_remove(ams->encoded_buffer, pkt->payload, pkt->payload_len) != (int)pkt->payload_len)
    return -1;

  for (session = airplay_sessions; session; session = session->next)
    {
      if (session->master_session != ams)
	continue;

      // Device just joined
      if (session->state == AIRPLAY_STATE_CONNECTED) {
        pkt->header[1] |= RTP_MARKER_BIT;  // Set marker bit, value becomes 0xe0
        int sent = packet_send(session, pkt);
        pkt->header[1] &= ~RTP_MARKER_BIT;  // Clear marker bit
        if (sent < 0) return -1;
        protocol_statistic(PROTOCOL_SENT, 1);
      } else if (session->state == AIRPLAY_STATE_STREAMING) {
        if (packet_send(session, pkt) < 0) return -1;
        protocol_statistic(PROTOCOL_SENT, 1);
      }
  }

  // Commits packet to retransmit buffer, and prepares the session for the next
  // packet
  rtp_packet_commit(ams->rtp_session, pkt);

  return 0;
}

// Overview of rtptimes as they should be when starting a stream, and assuming
// the first rtptime (pos) is 88200:
//   sync pkt:  cur_pos = 0, rtptime = 88200
//   audio pkt: rtptime = 88200
//   RECORD:    rtptime = 88200
//   SET_PARAMETER text/artwork:
//              rtptime = 88200
//   SET_PARAMETER progress:
//              progress = 72840/~88200/[len]
static int64_t
timespec_elapsed_ns(struct timespec later, struct timespec earlier)
{
  int64_t sec = (int64_t)later.tv_sec - (int64_t)earlier.tv_sec;
  int64_t nsec = (int64_t)later.tv_nsec - (int64_t)earlier.tv_nsec;

  if (nsec < 0)
    {
      --sec;
      nsec += 1000000000;
    }
  if (sec < 0)
    return -1;
  if (sec > INT64_MAX / 1000000000)
    return INT64_MAX;
  if (nsec > INT64_MAX - sec * 1000000000)
    return INT64_MAX;
  return sec * 1000000000 + nsec;
}

/* Silence stops RTP while the receiver clock keeps running. Move pos to the
 * current timestamp so the next packet and the FLUSH that announces it share
 * that position. */
static void
cover_rtp_gap(struct airplay_master_session *ams, struct timespec now)
{
  uint32_t covered;

  if (!ams->stamp_valid || !ams->rtp_session || !ams->quality.sample_rate)
    return;
  covered = rtp_cover_elapsed(ams->rtp_session->pos, ams->pos_at_stamp,
                              timespec_elapsed_ns(now, ams->cur_stamp.ts),
                              ams->quality.sample_rate);
  if (covered == ams->rtp_session->pos)
    return;
  ams->rtp_session->pos = covered;
  ams->pos_at_stamp = covered;
  ams->cur_stamp.ts = now;
  if (ams->rtp_session->sync_each_nsamples > 0)
    ams->rtp_session->sync_counter = ams->rtp_session->sync_each_nsamples + 1;
}

/* FLUSH has announced the covered position. The packet that follows keeps
 * that position; its capture time is the sync clock. Another gap cover here
 * would name a different RTP time from the one the receiver was told. */
static void
flush_rtp_timeline(struct airplay_master_session *ams, struct timespec now)
{
  cover_rtp_gap(ams, now);
  ams->stamp_valid = false;
}

static inline void
timestamp_set(struct airplay_master_session *ams, struct timespec ts)
{
  // The last write from the player had a timestamp which has been passed to
  // this function as ts. This is the player clock, which is more precise than
  // the actual clock because it gives us a calculated time reference, which is
  // independent of how busy the thread is. We save that here, we need this for
  // reference when sending sync packets and progress.
  cover_rtp_gap(ams, ts);
  if (!ams->stamp_valid || timespec_elapsed_ns(ts, ams->cur_stamp.ts) >= 0)
    {
      ams->pos_at_stamp = ams->rtp_session->pos;
      ams->cur_stamp.ts = ts;
    }
  ams->stamp_valid = true;

  // So what rtptime should be playing, i.e. coming out of the speaker, at time
  // ts (which is normally "now")? Let's calculate by example:
  //   - we started playback with a rtptime (pos) of X
  //   - up until time ts we have received a 1000 samples from the player
  //   - ams->output_buffer_samples is configured to 400 samples
  //   -> we should be playing rtptime X + 600
  //
  // So how do we measure samples received from player? We know that from the
  // pos, which says how much has been sent to the device, and from ams->input_buffer,
  // which is the unsent stuff being buffered:
  //   - received = (pos - X) + ams->input_buffer_samples
  //
  // This means the rtptime is computed as:
  //   - rtptime = X + received - ams->output_buffer_samples
  //   -> rtptime = X + (pos - X) + ams->input_buffer_samples - ams->out_buffer_samples
  //   -> rtptime = pos + ams->input_buffer_samples - ams->output_buffer_samples
  ams->cur_stamp.pos = ams->rtp_session->pos + ams->input_buffer_samples - ams->output_buffer_samples;
}

static void
packets_sync_send(struct airplay_master_session *ams)
{
  struct rtp_packet *sync_pkt;
  struct rtcp_timestamp cur_stamp;
  struct airplay_session *session;
  struct timespec ts;
  bool is_sync_time;

  // Check if it is time send a sync packet to sessions that are already running
  is_sync_time = rtp_sync_is_time(ams->rtp_session);

  // Just used for logging, the clock shouldn't be too far from ams->cur_stamp.ts
  platform_time(PLATFORM_MONOTONIC, &ts);

  for (session = airplay_sessions; session; session = session->next)
    {
      if (session->master_session != ams)
	continue;

      cur_stamp = ams->cur_stamp;

      // Apply user configured offset
      cur_stamp.pos -= session->offset_samples;

      // A device has joined and should get an init sync packet
      if (session->state == AIRPLAY_STATE_CONNECTED)
	{
	  sync_pkt = rtp_sync_packet_next(ams->rtp_session, cur_stamp, 0x90);
	  if (!sync_pkt) { deferred_session_failure(session); continue; }
	  control_packet_send(session, sync_pkt);

	  DPRINTF(E_DBG, L_AIRPLAY, "Start sync packet sent to '%s': offset=%d, cur_pos=%" PRIu32 ", cur_ts=%ld.%09ld, clock=%ld.%09ld, rtptime=%" PRIu32 ", timing=%s\n",
	    session->devname, session->offset_samples, cur_stamp.pos, (long)cur_stamp.ts.tv_sec, (long)cur_stamp.ts.tv_nsec, (long)ts.tv_sec, (long)ts.tv_nsec,
	    ams->rtp_session->pos, ams->use_ptp ? "PTP" : "NTP");
	}
      else if (is_sync_time && session->state == AIRPLAY_STATE_STREAMING)
	{
	  sync_pkt = rtp_sync_packet_next(ams->rtp_session, cur_stamp, 0x80);
	  if (!sync_pkt) { deferred_session_failure(session); continue; }
	  control_packet_send(session, sync_pkt);
	}
    }
}


/* ------------------------- Time and control service ----------------------- */

static void
service_stop(struct airplay_service *svc)
{
  if (svc->ev4)
    event_free(svc->ev4);
  if (svc->ev6)
    event_free(svc->ev6);

  svc->ev4 = NULL;
  svc->ev6 = NULL;

  net_socket_close(&svc->socket);

  svc->port = 0;
}

static int
service_start(struct airplay_service *svc, event_callback_fn cb, unsigned short port, const char *log_service_name)
{
  int ret;

  memset(svc, 0, sizeof(struct airplay_service));

  svc->socket.fd4 = -1;
  svc->socket.fd6 = -1;

  ret = net_bind(&svc->socket, &port, SOCK_DGRAM, log_service_name);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not start '%s' service\n", log_service_name);
      goto error;
    }

  if (svc->socket.fd4 >= 0) {
    svc->ev4 = event_new(evbase_player, svc->socket.fd4, EV_READ | EV_PERSIST, cb, svc);
    if (!svc->ev4)
      goto error;

    event_add(svc->ev4, NULL);
  }

  if (svc->socket.fd6 >= 0) {
    svc->ev6 = event_new(evbase_player, svc->socket.fd6, EV_READ | EV_PERSIST, cb, svc);
    if (!svc->ev6)
      goto error;

    event_add(svc->ev6, NULL);
  }

  svc->port = port;

  return 0;

 error:
  service_stop(svc);
  return -1;
}

static void
timing_svc_cb(platform_socket fd, short what, void *arg)
{
  union net_sockaddr peer_addr;
  socklen_t peer_addrlen = sizeof(peer_addr);
  char address[INET6_ADDRSTRLEN];
  uint8_t req[32];
  uint8_t res[32];
  struct ntp_stamp recv_stamp;
  struct ntp_stamp xmit_stamp;
  int ret;

  ret = timing_get_clock_ntp(&recv_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get receive timestamp\n");
      return;
    }

  peer_addrlen = sizeof(peer_addr);
  ret = platform_recvfrom(fd, req, sizeof(req), 0, &peer_addr.sa, &peer_addrlen);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Error reading timing request: %s\n", strerror(errno));
      return;
    }

  if (ret != 32)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Got timing request from %s with size %d\n", address, ret);
      return;
    }

  if ((req[0] != 0x80) || (req[1] != 0xd2))
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Packet header from %s doesn't match timing request (got 0x%02x%02x, expected 0x80d2)\n", address, req[0], req[1]);
      return;
    }

  memset(res, 0, sizeof(res));

  /* Header */
  res[0] = 0x80;
  res[1] = 0xd3;
  res[2] = req[2];

  /* Copy client timestamp */
  memcpy(res + 8, req + 24, 8);

  /* Receive timestamp */
  recv_stamp.sec = htonl(recv_stamp.sec);
  recv_stamp.frac = htonl(recv_stamp.frac);
  memcpy(res + 16, &recv_stamp.sec, 4);
  memcpy(res + 20, &recv_stamp.frac, 4);

  /* Transmit timestamp */
  ret = timing_get_clock_ntp(&xmit_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get transmit timestamp, falling back to receive timestamp\n");

      /* Still better than failing altogether
       * recv/xmit are close enough that it shouldn't matter much
       */
      memcpy(res + 24, &recv_stamp.sec, 4);
      memcpy(res + 28, &recv_stamp.frac, 4);
    }
  else
    {
      xmit_stamp.sec = htonl(xmit_stamp.sec);
      xmit_stamp.frac = htonl(xmit_stamp.frac);
      memcpy(res + 24, &xmit_stamp.sec, 4);
      memcpy(res + 28, &xmit_stamp.frac, 4);
    }

  ret = platform_sendto(fd, res, sizeof(res), 0, &peer_addr.sa, peer_addrlen);
  if (ret < 0)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_LOG, L_AIRPLAY, "Could not send timing reply to %s: %s\n", address, strerror(errno));
      return;
    }
}

static void
control_svc_cb(platform_socket fd, short what, void *arg)
{
  union net_sockaddr peer_addr = { 0 };
  socklen_t peer_addrlen = sizeof(peer_addr);
  char address[INET6_ADDRSTRLEN];
  struct airplay_session *session;
  uint8_t req[8];
  uint16_t seq_start;
  uint16_t seq_len;
  int ret;

  ret = platform_recvfrom(fd, req, sizeof(req), 0, &peer_addr.sa, &peer_addrlen);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Error reading control request: %s\n", strerror(errno));
      return;
    }

  if (ret != 8)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Got control request from %s with size %d\n", address, ret);
      return;
    }

  if ((req[0] != 0x80) || (req[1] != 0xd5))
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Packet header from %s doesn't match retransmit request (got 0x%02x%02x, expected 0x80d5)\n", address, req[0], req[1]);
      return;
    }

  session = session_find_by_address(&peer_addr);
  if (!session)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Control request from %s; not a AirPlay client\n", address);
      return;
    }

  memcpy(&seq_start, req + 4, 2);
  memcpy(&seq_len, req + 6, 2);

  seq_start = ntohs(seq_start);
  seq_len = ntohs(seq_len);

  packets_resend(session, seq_start, seq_len);
}


/* -------------------- Handlers for sending RTSP requests ------------------ */

static int
payload_make_flush(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  struct airplay_master_session *ams = session->master_session;
  char buf[64];
  int ret;

  /* Restart sequence */
  ret = snprintf(buf, sizeof(buf), "seq=%" PRIu16 ";rtptime=%u", ams->rtp_session->seqnum, ams->rtp_session->pos);
  if ((ret < 0) || (ret >= sizeof(buf)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "RTP-Info too big for buffer in FLUSH request\n");
      return -1;
    }
  evrtsp_add_header(req->output_headers, "RTP-Info", buf);

  return 0;
}

// Sending an empty plist seems to mean close connection. At least according to
// shairport-sync handle_teardown_2(). iOS seems to send first a teardown with
// stream (to close that), then with an empty plist.
static int
payload_make_teardown(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  uint8_t *data;
  size_t len;
  int ret;

  // Normally we update status when we get the response, but teardown is an
  // exception because we want to stop writing to the device immediately
  session->state = AIRPLAY_STATE_TEARDOWN;

  // stream = plist_new_dict();
  // wplist_dict_add_uint(stream, "streamID", 0); // Do we have a stream ID?
  // wplist_dict_add_uint(stream, "type", AIRPLAY_RTP_PAYLOADTYPE);
  // streams = plist_new_array();
  // plist_array_append_item(streams, stream);

  root = plist_new_dict();
  // plist_dict_set_item(root, "streams", streams);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  // Serialization returns owned bytes; appending copies them into the request.
  ret = evbuffer_add(req->output_buffer, data, len);
  plist_to_bin_free((char *)data);
  return ret;
}

static int
payload_make_set_volume(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  float raop_volume;
  char volstr[32];
  int ret;

  int volume = arg ? *(const int *)arg : session->volume;
  raop_volume =
      volume == 0 ? -144.0f : airplay_volume_from_pct(volume, session->devname);

  /* Don't let locales get in the way here */
  /* We use -%d and -(int)raop_volume so -0.3 won't become 0.3 */
  snprintf(volstr, sizeof(volstr), "-%d.%06d", -(int)raop_volume, -(int)(1000000.0 * (raop_volume - (int)raop_volume)));

  DPRINTF(E_DBG, L_AIRPLAY, "Sending volume %s to '%s'\n", volstr, session->devname);

  ret = evbuffer_add_printf(req->output_buffer, "volume: %s\r\n", volstr);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Out of memory for SET_PARAMETER payload (volume)\n");
      return -1;
    }

  return 0;
}








/*
Audio formats

Bit 	Value 	Type
2 	0x4 	PCM/8000/16/1
3 	0x8 	PCM/8000/16/2
4 	0x10 	PCM/16000/16/1
5 	0x20 	PCM/16000/16/2
6 	0x40 	PCM/24000/16/1
7 	0x80 	PCM/24000/16/2
8 	0x100 	PCM/32000/16/1
9 	0x200 	PCM/32000/16/2
10 	0x400 	PCM/44100/16/1
11 	0x800 	PCM/44100/16/2
12 	0x1000 	PCM/44100/24/1
13 	0x2000 	PCM/44100/24/2
14 	0x4000 	PCM/48000/16/1
15 	0x8000 	PCM/48000/16/2
16 	0x10000 	PCM/48000/24/1
17 	0x20000 	PCM/48000/24/2
18 	0x40000 	ALAC/44100/16/2
19 	0x80000 	ALAC/44100/24/2
20 	0x100000 	ALAC/48000/16/2
21 	0x200000 	ALAC/48000/24/2
22 	0x400000 	AAC-LC/44100/2
23 	0x800000 	AAC-LC/48000/2
24 	0x1000000 	AAC-ELD/44100/2
25 	0x2000000 	AAC-ELD/48000/2
26 	0x4000000 	AAC-ELD/16000/1
27 	0x8000000 	AAC-ELD/24000/1
28 	0x10000000 	OPUS/16000/1
29 	0x20000000 	OPUS/24000/1
30 	0x40000000 	OPUS/48000/1
31 	0x80000000 	AAC-ELD/44100/1
32 	0x100000000 	AAC-ELD/48000/1
*/
static int
payload_make_setup_stream(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  plist_t streams;
  plist_t stream;
  uint8_t *data;
  size_t len;
  int ret;

  stream = plist_new_dict();
  wplist_dict_add_uint(stream, "audioFormat", 262144); // 0x40000 ALAC/44100/16/2
  wplist_dict_add_string(stream, "audioMode", "default");
  wplist_dict_add_uint(stream, "controlPort", session->control_svc->port);
  wplist_dict_add_uint(stream, "ct", 2); // Compression type, 1 LPCM, 2 ALAC, 3 AAC, 4 AAC ELD, 32 OPUS
  wplist_dict_add_bool(stream, "isMedia", true); // ?
  struct protocol_peer *peer = protocol_peer_get(session->device_id);
  uint32_t maximum =
      peer && peer->presentation_base_ms
          ? (uint32_t)((peer->presentation_base_ms + peer->offset_ms) * 44100 /
                       1000)
          : 88200;
  if (arg) {
    const sender_presentation *presentation = arg;
    maximum = (uint32_t)((presentation->base_ms + presentation->offset_ms) *
                         44100 / 1000);
  }
  wplist_dict_add_uint(stream, "latencyMax", receiver_window_samples(maximum));
  uint32_t base_ms = arg ? ((const sender_presentation *)arg)->base_ms
      : peer && peer->presentation_base_ms ? peer->presentation_base_ms : 2000;
  int offset_ms = arg ? ((const sender_presentation *)arg)->offset_ms
                      : peer ? peer->offset_ms : 0;
  uint32_t render_ms = receiver_latency_ms(peer ? peer->buffers.render_ms : 0,
                                           base_ms, offset_ms);
  wplist_dict_add_uint(stream, "latencyMin", render_ms * 44100 / 1000);
  wplist_dict_add_data(stream, "shk", session->shared_secret,
                       AIRPLAY_AUDIO_KEY_LEN);
  wplist_dict_add_uint(stream, "spf", AIRPLAY_SAMPLES_PER_PACKET); // frames per packet
  wplist_dict_add_uint(stream, "sr", AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT); // sample rate
  wplist_dict_add_uint(stream, "type", AIRPLAY_RTP_PAYLOADTYPE); // RTP type, 0x60 = 96 real time, 103 buffered
  wplist_dict_add_bool(stream, "supportsDynamicStreamID", false);
  wplist_dict_add_uint(stream, "streamConnectionID", session->session_id); // Hopefully fine since we have one stream per session
  streams = plist_new_array();
  plist_array_append_item(streams, stream);

  root = plist_new_dict();
  plist_dict_set_item(root, "streams", streams);
  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  ret = evbuffer_add(req->output_buffer, data, len);
  plist_to_bin_free((char *)data);
  return ret;
}

// Mysterious empty request, but iOS sends it
static int
payload_make_record(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  return 0;
}

static int
payload_make_setpeers(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  uint8_t *data;
  size_t len;
  int ret;

  if (!session->master_session->use_ptp)
    return 1; // Skip to next request

  plist_t root;

  root = plist_new_array();

  plist_array_append_item(root, plist_new_string(session->address));
  if (session->local_v4_address)
    plist_array_append_item(root, plist_new_string(session->local_v4_address));
  if (session->local_v6_address)
    plist_array_append_item(root, plist_new_string(session->local_v6_address));

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  ret = evbuffer_add(req->output_buffer, data, len);
  plist_to_bin_free((char *)data);
  return ret;
}

static int
payload_make_setup_session_ntp(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  char device_id_colon[24];
  uint8_t *data;
  size_t len;
  int ret;

  device_id_colon_make(device_id_colon, sizeof(device_id_colon), airplay_device_id);

  root = plist_new_dict();
  wplist_dict_add_string(root, "deviceID", device_id_colon);
  wplist_dict_add_string(root, "sessionUUID", session->session_uuid);
  wplist_dict_add_uint(root, "timingPort", session->timing_svc->port);
  wplist_dict_add_string(root, "timingProtocol", "NTP"); // If set to "None" then an ATV4 will not respond to stream SETUP request

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  ret = evbuffer_add(req->output_buffer, data, len);
  plist_to_bin_free((char *)data);
  return ret;
}

static int
payload_make_setup_session_ptp(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  plist_t root;
  plist_t addresses;
  plist_t timingpeerinfo;
  plist_t timingpeerlist;
  char device_id_colon[24];
  uint8_t *data;
  size_t len;
  int ret;

  device_id_colon_make(device_id_colon, sizeof(device_id_colon), airplay_device_id);

  addresses = plist_new_array();
  if (session->local_v4_address)
    plist_array_append_item(addresses, plist_new_string(session->local_v4_address));
  if (session->local_v6_address)
    plist_array_append_item(addresses, plist_new_string(session->local_v6_address));

  timingpeerinfo = plist_new_dict();
  wplist_dict_add_string(timingpeerinfo, "ID", airplay_ptp_clock_uuid); // iOS sends a UUID, but where does it come from?
  wplist_dict_add_uint(timingpeerinfo, "DeviceType", 0);
  wplist_dict_add_int(timingpeerinfo, "ClockID", (int64_t)ptpd_clock_id_get()); // ClockID in plist is signed, so e.g. 0xf842885f71750008 -> -557733460333756408
  wplist_dict_add_bool(timingpeerinfo, "SupportsClockPortMatchingOverride", false); // iOS says true, no idea what it means
  plist_dict_set_item(timingpeerinfo, "Addresses", addresses);

  timingpeerlist = plist_new_array();
  plist_array_append_item(timingpeerlist, plist_copy(timingpeerinfo));

  root = plist_new_dict();
  wplist_dict_add_string(root, "name", airplay_client_name);
  wplist_dict_add_string(root, "deviceID", device_id_colon);
  wplist_dict_add_string(root, "sessionUUID", session->session_uuid);
  wplist_dict_add_string(root, "timingProtocol", "PTP"); // If set to "None" then an ATV4 will not respond to stream SETUP request
  wplist_dict_add_string(root, "macAddress", session->local_mac_address);

  wplist_dict_add_string(root, "groupUUID", session->group_uuid);
  wplist_dict_add_bool(root, "groupContainsGroupLeader", false); // iOS Music app sets this to false, let's roll with that

  plist_dict_set_item(root, "timingPeerInfo", timingpeerinfo);
  plist_dict_set_item(root, "timingPeerList", timingpeerlist);

  ret = wplist_to_bin(&data, &len, root);
  plist_free(root);

  if (ret < 0)
    return -1;

  ret = evbuffer_add(req->output_buffer, data, len);
  plist_to_bin_free((char *)data);
  return ret;
}

static int
payload_make_setup_session(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  if (!session->master_session->use_ptp)
    return payload_make_setup_session_ntp(req, session, arg);

  return payload_make_setup_session_ptp(req, session, arg);
}

/*
The purpose of auth-setup is to authenticate the device and to exchange keys
for encryption. We don't do that, but some AirPlay 2 speakers (Sonos beam,
Airport Express fw 7.8) require this step anyway, otherwise we get a 403 to
our ANNOUNCE. So we do it with a flag for no encryption, and without actually
authenticating the device.

Good to know (source Apple's MFi Accessory Interface Specification):
- Curve25519 Elliptic-Curve Diffie-Hellman technology for key exchange
- RSA for signing and verifying and AES-128 in counter mode for encryption
- We start by sending a Curve25519 public key + no encryption flag
- The device responds with public key, MFi certificate and a signature, which
  is created by the device signing the two public keys with its RSA private
  key and then encrypting the result with the AES master key derived from the
  Curve25519 shared secret (generated from device private key and our public
  key)
- The AES key derived from the Curve25519 shared secret can then be used to
  encrypt future content
- New keys should be generated for each authentication attempt, but we don't
  do that because we don't really use this + it adds a libsodium dependency

Since we don't do auth nor encryption, we currently just ignore the reponse.
*/

#if AIRPLAY_USE_AUTH_SETUP
static int
payload_make_auth_setup(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  if (!session->supports_auth_setup)
    return 1; // skip this request

  // Flag for no encryption. 0x10 may mean encryption.
  evbuffer_add(req->output_buffer, "\x01", 1);

  evbuffer_add(req->output_buffer, airplay_auth_setup_pubkey, sizeof(airplay_auth_setup_pubkey) - 1);

  return 0;
}
#endif

/* airplay2-receiver says this about X-Apple-HKP:
 Values 0,2,3,4,6 seen.
 0 = Unauth. When Ft48TransientPairing and Ft43SystemPairing are absent
 2 = (pair-setup complete, pair-verify starts)
 3 = SystemPairing (with Ft43SystemPairing)
 4 = Transient
 6 = HomeKit
 7 = HomeKit (administration)
 */
static int
payload_make_pin_start(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  DPRINTF(E_LOG, L_AIRPLAY, "Starting device pairing for '%s', go to the web interface and enter PIN\n", session->devname);

  if (session->pair_type == PAIR_CLIENT_HOMEKIT_NORMAL)
    evrtsp_add_header(req->output_headers, "X-Apple-HKP", "3");
  else if (session->pair_type == PAIR_CLIENT_HOMEKIT_TRANSIENT)
    evrtsp_add_header(req->output_headers, "X-Apple-HKP", "4");

  return 0;
}

static int
payload_make_pair_generic(int step, struct evrtsp_request *req, struct airplay_session *session)
{
  uint8_t *body;
  size_t len;
  const char *errmsg;

  switch (step)
    {
      case 1:
	body    = pair_setup_request1(&len, session->pair_setup_ctx);
	errmsg  = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 2:
	body    = pair_setup_request2(&len, session->pair_setup_ctx);
	errmsg  = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 3:
	body    = pair_setup_request3(&len, session->pair_setup_ctx);
	errmsg  = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 4:
	body    = pair_verify_request1(&len, session->pair_verify_ctx);
	errmsg  = pair_verify_errmsg(session->pair_verify_ctx);
	break;
      case 5:
	body    = pair_verify_request2(&len, session->pair_verify_ctx);
	errmsg  = pair_verify_errmsg(session->pair_verify_ctx);
	break;
      default:
	body    = NULL;
	errmsg  = "Bug! Bad step number";
    }

  if (!body)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Verification step %d request error: %s\n", step, errmsg);
      return -1;
    }

  evbuffer_add(req->output_buffer, body, len);
  free(body);

  // Required!!
  if (session->pair_type == PAIR_CLIENT_HOMEKIT_NORMAL)
    evrtsp_add_header(req->output_headers, "X-Apple-HKP", "3");
  else if (session->pair_type == PAIR_CLIENT_HOMEKIT_TRANSIENT)
    evrtsp_add_header(req->output_headers, "X-Apple-HKP", "4");

  return 0;
}

static int payload_make_pair_setup1(struct evrtsp_request *req,
                                    struct airplay_session *session,
                                    void *arg) {
  const char *pin = arg;
  char device_id_hex[16 + 1];

  if (!pin && session->password &&
      session->pair_type == PAIR_CLIENT_HOMEKIT_NORMAL)
    pin = session->password;  // For password based authentication

  if (pin) session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;

  snprintf(device_id_hex, sizeof(device_id_hex), "%016" PRIX64,
           airplay_device_id);

  session->pair_setup_ctx =
      pair_setup_new(session->pair_type, pin, NULL, NULL, device_id_hex);
  if (!session->pair_setup_ctx) {
    DPRINTF(E_LOG, L_AIRPLAY, "Out of memory for verification setup context\n");
    return -1;
  }

  session->state = AIRPLAY_STATE_AUTH;

  return payload_make_pair_generic(1, req, session);
}

static int
payload_make_pair_setup2(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  return payload_make_pair_generic(2, req, session);
}

static int
payload_make_pair_setup3(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  return payload_make_pair_generic(3, req, session);
}

static int
payload_make_pair_verify1(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  struct protocol_peer *device;
  char device_id_hex[16 + 1];

  device = protocol_peer_get(session->device_id);
  if (!device)
    return -1;

  snprintf(device_id_hex, sizeof(device_id_hex), "%016" PRIX64, airplay_device_id);

  session->pair_verify_ctx = pair_verify_new(session->pair_type, device->auth_key, NULL, NULL, device_id_hex);
  if (!session->pair_verify_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't create verification verify context (invalid auth key?)\n");
      return -1;
    }

  return payload_make_pair_generic(4, req, session);
}

static int
payload_make_pair_verify2(struct evrtsp_request *req, struct airplay_session *session, void *arg)
{
  return payload_make_pair_generic(5, req, session);
}


/* ------------------------------ Session startup --------------------------- */

static void
start_failure(struct airplay_session *session)
{
  struct protocol_peer *device;

  device = protocol_peer_get(session->device_id);
  if (!device)
    {
      session_failure(session);
      return;
    }

  // If our key was incorrect, or the device reset its pairings, then this
  // function was called because the encrypted request (SETUP) timed out
  if (device->auth_key && !session->initial_volume_failed)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Clearing '%s' pairing keys, you need to pair again\n", session->devname);

      secure_string_free(device->auth_key);
      device->auth_key = NULL;
      device->requires_auth = 1;
    }

  session_failure(session);
}

static void
start_retry(struct airplay_session *session)
{
  session_failure(session);
}


/* ---------------------------- RTSP response handlers ---------------------- */

static enum airplay_seq_type
response_handler_pin_start(struct evrtsp_request *req, struct airplay_session *session)
{
  session->state = AIRPLAY_STATE_AUTH;

  return AIRPLAY_SEQ_CONTINUE; // TODO before we reported failure since device is locked
}

/* Opt-in numeric observations only: never dump plists containing session keys.
 * Missing fields and reported zero remain distinct. These values describe the
 * receiver response, not measured playback or sender-requested parameters. */
struct receiver_latency_observation {
  uint64_t values[3];
  unsigned available, invalid;
};

static struct receiver_latency_observation receiver_latency_read(
    plist_t stream) {
  struct receiver_latency_observation result = {0};
  const char *keys[] = {"latencyMin", "latencyMax", "arrivalToRenderLatencyMs"};
  for (unsigned i = 0; i < ARRAY_SIZE(keys); ++i) {
    plist_t item = stream && plist_get_node_type(stream) == PLIST_DICT
                       ? plist_dict_get_item(stream, keys[i])
                       : NULL;
    if (!item) continue;
    if (plist_get_node_type(item) != PLIST_UINT) {
      result.invalid |= 1u << i;
      continue;
    }
    plist_get_uint_val(item, &result.values[i]);
    result.available |= 1u << i;
  }
  return result;
}

static bool receiver_latency_accept(struct airplay_session *session,
                                    plist_t stream) {
  struct protocol_peer *peer = protocol_peer_get(session->device_id);
  if (!peer) return false;
  struct receiver_latency_observation value = receiver_latency_read(stream);
  if ((value.invalid & 4u) ||
      ((value.available & 4u) && value.values[2] > UINT32_MAX)) {
    protocol_diagnostic("Invalid arrivalToRenderLatencyMs in stream SETUP response");
    return false;
  }
  peer->receiver_render_available = (value.available & 4u) != 0;
  peer->receiver_render_ms = peer->receiver_render_available
                                 ? (uint32_t)value.values[2]
                                 : 0;
  return true;
}

static enum airplay_seq_type response_handler_feedback(
    struct evrtsp_request *req, struct airplay_session *session) {
  (void)req;
  (void)session;
  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type response_handler_record(
    struct evrtsp_request *req, struct airplay_session *session) {
  (void)req;
  session->state = AIRPLAY_STATE_RECORD;

  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type response_handler_setup_stream(
    struct evrtsp_request *req, struct airplay_session *session) {
  plist_t response;
  plist_t streams;
  plist_t stream;
  plist_t item;
  uint64_t uintval;
  int ret;

  DPRINTF(E_INFO, L_AIRPLAY, "Setting up AirPlay session %u to %s\n",
          session->session_id, session->address);

  ret = wplist_from_evbuf(&response, req->input_buffer);
  if (ret < 0) {
    DPRINTF(E_LOG, L_AIRPLAY, "Could not parse plist from '%s'\n",
            session->devname);
    return AIRPLAY_SEQ_ABORT;
  }

  streams = plist_dict_get_item(response, "streams");
  if (!streams) {
    DPRINTF(E_LOG, L_AIRPLAY,
            "Could not find streams item in response from '%s'\n",
            session->devname);
    goto error;
  }

  stream = plist_array_get_item(streams, 0);
  if (!stream)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not find stream item in response from '%s'\n", session->devname);
      goto error;
    }

  if (!receiver_latency_accept(session, stream)) goto error;

  item = plist_dict_get_item(stream, "dataPort");
  if (item)
    {
      plist_get_uint_val(item, &uintval);
      session->data_port = uintval;
    }

  item = plist_dict_get_item(stream, "controlPort");
  if (item)
    {
      plist_get_uint_val(item, &uintval);
      session->control_port = uintval;
    }

  if (session->data_port == 0 || session->control_port == 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Missing port number in reply from '%s' (d=%u, c=%u)\n", session->devname, session->data_port, session->control_port);
      goto error;
    }

  DPRINTF(E_DBG, L_AIRPLAY, "Negotiated UDP streaming session; ports d=%u c=%u e=%u\n", session->data_port, session->control_port, session->events_port);

  session->server_fd = net_connect(session->address, session->data_port, SOCK_DGRAM, "AirPlay data");
  if (session->server_fd < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not connect to data port of '%s'\n", session->devname);
      goto error;
    }

  session->state = AIRPLAY_STATE_SETUP;

  plist_free(response);
  return AIRPLAY_SEQ_CONTINUE;

 error:
  plist_free(response);
  return AIRPLAY_SEQ_ABORT;
 }

static enum airplay_seq_type response_handler_volume_start(
    struct evrtsp_request *req, struct airplay_session *session) {
  if (req && req->response_code == RTSP_OK) return AIRPLAY_SEQ_CONTINUE;
  session->initial_volume_failed = true;
  protocol_operation("SET_PARAMETER (volume)", req ? req->response_code : 0);
  protocol_diagnostic("Receiver did not accept initial volume (peer=%d)",
                      req ? req->response_code : 0);
  return AIRPLAY_SEQ_ABORT;
}

static int
handle_timingpeerinfo(uint32_t *slave_id, plist_t response, const char *local_v4_address, const char *local_v6_address)
{
  int family = local_v6_address ? AF_INET6 : AF_INET;
  plist_t item;
  plist_t peer_addresses;
  plist_t peer_address;
  const char *ptr;
  union net_sockaddr naddr;
  char peer_straddress[128];
  char ifname[32];
  char *response_str;
  int ret;
  int i;

  item = plist_dict_get_item(response, "timingPeerInfo");
  if (!item)
    goto error;

  peer_addresses = plist_dict_get_item(item, "Addresses");
  if (!peer_addresses)
    goto error;

  // Walk through addresses to get one from the right family
  for (i = 0; (peer_address = plist_array_get_item(peer_addresses, i)); i++)
    {
      ptr = plist_get_string_ptr(peer_address, NULL);
      if (!ptr)
	continue;

      DPRINTF(E_SPAM, L_AIRPLAY, "Checking timing peer '%s'\n", ptr);

      // If address is ipv6 and ipv6 is not enabled, this will return -1
      ret = net_sockaddr_get(&naddr, ptr, 0);
      if (ret < 0 || naddr.sa.sa_family != family)
	continue;

      // Append %ifname to ipv6 because libairptpd needs it to send if the address is link-local (fe80)
      if (family == AF_INET6 && net_if_get(ifname, sizeof(ifname), local_v6_address) == 0)
	snprintf(peer_straddress, sizeof(peer_straddress), "%s%%%s", ptr, ifname);
      else if (family == AF_INET)
	snprintf(peer_straddress, sizeof(peer_straddress), "%s", ptr);
      else
	continue;

      // Just add the first good address
      ret = ptpd_slave_add(slave_id, peer_straddress);
      if (ret == 0)
	return 0;
    }

 error:
  response_str = wplist_to_xml(response);
  DPRINTF(E_LOG, L_AIRPLAY, "SETUP timing peer information has no reachable address for the selected local address family\n");
  free(response_str);
  return -1;
}

static enum airplay_seq_type
response_handler_setup_session(struct evrtsp_request *req, struct airplay_session *session)
{
  plist_t response;
  plist_t item;
  uint64_t uintval;
  int ret;
  if (req->response_code == RTSP_UNAUTHORIZED)
    {
      if (session->req_has_auth)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Bad or missing password for device '%s' (%s)\n", session->devname, session->address);
	  return AIRPLAY_SEQ_ABORT;
	}

      // We haven't tried authenticating yet, so save realm and nonce from the
      // received WWW-Authenticate header and trigger a re-run with auth header
      ret = auth_header_parse(session, req);
      if (ret < 0)
	return AIRPLAY_SEQ_ABORT;

      return AIRPLAY_SEQ_START_PLAYBACK;
    }
  else if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Unexpected reply to SETUP (session) from '%s'\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  ret = wplist_from_evbuf(&response, req->input_buffer);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not parse plist from '%s'\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  item = plist_dict_get_item(response, "eventPort");
  if (item)
    {
      plist_get_uint_val(item, &uintval);
      session->events_port = uintval;
    }

  if (session->events_port == 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "SETUP reply is missing event port\n");
      goto error;
    }

  // Reverse connection, used to receive playback events from device
  ret = airplay_events_listen(session->devname, session->address, session->events_port, session->shared_secret, session->shared_secret_len);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Could not connect to '%s' events port %u, proceeding anyway\n", session->devname, session->events_port);
    }

  if (session->master_session->use_ptp)
    {
      ret = handle_timingpeerinfo(&session->ptpd_slave_id, response, session->local_v4_address, session->local_v6_address);
      if (ret < 0)
	goto error;
    }

  plist_free(response);
  return AIRPLAY_SEQ_CONTINUE;

 error:
  plist_free(response);
  return AIRPLAY_SEQ_ABORT;
}

static enum airplay_seq_type
response_handler_flush(struct evrtsp_request *req, struct airplay_session *session)
{
  session->state = AIRPLAY_STATE_CONNECTED;
  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_teardown(struct evrtsp_request *req, struct airplay_session *session)
{
  if (req->response_code != RTSP_OK) protocol_end_error(req->response_code);
  session->state = AIRPLAY_STATE_STOPPED;
  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_teardown_failure(struct evrtsp_request *req, struct airplay_session *session)
{
  if (session->state != AIRPLAY_STATE_AUTH)
    session->state = AIRPLAY_STATE_FAILED;
  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_info_generic(struct evrtsp_request *req, struct airplay_session *session)
{
  struct protocol_peer *device;
  plist_t response;
  plist_t item;
  int ret;

  device = protocol_peer_get(session->device_id);
  if (!device)
    return AIRPLAY_SEQ_ABORT;

  ret = session_ids_set(session);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not make session url or id for device '%s'\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  ret = wplist_from_evbuf(&response, req->input_buffer);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not parse plist from '%s'\n", session->devname);
      return AIRPLAY_SEQ_ABORT;
    }

  item = plist_dict_get_item(response, "statusFlags");
  if (item)
    plist_get_uint_val(item, &session->statusflags);

  plist_free(response);

  DPRINTF(E_DBG, L_AIRPLAY,
          "Status flags from '%s' was %" PRIu64
          ": cable attached %d, one time pairing %d, password %d, PIN %d\n",
          session->devname, session->statusflags,
          (bool)(session->statusflags & AIRPLAY_FLAG_AUDIO_CABLE_ATTACHED),
          (bool)(session->statusflags & AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED),
          (bool)(session->statusflags & AIRPLAY_FLAG_PASSWORD_REQUIRED),
          (bool)(session->statusflags & AIRPLAY_FLAG_PIN_REQUIRED));

  // Identify next sequence based on response
  if (session->statusflags & AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED)
    {
      session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;

      if (!device->auth_key)
	{
	  if (session->password)
	    return AIRPLAY_SEQ_PAIR_SETUP;
	  device->requires_auth = 1;
          session->state = AIRPLAY_STATE_AUTH;
	  return AIRPLAY_SEQ_PIN_START;
	}

      session->state = AIRPLAY_STATE_INFO;
      return AIRPLAY_SEQ_PAIR_VERIFY;
    }
  else if (session->statusflags & AIRPLAY_FLAG_PIN_REQUIRED)
    {
      secure_string_free(device->auth_key);
      device->auth_key = NULL;
      device->requires_auth = 1;

      session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;
      session->state = AIRPLAY_STATE_AUTH;
      if (session->password) return AIRPLAY_SEQ_PAIR_SETUP;
      return AIRPLAY_SEQ_PIN_START;
    } else if (session->statusflags & AIRPLAY_FLAG_PASSWORD_REQUIRED) {
      session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;

      /* Use a supplied password once, then verify the replacement keys on the
       * fresh control connection created by successful pairing. */
      if (session->password && !device->password_auth_started) {
        device->password_auth_started = true;
        session->state = AIRPLAY_STATE_AUTH;
        return AIRPLAY_SEQ_PAIR_SETUP;
      }

      if (!session->password && !device->auth_key) {
        /* Interactive callers must receive an authentication result so they
         * can request input instead of retrying the network connection. */
        device->requires_auth = 1;
        session->state = AIRPLAY_STATE_AUTH;
        DPRINTF(E_LOG, L_AIRPLAY,
                "Receiver requests an AirPlay password (statusFlags=%" PRIu64
                "); enter the receiver password to continue\n",
                session->statusflags);
        return AIRPLAY_SEQ_ABORT;
      } else if (!device->auth_key) {
        session->state = AIRPLAY_STATE_AUTH;
        return AIRPLAY_SEQ_PAIR_SETUP;
      }

      session->state = AIRPLAY_STATE_INFO;
      return AIRPLAY_SEQ_PAIR_VERIFY;
    }

    session->pair_type = PAIR_CLIENT_HOMEKIT_TRANSIENT;
    session->state = AIRPLAY_STATE_INFO;
    return AIRPLAY_SEQ_PAIR_TRANSIENT;
}

static enum airplay_seq_type response_handler_info_probe(
    struct evrtsp_request *req, struct airplay_session *session) {
  enum airplay_seq_type seq_type;

  seq_type = response_handler_info_generic(req, session);
  if (seq_type == AIRPLAY_SEQ_PAIR_TRANSIENT || seq_type == AIRPLAY_SEQ_PAIR_VERIFY)
    seq_type = AIRPLAY_SEQ_CONTINUE; // When probing we don't proceed to PAIR_TRANSIENT/VERIFY

  return seq_type;
}

static enum airplay_seq_type
response_handler_info_start(struct evrtsp_request *req, struct airplay_session *session)
{
  enum airplay_seq_type seq_type;

  seq_type = response_handler_info_generic(req, session);
  if (seq_type != AIRPLAY_SEQ_ABORT && seq_type != AIRPLAY_SEQ_PIN_START)
    session->next_seq = AIRPLAY_SEQ_START_PLAYBACK; // Pair and then run SEQ_START_PLAYBACK which sets up the playback

  return seq_type;
}

static enum airplay_seq_type
response_handler_pair_generic(int step, struct evrtsp_request *req, struct airplay_session *session)
{
  uint8_t *response;
  const char *errmsg;
  size_t len;
  int ret;

  response = evbuffer_pullup(req->input_buffer, -1);
  len = evbuffer_get_length(req->input_buffer);

  switch (step)
    {
      case 1:
	ret = pair_setup_response1(session->pair_setup_ctx, response, len);
	errmsg = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 2:
	ret = pair_setup_response2(session->pair_setup_ctx, response, len);
	errmsg = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 3:
	ret = pair_setup_response3(session->pair_setup_ctx, response, len);
	errmsg = pair_setup_errmsg(session->pair_setup_ctx);
	break;
      case 4:
	ret = pair_verify_response1(session->pair_verify_ctx, response, len);
	errmsg = pair_verify_errmsg(session->pair_verify_ctx);
	break;
      case 5:
	ret = pair_verify_response2(session->pair_verify_ctx, response, len);
	errmsg = pair_verify_errmsg(session->pair_verify_ctx);
	break;
      default:
	ret = -1;
	errmsg = "Bug! Bad step number";
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pairing step %d response from '%s' error: %s\n", step, session->devname, errmsg);
      DHEXDUMP(E_DBG, L_AIRPLAY, response, len, "Raw response");
      return AIRPLAY_SEQ_ABORT;
    }

  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_pair_setup1(struct evrtsp_request *req, struct airplay_session *session)
{
  struct protocol_peer *device;

  if (session->pair_type == PAIR_CLIENT_HOMEKIT_TRANSIENT && req->response_code == RTSP_CONNECTION_AUTH_REQUIRED)
    {
      device = protocol_peer_get(session->device_id);
      if (!device)
	return AIRPLAY_SEQ_ABORT;

      device->requires_auth = 1; // FIXME might be reset by mdns announcement
      session->pair_type = PAIR_CLIENT_HOMEKIT_NORMAL;

      return AIRPLAY_SEQ_PIN_START;
    }

  return response_handler_pair_generic(1, req, session);
}

static enum airplay_seq_type
response_handler_pair_setup2(struct evrtsp_request *req, struct airplay_session *session)
{
  enum airplay_seq_type seq_type;
  struct pair_result *result;
  int ret;

  seq_type = response_handler_pair_generic(2, req, session);
  if (seq_type != AIRPLAY_SEQ_CONTINUE)
    return seq_type;

  if (session->pair_type != PAIR_CLIENT_HOMEKIT_TRANSIENT)
    return seq_type;

  ret = pair_setup_result(NULL, &result, session->pair_setup_ctx);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Transient setup result error: %s\n", pair_setup_errmsg(session->pair_setup_ctx));
      goto error;
    }

  ret = session_cipher_setup(session, result->shared_secret, result->shared_secret_len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pair transient error setting up encryption for '%s'\n", session->devname);
      goto error;
    }

  return AIRPLAY_SEQ_CONTINUE;

 error:
  session->state = AIRPLAY_STATE_FAILED;
  return AIRPLAY_SEQ_ABORT;
}

static enum airplay_seq_type
response_handler_pair_setup3(struct evrtsp_request *req, struct airplay_session *session)
{
  struct protocol_peer *device;
  const char *authorization_key;
  enum airplay_seq_type seq_type;
  int ret;

  seq_type = response_handler_pair_generic(3, req, session);
  if (seq_type != AIRPLAY_SEQ_CONTINUE)
    return seq_type;

  ret = pair_setup_result(&authorization_key, NULL, session->pair_setup_ctx);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pair setup result error: %s\n", pair_setup_errmsg(session->pair_setup_ctx));
      return AIRPLAY_SEQ_ABORT;
    }

  DPRINTF(E_LOG, L_AIRPLAY, "Pair setup stage complete, saving authorization key\n");

  device = protocol_peer_get(session->device_id);
  if (!device)
    return AIRPLAY_SEQ_ABORT;

  secure_string_free(device->auth_key);
  device->auth_key = strdup(authorization_key);
  if (!device->auth_key) return AIRPLAY_SEQ_ABORT;

  protocol_credentials_changed(device->auth_key);

  // No longer AIRPLAY_STATE_AUTH
  session->state = AIRPLAY_STATE_STOPPED;

  if (session->next_seq == AIRPLAY_SEQ_START_PLAYBACK)
    session->pair_restart_pending = true;

  return AIRPLAY_SEQ_CONTINUE;
}

static enum airplay_seq_type
response_handler_pair_verify1(struct evrtsp_request *req, struct airplay_session *session)
{
  struct protocol_peer *device;
  enum airplay_seq_type seq_type;

  seq_type = response_handler_pair_generic(4, req, session);
  if (seq_type != AIRPLAY_SEQ_CONTINUE)
    {
      session->state = AIRPLAY_STATE_AUTH;

      device = protocol_peer_get(session->device_id);
      if (!device)
	return AIRPLAY_SEQ_ABORT;

      // Clear auth_key, the device did not accept it
      secure_string_free(device->auth_key);
      device->auth_key = NULL;

      return AIRPLAY_SEQ_ABORT;
    }

  return seq_type;
}

static enum airplay_seq_type
response_handler_pair_verify2(struct evrtsp_request *req, struct airplay_session *session)
{
  struct protocol_peer *device;
  enum airplay_seq_type seq_type;
  struct pair_result *result;
  int ret;

  seq_type = response_handler_pair_generic(5, req, session);
  if (seq_type != AIRPLAY_SEQ_CONTINUE)
    goto error;

  ret = pair_verify_result(&result, session->pair_verify_ctx);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pair verify result error: %s\n", pair_verify_errmsg(session->pair_verify_ctx));
      goto error;
    }

  ret = session_cipher_setup(session, result->shared_secret, result->shared_secret_len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Pair verify error setting up encryption for '%s'\n", session->devname);
      goto error;
    }

  return AIRPLAY_SEQ_CONTINUE;

 error:
  device = protocol_peer_get(session->device_id);
  if (!device)
    return AIRPLAY_SEQ_ABORT;

  // Clear auth_key, the device did not accept it, or some other unexpected error
  secure_string_free(device->auth_key);
  device->auth_key = NULL;

  session->state = AIRPLAY_STATE_AUTH;

  return AIRPLAY_SEQ_ABORT;
}


/* ---------------------- Request/response sequence control ----------------- */

/*
 * Request queueing HOWTO
 *
 * Sending:
 * - increment session->reqs_in_flight
 * - set evrtsp connection closecb to NULL
 *
 * Request callback:
 * - decrement session->reqs_in_flight first thing, even if the callback is
 *   called for error handling (req == NULL or HTTP error code)
 * - if session->reqs_in_flight == 0, setup evrtsp connection closecb
 *
 * When a request fails, the whole AirPlay session is declared failed and
 * torn down by calling session_failure(), even if there are requests
 * queued on the evrtsp connection. There is no reason to think pending
 * requests would work out better than the one that just failed and recovery
 * would be tricky to get right.
 *
 * evrtsp behaviour with queued requests:
 * - request callback is called with req == NULL to indicate a connection
 *   error; if there are several requests queued on the connection, this can
 *   happen for each request if the connection isn't destroyed
 * - the connection is reset, and the closecb is called if the connection was
 *   previously connected. There is no closecb set when there are requests in
 *   flight
 */

static struct airplay_seq_definition airplay_seq_definition[] =
{
  { AIRPLAY_SEQ_START, NULL, start_retry },
  { AIRPLAY_SEQ_START_PLAYBACK, session_connected, start_failure },
  { AIRPLAY_SEQ_PROBE, session_success, session_failure },
  { AIRPLAY_SEQ_FLUSH, session_status, session_failure },
  { AIRPLAY_SEQ_STOP, session_success, session_failure },
  { AIRPLAY_SEQ_FAILURE, session_success, session_failure},
  { AIRPLAY_SEQ_PIN_START, session_success, session_failure },
  { AIRPLAY_SEQ_SEND_VOLUME, session_status, session_failure },
  { AIRPLAY_SEQ_PAIR_SETUP, session_pair_success, session_failure },
  { AIRPLAY_SEQ_PAIR_VERIFY, session_pair_success, session_failure },
  { AIRPLAY_SEQ_PAIR_TRANSIENT, session_pair_success, session_failure },
  { AIRPLAY_SEQ_FEEDBACK, NULL, session_failure },
};

// The size of the second array dimension MUST at least be the size of largest
// sequence + 1, because then we can count on a zero terminator when iterating
static struct airplay_seq_request airplay_seq_request[][7] =
{
  {
    { AIRPLAY_SEQ_START, "GET /info", EVRTSP_REQ_GET, NULL, response_handler_info_start, NULL, "/info", false },
  },
  {
#if AIRPLAY_USE_AUTH_SETUP
    { AIRPLAY_SEQ_START_PLAYBACK, "auth-setup", EVRTSP_REQ_POST, payload_make_auth_setup, NULL, "application/octet-stream", "/auth-setup", true },
#endif
    // proceed_on_rtsp_not_ok is true because a device may reply with 401 Unauthorized
    // and a WWW-Authenticate header, and then we may need re-run with password auth
    { AIRPLAY_SEQ_START_PLAYBACK, "SETUP (session)", EVRTSP_REQ_SETUP, payload_make_setup_session, response_handler_setup_session, "application/x-apple-binary-plist", NULL, true },
    { AIRPLAY_SEQ_START_PLAYBACK, "RECORD", EVRTSP_REQ_RECORD, payload_make_record, response_handler_record, NULL, NULL, false },
    { AIRPLAY_SEQ_START_PLAYBACK, "SETPEERS", EVRTSP_REQ_SETPEERS, payload_make_setpeers, NULL, "/peer-list-changed", NULL, false },
    { AIRPLAY_SEQ_START_PLAYBACK, "SETUP (stream)", EVRTSP_REQ_SETUP, payload_make_setup_stream, response_handler_setup_stream, "application/x-apple-binary-plist", NULL, false },
    // Some devices (e.g. Sonos Symfonisk) don't register the volume if it isn't last
    { AIRPLAY_SEQ_START_PLAYBACK, "SET_PARAMETER (volume)", EVRTSP_REQ_SET_PARAMETER, payload_make_set_volume, response_handler_volume_start, "text/parameters", NULL, true },
  },
  {
    { AIRPLAY_SEQ_PROBE, "GET /info (probe)", EVRTSP_REQ_GET, NULL, response_handler_info_probe, NULL, "/info", false },
  },
  {
    { AIRPLAY_SEQ_FLUSH, "FLUSH", EVRTSP_REQ_FLUSH, payload_make_flush, response_handler_flush, NULL, NULL, false },
  },
  {
    { AIRPLAY_SEQ_STOP, "TEARDOWN", EVRTSP_REQ_TEARDOWN, payload_make_teardown, response_handler_teardown, NULL, NULL, true },
  },
  {
    { AIRPLAY_SEQ_FAILURE, "TEARDOWN (failure)", EVRTSP_REQ_TEARDOWN, payload_make_teardown, response_handler_teardown_failure, NULL, NULL, false },
  },
  {
    { AIRPLAY_SEQ_PIN_START, "PIN start", EVRTSP_REQ_POST, payload_make_pin_start, response_handler_pin_start, NULL, "/pair-pin-start", false },
  },
  {
    { AIRPLAY_SEQ_SEND_VOLUME, "SET_PARAMETER (volume)", EVRTSP_REQ_SET_PARAMETER, payload_make_set_volume, NULL, "text/parameters", NULL, true },
  },
  {
    { AIRPLAY_SEQ_PAIR_SETUP, "pair setup 1", EVRTSP_REQ_POST, payload_make_pair_setup1, response_handler_pair_setup1, "application/octet-stream", "/pair-setup", false },
    { AIRPLAY_SEQ_PAIR_SETUP, "pair setup 2", EVRTSP_REQ_POST, payload_make_pair_setup2, response_handler_pair_setup2, "application/octet-stream", "/pair-setup", false },
    { AIRPLAY_SEQ_PAIR_SETUP, "pair setup 3", EVRTSP_REQ_POST, payload_make_pair_setup3, response_handler_pair_setup3, "application/octet-stream", "/pair-setup", false },
  },
  {
    // Proceed on error is true because we want to delete the device key in the response handler if the verification fails
    { AIRPLAY_SEQ_PAIR_VERIFY, "pair verify 1", EVRTSP_REQ_POST, payload_make_pair_verify1, response_handler_pair_verify1, "application/octet-stream", "/pair-verify", true },
    { AIRPLAY_SEQ_PAIR_VERIFY, "pair verify 2", EVRTSP_REQ_POST, payload_make_pair_verify2, response_handler_pair_verify2, "application/octet-stream", "/pair-verify", false },
  },
  {
    // Some devices (i.e. my ATV4) gives a 470 when trying transient, so we proceed on that so the handler can trigger PIN setup sequence
    { AIRPLAY_SEQ_PAIR_TRANSIENT, "pair setup 1", EVRTSP_REQ_POST, payload_make_pair_setup1, response_handler_pair_setup1, "application/octet-stream", "/pair-setup", true },
    { AIRPLAY_SEQ_PAIR_TRANSIENT, "pair setup 2", EVRTSP_REQ_POST, payload_make_pair_setup2, response_handler_pair_setup2, "application/octet-stream", "/pair-setup", false },
  },
  {
    { AIRPLAY_SEQ_FEEDBACK, "POST /feedback", EVRTSP_REQ_POST, NULL, response_handler_feedback, NULL, "/feedback", true },
  },
};


static void
sequence_continue_cb(struct evrtsp_request *req, void *arg)
{
  struct airplay_seq_ctx *seq_ctx = arg;
  struct airplay_seq_request *cur_request = seq_ctx->cur_request;
  struct airplay_session *session = seq_ctx->session;
  enum airplay_seq_type seq_type;

  protocol_operation(cur_request->name, req ? req->response_code : 0);
  session->reqs_in_flight--;
  if (!session->reqs_in_flight)
    evrtsp_connection_set_closecb(session->ctrl, rtsp_close_cb, session);

  if (!req)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "No response to %s from '%s'\n", cur_request->name, session->devname);
      goto error;
    }

  if (req->response_code != RTSP_OK)
    {
      if (!cur_request->proceed_on_rtsp_not_ok)
	{
	  DPRINTF(E_LOG, L_AIRPLAY, "Response to %s from '%s' was negative, aborting (%d %s)\n", cur_request->name, session->devname, req->response_code, req->response_code_line);
	  goto error;
	}

      DPRINTF(E_WARN, L_AIRPLAY, "Response to %s from '%s' was negative, proceeding anyway (%d %s)\n", cur_request->name, session->devname, req->response_code, req->response_code_line);
    }

  // We don't check that the reply CSeq matches the request CSeq, because some
  // targets like Reflector and AirFoil don't return the CSeq according to the
  // rtsp spec. And the CSeq is not really important anyway.

  if (cur_request->response_handler)
    {
      seq_type = cur_request->response_handler(req, session);
      if (seq_type != AIRPLAY_SEQ_CONTINUE)
	{
	  if (seq_type == AIRPLAY_SEQ_ABORT)
	    goto error;

	  // Handler wanted to start a new sequence
	  sequence_start(seq_type, seq_ctx->session, seq_ctx->payload_make_arg, seq_ctx->log_caller);
	  free(seq_ctx);
	  return;
	}
    }

  seq_ctx->cur_request++;

  if (seq_ctx->cur_request->name)
    {
      sequence_continue(seq_ctx);
      return;
    }

  if (seq_ctx->on_success)
    seq_ctx->on_success(session);

  free(seq_ctx);
  return;

 error:
  if (cur_request->response_handler == response_handler_volume_start)
    session->initial_volume_failed = true;
  if (seq_ctx->on_error)
    seq_ctx->on_error(session);

  free(seq_ctx);
}

static void
sequence_continue(struct airplay_seq_ctx *seq_ctx)
{
  struct airplay_session *session = seq_ctx->session;
  struct airplay_seq_request *cur_request = seq_ctx->cur_request;
  struct evrtsp_request *req = NULL;
  const char *uri;
  int ret;

  req = evrtsp_request_new(sequence_continue_cb, seq_ctx);
  if (!req)
    goto error;

  ret = request_headers_add(req, session, cur_request->rtsp_type);
  if (ret < 0)
    goto error;

  if (cur_request->content_type)
    evrtsp_add_header(req->output_headers, "Content-Type", cur_request->content_type);

  if (cur_request->payload_make)
    {
      ret = cur_request->payload_make(req, session, seq_ctx->payload_make_arg);
      if (ret > 0) // Skip to next request in sequence, if none -> error
        {
	  seq_ctx->cur_request++;
	  if (!seq_ctx->cur_request->name)
	    {
	      DPRINTF(E_LOG, L_AIRPLAY, "Bug! payload_make signaled skip request, but there is nothing to skip to\n");
	      goto error;
	    }

	  evrtsp_request_free(req);
	  sequence_continue(seq_ctx);
	  return;
        }
      else if (ret < 0)
	goto error;
    }

  uri = (cur_request->uri) ? cur_request->uri : session->session_url;

  DPRINTF(E_DBG, L_AIRPLAY, "%s: Sending %s to '%s'\n", seq_ctx->log_caller, cur_request->name, session->devname);

  ret = evrtsp_make_request(session->ctrl, req, cur_request->rtsp_type, uri);
  if (ret < 0)
    goto error;

  evrtsp_connection_set_closecb(session->ctrl, NULL, NULL);

  session->reqs_in_flight++;

  return;

 error:
  DPRINTF(E_LOG, L_AIRPLAY, "%s: Error sending %s to '%s'\n", seq_ctx->log_caller, cur_request->name, session->devname);

  if (req)
    evrtsp_request_free(req);

  // Sets status to FAILED, gives status to player and frees session. Must be
  // deferred, otherwise sequence_start() could invalidate the session, meaning
  // any dereference of the session by the caller after sequence_start() would
  // segfault.
  deferred_session_failure(session);

  free(seq_ctx);
}

// All errors that may occur during a sequence are called back async
static void
sequence_start(enum airplay_seq_type seq_type, struct airplay_session *session, void *arg, const char *log_caller)
{
  struct airplay_seq_ctx *seq_ctx;
  if (seq_type == AIRPLAY_SEQ_PAIR_SETUP || seq_type == AIRPLAY_SEQ_PAIR_VERIFY ||
      seq_type == AIRPLAY_SEQ_PAIR_TRANSIENT) protocol_authenticating();
  if (seq_type == AIRPLAY_SEQ_PAIR_SETUP || seq_type == AIRPLAY_SEQ_PAIR_VERIFY ||
      seq_type == AIRPLAY_SEQ_PAIR_TRANSIENT || seq_type == AIRPLAY_SEQ_PIN_START) protocol_step_changed(SENDER_STEP_PAIRING);
  else if (seq_type == AIRPLAY_SEQ_START_PLAYBACK) protocol_step_changed(SENDER_STEP_SETTING_UP);

  seq_ctx = calloc(1, sizeof(struct airplay_seq_ctx));
  if (!seq_ctx) {
    DPRINTF(E_LOG, L_AIRPLAY, "RTSP sequence allocation failed\n");
    deferred_session_failure(session);
    return;
  }

  seq_ctx->session = session;
  seq_ctx->cur_request = &airplay_seq_request[seq_type][0]; // First step of the sequence
  seq_ctx->on_success = airplay_seq_definition[seq_type].on_success;
  seq_ctx->on_error = airplay_seq_definition[seq_type].on_error;
  seq_ctx->payload_make_arg = arg;
  seq_ctx->log_caller = log_caller;

  sequence_continue(seq_ctx); // Ownership transferred
}


static int
airplay_device_start(struct protocol_peer *device, int callback_id)
{
  struct airplay_session *session;

  session = session_make(device, callback_id);
  if (!session)
    return -1;

  sequence_start(AIRPLAY_SEQ_START, session, NULL, "device_start");

  return 1;
}

static int
airplay_device_stop(struct protocol_peer *device, int callback_id)
{
  struct airplay_session *session = device->session;

  session->callback_id = callback_id;

  sequence_start(AIRPLAY_SEQ_STOP, session, NULL, "device_stop");

  return 1;
}

static int
airplay_device_flush(struct protocol_peer *device, int callback_id)
{
  struct airplay_session *session = device->session;

  if (session->state == AIRPLAY_STATE_CONNECTED && session->master_session) {
    /* A partial first packet is local only, but must not survive a new source
     * timeline even though no receiver-side FLUSH is required yet. */
    evbuffer_drain(session->master_session->input_buffer, -1);
    session->master_session->input_buffer_samples = 0;
    session->master_session->silent_samples = 0;
    session->master_session->stamp_valid = false;
    protocol_statistic(PROTOCOL_PENDING, 0);
    return 0;
  }
  if (session->state != AIRPLAY_STATE_STREAMING)
    return 0; // No-op, nothing to flush
  if (session->reqs_in_flight)
    return 2;

  session->callback_id = callback_id;

  if (session->master_session && session->master_session->rtp_session) {
    struct timespec now;
    if (platform_time(PLATFORM_MONOTONIC, &now) == 0)
      flush_rtp_timeline(session->master_session, now);
    /* RTP time covers the idle clock gap. Sequence numbers keep counting
     * packets within the same stream, including after FLUSH. */
  }
  evbuffer_drain(session->master_session->input_buffer, -1);
  session->master_session->input_buffer_samples = 0;
  session->master_session->silent_samples = 0;
  protocol_statistic(PROTOCOL_PENDING, 0);
  rtp_session_flush(session->master_session->rtp_session);
  sequence_start(AIRPLAY_SEQ_FLUSH, session, NULL, "flush");

  return 1;
}

static int airplay_write(struct output_buffer *obuf) {
  struct airplay_master_session *ams;
  struct airplay_session *session;
  int i;

  for (ams = airplay_master_sessions; ams; ams = ams->next)
    {
      bool packet_sent = false;
      for (i = 0; obuf->data[i].buffer; i++)
	{
	  if (!quality_is_equal(&obuf->data[i].quality, &ams->rtp_session->quality))
	    continue;

	  // Set ams->cur_stamp, which involves a calculation of which session
	  // rtptime corresponds to the pts we are given by the player.
	  timestamp_set(ams, obuf->pts);

	  // Sends sync packets to new sessions, and if it is sync time then also to old sessions
	  packets_sync_send(ams);

	  // TODO avoid this copy
	  if (evbuffer_add(ams->input_buffer, obuf->data[i].buffer, obuf->data[i].bufsize) < 0)
	    return -1;
	  ams->input_buffer_samples += obuf->data[i].samples;

	  // Send as many packets as we have data for (one packet requires rawbuf_size bytes)
	  while (evbuffer_get_length(ams->input_buffer) >= ams->rawbuf_size)
	    {
	      if (evbuffer_remove(ams->input_buffer, ams->rawbuf, ams->rawbuf_size) != ams->rawbuf_size)
	        return -1;
              ams->input_buffer_samples -= ams->samples_per_packet;

              bool silent = true;
              for (size_t byte = 0; byte < ams->rawbuf_size; ++byte)
                if (ams->rawbuf[byte]) {
                  silent = false;
                  break;
                }
              ams->silent_samples =
                  silent ? MIN(ams->silent_samples + ams->samples_per_packet,
                               ams->quality.sample_rate * 2u)
                         : 0;

              if (packets_send(ams) < 0) return -1;
              packet_sent = true;
          }
          protocol_statistic(PROTOCOL_PENDING, ams->input_buffer_samples);
      }
      /* A PCM write may contain less than one RTP packet. Keep the start
       * marker until this master actually emits its first complete packet. */
      if (!packet_sent) continue;
      for (session = airplay_sessions; session; session = session->next) {
        if (session->master_session != ams ||
            session->state != AIRPLAY_STATE_CONNECTED)
          continue;
        if (!event_pending(keep_alive_timer, EV_TIMEOUT, NULL) &&
            evtimer_add(keep_alive_timer, &keep_alive_interval) < 0)
          return -1;
        session->state = AIRPLAY_STATE_STREAMING;
      }
    }

  return 0;
}

static void keep_alive_callback(platform_socket fd, short events, void *context)
{
  (void)fd; (void)events; (void)context;
  for (struct airplay_session *session = airplay_sessions; session; session = session->next)
    if (session->state & AIRPLAY_STATE_F_CONNECTED)
      sequence_start(AIRPLAY_SEQ_FEEDBACK, session, NULL, "keep-alive");
  if (airplay_sessions && evtimer_add(keep_alive_timer, &keep_alive_interval) < 0)
    for (struct airplay_session *session = airplay_sessions; session; session = session->next)
      deferred_session_failure(session);
}

int protocol_start(struct event_base *base, struct protocol_peer *peer, uint64_t identity, const char *client, const char *agent)
{
  static struct airplay_extra extra;
  memset(&extra, 0, sizeof(extra));
  extra.use_ptp = true;
  extra.supports_encryption = true;
  extra.supports_auth_setup = (peer->features & (UINT64_C(1) << 26)) != 0;
  peer->extra_device_info = &extra;
  evbase_player = base;
  keep_alive_timer = evtimer_new(base, keep_alive_callback, NULL);
  if (!keep_alive_timer) return -1;
  airplay_device_id = identity;
  airplay_client_name = client;
  airplay_user_agent = agent;
  if (uuid_make(airplay_ptp_clock_uuid) < 0) return -1;
  if (ptpd_find_or_bind() < 0 || ptpd_init(identity) < 0)
    return -1;
  if (service_start(&airplay_control_svc, control_svc_cb, 0, "AirPlay control") < 0)
    return -1;
  if (airplay_events_init(base) < 0)
    return -1;
  return airplay_device_start(peer, 0) < 0 ? -1 : 0;
}

int protocol_write(const void *pcm, size_t bytes, struct timespec pts)
{
  struct output_buffer buffer = {0};
  buffer.pts = pts;
  buffer.data[0].buffer = (void *)pcm;
  buffer.data[0].bufsize = bytes;
  buffer.data[0].samples = bytes / 4;
  buffer.data[0].quality = airplay_quality_default;
  if (airplay_write(&buffer) < 0) return -1;
  return airplay_sessions && !(airplay_sessions->state & AIRPLAY_STATE_F_FAILED) ? 0 : -1;
}

int protocol_flush(struct protocol_peer *peer)
{
  return airplay_device_flush(peer, 0);
}

struct receiver_volume_query {
  struct airplay_session *session;
  int timeout;
};
static void receiver_volume_response(struct evrtsp_request *request,
                                     void *context) {
  struct receiver_volume_query *query = context;
  struct airplay_session *session = query->session;
  --session->reqs_in_flight;
  session->volume_query_pending = false;
  session->ctrl->timeout = query->timeout;
  if (request && request->response_code == RTSP_OK) {
    size_t length = evbuffer_get_length(request->input_buffer);
    if (length && length < 128) {
      char text[128] = {0};
      evbuffer_copyout(request->input_buffer, text, length);
      double db;
      if (sscanf(text, "volume: %lf", &db) == 1) protocol_receiver_volume(db);
    }
  } else {
    session->volume_query_unsupported = true;
  }
  if (!session->reqs_in_flight)
    evrtsp_connection_set_closecb(session->ctrl, rtsp_close_cb, session);
  free(query);
  if (!request) deferred_session_failure(session);
}
int protocol_refresh_volume(struct protocol_peer *peer) {
  struct airplay_session *session = peer ? peer->session : NULL;
  struct timespec now;
  if (!session || session->volume_query_unsupported ||
      session->reqs_in_flight || session->volume_query_pending ||
      !(session->state & AIRPLAY_STATE_F_CONNECTED) ||
      platform_time(PLATFORM_MONOTONIC, &now) < 0 ||
      now.tv_sec < session->next_volume_query)
    return 1;
  struct receiver_volume_query *query = calloc(1, sizeof(*query));
  if (!query) return -1;
  query->session = session;
  query->timeout = session->ctrl->timeout;
  struct evrtsp_request *request =
      evrtsp_request_new(receiver_volume_response, query);
  if (!request ||
      request_headers_add(request, session, EVRTSP_REQ_GET_PARAMETER) < 0 ||
      evrtsp_add_header(request->output_headers, "Content-Type",
                        "text/parameters") < 0 ||
      evbuffer_add(request->output_buffer, "volume\r\n", 8) < 0)
    goto fail;
  session->ctrl->timeout = 2;
  if (evrtsp_make_request(session->ctrl, request, EVRTSP_REQ_GET_PARAMETER,
                          session->session_url) < 0)
    goto fail;
  session->volume_query_pending = true;
  session->next_volume_query = now.tv_sec + 1;
  ++session->reqs_in_flight;
  evrtsp_connection_set_closecb(session->ctrl, NULL, NULL);
  return 0;
fail:
  session->ctrl->timeout = query->timeout;
  if (request) evrtsp_request_free(request);
  free(query);
  return -1;
}
struct now_playing_request {
  struct airplay_session *session;
  int timeout;
  size_t artwork_size;
  bool artwork;
  char type[11];
  unsigned char bytes[];
};
static int now_playing_artwork(struct now_playing_request *);
static void now_playing_response(struct evrtsp_request *request,
                                 void *context) {
  struct now_playing_request *pending = context;
  struct airplay_session *session = pending->session;
  --session->reqs_in_flight;
  if (request && request->response_code == RTSP_OK && !pending->artwork) {
    pending->artwork = true;
    if (now_playing_artwork(pending) == 0) return;
    protocol_metadata_completed(SENDER_PLATFORM, 0);
  } else {
    protocol_metadata_completed(request && request->response_code == RTSP_OK
                                    ? SENDER_OK
                                    : SENDER_NETWORK,
                                request ? request->response_code : 0);
  }
  session->metadata_pending = false;
  session->ctrl->timeout = pending->timeout;
  if (!session->reqs_in_flight)
    evrtsp_connection_set_closecb(session->ctrl, rtsp_close_cb, session);
  free(pending);
  if (!request) deferred_session_failure(session);
}
static int now_playing_send(struct now_playing_request *pending,
                            struct evbuffer *body, const char *type) {
  struct airplay_session *session = pending->session;
  if (!pending->artwork) pending->timeout = session->ctrl->timeout;
  struct evrtsp_request *request =
      evrtsp_request_new(now_playing_response, pending);
  char info[64];
  snprintf(info, sizeof(info), "rtptime=%" PRIu32,
           session->master_session->rtp_session->pos);
  if (!request ||
      request_headers_add(request, session, EVRTSP_REQ_SET_PARAMETER) < 0 ||
      evrtsp_add_header(request->output_headers, "Content-Type", type) < 0 ||
      evrtsp_add_header(request->output_headers, "RTP-Info", info) < 0 ||
      evbuffer_add_buffer(request->output_buffer, body) < 0)
    goto fail;
  session->ctrl->timeout = 2;
  if (evrtsp_make_request(session->ctrl, request, EVRTSP_REQ_SET_PARAMETER,
                          session->session_url) < 0)
    goto fail;
  ++session->reqs_in_flight;
  evrtsp_connection_set_closecb(session->ctrl, NULL, NULL);
  return 0;
fail:
  session->ctrl->timeout = pending->timeout;
  if (request) evrtsp_request_free(request);
  return -1;
}
static int now_playing_artwork(struct now_playing_request *pending) {
  struct evbuffer *body = evbuffer_new();
  if (!body) return -1;
  int result = evbuffer_add(body, pending->bytes, pending->artwork_size) < 0
                   ? -1
                   : now_playing_send(pending, body, pending->type);
  evbuffer_free(body);
  return result;
}
static int metadata_tag(struct evbuffer *body, const char *tag,
                        const char *text) {
  uint32_t size = htonl((uint32_t)strlen(text));
  return evbuffer_add(body, tag, 4) < 0 || evbuffer_add(body, &size, 4) < 0 ||
                 evbuffer_add(body, text, strlen(text)) < 0
             ? -1
             : 0;
}
int protocol_now_playing(struct protocol_peer *peer, const char *title,
                         const char *artist, const char *album,
                         const char *type, const void *artwork, size_t bytes) {
  struct airplay_session *session = peer ? peer->session : NULL;
  if (!session || !(session->state & AIRPLAY_STATE_F_CONNECTED)) return -1;
  if (session->reqs_in_flight || session->metadata_pending) return 1;
  struct now_playing_request *pending = calloc(1, sizeof(*pending) + bytes);
  struct evbuffer *body = evbuffer_new();
  if (!pending || !body) {
    free(pending);
    if (body) evbuffer_free(body);
    return -1;
  }
  pending->session = session;
  pending->artwork_size = bytes;
  snprintf(pending->type, sizeof(pending->type), "%s",
           bytes ? type : "image/jpeg");
  if (bytes) memcpy(pending->bytes, artwork, bytes);
  uint32_t size =
      htonl((uint32_t)(24 + strlen(title) + strlen(artist) + strlen(album)));
  int result = -1;
  if (evbuffer_add(body, "mlit", 4) == 0 && evbuffer_add(body, &size, 4) == 0 &&
      metadata_tag(body, "minm", title) == 0 &&
      metadata_tag(body, "asar", artist) == 0 &&
      metadata_tag(body, "asal", album) == 0)
    result = now_playing_send(pending, body, "application/x-dmap-tagged");
  evbuffer_free(body);
  if (result < 0)
    free(pending);
  else
    session->metadata_pending = true;
  return result;
}
static void volume_response(struct evrtsp_request *request, void *context) {
  struct volume_request *pending = context;
  struct airplay_session *session = pending->session;
  session->reqs_in_flight--;
  if (!session->reqs_in_flight)
    evrtsp_connection_set_closecb(session->ctrl, rtsp_close_cb, session);
  int peer_code = request ? request->response_code : 0;
  sender_result result = peer_code == RTSP_OK ? SENDER_OK : SENDER_NETWORK;
  if (!peer_code && platform_last_failure()->code == ETIMEDOUT)
    result = SENDER_TIMEOUT;
  if (result == SENDER_OK) session->volume = pending->volume;
  protocol_volume_completed(pending->command_id, result, peer_code,
                            peer_code ? 0 : platform_last_failure()->code);
  volume_request_release(session);
  if (!peer_code) deferred_session_failure(session);
}

int protocol_set_volume(struct protocol_peer *peer, uint64_t command_id,
                        uint32_t volume) {
  if (!peer->session) return -1;
  struct airplay_session *session = peer->session;
  if (session->volume_request) return -1;
  if (session->reqs_in_flight) return 1;
  struct volume_request *pending = calloc(1, sizeof(*pending));
  if (!pending) return -1;
  pending->session = session;
  pending->command_id = command_id;
  pending->volume = (int)volume;
  session->volume_request = pending;
  struct evrtsp_request *request = evrtsp_request_new(volume_response, pending);
  if (!request ||
      request_headers_add(request, session, EVRTSP_REQ_SET_PARAMETER) < 0)
    goto error;
  if (evrtsp_add_header(request->output_headers, "Content-Type",
                        "text/parameters") < 0 ||
      payload_make_set_volume(request, session, &pending->volume) < 0)
    goto error;
  if (evrtsp_make_request(session->ctrl, request, EVRTSP_REQ_SET_PARAMETER,
                          session->session_url) < 0)
    goto error;
  evrtsp_connection_set_closecb(session->ctrl, NULL, NULL);
  session->reqs_in_flight++;
  return 0;
error:
  if (request) evrtsp_request_free(request);
  volume_request_release(session);
  return -1;
}

static void presentation_response(struct evrtsp_request *, void *);
static int presentation_send(struct presentation_request *pending) {
  struct airplay_session *session = pending->session;
  enum evrtsp_cmd_type method =
      pending->setup ? EVRTSP_REQ_SETUP : EVRTSP_REQ_FLUSH;
  struct evrtsp_request *request =
      evrtsp_request_new(presentation_response, pending);
  if (!request || request_headers_add(request, session, method) < 0) goto error;
  if (pending->setup) {
    if (evrtsp_add_header(request->output_headers, "Content-Type",
                          "application/x-apple-binary-plist") < 0 ||
        payload_make_setup_stream(request, session, &pending->candidate) < 0)
      goto error;
  } else if (payload_make_flush(request, session, NULL) < 0)
    goto error;
  if (evrtsp_make_request(session->ctrl, request, method,
                          session->session_url) < 0)
    goto error;
  evrtsp_connection_set_closecb(session->ctrl, NULL, NULL);
  ++session->reqs_in_flight;
  return 0;
error:
  if (request) evrtsp_request_free(request);
  return -1;
}
static sender_result presentation_accept(struct presentation_request *pending,
                                         struct evrtsp_request *request) {
  struct airplay_session *session = pending->session;
  plist_t response = NULL;
  if (wplist_from_evbuf(&response, request->input_buffer) < 0)
    return SENDER_CAPABILITY;
  plist_t stream =
      plist_array_get_item(plist_dict_get_item(response, "streams"), 0);
  if (!receiver_latency_accept(session, stream)) {
    plist_free(response);
    return SENDER_CAPABILITY;
  }
  uint64_t data = 0, control = 0;
  plist_t data_node = stream ? plist_dict_get_item(stream, "dataPort") : NULL;
  plist_t control_node =
      stream ? plist_dict_get_item(stream, "controlPort") : NULL;
  if (data_node && plist_get_node_type(data_node) == PLIST_UINT)
    plist_get_uint_val(data_node, &data);
  if (control_node && plist_get_node_type(control_node) == PLIST_UINT)
    plist_get_uint_val(control_node, &control);
  plist_free(response);
  if (!data || data > UINT16_MAX || !control || control > UINT16_MAX)
    return SENDER_CAPABILITY;
  platform_socket socket = net_connect(session->address, (unsigned short)data,
                                       SOCK_DGRAM, "AirPlay presentation data");
  if (socket < 0) return SENDER_NETWORK;
  if (session->server_fd >= 0) platform_socket_close(session->server_fd);
  session->server_fd = socket;
  session->data_port = (unsigned short)data;
  session->control_port = (unsigned short)control;
  struct protocol_peer *peer = protocol_peer_get(session->device_id);
  if (!peer) return SENDER_STATE;
  peer->presentation_base_ms = pending->candidate.base_ms;
  peer->offset_ms = pending->candidate.offset_ms;
  session->offset_samples = peer->offset_ms * 44100 / 1000;
  uint32_t render_ms = receiver_latency_ms(peer->buffers.render_ms,
                                           peer->presentation_base_ms,
                                           peer->offset_ms);
  session->master_session->rtp_session->render_samples = render_ms * 44100 / 1000;
  session->master_session->output_buffer_samples =
      (uint32_t)(peer->presentation_base_ms * 44100 / 1000) -
      session->master_session->rtp_session->render_samples;
  protocol_timing_applied(
      session->master_session->output_buffer_samples,
      session->master_session->rtp_session->render_samples,
      receiver_window_samples(
          (uint32_t)((peer->presentation_base_ms + peer->offset_ms) * 44100 /
                     1000)));
  return SENDER_OK;
}
static void presentation_response(struct evrtsp_request *request,
                                  void *context) {
  struct presentation_request *pending = context;
  struct airplay_session *session = pending->session;
  --session->reqs_in_flight;
  int peer_code = request ? request->response_code : 0;
  sender_result result = peer_code == RTSP_OK ? SENDER_OK : SENDER_NETWORK;
  if (result == SENDER_OK && !pending->setup) {
    evbuffer_drain(session->master_session->input_buffer, -1);
    evbuffer_drain(session->master_session->encoded_buffer, -1);
    session->master_session->input_buffer_samples = 0;
    session->master_session->silent_samples = 0;
    protocol_statistic(PROTOCOL_PENDING, 0);
    rtp_session_flush(session->master_session->rtp_session);
    pending->setup = true;
    if (presentation_send(pending) == 0) return;
    result = SENDER_NETWORK;
  } else if (result == SENDER_OK)
    result = presentation_accept(pending, request);
  session->state = AIRPLAY_STATE_CONNECTED;
  if (!session->reqs_in_flight)
    evrtsp_connection_set_closecb(session->ctrl, rtsp_close_cb, session);
  uint64_t command_id = pending->command_id;
  presentation_request_release(session);
  protocol_presentation_completed(
      command_id, result, peer_code,
      peer_code ? 0 : platform_last_failure()->code);
  if (!peer_code || (peer_code == RTSP_OK && result != SENDER_OK))
    deferred_session_failure(session);
}
int protocol_retarget_presentation(struct protocol_peer *peer,
                                   const sender_presentation *value) {
  struct airplay_session *session = peer ? peer->session : NULL;
  if (!value || !session || !session->master_session ||
      !session->master_session->rtp_session || session->presentation_request ||
      !(session->state & AIRPLAY_STATE_F_CONNECTED))
    return -1;
  uint32_t render = session->master_session->rtp_session->render_samples;
  uint32_t base_samples = (uint32_t)value->base_ms * 44100 / 1000;
  if (base_samples < render) return -1;
  int64_t old_lead = (int64_t)peer->presentation_base_ms + peer->offset_ms;
  int64_t new_lead = (int64_t)value->base_ms + value->offset_ms;
  /* Before the first PCM timestamp there is no scheduled audio to consume.
   * This also lets the caller apply receiver-reported timing before capture. */
  if (new_lead < old_lead && session->master_session->stamp_valid) {
    /* Earlier playout consumes silence, never voiced samples. Keep enough
     * quiet PCM for the render reserve and the next packet boundary. */
    struct airplay_master_session *master = session->master_session;
    uint64_t quiet_needed = (uint64_t)(old_lead - new_lead) * 44100 / 1000 +
                            render +
                            (uint64_t)peer->buffers.safety_ms * 44100 / 1000 +
                            master->samples_per_packet;
    if (master->silent_samples < quiet_needed) return -1;
    size_t pending = evbuffer_get_length(master->input_buffer);
    const uint8_t *pcm =
        pending ? evbuffer_pullup(master->input_buffer, -1) : NULL;
    if (pending && !pcm) return -1;
    for (size_t byte = 0; byte < pending; ++byte)
      if (pcm[byte]) return -1;
    master->silent_samples -= (uint32_t)((old_lead - new_lead) * 44100 / 1000);
  }
  peer->presentation_base_ms = value->base_ms;
  peer->offset_ms = value->offset_ms;
  session->offset_samples = value->offset_ms * 44100 / 1000;
  session->master_session->output_buffer_samples = base_samples - render;
  return (int)session->master_session->output_buffer_samples;
}
int protocol_set_presentation(struct protocol_peer *peer, uint64_t command_id,
                              const sender_presentation *value) {
  struct airplay_session *session = peer->session;
  if (!session || session->presentation_request ||
      !(session->state & AIRPLAY_STATE_F_CONNECTED))
    return -1;
  struct presentation_request *pending = calloc(1, sizeof(*pending));
  if (!pending) return -1;
  pending->session = session;
  pending->command_id = command_id;
  pending->candidate = *value;
  session->presentation_request = pending;
  if (presentation_send(pending) == 0) return 0;
  presentation_request_release(session);
  return -1;
}
void protocol_cancel_presentation(struct protocol_peer *peer) {
  struct airplay_session *session = peer->session;
  if (session && session->presentation_request) {
    presentation_request_release(session);
    deferred_session_failure(session);
  }
}
int protocol_end(struct protocol_peer *peer) {
  if (!peer->session) return 0;
  struct airplay_session *session = peer->session;
  if (!(session->state & AIRPLAY_STATE_F_CONNECTED)) {
    session_cleanup(session);
    return 0;
  }
  return airplay_device_stop(peer, 0);
}

void protocol_cleanup(void) {
  if (keep_alive_timer) {
    event_free(keep_alive_timer);
    keep_alive_timer = NULL;
  }
  while (airplay_sessions) session_cleanup(airplay_sessions);
  airplay_events_deinit();
  service_stop(&airplay_control_svc);
  ptpd_deinit();
  evbase_player = NULL;
}
