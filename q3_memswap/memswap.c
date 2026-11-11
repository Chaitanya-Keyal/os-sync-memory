/*
 * CS F372 Assignment 3, Problem 3
 *
 * Memory pressure, locality and swap performance.
 *
 * One trial = one process:
 *   allocation      size_mib page-aligned 1 MiB chunks (posix_memalign)
 *   initialization  write (global page number % 256) to the first byte of
 *                   every page of every chunk
 *   access_1..5     5 passes of 10,000 one-byte lookups, one per selected
 *                   page; the page indices are generated before the pass is
 *                   timed and the random generator continues across passes
 *   deallocation    free every chunk
 *
 * Patterns:
 *   uniform  any page of the allocation, uniformly
 *   hot      first 10% of pages = hot region; 90% of lookups pick the hot
 *            region, 10% the rest, then a page uniformly inside it
 *
 * Each phase is timed with CLOCK_MONOTONIC. Around each phase (outside the
 * timed interval) we read the major-fault count (getrusage RUSAGE_SELF) and
 * pswpin/pswpout from /proc/vmstat (pages, VM-wide), and report the deltas,
 * swap traffic converted to KiB. Printing and file output happen only after
 * all phases are done.
 *
 * Every lookup adds the byte it read to a checksum. The expected checksum is
 * computed from the indices alone (page % 256), so a match proves every read
 * really happened and returned the initialized value; the reads go through a
 * volatile pointer so the compiler cannot drop them.
 *
 * Build: gcc -Wall -Wextra -O2 -o memswap memswap.c
 * Usage: ./memswap <size_mib> <uniform|hot> [seed=42] [backing=internal]
 *                  [csv_file]
 *        backing is only a label for the output (the swap device is chosen
 *        with swap_setup.sh). With csv_file, one row per phase is appended.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define CHUNK_SIZE (1024 * 1024)
#define PASSES 5
#define LOOKUPS 10000
#define HOT_FRACTION 10    /* hot region = first 1/10 of the pages */
#define HOT_PROBABILITY 90 /* percent of lookups that go to the hot region */

enum { PH_ALLOC, PH_INIT, PH_ACCESS1, PH_FREE = PH_ACCESS1 + PASSES, NUM_PH };

struct counters {
    long majflt;
    long pswpin, pswpout; /* pages */
};

struct phase {
    char name[16];
    double ms;
    long majflt, in_kib, out_kib;
    int has_checksum;
    uint64_t checksum, expected;
};

static long page_size;

/* ---- counters, read outside every timed interval ----------------------- */

static void read_counters(struct counters* c) {
    struct rusage ru;
    char key[64];
    long long val;
    FILE* f;

    getrusage(RUSAGE_SELF, &ru);
    c->majflt = ru.ru_majflt;
    c->pswpin = c->pswpout = 0;
    f = fopen("/proc/vmstat", "r");
    if (!f) {
        perror("/proc/vmstat");
        exit(1);
    }
    while (fscanf(f, "%63s %lld", key, &val) == 2) {
        if (strcmp(key, "pswpin") == 0) c->pswpin = val;
        if (strcmp(key, "pswpout") == 0) c->pswpout = val;
    }
    fclose(f);
}

static double now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Store the deltas between two counter readings in phase p. */
static void record(struct phase* p, const char* name, double t0, double t1,
                   const struct counters* a, const struct counters* b) {
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->ms = t1 - t0;
    p->majflt = b->majflt - a->majflt;
    /* pswpin/pswpout count pages; convert to KiB. */
    p->in_kib = (b->pswpin - a->pswpin) * (page_size / 1024);
    p->out_kib = (b->pswpout - a->pswpout) * (page_size / 1024);
}

/* ---- pseudo-random generator: splitmix64, seeded once per trial --------- */

static uint64_t rng_state;

static uint64_t rng_next(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform in [0, n). */
static uint64_t rng_below(uint64_t n) {
    return (uint64_t)(((unsigned __int128)rng_next() * n) >> 64);
}

int main(int argc, char* argv[]) {
    long size_mib;
    int hot;
    uint64_t seed;
    const char *backing, *csv_path;
    size_t num_chunks, pages_per_chunk, total_pages, hot_pages;
    unsigned char** chunks;
    size_t* idx;
    struct phase ph[NUM_PH] = {0};
    struct counters c0, c1;
    double t0, t1;
    int all_ok = 1;

    if (argc < 3 || (strcmp(argv[2], "uniform") && strcmp(argv[2], "hot")) ||
        (size_mib = strtol(argv[1], NULL, 10)) <= 0) {
        fprintf(stderr,
                "usage: %s <size_mib> <uniform|hot> [seed=42] "
                "[backing=internal] [csv_file]\n",
                argv[0]);
        return 1;
    }
    hot = strcmp(argv[2], "hot") == 0;
    seed = argc > 3 ? strtoull(argv[3], NULL, 10) : 42;
    backing = argc > 4 ? argv[4] : "internal";
    csv_path = argc > 5 ? argv[5] : NULL;

    page_size = sysconf(_SC_PAGESIZE);
    num_chunks = (size_t)size_mib;
    pages_per_chunk = CHUNK_SIZE / page_size;
    total_pages = num_chunks * pages_per_chunk;
    hot_pages = total_pages / HOT_FRACTION;
    rng_state = seed;

    chunks = calloc(num_chunks, sizeof(*chunks));
    idx = malloc(LOOKUPS * sizeof(*idx));
    if (!chunks || !idx) {
        perror("malloc");
        return 1;
    }

    /* Allocation. */
    read_counters(&c0);
    t0 = now_ms();
    for (size_t i = 0; i < num_chunks; i++) {
        void* p;
        int rc = posix_memalign(&p, page_size, CHUNK_SIZE);
        if (rc != 0) {
            fprintf(stderr, "posix_memalign failed at chunk %zu: %s\n", i,
                    strerror(rc));
            return 1;
        }
        chunks[i] = p;
    }
    t1 = now_ms();
    read_counters(&c1);
    record(&ph[PH_ALLOC], "allocation", t0, t1, &c0, &c1);

    /* Initialization: first byte of every page. */
    read_counters(&c0);
    t0 = now_ms();
    for (size_t i = 0; i < num_chunks; i++)
        for (size_t j = 0; j < pages_per_chunk; j++)
            chunks[i][j * page_size] =
                (unsigned char)((i * pages_per_chunk + j) % 256);
    t1 = now_ms();
    read_counters(&c1);
    record(&ph[PH_INIT], "initialization", t0, t1, &c0, &c1);

    /* Access passes. */
    for (int pass = 0; pass < PASSES; pass++) {
        struct phase* p = &ph[PH_ACCESS1 + pass];
        uint64_t sum = 0, expected = 0;
        char name[16];

        /* Indices first, untimed. The generator is not reseeded. */
        for (int k = 0; k < LOOKUPS; k++) {
            size_t page;
            if (!hot)
                page = rng_below(total_pages);
            else if (rng_below(100) < HOT_PROBABILITY)
                page = rng_below(hot_pages);
            else
                page = hot_pages + rng_below(total_pages - hot_pages);
            idx[k] = page;
            expected += page % 256;
        }

        read_counters(&c0);
        t0 = now_ms();
        for (int k = 0; k < LOOKUPS; k++) {
            size_t page = idx[k];
            volatile unsigned char* base = chunks[page / pages_per_chunk];
            sum += base[(page % pages_per_chunk) * page_size];
        }
        t1 = now_ms();
        read_counters(&c1);

        snprintf(name, sizeof(name), "access_%d", pass + 1);
        record(p, name, t0, t1, &c0, &c1);
        p->has_checksum = 1;
        p->checksum = sum;
        p->expected = expected;
        if (sum != expected) all_ok = 0;
    }

    /* Deallocation. */
    read_counters(&c0);
    t0 = now_ms();
    for (size_t i = 0; i < num_chunks; i++) free(chunks[i]);
    t1 = now_ms();
    read_counters(&c1);
    record(&ph[PH_FREE], "deallocation", t0, t1, &c0, &c1);

    /* Report. */
    printf("%ld MiB | %s | %s swap | seed %" PRIu64 "\n", size_mib,
           hot ? "hot" : "uniform", backing, seed);
    printf(
        "%d passes, %d lookups each; page size %ld B, %zu pages, hot "
        "region %zu pages; swap traffic is VM-wide\n\n",
        PASSES, LOOKUPS, page_size, total_pages, hot ? hot_pages : 0);
    printf("%-15s %12s %8s %10s %10s %10s\n", "Phase", "Time (ms)", "Major",
           "In (KiB)", "Out (KiB)", "Checksum");
    for (int i = 0; i < NUM_PH; i++) {
        printf("%-15s %12.3f %8ld %10ld %10ld ", ph[i].name, ph[i].ms,
               ph[i].majflt, ph[i].in_kib, ph[i].out_kib);
        if (ph[i].has_checksum)
            printf("%10" PRIu64 "%s\n", ph[i].checksum,
                   ph[i].checksum == ph[i].expected ? "" : "  MISMATCH");
        else
            printf("%10s\n", "-");
    }
    printf("\n%s\n", all_ok ? "Completed: all 5 checksums verified."
                            : "ERROR: checksum mismatch.");

    if (csv_path) {
        FILE* f = fopen(csv_path, "a");
        if (!f) {
            perror(csv_path);
            return 1;
        }
        if (ftell(f) == 0)
            fprintf(f,
                    "backing,size_mib,pattern,seed,phase,time_ms,major,"
                    "in_kib,out_kib,checksum\n");
        for (int i = 0; i < NUM_PH; i++)
            fprintf(f,
                    "%s,%ld,%s,%" PRIu64 ",%s,%.3f,%ld,%ld,%ld,%" PRIu64 "\n",
                    backing, size_mib, hot ? "hot" : "uniform", seed,
                    ph[i].name, ph[i].ms, ph[i].majflt, ph[i].in_kib,
                    ph[i].out_kib, ph[i].has_checksum ? ph[i].checksum : 0);
        fclose(f);
    }
    free(idx);
    free(chunks);
    return all_ok ? 0 : 1;
}
