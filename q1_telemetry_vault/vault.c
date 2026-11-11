/*
 * CS F372 Assignment 3, Problem 1
 *
 * Mission Control telemetry vault: a monitor built from one pthread mutex and
 * condition variables, guarding a record of a version number and 8 fields.
 *
 *   6 analysts  - read, up to MAX_READERS at a time
 *   2 uplinks   - write, exclusive
 *   1 archiver  - snapshot every 12 s, exclusive
 *
 * Admission rule. Every request takes a ticket from a global counter and
 * joins one waiting list. The list is ordered by (urgent first, then ticket).
 * Only the request at the head may enter, and only when the vault is free
 * for it (an analyst needs no uplink/archiver inside and a free reader slot;
 * an uplink or the archiver needs the vault empty). So nobody overtakes an
 * older request, urgent requests come before non-urgent ones, and once the
 * archiver's request is queued every later request waits behind it while the
 * threads already inside finish. A request becomes URGENT after waiting 4 s
 * and is abandoned (TIMEOUT) after 10 s; the thread backs off 2 s and retries
 * with a new ticket.
 *
 * Condition variables (all use CLOCK_MONOTONIC, see init_condvars()):
 *   analyst_cv  - analysts wait here       (broadcast, see wake_next())
 *   uplink_cv   - uplinks wait here        (broadcast, see wake_next())
 *   archiver_cv - the archiver waits here  (signal,    see wake_next())
 *   stop_cv     - idle/backoff naps, so shutdown can cut them short
 *
 * The data itself is read and written outside the mutex; the monitor only
 * hands out access. The mutex release in vault_enter() and the acquire in
 * vault_exit() order those accesses, so there is no data race.
 *
 * Build: gcc -Wall -Wextra -O2 -pthread -o vault vault.c
 * Usage: ./vault [seconds=60] [seed]
 *
 * Test builds: -DTIMEOUT_MS=3000 -DURGENT_MS=1500 makes timeouts common;
 * -DBROKEN_EXCLUSION admits everyone at once, to show CORRUPT detection.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define NUM_ANALYSTS 6
#define NUM_UPLINKS 2
#define NUM_THREADS (NUM_ANALYSTS + NUM_UPLINKS + 1)
#define NUM_FIELDS 8
#define MAX_READERS 3

#define MS 1000000LL /* nanoseconds per millisecond */
/* Overridable at build time, e.g. -DTIMEOUT_MS=3000 to force timeouts. */
#ifndef URGENT_MS
#define URGENT_MS 4000
#endif
#ifndef TIMEOUT_MS
#define TIMEOUT_MS 10000
#endif
#define URGENT_AFTER (URGENT_MS * MS)
#define TIMEOUT_AFTER (TIMEOUT_MS * MS)
#define BACKOFF (2000 * MS)
#define ARCHIVE_PERIOD (12000 * MS)
#define ARCHIVE_HOLD (2000 * MS)

enum kind { ANALYST, UPLINK, ARCHIVER };
static const char* kind_name[] = {"Analyst", "Uplink", "Archiver"};

/* One per thread: its current request and its statistics. */
struct worker {
    enum kind kind;
    int id;
    unsigned int seed;
    /* current request (valid while queued) */
    long ticket;
    int64_t t_request;
    int urgent;
    /* statistics for the summary table */
    int accesses;
    int64_t max_wait;
    int timeouts;
};

/* The shared telemetry record. volatile: every field access really happens,
 * so an unprotected interleaving would show up as CORRUPT. */
static volatile struct {
    int version;
    int field[NUM_FIELDS];
} record;

/* Monitor state, all protected by vault_lock. */
static pthread_mutex_t vault_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t analyst_cv, uplink_cv, archiver_cv, stop_cv;
static int readers_inside;
static int uplink_inside;
static int archiver_inside;
static struct worker* waiting[NUM_THREADS]; /* unordered; see queue_head() */
static int num_waiting;
static long next_ticket = 1;
static int stopping;
static int corrupt_count;

static struct worker workers[NUM_THREADS];
static int64_t t_start;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static struct timespec to_timespec(int64_t ns) {
    struct timespec ts = {ns / 1000000000LL, ns % 1000000000LL};
    return ts;
}

/* Prints "[  12.34s] <message>". Lock order is vault_lock -> log_lock. */
static void logf_(const char* fmt, ...) {
    va_list ap;

    pthread_mutex_lock(&log_lock);
    printf("[%6.2fs] ", (now_ns() - t_start) / 1e9);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

/* Uniform random duration in [lo_ms, hi_ms], in nanoseconds. */
static int64_t rand_ms(struct worker* w, int lo_ms, int hi_ms) {
    return (lo_ms + rand_r(&w->seed) % (hi_ms - lo_ms + 1)) * MS;
}

static void init_condvars(void) {
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&analyst_cv, &attr);
    pthread_cond_init(&uplink_cv, &attr);
    pthread_cond_init(&archiver_cv, &attr);
    pthread_cond_init(&stop_cv, &attr);
    pthread_condattr_destroy(&attr);
}

/* ---- waiting list (caller holds vault_lock) ---------------------------- */

static int is_urgent(const struct worker* w, int64_t now) {
    return w->urgent || now - w->t_request > URGENT_AFTER;
}

/* Does a come before b? Urgent first, then by ticket. */
static int before(const struct worker* a, const struct worker* b, int64_t now) {
    int ua = is_urgent(a, now), ub = is_urgent(b, now);

    if (ua != ub) return ua;
    return a->ticket < b->ticket;
}

static struct worker* queue_head(int64_t now) {
    struct worker* best = NULL;

    for (int i = 0; i < num_waiting; i++)
        if (!best || before(waiting[i], best, now)) best = waiting[i];
    return best;
}

static void queue_remove(struct worker* w) {
    for (int i = 0; i < num_waiting; i++)
        if (waiting[i] == w) {
            waiting[i] = waiting[--num_waiting];
            return;
        }
}

/* Could w go in right now, ignoring everyone else who is waiting? */
static int vault_free_for(const struct worker* w) {
#ifdef BROKEN_EXCLUSION /* test build: no exclusion, CORRUPT must show up */
    (void)w;
    return 1;
#endif
    if (uplink_inside || archiver_inside) return 0;
    if (w->kind == ANALYST) return readers_inside < MAX_READERS;
    return readers_inside == 0;
}

static pthread_cond_t* cv_of(enum kind k) {
    return k == ANALYST ? &analyst_cv : k == UPLINK ? &uplink_cv : &archiver_cv;
}

/*
 * Called after every change that can let the head of the list in: a thread
 * leaving, a thread entering (the next analyst may follow it), a request
 * leaving the list on timeout, and a request turning urgent.
 *
 * Only the head can enter, so only the head's condition variable is woken.
 *   Analyst head  -> broadcast: up to 6 analysts sleep on analyst_cv and we
 *                    cannot choose which one pthread_cond_signal wakes. If it
 *                    woke a non-head analyst, that one would go back to sleep
 *                    and the head would miss the wakeup until its 4 s / 10 s
 *                    deadline. Broadcast wakes all; the head enters and calls
 *                    wake_next() again for the analyst behind it, the rest
 *                    see they are not the head and sleep again.
 *   Uplink head   -> broadcast, for the same reason: both uplinks may be
 *                    waiting and only one of them is the head.
 *   Archiver head -> signal: there is exactly one archiver thread, so at
 *                    most one waiter, and it is the head.
 */
static void wake_next(void) {
    struct worker* head = queue_head(now_ns());

    if (!head || !vault_free_for(head)) return;
    if (head->kind == ARCHIVER)
        pthread_cond_signal(&archiver_cv);
    else
        pthread_cond_broadcast(cv_of(head->kind));
}

/* ---- monitor entry and exit -------------------------------------------- */

enum { ADMITTED, TIMED_OUT, SHUTDOWN };

static void note_wait(struct worker* w, int64_t now) {
    if (now - w->t_request > w->max_wait) w->max_wait = now - w->t_request;
}

static const char* access_name(enum kind k) {
    return k == ANALYST ? "read" : k == UPLINK ? "write" : "exclusive access";
}

/* Request access and wait for it. Returns ADMITTED, TIMED_OUT or SHUTDOWN. */
static int vault_enter(struct worker* w) {
    int64_t now, deadline;
    struct timespec ts;

    pthread_mutex_lock(&vault_lock);
    if (stopping) {
        pthread_mutex_unlock(&vault_lock);
        return SHUTDOWN;
    }
    w->ticket = next_ticket++;
    w->t_request = now_ns();
    w->urgent = 0;
    waiting[num_waiting++] = w;
    logf_("%s %d requesting %s (ticket %ld)", kind_name[w->kind], w->id,
          access_name(w->kind), w->ticket);

    for (;;) {
        now = now_ns();
        if (stopping) { /* leave the list so no one waits on us */
            queue_remove(w);
            wake_next();
            pthread_mutex_unlock(&vault_lock);
            logf_("%s %d abandons ticket %ld for shutdown", kind_name[w->kind],
                  w->id, w->ticket);
            return SHUTDOWN;
        }
        /* Timeout is checked first: a request that has waited more than
         * 10 s is abandoned even if it could enter at this instant. */
        if (now - w->t_request > TIMEOUT_AFTER) {
            queue_remove(w);
            note_wait(w, now);
            w->timeouts++;
            logf_(
                "%s %d TIMEOUT on ticket %ld (waited %.1fs), backing off "
                "2s",
                kind_name[w->kind], w->id, w->ticket,
                (now - w->t_request) / 1e9);
            wake_next(); /* we may have been the head blocking others */
            pthread_mutex_unlock(&vault_lock);
            return TIMED_OUT;
        }
        if (queue_head(now) == w && vault_free_for(w)) break;

        if (!w->urgent && now - w->t_request > URGENT_AFTER) {
            w->urgent = 1;
            logf_("%s %d request is now URGENT (waited %.1fs)",
                  kind_name[w->kind], w->id, (now - w->t_request) / 1e9);
            wake_next(); /* the order may have changed */
        }
        /* Sleep until woken, or until we turn urgent / time out. */
        deadline =
            w->t_request + (w->urgent ? TIMEOUT_AFTER : URGENT_AFTER) + MS;
        ts = to_timespec(deadline);
        pthread_cond_timedwait(cv_of(w->kind), &vault_lock, &ts);
    }

    queue_remove(w);
    note_wait(w, now);
    if (w->kind == ANALYST) {
        readers_inside++;
        logf_("Analyst %d reading (readers active: %d, version: %d)", w->id,
              readers_inside, record.version);
    } else if (w->kind == UPLINK) {
        uplink_inside = 1;
        logf_("Uplink %d writing (version: %d -> %d)", w->id, record.version,
              record.version + 1);
    } else {
        archiver_inside = 1;
        logf_("Archiver %d archiving snapshot (version: %d)", w->id,
              record.version);
    }
    wake_next(); /* an analyst may let the next analyst in behind it */
    pthread_mutex_unlock(&vault_lock);
    return ADMITTED;
}

static void vault_exit(struct worker* w) {
    pthread_mutex_lock(&vault_lock);
    w->accesses++;
    if (w->kind == ANALYST) {
        readers_inside--;
        logf_("Analyst %d finished reading, idle", w->id);
    } else if (w->kind == UPLINK) {
        uplink_inside = 0;
        logf_(
            "Uplink %d finished writing (version: %d), waiting for next "
            "pass",
            w->id, record.version);
    } else {
        archiver_inside = 0;
        logf_("Archiver %d finished, idle until the next 12 s mark", w->id);
    }
    wake_next();
    pthread_mutex_unlock(&vault_lock);
}

/* Sleep until `until` (monotonic ns) or shutdown. Returns 0 on shutdown. */
static int nap_until(int64_t until) {
    struct timespec ts = to_timespec(until);
    int running;

    pthread_mutex_lock(&vault_lock);
    while (!stopping &&
           pthread_cond_timedwait(&stop_cv, &vault_lock, &ts) != ETIMEDOUT);
    running = !stopping;
    pthread_mutex_unlock(&vault_lock);
    return running;
}

static int nap(int64_t ns) { return nap_until(now_ns() + ns); }

/* ---- the three thread types -------------------------------------------- */

/* Request access, backing off and retrying on timeout. 0 on shutdown. */
static int get_access(struct worker* w) {
    for (;;) {
        int r = vault_enter(w);

        if (r == ADMITTED) return 1;
        if (r == SHUTDOWN || !nap(BACKOFF)) return 0;
    }
}

static void* analyst(void* arg) {
    struct worker* w = arg;

    while (nap(rand_ms(w, 1000, 4000)) && get_access(w)) {
        int64_t each = rand_ms(w, 1000, 2000) / NUM_FIELDS;
        int version = record.version, ok = 1;

        for (int i = 0; i < NUM_FIELDS; i++) {
            usleep(each / 1000);
            if (record.field[i] != version) ok = 0;
        }
        if (record.version != version) ok = 0;
        logf_("Analyst %d reading verified: %s (version: %d)", w->id,
              ok ? "CONSISTENT" : "CORRUPT", version);
        if (!ok) {
            pthread_mutex_lock(&vault_lock);
            corrupt_count++;
            pthread_mutex_unlock(&vault_lock);
        }
        vault_exit(w);
    }
    logf_("Analyst %d finished", w->id);
    return NULL;
}

static void* uplink(void* arg) {
    struct worker* w = arg;

    while (nap(rand_ms(w, 2000, 5000)) && get_access(w)) {
        int64_t each = rand_ms(w, 1000, 2000) / NUM_FIELDS;
        int version = record.version + 1;

        record.version = version;
        for (int i = 0; i < NUM_FIELDS; i++) {
            usleep(each / 1000); /* widen the window a broken lock would show */
            record.field[i] = version;
        }
        vault_exit(w);
    }
    logf_("Uplink %d finished", w->id);
    return NULL;
}

static void* archiver(void* arg) {
    struct worker* w = arg;
    int64_t next = t_start + ARCHIVE_PERIOD;

    while (nap_until(next) && get_access(w)) {
        int snapshot[1 + NUM_FIELDS], ok = 1;

        snapshot[0] = record.version;
        for (int i = 0; i < NUM_FIELDS; i++) {
            snapshot[1 + i] = record.field[i];
            if (snapshot[1 + i] != snapshot[0]) ok = 0;
        }
        usleep(ARCHIVE_HOLD / 1000);
        logf_("Archiver %d snapshot verified: %s (version: %d)", w->id,
              ok ? "CONSISTENT" : "CORRUPT", snapshot[0]);
        if (!ok) {
            pthread_mutex_lock(&vault_lock);
            corrupt_count++;
            pthread_mutex_unlock(&vault_lock);
        }
        vault_exit(w);
        while (next <= now_ns()) next += ARCHIVE_PERIOD;
    }
    logf_("Archiver %d finished", w->id);
    return NULL;
}

int main(int argc, char* argv[]) {
    int seconds = argc > 1 ? atoi(argv[1]) : 60;
    unsigned int seed =
        argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : (unsigned)time(NULL);
    pthread_t tids[NUM_THREADS];
    void* (*body[])(void*) = {analyst, uplink, archiver};

    if (seconds <= 0) {
        fprintf(stderr, "usage: %s [seconds=60] [seed]\n", argv[0]);
        return 1;
    }
    init_condvars();
    printf(
        "Telemetry vault: %d analysts (max %d readers), %d uplinks, 1 "
        "archiver, %d s, seed %u\n",
        NUM_ANALYSTS, MAX_READERS, NUM_UPLINKS, seconds, seed);

    t_start = now_ns();
    for (int i = 0; i < NUM_THREADS; i++) {
        struct worker* w = &workers[i];

        w->kind = i < NUM_ANALYSTS                 ? ANALYST
                  : i < NUM_ANALYSTS + NUM_UPLINKS ? UPLINK
                                                   : ARCHIVER;
        w->id = w->kind == ANALYST  ? i
                : w->kind == UPLINK ? i - NUM_ANALYSTS
                                    : 0;
        w->seed = seed * 31u + (unsigned)i;
        if (pthread_create(&tids[i], NULL, body[w->kind], w) != 0) {
            perror("pthread_create");
            return 1;
        }
        logf_("%s %d idle", kind_name[w->kind], w->id);
    }

    {
        struct timespec end = to_timespec(t_start + seconds * 1000 * MS);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &end, NULL));
    }

    pthread_mutex_lock(&vault_lock);
    stopping = 1;
    logf_("Shutdown requested, waiting for threads");
    /* Everyone must notice: every sleeper on every condition variable. */
    pthread_cond_broadcast(&analyst_cv);
    pthread_cond_broadcast(&uplink_cv);
    pthread_cond_broadcast(&archiver_cv);
    pthread_cond_broadcast(&stop_cv);
    pthread_mutex_unlock(&vault_lock);

    for (int i = 0; i < NUM_THREADS; i++) pthread_join(tids[i], NULL);
    logf_("All threads joined");

    printf("\n%-9s %3s %9s %11s %9s\n", "Thread", "ID", "Accesses",
           "MaxWait(s)", "Timeouts");
    for (int i = 0; i < NUM_THREADS; i++) {
        struct worker* w = &workers[i];
        printf("%-9s %3d %9d %11.2f %9d\n", kind_name[w->kind], w->id,
               w->accesses, w->max_wait / 1e9, w->timeouts);
    }
    printf("\nCORRUPT detections: %d\n", corrupt_count);
    printf("Final version: %d\n", record.version);
    return corrupt_count ? 1 : 0;
}
