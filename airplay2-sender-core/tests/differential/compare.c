#include "trace.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct identity { unsigned domain; size_t length; uint8_t left[TRACE_VALUE_BYTES],right[TRACE_VALUE_BYTES]; };
static void identities_free(struct identity *identities)
{
    volatile uint8_t *bytes=(volatile uint8_t *)identities;
    for(size_t i=0;i<256*sizeof(*identities);++i) bytes[i]=0;
    free(identities);
}
static int fail(struct trace_difference *difference,size_t event,const char *field,const char *reason)
{
    difference->event=event;
    strncpy(difference->field,field,sizeof(difference->field)-1);
    strncpy(difference->reason,reason,sizeof(difference->reason)-1);
    return -1;
}
int trace_compare(const struct protocol_trace *a,const struct protocol_trace *b,struct trace_difference *d)
{
    uint64_t origins_left[32]={0},origins_right[32]={0};
    int origins_set[32]={0};
    int domain_kind[32]={0};
    size_t domain_width[32]={0};
    size_t mapped=0;
    struct identity *identities;
    if (!d) return -1;
    memset(d,0,sizeof(*d));
    if (!a || !b || !a->count || a->count>TRACE_MAX_EVENTS || a->count!=b->count)
        return fail(d,0,"events","missing-or-extra-events");
    identities=calloc(256,sizeof(*identities));
    if (!identities) return fail(d,0,"mapping","allocation-failed");
    for (size_t i=0;i<a->count;++i) {
        const struct trace_event *x=&a->events[i],*y=&b->events[i];
        if (!memchr(x->operation,0,sizeof(x->operation)) || !memchr(y->operation,0,sizeof(y->operation)) ||
            strcmp(x->operation,y->operation) || x->direction!=y->direction || x->state!=y->state ||
            !x->count || x->count>TRACE_MAX_FIELDS || x->count!=y->count) {
            fail(d,i,"operation","order-state-or-fields-differ"); goto failed;
        }
        for (size_t j=0;j<x->count;++j) {
            const struct trace_field *left=&x->fields[j],*right=&y->fields[j];
            if (!memchr(left->name,0,sizeof(left->name)) || !memchr(right->name,0,sizeof(right->name)) ||
                strcmp(left->name,right->name) || left->domain!=right->domain || left->domain>=32 ||
                left->semantics!=right->semantics || left->length!=right->length || left->length>TRACE_VALUE_BYTES) {
                fail(d,i,memchr(left->name,0,sizeof(left->name)) ? left->name : "field","field-schema-differs"); goto failed;
            }
            for(size_t previous=0;previous<j;++previous)
                if(!strcmp(x->fields[previous].name,left->name)) { fail(d,i,left->name,"duplicate-field"); goto failed; }
            if(left->semantics!=TRACE_EXACT) {
                unsigned domain=left->domain;
                if(domain_kind[domain] && (domain_kind[domain]!=(int)left->semantics || domain_width[domain]!=left->length)) {
                    fprintf(stderr,"operation=normalization-domain field=%s domain=%u previous-width=%zu width=%zu\n",left->name,domain,domain_width[domain],left->length);
                    fail(d,i,left->name,"inconsistent-domain"); goto failed;
                }
                domain_kind[domain]=(int)left->semantics; domain_width[domain]=left->length;
            }
            if (left->semantics==TRACE_IDENTITY) {
                size_t k;
                if (!left->length) { fail(d,i,left->name,"empty-identity"); goto failed; }
                for (k=0;k<mapped;++k) {
                    struct identity *entry=&identities[k];
                    if (entry->domain!=left->domain || entry->length!=left->length) continue;
                    int same_left=!memcmp(entry->left,left->value,left->length);
                    int same_right=!memcmp(entry->right,right->value,right->length);
                    if (same_left || same_right) {
                        if (!same_left || !same_right) { fail(d,i,left->name,"identity-relationship-differs"); goto failed; }
                        break;
                    }
                }
                if (k==mapped) {
                    if (mapped==256) { fail(d,i,left->name,"identity-map-full"); goto failed; }
                    struct identity *entry=&identities[mapped++];
                    entry->domain=left->domain; entry->length=left->length;
                    memcpy(entry->left,left->value,left->length); memcpy(entry->right,right->value,right->length);
                }
            } else if (trace_field_equal(left,right,&origins_left[left->domain],&origins_right[left->domain],&origins_set[left->domain])) {
                fail(d,i,left->name,"value-or-relative-position-differs"); goto failed;
            }
        }
    }
    identities_free(identities); return 0;
failed:
    identities_free(identities); return -1;
}
