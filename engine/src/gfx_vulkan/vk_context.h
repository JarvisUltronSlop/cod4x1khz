#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <Windows.h>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

namespace gfx::vk {
#define GFX_VK_INSTANCE_FUNCTIONS(X) \
 X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceMemoryProperties) \
 X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFormatProperties) \
 X(EnumerateDeviceExtensionProperties) X(CreateDevice) X(GetDeviceProcAddr) \
 X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) \
 X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR) \
 X(GetPhysicalDeviceSurfacePresentModesKHR) X(CreateDebugUtilsMessengerEXT) X(DestroyDebugUtilsMessengerEXT)
#define GFX_VK_DEVICE_FUNCTIONS(X) \
 X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) X(QueueWaitIdle) X(QueueSubmit2) \
 X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) \
 X(CreateSemaphore) X(DestroySemaphore) X(GetSemaphoreCounterValue) X(WaitSemaphores) \
 X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(GetFenceStatus) \
 X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) X(AllocateCommandBuffers) \
 X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier2) X(CmdCopyBuffer) X(CmdCopyBufferToImage) \
 X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdBlitImage) X(CmdResolveImage) \
 X(CmdClearColorImage) X(CmdClearDepthStencilImage) X(CmdBeginRendering) X(CmdEndRendering) \
 X(CmdClearAttachments) X(CmdSetViewport) X(CmdSetScissor) X(CmdSetDepthBias) X(CmdSetBlendConstants) \
 X(CmdSetStencilReference) X(CmdBindPipeline) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) \
 X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDraw) X(CmdDrawIndexed) X(CmdExecuteCommands) \
 X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
 X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
 X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreatePipelineCache) X(DestroyPipelineCache) \
 X(GetPipelineCacheData) X(CreateGraphicsPipelines) X(DestroyPipeline) \
 X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
 X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(CmdWriteTimestamp2)
struct Dispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
    PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
#define GFX_VK_DECLARE(n) PFN_vk##n n = nullptr;
    GFX_VK_INSTANCE_FUNCTIONS(GFX_VK_DECLARE)
    GFX_VK_DEVICE_FUNCTIONS(GFX_VK_DECLARE)
#undef GFX_VK_DECLARE
};
struct Config {
    HWND window = nullptr;
    int deviceIndex = -1;
    bool validation = false;
};
struct DeviceInfo {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceFeatures features{};
    VkDeviceSize localBytes = 0;
    unsigned queueFamily = UINT32_MAX;
    bool compatible = false;
    bool depthBiasFloat = false;
    bool depthBiasUnorm = false;
    bool depthBiasExact = false;
    std::string reason;
};
void check(VkResult result, const char* operation);
class Context {
public:
    explicit Context(const Config& config);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Dispatch api;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug = VK_NULL_HANDLE;
    DeviceInfo selected;
    std::vector<DeviceInfo> devices;
    std::atomic<unsigned> validationErrors{0};
    bool validationEnabled = false;
    std::string json() const;
    VkFormat depthFormat() const;
private:
    HMODULE loader = nullptr;
    void cleanup() noexcept;
};
}
