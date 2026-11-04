/* SPDX-License-Identifier: MIT */
#ifndef A830_SCHED_H
#define A830_SCHED_H
#include <stddef.h>
#include <stdint.h>

/* The XNU scheduling services the interrupt, fence and self-test code uses (ARCHITECTURE 4.12.3, 4.13.3), behind one small
 * contract so the host tests can run the same code on threads:
 *   - wait events: a830AssertWait (assert_wait_deadline, THREAD_ABORTSAFE), a830Block (thread_block), a830ClearWait
 *     (clear_wait on the current thread, THREAD_AWAKENED), a830Wakeup (thread_wakeup: every waiter of the event; callable
 *     from the primary interrupt context);
 *   - thread calls: THREAD_CALL_PRIORITY_KERNEL_HIGH with THREAD_CALL_OPTIONS_ONCE (an entry while the call runs re-runs it
 *     once after it returns, never concurrently); a830CallEnter is callable from the primary interrupt context;
 *   - time: mach absolute time and its conversions; a830SleepUs sleeps (thread context), a830DelayUs spins.
 * In the kernel every function is the XNU call itself, inline. With ADRENO830_HOST_TEST (the host tests) they are declared
 * here and implemented by tests/A830HostSched.cpp. */

#include <IOKit/IOTypes.h>
/* wait_result_t values (kern/kern_types.h). */
enum : int {A830WaitWaiting=-1,A830WaitAwakened=0,A830WaitTimedOut=1,A830WaitInterrupted=2,A830WaitRestart=3};
typedef void (*A830CallFn)(void *param0,void *param1);

#ifndef ADRENO830_HOST_TEST
/* The IOReturn codes these modules return beyond the common ones (the host fixture defines only the common ones). */
static constexpr IOReturn kA830NotResponding=kIOReturnNotResponding,kA830Aborted=kIOReturnAborted,
                          kA830ExclusiveAccess=kIOReturnExclusiveAccess;
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
static_assert(A830WaitWaiting==THREAD_WAITING && A830WaitAwakened==THREAD_AWAKENED && A830WaitTimedOut==THREAD_TIMED_OUT &&
              A830WaitInterrupted==THREAD_INTERRUPTED && A830WaitRestart==THREAD_RESTART,"wait results are XNU's");
typedef struct thread_call A830Call;
static inline uint64_t a830Now(void) {return mach_absolute_time();}
static inline uint64_t a830NsToAbs(uint64_t ns) {uint64_t abs=0;nanoseconds_to_absolutetime(ns,&abs);return abs;}
static inline uint64_t a830AbsToNs(uint64_t abs) {uint64_t ns=0;absolutetime_to_nanoseconds(abs,&ns);return ns;}
static inline int a830AssertWait(void *event,uint64_t deadline)
{
    return assert_wait_deadline(static_cast<event_t>(event),THREAD_ABORTSAFE,deadline);
}
static inline int a830Block(void) {return thread_block(THREAD_CONTINUE_NULL);}
static inline void a830ClearWait(void) {clear_wait(current_thread(),THREAD_AWAKENED);}
static inline void a830Wakeup(void *event) {thread_wakeup(static_cast<event_t>(event));}
static inline A830Call *a830CallAllocate(A830CallFn function,void *param0)
{
    return thread_call_allocate_with_options(function,param0,THREAD_CALL_PRIORITY_KERNEL_HIGH,THREAD_CALL_OPTIONS_ONCE);
}
static inline bool a830CallEnter(A830Call *call) {return thread_call_enter(call);}
static inline bool a830CallEnterDelayed(A830Call *call,uint64_t deadline) {return thread_call_enter_delayed(call,deadline);}
static inline bool a830CallCancel(A830Call *call) {return thread_call_cancel(call);}
static inline void a830SleepUs(uint32_t microseconds)
{
    if(microseconds>=1000)IOSleep(microseconds/1000);
    if(microseconds%1000)IODelay(microseconds%1000);
}
static inline void a830DelayUs(uint32_t microseconds) {IODelay(microseconds);}
#else
static constexpr IOReturn kA830NotResponding=0x2ed,kA830Aborted=0x2eb,kA830ExclusiveAccess=0x2c5;
struct A830Call;
uint64_t a830Now(void);
uint64_t a830NsToAbs(uint64_t ns);
uint64_t a830AbsToNs(uint64_t abs);
int a830AssertWait(void *event,uint64_t deadline);
int a830Block(void);
void a830ClearWait(void);
void a830Wakeup(void *event);
A830Call *a830CallAllocate(A830CallFn function,void *param0);
bool a830CallEnter(A830Call *call);
bool a830CallEnterDelayed(A830Call *call,uint64_t deadline);
bool a830CallCancel(A830Call *call);
void a830SleepUs(uint32_t microseconds);
void a830DelayUs(uint32_t microseconds);
#endif

/* A deadline timeout_ns from now; UINT64_MAX stays "never". */
static inline uint64_t a830DeadlineAfterNs(uint64_t timeout_ns)
{
    if(timeout_ns==UINT64_MAX)return UINT64_MAX;
    const uint64_t now=a830Now(),delta=a830NsToAbs(timeout_ns);
    return delta>UINT64_MAX-now?UINT64_MAX:now+delta;
}
#endif
