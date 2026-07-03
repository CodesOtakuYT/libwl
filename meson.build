project('libwl', 'c',
        version : '0.1',
        default_options : ['warning_level=2', 'c_std=c23'])

wayland_client  = dependency('wayland-client')
libdrm          = dependency('libdrm')
gbm             = dependency('gbm')
vulkan          = dependency('vulkan')
libsystemd      = dependency('libsystemd')

libwl_inc = include_directories('include', 'proto')

arena_src = 'src/arena.c'

executable('libwl-demo',
    'demo/demo.c', 'src/libwl.c', arena_src,
    'proto/xdg-shell-protocol.c',
    'proto/linux-dmabuf-unstable-v1-protocol.c',
    include_directories: libwl_inc,
    dependencies: [wayland_client, libdrm, gbm, vulkan, libsystemd],
    install: false)

# Arena tests

## 1. Standalone copy (test/arena.h + test/arena.c) — no libwl dependency
test_arena_std = executable('test-arena-std',
    ['test/arena.c', 'test/test_arena.c'],
    include_directories: include_directories('test'))
test('arena-std', test_arena_std)

## 2. Canonical implementation (src/arena.c + include/libwl.h)
test_arena_canonical = executable('test-arena-canonical',
    ['src/arena.c', 'test/test_arena.c'],
    include_directories: libwl_inc,
    c_args: '-DTEST_CANONICAL')
test('arena-canonical', test_arena_canonical)

## 3. Benchmark (uses standalone copy, no libwl dependency)
bench_arena = executable('bench-arena',
    ['test/arena.c', 'test/bench_arena.c'],
    include_directories: include_directories('test'))
