#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "libwl.h"

static float t = 0;

static void frame(Libwl *lib, [[maybe_unused]] void *user)
{
    VkCommandBuffer cmd = lib->cur_buf->cmd;
    VkImage         img = lib->cur_buf->image;

    t += 0.02f;
    float r = 0.5f + 0.5f * __builtin_sinf(t);
    float g = 0.5f + 0.5f * __builtin_sinf(t + 2.094f);
    float b = 0.5f + 0.5f * __builtin_sinf(t + 4.188f);

    VkImageMemoryBarrier barrier = {
        .sType          = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout      = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout      = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image          = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .dstAccessMask  = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue color = {{ r, g, b, 1.0f }};
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);

    if (libwl_present(lib) != 0) {
        fprintf(stderr, "libwl_present failed\n");
        lib->running = false;
    }
}

int main(void)
{
    Libwl lib;
    LibwlConfig cfg = {
        .title  = "libwl demo",
        .width  = 800,
        .height = 600,
    };

    if (libwl_create(&lib, &cfg) != 0) {
        fprintf(stderr, "libwl_create failed\n");
        return 1;
    }

    printf("libwl: VkDevice %p, wl_display %p, wl_surface %p\n",
           (void*)lib.dev, (void*)lib.wl, (void*)lib.surface);
    printf("libwl: drm_fd %d, gbm %p, format 0x%x\n",
           lib.drm_fd, (void*)lib.gbm, lib.format);

    int r = libwl_run(&lib, frame, nullptr);
    if (r < 0) fprintf(stderr, "libwl_run failed\n");
    libwl_destroy(&lib);
    return 0;
}
