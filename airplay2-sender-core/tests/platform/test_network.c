#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static void backpressure(void)
{
    platform_socket listener=platform_socket_open(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(listener>=0 && platform_bind(listener,(struct sockaddr *)&address,sizeof(address))==0);
    socklen_t size=sizeof(address); assert(getsockname(listener,(struct sockaddr *)&address,&size)==0 && listen(listener,1)==0);
    platform_socket client=platform_target_socket("127.0.0.1",ntohs(address.sin_port),SOCK_STREAM,1000,true);
    assert(client>=0);
    platform_socket receiver=accept(listener,NULL,NULL); assert(receiver>=0 && platform_socket_configure(receiver,true)==0);
    int capacity=65536; assert(setsockopt(client,SOL_SOCKET,SO_SNDBUF,(const char *)&capacity,sizeof(capacity))==0);
    const size_t length=8*1024*1024; unsigned char *input=malloc(length),*output=malloc(length); assert(input && output);
    for(size_t i=0;i<length;++i) input[i]=(unsigned char)(i*37+11);
    size_t sent=0,received=0; int blocked=0;
    while(sent<length) {
        size_t chunk=length-sent; if(chunk>8192) chunk=8192;
        ev_ssize_t n=platform_send(client,input+sent,chunk,0);
        if(n<0) { assert(platform_socket_retryable(platform_last_failure()->code)); blocked=1; break; }
        assert(n>0 && (size_t)n<=chunk); sent+=(size_t)n;
    }
    assert(blocked && sent>0);
    struct timespec start,now; assert(platform_time(PLATFORM_MONOTONIC,&start)==0);
    while(received<length) {
        if(sent<length) {
            size_t chunk=length-sent; if(chunk>8192) chunk=8192;
            ev_ssize_t n=platform_send(client,input+sent,chunk,0);
            if(n>0) sent+=(size_t)n; else assert(n<0 && platform_socket_retryable(platform_last_failure()->code));
        }
        ev_ssize_t n=platform_recv(receiver,output+received,length-received,0);
        if(n>0) received+=(size_t)n; else assert(n<0 && platform_socket_retryable(platform_last_failure()->code));
        assert(platform_time(PLATFORM_MONOTONIC,&now)==0 && now.tv_sec-start.tv_sec<10);
    }
    assert(sent==length && !memcmp(input,output,length)); free(input); free(output);
    assert(platform_socket_close(receiver)==0 && platform_socket_close(client)==0 && platform_socket_close(listener)==0);
}
static void transfer(int type)
{
    platform_socket server=platform_socket_open(AF_INET,type,0),client=platform_socket_open(AF_INET,type,0);
    assert(server>=0 && client>=0);
    struct sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(platform_bind(server,(struct sockaddr *)&address,sizeof(address))==0);
    socklen_t size=sizeof(address); assert(getsockname(server,(struct sockaddr *)&address,&size)==0);
    assert(platform_socket_configure(client,true)==0);
    if(type==SOCK_STREAM) assert(listen(server,1)==0);
    int connected=platform_connect(client,(struct sockaddr *)&address,sizeof(address));
    platform_socket receiver=server;
    if(type==SOCK_STREAM) {
        assert(connected==0 || platform_wait(client,true,1000)>0);
        int error; assert(platform_socket_error(client,&error)==0 && error==0);
        receiver=accept(server,NULL,NULL); assert(receiver>=0);
    } else assert(connected==0);
    assert(platform_socket_configure(receiver,true)==0);
    const char text[]="partial transport preserves every byte"; char buffer[sizeof(text)]={0}; size_t sent=0,received=0;
    while(sent<sizeof(text)) {
        size_t chunk=type==SOCK_STREAM ? 3 : sizeof(text);
        if(chunk>sizeof(text)-sent) chunk=sizeof(text)-sent;
        ev_ssize_t n=platform_send(client,text+sent,chunk,0); assert(n>0); sent+=(size_t)n;
    }
    while(received<sizeof(text)) {
        assert(platform_wait(receiver,false,1000)>0);
        size_t chunk=type==SOCK_STREAM ? 2 : sizeof(text);
        if(chunk>sizeof(text)-received) chunk=sizeof(text)-received;
        ev_ssize_t n=platform_recv(receiver,buffer+received,chunk,0); assert(n>0); received+=(size_t)n;
    }
    assert(!memcmp(text,buffer,sizeof(text)));
    assert(platform_socket_close(client)==0);
    if(receiver!=server) assert(platform_socket_close(receiver)==0);
    assert(platform_socket_close(server)==0);
}
int main(void)
{
    assert(platform_init()==0);
#ifdef _WIN32
    _Static_assert(sizeof(platform_socket)>=sizeof(SOCKET),"socket handle width");
#endif
    transfer(SOCK_STREAM); transfer(SOCK_DGRAM);
    backpressure();
    unsigned short datagram_port=0;
    platform_socket datagram=platform_bound_socket("127.0.0.1",&datagram_port,SOCK_DGRAM,AF_INET,false);
    if(datagram<0) fprintf(stderr,"UDP bind failed: operation=%s code=%d\n",platform_last_failure()->operation,platform_last_failure()->code);
    assert(datagram>=0 && datagram_port && platform_socket_close(datagram)==0);
    struct platform_interface items[128]; size_t count=0;
    assert(platform_interfaces(items,128,&count)==0 && count);
    unsigned loopback=0,loopback6=0;
    for(size_t i=0;i<count;++i) {
        assert(items[i].index && platform_interface_index(items[i].name)==items[i].index);
        char name[256];
        assert(platform_interface_name(items[i].index,name,sizeof(name))==0);
        assert(!strcmp(name,items[i].name));
        assert(platform_interface_name(items[i].index,name,1)<0);
        if(items[i].family==AF_INET && !strcmp(items[i].address,"127.0.0.1")) loopback=items[i].index;
        if(items[i].family==AF_INET6 && !strcmp(items[i].address,"::1")) loopback6=items[i].index;
    }
    assert(loopback);
    platform_socket multicast=platform_socket_open(AF_INET,SOCK_DGRAM,0); assert(multicast>=0);
    assert(platform_multicast(multicast,"239.255.42.42",loopback,true)==0);
    assert(platform_multicast(multicast,"239.255.42.42",loopback,false)==0);
    assert(platform_multicast(multicast,"127.0.0.1",loopback,true)<0);
    assert(platform_interface_index("missing-test-interface")==0);
    assert(platform_socket_close(multicast)==0);
    assert(loopback6);
    platform_socket multicast6=platform_socket_open(AF_INET6,SOCK_DGRAM,0); assert(multicast6>=0);
    assert(platform_multicast(multicast6,"ff02::1234",loopback6,true)==0);
    assert(platform_multicast(multicast6,"ff02::1234",loopback6,false)==0);
    assert(platform_socket_close(multicast6)==0);
    unsigned short port6=0;
    platform_socket bound6=platform_bound_socket("::1",&port6,SOCK_STREAM,AF_INET6,false);
    assert(bound6>=0 && port6 && listen(bound6,1)==0);
    platform_socket scoped_client=platform_target_socket("::1",port6,SOCK_STREAM,1000,true);
    assert(scoped_client>=0);
    platform_socket accepted=accept(bound6,NULL,NULL); assert(accepted>=0);
    char byte; assert(platform_recv(scoped_client,&byte,1,0)<0);
    assert(platform_last_failure()->code);
    assert(platform_socket_close(accepted)==0 && platform_socket_close(scoped_client)==0 && platform_socket_close(bound6)==0);
    for(size_t i=0;i<count;++i) {
        if(items[i].family!=AF_INET6) continue;
        struct sockaddr_in6 address={0}; address.sin6_family=AF_INET6;
        assert(inet_pton(AF_INET6,items[i].address,&address.sin6_addr)==1);
        if(!IN6_IS_ADDR_LINKLOCAL(&address.sin6_addr)) continue;
        address.sin6_scope_id=items[i].index;
        platform_socket receiver=platform_socket_open(AF_INET6,SOCK_DGRAM,0); assert(receiver>=0);
        assert(platform_bind(receiver,(struct sockaddr *)&address,sizeof(address))==0);
        socklen_t size=sizeof(address); assert(getsockname(receiver,(struct sockaddr *)&address,&size)==0);
        char scoped[INET6_ADDRSTRLEN+16]; snprintf(scoped,sizeof(scoped),"%s%%%u",items[i].address,items[i].index);
        platform_socket client=platform_target_socket(scoped,ntohs(address.sin6_port),SOCK_DGRAM,1000,true);
        if(client<0) fprintf(stderr,"scoped connect failed: operation=%s code=%d address=%s\n",platform_last_failure()->operation,platform_last_failure()->code,scoped);
        assert(client>=0 && platform_send(client,"s",1,0)==1);
        assert(platform_wait(receiver,false,1000)>0 && platform_recv(receiver,&byte,1,0)==1 && byte=='s');
        assert(platform_socket_close(client)==0);
        assert(memchr(items[i].address,0,sizeof(items[i].address)) && memchr(items[i].name,0,sizeof(items[i].name)));
        char named[INET6_ADDRSTRLEN+257];
        int formatted=snprintf(named,sizeof(named),"%.*s%%%.*s",(int)sizeof(items[i].address)-1,items[i].address,(int)sizeof(items[i].name)-1,items[i].name);
        assert(formatted>0 && (size_t)formatted<sizeof(named));
        client=platform_target_socket(named,ntohs(address.sin6_port),SOCK_DGRAM,1000,true);
        assert(client>=0 && platform_send(client,"n",1,0)==1);
        assert(platform_wait(receiver,false,1000)>0 && platform_recv(receiver,&byte,1,0)==1 && byte=='n');
        assert(platform_socket_close(client)==0 && platform_socket_close(receiver)==0);
    }
    assert(platform_target_socket("::1%missing-test-interface",9,SOCK_DGRAM,1000,true)<0);
    assert(platform_target_socket("::1%+1",9,SOCK_DGRAM,1000,true)<0);
    assert(platform_target_socket("::1%9999999999999999999999999999999999999999",9,SOCK_DGRAM,1000,true)<0);
    platform_context(42,3,"missing-test-interface");
    assert(platform_target_socket("127.0.0.1",0,SOCK_STREAM,1000,false)<0);
    assert(platform_last_failure()->generation==42 && platform_last_failure()->stage==3);
    assert(!strcmp(platform_last_failure()->interface_name,"missing-test-interface"));
    platform_cleanup(); puts("TCP partial transfer, UDP, socket ownership, interface selection and multicast membership verified"); return 0;
}
