#ifndef TEST_PROTOCOL_TRACE_H
#define TEST_PROTOCOL_TRACE_H
#include <stddef.h>
#include <stdint.h>
#define TRACE_MAX_EVENTS 256
#define TRACE_MAX_FIELDS 128
#define TRACE_VALUE_BYTES 512
#define TRACE_PATH_BYTES 128
enum trace_semantics { TRACE_EXACT, TRACE_IDENTITY, TRACE_SEQUENCE, TRACE_TIME };
struct trace_field {
    char name[TRACE_PATH_BYTES];
    enum trace_semantics semantics;
    unsigned domain;
    size_t length;
    uint8_t value[TRACE_VALUE_BYTES];
};
struct trace_event {
    char operation[48];
    unsigned direction;
    unsigned state;
    size_t count;
    struct trace_field fields[TRACE_MAX_FIELDS];
};
struct protocol_trace { size_t count; struct trace_event events[TRACE_MAX_EVENTS]; };
struct trace_difference { size_t event; char field[TRACE_PATH_BYTES]; char reason[64]; };
int trace_compare(const struct protocol_trace *, const struct protocol_trace *, struct trace_difference *);
int trace_field_equal(const struct trace_field *, const struct trace_field *,
                      uint64_t *, uint64_t *, int *);
int trace_append(struct protocol_trace *, const struct trace_event *);
#endif
