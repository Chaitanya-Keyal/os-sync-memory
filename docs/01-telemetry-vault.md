# Problem 1: The telemetry vault

## The task

One shared record (a version number and 8 fields) is used by:

* 6 **analysts**: idle 1-4 s, then read for 1-2 s, checking that every field
  equals the version. They print CONSISTENT or CORRUPT.
* 2 **uplinks**: idle 2-5 s, then write for 1-2 s. A write increments the
  version and sets the 8 fields one at a time, with `usleep` between them.
* 1 **archiver**: every 12 s it takes exclusive access for 2 s.

Rules: at most 3 readers at once; uplinks and the archiver are exclusive;
once the archiver has asked, nobody new gets in before it. Every request
gets a ticket. After 4 s of waiting a request is URGENT (served first, in
ticket order). After 10 s it times out, backs off 2 s and retries with a new
ticket. A non-urgent request may not overtake an older request of the other
type. Only `pthread_mutex_t` and `pthread_cond_t` are allowed, with one
condition variable per thread type, set to `CLOCK_MONOTONIC`. Run 60 s,
shut down cleanly and print a summary.

## The monitor

C has no monitor construct. A monitor here is: one mutex that every entry
and exit takes, state that is only touched with the mutex held, and
condition variables to wait for that state to change.

```c
static pthread_mutex_t vault_lock;
static pthread_cond_t analyst_cv, uplink_cv, archiver_cv, stop_cv;
static int readers_inside, uplink_inside, archiver_inside;
static struct worker *waiting[9]; static int num_waiting;   /* the waiting list */
static long next_ticket = 1;
static int stopping;
```

Each thread owns a `struct worker` holding its current request (ticket,
request time, urgent flag) and its statistics (accesses, maximum wait,
timeouts). The waiting list holds pointers to the workers that are waiting.

### One rule for admission

`queue_head(now)` returns the waiting request that comes first by
`before()`: **urgent before non-urgent, then lower ticket first**. A request
is urgent when it has waited more than 4 s. A thread may enter only if:

1. it is the head of the list, and
2. `vault_free_for(it)`: no uplink or archiver inside, and for an analyst
   fewer than 3 readers; for an uplink or the archiver, no readers either.

Every rule of the problem follows from this:

| Rule | Why it holds |
|------|--------------|
| ≤ 3 readers, writers exclusive | condition 2 |
| Urgent first, in ticket order | `before()` |
| Non-urgent never overtakes an older request of the other type | only the head enters, and among non-urgent requests the head is the oldest |
| Archiver announced → no new analyst/uplink enters | the archiver's request is in the list; every later request has a higher ticket and is behind it; those inside finish normally |
| No starvation | strict order and bounded accesses (≤ 2 s): every exit moves the queue forward |

Tickets are given in arrival order, and a request turns urgent 4 s after it
arrives. So urgency order and ticket order agree: the oldest requests are
the urgent ones. The explicit "urgent first" key still matters for
correctness by construction, and it would decide the order if the urgency
threshold differed by type.

"Analysts batch together" still happens. When an analyst enters it calls
`wake_next()`. If the new head is also an analyst and a slot is free, it
enters right behind, so up to 3 consecutive analysts read together (see the
sample run at 4.58 s).

### `vault_enter()`

```
lock
ticket = next_ticket++, t_request = now, add to the list, log "requesting"
loop:
    if stopping           -> leave the list, wake_next, return SHUTDOWN
    if waited > 10 s      -> leave the list, timeouts++, log TIMEOUT, wake_next, return TIMED_OUT
    if head && free       -> break
    if waited > 4 s first time -> urgent = 1, log URGENT, wake_next
    timedwait(my type's cv, deadline = t_request + (urgent ? 10 s : 4 s))
leave the list, readers++/uplink=1/archiver=1, log "reading/writing/archiving"
wake_next()          (the next analyst may follow)
unlock
```

The timeout check comes before the admission check. A request that has
waited more than 10 s is abandoned even if it could enter at that instant,
as the rule says. The thread then naps 2 s (`nap(BACKOFF)`) and calls
`vault_enter` again, which gives it a new ticket.

### `vault_exit()`

Takes the mutex, counts the access, updates the state, logs "finished ...",
calls `wake_next()`, and unlocks. The "finished" line is printed inside the
monitor so the log order always matches the real admission order.

### `wake_next()`: signal vs broadcast

Only the head can enter, so only the head's condition variable is woken, and
only if the vault is free for it:

* analyst head → `pthread_cond_broadcast(&analyst_cv)`. Up to 6 analysts
  wait on that one condition variable, and signal would wake an arbitrary
  one. If it is not the head, it goes back to sleep and the head misses its
  wakeup (it would only notice at its 4 s / 10 s deadline).
* uplink head → broadcast, for the same reason (2 uplinks).
* archiver head → `pthread_cond_signal(&archiver_cv)`. There is one archiver
  thread, so one waiter at most.
* shutdown → broadcast on all four.

`wake_next()` is called when a thread exits, when one enters, when a request
leaves the list (timeout or shutdown), and when one turns urgent.

### Why the data is safe outside the mutex

Readers and writers access the record **after** `vault_enter` returns and
**before** `vault_exit`, without holding the mutex (holding it for 2 s would
serialize everything). The protocol guarantees that a writer never overlaps
anyone. The mutex unlock in `vault_enter` and lock in `vault_exit` give the
needed happens-before ordering. The record is `volatile` so every field
access really happens. That is what lets the `-DBROKEN_EXCLUSION` test build
catch the corruption.

### Timing details

* All four condition variables are created with
  `pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)`, and every deadline is
  a `CLOCK_MONOTONIC` timestamp. A wall-clock change cannot make a waiter
  time out early or late.
* Idle and backoff waits use `nap()`, a `pthread_cond_timedwait` on
  `stop_cv`, rather than `sleep()`. That way the shutdown broadcast ends them
  immediately and no thread is left sleeping at the end.
* The archiver aims at absolute times (12 s, 24 s, ...), so its period does
  not drift.

### Shutdown

After 60 s `main` sets `stopping` under the mutex and broadcasts every
condition variable. Each thread then:

* waiting: leaves the list ("abandons ticket N for shutdown") and exits,
* napping: wakes and exits,
* inside: finishes its read/write (the access is not interrupted), exits the
  monitor, and its loop ends.

`main` joins all 9 threads ("All threads joined") and prints the table:
accesses completed, maximum wait (over admitted and timed-out requests),
timeouts, and the CORRUPT count.

## Evidence

* `q1_telemetry_vault/sample_run.txt`: 60 s on the VM, seed 42.
  31 URGENT, 0 timeouts, 0 CORRUPT, 61 accesses.
* `docs/q1-evidence/forced_timeouts.txt`: built with `-DURGENT_MS=1500
  -DTIMEOUT_MS=3000`, so timeouts, backoff and retries with new tickets
  happen often. Still 0 CORRUPT.
* `docs/q1-evidence/broken_exclusion.txt`: built with `-DBROKEN_EXCLUSION`,
  which admits everyone. 14 CORRUPT in 15 s. This proves the check works.
