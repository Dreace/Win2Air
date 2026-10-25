#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform.h"
#ifdef FROZEN_PTP
#include "src/airptp_internal.h"
#include "src/ptp_definitions.h"
#include "src/daemon.h"
#include "src/ptp_msg_handle.h"
#else
#include "../ptp/src/airptp_internal.h"
#include "../ptp/src/ptp_definitions.h"
#include "../ptp/src/daemon.h"
#include "../ptp/src/ptp_msg_handle.h"
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(struct ptp_header)==34,"ptp header wire size");
_Static_assert(sizeof(struct ptp_sync_message)==44,"sync wire size");
_Static_assert(sizeof(struct ptp_delay_req_message)==44,"delay request wire size");
_Static_assert(sizeof(struct ptp_pdelay_req_message)==54,"pdelay request wire size");
_Static_assert(sizeof(struct ptp_pdelay_resp_message)==54,"pdelay response wire size");
_Static_assert(sizeof(struct ptp_follow_up_message)==96,"follow up wire size");
_Static_assert(sizeof(struct ptp_delay_resp_message)==54,"delay response wire size");
_Static_assert(sizeof(struct ptp_pdelay_resp_follow_up_message)==54,"pdelay follow up wire size");
_Static_assert(sizeof(struct ptp_announce_message)==76,"announce wire size");
_Static_assert(sizeof(struct ptp_signaling_message)==106,"signaling wire size");
_Static_assert(sizeof(struct ptp_peer_signaling_message)==87,"peer signaling wire size");

#define TEST_CLOCK_ID UINT64_C(0xffff567890abcdef)

static struct airptp_daemon daemon;
static struct event_base *evbase;
static platform_socket event_capture,general_capture,event_tx,general_tx;
static unsigned short event_port,general_port;
static unsigned messages_logged,hexdumps;
#ifdef PTP_CAPTURE
static void ptp_trace_packet(const uint8_t *packet,size_t length);
#endif

static void counted_log(const char *fmt,...) { (void)fmt; ++messages_logged; }
static void counted_hexdump(const char *msg,uint8_t *data,size_t len) { (void)msg; (void)data; (void)len; ++hexdumps; }
static void noop_timer(evutil_socket_t fd,short what,void *arg) { (void)fd; (void)what; (void)arg; }

static uint8_t rd8(const void *base,size_t offset) { return ((const uint8_t *)base)[offset]; }
static uint16_t rd16(const void *base,size_t offset) { uint16_t value; memcpy(&value,(const uint8_t *)base+offset,2); return be16toh(value); }
static uint32_t rd32(const void *base,size_t offset) { uint32_t value; memcpy(&value,(const uint8_t *)base+offset,4); return be32toh(value); }
static uint64_t rd64(const void *base,size_t offset) { uint64_t value; memcpy(&value,(const uint8_t *)base+offset,8); return be64toh(value); }
static uint64_t to_ns(const struct timespec *moment) { return (uint64_t)moment->tv_sec*UINT64_C(1000000000)+(uint64_t)moment->tv_nsec; }
static uint64_t wire_timestamp_ns(const void *base,size_t offset)
{
    uint64_t seconds=((uint64_t)rd16(base,offset)<<32)|rd32(base,offset+2);
    return seconds*UINT64_C(1000000000)+rd32(base,offset+6);
}

static ev_ssize_t capture_recv(platform_socket fd,void *buffer,size_t length,uint64_t timeout_ms)
{
    struct timespec started,now;
    assert(platform_time(PLATFORM_MONOTONIC,&started)==0);
    for(;;) {
        ev_ssize_t received=platform_recvfrom(fd,buffer,length,0,NULL,NULL);
        if(received>=0) {
#ifdef PTP_CAPTURE
            ptp_trace_packet(buffer,(size_t)received);
#endif
            return received;
        }
        assert(platform_time(PLATFORM_MONOTONIC,&now)==0);
        if(to_ns(&now)-to_ns(&started)>timeout_ms*UINT64_C(1000000)) return -1;
    }
}

static void check_header(const void *buffer,uint8_t type,uint16_t length,uint16_t flags,uint16_t sequence,int8_t interval,uint8_t control)
{
    assert(rd8(buffer,0)==(uint8_t)(type|0x10));
    assert(rd8(buffer,1)==0x02);
    assert(rd16(buffer,2)==length);
    assert(rd8(buffer,4)==AIRPTP_DOMAIN);
    assert(rd16(buffer,6)==flags);
    assert(rd64(buffer,8)==0);
    assert(rd64(buffer,20)==TEST_CLOCK_ID);
    assert(rd8(buffer,28)==0x80 && rd8(buffer,29)==0x05);
    assert(rd16(buffer,30)==sequence);
    assert(rd8(buffer,32)==control);
    assert((int8_t)rd8(buffer,33)==interval);
}

static void check_sync_follow_up(void)
{
    uint8_t sync_wire[128],follow_wire[128];
    struct timespec before,after;
    uint64_t sequence=daemon.sync_seq;

    assert(platform_time(PLATFORM_MONOTONIC,&before)==0);
    ptp_msg_sync_send(&daemon);
    assert(platform_time(PLATFORM_MONOTONIC,&after)==0);

    assert(capture_recv(event_capture,sync_wire,sizeof sync_wire,2000)==(ev_ssize_t)sizeof(struct ptp_sync_message));
    check_header(sync_wire,PTP_MSGTYPE_SYNC,sizeof(struct ptp_sync_message),
        PTP_FLAG_UNICAST|PTP_FLAG_TIMESCALE|PTP_FLAG_TWO_STEP,sequence,AIRPTP_LOGMESSAGEINT_SYNC,0);
    assert(wire_timestamp_ns(sync_wire,offsetof(struct ptp_sync_message,originTimestamp))==0);

    assert(capture_recv(general_capture,follow_wire,sizeof follow_wire,2000)==(ev_ssize_t)sizeof(struct ptp_follow_up_message));
    check_header(follow_wire,PTP_MSGTYPE_FOLLOW_UP,sizeof(struct ptp_follow_up_message),
        PTP_FLAG_UNICAST|PTP_FLAG_TIMESCALE,sequence,AIRPTP_LOGMESSAGEINT_SYNC,0);
    uint64_t precise=wire_timestamp_ns(follow_wire,offsetof(struct ptp_follow_up_message,preciseOriginTimestamp));
    assert(precise>=to_ns(&before) && precise<=to_ns(&after));
    size_t tlv1=offsetof(struct ptp_follow_up_message,tlv_apple1),tlv2=offsetof(struct ptp_follow_up_message,tlv_apple2);
    assert(rd16(follow_wire,tlv1)==PTP_TLV_ORG_EXTENSION && rd16(follow_wire,tlv1+2)==28);
    assert(!memcmp((const uint8_t *)follow_wire+tlv1+4,"\x00\x80\xc2\x00\x00\x01",6));
    assert(rd16(follow_wire,tlv2)==PTP_TLV_ORG_EXTENSION && rd16(follow_wire,tlv2+2)==16);
    assert(!memcmp((const uint8_t *)follow_wire+tlv2+4,"\x00\x0d\x93\x00\x00\x04",6));
    assert(rd64(follow_wire,tlv2+10)==TEST_CLOCK_ID);
    assert(daemon.sync_seq==sequence+1);
}

static void check_announce(void)
{
    uint8_t wire[128];
    uint16_t sequence=daemon.announce_seq;
    ptp_msg_announce_send(&daemon);
    assert(capture_recv(general_capture,wire,sizeof wire,2000)==(ev_ssize_t)sizeof(struct ptp_announce_message));
    check_header(wire,PTP_MSGTYPE_ANNOUNCE,sizeof(struct ptp_announce_message),
        PTP_FLAG_UNICAST|PTP_FLAG_TIMESCALE,sequence,AIRPTP_LOGMESSAGEINT_ANNOUNCE,0);
    assert(wire_timestamp_ns(wire,offsetof(struct ptp_announce_message,originTimestamp))==0);
    assert(rd16(wire,offsetof(struct ptp_announce_message,currentUtcOffset))==0);
    assert(((const uint8_t *)wire)[offsetof(struct ptp_announce_message,grandmasterPriority1)]==128);
    assert(rd32(wire,offsetof(struct ptp_announce_message,grandmasterClockQuality))==0x0621436A);
    assert(((const uint8_t *)wire)[offsetof(struct ptp_announce_message,grandmasterPriority2)]==128);
    assert(rd64(wire,offsetof(struct ptp_announce_message,grandmasterIdentity))==TEST_CLOCK_ID);
    assert(rd16(wire,offsetof(struct ptp_announce_message,stepsRemoved))==0);
    assert(((const uint8_t *)wire)[offsetof(struct ptp_announce_message,timeSource)]==0x20);
    size_t trace=offsetof(struct ptp_announce_message,tlv_path_trace);
    assert(rd16(wire,trace)==PTP_TLV_PATH_TRACE && rd16(wire,trace+2)==8 && rd64(wire,trace+4)==TEST_CLOCK_ID);
    assert(daemon.announce_seq==sequence+1);
}

static void check_signaling(void)
{
    uint8_t wire[128];
    uint16_t sequence=daemon.signaling_seq;
    ptp_msg_signaling_send(&daemon);
    assert(capture_recv(general_capture,wire,sizeof wire,2000)==(ev_ssize_t)sizeof(struct ptp_signaling_message));
    check_header(wire,PTP_MSGTYPE_SIGNALING,sizeof(struct ptp_signaling_message),
        PTP_FLAG_UNICAST|PTP_FLAG_TIMESCALE,sequence,AIRPTP_LOGMESSAGEINT_SIGNALING,5);
    assert(rd8(wire,32)==0x05);
    assert(rd64(wire,34)==0 && rd16(wire,42)==0);
    size_t tlv1=offsetof(struct ptp_signaling_message,tlv_apple1),tlv2=offsetof(struct ptp_signaling_message,tlv_apple2);
    assert(rd16(wire,tlv1)==PTP_TLV_ORG_EXTENSION && rd16(wire,tlv1+2)==22);
    assert(!memcmp((const uint8_t *)wire+tlv1+4,"\x00\x0d\x93\x00\x00\x01\x00\x00\x03\x01",10));
    assert(rd16(wire,tlv2)==PTP_TLV_ORG_EXTENSION && rd16(wire,tlv2+2)==32);
    assert(!memcmp((const uint8_t *)wire+tlv2+4,"\x00\x0d\x93\x00\x00\x05\x00\x00\x03\x01",10));
    assert(daemon.signaling_seq==sequence+1);
}

static void make_delay_request(struct ptp_delay_req_message *request,uint16_t sequence)
{
    memset(request,0,sizeof *request);
    request->header.messageType=PTP_MSGTYPE_DELAY_REQ|0x10;
    request->header.versionPTP=0x02;
    request->header.messageLength=htobe16(sizeof *request);
    request->header.domainNumber=AIRPTP_DOMAIN;
    request->header.flags=htobe16(PTP_FLAG_UNICAST|PTP_FLAG_TIMESCALE);
    uint64_t foreign=htobe64(UINT64_C(0x1122334455667788));
    memcpy(request->header.sourcePortIdentity,&foreign,8);
    request->header.sourcePortIdentity[8]=0x80;
    request->header.sourcePortIdentity[9]=0x05;
    request->header.sequenceId=htobe16(sequence);
}

static void check_delay_response(void)
{
    struct ptp_delay_req_message request;
    uint8_t wire[128];
    union utils_net_sockaddr from;
    struct timespec before,after;

    make_delay_request(&request,0xbeef);
    assert(utils_net_sockaddr_get(&from,"127.0.0.1",0)==0);
    assert(platform_time(PLATFORM_MONOTONIC,&before)==0);
    ptp_msg_handle(&daemon,(uint8_t *)&request,sizeof request,&from,(socklen_t)sizeof from.sin);
    assert(platform_time(PLATFORM_MONOTONIC,&after)==0);

    assert(capture_recv(general_capture,wire,sizeof wire,2000)==(ev_ssize_t)sizeof(struct ptp_delay_resp_message));
    check_header(wire,PTP_MSGTYPE_DELAY_RESP,sizeof(struct ptp_delay_resp_message),
        PTP_FLAG_UNICAST|PTP_FLAG_TIMESCALE|PTP_FLAG_TWO_STEP,0xbeef,AIRPTP_LOGMESSAGEINT_DELAY_RESP,0);
    uint64_t received=wire_timestamp_ns(wire,offsetof(struct ptp_delay_resp_message,receiveTimestamp));
    assert(received>=to_ns(&before) && received<=to_ns(&after));
    size_t requester=offsetof(struct ptp_delay_resp_message,requestingPortIdentity);
    assert(rd64(wire,requester)==UINT64_C(0x1122334455667788));
    assert(rd8(wire,requester+8)==0x80 && rd8(wire,requester+9)==0x05);
}

static void check_pdelay_exchange(void)
{
    struct ptp_pdelay_req_message request;
    uint8_t response_wire[128],follow_wire[128];
    union utils_net_sockaddr from;
    struct timespec before,after;

    memset(&request,0,sizeof request);
    request.header.messageType=PTP_MSGTYPE_PDELAY_REQ|0x10;
    request.header.versionPTP=0x02;
    request.header.messageLength=htobe16(sizeof request);
    request.header.domainNumber=AIRPTP_DOMAIN;
    request.header.flags=htobe16(PTP_FLAG_UNICAST|PTP_FLAG_TIMESCALE);
    uint64_t foreign=htobe64(UINT64_C(0x1122334455667788));
    memcpy(request.header.sourcePortIdentity,&foreign,8);
    request.header.sourcePortIdentity[8]=0x80;
    request.header.sourcePortIdentity[9]=0x05;
    request.header.sequenceId=htobe16(0x1234);

    assert(utils_net_sockaddr_get(&from,"127.0.0.1",0)==0);
    assert(platform_time(PLATFORM_MONOTONIC,&before)==0);
    ptp_msg_handle(&daemon,(uint8_t *)&request,sizeof request,&from,(socklen_t)sizeof from.sin);
    assert(platform_time(PLATFORM_MONOTONIC,&after)==0);

    assert(capture_recv(event_capture,response_wire,sizeof response_wire,2000)==(ev_ssize_t)sizeof(struct ptp_pdelay_resp_message));
    check_header(response_wire,PTP_MSGTYPE_PDELAY_RESP,sizeof(struct ptp_pdelay_resp_message),
        PTP_FLAG_UNICAST|PTP_FLAG_TIMESCALE|PTP_FLAG_TWO_STEP,0x1234,AIRPTP_LOGMESSAGEINT_SYNC,0);
    uint64_t receipt=wire_timestamp_ns(response_wire,offsetof(struct ptp_pdelay_resp_message,requestReceiptTimestamp));
    assert(receipt>=to_ns(&before) && receipt<=to_ns(&after));

    assert(capture_recv(general_capture,follow_wire,sizeof follow_wire,2000)==(ev_ssize_t)sizeof(struct ptp_pdelay_resp_follow_up_message));
    check_header(follow_wire,PTP_MSGTYPE_PDELAY_RESP_FOLLOW_UP,sizeof(struct ptp_pdelay_resp_follow_up_message),
        PTP_FLAG_UNICAST|PTP_FLAG_TIMESCALE,0x1234,AIRPTP_LOGMESSAGEINT_SYNC,0);
    uint64_t origin=wire_timestamp_ns(follow_wire,offsetof(struct ptp_pdelay_resp_follow_up_message,responseOriginTimestamp));
    assert(origin>=receipt && origin<=to_ns(&after));
}

static void check_peer_state_mapping(void)
{
    union utils_net_sockaddr address;
    uint8_t wire[128],*body;
    size_t length;
    unsigned base_hexdumps=hexdumps;

    assert(utils_net_sockaddr_get(&address,"192.0.2.7",0)==0);

    memset(wire,0,sizeof wire);
    wire[0]=PTP_MSGTYPE_SIGNALING|0x10;
    wire[1]=0x02;
    length=sizeof(struct ptp_header)+PTP_PORT_ID_SIZE+4+3+3+4+1+(uint8_t)sizeof address.sin;
    uint16_t be_length=htobe16(length);
    memcpy(wire+2,&be_length,2);
    wire[30]=0; wire[31]=7;
    wire[33]=(uint8_t)-128;
    body=wire+sizeof(struct ptp_header)+PTP_PORT_ID_SIZE;
    uint16_t be16; uint32_t be32;
    be16=htobe16(PTP_TLV_ORG_EXTENSION); memcpy(body,&be16,2);
    be16=htobe16(3+3+4+1+(uint16_t)sizeof address.sin); memcpy(body+2,&be16,2);
    memcpy(body+4,"\x99\x99\x99\x00\x00\x01",6);
    be32=htobe32(0x0badf00d); memcpy(body+10,&be32,4);
    body[14]=(uint8_t)sizeof address.sin;
    memcpy(body+15,&address.sin,sizeof address.sin);

    assert(daemon.num_peers==1);
    ptp_msg_handle(&daemon,wire,length,&address,(socklen_t)sizeof address.sin);
    assert(daemon.num_peers==2);
    assert(daemon.peers[1].id==0x0badf00d && daemon.peers[1].is_active);
    char rendered[64]; assert(utils_net_address_get(rendered,sizeof rendered,&daemon.peers[1].naddr)==0);
    assert(!strcmp(rendered,"192.0.2.7"));

    memset(wire,0,sizeof wire);
    wire[0]=PTP_MSGTYPE_SIGNALING|0x10;
    wire[1]=0x02;
    length=sizeof(struct ptp_header)+PTP_PORT_ID_SIZE+4+3+3+4;
    be_length=htobe16(length); memcpy(wire+2,&be_length,2);
    body=wire+sizeof(struct ptp_header)+PTP_PORT_ID_SIZE;
    be16=htobe16(PTP_TLV_ORG_EXTENSION); memcpy(body,&be16,2);
    be16=htobe16(3+3+4); memcpy(body+2,&be16,2);
    memcpy(body+4,"\x99\x99\x99\x00\x00\x02",6);
    be32=htobe32(0x0badf00d); memcpy(body+10,&be32,4);

    ptp_msg_handle(&daemon,wire,length,&address,(socklen_t)sizeof address.sin);
    assert(daemon.num_peers==1);
    assert(daemon.peers[0].id==1 && daemon.peers[0].is_active);

    memset(wire,0,sizeof wire);
    wire[0]=PTP_MSGTYPE_SIGNALING|0x10;
    wire[1]=0x02;
    length=sizeof(struct ptp_header)+PTP_PORT_ID_SIZE+4+10;
    be_length=htobe16(length); memcpy(wire+2,&be_length,2);
    body=wire+sizeof(struct ptp_header)+PTP_PORT_ID_SIZE;
    be16=htobe16(PTP_TLV_ORG_EXTENSION); memcpy(body,&be16,2);
    be16=htobe16(200); memcpy(body+2,&be16,2);
    memset(body+4,0x99,10);

    ptp_msg_handle(&daemon,wire,length,&address,(socklen_t)sizeof address.sin);
    assert(daemon.num_peers==1);
    assert(hexdumps==base_hexdumps+1);

    uint8_t unknown[sizeof(struct ptp_header)];
    memset(unknown,0,sizeof unknown);
    unknown[0]=(uint8_t)((PTP_MSGTYPE_MANAGEMENT+1)|0x10);
    unknown[1]=0x02;
    be_length=htobe16(sizeof unknown); memcpy(unknown+2,&be_length,2);
    ptp_msg_handle(&daemon,unknown,sizeof unknown,&address,(socklen_t)sizeof address.sin);
    assert(hexdumps==base_hexdumps+2);
}

static void check_stale_peer_skipped(void)
{
    uint8_t wire[128];
    daemon.peers[0].last_seen=(uint64_t)time(NULL)-AIRPTP_STALE_SECS-1;
    ptp_msg_sync_send(&daemon);
    assert(capture_recv(event_capture,wire,sizeof wire,200)<0);
    assert(capture_recv(general_capture,wire,sizeof wire,200)<0);
    assert(!daemon.peers[0].is_active);
}

#ifndef FROZEN_PTP
static void check_invalid_peer_address(void)
{
    uint8_t wire[512]={0};
    union utils_net_sockaddr address={0};
    address.sin.sin_family=AF_INET;
    uint8_t *body=wire+sizeof(struct ptp_header)+PTP_PORT_ID_SIZE;
    wire[0]=PTP_MSGTYPE_SIGNALING|0x10;
    wire[1]=0x02;
    uint16_t value=htobe16(PTP_TLV_ORG_EXTENSION); memcpy(body,&value,2);
    memcpy(body+4,"\x99\x99\x99\x00\x00\x01",6);
    const unsigned lengths[]={255,0,1,sizeof(struct sockaddr_in)-1,sizeof(struct sockaddr_in)+1,sizeof(struct sockaddr_in6)};
    for(size_t i=0;i<sizeof lengths/sizeof lengths[0];++i) {
        unsigned length=lengths[i];
        body[14]=(uint8_t)length;
        memset(body+15,0,length);
        memcpy(body+15,&address.sin.sin_family,sizeof address.sin.sin_family);
        value=htobe16((uint16_t)(11+length)); memcpy(body+2,&value,2);
        size_t total=sizeof(struct ptp_header)+PTP_PORT_ID_SIZE+15+length;
        value=htobe16((uint16_t)total); memcpy(wire+2,&value,2);
        int peers=daemon.num_peers;
        ptp_msg_handle(&daemon,wire,total,&address,sizeof address.sin);
        assert(daemon.num_peers==peers);
    }
    body[14]=sizeof address.sin;
    memset(body+15,0,sizeof address.sin);
    value=htobe16(11+sizeof address.sin); memcpy(body+2,&value,2);
    size_t total=sizeof(struct ptp_header)+PTP_PORT_ID_SIZE+15+sizeof address.sin;
    value=htobe16((uint16_t)total); memcpy(wire+2,&value,2);
    int peers=daemon.num_peers;
    ptp_msg_handle(&daemon,wire,total,&address,sizeof address.sin);
    assert(daemon.num_peers==peers);
}
#endif

static void check_ports_override(void)
{
    extern unsigned short airptp_event_port,airptp_general_port;
    unsigned short saved_event=airptp_event_port,saved_general=airptp_general_port;
    airptp_ports_override(41001,41002);
    assert(airptp_event_port==41001 && airptp_general_port==41002);
    airptp_ports_override(saved_event,saved_general);
    assert(airptp_event_port==saved_event && airptp_general_port==saved_general);
}

int main(void)
{
    assert(platform_init()==0);
    struct airptp_callbacks callbacks={0};
    callbacks.logmsg=counted_log;
    callbacks.hexdump=counted_hexdump;
    airptp_callbacks_register(&callbacks);
    assert(PTP_EVENT_PORT==319 && PTP_GENERAL_PORT==320);
    assert(ptp_msg_handle_init()==0);

    memset(&daemon,0,sizeof daemon);
    daemon.clock_id=TEST_CLOCK_ID;
    evbase=event_base_new(); assert(evbase);
    daemon.evbase=evbase;
    daemon.send_announce_timer=evtimer_new(evbase,noop_timer,&daemon);
    daemon.send_signaling_timer=evtimer_new(evbase,noop_timer,&daemon);
    daemon.send_sync_timer=evtimer_new(evbase,noop_timer,&daemon);
    assert(daemon.send_announce_timer && daemon.send_signaling_timer && daemon.send_sync_timer);

    event_port=general_port=0;
    event_capture=platform_bound_socket("127.0.0.1",&event_port,SOCK_DGRAM,AF_INET,false);
    general_capture=platform_bound_socket("127.0.0.1",&general_port,SOCK_DGRAM,AF_INET,false);
    assert(event_capture!=(platform_socket)EVUTIL_INVALID_SOCKET && general_capture!=(platform_socket)EVUTIL_INVALID_SOCKET);
    assert(event_port && general_port && event_port!=general_port);
    assert(platform_socket_configure(event_capture,true)==0 && platform_socket_configure(general_capture,true)==0);
    event_tx=platform_socket_open(AF_INET,SOCK_DGRAM,0);
    general_tx=platform_socket_open(AF_INET,SOCK_DGRAM,0);
    assert(event_tx!=(platform_socket)EVUTIL_INVALID_SOCKET && general_tx!=(platform_socket)EVUTIL_INVALID_SOCKET);

    daemon.event_svc.socket.fd4=event_tx;
    daemon.event_svc.socket.fd6=-1;
    daemon.event_svc.port=event_port;
    daemon.general_svc.socket.fd4=general_tx;
    daemon.general_svc.socket.fd6=-1;
    daemon.general_svc.port=general_port;

    assert(utils_net_sockaddr_get(&daemon.peers[0].naddr,"127.0.0.1",0)==0);
    daemon.peers[0].naddr_len=(socklen_t)sizeof daemon.peers[0].naddr.sin;
    daemon.peers[0].id=1;
    daemon.peers[0].is_active=true;
    daemon.peers[0].last_seen=(uint64_t)time(NULL);
    daemon.num_peers=1;

    check_sync_follow_up();
    check_sync_follow_up();
    check_announce();
    check_signaling();
    check_delay_response();
    check_pdelay_exchange();
    check_peer_state_mapping();
#ifndef FROZEN_PTP
    check_invalid_peer_address();
#endif
    check_stale_peer_skipped();
    check_ports_override();
    assert(messages_logged>=1);

    platform_socket_close(event_capture); platform_socket_close(general_capture);
    platform_socket_close(event_tx); platform_socket_close(general_tx);
    event_free(daemon.send_announce_timer); event_free(daemon.send_signaling_timer); event_free(daemon.send_sync_timer);
    event_base_free(evbase);
    platform_cleanup();
#ifndef PTP_CAPTURE
    puts("ptp wire construction, sync/follow-up and request/response correlation, clock identity and signaling state mapping verified");
#endif
    return 0;
}
