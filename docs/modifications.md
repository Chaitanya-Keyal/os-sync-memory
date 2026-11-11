# Making changes on the spot

After editing, run `make` in the repository root (or the `gcc` line at the
top of the file).

## Problem 1: vault.c

| Change | Where |
|--------|-------|
| More/fewer analysts or uplinks | `NUM_ANALYSTS`, `NUM_UPLINKS` (the thread table in `main` follows) |
| Max concurrent readers | `MAX_READERS` |
| Urgent / timeout thresholds | `URGENT_MS`, `TIMEOUT_MS` (or `-D` on the gcc line) |
| Backoff, archive period or hold | `BACKOFF`, `ARCHIVE_PERIOD`, `ARCHIVE_HOLD` |
| Run length | first argument: `./vault 30` |
| Idle / read / write durations | the `rand_ms(w, lo, hi)` calls in `analyst()` and `uplink()` |
| Reader preference (analysts may join active readers past a waiting uplink) | in the admission check in `vault_enter()`, also let an analyst in if `vault_free_for(w)` and no *urgent* request is ahead of it; write a small `urgent_ahead(w)` that scans `waiting[]` |
| Writer priority over analysts | in `before()`, put uplinks before analysts when neither is urgent |
| Archiver jumps the queue even past older requests | in `before()`, return true first for `a->kind == ARCHIVER` |
| Print the queue on every change | loop over `waiting[]` in `wake_next()` and print each ticket |
| Also count abandoned waits in MaxWait | call `note_wait(w, now)` in the `stopping` branch of `vault_enter()` |
| Use signal for uplinks | give each uplink its own condition variable (an array indexed by id) and signal the head's. That is the only correct way |

## Problem 2: prio_chain.c, deadlock.c

| Change | Where |
|--------|-------|
| Priorities / start times | `thread_prio[]`, `thread_start_ms[]` |
| Work amounts | the `busy_work(...)` calls in `thread_l/d/h/x` |
| Ceiling value | `CEILING` (must be ≥ every user's priority, or lock returns EINVAL) |
| Number of runs | argument: `sudo ./prio_chain 10` |
| Predictions in the table | `versions[]` |
| Give H a critical section | `busy_work(n)` between H's acquired and released |
| Only one protocol | loop bounds in `main`, or shrink `versions[]` |
| Plain timers instead of the releaser | `-t` |
| Deadlock: timeout length / backoff range | `TIMEOUT_MS`, `BACKOFF_MIN_MS`, `BACKOFF_MAX_MS` |
| Deadlock: 4 threads in a cycle | `NUM_THREADS`, `NUM_MUTEXES` = 4 and `mutex_name = "ABCD"` (the order is `t, (t+1) % n`) |
| Fix by lock ordering instead | in `worker()`, lock `min(first, second)` then `max(...)`: T3 then locks A before C and the cycle cannot form |
| Fixed backoff (to show livelock risk) | replace the `rand_r` term with a constant, run `./deadlock fix 20`, compare retries |
| Use a monotonic timed lock | `pthread_mutex_clocklock(&mtx[m], CLOCK_MONOTONIC, &deadline)` with a MONOTONIC deadline |

## Problem 3: memswap.c and scripts

| Change | Where |
|--------|-------|
| Passes / lookups | `PASSES`, `LOOKUPS` (also `LOOKUPS`/`PASSES` in `plot.py`) |
| Hot region size or probability | `HOT_FRACTION`, `HOT_PROBABILITY` |
| Hot region at the end instead of the start | `page = total_pages - hot_pages + rng_below(hot_pages)` and the cold branch `rng_below(total_pages - hot_pages)`. It should then be faster than uniform, since the newest pages are still in RAM |
| Sequential pattern | a third pattern: `page = (start + k) % total_pages` |
| Chunk size | `CHUNK_SIZE` |
| Also show minor faults | add `ru_minflt` to `struct counters` and print it |
| Different seed | 3rd argument, `SEED` in `run_trials.sh` |
| Swap size | `SIZE_MIB` in `swap_setup.sh` |
| More sizes | arguments to `run_trials.sh`; add them to `PART_A_SIZES` in `plot.py` for the ticks |
