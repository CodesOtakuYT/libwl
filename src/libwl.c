#define _GNU_SOURCE

#include "libwl.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <systemd/sd-event.h>

#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

/* ── forward declarations ─────────────────────────────────────────── */

static void on_frame_done(void *data, struct wl_callback *cb, uint32_t time);

static const struct wl_callback_listener frame_listener = {
    .done = (void (*)(void*,struct wl_callback*,uint32_t))on_frame_done,
};

/* ── buffer listeners ────────────────────────────────────────────── */

static void on_buffer_release(void *data, [[maybe_unused]] struct wl_buffer *wl_buffer)
{
    LibwlBuffer *buf = data;
    buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = on_buffer_release,
};

/* ── Vulkan image creation ───────────────────────────────────────── */

static void create_vk_image(Libwl *lib, LibwlBuffer *buf)
{
    VkResult ret;

    VkImageCreateInfo image_ci = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = VK_FORMAT_B8G8R8A8_UNORM,
        .extent      = { (uint32_t)buf->width, (uint32_t)buf->height, 1 },
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkExternalMemoryImageCreateInfo ext_ci = {
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    if (buf->modifier != DRM_FORMAT_MOD_INVALID) {
        VkSubresourceLayout plane_layout = {
            .offset   = buf->offset,
            .rowPitch = buf->stride,
        };
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod_ci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier           = buf->modifier,
            .drmFormatModifierPlaneCount = 1,
            .pPlaneLayouts               = &plane_layout,
        };
        image_ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        image_ci.pNext  = &mod_ci;
        mod_ci.pNext    = &ext_ci;
    } else {
        image_ci.tiling = VK_IMAGE_TILING_LINEAR;
        image_ci.pNext  = &ext_ci;
    }

    ret = vkCreateImage(lib->dev, &image_ci, nullptr, &buf->image);
    assert(ret == VK_SUCCESS);

    int fd = dup(buf->dmabuf_fd);
    assert(fd >= 0);

    VkMemoryFdPropertiesKHR fd_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    ret = lib->vkGetMemoryFdPropertiesKHR(lib->dev,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        fd, &fd_props);
    assert(ret == VK_SUCCESS);

    VkImageMemoryRequirementsInfo2 req_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = buf->image,
    };
    VkMemoryRequirements2 mem_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    lib->vkGetImageMemoryRequirements2KHR(lib->dev, &req_info, &mem_reqs);

    uint32_t mem_type_bits = fd_props.memoryTypeBits &
        mem_reqs.memoryRequirements.memoryTypeBits;
    assert(mem_type_bits > 0);

    VkImportMemoryFdInfoKHR import_info = {
        .sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd         = fd,
    };

    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &import_info,
        .allocationSize  = mem_reqs.memoryRequirements.size,
        .memoryTypeIndex = (uint32_t)(__builtin_ffs((int)mem_type_bits) - 1),
    };

    ret = vkAllocateMemory(lib->dev, &alloc_info, nullptr, &buf->memory);
    assert(ret == VK_SUCCESS);

    VkBindImageMemoryInfo bind_info = {
        .sType  = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image  = buf->image,
        .memory = buf->memory,
    };

    ret = lib->vkBindImageMemory2KHR(lib->dev, 1, &bind_info);
    assert(ret == VK_SUCCESS);

    VkCommandBufferAllocateInfo cmd_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = lib->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    ret = vkAllocateCommandBuffers(lib->dev, &cmd_info, &buf->cmd);
    assert(ret == VK_SUCCESS);

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    ret = vkCreateFence(lib->dev, &fence_info, nullptr, &buf->fence);
    assert(ret == VK_SUCCESS);
}

/* ── buffer creation / destruction ────────────────────────────────── */

static int create_buffer(Libwl *lib, LibwlBuffer *buf, int width, int height)
{
    memset(buf, 0, sizeof(*buf));
    buf->dmabuf_fd = -1;
    buf->width  = width;
    buf->height = height;

    buf->bo = gbm_bo_create(lib->gbm, width, height, lib->format,
                            GBM_BO_USE_RENDERING);
    if (!buf->bo) {
        fprintf(stderr, "libwl: gbm_bo_create failed\n");
        return -1;
    }
    buf->modifier = gbm_bo_get_modifier(buf->bo);

    union gbm_bo_handle handle = gbm_bo_get_handle(buf->bo);
    int ret = drmPrimeHandleToFD(lib->drm_fd, handle.u32, 0, &buf->dmabuf_fd);
    assert(ret == 0);
    buf->stride = gbm_bo_get_stride_for_plane(buf->bo, 0);
    buf->offset = gbm_bo_get_offset(buf->bo, 0);

    create_vk_image(lib, buf);

    struct zwp_linux_buffer_params_v1 *params;
    params = zwp_linux_dmabuf_v1_create_params(lib->dmabuf);
    zwp_linux_buffer_params_v1_add(params,
        buf->dmabuf_fd, 0,
        buf->offset, buf->stride,
        buf->modifier >> 32, buf->modifier & 0xffffffff);

    buf->wl_buf = zwp_linux_buffer_params_v1_create_immed(params,
        width, height, lib->format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!buf->wl_buf) {
        fprintf(stderr, "libwl: create_immed failed\n");
        return -1;
    }
    wl_buffer_add_listener(buf->wl_buf, &buffer_listener, buf);

    close(buf->dmabuf_fd);
    buf->dmabuf_fd = -1;
    return 0;
}

static void destroy_buffer(Libwl *lib, LibwlBuffer *buf)
{
    if (buf->fence)   vkDestroyFence(lib->dev, buf->fence, nullptr);
    if (buf->cmd)     vkFreeCommandBuffers(lib->dev, lib->cmd_pool, 1, &buf->cmd);
    if (buf->memory)  vkFreeMemory(lib->dev, buf->memory, nullptr);
    if (buf->image)   vkDestroyImage(lib->dev, buf->image, nullptr);
    if (buf->wl_buf)  wl_buffer_destroy(buf->wl_buf);
    if (buf->bo)      gbm_bo_destroy(buf->bo);
    if (buf->dmabuf_fd >= 0) close(buf->dmabuf_fd);
}

static void recreate_buffers(Libwl *lib, int width, int height)
{
    vkDeviceWaitIdle(lib->dev);
    for (int i = 0; i < LIBWL_NUM_BUFFERS; i++)
        destroy_buffer(lib, &lib->buffers[i]);
    for (int i = 0; i < LIBWL_NUM_BUFFERS; i++) {
        int r = create_buffer(lib, &lib->buffers[i], width, height);
        assert(r == 0);
    }
    lib->width  = width;
    lib->height = height;
    lib->pending_resize = false;
}

static LibwlBuffer *next_free_buffer(Libwl *lib)
{
    for (int i = 0; i < LIBWL_NUM_BUFFERS; i++) {
        if (!lib->buffers[i].busy)
            return &lib->buffers[i];
    }
    return nullptr;
}

/* ── Vulkan setup ────────────────────────────────────────────────── */

static int setup_vulkan(Libwl *lib, const LibwlConfig *cfg)
{
    VkResult ret;

    // Temp allocations use the arena — freed implicitly after setup
    size_t mark = arena_save(&lib->arena);

    const char *base_instance_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    int num_inst_exts = 2;
    const char **inst_exts = nullptr;

    if (cfg->extra_instance_ext_count > 0) {
        int n = num_inst_exts + cfg->extra_instance_ext_count;
        inst_exts = arena_alloc(&lib->arena, (size_t)n * sizeof(char*));
        if (!inst_exts) goto fail;
        memcpy(inst_exts, base_instance_exts, num_inst_exts * sizeof(char*));
        memcpy(inst_exts + num_inst_exts, cfg->extra_instance_exts,
               cfg->extra_instance_ext_count * sizeof(char*));
        num_inst_exts = n;
    } else {
        inst_exts = (const char**)base_instance_exts;
    }

    VkApplicationInfo app = {
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = cfg->title ? cfg->title : "libwl",
        .apiVersion       = VK_MAKE_API_VERSION(0, 1, 4, 0),
    };
    VkInstanceCreateInfo ici = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = cfg->instance_pnext,
        .pApplicationInfo        = &app,
        .enabledExtensionCount   = (uint32_t)num_inst_exts,
        .ppEnabledExtensionNames = inst_exts,
    };
    ret = vkCreateInstance(&ici, nullptr, &lib->inst);
    if (ret != VK_SUCCESS) goto fail;

    uint32_t count;
    ret = vkEnumeratePhysicalDevices(lib->inst, &count, nullptr);
    if (ret != VK_SUCCESS || count == 0) goto fail;

    VkPhysicalDevice *devs = arena_alloc(&lib->arena,
                                         count * sizeof(VkPhysicalDevice));
    if (!devs) goto fail;
    ret = vkEnumeratePhysicalDevices(lib->inst, &count, devs);
    if (ret != VK_SUCCESS) goto fail;
    lib->phys_dev = devs[0];

    vkGetPhysicalDeviceQueueFamilyProperties(lib->phys_dev, &count, nullptr);
    VkQueueFamilyProperties *props = arena_alloc(&lib->arena,
        count * sizeof(VkQueueFamilyProperties));
    if (!props) goto fail;
    vkGetPhysicalDeviceQueueFamilyProperties(lib->phys_dev, &count, props);
    lib->queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            lib->queue_family = i;
            break;
        }
    }
    if (lib->queue_family == UINT32_MAX) goto fail;

    const char *base_device_exts[] = {
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
    };
    int num_dev_exts = 8;
    const char **dev_exts = nullptr;

    if (cfg->extra_device_ext_count > 0) {
        int n = num_dev_exts + cfg->extra_device_ext_count;
        dev_exts = arena_alloc(&lib->arena, (size_t)n * sizeof(char*));
        if (!dev_exts) goto fail;
        memcpy(dev_exts, base_device_exts, num_dev_exts * sizeof(char*));
        memcpy(dev_exts + num_dev_exts, cfg->extra_device_exts,
               cfg->extra_device_ext_count * sizeof(char*));
        num_dev_exts = n;
    } else {
        dev_exts = (const char**)base_device_exts;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = lib->queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo dci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = cfg->device_pnext,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = (uint32_t)num_dev_exts,
        .ppEnabledExtensionNames = dev_exts,
    };
    ret = vkCreateDevice(lib->phys_dev, &dci, nullptr, &lib->dev);
    if (ret != VK_SUCCESS) goto fail;

    vkGetDeviceQueue(lib->dev, lib->queue_family, 0, &lib->queue);

    VkCommandPoolCreateInfo cpi = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = lib->queue_family,
    };
    ret = vkCreateCommandPool(lib->dev, &cpi, nullptr, &lib->cmd_pool);
    if (ret != VK_SUCCESS) goto fail;

    lib->vkGetImageMemoryRequirements2KHR =
        (PFN_vkGetImageMemoryRequirements2KHR)
        vkGetDeviceProcAddr(lib->dev, "vkGetImageMemoryRequirements2KHR");
    lib->vkGetMemoryFdPropertiesKHR =
        (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(lib->dev, "vkGetMemoryFdPropertiesKHR");
    lib->vkBindImageMemory2KHR =
        (PFN_vkBindImageMemory2KHR)
        vkGetDeviceProcAddr(lib->dev, "vkBindImageMemory2KHR");

    if (!lib->vkGetImageMemoryRequirements2KHR ||
        !lib->vkGetMemoryFdPropertiesKHR ||
        !lib->vkBindImageMemory2KHR)
        goto fail;

    arena_restore(&lib->arena, mark);
    return 0;

fail:
    arena_restore(&lib->arena, mark);
    return -1;
}

/* ── xdg_wm_base ping ────────────────────────────────────────────── */

static void on_wm_base_ping([[maybe_unused]] void *data, struct xdg_wm_base *wm_base,
                            uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = on_wm_base_ping,
};

/* ── xdg_toplevel listeners ──────────────────────────────────────── */

static void on_toplevel_configure(void *data, [[maybe_unused]] struct xdg_toplevel *toplevel,
                                  int32_t w, int32_t h,
                                  [[maybe_unused]] struct wl_array *states)
{
    Libwl *lib = data;
    if (w > 0 && h > 0 && (lib->width != w || lib->height != h)) {
        lib->width  = w;
        lib->height = h;
        lib->pending_resize = true;
    }
}

static void on_toplevel_close(void *data, [[maybe_unused]] struct xdg_toplevel *toplevel)
{
    Libwl *lib = data;
    lib->running = false;
    if (lib->event)
        sd_event_exit(lib->event, 0);
}

static void on_toplevel_configure_bounds(void *data,
                                         struct xdg_toplevel *toplevel,
                                         int32_t width, int32_t height)
{
    (void)data; (void)toplevel; (void)width; (void)height;
}

static void on_toplevel_wm_capabilities(void *data,
                                        struct xdg_toplevel *toplevel,
                                        struct wl_array *capabilities)
{
    (void)data; (void)toplevel; (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure        = on_toplevel_configure,
    .close            = on_toplevel_close,
    .configure_bounds = on_toplevel_configure_bounds,
    .wm_capabilities  = on_toplevel_wm_capabilities,
};

/* ── xdg_surface listener ────────────────────────────────────────── */

static void on_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                 uint32_t serial)
{
    Libwl *lib = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    lib->pending_configure = false;
    if (lib->initialized)
        on_frame_done(lib, nullptr, 0);
}

static const struct xdg_surface_listener surface_listener = {
    .configure = on_surface_configure,
};

/* ── input listeners ─────────────────────────────────────────────── */

static void on_pointer_enter(void *data, struct wl_pointer *pointer,
                             uint32_t serial, struct wl_surface *surface,
                             wl_fixed_t sx, wl_fixed_t sy)
{
    Libwl *lib = data;
    (void)pointer; (void)serial; (void)surface;
    lib->pointer_entered = true;
    lib->pointer_x = wl_fixed_to_double(sx);
    lib->pointer_y = wl_fixed_to_double(sy);
}

static void on_pointer_leave(void *data, struct wl_pointer *pointer,
                             uint32_t serial, struct wl_surface *surface)
{
    Libwl *lib = data;
    (void)pointer; (void)serial; (void)surface;
    lib->pointer_entered = false;
}

static void on_pointer_motion(void *data, struct wl_pointer *pointer,
                              uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    Libwl *lib = data;
    (void)pointer; (void)time;
    lib->pointer_x = wl_fixed_to_double(sx);
    lib->pointer_y = wl_fixed_to_double(sy);
}

static void on_pointer_button(void *data, struct wl_pointer *pointer,
                              uint32_t serial, uint32_t time, uint32_t button,
                              uint32_t state)
{
    (void)data; (void)pointer; (void)serial; (void)time; (void)button; (void)state;
}

static void on_pointer_axis(void *data, struct wl_pointer *pointer,
                            uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static void on_pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
}

static void on_pointer_axis_source(void *data, struct wl_pointer *pointer,
                                   uint32_t axis_source)
{
    (void)data; (void)pointer; (void)axis_source;
}

static void on_pointer_axis_stop(void *data, struct wl_pointer *pointer,
                                 uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void on_pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                     uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter         = on_pointer_enter,
    .leave         = on_pointer_leave,
    .motion        = on_pointer_motion,
    .button        = on_pointer_button,
    .axis          = on_pointer_axis,
    .frame         = on_pointer_frame,
    .axis_source   = on_pointer_axis_source,
    .axis_stop     = on_pointer_axis_stop,
    .axis_discrete = on_pointer_axis_discrete,
};

static void on_keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                               uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)keyboard; (void)format; (void)fd; (void)size;
    close(fd);
}

static void on_keyboard_enter(void *data, struct wl_keyboard *keyboard,
                              uint32_t serial, struct wl_surface *surface,
                              struct wl_array *keys)
{
    Libwl *lib = data;
    (void)keyboard; (void)serial; (void)surface;
    lib->keyboard_entered = true;
    memset(lib->pressed_keys, 0, 256);
    uint32_t *k;
    for (k = (uint32_t*)keys->data;
         (const char*)k < (const char*)keys->data + keys->size;
         k++)
        lib->pressed_keys[*k] = 1;
}

static void on_keyboard_leave(void *data, struct wl_keyboard *keyboard,
                              uint32_t serial, struct wl_surface *surface)
{
    Libwl *lib = data;
    (void)keyboard; (void)serial; (void)surface;
    lib->keyboard_entered = false;
    memset(lib->pressed_keys, 0, 256);
}

static void on_keyboard_key(void *data, struct wl_keyboard *keyboard,
                            uint32_t serial, uint32_t time, uint32_t key,
                            uint32_t state)
{
    Libwl *lib = data;
    (void)keyboard; (void)serial; (void)time;
    lib->pressed_keys[key] = (state == WL_KEYBOARD_KEY_STATE_PRESSED) ? 1 : 0;
}

static void on_keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, uint32_t mods_depressed,
                                  uint32_t mods_latched, uint32_t mods_locked,
                                  uint32_t group)
{
    (void)data; (void)keyboard; (void)serial;
    (void)mods_depressed; (void)mods_latched;
    (void)mods_locked; (void)group;
}

static void on_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                    int32_t rate, int32_t delay)
{
    (void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = on_keyboard_keymap,
    .enter       = on_keyboard_enter,
    .leave       = on_keyboard_leave,
    .key         = on_keyboard_key,
    .modifiers   = on_keyboard_modifiers,
    .repeat_info = on_keyboard_repeat_info,
};

static void on_seat_capabilities(void *data, struct wl_seat *seat,
                                 uint32_t capabilities)
{
    Libwl *lib = data;
    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        lib->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(lib->pointer, &pointer_listener, lib);
    }
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        lib->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(lib->keyboard, &keyboard_listener, lib);
    }
}

static void on_seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = on_seat_capabilities,
    .name         = on_seat_name,
};

/* ── registry ─────────────────────────────────────────────────────── */

static void on_registry_global(void *data, struct wl_registry *registry,
                               uint32_t name, const char *interface,
                               uint32_t version)
{
    Libwl *lib = data;
    if (strcmp(interface, "wl_compositor") == 0) {
        lib->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface, 4);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        lib->wm_base = wl_registry_bind(registry, name,
                                        &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(lib->wm_base, &wm_base_listener, lib);
    } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
        if (version < 3) return;
        lib->dmabuf = wl_registry_bind(registry, name,
                                       &zwp_linux_dmabuf_v1_interface, 3);
    } else if (strcmp(interface, "wl_seat") == 0) {
        lib->seat = wl_registry_bind(registry, name,
                                     &wl_seat_interface, 7);
        wl_seat_add_listener(lib->seat, &seat_listener, lib);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global        = on_registry_global,
    .global_remove = nullptr,
};

/* ── frame callback cycle ─────────────────────────────────────────── */

static void on_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    Libwl *lib = data;
    (void)time;
    if (cb) {
        wl_callback_destroy(cb);
        lib->frame_callback = nullptr;
    }

    if (lib->pending_resize)
        recreate_buffers(lib, lib->width, lib->height);

    LibwlBuffer *buf = next_free_buffer(lib);
    if (!buf) {
        fprintf(stderr, "libwl: no free buffer\n");
        return;
    }

    vkWaitForFences(lib->dev, 1, &buf->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(lib->dev, 1, &buf->fence);

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(buf->cmd, &begin);

    lib->cur_buf  = buf;
    lib->cur_slot = (int)(buf - lib->buffers);

    lib->frame_cb(lib, lib->frame_user);
}

/* ── public API ───────────────────────────────────────────────────── */

int libwl_create(Libwl *lib, const LibwlConfig *cfg)
{
    memset(lib, 0, sizeof(*lib));
    if (!arena_init(&lib->arena)) return -1;

    lib->format = GBM_FORMAT_XRGB8888;

    lib->wl = wl_display_connect(nullptr);
    if (!lib->wl) return -1;

    lib->registry = wl_display_get_registry(lib->wl);
    wl_registry_add_listener(lib->registry, &registry_listener, lib);
    wl_display_roundtrip(lib->wl);
    if (!lib->compositor || !lib->wm_base || !lib->dmabuf || !lib->seat)
        return -1;

    const char *drm_node = getenv("DRM_RENDER_NODE");
    if (!drm_node) drm_node = "/dev/dri/renderD128";
    lib->drm_fd = open(drm_node, O_RDWR);
    if (lib->drm_fd < 0) return -1;

    lib->gbm = gbm_create_device(lib->drm_fd);
    if (!lib->gbm) return -1;

    if (setup_vulkan(lib, cfg) != 0) return -1;

    lib->width  = cfg->width;
    lib->height = cfg->height;

    lib->surface = wl_compositor_create_surface(lib->compositor);
    lib->xdg_surface = xdg_wm_base_get_xdg_surface(lib->wm_base, lib->surface);
    xdg_surface_add_listener(lib->xdg_surface, &surface_listener, lib);

    lib->xdg_toplevel = xdg_surface_get_toplevel(lib->xdg_surface);
    xdg_toplevel_add_listener(lib->xdg_toplevel, &toplevel_listener, lib);
    xdg_toplevel_set_title(lib->xdg_toplevel,
                           cfg->title ? cfg->title : "libwl");

    lib->pending_configure = true;
    wl_surface_commit(lib->surface);

    for (int i = 0; i < LIBWL_NUM_BUFFERS; i++) {
        if (create_buffer(lib, &lib->buffers[i], cfg->width, cfg->height) != 0)
            return -1;
    }

    return 0;
}

void libwl_destroy(Libwl *lib)
{
    if (!lib) return;

    if (lib->frame_callback)
        wl_callback_destroy(lib->frame_callback);
    lib->frame_callback = nullptr;

    for (int i = 0; i < LIBWL_NUM_BUFFERS; i++)
        destroy_buffer(lib, &lib->buffers[i]);

    if (lib->pointer)   wl_pointer_destroy(lib->pointer);
    if (lib->keyboard)  wl_keyboard_destroy(lib->keyboard);
    if (lib->seat)      wl_seat_destroy(lib->seat);
    if (lib->xdg_toplevel) xdg_toplevel_destroy(lib->xdg_toplevel);
    if (lib->xdg_surface)  xdg_surface_destroy(lib->xdg_surface);
    if (lib->surface)      wl_surface_destroy(lib->surface);

    if (lib->cmd_pool)  vkDestroyCommandPool(lib->dev, lib->cmd_pool, nullptr);
    if (lib->dev)       vkDestroyDevice(lib->dev, nullptr);
    if (lib->inst)      vkDestroyInstance(lib->inst, nullptr);

    if (lib->gbm)       gbm_device_destroy(lib->gbm);
    if (lib->drm_fd >= 0) close(lib->drm_fd);

    if (lib->dmabuf)    zwp_linux_dmabuf_v1_destroy(lib->dmabuf);
    if (lib->wm_base)   xdg_wm_base_destroy(lib->wm_base);
    if (lib->compositor) wl_compositor_destroy(lib->compositor);
    if (lib->registry)  wl_registry_destroy(lib->registry);
    if (lib->wl) {
        wl_display_flush(lib->wl);
        wl_display_disconnect(lib->wl);
    }
}

int libwl_present(Libwl *lib)
{
    LibwlBuffer *buf = lib->cur_buf;
    if (!buf) return -1;

    VkResult ret = vkEndCommandBuffer(buf->cmd);
    if (ret != VK_SUCCESS) return -1;

    VkSubmitInfo submit = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &buf->cmd,
    };
    ret = vkQueueSubmit(lib->queue, 1, &submit, buf->fence);
    if (ret != VK_SUCCESS) return -1;

    wl_surface_attach(lib->surface, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(lib->surface, 0, 0, lib->width, lib->height);

    lib->frame_callback = wl_surface_frame(lib->surface);
    wl_callback_add_listener(lib->frame_callback, &frame_listener, lib);
    wl_surface_commit(lib->surface);
    wl_display_flush(lib->wl);
    buf->busy = true;

    return 0;
}

void libwl_clear(Libwl *lib, float r, float g, float b, float a)
{
    LibwlBuffer *buf = lib->cur_buf;
    if (!buf) return;

    VkImageMemoryBarrier barrier = {
        .sType          = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout      = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout      = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image          = buf->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .dstAccessMask  = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(buf->cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue color = {{ r, g, b, a }};
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(buf->cmd, buf->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);

    VkImageMemoryBarrier barrier2 = {
        .sType          = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout      = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout      = VK_IMAGE_LAYOUT_GENERAL,
        .image          = buf->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcAccessMask  = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask  = VK_ACCESS_MEMORY_READ_BIT,
    };
    vkCmdPipelineBarrier(buf->cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier2);
}

/* ── sd_event callbacks ──────────────────────────────────────────── */

static int on_wl_event(sd_event_source *s, int fd, uint32_t revents,
                       void *userdata)
{
    Libwl *lib = userdata;
    (void)s; (void)fd;
    if (revents & (EPOLLERR | EPOLLHUP)) {
        lib->running = 0;
        sd_event_exit(lib->event, 0);
        return 0;
    }
    if (revents & EPOLLIN) {
        if (wl_display_dispatch(lib->wl) == -1) {
            lib->running = 0;
            sd_event_exit(lib->event, 0);
        }
    }
    return 0;
}

static int on_signal(sd_event_source *s, const struct signalfd_siginfo *si,
                     void *userdata)
{
    Libwl *lib = userdata;
    (void)s; (void)si;
    lib->running = 0;
    sd_event_exit(lib->event, 0);
    return 0;
}

/* ── libwl_run ────────────────────────────────────────────────────── */

int libwl_run(Libwl *lib, libwl_frame_fn frame_cb, void *user)
{
    lib->running    = true;
    lib->frame_cb   = frame_cb;
    lib->frame_user = user;
    lib->initialized = true;

    int r = sd_event_new(&lib->event);
    if (r < 0) return -1;

    int wl_fd = wl_display_get_fd(lib->wl);
    r = sd_event_add_io(lib->event, nullptr, wl_fd, EPOLLIN, on_wl_event, lib);
    if (r < 0) return -1;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    r = sd_event_add_signal(lib->event, nullptr, SIGINT, on_signal, lib);
    if (r < 0) return -1;
    r = sd_event_add_signal(lib->event, nullptr, SIGTERM, on_signal, lib);
    if (r < 0) return -1;

    if (!lib->pending_configure) {
        lib->frame_callback = wl_surface_frame(lib->surface);
        wl_callback_add_listener(lib->frame_callback, &frame_listener, lib);
        wl_surface_commit(lib->surface);
    }

    wl_display_flush(lib->wl);

    r = sd_event_loop(lib->event);

    sd_event_unref(lib->event);
    lib->event = nullptr;

    return r < 0 ? -1 : 0;
}
