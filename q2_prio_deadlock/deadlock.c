/*
 * CS F372 Assignment 3, Problem 2, Part B
 *
 * Circular wait with three mutexes A, B, C and three threads:
 *   T1: lock A, sleep 100 ms, lock B
 *   T2: lock B, sleep 100 ms, lock C
 *   T3: lock C, sleep 100 ms, lock A
 * After both locks a thread does 20 ms of busy work and unlocks both.
 *
 * All locking goes through wrappers that keep a wait-for graph:
 *   holder[m]    = thread holding mutex m, or -1
 *   waits_for[t] = mutex thread t is blocked on, or -1
 * A watchdog thread checks the graph every millisecond and prints any cycle
 * (T1 -> B -> T2 -> C -> T3 -> A -> T1 means T1 waits for B, held by T2, ...).
 *
 *   ./deadlock hang      blocking pthread_mutex_lock: the threads deadlock,
 *                        the watchdog reports the cycle and exits with code 2
 *   ./deadlock fix [n]   pthread_mutex_timedlock with a 300 ms timeout. On a
 *                        timeout the thread releases what it holds, waits a
 *                        random 50-250 ms and starts again. Runs n times
 *                        (default 20) and reports retries and completion.
 *
 * Everything runs on CPU 0 like Part A. Times use CLOCK_MONOTONIC.
 * pthread_mutex_timedlock itself takes a CLOCK_REALTIME deadline (POSIX).
 *
 * Build: gcc -Wall -Wextra -O2 -pthread -o deadlock deadlock.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NUM_THREADS 3
#define NUM_MUTEXES 3
#define HOLD_MS 100
#define WORK_MS 20
#define TIMEOUT_MS 300
#define BACKOFF_MIN_MS 50
#define BACKOFF_MAX_MS 250
#define MAX_RUNS 1000

static const char mutex_name[] = "ABC";
static pthread_mutex_t mtx[NUM_MUTEXES];

/* Wait-for graph, protected by graph_lock. */
static pthread_mutex_t graph_lock = PTHREAD_MUTEX_INITIALIZER;
static int holder[NUM_MUTEXES];
static int waits_for[NUM_THREADS];

static int use_timeout;       /* 0 = hang demo, 1 = fixed version */
static volatile int run_over; /* tells the watchdog to stop */
static int cycles_seen;       /* cycle episodes in this run */
static int retries[NUM_THREADS];
static unsigned int seeds[NUM_THREADS];
static int64_t t_run;

static int64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static double ms_since(int64_t t) { return (now_ns() - t) / 1e6; }

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == EINTR);
}

static void busy_ms(int ms) {
    int64_t end = now_ns() + (int64_t)ms * 1000000LL;
    while (now_ns() < end);
}

/* ---- lock wrappers that maintain the wait-for graph -------------------- */

static void set_waiting(int t, int m) {
    pthread_mutex_lock(&graph_lock);
    waits_for[t] = m;
    pthread_mutex_unlock(&graph_lock);
}

static void got_lock(int t, int m) {
    pthread_mutex_lock(&graph_lock);
    waits_for[t] = -1;
    holder[m] = t;
    pthread_mutex_unlock(&graph_lock);
}

/* Blocking lock (hang demo). */
static void lock_wait(int t, int m) {
    set_waiting(t, m);
    pthread_mutex_lock(&mtx[m]);
    got_lock(t, m);
}

/* Lock with a TIMEOUT_MS timeout. Returns 0 or ETIMEDOUT. */
static int lock_timed(int t, int m) {
    struct timespec deadline;
    int rc;

    clock_gettime(CLOCK_REALTIME, &deadline); /* timedlock wants REALTIME */
    deadline.tv_nsec += TIMEOUT_MS * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;

    set_waiting(t, m);
    rc = pthread_mutex_timedlock(&mtx[m], &deadline);
    if (rc == 0) {
        got_lock(t, m);
    } else {
        set_waiting(t, -1);
    }
    return rc;
}

static void unlock(int m) {
    pthread_mutex_lock(&graph_lock);
    holder[m] = -1;
    pthread_mutex_unlock(&graph_lock);
    pthread_mutex_unlock(&mtx[m]);
}

/* ---- watchdog ---------------------------------------------------------- */

/* Writes the cycle through thread s into buf; returns 0 if s is on none.
 * Caller holds graph_lock. */
static int find_cycle(int s, char* buf, size_t len) {
    int t = s, n = 0;

    n += snprintf(buf + n, len - n, "T%d", s + 1);
    for (int step = 0; step < NUM_THREADS; step++) {
        int m = waits_for[t];
        if (m < 0 || holder[m] < 0) return 0;
        t = holder[m];
        n += snprintf(buf + n, len - n, " -> %c -> T%d", mutex_name[m], t + 1);
        if (t == s) return 1;
    }
    return 0;
}

static void* watchdog(void* arg) {
    int in_cycle = 0; /* print each cycle once, not every millisecond */
    char buf[128];

    (void)arg;
    while (!run_over) {
        int found = 0;

        pthread_mutex_lock(&graph_lock);
        for (int s = 0; s < NUM_THREADS && !found; s++)
            found = find_cycle(s, buf, sizeof(buf));
        pthread_mutex_unlock(&graph_lock);

        if (found && !in_cycle) {
            cycles_seen++;
            printf("  [watchdog %7.1f ms] DEADLOCK cycle detected: %s\n",
                   ms_since(t_run), buf);
            fflush(stdout);
            if (!use_timeout) {
                printf(
                    "  [watchdog] threads are blocked forever (no timeout "
                    "in this version); exiting with code 2\n");
                fflush(stdout);
                exit(2);
            }
        }
        in_cycle = found;
        sleep_ms(1);
    }
    return NULL;
}

/* ---- worker threads ---------------------------------------------------- */

static void* worker(void* arg) {
    int t = (int)(intptr_t)arg;
    int first = t, second = (t + 1) % NUM_MUTEXES;

    if (!use_timeout) {
        lock_wait(t, first);
        sleep_ms(HOLD_MS);
        lock_wait(t, second); /* never returns: circular wait */
        busy_ms(WORK_MS);
        unlock(second);
        unlock(first);
        return NULL;
    }

    for (;;) {
        if (lock_timed(t, first) == 0) {
            sleep_ms(HOLD_MS);
            if (lock_timed(t, second) == 0) break;
            unlock(first); /* give up everything we hold */
        }
        retries[t]++;
        sleep_ms(BACKOFF_MIN_MS +
                 rand_r(&seeds[t]) % (BACKOFF_MAX_MS - BACKOFF_MIN_MS + 1));
    }
    busy_ms(WORK_MS);
    unlock(second);
    unlock(first);
    return NULL;
}

/* One run; returns the completion time in ms. */
static double run_once(void) {
    pthread_t tids[NUM_THREADS], wd;
    double elapsed;

    for (int m = 0; m < NUM_MUTEXES; m++) {
        pthread_mutex_init(&mtx[m], NULL);
        holder[m] = -1;
    }
    for (int t = 0; t < NUM_THREADS; t++) {
        waits_for[t] = -1;
        retries[t] = 0;
    }
    cycles_seen = 0;
    run_over = 0;
    t_run = now_ns();
    pthread_create(&wd, NULL, watchdog, NULL);
    for (int t = 0; t < NUM_THREADS; t++)
        pthread_create(&tids[t], NULL, worker, (void*)(intptr_t)t);
    for (int t = 0; t < NUM_THREADS; t++) pthread_join(tids[t], NULL);
    elapsed = ms_since(t_run);
    run_over = 1;
    pthread_join(wd, NULL);
    for (int m = 0; m < NUM_MUTEXES; m++) pthread_mutex_destroy(&mtx[m]);
    return elapsed;
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

int main(int argc, char* argv[]) {
    cpu_set_t cpu0;
    int runs = argc > 2 ? atoi(argv[2]) : 20;

    CPU_ZERO(&cpu0);
    CPU_SET(0, &cpu0);
    sched_setaffinity(0, sizeof(cpu0), &cpu0);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc > 1 && strcmp(argv[1], "hang") == 0) {
        printf(
            "Part B: hang demo - T1: A->B, T2: B->C, T3: C->A, blocking "
            "locks\n");
        run_once(); /* the watchdog exits the process */
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "fix") != 0 || runs < 1 ||
        runs > MAX_RUNS) {
        fprintf(stderr, "usage: %s hang | fix [runs=20]\n", argv[0]);
        return 1;
    }

    use_timeout = 1;
    printf(
        "Part B: fixed version - pthread_mutex_timedlock (%d ms), release "
        "all and back off %d-%d ms on timeout, %d runs\n",
        TIMEOUT_MS, BACKOFF_MIN_MS, BACKOFF_MAX_MS, runs);
    {
        double times[MAX_RUNS], sorted[MAX_RUNS], sum = 0, max = 0, median;
        int total_retries = 0, max_retries = 0, run_retries[MAX_RUNS];
        unsigned int base = (unsigned)time(NULL);

        for (int r = 0; r < runs; r++) {
            int rr;

            for (int t = 0; t < NUM_THREADS; t++)
                seeds[t] = base * 7919u + (unsigned)(r * NUM_THREADS + t);
            times[r] = run_once();
            rr = retries[0] + retries[1] + retries[2];
            run_retries[r] = rr;
            printf(
                "run %2d: completion %7.1f ms   retries %d (T1 %d, T2 %d, "
                "T3 %d)   deadlock cycles seen by watchdog: %d\n",
                r + 1, times[r], rr, retries[0], retries[1], retries[2],
                cycles_seen);
            sum += times[r];
            if (times[r] > max) max = times[r];
            total_retries += rr;
            if (rr > max_retries) max_retries = rr;
        }
        memcpy(sorted, times, sizeof(double) * runs);
        qsort(sorted, runs, sizeof(double), cmp_double);
        median = sorted[runs / 2];

        printf("\nsummary over %d runs:\n", runs);
        printf(
            "  completion time  average %.1f ms, maximum %.1f ms, median "
            "%.1f ms\n",
            sum / runs, max, median);
        printf("  retries per run  average %.2f, maximum %d\n",
               (double)total_retries / runs, max_retries);
        printf("  unusually long runs (> 1.5 x median = %.1f ms):",
               1.5 * median);
        {
            int any = 0;
            for (int r = 0; r < runs; r++)
                if (times[r] > 1.5 * median) {
                    printf("%s run %d (%.1f ms, %d retries)", any ? "," : "",
                           r + 1, times[r], run_retries[r]);
                    any = 1;
                }
            printf("%s\n", any ? "" : " none");
        }
    }
    return 0;
}
