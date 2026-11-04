/* SPDX-License-Identifier: MIT */
/* Host double of A830Sched.h (ADRENO830_HOST_TEST): the XNU wait-event and thread-call contract on std::thread, written for
 * the A830Irq/A830Fence/A830SelfTest tests. Semantics kept from XNU:
 *   - a830AssertWait registers the calling thread on the event with a deadline; a830Wakeup wakes every registered waiter
 *     (also before it blocks, so a830Block then returns at once); a830ClearWait withdraws the registration; a830Block
 *     returns AWAKENED, TIMED_OUT or INTERRUPTED (a830HostAbort, THREAD_ABORTSAFE);
 *   - thread calls run on one worker each (THREAD_CALL_OPTIONS_ONCE: never concurrently with themselves; an entry while the
 *     call runs makes it run once more afterwards); enter of a pending call returns true; cancel only stops a call that has
 *     not started. Absolute time is steady-clock nanoseconds. */
#include "A830HostSched.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;

struct Waiter {
    std::thread::id id;
    void *event=nullptr;
    uint64_t deadline=0;
    int result=A830WaitAwakened;
    bool waiting=false,abort=false;
};

std::mutex &lock() {static std::mutex m;return m;}
std::condition_variable &changed() {static std::condition_variable c;return c;}
std::vector<Waiter *> &waiters() {static std::vector<Waiter *> v;return v;}
/* Aborts aimed at threads that have not waited yet (they register on their first wait). */
std::vector<std::thread::id> &pendingAborts() {static std::vector<std::thread::id> v;return v;}

struct Registered {
    Waiter waiter;
    Registered()
    {
        waiter.id=std::this_thread::get_id();
        std::lock_guard<std::mutex> g(lock());
        waiters().push_back(&waiter);
        auto &p=pendingAborts();
        auto at=std::find(p.begin(),p.end(),waiter.id);
        if(at!=p.end()) {waiter.abort=true;p.erase(at);}
    }
    ~Registered()
    {
        std::lock_guard<std::mutex> g(lock());
        auto &v=waiters();
        v.erase(std::remove(v.begin(),v.end(),&waiter),v.end());
    }
};
Waiter &self() {thread_local Registered registered;return registered.waiter;}

Clock::time_point at(uint64_t abs)
{
    if(abs>uint64_t(INT64_MAX)/2)return Clock::time_point::max();
    return Clock::time_point(std::chrono::nanoseconds(abs));
}
}

uint64_t a830Now(void) {return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());}
uint64_t a830NsToAbs(uint64_t ns) {return ns;}
uint64_t a830AbsToNs(uint64_t abs) {return abs;}

int a830AssertWait(void *event,uint64_t deadline)
{
    Waiter &w=self();
    std::lock_guard<std::mutex> g(lock());
    if(w.abort) {w.abort=false;w.waiting=false;w.result=A830WaitInterrupted;return A830WaitInterrupted;}
    w.event=event;w.deadline=deadline;w.waiting=true;w.result=A830WaitWaiting;
    return A830WaitWaiting;
}

int a830Block(void)
{
    Waiter &w=self();
    std::unique_lock<std::mutex> g(lock());
    while(w.waiting) {
        if(w.abort) {w.abort=false;w.waiting=false;w.result=A830WaitInterrupted;break;}
        if(a830Now()>=w.deadline) {w.waiting=false;w.result=A830WaitTimedOut;break;}
        changed().wait_until(g,at(w.deadline));
    }
    return w.result==A830WaitWaiting?A830WaitAwakened:w.result;
}

void a830ClearWait(void)
{
    Waiter &w=self();
    std::lock_guard<std::mutex> g(lock());
    w.waiting=false;w.result=A830WaitAwakened;
}

void a830Wakeup(void *event)
{
    std::lock_guard<std::mutex> g(lock());
    for(Waiter *w:waiters())
        if(w->waiting && w->event==event) {w->waiting=false;w->result=A830WaitAwakened;}
    changed().notify_all();
}

void a830HostAbort(std::thread::id thread)
{
    std::lock_guard<std::mutex> g(lock());
    bool found=false;
    for(Waiter *w:waiters())
        if(w->id==thread) {
            found=true;
            if(w->waiting) {w->waiting=false;w->result=A830WaitInterrupted;}
            else w->abort=true;
        }
    if(!found)pendingAborts().push_back(thread);
    changed().notify_all();
}

unsigned a830HostWaiting(void *event)
{
    std::lock_guard<std::mutex> g(lock());
    unsigned n=0;
    for(Waiter *w:waiters())if(w->waiting && w->event==event)++n;
    return n;
}

/* ---- thread calls ---- */
struct A830Call {
    A830CallFn function=nullptr;
    void *param0=nullptr;
    bool pending=false,running=false,stop=false;
    uint64_t deadline=0,runs=0;
    std::thread worker;
};

namespace {
std::vector<A830Call *> &calls() {static std::vector<A830Call *> v;return v;}
bool gManual=false;
uint64_t gDelayed=0;

void work(A830Call *call)
{
    std::unique_lock<std::mutex> g(lock());
    for(;;) {
        while(!call->stop && (gManual || !call->pending || a830Now()<call->deadline)) {
            if(!gManual && call->pending)changed().wait_until(g,at(call->deadline));
            else changed().wait(g);
        }
        if(call->stop)return;
        call->pending=false;call->running=true;
        g.unlock();
        call->function(call->param0,nullptr);
        g.lock();
        call->running=false;++call->runs;
        changed().notify_all();
    }
}
}

A830Call *a830CallAllocate(A830CallFn function,void *param0)
{
    A830Call *call=new A830Call;
    call->function=function;call->param0=param0;
    {
        std::lock_guard<std::mutex> g(lock());
        calls().push_back(call);
    }
    call->worker=std::thread(work,call);
    return call;
}

bool a830CallEnter(A830Call *call)
{
    std::lock_guard<std::mutex> g(lock());
    const bool was=call->pending;
    call->pending=true;call->deadline=0;
    changed().notify_all();
    return was;
}

bool a830CallEnterDelayed(A830Call *call,uint64_t deadline)
{
    std::lock_guard<std::mutex> g(lock());
    const bool was=call->pending;
    call->pending=true;call->deadline=deadline;
    changed().notify_all();
    return was;
}

bool a830CallCancel(A830Call *call)
{
    std::lock_guard<std::mutex> g(lock());
    const bool was=call->pending;
    call->pending=false;
    changed().notify_all();
    return was;
}

void a830SleepUs(uint32_t microseconds) {std::this_thread::sleep_for(std::chrono::microseconds(microseconds));}
void a830DelayUs(uint32_t microseconds)
{
    {std::lock_guard<std::mutex> g(lock());gDelayed+=microseconds;}
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

void a830HostManualCalls(bool manual)
{
    std::lock_guard<std::mutex> g(lock());
    gManual=manual;
    changed().notify_all();
}

unsigned a830HostRunCalls(bool ignore_deadline)
{
    std::vector<A830Call *> due;
    {
        std::lock_guard<std::mutex> g(lock());
        const uint64_t now=a830Now();
        for(A830Call *call:calls())
            if(call->pending && !call->running && (ignore_deadline || now>=call->deadline)) {
                call->pending=false;call->running=true;due.push_back(call);
            }
    }
    for(A830Call *call:due) {
        call->function(call->param0,nullptr);
        std::lock_guard<std::mutex> g(lock());
        call->running=false;++call->runs;
        changed().notify_all();
    }
    return unsigned(due.size());
}

bool a830HostCallPending(A830Call *call)
{
    std::lock_guard<std::mutex> g(lock());
    return call->pending || call->running;
}

uint64_t a830HostCallRuns(A830Call *call)
{
    std::lock_guard<std::mutex> g(lock());
    return call->runs;
}

bool a830HostCallsIdle(unsigned timeout_ms)
{
    const Clock::time_point until=Clock::now()+std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> g(lock());
    for(;;) {
        bool busy=false;
        const uint64_t now=a830Now();
        for(A830Call *call:calls())if(call->running || (call->pending && now>=call->deadline))busy=true;
        if(!busy)return true;
        if(Clock::now()>=until)return false;
        changed().wait_until(g,std::min(until,Clock::now()+std::chrono::milliseconds(1)));
    }
}

void a830HostShutdown(void)
{
    std::vector<A830Call *> all;
    {
        std::lock_guard<std::mutex> g(lock());
        all=calls();
        for(A830Call *call:all)call->stop=true;
        changed().notify_all();
    }
    for(A830Call *call:all)if(call->worker.joinable())call->worker.join();
    std::lock_guard<std::mutex> g(lock());
    for(A830Call *call:all)delete call;
    calls().clear();
}

uint64_t a830HostDelayed(void)
{
    std::lock_guard<std::mutex> g(lock());
    return gDelayed;
}
