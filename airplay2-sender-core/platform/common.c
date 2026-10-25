#include "platform.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static _Thread_local struct platform_failure failure;
void platform_context(uint64_t generation,int stage,const char *interface_name)
{
    memset(&failure,0,sizeof(failure)); failure.generation=generation; failure.stage=stage;
    if(interface_name) snprintf(failure.interface_name,sizeof(failure.interface_name),"%s",interface_name);
}
const struct platform_failure *platform_last_failure(void) { return &failure; }
int platform_fail(const char *operation,int code)
{
    failure.code=code; snprintf(failure.operation,sizeof(failure.operation),"%s",operation);
    return -1;
}
int platform_scale(uint64_t ticks,uint64_t frequency,uint64_t *nanoseconds)
{
    if(!frequency || !nanoseconds) return platform_fail("clock-convert",EINVAL);
    __uint128_t result=(__uint128_t)ticks*UINT64_C(1000000000)/frequency;
    if(result>UINT64_MAX) return platform_fail("clock-overflow",ERANGE);
    *nanoseconds=(uint64_t)result; return 0;
}
int platform_interface_name(unsigned index,char *name,size_t capacity)
{
    if(!index || !name || !capacity) return platform_fail("interface-select",EINVAL);
    struct platform_interface items[256]; size_t count;
    if(platform_interfaces(items,256,&count)<0) return -1;
    for(size_t i=0;i<count;++i) if(items[i].index==index) {
        if(strlen(items[i].name)>=capacity) return platform_fail("interface-name-capacity",ENOSPC);
        memcpy(name,items[i].name,strlen(items[i].name)+1); return 0;
    }
    return platform_fail("interface-select",ENODEV);
}
int platform_wakeup_pair(platform_socket pair[2])
{
    if(!pair) return platform_fail("event-wakeup-create",EINVAL);
    if(evutil_socketpair(AF_UNIX,SOCK_STREAM,0,pair)<0) return platform_fail("event-wakeup-create",EVUTIL_SOCKET_ERROR());
    if(platform_socket_configure(pair[0],true)<0 || platform_socket_configure(pair[1],true)<0) {
        struct platform_failure saved=failure; platform_socket_close(pair[0]); platform_socket_close(pair[1]);
        pair[0]=pair[1]=-1; failure=saved; return -1;
    }
    return 0;
}
