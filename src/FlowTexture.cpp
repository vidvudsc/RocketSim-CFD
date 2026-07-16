#include "FlowTexture.h"

#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rocket {
namespace {
void require(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(operation);
}
}

uint32_t FlowTexture::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("No compatible Vulkan memory type for flow texture");
}

FlowTexture::FlowTexture(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue,
                         uint32_t queueFamily, int width, int height)
    : physicalDevice_(physicalDevice), device_(device), queue_(queue), width_(width), height_(height) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width_) * height_ * 4;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    require(vkCreateBuffer(device_, &bufferInfo, nullptr, &stagingBuffer_), "Creating flow staging buffer failed");
    VkMemoryRequirements bufferRequirements{};
    vkGetBufferMemoryRequirements(device_, stagingBuffer_, &bufferRequirements);
    VkMemoryAllocateInfo bufferAllocation{};
    bufferAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufferAllocation.allocationSize = bufferRequirements.size;
    bufferAllocation.memoryTypeIndex = memoryType(bufferRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    require(vkAllocateMemory(device_, &bufferAllocation, nullptr, &stagingMemory_), "Allocating flow staging memory failed");
    require(vkBindBufferMemory(device_, stagingBuffer_, stagingMemory_, 0), "Binding flow staging memory failed");
    require(vkMapMemory(device_, stagingMemory_, 0, bytes, 0, reinterpret_cast<void**>(&mapped_)), "Mapping flow staging memory failed");

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    require(vkCreateImage(device_, &imageInfo, nullptr, &image_), "Creating flow image failed");
    VkMemoryRequirements imageRequirements{};
    vkGetImageMemoryRequirements(device_, image_, &imageRequirements);
    VkMemoryAllocateInfo imageAllocation{};
    imageAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAllocation.allocationSize = imageRequirements.size;
    imageAllocation.memoryTypeIndex = memoryType(imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    require(vkAllocateMemory(device_, &imageAllocation, nullptr, &imageMemory_), "Allocating flow image failed");
    require(vkBindImageMemory(device_, image_, imageMemory_, 0), "Binding flow image failed");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    require(vkCreateImageView(device_, &viewInfo, nullptr, &imageView_), "Creating flow image view failed");
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    require(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_), "Creating flow sampler failed");
    descriptor_ = ImGui_ImplVulkan_AddTexture(sampler_, imageView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    require(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "Creating flow command pool failed");
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    require(vkAllocateCommandBuffers(device_, &commandInfo, &commandBuffer_), "Allocating flow command buffer failed");
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    require(vkCreateFence(device_, &fenceInfo, nullptr, &uploadFence_), "Creating flow upload fence failed");
}

FlowTexture::~FlowTexture() {
    if (!device_) return;
    vkWaitForFences(device_, 1, &uploadFence_, VK_TRUE, UINT64_MAX);
    if (descriptor_) ImGui_ImplVulkan_RemoveTexture(descriptor_);
    if (mapped_) vkUnmapMemory(device_, stagingMemory_);
    vkDestroyFence(device_, uploadFence_, nullptr);
    vkDestroyCommandPool(device_, commandPool_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
    vkDestroyImageView(device_, imageView_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    vkFreeMemory(device_, imageMemory_, nullptr);
    vkDestroyBuffer(device_, stagingBuffer_, nullptr);
    vkFreeMemory(device_, stagingMemory_, nullptr);
}

ImTextureID FlowTexture::textureId() const {
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(descriptor_));
}

void FlowTexture::update(const FlowSolver& solver, FieldView view) {
    require(vkWaitForFences(device_, 1, &uploadFence_, VK_TRUE, UINT64_MAX), "Waiting for flow upload failed");
    require(vkResetFences(device_, 1, &uploadFence_), "Resetting flow upload fence failed");
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const auto color = solver.colorAt(x, height_ - 1 - y, view);
            const size_t offset = static_cast<size_t>((y * width_ + x) * 4);
            mapped_[offset] = static_cast<unsigned char>(255.0f * std::clamp(color[0], 0.0f, 1.0f));
            mapped_[offset + 1] = static_cast<unsigned char>(255.0f * std::clamp(color[1], 0.0f, 1.0f));
            mapped_[offset + 2] = static_cast<unsigned char>(255.0f * std::clamp(color[2], 0.0f, 1.0f));
            mapped_[offset + 3] = 255;
        }
    }

    require(vkResetCommandPool(device_, commandPool_, 0), "Resetting flow command pool failed");
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    require(vkBeginCommandBuffer(commandBuffer_, &beginInfo), "Beginning flow upload failed");
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = initialized_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image_;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commandBuffer_, initialized_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    vkCmdCopyBufferToImage(commandBuffer_, stagingBuffer_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier toShader = toTransfer;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toShader);
    require(vkEndCommandBuffer(commandBuffer_), "Ending flow upload failed");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    require(vkQueueSubmit(queue_, 1, &submit, uploadFence_), "Submitting flow upload failed");
    initialized_ = true;
}

} // namespace rocket
