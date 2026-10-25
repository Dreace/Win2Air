#include "platform.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <bcrypt.h>
static bool fail_random;
NTSTATUS WINAPI __real_BCryptGenRandom(BCRYPT_ALG_HANDLE,PUCHAR,ULONG,ULONG);
NTSTATUS WINAPI __wrap_BCryptGenRandom(BCRYPT_ALG_HANDLE handle,PUCHAR data,ULONG length,ULONG flags)
{ return fail_random ? (NTSTATUS)0xc0000022 : __real_BCryptGenRandom(handle,data,length,flags); }
#else
#include <sys/random.h>
static bool fail_random;
ssize_t __real_getrandom(void *,size_t,unsigned);
ssize_t __wrap_getrandom(void *data,size_t length,unsigned flags)
{ if(fail_random) { errno=EPERM; return -1; } return __real_getrandom(data,length,flags); }
#endif
static bool fail_init;
int __real_sodium_init(void);
int __wrap_sodium_init(void) { return fail_init ? -1 : __real_sodium_init(); }
struct context { platform_socket pair[2]; struct event_base *base; platform_mutex mutex; unsigned writes,reads,cancelled; };
static void *worker(void *argument)
{
    struct context *context=argument;
    assert(platform_mutex_lock(&context->mutex)==0); ++context->writes; assert(platform_mutex_unlock(&context->mutex)==0);
    uint8_t byte=42; assert(platform_send(context->pair[1],&byte,1,0)==1); return NULL;
}
static void incoming(evutil_socket_t fd,short flags,void *argument)
{
    (void)flags; struct context *context=argument; uint8_t byte;
    assert(platform_recv(fd,&byte,1,0)==1 && byte==42); ++context->reads; event_base_loopbreak(context->base);
}
static void cancelled(evutil_socket_t fd,short flags,void *argument)
{ (void)fd; (void)flags; ++((struct context *)argument)->cancelled; }
int main(void)
{
    fail_init=true; assert(platform_init()<0); fail_init=false; assert(platform_init()==0);
    struct context context={.pair={-1,-1},.mutex=PLATFORM_MUTEX_INIT};
    context.base=event_base_new(); assert(context.base && platform_wakeup_pair(context.pair)==0);
    struct event *read=event_new(context.base,context.pair[0],EV_READ,incoming,&context);
    struct event *timer=evtimer_new(context.base,cancelled,&context); assert(read && timer);
    struct timeval immediate={0,0},limit={1,0};
    assert(event_add(timer,&immediate)==0 && event_del(timer)==0 && event_add(read,NULL)==0);
    platform_thread thread; assert(platform_thread_start(&thread,worker,&context)==0);
    assert(event_base_loopexit(context.base,&limit)==0 && event_base_dispatch(context.base)==0);
    assert(platform_thread_join(thread)==0 && context.reads==1 && context.writes==1 && context.cancelled==0);
    uint8_t random[32]; assert(platform_random(random,sizeof(random))==0);
    fail_random=true; memset(random,42,sizeof(random)); assert(platform_random(random,sizeof(random))<0);
    for(size_t i=0;i<sizeof(random);++i) assert(random[i]==0);
    assert(platform_last_failure()->code && !strcmp(platform_last_failure()->operation,"secure-random"));
    fail_random=false;
    event_free(timer); event_free(read); event_base_free(context.base);
    assert(platform_socket_close(context.pair[0])==0 && platform_socket_close(context.pair[1])==0);
    platform_cleanup(); puts("initialization rollback, joined worker, cross-thread wakeup, timer cancellation and random failure clearing verified"); return 0;
}
