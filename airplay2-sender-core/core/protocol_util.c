/*
 * Copyright (C) 2009-2010 Julien BLACHE <jb@jblache.org>
 *
 * Some code included below is in the public domain, check comments
 * in the file.
 *
 * Pieces of code adapted from mt-daapd:
 * Copyright (C) 2003-2007 Ron Pedde (ron@pedde.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include "protocol.h"
const void *protocol_bytes_find(const void *buffer,size_t length,const void *pattern,size_t pattern_length)
{
  const unsigned char *bytes=buffer;
  if(!pattern_length) return buffer;
  if(!buffer || !pattern || pattern_length>length) return NULL;
  for(size_t i=0;i<=length-pattern_length;++i)
    if(!memcmp(bytes+i,pattern,pattern_length)) return bytes+i;
  return NULL;
}
#include "logger.h"
#include <errno.h>

int
net_address_get(char *addr, size_t addr_len, union net_sockaddr *naddr)
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

int
net_sockaddr_get(union net_sockaddr *naddr, const char *addr, unsigned short port)
{
  memset(naddr, 0, sizeof(union net_sockaddr));

  if (inet_pton(AF_INET, addr, &naddr->sin.sin_addr) == 1)
    {
      naddr->sin.sin_family = AF_INET;
      naddr->sin.sin_port = htons(port);
      return 0;
    }

  if (true && inet_pton(AF_INET6, addr, &naddr->sin6.sin6_addr) == 1)
    {
      naddr->sin6.sin6_family = AF_INET6;
      naddr->sin6.sin6_port = htons(port);
      return 0;
    }

  return -1;
}

int
net_port_get(unsigned short *port, union net_sockaddr *naddr)
{
  if (naddr->sa.sa_family == AF_INET6)
     *port = ntohs(naddr->sin6.sin6_port);
  else if (naddr->sa.sa_family == AF_INET)
     *port = ntohs(naddr->sin.sin_port);
  else
    return -1;

  return 0;
}

int net_if_get(char *name,size_t capacity,const char *address)
{
    struct platform_interface items[256]; size_t count;
    if(!name || !capacity || !address || platform_interfaces(items,256,&count)<0) return -1;
    for(size_t i=0;i<count;++i) if(!strcmp(items[i].address,address)) {
        if(strlen(items[i].name)>=capacity) return platform_fail("interface-name-capacity",ENOSPC);
        memcpy(name,items[i].name,strlen(items[i].name)+1); return 0;
    }
    return platform_fail("interface-address-unavailable",ENODEV);
}
int net_mac_get(uint8_t *mac,size_t capacity,const char *name)
{
    struct platform_interface items[256]; size_t count;
    if(!mac || capacity<6 || !name || platform_interfaces(items,256,&count)<0) return -1;
    for(size_t i=0;i<count;++i) if(!strcmp(items[i].name,name)) { memcpy(mac,items[i].mac,6); return 0; }
    return platform_fail("interface-hardware-unavailable",ENODEV);
}
platform_socket net_connect(const char *address,unsigned short port,int type,const char *service)
{
    platform_socket fd=platform_target_socket(address,port,type,3000,false);
    if(fd<0) { const struct platform_failure *error=platform_last_failure(); DPRINTF(E_LOG,L_MISC,"Connecting service %s failed: operation=%s code=%d\n",service,error->operation,error->code); }
    return fd;
}
void net_socket_close(struct net_socket *socket)
{
    if(socket->fd4>=0) platform_socket_close(socket->fd4);
    if(socket->fd6>=0) platform_socket_close(socket->fd6);
    socket->fd4=socket->fd6=-1;
}
int net_bind(struct net_socket *socket,unsigned short *port,int type,const char *service)
{
    unsigned short selected=port ? *port : 0;
    socket->fd4=socket->fd6=-1;
    socket->fd4=platform_bound_socket(NULL,&selected,type,AF_INET,false);
    if(socket->fd4<0) {
        const struct platform_failure *error=platform_last_failure();
        DPRINTF(E_LOG,L_MISC,"Binding service %s failed: operation=%s code=%d\n",service,error->operation,error->code);
        return -1;
    }
    unsigned short shared=selected;
    socket->fd6=platform_bound_socket(NULL,&shared,type,AF_INET6,false);
    if(socket->fd6<0) socket->fd6=-1;
    if(port) *port=selected;
    return 0;
}

int
safe_hextou64(const char *str, uint64_t *val)
{
  char *end;
  unsigned long long intval;

  if (str == NULL)
    {
      DPRINTF(E_SPAM, L_MISC, "Input to safe_hextou64 is NULL\n");
      return -1;
    }

  errno = 0;
  intval = strtoull(str, &end, 16);

  if (((errno == ERANGE) && (intval == ULLONG_MAX))
      || ((errno != 0) && (intval == 0)))
    {
      DPRINTF(E_DBG, L_MISC, "Invalid u64 in string (%s): %s\n", str, strerror(errno));
      return -1;
    }

  if (end == str)
    {
      DPRINTF(E_DBG, L_MISC, "No u64 found in string (%s)\n", str);
      return -1;
    }

  if (intval > UINT64_MAX)
    {
      DPRINTF(E_DBG, L_MISC, "u64 value out of range (%s)\n", str);
      return -1;
    }

  *val = (uint64_t)intval;

  return 0;
}

bool
quality_is_equal(struct media_quality *a, struct media_quality *b)
{
  return (a->sample_rate == b->sample_rate && a->bits_per_sample == b->bits_per_sample && a->channels == b->channels && a->bit_rate == b->bit_rate);
}

int uuid_make(char *text) { return platform_uuid(text); }
void thread_setname(const char *name) { platform_thread_name(name); }
void secure_string_free(char *value) { if (value) { sodium_memzero(value, strlen(value)); free(value); } }
