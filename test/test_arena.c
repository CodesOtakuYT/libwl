#ifdef TEST_CANONICAL
#  include "libwl.h"
#else
#  include "arena.h"
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static void test_basic_alloc(void)
{
    Arena a;
    assert(arena_init(&a));

    void *p = arena_alloc(&a, 100);
    assert(p != nullptr);
    assert(a.ptr == a.base + 112); // 100 aligned up to 112 (next mult of 16)

    arena_destroy(&a);
    printf("  PASS basic_alloc\n");
}

static void test_alignment(void)
{
    Arena a;
    assert(arena_init(&a));

    void *p1 = arena_alloc(&a, 1);
    assert((uintptr_t)p1 % 16 == 0);

    void *p2 = arena_alloc(&a, 3);
    assert((uintptr_t)p2 % 16 == 0);

    // p2 starts at 16 past p1 (we always advance by aligned size)
    assert((char *)p2 == (char *)p1 + 16);

    arena_destroy(&a);
    printf("  PASS alignment\n");
}

static void test_auto_grow(void)
{
    Arena a;
    // Use tiny virtual size: 2 pages
    bool ok = arena_init_size(&a, 8192, 4096);
    assert(ok);

    // First alloc stays within initial commit
    void *p1 = arena_alloc(&a, 3000);
    assert(p1 != nullptr);
    assert(a.committed == 4096);

    // Second alloc should trigger grow
    void *p2 = arena_alloc(&a, 3000);
    assert(p2 != nullptr);
    assert(a.committed > 4096);

    arena_destroy(&a);
    printf("  PASS auto_grow\n");
}

static void test_oom(void)
{
    Arena a;
    // Tiny reservation: 1 page
    bool ok = arena_init_size(&a, 4096, 4096);
    assert(ok);

    // Should fail
    void *p = arena_alloc(&a, 8192);
    assert(p == nullptr);

    arena_destroy(&a);
    printf("  PASS oom\n");
}

static void test_reset_zeros(void)
{
    Arena a;
    assert(arena_init(&a));

    // Fill with pattern
    void *p = arena_alloc(&a, 256);
    assert(p != nullptr);
    memset(p, 0xAB, 256);

    // Reset
    arena_reset(&a);
    assert(a.ptr == a.base);

    // Re-alloc should get zeroed memory (MADV_DONTNEED + page fault zero-fill)
    void *q = arena_alloc(&a, 256);
    assert(q != nullptr);
    // Might be the same address after reset
    unsigned char *buf = (unsigned char *)q;
    for (int i = 0; i < 256; i++) assert(buf[i] == 0);

    arena_destroy(&a);
    printf("  PASS reset_zeros\n");
}

static void test_alloc_zero(void)
{
    Arena a;
    assert(arena_init(&a));

    // Alloc normal, set pattern
    void *p = arena_alloc(&a, 64);
    memset(p, 0xFF, 64);

    // Save & alloc_zero after it
    size_t m = arena_save(&a);
    (void)m; // just testing that save works
    void *z = arena_alloc_zero(&a, 128);
    assert(z != nullptr);
    for (int i = 0; i < 128; i++) assert(((unsigned char *)z)[i] == 0);

    arena_destroy(&a);
    printf("  PASS alloc_zero\n");
}

static void test_save_restore(void)
{
    Arena a;
    assert(arena_init(&a));

    void *p1 = arena_alloc(&a, 32);
    assert(p1 != nullptr);
    memset(p1, 0xAA, 32);

    size_t mark = arena_save(&a);

    void *p2 = arena_alloc(&a, 64);
    assert(p2 != nullptr);
    memset(p2, 0xBB, 64);

    // Restore — p2 is "freed", ptr back to where it was after p1 alloc
    arena_restore(&a, mark);
    assert(a.ptr == a.base + 32); // p1(32 bytes, 16-aligned = stays 32)

    // Re-alloc — should get same address as p2
    void *p3 = arena_alloc(&a, 64);
    assert(p3 == p2);

    arena_destroy(&a);
    printf("  PASS save_restore\n");
}

static void test_strdup(void)
{
    Arena a;
    assert(arena_init(&a));

    char *s = arena_strdup(&a, "hello arena");
    assert(s != nullptr);
    assert(strcmp(s, "hello arena") == 0);

    // Modify to ensure it's a copy
    s[0] = 'H';
    assert(s[0] == 'H');

    arena_destroy(&a);
    printf("  PASS strdup\n");
}

static void test_multi_arena(void)
{
    Arena a, b;
    assert(arena_init(&a));
    assert(arena_init(&b));

    void *pa = arena_alloc(&a, 64);
    void *pb = arena_alloc(&b, 64);
    assert(pa != nullptr);
    assert(pb != nullptr);

    // Should be in different memory regions
    assert(pa != pb);

    // Writing to one shouldn't affect the other
    memset(pa, 0x11, 64);
    memset(pb, 0x22, 64);
    assert(((unsigned char *)pa)[0] == 0x11);
    assert(((unsigned char *)pb)[0] == 0x22);

    arena_destroy(&a);
    arena_destroy(&b);
    printf("  PASS multi_arena\n");
}

static void test_stress(void)
{
    Arena a;
    assert(arena_init(&a));

    for (int i = 0; i < 100000; i++) {
        // Varying sizes, small
        size_t sz = (size_t)(i % 127) + 1;
        void *p = arena_alloc(&a, sz);
        assert(p != nullptr);
        // Write to ensure pages are touched
        ((unsigned char *)p)[0] = (unsigned char)i;
    }

    arena_reset(&a);
    assert(a.ptr == a.base);

    // After reset, new allocs succeed
    void *p = arena_alloc(&a, 100);
    assert(p != nullptr);

    arena_destroy(&a);
    printf("  PASS stress\n");
}

int main(void)
{
    test_basic_alloc();
    test_alignment();
    test_auto_grow();
    test_oom();
    test_reset_zeros();
    test_alloc_zero();
    test_save_restore();
    test_strdup();
    test_multi_arena();
    test_stress();
    printf("All tests passed.\n");
    return 0;
}
