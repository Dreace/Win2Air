#include "platform.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
platform_socket platform_target_socket(const char *address,unsigned short port,int type,unsigned timeout_ms,bool nonblocking)
{
    if(!address || !*address || !port) { platform_fail("target-address",EINVAL); return -1; }
    char literal[INET6_ADDRSTRLEN]; unsigned scope=0;
    const char *separator=strchr(address,'%');
    if(separator) {
        size_t size=(size_t)(separator-address); struct in6_addr bytes;
        if(!size || size>=sizeof(literal) || !separator[1]) { platform_fail("target-scope",EINVAL); return -1; }
        memcpy(literal,address,size); literal[size]='\0';
        if(inet_pton(AF_INET6,literal,&bytes)!=1) { platform_fail("target-scope",EINVAL); return -1; }
        const char *selection=separator+1;
        if(strspn(selection,"0123456789")!=strlen(selection)) {
            struct platform_interface items[256]; size_t count;
            if(platform_interfaces(items,256,&count)<0) return -1;
            for(size_t i=0;i<count;++i)
                if(items[i].family==AF_INET6 && !strcmp(items[i].name,selection)) { scope=items[i].index; break; }
            if(!scope) { platform_fail("target-scope-interface",ENODEV); return -1; }
        } else {
            errno=0; unsigned long parsed=strtoul(selection,NULL,10);
            if(errno==ERANGE || !parsed || parsed>UINT_MAX) { platform_fail("target-scope",EINVAL); return -1; }
            char name[256]; scope=(unsigned)parsed;
            if(platform_interface_name(scope,name,sizeof(name))<0) return -1;
        }
        if(!scope) { platform_fail("target-scope",EINVAL); return -1; }
        address=literal;
    }
    struct addrinfo hints={0},*list=NULL; hints.ai_socktype=type; char service[8]; snprintf(service,sizeof(service),"%hu",port);
    int status=getaddrinfo(address,service,&hints,&list);
    if(status) { platform_fail("target-resolve",status); return -1; }
    platform_socket fd=-1;
    for(struct addrinfo *p=list;p;p=p->ai_next) {
        if(scope && p->ai_family==AF_INET6) ((struct sockaddr_in6 *)p->ai_addr)->sin6_scope_id=scope;
        fd=platform_socket_open(p->ai_family,p->ai_socktype,p->ai_protocol); if(fd<0) continue;
        bool ready=platform_socket_configure(fd,true)==0;
        if(ready && platform_connect(fd,p->ai_addr,(socklen_t)p->ai_addrlen)<0) {
            ready=(errno==EINPROGRESS || errno==EAGAIN);
            if(ready) {
                int waited=platform_wait(fd,true,timeout_ms),error=0;
                ready=waited>0 && platform_socket_error(fd,&error)==0 && error==0;
                if(!waited) platform_fail("socket-connect-timeout",ETIMEDOUT);
                else if(error) platform_fail("socket-connect",error);
            }
        }
        if(ready && platform_socket_configure(fd,nonblocking)==0) break;
        struct platform_failure failure=*platform_last_failure(); platform_socket_close(fd); platform_fail(failure.operation,failure.code); fd=-1;
    }
    freeaddrinfo(list); return fd;
}
platform_socket platform_bound_socket(const char *address,unsigned short *port,int type,int family,bool reuseport)
{
    if(!port) { platform_fail("bind-port",EINVAL); return -1; }
    struct addrinfo hints={0},*list=NULL; hints.ai_socktype=type; hints.ai_family=family; hints.ai_flags=address ? 0 : AI_PASSIVE;
    char service[8]; snprintf(service,sizeof(service),"%hu",*port);
    int status=getaddrinfo(address,service,&hints,&list); if(status) { platform_fail("bind-resolve",status); return -1; }
    platform_socket fd=-1;
    for(struct addrinfo *p=list;p;p=p->ai_next) {
        fd=platform_socket_open(p->ai_family,p->ai_socktype,p->ai_protocol); if(fd<0) continue;
        int yes=1,reuse=type==SOCK_STREAM;
        bool ready=platform_socket_configure(fd,type==SOCK_STREAM)==0 && platform_socket_reuseport(fd,reuseport)==0 &&
            (type!=SOCK_STREAM || platform_socket_option(fd,SOL_SOCKET,SO_KEEPALIVE,&yes,sizeof(yes))==0) &&
            platform_socket_option(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse))==0;
        if(ready && family==AF_INET6) ready=platform_socket_option(fd,IPPROTO_IPV6,IPV6_V6ONLY,&yes,sizeof(yes))==0;
        if(ready && platform_bind(fd,p->ai_addr,(socklen_t)p->ai_addrlen)==0) break;
        struct platform_failure failure=*platform_last_failure(); platform_socket_close(fd); platform_fail(failure.operation,failure.code); fd=-1;
    }
    freeaddrinfo(list);
    if(fd>=0) {
        struct sockaddr_storage local; socklen_t length=sizeof(local);
        if(getsockname(fd,(struct sockaddr *)&local,&length)<0) { int code=EVUTIL_SOCKET_ERROR(); platform_socket_close(fd); platform_fail("socket-local-address",code); return -1; }
        *port=family==AF_INET ? ntohs(((struct sockaddr_in *)&local)->sin_port) : ntohs(((struct sockaddr_in6 *)&local)->sin6_port);
    }
    return fd;
}
