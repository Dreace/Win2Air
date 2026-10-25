#include "../integration/receiver_output.h"
#include "../fixtures/file_create.h"
#include "platform.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
    assert(argc==2 && !platform_init());
    char identity[37],path[1024]; assert(!platform_uuid(identity));
    assert(snprintf(path,sizeof(path),"%s/receiver-output-%s.log",argv[1],identity)>0);
    FILE *file=fixture_file_create(path); assert(file && !fclose(file));
    for(unsigned mutation=0;mutation<7;++mutation) {
        file=fopen(path,"wb"); assert(file);
        for(unsigned i=0;i<12;++i) {
            uint64_t origin=UINT64_C(1000000000)+i*UINT64_C(125000000);
            unsigned sequence=(65530+i)&UINT16_MAX;
            if(mutation==1 && i==5) sequence=(sequence-1)&UINT16_MAX;
            if(mutation==2 && i==5) origin-=UINT64_C(250000000);
            if(mutation==3 && i==2) break;
            fprintf(file,"Clock %s, grandmaster abc. Raw Precise Origin Timestamp: %" PRIx64 ". ID: %u, Follow_Up Number: %u. Source: %s\n",
                mutation==4?"abd":"abc",origin,sequence,i+1,mutation==5?"192.0.2.8":"192.0.2.7");
        }
        if(mutation==6) fputs("Timing discontinuity on clock abc\n",file);
        assert(!fclose(file));
        struct receiver_timeline timeline;
        assert((receiver_ptp_timeline(path,UINT64_C(0xabc),"192.0.2.7",&timeline)!=0)==(mutation!=0));
    }
    file=fopen(path,"wb"); assert(file);
    for(unsigned session=0;session<2;++session) {
        fputs("Clear timing peer group.\nStart Timing\n",file);
        for(unsigned i=0;i<12;++i)
            fprintf(file,"Clock abc, grandmaster abc. Raw Precise Origin Timestamp: %" PRIx64 ". ID: %u, Follow_Up Number: %u. Source: 192.0.2.7\n",
                UINT64_C(1000000000)+i*UINT64_C(125000000),i,i+1);
    }
    assert(!fclose(file));
    struct receiver_timeline timeline;
    assert(!receiver_ptp_timeline(path,UINT64_C(0xabc),"192.0.2.7",&timeline) && timeline.sessions==2 && timeline.messages==24);
    file=fopen(path,"wb"); assert(file);
    uint32_t header[6]={UINT32_C(0xa1b2c3d4),0,0,0,65535,1};
    assert(fwrite(header,1,sizeof(header),file)==sizeof(header));
    for(unsigned type=0;type<5;++type) {
        uint8_t packet[58]={0};
        uint32_t record[4]={1,0,sizeof(packet),sizeof(packet)};
        packet[12]=8; packet[14]=0x45; packet[23]=17;
        packet[36]=0x12; packet[37]=0x34;
        packet[38]=0; packet[39]=24;
        packet[42]=0x80; packet[43]=type==1?0xd5:0x60;
        if(type==1) { packet[39]=16; packet[49]=1; }
        if(type==4) { packet[44]=0xff; packet[45]=0xff; }
        assert(fwrite(record,1,sizeof(record),file)==sizeof(record));
        assert(fwrite(packet,1,sizeof(packet),file)==sizeof(packet));
    }
    assert(!fclose(file));
    struct receiver_packets packets;
    assert(!receiver_packet_evidence(path,&packets) && packets.rtp==4 && packets.requests==1 && packets.responses==1
        && packets.reordered==1 && packets.duplicates==1 && packets.audio_port==0x1234);
    file=fopen(path,"ab"); assert(file && fputc(0,file)!=EOF && !fclose(file));
    assert(receiver_packet_evidence(path,&packets));
    assert(!remove(path)); platform_cleanup();
    puts("receiver time identity, sequence wrap, origin order, missing evidence and truncated capture rejection verified");
    return 0;
}
