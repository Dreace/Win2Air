#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform.h"
#include "../ptp/airptp.h"
struct airptp_daemon;
#include "../ptp/src/ptp_definitions.h"
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_CLOCK_SEED UINT64_C(0x1234567890abcdef)
#define TEST_CLOCK_ID UINT64_C(0xffff567890abcdef)

static char log_lines[65536];
static size_t log_used;
static platform_mutex log_mutex=PLATFORM_MUTEX_INIT;
static bool reject_thread,reject_bind,reject_event,reject_wakeup;
static bool reject_start_send,reject_start_recv;
static unsigned event_calls,reject_event_at;
static unsigned event_add_calls,reject_event_add_at;
ev_ssize_t __real_airptp_wakeup_send(platform_socket,const void *,size_t);
ev_ssize_t __wrap_airptp_wakeup_send(platform_socket fd,const void *data,size_t length)
{
    if((reject_wakeup && length==1) || (reject_start_send && length>1))
        return platform_fail("ptp-wakeup-send",EIO);
    return __real_airptp_wakeup_send(fd,data,length);
}
ev_ssize_t __real_airptp_wakeup_recv(platform_socket,void *,size_t);
ev_ssize_t __wrap_airptp_wakeup_recv(platform_socket fd,void *data,size_t length)
{
    return reject_start_recv && length>1 ? platform_fail("ptp-start-recv",EIO) : __real_airptp_wakeup_recv(fd,data,length);
}
int __real_platform_thread_start(platform_thread *,void *(*)(void *),void *);
int __wrap_platform_thread_start(platform_thread *thread,void *(*callback)(void *),void *context)
{
    return reject_thread ? platform_fail("ptp-thread-start",EAGAIN) : __real_platform_thread_start(thread,callback,context);
}
int __real_platform_bind(platform_socket,const struct sockaddr *,socklen_t);
int __wrap_platform_bind(platform_socket fd,const struct sockaddr *address,socklen_t size)
{
    return reject_bind ? platform_fail("ptp-bind",EACCES) : __real_platform_bind(fd,address,size);
}
struct event *__real_event_new(struct event_base *,evutil_socket_t,short,event_callback_fn,void *);
struct event *__wrap_event_new(struct event_base *base,evutil_socket_t fd,short flags,event_callback_fn callback,void *context)
{
    ++event_calls;
    return reject_event || (reject_event_at && event_calls==reject_event_at) ? NULL : __real_event_new(base,fd,flags,callback,context);
}
int __real_event_add(struct event *,const struct timeval *);
int __wrap_event_add(struct event *event,const struct timeval *timeout)
{
    ++event_add_calls;
    return reject_event_add_at && event_add_calls==reject_event_add_at ? -1 : __real_event_add(event,timeout);
}
static bool log_contains(const char *needle)
{
    assert(!platform_mutex_lock(&log_mutex));
    bool found=strstr(log_lines,needle)!=NULL;
    assert(!platform_mutex_unlock(&log_mutex));
    return found;
}
static void collect_log(const char *fmt,...)
{
    assert(!platform_mutex_lock(&log_mutex));
    va_list ap; va_start(ap,fmt);
    int written=vsnprintf(log_lines+log_used,sizeof(log_lines)-log_used,fmt,ap);
    va_end(ap);
    assert(written>0 && (size_t)written<sizeof(log_lines)-log_used);
    log_used+=(size_t)written;
    log_lines[log_used++]='\n';
    log_lines[log_used]='\0';
    assert(!platform_mutex_unlock(&log_mutex));
}
static void drop_hexdump(const char *msg,uint8_t *data,size_t len) { (void)msg; (void)data; (void)len; }

static int wait_for_log(const char *needle,unsigned attempts)
{
    for(unsigned i=0;i<attempts;i++) {
        if(log_contains(needle)) return 1;
        usleep(100*1000);
    }
    return 0;
}

int main(void)
{
    assert(platform_init()==0);
    struct airptp_callbacks callbacks={0};
    callbacks.logmsg=collect_log;
    callbacks.hexdump=drop_hexdump;
    airptp_callbacks_register(&callbacks);

    unsigned short event_port=0,general_port=0;
    platform_socket probe=platform_bound_socket(NULL,&event_port,SOCK_DGRAM,AF_INET,false);
    assert(probe!=(platform_socket)EVUTIL_INVALID_SOCKET && event_port);
    platform_socket_close(probe);
    probe=platform_bound_socket(NULL,&general_port,SOCK_DGRAM,AF_INET,false);
    assert(probe!=(platform_socket)EVUTIL_INVALID_SOCKET && general_port && general_port!=event_port);
    platform_socket_close(probe);
    airptp_ports_override(event_port,general_port);

    struct airptp_handle *daemon=airptp_daemon_bind(NULL);
    assert(daemon);
    assert(!airptp_daemon_bind(NULL));
    assert(strstr(airptp_errmsg_get(),"Could not bind to PTP event port"));

    assert(airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)==0);
    uint64_t clock_id=0;
    assert(airptp_clock_id_get(&clock_id,daemon)==0 && clock_id==TEST_CLOCK_ID);

    assert(airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)<0);
    assert(strstr(airptp_errmsg_get(),"ports not bound"));

    uint32_t peer_id=0,foreign_id=0;
    assert(airptp_peer_add(&peer_id,"127.0.0.1",daemon)==0 && peer_id);
    assert(airptp_peer_add(&peer_id,"in valid",daemon)<0);
    assert(strstr(airptp_errmsg_get(),"address is invalid"));
    assert(airptp_peer_add(&foreign_id,"255.255.255.255",daemon)==0 && foreign_id);
    assert(wait_for_log("Error sending PTP msg",30));

    airptp_peer_remove(peer_id,daemon);
    airptp_peer_remove(foreign_id,daemon);
    airptp_end(daemon);

    for(unsigned round=0;round<2;round++) {
        daemon=airptp_daemon_bind(NULL);
        assert(daemon);
        assert(airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)==0);
        clock_id=0;
        assert(airptp_clock_id_get(&clock_id,daemon)==0 && clock_id==TEST_CLOCK_ID);
        airptp_end(daemon);
    }
    daemon=airptp_daemon_bind(NULL);
    assert(daemon);
    airptp_end(daemon);
    airptp_end(NULL);

    union { struct sockaddr sa; struct sockaddr_in sin; struct sockaddr_in6 sin6; struct sockaddr_storage storage; } localhost;
    {
        struct addrinfo hints={0},*resolved=NULL;
        hints.ai_family=AF_UNSPEC;
        hints.ai_socktype=SOCK_DGRAM;
        assert(getaddrinfo("localhost","0",&hints,&resolved)==0 && resolved);
        memcpy(&localhost,resolved->ai_addr,resolved->ai_addrlen);
        freeaddrinfo(resolved);
    }
    const char *single_node=localhost.sa.sa_family==AF_INET ? "127.0.0.1" : "::1";
    const char *wrong_family=localhost.sa.sa_family==AF_INET ? "::1" : "127.0.0.1";
    const char *family_message=localhost.sa.sa_family==AF_INET ? "ipv4-only" : "ipv6-only";
    assert(localhost.sa.sa_family==AF_INET || localhost.sa.sa_family==AF_INET6);
    daemon=airptp_daemon_bind(single_node);
    assert(daemon);
    assert(airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)==0);
    assert(airptp_peer_add(&peer_id,wrong_family,daemon)<0);
    assert(strstr(airptp_errmsg_get(),family_message));
    airptp_end(daemon);

    airptp_ports_override(PTP_EVENT_PORT,PTP_GENERAL_PORT);
    for(unsigned short port=PTP_EVENT_PORT;port<=PTP_GENERAL_PORT;++port) {
        unsigned short occupied=port;
        probe=platform_bound_socket("0.0.0.0",&occupied,SOCK_DGRAM,AF_INET,false);
        assert(probe!=(platform_socket)EVUTIL_INVALID_SOCKET && occupied==port);
        assert(!airptp_daemon_bind("0.0.0.0"));
        assert(airptp_errmsg_get() && strstr(airptp_errmsg_get(),"Could not bind"));
        assert(!platform_socket_close(probe));
    }
    reject_bind=true;
    assert(!airptp_daemon_bind("0.0.0.0"));
    assert(platform_last_failure()->code==EACCES && !strcmp(platform_last_failure()->operation,"ptp-bind"));
    reject_bind=false;
    for(unsigned registration=1;registration<=4;++registration) {
        daemon=airptp_daemon_bind("0.0.0.0"); assert(daemon);
        event_add_calls=0; reject_event_add_at=registration;
        assert(airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)<0);
        assert(strstr(airptp_errmsg_get(),"Error"));
        reject_event_add_at=0;
        assert(!airptp_daemon_start(daemon,TEST_CLOCK_SEED,false));
        airptp_end(daemon);
    }
    daemon=airptp_daemon_bind("0.0.0.0"); assert(daemon);
    event_calls=0; reject_event_at=4;
    assert(airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)<0);
    reject_event_at=0; reject_thread=true;
    assert(airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)<0);
    reject_thread=false;
    assert(!airptp_daemon_start(daemon,TEST_CLOCK_SEED,false));
    airptp_end(daemon);
    for(unsigned failure=0;failure<2;++failure) {
        daemon=airptp_daemon_bind("0.0.0.0"); assert(daemon);
        reject_start_send=failure==0; reject_start_recv=failure==1;
        assert(airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)<0);
        assert(strstr(airptp_errmsg_get(),"Error reading thread start result"));
        reject_start_send=reject_start_recv=false;
        assert(!airptp_daemon_start(daemon,TEST_CLOCK_SEED,false));
        airptp_end(daemon);
    }
    for(unsigned failure=0;failure<2;++failure) {
        daemon=airptp_daemon_bind("0.0.0.0"); assert(daemon);
        reject_thread=failure==0; reject_event=failure==1;
        assert(airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)<0);
        reject_thread=reject_event=false;
        airptp_end(daemon);
        daemon=airptp_daemon_bind("0.0.0.0"); assert(daemon);
        assert(!airptp_daemon_start(daemon,TEST_CLOCK_SEED,false));
        airptp_end(daemon);
    }
    struct airptp_handle *standard=airptp_daemon_bind(NULL);
    if(standard) {
        airptp_end(standard);
        puts("standard PTP ports bound and released with sufficient privileges");
    } else {
        assert(strstr(airptp_errmsg_get(),"privileges"));
        puts("standard PTP port rejection reports privileges requirement");
    }

    daemon=airptp_daemon_bind("0.0.0.0"); assert(daemon);
    assert(!airptp_daemon_start(daemon,TEST_CLOCK_SEED,false));
    reject_wakeup=true; airptp_end(daemon); reject_wakeup=false;
    assert(log_contains("closing writer to wake daemon"));
    daemon=airptp_daemon_bind("0.0.0.0"); assert(daemon);
    assert(!airptp_daemon_start(daemon,TEST_CLOCK_SEED,false)); airptp_end(daemon);

    platform_cleanup();
    puts("ptp port occupancy, start/stop/restart cycles, clock identity, peer rejection and release cleanup verified");
    return 0;
}
