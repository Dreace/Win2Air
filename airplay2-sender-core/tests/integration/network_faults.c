#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "protocol.h"
#include "rtp_common.h"
#include <assert.h>
#include <arpa/inet.h>
#include <gcrypt.h>
#include <unistd.h>
static int sender,receiver;
static void transmit(struct rtp_packet *packet)
{ assert(packet && send(sender,packet->data,packet->data_len,0)==(ssize_t)packet->data_len); }
static void receive_packet(uint16_t sequence,uint8_t payload)
{
    uint8_t data[64]; ssize_t size=recv(receiver,data,sizeof(data),0); assert(size==44 && data[0]==0x80 && data[1]==96);
    assert(((unsigned)data[2]<<8|data[3])==sequence);
    for(size_t i=12;i<(size_t)size;++i) assert(data[i]==payload);
    printf("received sequence=%u payload=%02x bytes=%zd\n",sequence,payload,size);
}
int main(void)
{
    assert(gcry_check_version(NULL)); alarm(5);
    sender=socket(AF_INET,SOCK_DGRAM,0); receiver=socket(AF_INET,SOCK_DGRAM,0); assert(sender>=0 && receiver>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(receiver,(struct sockaddr *)&address,sizeof(address))); socklen_t size=sizeof(address); assert(!getsockname(receiver,(struct sockaddr *)&address,&size) && !connect(sender,(struct sockaddr *)&address,size));
    struct timeval timeout={1,0}; assert(!setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    struct media_quality quality={44100,16,2,0}; struct rtp_session *session=rtp_session_new(&quality,8,0,UINT64_C(0x123456789abcdef0)); assert(session);
    session->seqnum=65534; session->pos=UINT32_MAX-351;
    struct rtcp_timestamp stamp={.pos=UINT32_MAX-100000,.ts={123456,789012345}};
    struct rtp_packet *sync=rtp_sync_packet_next(session,stamp,0x90); assert(sync && sync->data_len==28);
    uint8_t expected_sync[28]={0x90,0xd7,0,6};
    uint32_t wire_pos=htonl(stamp.pos),wire_time=htonl(session->pos);
    uint64_t wire_ns=htobe64(UINT64_C(123456789012345)),wire_clock=htobe64(UINT64_C(0x123456789abcdef0));
    memcpy(expected_sync+4,&wire_pos,4); memcpy(expected_sync+8,&wire_ns,8); memcpy(expected_sync+16,&wire_time,4); memcpy(expected_sync+20,&wire_clock,8);
    assert(!memcmp(sync->data,expected_sync,sizeof(expected_sync)));
    sync=rtp_sync_packet_next(session,stamp,0x80); expected_sync[0]=0x80; assert(sync && !memcmp(sync->data,expected_sync,sizeof(expected_sync)));
    puts("PTP sync marker, position, nanoseconds, RTP latency and clock identity matched fixed bytes");
    uint16_t sequence[4];
    for(unsigned i=0;i<4;++i) {
        struct rtp_packet *packet=rtp_packet_next(session,32,352,96); assert(packet); memset(packet->payload,(int)(0x40+i),32); sequence[i]=packet->seqnum; rtp_packet_commit(session,packet);
    }
    /* Withhold the missing datagram, reorder the retained datagrams, duplicate a delivery. */
    transmit(rtp_packet_get(session,sequence[2])); transmit(rtp_packet_get(session,sequence[0])); transmit(rtp_packet_get(session,sequence[2])); transmit(rtp_packet_get(session,sequence[3]));
    receive_packet(sequence[2],0x42); receive_packet(sequence[0],0x40); receive_packet(sequence[2],0x42); receive_packet(sequence[3],0x43);
    /* The same packet history path supplies the lost packet for retransmission. */
    transmit(rtp_packet_get(session,sequence[1])); receive_packet(sequence[1],0x41);
    /* A datagram already queued in the socket survives a sender FLUSH; history does not. */
    transmit(rtp_packet_get(session,sequence[3])); uint32_t position=session->pos; uint16_t next=session->seqnum;
    rtp_session_flush(session); assert(session->pos==position && session->seqnum==next && !session->pktbuf_len);
    for(unsigned i=0;i<4;++i) assert(!rtp_packet_get(session,sequence[i]));
    receive_packet(sequence[3],0x43);
    struct rtp_packet *packet=rtp_packet_next(session,32,352,96); assert(packet); memset(packet->payload,0x55,32); rtp_packet_commit(session,packet); transmit(rtp_packet_get(session,next)); receive_packet(next,0x55);
    printf("flush position=%u next=%u stale-retransmission=rejected\n",position,next);
    rtp_session_free(session); close(sender); close(receiver); alarm(0); return 0;
}
