#include "platform.h"
#include <iphlpapi.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int platform_interfaces(struct platform_interface *items,size_t capacity,size_t *count)
{
    if(!items || !capacity || !count) return platform_fail("interface-enumerate",EINVAL);
    *count=0; ULONG length=16384; IP_ADAPTER_ADDRESSES *list=NULL; ULONG status;
    unsigned attempts=0;
    do {
        free(list); list=malloc(length); if(!list) return platform_fail("interface-allocate",ENOMEM);
        status=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_INCLUDE_ALL_INTERFACES,NULL,list,&length);
    } while(status==ERROR_BUFFER_OVERFLOW && ++attempts<3);
    if(status!=NO_ERROR) { free(list); return platform_fail("interface-enumerate",(int)status); }
    for(IP_ADAPTER_ADDRESSES *p=list;p;p=p->Next) {
        if(p->OperStatus!=IfOperStatusUp) continue;
        for(IP_ADAPTER_UNICAST_ADDRESS *address=p->FirstUnicastAddress;address;address=address->Next) {
            int family=address->Address.lpSockaddr->sa_family; if(family!=AF_INET && family!=AF_INET6) continue;
            if(*count==capacity) { free(list); return platform_fail("interface-capacity",ENOSPC); }
            struct platform_interface *item=&items[(*count)++]; memset(item,0,sizeof(*item));
            snprintf(item->name,sizeof(item->name),"%s",p->AdapterName); item->index=family==AF_INET ? p->IfIndex : p->Ipv6IfIndex; item->family=family;
            const void *bytes=family==AF_INET ? (const void *)&((struct sockaddr_in *)address->Address.lpSockaddr)->sin_addr : (const void *)&((struct sockaddr_in6 *)address->Address.lpSockaddr)->sin6_addr;
            if(!item->index || !inet_ntop(family,bytes,item->address,sizeof(item->address))) { int error=WSAGetLastError(); free(list); return platform_fail("interface-address",error); }
            if(p->PhysicalAddressLength==6) memcpy(item->mac,p->PhysicalAddress,6);
        }
    }
    free(list); return 0;
}
unsigned platform_interface_index(const char *name)
{
    if(!name || !*name) { platform_fail("interface-select",ERROR_INVALID_PARAMETER); return 0; }
    struct platform_interface items[256]; size_t count;
    if(platform_interfaces(items,256,&count)<0) return 0;
    for(size_t i=0;i<count;++i) if(!strcmp(items[i].name,name)) return items[i].index;
    platform_fail("interface-select",ERROR_NOT_FOUND); return 0;
}
int platform_multicast(platform_socket fd,const char *group,unsigned index,bool join)
{
    struct in_addr v4; struct in6_addr v6;
    if(!group || !index) return platform_fail("multicast-select",EINVAL);
    if(inet_pton(AF_INET,group,&v4)==1 && (ntohl(v4.s_addr)&0xf0000000)==0xe0000000) {
        struct ip_mreq request={0}; request.imr_multiaddr=v4; request.imr_interface.s_addr=htonl(index);
        return platform_socket_option(fd,IPPROTO_IP,join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,&request,sizeof(request));
    }
    if(inet_pton(AF_INET6,group,&v6)==1 && IN6_IS_ADDR_MULTICAST(&v6)) {
        struct ipv6_mreq request={v6,index}; return platform_socket_option(fd,IPPROTO_IPV6,join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,&request,sizeof(request));
    }
    return platform_fail("multicast-address",EINVAL);
}
