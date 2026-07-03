#ifndef LIBWL_H
#define LIBWL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>

struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct zwp_linux_dmabuf_v1;
struct wl_callback;
struct wl_buffer;
struct gbm_device;
struct gbm_bo;
struct sd_event;

#define LIBWL_NUM_BUFFERS 3

/* ── Arena: bump allocator backed by virtual memory ────────────────────
 *
 * mmap(PROT_NONE|MAP_NORESERVE) reserves virtual address space at init.
 * Physical pages are committed on demand via mmap(MAP_FIXED) as the
 * bump pointer advances.  arena_reset uses MADV_DONTNEED to return
 * pages to the kernel — the next access gets zero-fill.
 *
 * The base address is fixed for the arena's lifetime, so all pointers
 * into the arena remain valid forever.  No realloc, no invalidation.
 *
 * The arena embedded in Libwl is auto-initialized by libwl_create.
 * Call arena_init_size(&lib->arena, ...) after libwl_create to override
 * the default size.  Use arena_alloc/arena_strdup/arena_save/arena_restore
 * for temporary allocations during frame callbacks.
 *
 * Default: 1 GB virtual reservation, 64 KB initial commit.
 */

#define ARENA_DEFAULT_VIRTUAL_SIZE  ((size_t)1 << 30)  // 1 GB
#define ARENA_DEFAULT_COMMIT_SIZE   ((size_t)64 << 10) // 64 KB

typedef struct {
    char   *base;      // fixed virtual address (never changes)
    char   *ptr;       // current bump position
    size_t  committed; // bytes with physical backing
    size_t  reserved;  // total virtual reservation
    bool    owned;     // true if we mmap'd it
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

typedef struct {
    struct wl_buffer  *wl_buf;
    struct gbm_bo     *bo;
    int                dmabuf_fd;
    uint32_t           stride;
    uint32_t           offset;
    uint64_t           modifier;
    bool               busy;
    int                width;
    int                height;
    VkImage            image;
    VkDeviceMemory     memory;
    VkCommandBuffer    cmd;
    VkFence            fence;
} LibwlBuffer;

typedef struct {
    const char    *title;
    int            width;
    int            height;
    const char   **extra_instance_exts;
    int            extra_instance_ext_count;
    const char   **extra_device_exts;
    int            extra_device_ext_count;
    void          *instance_pnext;
    void          *device_pnext;
} LibwlConfig;

struct Libwl;

typedef void (*libwl_frame_fn)(struct Libwl *lib, void *user);

typedef struct Libwl {
    /* ── Arena ──────────────────────────────────────────────── */
    Arena               arena;

    /* ── Wayland ────────────────────────────────────────────── */
    struct wl_display           *wl;
    struct wl_registry          *registry;
    struct wl_compositor        *compositor;
    struct xdg_wm_base          *wm_base;
    struct zwp_linux_dmabuf_v1  *dmabuf;
    struct wl_surface           *surface;
    struct xdg_surface          *xdg_surface;
    struct xdg_toplevel         *xdg_toplevel;

    /* ── DRM / GBM ──────────────────────────────────────────── */
    int                          drm_fd;
    struct gbm_device           *gbm;
    uint32_t                     format;

    /* ── Vulkan ─────────────────────────────────────────────── */
    VkInstance                   inst;
    VkPhysicalDevice             phys_dev;
    VkDevice                     dev;
    VkQueue                      queue;
    uint32_t                     queue_family;
    VkCommandPool                cmd_pool;
    PFN_vkGetImageMemoryRequirements2KHR vkGetImageMemoryRequirements2KHR;
    PFN_vkGetMemoryFdPropertiesKHR       vkGetMemoryFdPropertiesKHR;
    PFN_vkBindImageMemory2KHR            vkBindImageMemory2KHR;

    /* ── Event loop ─────────────────────────────────────────── */
    struct sd_event             *event;

    /* ── Input ──────────────────────────────────────────────── */
    struct wl_seat              *seat;
    struct wl_pointer           *pointer;
    struct wl_keyboard          *keyboard;
    bool                         pointer_entered;
    double                       pointer_x;
    double                       pointer_y;
    bool                         keyboard_entered;
    uint8_t                      pressed_keys[256];

    /* ── Window state ───────────────────────────────────────── */
    int                          width;
    int                          height;

    /* ── Buffer state ───────────────────────────────────────── */
    LibwlBuffer                  buffers[LIBWL_NUM_BUFFERS];
    LibwlBuffer                 *cur_buf;
    int                          cur_slot;
    struct wl_callback          *frame_callback;
    bool                         pending_configure;
    bool                         pending_resize;
    bool                         initialized;
    bool                         running;

    /* ── User frame callback ────────────────────────────────── */
    libwl_frame_fn               frame_cb;
    void                        *frame_user;
} Libwl;

[[nodiscard]] int  libwl_create(Libwl *lib, const LibwlConfig *cfg);
void libwl_destroy(Libwl *lib);
[[nodiscard]] int  libwl_run(Libwl *lib, libwl_frame_fn frame_cb, void *user);
[[nodiscard]] int  libwl_present(Libwl *lib);
void libwl_clear(Libwl *lib, float r, float g, float b, float a);

#endif
