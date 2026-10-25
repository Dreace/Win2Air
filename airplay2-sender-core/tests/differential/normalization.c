#include "trace.h"
#include <string.h>
static uint64_t integer(const struct trace_field *field)
{
    uint64_t value=0;
    for (size_t i=0;i<field->length;++i) value=(value<<8)|field->value[i];
    return value;
}
int trace_field_equal(const struct trace_field *a,const struct trace_field *b,uint64_t *x,uint64_t *y,int *set)
{
    if (!a || !b || a->length!=b->length || a->length>TRACE_VALUE_BYTES || a->semantics!=b->semantics) return -1;
    if (a->semantics==TRACE_EXACT) return memcmp(a->value,b->value,a->length) ? -1 : 0;
    if ((a->semantics!=TRACE_SEQUENCE && a->semantics!=TRACE_TIME) ||
        (a->length!=2 && a->length!=4 && a->length!=8)) return -1;
    uint64_t left=integer(a),right=integer(b);
    uint64_t mask=a->length==8 ? UINT64_MAX : (UINT64_C(1)<<(a->length*8))-1;
    if (!*set) { *x=left; *y=right; *set=1; }
    return ((left-*x)&mask)==((right-*y)&mask) ? 0 : -1;
}
