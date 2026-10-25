#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <fcntl.h>
#include "platform.h"
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef FROZEN_SESSION
#include "outputs/airplay.c"
struct event_base *evbase_player;
#else
#include "session.c"
#endif
#ifdef TRACK_CIPHER_OUTPUT
static void *cipher_output;
static unsigned cipher_allocations, cipher_releases;
static struct evbuffer *reject_cipher_output;
static bool reject_allocation;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) {
  if (reject_allocation) {
    reject_allocation = false;
    return NULL;
  }
  return __real_malloc(size);
}
void __real_plist_to_bin(plist_t, char **, uint32_t *);
void __wrap_plist_to_bin(plist_t node, char **output, uint32_t *length) {
  assert(!cipher_output);
  __real_plist_to_bin(node, output, length);
  if (*output) {
    cipher_output = *output;
    ++cipher_allocations;
  }
}
ssize_t __real_pair_encrypt(uint8_t **, size_t *, const uint8_t *, size_t,
                            struct pair_cipher_context *);
ssize_t __real_pair_decrypt(uint8_t **, size_t *, const uint8_t *, size_t,
                            struct pair_cipher_context *);
void __real_free(void *);
int __real_evbuffer_add(struct evbuffer *, const void *, size_t);
ssize_t __wrap_pair_encrypt(uint8_t **output, size_t *length,
                            const uint8_t *input, size_t bytes,
                            struct pair_cipher_context *context) {
  assert(!cipher_output);
  ssize_t result = __real_pair_encrypt(output, length, input, bytes, context);
  if (result >= 0 && *output) {
    cipher_output = *output;
    ++cipher_allocations;
  }
  return result;
}
ssize_t __wrap_pair_decrypt(uint8_t **output, size_t *length,
                            const uint8_t *input, size_t bytes,
                            struct pair_cipher_context *context) {
  assert(!cipher_output);
  ssize_t result = __real_pair_decrypt(output, length, input, bytes, context);
  if (result >= 0 && *output) {
    cipher_output = *output;
    ++cipher_allocations;
  }
  return result;
}
void __wrap_free(void *pointer) {
  if (pointer && pointer == cipher_output) {
    cipher_output = NULL;
    ++cipher_releases;
  }
  __real_free(pointer);
}
int __wrap_evbuffer_add(struct evbuffer *buffer, const void *data,
                        size_t size) {
  if (buffer == reject_cipher_output) {
    reject_cipher_output = NULL;
    return -1;
  }
  return __real_evbuffer_add(buffer, data, size);
}
static void verify_control_cipher_buffers(void) {
  struct rtp_packet packet = {.data_len = 32};
  struct airplay_session packet_session = {0};
  uint8_t *packet_bytes = NULL;
  size_t packet_length = 0;
  reject_allocation = true;
  assert(packet_encrypt(&packet_bytes, &packet_length, &packet,
                        &packet_session) < 0);
  assert(!packet_bytes && !packet_length && !reject_allocation);
  struct evrtsp_request *request = evrtsp_request_new(NULL, NULL);
  assert(request);
  struct airplay_session plist_session = {0};
  for (unsigned i = 0; i < 1000; ++i) {
    assert(!payload_make_teardown(request, &plist_session, NULL));
    assert(!cipher_output);
    evbuffer_drain(request->output_buffer,
                   evbuffer_get_length(request->output_buffer));
  }
  reject_cipher_output = request->output_buffer;
  assert(payload_make_teardown(request, &plist_session, NULL) < 0);
  assert(!cipher_output && !evbuffer_get_length(request->output_buffer));
  evrtsp_request_free(request);
  uint8_t secret[32] = {0}, message[] = {1, 2, 3, 4};
  struct airplay_session sender = {0}, receiver = {0};
  sender.control_cipher_ctx =
      pair_cipher_new(PAIR_CLIENT_HOMEKIT_NORMAL, PAIR_CHANNEL_CONTROL, secret,
                      sizeof(secret), NULL);
  receiver.control_cipher_ctx = pair_cipher_new(
      PAIR_SERVER_HOMEKIT, PAIR_CHANNEL_CONTROL, secret, sizeof(secret), NULL);
  assert(sender.control_cipher_ctx && receiver.control_cipher_ctx);
  struct evbuffer *plain = evbuffer_new(), *wire = evbuffer_new(),
                  *partial = evbuffer_new(), *decoded = evbuffer_new();
  assert(plain && wire && partial && decoded);
  for (unsigned i = 0; i < 1000; ++i) {
    assert(!evbuffer_add(plain, message, sizeof(message)));
    assert(!rtsp_cipher(wire, plain, &sender, 1));
    assert(!cipher_output && !evbuffer_get_length(plain));
    assert(!rtsp_cipher(decoded, wire, &receiver, 0));
    assert(!cipher_output && !evbuffer_get_length(wire));
    uint8_t result[sizeof(message)];
    assert(evbuffer_remove(decoded, result, sizeof(result)) == sizeof(result));
    assert(!memcmp(result, message, sizeof(message)));
  }
  assert(!evbuffer_add(plain, message, sizeof(message)));
  reject_cipher_output = wire;
  assert(rtsp_cipher(wire, plain, &sender, 1) < 0);
  assert(!cipher_output && evbuffer_get_length(plain) == sizeof(message));
  assert(!rtsp_cipher(wire, plain, &sender, 1));
  size_t wire_length = evbuffer_get_length(wire);
  assert(evbuffer_remove_buffer(wire, partial, 1) == 1);
  assert(!rtsp_cipher(decoded, partial, &receiver, 0));
  assert(!cipher_output && !evbuffer_get_length(decoded));
  assert(evbuffer_remove_buffer(wire, partial, wire_length - 2) ==
         (ssize_t)wire_length - 2);
  for (unsigned i = 0; i < 1000; ++i) {
    assert(!rtsp_cipher(decoded, partial, &receiver, 0));
    assert(!cipher_output && !evbuffer_get_length(decoded));
  }
  assert(evbuffer_remove_buffer(wire, partial, 1) == 1);
  reject_cipher_output = decoded;
  assert(rtsp_cipher(decoded, partial, &receiver, 0) < 0);
  assert(!cipher_output && evbuffer_get_length(partial) == wire_length);
  assert(!rtsp_cipher(decoded, partial, &receiver, 0));
  assert(!cipher_output && !evbuffer_get_length(partial));
  assert(evbuffer_get_length(decoded) == sizeof(message));
  evbuffer_drain(decoded, sizeof(message));
  assert(!evbuffer_add(plain, message, sizeof(message)));
  assert(!rtsp_cipher(wire, plain, &sender, 1));
  uint8_t *corrupt = evbuffer_pullup(wire, -1);
  corrupt[evbuffer_get_length(wire) - 1] ^= 1;
  assert(rtsp_cipher(decoded, wire, &receiver, 0) < 0);
  assert(!cipher_output && !evbuffer_get_length(decoded));
  assert(cipher_allocations == cipher_releases && cipher_allocations >= 3000);
  evbuffer_free(plain);
  evbuffer_free(wire);
  evbuffer_free(partial);
  evbuffer_free(decoded);
  pair_cipher_free(sender.control_cipher_ctx);
  pair_cipher_free(receiver.control_cipher_ctx);
  puts(
      "Production control cipher round-trips, fragmented input, append "
      "failures and buffer releases verified");
}
#endif
static enum output_device_state observed_state;
static unsigned notifications, removed;
#ifdef FROZEN_SESSION
void outputs_cb(int callback_id, uint64_t id, enum output_device_state state) {
  (void)callback_id;
  assert(id == 7);
  observed_state = state;
  ++notifications;
}
void outputs_device_session_remove(uint64_t id) {
  assert(id == 7);
  ++removed;
}
void outputs_quality_unsubscribe(struct media_quality *quality) {
  (void)quality;
}
#else
static unsigned feedback_responses;
void protocol_diagnostic(const char *format, ...) { (void)format; }
void protocol_receiver_volume(double db) { (void)db; }
void protocol_receiver_media(uint32_t command) { (void)command; assert(0); }
void protocol_metadata_completed(sender_result result, int peer_code) { (void)result; (void)peer_code; assert(0); }
void protocol_statistic(enum protocol_statistic event, uint64_t value) {
  (void)event;
  (void)value;
}
void protocol_operation(const char *name, int code) {
  assert(!strcmp(name, "FLUSH") || !strcmp(name, "POST /feedback"));
  assert(code == 0 || code == 200 || (code == 500 && !strcmp(name, "FLUSH")));
  if (!strcmp(name, "POST /feedback") && code == 200) {
    ++feedback_responses;
    event_base_loopbreak(evbase_player);
  }
  if (!strcmp(name, "FLUSH") && code == 500)
    event_base_loopbreak(evbase_player);
}
void protocol_authenticating(void) { assert(0); }
void protocol_step_changed(sender_step step) { (void)step;assert(0); }
void protocol_timing_applied(uint32_t buffer,uint32_t minimum,uint32_t maximum) { (void)buffer;(void)minimum;(void)maximum;assert(0); }
void protocol_timing_sync(uint32_t position,uint32_t minimum,uint32_t next,uint32_t pending,uint64_t capture) { (void)position;(void)minimum;(void)next;(void)pending;(void)capture;assert(0); }
void protocol_end_error(int code) { (void)code; assert(0); }
void protocol_credentials_changed(const char *material) { (void)material; assert(0); }
void protocol_volume_completed(uint64_t command_id, sender_result result,
                               int peer_code, int platform_code) {
  (void)command_id;
  (void)result;
  (void)peer_code;
  (void)platform_code;
  assert(0);
}
void protocol_presentation_completed(uint64_t command_id, sender_result result,
                                     int peer_code, int platform_code) {
  (void)command_id; (void)result; (void)peer_code; (void)platform_code; assert(0);
}
struct protocol_peer *protocol_peer_get(uint64_t id) {
  (void)id;
  assert(0);
  return NULL;
}
void protocol_peer_session_add(uint64_t id,void *session) { (void)id; (void)session; assert(0); }
void protocol_state_changed(int callback_id,uint64_t id,enum output_device_state state)
{ (void)callback_id; assert(id==7); observed_state=state; ++notifications; }
void protocol_peer_session_remove(uint64_t id) { assert(id==7); ++removed; }
#endif
static platform_socket listener;
#ifndef FROZEN_SESSION
static void *feedback_peer(void *context)
{
    (void)context;
    platform_socket fd=accept(listener,NULL,NULL); assert(fd>=0);
    char request[4096]={0}; size_t length=0;
    while(!strstr(request,"\r\n\r\n")) {
        assert(length<sizeof(request)-1);
        ev_ssize_t received=platform_recv(fd,request+length,sizeof(request)-1-length,0); assert(received>0);
        length+=(size_t)received; request[length]=0;
    }
    assert(!strncmp(request,"POST /feedback RTSP/1.0\r\n",25));
    const char response[]="RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
    assert(platform_send(fd,response,sizeof(response)-1,0)==(ev_ssize_t)sizeof(response)-1);
    char finished; assert(platform_recv(fd,&finished,1,0)==0);
    assert(!platform_socket_close(fd)); return NULL;
}
static void *reject_flush_peer(void *context)
{
    (void)context;
    platform_socket fd=accept(listener,NULL,NULL); assert(fd>=0);
    char request[4096]={0}; ssize_t length=platform_recv(fd,request,sizeof(request)-1,0);
    assert(length>0 && !strncmp(request,"FLUSH ",6));
    const char reply[]="RTSP/1.0 500 Internal Server Error\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
    assert(platform_send(fd,reply,sizeof(reply)-1,0)==(ssize_t)sizeof(reply)-1);
    char extra; assert(platform_recv(fd,&extra,1,0)==0);
    assert(!platform_socket_close(fd)); return NULL;
}
static void verify_rejected_flush(void)
{
    evbase_player=event_base_new(); assert(evbase_player);
    struct media_quality quality={44100,16,2,0};
    struct airplay_master_session *master=calloc(1,sizeof(*master)); assert(master);
    master->rtp_session=rtp_session_new(&quality,8,0,1); assert(master->rtp_session);
    master->input_buffer=evbuffer_new(); master->encoded_buffer=evbuffer_new();
    airplay_master_sessions=master;
    struct airplay_session *session=calloc(1,sizeof(*session)); assert(session);
    session->device_id=7; session->master_session=master; session->devname=strdup("Rejected flush");
    session->state=AIRPLAY_STATE_STREAMING; session->server_fd=-1;
    session->deferredev=evtimer_new(evbase_player,deferred_session_failure_cb,session); assert(session->deferredev);
    airplay_sessions=session;
    listener=platform_socket_open(AF_INET,SOCK_STREAM,0); assert(listener>=0);
    struct sockaddr_in tcp={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(listener,(struct sockaddr *)&tcp,sizeof(tcp)) && !listen(listener,1));
    socklen_t size=sizeof(tcp); assert(!getsockname(listener,(struct sockaddr *)&tcp,&size));
    session->ctrl=evrtsp_connection_new("127.0.0.1",ntohs(tcp.sin_port)); assert(session->ctrl);
    evrtsp_connection_set_base(session->ctrl,evbase_player);
    evrtsp_connection_set_closecb(session->ctrl,rtsp_close_cb,session);
    airplay_client_name="Fault sender"; airplay_user_agent="AirPlay/999.0.0"; airplay_device_id=1;
    strcpy(session->session_url,"rtsp://127.0.0.1/1"); session->cseq=1;
    struct protocol_peer target={.session=session};
    unsigned before=notifications;
    assert(airplay_device_flush(&target,0)==1);
    platform_thread worker; assert(!platform_thread_start(&worker,reject_flush_peer,NULL));
    struct timeval deadline={2,0}; event_base_loopexit(evbase_player,&deadline);
    assert(event_base_dispatch(evbase_player)>=0);
    assert(!airplay_sessions && !airplay_master_sessions);
    assert(observed_state==OUTPUT_STATE_FAILED && notifications==before+1);
    assert(!platform_thread_join(worker));
    assert(!airplay_sessions && !airplay_master_sessions);
    assert(!platform_socket_close(listener)); event_base_free(evbase_player);
}
static void verify_keep_alive(void)
{
    evbase_player=event_base_new(); assert(evbase_player);
#ifndef FROZEN_SESSION
    {
      struct airplay_session auth = {0};
      const char *headers[] = {
        "Digest realm=\"room\" nonce=\"token\"",
        "Digest realm=\"room\" realm=\"duplicate\" nonce=\"token\"",
        "Digest realm=\"room\" nonce=\"token\" nonce=\"duplicate\""
      };
      for (unsigned i = 0; i < sizeof(headers) / sizeof(headers[0]); ++i) {
        struct evrtsp_request *request = evrtsp_request_new(NULL, NULL);
        assert(request && !evrtsp_add_header(request->input_headers, "WWW-Authenticate", headers[i]));
        assert((auth_header_parse(&auth, request) == 0) == (i == 0));
        if (i) assert(!auth.realm && !auth.nonce);
        evrtsp_request_free(request);
      }
      free(auth.realm);
      free(auth.nonce);
    }
#endif
    listener=platform_socket_open(AF_INET,SOCK_STREAM,0); assert(listener>=0);
    struct sockaddr_in tcp={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(listener,(struct sockaddr *)&tcp,sizeof(tcp)) && !listen(listener,1));
    socklen_t size=sizeof(tcp); assert(!getsockname(listener,(struct sockaddr *)&tcp,&size));
    struct airplay_session *session=calloc(1,sizeof(*session)); assert(session);
    session->device_id=7; session->server_fd=-1; session->state=AIRPLAY_STATE_STREAMING;
    session->devname=strdup("Keep-alive peer"); session->cseq=1;
    session->deferredev=evtimer_new(evbase_player,deferred_session_failure_cb,session); assert(session->deferredev);
    session->ctrl=evrtsp_connection_new("127.0.0.1",ntohs(tcp.sin_port)); assert(session->ctrl);
    evrtsp_connection_set_base(session->ctrl,evbase_player); airplay_sessions=session;
    keep_alive_timer=evtimer_new(evbase_player,keep_alive_callback,NULL); assert(keep_alive_timer);
    platform_thread worker; assert(!platform_thread_start(&worker,feedback_peer,NULL));
    keep_alive_callback(-1,EV_TIMEOUT,NULL);
    assert(event_pending(keep_alive_timer,EV_TIMEOUT,NULL));
    struct timeval timeout={2,0}; assert(!event_base_loopexit(evbase_player,&timeout));
    assert(event_base_dispatch(evbase_player)>=0 && feedback_responses==1);
    assert(session->state==AIRPLAY_STATE_STREAMING && !session->reqs_in_flight);
    event_free(keep_alive_timer); keep_alive_timer=NULL;
    session_cleanup(session); assert(!airplay_sessions);
    assert(!platform_thread_join(worker)); assert(!platform_socket_close(listener));
    event_base_free(evbase_player);
}
#endif
static void *close_peer(void *context)
{
    (void)context; platform_socket fd=accept(listener,NULL,NULL); assert(fd>=0);
    char request[4096]={0}; ssize_t length=platform_recv(fd,request,sizeof(request)-1,0); assert(length>0 && !strncmp(request,"FLUSH ",6));
    const char *rtp=strstr(request,"RTP-Info: "); assert(rtp); unsigned sequence,timestamp; assert(sscanf(rtp,"RTP-Info: seq=%u;rtptime=%u",&sequence,&timestamp)==2 && sequence==3 && timestamp==1408);
    const char reply[]="RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
    assert(platform_send(fd,reply,sizeof(reply)-1,0)==(ssize_t)sizeof(reply)-1);
    #ifdef _WIN32
    Sleep(20);
#else
    struct timespec tick={0,20000000}; nanosleep(&tick,NULL);
#endif
    assert(!platform_socket_close(fd)); return NULL;
}
static platform_socket endpoint(struct sockaddr_in *address)
{
    platform_socket fd=platform_socket_open(AF_INET,SOCK_DGRAM,0); assert(fd>=0);
    *address=(struct sockaddr_in){.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(fd,(struct sockaddr *)address,sizeof(*address)));
    socklen_t size=sizeof(*address); assert(!getsockname(fd,(struct sockaddr *)address,&size)); return fd;
}
static void receive_exact(platform_socket fd,const uint8_t *expected,size_t length)
{
    uint8_t data[128]; assert(platform_recv(fd,data,sizeof(data),0)==(ssize_t)length && !memcmp(data,expected,length));
}
static void request_resend(platform_socket control,platform_socket client,const struct sockaddr_in *address,uint16_t sequence,uint16_t count,int malformed)
{
    uint8_t request[8]={0x80,0xd5,0,1,(uint8_t)(sequence>>8),(uint8_t)sequence,(uint8_t)(count>>8),(uint8_t)count};
    if(malformed==2) request[0]=0;
    size_t length=malformed==1 ? 7 : sizeof(request);
    assert(platform_sendto(client,request,length,0,(const struct sockaddr *)address,sizeof(*address))==(ssize_t)length);
    control_svc_cb(control,EV_READ,NULL);
}
static ev_ssize_t receive_empty(platform_socket fd,void *bytes,size_t length)
{
    assert(!platform_socket_configure(fd,true));
    ev_ssize_t result=platform_recv(fd,bytes,length,0);
    assert(!platform_socket_configure(fd,false));
    return result;
}
int main(int argc,char **argv)
{
    assert(argc==1 || (argc==2 && !strcmp(argv[1],"--state-mapping")));
    assert(gcry_check_version(NULL) && !platform_init());
#ifdef TRACK_CIPHER_OUTPUT
    verify_control_cipher_buffers();
#endif
#ifndef _WIN32
    alarm(5);
#endif
    evbase_player=event_base_new(); assert(evbase_player);
    struct media_quality quality={44100,16,2,0};
    struct airplay_master_session *master=calloc(1,sizeof(*master)); assert(master);
    master->rtp_session=rtp_session_new(&quality,8,0,1); assert(master->rtp_session);
    master->input_buffer=evbuffer_new(); master->encoded_buffer=evbuffer_new();
    airplay_master_sessions=master; master->rtp_session->seqnum=65534; master->rtp_session->pos=UINT32_MAX-351;
    struct airplay_session *session=calloc(1,sizeof(*session)); assert(session);
    session->device_id=7; session->master_session=master; session->devname=strdup("Fault peer");
    session->family=AF_INET; session->naddr.sin.sin_family=AF_INET; session->naddr.sin.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    session->state=AIRPLAY_STATE_STREAMING; session->server_fd=-1;
    session->deferredev=evtimer_new(evbase_player,deferred_session_failure_cb,session); assert(session->deferredev);
    airplay_sessions=session;
    const enum airplay_state states[]={AIRPLAY_STATE_AUTH,AIRPLAY_STATE_FAILED,AIRPLAY_STATE_STOPPED,AIRPLAY_STATE_INFO,AIRPLAY_STATE_ENCRYPTED,AIRPLAY_STATE_SETUP,AIRPLAY_STATE_RECORD,AIRPLAY_STATE_CONNECTED,AIRPLAY_STATE_STREAMING,AIRPLAY_STATE_TEARDOWN,(enum airplay_state)99};
    const enum output_device_state expected[]={OUTPUT_STATE_PASSWORD,OUTPUT_STATE_FAILED,OUTPUT_STATE_STOPPED,OUTPUT_STATE_STARTUP,OUTPUT_STATE_STARTUP,OUTPUT_STATE_STARTUP,OUTPUT_STATE_STARTUP,OUTPUT_STATE_CONNECTED,OUTPUT_STATE_STREAMING,OUTPUT_STATE_STOPPED,OUTPUT_STATE_FAILED};
    for(size_t i=0;i<sizeof(states)/sizeof(states[0]);++i) { session->state=states[i]; session_status(session); assert(observed_state==expected[i]); }
    if(argc==2) { session_cleanup(session); assert(!airplay_sessions && !airplay_master_sessions && removed==1); event_base_free(evbase_player); platform_cleanup(); puts("authentication failure stopped startup connected streaming teardown and invalid-state mapping verified"); return 0; }
    session->state=AIRPLAY_STATE_STREAMING;
    struct sockaddr_in data_address,control_address,client_address;
    platform_socket receiver=endpoint(&data_address),control=endpoint(&control_address),client=endpoint(&client_address);
    session->server_fd=platform_socket_open(AF_INET,SOCK_DGRAM,0); assert(session->server_fd>=0 && !connect(session->server_fd,(struct sockaddr *)&data_address,sizeof(data_address)));
    uint8_t shared[32]; memset(shared,0x75,sizeof(shared));
    session->packet_cipher_hd=chacha_open(shared,sizeof(shared)); assert(session->packet_cipher_hd); session->supports_encryption=true;
    uint8_t saved[4][128]; size_t sizes[4]; uint16_t sequences[4];
    for(unsigned i=0;i<4;++i) {
        struct rtp_packet *packet=rtp_packet_next(master->rtp_session,32,352,96); assert(packet);
        memset(packet->payload,(int)(0x40+i),32); sequences[i]=packet->seqnum;
        uint8_t *encrypted=NULL; assert(!packet_encrypt(&encrypted,&sizes[i],packet,session)); assert(sizes[i]<=sizeof(saved[i])); memcpy(saved[i],encrypted,sizes[i]); free(encrypted); rtp_packet_commit(master->rtp_session,packet);
    }
    /* Drop a packet and deliver retained audio out of order with a duplicate. */
    assert(!packet_send(session,rtp_packet_get(master->rtp_session,sequences[2])));
    assert(!packet_send(session,rtp_packet_get(master->rtp_session,sequences[0])));
    assert(!packet_send(session,rtp_packet_get(master->rtp_session,sequences[2])));
    receive_exact(receiver,saved[2],sizes[2]); receive_exact(receiver,saved[0],sizes[0]); receive_exact(receiver,saved[2],sizes[2]);
    request_resend(control,client,&control_address,sequences[1],3,0);
    for(unsigned i=1;i<4;++i) receive_exact(receiver,saved[i],sizes[i]);
    request_resend(control,client,&control_address,sequences[1],1,0); receive_exact(receiver,saved[1],sizes[1]);
    request_resend(control,client,&control_address,sequences[1],1,1);
    request_resend(control,client,&control_address,sequences[1],1,2);
    uint8_t scratch[128]; assert(receive_empty(receiver,scratch,sizeof(scratch))<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
    struct sockaddr_in unknown_address; platform_socket unknown=endpoint(&unknown_address);
    unknown_address.sin_addr.s_addr=htonl(0x7f000002); assert(!platform_socket_close(unknown)); unknown=platform_socket_open(AF_INET,SOCK_DGRAM,0); assert(unknown>=0);
    unknown_address.sin_port=0; assert(!bind(unknown,(struct sockaddr *)&unknown_address,sizeof(unknown_address)));
    request_resend(control,unknown,&control_address,sequences[1],1,0);
    assert(receive_empty(receiver,scratch,sizeof(scratch))<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
    /* A queued datagram survives history reset; stale requests produce no audio. */
    assert(!packet_send(session,rtp_packet_get(master->rtp_session,sequences[3])));
    rtp_session_flush(master->rtp_session); receive_exact(receiver,saved[3],sizes[3]);
    request_resend(control,client,&control_address,sequences[3],1,0);
    assert(receive_empty(receiver,scratch,sizeof(scratch))<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
    listener=platform_socket_open(AF_INET,SOCK_STREAM,0); assert(listener>=0);
    struct sockaddr_in tcp={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)}; assert(!bind(listener,(struct sockaddr *)&tcp,sizeof(tcp)) && !listen(listener,1)); socklen_t size=sizeof(tcp); assert(!getsockname(listener,(struct sockaddr *)&tcp,&size));
    session->ctrl=evrtsp_connection_new("127.0.0.1",ntohs(tcp.sin_port)); assert(session->ctrl); evrtsp_connection_set_base(session->ctrl,evbase_player); evrtsp_connection_set_closecb(session->ctrl,rtsp_close_cb,session);
    struct rtp_packet *latest=rtp_packet_next(master->rtp_session,32,352,96); assert(latest); memset(latest->payload,0x55,32); rtp_packet_commit(master->rtp_session,latest);
    uint8_t *latest_bytes=NULL; size_t latest_length=0; assert(!packet_encrypt(&latest_bytes,&latest_length,latest,session));
    assert(!packet_send(session,latest));
    assert(!evbuffer_add(master->input_buffer,"abcd",4)); master->input_buffer_samples=1;
    airplay_client_name="Fault sender"; airplay_user_agent="AirPlay/999.0.0"; airplay_device_id=1; strcpy(session->session_url,"rtsp://127.0.0.1/1"); session->cseq=1;
#ifdef FROZEN_SESSION
    struct output_device target={.session=session};
#else
    struct protocol_peer target={.session=session};
#endif
    assert(airplay_device_flush(&target,0)==1);
#ifdef FROZEN_SESSION
    assert(master->rtp_session->pktbuf_len==1 && evbuffer_get_length(master->input_buffer)==4 && master->input_buffer_samples==1);
#else
    assert(!master->rtp_session->pktbuf_len && !evbuffer_get_length(master->input_buffer) && !master->input_buffer_samples);
#endif
    receive_exact(receiver,latest_bytes,latest_length);
    request_resend(control,client,&control_address,2,1,0);
#ifdef FROZEN_SESSION
    receive_exact(receiver,latest_bytes,latest_length);
#else
    assert(receive_empty(receiver,scratch,sizeof(scratch))<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
#endif
    free(latest_bytes);
    platform_socket audio_fd=session->server_fd; unsigned previous=notifications; platform_thread worker; assert(!platform_thread_start(&worker,close_peer,NULL));
    struct timeval deadline={0,200000}; event_base_loopexit(evbase_player,&deadline); assert(event_base_dispatch(evbase_player)>=0);
    assert(!platform_thread_join(worker)); assert(!airplay_sessions && !airplay_master_sessions && removed==1 && notifications==previous+2 && observed_state==OUTPUT_STATE_FAILED);
    #ifdef _WIN32
    int socket_error;
    assert(platform_socket_error(audio_fd,&socket_error)<0 && platform_last_failure()->code==WSAENOTSOCK);
#else
    assert(fcntl(audio_fd,F_GETFD)<0 && errno==EBADF);
#endif
    event_base_free(evbase_player); platform_socket_close(listener); platform_socket_close(receiver); platform_socket_close(control); platform_socket_close(client); platform_socket_close(unknown);
#ifndef FROZEN_SESSION
    verify_rejected_flush();
    verify_keep_alive();
#endif
    platform_cleanup();
    puts("encrypted audio loss reordering duplicate delivery; UDP retransmission range wrap duplicate malformed unknown-source stale rejection; production FLUSH wire boundary and recorded queue/history isolation correction; RTSP peer disconnect deferred failure state mapping socket and session cleanup verified"); return 0;
}
