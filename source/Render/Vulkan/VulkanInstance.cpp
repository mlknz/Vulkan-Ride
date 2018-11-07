#include "VulkanInstance.hpp"

#include "source/Render/Vulkan/Utils.hpp"

#include <cstring>

using namespace Ride;

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
    VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT objType,
    uint64_t obj,
    size_t location,
    int32_t code,
    const char* layerPrefix,
    const char* msg,
    void* userData)
{
    printf("VULKAN VALIDATION: %s\n", msg);
    return VK_FALSE;
}

VkResult CreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT* pCreateInfo,
                                      const VkAllocationCallbacks* pAllocator, VkDebugReportCallbackEXT* pCallback) {
    auto func = (PFN_vkCreateDebugReportCallbackEXT) vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pCallback);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

bool VulkanInstance::SetupDebugCallback() {
    if (!enableValidationLayers) return true;

    VkDebugReportCallbackCreateInfoEXT createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    createInfo.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;
    createInfo.pfnCallback = VulkanDebugCallback;

    if (CreateDebugReportCallbackEXT(instance, &createInfo, nullptr, &vkDebugCallback) != VK_SUCCESS) {
        printf("failed to set up debug callback!");
        return false;
    }

    return true;
}

VulkanInstance::VulkanInstance()
{
    ready = CreateVulkanInstance();
}

void DestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT callback, const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugReportCallbackEXT) vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT");
    if (func != nullptr) {
        func(instance, callback, pAllocator);
    }
}

VulkanInstance::~VulkanInstance()
{
    if (enableValidationLayers && vkDebugCallback)
    {
        DestroyDebugReportCallbackEXT(instance, vkDebugCallback, 0);
    }

    vkDestroyInstance(instance, 0);
}

bool VulkanInstance::CreateVulkanInstance()
{
    if (enableValidationLayers && !CheckValidationLayerSupport()) {
        printf("Error: validation layers requested, but not available!");
        return false;
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan Ride";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    // printf("Found %d extensions:\n", extensionCount);

    std::vector<VkExtensionProperties> extensionsProps(extensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensionsProps.data());
    for (const auto& extension : extensionsProps) {
        supportedExtensions.push_back(extension.extensionName);
        // std::cout << "\t" << extension.extensionName << std::endl;
    }

    // todo: clean up asking for extensions
    /*
    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XCB_SURFACE_EXTENSION_NAME
    };

    if (enableValidationLayers) {
        extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    } */

    /*uint32_t extensionCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, nullptr))
    {
        printf("Can't get vulkan extension count");
        return false;
    }

    std::vector<const char*> supportedExtensions = {};
    SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, &supportedExtensions[0]);
    if (enableValidationLayers) {
        supportedExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    } */

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = supportedExtensions.size();
    createInfo.ppEnabledExtensionNames = supportedExtensions.data();

    if (enableValidationLayers) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
    } else {
        createInfo.enabledLayerCount = 0;
    }

    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
    {
        printf("Can't create vk instance");
        return false;
    }

    if (!SetupDebugCallback())
    {
        return false;
    }

    return true;
}

bool VulkanInstance::CheckValidationLayerSupport() {
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : validationLayers) {
        bool layerFound = false;

        for (const auto& layerProperties : availableLayers) {
            if (strcmp(layerName, layerProperties.layerName) == 0) {
                layerFound = true;
                break;
            }
        }

        if (!layerFound) {
            return false;
        }
    }

    return true;
}