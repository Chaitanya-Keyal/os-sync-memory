/*
 * CS F372 Assignment 3, Problem 2, Part A
 *
 * Transitive priority inversion: H waits for D (on M2), D waits for L (on M1),
 * while X, at a priority between L's and H's, hogs the CPU. Four SCHED_FIFO
 * threads on CPU 0:
 *
 *   L  prio 10  t=0 ms    lock M1, 600 ms busy work, unlock M1
 *   D  prio 20  t=100 ms  lock M2, lock M1, 100 ms busy work, unlock both
 *   H  prio 30  t=200 ms  lock M2, unlock M2
 *   X  prio 25  t=300 ms  2000 ms busy work, no mutex
 *
 * The scenario runs RUNS times for each mutex protocol (PTHREAD_PRIO_NONE,
 * PTHREAD_PRIO_INHERIT, PTHREAD_PRIO_PROTECT with ceiling 30). For H it
 * measures
 *   blocking time    = lock(M2) acquired - lock(M2) called
 *   response latency = lock(M2) acquired - intended release (t0 + 200 ms)
 * and prints the event trace of run 1 plus a predicted-vs-measured table.
 *
 * Busy work of N ms spins until the thread itself has used N ms of CPU
 * (CLOCK_THREAD_CPUTIME_ID). A preempted thread makes no progress, unlike a
 * sleep or a wall-clock spin, and the amount of work does not depend on
 * whether the VM's vCPU sits on a fast or a slow host core (a fixed loop
 * count varied by +-15% on our hybrid laptop CPU). Every timestamp and
 * measurement uses CLOCK_MONOTONIC.
 *
 * Releasing the threads on time. The four threads above all run on CPU 0.
 * By default a separate "releaser" thread on CPU 1 watches CLOCK_MONOTONIC
 * and wakes each thread at its start time through a condition variable, so
 * the wakeup arrives as an inter-processor interrupt. In our VirtualBox VM
 * (running on Hyper-V) timer interrupts reach a busy vCPU 10-200 ms late,
 * which scrambled the start order; cross-CPU wakeups arrive within 1 ms.
 * With -t each thread instead sleeps until its start time with
 * clock_nanosleep(TIMER_ABSTIME) on CPU 0 (fine on bare metal).
 *
 * Build: gcc -Wall -Wextra -O2 -pthread -o prio_chain prio_chain.c
 * Usage: sudo ./prio_chain [-t] [runs=5]  (root: SCHED_FIFO and mlockall)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define CEILING 30
#define MAX_EVENTS 64
#define MAX_RUNS 50

enum { L, D, H, X, NUM_THREADS };
static const char thread_name[] = "LDHX";
static const int thread_prio[] = {10, 20, 30, 25};
static const int thread_start_ms[] = {0, 100, 200, 300};

struct version {
    const char* name;
    int protocol;
    double predicted_block_ms, predicted_resp_ms;
};
static const struct version versions[] = {
    {"PRIO_NONE", PTHREAD_PRIO_NONE, 2500, 2500},
    {"PRIO_INHERIT", PTHREAD_PRIO_INHERIT, 500, 500},
    {"PRIO_PROTECT", PTHREAD_PRIO_PROTECT, 0, 400},
};
#define NUM_VERSIONS (int)(sizeof(versions) / sizeof(versions[0]))

struct event {
    int64_t t;
    char who;
    char what[32];
};

static pthread_mutex_t m1, m2;
static int64_t t0; /* scenario start, CLOCK_MONOTONIC ns */
static struct event events[MAX_EVENTS];
static int num_events;
static int64_t h_called, h_acquired;
static int use_timers; /* -t: clock_nanosleep instead of the releaser */

/* Releaser -> thread start signals. */
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int released[NUM_THREADS];

static int64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Record an event. Called by one thread at a time in practice (one CPU, all
 * FIFO), but the slot is still claimed atomically. Printing happens later so
 * it cannot disturb the timing. */
static int64_t note(int who, const char* what) {
    int64_t t = now_ns();
    int i = __atomic_fetch_add(&num_events, 1, __ATOMIC_RELAXED);

    if (i < MAX_EVENTS) {
        events[i].t = t;
        events[i].who = thread_name[who];
        snprintf(events[i].what, sizeof(events[i].what), "%s", what);
    }
    return t;
}

static int64_t cpu_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ms of CPU work by this thread. While preempted it makes no progress. */
static void busy_work(int ms) {
    int64_t end = cpu_ns() + (int64_t)ms * 1000000LL;

    while (cpu_ns() < end);
}

static int64_t start_time(int who) {
    return t0 + (int64_t)thread_start_ms[who] * 1000000LL;
}

/* Block until thread `who`'s start time. */
static void wait_for_release(int who) {
    if (use_timers) {
        int64_t t = start_time(who);
        struct timespec ts = {t / 1000000000LL, t % 1000000000LL};

        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) ==
               EINTR);
        return;
    }
    pthread_mutex_lock(&gate_lock);
    while (!released[who]) pthread_cond_wait(&gate_cv, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
}

/* Runs on CPU 1 at priority 99: spins on the clock (no timer interrupt
 * involved) and opens each thread's gate at its start time. */
static void* releaser(void* arg) {
    (void)arg;
    for (int who = 0; who < NUM_THREADS; who++) {
        while (now_ns() < start_time(who));
        pthread_mutex_lock(&gate_lock);
        released[who] = 1;
        pthread_cond_broadcast(&gate_cv);
        pthread_mutex_unlock(&gate_lock);
    }
    return NULL;
}

static void* thread_l(void* arg) {
    (void)arg;
    wait_for_release(L);
    note(L, "started");
    note(L, "lock M1 requested");
    pthread_mutex_lock(&m1);
    note(L, "lock M1 acquired");
    busy_work(600);
    note(L, "lock M1 released");
    pthread_mutex_unlock(&m1);
    note(L, "finished");
    return NULL;
}

static void* thread_d(void* arg) {
    (void)arg;
    wait_for_release(D);
    note(D, "started");
    note(D, "lock M2 requested");
    pthread_mutex_lock(&m2);
    note(D, "lock M2 acquired");
    note(D, "lock M1 requested");
    pthread_mutex_lock(&m1);
    note(D, "lock M1 acquired");
    busy_work(100);
    note(D, "lock M1 released");
    pthread_mutex_unlock(&m1);
    note(D, "lock M2 released");
    pthread_mutex_unlock(&m2);
    note(D, "finished");
    return NULL;
}

static void* thread_h(void* arg) {
    (void)arg;
    wait_for_release(H);
    note(H, "started");
    h_called = note(H, "lock M2 requested");
    pthread_mutex_lock(&m2);
    h_acquired = note(H, "lock M2 acquired");
    note(H, "lock M2 released");
    pthread_mutex_unlock(&m2);
    note(H, "finished");
    return NULL;
}

static void* thread_x(void* arg) {
    (void)arg;
    wait_for_release(X);
    note(X, "started");
    busy_work(2000);
    note(X, "finished");
    return NULL;
}

static void init_mutex(pthread_mutex_t* m, int protocol) {
    pthread_mutexattr_t attr;
    int rc;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, protocol);
    if (protocol == PTHREAD_PRIO_PROTECT)
        pthread_mutexattr_setprioceiling(&attr, CEILING);
    rc = pthread_mutex_init(m, &attr);
    if (rc) {
        fprintf(stderr, "pthread_mutex_init: %s\n", strerror(rc));
        exit(1);
    }
    pthread_mutexattr_destroy(&attr);
}

/* One run of the scenario. Fills the H measurements in ms. */
static void run_once(int protocol, double* block_ms, double* resp_ms) {
    void* (*body[])(void*) = {thread_l, thread_d, thread_h, thread_x};
    pthread_t tids[NUM_THREADS], rel;

    init_mutex(&m1, protocol);
    init_mutex(&m2, protocol);
    num_events = 0;
    for (int i = 0; i < NUM_THREADS; i++) released[i] = 0;
    /* Create every thread first; they all sleep until t0 + start. main runs
     * at priority 99, so none of them runs before main blocks in join. */
    t0 = now_ns() + 200 * 1000000LL;
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_attr_t attr;
        struct sched_param sp = {.sched_priority = thread_prio[i]};
        int rc;

        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024); /* mlockall'd */
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &sp);
        rc = pthread_create(&tids[i], &attr, body[i], NULL);
        if (rc) {
            fprintf(stderr, "pthread_create: %s (run as root)\n", strerror(rc));
            exit(1);
        }
        pthread_attr_destroy(&attr);
    }
    if (!use_timers) {
        pthread_attr_t attr;
        struct sched_param sp = {.sched_priority = 99};
        cpu_set_t cpu1;

        CPU_ZERO(&cpu1);
        CPU_SET(1, &cpu1);
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &sp);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu1), &cpu1);
        if (pthread_create(&rel, &attr, releaser, NULL) != 0) {
            fprintf(stderr, "releaser needs a second CPU; use -t\n");
            exit(1);
        }
        pthread_attr_destroy(&attr);
    }
    for (int i = 0; i < NUM_THREADS; i++) pthread_join(tids[i], NULL);
    if (!use_timers) pthread_join(rel, NULL);
    pthread_mutex_destroy(&m1);
    pthread_mutex_destroy(&m2);

    *block_ms = (h_acquired - h_called) / 1e6;
    *resp_ms = (h_acquired - (t0 + thread_start_ms[H] * 1000000LL)) / 1e6;
}

static void print_trace(const char* name) {
    printf("  event trace (%s, run 1):\n", name);
    for (int i = 0; i < num_events && i < MAX_EVENTS; i++)
        printf("    [%9.2f ms] %c  %s\n", (events[i].t - t0) / 1e6,
               events[i].who, events[i].what);
}

int main(int argc, char* argv[]) {
    int runs;
    struct sched_param sp = {.sched_priority = 99};
    cpu_set_t cpu0;
    double block[NUM_VERSIONS][MAX_RUNS], resp[NUM_VERSIONS][MAX_RUNS];

    if (argc > 1 && strcmp(argv[1], "-t") == 0) {
        use_timers = 1;
        argc--;
        argv++;
    }
    runs = argc > 1 ? atoi(argv[1]) : 5;
    if (runs < 1 || runs > MAX_RUNS) {
        fprintf(stderr, "usage: prio_chain [-t] [runs=5]  (1..%d)\n", MAX_RUNS);
        return 1;
    }
    /* Everything on one CPU: threads inherit main's affinity. */
    CPU_ZERO(&cpu0);
    CPU_SET(0, &cpu0);
    if (sched_setaffinity(0, sizeof(cpu0), &cpu0) != 0) {
        perror("sched_setaffinity");
        return 1;
    }
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        perror("sched_setscheduler(SCHED_FIFO) - run as root");
        return 1;
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) perror("mlockall");
    printf(
        "Part A: L(10) D(20) H(30) X(25) on CPU 0, SCHED_FIFO, %d runs "
        "per version\n",
        runs);
    printf("threads released by %s\n\n", use_timers
                                             ? "clock_nanosleep on CPU 0"
                                             : "the releaser thread on CPU 1");

    for (int v = 0; v < NUM_VERSIONS; v++) {
        printf("=== %s ===\n", versions[v].name);
        for (int r = 0; r < runs; r++) {
            run_once(versions[v].protocol, &block[v][r], &resp[v][r]);
            if (r == 0) print_trace(versions[v].name);
            printf("  run %d: H blocking %8.2f ms   H response %8.2f ms\n",
                   r + 1, block[v][r], resp[v][r]);
        }
        printf("\n");
    }

    printf("H, average of %d runs (ms)\n", runs);
    printf("%-13s %10s %10s   %10s %10s\n", "version", "block pred",
           "block meas", "resp pred", "resp meas");
    for (int v = 0; v < NUM_VERSIONS; v++) {
        double b = 0, s = 0;
        for (int r = 0; r < runs; r++) {
            b += block[v][r];
            s += resp[v][r];
        }
        printf("%-13s %10.1f %10.2f   %10.1f %10.2f\n", versions[v].name,
               versions[v].predicted_block_ms, b / runs,
               versions[v].predicted_resp_ms, s / runs);
    }
    return 0;
}
