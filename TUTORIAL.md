# Linux Graphics from Scratch: A Book in 14 Chapters

*by **Codotaku** — a human and an AI writing together, documenting every stumble along the way*

## Who This Is For

You know C. You've heard of Wayland, Vulkan, and systemd. You want to understand how they fit together — not just "how to use the API" but *why the APIs exist, what problem each solves, and how to think in their mental models.*

We're building toward one concrete thing: **a composable Wayland window with zero-copy dmabuf buffers, Vulkan rendering, and sd_event dispatch.** The final code is a ~900-line library. But the library is a gift — the real goal is the understanding you accumulate along the way.

## Setup

```
OS:        Arch Linux
GPU:       NVIDIA RTX 3050
DE:        KDE Plasma (Wayland session)
Compiler:  GCC 16 / Clang
Build:     Meson 1.11 + Ninja
IDE:       CLion
Vulkan:    1.4 (system SDK)
```

Before we start, install the essentials:

```sh
sudo pacman -S wayland wayland-protocols libdrm gbm vulkan-headers vulkan-icd-loader \
               libglvnd egl-wayland mesa systemd-libs
```

This gives you the libraries we'll link against. Each chapter introduces its own dependencies.

## How to Read This

Every chapter ends with a **working program** you can compile and run. Type the code yourself. Don't copy-paste — the muscle memory of typing it helps the mental model stick.

Occasionally I (the AI) made mistakes while writing this. I'll flag them with **AI Mistake** so you learn to spot the kind of bugs AI code generators produce.

---

## Chapter 1: Meson — The Build System

Before we write a single C expression, we need to understand how the build works. Meson is our build system. Ninja is the backend that actually runs the compiler commands.

### Why Meson?

Linux has a lot of build systems:
- **Makefiles:** raw, manual, you write every compile rule. Fine for tiny projects, hell for anything with dependencies.
- **Autotools:** `./configure && make`. The old standard. Portable to every Unix ever made. Also a confusing maze of m4 macros and generated shell scripts.
- **CMake:** The industry standard. Works cross-platform. Syntax is... polarizing.
- **Meson:** Python-like syntax, fast (generates Ninja files instead of Makefiles), first-class dependency handling via pkg-config.

For a project that links against `libwayland-client`, `libdrm`, `libgbm`, `libvulkan`, and `libsystemd`, Meson's `dependency()` function is a lifesaver. It calls `pkg-config` under the hood and gives you the compiler flags automatically.

### The Simple Meson Build

Create `meson.build`:

```meson
project('tutorial', 'c',
    default_options : ['warning_level=2', 'c_std=c11'])

executable('01-hello', 'main.c', install: false)
```

Then:

```sh
meson setup build
ninja -C build
./build/01-hello
```

**What's happening:**

- `project()` declares the project name and language. `warning_level=2` enables `-Wall` and friends. `c_std=c11` sets the C standard.
- `executable()` says "build this source file into this binary." `install: false` means we don't try to install it system-wide when we run `meson install`.
- `meson setup build` creates a `build/` directory with Ninja files. You only run this once, or when you change `meson.build`.
- `ninja -C build` compiles. Ninja only recompiles files that changed.

### Adding Dependencies

Let's link against `wayland-client`:

```meson
project('tutorial', 'c',
    default_options : ['warning_level=2', 'c_std=c11'])

wayland_client = dependency('wayland-client')

executable('01-wayland', 'main.c', dependencies: [wayland_client], install: false)
```

`dependency()` calls `pkg-config --cflags --libs wayland-client` and stores the result. When you add it to `dependencies:`, Meson automatically adds the necessary `-I` and `-l` flags to the compiler command.

As we add more libraries — `gbm`, `libdrm`, `vulkan`, `libsystemd` — we'll add more `dependency()` lines and add them to the array. You'll see this grow naturally across the chapters.

### The Build Directory

Meson creates a build directory with a `compile_commands.json` file. CLion and other IDEs read this file to know what compiler flags to use for code completion. If you use CLion, point it at this project and it will find `compile_commands.json` automatically.

**AI Mistake:** I once forgot that `meson setup build` only needs to be run once. The user ran it every time, which works but is slow. After the initial `setup`, just run `ninja -C build`. Only re-run `meson setup build --reconfigure` if you change the `meson.build` structure.

### Reference

- [Meson documentation: Basic usage](https://mesonbuild.com/Quick-guide.html)
- [Meson documentation: Dependencies](https://mesonbuild.com/Dependencies.html)
- `man pkg-config` — understand what Meson calls under the hood

---

## Chapter 2: sd_event — The Heartbeat

### The Mental Model of Event Loops

Nearly every GUI application follows the same pattern:

```
while (running) {
    event = wait_for_something_to_happen();
    handle(event);
}
```

This is called an **event loop** or a **dispatch loop**. Without it, your program starts, runs once, and exits. With it, your program stays alive, waiting for input, timers, or network data.

The fundamental building block is a system call called `poll()` (or `epoll()` on Linux). You give it a list of file descriptors you're interested in, and it blocks until at least one of them has data available.

`poll()` is powerful but raw. You have to:
1. Create the fd set
2. Call poll()
3. Check each fd to see if it fired
4. Dispatch manually
5. Loop

`sd_event` is a higher-level wrapper built on `epoll` that does all this for you. You tell it "call this function when this fd has data" or "call this function in 2 seconds" or "call this function when SIGINT arrives." It handles the polling and dispatching.

### Why sd_event?

Alternatives for event loops on Linux:
- **`epoll` directly:** You do everything yourself. Fine for a dedicated server. Tedious for a GUI app where you also need timers and signal handling.
- **GLib main loop:** Part of GTK, heavy dependency, pulls in glib, gobject, etc.
- **libevent / libuv:** Battle-tested network event libraries. Great for async I/O, but designed for servers, not desktop apps.
- **sd_event:** Part of systemd (already installed on Arch). Clean API, integrates signalfd natively (no signal handling hacks), handles timers, I/O, signals, and child process exits in one unified loop.

Since systemd is already on every Arch Linux system, sd_event is zero extra cost.

### The Simplest sd_event Program

Create `02-timer.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <systemd/sd-event.h>

static int on_timer(sd_event_source *s, void *userdata) {
    printf("Timer fired! Exiting.\n");
    sd_event_exit(sd_event_source_get_event(s), 0);
    return 0;
}

int main(void) {
    sd_event *event;
    int r = sd_event_new(&event);
    if (r < 0) { fprintf(stderr, "sd_event_new failed\n"); return 1; }

    r = sd_event_add_time(event, NULL, CLOCK_MONOTONIC,
                          sd_now(CLOCK_MONOTONIC) + 2000000, 0,
                          on_timer, NULL);
    if (r < 0) { fprintf(stderr, "sd_event_add_time failed\n"); return 1; }

    r = sd_event_loop(event);
    sd_event_unref(event);

    return r < 0 ? 1 : 0;
}
```

Compile with:

```meson
project('tutorial', 'c', default_options : ['warning_level=2', 'c_std=c11'])
systemd = dependency('libsystemd')
executable('02-timer', '02-timer.c', dependencies: [systemd], install: false)
```

**What's happening:**

- `sd_event_new(&event)` creates an event loop object. This allocates an `epoll` fd internally.
- `sd_event_add_time()` registers a timer. `sd_now(CLOCK_MONOTONIC) + 2000000` means "2 seconds from now" in microseconds. When the timer fires, `on_timer` is called.
- Inside `on_timer`, `sd_event_exit()` tells the loop to stop running. `sd_event_source_get_event()` recovers the event pointer from the source handle.
- `sd_event_loop(event)` blocks until `sd_event_exit` is called.
- `sd_event_unref(event)` frees the event loop.

### Adding Signal Handling

A program that ignores SIGINT (Ctrl+C) is annoying. Let's handle it:

```c
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <systemd/sd-event.h>

static int on_timer(sd_event_source *s, void *userdata) {
    (void)userdata;
    printf("Timer fired!\n");
    return 1;  /* return 1 to keep the timer firing */
}

static int on_signal(sd_event_source *s, const struct signalfd_siginfo *si,
                     void *userdata) {
    (void)s; (void)si; (void)userdata;
    printf("Saw signal, exiting cleanly.\n");
    sd_event_exit(sd_event_source_get_event(s), 0);
    return 0;
}

int main(void) {
    sd_event *event;
    int r = sd_event_new(&event);
    if (r < 0) return 1;

    /* Timer: fires every 1 second, starting 1 second from now */
    r = sd_event_add_time(event, NULL, CLOCK_MONOTONIC,
                          sd_now(CLOCK_MONOTONIC) + 1000000, 1000000,
                          on_timer, NULL);
    if (r < 0) return 1;

    /* Block signals so they go to signalfd instead of killing the process */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    r = sd_event_add_signal(event, NULL, SIGINT, on_signal, NULL);
    if (r < 0) return 1;
    r = sd_event_add_signal(event, NULL, SIGTERM, on_signal, NULL);
    if (r < 0) return 1;

    sd_event_loop(event);
    sd_event_unref(event);
    return 0;
}
```

**AI Mistake:** I forgot `sigprocmask(SIG_BLOCK, &mask, NULL)` in the first version. sd_event uses `signalfd` under the hood, which only works when the signals are blocked from normal delivery. Without this, SIGINT still kills the process before sd_event can catch it. This is a subtle point — signalfd intercepts blocked signals. If you don't block them, they go to the default handler (kill).

**Key concept — signalfd:** Instead of setting a signal handler with `signal()` or `sigaction()`, signalfd creates a file descriptor you can poll. When a signal arrives, the fd becomes readable, and you read a `signalfd_siginfo` struct with details about the signal. sd_event wraps this completely — you just use `sd_event_add_signal`.

### What We Learned

- Event loops are `wait → handle → wait → handle`
- sd_event wraps epoll + signalfd + timers into one API
- Signals must be blocked before they can be caught via signalfd

### Reference

- `man sd_event_new`, `man sd_event_add_io`, `man sd_event_add_signal`
- `man signalfd` — the underlying mechanism
- `man epoll` — the underlying mechanism
- [systemd sd_event documentation](https://www.freedesktop.org/software/systemd/man/latest/sd-event.html)

---

## Chapter 3: Wayland — Not a Server, a Conversation

### The Mental Model

If you come from X11, you have a mental model of "display server" where your app sends requests and the server renders them remotely. Wayland is fundamentally different:

- **Wayland is a protocol, not a server.** It's a way for your app and the compositor to exchange messages over a Unix socket.
- **There is no remote rendering.** Your app renders to buffers in its own process memory (or GPU memory). It hands those buffers to the compositor via the protocol. The compositor composites them onto the screen.
- **Everything is an object.** `wl_display`, `wl_compositor`, `wl_surface`, `xdg_toplevel` — these are all objects. You send requests to them and receive events from them.

The protocol works like this:
1. You connect to the compositor's socket
2. You send requests (e.g., "create a surface")
3. The compositor sends events back (e.g., "here's your surface object", "configure your window to 800x600")
4. You roundtrip or dispatch

### Connecting to the Display

```c
#include <stdio.h>
#include <wayland-client.h>

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "Can't connect to Wayland compositor.\n");
        fprintf(stderr, "Is WAYLAND_DISPLAY set? Is a compositor running?\n");
        return 1;
    }
    printf("Connected to Wayland display!\n");
    wl_display_disconnect(display);
    return 0;
}
```

`wl_display_connect(NULL)` reads the `$WAYLAND_DISPLAY` environment variable (default: `wayland-0`) and connects to the Unix socket at `$XDG_RUNTIME_DIR/wayland-0`. In KDE Plasma on Wayland, this socket is always available.

### The Registry — Meeting the Compositor's Objects

A Wayland compositor provides several **global** objects — things like `wl_compositor` (to create surfaces), `xdg_wm_base` (to create windows), `zwp_linux_dmabuf_v1` (to share GPU buffers), `wl_seat` (for input). To discover what globals exist, you ask the registry:

```c
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

static void on_global(void *data, struct wl_registry *registry,
                      uint32_t name, const char *interface, uint32_t version) {
    printf("Global: %s (version %u, name %u)\n", interface, version, name);
}

static void on_global_remove(void *data, struct wl_registry *registry,
                             uint32_t name) {
    printf("Global removed: %u\n", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    /* Roundtrip: flush outgoing requests, then block until all
     * pending events are received and dispatched. */
    wl_display_roundtrip(display);

    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
```

**The roundtrip is essential.** When you call `wl_display_get_registry`, libwayland queues a "get registry" request in the send buffer. Nothing is actually sent until you either:
- Call `wl_display_roundtrip()` — sends everything, blocks until the compositor responds, then dispatches all events
- Call `wl_display_dispatch()` — flushes then blocks waiting for events
- Call `wl_display_flush()` — sends without waiting for response

Without a roundtrip or dispatch, your registry request sits in the buffer and the compositor never sees it.

**AI Mistake:** In the first version of libwl, I set up the registry, added the listener, and then relied on sd_event to dispatch events later. But the event sources (Wayland fd) weren't added to sd_event yet — they were added after `libwl_create` returned. So the registry request sat in the buffer, unflushed, during the gap between `libwl_create` and `libwl_run`. The fix was using `wl_display_roundtrip` in `create` to ensure all init globals are received synchronously before proceeding.

### Binding a Global

Printing globals is nice. Actually using them is better:

```c
static struct wl_compositor *compositor = NULL;

static void on_global(void *data, struct wl_registry *registry,
                      uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(registry, name,
                                      &wl_compositor_interface, 4);
    }
}
```

`wl_registry_bind` tells the compositor "I want to use this global." It returns a pointer to the bound object. You specify the interface (`&wl_compositor_interface`) and the version you want (4 is the latest stable version of `wl_compositor`).

The compositor might not support version 4 — in production you'd check the version argument in `on_global` and bind the min of what you need and what's available. For our tutorial, version 4 works on KDE Plasma 6.

### Creating a Surface

```c
struct wl_surface *surface = wl_compositor_create_surface(compositor);
```

A `wl_surface` is a rectangular pixel canvas. By itself it's invisible — it has no window decorations, no title bar, no close button. Those come from `xdg_surface` and `xdg_toplevel` in the next chapter.

### The Full Picture

Let's put it together into a program that connects to Wayland, gets the compositor, creates a surface, and prints confirmation:

```c
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

static struct wl_compositor *compositor = NULL;

static void on_global(void *data, struct wl_registry *registry,
                      uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(registry, name,
                                      &wl_compositor_interface, 4);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = NULL,
};

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor) {
        fprintf(stderr, "Compositor doesn't support wl_compositor\n");
        return 1;
    }

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    printf("Surface created: %p\n", (void*)surface);

    wl_surface_destroy(surface);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
```

### Reference

- [wayland-book.com](https://wayland-book.com/) — the best Wayland tutorial, written by Drew DeVault
- `man wl_display_connect` — libwayland-client man pages
- [Wayland protocol definition](https://gitlab.freedesktop.org/wayland/wayland/-/blob/main/protocol/wayland.xml) — the actual protocol spec

---

## Chapter 4: xdg-shell — Making a Real Window

### Why xdg-shell Exists

A `wl_surface` is just a pixel canvas. It has no concept of "window" — no decorations, no title bar, no close button, no minimize/maximize, no positioning. The compositor doesn't know if a surface is meant to be a fullscreen game, a tooltip, a video player, or a dialog.

xdg-shell is the Wayland protocol that adds window semantics. It's a **stable** protocol (since Wayland 1.17), meaning it won't change. There was an earlier experiment called `wl_shell` (deprecated) and a "unstable" xdg-shell before it stabilized. The stable version is `xdg_wm_base`.

### The Protocol Hierarchy

```
wl_surface                    — pixel canvas
  └── xdg_surface             — adds compositor-driven semantics
        └── xdg_toplevel       — a standard window (has titlebar, can be maximized)
        └── xdg_popup           — a popup menu/tooltip (dismissed on click outside)
        └── xdg_positioner      — helper for positioning popups
```

For our window, we need:
1. `wl_surface` — the canvas
2. `xdg_surface` — wraps the canvas with window protocol
3. `xdg_toplevel` — says "this is a normal window"

### The Configure/Ack Dance

This is the most important concept in xdg-shell:

1. You create the surface → xdg_surface → xdg_toplevel
2. You commit the surface (`wl_surface_commit`)
3. The compositor receives the commit and responds with:
   - `xdg_toplevel.configure` — tells you "your window should be WxH" (or 0x0 for "you decide")
   - `xdg_surface.configure` — gives you a serial number
4. You MUST call `xdg_surface_ack_configure(surface, serial)` to acknowledge
5. You can now draw

This dance exists because the compositor may need to negotiate the window size with the window manager (KWin in our case). You can't just pick any size — the compositor has the final say.

### The xdg_wm_base Ping

`xdg_wm_base` has a ping/pong mechanism. The compositor pings you periodically to check if you're still alive. You must respond with `xdg_wm_base_pong(hm_base, serial)`. If you don't, the compositor assumes you crashed and may close your window.

### Complete Window Example

```c
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor = NULL;
static struct xdg_wm_base *wm_base = NULL;

/* ── Registry ──────────────────────────────────────────────────── */

static void on_global(void *data, struct wl_registry *registry,
                      uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, "wl_compositor") == 0)
        compositor = wl_registry_bind(registry, name,
                                      &wl_compositor_interface, 4);
    else if (strcmp(interface, "xdg_wm_base") == 0)
        wm_base = wl_registry_bind(registry, name,
                                   &xdg_wm_base_interface, 1);
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = NULL,
};

/* ── xdg_wm_base ───────────────────────────────────────────────── */

static void on_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = on_ping,
};

/* ── xdg_toplevel ───────────────────────────────────────────────── */

static int running = 1;

static void on_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                  int32_t w, int32_t h,
                                  struct wl_array *states) {
    printf("Configured: %dx%d\n", w, h);
}

static void on_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    printf("Window closed by compositor\n");
    running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = on_toplevel_configure,
    .close = on_toplevel_close,
    .configure_bounds = NULL,
    .wm_capabilities = NULL,
};

/* ── xdg_surface ────────────────────────────────────────────────── */

static void on_surface_configure(void *data, struct xdg_surface *surface,
                                 uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    printf("Configure acknowledged (serial %u)\n", serial);
}

static const struct xdg_surface_listener surface_listener = {
    .configure = on_surface_configure,
};

/* ── Main ───────────────────────────────────────────────────────── */

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !wm_base) return 1;

    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xdg_surface =
        xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);

    struct xdg_toplevel *toplevel =
        xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "Tutorial Window");

    /* Commit the surface to trigger the configure exchange */
    wl_surface_commit(surface);
    wl_display_flush(display);

    printf("Window created, running event loop...\n");
    while (running && wl_display_dispatch(display) != -1)
        ;

    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surface);
    wl_surface_destroy(surface);
    xdg_wm_base_destroy(wm_base);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
```

**The xdg-shell protocol files** come from `wayland-protocols`. You generate them with `wayland-scanner`:

```sh
wayland-scanner client-header \
    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    xdg-shell-client-protocol.h

wayland-scanner private-code \
    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    xdg-shell-protocol.c
```

Then compile:

```meson
project('tutorial', 'c', default_options : ['warning_level=2', 'c_std=c11'])

wayland_client = dependency('wayland-client')

executable('04-window', '04-window.c', 'xdg-shell-protocol.c',
    dependencies: [wayland_client], install: false)
```

### What We Learned

- xdg-shell adds window semantics to a raw surface
- The configure/ack dance is cooperative: the compositor suggests, you must ack
- xdg_wm_base ping/pong keeps the connection alive
- `wl_display_dispatch` flushes outgoing, then blocks waiting for incoming events

### AI Mistake

In the first version of `libwl`, I called `wl_surface_commit` but **didn't flush**. The commit sat in the send buffer. Without `wl_display_flush` or `wl_display_dispatch`, no data goes to the compositor. The window never appeared.

The fix: either call `wl_display_flush()` after commit, or rely on `wl_display_dispatch()` (which flushes before reading). In a plain dispatch loop like above, `wl_display_dispatch` handles the flush. In the sd_event version, we need explicit flushes because the dispatch only fires when epoll says data is available — and no data arrives because the commit was never sent.

### Reference

- [wayland.app/protocols/xdg-shell](https://wayland.app/protocols/xdg-shell) — interactive docs for every request, event, and enum
- `/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml` — the spec itself
- [wayland-book: xdg-shell basics](https://wayland-book.com/xdg-shell-basics.html)

---

## Chapter 5: wl_shm — Pixels from the CPU

### The Mental Model

Before we involve the GPU, let's put pixels on screen the simplest possible way: **shared memory**.

`wl_shm` is a Wayland protocol that lets you:
1. Create a chunk of shared memory (a `memfd` — a memory-backed file descriptor)
2. Register it with the compositor as a `wl_shm_pool`
3. Allocate a `wl_buffer` from the pool
4. Write pixel data directly into the memory
5. Attach the buffer to your surface and commit

The compositor reads the pixel data from the shared memory and composites it onto the screen. No GPU involved — pure CPU pixels.

### Alternatives — Why Not Always Use SHM?

SHM works well for static content (a text editor, a terminal emulator) and for software-rendered content. But:
- **Every pixel written by the CPU.** For games, video, or anything complex, the CPU is orders of magnitude slower than the GPU.
- **Buffer copying.** The compositor may need to copy the SHM buffer to GPU memory for display, wasting bandwidth.
- **No hardware acceleration.** No shaders, no transforms, no effects.

Despite these limitations, SHM is the perfect teaching tool: it shows you the buffer lifecycle with zero GPU complexity.

### Creating a memfd

```c
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

static int create_shm_fd(size_t size) {
    int fd = memfd_create("wl-shm", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}
```

`memfd_create` creates an anonymous file in memory (Linux 3.17+). It's like `shm_open` but doesn't need a name. `MFD_CLOEXEC` closes the fd on `exec()` — a standard security practice. `ftruncate` sets the size of the memory region.

### The SHM Pool

```c
struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
close(fd);  /* The pool keeps its own reference */
```

`wl_shm_create_pool` tells the compositor "I have this shared memory fd." The compositor mmaps it on its end. You also mmap it on your end to write pixels. After creating the pool, you can close your copy of the fd — both sides have their own references.

### Allocating a wl_buffer

```c
struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
    width * 4, WL_SHM_FORMAT_XRGB8888);
```

The arguments:
- `offset` = 0 (start of the pool, since we have one buffer per pool)
- `width`, `height` = the buffer dimensions
- `stride` = bytes per row. For XRGB8888 (4 bytes per pixel), it's `width * 4`.
- `format` = `WL_SHM_FORMAT_XRGB8888` — 8 bits each for blue, green, red, and an unused byte. Same as `GBM_FORMAT_XRGB8888` we'll use later.

### Writing Pixels

```c
uint32_t *pixels = data;  /* data is the mmap'd pointer */
for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
        pixels[y * width + x] = 0xFF0000FF;  /* blue, red, green, unused */
```

The pixel format is 0xRRGGBBAA in memory (little-endian). `0xFF0000FF` = blue at full intensity.

### Full SHM Example

Create `05-shm.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define WIDTH  256
#define HEIGHT 256

static struct wl_compositor *compositor = NULL;
static struct xdg_wm_base *wm_base = NULL;
static struct wl_shm *shm = NULL;
static int running = 1;

/* ── Registry ──────────────────────────────────────────────────── */

static void on_global(void *data, struct wl_registry *registry,
                      uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, "wl_compositor") == 0)
        compositor = wl_registry_bind(registry, name,
                                      &wl_compositor_interface, 4);
    else if (strcmp(interface, "xdg_wm_base") == 0)
        wm_base = wl_registry_bind(registry, name,
                                   &xdg_wm_base_interface, 1);
    else if (strcmp(interface, "wl_shm") == 0)
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
}

/* ── Listeners ─────────────────────────────────────────────────── */

static void on_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

static void on_toplevel_close(void *data, struct xdg_toplevel *t,
                              void *states) {
    (void)t; (void)states;
    running = 0;
}

static void on_toplevel_configure(void *data, struct xdg_toplevel *t,
                                  int32_t w, int32_t h,
                                  struct wl_array *states) {
    (void)t; (void)states;
    printf("Configured: %dx%d\n", w, h);
}

static void on_surface_configure(void *data, struct xdg_surface *s,
                                 uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
}

/* ── Create wl_buffer from SHM ──────────────────────────────────── */

static int create_shm_fd(size_t size) {
    int fd = memfd_create("wl-shm", MFD_CLOEXEC);
    if (fd < 0) return -1;
    ftruncate(fd, (off_t)size);
    return fd;
}

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm,
                                           int width, int height,
                                           void **data_out) {
    int stride = width * 4;
    size_t size = (size_t)stride * height;

    int fd = create_shm_fd(size);
    if (fd < 0) return NULL;

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    *data_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool,
        0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    return buf;
}

/* ── Fill with rainbow ─────────────────────────────────────────── */

static void fill_rainbow(void *data, int width, int height, float t) {
    uint32_t *pixels = data;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float r = 0.5f + 0.5f * __builtin_sinf(t + (float)x / width * 6.28f);
            float g = 0.5f + 0.5f * __builtin_sinf(t + 2.094f + (float)y / height * 6.28f);
            float b = 0.5f + 0.5f * __builtin_sinf(t + 4.188f);
            uint8_t ri = (uint8_t)(r * 255);
            uint8_t gi = (uint8_t)(g * 255);
            uint8_t bi = (uint8_t)(b * 255);
            pixels[y * width + x] = (ri << 16) | (gi << 8) | bi;
        }
    }
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry,
        &(struct wl_registry_listener){.global = on_global,
                                        .global_remove = NULL}, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !wm_base || !shm) return 1;

    xdg_wm_base_add_listener(wm_base,
        &(struct xdg_wm_base_listener){.ping = on_ping}, NULL);

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xdg_surface =
        xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface,
        &(struct xdg_surface_listener){.configure = on_surface_configure}, NULL);

    struct xdg_toplevel *toplevel =
        xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel,
        &(struct xdg_toplevel_listener){
            .configure = on_toplevel_configure,
            .close = on_toplevel_close}, NULL);
    xdg_toplevel_set_title(toplevel, "SHM Rainbow");

    wl_surface_commit(surface);
    wl_display_flush(display);

    /* Wait for configure to arrive */
    wl_display_roundtrip(display);

    /* Create one SHM buffer */
    void *pixels = NULL;
    struct wl_buffer *buf = create_shm_buffer(shm, WIDTH, HEIGHT, &pixels);
    if (!buf) return 1;

    fill_rainbow(pixels, WIDTH, HEIGHT, 0);

    /* Attach, damage, commit */
    wl_surface_attach(surface, buf, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(surface);
    wl_display_flush(display);

    printf("Window has a rainbow. Close it to exit.\n");
    while (running && wl_display_dispatch(display) != -1)
        ;

    wl_buffer_destroy(buf);
    munmap(pixels, WIDTH * HEIGHT * 4);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surface);
    wl_surface_destroy(surface);
    xdg_wm_base_destroy(wm_base);
    wl_compositor_destroy(compositor);
    wl_shm_destroy(shm);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
```

**Key points:**
- We bind `wl_shm` from the registry — this is a core Wayland protocol, always present
- The SHM pool allocates memory; the buffer references a region within the pool
- We write pixels via the mmap'd pointer, attach, commit — the compositor displays them
- `wl_surface_damage_buffer` tells the compositor which region changed (here, the whole buffer)

### What We Learned

- wl_shm is the simplest Wayland buffer sharing mechanism — CPU memory, no GPU
- A `wl_shm_pool` wraps a memfd; a `wl_buffer` is a slice of the pool
- Pixels are written to mmap'd memory; the compositor reads from the same memory
- `WL_SHM_FORMAT_XRGB8888` = 4 bytes per pixel, BGR order in memory

### AI Mistake

In the first version of the SHM code, I tried to reuse the pool for multiple buffers (allocation at offset 0, offset 1, etc.). This works but requires tracking offsets. For learning, one buffer per pool is simpler. In production with triple buffering, you'd use a single pool with three offsets, or three pools.

### Reference

- [wayland.app/protocols/wl_shm](https://wayland.app/protocols/wl_shm) — the protocol spec
- `man memfd_create` — Linux memfd documentation
- [wayland-book: Shared Memory](https://wayland-book.com/surfaces/shared-memory.html)

---

## Chapter 6: Frame Callbacks — The Animated Rainbow

### The Problem with Static Windows

Our SHM window shows a rainbow, but it's frozen. To animate it, we need to:
1. Update the pixel data
2. Tell the compositor the buffer changed
3. Repeat at 60fps

We *could* just write pixels in a tight loop and commit every frame. But:
- The compositor might be in the middle of reading the buffer when we overwrite it (tearing)
- We'd render many more frames than the monitor can display (wasted CPU)
- Wayland frame pacing ensures smooth animation

### The Frame Callback

`wl_surface_frame(surface)` returns a `wl_callback` object. When the compositor is ready for the next frame, it fires the callback's `done` event. You then draw the next frame, commit, and request another frame callback.

```
Frame callback done → you draw → you commit → compositor displays
                                                   ↓
                                          Frame callback done → you draw → ...
```

This is a **pull model**: the compositor tells you when to draw. You never draw without being asked.

### The Release Listener

Remember `wl_buffer.release`? When the compositor is done displaying a buffer, it sends the release event. For SHM buffers, this means "I've copied the pixels to my display buffer, you can reuse the shared memory." For dmabuf buffers (later), it means "I'm done scanning out this buffer."

Without the release listener, you don't know when it's safe to reuse or destroy a buffer.

### Animated SHM with Frame Callbacks

Create `06-animated.c`. The key additions to the previous chapter's code:

```c
static void on_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    int *busy = data;
    *busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = on_buffer_release,
};
```

And the frame callback:

```c
static void on_frame_done(void *data, struct wl_callback *callback,
                          uint32_t time) {
    struct wl_callback **cb_ptr = data;
    /* Destroy the old callback */
    if (callback) wl_callback_destroy(callback);
    *cb_ptr = NULL;

    /* We'll be called from the dispatch loop; set a flag to draw */
    should_draw = 1;
}
```

And the loop now handles the dispatch and drawing:

```c
static int should_draw = 0;
static int buffer_busy = 0;

/* ... create a second buffer for double-buffering ... */

/* Initial frame callback request */
struct wl_callback *frame_cb = wl_surface_frame(surface);
wl_callback_add_listener(frame_cb,
    &(struct wl_callback_listener){.done = on_frame_done}, &frame_cb);

while (running && wl_display_dispatch(display) != -1) {
    if (should_draw) {
        should_draw = 0;

        /* Find a non-busy buffer */
        int idx = (buffer_busy == 0) ? 0 : 1;
        if (buffer_busy) continue;  /* skip frame, both busy */

        struct wl_buffer *buf = buffers[idx];
        void *pixels = pixel_data[idx];

        t += 0.05f;
        fill_rainbow(pixels, WIDTH, HEIGHT, t);

        wl_surface_attach(surface, buf, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
        wl_surface_commit(surface);
        wl_display_flush(display);
        buffer_busy = 1;

        /* Request next frame callback */
        frame_cb = wl_surface_frame(surface);
        wl_callback_add_listener(frame_cb,
            &(struct wl_callback_listener){.done = on_frame_done}, &frame_cb);
    }
}
```

For a complete working program, we need **two** SHM buffers so we can write to one while the compositor displays the other (double-buffering). The full code is in the repository at `chapters/06-animated.c`, but the pattern is this:

```
1. Request frame callback
2. Wait for frame callback done
3. Draw into a non-busy buffer
4. Attach, damage, commit, flush
5. Request next frame callback
6. Go to step 2
```

### What We Learned

- Frame callbacks are the compositor's way of saying "I'm ready, draw the next frame"
- Without frame callbacks, you tear or waste CPU
- wl_buffer.release tells you when to reuse a buffer
- Double-buffering (2 buffers) lets you draw while the compositor displays

### Alternatives to Frame Callbacks

- **Mailbox** (Vulkan's `VK_PRESENT_MODE_MAILBOX_KHR`): always keep one buffer ready. The compositor takes the latest. Higher latency, no tearing.
- **Immediate** (`VK_PRESENT_MODE_IMMEDIATE_KHR`): send frames whenever you want. Tearing if you write while the compositor reads.
- **FIFO** (`VK_PRESENT_MODE_FIFO_KHR`): queue frames, the compositor takes them one per vblank. Same as frame callbacks but driven by Vulkan.

Frame callbacks are Wayland's native vsync mechanism. They're the most reliable for tear-free, vsync-aligned rendering.

### AI Mistake

In the libwl implementation, I reused the same `frame_callback` pointer without checking if the previous callback was already destroyed. The frame callback fires once and must be destroyed. I destroyed it in `on_frame_done` and re-created it in `libwl_present`. But there was a race: if two frame done events arrived before the user called `libwl_present`, the second callback's pointer would be stale. The fix was setting the pointer to NULL after destruction and checking it in `on_frame_done`.

### Reference

- [wayland.app/protocols/wl_callback](https://wayland.app/protocols/wl_callback)
- [wayland-book: Frame callbacks](https://wayland-book.com/surfaces/frame-callbacks.html)

---

## Chapter 7: EGL + GLES — GPU Rendering Without the Pain

### Why EGL Before Vulkan

Vulkan is explicit and powerful. It's also verbose — creating a VkImage backed by a dmabuf fd takes ~60 lines of setup. Before we dive into that, let's see the "easy way" to get GPU pixels on screen: **EGL + OpenGL ES**.

EGL is the interface between a windowing system (Wayland) and a rendering API (OpenGL ES). It handles:
- Creating a rendering surface from a `wl_surface` (via `wl_egl_window`)
- Managing the swapchain internally
- Presenting frames with `eglSwapBuffers`

The mental model:
- **EGLDisplay** = connection to the GPU
- **EGLConfig** = pixel format/attributes (you ask for one that matches your needs)
- **EGLSurface** = the drawing surface (wraps the `wl_surface`)
- **EGLContext** = the rendering state (shaders, textures, etc.)
- **eglSwapBuffers** = present the current back buffer, make the next back buffer current

### Why OpenGL ES (glesv2)

OpenGL ES 2.0 is the simplest GPU API that can clear a surface with a color. It's ~10 lines of setup after EGL is ready. We use GLESv2 (not full OpenGL) because it's more portable and sufficient for "fill with color."

### The wl_egl_window

Wayland doesn't know about EGL directly. The bridge is `wl_egl_window`:

```c
#include <wayland-egl.h>

struct wl_egl_window *egl_window = wl_egl_window_create(surface, width, height);
EGLSurface egl_surface = eglCreateWindowSurface(display, config, egl_window, NULL);
```

`wl_egl_window_create` wraps a `wl_surface` and tells EGL "this is a window of WxH pixels." EGL internally allocates buffers (GBM bos), creates `wl_buffer` objects via the linux-dmabuf protocol, and manages the swapchain. You don't see any of it.

### Complete EGL + GLES Program

The xdg-shell and registry setup is the same as before. Here's the EGL-specific part:

```c
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <wayland-egl.h>

/* After display connection and surface creation */

/* Initialize EGL */
EGLDisplay egl_display = eglGetDisplay((EGLNativeDisplayType)display);
eglInitialize(egl_display, NULL, NULL);

/* Choose config */
EGLConfig egl_config;
EGLint attrs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE,
};
EGLint count;
eglChooseConfig(egl_display, attrs, &egl_config, 1, &count);

/* Create context */
EGLContext egl_context = eglCreateContext(egl_display, egl_config,
                                          EGL_NO_CONTEXT, NULL);

/* Create window surface */
struct wl_egl_window *egl_window = wl_egl_window_create(surface, 800, 600);
EGLSurface egl_surface = eglCreateWindowSurface(egl_display, egl_config,
                                                egl_window, NULL);

eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
```

Then in the frame callback:

```c
float t = 0;

static void on_frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    /* ... destroy old callback ... */

    t += 0.02f;
    float r = 0.5f + 0.5f * __builtin_sinf(t);
    float g = 0.5f + 0.5f * __builtin_sinf(t + 2.094f);
    float b = 0.5f + 0.5f * __builtin_sinf(t + 4.188f);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(egl_display, egl_surface);

    /* Request next frame callback */
    struct wl_callback *frame_cb = wl_surface_frame(surface);
    wl_callback_add_listener(frame_cb, &frame_listener, &frame_cb);
    wl_surface_commit(surface);
    wl_display_flush(display);
}
```

Notice: `eglSwapBuffers` replaces the manual attach/damage/commit cycle. EGL handles presenting the buffer internally.

### What EGL Does for You

When you call `eglSwapBuffers`, EGL:
1. Gets the next available GBM buffer from its internal pool
2. Imports it as an EGL image
3. Renders the current GLES commands into it
4. Creates a `wl_buffer` via the linux-dmabuf protocol (internally)
5. Attaches it to the `wl_surface`, damages, and commits
6. Waits for the buffer to be released before reusing it

This is exactly what we'll do manually with Vulkan. EGL just automates it.

### Compiling with EGL

```meson
project('tutorial', 'c', default_options : ['warning_level=2', 'c_std=c11'])

wayland_client = dependency('wayland-client')
egl = dependency('egl')
glesv2 = dependency('glesv2')

executable('07-egl', '07-egl.c', 'xdg-shell-protocol.c',
    dependencies: [wayland_client, egl, glesv2], install: false)
```

### What We Learned

- EGL abstracts the window system <-> rendering API bridge
- `wl_egl_window` wraps a `wl_surface` for EGL
- `eglSwapBuffers` automates the entire present cycle
- EGL hides GBM, dmabuf, and buffer management — you see none of it
- GLES `glClear` is the simplest GPU operation

### Why This Matters for Vulkan

EGL is "the easy button." Now that you understand it, we'll tear it apart and rebuild it manually with Vulkan. Every EGL concept has a Vulkan equivalent:
- `eglCreateWindowSurface` → `VkImage` + import dmabuf
- `eglSwapBuffers` → `vkQueueSubmit` + `wl_surface_commit`
- EGL context → VkDevice/VkQueue
- EGL config → VkPhysicalDevice selection

### AI Mistake

The first EGL code used `EGLNativeWindowType` cast of the `wl_surface` pointer directly, which works on some platforms but not others. The correct Wayland EGL path is always via `wl_egl_window_create`. The `EGLNativeWindowType` cast is a Mesa extension, not standard EGL.

### Reference

- [EGL 1.5 specification](https://www.khronos.org/registry/EGL/specs/eglspec.1.5.pdf)
- [Mesa EGL documentation](https://docs.mesa3d.org/egl.html)
- `man eglIntro` — if you have the EGL man pages installed
- [wayland-egl.h](https://gitlab.freedesktop.org/wayland/wayland/-/blob/main/egl/wayland-egl.h) — the wl_egl_window API

---

## Chapter 8: GBM + DRM — What EGL Hides

### The Mental Model

EGL creates windows and swaps buffers. But where do the buffers come from?

On Linux, GPU memory is managed by the **Direct Rendering Manager (DRM)** kernel subsystem. DRM deals with:
- **CRTCs** — scan-out engines that read pixels and send them to displays
- **Planes** — hardware layers that can be composited (like Photoshop layers)
- **Framebuffers** — a region of memory containing pixel data
- **GEM handles** — references to GPU buffers

GBM (Generic Buffer Manager) is a userspace library that abstracts DRM buffer allocation. You say "I want a buffer of W×H with format XRGB8888" and GBM gives you a `gbm_bo` (buffer object). Under the hood, GBM calls DRM `IOCTL`s to allocate GEM handles and prepares the buffer for scan-out.

When EGL creates a window surface, it:
1. Uses GBM to allocate a pool of buffers
2. Wraps them in EGL images
3. Presents them via the linux-dmabuf protocol (next chapter)

Now we'll do step 1 manually.

### Opening the Render Node

```c
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <xf86drm.h>

const char *drm_node = getenv("DRM_RENDER_NODE");
if (!drm_node) drm_node = "/dev/dri/renderD128";

int drm_fd = open(drm_node, O_RDWR);
if (drm_fd < 0) { /* error */ }

struct gbm_device *gbm = gbm_create_device(drm_fd);
if (!gbm) { /* error */ }
```

**Render nodes** (`/dev/dri/renderD128`, `renderD129`, etc.) are DRM devices designed for GPU compute and rendering without display control. They don't have modesetting capabilities (no CRTCs, no planes), which makes them safer for non-display-server applications.

Compare to **card nodes** (`/dev/dri/card0`): these have full DRM capabilities including modesetting. The compositor (KWin) uses card0 directly. Your app uses a render node.

The `DRM_RENDER_NODE` environment variable lets users override the render node. On Arch with an RTX 3050, the render node is typically `renderD128`.

### Allocating a Buffer

```c
uint32_t format = GBM_FORMAT_XRGB8888;

struct gbm_bo *bo = gbm_bo_create(gbm, width, height, format,
                                   GBM_BO_USE_RENDERING);
if (!bo) { /* error */ }
```

`GBM_BO_USE_RENDERING` tells GBM "I want to render to this buffer with OpenGL/Vulkan." Other flags include `GBM_BO_USE_SCANOUT` (for direct display scan-out), `GBM_BO_USE_CURSOR` (for cursor planes), etc.

### Getting Metadata

```c
uint32_t stride = gbm_bo_get_stride_for_plane(bo, 0);
uint32_t offset = gbm_bo_get_offset(bo, 0);
uint64_t modifier = gbm_bo_get_modifier(bo);
union gbm_bo_handle handle = gbm_bo_get_handle(bo);
```

- **Stride**: bytes per row (may be larger than `width * 4` due to alignment)
- **Offset**: bytes from the start of the buffer to the first pixel of plane 0
- **Modifier**: the tiling/compression format of the buffer. `DRM_FORMAT_MOD_INVALID` means linear.
- **Handle**: the GEM handle, needed to export a dmabuf fd

### Exporting a dmabuf fd

```c
int dmabuf_fd;
int ret = drmPrimeHandleToFD(drm_fd, handle.u32, 0, &dmabuf_fd);
if (ret != 0) { /* error */ }
```

`drmPrimeHandleToFD` converts a GEM handle into a **file descriptor** that can be sent over Unix sockets (used by Wayland) and imported into other GPU APIs (used by Vulkan).

The "Prime" in the name refers to **DRM Prime** — the infrastructure for sharing buffers between different device drivers. For example, an NVIDIA GPU and an Intel integrated GPU can share buffers via dmabuf.

### Cleaning Up

```c
close(dmabuf_fd);
gbm_bo_destroy(bo);
gbm_device_destroy(gbm);
close(drm_fd);
```

### Full GBM Example

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

int main(void) {
    const char *node = getenv("DRM_RENDER_NODE");
    if (!node) node = "/dev/dri/renderD128";

    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }

    struct gbm_bo *bo = gbm_bo_create(gbm, 256, 256, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_RENDERING);
    if (!bo) { fprintf(stderr, "gbm_bo_create failed\n"); return 1; }

    uint64_t modifier = gbm_bo_get_modifier(bo);
    uint32_t stride = gbm_bo_get_stride_for_plane(bo, 0);
    uint32_t offset = gbm_bo_get_offset(bo, 0);

    union gbm_bo_handle handle = gbm_bo_get_handle(bo);
    int dmabuf_fd;
    int ret = drmPrimeHandleToFD(fd, handle.u32, 0, &dmabuf_fd);
    if (ret != 0) { fprintf(stderr, "drmPrimeHandleToFD failed\n"); return 1; }

    printf("GBM buffer: %dx%d, modifier=0x%lx, stride=%d, offset=%d, fd=%d\n",
           256, 256, modifier, stride, offset, dmabuf_fd);

    close(dmabuf_fd);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    close(fd);
    return 0;
}
```

Compile:

```meson
gbm = dependency('gbm')
libdrm = dependency('libdrm')
executable('08-gbm', '08-gbm.c', dependencies: [gbm, libdrm], install: false)
```

### What We Learned

- GBM allocates GPU buffers via the DRM kernel driver
- A `gbm_bo` contains a GEM handle pointing to GPU memory
- `drmPrimeHandleToFD` exports the GEM handle as a dmabuf fd
- The dmabuf fd is the universal currency for sharing GPU buffers between processes and APIs
- Modifiers describe the tiling/compression layout of the buffer

### Alternatives to GBM

- **Dumb buffers:** Allocated via DRM directly (`drmModeCreateDumbBuffer`). CPU-accessible, slow, for software rendering only.
- **EGL:** Actually uses GBM internally (on Mesa). `eglCreateWindowSurface` calls GBM for you.
- **Vulkan WSI:** `VK_KHR_swapchain` creates swapchain images internally. You don't control allocation.

GBM gives us direct control over buffer allocation without Vulkan's overhead.

### AI Mistake

In the libwl code, I called `close(dmabuf_fd)` after `create_immed` but before the Vulkan import had a chance to dup it. The order matters:
1. Wait for Vulkan to dup the dmabuf fd
2. Wait for create_immed (which internally dups for the compositor)
3. Then close the original

The fix was: import into Vulkan first (with a dup), then create the wl_buffer (which also dups), then close the original.

### Reference

- [kernel.org: DRM overview](https://www.kernel.org/doc/html/latest/gpu/drm-overview.html)
- [kernel.org: DRM memory management](https://www.kernel.org/doc/html/latest/gpu/drm-mm.html)
- `man gbm_create_device` — GBM library documentation
- `man drmPrimeHandleToFD` — libdrm documentation

---

## Chapter 9: linux-dmabuf Protocol — Shipping the Buffer

### The Mental Model

We have a dmabuf fd pointing to GPU memory. The compositor needs access to that same memory to display it. The `zwp_linux_dmabuf_v1` Wayland protocol is how you hand the fd to the compositor.

Think of it as passing a file descriptor over a Unix socket. The compositor receives the fd, imports it into its GPU driver, creates a framebuffer from it, and scans it out.

### Binding the Protocol

```c
static struct zwp_linux_dmabuf_v1 *dmabuf = NULL;

/* In the registry listener */
if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
    if (version < 3) return;  /* need at least version 3 for create_immed */
    dmabuf = wl_registry_bind(registry, name,
                               &zwp_linux_dmabuf_v1_interface, 3);
}
```

Version 3 introduced `create_immed`, which creates the buffer immediately without a roundtrip. We'll use version 4 if available (it adds format/modifier enumeration), but 3 is sufficient.

### Creating a wl_buffer from a dmabuf

```c
struct zwp_linux_buffer_params_v1 *params;
params = zwp_linux_dmabuf_v1_create_params(dmabuf);

zwp_linux_buffer_params_v1_add(params,
    dmabuf_fd,       /* the dmabuf file descriptor */
    0,               /* plane index (0 for single-planar formats) */
    offset,          /* offset in bytes to plane 0 */
    stride,          /* bytes per row */
    modifier >> 32,  /* modifier hi 32 bits */
    modifier & 0xffffffff);  /* modifier lo 32 bits */

struct wl_buffer *wl_buf = zwp_linux_buffer_params_v1_create_immed(params,
    width, height, GBM_FORMAT_XRGB8888, 0);

zwp_linux_buffer_params_v1_destroy(params);
```

**The modifier is split into two 32-bit halves.** This is a quirk of the protocol wire format.

**`create_immed` vs `create`:** The protocol also has `create()` which returns the buffer asynchronously — the compositor sends a `created` event when it's ready. `create_immed` skips this confirmation and creates the buffer immediately. Use `create_immed` for simplicity; use `create` if you need to handle format/modifier negotiation.

### The Buffer Release Listener

Same as SHM:

```c
static void on_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    int *busy = data;
    *busy = 0;
}
```

### What About the fd Ownership?

When you pass the dmabuf fd to `zwp_linux_buffer_params_v1_add`, libwayland internally dups the fd before sending it over the Unix socket. You can close your copy after `create_immed` — or keep it for Vulkan import.

The rule: **each consumer dups the fd, you close your original.**

### Full Example (Integrating with GBM)

```c
/* After creating the GBM bo and exporting the dmabuf fd */

/* Create wl_buffer from dmabuf */
struct zwp_linux_buffer_params_v1 *params;
params = zwp_linux_dmabuf_v1_create_params(dmabuf);
zwp_linux_buffer_params_v1_add(params, dmabuf_fd, 0, offset, stride,
                                modifier >> 32, modifier & 0xffffffff);
struct wl_buffer *wl_buf = zwp_linux_buffer_params_v1_create_immed(params,
    width, height, GBM_FORMAT_XRGB8888, 0);
zwp_linux_buffer_params_v1_destroy(params);
if (!wl_buf) { /* error */ }

/* The compositor has a dup of the fd — close ours.
   We'll keep the fd for Vulkan import (chapter 10). */

wl_buffer_add_listener(wl_buf, &buffer_listener, &buffer_busy);
```

### What We Learned

- `zwp_linux_dmabuf_v1` sends dmabuf fds to the compositor
- `create_immed` creates the buffer in one roundtrip-free request
- The modifier is split into two u32 halves on the wire
- The compositor dups the fd internally; you close your copy

### Alternatives

- **wl_shm**: CPU memory, no GPU involved. Simpler but slower.
- **wl_drm** (Mesa-specific, deprecated): The old way to share DRM buffers. linux-dmabuf is the standard now.

### AI Mistake

I originally passed the modifier incorrectly, swapping hi and lo halves:
```c
/* WRONG */
zwp_linux_buffer_params_v1_add(params, fd, 0, offset, stride,
    modifier & 0xffffffff, modifier >> 32);
```

The protocol expects hi first, then lo. Swapping them causes the compositor to misinterpret the tiling format, resulting in corrupted pixels or a black screen.

### Reference

- [wayland.app/protocols/linux-dmabuf-unstable-v1](https://wayland.app/protocols/linux-dmabuf-unstable-v1)
- [Freedesktop: DMA-BUF protocol](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/tree/main/unstable/linux-dmabuf)

---

## Chapter 10: Vulkan — The Explicit API

### Why Vulkan?

OpenGL ES + EGL works perfectly for our rainbow demo. Why switch?

- **Control.** EGL manages the swapchain. You don't know when buffers are created, how they're allocated, or when they're recycled. Vulkan makes you own every step.
- **Zero-copy.** With EGL, `eglSwapBuffers` might internally copy the buffer. With Vulkan + dmabuf, you allocate the buffer in GBM, import it into Vulkan, and hand it to the compositor. The pixels never move.
- **Extension hooks.** Vulkan lets you add any extension (raytracing, mesh shaders, video encode/decode) without changing the windowing library. EGL can't do that.
- **Future.** Vulkan 1.4 makes dynamic rendering, descriptor indexing, and timeline semaphores core. These are the foundation of modern GPU programming.

Vulkan 1.4 was released in late 2024. Your RTX 3050 supports it fully. Arch Linux ships Vulkan 1.4 SDK.

### The Vulkan Object Hierarchy

```
VkInstance                    — connection to the Vulkan driver
  └── VkPhysicalDevice        — a GPU (e.g., RTX 3050)
        └── VkDevice           — logical interface to the GPU
              ├── VkQueue        — command stream (e.g., graphics queue)
              ├── VkCommandPool  — allocates command buffers
              │     └── VkCommandBuffer — recorded GPU commands
              ├── VkFence         — CPU-GPU synchronization
              ├── VkImage         — a GPU image (no memory attached!)
              └── VkDeviceMemory   — a chunk of GPU memory
```

Key insight: **VkImage and VkDeviceMemory are separate**. Creating an image doesn't allocate memory for it. You create the image, figure out how much memory it needs, allocate that memory, and bind them together. This is the "explicit" in Vulkan.

### Instance Creation

```c
#include <vulkan/vulkan.h>

VkInstance inst;
VkApplicationInfo app = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Tutorial",
    .apiVersion = VK_MAKE_VERSION(1, 0, 0),
};
const char *inst_exts[] = {
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
};
VkInstanceCreateInfo ici = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &app,
    .enabledExtensionCount = 2,
    .ppEnabledExtensionNames = inst_exts,
};
vkCreateInstance(&ici, NULL, &inst);
```

**Why API version 1.0?** Setting `apiVersion` to 1.0 tells the loader "I'm compatible with any 1.x version." If you set it to 1.4, older drivers would reject it. The loader gives you the latest available version regardless. You just can't use 1.4-only features without checking, which we don't need to worry about since we know we're on 1.4.

**The two instance extensions** are required for importing external memory (dmabuf). `VK_KHR_external_memory_capabilities` lets us query which memory types support external fd import. `VK_KHR_get_physical_device_properties_2` lets us use the pNext chain for extended queries.

### Physical Device Selection

```c
VkPhysicalDevice phys_dev;
uint32_t count;
vkEnumeratePhysicalDevices(inst, &count, NULL);
VkPhysicalDevice *devices = malloc(count * sizeof(VkPhysicalDevice));
vkEnumeratePhysicalDevices(inst, &count, devices);
phys_dev = devices[0];  /* pick the first GPU */
free(devices);
```

For production, you'd iterate devices, check properties, look for the discrete GPU. For learning, the first one is fine. On a laptop with an RTX 3050, `devices[0]` is likely the NVIDIA GPU.

### Queue Family

```c
uint32_t queue_family = UINT32_MAX;
vkGetPhysicalDeviceQueueFamilyProperties(phys_dev, &count, NULL);
VkQueueFamilyProperties *props = malloc(count * sizeof(*props));
vkGetPhysicalDeviceQueueFamilyProperties(phys_dev, &count, props);
for (uint32_t i = 0; i < count; i++) {
    if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        queue_family = i;
        break;
    }
}
free(props);
```

We need a queue that supports `VK_QUEUE_GRAPHICS_BIT` (compute, transfer, and presentation). The first such family works.

### Device Creation

```c
const char *dev_exts[] = {
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
};

float priority = 1.0f;
VkDeviceQueueCreateInfo qci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = queue_family,
    .queueCount = 1,
    .pQueuePriorities = &priority,
};
VkDeviceCreateInfo dci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &qci,
    .enabledExtensionCount = 8,
    .ppEnabledExtensionNames = dev_exts,
};
VkDevice dev;
vkCreateDevice(phys_dev, &dci, NULL, &dev);

VkQueue queue;
vkGetDeviceQueue(dev, queue_family, 0, &queue);
```

**The device extensions** are required for importing dmabuf fds:
- `VK_EXT_external_memory_dma_buf` — allows importing Linux dmabuf fds as Vulkan memory
- `VK_EXT_image_drm_format_modifier` — allows creating VkImages with DRM format modifiers
- `VK_KHR_external_memory` / `VK_KHR_external_memory_fd` — memory import from fd
- `VK_KHR_bind_memory_2` / `VK_KHR_get_memory_requirements_2` — extended binding/requirements
- `VK_KHR_dedicated_allocation` — better performance for dedicated resources (images)
- `VK_KHR_maintenance_1` — allows `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` etc.

### Command Pool and Command Buffer

```c
VkCommandPool cmd_pool;
VkCommandPoolCreateInfo cpi = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    .queueFamilyIndex = queue_family,
};
vkCreateCommandPool(dev, &cpi, NULL, &cmd_pool);

VkCommandBuffer cmd;
VkCommandBufferAllocateInfo ai = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = cmd_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
};
vkAllocateCommandBuffers(dev, &ai, &cmd);
```

### Loading Function Pointers

Some Vulkan functions are extensions and need to be loaded at runtime:

```c
PFN_vkGetImageMemoryRequirements2KHR vkGetImageMemoryRequirements2KHR =
    (PFN_vkGetImageMemoryRequirements2KHR)
    vkGetDeviceProcAddr(dev, "vkGetImageMemoryRequirements2KHR");
PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR =
    (PFN_vkGetMemoryFdPropertiesKHR)
    vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
PFN_vkBindImageMemory2KHR vkBindImageMemory2KHR =
    (PFN_vkBindImageMemory2KHR)
    vkGetDeviceProcAddr(dev, "vkBindImageMemory2KHR");
```

These three functions are critical for importing dmabuf fds. They're part of extensions, not core Vulkan, so we load them explicitly.

`vkGetDeviceProcAddr` returns a function pointer. The `PFN_*` types are defined in `vulkan.h`.

### Fence

```c
VkFence fence;
VkFenceCreateInfo fi = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
};
vkCreateFence(dev, &fi, NULL, &fence);
```

A fence signals when the GPU is done with submitted commands. The `SIGNALED_BIT` flag creates it in the signaled state, so the first `vkWaitForFences` returns immediately.

### What We Learned

- Vulkan separates allocation from everything: VkImage ≠ VkDeviceMemory
- Extension functions must be loaded via `vkGetDeviceProcAddr`
- Vulkan 1.4 baseline: 10 required device extensions for dmabuf import
- Fences are the CPU-GPU synchronization primitive

### AI Mistake

The list of device extensions I used includes `VK_KHR_MAINTENANCE_1`. In Vulkan 1.4, some of these might be promoted to core. I didn't verify which, because adding an extension the device already supports is harmless. But it's sloppy — production code should only request what's actually needed.

A real AI mistake: I originally forgot to load `vkBindImageMemory2KHR` via `vkGetDeviceProcAddr` and tried calling it directly. The linker couldn't find it because it's an extension function.

### Reference

- [Vulkan 1.4 specification](https://registry.khronos.org/vulkan/specs/1.4/pdf/vkspec.pdf)
- [Vulkan Tutorial (Alexander Overvoorde)](https://vulkan-tutorial.com/) — excellent introduction
- `man vkCreateInstance` — Vulkan man pages
- `VK_EXT_external_memory_dma_buf` spec (`/usr/share/vulkan/registry/specs/`)

---

## Chapter 11: Vulkan dmabuf Import — The Core Chain

### Why This Is the Hardest Part

This is the chapter where we combine GBM, Wayland, and Vulkan into one coherent pipeline. The mental model is:

```
GBM allocates buffer  →  dmabuf fd  →  Vulkan imports as VkImage
                                    →  Wayland sends to compositor
```

Both Vulkan and Wayland need access to the same GPU memory. The dmabuf fd is the bridge.

### The Import Chain

We have a GBM buffer object (`gbm_bo`). We've exported its dmabuf fd. Now we need to:

1. **Create a VkImage** with the same dimensions and format, marked as externally backed
2. **Query the fd's memory type bits** — which Vulkan memory types support this particular fd
3. **Query the VkImage's memory requirements** — size and required memory type bits
4. **Intersect** the two sets of bits to find a compatible memory type
5. **Import the fd** into a VkDeviceMemory allocation
6. **Bind** the VkImage to the allocated memory

### Step 1: Create the VkImage

```c
VkImageCreateInfo image_ci = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = VK_FORMAT_B8G8R8A8_UNORM,
    .extent = { (uint32_t)width, (uint32_t)height, 1 },
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
};

VkExternalMemoryImageCreateInfo ext_ci = {
    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
};
```

The `VkExternalMemoryImageCreateInfo` tells Vulkan "this image's memory will come from outside Vulkan." Without this, Vulkan would allocate its own private memory for the image.

For the tiling, there are two cases:

**Buffer has a modifier (tiled):**
```c
VkSubresourceLayout layout = { .offset = offset, .rowPitch = stride };
VkImageDrmFormatModifierExplicitCreateInfoEXT mod_ci = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
    .drmFormatModifier = modifier,
    .drmFormatModifierPlaneCount = 1,
    .pPlaneLayouts = &layout,
};
image_ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
image_ci.pNext = &mod_ci;
mod_ci.pNext = &ext_ci;
```

**Buffer has no modifier (linear):**
```c
image_ci.tiling = VK_IMAGE_TILING_LINEAR;
image_ci.pNext = &ext_ci;
```

The modifier tells Vulkan how the pixels are laid out in memory. NVIDIA GPUs often use block-linear tiling, which is more efficient for rendering.

### Step 2: Query the fd's memory type bits

```c
int fd = dup(dmabuf_fd);  /* dup before passing to Vulkan */

VkMemoryFdPropertiesKHR fd_props = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
};
vkGetMemoryFdPropertiesKHR(dev,
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    fd, &fd_props);
```

This tells us **which Vulkan memory types can import this particular fd.** Not all memory types support dmabuf import.

### Step 3: Query the VkImage's memory requirements

```c
VkImageMemoryRequirementsInfo2 req_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
    .image = image,
};
VkMemoryRequirements2 mem_reqs = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
};
vkGetImageMemoryRequirements2KHR(dev, &req_info, &mem_reqs);
```

This gives us the size and memory type bits required by the image itself.

### Step 4: Intersect memory type bits

```c
uint32_t mem_type_bits = fd_props.memoryTypeBits &
    mem_reqs.memoryRequirements.memoryTypeBits;
assert(mem_type_bits > 0);
int memory_type_index = __builtin_ffs(mem_type_bits) - 1;
```

The valid memory type is the intersection of what the fd supports and what the image needs. `__builtin_ffs` finds the first set bit (lowest memory type index).

### Step 5: Import the fd

```c
VkImportMemoryFdInfoKHR import_info = {
    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    .fd = fd,  /* Vulkan takes ownership of this dup'd fd */
};

VkMemoryAllocateInfo alloc_info = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .pNext = &import_info,
    .allocationSize = mem_reqs.memoryRequirements.size,
    .memoryTypeIndex = memory_type_index,
};

VkDeviceMemory memory;
vkAllocateMemory(dev, &alloc_info, NULL, &memory);
```

**Vulkan takes ownership of the fd.** When you call `vkAllocateMemory` with `VkImportMemoryFdInfoKHR`, Vulkan internally dups the fd (again). You don't need to close it after.

### Step 6: Bind

```c
VkBindImageMemoryInfo bind_info = {
    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
    .image = image,
    .memory = memory,
};
vkBindImageMemory2KHR(dev, 1, &bind_info);
```

Now the VkImage is backed by the GBM buffer's memory. Writing to the VkImage writes to the same pixels the compositor reads.

### The Complete Chain

```c
/* After gbm_bo_create + drmPrimeHandleToFD */

/* Create wl_buffer first (libwayland internally dups the fd) */
struct zwp_linux_buffer_params_v1 *params =
    zwp_linux_dmabuf_v1_create_params(dmabuf);
zwp_linux_buffer_params_v1_add(params, dmabuf_fd, 0, offset, stride,
                                modifier >> 32, modifier & 0xffffffff);
struct wl_buffer *wl_buf = zwp_linux_buffer_params_v1_create_immed(params,
    width, height, GBM_FORMAT_XRGB8888, 0);
zwp_linux_buffer_params_v1_destroy(params);

/* Create VkImage from the same buffer */
int vulkan_fd = dup(dmabuf_fd);

/* ... create VkImage, query fd props, query image requirements,
     intersect bits, allocate memory with import, bind ... */

/* Close our original fd */
close(dmabuf_fd);
```

**The fd lifetime:**
1. `drmPrimeHandleToFD` gives us the original fd
2. `zwp_linux_buffer_params_v1_add` internally dups for the compositor
3. We dup again for Vulkan import
4. We close the original
5. Vulkan internally dups again during `vkAllocateMemory`
6. We can close our Vulkan dup after `vkAllocateMemory` (or let it stay open — Vulkan has its own reference)

### What We Learned

- dmabuf fd is the bridge between GBM, Vulkan, and Wayland
- VkExternalMemoryImageCreateInfo marks an image as externally backed
- VkImportMemoryFdInfoKHR imports the fd into Vulkan memory
- Memory type must satisfy both the fd and the image
- Each consumer dups the fd; you close the original after all dups

### AI Mistake

I originally put the `close(dmabuf_fd)` before the Vulkan import. The Vulkan import happened on a dup'd fd, so it worked, but the Wayland `create_immed` also dups internally. The issue was conceptual: I closed the original before understanding where all the references went.

The correct mental model:
- Wayland dups internally? Yes, during `add()`.
- Vulkan dups internally? Yes, during `vkAllocateMemory` with `VkImportMemoryFdInfoKHR`.
- We dup before Vulkan? Yes, so we don't lose the reference.
- When do we close the original? After both dups are taken.

### Reference

- `VK_EXT_external_memory_dma_buf` spec section
- `VK_KHR_external_memory_fd` spec section
- `VK_EXT_image_drm_format_modifier` spec section
- [NVIDIA Vulkan developer guide: external memory](https://developer.nvidia.com/vulkan-shader-resource-association)

---

## Chapter 12: Triple-buffer Swapchain — The Production Pattern

### Why Three Buffers?

With one buffer, you can't draw while the compositor displays — you'd overwrite the pixels being scanned out (tearing).

With two buffers (double-buffering), you draw to one while the compositor displays the other. This works, but consider the timing:

```
Frame 1: you draw to A        → commit A → compositor displays A
Frame 2: you draw to B        → commit B → compositor displays B
Frame 3: you want to draw to A, but compositor still hasn't released A
         → you STALL, waiting for A to be released
```

With three buffers, there's always at least one buffer available:

```
Frame 1: you draw to A, commit A  → compositor displays A (busy)
Frame 2: you draw to B, commit B  → compositor queues B (busy)
Frame 3: A is released, draw to A → compositor now displaying B
Frame 4: B is released or C       → etc.
```

Three buffers prevent the stall. In Wayland, a buffer is "busy" from the moment you commit it until the compositor sends `wl_buffer.release`. In Vulkan, a buffer is "busy" from the moment you submit it until the fence signals. Both conditions must be clear before reuse.

### The Buffer Struct

```c
#define NUM_BUFFERS 3

typedef struct {
    struct wl_buffer  *wl_buf;
    struct gbm_bo     *bo;
    int                dmabuf_fd;
    uint32_t           stride;
    uint32_t           offset;
    uint64_t           modifier;
    int                busy;       /* compositor still has it? */
    VkImage            image;
    VkDeviceMemory     memory;
    VkCommandBuffer    cmd;
    VkFence            fence;
} Buffer;
```

The `busy` flag is set to 1 when we commit and cleared when `wl_buffer.release` fires. The fence is waited on before reuse.

### Buffer Lifecycle

```c
/* ── In the frame callback ── */

/* 1. Find a free buffer */
Buffer *buf = NULL;
for (int i = 0; i < NUM_BUFFERS; i++) {
    if (!buffers[i].busy) { buf = &buffers[i]; break; }
}
if (!buf) return;  /* all busy, skip this frame */

/* 2. Wait for GPU to finish with this buffer */
vkWaitForFences(dev, 1, &buf->fence, VK_TRUE, UINT64_MAX);
vkResetFences(dev, 1, &buf->fence);

/* 3. Begin recording */
VkCommandBufferBeginInfo begin = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
};
vkBeginCommandBuffer(buf->cmd, &begin);

/* 4. User records commands (clear, barrier, etc.) */
// (this is where the user writes their rendering code)

/* 5. Submit */
vkEndCommandBuffer(buf->cmd);
VkSubmitInfo submit = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
    .pCommandBuffers = &buf->cmd,
};
vkQueueSubmit(queue, 1, &submit, buf->fence);

/* 6. Present to Wayland */
wl_surface_attach(surface, buf->wl_buf, 0, 0);
wl_surface_damage_buffer(surface, 0, 0, width, height);
struct wl_callback *cb = wl_surface_frame(surface);
wl_callback_add_listener(cb, &frame_listener, ...);
wl_surface_commit(surface);
wl_display_flush(display);

/* 7. Mark as busy */
buf->busy = 1;
```

### Buffer Release Listener

```c
static void on_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    Buffer *buf = data;
    buf->busy = 0;
}
```

Attach the listener when creating the buffer:

```c
wl_buffer_add_listener(buf->wl_buf, &buffer_listener, buf);
```

### Creating All Buffers Upfront

All three buffers are created once in `create`. They have the same format, size, and modifier (which comes from the first `gbm_bo_create`).

```c
for (int i = 0; i < NUM_BUFFERS; i++) {
    buffers[i].bo = gbm_bo_create(gbm, width, height, format,
                                   GBM_BO_USE_RENDERING);
    // ... export dmabuf fd ...
    // ... create VkImage + allocate + bind ...
    // ... create wl_buffer via linux-dmabuf ...
    // ... create command buffer ...
    // ... create fence (VK_FENCE_CREATE_SIGNALED_BIT) ...
}
```

### Handling Resize

When the compositor sends a new size via `xdg_toplevel.configure`, we must destroy all buffers and recreate them at the new size:

```c
static void recreate_buffers(int width, int height) {
    vkDeviceWaitIdle(dev);  /* wait for all GPU work to finish */
    for (int i = 0; i < NUM_BUFFERS; i++)
        destroy_buffer(&buffers[i]);
    for (int i = 0; i < NUM_BUFFERS; i++)
        create_buffer(&buffers[i], width, height);
}
```

`vkDeviceWaitIdle` ensures no buffer is in GPU hands before we destroy its resources.

### The Frame Callback Flow (sd_event Edition)

```
sd_event_add_io (Wayland fd) → on_wl_event:
    wl_display_dispatch(display):
        → wl_callback.done → on_frame_done:
            find free buffer
            wait fence, reset fence
            begin command buffer
            call user's frame callback
                → user records Vulkan commands
                → user calls present()
                    end command buffer
                    sumbit to queue
                    attach + commit + flush
                    mark buffer busy
```

### What We Learned

- Triple-buffering prevents stalls when the compositor holds multiple buffers
- Each buffer tracks its own `busy` flag (compositor) and `fence` (GPU)
- Buffers are created upfront, destroyed + recreated on resize
- The present cycle: submit GPU work → attach buffer → commit → flush

### AI Mistake

In the libwl implementation, I didn't properly handle the case where `on_frame_done` is called but `libwl_present` hasn't been called yet by the user. The command buffer is left in the recording state. The fix would be to end the command buffer or discard it, but in practice the user calls `libwl_present` before returning from the frame callback.

A more serious mistake: I forgot to reset the fence with `vkResetFences` after waiting. Without reset, the next `vkWaitForFences` returns immediately (the fence is already signaled). This caused a race where the new frame's submit didn't actually wait for the previous frame's GPU work.

### Reference

- The complete swapchain code in `src/libwl.c` (lines 634–703)
- [Vulkan specification: Fences](https://registry.khronos.org/vulkan/specs/1.4/html/vkspec.html#fences)
- [Wayland: wl_callback](https://wayland.app/protocols/wl_surface#wl_callback)

---

## Chapter 13: Input — The User Interacts

### The Input Architecture

Wayland input is mediated entirely by the compositor. KWin decides which surface gets keyboard focus and which receives pointer events. Your app only receives input when it has focus.

This is fundamentally different from X11, where an app could listen to input events globally (keyloggers, anyone?). Wayland's security model means: the compositor controls focus, and you get events only for your own surface.

### The Seat

A `wl_seat` represents a physical setup with a user (chair), a pointer (mouse), and a keyboard. There can be multiple seats (multiple users), but typically there's one.

```c
/* In the registry listener */
if (strcmp(interface, "wl_seat") == 0) {
    seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
    wl_seat_add_listener(seat, &seat_listener, ...);
}
```

The seat tells you about its capabilities:

```c
static void on_seat_capabilities(void *data, struct wl_seat *seat,
                                  uint32_t capabilities) {
    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        /* Get the pointer object */
        struct wl_pointer *pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointer_listener, data);
    }
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, data);
    }
}
```

### Pointer Events

The pointer listener has many callbacks. We only need a few:

```c
static void on_pointer_enter(void *data, struct wl_pointer *pointer,
                              uint32_t serial, struct wl_surface *surface,
                              wl_fixed_t sx, wl_fixed_t sy) {
    /* Cursor entered our surface at (sx, sy) in surface-local coords */
    Libwl *lib = data;
    lib->pointer_entered = 1;
    lib->pointer_x = wl_fixed_to_double(sx);
    lib->pointer_y = wl_fixed_to_double(sy);
}

static void on_pointer_leave(void *data, struct wl_pointer *pointer,
                              uint32_t serial, struct wl_surface *surface) {
    Libwl *lib = data;
    lib->pointer_entered = 0;
}

static void on_pointer_motion(void *data, struct wl_pointer *pointer,
                               uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    Libwl *lib = data;
    lib->pointer_x = wl_fixed_to_double(sx);
    lib->pointer_y = wl_fixed_to_double(sy);
}
```

**Coordinates are `wl_fixed_t`** — a fixed-point number with 8 bits of fractional precision. `wl_fixed_to_double` converts to a regular `double`.

The other pointer events (button, axis, frame, etc.) are useful for click handling and scrolling but not essential for the basic tutorial.

### Keyboard Events

The keyboard listener:

```c
static void on_keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                                uint32_t format, int32_t fd, uint32_t size) {
    /* The compositor sent us an fd with the keymap. We don't need
     * to parse it for basic key tracking — close the fd. */
    close(fd);
}

static void on_keyboard_enter(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, struct wl_surface *surface,
                               struct wl_array *keys) {
    /* Keyboard focus entered our surface. keys contains currently
     * pressed keys (scancodes). */
    Libwl *lib = data;
    lib->keyboard_entered = 1;
    memset(lib->pressed_keys, 0, 256);
    uint32_t *k;
    for (k = keys->data; (const char*)k < (const char*)keys->data + keys->size; k++)
        lib->pressed_keys[*k] = 1;
}

static void on_keyboard_leave(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, struct wl_surface *surface) {
    Libwl *lib = data;
    lib->keyboard_entered = 0;
    memset(lib->pressed_keys, 0, 256);
}

static void on_keyboard_key(void *data, struct wl_keyboard *keyboard,
                             uint32_t serial, uint32_t time, uint32_t key,
                             uint32_t state) {
    Libwl *lib = data;
    lib->pressed_keys[key] = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
}
```

**The keymap fd:** The compositor sends the keymap (a compiled xkbcommon keymap) as a file descriptor. We must close the fd when we're done with it. For basic key tracking (which keys are pressed), we don't need to parse the keymap — scancodes are the raw hardware codes (typically evdev scancodes, same as `KEY_A` etc. in `linux/input-event-codes.h`).

**Key indices:** The `keys` array and the `key` argument in `on_keyboard_key` contain Linux scancodes (offset by 8). For example, Escape is 1, 'A' is 30. You can find the full list in `/usr/include/linux/input-event-codes.h`.

### State in the Lib Struct

```c
typedef struct {
    /* ... */
    struct wl_seat      *seat;
    struct wl_pointer   *pointer;
    struct wl_keyboard  *keyboard;
    int                  pointer_entered;
    double               pointer_x;
    double               pointer_y;
    int                  keyboard_entered;
    uint8_t              pressed_keys[256];
} Window;
```

The user's frame callback can check these fields to respond to input:

```c
static void frame(Window *win, void *user) {
    if (win->keyboard_entered && win->pressed_keys[30]) {
        /* 'A' is pressed — do something! */
    }
    if (win->pointer_entered) {
        /* Mouse is over our window at (win->pointer_x, win->pointer_y) */
    }
    /* ... render and present ... */
}
```

### What We Learned

- wl_seat represents the input setup; the compositor controls focus
- Pointer events use wl_fixed_t fixed-point coordinates
- Key events use hardware scancodes (not X11 keysyms)
- The keymap fd must be closed
- Input state is stored in the window struct, read by the user's frame callback

### Alternatives

- **libxkbcommon** — for converting scancodes to human-readable key names and handling keyboard layouts
- **wl_touch** — multi-touch input
- **libinput** — raw input device access (bypasses Wayland, useful for games)

### AI Mistake

In the keyboard listener, I forgot to close the keymap fd in `on_keyboard_keymap`. This leaks file descriptors. Every time the keymap changes (layout switch, keyboard replug), a new fd is sent. Without closing, the process eventually runs out of fds.

### Reference

- [wayland.app/protocols/wl_seat](https://wayland.app/protocols/wl_seat)
- [wayland.app/protocols/wl_pointer](https://wayland.app/protocols/wl_pointer)
- [wayland.app/protocols/wl_keyboard](https://wayland.app/protocols/wl_keyboard)
- `/usr/include/linux/input-event-codes.h` — scancode definitions
- [libxkbcommon documentation](https://xkbcommon.org/)

---

## Chapter 14: Put It All Together — You Write the Library

### The Assignment

You have all the pieces. Now write a C library that:
1. Creates a Wayland window with xdg-shell
2. Allocates 3 GBM buffers
3. Imports each into Vulkan as a VkImage
4. Creates wl_buffers via the linux-dmabuf protocol
5. Runs an sd_event loop dispatching Wayland events
6. Calls a user-provided frame callback for each frame
7. Presents the rendered buffer to the compositor
8. Handles resize and input
9. Exposes every handle in a transparent struct

### The API Signature

```c
typedef struct {
    const char    *title;
    int            width, height;
    const char   **extra_instance_exts;
    int            extra_instance_ext_count;
    const char   **extra_device_exts;
    int            extra_device_ext_count;
    void          *instance_pnext;
    void          *device_pnext;
} LibConfig;

typedef struct Lib {
    /* Wayland */
    struct wl_display           *wl;
    struct wl_compositor        *compositor;
    struct xdg_wm_base          *wm_base;
    struct zwp_linux_dmabuf_v1  *dmabuf;
    struct wl_surface           *surface;
    struct xdg_surface          *xdg_surface;
    struct xdg_toplevel         *xdg_toplevel;

    /* DRM / GBM */
    int                          drm_fd;
    struct gbm_device           *gbm;
    uint32_t                     format;

    /* Vulkan */
    VkInstance                   inst;
    VkPhysicalDevice             phys_dev;
    VkDevice                     dev;
    VkQueue                      queue;
    uint32_t                     queue_family;
    VkCommandPool                cmd_pool;
    PFN_vkGetImageMemoryRequirements2KHR vkGetImageMemoryRequirements2KHR;
    PFN_vkGetMemoryFdPropertiesKHR       vkGetMemoryFdPropertiesKHR;
    PFN_vkBindImageMemory2KHR            vkBindImageMemory2KHR;

    /* sd_event */
    struct sd_event             *event;

    /* Input */
    struct wl_seat              *seat;
    struct wl_pointer           *pointer;
    struct wl_keyboard          *keyboard;
    int                          pointer_entered;
    double                       pointer_x, pointer_y;
    int                          keyboard_entered;
    uint8_t                      pressed_keys[256];

    /* Window state */
    int                          width, height;
    int                          running;

    /* Buffer state (3 buffers) */
    /* ... you design this ... */

    /* User frame callback */
    void (*frame_cb)(struct Lib *lib, void *user);
    void                        *frame_user;
} Lib;

int  lib_create(Lib *lib, const LibConfig *cfg);
void lib_destroy(Lib *lib);
int  lib_run(Lib *lib, void (*frame)(struct Lib *, void *), void *user);
int  lib_present(Lib *lib);
void lib_clear(Lib *lib, float r, float g, float b, float a);
```

### The Design Philosophy

**Transparent struct, zero accessors.** Every handle is a public field. The user can pass `lib->dev` to any Vulkan function, `lib->wl` to any Wayland function, `lib->event` to any sd_event function. No wrapping, no getters, no abstract interfaces.

**Extension hooks.** The `extra_instance_exts`/`extra_device_exts` arrays let users add Vulkan extensions (raytracing, mesh shaders, etc.) without modifying the library. The `pnext` chains let users inject custom chained structs into instance/device creation.

**sd_event ownership.** The library creates the sd_event object. The user can add their own sources before calling `lib_run`. Everything runs in one loop.

### What Your Library Must Handle

- **Init order:** Wayland first (needs globals for surface), then DRM/GBM, then Vulkan, then buffers
- **Destroy order:** Reverse — stop event loop, destroy buffers, destroy Vulkan, destroy DRM, destroy Wayland
- **Resize:** Destroy all buffers on size change, recreate at new size
- **Buffer busy tracking:** Don't draw to a buffer the compositor still has
- **The fd lifecycle:** dup before each consumer, close the original after

### Your First Library User

```c
#include "lib.h"
#include <stdio.h>

static float t = 0;

static void frame(Lib *lib, void *user) {
    (void)user;
    VkCommandBuffer cmd = lib->cur_buf->cmd;
    VkImage img = lib->cur_buf->image;

    t += 0.02f;
    float r = 0.5f + 0.5f * __builtin_sinf(t);
    float g = 0.5f + 0.5f * __builtin_sinf(t + 2.094f);
    float b = 0.5f + 0.5f * __builtin_sinf(t + 4.188f);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);

    VkClearColorValue color = {{ r, g, b, 1.0f }};
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);

    lib_present(lib);
}

int main(void) {
    Lib lib;
    LibConfig cfg = { .title = "My Window", .width = 800, .height = 600 };

    if (lib_create(&lib, &cfg) != 0) {
        fprintf(stderr, "lib_create failed\n");
        return 1;
    }

    lib_run(&lib, frame, NULL);
    lib_destroy(&lib);
    return 0;
}
```

### What You Should Have Learned

By now, you should understand:
- Why Wayland uses a registry pattern (discovery, version negotiation)
- Why xdg-shell needs configure/ack (cooperative window management)
- Why frame callbacks exist (compositor-driven rendering)
- How GBM allocates GPU memory (render node → gbm_device → gbm_bo → dmabuf fd)
- How Vulkan imports external memory (VkExternalMemoryImageCreateInfo → VkImportMemoryFdInfoKHR)
- How linux-dmabuf protocol ships buffers (create_params → add → create_immed)
- Why triple-buffering prevents stalls (busy tracking, fence synchronization)
- How sd_event unifies the event loop (I/O, signals, timers)
- Why the transparent struct design empowers the user (zero accessors, full control)

### Where to Go From Here

- **Add raytracing** via `extra_device_exts = {"VK_KHR_ray_tracing_pipeline"}`
- **Integrate Skia** — use `lib->dev` to create a Skia GrContext, render UI over Vulkan
- **Add FFmpeg video** — decode frames into VkImages, present via libwl
- **Multi-window** — create multiple `Lib` instances, each with its own surface
- **Presentation Feedback** — `wp_presentation` protocol for precise frame timing

---

## Chapter 15: Production Edge Cases (Optional)

*For when you're ready to ship.*

This chapter is a checklist, not a tutorial. Each point has a reference for further reading.

### Robust Device Selection

Instead of `devices[0]`, iterate and pick:
- The discrete GPU (NVIDIA) over integrated (Intel)
- A device that supports all required extensions
- A device with a dedicated transfer queue for async uploads

Reference: [Vulkan Device Selection](https://github.com/SaschaWillems/Vulkan/blob/master/ examples/vulkanexamples.h)

### Modifier Enumeration

Some GPUs support multiple tiling formats. Instead of relying on GBM's default modifier, enumerate supported modifiers via `zwp_linux_dmabuf_v1` events and pick the best one.

Reference: `zwp_linux_dmabuf_v1_listener.format` and `.modifier` events

### Error Recovery

- **Lost device** (`VK_ERROR_DEVICE_LOST`): destroy and recreate the Vulkan device and all buffers
- **Wayland disconnect:** clean up and exit gracefully
- **OOM:** reduce buffer count, reduce resolution, fall back to SHM

### Multi-plane dmabuf

The code assumes single-planar formats (XRGB8888). NV12 (video) has two planes. Semi-planar formats have multiple dmabuf fds. The `add()` call handles this with the plane index parameter.

Reference: `GBM_FORMAT_NV12`, `drm_fourcc.h`

### Threading

sd_event is not thread-safe by default. If you dispatch from multiple threads, use `sd_event_add_io` with `sd_event_source_get_event` for per-source locking. Vulkan's `VK_QUEUE_FAMILY_EXTERNAL` and timeline semaphores enable cross-process synchronization.

### Validation

Use `VK_LAYER_KHRONOS_validation` (enabled via `vkconfig` or `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`). Do NOT enable it in code — vkconfig is the standard way on Arch.

### Memory Allocation

The code uses `vkAllocateMemory` directly, which is fine for a handful of buffers. For thousands, use `VK_EXT_memory_budget` and a custom allocator (VMA).

Reference: [Vulkan Memory Allocator](https://gpuopen.com/vulkan-memory-allocator/)

### Wayland Protocol Version Negotiation

Always check the `version` parameter in the registry listener and bind the minimum of what you need and what's available. A compositor might offer version 1 when you need version 3.

---

## Final Words

This tutorial was written by a human and an AI, documenting every misstep along the way. The AI made mistakes — forgetting flushes, swapping modifier halves, leaking fds, misordering operations. A human reviewer caught some. Some probably remain.

**That's the point.** No code is perfect. No tutorial is complete. The goal isn't "copy this and it works" — it's "understand this so you can build your own, fix the bugs, and go further than we did."

Go write your library now.

*— Codotaku, 2026*
