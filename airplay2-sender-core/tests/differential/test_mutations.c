#include "trace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct protocol_trace reference,candidate;
int main(void)
{
    struct trace_event event={.operation="SETUP",.direction=1,.state=1,.count=5};
    event.fields[0]=(struct trace_field){.name="proof",.length=3,.value={1,2,3}};
    event.fields[1]=(struct trace_field){.name="alac",.length=3,.value={4,5,6}};
    event.fields[2]=(struct trace_field){.name="sequence",.semantics=TRACE_SEQUENCE,.domain=1,.length=2,.value={0,1}};
    event.fields[3]=(struct trace_field){.name="timestamp",.semantics=TRACE_TIME,.domain=2,.length=4,.value={0,0,0,1}};
    event.fields[4]=(struct trace_field){.name="session",.semantics=TRACE_IDENTITY,.domain=3,.length=4,.value={7,8,9,10}};
    assert(!trace_append(&reference,&event)); strcpy(event.operation,"RECORD"); event.state=2; ++event.fields[2].value[1]; event.fields[3].value[3]+=35; assert(!trace_append(&reference,&event));
    struct trace_difference difference;
    for(unsigned mutation=0;mutation<8;++mutation) {
        candidate=reference;
        switch(mutation) {
            case 0: { struct trace_event swap=candidate.events[0]; candidate.events[0]=candidate.events[1]; candidate.events[1]=swap; break; }
            case 1: candidate.events[0].fields[0].value[0]^=1; break;
            case 2: candidate.events[0].fields[1].value[0]^=1; break;
            case 3: ++candidate.events[1].fields[2].value[1]; break;
            case 4: ++candidate.events[1].fields[3].value[3]; break;
            case 5: ++candidate.events[1].state; break;
            case 6: candidate.events[1].fields[2].semantics=TRACE_TIME; candidate.events[1].fields[2].domain=2; break;
            case 7: strcpy(candidate.events[0].fields[1].name,"proof"); break;
        }
        assert(trace_compare(&reference,&candidate,&difference)<0 && difference.reason[0]);
        printf("mutation=%u event=%zu field=%s reason=%s\n",mutation,difference.event,difference.field,difference.reason);
    }
    candidate=reference;
    for(size_t i=0;i<candidate.count;++i) { candidate.events[i].fields[2].value[1]+=40; candidate.events[i].fields[3].value[3]+=50; candidate.events[i].fields[4].value[0]=99; }
    assert(!trace_compare(&reference,&candidate,&difference)); puts("legal dynamic changes preserve identity, sequence and timing relationships"); return 0;
}
