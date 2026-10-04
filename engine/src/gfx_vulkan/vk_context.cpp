#include "vk_context.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sstream>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

namespace gfx::vk {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
namespace {
std::string quote(const char* text) {
    std::string out = "\"";
    for (auto p = text; *p; ++p) {
        if (*p == '"' || *p == '\\') out += '\\';
        if (static_cast<unsigned char>(*p) >= 32) out += *p;
    }
    return out + '"';
}
VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* context = static_cast<Context*>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        context->validationErrors.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
        char message[8192];std::snprintf(message,sizeof(message),"Vulkan validation: %s\n",data->pMessage);
        OutputDebugStringA(message);
    }
    return VK_FALSE;
}
bool hasExtension(Dispatch& api, VkPhysicalDevice device, const char* name) {
    unsigned count = 0;
    check(api.EnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr), "enumerate device extensions");
    std::vector<VkExtensionProperties> extensions(count);
    check(api.EnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()), "enumerate device extensions");
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& e) { return !std::strcmp(e.extensionName, name); });
}
}
Context::Context(const Config& config) {
    try {
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) throw std::runtime_error(sizeof(void*) == 8 ?
            "The system's 64-bit Vulkan loader is unavailable" :
            "The system's 32-bit Vulkan loader is unavailable");
        api.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!api.GetInstanceProcAddr) throw std::runtime_error("vkGetInstanceProcAddr is unavailable");
#define GFX_VK_GLOBAL(n) api.n = reinterpret_cast<PFN_vk##n>(api.GetInstanceProcAddr(nullptr, "vk" #n));
        GFX_VK_GLOBAL(CreateInstance)
        GFX_VK_GLOBAL(EnumerateInstanceLayerProperties)
        GFX_VK_GLOBAL(EnumerateInstanceVersion)
#undef GFX_VK_GLOBAL
        unsigned version = VK_API_VERSION_1_0;
        if (api.EnumerateInstanceVersion) check(api.EnumerateInstanceVersion(&version), "loader version");
        if (version < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan 1.3 is required");
        if (config.validation) {
            unsigned count = 0;
            check(api.EnumerateInstanceLayerProperties(&count, nullptr), "enumerate validation layers");
            std::vector<VkLayerProperties> layers(count);
            check(api.EnumerateInstanceLayerProperties(&count, layers.data()), "enumerate validation layers");
            validationEnabled = std::any_of(layers.begin(), layers.end(), [](const auto& l) { return !std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation"); });
            if (!validationEnabled) throw std::runtime_error("Requested Vulkan validation layer is unavailable; validation cannot be claimed");
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "cod4x1khz"; app.applicationVersion = 1;
        app.pEngineName = "KisakCOD native Vulkan"; app.engineVersion = 1; app.apiVersion = VK_API_VERSION_1_3;
        const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        const char* layer = "VK_LAYER_KHRONOS_validation";
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = validationEnabled ? 4 : 2; info.ppEnabledExtensionNames = extensions;
        info.enabledLayerCount = validationEnabled ? 1 : 0; info.ppEnabledLayerNames = &layer;
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = validation; debugInfo.pUserData = this;
        const VkValidationFeatureEnableEXT validationFeature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validationSettings{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validationSettings.enabledValidationFeatureCount = 1; validationSettings.pEnabledValidationFeatures = &validationFeature;
        validationSettings.pNext = &debugInfo; if (validationEnabled) info.pNext = &validationSettings;
        check(api.CreateInstance(&info, nullptr, &instance), "create Vulkan instance");
#define GFX_VK_LOAD_INSTANCE(n) api.n = reinterpret_cast<PFN_vk##n>(api.GetInstanceProcAddr(instance, "vk" #n));
        GFX_VK_INSTANCE_FUNCTIONS(GFX_VK_LOAD_INSTANCE)
#undef GFX_VK_LOAD_INSTANCE
        if (validationEnabled) {
            check(api.CreateDebugUtilsMessengerEXT(instance, &debugInfo, nullptr, &debug), "create validation messenger");
        }
        if (config.window) {
            VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            surfaceInfo.hinstance = GetModuleHandleW(nullptr); surfaceInfo.hwnd = config.window;
            check(api.CreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface), "create Win32 Vulkan surface");
        }
        unsigned count = 0;
        check(api.EnumeratePhysicalDevices(instance, &count, nullptr), "enumerate Vulkan GPUs");
        std::vector<VkPhysicalDevice> physical(count);
        check(api.EnumeratePhysicalDevices(instance, &count, physical.data()), "enumerate Vulkan GPUs");
        for (auto p : physical) {
            DeviceInfo candidate; candidate.device = p;
            api.GetPhysicalDeviceProperties(p, &candidate.properties);
            api.GetPhysicalDeviceMemoryProperties(p, &candidate.memory);
            for (unsigned h = 0; h < candidate.memory.memoryHeapCount; ++h)
                if (candidate.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) candidate.localBytes += candidate.memory.memoryHeaps[h].size;
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceDepthBiasControlFeaturesEXT bias{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT};
            if (hasExtension(api, p, VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME)) f13.pNext = &bias;
            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            f12.pNext = &f13;
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext = &f12;
            api.GetPhysicalDeviceFeatures2(p, &f2); candidate.features = f2.features;
            candidate.depthBiasFloat = bias.depthBiasControl && bias.floatRepresentation;
            candidate.depthBiasUnorm = bias.depthBiasControl && bias.leastRepresentableValueForceUnormRepresentation;
            candidate.depthBiasExact = (candidate.depthBiasFloat || candidate.depthBiasUnorm) && bias.depthBiasExact;
            unsigned queues = 0; api.GetPhysicalDeviceQueueFamilyProperties(p, &queues, nullptr);
            std::vector<VkQueueFamilyProperties> properties(queues); api.GetPhysicalDeviceQueueFamilyProperties(p, &queues, properties.data());
            for (unsigned i = 0; i < queues; ++i) {
                VkBool32 present = VK_TRUE;
                if (surface) check(api.GetPhysicalDeviceSurfaceSupportKHR(p, i, surface, &present), "query presentation support");
                if ((properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present && properties[i].queueCount) { candidate.queueFamily = i; break; }
            }
            if (candidate.properties.apiVersion < VK_API_VERSION_1_3) candidate.reason = "Vulkan 1.3 is required";
            else if (!f12.timelineSemaphore || !f13.synchronization2 || !f13.dynamicRendering) candidate.reason = "timeline semaphores, synchronization2 and dynamic rendering are required";
            else if (candidate.queueFamily == UINT32_MAX) candidate.reason = "no graphics queue supporting this window";
            else if (surface && !hasExtension(api, p, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) candidate.reason = "swapchains are unavailable";
            else candidate.compatible = true;
            devices.push_back(candidate);
        }
        int selectedIndex = config.deviceIndex;
        if (selectedIndex < 0) {
            auto score = [](const DeviceInfo& d) { return std::pair(d.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : d.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0, d.localBytes); };
            for (unsigned i = 0; i < devices.size(); ++i) if (devices[i].compatible &&
                (selectedIndex < 0 || score(devices[i]) > score(devices[selectedIndex]))) selectedIndex = int(i);
        }
        if (selectedIndex < 0 || unsigned(selectedIndex) >= devices.size()) throw std::runtime_error("No compatible Vulkan GPU was selected");
        selected = devices[selectedIndex];
        if (!selected.compatible) throw std::runtime_error(selected.reason);
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = selected.queueFamily; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enable13.dynamicRendering = VK_TRUE; enable13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceDepthBiasControlFeaturesEXT enableBias{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT};
        if (selected.depthBiasFloat || selected.depthBiasUnorm) {
            enableBias.depthBiasControl = VK_TRUE;
            enableBias.floatRepresentation = selected.depthBiasFloat;
            enableBias.leastRepresentableValueForceUnormRepresentation = selected.depthBiasUnorm;
            enableBias.depthBiasExact = selected.depthBiasExact;
            enable13.pNext = &enableBias;
        }
        VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enable12.timelineSemaphore = VK_TRUE; enable12.pNext = &enable13;
        VkPhysicalDeviceFeatures features{};
        features.samplerAnisotropy = selected.features.samplerAnisotropy;
        features.fillModeNonSolid = selected.features.fillModeNonSolid;
        features.depthClamp = selected.features.depthClamp;
        features.shaderClipDistance = selected.features.shaderClipDistance;
        features.occlusionQueryPrecise = selected.features.occlusionQueryPrecise;
        features.sampleRateShading = selected.features.sampleRateShading;
        features.textureCompressionBC = selected.features.textureCompressionBC;
        std::vector<const char*> deviceExtensions;
        if (surface) deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if (selected.depthBiasFloat || selected.depthBiasUnorm) deviceExtensions.push_back(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME);
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &enable12; deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.pEnabledFeatures = &features; deviceInfo.enabledExtensionCount = unsigned(deviceExtensions.size()); deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        check(api.CreateDevice(selected.device, &deviceInfo, nullptr, &device), "create Vulkan device");
#define GFX_VK_LOAD_DEVICE(n) api.n = reinterpret_cast<PFN_vk##n>(api.GetDeviceProcAddr(device, "vk" #n));
        GFX_VK_DEVICE_FUNCTIONS(GFX_VK_LOAD_DEVICE)
#undef GFX_VK_LOAD_DEVICE
        api.GetDeviceQueue(device, selected.queueFamily, 0, &queue);
        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = api.GetInstanceProcAddr; functions.vkGetDeviceProcAddr = api.GetDeviceProcAddr;
        VmaAllocatorCreateInfo allocInfo{};
        allocInfo.vulkanApiVersion = VK_API_VERSION_1_3; allocInfo.physicalDevice = selected.device;
        allocInfo.device = device; allocInfo.instance = instance; allocInfo.pVulkanFunctions = &functions;
        check(vmaCreateAllocator(&allocInfo, &allocator), "create Vulkan memory allocator");
    } catch (...) { cleanup(); throw; }
}
Context::~Context() { cleanup(); }
void Context::cleanup() noexcept {
    if (device && api.DeviceWaitIdle) api.DeviceWaitIdle(device);
    if (allocator) vmaDestroyAllocator(allocator);
    if (device && api.DestroyDevice) api.DestroyDevice(device, nullptr);
    if (surface && api.DestroySurfaceKHR) api.DestroySurfaceKHR(instance, surface, nullptr);
    if (debug && api.DestroyDebugUtilsMessengerEXT) api.DestroyDebugUtilsMessengerEXT(instance, debug, nullptr);
    if (instance && api.DestroyInstance) api.DestroyInstance(instance, nullptr);
    if (loader) FreeLibrary(loader);
    allocator = VK_NULL_HANDLE; device = VK_NULL_HANDLE; surface = VK_NULL_HANDLE; instance = VK_NULL_HANDLE; loader = nullptr;
}
VkFormat Context::depthFormat() const {
    for (auto format : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkFormatProperties props{}; api.GetPhysicalDeviceFormatProperties(selected.device, format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return format;
    }
    throw std::runtime_error("No supported stock depth/stencil format");
}
std::string Context::json() const {
    std::ostringstream out;
    out << "{\"process_bits\":" << sizeof(void*) * 8 << ",\"api\":\"native Vulkan 1.3\",\"validation_enabled\":"
        << (validationEnabled ? "true" : "false") << ",\"synchronization_validation\":" << (validationEnabled ? "true" : "false")
        << ",\"validation_errors\":" << validationErrors.load(std::memory_order_relaxed) << ",\"devices\":[";
    for (unsigned i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i]; if (i) out << ',';
        out << "{\"index\":" << i << ",\"name\":" << quote(d.properties.deviceName) << ",\"version\":\""
            << VK_API_VERSION_MAJOR(d.properties.apiVersion) << '.' << VK_API_VERSION_MINOR(d.properties.apiVersion) << '.' << VK_API_VERSION_PATCH(d.properties.apiVersion)
            << "\",\"driver_version_raw\":" << d.properties.driverVersion << ",\"device_type\":" << d.properties.deviceType
            << ",\"local_bytes\":" << d.localBytes << ",\"compatible\":" << (d.compatible ? "true" : "false")
            << ",\"depth_bias_float\":" << (d.depthBiasFloat ? "true" : "false") << ",\"depth_bias_unorm\":" << (d.depthBiasUnorm ? "true" : "false") << ",\"depth_bias_exact\":" << (d.depthBiasExact ? "true" : "false")
            << ",\"selected\":" << (d.device == selected.device ? "true" : "false") << ",\"reason\":" << quote(d.reason.c_str()) << '}';
    }
    out << "]}"; return out.str();
}
}
