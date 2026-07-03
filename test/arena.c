#define _GNU_SOURCE
#include "arena.h"
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#define ALIGNMENT 16

static size_t page_size(void)
{
    static long pg;
    if (!pg) pg = sysconf(_SC_PAGESIZE);
    return (size_t)pg;
}

static size_t align_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

static bool arena_grow(Arena *a, size_t needed)
{
    size_t end = (size_t)(a->ptr - a->base) + needed;
    if (end > a->reserved) return false;

    size_t pg  = page_size();
    size_t new_committed = align_up(end, pg);
    if (new_committed > a->reserved) new_committed = a->reserved;

    size_t grow  = new_committed - a->committed;
    void  *addr  = mmap(a->base + a->committed, grow,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         -1, 0);
    if (addr == MAP_FAILED) return false;

    a->committed = new_committed;
    return true;
}

bool arena_init_size(Arena *a, size_t virtual_size, size_t initial_commit)
{
    size_t pg = page_size();
    virtual_size   = align_up(virtual_size,   pg);
    initial_commit = align_up(initial_commit, pg);
    if (initial_commit > virtual_size) initial_commit = virtual_size;

    a->base = mmap(nullptr, virtual_size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                   -1, 0);
    if (a->base == MAP_FAILED) return false;

    a->reserved  = virtual_size;
    a->committed = 0;
    a->ptr       = a->base;
    a->owned     = true;

    if (initial_commit) {
        void *r = mmap(a->base, initial_commit,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                       -1, 0);
        if (r == MAP_FAILED) {
            munmap(a->base, virtual_size);
            return false;
        }
        a->committed = initial_commit;
    }
    return true;
}

bool arena_init(Arena *a)
{
    return arena_init_size(a, ARENA_DEFAULT_VIRTUAL_SIZE,
                              ARENA_DEFAULT_COMMIT_SIZE);
}

void arena_destroy(Arena *a)
{
    if (a && a->owned && a->base) {
        munmap(a->base, a->reserved);
        a->base = nullptr;
        a->ptr  = nullptr;
        a->committed = 0;
        a->reserved  = 0;
    }
}

void *arena_alloc(Arena *a, size_t size)
{
    if (!size) size = 1;
    size_t aligned = align_up(size, ALIGNMENT);

    if (a->ptr + aligned > a->base + a->committed) {
        if (!arena_grow(a, aligned)) return nullptr;
    }

    void *p = a->ptr;
    a->ptr += aligned;
    return p;
}

void *arena_alloc_zero(Arena *a, size_t size)
{
    void *p = arena_alloc(a, size);
    if (p) memset(p, 0, size);
    return p;
}

char *arena_strdup(Arena *a, const char *s)
{
    size_t len = strlen(s) + 1;
    char  *p   = (char *)arena_alloc(a, len);
    if (p) memcpy(p, s, len);
    return p;
}

void arena_reset(Arena *a)
{
    size_t used = (size_t)(a->ptr - a->base);
    if (used) madvise(a->base, used, MADV_DONTNEED);
    a->ptr = a->base;
}

size_t arena_save(Arena *a)
{
    return (size_t)(a->ptr - a->base);
}

void arena_restore(Arena *a, size_t mark)
{
    a->ptr = a->base + mark;
}
