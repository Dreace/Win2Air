#include "platform.h"
#include <errno.h>
#include <limits.h>
#include <mswsock.h>
static int socket_failure(const char *operation)
{
    int error=WSAGetLastError();
    errno=error==WSAEWOULDBLOCK ? EAGAIN : error==WSAEINPROGRESS ? EINPROGRESS : error==WSAECONNREFUSED ? ECONNREFUSED : error==WSAECONNRESET ? ECONNRESET : error==WSAEINTR ? EINTR : EIO;
    return platform_fail(operation,error);
}
platform_socket platform_socket_open(int family,int type,int protocol)
{
    SOCKET socket=WSASocketW(family,type,protocol,NULL,0,WSA_FLAG_NO_HANDLE_INHERIT);
    if(socket==INVALID_SOCKET) return socket_failure("socket-create");
    if(type==SOCK_DGRAM) {
        BOOL report_reset=FALSE;
        DWORD returned=0;
        if(WSAIoctl(socket,SIO_UDP_CONNRESET,&report_reset,sizeof(report_reset),NULL,0,&returned,NULL,NULL)==SOCKET_ERROR) {
            closesocket(socket);
            return socket_failure("socket-udp-reset");
        }
    }
    return (platform_socket)socket;
}
int platform_socket_close(platform_socket fd)
{ return closesocket((SOCKET)fd)==0 ? 0 : socket_failure("socket-close"); }
int platform_socket_configure(platform_socket fd,bool nonblocking)
{
    u_long flag=nonblocking ? 1 : 0;
    if(ioctlsocket((SOCKET)fd,FIONBIO,&flag)<0) return socket_failure("socket-configure");
    if(!SetHandleInformation((HANDLE)(uintptr_t)fd,HANDLE_FLAG_INHERIT,0)) return platform_fail("socket-inheritance",(int)GetLastError());
    return 0;
}
int platform_socket_option(platform_socket fd,int level,int option,const void *value,socklen_t length)
{ return setsockopt((SOCKET)fd,level,option,(const char *)value,length)==0 ? 0 : socket_failure("socket-option"); }
int platform_socket_reuseport(platform_socket fd,bool enabled)
{ if(enabled) return platform_fail("socket-reuseport",WSAEOPNOTSUPP); (void)fd; return 0; }
int platform_socket_error(platform_socket fd,int *error)
{
    if(!error) return platform_fail("socket-error",EINVAL);
    int length=sizeof(*error); return getsockopt((SOCKET)fd,SOL_SOCKET,SO_ERROR,(char *)error,&length)==0 ? 0 : socket_failure("socket-error");
}
int platform_bind(platform_socket fd,const struct sockaddr *address,socklen_t length)
{ return bind((SOCKET)fd,address,length)==0 ? 0 : socket_failure("socket-bind"); }
int platform_connect(platform_socket fd,const struct sockaddr *address,socklen_t length)
{ return connect((SOCKET)fd,address,length)==0 ? 0 : socket_failure("socket-connect"); }
int platform_wait(platform_socket fd,bool writable,unsigned timeout_ms)
{
    if(timeout_ms>INT_MAX) return platform_fail("socket-wait",EINVAL);
    WSAPOLLFD item={(SOCKET)fd,writable ? POLLWRNORM : POLLRDNORM,0}; int n=WSAPoll(&item,1,(int)timeout_ms);
    return n<0 ? socket_failure("socket-wait") : n;
}
ev_ssize_t platform_send(platform_socket fd,const void *data,size_t length,int flags)
{
    if(length>INT_MAX) return platform_fail("socket-send-size",EMSGSIZE);
    int n=send((SOCKET)fd,data,(int)length,flags); return n<0 ? socket_failure("socket-send") : n;
}
ev_ssize_t platform_recv(platform_socket fd,void *data,size_t length,int flags)
{
    if(length>INT_MAX) return platform_fail("socket-receive-size",EMSGSIZE);
    int n=recv((SOCKET)fd,data,(int)length,flags); return n<0 ? socket_failure("socket-receive") : n;
}
ev_ssize_t platform_sendto(platform_socket fd,const void *data,size_t length,int flags,const struct sockaddr *address,socklen_t size)
{
    if(length>INT_MAX) return platform_fail("datagram-send-size",EMSGSIZE);
    int n=sendto((SOCKET)fd,data,(int)length,flags,address,size); return n<0 ? socket_failure("datagram-send") : n;
}
ev_ssize_t platform_recvfrom(platform_socket fd,void *data,size_t length,int flags,struct sockaddr *address,socklen_t *size)
{
    if(length>INT_MAX) return platform_fail("datagram-receive-size",EMSGSIZE);
    int n=recvfrom((SOCKET)fd,data,(int)length,flags,address,size); return n<0 ? socket_failure("datagram-receive") : n;
}
