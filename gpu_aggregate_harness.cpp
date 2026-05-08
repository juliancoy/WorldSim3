#include "heatmap_gpu_aggregate.h"
#include "worldsim_app_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

VkAllocationCallbacks* g_Allocator = nullptr;
VkInstance g_Instance = VK_NULL_HANDLE;
VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
VkDevice g_Device = VK_NULL_HANDLE;
uint32_t g_QueueFamily = (uint32_t)-1;
VkQueue g_Queue = VK_NULL_HANDLE;
std::mutex g_QueueSubmitMutex;
VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
VkSampler g_TileSampler = VK_NULL_HANDLE;
ImGui_ImplVulkanH_Window g_MainWindowData{};
int g_MinImageCount = 2;
bool g_SwapChainRebuild = false;
std::unordered_map<std::string, TileCacheEntry> g_TileCache;
std::list<std::string> g_TileLRU;
bool g_EnableValidationLayers = false;
std::vector<TileTexture> g_RetiredTextures;
ScreenshotRequestState g_ScreenshotState;

void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) return;
    std::fprintf(stderr, "[harness][vulkan] VkResult=%d\n", (int)err);
    std::abort();
}

void SetupVulkanWindow(ImGui_ImplVulkanH_Window*, VkSurfaceKHR, int, int) {}
void CleanupVulkanWindow() {}
void FrameRender(ImGui_ImplVulkanH_Window*, ImDrawData*) {}
void FramePresent(ImGui_ImplVulkanH_Window*) {}
void FrameRenderSecondary(ImGui_ImplVulkanH_Window*, ImDrawData*, bool&) {}
void FramePresentSecondary(ImGui_ImplVulkanH_Window*, bool&) {}
void drainRetiredTextures(bool) {}
void destroyTileTexture(TileTexture&) {}
void destroyTileTextureNow(TileTexture&) {}
bool uploadRgbaTexture(const unsigned char*, uint32_t, uint32_t, TileTexture&) { return false; }
TileSample getTileSample(const std::filesystem::path&, const std::string&, int, int, int) { return {}; }
const std::vector<std::vector<ImVec2>>& getTopoVectorLines(const std::filesystem::path&) {
    static const std::vector<std::vector<ImVec2>> empty;
    return empty;
}

static void setup_vulkan_minimal() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "ws3-gpu-aggregate-harness";
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app_info;
    check_vk_result(vkCreateInstance(&ici, g_Allocator, &g_Instance));

    uint32_t gpu_count = 0;
    check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpu_count, nullptr));
    if (gpu_count == 0) {
        std::fprintf(stderr, "[harness] No Vulkan physical devices found\n");
        std::abort();
    }
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpu_count, gpus.data()));
    g_PhysicalDevice = gpus[0];

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &qf_count, qfs.data());
    for (uint32_t i = 0; i < qf_count; ++i) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_QueueFamily = i;
            break;
        }
    }
    if (g_QueueFamily == (uint32_t)-1) {
        std::fprintf(stderr, "[harness] No graphics queue family found\n");
        std::abort();
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_QueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    check_vk_result(vkCreateDevice(g_PhysicalDevice, &dci, g_Allocator, &g_Device));
    vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
}

void SetupVulkan(const char**, uint32_t) { setup_vulkan_minimal(); }

void CleanupVulkan() {
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
        vkDestroyDevice(g_Device, g_Allocator);
        g_Device = VK_NULL_HANDLE;
    }
    if (g_Instance != VK_NULL_HANDLE) {
        vkDestroyInstance(g_Instance, g_Allocator);
        g_Instance = VK_NULL_HANDLE;
    }
}

int main() {
#if defined(__linux__)
    setenv("WS3_ENABLE_EXPERIMENTAL_GPU_AGGREGATE", "1", 1);
#endif

    setup_vulkan_minimal();

    constexpr int rw = 512;
    constexpr int rh = 512;
    std::vector<HeatSample> samples;
    samples.reserve(20000);
    for (int i = 0; i < 20000; ++i) {
        const float t = (float)i / 20000.0f;
        HeatSample s;
        s.lon = -76.8f + 0.4f * t;
        s.lat = 39.1f + 0.35f * std::fabs(std::sin(t * 31.4159f));
        s.color = ImVec4(0.2f + 0.8f * t, 0.3f, 0.9f - 0.6f * t, 1.0f);
        s.prefer_gradient = true;
        s.algo = 2;
        samples.push_back(s);
    }

    std::vector<float> density, cr, cg, cb, cw, gv, sv;
    std::string err;
    const bool ok = buildGpuSplatAggregate(
        samples,
        rw,
        rh,
        -76.9f,
        39.0f,
        -76.3f,
        39.6f,
        6.0f,
        density,
        cr,
        cg,
        cb,
        cw,
        gv,
        sv,
        &err);

    if (!ok) {
        std::fprintf(stderr, "[harness] GPU aggregate FAILED: %s\n", err.c_str());
        CleanupVulkan();
        return 2;
    }

    double sum_density = 0.0;
    double max_density = 0.0;
    for (float d : density) {
        sum_density += d;
        max_density = std::max(max_density, (double)d);
    }
    std::printf("[harness] GPU aggregate OK\n");
    std::printf("[harness] output pixels=%zu sum_density=%.3f max_density=%.3f\n", density.size(), sum_density, max_density);

    CleanupVulkan();
    return 0;
}
