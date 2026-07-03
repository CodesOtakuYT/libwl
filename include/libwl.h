#ifndef LIBWL_H
#define LIBWL_H

#include <stdint.h>
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

typedef struct {
    struct wl_buffer  *wl_buf;
    struct gbm_bo     *bo;
    int                dmabuf_fd;
    uint32_t           stride;
    uint32_t           offset;
    uint64_t           modifier;
    int                busy;
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
    int                          pointer_entered;
    double                       pointer_x;
    double                       pointer_y;
    int                          keyboard_entered;
    uint8_t                      pressed_keys[256];

    /* ── Window state ───────────────────────────────────────── */
    int                          width;
    int                          height;

    /* ── Buffer state ───────────────────────────────────────── */
    LibwlBuffer                  buffers[LIBWL_NUM_BUFFERS];
    LibwlBuffer                 *cur_buf;
    int                          cur_slot;
    struct wl_callback          *frame_callback;
    int                          pending_configure;
    int                          pending_resize;
    int                          initialized;
    int                          running;

    /* ── User frame callback ────────────────────────────────── */
    libwl_frame_fn               frame_cb;
    void                        *frame_user;
} Libwl;

int  libwl_create(Libwl *lib, const LibwlConfig *cfg);
void libwl_destroy(Libwl *lib);
int  libwl_run(Libwl *lib, libwl_frame_fn frame_cb, void *user);
int  libwl_present(Libwl *lib);
void libwl_clear(Libwl *lib, float r, float g, float b, float a);

#endif
