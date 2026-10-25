#ifndef SENDER_EVENTS_H
#define SENDER_EVENTS_H
#include "protocol.h"
int airplay_events_init(struct event_base *);
void airplay_events_deinit(void);
int airplay_events_listen(const char *, const char *, unsigned short, const uint8_t *, size_t);
#endif
