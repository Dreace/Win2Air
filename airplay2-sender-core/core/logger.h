#ifndef SENDER_INTERNAL_LOGGER_H
#define SENDER_INTERNAL_LOGGER_H
#include "protocol.h"
enum { E_LOG, E_WARN, E_INFO, E_DBG, E_SPAM, L_AIRPLAY, L_MISC, L_MAIN };
/* Wire payloads and authentication values stay out of diagnostics. */
#define DPRINTF(level, domain, ...) do { if ((level) == E_LOG) protocol_diagnostic(__VA_ARGS__); } while (0)
#define DVPRINTF(level, domain, ...) ((void)0)
#define DHEXDUMP(level, domain, ...) ((void)0)
#endif
