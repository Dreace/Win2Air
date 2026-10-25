#include "platform.h"
#include <errno.h>
#include <stdlib.h>
#include <process.h>
#include <bcrypt.h>
#include <sodium.h>
#include <stdio.h>
int platform_uuid(char text[37])
{
    uint8_t bytes[16]; if(platform_random(bytes,sizeof(bytes))<0) return -1;
    bytes[6]=(bytes[6]&15)|64; bytes[8]=(bytes[8]&63)|128;
    char *cursor=text;
    for(unsigned i=0;i<16;++i) { if(i==4 || i==6 || i==8 || i==10) *cursor++='-'; snprintf(cursor,3,"%02X",bytes[i]); cursor+=2; }
    return 0;
}
int platform_mutex_lock(platform_mutex *mutex) { AcquireSRWLockExclusive(mutex); return 0; }
int platform_mutex_unlock(platform_mutex *mutex) { ReleaseSRWLockExclusive(mutex); return 0; }
struct once_context { void (*callback)(void); };
static BOOL CALLBACK once_callback(PINIT_ONCE once,PVOID argument,PVOID *result)
{ (void)once; (void)result; ((struct once_context *)argument)->callback(); return TRUE; }
int platform_call_once(platform_once *once,void (*callback)(void))
{
    struct once_context context={callback};
    return InitOnceExecuteOnce(once,once_callback,&context,NULL) ? 0 : platform_fail("initialize-once",(int)GetLastError());
}
struct thread_context { void *(*callback)(void *); void *argument; };
static unsigned __stdcall thread_callback(void *argument)
{
    struct thread_context context=*(struct thread_context *)argument; free(argument); context.callback(context.argument); return 0;
}
int platform_thread_start(platform_thread *thread,void *(*callback)(void *),void *argument)
{
    if(!thread || !callback) return platform_fail("thread-create",EINVAL);
    struct thread_context *context=malloc(sizeof(*context)); if(!context) return platform_fail("thread-allocate",ENOMEM);
    *context=(struct thread_context){callback,argument}; uintptr_t handle=_beginthreadex(NULL,0,thread_callback,context,0,NULL);
    if(!handle) { int status=errno; free(context); return platform_fail("thread-create",status); }
    *thread=(HANDLE)handle; return 0;
}
int platform_thread_join(platform_thread thread)
{
    if(WaitForSingleObject(thread,INFINITE)!=WAIT_OBJECT_0) return platform_fail("thread-join",(int)GetLastError());
    if(!CloseHandle(thread)) return platform_fail("thread-close",(int)GetLastError());
    return 0;
}
void platform_thread_name(const char *name) { (void)name; }
int platform_random(void *data,size_t length)
{
    if(!data || !length || length>ULONG_MAX) return platform_fail("secure-random",EINVAL);
    NTSTATUS status=BCryptGenRandom(NULL,data,(ULONG)length,BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if(status<0) { sodium_memzero(data,length); return platform_fail("secure-random",(int)status); }
    return 0;
}
