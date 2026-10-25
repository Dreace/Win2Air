#include "platform.h"
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
platform_socket platform_socket_open(int family,int type,int protocol)
{ int fd=socket(family,type,protocol); if(fd<0) platform_fail("socket-create",errno); return fd; }
int platform_socket_close(platform_socket fd)
{ if(close(fd)<0) return platform_fail("socket-close",errno); return 0; }
int platform_socket_configure(platform_socket fd,bool nonblocking)
{
    int flags=fcntl(fd,F_GETFL,0);
    if(flags<0 || fcntl(fd,F_SETFL,nonblocking ? flags|O_NONBLOCK : flags&~O_NONBLOCK)<0 || evutil_make_socket_closeonexec(fd)<0) return platform_fail("socket-configure",errno);
    return 0;
}
int platform_socket_option(platform_socket fd,int level,int option,const void *value,socklen_t length)
{ if(setsockopt(fd,level,option,value,length)<0) return platform_fail("socket-option",errno); return 0; }
int platform_socket_reuseport(platform_socket fd,bool enabled)
{ int value=enabled; return platform_socket_option(fd,SOL_SOCKET,SO_REUSEPORT,&value,sizeof(value)); }
int platform_socket_error(platform_socket fd,int *error)
{ socklen_t length=sizeof(*error); if(!error || getsockopt(fd,SOL_SOCKET,SO_ERROR,error,&length)<0) return platform_fail("socket-error",errno); return 0; }
int platform_bind(platform_socket fd,const struct sockaddr *address,socklen_t length)
{ if(bind(fd,address,length)<0) return platform_fail("socket-bind",errno); return 0; }
int platform_connect(platform_socket fd,const struct sockaddr *address,socklen_t length)
{ if(connect(fd,address,length)<0) return platform_fail("socket-connect",errno); return 0; }
int platform_wait(platform_socket fd,bool writable,unsigned timeout_ms)
{
    if(timeout_ms>INT32_MAX) return platform_fail("socket-wait",EINVAL);
    struct pollfd item={fd,writable ? POLLOUT : POLLIN,0}; int n=poll(&item,1,(int)timeout_ms);
    if(n<0) return platform_fail("socket-wait",errno);
    return n;
}
ev_ssize_t platform_send(platform_socket fd,const void *data,size_t length,int flags)
{ ssize_t n=send(fd,data,length,flags); if(n<0) platform_fail("socket-send",errno); return n; }
ev_ssize_t platform_recv(platform_socket fd,void *data,size_t length,int flags)
{ ssize_t n=recv(fd,data,length,flags); if(n<0) platform_fail("socket-receive",errno); return n; }
ev_ssize_t platform_sendto(platform_socket fd,const void *data,size_t length,int flags,const struct sockaddr *address,socklen_t size)
{ ssize_t n=sendto(fd,data,length,flags,address,size); if(n<0) platform_fail("datagram-send",errno); return n; }
ev_ssize_t platform_recvfrom(platform_socket fd,void *data,size_t length,int flags,struct sockaddr *address,socklen_t *size)
{ ssize_t n=recvfrom(fd,data,length,flags,address,size); if(n<0) platform_fail("datagram-receive",errno); return n; }
