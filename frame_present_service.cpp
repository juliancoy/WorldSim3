#include "worldsim_app_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
bool writePpmRgb(
    const fs::path& out_path,
    const uint8_t* pixels,
    uint32_t width,
    uint32_t height,
    size_t row_pitch,
    VkFormat fmt,
    std::string& err) {
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        err = "failed to open screenshot output";
        return false;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    const bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
    const bool rgba = (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB);
    if (!bgra && !rgba) {
        err = "unsupported swapchain format for screenshot";
        return false;
    }
    std::vector<uint8_t> line((size_t)width * 3);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = pixels + (size_t)y * row_pitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* px = src + (size_t)x * 4;
            uint8_t r = rgba ? px[0] : px[2];
            uint8_t g = px[1];
            uint8_t b = rgba ? px[2] : px[0];
            size_t i = (size_t)x * 3;
            line[i + 0] = r;
            line[i + 1] = g;
            line[i + 2] = b;
        }
        out.write(reinterpret_cast<const char*>(line.data()), (std::streamsize)line.size());
    }
    if (!out.good()) {
        err = "failed writing screenshot file";
        return false;
    }
    return true;
}

bool writePpmRgbResized(
    const fs::path& out_path,
    const uint8_t* pixels,
    uint32_t src_width,
    uint32_t src_height,
    size_t row_pitch,
    VkFormat fmt,
    uint32_t out_width,
    uint32_t out_height,
    std::string& err) {
    if (out_width == src_width && out_height == src_height) {
        return writePpmRgb(out_path, pixels, src_width, src_height, row_pitch, fmt, err);
    }

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        err = "failed to open screenshot output";
        return false;
    }
    const bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
    const bool rgba = (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB);
    if (!bgra && !rgba) {
        err = "unsupported swapchain format for screenshot";
        return false;
    }

    out << "P6\n" << out_width << " " << out_height << "\n255\n";
    std::vector<uint8_t> line((size_t)out_width * 3, 0);
    const double scale = std::min((double)out_width / (double)src_width, (double)out_height / (double)src_height);
    const uint32_t fit_width = std::max(1u, std::min(out_width, (uint32_t)std::lround((double)src_width * scale)));
    const uint32_t fit_height = std::max(1u, std::min(out_height, (uint32_t)std::lround((double)src_height * scale)));
    const uint32_t offset_x = (out_width - fit_width) / 2;
    const uint32_t offset_y = (out_height - fit_height) / 2;

    auto sample_channel = [&](uint32_t sx, uint32_t sy, int channel) -> uint8_t {
        const uint8_t* row = pixels + (size_t)sy * row_pitch;
        const uint8_t* px = row + (size_t)sx * 4;
        if (channel == 0) return rgba ? px[0] : px[2];
        if (channel == 1) return px[1];
        return rgba ? px[2] : px[0];
    };

    for (uint32_t y = 0; y < out_height; ++y) {
        std::fill(line.begin(), line.end(), 0);
        if (y >= offset_y && y < offset_y + fit_height) {
            const double src_y = ((double)(y - offset_y) + 0.5) * (double)src_height / (double)fit_height - 0.5;
            const uint32_t y0 = (uint32_t)std::clamp((int)std::floor(src_y), 0, (int)src_height - 1);
            const uint32_t y1 = std::min(y0 + 1, src_height - 1);
            const double fy = std::clamp(src_y - (double)y0, 0.0, 1.0);
            for (uint32_t x = offset_x; x < offset_x + fit_width; ++x) {
                const double src_x = ((double)(x - offset_x) + 0.5) * (double)src_width / (double)fit_width - 0.5;
                const uint32_t x0 = (uint32_t)std::clamp((int)std::floor(src_x), 0, (int)src_width - 1);
                const uint32_t x1 = std::min(x0 + 1, src_width - 1);
                const double fx = std::clamp(src_x - (double)x0, 0.0, 1.0);
                const size_t i = (size_t)x * 3;
                for (int channel = 0; channel < 3; ++channel) {
                    const double c00 = (double)sample_channel(x0, y0, channel);
                    const double c10 = (double)sample_channel(x1, y0, channel);
                    const double c01 = (double)sample_channel(x0, y1, channel);
                    const double c11 = (double)sample_channel(x1, y1, channel);
                    const double c0 = c00 + (c10 - c00) * fx;
                    const double c1 = c01 + (c11 - c01) * fx;
                    const double c = c0 + (c1 - c0) * fy;
                    line[i + (size_t)channel] = (uint8_t)std::clamp((int)std::lround(c), 0, 255);
                }
            }
        }
        out.write(reinterpret_cast<const char*>(line.data()), (std::streamsize)line.size());
    }
    if (!out.good()) {
        err = "failed writing screenshot file";
        return false;
    }
    return true;
}

uint32_t findMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties{};
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem_properties);
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    std::abort();
}

void createBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& memory) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check_vk_result(vkCreateBuffer(g_Device, &buffer_info, g_Allocator, &buffer));

    VkMemoryRequirements mem_requirements{};
    vkGetBufferMemoryRequirements(g_Device, buffer, &mem_requirements);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = findMemoryType(mem_requirements.memoryTypeBits, properties);
    check_vk_result(vkAllocateMemory(g_Device, &alloc_info, g_Allocator, &memory));
    check_vk_result(vkBindBufferMemory(g_Device, buffer, memory, 0));
}
}  // namespace

void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    if (g_VulkanDeviceLost.load(std::memory_order_relaxed)) return;
    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    if (!check_vk_result_allow_device_loss(err)) return;
    g_CurrentFrameRenderIndex = wd->FrameIndex;

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    if (!check_vk_result_allow_device_loss(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX))) return;
    if (!check_vk_result_allow_device_loss(vkResetFences(g_Device, 1, &fd->Fence))) return;
    if (!check_vk_result_allow_device_loss(vkResetCommandPool(g_Device, fd->CommandPool, 0))) return;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check_vk_result_allow_device_loss(vkBeginCommandBuffer(fd->CommandBuffer, &begin))) return;
    recordZoningOutlineIndirectComputeDispatches(fd->CommandBuffer);

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = wd->RenderPass;
    rp.framebuffer = fd->Framebuffer;
    rp.renderArea.extent.width = wd->Width;
    rp.renderArea.extent.height = wd->Height;
    rp.clearValueCount = 1;
    rp.pClearValues = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    g_CurrentFrameRenderCommandBuffer = fd->CommandBuffer;
    g_CurrentFrameRenderPass = wd->RenderPass;
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    g_CurrentFrameRenderCommandBuffer = VK_NULL_HANDLE;
    g_CurrentFrameRenderPass = VK_NULL_HANDLE;
    vkCmdEndRenderPass(fd->CommandBuffer);
    if (!check_vk_result_allow_device_loss(vkEndCommandBuffer(fd->CommandBuffer))) return;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_acquired_semaphore;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &fd->CommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_complete_semaphore;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        if (!check_vk_result_allow_device_loss(vkQueueSubmit(g_Queue, 1, &submit, fd->Fence))) return;
    }

    uint64_t shot_req_id = 0;
    bool shot_request_native = false;
    uint32_t shot_requested_output_width = 0;
    uint32_t shot_requested_output_height = 0;
    {
        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
        if (g_ScreenshotState.pending) {
            shot_req_id = g_ScreenshotState.req_id;
            shot_request_native = g_ScreenshotState.request_native;
            shot_requested_output_width = g_ScreenshotState.requested_output_width;
            shot_requested_output_height = g_ScreenshotState.requested_output_height;
        }
    }
    if (shot_req_id == 0) return;

    auto complete_screenshot = [&](
        bool ok,
        const std::string& path,
        const std::string& error,
        uint32_t native_width = 0,
        uint32_t native_height = 0,
        uint32_t logical_width = 0,
        uint32_t logical_height = 0,
        uint32_t output_width = 0,
        uint32_t output_height = 0,
        float framebuffer_scale_x = 1.0f,
        float framebuffer_scale_y = 1.0f) {
        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
        if (g_ScreenshotState.pending && g_ScreenshotState.req_id == shot_req_id) {
            g_ScreenshotState.pending = false;
            g_ScreenshotState.done_id = shot_req_id;
            g_ScreenshotState.ok = ok;
            g_ScreenshotState.path = path;
            g_ScreenshotState.error = error;
            g_ScreenshotState.native_width = native_width;
            g_ScreenshotState.native_height = native_height;
            g_ScreenshotState.logical_width = logical_width;
            g_ScreenshotState.logical_height = logical_height;
            g_ScreenshotState.output_width = output_width;
            g_ScreenshotState.output_height = output_height;
            g_ScreenshotState.framebuffer_scale_x = framebuffer_scale_x;
            g_ScreenshotState.framebuffer_scale_y = framebuffer_scale_y;
            g_ScreenshotState.cv.notify_all();
        }
    };

    check_vk_result(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));

    VkImage src_image = fd->Backbuffer;
    if (src_image == VK_NULL_HANDLE || wd->Width == 0 || wd->Height == 0) {
        complete_screenshot(false, "", "invalid backbuffer");
        return;
    }
    if (!g_MainSwapchainTransferSrcSupported) {
        complete_screenshot(false, "", "surface does not support swapchain transfer-source screenshots");
        return;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    const uint32_t width = wd->Width;
    const uint32_t height = wd->Height;
    const ImVec2 framebuffer_scale = draw_data ? draw_data->FramebufferScale : ImVec2(1.0f, 1.0f);
    const float scale_x = framebuffer_scale.x > 0.0f ? framebuffer_scale.x : 1.0f;
    const float scale_y = framebuffer_scale.y > 0.0f ? framebuffer_scale.y : 1.0f;
    const uint32_t logical_width = std::max(1u, (uint32_t)std::lround((float)width / scale_x));
    const uint32_t logical_height = std::max(1u, (uint32_t)std::lround((float)height / scale_y));
    const uint32_t output_width =
        shot_request_native ? width :
        (shot_requested_output_width > 0 ? shot_requested_output_width : std::min(width, logical_width));
    const uint32_t output_height =
        shot_request_native ? height :
        (shot_requested_output_height > 0 ? shot_requested_output_height : std::min(height, logical_height));
    const VkDeviceSize image_size = (VkDeviceSize)width * (VkDeviceSize)height * 4;
    createBuffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging,
        staging_mem);

    VkCommandPool cap_pool = VK_NULL_HANDLE;
    VkCommandBuffer cap_cmd = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = g_QueueFamily;
    check_vk_result(vkCreateCommandPool(g_Device, &pool_info, g_Allocator, &cap_pool));
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = cap_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    check_vk_result(vkAllocateCommandBuffers(g_Device, &alloc_info, &cap_cmd));

    VkCommandBufferBeginInfo begin2{};
    begin2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(cap_cmd, &begin2));

    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = src_image;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        cap_cmd,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &to_src);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cap_cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);

    VkImageMemoryBarrier back_to_present{};
    back_to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    back_to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back_to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    back_to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back_to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    back_to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back_to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back_to_present.image = src_image;
    back_to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    back_to_present.subresourceRange.levelCount = 1;
    back_to_present.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        cap_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &back_to_present);
    check_vk_result(vkEndCommandBuffer(cap_cmd));

    VkSubmitInfo cap_submit{};
    cap_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    cap_submit.commandBufferCount = 1;
    cap_submit.pCommandBuffers = &cap_cmd;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        check_vk_result(vkQueueSubmit(g_Queue, 1, &cap_submit, VK_NULL_HANDLE));
    }
    check_vk_result(vkQueueWaitIdle(g_Queue));

    std::string out_path;
    std::string capture_err;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_Device, staging, &req);
    void* mapped = nullptr;
    check_vk_result(vkMapMemory(g_Device, staging_mem, 0, req.size, 0, &mapped));
    const fs::path shot_dir = fs::current_path() / "screenshot";
    std::error_code ec;
    fs::create_directories(shot_dir, ec);
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    fs::path out_file = shot_dir / ("shot_" + std::to_string(ts) + "_" + std::to_string(output_width) + "x" + std::to_string(output_height) + ".ppm");
    const bool ok = writePpmRgbResized(
        out_file,
        static_cast<const uint8_t*>(mapped),
        width,
        height,
        (size_t)width * 4,
        wd->SurfaceFormat.format,
        output_width,
        output_height,
        capture_err);
    vkUnmapMemory(g_Device, staging_mem);
    if (ok) out_path = out_file.string();

    vkFreeCommandBuffers(g_Device, cap_pool, 1, &cap_cmd);
    vkDestroyCommandPool(g_Device, cap_pool, g_Allocator);
    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);

    complete_screenshot(
        ok,
        out_path,
        capture_err,
        width,
        height,
        logical_width,
        logical_height,
        output_width,
        output_height,
        framebuffer_scale.x,
        framebuffer_scale.y);
}

void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild || g_VulkanDeviceLost.load(std::memory_order_relaxed)) return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        err = vkQueuePresentKHR(g_Queue, &info);
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    if (!check_vk_result_allow_device_loss(err)) return;
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
    g_PresentedFrameSerial.fetch_add(1, std::memory_order_relaxed);
}

void FrameRenderSecondary(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data, bool& swapchain_rebuild) {
    if (g_VulkanDeviceLost.load(std::memory_order_relaxed)) return;
    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        swapchain_rebuild = true;
        return;
    }
    if (!check_vk_result_allow_device_loss(err)) return;

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    if (!check_vk_result_allow_device_loss(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX))) return;
    if (!check_vk_result_allow_device_loss(vkResetFences(g_Device, 1, &fd->Fence))) return;
    if (!check_vk_result_allow_device_loss(vkResetCommandPool(g_Device, fd->CommandPool, 0))) return;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check_vk_result_allow_device_loss(vkBeginCommandBuffer(fd->CommandBuffer, &begin))) return;

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = wd->RenderPass;
    rp.framebuffer = fd->Framebuffer;
    rp.renderArea.extent.width = wd->Width;
    rp.renderArea.extent.height = wd->Height;
    rp.clearValueCount = 1;
    rp.pClearValues = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);
    if (!check_vk_result_allow_device_loss(vkEndCommandBuffer(fd->CommandBuffer))) return;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_acquired_semaphore;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &fd->CommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_complete_semaphore;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        if (!check_vk_result_allow_device_loss(vkQueueSubmit(g_Queue, 1, &submit, fd->Fence))) return;
    }
}

void FramePresentSecondary(ImGui_ImplVulkanH_Window* wd, bool& swapchain_rebuild) {
    if (swapchain_rebuild || g_VulkanDeviceLost.load(std::memory_order_relaxed)) return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        err = vkQueuePresentKHR(g_Queue, &info);
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        swapchain_rebuild = true;
        return;
    }
    if (!check_vk_result_allow_device_loss(err)) return;
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}
