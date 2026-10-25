#include "platform.h"
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <string.h>
#include <stdio.h>
int platform_interfaces(struct platform_interface *items,size_t capacity,size_t *count)
{
    if(!items || !capacity || !count) return platform_fail("interface-enumerate",EINVAL);
    *count=0; struct ifaddrs *list=NULL;
    if(getifaddrs(&list)<0) return platform_fail("interface-enumerate",errno);
    for(struct ifaddrs *p=list;p;p=p->ifa_next) {
        if(!p->ifa_addr || !(p->ifa_flags&IFF_UP) || (p->ifa_addr->sa_family!=AF_INET && p->ifa_addr->sa_family!=AF_INET6)) continue;
        if(*count==capacity) { freeifaddrs(list); return platform_fail("interface-capacity",ENOSPC); }
        struct platform_interface *item=&items[(*count)++]; memset(item,0,sizeof(*item));
        snprintf(item->name,sizeof(item->name),"%s",p->ifa_name); item->index=if_nametoindex(p->ifa_name); item->family=p->ifa_addr->sa_family;
        const void *address=item->family==AF_INET ? (const void *)&((struct sockaddr_in *)p->ifa_addr)->sin_addr : (const void *)&((struct sockaddr_in6 *)p->ifa_addr)->sin6_addr;
        if(!item->index || !inet_ntop(item->family,address,item->address,sizeof(item->address))) { freeifaddrs(list); return platform_fail("interface-address",errno); }
        for(struct ifaddrs *link=list;link;link=link->ifa_next) if(link->ifa_addr && link->ifa_addr->sa_family==AF_PACKET && !strcmp(link->ifa_name,p->ifa_name)) {
            struct sockaddr_ll *hardware=(struct sockaddr_ll *)link->ifa_addr; if(hardware->sll_halen==6) memcpy(item->mac,hardware->sll_addr,6);
        }
    }
    freeifaddrs(list); return 0;
}
unsigned platform_interface_index(const char *name)
{ unsigned index=name ? if_nametoindex(name) : 0; if(!index) platform_fail("interface-select",ENODEV); return index; }
int platform_multicast(platform_socket fd,const char *group,unsigned index,bool join)
{
    struct in_addr v4; struct in6_addr v6;
    if(!group || !index) return platform_fail("multicast-select",EINVAL);
    if(inet_pton(AF_INET,group,&v4)==1 && IN_MULTICAST(ntohl(v4.s_addr))) {
        struct ip_mreqn request={0}; request.imr_multiaddr=v4; request.imr_ifindex=(int)index;
        return platform_socket_option(fd,IPPROTO_IP,join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,&request,sizeof(request));
    }
    if(inet_pton(AF_INET6,group,&v6)==1 && IN6_IS_ADDR_MULTICAST(&v6)) {
        struct ipv6_mreq request={v6,index}; return platform_socket_option(fd,IPPROTO_IPV6,join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,&request,sizeof(request));
    }
    return platform_fail("multicast-address",EINVAL);
}
