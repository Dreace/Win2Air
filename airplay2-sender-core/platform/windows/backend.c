#include "platform.h"
#include <errno.h>
#include <sodium.h>
int platform_init(void)
{
    WSADATA data; int status=WSAStartup(MAKEWORD(2,2),&data);
    if(status) return platform_fail("winsock-initialize",status);
    if(data.wVersion!=MAKEWORD(2,2)) { WSACleanup(); return platform_fail("winsock-version",WSAVERNOTSUPPORTED); }
    if(sodium_init()<0) { WSACleanup(); return platform_fail("crypto-initialize",EIO); }
    return 0;
}
void platform_cleanup(void) { WSACleanup(); }
