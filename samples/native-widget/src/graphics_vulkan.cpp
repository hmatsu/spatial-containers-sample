// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Vulkan backend (XR_KHR_vulkan_enable2): copies the panel image into the swapchain
// with a staging buffer. No pipeline or shaders are needed.

#if defined(WIDGET_HAS_VULKAN)

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>

#include "graphics.h"

namespace {

#define CHECK_VK(call)                                                                             \
    do {                                                                                           \
        const VkResult vkResult_ = (call);                                                         \
        if (vkResult_ != VK_SUCCESS) {                                                             \
            std::fprintf(stderr, "%s failed: %d (%s:%d)\n", #call, vkResult_, __FILE__, __LINE__); \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_XR(call)                                                                             \
    do {                                                                                           \
        const XrResult xrResult_ = (call);                                                         \
        if (XR_FAILED(xrResult_)) {                                                                \
            std::fprintf(stderr, "%s failed: %d (%s:%d)\n", #call, xrResult_, __FILE__, __LINE__); \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

class VulkanGraphics : public Graphics {
public:
    ~VulkanGraphics() override {
        ReleaseSwapchain();
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
            if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    }

    std::vector<const char*> RequiredExtensions() const override { return {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME}; }

    bool Initialize(XrInstance instance, XrSystemId systemId) override {
        PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
        PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
        PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
        PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
        CHECK_XR(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
        CHECK_XR(xrGetInstanceProcAddr(instance, "xrCreateVulkanInstanceKHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&createInstance)));
        CHECK_XR(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDevice2KHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&getDevice)));
        CHECK_XR(xrGetInstanceProcAddr(instance, "xrCreateVulkanDeviceKHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&createDevice)));

        // Required before creating the session, even though Vulkan 1.1 is enough here.
        XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        CHECK_XR(getRequirements(instance, systemId, &requirements));

        VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.pApplicationName = "spatial-container-native-widget";
        appInfo.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &appInfo;
        XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        xrInstanceInfo.systemId = systemId;
        xrInstanceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
        VkResult vkResult = VK_SUCCESS;
        CHECK_XR(createInstance(instance, &xrInstanceInfo, &instance_, &vkResult));
        CHECK_VK(vkResult);

        XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        deviceGetInfo.systemId = systemId;
        deviceGetInfo.vulkanInstance = instance_;
        CHECK_XR(getDevice(instance, &deviceGetInfo, &physicalDevice_));

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
        queueFamily_ = familyCount;
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queueFamily_ = i;
                break;
            }
        }
        if (queueFamily_ == familyCount) return false;

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        xrDeviceInfo.systemId = systemId;
        xrDeviceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xrDeviceInfo.vulkanPhysicalDevice = physicalDevice_;
        xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
        CHECK_XR(createDevice(instance, &xrDeviceInfo, &device_, &vkResult));
        CHECK_VK(vkResult);
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        CHECK_VK(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_));
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        CHECK_VK(vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer_));
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CHECK_VK(vkCreateFence(device_, &fenceInfo, nullptr, &fence_));

        binding_.instance = instance_;
        binding_.physicalDevice = physicalDevice_;
        binding_.device = device_;
        binding_.queueFamilyIndex = queueFamily_;
        binding_.queueIndex = 0;
        return true;
    }

    const void* SessionBinding() const override { return &binding_; }

    bool CreateSwapchain(XrSession session, int width, int height) override {
        uint32_t count = 0;
        CHECK_XR(xrEnumerateSwapchainFormats(session, 0, &count, nullptr));
        std::vector<int64_t> formats(count);
        CHECK_XR(xrEnumerateSwapchainFormats(session, count, &count, formats.data()));
        const SwapchainFormat chosen = ChooseSwapchainFormat(formats, {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM},
                                                             {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM});
        if (chosen.format == 0) {
            std::fprintf(stderr, "Vulkan: no 8-bit RGBA or BGRA swapchain format\n");
            return false;
        }
        bgra_ = chosen.bgra;

        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags =
            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = chosen.format;
        info.sampleCount = 1;
        info.width = static_cast<uint32_t>(width);
        info.height = static_cast<uint32_t>(height);
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        CHECK_XR(xrCreateSwapchain(session, &info, &swapchain_));
        CHECK_XR(xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr));
        images_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        CHECK_XR(xrEnumerateSwapchainImages(swapchain_, count, &count,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data())));
        width_ = width;
        height_ = height;
        return CreateStagingBuffer(static_cast<VkDeviceSize>(width) * height * 4);
    }

    XrSwapchain Swapchain() const override { return swapchain_; }

    bool Upload(const Image& image) override {
        if (swapchain_ == XR_NULL_HANDLE || image.width() != width_ || image.height() != height_) return false;
        CopyPixels(image, bgra_, static_cast<uint8_t*>(mapped_), image.rowPitch());

        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        CHECK_XR(xrAcquireSwapchainImage(swapchain_, &acquireInfo, &index));
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        CHECK_XR(xrWaitSwapchainImage(swapchain_, &waitInfo));

        const VkImage target = images_[index].image;
        CHECK_VK(vkResetCommandBuffer(commandBuffer_, 0));
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK_VK(vkBeginCommandBuffer(commandBuffer_, &beginInfo));
        Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
        vkCmdCopyBufferToImage(commandBuffer_, staging_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        // The runtime expects released color images in COLOR_ATTACHMENT_OPTIMAL.
        Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        CHECK_VK(vkEndCommandBuffer(commandBuffer_));

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        CHECK_VK(vkResetFences(device_, 1, &fence_));
        CHECK_VK(vkQueueSubmit(queue_, 1, &submit, fence_));
        CHECK_VK(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX));

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        CHECK_XR(xrReleaseSwapchainImage(swapchain_, &releaseInfo));
        return true;
    }

    void DestroySwapchain() override { ReleaseSwapchain(); }

private:
    // Not virtual, so the destructor can call it.
    void ReleaseSwapchain() {
        if (swapchain_ != XR_NULL_HANDLE) xrDestroySwapchain(swapchain_);
        swapchain_ = XR_NULL_HANDLE;
        images_.clear();
        if (device_ != VK_NULL_HANDLE) {
            if (staging_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, staging_, nullptr);
            if (stagingMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, stagingMemory_, nullptr);
        }
        staging_ = VK_NULL_HANDLE;
        stagingMemory_ = VK_NULL_HANDLE;
        mapped_ = nullptr;
    }

    bool CreateStagingBuffer(VkDeviceSize size) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        CHECK_VK(vkCreateBuffer(device_, &bufferInfo, nullptr, &staging_));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device_, staging_, &requirements);
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memory);
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = memory.memoryTypeCount;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i;
                break;
            }
        }
        if (type == memory.memoryTypeCount) return false;
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = type;
        CHECK_VK(vkAllocateMemory(device_, &allocInfo, nullptr, &stagingMemory_));
        CHECK_VK(vkBindBufferMemory(device_, staging_, stagingMemory_, 0));
        CHECK_VK(vkMapMemory(device_, stagingMemory_, 0, size, 0, &mapped_));
        return true;
    }

    void Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                 VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commandBuffer_, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    XrGraphicsBindingVulkan2KHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};

    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkan2KHR> images_;
    int width_ = 0, height_ = 0;
    bool bgra_ = false;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
};

}  // namespace

std::unique_ptr<Graphics> CreateVulkanGraphics() {
    return std::make_unique<VulkanGraphics>();
}

#endif  // WIDGET_HAS_VULKAN
