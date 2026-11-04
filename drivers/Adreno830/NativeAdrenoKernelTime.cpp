/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoKernelTime.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
int NativeAdrenoKernelTime::now_us(void *cookie,uint64_t *value)
{
    if(cookie || !value)return 1;
    *value=monotonic_us();
    return 0;
}
int NativeAdrenoKernelTime::wait_us(void *cookie,unsigned microseconds)
{
    if(cookie)return 1;
    IODelay(microseconds);
    return 0;
}
void NativeAdrenoKernelTime::sleep_us(unsigned microseconds)
{
    /* delay_for_interval blocks the thread on a timer when the interval is long enough and preemption is enabled, and
     * spins otherwise: a 100 us poll step gives the CPU away. */
    delay_for_interval(microseconds,1000);
}
void NativeAdrenoKernelTime::spin_us(unsigned microseconds)
{
    uint64_t interval=0;
    nanoseconds_to_absolutetime(uint64_t(microseconds)*1000,&interval);
    const uint64_t deadline=mach_absolute_time()+interval;
    while(mach_absolute_time()<deadline)__asm__ volatile("yield");
}
uint64_t NativeAdrenoKernelTime::monotonic_us()
{
    uint64_t nanoseconds=0;
    absolutetime_to_nanoseconds(mach_absolute_time(),&nanoseconds);
    return nanoseconds/1000;
}
uint64_t NativeAdrenoKernelTime::counter()
{
    return mach_absolute_time();
}
uintptr_t NativeAdrenoKernelTime::interruptsOff()
{
    return uintptr_t(ml_set_interrupts_enabled(FALSE));
}
void NativeAdrenoKernelTime::interruptsRestore(uintptr_t state)
{
    (void)ml_set_interrupts_enabled(boolean_t(state));
}
