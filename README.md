# CS F372 Operating Systems - Assignment 3

## Group details

| Name | ID number |
|------|-----------|
| Chaitanya Keyal | 2023B4A70727H |
| Arpit Wallecha  | 2023B3A70482H |
| Vishesh Agarwal | 2023B4A70611H |
| Abhishek Verma  | 2023B4A71256H |

## Contents

Longer explanations of each problem, the concepts behind them and a demo
script are in `docs/`.

| Directory             | Problem                                              | Program(s) and results |
|-----------------------|------------------------------------------------------|------------------------|
| `q1_telemetry_vault/` | 1 - telemetry vault monitor (mutex + condvars)       | `vault`, `sample_run.txt` |
| `q2_prio_deadlock/`   | 2 - priority inheritance/ceiling, deadlock detection | `prio_chain`, `deadlock`, `run_all.sh`, `results.txt` |
| `q3_memswap/`         | 3 - memory pressure, locality and swap               | `memswap`, `swap_setup.sh`, `run_trials.sh`, `run_all.sh`, `plot.py`, `results.txt`, `results/` |

Tested on Ubuntu 24.04 (VirtualBox VM) with gcc 13.3 and Linux 6.14.
Problem 3 was measured with the VM set to 1 vCPU and 2 GiB RAM, as the
problem asks; Problems 1 and 2 with 4 vCPUs and 4 GiB (Problem 1's sample
run with 1 vCPU, see below).

## Build

```bash
make            # all programs, gcc -Wall -Wextra -O2 -pthread, no warnings
make clean
make dist       # assignment3_submission.tar.gz with sources, executables and results
```

Each program can also be built by hand with the `gcc` line at the top of its
source file.

---

## Problem 1 - `vault`

```bash
cd q1_telemetry_vault && ./vault            # 60 s; ./vault <seconds> [seed]
```

* One `pthread_mutex_t vault_lock` guards the monitor state: readers inside,
  uplink inside, archiver inside, and the list of waiting requests.
  6 analyst threads read (at most 3 at a time), 2 uplink threads write, and
  the archiver takes a snapshot every 12 s. Uplinks and the archiver need
  the vault to themselves.
* Every request takes a ticket from a global counter and joins one waiting
  list, ordered **urgent first, then by ticket**. Only the head of the list
  may enter, and only when the vault is free for it. So:
  * nobody is admitted ahead of an older request (of either type),
  * urgent requests (waited more than 4 s) come before non-urgent ones, in
    ticket order,
  * once the archiver has requested, every later analyst or uplink queues
    behind it, while the threads already inside finish.
* Each waiter sleeps with `pthread_cond_timedwait()` on its type's condition
  variable, which is set to `CLOCK_MONOTONIC`. The deadline is the moment it
  turns urgent (4 s) and then the moment it times out (10 s). On a timeout
  it leaves the list (and wakes whoever is now the head), backs off 2 s, and
  retries with a new ticket.
* Shutdown after 60 s: `stopping` is set and every condition variable is
  broadcast. Waiting threads abandon their requests, threads inside finish
  their access, idle and backing-off threads wake early (their naps wait on
  `stop_cv`). Everything is joined, and the summary table is printed.
* Data is checked field by field while reading. `CORRUPT detections: 0` in
  every run. A test build with `-DBROKEN_EXCLUSION` lets everyone in at once
  and reports CORRUPT within seconds (`docs/q1-evidence/`).
* No deadlock: there is a single mutex, and no thread holds it while
  waiting for anything except inside `pthread_cond_timedwait`, which
  releases it. No starvation: admission is in ticket order and every access
  is bounded (at most 2 s), so a waiting request moves forward every time
  someone leaves. The timeout is the backstop.

### Where `pthread_cond_signal` and where `pthread_cond_broadcast`, and why

All wakeups for admission go through `wake_next()`, which wakes only the
condition variable of the request at the head of the list, and only if the
vault is free for it.

| Place | Call | Why |
|-------|------|-----|
| `wake_next()`, head is an **analyst** (`analyst_cv`) | broadcast | Up to 6 analysts wait on `analyst_cv`, and only the head may enter. `pthread_cond_signal` wakes an arbitrary waiter: if it woke an analyst that is not the head, that one would see it cannot enter and sleep again, and the head would miss the wakeup until its own 4 s / 10 s deadline. The broadcast wakes all of them. The head enters and calls `wake_next()` again, so the next analyst in line follows it in if a reader slot is free; the rest go back to sleep. |
| `wake_next()`, head is an **uplink** (`uplink_cv`) | broadcast | Same reason: both uplinks may be waiting and only one of them is the head. With 2 waiters the cost is negligible. |
| `wake_next()`, head is the **archiver** (`archiver_cv`) | signal | There is exactly one archiver thread, so at most one waiter, and it is the head. Signal is enough. |
| `main()`, shutdown (all three, plus `stop_cv`) | broadcast | Every waiting and napping thread must wake, see `stopping`, and exit. |

`wake_next()` is called after a thread leaves (`vault_exit`), after a thread
enters (the analyst behind it may follow), when a request times out or is
abandoned at shutdown (the list head may change), and when a request turns
urgent.

## Problem 2 - `prio_chain`, `deadlock`

```bash
cd q2_prio_deadlock && sudo ./run_all.sh    # everything below; writes run_output.txt
sudo ./prio_chain 5                         # Part A: 3 mutex protocols x 5 runs
./deadlock hang; echo $?                    # Part B: the deadlock, watchdog exits with 2
./deadlock fix 20                           # Part B: timedlock + backoff, 20 runs
```

* **Part A.** L(10), D(20), H(30) and X(25) are SCHED_FIFO threads pinned to
  CPU 0, with `mlockall`. Busy work spins until the thread has used N ms of
  CPU. All timing uses `CLOCK_MONOTONIC`. The program runs all three
  protocols, prints the event trace of run 1 and a predicted-vs-measured
  table. Measured (average of 5): H blocking 2500.8 / 500.5 / 0.05 ms and H
  response 2501.2 / 500.8 / 400.8 ms for NONE / INHERIT / PROTECT, against
  predictions of 2500 / 500 / 0 and 2500 / 500 / 400.
* The threads are started on time by a releaser thread on CPU 1 (with 4
  vCPUs, timer interrupts reach the VM up to 200 ms late; see
  `results.txt`). `./prio_chain -t` uses plain `clock_nanosleep` instead,
  and gives the same results on a 1-vCPU VM.
* **Part B.** Lock wrappers keep a wait-for graph (`holder[mutex]`,
  `waits_for[thread]`). A watchdog checks it every 1 ms and prints
  `T1 -> B -> T2 -> C -> T3 -> A -> T1`. The fixed version uses
  `pthread_mutex_timedlock` (300 ms). On timeout a thread releases
  everything, sleeps a random 50-250 ms and starts over. 20 runs: average
  759.8 ms, maximum 928.3 ms, 1-2 retries per run.
* `results.txt` has the predictions with their reasoning, the table, the
  traces, the hang and fix evidence and the short note.

## Problem 3 - `memswap`

```bash
cd q3_memswap
./memswap 1536 uniform 42 internal          # one trial: <MiB> <uniform|hot> [seed] [label] [csv]
sudo ./swap_setup.sh internal               # only a 4 GiB swap file on the VM disk, zswap off
sudo ./run_trials.sh internal 512 1024 1536 2048 2560    # Part A
sudo ./swap_setup.sh usb /dev/sdb           # only the USB swap area (erases its start)
sudo ./run_trials.sh usb 512 1024 1536 2048 # Part B
sudo ./run_all.sh /dev/sdb                  # or: everything above in one go (~30 min)
python3 plot.py                             # tables.txt and the three Part A plots in results/
```

* Allocates `size` MiB as page-aligned 1 MiB chunks (`posix_memalign`,
  each result checked), writes `page_number % 256` to the first byte of
  every page, then runs 5 passes of 10,000 one-byte lookups (uniform, or
  90% in the hot first 10% of pages). Indices are generated before each
  timed pass from a splitmix64 generator that continues across passes. The
  reads go through a `volatile` pointer into a checksum, which is checked
  against the value expected from the indices.
* Each phase (allocation, initialization, each pass, deallocation) is timed
  with `CLOCK_MONOTONIC`. Major faults (`getrusage`) and `pswpin`/`pswpout`
  (converted from pages to KiB) are read outside the timed intervals.
  Output is printed only at the end.
* `results.txt` holds the tables, the tested sizes and the findings. The
  three plots are in `results/`, and every trial's raw output is in
  `results/raw/`.
* Part A: the pressure transition is between 1536 MiB (no major faults)
  and 2048 MiB (about 1,700 major faults per uniform pass, 0.5-0.9 ms
  each). The hot region starts slower than uniform, because it is the
  oldest memory and is swapped out first, and improves pass by pass.
* Part B: the USB device is a SanDisk pen drive passed through to the VM.
  512-1536 MiB never swap, so both backings look the same. At 2048 MiB,
  added to show the effect, initialization is 8-12x slower on USB and each
  major fault 3-8x slower. Both parts ran on the stock 6.8 kernel (the
  custom kernel has no USB storage driver). Setup steps are in
  `docs/03-memory-swap.md`.
