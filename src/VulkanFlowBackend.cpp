#include "VulkanFlowBackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifndef ROCKETSIM_FLOW_SPV
#define ROCKETSIM_FLOW_SPV "flow.comp.spv"
#endif

namespace rocket {
namespace {
void require(VkResult result, const char* operation) { if(result!=VK_SUCCESS) throw std::runtime_error(operation); }
struct Push {
    int width, height; float dx,dy,xMin,yMin,gamma,gasR,ambientP,ambientT,chamberP,chamberT;
    float chamberRadius,throatRadius,exitRadius,chamberLength,convergingLength,divergingLength,dt; int secondStage;
};
}

uint32_t VulkanFlowBackend::memoryType(uint32_t bits,VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(physicalDevice_,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;++i) if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;
    throw std::runtime_error("No host-visible Vulkan storage memory");
}
VulkanFlowBackend::Buffer VulkanFlowBackend::createBuffer(VkDeviceSize size,VkBufferUsageFlags usage) {
    Buffer b; VkBufferCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; ci.size=size; ci.usage=usage;
    require(vkCreateBuffer(device_,&ci,nullptr,&b.buffer),"Creating CFD storage buffer failed");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device_,b.buffer,&req);
    VkMemoryAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize=req.size;
    ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    require(vkAllocateMemory(device_,&ai,nullptr,&b.memory),"Allocating CFD storage memory failed");
    require(vkBindBufferMemory(device_,b.buffer,b.memory,0),"Binding CFD storage memory failed");
    require(vkMapMemory(device_,b.memory,0,size,0,&b.mapped),"Mapping CFD storage memory failed"); return b;
}

VulkanFlowBackend::VulkanFlowBackend(VkPhysicalDevice physicalDevice,VkDevice device,VkQueue queue,uint32_t queueFamily,
                                     int width,int height,float dx,float dy,float xMin,float yMin)
    :physicalDevice_(physicalDevice),device_(device),queue_(queue),width_(width),height_(height),dx_(dx),dy_(dy),xMin_(xMin),yMin_(yMin) {
    VkDeviceSize stateBytes=VkDeviceSize(width_)*height_*5*sizeof(float), maskBytes=VkDeviceSize(width_)*height_*sizeof(uint32_t);
    stateA_=createBuffer(stateBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); stateB_=createBuffer(stateBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    stateBase_=createBuffer(stateBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    mask_=createBuffer(maskBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding bindings[4]{};
    for(uint32_t i=0;i<4;++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;li.bindingCount=4;li.pBindings=bindings;
    require(vkCreateDescriptorSetLayout(device_,&li,nullptr,&descriptorLayout_),"Creating CFD descriptor layout failed");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8}; VkDescriptorPoolCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pi.maxSets=2;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
    require(vkCreateDescriptorPool(device_,&pi,nullptr,&descriptorPool_),"Creating CFD descriptor pool failed");
    VkDescriptorSetLayout layouts[2]{descriptorLayout_,descriptorLayout_}; VkDescriptorSetAllocateInfo si{};si.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;si.descriptorPool=descriptorPool_;si.descriptorSetCount=2;si.pSetLayouts=layouts;
    VkDescriptorSet sets[2]{};require(vkAllocateDescriptorSets(device_,&si,sets),"Allocating CFD descriptor sets failed");stage1Set_=sets[0];stage2Set_=sets[1];
    writeSet(stage1Set_,stateA_.buffer,stateB_.buffer,stateBase_.buffer);writeSet(stage2Set_,stateB_.buffer,stateA_.buffer,stateBase_.buffer);
    std::ifstream file(ROCKETSIM_FLOW_SPV,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Opening CFD compute shader failed");
    size_t bytes=static_cast<size_t>(file.tellg());std::vector<uint32_t> code((bytes+3)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),static_cast<std::streamsize>(bytes));
    VkShaderModuleCreateInfo sm{};sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;sm.codeSize=bytes;sm.pCode=code.data();VkShaderModule module{};
    require(vkCreateShaderModule(device_,&sm,nullptr,&module),"Creating CFD compute shader failed");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};VkPipelineLayoutCreateInfo pli{};pli.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pli.setLayoutCount=1;pli.pSetLayouts=&descriptorLayout_;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
    require(vkCreatePipelineLayout(device_,&pli,nullptr,&pipelineLayout_),"Creating CFD pipeline layout failed");
    VkPipelineShaderStageCreateInfo stage{};stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;stage.module=module;stage.pName="main";
    VkComputePipelineCreateInfo pci{};pci.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;pci.stage=stage;pci.layout=pipelineLayout_;
    require(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline_),"Creating CFD compute pipeline failed");vkDestroyShaderModule(device_,module,nullptr);
    VkCommandPoolCreateInfo cpi{};cpi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cpi.queueFamilyIndex=queueFamily;
    require(vkCreateCommandPool(device_,&cpi,nullptr,&commandPool_),"Creating CFD command pool failed");VkCommandBufferAllocateInfo cai{};cai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;cai.commandPool=commandPool_;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    require(vkAllocateCommandBuffers(device_,&cai,&commandBuffer_),"Allocating CFD command buffer failed");VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;require(vkCreateFence(device_,&fi,nullptr,&fence_),"Creating CFD fence failed");
}
void VulkanFlowBackend::writeSet(VkDescriptorSet set,VkBuffer input,VkBuffer output,VkBuffer base){VkDescriptorBufferInfo infos[4]{{input,0,VK_WHOLE_SIZE},{output,0,VK_WHOLE_SIZE},{mask_.buffer,0,VK_WHOLE_SIZE},{base,0,VK_WHOLE_SIZE}};VkWriteDescriptorSet writes[4]{};for(uint32_t i=0;i<4;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];}vkUpdateDescriptorSets(device_,4,writes,0,nullptr);}
void VulkanFlowBackend::wait(){require(vkWaitForFences(device_,1,&fence_,VK_TRUE,UINT64_MAX),"Waiting for CFD compute failed");}
void VulkanFlowBackend::upload(const std::vector<float>& state,const std::vector<uint32_t>& mask){wait();std::memcpy(stateA_.mapped,state.data(),state.size()*sizeof(float));std::memcpy(stateB_.mapped,state.data(),state.size()*sizeof(float));std::memcpy(stateBase_.mapped,state.data(),state.size()*sizeof(float));std::memcpy(mask_.mapped,mask.data(),mask.size()*sizeof(uint32_t));}
float VulkanFlowBackend::advance(int steps,const Parameters& p){if(steps<=0)return 0;wait();require(vkResetFences(device_,1,&fence_),"Resetting CFD fence failed");require(vkResetCommandPool(device_,commandPool_,0),"Resetting CFD command pool failed");
    float sound=std::sqrt(p.gamma*(8.314462618f/(p.molarMassGPerMol*0.001f))*p.chamberTemperatureK);float dt=p.cfl*std::min(dx_,dy_)/(sound+4200.0f);
    Push push{width_,height_,dx_,dy_,xMin_,yMin_,p.gamma,8.314462618f/(p.molarMassGPerMol*0.001f),p.ambientPressureKPa*1000.0f,p.ambientTemperatureK,p.chamberPressureMPa*1e6f,p.chamberTemperatureK,p.chamberRadiusM,p.throatRadiusM,p.exitRadiusM,p.chamberLengthM,p.convergingLengthM,p.divergingLengthM,dt,0};
    VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;require(vkBeginCommandBuffer(commandBuffer_,&bi),"Beginning CFD compute failed");vkCmdBindPipeline(commandBuffer_,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    for(int i=0;i<steps;++i){push.secondStage=0;vkCmdBindDescriptorSets(commandBuffer_,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&stage1Set_,0,nullptr);vkCmdPushConstants(commandBuffer_,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push),&push);vkCmdDispatch(commandBuffer_,(width_+7)/8,(height_+7)/8,1);vkCmdPipelineBarrier(commandBuffer_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        push.secondStage=1;vkCmdBindDescriptorSets(commandBuffer_,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&stage2Set_,0,nullptr);vkCmdPushConstants(commandBuffer_,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push),&push);vkCmdDispatch(commandBuffer_,(width_+7)/8,(height_+7)/8,1);vkCmdPipelineBarrier(commandBuffer_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        }
    require(vkEndCommandBuffer(commandBuffer_),"Ending CFD compute failed");VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&commandBuffer_;require(vkQueueSubmit(queue_,1,&submit,fence_),"Submitting CFD compute failed");wait();return dt;}
void VulkanFlowBackend::download(std::vector<float>& state){wait();state.resize(static_cast<size_t>(width_)*height_*5);std::memcpy(state.data(),stateA_.mapped,state.size()*sizeof(float));}
VulkanFlowBackend::~VulkanFlowBackend(){if(!device_)return;wait();vkDestroyFence(device_,fence_,nullptr);vkDestroyCommandPool(device_,commandPool_,nullptr);vkDestroyPipeline(device_,pipeline_,nullptr);vkDestroyPipelineLayout(device_,pipelineLayout_,nullptr);vkDestroyDescriptorPool(device_,descriptorPool_,nullptr);vkDestroyDescriptorSetLayout(device_,descriptorLayout_,nullptr);for(Buffer* b:{&stateA_,&stateB_,&stateBase_,&mask_}){if(b->mapped)vkUnmapMemory(device_,b->memory);vkDestroyBuffer(device_,b->buffer,nullptr);vkFreeMemory(device_,b->memory,nullptr);}}
} // namespace rocket
