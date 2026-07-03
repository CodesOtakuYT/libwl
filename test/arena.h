#ifndef ARENA_H
#define ARENA_H

#include <stdbool.h>
#include <stddef.h>

#define ARENA_DEFAULT_VIRTUAL_SIZE  ((size_t)1 << 30)  // 1 GB
#define ARENA_DEFAULT_COMMIT_SIZE   ((size_t)64 << 10) // 64 KB

typedef struct {
    char   *base;
    char   *ptr;
    size_t  committed;
    size_t  reserved;
    bool    owned;
} Arena;

[[nodiscard]] bool  arena_init(Arena *a);
[[nodiscard]] bool  arena_init_size(Arena *a, size_t virtual_size, size_t initial_commit);
void                arena_destroy(Arena *a);
[[nodiscard]] void *arena_alloc(Arena *a, size_t size);
void               *arena_alloc_zero(Arena *a, size_t size);
char               *arena_strdup(Arena *a, const char *s);
void                arena_reset(Arena *a);
size_t              arena_save(Arena *a);
void                arena_restore(Arena *a, size_t mark);

#endif
