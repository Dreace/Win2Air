#include "platform.h"
#include <errno.h>
int platform_time(enum platform_clock clock,struct timespec *value)
{
    if(!value) return platform_fail("clock-read",EINVAL);
    uint64_t ns;
    if(clock==PLATFORM_MONOTONIC) {
        LARGE_INTEGER ticks,frequency;
        if(!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency) || ticks.QuadPart<0 || frequency.QuadPart<=0) return platform_fail("qpc-read",(int)GetLastError());
        if(platform_scale((uint64_t)ticks.QuadPart,(uint64_t)frequency.QuadPart,&ns)<0) return -1;
    } else if(clock==PLATFORM_WALL) {
        FILETIME time; GetSystemTimePreciseAsFileTime(&time);
        uint64_t ticks=((uint64_t)time.dwHighDateTime<<32)|time.dwLowDateTime;
        const uint64_t epoch=UINT64_C(116444736000000000);
        if(ticks<epoch || ticks-epoch>UINT64_MAX/100) return platform_fail("wall-clock-range",ERANGE);
        ns=(ticks-epoch)*100;
    } else return platform_fail("clock-read",EINVAL);
    value->tv_sec=(time_t)(ns/1000000000); value->tv_nsec=(long)(ns%1000000000); return 0;
}
