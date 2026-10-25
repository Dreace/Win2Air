#include "sender.h"
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <iphlpapi.h>

static bool adapter_disappeared;
ULONG WINAPI __real_GetAdaptersAddresses(ULONG,ULONG,PVOID,PIP_ADAPTER_ADDRESSES,PULONG);
ULONG WINAPI __wrap_GetAdaptersAddresses(ULONG family,ULONG flags,PVOID reserved,PIP_ADAPTER_ADDRESSES addresses,PULONG size)
{
    return adapter_disappeared ? ERROR_NO_DATA : __real_GetAdaptersAddresses(family,flags,reserved,addresses,size);
}

static sender_error observed;
static unsigned failures;
static void changed(void *context,uint64_t generation,sender_state state,const sender_error *error)
{
    (void)context;
    if(state==SENDER_FAILED) {
        assert(error && error->generation==generation && error->operation[0] && error->message[0]);
        observed=*error;
        ++failures;
    }
}
static void await_failure(sender_runtime *runtime,uint64_t generation)
{
    sender_error error;
    for(unsigned i=0;i<250 && sender_get_state(runtime,generation)!=SENDER_FAILED;++i)
        assert(sender_poll(runtime,20,&error)==SENDER_OK);
    assert(sender_get_state(runtime,generation)==SENDER_FAILED);
    assert(sender_poll(runtime,0,&error)==SENDER_OK);
    assert(observed.generation==generation && observed.code==SENDER_NETWORK && observed.stage==SENDER_CONNECTING);
}
static void *disconnect_peer(void *context)
{
    platform_socket listener=*(platform_socket *)context;
    platform_socket fd=accept(listener,NULL,NULL); assert(fd>=0);
    char request[4096];
    assert(platform_wait(fd,false,1000)>0);
    assert(platform_recv(fd,request,sizeof(request),0)>0);
    assert(!platform_socket_close(fd));
    return NULL;
}
int main(void)
{
    assert(!platform_init());
    sender_options options={UINT64_C(0x12345678),"Fault sender","OwnTone/29.3",8192,changed,NULL,NULL};
    sender_runtime *runtime,*other; sender_error error;
    assert(sender_create(&options,&runtime,&error)==SENDER_OK);
    assert(sender_create(&options,&other,&error)==SENDER_OK);
    sender_audio_format format={44100,16,2};
    sender_target target={"127.0.0.1",1,"AA:BB:CC:DD:EE:FF","Fault peer",
        (UINT64_C(1)<<9)|(UINT64_C(1)<<41)|(UINT64_C(1)<<48),0,50};
    uint64_t generation=0,next=0;
    target.address="invalid-target";
    assert(sender_connect(runtime,&target,&format,NULL,NULL,&generation,&error)==SENDER_INPUT && !generation);
    target.address="fe80::1"; target.scope_id=UINT32_MAX;
    assert(sender_connect(runtime,&target,&format,NULL,NULL,&generation,&error)==SENDER_INPUT && !generation);
    assert(!strcmp(platform_last_failure()->operation,"interface-select") && platform_last_failure()->code);
    assert(error.platform_code==platform_last_failure()->code && strstr(error.message,"4294967295"));
    struct platform_interface interfaces[128]; size_t count;
    assert(platform_interfaces(interfaces,128,&count)==0);
    unsigned ipv6_interface=0;
    for(size_t i=0;i<count;++i) if(interfaces[i].family==AF_INET6 && !strcmp(interfaces[i].address,"::1")) ipv6_interface=interfaces[i].index;
    assert(ipv6_interface);
    target.address="::1"; target.scope_id=ipv6_interface;
    adapter_disappeared=true;
    assert(sender_connect(runtime,&target,&format,NULL,NULL,&generation,&error)==SENDER_INPUT && !generation);
    assert(error.platform_code==ERROR_NO_DATA && !strcmp(platform_last_failure()->operation,"interface-enumerate"));
    adapter_disappeared=false;
    target.address="127.0.0.1"; target.scope_id=0;
    unsigned short port=0;
    platform_socket reserved=platform_bound_socket(target.address,&port,SOCK_STREAM,AF_INET,false); assert(reserved>=0);
    target.control_port=port;
    assert(sender_connect(runtime,&target,&format,NULL,NULL,&generation,&error)==SENDER_OK);
    assert(sender_connect(other,&target,&format,NULL,NULL,&next,&error)==SENDER_OCCUPIED);
    await_failure(runtime,generation);
    assert(observed.platform_code==WSAECONNREFUSED && !observed.peer_code);
    assert(!platform_socket_close(reserved));
    port=0;
    platform_socket listener=platform_bound_socket(target.address,&port,SOCK_STREAM,AF_INET,false); assert(listener>=0);
    assert(!platform_socket_configure(listener,false) && !listen(listener,1));
    platform_thread worker; assert(!platform_thread_start(&worker,disconnect_peer,&listener));
    target.control_port=port;
    assert(sender_connect(other,&target,&format,NULL,NULL,&next,&error)==SENDER_OK && next>generation);
    assert(sender_flush(other,generation,&error)==SENDER_STATE);
    await_failure(other,next);
    assert(!observed.peer_code);
    assert(!platform_thread_join(worker) && !platform_socket_close(listener));
    assert(sender_connect(runtime,&target,&format,NULL,NULL,&generation,&error)==SENDER_OK && generation>next);
    assert(sender_end(runtime,generation,&error)==SENDER_OK);
    assert(sender_connect(other,&target,&format,NULL,NULL,&next,&error)==SENDER_OCCUPIED);
    assert(sender_poll(runtime,0,&error)==SENDER_OK && sender_get_state(runtime,generation)==SENDER_ENDED);
    assert(sender_connect(other,&target,&format,NULL,NULL,&next,&error)==SENDER_OK && next>generation);
    assert(sender_end(other,next,&error)==SENDER_OK && sender_poll(other,0,&error)==SENDER_OK);
    assert(sender_destroy(runtime,&error)==SENDER_OK && sender_destroy(other,&error)==SENDER_OK && failures==2);
    platform_cleanup();
    puts("invalid target, missing interface, disappearance of a captured live interface, native connection refusal, peer disconnect, stale generation, ending exclusion and released ownership verified");
    return 0;
}
