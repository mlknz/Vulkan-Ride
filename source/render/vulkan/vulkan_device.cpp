#include "vulkan_device.hpp"

#include <SDL_vulkan.h>

#include <map>
#include <set>
#include <string>

#include "core/log_assert.hpp"
#include "render/config.hpp"
#include "render/vulkan/utils.hpp"
#include "render/vulkan/vulkan_swapchain.hpp"

using namespace ez;

const std::vector<const char*> requiredDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

ResultValue<std::unique_ptr<VulkanDevice>> VulkanDevice::CreateVulkanDevice(
    vk::Instance instance)
{
    SDL_Window* window =
        SDL_CreateWindow("ELEKTROZARYA Vulkan C++ Sandbox",
                         SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED,
                         Config::WindowWidth,
                         Config::WindowHeight,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr)
    {
        EZLOG("Failed to create SDL window");
        return GraphicsResult::Error;
    }

    VkSurfaceKHR surfaceHandle;
    if (SDL_Vulkan_CreateSurface(window, instance.operator VkInstance(), &surfaceHandle) !=
        SDL_TRUE)
    {
        EZLOG("Failed to create SDL vulkan surface");
        return GraphicsResult::Error;
    }
    vk::SurfaceKHR surface(surfaceHandle);

    auto physicalDeviceRV = PickPhysicalDevice(instance, surface);
    if (physicalDeviceRV.result != GraphicsResult::Ok)
    {
        EZLOG("Failed to choose physical device");
        return physicalDeviceRV.result;
    }

    auto deviceRV = CreateDevice(physicalDeviceRV.value, surface);
    if (deviceRV.result != GraphicsResult::Ok)
    {
        EZLOG("Failed to create device");
        return deviceRV.result;
    }

    auto graphicsCommandPoolRV =
        CreateGraphicsCommandPool(physicalDeviceRV.value, deviceRV.value, surface);
    if (graphicsCommandPoolRV.result != GraphicsResult::Ok)
    {
        EZLOG("Failed to create graphics command pool");
        return deviceRV.result;
    }

    auto descriptorPoolRV = CreateDescriptorPool(deviceRV.value);
    if (descriptorPoolRV.result != GraphicsResult::Ok)
    {
        EZLOG("Failed to create descriptor pool");
        return deviceRV.result;
    }

    return { GraphicsResult::Ok,
             std::make_unique<VulkanDevice>(instance,
                                            physicalDeviceRV.value,
                                            deviceRV.value,
                                            graphicsCommandPoolRV.value,
                                            descriptorPoolRV.value,
                                            window,
                                            surface) };
}

VulkanDevice::VulkanDevice(vk::Instance aInstance,
                           vk::PhysicalDevice aPhysicalDevice,
                           vk::Device aDevice,
                           vk::CommandPool aGraphicsCommandPool,
                           vk::DescriptorPool aDescriptorPool,
                           SDL_Window* aWindow,
                           vk::SurfaceKHR aSurface)
    : instance(aInstance)
    , physicalDevice(aPhysicalDevice)
    , device(aDevice)
    , window(aWindow)
    , surface(aSurface)
    , graphicsCommandPool(aGraphicsCommandPool)
    , descriptorPool(aDescriptorPool)
{
    QueueFamilyIndices indices = FindQueueFamilies(
        physicalDevice,
        surface);  // todo: find once and use elsewhere instead of FindQueueFamilies calls

    device.getQueue(indices.graphicsFamily, 0, &graphicsQueue);
    device.getQueue(indices.presentFamily, 0, &presentQueue);
}

bool VulkanDevice::CheckDeviceExtensionSupport(vk::PhysicalDevice device)
{
    auto extensionsRV = device.enumerateDeviceExtensionProperties();
    if (extensionsRV.result != vk::Result::eSuccess) { return false; }
    const std::vector<vk::ExtensionProperties>& availableExtensions = extensionsRV.value;

    std::set<std::string> requiredExtensions(requiredDeviceExtensions.begin(),
                                             requiredDeviceExtensions.end());

    for (const auto& extension : availableExtensions)
    {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}

bool VulkanDevice::IsDeviceSuitable(vk::PhysicalDevice device, vk::SurfaceKHR surface)
{
    QueueFamilyIndices indices = FindQueueFamilies(device, surface);

    bool extensionsSupported = CheckDeviceExtensionSupport(device);

    bool swapChainAdequate = false;
    if (extensionsSupported)
    {
        auto swapChainSupport = VulkanSwapchain::QuerySwapchainSupport(device, surface);
        swapChainAdequate = swapChainSupport.result == GraphicsResult::Ok &&
                            !swapChainSupport.value.formats.empty() &&
                            !swapChainSupport.value.presentModes.empty();
    }

    vk::PhysicalDeviceFeatures supportedFeatures;
    device.getFeatures(&supportedFeatures);

    return indices.isComplete() && extensionsSupported && swapChainAdequate &&
           supportedFeatures.samplerAnisotropy;
}

ResultValue<vk::PhysicalDevice> VulkanDevice::PickPhysicalDevice(vk::Instance instance,
                                                                 vk::SurfaceKHR surface)
{
    auto devicesRV = instance.enumeratePhysicalDevices();
    if (devicesRV.result != vk::Result::eSuccess)
    {
        EZLOG("Failed to find GPUs with Vulkan support!");
        return GraphicsResult::Error;
    }
    const std::vector<vk::PhysicalDevice>& devices = devicesRV.value;

    vk::PhysicalDevice chosenDevice;
    for (const auto& device : devices)
    {
        if (IsDeviceSuitable(device, surface))
        {
            chosenDevice = device;
            break;
        }
    }

    if (!chosenDevice)
    {
        EZLOG("Failed to find a suitable GPU!");
        return GraphicsResult::Error;
    }
    return { GraphicsResult::Ok, chosenDevice };
}

ResultValue<vk::Device> VulkanDevice::CreateDevice(vk::PhysicalDevice physicalDevice,
                                                   vk::SurfaceKHR surface)
{
    QueueFamilyIndices indices = FindQueueFamilies(physicalDevice, surface);

    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
    std::set<int> uniqueQueueFamilies = { indices.graphicsFamily, indices.presentFamily };

    float queuePriority = 1.0f;
    for (int queueFamily : uniqueQueueFamilies)
    {
        vk::DeviceQueueCreateInfo queueCreateInfo = {};
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    vk::PhysicalDeviceFeatures deviceFeatures = {};
    deviceFeatures.samplerAnisotropy = VK_TRUE;  // request for anisotropy

    vk::DeviceCreateInfo createInfo = {};

    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &deviceFeatures;

    createInfo.enabledExtensionCount = static_cast<uint32_t>(requiredDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

    if (enableValidationLayers)
    {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
    }
    else
    {
        createInfo.enabledLayerCount = 0;
    }

    vk::Device device;
    if (physicalDevice.createDevice(&createInfo, nullptr, &device) != vk::Result::eSuccess)
    {
        EZLOG("Failed to create vk::device!");
        return GraphicsResult::Error;
    }

    return { GraphicsResult::Ok, device };
}

ResultValue<vk::CommandPool> VulkanDevice::CreateGraphicsCommandPool(
    vk::PhysicalDevice physicalDevice, vk::Device device, vk::SurfaceKHR surface)
{
    vk::CommandPool commandPool;
    ez::QueueFamilyIndices queueFamilyIndices = FindQueueFamilies(physicalDevice, surface);

    vk::CommandPoolCreateInfo poolInfo = {};
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily;
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
                     vk::CommandPoolCreateFlagBits::eTransient;

    if (device.createCommandPool(&poolInfo, nullptr, &commandPool) != vk::Result::eSuccess)
    {
        EZLOG("Failed to create graphics command pool!");
        return GraphicsResult::Error;
    }
    return { GraphicsResult::Ok, commandPool };
}

ResultValue<vk::DescriptorPool> VulkanDevice::CreateDescriptorPool(vk::Device device)
{
    vk::DescriptorPool descriptorPool;

    std::vector<vk::DescriptorPoolSize> poolSizes;
    for (auto& poolSizeConfig : Config::VulkanDescriptorPoolSizes)
    {
        poolSizes.emplace_back();
        poolSizes.back().setType(poolSizeConfig.first);
        poolSizes.back().descriptorCount = poolSizeConfig.second;
    }

    vk::DescriptorPoolCreateInfo poolInfo = {};
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = Config::MaxDescriptorSetsCount;

    if (device.createDescriptorPool(&poolInfo, nullptr, &descriptorPool) !=
        vk::Result::eSuccess)
    {
        EZLOG("Failed to create descriptor pool!");
        return GraphicsResult::Error;
    }
    return { GraphicsResult::Ok, descriptorPool };
}

VulkanDevice::~VulkanDevice()
{
    device.destroyCommandPool(graphicsCommandPool);
    device.destroyDescriptorPool(descriptorPool);

    device.destroy();
    instance.destroySurfaceKHR(surface);
    SDL_DestroyWindow(window);
}
