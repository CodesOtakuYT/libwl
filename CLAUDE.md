# libwl — Linux Graphics from Scratch

## Build
```sh
meson setup buildDir --reconfigure
ninja -C buildDir
```

## Project Structure
- `src/libwl.c` — the library implementation (Vulkan, GBM, Wayland, sd_event)
- `include/libwl.h` — public API header (transparent struct design)
- `demo/demo.c` — demo application using the library
- `TUTORIAL.md` — 16-chapter book explaining every concept
- `proto/` — generated Wayland protocol code (xdg-shell, linux-dmabuf)
- `PROCESS.md` — development methodology (read it first)

## Development Process
Follow the process documented in `.opencode/instructions/process.md`:
1. Research → 2. Prototype → 3. Cross-Reference → 4. Verify & Iterate → 5. Document

## Key Conventions
- C23 standard (`c_std=c23` in meson.build)
- Linux-native methods over POSIX (e.g., `memfd_create`, `pthread_sigmask`)
- `nullptr` instead of `NULL`
- `bool` (built-in C23) for boolean flags
- `[[nodiscard]]` on functions whose return values must not be ignored
- `[[maybe_unused]]` on unused parameters instead of `(void)var;`
- Transparent struct design — all handles are public fields

## Cross-Reference
When implementing or modifying any Wayland/Vulkan/GBM code, compare against:
- Weston reference clients: `simple-shm.c`, `simple-egl.c`, `simple-dmabuf-egl.c`
- Wayland protocol specs (`.xml` files in `/usr/share/wayland-protocols/`)
- Vulkan spec for `VK_EXT_external_memory_dma_buf`

## Important Files to Read First
1. `TUTORIAL.md` — explains the mental model for every component
2. `include/libwl.h` — the full API surface (~100 lines)
3. `src/libwl.c` — the implementation (~900 lines)
4. `PROCESS.md` — how we work
