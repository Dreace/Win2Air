#ifndef __AIRPTP_UTILS_H__
#define __AIRPTP_UTILS_H__

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "../../platform/platform.h"

#ifdef HAVE_ENDIAN_H
# include <endian.h>
#elif defined(HAVE_SYS_ENDIAN_H)
# include <sys/endian.h>
#elif defined(HAVE_LIBKERN_OSBYTEORDER_H)
#include <libkern/OSByteOrder.h>
#define htobe16(x) OSSwapHostToBigInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#elif defined(_WIN32)
/* All supported hosts are little endian; the macros map to the Posix names. */
#define htobe16(x) htons(x)
#define be16toh(x) ntohs(x)
#define htobe32(x) htonl(x)
#define be32toh(x) ntohl(x)
#define htobe64(x) __builtin_bswap64(x)
#define be64toh(x) __builtin_bswap64(x)
#endif

#define ARRAY_SIZE(x) ((unsigned int)(sizeof(x) / sizeof((x)[0])))

#define UTILS_NET_SOCKET_INIT {(platform_socket)EVUTIL_INVALID_SOCKET, (platform_socket)EVUTIL_INVALID_SOCKET}

struct utils_net_socket
{
  platform_socket fd4;
  platform_socket fd6;
};

union utils_net_sockaddr
{
  struct sockaddr_in sin;
  struct sockaddr_in6 sin6;
  struct sockaddr sa;
  struct sockaddr_storage ss;
};

int
utils_net_bind(struct utils_net_socket *sock, const char *node, unsigned short port);

int
utils_net_sockaddr_get(union utils_net_sockaddr *naddr, const char *addr, unsigned short port);

int
utils_net_address_get(char *addr, size_t addr_len, union utils_net_sockaddr *naddr);

bool
utils_net_address_is_same(union utils_net_sockaddr *a, union utils_net_sockaddr *b);

ev_ssize_t
utils_net_sendto(struct utils_net_socket *sock, const void *buf, size_t len, union utils_net_sockaddr *addr);

void
utils_net_socket_close(struct utils_net_socket *sock);

uint32_t
utils_djb_hash(const void *data, size_t len);

#endif // __AIRPTP_UTILS_H__
