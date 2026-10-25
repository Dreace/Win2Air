#include "platform.h"
#include <errno.h>
#include <unistd.h>
#include <sys/random.h>
#include <sodium.h>
#include <uuid/uuid.h>
int platform_uuid(char text[37]) { uuid_t id; uuid_generate_random(id); uuid_unparse_upper(id,text); return 0; }
int platform_init(void) { return sodium_init()<0 ? platform_fail("crypto-initialize",EIO) : 0; }
void platform_cleanup(void) { }
int platform_mutex_lock(platform_mutex *mutex) { return pthread_mutex_lock(mutex); }
int platform_mutex_unlock(platform_mutex *mutex) { return pthread_mutex_unlock(mutex); }
int platform_call_once(platform_once *once,void (*callback)(void)) { return pthread_once(once,callback); }
int platform_thread_start(platform_thread *thread,void *(*callback)(void *),void *context)
{
    if(!thread || !callback) return platform_fail("thread-create",EINVAL);
    int status=pthread_create(thread,NULL,callback,context); return status ? platform_fail("thread-create",status) : 0;
}
int platform_thread_join(platform_thread thread)
{ int status=pthread_join(thread,NULL); return status ? platform_fail("thread-join",status) : 0; }
void platform_thread_name(const char *name) { pthread_setname_np(pthread_self(),name); }
int platform_random(void *data,size_t length)
{
    if(!data || !length) return platform_fail("secure-random",EINVAL);
    size_t offset=0;
    while(offset<length) {
        ssize_t n=getrandom((uint8_t *)data+offset,length-offset,0);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { sodium_memzero(data,length); return platform_fail("secure-random",n<0 ? errno : EIO); }
        offset+=(size_t)n;
    }
    return 0;
}
