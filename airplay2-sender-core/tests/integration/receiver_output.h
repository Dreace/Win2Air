#ifndef RECEIVER_OUTPUT_H
#define RECEIVER_OUTPUT_H
#include <stdint.h>
struct receiver_timeline { unsigned messages,sessions; uint64_t span_ns; };
struct receiver_packets { unsigned rtp,requests,responses,reordered,duplicates; unsigned short audio_port; char audio_source[16],audio_destination[16]; };
int receiver_ptp_timeline(const char *log,uint64_t identity,const char *source,struct receiver_timeline *timeline);
int receiver_packet_evidence(const char *capture,struct receiver_packets *packets);
#endif
