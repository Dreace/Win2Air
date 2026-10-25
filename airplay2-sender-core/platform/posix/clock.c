#include "platform.h"
#include <errno.h>
int platform_time(enum platform_clock clock,struct timespec *value)
{
    if(!value || (clock!=PLATFORM_MONOTONIC && clock!=PLATFORM_WALL)) return platform_fail("clock-read",EINVAL);
    if(clock_gettime(clock==PLATFORM_MONOTONIC ? CLOCK_MONOTONIC : CLOCK_REALTIME,value)<0) return platform_fail("clock-read",errno);
    return 0;
}
