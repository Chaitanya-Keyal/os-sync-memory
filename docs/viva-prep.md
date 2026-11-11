# Viva preparation

Every member may be asked about any problem and may be asked to change code
on the spot. Read in this order:

1. `concepts.md`: background for everything (30 min)
2. `01-telemetry-vault.md`, then `q1_telemetry_vault/vault.c`
3. `02-priority-deadlock.md` and `q2_prio_deadlock/results.txt`
4. `03-memory-swap.md` and `q3_memswap/results.txt`
5. `demo.md`: run through it once on the VM
6. `modifications.md`: try one change per problem

## What every member must be able to explain

* Why a condition variable wait is always in a `while` loop.
* Signal vs broadcast, and why the vault uses each where it does.
* Why the condition variables use `CLOCK_MONOTONIC`.
* How the vault guarantees ticket order, urgency first, the archiver rule
  and no starvation.
* SCHED_FIFO; unbounded priority inversion; inheritance vs ceiling; why
  blocking time and response time differ under the ceiling.
* The four Coffman conditions, which one the timeout fix breaks, and why
  that can livelock.
* Minor vs major faults, what swap-in/out are, why memory pressure starts
  between 1536 and 2048 MiB, and why the hot pattern starts slower.

## Each problem in 60 seconds

**Problem 1.** One mutex, four condition variables (analysts, uplinks,
archiver, plus one for naps). Every request takes a ticket and goes into a
waiting list ordered urgent-first, then by ticket. Only the head may enter,
and only when the vault is free for it: ≤3 readers, writers and archiver
alone. Waiters use `pthread_cond_timedwait` on a monotonic clock with a
deadline at 4 s (become URGENT) and 10 s (TIMEOUT, back off 2 s, new
ticket). After any change, `wake_next()` wakes the head's type: broadcast
for analysts/uplinks (several waiters, only the head may go), signal for the
single archiver. At 60 s a broadcast on everything makes everyone exit.
0 CORRUPT; a deliberately broken build shows CORRUPT immediately.

**Problem 2A.** L/D/H/X on CPU 0 under SCHED_FIFO. H waits on D waits on L,
and X can starve L. NONE: X runs first, H waits 2500 ms. INHERIT: the boost
passes through D to L, X waits, H waits 500. PROTECT: L runs at the ceiling
from its lock, so H cannot even start until 600. Its lock is then instant
(blocking 0) but it was released at 200 (response 400). Measured within
about 1 ms of every prediction.

**Problem 2B.** Lock wrappers keep a wait-for graph; a watchdog finds the
cycle T1→B→T2→C→T3→A→T1 within 1 ms of polling and exits with 2. Fix:
`pthread_mutex_timedlock` 300 ms, on timeout release everything and back
off a random 50-250 ms. This breaks "no preemption". ~760 ms average, 1-2
retries.

**Problem 3.** 1 MiB page-aligned chunks; write one byte per page; 5 × 10,000
single-byte lookups, uniform or 90% in the first 10%. Time each phase; read
major faults and pswpin/pswpout around it. With 2 GiB RAM, 1536 MiB still
fits (0 faults), and 2048 MiB pages heavily (~2,000 faults/pass, ~0.6 ms
each). The hot region is the oldest memory, so it is swapped out first and
only warms up slowly, pass by pass.

## Likely questions

| Question | Short answer |
|----------|--------------|
| Why not one condition variable for everyone? | The problem asks for one per type, and it lets us wake only the type that can enter. |
| Could the vault use `pthread_cond_signal` for analysts? | Only if it could choose *which* analyst wakes. It cannot, so a wakeup could be lost until the next deadline. |
| What if the archiver arrives while 3 analysts read? | It queues. Later requests queue behind it, the 3 finish, then it enters alone. |
| Can an urgent analyst overtake the archiver? | Only if it is older (lower ticket), and then it was ahead anyway. |
| Why is the record read outside the mutex? | Holding a mutex for 1-2 s would serialize the readers. The monitor hands out permission; the mutex orders memory. |
| Why does H wait 2500 ms with PRIO_NONE? | X (25) preempts L (10), which holds M1, which D needs before releasing M2. X's 2 s plus L's last 300 ms plus D's 100 ms. |
| Why does the ceiling delay D? | L runs at 30 from 0 to 600, so D (20) cannot run, and after that X (25) runs. D starts at 2600. |
| Why the releaser thread on CPU 1? | VM timer interrupts arrive late with 4 vCPUs, and thread starts came out of order. A cross-CPU wakeup is prompt. The test threads are still all on CPU 0. |
| Why `CLOCK_THREAD_CPUTIME_ID` for busy work? | Work must not progress while preempted, and a loop count varies with the host core speed. |
| Why does `pthread_mutex_timedlock` take CLOCK_REALTIME? | POSIX defines it so. `pthread_mutex_clocklock` (glibc 2.30+) accepts CLOCK_MONOTONIC. Our measurements all use MONOTONIC. |
| Why is In (KiB) = 4 × Major? | Each major fault is one 4 KiB page read from swap. |
| Why do later passes write almost nothing to swap? | Clean pages already have a copy in swap (swap cache), so they can be dropped. |
| Why are there major faults in deallocation? | `free()` reads glibc's chunk header, which sits in a page just before the block that may have been swapped out. |
| Why is allocation so fast? | `mmap` only reserves addresses; frames come on first touch, i.e. during initialization. |
