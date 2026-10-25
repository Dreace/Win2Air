#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
int main(void)
{
    assert(platform_init()==0);
    uint64_t ns;
    assert(platform_scale(3,2,&ns)==0 && ns==UINT64_C(1500000000));
    assert(platform_scale(UINT64_MAX,UINT64_MAX,&ns)==0 && ns==1000000000);
    assert(platform_scale(UINT64_MAX,1,&ns)<0);
    assert(platform_scale(1,0,&ns)<0);
    struct timespec before,after,wall;
    assert(platform_time(PLATFORM_MONOTONIC,&before)==0);
    assert(platform_time(PLATFORM_WALL,&wall)==0);
    assert(platform_time(PLATFORM_MONOTONIC,&after)==0);
    assert(after.tv_sec>before.tv_sec || (after.tv_sec==before.tv_sec && after.tv_nsec>=before.tv_nsec));
    assert(before.tv_nsec>=0 && before.tv_nsec<1000000000 && wall.tv_nsec>=0 && wall.tv_nsec<1000000000);
    assert(wall.tv_sec>1577836800);
    platform_context(42,3,"clock-test");
    assert(platform_time((enum platform_clock)99,&after)<0);
    const struct platform_failure *failure=platform_last_failure();
    assert(failure->code && failure->generation==42 && failure->stage==3);
    platform_cleanup(); puts("monotonic and wall clock domains, rational conversion and overflow rejection verified");
    return 0;
}
