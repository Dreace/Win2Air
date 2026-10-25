#include "trace.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"operation=normalization check=%s line=%d\n",#c,__LINE__); return 1; } } while (0)
int main(void)
{
    static struct protocol_trace reference, candidate;
    struct trace_difference difference;
    struct trace_event event = {.operation="SETUP",.state=1,.count=3};
    event.fields[0] = (struct trace_field){.name="session",.semantics=TRACE_IDENTITY,.domain=1,.length=4,.value={1,2,3,4}};
    event.fields[1] = (struct trace_field){.name="sequence",.semantics=TRACE_SEQUENCE,.domain=2,.length=2,.value={255,255}};
    event.fields[2] = (struct trace_field){.name="time",.semantics=TRACE_TIME,.domain=3,.length=4,.value={0,0,0,10}};
    CHECK(!trace_append(&reference,&event));
    strcpy(event.operation,"RECORD"); event.fields[1].value[0]=0; event.fields[1].value[1]=0; event.fields[2].value[3]=20;
    CHECK(!trace_append(&reference,&event));
    candidate=reference;
    for (size_t i=0;i<candidate.count;++i) {
        candidate.events[i].fields[0].value[0]=9;
        candidate.events[i].fields[1].value[0]=0; candidate.events[i].fields[1].value[1]=(uint8_t)(50+i);
        candidate.events[i].fields[2].value[3]=(uint8_t)(30+i*10);
    }
    CHECK(!trace_compare(&reference,&candidate,&difference));
    strcpy(reference.events[0].fields[0].name,"timingPeerInfo.SupportsClockPortMatchingOverride");
    strcpy(candidate.events[0].fields[0].name,reference.events[0].fields[0].name);
    CHECK(!trace_compare(&reference,&candidate,&difference));
    candidate.events[1].fields[0].value[1]=8;
    CHECK(trace_compare(&reference,&candidate,&difference)<0 && difference.event==1);
    candidate=reference; candidate.events[1].fields[1].value[1]=1;
    CHECK(trace_compare(&reference,&candidate,&difference)<0);
    candidate=reference; candidate.events[1].fields[2].value[3]=21;
    CHECK(trace_compare(&reference,&candidate,&difference)<0);
    candidate=reference; candidate.events[0].count=2;
    CHECK(trace_compare(&reference,&candidate,&difference)<0);
    reference.count=0; candidate.count=0;
    CHECK(trace_compare(&reference,&candidate,&difference)<0);
    return 0;
}
