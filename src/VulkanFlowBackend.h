#pragma once

#include "FlowSolver.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace rocket {

class VulkanFlowBackend {
public:
    VulkanFlowBackend(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, uint32_t queueFamily,
                      int width, int height, float dx, float dy, float xMin, float yMin);
    ~VulkanFlowBackend();
    VulkanFlowBackend(const VulkanFlowBackend&) = delete;
    VulkanFlowBackend& operator=(const VulkanFlowBackend&) = delete;

    void upload(const std::vector<float>& state, const std::vector<uint32_t>& mask);
    float advance(int steps, const Parameters& parameters);
    void download(std::vector<float>& state);

private:
    struct Buffer { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; void* mapped=nullptr; };
    VkPhysicalDevice physicalDevice_=VK_NULL_HANDLE;
    VkDevice device_=VK_NULL_HANDLE;
    VkQueue queue_=VK_NULL_HANDLE;
    int width_=0, height_=0;
    float dx_=0, dy_=0, xMin_=0, yMin_=0;
    Buffer stateA_, stateB_, stateBase_, mask_;
    VkDescriptorPool descriptorPool_=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_=VK_NULL_HANDLE;
    VkDescriptorSet stage1Set_=VK_NULL_HANDLE, stage2Set_=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkCommandPool commandPool_=VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_=VK_NULL_HANDLE;
    VkFence fence_=VK_NULL_HANDLE;

    [[nodiscard]] uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void writeSet(VkDescriptorSet set, VkBuffer input, VkBuffer output, VkBuffer base);
    void wait();
};

} // namespace rocket
