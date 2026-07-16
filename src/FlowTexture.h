#pragma once

#include "FlowSolver.h"

#include "imgui.h"
#include <vulkan/vulkan.h>

namespace rocket {

// Streams the CPU solver field into one filtered GPU image. Dear ImGui then
// emits a single textured quad instead of one draw primitive per CFD cell.
class FlowTexture {
public:
    FlowTexture(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue,
                uint32_t queueFamily, int width, int height);
    ~FlowTexture();
    FlowTexture(const FlowTexture&) = delete;
    FlowTexture& operator=(const FlowTexture&) = delete;

    void update(const FlowSolver& solver, FieldView view);
    [[nodiscard]] ImTextureID textureId() const;

private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    int width_ = 0;
    int height_ = 0;
    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    unsigned char* mapped_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;
    bool initialized_ = false;

    [[nodiscard]] uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
};

} // namespace rocket
