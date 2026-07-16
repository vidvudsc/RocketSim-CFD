#include "HeadlessVulkanContext.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace rocket {
namespace {
void require(VkResult result,const char* message){if(result!=VK_SUCCESS)throw std::runtime_error(message);}
}

HeadlessVulkanContext::HeadlessVulkanContext(){
    uint32_t count=0;vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr);std::vector<VkExtensionProperties> available(count);vkEnumerateInstanceExtensionProperties(nullptr,&count,available.data());
    std::vector<const char*> extensions;VkInstanceCreateFlags flags=0;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    for(const auto& e:available)if(std::strcmp(e.extensionName,VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)==0){extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);flags|=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;}
#endif
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.pApplicationName="RocketSim offline compute";app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ici.flags=flags;ici.pApplicationInfo=&app;ici.enabledExtensionCount=static_cast<uint32_t>(extensions.size());ici.ppEnabledExtensionNames=extensions.data();
    require(vkCreateInstance(&ici,nullptr,&instance_),"Creating offline Vulkan instance failed");
    uint32_t physicalCount=0;require(vkEnumeratePhysicalDevices(instance_,&physicalCount,nullptr),"Enumerating GPUs failed");if(!physicalCount)throw std::runtime_error("No Vulkan compute GPU found");
    std::vector<VkPhysicalDevice> devices(physicalCount);vkEnumeratePhysicalDevices(instance_,&physicalCount,devices.data());physicalDevice_=devices[0];
    uint32_t familyCount=0;vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_,&familyCount,nullptr);std::vector<VkQueueFamilyProperties> families(familyCount);vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_,&familyCount,families.data());
    while(queueFamily_<familyCount&&(families[queueFamily_].queueFlags&VK_QUEUE_COMPUTE_BIT)==0)++queueFamily_;if(queueFamily_==familyCount)throw std::runtime_error("GPU has no compute queue");
    uint32_t extensionCount=0;vkEnumerateDeviceExtensionProperties(physicalDevice_,nullptr,&extensionCount,nullptr);std::vector<VkExtensionProperties> deviceAvailable(extensionCount);vkEnumerateDeviceExtensionProperties(physicalDevice_,nullptr,&extensionCount,deviceAvailable.data());std::vector<const char*> deviceExtensions;
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    for(const auto& e:deviceAvailable)if(std::strcmp(e.extensionName,VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)==0)deviceExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
    float priority=1.0f;VkDeviceQueueCreateInfo qci{};qci.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qci.queueFamilyIndex=queueFamily_;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkDeviceCreateInfo dci{};dci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.enabledExtensionCount=static_cast<uint32_t>(deviceExtensions.size());dci.ppEnabledExtensionNames=deviceExtensions.data();
    require(vkCreateDevice(physicalDevice_,&dci,nullptr,&device_),"Creating offline Vulkan device failed");vkGetDeviceQueue(device_,queueFamily_,0,&queue_);
}
HeadlessVulkanContext::~HeadlessVulkanContext(){if(device_){vkDeviceWaitIdle(device_);vkDestroyDevice(device_,nullptr);}if(instance_)vkDestroyInstance(instance_,nullptr);}
} // namespace rocket
