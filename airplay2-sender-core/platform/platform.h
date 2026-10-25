#ifndef SENDER_PLATFORM_H
#define SENDER_PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SRWLOCK platform_mutex;
typedef INIT_ONCE platform_once;
typedef HANDLE platform_thread;
#define PLATFORM_MUTEX_INIT SRWLOCK_INIT
#define PLATFORM_ONCE_INIT INIT_ONCE_STATIC_INIT
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
typedef pthread_mutex_t platform_mutex;
typedef pthread_once_t platform_once;
typedef pthread_t platform_thread;
#define PLATFORM_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define PLATFORM_ONCE_INIT PTHREAD_ONCE_INIT
#endif
#include <event2/event.h>
#include <event2/util.h>
typedef evutil_socket_t platform_socket;
static inline bool platform_socket_retryable(int code)
{
#ifdef _WIN32
    return code==WSAEINTR || code==WSAEWOULDBLOCK;
#else
    return code==EINTR || code==EAGAIN || code==EWOULDBLOCK;
#endif
}
static inline uint64_t platform_to_be64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}
enum platform_clock { PLATFORM_MONOTONIC, PLATFORM_WALL };
struct platform_failure { char operation[64],interface_name[256]; int code,stage; uint64_t generation; };
struct platform_interface { char name[256],address[INET6_ADDRSTRLEN]; unsigned index; int family; uint8_t mac[6]; };
int platform_init(void);
void platform_cleanup(void);
void platform_context(uint64_t generation,int stage,const char *interface_name);
const struct platform_failure *platform_last_failure(void);
int platform_fail(const char *operation,int code);
platform_socket platform_socket_open(int family,int type,int protocol);
int platform_socket_close(platform_socket fd);
int platform_socket_configure(platform_socket fd,bool nonblocking);
int platform_socket_reuseport(platform_socket fd,bool enabled);
platform_socket platform_target_socket(const char *address,unsigned short port,int type,unsigned timeout_ms,bool nonblocking);
platform_socket platform_bound_socket(const char *address,unsigned short *port,int type,int family,bool reuseport);
int platform_socket_option(platform_socket fd,int level,int option,const void *value,socklen_t length);
int platform_socket_error(platform_socket fd,int *error);
int platform_bind(platform_socket fd,const struct sockaddr *address,socklen_t length);
int platform_connect(platform_socket fd,const struct sockaddr *address,socklen_t length);
int platform_wait(platform_socket fd,bool writable,unsigned timeout_ms);
ev_ssize_t platform_send(platform_socket fd,const void *data,size_t length,int flags);
ev_ssize_t platform_recv(platform_socket fd,void *data,size_t length,int flags);
ev_ssize_t platform_sendto(platform_socket fd,const void *data,size_t length,int flags,const struct sockaddr *address,socklen_t size);
ev_ssize_t platform_recvfrom(platform_socket fd,void *data,size_t length,int flags,struct sockaddr *address,socklen_t *size);
int platform_interfaces(struct platform_interface *items,size_t capacity,size_t *count);
unsigned platform_interface_index(const char *name);
int platform_interface_name(unsigned index,char *name,size_t capacity);
int platform_multicast(platform_socket fd,const char *group,unsigned index,bool join);
int platform_time(enum platform_clock clock,struct timespec *value);
int platform_scale(uint64_t ticks,uint64_t frequency,uint64_t *nanoseconds);
int platform_mutex_lock(platform_mutex *mutex);
int platform_mutex_unlock(platform_mutex *mutex);
int platform_call_once(platform_once *once,void (*callback)(void));
int platform_thread_start(platform_thread *thread,void *(*callback)(void *),void *context);
int platform_thread_join(platform_thread thread);
void platform_thread_name(const char *name);
int platform_random(void *data,size_t length);
int platform_uuid(char text[37]);
int platform_wakeup_pair(platform_socket pair[2]);
#endif
