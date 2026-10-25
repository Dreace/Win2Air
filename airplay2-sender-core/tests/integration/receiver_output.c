#include "receiver_output.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>
int receiver_packet_evidence(const char *path,struct receiver_packets *packets)
{
    FILE *file=fopen(path,"rb"); if(!file) return 1;
    uint32_t header[6],record[4]; uint8_t packet[65536];
    struct packet_history { unsigned length; uint8_t digest[32]; bool requested; };
    memset(packets,0,sizeof(*packets));
    if(fread(header,1,sizeof(header),file)!=sizeof(header) || header[0]!=UINT32_C(0xa1b2c3d4) || header[5]!=1) { fclose(file); return 1; }
    struct packet_history *history=calloc(65536,sizeof(*history));
    if(!history) { fclose(file); return 1; }
    int failed=0;
    bool have_sequence=false; unsigned highest_sequence=0;
    for(;;) {
        size_t bytes=fread(record,1,sizeof(record),file);
        if(!bytes) break;
        if(bytes!=sizeof(record)) { failed=1; break; }
        if(record[2]>sizeof(packet)) { failed=1; break; }
        if(fread(packet,1,record[2],file)!=record[2]) { failed=1; break; }
        size_t length=record[2];
        if(length<42 || packet[12]!=8 || packet[13]!=0 || (packet[14]>>4)!=4 || packet[23]!=17) continue;
        size_t offset=14+(packet[14]&15)*4;
        if(offset+10>length) continue;
        size_t payload_length=((size_t)packet[offset+4]<<8)|packet[offset+5];
        if(payload_length<10 || payload_length>length-offset) { failed=1; break; }
        payload_length-=8;
        const uint8_t *payload=packet+offset+8;
        unsigned type=payload[1]&0x7f;
        if(payload[0]==0x80 && type==0x60) {
            if(payload_length<12) { failed=1; break; }
            unsigned sequence=((unsigned)payload[2]<<8)|payload[3];
            uint8_t digest[32]; crypto_hash_sha256(digest,payload,payload_length);
            if(history[sequence].requested && history[sequence].length) {
                if(history[sequence].length!=payload_length || memcmp(history[sequence].digest,digest,sizeof(digest))) { failed=1; break; }
                ++packets->responses; history[sequence].requested=false;
            } else if(history[sequence].length) {
                if(history[sequence].length!=payload_length || memcmp(history[sequence].digest,digest,sizeof(digest))) { failed=1; break; }
                ++packets->duplicates;
            } else {
                unsigned advance=(sequence-highest_sequence)&UINT16_MAX;
                if(have_sequence && advance>UINT16_MAX/2) ++packets->reordered;
                else { highest_sequence=sequence; have_sequence=true; }
            }
            history[sequence].length=(unsigned)payload_length;
            memcpy(history[sequence].digest,digest,sizeof(digest));
            ++packets->rtp;
            packets->audio_port=(unsigned short)((packet[offset+2]<<8)|packet[offset+3]);
            snprintf(packets->audio_source,sizeof(packets->audio_source),"%u.%u.%u.%u",packet[26],packet[27],packet[28],packet[29]);
            snprintf(packets->audio_destination,sizeof(packets->audio_destination),"%u.%u.%u.%u",packet[30],packet[31],packet[32],packet[33]);
        } else if(payload[0]==0x80 && type==0x55) {
            if(payload_length!=8) { failed=1; break; }
            unsigned first=((unsigned)payload[4]<<8)|payload[5],count=((unsigned)payload[6]<<8)|payload[7];
            if(!count || count>65535/2) { failed=1; break; }
            for(unsigned i=0;i<count;++i) history[(first+i)&UINT16_MAX].requested=true;
            ++packets->requests;
        }
    }
    if(ferror(file)) failed=1;
    if(fclose(file)) failed=1;
    free(history);
    return failed;
}
int receiver_ptp_timeline(const char *path,uint64_t identity,const char *source,struct receiver_timeline *timeline)
{
    FILE *file=fopen(path,"r"); if(!file) return 1;
    char line[8192]; uint64_t first=0,last=0; unsigned previous=0,follow_previous=0;
    unsigned session_messages=0;
    memset(timeline,0,sizeof(*timeline));
    int failed=0;
    while(fgets(line,sizeof(line),file)) {
        if(strstr(line,"Clear timing peer group.") || strstr(line,"Start Timing")) {
            if(session_messages) {
                if(session_messages<8 || last-first<UINT64_C(1000000000)) { failed=1; break; }
                ++timeline->sessions; timeline->span_ns+=last-first; session_messages=0;
            }
            continue;
        }
        const char *clock=strstr(line,"Clock "),*stamp=strstr(line,"Raw Precise Origin Timestamp: "),*number=strstr(line,"ID: ");
        if(strstr(line,"Timing discontinuity on clock")) { failed=1; break; }
        if(!clock || !strstr(clock,", grandmaster ")) continue;
        uint64_t actual,grandmaster,origin; unsigned sequence,follow; char address[64];
        if(!stamp || !number || sscanf(clock,"Clock %" SCNx64 ", grandmaster %" SCNx64,&actual,&grandmaster)!=2 ||
           sscanf(stamp,"Raw Precise Origin Timestamp: %" SCNx64,&origin)!=1 ||
           sscanf(number,"ID: %u, Follow_Up Number: %u. Source: %63s",&sequence,&follow,address)!=3 ||
           actual!=identity || grandmaster!=identity || strcmp(source,address) || !origin || !follow || sequence>UINT16_MAX) { failed=1; break; }
        if(session_messages) {
            unsigned advance=(sequence-previous)&UINT16_MAX;
            unsigned expected=follow_previous<100?follow_previous+1:100;
            if(!advance || advance>UINT16_MAX/2 || origin<=last || follow!=expected) { failed=1; break; }
        } else first=origin;
        previous=sequence; follow_previous=follow; last=origin; ++timeline->messages; ++session_messages;
    }
    if(ferror(file)) failed=1;
    if(fclose(file)) failed=1;
    if(session_messages) {
        if(session_messages<8 || last-first<UINT64_C(1000000000)) failed=1;
        ++timeline->sessions; timeline->span_ns+=last-first;
    }
    return failed || !timeline->sessions;
}
