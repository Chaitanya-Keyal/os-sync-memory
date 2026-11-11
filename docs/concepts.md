# Concepts used in this assignment

## Mutexes and condition variables

A **mutex** lets one thread at a time into a critical section.
`pthread_mutex_lock` blocks until the mutex is free, `pthread_mutex_unlock`
releases it.

A **condition variable** lets a thread sleep until some shared state
changes. It is always used with a mutex and a loop:

```c
pthread_mutex_lock(&m);
while (!condition)                    /* always a loop, never an if */
    pthread_cond_wait(&cv, &m);       /* atomically: unlock m, sleep, relock m */
/* condition holds and we own m */
pthread_mutex_unlock(&m);
```

* The loop is needed because wakeups can be *spurious*, and because another
  thread may have changed the state between the wakeup and the relock.
* `pthread_cond_signal` wakes **one** waiter (any one).
  `pthread_cond_broadcast` wakes **all** of them. Signal is only correct if
  any waiter can make progress, or there is only one waiter. Otherwise the
  woken one may be the wrong one and the wakeup is lost. Problem 1 uses
  broadcast for analysts and uplinks and signal for the single archiver.
* `pthread_cond_timedwait(&cv, &m, &abstime)` also returns when an
  **absolute** deadline passes (`ETIMEDOUT`). By default the deadline is on
  `CLOCK_REALTIME`, which jumps when the clock is set.
  `pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)` makes it use the
  monotonic clock, which only moves forward.

## Monitors

A monitor is a module whose state can be touched only through its
procedures, with at most one thread inside at a time, plus condition
variables to wait inside. Java's `synchronized` and `wait`/`notify` are
built in. In C we build one by hand: one mutex taken by every procedure,
state accessed only under it, and `pthread_cond_t`s.

Problem 1's monitor does not guard the record itself. It hands out
*permission* to use it (enter/exit), which is the classic readers-writers
structure.

## Readers-writers, fairness, aging

* **Readers-writers:** many readers may share, writers need exclusivity.
  Reader preference can starve writers, and writer preference can starve
  readers.
* **Tickets (FIFO):** serve in arrival order. No starvation as long as each
  access is bounded.
* **Aging:** a waiting request gains priority over time (here: URGENT after
  4 s). This guards against starvation in schemes that are not strictly
  FIFO.
* **Timeout + backoff:** give up after a bound, step back, retry. This
  bounds the wait seen by the caller.

## Real-time scheduling

* `SCHED_FIFO`: fixed priority 1-99, higher always preempts lower, and equal
  priorities run in order until they block or yield. No time slice.
  (`SCHED_RR` adds a time slice among equals.) Setting it needs root
  (`CAP_SYS_NICE`).
* Pinning to one CPU (`sched_setaffinity`, `taskset -c 0`) makes "who runs"
  depend only on priority. On several CPUs X could simply run elsewhere.
* The kernel's **fair server** (Linux 6.12+) reserves normal tasks up to
  50 ms per second on each CPU even when RT tasks are busy, so a runaway RT
  thread cannot freeze the system. It replaced RT throttling
  (`sched_rt_runtime_us`).

## Priority inversion and its fixes

**Priority inversion:** a high-priority thread waits for a lock held by a
low-priority one. That is unavoidable, but short if the holder runs. It
becomes **unbounded** when medium-priority threads, which need no lock,
preempt the holder (Mars Pathfinder, 1997). With a chain (H → D → L), every
link can be delayed.

* **Priority inheritance (`PTHREAD_PRIO_INHERIT`):** while a higher-priority
  thread waits on a mutex, the owner runs at the waiter's priority. It is
  **transitive** in Linux: the boost follows the chain of blocked owners.
  It costs nothing when there is no contention, and is reactive.
* **Priority ceiling (`PTHREAD_PRIO_PROTECT`):** each mutex has a ceiling
  (the highest priority of any thread that uses it). Whoever locks it runs at
  the ceiling immediately. This is proactive: it prevents the inversion and
  also prevents deadlock between mutexes with the same ceiling, but it costs
  a priority change on every lock and unlock, and it can block threads that
  never touch the mutex (D and H in Part A could not even start).
* **Blocking time vs response time:** with a ceiling, the wait moves from
  "inside lock()" to "before being scheduled". Measure from the release time
  to see it.

## Deadlock

**Coffman conditions** (all four are needed for deadlock):

1. **Mutual exclusion:** a resource is held by one thread at a time.
2. **Hold and wait:** a thread holds one resource while waiting for another.
3. **No preemption:** a resource cannot be taken away; only its holder
   releases it.
4. **Circular wait:** a cycle of threads, each waiting for the next.

Ways to deal with it:

* Prevention breaks a condition. A global lock order breaks circular wait.
  Try-lock or timeout plus releasing everything breaks no-preemption and
  hold-and-wait.
* Avoidance: banker's algorithm.
* Detection: a **wait-for graph** (thread → thread it waits for). Any cycle
  is a deadlock.

**Livelock:** threads keep running (timing out, retrying) but none
progresses, e.g. all back off for the same time and collide again.
Randomized backoff breaks the symmetry.

## Virtual memory, paging, swap

* Every process sees virtual addresses. The kernel maps each **page**
  (4 KiB) to a physical frame on first touch (**demand paging**). `malloc`
  or `mmap` of 1 MiB only reserves addresses.
* **Minor fault:** the page is resolved without I/O (fresh zero page, or the
  page is still in memory). **Major fault:** the page has to be read from
  disk; for anonymous memory that means from **swap**.
* When RAM runs out, the kernel picks old, inactive pages (an LRU
  approximation) and writes them to swap (`pswpout`). Touching them later
  reads them back (`pswpin`, one major fault per page).
* **Swap cache:** a page that was read back and not modified still has its
  copy in swap, so it can be evicted again without being written.
* **Locality / working set:** if the pages in active use fit in RAM, faults
  are rare. **Thrashing:** the working set does not fit and the system
  spends its time swapping.
* **zswap / zram:** compress pages in RAM before or instead of swapping to
  disk. Disabled here so we measure the real backing device.
* `/proc/vmstat` counters are system-wide; `getrusage` is per process.
