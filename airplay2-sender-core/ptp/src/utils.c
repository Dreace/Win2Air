/*
MIT License

Copyright (c) 2026 OwnTone

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <errno.h>
#include <inttypes.h>

#include "utils.h"

extern _Thread_local struct airptp_callbacks airptp_cb;

static platform_socket
bind_one(const char *node, unsigned short port, int family)
{
  struct addrinfo hints = { 0 };
  struct addrinfo *servinfo = NULL;
  struct addrinfo *ptr;
  char strport[8];
  int yes = 1;
  platform_socket fd = (platform_socket)EVUTIL_INVALID_SOCKET;
  int ret;

  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_family = family;
  hints.ai_flags = node ? 0 : AI_PASSIVE;

  snprintf(strport, sizeof(strport), "%hu", port);
  ret = getaddrinfo(node, strport, &hints, &servinfo);
  if (ret != 0)
    goto error;

  for (ptr = servinfo; ptr != NULL; ptr = ptr->ai_next) {
    if (fd != (platform_socket)EVUTIL_INVALID_SOCKET)
      platform_socket_close(fd);

    fd = platform_socket_open(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (fd == (platform_socket)EVUTIL_INVALID_SOCKET)
      continue;

    // Blocking mode with a close-on-exec/non-inheritable handle
    ret = platform_socket_configure(fd, false);
    if (ret < 0)
      continue;

    // We won't be using dual stack for BSD compability. We also don't use
    // ifdef __linux__ to avoid branching.
    ret = (ptr->ai_family == AF_INET6) ? platform_socket_option(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) : 0;
    if (ret < 0)
      continue;

    ret = platform_bind(fd, ptr->ai_addr, (socklen_t)ptr->ai_addrlen);
    if (ret < 0)
      continue;
/*
    char straddr[128];
    utils_net_address_get(straddr, sizeof(straddr), (union utils_net_sockaddr *)ptr->ai_addr);
    printf("Bound %s (%s) to port %hu\n", straddr, (ptr->ai_family == AF_INET) ? "AF_INET" : "AF_INET6", port);
*/
    break;
  }

  freeaddrinfo(servinfo);

  if (!ptr)
    goto error;

  return fd;

 error:
  if (fd != (platform_socket)EVUTIL_INVALID_SOCKET)
    platform_socket_close(fd);
  return (platform_socket)EVUTIL_INVALID_SOCKET;
}

int
utils_net_bind(struct utils_net_socket *sock, const char *node, unsigned short port)
{
  sock->fd4 = bind_one(node, port, AF_INET);
  sock->fd6 = bind_one(node, port, AF_INET6);

  if (sock->fd4 == (platform_socket)EVUTIL_INVALID_SOCKET && sock->fd6 == (platform_socket)EVUTIL_INVALID_SOCKET)
    return -1;

  return 0;
}

int
utils_net_sockaddr_get(union utils_net_sockaddr *naddr, const char *addr, unsigned short port)
{
  const char *ipv4mapped_prefix = "::ffff:";
  struct addrinfo hints = { 0 };
  struct addrinfo *servinfo = NULL;
  char strport[8];
  int ret;

  // Trying to send to an ipv4-mapped address on our ipv6 socket will yield
  // Network unreachable, so we want to just have a clean ipv4 address
  if (strncmp(addr, ipv4mapped_prefix, strlen(ipv4mapped_prefix)) == 0)
    addr += strlen(ipv4mapped_prefix);

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  snprintf(strport, sizeof(strport), "%hu", port);
  ret = getaddrinfo(addr, strport, &hints, &servinfo);
  if (ret != 0)
    goto error;

  memcpy(&naddr->sa, servinfo->ai_addr, servinfo->ai_addrlen);

 error:
  freeaddrinfo(servinfo);
  return (ret != 0) ? -1 : 0;
}

int
utils_net_address_get(char *addr, size_t addr_len, union utils_net_sockaddr *naddr)
{
  const char *s = NULL;

  memset(addr, 0, addr_len); // Just in case caller doesn't check for errors

  if (naddr->sa.sa_family == AF_INET6)
     s = inet_ntop(AF_INET6, &naddr->sin6.sin6_addr, addr, addr_len);
  else if (naddr->sa.sa_family == AF_INET)
     s = inet_ntop(AF_INET, &naddr->sin.sin_addr, addr, addr_len);

  if (!s)
    return -1;

  return 0;
}

bool
utils_net_address_is_same(union utils_net_sockaddr *a, union utils_net_sockaddr *b)
{
  int a_family = a->sa.sa_family;
  int b_family = b->sa.sa_family;
  struct in_addr *a_sin_addr = &a->sin.sin_addr;
  struct in_addr *b_sin_addr = &b->sin.sin_addr;
  struct in6_addr *a_sin6_addr = &a->sin6.sin6_addr;
  struct in6_addr *b_sin6_addr = &b->sin6.sin6_addr;
  int cmp;

  if (a_family == AF_INET && b_family == AF_INET)
    cmp = memcmp(a_sin_addr, b_sin_addr, sizeof(struct in_addr));
  else if (a_family == AF_INET6 && b_family == AF_INET6)
    cmp = memcmp(a_sin6_addr, b_sin6_addr, sizeof(struct in6_addr));
  else if (a_family == AF_INET && IN6_IS_ADDR_V4MAPPED(b_sin6_addr))
    cmp = memcmp(a_sin_addr, ((uint8_t *)b_sin6_addr) + 12, sizeof(struct in_addr));
  else if (b_family == AF_INET && IN6_IS_ADDR_V4MAPPED(a_sin6_addr))
    cmp = memcmp(b_sin_addr, ((uint8_t *)a_sin6_addr) + 12, sizeof(struct in_addr));
  else
    cmp = 1;

  return (cmp == 0);
}

// In Linux, you just need one socket for sending both ipv4 and ipv6, but BSD
// and Mac OS think that would be too easy. We have to go with the lowest
// denominator.
ev_ssize_t
utils_net_sendto(struct utils_net_socket *sock, const void *buf, size_t len, union utils_net_sockaddr *addr)
{
  if (addr->sa.sa_family == AF_INET6)
    return platform_sendto(sock->fd6, buf, len, 0, &addr->sa, (socklen_t)sizeof(addr->sin6));
  else
    return platform_sendto(sock->fd4, buf, len, 0, &addr->sa, (socklen_t)sizeof(addr->sin));
}

void
utils_net_socket_close(struct utils_net_socket *sock)
{
  if (sock->fd4 != (platform_socket)EVUTIL_INVALID_SOCKET)
    platform_socket_close(sock->fd4);
  if (sock->fd6 != (platform_socket)EVUTIL_INVALID_SOCKET)
    platform_socket_close(sock->fd6);

  sock->fd4 = (platform_socket)EVUTIL_INVALID_SOCKET;
  sock->fd6 = (platform_socket)EVUTIL_INVALID_SOCKET;
}

uint32_t
utils_djb_hash(const void *data, size_t len)
{
  const unsigned char *bytes = data;
  uint32_t hash = 5381;

  while (len--) {
    hash = ((hash << 5) + hash) + *bytes;
    bytes++;
  }

  return hash;
}
