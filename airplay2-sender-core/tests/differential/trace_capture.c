#include "trace.h"
#include <string.h>
int trace_append(struct protocol_trace *trace,const struct trace_event *event)
{
    if (!trace || !event || trace->count>=TRACE_MAX_EVENTS || !event->count || event->count>TRACE_MAX_FIELDS ||
        !memchr(event->operation,0,sizeof(event->operation))) return -1;
    trace->events[trace->count++]=*event;
    return 0;
}
