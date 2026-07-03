#define _GNU_SOURCE
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

// Utility ────────────────────────────────────────────────────────────────

static long long ns(struct timespec t)
{
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static long rss_now(void)
{
    long rss = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { fscanf(f, "%*s %ld", &rss); fclose(f); }
    return rss * 4096 / 1024; // pages → KB
}

// Helper: allocate & touch in one step
static void *alloc_touch(Arena *a, size_t sz)
{
    void *p = arena_alloc(a, sz);
    if (p) ((volatile char *)p)[0] = 0;
    return p;
}

// 1. Frame loop ────────────────────────────────────────────────────────

static int bench_frames(void)
{
    enum { FRAMES = 100000, ALLOCS = 50, MAX_SZ = 512 };

    // ── malloc ──
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < FRAMES; f++) {
        void *p[ALLOCS];
        for (int i = 0; i < ALLOCS; i++) {
            size_t sz = (size_t)((f * ALLOCS + i) % MAX_SZ) + 1;
            p[i] = malloc(sz);
            if (!p[i]) return 1;
        }
        for (int i = 0; i < ALLOCS; i++)
            free(p[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long t_m = ns(t1) - ns(t0);
    long rss_m = rss_now();

    // ── arena (save/restore per frame) ──
    Arena a;
    if (!arena_init(&a)) return 1;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < FRAMES; f++) {
        size_t mark = arena_save(&a);
        for (int i = 0; i < ALLOCS; i++) {
            size_t sz = (size_t)((f * ALLOCS + i) % MAX_SZ) + 1;
            if (!alloc_touch(&a, sz)) return 1;
        }
        arena_restore(&a, mark);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long t_a = ns(t1) - ns(t0);
    long rss_a = rss_now();

    arena_destroy(&a);

    printf("[frames]    %d frames × %d allocs (1–%d bytes)\n",
           FRAMES, ALLOCS, MAX_SZ);
    printf("  malloc:  %4lld ms  %5ld KB RSS\n", t_m / 1000000, rss_m);
    printf("  arena:   %4lld ms  %5ld KB RSS  (%5.1fx)\n",
           t_a / 1000000, rss_a, (double)t_m / t_a);
    return 0;
}

// 2. Bulk deallocation ─────────────────────────────────────────────────

static int bench_bulk_free(void)
{
    enum { N = 2000000 };   // 2M allocs (fits in 1GB virtual)

    // Allocate everything first (not timed)
    void **ptrs = malloc((size_t)N * sizeof(void *));
    if (!ptrs) return 1;
    for (int i = 0; i < N; i++) {
        size_t sz = (size_t)(i % 256) + 1;
        ptrs[i] = malloc(sz);
        if (!ptrs[i]) return 1;
        ((volatile char *)ptrs[i])[0] = 0;
    }
    // Time free
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) free(ptrs[i]);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long t_free = ns(t1) - ns(t0);
    free(ptrs);

    // Arena
    Arena a;
    if (!arena_init(&a)) return 1;
    for (int i = 0; i < N; i++) {
        size_t sz = (size_t)(i % 256) + 1;
        if (!alloc_touch(&a, sz)) return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    arena_reset(&a);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long t_reset = ns(t1) - ns(t0);
    arena_destroy(&a);

    printf("[bulk-free] %d allocations, %s\n", N, "bulk deallocation");
    printf("  free:   %4lld ms  (%d calls)\n", t_free / 1000000, N);
    printf("  reset:  %4lld ms  ( 1 call)  (%lldx speedup)\n",
           t_reset / 1000000, t_free / (t_reset ? t_reset : 1));
    return 0;
}

// 3. Fragmentation stress ──────────────────────────────────────────────

static int bench_fragmentation(void)
{
    enum { ROUNDS = 5000, OBJS = 100, MAX_SZ = 1024 };

    printf("[frag]      %d rounds × %d objs (1–%d bytes, random free order)\n",
           ROUNDS, OBJS, MAX_SZ);

    // ── malloc: alloc/free in random order to fragment the heap ──
    // Store pointers in a flat array, free every round in  reverse order
    struct rusage ru;
    long rss_m = 0;
    size_t total_ptrs = (size_t)ROUNDS * OBJS;
    void **all = malloc(total_ptrs * sizeof(void *));
    if (!all) return 1;

    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < OBJS; i++)
            all[(size_t)r * OBJS + i] = malloc(
                (size_t)(((size_t)r * OBJS + i) % MAX_SZ) + 1);

        // Free previous round in reverse to fragment
        if (r > 0)
            for (int i = OBJS - 1; i >= 0; i--)
                free(all[(size_t)(r - 1) * OBJS + i]);
    }
    for (int i = 0; i < OBJS; i++)
        free(all[(size_t)(ROUNDS - 1) * OBJS + i]);

    getrusage(RUSAGE_SELF, &ru);
    rss_m = ru.ru_maxrss;
    free(all);

    // ── arena: simple reset each round ──
    Arena a;
    if (!arena_init(&a)) return 1;
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < OBJS; i++) {
            size_t sz = (size_t)(((size_t)r * OBJS + i) % MAX_SZ) + 1;
            if (!alloc_touch(&a, sz)) return 1;
        }
        arena_reset(&a);
    }
    getrusage(RUSAGE_SELF, &ru);
    long rss_a = ru.ru_maxrss;
    arena_destroy(&a);

    printf("  malloc:  %5ld KB peak RSS\n", rss_m);
    printf("  arena:   %5ld KB peak RSS  (%.1fx less)\n",
           rss_a, (double)rss_m / (rss_a ? rss_a : 1));
    return 0;
}

// 4. Overhead: metadata cost per allocation ────────────────────────────

static int bench_overhead(void)
{
    enum { N = 1000000, MAX_SZ = 256 };

    // Arena: how much total memory committed for N allocs
    Arena a;
    if (!arena_init(&a)) return 1;
    long rss_before = rss_now();

    for (int i = 0; i < N; i++) {
        size_t sz = (size_t)(i % MAX_SZ) + 1;
        if (!arena_alloc(&a, sz)) return 1;
    }
    size_t arena_bytes = a.committed;

    // Malloc: how much RSS increases for N allocs kept alive
    void **ptrs = malloc((size_t)N * sizeof(void *));
    if (!ptrs) return 1;
    rss_before = rss_now();

    for (int i = 0; i < N; i++) {
        size_t sz = (size_t)(i % MAX_SZ) + 1;
        ptrs[i] = malloc(sz);
        if (!ptrs[i]) return 1;
    }
    long rss_malloc = rss_now() - rss_before;

    // Free everything
    for (int i = 0; i < N; i++) free(ptrs[i]);
    free(ptrs);
    arena_destroy(&a);

    // Total bytes requested
    size_t total_requested = 0;
    for (int i = 0; i < N; i++)
        total_requested += (size_t)(i % MAX_SZ) + 1;

    printf("[overhead]  %d allocs, %.1f MB requested\n",
           N, (double)total_requested / 1000000);
    printf("  arena committed:  %zu KB  (%.0f KB overhead)\n",
           arena_bytes / 1000,
           (double)(arena_bytes - total_requested) / 1000);
    printf("  malloc RSS inc:   %ld KB  (%.0f KB overhead)\n",
           rss_malloc,
           (double)rss_malloc - (double)total_requested / 1000);
    printf("  arena metadata:   0 B/alloc   (just a ptr bump)\n");
    printf("  malloc metadata:  ~%zu B/alloc  (chunk header)\n",
           sizeof(size_t) * 2);  // typical glibc chunk overhead
    return 0;
}

// 5. Pre-grown frame loop (arena at its best) ─────────────────────────

static int bench_pre_grown(void)
{
    enum { FRAMES = 100000, ALLOCS = 100, MAX_SZ = 1024 };

    // Pre-grow arena to hold one frame's worth of allocations
    Arena a;
    if (!arena_init(&a)) return 1;
    size_t frame_sz = (size_t)ALLOCS * MAX_SZ;
    void *pre = arena_alloc(&a, frame_sz);
    if (!pre) return 1;
    memset(pre, 0, frame_sz);  // touch all pages → commit them
    a.ptr = a.base;            // rewind (no madvise — pages stay hot)

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < FRAMES; f++) {
        size_t mark = arena_save(&a);
        for (int i = 0; i < ALLOCS; i++) {
            size_t sz = (size_t)((f * ALLOCS + i) % MAX_SZ) + 1;
            void *p = arena_alloc(&a, sz);
            if (!p) return 1;
            ((volatile char *)p)[0] = 0;  // touch the memory
        }
        arena_restore(&a, mark);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long t_arena = ns(t1) - ns(t0);

    // Malloc: same workload but real alloc/free cycles
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < FRAMES; f++) {
        void *p[ALLOCS];
        for (int i = 0; i < ALLOCS; i++) {
            size_t sz = (size_t)((f * ALLOCS + i) % MAX_SZ) + 1;
            p[i] = malloc(sz);
            if (!p[i]) return 1;
            ((volatile char *)p[i])[0] = 0;
        }
        for (int i = 0; i < ALLOCS; i++) free(p[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long t_malloc = ns(t1) - ns(t0);

    arena_destroy(&a);

    printf("[pre-grown] %d frames × %d allocs (hot cache, %s)\n",
           FRAMES, ALLOCS, "no mmap during timed section");
    printf("  malloc:  %4lld ms\n", t_malloc / 1000000);
    printf("  arena:   %4lld ms  (%5.1fx)\n",
           t_arena / 1000000, (double)t_malloc / t_arena);
    return 0;
}

int main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("═══ Arena vs malloc: comprehensive benchmarks ═══\n\n");

    #define RUN(fn, label) do { \
        pid_t pid = fork(); \
        if (pid == 0) { int r = fn(); exit(r); } \
        int st; \
        waitpid(pid, &st, 0); \
        if (!WIFEXITED(st) || WEXITSTATUS(st)) \
            fprintf(stderr, "  %s: FAILED\n", label); \
    } while(0)

    RUN(bench_frames,       "frames");
    printf("\n");
    RUN(bench_pre_grown,    "pre-grown");
    printf("\n");
    RUN(bench_bulk_free,    "bulk-free");
    printf("\n");
    RUN(bench_fragmentation,"fragmentation");
    printf("\n");
    RUN(bench_overhead,     "overhead");

    printf("\n── Run with perf for deeper analysis ──\n");
    printf("perf stat -e cache-misses,faults,minor-faults \\\n");
    printf("  ./buildRelease/bench-arena\n");
    return 0;
}
