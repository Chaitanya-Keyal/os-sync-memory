# Problem 2: Priority inversion, inheritance, ceiling and deadlock

## Part A: the scenario

Four SCHED_FIFO threads, all on CPU 0, two mutexes:

| Thread | Prio | Release | Does |
|--------|------|---------|------|
| L | 10 | 0 ms   | lock M1, 600 ms work, unlock M1 |
| D | 20 | 100 ms | lock M2, lock M1, 100 ms work, unlock M1, unlock M2 |
| H | 30 | 200 ms | lock M2, unlock M2 |
| X | 25 | 300 ms | 2000 ms work, no mutex |

H waits for D (M2), and D waits for L (M1): a **chain**. X has a priority
between L and H, so it can keep L off the CPU and stall H indirectly. That
is unbounded priority inversion.

Run with each protocol: `PTHREAD_PRIO_NONE`, `PTHREAD_PRIO_INHERIT`,
`PTHREAD_PRIO_PROTECT` (ceiling 30), 5 runs each. For H measure
**blocking** (lock called → acquired) and **response** (200 ms release →
acquired).

### Predictions and results

The timeline for each version is worked out in
`q2_prio_deadlock/results.txt`. In short:

| Version | What happens | Blocking | Response | Measured |
|---------|--------------|---------:|---------:|----------|
| NONE    | X preempts L 300-2300; L finishes 2600; D 2600-2700 | 2500 | 2500 | 2500.8 / 2501.2 |
| INHERIT | H's 30 passes D → L; X cannot preempt L; L done 600, D 600-700 | 500 | 500 | 500.5 / 500.8 |
| PROTECT | L runs at 30 from its lock on; nobody else runs until 600; then H finds M2 free | 0 | 400 | 0.05 / 400.8 |

**The PROTECT puzzle (asked in the problem).** With a ceiling the delay
moves *before* the lock call. L holds M1 at ceiling priority 30, so H
(priority 30, FIFO, equal and not higher) cannot even start running until L
unlocks at 600. When H finally calls `pthread_mutex_lock(&M2)`, D has never
run, so M2 is free and the lock succeeds at once: blocking ≈ 0. But H was
released at 200 and got M2 at 600, so its response is 400 ms. Blocking time
alone hides the delay; response latency shows it.

### How the program works (`prio_chain.c`)

* `main` pins itself to CPU 0 (`sched_setaffinity`), becomes SCHED_FIFO 99
  and calls `mlockall` so no page fault can stall a thread. It needs root.
* For each run it creates the mutexes with the protocol
  (`pthread_mutexattr_setprotocol`, `setprioceiling(30)`), then the four
  threads with `PTHREAD_EXPLICIT_SCHED` + `SCHED_FIFO` + their priority.
  They inherit the CPU 0 affinity.
* **Busy work** spins until the thread itself has used N ms of CPU
  (`CLOCK_THREAD_CPUTIME_ID`). That is real work: a preempted thread makes no
  progress, which is what makes the inversion visible. A `sleep` would give
  up the CPU and hide it. A fixed loop count was tried first, but the
  laptop's CPU has fast and slow cores and the same count took 1.8-2.3 s.
* **Events** are written to an array with a `CLOCK_MONOTONIC` timestamp and
  printed after the run, so `printf` cannot disturb the timing. A "released"
  event is stamped just *before* the unlock, because the unlock can switch
  to a higher-priority thread at once.
* **Releasing threads on time.** On the VM with 4 vCPUs, timer interrupts
  arrive 10-200 ms late (VirtualBox runs on Hyper-V because WSL2 is on).
  D sometimes started after H, which breaks the chain. So a releaser thread
  on **CPU 1** (prio 99) spins on `CLOCK_MONOTONIC` and opens each thread's
  gate (a condition variable) at its start time. That wakeup comes as an
  inter-processor interrupt, which arrives in under 1 ms. The four test
  threads still share CPU 0. `-t` switches to plain `clock_nanosleep`. On a
  1-vCPU VM, where timers are fine, `-t` gives the same table
  (`run_output_1cpu_timers.txt`).

### Why inheritance works transitively

Linux PI mutexes are rt_mutexes. When H blocks on M2, the kernel boosts M2's
owner (D) to 30. It then sees that D is itself blocked on M1, and boosts
M1's owner (L) to 30 as well, walking the whole chain. With
`PTHREAD_PRIO_PROTECT` there is no chain walk: glibc raises the locking
thread to the ceiling as soon as it locks, and restores it on unlock.

## Part B: deadlock

T1 locks A then B, T2 locks B then C, T3 locks C then A, each with a 100 ms
sleep in between. After 100 ms each holds one lock and wants the next: a
cycle.

### Detection (`./deadlock hang`)

All locking goes through wrappers that update a **wait-for graph** under a
separate `graph_lock`:

* `lock_wait(t, m)`: `waits_for[t] = m`; `pthread_mutex_lock`; then
  `waits_for[t] = -1`, `holder[m] = t`
* `unlock(m)`: `holder[m] = -1`, then `pthread_mutex_unlock`

The watchdog thread checks every 1 ms. From each thread it follows
`waits_for → holder → waits_for ...` for up to 3 steps. If it returns to the
start, that is a cycle, printed as `T1 -> B -> T2 -> C -> T3 -> A -> T1`.
In the hang version it then exits the process with code 2 (the threads can
never wake up). Detected at about 100-110 ms, well within 1 s.

The wrappers cannot produce a false cycle. A holder is cleared *before* the
real unlock, and a waiter is recorded as holder only *after* it really has
the lock.

### The fix (`./deadlock fix 20`)

Every lock is `pthread_mutex_timedlock` with a 300 ms deadline (POSIX
defines the deadline on `CLOCK_REALTIME`; all measurements still use
`CLOCK_MONOTONIC`). If the second lock times out, the thread unlocks the
first one, sleeps a random 50-250 ms and starts over. The lock order is
unchanged.

Typical run: the cycle forms at 100 ms (the watchdog still reports it once),
and the first timeouts come at about 400 ms. A thread that times out frees
its lock, so its neighbour gets it and finishes. Usually 2 threads retry
once each. 20 runs: average 759.8 ms, maximum 928.3 ms, no outliers.

### The short note (also in results.txt)

* **Why inheritance cannot help in Part B.** Inheritance speeds up a lock
  holder that *can run*. In a deadlock every holder is blocked waiting for
  another, so boosting priorities changes nothing.
* **Coffman condition broken:** *no preemption*. A timed-out thread gives up
  the lock it holds, which is voluntary preemption of resources. That also
  stops it from holding and waiting forever. Mutual exclusion and circular
  wait (the order) are unchanged.
* **Livelock.** If all three time out together, back off equally and retry
  in lockstep, the cycle can repeat forever. Random backoff (different seed
  per thread and run) makes that very unlikely. In 20 runs no thread retried
  more than once.
