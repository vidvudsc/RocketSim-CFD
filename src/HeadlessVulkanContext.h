#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace rocket {

class HeadlessVulkanContext {
public:
    HeadlessVulkanContext();
    ~HeadlessVulkanContext();
    HeadlessVulkanContext(const HeadlessVulkanContext&) = delete;
    HeadlessVulkanContext& operator=(const HeadlessVulkanContext&) = delete;
    [[nodiscard]] VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    [[nodiscard]] VkQueue queue() const { return queue_; }
    [[nodiscard]] uint32_t queueFamily() const { return queueFamily_; }
private:
    VkInstance instance_=VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_=VK_NULL_HANDLE;
    VkDevice device_=VK_NULL_HANDLE;
    VkQueue queue_=VK_NULL_HANDLE;
    uint32_t queueFamily_=0;
};

} // namespace rocket
